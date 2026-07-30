#include "gaussian_splat_world_io.h"

#include "../core/gs_vector_alloc.h"

#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/compression.h"
#include "core/string/ustring.h"
#include "core/math/math_funcs.h"
#include "io_settings_utils.h"
#include "gs_atomic_file_writer.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_world.h"
#include "../interfaces/gpu_culler.h"
#include "../logger/gs_logger.h"

#include <cstring>
#include <cstdint>

namespace {

static constexpr uint32_t kWorldMagic = 0x57505347; // 'GSPW' little-endian.
static constexpr uint32_t kWorldVersion = 1;
static constexpr uint32_t kMaxShDegree = 3;
// Gaussian.sh_1[3] holds at most 3 first-order coefficients on disk, and
// GaussianData::set_gaussian_payload() clamps p_sh_first_order_count to 3, so
// a value above 3 cannot have been produced by the in-tree saver. sh_high_order
// has no equivalent ceiling — set_gaussian_payload accepts arbitrary counts and
// the saver writes them verbatim — so it is bounded only by the structural
// `fits_within` + `_checked_mul_u64` checks below, not by a magnitude cap.
static constexpr uint32_t kMaxShFirstOrder = 3u;
static constexpr uint32_t kFlagHasMetadata = 1u << 0u;
static constexpr uint32_t kFlagIs2D = 1u << 1u;
static constexpr uint32_t kFlagHasChunks = 1u << 2u;
static constexpr uint32_t kFlagHasHighSh = 1u << 3u;
static constexpr uint32_t kFlagCompressed = 1u << 4u;
static constexpr uint32_t kFlagResidentPayload = 1u << 5u;
static constexpr uint64_t kHeaderSizeBytes = 104u;

// Bound on the *decompressed* resident gaussian payload for compressed worlds.
// The uncompressed path is bounded structurally by `fits_within` (you cannot claim
// more splats than the file has bytes); the compressed path decouples splat_count
// from file_len, so without this cap a crafted ~100-byte file can set a large
// splat_count and drive `gaussians.resize(splat_count)` to a multi-GiB allocation
// -> LocalVector CRASH_COND aborts the engine instead of returning an error.
// KEEP IN SYNC with the header validator in io/resource_importer_gsplatworld.cpp.
//
// The cap is INT32_MAX (~2 GiB), the gzip decompression limit: `Compression::
// decompress` for MODE_GZIP ERR_FAILs on any destination larger than INT32_MAX
// (core/io/compression.cpp) because zlib's implementation uses C++ `int`. A
// compressed world declaring more than that can therefore NEVER decompress, so
// rejecting it here is not a false positive — it just moves the guaranteed failure
// ahead of a pointless multi-GiB allocation. (A ratio-of-compressed-size cap is
// deliberately NOT used: gzip on a highly regular payload can legitimately exceed
// any fixed ratio, which would reject valid round-trippable files.)
static constexpr uint64_t kMaxCompressedGaussianBytes = INT32_MAX;

static bool fits_within(uint64_t p_offset, uint64_t p_size, uint64_t p_file_len) {
	if (p_offset > p_file_len) {
		return false;
	}
	return p_size <= (p_file_len - p_offset);
}

static bool checked_mul_u64(uint64_t p_a, uint64_t p_b, uint64_t &r_result) {
	if (p_a != 0u && p_b > (UINT64_MAX / p_a)) {
		return false;
	}
	r_result = p_a * p_b;
	return true;
}

struct ChunkRecord {
	Vector3 bounds_pos;
	Vector3 bounds_size;
	Vector3 center;
	float radius = 0.0f;
	uint64_t indices_offset = 0;
	uint32_t index_count = 0;
	uint32_t reserved = 0;
};

static bool _read_exact(Ref<FileAccess> p_file, void *p_dst, uint64_t p_size) {
	ERR_FAIL_COND_V(p_file.is_null(), false);
	uint8_t *dst = static_cast<uint8_t *>(p_dst);
	// Read in <=256 MB slices to avoid MSVC CRT fread issues with >2 GB reads.
	constexpr uint64_t kSliceBytes = 256u * 1024u * 1024u;
	uint64_t remaining = p_size;
	while (remaining > 0) {
		uint64_t slice = (remaining > kSliceBytes) ? kSliceBytes : remaining;
		uint64_t got = p_file->get_buffer(dst, slice);
		if (got != slice) {
			return false;
		}
		dst += slice;
		remaining -= slice;
	}
	return true;
}

static void _write_vec3(Ref<FileAccess> p_file, const Vector3 &p_value) {
	p_file->store_float(p_value.x);
	p_file->store_float(p_value.y);
	p_file->store_float(p_value.z);
}

static Vector3 _read_vec3(Ref<FileAccess> p_file) {
	return Vector3(p_file->get_float(), p_file->get_float(), p_file->get_float());
}

static void _write_chunk_record(Ref<FileAccess> p_file, const ChunkRecord &p_record) {
	_write_vec3(p_file, p_record.bounds_pos);
	_write_vec3(p_file, p_record.bounds_size);
	_write_vec3(p_file, p_record.center);
	p_file->store_float(p_record.radius);
	p_file->store_64(p_record.indices_offset);
	p_file->store_32(p_record.index_count);
	p_file->store_32(p_record.reserved);
}

static ChunkRecord _read_chunk_record(Ref<FileAccess> p_file) {
	ChunkRecord record;
	record.bounds_pos = _read_vec3(p_file);
	record.bounds_size = _read_vec3(p_file);
	record.center = _read_vec3(p_file);
	record.radius = p_file->get_float();
	record.indices_offset = p_file->get_64();
	record.index_count = p_file->get_32();
	record.reserved = p_file->get_32();
	return record;
}

// Compression helpers
static PackedByteArray _compress_data(const uint8_t *p_data, uint64_t p_size) {
	PackedByteArray result;
	if (p_size == 0) return result;
	
	int64_t max_compressed = Compression::get_max_compressed_buffer_size(p_size, Compression::MODE_GZIP);
	result.resize(max_compressed);
	
	int64_t compressed_size = Compression::compress(result.ptrw(), p_data, p_size, Compression::MODE_GZIP);
	if (compressed_size <= 0 || compressed_size > max_compressed) {
		return PackedByteArray(); // Compression failed
	}
	
	result.resize(compressed_size);
	return result;
}

static bool _decompress_data(const uint8_t *p_compressed, uint64_t p_compressed_size, uint8_t *p_dst, uint64_t p_original_size) {
	if (p_compressed_size == 0 || p_original_size == 0) return false;
	
	int64_t result = Compression::decompress(p_dst, p_original_size, p_compressed, p_compressed_size, Compression::MODE_GZIP);
	return result == static_cast<int64_t>(p_original_size);
}

static Error _ensure_file_write_ok(const Ref<FileAccess> &p_file, const char *p_context) {
	ERR_FAIL_COND_V_MSG(p_file.is_null(), ERR_INVALID_PARAMETER, "FileAccess is null while checking write status.");
	const Error io_error = p_file->get_error();
	if (io_error != OK) {
		ERR_PRINT(vformat("[GaussianSplatWorldIO] Write failure in %s (error=%d).",
				p_context ? p_context : "unknown",
				(int)io_error));
		return io_error;
	}
	return OK;
}

static Error _materialize_payload_source_for_save(const GaussianSplatWorld *p_world, Ref<GaussianData> &r_gaussian_data) {
	ERR_FAIL_COND_V(p_world == nullptr, ERR_INVALID_PARAMETER);
	Ref<ChunkPayloadSource> payload_source = p_world->get_chunk_payload_source();
	ERR_FAIL_COND_V_MSG(payload_source.is_null() || !payload_source->is_valid(), ERR_UNCONFIGURED,
			"GaussianSplatWorld has no valid source-backed payload to save.");

	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh_high_order;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order_count = 0;
	if (!payload_source->capture_chunk_snapshot(0, payload_source->get_count(),
				gaussians, sh_high_order, sh_first_order, sh_high_order_count)) {
		return ERR_FILE_CANT_READ;
	}

	r_gaussian_data.instantiate();
	r_gaussian_data->set_gaussian_payload(gaussians, sh_high_order,
			sh_first_order, sh_high_order_count, p_world->get_2d_mode());
	return OK;
}

struct WorldSaveLayout {
	uint32_t splat_count = 0;
	uint32_t sh_degree = 0;
	uint32_t sh_first_order = 0;
	uint32_t sh_high_order = 0;
	uint32_t chunk_count = 0;
	uint32_t flags = 0;
	uint64_t gaussian_bytes = 0;
	uint64_t sh_bytes = 0;
	uint64_t chunk_table_bytes = 0;
	uint64_t indices_bytes = 0;
	uint64_t gaussian_stored_bytes = 0;
	uint64_t gaussian_offset = kHeaderSizeBytes;
	uint64_t sh_offset = 0;
	uint64_t chunk_table_offset = 0;
	uint64_t indices_offset = 0;
	uint64_t metadata_offset = 0;
	uint64_t metadata_size = 0;
	bool use_compression = false;
	PackedByteArray compressed_gaussians;
	PackedByteArray metadata_bytes;
};

static uint64_t _count_chunk_indices(const Vector<StaticChunk> &p_chunks) {
	uint64_t total_indices = 0;
	for (int i = 0; i < p_chunks.size(); i++) {
		total_indices += p_chunks[i].indices.size();
	}
	return total_indices;
}

static uint32_t _build_world_save_flags(const GaussianSplatWorld *p_world,
		const Ref<GaussianData> &p_gaussian_data,
		const Vector<StaticChunk> &p_chunks) {
	uint32_t flags = 0;
	if (!p_world->get_metadata().is_empty()) {
		flags |= kFlagHasMetadata;
	}
	if (p_gaussian_data->get_2d_mode()) {
		flags |= kFlagIs2D;
	}
	if (!p_chunks.is_empty()) {
		flags |= kFlagHasChunks;
	}
	if (p_gaussian_data->get_sh_high_order_count() > 0) {
		flags |= kFlagHasHighSh;
	}
	return flags;
}

static PackedByteArray _build_world_metadata_bytes(const GaussianSplatWorld *p_world, uint32_t p_flags) {
	String metadata_json;
	if ((p_flags & kFlagHasMetadata) != 0) {
		metadata_json = JSON::stringify(p_world->get_metadata());
	}
	return metadata_json.to_utf8_buffer();
}

static bool _compress_world_gaussians(const Ref<GaussianData> &p_gaussian_data,
		uint64_t p_gaussian_bytes,
		PackedByteArray &r_compressed_gaussians) {
	if (p_gaussian_bytes == 0) {
		return false;
	}

	r_compressed_gaussians = _compress_data(
			reinterpret_cast<const uint8_t *>(p_gaussian_data->get_gaussian_storage().ptr()),
			p_gaussian_bytes);
	if (r_compressed_gaussians.is_empty()) {
		return false;
	}

	GS_LOG_STREAMING_INFO(vformat("Compressed gaussian data: %d KB -> %d KB (%.1fx)",
			int(p_gaussian_bytes / 1024), int(r_compressed_gaussians.size() / 1024),
			float(p_gaussian_bytes) / float(r_compressed_gaussians.size())));
	return true;
}

static Error _build_world_save_layout(const GaussianSplatWorld *p_world,
		const Ref<GaussianData> &p_gaussian_data,
		const Vector<StaticChunk> &p_chunks,
		ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode p_mode,
		WorldSaveLayout &r_layout) {
	WorldSaveLayout layout;
	layout.splat_count = p_gaussian_data->get_count();
	layout.sh_degree = p_gaussian_data->get_sh_degree();
	layout.sh_first_order = p_gaussian_data->get_sh_first_order_count();
	layout.sh_high_order = p_gaussian_data->get_sh_high_order_count();
	layout.chunk_count = p_chunks.size();
	layout.flags = _build_world_save_flags(p_world, p_gaussian_data, p_chunks);
	layout.metadata_bytes = _build_world_metadata_bytes(p_world, layout.flags);

	const uint64_t total_indices = _count_chunk_indices(p_chunks);
	layout.gaussian_bytes = uint64_t(layout.splat_count) * sizeof(Gaussian);
	layout.sh_bytes = uint64_t(layout.splat_count) * uint64_t(layout.sh_high_order) * sizeof(Vector3);
	layout.chunk_table_bytes = uint64_t(layout.chunk_count) * 56u;
	layout.indices_bytes = total_indices * sizeof(uint32_t);

	// Generic ResourceSaver::save() must preserve streamability. Compression is
	// only for the explicit resident export path because this gzip layout cannot
	// serve random-access chunk reads.
	const bool resident_only = p_mode == ResourceFormatSaverGaussianSplatWorld::SAVE_PAYLOAD_RESIDENT_COMPRESSED ||
			p_mode == ResourceFormatSaverGaussianSplatWorld::SAVE_PAYLOAD_RESIDENT_UNCOMPRESSED;
	if (resident_only) {
		layout.flags |= kFlagResidentPayload;
	}
	if (p_mode == ResourceFormatSaverGaussianSplatWorld::SAVE_PAYLOAD_RESIDENT_COMPRESSED) {
		layout.use_compression = _compress_world_gaussians(p_gaussian_data, layout.gaussian_bytes, layout.compressed_gaussians);
		if (!layout.use_compression || layout.compressed_gaussians.is_empty()) {
			ERR_PRINT("[GaussianSplatWorldIO] Explicit resident-compressed save requested, but gaussian payload compression failed.");
			return ERR_CANT_CREATE;
		}
	}
	if (layout.use_compression) {
		layout.flags |= kFlagCompressed;
	}

	layout.gaussian_stored_bytes = layout.use_compression
			? (8 + layout.compressed_gaussians.size())
			: layout.gaussian_bytes;
	layout.sh_offset = layout.gaussian_offset + layout.gaussian_stored_bytes;
	layout.chunk_table_offset = layout.sh_offset + layout.sh_bytes;
	layout.indices_offset = layout.chunk_table_offset + layout.chunk_table_bytes;
	layout.metadata_offset = layout.metadata_bytes.is_empty() ? 0u
			: (kHeaderSizeBytes + layout.gaussian_stored_bytes + layout.sh_bytes + layout.chunk_table_bytes + layout.indices_bytes);
	layout.metadata_size = layout.metadata_bytes.size();
	r_layout = layout;
	return OK;
}

static Error _write_buffer_sliced(const Ref<FileAccess> &p_file,
		const uint8_t *p_src,
		uint64_t p_size,
		const char *p_context) {
	// Write in <=256 MB slices to avoid MSVC CRT fwrite issues with >2 GB writes.
	constexpr uint64_t kSliceBytes = 256u * 1024u * 1024u;
	uint64_t written = 0;
	while (written < p_size) {
		const uint64_t slice = MIN(kSliceBytes, p_size - written);
		p_file->store_buffer(p_src + written, slice);
		const Error err = _ensure_file_write_ok(p_file, p_context);
		if (err != OK) {
			return err;
		}
		written += slice;
	}
	return OK;
}

static Error _write_world_save_header(const Ref<FileAccess> &p_file,
		const GaussianSplatWorld *p_world,
		const WorldSaveLayout &p_layout) {
	p_file->store_32(kWorldMagic);
	p_file->store_32(kWorldVersion);
	p_file->store_32(p_layout.flags);
	p_file->store_32(p_layout.splat_count);
	p_file->store_32(p_layout.sh_degree);
	p_file->store_32(p_layout.sh_first_order);
	p_file->store_32(p_layout.sh_high_order);
	_write_vec3(p_file, p_world->get_bounds().position);
	_write_vec3(p_file, p_world->get_bounds().size);
	p_file->store_32(p_layout.chunk_count);
	p_file->store_64(p_layout.gaussian_offset);
	p_file->store_64(p_layout.sh_offset);
	p_file->store_64(p_layout.chunk_table_offset);
	p_file->store_64(p_layout.indices_offset);
	p_file->store_64(p_layout.metadata_offset);
	p_file->store_64(p_layout.metadata_size);
	return _ensure_file_write_ok(p_file, "save(header)");
}

static Error _write_world_gaussian_payload(const Ref<FileAccess> &p_file,
		const Ref<GaussianData> &p_gaussian_data,
		const WorldSaveLayout &p_layout) {
	if (p_layout.gaussian_bytes == 0) {
		return OK;
	}

	const uint8_t *write_src = nullptr;
	uint64_t write_total = 0;
	if (p_layout.use_compression && !p_layout.compressed_gaussians.is_empty()) {
		// Write compressed format: [8 bytes size][compressed data].
		p_file->store_64(p_layout.compressed_gaussians.size());
		write_src = p_layout.compressed_gaussians.ptr();
		write_total = p_layout.compressed_gaussians.size();
	} else {
		// Write uncompressed format: raw gaussian data.
		write_src = reinterpret_cast<const uint8_t *>(p_gaussian_data->get_gaussian_storage().ptr());
		write_total = p_layout.gaussian_bytes;
	}

	return _write_buffer_sliced(p_file, write_src, write_total, "save(gaussian_data)");
}

static Error _write_world_sh_payload(const Ref<FileAccess> &p_file,
		const Ref<GaussianData> &p_gaussian_data,
		const WorldSaveLayout &p_layout) {
	if (p_layout.sh_bytes == 0) {
		return OK;
	}

	const Vector3 *sh_ptr = p_gaussian_data->get_sh_high_order_coefficients_ptr();
	ERR_FAIL_COND_V_MSG(sh_ptr == nullptr, ERR_INVALID_DATA,
			"GaussianSplatWorld missing high-order SH coefficients while sh_high_order_count > 0.");
	p_file->store_buffer(reinterpret_cast<const uint8_t *>(sh_ptr), p_layout.sh_bytes);
	return _ensure_file_write_ok(p_file, "save(sh_data)");
}

static Error _write_world_chunk_table(const Ref<FileAccess> &p_file, const Vector<StaticChunk> &p_chunks) {
	uint64_t indices_cursor = 0;
	for (int i = 0; i < p_chunks.size(); i++) {
		const StaticChunk &chunk = p_chunks[i];
		ChunkRecord record;
		record.bounds_pos = chunk.bounds.position;
		record.bounds_size = chunk.bounds.size;
		record.center = chunk.center;
		record.radius = chunk.radius;
		record.indices_offset = indices_cursor;
		record.index_count = chunk.indices.size();
		_write_chunk_record(p_file, record);
		indices_cursor += record.index_count;
	}
	return _ensure_file_write_ok(p_file, "save(chunk_table)");
}

static Error _write_world_chunk_indices(const Ref<FileAccess> &p_file, const Vector<StaticChunk> &p_chunks) {
	for (int i = 0; i < p_chunks.size(); i++) {
		const StaticChunk &chunk = p_chunks[i];
		if (chunk.indices.is_empty()) {
			continue;
		}
		p_file->store_buffer(reinterpret_cast<const uint8_t *>(chunk.indices.ptr()),
				uint64_t(chunk.indices.size()) * sizeof(uint32_t));
		const Error err = _ensure_file_write_ok(p_file, "save(chunk_indices)");
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

static Error _write_world_chunks(const Ref<FileAccess> &p_file,
		const Vector<StaticChunk> &p_chunks,
		const WorldSaveLayout &p_layout) {
	if (p_layout.chunk_count == 0) {
		return OK;
	}

	const Error chunk_table_err = _write_world_chunk_table(p_file, p_chunks);
	if (chunk_table_err != OK) {
		return chunk_table_err;
	}
	return _write_world_chunk_indices(p_file, p_chunks);
}

static Error _write_world_metadata(const Ref<FileAccess> &p_file, const WorldSaveLayout &p_layout) {
	if (p_layout.metadata_size == 0) {
		return OK;
	}

	p_file->store_buffer(p_layout.metadata_bytes.ptr(), p_layout.metadata_size);
	return _ensure_file_write_ok(p_file, "save(metadata)");
}

static Error _write_world_save_sections(const Ref<FileAccess> &p_file,
		const GaussianSplatWorld *p_world,
		const Ref<GaussianData> &p_gaussian_data,
		const Vector<StaticChunk> &p_chunks,
		const WorldSaveLayout &p_layout) {
	Error err = _write_world_save_header(p_file, p_world, p_layout);
	if (err != OK) {
		return err;
	}
	err = _write_world_gaussian_payload(p_file, p_gaussian_data, p_layout);
	if (err != OK) {
		return err;
	}
	err = _write_world_sh_payload(p_file, p_gaussian_data, p_layout);
	if (err != OK) {
		return err;
	}
	err = _write_world_chunks(p_file, p_chunks, p_layout);
	if (err != OK) {
		return err;
	}
	err = _write_world_metadata(p_file, p_layout);
	if (err != OK) {
		return err;
	}
	return _ensure_file_write_ok(p_file, "save(final)");
}

} // namespace

static Ref<Resource> _load_gsplatworld_resource(const String &p_path, Error *r_error, float *r_progress, bool p_force_resident) {
	if (r_progress) {
		*r_progress = 0.0f;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		if (r_error) {
			*r_error = ERR_FILE_CANT_READ;
		}
		return Ref<Resource>();
	}

	const uint64_t file_len = file->get_length();
	if (file_len < kHeaderSizeBytes) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	const uint32_t magic = file->get_32();
	if (magic != kWorldMagic) {
		if (r_error) {
			*r_error = ERR_FILE_UNRECOGNIZED;
		}
		return Ref<Resource>();
	}

	const uint32_t version = file->get_32();
	if (version != kWorldVersion) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	const uint32_t flags = file->get_32();
	const uint32_t splat_count = file->get_32();
	const uint32_t sh_degree = file->get_32();
	if (sh_degree > kMaxShDegree) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}
	const uint32_t sh_first_order = file->get_32();
	const uint32_t sh_high_order = file->get_32();
	// Cap sh_first_order at the on-disk struct slot count. The saver clamps
	// here too. Do NOT cap sh_high_order with a magnitude limit and do NOT
	// enforce a canonical (degree+1)^2 mapping: GaussianData::set_gaussian_payload
	// accepts arbitrary high-order counts and arbitrary (degree, first_order,
	// high_order) triples, and ResourceFormatSaverGaussianSplatWorld writes
	// them verbatim. Magnitude is bounded structurally by `fits_within` plus
	// `_checked_mul_u64` on the SH payload range below.
	if (sh_first_order > kMaxShFirstOrder) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}
	const Vector3 bounds_pos = _read_vec3(file);
	const Vector3 bounds_size = _read_vec3(file);
	const uint32_t chunk_count = file->get_32();
	const uint64_t gaussian_offset = file->get_64();
	const uint64_t sh_offset = file->get_64();
	const uint64_t chunk_table_offset = file->get_64();
	const uint64_t indices_offset = file->get_64();
	const uint64_t metadata_offset = file->get_64();
	const uint64_t metadata_size = file->get_64();

	if (gaussian_offset >= file_len || gaussian_offset < kHeaderSizeBytes) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	const uint64_t gaussian_bytes = uint64_t(splat_count) * sizeof(Gaussian);
	const bool gaussian_data_compressed = (flags & kFlagCompressed) != 0;
	if (gaussian_data_compressed) {
		if (!fits_within(gaussian_offset, sizeof(uint64_t), file_len)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}

		const uint64_t saved_pos = file->get_position();
		file->seek(gaussian_offset);
		const uint64_t compressed_size = file->get_64();
		file->seek(saved_pos);

		const uint64_t compressed_capacity = file_len - (gaussian_offset + sizeof(uint64_t));
		if ((gaussian_bytes > 0 && compressed_size == 0) || compressed_size > compressed_capacity) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}

		// Bound splat_count on the compressed path before `gaussians.resize()`
		// below. The uncompressed path is bounded by the `fits_within` check in
		// the else branch; the compressed path is not, so a crafted splat_count
		// would otherwise abort the engine via an out-of-memory resize. The gzip
		// decompression limit rejects payloads that can never decompress, without
		// rejecting any valid (possibly high-ratio) file.
		if (gaussian_bytes > kMaxCompressedGaussianBytes) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	} else {
		if (!fits_within(gaussian_offset, gaussian_bytes, file_len)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	}

	if (sh_high_order > 0 && (flags & kFlagHasHighSh) == 0) {
		if (r_error) {
			*r_error = ERR_FILE_CORRUPT;
		}
		return Ref<Resource>();
	}

	if ((flags & kFlagHasHighSh) != 0 && sh_high_order > 0) {
		uint64_t sh_count = 0;
		uint64_t sh_bytes = 0;
		if (!checked_mul_u64(uint64_t(splat_count), uint64_t(sh_high_order), sh_count) ||
				!checked_mul_u64(sh_count, uint64_t(sizeof(Vector3)), sh_bytes)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
		// The SH coefficient array is held in a uint32_t-indexed LocalVector<Vector3>
		// by BOTH the resident payload (GaussianData, materialized below) and the
		// file-backed staged capture (StagedFileChunkPayloadSource, published below).
		// An element count above UINT32_MAX cannot be represented: the staged capture
		// resizes its destination with a 32-bit-truncated count yet copies the full
		// 64-bit width, overrunning the heap on a hostile world that declares a huge
		// (e.g. sparse ~51 GiB) SH extent while satisfying the byte-range `fits_within`
		// check below. Refuse such an asset here rather than truncate downstream. A
		// legitimate world never exceeds this bound (GaussianData itself cannot hold
		// more than UINT32_MAX coefficients), so this rejects only crafted inputs. This
		// is the *count* cap the extent-only checked_mul/fits_within guards deliberately
		// do not provide (#582).
		if (sh_count > uint64_t(UINT32_MAX)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			ERR_PRINT(vformat("[GaussianSplatWorld] Refusing to load %s: declared SH extent (%s coefficients = splat_count %d * sh_high_order %d) exceeds the 32-bit container capacity.",
					p_path, String::num_uint64(sh_count), splat_count, sh_high_order));
			return Ref<Resource>();
		}
		if (!fits_within(sh_offset, sh_bytes, file_len)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	}

	if ((flags & kFlagHasChunks) != 0 && chunk_count > 0) {
		const uint64_t chunk_table_bytes = uint64_t(chunk_count) * 56u;
		if (!fits_within(chunk_table_offset, chunk_table_bytes, file_len)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
		if (indices_offset >= file_len) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	}

	if ((flags & kFlagHasMetadata) != 0 && metadata_size > 0) {
		if (!fits_within(metadata_offset, metadata_size, file_len)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	}

	const bool resident_payload_file = (flags & kFlagResidentPayload) != 0;
	const bool should_materialize_resident_payload = gaussian_data_compressed || resident_payload_file || p_force_resident;
	LocalVector<Gaussian> gaussians;

	if (should_materialize_resident_payload) {
		// Platform-independent OOM guard. The caps above bound gaussian_bytes
		// (compressed: <= INT32_MAX; uncompressed: <= file_len), but a crafted file
		// can still declare a multi-GiB payload and reach `gaussians.resize()`,
		// which aborts via LocalVector's CRASH_COND when the allocation fails. Probe
		// the allocation fallibly first: `memalloc` returns null on failure (unlike
		// resize), so a hostile oversized payload becomes a clean ERR_OUT_OF_MEMORY
		// on EVERY platform (OS::get_memory_info is only implemented on Windows).
		// The probe is freed immediately; the resize below reuses the just-proven
		// available memory. This never rejects a payload that actually fits.
		if (gaussian_bytes > 0) {
			void *alloc_probe = memalloc(gaussian_bytes);
			if (alloc_probe == nullptr) {
				ERR_PRINT(vformat("[GaussianSplatWorld] Refusing to load %s: cannot allocate the declared resident payload (%d MiB).",
						p_path, int(gaussian_bytes / (1024 * 1024))));
				if (r_error) {
					*r_error = ERR_OUT_OF_MEMORY;
				}
				return Ref<Resource>();
			}
			memfree(alloc_probe);
		}
		gaussians.resize(splat_count);
		file->seek(gaussian_offset);

		if ((flags & kFlagCompressed) != 0) {
			// Compressed format: [8 bytes compressed_size][compressed_data].
			// This path is intentionally resident-only because the current
			// compressed file layout has no per-chunk random-access blocks.
			const uint64_t compressed_size = file->get_64();
			const uint64_t compressed_data_offset = file->get_position();
			if (!fits_within(compressed_data_offset, compressed_size, file_len)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
			PackedByteArray compressed_data;
			compressed_data.resize(compressed_size);
			if (!_read_exact(file, compressed_data.ptrw(), compressed_size)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
			if (!_decompress_data(compressed_data.ptr(), compressed_size,
					reinterpret_cast<uint8_t *>(gaussians.ptr()), gaussian_bytes)) {
				GS_LOG_ERROR_DEFAULT("Failed to decompress gaussian data");
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
			GS_LOG_STREAMING_INFO(vformat("Decompressed gaussian data: %d KB -> %d KB (%.1fx)",
					int(compressed_size / 1024), int(gaussian_bytes / 1024),
					float(gaussian_bytes) / float(compressed_size)));
		} else {
			// Explicit resident compatibility path for uncompressed worlds.
			if (!_read_exact(file, gaussians.ptr(), gaussian_bytes)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
		}
	}

	LocalVector<Vector3> sh_high_coeffs;
	if (should_materialize_resident_payload && (flags & kFlagHasHighSh) != 0 && sh_high_order > 0) {
		uint64_t sh_count = 0;
		uint64_t sh_bytes = 0;
		if (!checked_mul_u64(uint64_t(splat_count), uint64_t(sh_high_order), sh_count) ||
				!checked_mul_u64(sh_count, uint64_t(sizeof(Vector3)), sh_bytes)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
		sh_high_coeffs.resize(sh_count);
		file->seek(sh_offset);
		if (!_read_exact(file, sh_high_coeffs.ptr(), sh_bytes)) {
			if (r_error) {
				*r_error = ERR_FILE_CORRUPT;
			}
			return Ref<Resource>();
		}
	}

	Vector<ChunkRecord> chunk_records;
	if ((flags & kFlagHasChunks) != 0 && chunk_count > 0) {
		// #794: the read loop is bounded by chunk_count, not chunk_records.size(), so
		// an ignored resize failure would write past the end and hard-trap. chunk_count
		// comes straight off disk, so this allocation is attacker/corruption-influenced.
		// Fail closed exactly like the other header/payload read failures in this
		// function: ERR_FILE_CORRUPT is wrong here (the file may be fine), so report
		// the allocation failure itself.
		if (!gs_resize_or_fail(chunk_records, (int64_t)chunk_count,
					"GaussianSplatWorldIO::load chunk table")) {
			if (r_error) {
				*r_error = ERR_OUT_OF_MEMORY;
			}
			return Ref<Resource>();
		}
		file->seek(chunk_table_offset);
		for (uint32_t i = 0; i < chunk_count; i++) {
			chunk_records.write[i] = _read_chunk_record(file);
		}
	}

	Vector<uint32_t> all_indices;
	if (!chunk_records.is_empty()) {
		uint64_t total_indices = 0;
		for (int i = 0; i < chunk_records.size(); i++) {
			const ChunkRecord &record = chunk_records[i];
			if (record.indices_offset > UINT64_MAX - uint64_t(record.index_count)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				ERR_PRINT(vformat("[GaussianSplatWorld] Corrupt chunk index range: chunk=%d offset=%s count=%u overflows uint64.",
						i, String::num_uint64(record.indices_offset), record.index_count));
				return Ref<Resource>();
			}
			total_indices = MAX<uint64_t>(total_indices, record.indices_offset + uint64_t(record.index_count));
		}
		if (total_indices > 0) {
			uint64_t bytes = 0;
			if (!checked_mul_u64(total_indices, uint64_t(sizeof(uint32_t)), bytes)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
			if (!fits_within(indices_offset, bytes, file_len)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
			all_indices.resize(total_indices);
			file->seek(indices_offset);
			if (!_read_exact(file, all_indices.ptrw(), bytes)) {
				if (r_error) {
					*r_error = ERR_FILE_CORRUPT;
				}
				return Ref<Resource>();
			}
		}
	}

	Ref<GaussianData> gaussian_data;
	if (should_materialize_resident_payload) {
		gaussian_data.instantiate();
		gaussian_data->set_gaussian_payload(gaussians, sh_high_coeffs, sh_first_order, sh_high_order, (flags & kFlagIs2D) != 0);
	}

	Vector<StaticChunk> chunks;
	if (!chunk_records.is_empty()) {
		chunks.resize(chunk_records.size());
		for (int i = 0; i < chunk_records.size(); i++) {
			const ChunkRecord &record = chunk_records[i];
			StaticChunk chunk;
			chunk.bounds = AABB(record.bounds_pos, record.bounds_size);
			chunk.center = record.center;
			chunk.radius = record.radius;
			if (record.index_count > 0 && !all_indices.is_empty()) {
				chunk.indices.resize(record.index_count);
				const uint64_t src_offset = record.indices_offset;
				const uint64_t src_size = uint64_t(record.index_count);
				if (src_offset <= uint64_t(all_indices.size()) && src_size <= uint64_t(all_indices.size()) - src_offset) {
					memcpy(chunk.indices.ptrw(),
							all_indices.ptr() + size_t(src_offset),
							uint64_t(record.index_count) * sizeof(uint32_t));
				} else {
					if (r_error) {
						*r_error = ERR_FILE_CORRUPT;
					}
					ERR_PRINT(vformat("[GaussianSplatWorld] Corrupt chunk indices: chunk=%d offset=%s count=%u total=%d",
							i, String::num_uint64(src_offset), record.index_count, all_indices.size()));
					return Ref<Resource>();
				}
			}
			chunks.write[i] = chunk;
		}
	}

	Dictionary file_metadata;
	if ((flags & kFlagHasMetadata) != 0 && metadata_size > 0) {
		file->seek(metadata_offset);
		PackedByteArray metadata_bytes = file->get_buffer(metadata_size);
		String metadata_text = String::utf8((const char *)metadata_bytes.ptr(), metadata_bytes.size());
		Variant parsed = JSON::parse_string(metadata_text);
		if (parsed.get_type() == Variant::DICTIONARY) {
			file_metadata = parsed;
		}
	}

	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_payload_metadata(splat_count, sh_degree, sh_first_order, sh_high_order, (flags & kFlagIs2D) != 0);
	if (gaussian_data.is_valid()) {
		world->set_gaussian_data(gaussian_data);
	}
	world->set_bounds(AABB(bounds_pos, bounds_size));
	world->set_metadata(file_metadata);
	world->set_static_chunks(chunks);

	// Only uncompressed files support file-backed chunk payload reads. Compressed
	// .gsplatworld files are resident-only: the decoded payload lives in memory on
	// the GaussianData above and no staged-file payload source is attached.
	if (!gaussian_data_compressed && !resident_payload_file && !p_force_resident) {
		Ref<StagedFileChunkPayloadSource> file_source;
		file_source.instantiate();
		const uint64_t staged_sh_offset = ((flags & kFlagHasHighSh) != 0 && sh_high_order > 0) ? sh_offset : 0u;
		file_source->configure(p_path, gaussian_offset, staged_sh_offset,
				splat_count, sh_degree, sh_first_order, sh_high_order,
				AABB(bounds_pos, bounds_size));
		world->set_chunk_payload_source(file_source);
	}

	if (GaussianSplattingIO::is_data_log_enabled()) {
		GS_LOG_STREAMING_INFO(vformat("[GaussianSplatWorld] Loaded: %d splats, %d spatial chunks, bounds: pos=%s size=%s",
				splat_count, chunks.size(), bounds_pos, bounds_size));
	}

	if (r_error) {
		*r_error = OK;
	}
	if (r_progress) {
		*r_progress = 1.0f;
	}
	return world;
}

Ref<Resource> ResourceFormatLoaderGaussianSplatWorld::load(const String &p_path, const String &p_original_path,
		Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	(void)p_original_path;
	(void)p_use_sub_threads;
	(void)p_cache_mode;
	return _load_gsplatworld_resource(p_path, r_error, r_progress, false);
}

Ref<GaussianSplatWorld> ResourceFormatLoaderGaussianSplatWorld::load_resident(const String &p_path, Error *r_error) const {
	Ref<Resource> resource = _load_gsplatworld_resource(p_path, r_error, nullptr, true);
	Ref<GaussianSplatWorld> world = resource;
	return world;
}

void ResourceFormatLoaderGaussianSplatWorld::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gsplatworld");
}

bool ResourceFormatLoaderGaussianSplatWorld::handles_type(const String &p_type) const {
	return p_type == "GaussianSplatWorld";
}

String ResourceFormatLoaderGaussianSplatWorld::get_resource_type(const String &p_path) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "gsplatworld") {
		return "GaussianSplatWorld";
	}
	return "";
}

Error ResourceFormatSaverGaussianSplatWorld::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	return save_with_payload_mode(p_resource, p_path, SAVE_PAYLOAD_PRESERVE, p_flags);
}

Error ResourceFormatSaverGaussianSplatWorld::save_resident_compressed(const Ref<Resource> &p_resource,
		const String &p_path, uint32_t p_flags) {
	return save_with_payload_mode(p_resource, p_path, SAVE_PAYLOAD_RESIDENT_COMPRESSED, p_flags);
}

Error ResourceFormatSaverGaussianSplatWorld::save_resident_uncompressed(const Ref<Resource> &p_resource,
		const String &p_path, uint32_t p_flags) {
	return save_with_payload_mode(p_resource, p_path, SAVE_PAYLOAD_RESIDENT_UNCOMPRESSED, p_flags);
}

Error ResourceFormatSaverGaussianSplatWorld::save_with_payload_mode(const Ref<Resource> &p_resource,
		const String &p_path, PayloadSaveMode p_mode, uint32_t p_flags) {
	(void)p_flags;
	GaussianSplatWorld *world = Object::cast_to<GaussianSplatWorld>(*p_resource);
	ERR_FAIL_COND_V_MSG(world == nullptr, ERR_INVALID_PARAMETER, "Resource is not a GaussianSplatWorld.");

	Ref<GaussianData> gaussian_data = world->get_gaussian_data();
	const bool source_backed_without_resident = gaussian_data.is_null() && world->has_chunk_payload_source();
	if (source_backed_without_resident) {
		const Error materialize_err = _materialize_payload_source_for_save(world, gaussian_data);
		ERR_FAIL_COND_V_MSG(materialize_err != OK, materialize_err,
				vformat("Failed to read source-backed GaussianSplatWorld payload before save: %s", p_path));
	}
	ERR_FAIL_COND_V_MSG(gaussian_data.is_null(), ERR_INVALID_DATA, "GaussianSplatWorld has no GaussianData.");

	const Vector<StaticChunk> &chunks = world->get_static_chunks();
	WorldSaveLayout layout;
	const Error layout_err = _build_world_save_layout(world, gaussian_data, chunks, p_mode, layout);
	ERR_FAIL_COND_V_MSG(layout_err != OK, layout_err,
			vformat("Cannot build gsplatworld save layout for %s.", p_path));

	// Atomic write: stream to a sibling temp file and rename over p_path only on
	// full success, so a crash mid-save cannot destroy an existing world file.
	return gs_atomic_file_write(p_path, [&](const Ref<FileAccess> &file) -> Error {
		return _write_world_save_sections(file, world, gaussian_data, chunks, layout);
	});
}

void ResourceFormatSaverGaussianSplatWorld::get_recognized_extensions(const Ref<Resource> &p_resource,
		List<String> *p_extensions) const {
	if (recognize(p_resource)) {
		p_extensions->push_back("gsplatworld");
	}
}

bool ResourceFormatSaverGaussianSplatWorld::recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<GaussianSplatWorld>(*p_resource) != nullptr;
}

bool ResourceFormatSaverGaussianSplatWorld::recognize_path(const Ref<Resource> &p_resource, const String &p_path) const {
	const String ext = p_path.get_extension().to_lower();
	return ext == "gsplatworld" && recognize(p_resource);
}
