#include "gaussian_splat_hlod_auto_tree.h"

#include "gaussian_splat_hlod_tiled_bake.h"
#include "gs_vector_alloc.h"
#include "streaming_chunk_payload_source.h"
#include "../io/gs_auto_tree_source.h"
#include "../io/gs_atomic_file_transaction.h"
#include "../io/gs_world_writer_v4.h"
#include "../io/gsq/gsq_codec.h"
#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/os/os.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// Keep native headers after engine headers (Windows macro pollution).
#if defined(WINDOWS_ENABLED)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstdio>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace gs_gsq {
// Shared with WriterV4; implemented beside the codec with its zstd headers.
uint64_t writer_v4_workspace_bytes(const EncodeParams &p_params);
}

namespace {

constexpr uint64_t kSlice = 1ull << 20;
constexpr uint64_t kManifestLimit = 16ull << 20;
// Covers hashing, input JSON and its parsed representation, and report strings.
constexpr uint64_t kBaseReservation = 128ull << 20;
std::atomic_flag job_busy = ATOMIC_FLAG_INIT;

struct JobLease {
	bool acquired = !job_busy.test_and_set();
	~JobLease() {
		if (acquired) {
			job_busy.clear();
		}
	}
};

struct Options {
	String scratch;
	String fingerprint;
	uint32_t workers = 1;
	uint32_t tile_splats = 1000000;
	gs_world_writer::CodecParams codec;
	gs_world_writer::Limits limits;
	uint64_t deadline = 0; // absolute ticks, unlike WriterV4's relative duration
};

Error refuse(String &r_reason, const String &p_reason, Error p_error = ERR_INVALID_PARAMETER) {
	r_reason = p_reason;
	return p_error;
}

Error writer_failure(const gs_world_writer::V4Report &p_report, Error p_error, String &r_reason) {
	return refuse(r_reason, p_report.full_reason.is_empty() ? p_report.reason : p_report.full_reason,
			p_report.reason == "limit_ram" ? ERR_OUT_OF_MEMORY : p_error);
}

bool digest_string(const String &p_value) {
	if (p_value.length() != 64) {
		return false;
	}
	for (int i = 0; i < 64; ++i) {
		const char32_t c = p_value[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

Error parse_options(const Dictionary &p_options, uint64_t p_started, Options &r_options, String &r_reason) {
	for (const Variant &key : p_options.keys()) {
		if (key.get_type() != Variant::STRING) {
			return refuse(r_reason, "unknown_option");
		}
		const String name = key;
		Variant::Type type;
		if (name == "scratch_dir" || name == "source_fingerprint" || name == "codec") {
			type = Variant::STRING;
		} else if (name == "worker_count" || name == "max_tile_splats") {
			type = Variant::INT;
		} else if (name == "limits") {
			type = Variant::DICTIONARY;
		} else {
			return refuse(r_reason, "unknown_option: " + name);
		}
		if (p_options[key].get_type() != type) {
			return refuse(r_reason, "option_type: " + name);
		}
	}
	const String codec = p_options.get("codec", "none");
	if (codec == "gsq") {
		r_options.codec = gs_world_writer::gsq_v1_codec_params();
	} else if (codec != "none") {
		return refuse(r_reason, "codec_invalid");
	}
	if (!p_options.has("source_fingerprint")) {
		return refuse(r_reason, "source_fingerprint_required");
	}
	r_options.fingerprint = p_options["source_fingerprint"];
	if (!digest_string(r_options.fingerprint)) {
		return refuse(r_reason, "source_fingerprint_invalid");
	}
	if (!p_options.has("scratch_dir") || String(p_options["scratch_dir"]).is_empty()) {
		return refuse(r_reason, "scratch_dir_required");
	}
	r_options.scratch = p_options["scratch_dir"];
	const int64_t workers = p_options.get("worker_count", 1);
	const int64_t tile = p_options.get("max_tile_splats", 1000000);
	if (workers < 1 || workers > 64) {
		return refuse(r_reason, "worker_count_invalid");
	}
	if (tile <= gs_hlod::kMaxNodeSplats || uint64_t(tile) > UINT32_MAX) {
		return refuse(r_reason, "max_tile_splats_invalid");
	}
	r_options.workers = uint32_t(workers);
	r_options.tile_splats = uint32_t(tile);
	if (!p_options.has("limits")) {
		return refuse(r_reason, "limits_required");
	}
	const Dictionary limits = p_options["limits"];
	for (const Variant &key : limits.keys()) {
		if (key.get_type() != Variant::STRING) {
			return refuse(r_reason, "unknown_limit");
		}
		const String name = key;
		if (name != "max_ram_bytes" && name != "max_scratch_bytes" && name != "max_output_bytes" && name != "deadline_sec") {
			return refuse(r_reason, "unknown_limit: " + name);
		}
		const Variant::Type type = limits[key].get_type();
		if (type != Variant::INT && !(name == "deadline_sec" && type == Variant::FLOAT)) {
			return refuse(r_reason, "option_type: limits." + name);
		}
	}
	for (const char *name : { "max_ram_bytes", "max_scratch_bytes", "max_output_bytes", "deadline_sec" }) {
		if (!limits.has(name)) {
			return refuse(r_reason, String("limits_required: ") + name);
		}
		if (double(limits[name]) <= 0.0 || !std::isfinite(double(limits[name]))) {
			return refuse(r_reason, String("limits_invalid: ") + name);
		}
	}
	const double duration = double(limits["deadline_sec"]) * 1000000.0;
	// Stay below INT64_MAX so conversion cannot overflow after double rounding.
	if (!std::isfinite(duration) || duration < 1 || duration >= double(INT64_MAX) || p_started > uint64_t(INT64_MAX) - uint64_t(duration)) {
		return refuse(r_reason, "limits_invalid: deadline_sec");
	}
	r_options.limits.max_ram_bytes = int64_t(limits["max_ram_bytes"]);
	r_options.limits.max_scratch_bytes = int64_t(limits["max_scratch_bytes"]);
	r_options.limits.max_output_bytes = int64_t(limits["max_output_bytes"]);
	r_options.limits.deadline_usec = uint64_t(duration);
	r_options.limits.min_free_disk_margin = gs_world_writer::kDefaultFreeDiskMargin;
	r_options.deadline = p_started + uint64_t(duration);
	return OK;
}

String native_path(const String &p_path) {
	return (ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path(p_path) : p_path).simplify_path();
}

String manifest_path(const String &p_path) {
	return p_path.get_basename() + ".manifest.json";
}

Error check_deadline(uint64_t p_deadline, String &r_reason) {
	return OS::get_singleton()->get_ticks_usec() >= p_deadline ? refuse(r_reason, "deadline", ERR_TIMEOUT) : OK;
}

Error hash_file(const String &p_path, uint64_t p_deadline, String &r_hash, String &r_reason) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return refuse(r_reason, "input_hash_open", ERR_FILE_CANT_OPEN);
	}
	Vector<uint8_t> bytes;
	if (!gs_resize_or_fail(bytes, kSlice, "gs_auto_tree hash_file buffer")) {
		return refuse(r_reason, "limit_ram: file_hash_buffer", ERR_OUT_OF_MEMORY);
	}
	CryptoCore::SHA256Context hash;
	hash.start();
	const uint64_t size = file->get_length();
	for (uint64_t offset = 0; offset < size;) {
		if (check_deadline(p_deadline, r_reason) != OK) {
			return ERR_TIMEOUT;
		}
		const uint64_t count = MIN(kSlice, size - offset);
		if (file->get_buffer(bytes.ptrw(), count) != count) {
			return refuse(r_reason, "file_hash_short_read", ERR_FILE_CORRUPT);
		}
		hash.update(bytes.ptr(), count);
		offset += count;
	}
	if (file->get_length() != size) {
		return refuse(r_reason, "file_changed", ERR_FILE_CORRUPT);
	}
	uint8_t digest[32];
	hash.finish(digest);
	r_hash = String::hex_encode_buffer(digest, 32);
	return OK;
}

void hash_payload(const Gaussian *p_g, const Vector3 *p_sh, uint32_t p_count, uint32_t p_high, uint8_t r_digest[32]) {
	CryptoCore::SHA256Context hash;
	hash.start();
	hash.update(reinterpret_cast<const uint8_t *>(p_g), uint64_t(p_count) * sizeof(Gaussian));
	if (p_high) {
		hash.update(reinterpret_cast<const uint8_t *>(p_sh), uint64_t(p_count) * p_high * sizeof(Vector3));
	}
	hash.finish(r_digest);
}

struct NodeDigest {
	uint8_t bytes[32] = {};
};

// Capture expected output BEFORE discarding the bake. The public GSQ spill
// digest hashes compressed scratch, not the final runtime payload (leaves use
// a separate node-relative page). Do not re-encode decoded scratch: that would
// quantize twice. All buffers here die before the next append or reopen.
Error gsq_node_digest(const Gaussian *p_g, const Vector3 *p_sh, uint32_t p_count, uint32_t p_degree,
		const gs_world_writer::V4BodyDesc &p_desc, const gs_gsq::EncodeParams &p_params,
		NodeDigest &r_digest, String &r_reason) {
	const uint64_t stored_limit = gs_world_paged::gsq_record_stored_bytes_limit(p_count, p_degree);
	if (!stored_limit) {
		return refuse(r_reason, "gsq_verify_layout", ERR_INVALID_DATA);
	}
	const uint32_t high = gs_gsq::sh_high_order_for_degree(p_degree);
	Vector<Gaussian> decoded;
	Vector<Vector3> sh;
	LocalVector<uint8_t> page;
	if (!gs_resize_or_fail(decoded, p_count, "gs_auto_tree GSQ verification gaussians") ||
			!gs_resize_or_fail(sh, uint64_t(p_count) * high, "gs_auto_tree GSQ verification SH") ||
			!gs_hlod::fallible_resize(page, stored_limit - gs_world_paged::kPayloadRecordHeaderBytes, "gs_auto_tree GSQ verification page")) {
		return refuse(r_reason, "limit_ram: gsq_verification", ERR_OUT_OF_MEMORY);
	}
	std::memcpy(decoded.ptrw(), p_g, uint64_t(p_count) * sizeof(Gaussian));
	if (p_desc.is_leaf) {
		Gaussian *g = decoded.ptrw();
		for (uint32_t i = 0; i < p_count; ++i) {
			for (uint32_t axis = 0; axis < 3; ++axis) {
				g[i].position[axis] = real_t(double(g[i].position[axis]) - p_desc.cell_center[axis]);
			}
		}
	}
	Error error = gs_gsq::encode_page(decoded.ptr(), p_sh, p_count, p_degree, p_params, page, r_reason);
	if (error != OK) {
		return error;
	}
	gs_gsq::DecodeContext decoder;
	error = decoder.decode_range(page.ptr(), page.size(), 0, p_count, decoded.ptrw(), sh.ptrw(), r_reason);
	if (error != OK) {
		return error;
	}
	if (p_desc.is_leaf) {
		gs_world_paged::reconstruct_leaf_positions(decoded.ptrw(), p_count, p_desc.cell_center);
	}
	gs_gsq::decoded_digest(decoded.ptr(), sh.ptr(), p_count, p_degree, r_digest.bytes);
	return OK;
}

bool same_metrics(const gs_world_paged::NodeMetrics &a, const gs_world_paged::NodeMetrics &b) {
	return a.footprint == b.footprint && a.dc_encoding == b.dc_encoding && a.center_exact == b.center_exact && a.homogeneous_dc == b.homogeneous_dc;
}

bool same_node(const GaussianSplatHlodNode &a, const GaussianSplatHlodNode &b, bool p_gsq, const double p_center[3]) {
	bool bounds_match = a.aabb_min == b.aabb_min && a.aabb_max == b.aabb_max && a.radius == b.radius;
	if (p_gsq) {
		// WriterV4 unions decoded support with bake bounds and propagates child
		// growth. A shifted midpoint must still contain the entire bake sphere.
		bounds_match = true;
		double motion_squared = 0;
		for (int axis = 0; axis < 3; ++axis) {
			bounds_match = bounds_match && a.aabb_min[axis] <= b.aabb_min[axis] && a.aabb_max[axis] >= b.aabb_max[axis];
			const double actual_mid = 0.5 * ((p_center[axis] + double(a.aabb_min[axis])) + (p_center[axis] + double(a.aabb_max[axis])));
			const double bake_mid = 0.5 * ((p_center[axis] + double(b.aabb_min[axis])) + (p_center[axis] + double(b.aabb_max[axis])));
			motion_squared += (actual_mid - bake_mid) * (actual_mid - bake_mid);
		}
		bounds_match = bounds_match && double(a.radius) >= double(b.radius) + std::sqrt(motion_squared);
	}
	return a.cell.e == b.cell.e && a.cell.ix == b.cell.ix && a.cell.iy == b.cell.iy && a.cell.iz == b.cell.iz &&
			bounds_match && a.geometric_error == b.geometric_error &&
			a.error_chosen == b.error_chosen && a.error_rejected == b.error_rejected && a.parent == b.parent &&
			a.first_child == b.first_child && a.child_count == b.child_count && a.height == b.height && a.payload_first == b.payload_first &&
			a.payload_count == b.payload_count && a.flags == b.flags && a.overlap_footprint == b.overlap_footprint;
}

void memory_report(Dictionary &r_report) {
	uint64_t peak = OS::get_singleton()->get_static_memory_peak_usage();
	String source = peak ? "godot_static" : "unavailable";
#if !defined(WINDOWS_ENABLED)
	struct rusage usage = {};
	if (::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
#if defined(__APPLE__)
		peak = uint64_t(usage.ru_maxrss);
#else
		peak = uint64_t(usage.ru_maxrss) * 1024;
#endif
		source = "process_rss";
	}
#endif
	r_report["process_peak_memory_available"] = peak > 0;
	r_report["process_peak_memory_bytes"] = int64_t(MIN(peak, uint64_t(INT64_MAX)));
	r_report["process_peak_memory_source"] = source;
}

// Unique directories own only this invocation's files. Never sweep the caller's
// scratch directory. WriterV4 is destroyed before these owners on every exit.
struct OwnedDirectory {
	String path;
	bool retain_for_recovery = false;
	Error create(const String &p_parent) {
		static std::atomic<uint64_t> sequence{ 0 };
		for (uint32_t attempt = 0; attempt < 64; ++attempt) {
			const String candidate = p_parent.path_join(".gs_auto_tree_" + itos(OS::get_singleton()->get_process_id()) + "_" +
					itos(OS::get_singleton()->get_ticks_usec()) + "_" + String::num_uint64(sequence.fetch_add(1)));
			const Error error = DirAccess::make_dir_absolute(candidate);
			if (error == OK) {
				path = candidate;
				return OK;
			}
			if (error != ERR_ALREADY_EXISTS) {
				return error;
			}
		}
		return ERR_ALREADY_EXISTS;
	}
	~OwnedDirectory() {
		if (path.is_empty() || retain_for_recovery) {
			return;
		}
		Ref<DirAccess> dir = DirAccess::open(path);
		if (dir.is_valid() && dir->list_dir_begin() == OK) {
			for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
				if (name != "." && name != ".." && !dir->current_is_dir()) {
					DirAccess::remove_absolute(path.path_join(name));
				}
			}
			dir->list_dir_end();
		}
		dir.unref();
		DirAccess::remove_absolute(path);
	}
};

// Hard-link backups preserve existing destinations until each atomic replace,
// without copying a potentially large old world or consuming extra data blocks.
// Refuse unsupported filesystems; never degrade to destructive delete-then-move.
Error backup_file(const String &p_from, const String &p_to) {
#if defined(WINDOWS_ENABLED)
	return CreateHardLinkW(reinterpret_cast<LPCWSTR>(p_to.utf16().get_data()), reinterpret_cast<LPCWSTR>(p_from.utf16().get_data()), nullptr) ? OK : ERR_CANT_CREATE;
#else
	return ::link(p_from.utf8().get_data(), p_to.utf8().get_data()) == 0 ? OK : ERR_CANT_CREATE;
#endif
}

Error replace_file(const String &p_from, const String &p_to) {
#if defined(WINDOWS_ENABLED)
	return MoveFileExW(reinterpret_cast<LPCWSTR>(p_from.utf16().get_data()), reinterpret_cast<LPCWSTR>(p_to.utf16().get_data()),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? OK : ERR_CANT_CREATE;
#else
	return ::rename(p_from.utf8().get_data(), p_to.utf8().get_data()) == 0 ? OK : ERR_CANT_CREATE;
#endif
}

Error sync_directory(const String &p_path) {
#if defined(WINDOWS_ENABLED)
	(void)p_path; // MoveFileExW above requests write-through.
	return OK;
#else
	const int fd = ::open(p_path.utf8().get_data(), O_RDONLY);
	if (fd < 0) {
		return ERR_FILE_CANT_OPEN;
	}
	const int synced = ::fsync(fd);
	const int closed = ::close(fd);
	return synced == 0 && closed == 0 ? OK : ERR_FILE_CANT_WRITE;
#endif
}

Error publish_pair(OwnedDirectory &p_stage, const String &p_output, const String &p_manifest, uint64_t p_deadline, String &r_reason) {
	const String destinations[2] = { p_output, p_manifest };
	const String staged[2] = { p_stage.path.path_join("world"), p_stage.path.path_join("manifest") };
	const String backups[2] = { p_stage.path.path_join("old_world"), p_stage.path.path_join("old_manifest") };
	bool backed[2] = {};
	bool replaced[2] = {};
	StagedFileChunkPayloadSource::ScopedReaderSuspend suspend(p_output);
	if (!suspend.drained()) {
		return refuse(r_reason, "publish_readers_busy", ERR_BUSY);
	}
	Error error = check_deadline(p_deadline, r_reason);
	for (uint32_t i = 0; error == OK && i < 2; ++i) {
		if (FileAccess::exists(destinations[i])) {
			error = backup_file(destinations[i], backups[i]);
			if (error != OK) {
				r_reason = "publish_backup";
			} else {
				backed[i] = true;
			}
		}
	}
	if (error == OK) {
		error = sync_directory(p_stage.path);
		if (error != OK) {
			r_reason = "publish_backup_sync";
		}
	}
	for (uint32_t i = 0; error == OK && i < 2; ++i) {
		error = check_deadline(p_deadline, r_reason);
		if (error == OK) {
			error = replace_file(staged[i], destinations[i]);
			if (error == OK) {
				replaced[i] = true;
			} else {
				r_reason = "publish_replace";
			}
		}
	}
	if (error == OK) {
		error = sync_directory(p_output.get_base_dir());
		if (error != OK) {
			r_reason = "publish_directory_sync";
		}
	}
	if (error != OK) {
		// Rollback never consults deadline: preserving old bytes takes precedence.
		bool restored = true;
		for (uint32_t i = 0; i < 2; ++i) {
			if (replaced[i]) {
				const Error undone = backed[i] ? replace_file(backups[i], destinations[i]) : DirAccess::remove_absolute(destinations[i]);
				restored = (undone == OK) && restored;
			}
		}
		if (!restored) {
			// A failing filesystem cannot promise rollback. Preserve the only old
			// copies and report their location; never clean them up as scratch.
			p_stage.retain_for_recovery = true;
			r_reason = "publish_rollback_failed: " + p_stage.path;
			return ERR_FILE_CANT_WRITE;
		}
		if (sync_directory(p_output.get_base_dir()) != OK) {
			p_stage.retain_for_recovery = true;
			r_reason = "publish_rollback_sync_failed: " + p_stage.path;
			return ERR_FILE_CANT_WRITE;
		}
	}
	return error;
}

Dictionary build_parameters(const Options &p_options) {
	const gs_hlod::InteriorBakeParams defaults;
	Dictionary interior;
	interior["merge_grid"] = defaults.merge_grid;
	interior["reduction"] = defaults.reduction;
	interior["node_max_splats"] = defaults.node_max_splats;
	interior["eps_growth"] = defaults.eps_growth;
	interior["max_eps_steps"] = defaults.max_eps_steps;
	Dictionary params;
	params["leaf_max"] = gs_hlod::kMaxNodeSplats;
	params["max_tile_splats"] = p_options.tile_splats;
	params["interior"] = interior;
	return params;
}

Error input_provenance(const String &p_input, uint64_t p_deadline, uint64_t p_parse_reservation, Dictionary &r_manifest, String &r_reason) {
	String hash;
	Error error = hash_file(p_input, p_deadline, hash, r_reason);
	if (error != OK) {
		return error;
	}
	r_manifest["input_file_sha256"] = hash;
	const String sidecar = manifest_path(p_input);
	if (!FileAccess::exists(sidecar)) {
		return OK;
	}
	Ref<FileAccess> file = FileAccess::open(sidecar, FileAccess::READ);
	if (file.is_null() || file->get_length() > kManifestLimit) {
		return refuse(r_reason, "input_manifest_size", ERR_INVALID_DATA);
	}
	const uint64_t size = file->get_length();
	if (size > p_parse_reservation / 128) {
		return refuse(r_reason, "limit_ram: input_manifest", ERR_OUT_OF_MEMORY);
	}
	Vector<uint8_t> bytes;
	if (!gs_resize_or_fail(bytes, size, "gs_auto_tree input_provenance buffer")) {
		return refuse(r_reason, "limit_ram: input_manifest_buffer", ERR_OUT_OF_MEMORY);
	}
	CryptoCore::SHA256Context sha;
	sha.start();
	for (uint64_t offset = 0; offset < size;) {
		if (check_deadline(p_deadline, r_reason) != OK) {
			return ERR_TIMEOUT;
		}
		const uint64_t count = MIN(kSlice, size - offset);
		if (file->get_buffer(bytes.ptrw() + offset, count) != count) {
			return refuse(r_reason, "input_manifest_read", ERR_FILE_CORRUPT);
		}
		sha.update(bytes.ptr() + offset, count);
		offset += count;
	}
	const Variant parsed = JSON::parse_string(String::utf8(reinterpret_cast<const char *>(bytes.ptr()), bytes.size()));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return refuse(r_reason, "input_manifest_invalid", ERR_INVALID_DATA);
	}
	uint8_t digest[32];
	sha.finish(digest);
	r_manifest["input_manifest_sha256"] = String::hex_encode_buffer(digest, 32);
	const Dictionary source = parsed;
	for (const char *name : { "gen_sha", "generator_params_sha" }) {
		if (source.has(name)) {
			if (source[name].get_type() != Variant::STRING || !digest_string(source[name])) {
				return refuse(r_reason, "input_manifest_digest_invalid", ERR_INVALID_DATA);
			}
			r_manifest[String("input_") + name] = source[name];
		}
	}
	return OK;
}

Error run_build(const String &p_input_path, const String &p_output_path, const Options &p_options, uint64_t p_started, Dictionary &r_report, String &r_reason) {
	const String input = native_path(p_input_path);
	const String output = native_path(p_output_path);
	const String manifest = native_path(manifest_path(p_output_path));
	const String scratch_root = native_path(p_options.scratch);
	if (p_input_path.is_empty() || !FileAccess::exists(input)) {
		return refuse(r_reason, "input_not_found", ERR_FILE_NOT_FOUND);
	}
	if (p_output_path.is_empty() || !input.is_absolute_path() || !output.is_absolute_path() || !scratch_root.is_absolute_path() ||
			input == output || input == manifest || output == manifest || manifest_path(input) == output || manifest_path(input) == manifest) {
		return refuse(r_reason, "path_invalid");
	}
	Ref<DirAccess> destination_dir = DirAccess::open(output.get_base_dir());
	if (destination_dir.is_null() || !DirAccess::dir_exists_absolute(scratch_root) ||
			DirAccess::dir_exists_absolute(output) || DirAccess::dir_exists_absolute(manifest) ||
			destination_dir->is_link(output.get_file()) || destination_dir->is_link(manifest.get_file())) {
		return refuse(r_reason, "destination_invalid", ERR_FILE_BAD_PATH);
	}
	// Include case aliases, hard links and aliases through parent directories.
	if (destination_dir->is_equivalent(input, output) || destination_dir->is_equivalent(input, manifest) ||
			destination_dir->is_equivalent(manifest_path(input), output) || destination_dir->is_equivalent(manifest_path(input), manifest) ||
			destination_dir->is_equivalent(output, manifest)) {
		return refuse(r_reason, "path_invalid");
	}
	if (p_options.limits.max_ram_bytes <= kBaseReservation) {
		return refuse(r_reason, "limit_ram", ERR_OUT_OF_MEMORY);
	}
	uint64_t base_reserved = kBaseReservation;
	if (FileAccess::exists(manifest_path(input))) {
		Ref<FileAccess> sidecar = FileAccess::open(manifest_path(input), FileAccess::READ);
		if (sidecar.is_null() || sidecar->get_length() > kManifestLimit) {
			return refuse(r_reason, "input_manifest_size", ERR_INVALID_DATA);
		}
		// The JSON DOM can be much larger than its encoded text. Reserve its
		// transient expansion for both provenance reads, not just the file bytes.
		base_reserved = MAX(base_reserved, sidecar->get_length() * 128 + 4 * kSlice);
	}
	if (base_reserved >= p_options.limits.max_ram_bytes) {
		return refuse(r_reason, "limit_ram: input_manifest", ERR_OUT_OF_MEMORY);
	}
	r_report["peak_reserved_bytes"] = int64_t(base_reserved);
	Dictionary document;
	Error error = input_provenance(input, p_options.deadline, base_reserved - 4 * kSlice, document, r_reason);
	if (error != OK) {
		return error;
	}
	gs_hlod::BakeResult result;
	gs_hlod::TiledBakeReport tiled_report;
	gs_world_writer::PagedV4Spec spec;
	{
		GsqAutoTreeSource source;
		error = source.open(input, &r_reason, p_options.limits.max_ram_bytes - base_reserved);
		r_report["peak_reserved_bytes"] = int64_t(base_reserved + source.get_open_peak_reserved_bytes());
		if (error != OK) {
			if (error == ERR_OUT_OF_MEMORY) {
				r_reason = "limit_ram: " + r_reason;
			}
			return error;
		}
		const auto info = source.get_info();
		const uint64_t source_reserved = source.get_reserved_bytes() + uint64_t(p_options.workers) * source.get_decode_scratch_bytes();
		if (source_reserved >= p_options.limits.max_ram_bytes - base_reserved) {
			return refuse(r_reason, "limit_ram", ERR_OUT_OF_MEMORY);
		}
		gs_hlod::TiledBakeParams params;
		params.worker_count = p_options.workers;
		params.max_tile_splats = p_options.tile_splats;
		params.limits.max_ram_bytes = p_options.limits.max_ram_bytes - base_reserved - source_reserved;
		params.limits.deadline_usec = p_options.deadline;
		error = gs_hlod::TiledAutoTree::build(source, params, result, tiled_report, &r_reason);
		const auto stats = source.get_stats();
		r_report["pages_decoded"] = int64_t(stats.pages_decoded);
		r_report["pages_decoded_digest_checked"] = int64_t(stats.pages_decoded_digest_checked);
		r_report["peak_reserved_bytes"] = int64_t(MAX(uint64_t(int64_t(r_report["peak_reserved_bytes"])), base_reserved + source_reserved + tiled_report.peak_reserved_bytes));
		r_report["tiles_baked"] = tiled_report.tiles_baked;
		r_report["stitch_levels"] = tiled_report.stitch_levels;
		if (error != OK) {
			if (error == ERR_OUT_OF_MEMORY) {
				r_reason = "limit_ram";
			} else if (stats.failures > 0) {
				// TiledAutoTree intentionally maps source I/O errors to INVALID_DATA.
				// Restore the adapter's integrity diagnosis at the file API boundary.
				r_reason = stats.last_reason;
				error = ERR_FILE_CORRUPT;
			}
			return error;
		}
		spec.sh_degree = source.get_sh_degree();
		spec.sh_first_order = info.sh_first_order_count;
		spec.sh_high_order = info.sh_high_order_count;
		spec.is_2d = source.is_2d();
		spec.bounds = source.get_bounds();
	} // No source directory or decoder survives the bake. Chunks are owned per read.
	if (check_deadline(p_options.deadline, r_reason) != OK) {
		return ERR_TIMEOUT;
	}
	const auto &tree = result.tree;
	spec.node_count = tree.nodes.size();
	spec.leaf_splat_count = tree.leaf_splat_count;
	spec.interior_splat_count = tree.interior_splat_count;
	spec.bake_rule_version = tree.bake_rule_version;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		spec.origin[axis] = tree.origin[axis];
	}
	// D2-3 deliberately retains the complete BakeResult (T0 scale). Account its
	// actual capacities in addition to writer buffers, then release all splats as
	// soon as every node has been spilled. No second full-world decoded copy.
	const uint64_t payload_reserved = uint64_t(result.leaf_gaussians.get_capacity() + uint64_t(tree.interior_gaussians.get_capacity())) * sizeof(Gaussian) +
			uint64_t(result.leaf_sh_high_order.get_capacity() + uint64_t(tree.interior_sh_high_order.get_capacity())) * sizeof(Vector3) +
			uint64_t(result.leaf_source_index.get_capacity()) * sizeof(uint32_t);
	// Includes node tables, manifest dictionaries/strings, verification arrays and
	// tree validation scratch. Writer has its own separately enforced reservation.
	const uint64_t metadata_reserved = base_reserved + uint64_t(spec.node_count) * 16384;
	const uint64_t max_body = uint64_t(gs_hlod::kMaxNodeSplats) * (sizeof(Gaussian) + uint64_t(spec.sh_high_order) * sizeof(Vector3));
	uint64_t writer_reserved = (16ull << 20) + 16 * max_body + uint64_t(spec.node_count) *
			(16384 + 64 * uint64_t(output.length() + scratch_root.length() + 256));
	const bool gsq = p_options.codec.codec_id == gs_world_writer::kCodecGsqV1;
	gs_gsq::EncodeParams encode_params;
	uint64_t verification_reserved = 0;
	if (gsq) {
		error = gs_world_writer::gsq_encode_params(p_options.codec, encode_params, r_reason);
		if (error != OK) {
			return error;
		}
		const uint64_t codec_workspace = gs_gsq::writer_v4_workspace_bytes(encode_params);
		const uint64_t max_stored = gs_world_paged::gsq_record_stored_bytes_limit(gs_hlod::kMaxNodeSplats, spec.sh_degree);
		if (!max_stored) {
			return refuse(r_reason, "gsq_verify_layout", ERR_INVALID_DATA);
		}
		// Independent per-node encode/decode plus reopen/readback: allow capacity
		// growth and allocation probes for decoded arrays, raw planes, log scales
		// and stored page, plus one retained reader decoder and the digest table.
		// Do not borrow WriterV4's reservation: it still owns metadata/spills.
		const uint64_t verify_buffers = 12 * max_body + 2 * max_stored +
				gs_world_paged::gsq_decode_workspace_bytes() + 2 * uint64_t(spec.node_count) * sizeof(NodeDigest);
		if (codec_workspace > p_options.limits.max_ram_bytes ||
				verify_buffers > p_options.limits.max_ram_bytes - codec_workspace ||
				writer_reserved > p_options.limits.max_ram_bytes - codec_workspace) {
			return refuse(r_reason, "limit_ram: gsq_verification", ERR_OUT_OF_MEMORY);
		}
		verification_reserved = codec_workspace + verify_buffers;
		writer_reserved += codec_workspace;
	}
	const uint64_t resident_reserved = metadata_reserved + payload_reserved;
	if (resident_reserved > p_options.limits.max_ram_bytes || writer_reserved > p_options.limits.max_ram_bytes - resident_reserved ||
			verification_reserved > p_options.limits.max_ram_bytes - resident_reserved - writer_reserved) {
		return refuse(r_reason, "limit_ram", ERR_OUT_OF_MEMORY);
	}
	const uint64_t writer_phase_reserved = resident_reserved + writer_reserved + verification_reserved;
	r_report["peak_reserved_bytes"] = int64_t(MAX(uint64_t(int64_t(r_report["peak_reserved_bytes"])), writer_phase_reserved));
	OwnedDirectory stage;
	OwnedDirectory scratch;
	if (stage.create(output.get_base_dir()) != OK || scratch.create(scratch_root) != OK) {
		return refuse(r_reason, "staging_create", ERR_CANT_CREATE);
	}
	gs_world_writer::WriterV4 writer;
	gs_world_writer::Limits writer_limits = p_options.limits;
	writer_limits.max_ram_bytes = writer_reserved;
	const uint64_t writer_start = OS::get_singleton()->get_ticks_usec();
	if (writer_start >= p_options.deadline) {
		r_reason = "deadline";
		return ERR_TIMEOUT;
	}
	writer_limits.deadline_usec = p_options.deadline - writer_start;
	const String staged_world = stage.path.path_join("world");
	error = writer.begin_paged(staged_world, scratch.path, spec, p_options.codec, writer_limits);
	if (error != OK) {
		return writer_failure(writer.get_report(), error, r_reason);
	}
	LocalVector<gs_world_writer::V4Node> nodes;
	LocalVector<uint32_t> depths;
	Vector<NodeDigest> decoded_digests;
	if (!gs_hlod::fallible_resize(nodes, spec.node_count, "gs_auto_tree writer nodes") ||
			!gs_hlod::fallible_resize(depths, spec.node_count, "gs_auto_tree node depths") ||
			(gsq && !gs_resize_or_fail(decoded_digests, spec.node_count, "gs_auto_tree GSQ node digests"))) {
		return refuse(r_reason, "limit_ram: node_tables", ERR_OUT_OF_MEMORY);
	}
	std::vector<std::vector<double>> errors;
	for (uint32_t i = 0; i < spec.node_count; ++i) {
		if (check_deadline(p_options.deadline, r_reason) != OK) {
			return ERR_TIMEOUT;
		}
		const auto &node = tree.nodes[i];
		if (i > 0 && node.parent >= i) {
			return refuse(r_reason, "v4_bake_parent_order", ERR_INVALID_DATA);
		}
		depths[i] = i == 0 ? 0 : depths[node.parent] + 1;
		gs_world_writer::V4BodyDesc desc;
		desc.segment.depth = depths[i];
		// The assembled result has no frontier IDs. One canonical root bucket
		// with a segment per depth preserves topology and bounds open handles.
		desc.segment.bucket = 0;
		desc.is_leaf = node.is_leaf();
		tree.node_cell_center(node, desc.cell_center);
		const uint64_t start = node.is_leaf() ? node.payload_first : node.payload_first - tree.leaf_splat_count;
		const Gaussian *g = node.is_leaf() ? result.leaf_gaussians.ptr() + start : tree.interior_gaussians.ptr() + start;
		const Vector3 *sh = nullptr;
		if (spec.sh_high_order) {
			sh = node.is_leaf() ? result.leaf_sh_high_order.ptr() + start * spec.sh_high_order : tree.interior_sh_high_order.ptr() + start * spec.sh_high_order;
		}
		nodes[i].topology = node;
		error = writer.append_node(desc, g, sh, node.payload_count, nodes[i].body);
		if (error != OK) {
			return writer_failure(writer.get_report(), error, r_reason);
		}
		if (gsq) {
			error = gsq_node_digest(g, sh, node.payload_count, spec.sh_degree, desc, encode_params, decoded_digests.write[i], r_reason);
			if (error != OK) {
				return error;
			}
		}
		if (errors.size() <= depths[i]) {
			errors.resize(depths[i] + 1);
		}
		errors[depths[i]].push_back(node.geometric_error);
	}
	result = gs_hlod::BakeResult();
	error = writer.finalize(nodes);
	if (error != OK) {
		return writer_failure(writer.get_report(), error, r_reason);
	}
	Ref<GaussianSplatHlodPagedSource> reopened;
	reopened.instantiate();
	GaussianSplatHlodTree reopened_tree;
	Dictionary metadata;
	error = reopened->open(staged_world, reopened_tree, metadata, r_reason);
	if (error != OK) {
		return error;
	}
	const auto &info = reopened->get_info();
	if (info.codec_id != p_options.codec.codec_id || info.node_count != spec.node_count || info.leaf_splat_count != spec.leaf_splat_count || info.interior_splat_count != spec.interior_splat_count ||
			info.sh_high_order != spec.sh_high_order || info.sh_first_order != spec.sh_first_order || info.bake_rule_version != spec.bake_rule_version ||
			info.sh_degree != spec.sh_degree || info.is_2d != spec.is_2d || info.bounds != spec.bounds ||
			std::memcmp(reopened_tree.origin, spec.origin, sizeof(spec.origin)) != 0) {
		return refuse(r_reason, "v4_reopen_identity", ERR_FILE_CORRUPT);
	}
	Array manifest_nodes;
	manifest_nodes.resize(spec.node_count);
	for (uint32_t i = 0; i < spec.node_count; ++i) {
		if (check_deadline(p_options.deadline, r_reason) != OK) {
			return ERR_TIMEOUT;
		}
		const auto &record = reopened->get_nodes()[i];
		gs_world_writer::V4SpillRecord spill;
		error = writer.get_spill_record(nodes[i].body, spill);
		if (error != OK) {
			return refuse(r_reason, "v4_reopen_spill_missing", error);
		}
		if (!same_node(record.topology, nodes[i].topology, gsq, spill.desc.cell_center) || record.is_leaf != spill.desc.is_leaf || record.topology.payload_count != spill.splat_count ||
				std::memcmp(record.cell_center, spill.desc.cell_center, sizeof(record.cell_center)) != 0) {
			return refuse(r_reason, "v4_reopen_topology", ERR_FILE_CORRUPT);
		}
		Vector<Gaussian> gaussians;
		Vector<Vector3> sh;
		// The reader also applies validate_node_payload to decoded support against
		// the final AABB/sphere, with its unchanged half-ULP tolerance.
		error = reopened->read_node_payload(i, gaussians, sh, r_reason);
		if (error != OK) {
			return error;
		}
		gs_world_paged::NodeMetrics metrics;
		if (!gs_world_paged::compute_node_metrics(gaussians.ptr(), gaussians.size(), record.cell_center, record.is_leaf, metrics, r_reason) ||
				!same_metrics(metrics, record.metrics) || !same_metrics(metrics, spill.metrics)) {
			return refuse(r_reason, "v4_reopen_metrics", ERR_FILE_CORRUPT);
		}
		uint8_t digest[32];
		if (gsq) {
			gs_gsq::decoded_digest(gaussians.ptr(), sh.ptr(), spill.splat_count, spec.sh_degree, digest);
		} else {
			// Preserve codec-none's existing raw-byte digest and manifest identity.
			hash_payload(gaussians.ptr(), sh.ptr(), spill.splat_count, spec.sh_high_order, digest);
		}
		const uint8_t *expected_digest = gsq ? decoded_digests[i].bytes : spill.stored_digest;
		if (std::memcmp(digest, expected_digest, 32) != 0) {
			return refuse(r_reason, "v4_reopen_runtime_digest", ERR_FILE_CORRUPT);
		}
		Dictionary entry;
		entry["decoded_sha256"] = String::hex_encode_buffer(digest, 32);
		entry["stored_sha256"] = String::hex_encode_buffer(record.digest, 32);
		manifest_nodes[i] = entry;
		if (!gsq) {
			// Codec none additionally audits the raw node-relative spill bytes.
			// GSQ's legacy output_body_digest describes decoded absolute scratch
			// converted to relative positions, not the separately encoded output.
			// read_node_payload verified its stored record SHA; above we compared
			// canonical output against the independently encoded original bake.
			if (record.is_leaf) {
				Gaussian *g = gaussians.ptrw();
				for (uint32_t j = 0; j < spill.splat_count; ++j) {
					for (uint32_t axis = 0; axis < 3; ++axis) {
						g[j].position[axis] = float(double(g[j].position[axis]) - record.cell_center[axis]);
					}
				}
			}
			hash_payload(gaussians.ptr(), sh.ptr(), spill.splat_count, spec.sh_high_order, digest);
			if (std::memcmp(digest, spill.output_body_digest, 32) != 0) {
				return refuse(r_reason, "v4_reopen_stored_body_digest", ERR_FILE_CORRUPT);
			}
		}
		r_report["nodes_verified"] = i + 1;
	}
	document["schema"] = "godotgs-auto-tree-manifest/1";
	document["nodes"] = manifest_nodes;
	document["content_generation"] = String::num_uint64(info.content_generation);
	Dictionary totals;
	totals["node_count"] = spec.node_count;
	totals["leaf_splat_count"] = spec.leaf_splat_count;
	totals["interior_splat_count"] = spec.interior_splat_count;
	document["totals"] = totals;
	Array levels;
	for (uint32_t depth = 0; depth < errors.size(); ++depth) {
		auto &eps = errors[depth];
		std::sort(eps.begin(), eps.end());
		Dictionary level;
		level["depth"] = depth;
		level["node_count"] = int64_t(eps.size());
		level["eps_min"] = eps.front();
		level["eps_max"] = eps.back();
		level["eps_median"] = (eps[(eps.size() - 1) / 2] + eps[eps.size() / 2]) / 2.0;
		levels.push_back(level);
	}
	document["levels"] = levels;
	String output_hash;
	error = hash_file(staged_world, p_options.deadline, output_hash, r_reason);
	if (error != OK) {
		return error;
	}
	document["output_file_sha256"] = output_hash;
	// Detect replacements/mutations across the whole multi-pass build, including
	// headers or directory bytes that would not be touched by a page read.
	Dictionary final_input;
	error = input_provenance(input, p_options.deadline, base_reserved - 4 * kSlice, final_input, r_reason);
	if (error != OK) {
		return error;
	}
	for (const char *key : { "input_file_sha256", "input_manifest_sha256", "input_gen_sha", "input_generator_params_sha" }) {
		if (document.get(key, Variant()) != final_input.get(key, Variant())) {
			return refuse(r_reason, "input_changed", ERR_FILE_CORRUPT);
		}
	}
	Dictionary codec;
	codec["codec"] = gsq ? "gsq" : "none";
	if (gsq) {
		codec["codec_id"] = int64_t(p_options.codec.codec_id);
		Array bits;
		for (uint8_t value : p_options.codec.sh_bits) {
			bits.push_back(int64_t(value));
		}
		codec["sh_bits"] = bits;
		codec["opacity_bits"] = int64_t(p_options.codec.opacity_bits);
		codec["pos_step_m"] = double(p_options.codec.pos_step_m);
		codec["zstd_level"] = int64_t(p_options.codec.zstd_level);
	}
	Dictionary params = build_parameters(p_options);
	Dictionary key;
	key["source_fingerprint"] = p_options.fingerprint;
	key["build_params"] = params;
	key["codec"] = codec;
	key["input_file_sha256"] = document["input_file_sha256"];
	const String tree_sha = JSON::stringify(key, "", true, true).sha256_text();
	params["worker_count"] = p_options.workers; // Excluded from the key above.
	document["source_fingerprint"] = p_options.fingerprint;
	document["build_params"] = params;
	document["codec"] = codec;
	document["tree_sha"] = tree_sha;
	r_report["error"] = OK;
	r_report["reason"] = String();
	r_report["tree_sha"] = tree_sha;
	r_report["manifest_path"] = manifest_path(p_output_path);
	r_report["node_count"] = spec.node_count;
	// The same report is returned and persisted. wall_usec ends at verified
	// staging, before JSON serialization/durable publication (no self-reference).
	r_report["wall_usec"] = int64_t(MAX(uint64_t(1), OS::get_singleton()->get_ticks_usec() - p_started));
	memory_report(r_report);
	document["report"] = r_report;
	CharString json;
	if (gsq) {
		// JSON::stringify(full_precision=true) emits fixed-point decimals, but
		// String::to_float's parser keeps only 18 mantissa digits INCLUDING leading
		// zeroes. For pos_step_m this loses bits even with full-precision output.
		// Emit this schema FLOAT as a normalized scientific JSON number instead.
		// Keep the hash key's numeric value/type and codec-none serialization intact.
		char step_text[32];
		const double step = double(p_options.codec.pos_step_m);
		const int step_length = std::snprintf(step_text, sizeof(step_text), "%.16e", step);
		if (step_length <= 0 || step_length >= int(sizeof(step_text)) || String::to_float(step_text) != step) {
			return refuse(r_reason, "manifest_codec_precision", ERR_INVALID_DATA);
		}
		// Serialize the two objects separately to insert a numeric token without
		// replacing text that could also occur in a user-supplied string value.
		Dictionary persisted_codec = codec.duplicate();
		persisted_codec.erase("pos_step_m");
		const String codec_json = JSON::stringify(persisted_codec, "", true, true).trim_suffix("}") +
				",\"pos_step_m\":" + String(step_text) + "}";
		Dictionary persisted_document = document.duplicate();
		persisted_document.erase("codec");
		json = (JSON::stringify(persisted_document, "", true, true).trim_suffix("}") + ",\"codec\":" + codec_json + "}").utf8();
	} else {
		json = JSON::stringify(document, "", true, true).utf8();
	}
	if (uint64_t(json.length()) > p_options.limits.max_scratch_bytes) {
		return refuse(r_reason, "limit_scratch", ERR_PARAMETER_RANGE_ERROR);
	}
	if (uint64_t(json.length()) > kManifestLimit || writer.get_report().output_bytes > p_options.limits.max_output_bytes ||
			uint64_t(json.length()) > p_options.limits.max_output_bytes - writer.get_report().output_bytes) {
		return refuse(r_reason, "limit_output", ERR_PARAMETER_RANGE_ERROR);
	}
	GSAtomicFileTransaction manifest_transaction;
	error = manifest_transaction.begin(stage.path.path_join("manifest"));
	for (uint64_t offset = 0; error == OK && offset < uint64_t(json.length());) {
		error = check_deadline(p_options.deadline, r_reason);
		if (error != OK) {
			break;
		}
		const uint64_t count = MIN(kSlice, uint64_t(json.length()) - offset);
		error = manifest_transaction.append(reinterpret_cast<const uint8_t *>(json.get_data()) + offset, count);
		offset += count;
	}
	if (error == OK) {
		error = manifest_transaction.commit();
	}
	if (error != OK) {
		if (r_reason.is_empty()) {
			r_reason = "manifest_write";
		}
		return error;
	}
	r_reason = String();
	return publish_pair(stage, output, manifest, p_options.deadline, r_reason);
}

} // namespace

void GaussianSplatHlodAutoTree::_bind_methods() {
	ClassDB::bind_static_method("GaussianSplatHlodAutoTree", D_METHOD("build", "input_path", "output_path", "options"), &GaussianSplatHlodAutoTree::build);
}

Dictionary GaussianSplatHlodAutoTree::build(const String &p_input_path, const String &p_output_path, const Dictionary &p_options) {
	const uint64_t started = OS::get_singleton()->get_ticks_usec();
	Dictionary report;
	report["error"] = ERR_UNAVAILABLE;
	report["reason"] = String();
	report["tree_sha"] = String();
	report["manifest_path"] = String();
	report["node_count"] = 0;
	report["nodes_verified"] = 0;
	report["wall_usec"] = 0;
	report["peak_reserved_bytes"] = 0;
	report["tiles_baked"] = 0;
	report["stitch_levels"] = 0;
	Options options;
	String reason;
	Error error = parse_options(p_options, started, options, reason);
	JobLease lease;
	if (error == OK && !lease.acquired) {
		error = refuse(reason, "auto_tree_busy", ERR_BUSY);
	}
	report["worker_count"] = options.workers;
	if (error == OK) {
		error = run_build(p_input_path, p_output_path, options, started, report, reason);
	}
	if (error != OK) {
		report["wall_usec"] = int64_t(OS::get_singleton()->get_ticks_usec() - started);
		memory_report(report);
	}
	report["error"] = error;
	report["reason"] = reason;
	return report;
}
