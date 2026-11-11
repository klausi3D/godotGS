#include "gs_world_writer_v4.h"

#include "../core/gs_vector_alloc.h"
#include "../core/streaming_chunk_payload_source.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace gs_gsq {
// Implemented beside the codec, using its configured zstd headers/library.
uint64_t writer_v4_workspace_bytes(const EncodeParams &p_params);
}

namespace gs_world_writer {
namespace {

constexpr uint64_t kSliceBytes = uint64_t(1) << 20;
std::atomic<uint64_t> next_body_handle{ 1 };

uint64_t allocate_body_handle() {
	uint64_t next = next_body_handle.load();
	while (next != UINT64_MAX) {
		if (next_body_handle.compare_exchange_weak(next, next + 1)) {
			return next;
		}
	}
	return UINT64_MAX; // Saturate; never alias an earlier writer's handle.
}

bool add_checked(uint64_t a, uint64_t b, uint64_t &r_value) {
	if (b > UINT64_MAX - a) {
		return false;
	}
	r_value = a + b;
	return true;
}

// Admit JSON's transient UTF32/UTF8 strings, sorted keys and parsed dictionary
// before creating them. Walk shared containers without copying their elements.
// Object/callable/signal stringification can execute arbitrary user code and has
// no finite allocation bound, so it is not admissible writer metadata.
Error metadata_cost(const Variant &p_value, uint64_t p_cap, uint64_t &r_cost, const void **p_stack, uint32_t p_depth = 0) {
	if (p_depth >= Variant::MAX_RECURSION_DEPTH) {
		return ERR_INVALID_DATA;
	}
	uint64_t characters = 8192; // Upper bound for fixed-size math value strings.
	const Variant::Type type = p_value.get_type();
	if (type == Variant::OBJECT || type == Variant::CALLABLE || type == Variant::SIGNAL) {
		return ERR_INVALID_DATA;
	}
	if (type == Variant::STRING || type == Variant::STRING_NAME) {
		characters = String(p_value).length(); // COW; no character copy.
	} else if (type == Variant::NODE_PATH) {
		const NodePath path = p_value;
		characters = 1;
		for (int i = 0; i < path.get_name_count(); ++i) {
			characters += uint64_t(String(path.get_name(i)).length()) + 1;
		}
		for (int i = 0; i < path.get_subname_count(); ++i) {
			characters += uint64_t(String(path.get_subname(i)).length()) + 1;
		}
	} else if (type == Variant::FLOAT) {
		characters = 1024;
	} else if (type == Variant::INT || type == Variant::BOOL || type == Variant::NIL) {
		characters = 32;
	} else if (type == Variant::DICTIONARY || type == Variant::ARRAY || type >= Variant::PACKED_BYTE_ARRAY) {
		characters = 4;
	}
	uint64_t charge;
	if (characters > (UINT64_MAX - 1024) / 64 || !add_checked(r_cost, characters * 64 + 1024, charge) || charge > p_cap) {
		return ERR_OUT_OF_MEMORY;
	}
	r_cost = charge;
	const void *id = nullptr;
	if (type == Variant::DICTIONARY) {
		id = Dictionary(p_value).id();
	} else if (type == Variant::ARRAY) {
		id = Array(p_value).id();
	}
	if (id) {
		for (uint32_t i = 0; i < p_depth; ++i) {
			if (p_stack[i] == id) {
				return ERR_INVALID_DATA;
			}
		}
	}
	p_stack[p_depth] = id;
	if (type == Variant::DICTIONARY) {
		const Dictionary dict = p_value;
		for (const Variant *key = dict.next(); key; key = dict.next(key)) {
			Error err = metadata_cost(*key, p_cap, r_cost, p_stack, p_depth + 1);
			if (err != OK) {
				return err;
			}
			err = metadata_cost(dict[*key], p_cap, r_cost, p_stack, p_depth + 1);
			if (err != OK) {
				return err;
			}
		}
	} else if (type == Variant::ARRAY || type >= Variant::PACKED_BYTE_ARRAY) {
		const uint64_t count = p_value.get_indexed_size();
		for (uint64_t i = 0; i < count; ++i) {
			bool valid = false, oob = false;
			const Variant item = p_value.get_indexed(i, valid, oob);
			if (!valid || oob) {
				return ERR_INVALID_DATA;
			}
			const Error err = metadata_cost(item, p_cap, r_cost, p_stack, p_depth + 1);
			if (err != OK) {
				return err;
			}
		}
	}
	return OK;
}

uint32_t bits_of(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

bool is_finite(float p_value) {
	return (bits_of(p_value) & 0x7F800000u) != 0x7F800000u;
}

float step_tolerance(float p_lo, float p_hi, float p_steps) {
	return (p_hi - p_lo) / p_steps * 0.6f + MAX(1.0f, MAX(std::fabs(p_lo), std::fabs(p_hi))) * 1e-6f;
}

void hash_bytes(const uint8_t *p_bytes, uint64_t p_size, uint8_t r_digest[32]) {
	CryptoCore::SHA256Context hash;
	hash.start();
	if (p_size) {
		hash.update(p_bytes, p_size);
	}
	hash.finish(r_digest);
}

void hash_body(const Gaussian *p_gaussians, const Vector3 *p_sh, uint32_t p_count, uint32_t p_high, uint8_t r_digest[32]) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(reinterpret_cast<const uint8_t *>(p_gaussians), uint64_t(p_count) * sizeof(Gaussian));
	if (p_high) {
		hash.update(reinterpret_cast<const uint8_t *>(p_sh), uint64_t(p_count) * p_high * sizeof(Vector3));
	}
	hash.finish(r_digest);
}

void make_relative(Gaussian *p_gaussians, uint32_t p_count, const V4BodyDesc &p_desc) {
	if (!p_desc.is_leaf) {
		return;
	}
	for (uint32_t i = 0; i < p_count; ++i) {
		for (int a = 0; a < 3; ++a) {
			p_gaussians[i].position[a] = real_t(double(p_gaussians[i].position[a]) - p_desc.cell_center[a]);
		}
	}
}

bool finite_body(const Gaussian *p_gaussians, const Vector3 *p_sh, uint32_t p_count, uint32_t p_high) {
	for (uint32_t i = 0; i < p_count; ++i) {
		const Gaussian &g = p_gaussians[i];
		const float fields[] = { g.position.x, g.position.y, g.position.z, g.opacity, g.scale.x, g.scale.y, g.scale.z,
			g.rotation.x, g.rotation.y, g.rotation.z, g.rotation.w, g.sh_dc.r, g.sh_dc.g, g.sh_dc.b, g.sh_dc.a,
			g.normal.x, g.normal.y, g.normal.z, g.area, g.stroke_age, g.brush_axes.x, g.brush_axes.y };
		for (float value : fields) {
			if (!is_finite(value)) {
				return false;
			}
		}
		for (const Vector3 &sh : g.sh_1) {
			for (int a = 0; a < 3; ++a) {
				if (!is_finite(sh[a])) {
					return false;
				}
			}
		}
		for (uint32_t k = 0; k < p_high; ++k) {
			for (int a = 0; a < 3; ++a) {
				if (!is_finite(p_sh[uint64_t(i) * p_high + k][a])) {
					return false;
				}
			}
		}
	}
	return true;
}

// Same W12 semantic tolerances as PageBuilder::_validate_page. We cannot use
// PageBuilder itself: it groups/Morton-reorders the caller's node payload.
Error validate_round_trip(const gs_gsq::PageInfo &info, const Gaussian *p_source, const Vector3 *p_source_sh,
		const Gaussian *decoded_gaussians, const Vector3 *decoded_sh, uint32_t p_count, uint32_t sh_high_order, String &r_reason) {
	float position_tol[3];
	float dc_tol[3];
	float scale_tol[3];
	float sh_tol[3];
	constexpr float kDcSteps[3] = { 2047.0f, 2047.0f, 1023.0f };
	for (uint32_t axis = 0u; axis < 3u; axis++) {
		position_tol[axis] = step_tolerance(info.position_min[axis], info.position_max[axis], float((1u << info.position_bits) - 1u));
		dc_tol[axis] = step_tolerance(info.dc_min[axis], info.dc_max[axis], kDcSteps[axis]);
		scale_tol[axis] = float(info.log_scale_max[axis] - info.log_scale_min[axis]) / 65536.0f / 1023.0f * 0.6f + 1e-4f;
		sh_tol[axis] = info.sh_max[axis] / float((1u << (info.sh_bits[axis] - 1u)) - 1u) * 0.6f + 1e-6f;
	}
	const float opacity_tol = 0.6f / float((1u << info.opacity_bits) - 1u) + 1e-6f;
	const float normal_tol = 0.6f / 32767.0f + 1e-6f;

	for (uint32_t i = 0u; i < p_count; i++) {
		const Gaussian &s = p_source[i];
		const Gaussian &d = decoded_gaussians[i];
		const char *field = nullptr;
		const float decoded_floats[] = { d.position.x, d.position.y, d.position.z, d.opacity, d.scale.x, d.scale.y, d.scale.z,
			d.rotation.x, d.rotation.y, d.rotation.z, d.rotation.w, d.sh_dc.r, d.sh_dc.g, d.sh_dc.b, d.normal.x, d.normal.y, d.normal.z };
		for (float value : decoded_floats) {
			if (!is_finite(value)) {
				field = "non-finite decoded field";
			}
		}
		if (bits_of(d.area) != bits_of(s.area) || bits_of(d.sh_dc.a) != bits_of(s.sh_dc.a) ||
				bits_of(d.stroke_age) != bits_of(s.stroke_age) || bits_of(d.brush_axes.x) != bits_of(s.brush_axes.x) ||
				bits_of(d.brush_axes.y) != bits_of(s.brush_axes.y) || d.painterly_meta != s.painterly_meta ||
				d.render_meta != s.render_meta) {
			field = "page constant";
		}
		for (uint32_t axis = 0u; axis < 3u && field == nullptr; axis++) {
			if (!(std::fabs(d.position[axis] - s.position[axis]) <= position_tol[axis])) {
				field = "position";
			} else if (!(std::fabs(d.sh_dc[axis] - s.sh_dc[axis]) <= dc_tol[axis])) {
				field = "DC";
			} else if (info.has_normals ? !(std::fabs(d.normal[axis] - CLAMP(s.normal[axis], -1.0f, 1.0f)) <= normal_tol)
										: bits_of(d.normal[axis]) != bits_of(s.normal[axis])) {
				field = "normal";
			} else {
				const float source_scale = std::fabs(s.scale[axis]);
				if (!(d.scale[axis] > 0.0f)) {
					field = "scale";
				} else if (source_scale < 0x1p-31f ? d.scale[axis] > 0x1p-30f
												   : !(std::fabs(std::log2(d.scale[axis]) - std::log2(source_scale)) <= scale_tol[axis])) {
					field = "scale";
				}
			}
		}
		if (field == nullptr && !(std::fabs(d.opacity - s.opacity) <= opacity_tol)) {
			field = "opacity";
		}
		if (field == nullptr) {
			const float length = std::sqrt(s.rotation.dot(s.rotation));
			if (!(length > 0.0f) || !(std::fabs(s.rotation.dot(d.rotation)) / length >= 0.9999f)) {
				field = "rotation";
			}
		}
		for (uint32_t v = 0u; v < 3u && field == nullptr; v++) {
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				if (!(std::fabs(d.sh_1[v][axis] - s.sh_1[v][axis]) <= sh_tol[0])) {
					field = "first-order SH";
				}
			}
		}
		for (uint32_t k = 0u; k < sh_high_order && field == nullptr; k++) {
			const Vector3 &a = p_source_sh[uint64_t(i) * sh_high_order + k];
			const Vector3 &b = decoded_sh[i * sh_high_order + k];
			const float tol = sh_tol[k < 5u ? 1u : 2u];
			for (uint32_t axis = 0u; axis < 3u; axis++) {
				if (!(std::fabs(a[axis] - b[axis]) <= tol)) {
					field = "high-order SH";
				}
			}
		}
		if (field != nullptr) {
			r_reason = vformat("w12_mismatch: splat %d %s outside its D-12 step after the round trip", i, field);
			return ERR_INVALID_DATA;
		}
	}

	return OK;
}

struct SegmentHasher {
	static uint32_t hash(const V4SegmentKey &p_key) {
		return hash_murmur3_one_32(p_key.depth, hash_murmur3_one_64(p_key.bucket));
	}
};

} // namespace

struct WriterV4::Impl {
	struct Spill {
		V4SpillRecord record;
		uint32_t segment = 0;
	};
	struct Segment {
		GSAtomicFileTransaction file;
		uint32_t remaining = 0;
	};
	bool open = false;
	bool output_begun = false;
	PagedV4Spec spec;
	CodecParams codec;
	gs_gsq::EncodeParams encode_params;
	Limits limits;
	String final_path;
	String scratch_dir;
	Vector<uint8_t> metadata;
	Vector<Spill> spills;
	HashMap<uint64_t, uint32_t> handles;
	HashMap<V4SegmentKey, uint32_t, SegmentHasher> segment_ids;
	LocalVector<Segment *> segments;
	GSAtomicFileTransaction output;
	uint64_t started = 0;
	uint64_t live_scratch = 0;
	uint64_t codec_workspace = 0;
	uint64_t body_reservation = 0;
	uint64_t metadata_reservation = 0;
	SafeFlag cancelled;
	SafeNumeric<uint32_t> phase{ uint32_t(Phase::IDLE) };
	SafeNumeric<uint64_t> nodes_done{ 0 };
	SafeNumeric<uint64_t> nodes_total{ 0 };
	SafeNumeric<uint64_t> splats_done{ 0 };
	SafeNumeric<uint64_t> splats_total{ 0 };
	SafeNumeric<uint64_t> bytes_written{ 0 };
#ifdef TESTS_ENABLED
	AssemblyObserver observer = nullptr;
	void *observer_userdata = nullptr;
#endif
};

WriterV4::WriterV4() {
	impl = memnew(Impl);
}

WriterV4::~WriterV4() {
	_cleanup();
	memdelete(impl);
}

void WriterV4::_cleanup() {
	impl->output.abort();
	for (uint32_t i = 0; i < impl->segments.size(); ++i) {
		Impl::Segment *segment = impl->segments[i];
		if (segment) {
			// Preserve measured file lengths in the report before removing files.
			report.segments[i].spill_bytes = segment->file.get_size();
			segment->file.abort();
			memdelete(segment);
		}
	}
	impl->segments.reset();
	impl->segment_ids.clear();
	impl->live_scratch = 0;
	impl->open = false;
}

Error WriterV4::_fail(Error p_error, const String &p_reason) {
	report.error = p_error;
	report.reason = p_reason.get_slicec(':', 0).strip_edges();
	report.committed = false;
	report.output_bytes = impl->output_begun ? impl->output.get_size() : 0;
	_cleanup();
	impl->phase.set(uint32_t(Phase::STOPPED));
	return p_error;
}

Error WriterV4::_check_stop() {
	if (impl->cancelled.is_set()) {
		return _fail(ERR_SKIP, "cancelled");
	}
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	if (impl->limits.deadline_usec != Limits::kUnbounded && now >= impl->started && now - impl->started > impl->limits.deadline_usec) {
		return _fail(ERR_TIMEOUT, "deadline");
	}
	return OK;
}

Error WriterV4::_check_ram(uint64_t p_body_bytes, uint32_t p_extra_records) {
	// Conservative capacity admission (COW power-of-two allocations, LocalVector
	// growth, hash buckets, temporary validator ranges and string references).
	// Caller-owned bodies and topology are excluded; our copies are included.
	impl->body_reservation = MAX(impl->body_reservation, p_body_bytes);
	const uint64_t count = MAX(uint64_t(impl->spec.node_count), uint64_t(impl->spills.size()) + p_extra_records);
	const uint64_t per_node = 4 * (sizeof(Impl::Spill) + sizeof(Impl::Segment) + sizeof(V4SegmentReport) +
			sizeof(GaussianSplatHlodPagedSource::NodeRecord) + sizeof(GaussianSplatHlodNode) + 256 +
			(uint64_t(impl->scratch_dir.length()) + uint64_t(impl->final_path.length())) * sizeof(char32_t));
	uint64_t bytes = kSliceBytes + gs_world_paged::kIndexPageBytes + gs_world_paged::kHeaderBytes;
	if (count > UINT64_MAX / per_node || !add_checked(bytes, count * per_node, bytes) ||
			!add_checked(bytes, (uint64_t(impl->handles.get_capacity()) + impl->segment_ids.get_capacity()) * 16, bytes) ||
			!add_checked(bytes, impl->metadata_reservation, bytes) ||
			!add_checked(bytes, impl->body_reservation, bytes) || !add_checked(bytes, impl->codec_workspace, bytes) ||
			bytes > impl->limits.max_ram_bytes) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "limit_ram");
	}
	return OK;
}

Error WriterV4::_write(GSAtomicFileTransaction &p_file, const uint8_t *p_bytes, uint64_t p_size, bool p_scratch) {
	while (p_size) {
		Error err = _check_stop();
		if (err != OK || (err = _check_ram()) != OK) {
			return err;
		}
		const uint64_t slice = MIN(p_size, kSliceBytes);
		const uint64_t used = p_scratch ? impl->live_scratch : p_file.get_size();
		const uint64_t cap = p_scratch ? impl->limits.max_scratch_bytes : impl->limits.max_output_bytes;
		if (used > cap || slice > cap - used) {
			return _fail(ERR_PARAMETER_RANGE_ERROR, p_scratch ? "limit_scratch" : "limit_output");
		}
		uint64_t next_total, next_progress, disk;
		if (!add_checked(report.total_scratch_bytes, p_scratch ? slice : 0, next_total) ||
				!add_checked(impl->bytes_written.get(), slice, next_progress) ||
				!add_checked(impl->live_scratch, report.output_bytes, disk) || !add_checked(disk, slice, disk)) {
			return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
		}
		err = p_file.append(p_bytes, slice);
		if (err != OK) {
			return _fail(err, "io");
		}
		if (p_scratch) {
			impl->live_scratch += slice;
			report.total_scratch_bytes = next_total;
		}
		report.output_bytes = impl->output_begun ? impl->output.get_size() : 0;
		report.peak_scratch_plus_output_bytes = MAX(report.peak_scratch_plus_output_bytes, disk);
		impl->bytes_written.set(next_progress);
		p_bytes += slice;
		p_size -= slice;
	}
	return OK;
}

Error WriterV4::_read(GSAtomicFileTransaction &p_file, uint64_t p_offset, uint8_t *p_bytes, uint64_t p_size) {
	if (p_offset > p_file.get_size() || p_size > p_file.get_size() - p_offset) {
		return _fail(ERR_FILE_CORRUPT, "v4_spill_extent");
	}
	while (p_size) {
		Error err = _check_stop();
		if (err != OK || (err = _check_ram()) != OK) {
			return err;
		}
		const uint64_t slice = MIN(p_size, kSliceBytes);
		err = p_file.read_at(p_offset, p_bytes, slice);
		if (err != OK) {
			return _fail(err, "io");
		}
		p_offset += slice;
		p_bytes += slice;
		p_size -= slice;
	}
	return OK;
}

void WriterV4::abort() {
	if (impl->open) {
		_fail(OK, "aborted");
	}
}

void WriterV4::request_cancel() {
	impl->cancelled.set();
}

Progress WriterV4::get_progress() const {
	Progress progress;
	progress.phase = Phase(impl->phase.get());
	progress.pass = progress.phase == Phase::WRITING ? 1 : 0;
	progress.pass_count = progress.phase == Phase::IDLE ? 0 : 1;
	progress.chunks_done = impl->nodes_done.get();
	progress.chunks_total = impl->nodes_total.get();
	progress.splats_done = impl->splats_done.get();
	progress.splats_total = impl->splats_total.get();
	progress.bytes_written = impl->bytes_written.get();
	return progress;
}

#ifdef TESTS_ENABLED
void WriterV4::set_assembly_observer(AssemblyObserver p_observer, void *p_userdata) {
	impl->observer = p_observer;
	impl->observer_userdata = p_userdata;
}
#endif

Error WriterV4::get_spill_record(V4BodyHandle p_handle, V4SpillRecord &r_record) const {
	const uint32_t *index = impl->handles.getptr(p_handle.value);
	if (!index) {
		return ERR_INVALID_PARAMETER;
	}
	r_record = impl->spills[*index].record;
	return OK;
}

Error WriterV4::begin_paged(const String &p_final_path, const String &p_scratch_dir, const PagedV4Spec &p_spec,
		const CodecParams &p_codec, const Limits &p_limits) {
	using namespace gs_world_paged;
	if (impl->open) {
		return _fail(ERR_ALREADY_IN_USE, "already_open");
	}
	_cleanup();
	report = V4Report();
	impl->spills.clear();
	impl->handles.clear();
	impl->metadata.clear();
	impl->spec = p_spec;
	impl->codec = p_codec;
	impl->limits = p_limits;
	impl->final_path = p_final_path;
	impl->scratch_dir = p_scratch_dir;
	impl->output_begun = false;
	impl->codec_workspace = 0;
	impl->body_reservation = 0;
	impl->metadata_reservation = 0;
	impl->started = OS::get_singleton()->get_ticks_usec();
	impl->cancelled.clear();
	impl->phase.set(uint32_t(Phase::WRITING));
	impl->nodes_done.set(0);
	impl->nodes_total.set(p_spec.node_count);
	impl->splats_done.set(0);
	impl->splats_total.set(uint64_t(p_spec.leaf_splat_count) + p_spec.interior_splat_count);
	impl->bytes_written.set(0);
	if (!p_limits.max_ram_bytes || !p_limits.max_scratch_bytes || !p_limits.max_output_bytes ||
			!p_limits.deadline_usec || !p_limits.min_free_disk_margin) {
		return _fail(ERR_INVALID_PARAMETER, "limits_unset");
	}
	const uint64_t total = uint64_t(p_spec.leaf_splat_count) + p_spec.interior_splat_count;
	if (p_final_path.is_empty() || p_scratch_dir.is_empty() || !p_spec.node_count || p_spec.node_count > kMaxNodes ||
			!p_spec.leaf_splat_count || total > UINT32_MAX || p_spec.node_count > total ||
			p_spec.sh_degree > 4 || p_spec.sh_first_order > 3 || p_spec.sh_high_order > kMaxHighOrder || !p_spec.bake_rule_version) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec");
	}
	for (int a = 0; a < 3; ++a) {
		if (!Math::is_finite(p_spec.origin[a]) || !is_finite(p_spec.bounds.position[a]) ||
				!is_finite(p_spec.bounds.size[a]) || p_spec.bounds.size[a] < 0) {
			return _fail(ERR_INVALID_PARAMETER, "invalid_spec");
		}
	}
	String reason;
	if (p_codec.codec_id == kCodecGsqV1) {
		Error err = gsq_encode_params(p_codec, impl->encode_params, reason);
		if (err != OK) {
			return _fail(err, reason);
		}
		if (p_spec.sh_degree > gs_gsq::kMaxShDegree || p_spec.sh_high_order != gs_gsq::sh_high_order_for_degree(p_spec.sh_degree) ||
				p_spec.sh_first_order != (p_spec.sh_degree > 0 ? 3u : 0u)) {
			return _fail(ERR_INVALID_PARAMETER, "gsq_sh_layout");
		}
		impl->codec_workspace = gs_gsq::writer_v4_workspace_bytes(impl->encode_params);
	} else if (p_codec.codec_id != kCodecNone) {
		return _fail(ERR_UNAVAILABLE, "codec_unavailable");
	} else if (p_codec.sh_bits[0] || p_codec.sh_bits[1] || p_codec.sh_bits[2] || p_codec.opacity_bits ||
			p_codec.pos_step_m != 0.0f || p_codec.zstd_level) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec");
	}
	Error err = _check_ram();
	if (err != OK) {
		return err;
	}
	if (!p_spec.metadata.is_empty()) {
		const void *stack[Variant::MAX_RECURSION_DEPTH] = {};
		err = metadata_cost(p_spec.metadata, p_limits.max_ram_bytes, impl->metadata_reservation, stack);
		if (err != OK) {
			return _fail(err == ERR_OUT_OF_MEMORY ? ERR_PARAMETER_RANGE_ERROR : err,
					err == ERR_OUT_OF_MEMORY ? "limit_ram" : "v4_metadata_not_representable");
		}
		if ((err = _check_ram()) != OK || (err = _check_stop()) != OK) {
			return err;
		}
		const String json = JSON::stringify(p_spec.metadata);
		const CharString utf8 = json.utf8();
		JSON parsed;
		if (!utf8.length() || uint64_t(utf8.length()) > kMaxMetadataBytes || parsed.parse(json) != OK ||
				parsed.get_data().get_type() != Variant::DICTIONARY) {
			return _fail(ERR_INVALID_DATA, "v4_metadata_not_representable");
		}
		if (!gs_resize_or_fail(impl->metadata, utf8.length(), "V4 spill metadata")) {
			return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_metadata");
		}
		memcpy(impl->metadata.ptrw(), utf8.get_data(), utf8.length());
	}
	if ((err = _check_ram()) != OK || (err = _check_stop()) != OK) {
		return err;
	}
	// All factors are bounded by reader u32 counts / kMaxHighOrder before these
	// products. Padding depends on the final node counts: use an upper estimate
	// for disk preflight, a lower bound for early output-cap refusal.
	const uint64_t raw = total * (sizeof(Gaussian) + uint64_t(p_spec.sh_high_order) * sizeof(Vector3));
	const uint64_t pages = (uint64_t(p_spec.node_count) + kRecordsPerIndexPage - 1) / kRecordsPerIndexPage;
	const uint64_t minimum_output = kHeaderBytes + pages * kIndexPageBytes + raw +
			uint64_t(p_spec.node_count) * kPayloadRecordHeaderBytes + impl->metadata.size();
	if (minimum_output > p_limits.max_output_bytes) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "limit_output");
	}
	const uint64_t output_estimate = minimum_output + uint64_t(p_spec.node_count) * (kPageBytes - 1);
	const uint64_t scratch_estimate = p_codec.codec_id == kCodecNone ? raw : 2 * raw + uint64_t(p_spec.node_count) * 1024;
	uint64_t need;
	if (!add_checked(output_estimate, scratch_estimate, need) || !add_checked(need, p_limits.min_free_disk_margin, need)) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
	}
	// Both directories may be on different volumes. No files have been created.
	for (const String &path : { p_final_path.get_base_dir(), p_scratch_dir }) {
		Ref<DirAccess> dir = DirAccess::open(path.is_empty() ? String(".") : path);
		if (dir.is_null()) {
			return _fail(ERR_FILE_BAD_PATH, "preflight_disk");
		}
		const uint64_t free_bytes = dir->get_space_left();
		if (!free_bytes || free_bytes < need) {
			return _fail(ERR_FILE_CANT_WRITE, "preflight_disk");
		}
	}
	impl->open = true;
	return OK;
}

Error WriterV4::append_node(const V4BodyDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high,
		uint32_t p_count, V4BodyHandle &r_handle) {
	using namespace gs_world_paged;
	r_handle = V4BodyHandle();
	if (!impl->open) {
		return report.error != OK ? report.error : ERR_UNCONFIGURED;
	}
	Error err = _check_stop();
	if (err != OK) {
		return err;
	}
	if (!p_count || p_count > gs_hlod::kMaxNodeSplats || !p_gaussians || (impl->spec.sh_high_order && !p_sh_high) ||
			p_desc.segment.depth > gs_hlod::kMaxHeight || uint64_t(impl->spills.size()) >= uint64_t(kMaxNodes) + 1) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_node");
	}
	for (double center : p_desc.cell_center) {
		if (!Math::is_finite(center)) {
			return _fail(ERR_INVALID_PARAMETER, "v4_cell_center_invalid");
		}
	}
	const uint32_t high = impl->spec.sh_high_order;
	uint64_t g_bytes, sh_bytes, stored;
	if (!record_stored_bytes(p_count, high, g_bytes, sh_bytes, stored)) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
	}
	const uint64_t raw_bytes = g_bytes + sh_bytes;
	// One node at a time, including page, raw codec planes/log scales, decoded
	// arrays and capacity growth. The codec context has its own admission above.
	if ((err = _check_ram(8 * raw_bytes + kSliceBytes, 1)) != OK) {
		return err;
	}
	Vector<Gaussian> decoded;
	Vector<Vector3> sh;
	if (!gs_resize_or_fail(decoded, p_count, "V4 spill decoded body") ||
			!gs_resize_or_fail(sh, uint64_t(p_count) * high, "V4 spill decoded SH")) {
		return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_body");
	}
	LocalVector<uint8_t> page;
	String reason;
	if (impl->codec.codec_id == kCodecGsqV1) {
		err = gs_gsq::encode_page(p_gaussians, p_sh_high, p_count, impl->spec.sh_degree, impl->encode_params, page, reason);
		if (err != OK) {
			return _fail(err, reason);
		}
		gs_gsq::PageInfo info;
		err = gs_gsq::verify_page(page.ptr(), page.size(), info, reason);
		if (err != OK) {
			return _fail(err, "w12_verify");
		}
		if (info.splat_count != p_count || info.sh_degree != impl->spec.sh_degree || info.stored_bytes != page.size()) {
			return _fail(ERR_INVALID_DATA, "w12_header");
		}
		gs_gsq::DecodeContext decoder;
		err = decoder.decode_range(page.ptr(), page.size(), 0, p_count, decoded.ptrw(), sh.ptrw(), reason, false);
		if (err != OK) {
			return _fail(err, "w12_decode");
		}
		err = validate_round_trip(info, p_gaussians, p_sh_high, decoded.ptr(), sh.ptr(), p_count, high, reason);
		if (err != OK) {
			return _fail(err, reason);
		}
	} else {
		memcpy(decoded.ptrw(), p_gaussians, g_bytes);
		if (sh_bytes) {
			memcpy(sh.ptrw(), p_sh_high, sh_bytes);
		}
	}
	if (!finite_body(decoded.ptr(), sh.ptr(), p_count, high)) {
		return _fail(ERR_INVALID_DATA, "v4_nonfinite_body");
	}
	Impl::Spill spill;
	spill.record.desc = p_desc;
	spill.record.splat_count = p_count;
	if (!compute_node_metrics(decoded.ptr(), p_count, p_desc.cell_center, p_desc.is_leaf, spill.record.metrics, reason)) {
		return _fail(ERR_INVALID_DATA, reason);
	}
	if (!spill.record.metrics.center_exact) {
		return _fail(ERR_UNAVAILABLE, "v4_center_reconstruction_not_exact");
	}
	if (impl->codec.codec_id == kCodecGsqV1) {
		spill.record.stored_bytes = page.size();
		hash_bytes(page.ptr(), page.size(), spill.record.stored_digest);
	} else {
		spill.record.stored_bytes = raw_bytes;
		hash_body(decoded.ptr(), sh.ptr(), p_count, high, spill.record.stored_digest);
	}
	make_relative(decoded.ptrw(), p_count, p_desc);
	hash_body(decoded.ptr(), sh.ptr(), p_count, high, spill.record.output_body_digest);
	if ((err = _check_stop()) != OK) {
		return err;
	}
	const uint32_t *existing = impl->segment_ids.getptr(p_desc.segment);
	if (existing) {
		spill.segment = *existing;
	} else {
		spill.segment = impl->segments.size();
		Impl::Segment *segment = memnew(Impl::Segment);
		impl->segments.push_back(segment);
		V4SegmentReport segment_report;
		segment_report.key = p_desc.segment;
		report.segments.push_back(segment_report);
		// begin() adds its collision-resistant CREATE_NEW suffix. The base is
		// never published; a segment is always an uncommitted transaction.
		const String base = impl->scratch_dir.path_join("v4_" + String::num_uint64(p_desc.segment.bucket) + "_" + itos(p_desc.segment.depth));
		err = segment->file.begin(base);
		if (err != OK) {
			return _fail(err, "io");
		}
		report.segments[spill.segment].path = segment->file.get_temp_path();
		impl->segment_ids.insert(p_desc.segment, spill.segment);
	}
	Impl::Segment *segment = impl->segments[spill.segment];
	spill.record.segment_path = segment->file.get_temp_path();
	spill.record.offset = segment->file.get_size();
	if (impl->codec.codec_id == kCodecGsqV1) {
		err = _write(segment->file, page.ptr(), page.size(), true);
	} else {
		err = _write(segment->file, reinterpret_cast<const uint8_t *>(p_gaussians), g_bytes, true);
		if (err == OK && sh_bytes) {
			err = _write(segment->file, reinterpret_cast<const uint8_t *>(p_sh_high), sh_bytes, true);
		}
	}
	if (err != OK) {
		return err; // _fail may already have deleted segment.
	}
	const uint32_t index = impl->spills.size();
	if (!gs_resize_or_fail(impl->spills, uint64_t(index) + 1, "V4 spill records")) {
		return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_spill_table");
	}
	impl->spills.write[index] = spill;
	const uint64_t handle = allocate_body_handle();
	if (handle == UINT64_MAX) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_handle_exhausted");
	}
	impl->handles.insert(handle, index);
	++segment->remaining;
	V4SegmentReport &segment_report = report.segments[spill.segment];
	segment_report.spill_bytes = segment->file.get_size();
	++segment_report.body_count;
	++report.nodes;
	impl->nodes_done.set(report.nodes);
	impl->splats_done.add(p_count);
	if ((err = _check_stop()) != OK) {
		return err;
	}
	r_handle.value = handle;
	return OK;
}

Error WriterV4::finalize(const LocalVector<V4Node> &p_nodes) {
	using namespace gs_world_paged;
	using NodeRecord = GaussianSplatHlodPagedSource::NodeRecord;
	if (!impl->open) {
		return report.error != OK ? report.error : ERR_UNCONFIGURED;
	}
	Error err = _check_stop();
	if (err != OK || (err = _check_ram()) != OK) {
		return err;
	}
	impl->phase.set(uint32_t(Phase::FINALIZING));
	if (!p_nodes.size() || p_nodes.size() > kMaxNodes) {
		return _fail(ERR_INVALID_DATA, "v4_node_count_mismatch");
	}
	const uint32_t count = p_nodes.size();
	if ((err = _check_ram(0, count > uint64_t(impl->spills.size()) ? count - uint32_t(impl->spills.size()) : 0)) != OK) {
		return err;
	}
	Vector<uint8_t> used;
	Vector<uint32_t> spill_indices;
	Vector<uint32_t> depths;
	if (!gs_resize_or_fail(used, impl->spills.size(), "V4 handle use table") ||
			!gs_resize_or_fail(spill_indices, count, "V4 handle order") ||
			!gs_resize_or_fail(depths, count, "V4 depths")) {
		return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_node_table");
	}
	if (!used.is_empty()) {
		memset(used.ptrw(), 0, used.size());
	}
	for (uint32_t i = 0; i < count; ++i) {
		if ((err = _check_stop()) != OK) {
			return err;
		}
		const uint32_t *slot = impl->handles.getptr(p_nodes[i].body.value);
		if (!slot) {
			return _fail(ERR_INVALID_DATA, "v4_handle_unknown");
		}
		if (used[*slot]) {
			return _fail(ERR_INVALID_DATA, "v4_handle_reused");
		}
		used.write[*slot] = 1;
		spill_indices.write[i] = *slot;
	}
	for (uint8_t present : used) {
		if (!present) {
			return _fail(ERR_INVALID_DATA, "v4_handle_unused");
		}
	}
	if (count != impl->spec.node_count) {
		return _fail(ERR_INVALID_DATA, "v4_node_count_mismatch");
	}
	// Parent ordering is checked for the entire table before checking links,
	// so a child-before-parent error has its own stable refusal reason.
	for (uint32_t i = 0; i < count; ++i) {
		const GaussianSplatHlodNode &node = p_nodes[i].topology;
		if ((i == 0 && node.parent != gs_hlod::kNoParent) || (i != 0 && node.parent >= i)) {
			return _fail(ERR_INVALID_DATA, "v4_parent_not_before_child");
		}
	}
	uint64_t next_child = 1;
	for (uint32_t i = 0; i < count; ++i) {
		const GaussianSplatHlodNode &node = p_nodes[i].topology;
		if (node.child_count) {
			if (node.child_count > gs_hlod::kMaxChildren || node.first_child <= i || node.first_child > count ||
					node.child_count > count - node.first_child) {
				return _fail(ERR_INVALID_DATA, "v4_children_not_contiguous");
			}
			for (uint32_t c = node.first_child; c < node.first_child + node.child_count; ++c) {
				if (p_nodes[c].topology.parent != i) {
					return _fail(ERR_INVALID_DATA, "v4_children_not_contiguous");
				}
			}
			if (node.first_child != next_child) {
				return _fail(ERR_INVALID_DATA, "v4_not_bfs");
			}
			next_child += node.child_count;
		}
	}
	GaussianSplatHlodTree tree;
	for (int a = 0; a < 3; ++a) {
		tree.origin[a] = impl->spec.origin[a];
	}
	tree.bake_rule_version = impl->spec.bake_rule_version;
	tree.leaf_splat_count = impl->spec.leaf_splat_count;
	tree.interior_splat_count = impl->spec.interior_splat_count;
	tree.sh_high_order_count = impl->spec.sh_high_order;
	// LocalVector is the validator's existing table API. RAM was admitted above.
	tree.nodes.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		tree.nodes[i] = p_nodes[i].topology;
	}
	String reason;
	Error validation_error = OK;
	if (!gs_hlod_validate_tree(tree, &reason, &validation_error)) {
		return _fail(validation_error, "v4_invalid_topology: " + reason);
	}
	if ((err = _check_stop()) != OK) {
		return err;
	}
	Vector<NodeRecord> records;
	if (!gs_resize_or_fail(records, count, "V4 assembly node records")) {
		return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_node_table");
	}
	HeaderFields header;
	header.flags = impl->spec.is_2d ? kFlagIs2D : 0;
	header.leaf = impl->spec.leaf_splat_count;
	header.interior = impl->spec.interior_splat_count;
	header.node_count = count;
	header.sh_degree = impl->spec.sh_degree;
	header.sh_first_order = impl->spec.sh_first_order;
	header.sh_high_order = impl->spec.sh_high_order;
	header.bake_rule = impl->spec.bake_rule_version;
	for (int a = 0; a < 3; ++a) {
		header.origin[a] = impl->spec.origin[a];
	}
	header.bounds_pos = impl->spec.bounds.position;
	header.bounds_size = impl->spec.bounds.size;
	header.index_page_count = (uint64_t(count) + kRecordsPerIndexPage - 1) / kRecordsPerIndexPage;
	header.index_offset = kHeaderBytes;
	header.payload_offset = kHeaderBytes + header.index_page_count * kIndexPageBytes;
	uint64_t cursor = header.payload_offset;
	for (uint32_t i = 0; i < count; ++i) {
		if ((err = _check_stop()) != OK) {
			return err;
		}
		const V4SpillRecord &spill = impl->spills[spill_indices[i]].record;
		NodeRecord &record = records.write[i];
		record.topology = p_nodes[i].topology;
		record.is_leaf = record.topology.is_leaf();
		tree.node_cell_center(record.topology, record.cell_center);
		depths.write[i] = i == 0 ? 0 : depths[record.topology.parent] + 1;
		if (depths[i] != spill.desc.segment.depth) {
			return _fail(ERR_INVALID_DATA, "v4_depth_mismatch");
		}
		for (int a = 0; a < 3; ++a) {
			if (record.cell_center[a] != spill.desc.cell_center[a]) {
				return _fail(ERR_INVALID_DATA, "v4_cell_center_mismatch");
			}
		}
		if (record.is_leaf != spill.desc.is_leaf) {
			return _fail(ERR_INVALID_DATA, "v4_is_leaf_mismatch");
		}
		if (record.topology.payload_count != spill.splat_count) {
			return _fail(ERR_INVALID_DATA, "v4_payload_count_mismatch");
		}
		record.metrics = spill.metrics;
		uint64_t g_bytes, sh_bytes, rounded;
		if (!record_stored_bytes(spill.splat_count, header.sh_high_order, g_bytes, sh_bytes, record.stored_bytes) ||
				!add_checked(record.stored_bytes, kPageBytes - 1, rounded)) {
			return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
		}
		record.extent_bytes = rounded & ~(kPageBytes - 1);
		record.offset = cursor;
		if (!add_checked(cursor, record.extent_bytes, cursor)) {
			return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
		}
	}
	header.payload_bytes = cursor - header.payload_offset;
	if (!impl->metadata.is_empty()) {
		header.flags |= kFlagHasMetadata;
		header.metadata_offset = cursor;
		header.metadata_size = impl->metadata.size();
		hash_bytes(impl->metadata.ptr(), impl->metadata.size(), header.metadata_digest);
	}
	if (!add_checked(cursor, header.metadata_size, header.file_bytes)) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
	}
	if (header.file_bytes > impl->limits.max_output_bytes) {
		return _fail(ERR_PARAMETER_RANGE_ERROR, "limit_output");
	}
	// Same identity header and digest order as write_package; no spill-codec
	// identity enters this hash. Each output body digest was fixed at append.
	uint8_t header_page[kHeaderBytes];
	write_header(header_page, header);
	CryptoCore::SHA256Context generation;
	generation.start();
	generation.update(header_page, kHeaderBytes - 32);
	for (uint32_t i = 0; i < count; ++i) {
		generation.update(impl->spills[spill_indices[i]].record.output_body_digest, 32);
	}
	uint8_t digest[32];
	generation.finish(digest);
	memcpy(&header.content_generation, digest, sizeof(header.content_generation));
	if ((err = _check_stop()) != OK) {
		return err;
	}
	err = impl->output.begin(impl->final_path);
	impl->output_begun = true;
	if (err != OK) {
		return _fail(err, "io");
	}
	Vector<uint8_t> page;
	if (!gs_resize_or_fail(page, kIndexPageBytes, "V4 assembly index page")) {
		return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_index_page");
	}
	memset(page.ptrw(), 0, page.size());
	if ((err = _write(impl->output, page.ptr(), kHeaderBytes, false)) != OK) {
		return err;
	}
	for (uint64_t pi = 0; pi < header.index_page_count; ++pi) {
		if ((err = _write(impl->output, page.ptr(), page.size(), false)) != OK) {
			return err;
		}
	}
	for (uint32_t i = 0; i < count; ++i) {
		const Impl::Spill &entry = impl->spills[spill_indices[i]];
		const V4SpillRecord &spill = entry.record;
		Impl::Segment *segment = impl->segments[entry.segment];
		NodeRecord &record = records.write[i];
		uint64_t g_bytes, sh_bytes, total;
		if (!record_stored_bytes(spill.splat_count, header.sh_high_order, g_bytes, sh_bytes, total)) {
			return _fail(ERR_PARAMETER_RANGE_ERROR, "v4_size_overflow");
		}
		if ((err = _check_ram(8 * (g_bytes + sh_bytes) + kSliceBytes)) != OK) {
			return err;
		}
		Vector<Gaussian> body;
		Vector<Vector3> sh;
		if (!gs_resize_or_fail(body, spill.splat_count, "V4 assembly body") ||
				!gs_resize_or_fail(sh, uint64_t(spill.splat_count) * header.sh_high_order, "V4 assembly SH")) {
			return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_body");
		}
		if (impl->codec.codec_id == kCodecGsqV1) {
			Vector<uint8_t> encoded;
			if (!gs_resize_or_fail(encoded, spill.stored_bytes, "V4 assembly GSQ page")) {
				return _fail(ERR_OUT_OF_MEMORY, "v4_cannot_allocate_body");
			}
			if ((err = _read(segment->file, spill.offset, encoded.ptrw(), spill.stored_bytes)) != OK) {
				return err;
			}
			hash_bytes(encoded.ptr(), encoded.size(), digest);
			if (memcmp(digest, spill.stored_digest, sizeof(digest)) != 0) {
				return _fail(ERR_FILE_CORRUPT, "v4_spill_digest_mismatch");
			}
			gs_gsq::DecodeContext decoder;
			err = decoder.decode_range(encoded.ptr(), encoded.size(), 0, spill.splat_count, body.ptrw(), sh.ptrw(), reason);
			if (err != OK) {
				return _fail(err, reason);
			}
		} else {
			if ((err = _read(segment->file, spill.offset, reinterpret_cast<uint8_t *>(body.ptrw()), g_bytes)) != OK ||
					(err = _read(segment->file, spill.offset + g_bytes, reinterpret_cast<uint8_t *>(sh.ptrw()), sh_bytes)) != OK) {
				return err;
			}
			hash_body(body.ptr(), sh.ptr(), spill.splat_count, header.sh_high_order, digest);
			if (memcmp(digest, spill.stored_digest, sizeof(digest)) != 0) {
				return _fail(ERR_FILE_CORRUPT, "v4_spill_digest_mismatch");
			}
		}
		make_relative(body.ptrw(), spill.splat_count, spill.desc);
		hash_body(body.ptr(), sh.ptr(), spill.splat_count, header.sh_high_order, digest);
		if (memcmp(digest, spill.output_body_digest, sizeof(digest)) != 0) {
			return _fail(ERR_FILE_CORRUPT, "v4_output_body_digest_mismatch");
		}
		if (!validate_node_payload(record.cell_center, header.sh_high_order, record.topology, body.ptr(), sh.ptr(), reason)) {
			return _fail(ERR_INVALID_DATA, "v4_invalid_payload: " + reason);
		}
		uint8_t record_header[kPayloadRecordHeaderBytes];
		if (!write_payload_header(record_header, i, record, header.sh_high_order, header.content_generation) ||
				impl->output.get_size() != record.offset) {
			return _fail(ERR_BUG, "v4_payload_record_misplaced");
		}
		CryptoCore::SHA256Context record_hash;
		record_hash.start();
		record_hash.update(record_header, sizeof(record_header));
		record_hash.update(reinterpret_cast<const uint8_t *>(body.ptr()), g_bytes);
		if (sh_bytes) {
			record_hash.update(reinterpret_cast<const uint8_t *>(sh.ptr()), sh_bytes);
		}
		record_hash.finish(record.digest);
		if ((err = _write(impl->output, record_header, sizeof(record_header), false)) != OK ||
				(err = _write(impl->output, reinterpret_cast<const uint8_t *>(body.ptr()), g_bytes, false)) != OK ||
				(err = _write(impl->output, reinterpret_cast<const uint8_t *>(sh.ptr()), sh_bytes, false)) != OK ||
				(err = _write(impl->output, page.ptr(), record.extent_bytes - record.stored_bytes, false)) != OK) {
			return err;
		}
		if (--segment->remaining == 0) {
			impl->live_scratch -= segment->file.get_size();
			segment->file.abort();
			memdelete(segment);
			impl->segments[entry.segment] = nullptr;
			++report.segments_deleted;
		}
#ifdef TESTS_ENABLED
		if (impl->observer) {
			AssemblyEvent event;
			event.node_index = i;
			event.output_temp_path = impl->output.get_temp_path();
			event.output_bytes = impl->output.get_size();
			impl->observer(event, impl->observer_userdata);
		}
#endif
		if ((err = _check_stop()) != OK) {
			return err;
		}
	}
	if ((err = _write(impl->output, impl->metadata.ptr(), impl->metadata.size(), false)) != OK) {
		return err;
	}
	if (impl->output.get_size() != header.file_bytes) {
		return _fail(ERR_BUG, "v4_payload_record_size_inconsistent");
	}
	// Hash page digests incrementally; never keep a second index image in RAM.
	CryptoCore::SHA256Context index_hash;
	index_hash.start();
	for (uint64_t pi = 0; pi < header.index_page_count; ++pi) {
		if ((err = _check_stop()) != OK || (err = _check_ram()) != OK) {
			return err;
		}
		const uint64_t first = pi * kRecordsPerIndexPage;
		const uint32_t in_page = uint32_t(MIN<uint64_t>(kRecordsPerIndexPage, count - first));
		write_index_page(page.ptrw(), uint32_t(pi), records.ptr() + first, in_page);
		index_hash.update(page.ptr() + 32, 32);
		err = impl->output.write_at(header.index_offset + pi * kIndexPageBytes, page.ptr(), page.size());
		if (err != OK) {
			return _fail(err, "io");
		}
		impl->bytes_written.add(page.size()); // bounded by the admitted kMaxNodes index.
	}
	index_hash.finish(header.index_digest);
	write_header(header_page, header);
	if ((err = _check_stop()) != OK) {
		return err;
	}
	err = impl->output.write_at(0, header_page, sizeof(header_page));
	if (err != OK) {
		return _fail(err, "io");
	}
	impl->bytes_written.add(sizeof(header_page));
	err = impl->output.flush_to_storage();
	if (err != OK) {
		return _fail(err, "io");
	}
	if ((err = _check_stop()) != OK) {
		return err;
	}
	{
		// Same short replacement window as Writer::finalize. V4 readers currently
		// use transient handles; a still-open Windows handle fails replace safely.
		StagedFileChunkPayloadSource::ScopedReaderSuspend readers(impl->final_path);
		if (!readers.drained()) {
			return _fail(ERR_BUSY, "readers_busy");
		}
		if ((err = _check_stop()) != OK) {
			return err;
		}
		err = impl->output.commit();
	}
	if (err != OK) {
		return _fail(err, "io");
	}
	_cleanup();
	report.error = OK;
	report.reason = String();
	report.committed = true;
	impl->phase.set(uint32_t(Phase::COMMITTED));
	return OK;
}

} // namespace gs_world_writer
