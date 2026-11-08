#include "gs_world_writer.h"

#include "gaussian_splat_world_format.h"
#include "gsq/gsq_page_builder.h"

#include "../core/streaming_chunk_payload_source.h"

#include "core/io/json.h"
#include "core/variant/variant.h"

#include <cstring>

#ifdef TESTS_ENABLED
gs_world_writer::Writer::DrainTestHook gs_world_writer::Writer::drain_test_hook = nullptr;
void *gs_world_writer::Writer::drain_test_userdata = nullptr;
#endif

namespace gs_world_writer {

namespace {

// Same bounds the plain loader enforces (gaussian_splat_world_io.cpp): degree <= 4,
// at most 3 first-order coefficients (Gaussian::sh_1), and at most UINT32_MAX
// high-order coefficients in total (#582). W11: refuse at begin what the reader
// would refuse at load.
constexpr uint32_t kMaxShDegree = 4u;
constexpr uint32_t kMaxShFirstOrder = 3u;

// W5 / W10: every payload write and copy goes through slices of at most 1 MiB, the
// granularity at which BV-03b observes cancellation.
constexpr uint64_t kSliceBytes = uint64_t(1) << 20u;

inline void _put_u32(uint8_t *p_dst, uint32_t p_value) {
	p_dst[0] = uint8_t(p_value & 0xFFu);
	p_dst[1] = uint8_t((p_value >> 8u) & 0xFFu);
	p_dst[2] = uint8_t((p_value >> 16u) & 0xFFu);
	p_dst[3] = uint8_t((p_value >> 24u) & 0xFFu);
}

inline void _put_u64(uint8_t *p_dst, uint64_t p_value) {
	_put_u32(p_dst, uint32_t(p_value & 0xFFFFFFFFu));
	_put_u32(p_dst + 4, uint32_t(p_value >> 32u));
}

// FileAccess::store_float semantics: the value narrowed to f32, stored little-endian.
inline void _put_f32(uint8_t *p_dst, real_t p_value) {
	const float f = float(p_value);
	uint32_t bits = 0;
	memcpy(&bits, &f, sizeof(bits));
	_put_u32(p_dst, bits);
}

inline void _put_vec3(uint8_t *p_dst, const Vector3 &p_value) {
	_put_f32(p_dst, p_value.x);
	_put_f32(p_dst + 4, p_value.y);
	_put_f32(p_dst + 8, p_value.z);
}

bool _mul_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_out) {
	if (p_a != 0u && p_b > UINT64_MAX / p_a) {
		return false;
	}
	r_out = p_a * p_b;
	return true;
}

bool _add_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_out) {
	if (p_b > UINT64_MAX - p_a) {
		return false;
	}
	r_out = p_a + p_b;
	return true;
}

} // namespace

CodecParams gsq_v1_codec_params() {
	const gs_gsq::EncodeParams defaults;
	CodecParams codec;
	codec.codec_id = kCodecGsqV1;
	for (int i = 0; i < 3; i++) {
		codec.sh_bits[i] = defaults.sh_bits[i];
	}
	codec.opacity_bits = defaults.opacity_bits;
	codec.pos_step_m = defaults.pos_step_m;
	codec.zstd_level = defaults.zstd_level;
	return codec;
}

Error gsq_encode_params(const CodecParams &p_codec, gs_gsq::EncodeParams &r_params, String &r_reason) {
	if (p_codec.codec_id != kCodecGsqV1) {
		r_reason = vformat("gsq_invalid_params: codec_id %d is not GSQ v1", p_codec.codec_id);
		return ERR_INVALID_PARAMETER;
	}
	gs_gsq::EncodeParams params;
	for (int i = 0; i < 3; i++) {
		params.sh_bits[i] = p_codec.sh_bits[i];
	}
	params.opacity_bits = p_codec.opacity_bits;
	params.pos_step_m = p_codec.pos_step_m;
	params.zstd_level = p_codec.zstd_level;
	const Error err = gs_gsq::validate_encode_params(params, r_reason);
	if (err != OK) {
		return err;
	}
	r_params = params;
	return OK;
}

Writer::~Writer() {
	abort();
}

void Writer::_cleanup() {
	output.abort();
	sh_scratch.abort();
	chunk_records.clear();
	chunk_counts.clear();
	metadata_bytes.clear();
	page_records.clear();
	built_pages.clear();
	if (page_builder != nullptr) {
		memdelete(page_builder);
		page_builder = nullptr;
	}
	pass = kNoPass;
}

Error Writer::_fail(Error p_error, const String &p_message) {
	_cleanup();
	error = p_error;
	error_message = "gs_world_writer: " + p_message + (final_path.is_empty() ? String() : " ('" + final_path + "')");
	state = State::FAILED;
	return p_error;
}

void Writer::abort() {
	if (state != State::OPEN) {
		return;
	}
	_cleanup();
	state = State::ABORTED;
}

uint32_t Writer::current_pass() const {
	return state == State::OPEN ? pass : kNoPass;
}

Error Writer::begin_plain(const String &p_final_path, const PlainSpec &p_spec, const CodecParams &p_codec, SourceMode p_mode) {
	if (state == State::OPEN) {
		return ERR_ALREADY_IN_USE;
	}
	state = State::IDLE;
	error = OK;
	error_message = String();
	final_path = p_final_path;
	spec = p_spec;
	mode = p_mode;
	layout = Layout();
	metadata_bytes.clear();
	chunk_records.clear();
	chunk_counts.clear();
	pass = kNoPass;
	pass_count = 0;
	chunks_this_pass = 0;
	splats_this_pass = 0;
	gsq = false;
	codec = p_codec;
	page_records.clear();
	built_pages.clear();
	page_count = 0;
	page_stats = gs_gsq::PageBuildStats();
	if (page_builder != nullptr) {
		memdelete(page_builder);
		page_builder = nullptr;
	}

	if (p_final_path.is_empty()) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: empty final path");
	}
	if (p_mode != SourceMode::REITERABLE && p_mode != SourceMode::SINGLE_PASS) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: unknown source mode");
	}
	gs_gsq::EncodeParams gsq_params;
	if (p_codec.codec_id == kCodecGsqV1) {
		String reason;
		if (gsq_encode_params(p_codec, gsq_params, reason) != OK) {
			return _fail(ERR_INVALID_PARAMETER, "invalid_spec: " + reason);
		}
		if (p_spec.sh_degree > gs_gsq::kMaxShDegree || p_spec.sh_high_order != gs_gsq::sh_high_order_for_degree(p_spec.sh_degree)) {
			return _fail(ERR_INVALID_PARAMETER, vformat("invalid_spec: GSQ v1 carries SH degree <= %d with its full high-order layout (degree %d, %d high-order)",
														gs_gsq::kMaxShDegree, p_spec.sh_degree, p_spec.sh_high_order));
		}
		if (!p_spec.write_chunk_table) {
			// Bit 7 always carries its chunk table: it is the page directory (W11).
			return _fail(ERR_INVALID_PARAMETER, "invalid_spec: GSQ v1 (plain v3 bit 7) needs write_chunk_table");
		}
		gsq = true;
	} else if (p_codec.codec_id != kCodecNone) {
		return _fail(ERR_UNAVAILABLE, vformat("codec_unavailable: codec_id %d", p_codec.codec_id));
	} else if (p_codec.sh_bits[0] != 0u || p_codec.sh_bits[1] != 0u || p_codec.sh_bits[2] != 0u || p_codec.opacity_bits != 0u ||
			p_codec.pos_step_m != 0.0f || p_codec.zstd_level != 0) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: codec parameters are set but codec_id is 0 (none)");
	}
	if (p_spec.splat_count == 0u) {
		// The loader accepts it, but the streaming payload source it attaches is
		// invalid for zero splats; there is nothing to stream.
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: splat_count is 0");
	}
	if (p_spec.chunk_count == 0u) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: chunk_count is 0 (at least one append unit is required)");
	}
	if (p_spec.sh_degree > kMaxShDegree) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_spec: sh_degree %d > %d (reader bound)", p_spec.sh_degree, kMaxShDegree));
	}
	if (p_spec.sh_first_order > kMaxShFirstOrder) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_spec: sh_first_order %d > %d (reader bound)", p_spec.sh_first_order, kMaxShFirstOrder));
	}
	const uint64_t sh_coefficients = uint64_t(p_spec.splat_count) * uint64_t(p_spec.sh_high_order);
	if (uint64_t(p_spec.chunk_count) * gs_world_format::kChunkRecordBytes > uint64_t(UINT32_MAX)) {
		// W5: the chunk table is held in RAM in a 32-bit-indexed container.
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_spec: chunk_count %d exceeds the in-memory chunk table bound", p_spec.chunk_count));
	}
	if (sh_coefficients > uint64_t(UINT32_MAX)) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: splat_count * sh_high_order exceeds UINT32_MAX coefficients (reader bound, #582)");
	}

	// Metadata exactly as the in-core saver writes it (_build_world_metadata_bytes):
	// JSON::stringify with its defaults -- no indent, keys SORTED at every level,
	// default precision -- as UTF-8, only when the dictionary is non-empty. Sorting
	// makes the bytes independent of the dictionary's insertion order (W7).
	if (!p_spec.metadata.is_empty()) {
		metadata_bytes = JSON::stringify(p_spec.metadata, "", true, false).to_utf8_buffer();
	}

	if (gsq) {
		// Plain v3 + bit 7 (gaussian_splat_world_format.h). Only the first page offset is
		// known now; the tables, the metadata and the file size follow from the pages and are
		// fixed in _write_tail().
		Layout l;
		l.flags = gs_world_format::kFlagHasChunks | gs_world_format::kFlagPagedChunks;
		if (!p_spec.metadata.is_empty()) {
			l.flags |= gs_world_format::kFlagHasMetadata;
		}
		if (p_spec.is_2d) {
			l.flags |= gs_world_format::kFlagIs2D;
		}
		if (p_spec.sh_high_order > 0u) {
			l.flags |= gs_world_format::kFlagHasHighSh;
		}
		l.header_chunk_count = p_spec.chunk_count;
		const uint64_t header_bytes = gs_world_format::kPrefixBytes + gs_world_format::kPagedExtensionBytes;
		l.gaussian_offset = (header_bytes + gs_world_format::kPagedPageAlignment - 1u) / gs_world_format::kPagedPageAlignment *
				gs_world_format::kPagedPageAlignment;
		l.metadata_size = metadata_bytes.size();
		layout = l;
		page_builder = memnew(gs_gsq::PageBuilder);
		String reason;
		if (page_builder->configure(gsq_params, p_spec.sh_degree, reason) != OK) {
			return _fail(ERR_INVALID_PARAMETER, "invalid_spec: " + reason);
		}
		pass_count = 1u; // the SH travels inside the pages
		Error err = output.begin(p_final_path);
		if (err != OK) {
			return _fail(err, "io: cannot create the output temp: " + output.get_error_message());
		}
		// Placeholder header, extension and padding up to the first page; the real header is
		// written last (W2's one rewrite).
		uint8_t zeros[gs_world_format::kPagedPageAlignment] = {};
		err = output.append(zeros, layout.gaussian_offset);
		if (err != OK) {
			return _fail(err, "io: header placeholder: " + output.get_error_message());
		}
		chunk_counts.reserve(p_spec.chunk_count);
		chunk_records.reserve(uint32_t(uint64_t(p_spec.chunk_count) * gs_world_format::kPagedChunkRecordBytes));
		pass = 1u;
		state = State::OPEN;
		return OK;
	}

	// Plain v3 layout, offset for offset as _build_world_save_layout computes it:
	// header | gaussians | high-order SH | chunk table | chunk indices | metadata.
	Layout l;
	if (!p_spec.metadata.is_empty()) {
		l.flags |= gs_world_format::kFlagHasMetadata;
	}
	if (p_spec.is_2d) {
		l.flags |= gs_world_format::kFlagIs2D;
	}
	if (p_spec.write_chunk_table) {
		l.flags |= gs_world_format::kFlagHasChunks;
		l.header_chunk_count = p_spec.chunk_count;
	}
	if (p_spec.sh_high_order > 0u) {
		l.flags |= gs_world_format::kFlagHasHighSh;
	}
	l.gaussian_offset = gs_world_format::kPrefixBytes;
	bool ok = _mul_u64(uint64_t(p_spec.splat_count), uint64_t(sizeof(Gaussian)), l.gaussian_bytes);
	ok = ok && _mul_u64(sh_coefficients, uint64_t(sizeof(Vector3)), l.sh_bytes);
	if (p_spec.write_chunk_table) {
		ok = ok && _mul_u64(uint64_t(p_spec.chunk_count), gs_world_format::kChunkRecordBytes, l.chunk_table_bytes);
		// Chunks tile the stream, so the index lists cover every splat exactly once.
		ok = ok && _mul_u64(uint64_t(p_spec.splat_count), uint64_t(sizeof(uint32_t)), l.indices_bytes);
	}
	ok = ok && _add_u64(l.gaussian_offset, l.gaussian_bytes, l.sh_offset);
	ok = ok && _add_u64(l.sh_offset, l.sh_bytes, l.chunk_table_offset);
	ok = ok && _add_u64(l.chunk_table_offset, l.chunk_table_bytes, l.indices_offset);
	uint64_t metadata_start = 0;
	ok = ok && _add_u64(l.indices_offset, l.indices_bytes, metadata_start);
	l.metadata_size = metadata_bytes.size();
	l.metadata_offset = metadata_bytes.is_empty() ? 0u : metadata_start;
	ok = ok && _add_u64(metadata_start, l.metadata_size, l.file_size);
	if (!ok) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: declared sizes overflow 64 bits");
	}
	layout = l;

	pass_count = (p_mode == SourceMode::REITERABLE && layout.sh_bytes > 0u) ? 2u : 1u;

	Error err = output.begin(p_final_path);
	if (err != OK) {
		return _fail(err, "io: cannot create the output temp: " + output.get_error_message());
	}
	if (p_mode == SourceMode::SINGLE_PASS && layout.sh_bytes > 0u) {
		// Scratch sibling of the destination (same filesystem); it is a transaction
		// that is read back and aborted, never committed, so its own "final" name
		// is never created.
		err = sh_scratch.begin(p_final_path + ".sh_scratch");
		if (err != OK) {
			return _fail(err, "io: cannot create the SH scratch: " + sh_scratch.get_error_message());
		}
	}
	// Placeholder header; the real one is written last (W2's one rewrite), so an
	// abandoned temp never carries a valid header.
	uint8_t zero_header[gs_world_format::kPrefixBytes] = {};
	err = output.append(zero_header, sizeof(zero_header));
	if (err != OK) {
		return _fail(err, "io: header placeholder: " + output.get_error_message());
	}

	chunk_counts.reserve(p_spec.chunk_count);
	if (p_spec.write_chunk_table) {
		chunk_records.reserve(uint32_t(layout.chunk_table_bytes));
	}
	pass = 1u;
	state = State::OPEN;
	return OK;
}

Error Writer::_append_sliced(GSAtomicFileTransaction &p_txn, const uint8_t *p_data, uint64_t p_size) {
	uint64_t done = 0;
	while (done < p_size) {
		const uint64_t slice = MIN(kSliceBytes, p_size - done);
		const Error err = p_txn.append(p_data + done, slice);
		if (err != OK) {
			return _fail(err, "io: " + p_txn.get_error_message());
		}
		done += slice;
	}
	return OK;
}

void Writer::_encode_chunk_record(const ChunkDesc &p_desc, uint64_t p_first_index, uint32_t p_count, uint8_t *r_record) const {
	// Field for field as _write_chunk_record in gaussian_splat_world_io.cpp.
	_put_vec3(r_record + 0, p_desc.bounds.position);
	_put_vec3(r_record + 12, p_desc.bounds.size);
	_put_vec3(r_record + 24, p_desc.center);
	_put_f32(r_record + 36, p_desc.radius);
	_put_u64(r_record + 40, p_first_index);
	_put_u32(r_record + 48, p_count);
	_put_u32(r_record + 52, 0u);
}

uint32_t Writer::_encode_header(uint8_t *r_header) const {
	// Field for field as _write_world_save_header in gaussian_splat_world_io.cpp.
	_put_u32(r_header + 0, gs_world_format::kMagic);
	_put_u32(r_header + 4, gs_world_format::kVersion);
	_put_u32(r_header + 8, layout.flags);
	_put_u32(r_header + 12, spec.splat_count);
	_put_u32(r_header + 16, spec.sh_degree);
	_put_u32(r_header + 20, spec.sh_first_order);
	_put_u32(r_header + 24, spec.sh_high_order);
	_put_vec3(r_header + 28, spec.bounds.position);
	_put_vec3(r_header + 40, spec.bounds.size);
	_put_u32(r_header + 52, layout.header_chunk_count);
	_put_u64(r_header + 56, layout.gaussian_offset);
	_put_u64(r_header + 64, layout.sh_offset);
	_put_u64(r_header + 72, layout.chunk_table_offset);
	_put_u64(r_header + 80, layout.indices_offset);
	_put_u64(r_header + 88, layout.metadata_offset);
	_put_u64(r_header + 96, layout.metadata_size);
	_put_u32(r_header + 104, GAUSSIAN_STRUCT_LAYOUT_VERSION);
	_put_u32(r_header + 108, 0u); // reserved, must be zero
	if (!gsq) {
		return uint32_t(gs_world_format::kPrefixBytes);
	}
	// Paged extension: the W16 codec block as given, then the page directory.
	uint8_t *ext = r_header + gs_world_format::kPrefixBytes;
	memset(ext, 0, gs_world_format::kPagedExtensionBytes);
	_put_u32(ext + 0, codec.codec_id);
	ext[4] = codec.sh_bits[0];
	ext[5] = codec.sh_bits[1];
	ext[6] = codec.sh_bits[2];
	ext[7] = codec.opacity_bits;
	_put_f32(ext + 8, codec.pos_step_m);
	_put_u32(ext + 12, uint32_t(codec.zstd_level));
	_put_u32(ext + 16, page_count);
	_put_u32(ext + 20, uint32_t(gs_world_format::kPagedPageAlignment));
	_put_u64(ext + 24, layout.page_table_offset);
	return uint32_t(gs_world_format::kPrefixBytes + gs_world_format::kPagedExtensionBytes);
}

Error Writer::append_chunk(const ChunkDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count) {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
	if (pass == kNoPass || chunks_this_pass >= spec.chunk_count) {
		return _fail(ERR_INVALID_DATA, vformat("count_mismatch: chunk beyond the declared chunk_count %d", spec.chunk_count));
	}
	if (uint64_t(p_count) > uint64_t(spec.splat_count) - splats_this_pass) {
		return _fail(ERR_INVALID_DATA, vformat("count_mismatch: pass %d appends past the declared splat_count %d", pass, spec.splat_count));
	}
	if (gsq) {
		return _append_chunk_gsq(p_desc, p_gaussians, p_sh_high, p_count);
	}
	const bool takes_gaussians = pass == 1u;
	const bool takes_sh = layout.sh_bytes > 0u && (mode == SourceMode::SINGLE_PASS || pass == 2u);
	if (p_count > 0u && takes_gaussians && p_gaussians == nullptr) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_chunk: chunk %d has %d splats but no gaussians", chunks_this_pass, p_count));
	}
	if (p_count > 0u && takes_sh && p_sh_high == nullptr) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_chunk: chunk %d has %d splats but no high-order SH", chunks_this_pass, p_count));
	}

	uint8_t record[gs_world_format::kChunkRecordBytes];
	if (spec.write_chunk_table) {
		_encode_chunk_record(p_desc, splats_this_pass, p_count, record);
	}
	if (pass == 1u) {
		chunk_counts.push_back(p_count);
		if (spec.write_chunk_table) {
			for (uint32_t i = 0; i < gs_world_format::kChunkRecordBytes; i++) {
				chunk_records.push_back(record[i]);
			}
		}
	} else {
		// Pass 2 must replay pass 1 (REITERABLE contract). A diverging source would
		// put SH behind the wrong splats; refuse it rather than write it.
		const uint32_t index = chunks_this_pass;
		if (chunk_counts[index] != p_count) {
			return _fail(ERR_INVALID_DATA, vformat("stream_diverged: pass 2 chunk %d has %d splats, pass 1 had %d", index, p_count, chunk_counts[index]));
		}
		if (spec.write_chunk_table &&
				memcmp(record, chunk_records.ptr() + uint64_t(index) * gs_world_format::kChunkRecordBytes, sizeof(record)) != 0) {
			return _fail(ERR_INVALID_DATA, vformat("stream_diverged: pass 2 chunk %d descriptor differs from pass 1", index));
		}
	}

	if (takes_gaussians && p_count > 0u) {
		const Error err = _append_sliced(output, reinterpret_cast<const uint8_t *>(p_gaussians), uint64_t(p_count) * sizeof(Gaussian));
		if (err != OK) {
			return err;
		}
	}
	if (takes_sh && p_count > 0u) {
		GSAtomicFileTransaction &sh_target = mode == SourceMode::SINGLE_PASS ? sh_scratch : output;
		const uint64_t sh_bytes = uint64_t(p_count) * uint64_t(spec.sh_high_order) * sizeof(Vector3);
		const Error err = _append_sliced(sh_target, reinterpret_cast<const uint8_t *>(p_sh_high), sh_bytes);
		if (err != OK) {
			return err;
		}
	}

	chunks_this_pass++;
	splats_this_pass += p_count;
	if (chunks_this_pass == spec.chunk_count) {
		return _end_pass();
	}
	return OK;
}

Error Writer::_append_chunk_gsq(const ChunkDesc &p_desc, const Gaussian *p_gaussians, const Vector3 *p_sh_high, uint32_t p_count) {
	const uint32_t chunk = chunks_this_pass;
	if (p_count > 0u && p_gaussians == nullptr) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_chunk: chunk %d has %d splats but no gaussians", chunk, p_count));
	}
	if (p_count > 0u && spec.sh_high_order > 0u && p_sh_high == nullptr) {
		return _fail(ERR_INVALID_PARAMETER, vformat("invalid_chunk: chunk %d has %d splats but no high-order SH", chunk, p_count));
	}
	// W12 / W15: the builder encodes, decodes and validates every page and records both digests.
	built_pages.clear();
	String reason;
	const Error build_err = page_builder->build_chunk(chunk, p_gaussians, p_sh_high, p_count, built_pages, reason);
	if (build_err != OK) {
		return _fail(build_err, vformat("chunk %d: %s", chunk, reason));
	}
	if (uint64_t(page_count) + built_pages.size() > uint64_t(UINT32_MAX) ||
			uint64_t(page_records.size()) + uint64_t(built_pages.size()) * gs_world_format::kPagedPageRecordBytes > uint64_t(UINT32_MAX)) {
		return _fail(ERR_INVALID_PARAMETER, "invalid_spec: the page table exceeds its in-memory bound");
	}

	uint8_t record[gs_world_format::kPagedChunkRecordBytes];
	_put_vec3(record + 0, p_desc.bounds.position);
	_put_vec3(record + 12, p_desc.bounds.size);
	_put_vec3(record + 24, p_desc.center);
	_put_f32(record + 36, p_desc.radius);
	_put_u32(record + 40, page_count);
	_put_u32(record + 44, built_pages.size());
	_put_u32(record + 48, p_count);
	_put_u32(record + 52, 0u);
	for (uint32_t i = 0; i < gs_world_format::kPagedChunkRecordBytes; i++) {
		chunk_records.push_back(record[i]);
	}
	chunk_counts.push_back(p_count);

	static const uint8_t zeros[gs_world_format::kPagedPageAlignment] = {};
	for (const gs_gsq::BuiltPage &page : built_pages) {
		// Every page starts on an alignment boundary; the gap is zero.
		const uint64_t at = output.get_size();
		const uint64_t aligned = (at + gs_world_format::kPagedPageAlignment - 1u) / gs_world_format::kPagedPageAlignment *
				gs_world_format::kPagedPageAlignment;
		if (aligned > at) {
			const Error pad_err = _append_sliced(output, zeros, aligned - at);
			if (pad_err != OK) {
				return pad_err;
			}
		}
		gs_gsq::PageInfo info;
		if (gs_gsq::parse_page_header(page.bytes.ptr(), page.bytes.size(), info, reason) != OK) {
			return _fail(ERR_BUG, vformat("chunk %d: a validated page no longer parses: %s", chunk, reason));
		}
		const Error err = _append_sliced(output, page.bytes.ptr(), page.bytes.size());
		if (err != OK) {
			return err;
		}
		uint8_t entry[gs_world_format::kPagedPageRecordBytes];
		_put_u64(entry + 0, aligned);
		_put_u32(entry + 8, page.bytes.size());
		_put_u32(entry + 12, page.record.splat_count);
		memcpy(entry + 16, page.record.stored_digest, gs_gsq::kDigestBytes);
		memcpy(entry + 48, page.record.decoded_digest, gs_gsq::kDigestBytes);
		for (int axis = 0; axis < 3; axis++) {
			_put_f32(entry + 80 + 4 * axis, info.position_min[axis]);
			_put_f32(entry + 92 + 4 * axis, info.position_max[axis]);
		}
		_put_u32(entry + 104, chunk);
		_put_u32(entry + 108, 0u);
		for (uint32_t i = 0; i < gs_world_format::kPagedPageRecordBytes; i++) {
			page_records.push_back(entry[i]);
		}
		page_count++;
	}
	built_pages.clear();

	chunks_this_pass++;
	splats_this_pass += p_count;
	if (chunks_this_pass == spec.chunk_count) {
		return _end_pass();
	}
	return OK;
}

Error Writer::_end_pass() {
	if (splats_this_pass != uint64_t(spec.splat_count)) {
		return _fail(ERR_INVALID_DATA, vformat("count_mismatch: pass %d ended with %s of %d declared splats", pass,
											   String::num_uint64(splats_this_pass), spec.splat_count));
	}
	// Structural self-check: the stream so far must sit exactly where the layout
	// says the next section starts.
	// (GSQ v1: the page section ends where its pages end; _write_tail() places the tables there.)
	const uint64_t expected_end = pass == 1u ? layout.sh_offset : layout.chunk_table_offset;
	if (!gsq && output.get_size() != expected_end) {
		return _fail(ERR_BUG, vformat("layout: pass %d ended at byte %s, expected %s", pass,
									  String::num_uint64(output.get_size()), String::num_uint64(expected_end)));
	}
	if (pass < pass_count) {
		pass++;
		chunks_this_pass = 0;
		splats_this_pass = 0;
	} else {
		pass = kNoPass;
	}
	return OK;
}

Error Writer::_copy_scratch_sh() {
	if (sh_scratch.get_size() != layout.sh_bytes) {
		return _fail(ERR_BUG, vformat("layout: SH scratch holds %s bytes, expected %s",
									  String::num_uint64(sh_scratch.get_size()), String::num_uint64(layout.sh_bytes)));
	}
	LocalVector<uint8_t> slice;
	slice.resize(uint32_t(MIN(kSliceBytes, layout.sh_bytes)));
	uint64_t done = 0;
	while (done < layout.sh_bytes) {
		const uint64_t n = MIN(kSliceBytes, layout.sh_bytes - done);
		const Error read_err = sh_scratch.read_at(done, slice.ptr(), n);
		if (read_err != OK) {
			return _fail(read_err, "io: SH scratch read-back: " + sh_scratch.get_error_message());
		}
		const Error err = _append_sliced(output, slice.ptr(), n);
		if (err != OK) {
			return err;
		}
		done += n;
	}
	sh_scratch.abort(); // removes the scratch; it is never committed
	return OK;
}

Error Writer::_write_tail() {
	if (gsq) {
		// Paged chunk table, page table, metadata; the header records where each landed.
		layout.chunk_table_offset = output.get_size();
		Error err = _append_sliced(output, chunk_records.ptr(), chunk_records.size());
		if (err != OK) {
			return err;
		}
		layout.page_table_offset = output.get_size();
		err = _append_sliced(output, page_records.ptr(), page_records.size());
		if (err != OK) {
			return err;
		}
		layout.metadata_offset = metadata_bytes.is_empty() ? 0u : output.get_size();
		if (!metadata_bytes.is_empty()) {
			err = _append_sliced(output, metadata_bytes.ptr(), metadata_bytes.size());
			if (err != OK) {
				return err;
			}
		}
		layout.file_size = output.get_size();
		return OK;
	}
	if (spec.write_chunk_table) {
		Error err = _append_sliced(output, chunk_records.ptr(), chunk_records.size());
		if (err != OK) {
			return err;
		}
		// W5: the index lists are the identity ranges of the stream, generated one
		// slice at a time and never held whole.
		LocalVector<uint32_t> indices;
		const uint32_t per_slice = uint32_t(kSliceBytes / sizeof(uint32_t));
		indices.resize(MIN(per_slice, spec.splat_count));
		uint32_t next = 0;
		while (next < spec.splat_count) {
			const uint32_t n = MIN(per_slice, spec.splat_count - next);
			for (uint32_t i = 0; i < n; i++) {
				indices[i] = next + i;
			}
			uint8_t *bytes = reinterpret_cast<uint8_t *>(indices.ptr());
			// The in-core saver stores the index arrays raw (store_buffer of the
			// host u32 array); so does this. Every supported target is little-endian.
			err = _append_sliced(output, bytes, uint64_t(n) * sizeof(uint32_t));
			if (err != OK) {
				return err;
			}
			next += n;
		}
	}
	if (!metadata_bytes.is_empty()) {
		const Error err = _append_sliced(output, metadata_bytes.ptr(), metadata_bytes.size());
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

Error Writer::finalize() {
	if (state != State::OPEN) {
		return state == State::FAILED ? error : ERR_UNCONFIGURED;
	}
	if (pass != kNoPass) {
		return _fail(ERR_INVALID_DATA, vformat("count_mismatch: finalize() before pass %d of %d completed (%d of %d chunks)",
											   pass, pass_count, chunks_this_pass, spec.chunk_count));
	}
	if (mode == SourceMode::SINGLE_PASS && layout.sh_bytes > 0u) {
		const Error err = _copy_scratch_sh();
		if (err != OK) {
			return err;
		}
	}
	Error err = _write_tail();
	if (err != OK) {
		return err;
	}
	if (output.get_size() != layout.file_size) {
		return _fail(ERR_BUG, vformat("layout: wrote %s bytes, expected %s",
									  String::num_uint64(output.get_size()), String::num_uint64(layout.file_size)));
	}
	uint8_t header[gs_world_format::kPrefixBytes + gs_world_format::kPagedExtensionBytes];
	const uint32_t header_size = _encode_header(header);
	err = output.write_at(0, header, header_size);
	if (err != OK) {
		return _fail(err, "io: header rewrite: " + output.get_error_message());
	}
	// Pay the long flush before readers are suspended; commit() then has (almost)
	// nothing left to flush inside the guard.
	err = output.flush_to_storage();
	if (err != OK) {
		return _fail(err, "io: " + output.get_error_message());
	}

	{
		// #714: suspend every live streaming source of the destination across the
		// replace only (see gs_world_writer.h).
		StagedFileChunkPayloadSource::ScopedReaderSuspend readers(final_path);
		bool drained = readers.drained();
#ifdef TESTS_ENABLED
		if (drain_test_hook != nullptr && !drain_test_hook(drain_test_userdata)) {
			drained = false;
		}
#endif
		if (!drained) {
			return _fail(ERR_BUSY, "readers_busy: a streaming reader still holds the destination open; the replace was not attempted and the destination is unchanged");
		}
		err = output.commit();
	}
	if (err != OK) {
		return _fail(err, "io: " + output.get_error_message());
	}
	chunk_records.clear();
	chunk_counts.clear();
	metadata_bytes.clear();
	page_records.clear();
	if (page_builder != nullptr) {
		page_stats = page_builder->get_stats();
		memdelete(page_builder);
		page_builder = nullptr;
	}
	state = State::COMMITTED;
	return OK;
}

} // namespace gs_world_writer
