#include "gaussian_scene_serializer.h"
#include "incremental_saver.h"
#include "../io/gs_atomic_file_writer.h"

#include "core/io/compression.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "../logger/gs_logger.h"

#include <cstdint>
#include <cstring>

namespace GaussianSplatting {

namespace {

static const uint32_t CHUNK_FLAG_COMPRESSED = 1 << 0;
static const uint32_t MAX_SCENE_HEADER_CHUNK_SIZE = 64 * 1024; // Guard format probes against oversized header payloads.
static const uint16_t SCENE_FLAG_INCREMENTAL = 1u << 0;
static const uint16_t SCENE_FLAG_CHECKSUM_ENABLED = 1u << 1;
// Anti-decompress-bomb policy for a compressed chunk (#603a, revised by the
// PR #618 independent review).
//
// The previous policy was a single arbitrary decompressed:compressed ratio of
// 4096:1 with a 16 MiB floor. That was WRONG in the dangerous direction: it
// rejected files this serializer's own writer produces. Zstd reaches ~10,800:1
// on a legitimately repetitive scene (measured: 1,000,000 identical splats ->
// 144,000,004 B payload -> 13,347 compressed B), so any scene above the 16 MiB
// floor with strongly repeated splats saved fine and then failed to load. A
// reader that cannot read its own writer's output is data loss, so the ratio
// axis is no longer used as a policy knob at all.
//
// The declared size is now accepted only if it passes BOTH of these, neither of
// which is a tunable heuristic:
//
//   1. The codec's PHYSICAL maximum expansion. A codec cannot emit more bytes
//      than its own container format allows per input byte. For Zstd the bound
//      is one 4-byte RLE block header per 128 KiB output block => 32768:1
//      (verified empirically: incompressible-to-all-zero input tops out at
//      32,696:1). For FastLZ the long-match opcode is 3 bytes per <= 264-byte
//      copy => 88:1; 128:1 is used as a conservative overshoot. Because this is
//      a property of the format rather than of the data, it can NEVER reject a
//      stream the writer produced.
//
//   2. A CORROBORATED ceiling supplied by the caller: the largest decompressed
//      size the surrounding file structure independently justifies. For
//      GAUSSIAN_DATA that is the scene header's own splat_count (already read
//      and checksum-verified before the chunk), so a hostile chunk can no longer
//      claim a multi-GiB allocation for free -- the lie must also be consistent
//      with a separate, checksum-covered field. Chunks with no independent
//      corroboration (ANIMATION_DATA) get a hard absolute ceiling instead.
//
//   3. An absolute MEMORY BUDGET for the load (added by the #618 review, MAJOR 5).
//      (1) and (2) bound a chunk against the FILE, and an internally CONSISTENT
//      hostile file satisfies both: a header declaring ~29.8M splats plus a
//      genuinely ~32768:1-compressible Zstd frame corroborates its own ~4 GiB
//      claim. Only an absolute ceiling refuses that, and an absolute CONSTANT
//      would re-break legitimately large assets -- the regression 80657370486
//      already had to undo once. So the ceiling is derived from the MACHINE
//      (see get_load_allocation_budget_bytes) rather than tuned: it can never
//      reject a file the host could actually have held in memory.
static const uint64_t ZSTD_MAX_EXPANSION_RATIO = 32768; // 128 KiB block / 4 B RLE block header
// FastLZ, derived from the codec rather than guessed. Godot's MODE_FASTLZ calls
// fastlz_compress() on the whole buffer (core/io/compression.cpp:55-64), which
// selects LEVEL 2 for any input >= 65536 bytes (thirdparty/misc/fastlz.c:566-571).
// A previous value of 128 described level 1 only (its MAX_LEN is 264 bytes per
// 3-byte opcode => 88:1, fastlz.c:58,178-183). Level 2 has NO match-length cap:
// its match loop runs to the input bound (fastlz.c:402-421) and encodes the
// length as a run of 0xFF bytes, one per 255 output bytes
// (fastlz.c:444-447, decoded at fastlz.c:528-535). Per opcode group of k input
// bytes (1 opcode + (k-2) length bytes + 1 distance byte) the decoder emits at
// most 9 + 255*(k-2) bytes, i.e. strictly under 255 bytes per input byte and
// asymptotically equal to it. 255 is therefore the codec's true physical bound.
// The old 128 REJECTED this writer's own LZ4 output for any repetitive scene
// over 64 KiB -- the same data-loss class already fixed once for Zstd.
static const uint64_t FASTLZ_MAX_EXPANSION_RATIO = 255;
// Fixed per-frame container overhead that produces no output bytes (Zstd frame
// header is 6-18 B). Added to the compressed size so a minimal frame is never
// under-budgeted.
static const uint64_t CODEC_FRAME_OVERHEAD_ALLOWANCE = 32;
// 0 = derive the load memory budget from the OS (the default). Non-zero pins it,
// which is how the guard is exercised end-to-end without allocating gigabytes.
static uint64_t _load_allocation_budget_override = 0;
// The absolute ceiling for chunks nothing corroborates lives in the header as
// GSF_MAX_UNCORROBORATED_DECOMPRESSED_CHUNK_SIZE so tests can pin it.

Compression::Mode _to_compression_mode(CompressionType type) {
    switch (type) {
        case CompressionType::ZSTD:
            return Compression::MODE_ZSTD;
        case CompressionType::LZ4:
            return Compression::MODE_FASTLZ;
        default:
            return Compression::MODE_FASTLZ;
    }
}

uint32_t _fnv1a(const uint8_t *data, int64_t length) {
    const uint32_t fnv_prime = 16777619u;
    uint32_t hash = 2166136261u;
    for (int64_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= fnv_prime;
    }
    return hash;
}

// V1 header size (before minimum_reader_version was added).
static const uint32_t SCENE_HEADER_V1_SIZE = 56;
// V2 header size (includes minimum_reader_version + _reserved_v2).
static const uint32_t SCENE_HEADER_V2_SIZE = 60;

PackedByteArray _pack_scene_header(const SceneHeader &header) {
    PackedByteArray bytes;
    bytes.resize(SCENE_HEADER_V2_SIZE);
    uint8_t *w = bytes.ptrw();

    memcpy(w, &header.magic, sizeof(uint32_t));             // 0
    memcpy(w + 4, &header.version, sizeof(uint16_t));       // 4
    memcpy(w + 6, &header.flags, sizeof(uint16_t));         // 6
    memcpy(w + 8, &header.total_chunks, sizeof(uint32_t));  // 8
    memcpy(w + 12, &header.splat_count, sizeof(uint32_t));  // 12
    memcpy(w + 16, header.bounds_min, sizeof(float) * 3);   // 16
    memcpy(w + 28, header.bounds_max, sizeof(float) * 3);   // 28
    memcpy(w + 40, &header.creation_time, sizeof(uint64_t));      // 40
    memcpy(w + 48, &header.modification_time, sizeof(uint64_t));  // 48
    // v2 fields
    memcpy(w + 56, &header.minimum_reader_version, sizeof(uint16_t)); // 56
    memcpy(w + 58, &header._reserved_v2, sizeof(uint16_t));           // 58

    return bytes;
}

SceneHeader _unpack_scene_header(const PackedByteArray &bytes) {
    SceneHeader header = {};
    const uint8_t *r = bytes.ptr();
    const int64_t sz = bytes.size();

    memcpy(&header.magic, r, sizeof(uint32_t));
    memcpy(&header.version, r + 4, sizeof(uint16_t));
    memcpy(&header.flags, r + 6, sizeof(uint16_t));
    memcpy(&header.total_chunks, r + 8, sizeof(uint32_t));
    memcpy(&header.splat_count, r + 12, sizeof(uint32_t));
    memcpy(header.bounds_min, r + 16, sizeof(float) * 3);
    memcpy(header.bounds_max, r + 28, sizeof(float) * 3);
    memcpy(&header.creation_time, r + 40, sizeof(uint64_t));
    memcpy(&header.modification_time, r + 48, sizeof(uint64_t));

    // v2 fields — only present when the buffer is large enough.
    // A v1 file will have a 56-byte header; default to safe values.
    if (sz >= 60) {
        memcpy(&header.minimum_reader_version, r + 56, sizeof(uint16_t));
        memcpy(&header._reserved_v2, r + 58, sizeof(uint16_t));
    } else {
        // V1 files implicitly have minimum_reader_version == 1.
        header.minimum_reader_version = 1;
        header._reserved_v2 = 0;
    }

    return header;
}

// THE single authoritative decode contract for a chunk's compression (#602).
//
// It is derived ONLY from the chunk's own on-disk flags. No serializer instance
// state participates, so validate_file() and load_scene() cannot disagree about
// whether (or how) a chunk is compressed. The previous code fell back to the
// *instance's* `compression_type` when the codec bits were zero, which meant a
// default-constructed validation probe (Zstd) and a caller-configured loader
// (LZ4/NONE) applied different rules to the same bytes -- a file could validate
// OK and then fail or mis-decode on load.
//
// Fails closed: the compressed flag with an unknown/zero codec id is corruption,
// never an invitation to guess. This serializer's writer always stamps a real
// codec id (it only sets CHUNK_FLAG_COMPRESSED from ZSTD/LZ4), so no file it has
// ever produced is affected.
Error _resolve_chunk_compression(uint32_t chunk_flags, bool &r_compressed, CompressionType &r_type) {
    r_compressed = (chunk_flags & CHUNK_FLAG_COMPRESSED) != 0;
    r_type = CompressionType::NONE;
    if (!r_compressed) {
        return OK;
    }
    const uint32_t codec_id = (chunk_flags >> 8) & 0xFF;
    switch (codec_id) {
        case (uint32_t)CompressionType::ZSTD:
            r_type = CompressionType::ZSTD;
            return OK;
        case (uint32_t)CompressionType::LZ4:
            r_type = CompressionType::LZ4;
            return OK;
        default:
            ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, vformat(
                    "GSF chunk sets the compressed flag but declares unsupported codec id %d "
                    "(flags=0x%08X).",
                    (int64_t)codec_id, chunk_flags));
    }
}

Error _ensure_file_write_ok(const Ref<FileAccess> &file, const char *context) {
    ERR_FAIL_COND_V_MSG(file.is_null(), ERR_INVALID_PARAMETER, "FileAccess is null while checking write status.");
    const Error io_error = file->get_error();
    if (io_error != OK) {
        ERR_PRINT(vformat("[GaussianSceneSerializer] Write failure in %s (error=%d).",
                context ? context : "unknown",
                (int)io_error));
        return io_error;
    }
    return OK;
}

} // namespace

GaussianSceneSerializer::GaussianSceneSerializer() = default;
GaussianSceneSerializer::~GaussianSceneSerializer() = default;

void GaussianSceneSerializer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("save_scene", "file_path", "gaussian_data", "animation", "metadata"), &GaussianSceneSerializer::save_scene_bind, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("load_scene", "file_path", "gaussian_data", "animation", "metadata"),
            &GaussianSceneSerializer::load_scene_bind,
            DEFVAL(Ref<GaussianAnimationStateMachine>()),
            DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("save_incremental", "file_path", "base_file_path", "gaussian_data", "animation"), &GaussianSceneSerializer::save_incremental_bind, DEFVAL(Ref<GaussianAnimationStateMachine>()));
    ClassDB::bind_method(D_METHOD("load_incremental", "file_path", "base_file_path", "gaussian_data", "animation"), &GaussianSceneSerializer::load_incremental_bind, DEFVAL(Ref<GaussianAnimationStateMachine>()));
    ClassDB::bind_method(D_METHOD("add_asset_reference", "path", "type"), &GaussianSceneSerializer::add_asset_reference);
    ClassDB::bind_method(D_METHOD("remove_asset_reference", "path"), &GaussianSceneSerializer::remove_asset_reference);
    ClassDB::bind_method(D_METHOD("has_asset_reference", "path"), &GaussianSceneSerializer::has_asset_reference);
    ClassDB::bind_method(D_METHOD("get_asset_references"), &GaussianSceneSerializer::get_asset_references);
    ClassDB::bind_method(D_METHOD("validate_assets"), &GaussianSceneSerializer::validate_assets);
    ClassDB::bind_method(D_METHOD("validate_file", "file_path"), &GaussianSceneSerializer::validate_file);
    ClassDB::bind_method(D_METHOD("get_file_info", "file_path"), &GaussianSceneSerializer::get_file_info);
    ClassDB::bind_method(D_METHOD("get_file_size_estimate", "gaussian_data", "animation"), &GaussianSceneSerializer::get_file_size_estimate, DEFVAL(Variant()));
    ClassDB::bind_method(D_METHOD("set_compression_type", "type"), &GaussianSceneSerializer::set_compression_type_bind);
    ClassDB::bind_method(D_METHOD("get_compression_type"), &GaussianSceneSerializer::get_compression_type_bind);
    ClassDB::bind_method(D_METHOD("set_compression_level", "level"), &GaussianSceneSerializer::set_compression_level);
    ClassDB::bind_method(D_METHOD("get_compression_level"), &GaussianSceneSerializer::get_compression_level);
    ClassDB::bind_method(D_METHOD("set_enable_checksum", "enable"), &GaussianSceneSerializer::set_enable_checksum);
    ClassDB::bind_method(D_METHOD("get_enable_checksum"), &GaussianSceneSerializer::get_enable_checksum);
    ClassDB::bind_method(D_METHOD("set_incremental_mode", "enable"), &GaussianSceneSerializer::set_incremental_mode);
    ClassDB::bind_method(D_METHOD("get_incremental_mode"), &GaussianSceneSerializer::get_incremental_mode);
    ClassDB::bind_static_method("GaussianSceneSerializer", D_METHOD("is_gaussian_scene_file", "file_path"), &GaussianSceneSerializer::is_gaussian_scene_file);
    ClassDB::bind_static_method("GaussianSceneSerializer", D_METHOD("get_supported_compression_types"), &GaussianSceneSerializer::get_supported_compression_types);
}

void GaussianSceneSerializer::set_compression_type_bind(int type) {
    if (type < (int)CompressionType::NONE || type > (int)CompressionType::LZ4) {
        type = (int)CompressionType::NONE;
    }
    set_compression_type((CompressionType)type);
}

int GaussianSceneSerializer::get_compression_type_bind() const {
    return (int)get_compression_type();
}

Error GaussianSceneSerializer::_write_chunk_header(Ref<FileAccess> file, ChunkType type, uint32_t size, uint32_t flags) {
    ERR_FAIL_COND_V(file.is_null(), ERR_INVALID_PARAMETER);

    file->store_32((uint32_t)type);
    file->store_32(size);
    file->store_32(pending_chunk_checksum);
    file->store_32(flags);
    return _ensure_file_write_ok(file, "_write_chunk_header");
}

Error GaussianSceneSerializer::_write_scene_header(Ref<FileAccess> file, const SceneHeader &header) {
    PackedByteArray payload = _pack_scene_header(header);
    pending_chunk_checksum = enable_checksum ? _calculate_checksum(payload) : 0;
    Error err = _write_chunk_header(file, ChunkType::HEADER, payload.size());
    if (err != OK) {
        return err;
    }
    file->store_buffer(payload);
    return _ensure_file_write_ok(file, "_write_scene_header");
}

PackedByteArray GaussianSceneSerializer::_compress_data(const PackedByteArray &data, CompressionType type, bool &r_used_compression) const {
    r_used_compression = false;
    if (type == CompressionType::NONE || data.is_empty()) {
        return data;
    }

    Compression::Mode mode = _to_compression_mode(type);
    PackedByteArray result;
    int64_t max_size = Compression::get_max_compressed_buffer_size(data.size(), mode);
    result.resize(max_size);
    int64_t compressed_size = Compression::compress(result.ptrw(), data.ptr(), data.size(), mode);
    if (compressed_size <= 0) {
        GS_LOG_ERROR_DEFAULT("Failed to compress chunk. Falling back to raw data.");
        return data;
    }
    if (compressed_size >= data.size()) {
        return data;
    }

    r_used_compression = true;
    result.resize(compressed_size);
    return result;
}

uint64_t GaussianSceneSerializer::max_codec_expansion_bytes(CompressionType type, uint64_t compressed_size) {
    uint64_t ratio = 1;
    switch (type) {
        case CompressionType::ZSTD:
            ratio = ZSTD_MAX_EXPANSION_RATIO;
            break;
        case CompressionType::LZ4:
            ratio = FASTLZ_MAX_EXPANSION_RATIO;
            break;
        case CompressionType::NONE:
        default:
            return compressed_size; // Stored verbatim; no expansion is possible.
    }
    // Overflow-safe: saturate instead of wrapping.
    if (compressed_size > (UINT64_MAX / ratio) - CODEC_FRAME_OVERHEAD_ALLOWANCE) {
        return UINT64_MAX;
    }
    return (compressed_size + CODEC_FRAME_OVERHEAD_ALLOWANCE) * ratio;
}

uint64_t GaussianSceneSerializer::get_load_allocation_budget_bytes() {
    if (_load_allocation_budget_override != 0) {
        return _load_allocation_budget_override;
    }
    uint64_t budget = GSF_MIN_LOAD_ALLOCATION_BUDGET_BYTES;
    OS *os = OS::get_singleton();
    if (os != nullptr) {
        const Dictionary mem = os->get_memory_info();
        const Variant available = mem.get("available", Variant());
        if (available.get_type() == Variant::INT) {
            const int64_t available_bytes = available;
            // OS::get_memory_info() reports -1 for fields a platform cannot
            // determine (core/os/os.cpp:396-405); only a positive answer is usable.
            if (available_bytes > 0) {
                // Half, not all: the decode buffer and the staging vector that
                // receives it are live at the same time (see the residual note on
                // _read_gaussian_data_chunk), so a chunk of B bytes costs ~2B.
                budget = MAX(budget, uint64_t(available_bytes) / 2);
            }
        }
    }
    return budget;
}

void GaussianSceneSerializer::set_load_allocation_budget_override(uint64_t bytes) {
    _load_allocation_budget_override = bytes;
}

bool GaussianSceneSerializer::is_decompressed_chunk_size_plausible(uint64_t original_size, uint64_t compressed_size,
        CompressionType type, uint64_t corroborated_max_size, uint64_t memory_budget) {
    // 1. The declared size can never exceed the uint32 on-disk field width.
    if (original_size > uint64_t(UINT32_MAX)) {
        return false;
    }
    // 2. No input bytes means no output bytes.
    if (compressed_size == 0) {
        return original_size == 0;
    }
    // 3. Independent corroboration from elsewhere in the file (see the policy
    //    comment at the top of this file). This is what stops a hostile chunk
    //    from claiming a large allocation on its own say-so.
    if (original_size > corroborated_max_size) {
        return false;
    }
    // 4. The absolute memory budget for this load. This is the ONLY bound that
    //    refuses an internally consistent hostile file, because such a file
    //    corroborates its own claim (see the policy comment at the top).
    if (original_size > memory_budget) {
        return false;
    }
    // 5. The codec's physical maximum expansion. A property of the container
    //    format, so this can never reject a stream the writer produced.
    return original_size <= max_codec_expansion_bytes(type, compressed_size);
}

PackedByteArray GaussianSceneSerializer::_decompress_data(const PackedByteArray &compressed_data, uint32_t original_size,
        CompressionType type, uint64_t corroborated_max_size) const {
    if (type == CompressionType::NONE) {
        return compressed_data;
    }

    // Reject an implausible declared size BEFORE allocating, so a few hostile
    // header bytes cannot drive a multi-GiB allocation, while every stream this
    // serializer's writer can produce still loads (#603a).
    const uint64_t memory_budget = get_load_allocation_budget_bytes();
    ERR_FAIL_COND_V_MSG(!is_decompressed_chunk_size_plausible(uint64_t(original_size), uint64_t(compressed_data.size()), type, corroborated_max_size, memory_budget),
            PackedByteArray(),
            vformat("Gaussian scene chunk declares an implausible decompressed size (%d bytes from %d compressed; "
                    "codec max %d, corroborated max %d, memory budget %d).",
                    (int64_t)original_size, (int64_t)compressed_data.size(),
                    (int64_t)MIN(max_codec_expansion_bytes(type, uint64_t(compressed_data.size())), uint64_t(INT64_MAX)),
                    (int64_t)MIN(corroborated_max_size, uint64_t(INT64_MAX)),
                    (int64_t)MIN(memory_budget, uint64_t(INT64_MAX))));

    Compression::Mode mode = _to_compression_mode(type);
    PackedByteArray result;
    // A failed resize leaves `result` SHORTER than original_size; decompressing
    // into it would then overrun the heap. Fail closed instead of trusting it.
    ERR_FAIL_COND_V_MSG(result.resize(original_size) != OK, PackedByteArray(),
            vformat("Out of memory allocating %d bytes for a Gaussian scene chunk.", (int64_t)original_size));
    int64_t decompressed_size = Compression::decompress(result.ptrw(), original_size, compressed_data.ptr(), compressed_data.size(), mode);
    if (decompressed_size < 0) {
        ERR_FAIL_V_MSG(PackedByteArray(), "Failed to decompress Gaussian scene chunk.");
    }
    // The declared size is a contract, not a hint: a stream that decodes to a
    // different length than the chunk advertised is corrupt, not "short".
    ERR_FAIL_COND_V_MSG(uint64_t(decompressed_size) != uint64_t(original_size), PackedByteArray(),
            vformat("Gaussian scene chunk decompressed to %d bytes but declared %d.",
                    (int64_t)decompressed_size, (int64_t)original_size));
    return result;
}

Error GaussianSceneSerializer::_decode_chunk_payload(const PackedByteArray &raw, const ChunkHeader &header,
        uint64_t corroborated_max_size, const char *chunk_name, PackedByteArray &r_payload) const {
    // Single unwrap path shared by every compressible chunk reader, so the
    // compression contract is applied in exactly one place (#602).
    bool compressed = false;
    CompressionType type = CompressionType::NONE;
    const Error codec_err = _resolve_chunk_compression(header.flags, compressed, type);
    if (codec_err != OK) {
        return codec_err;
    }

    if (!compressed) {
        // The corroborated ceiling and the memory budget are properties of the
        // CHUNK, not of the codec, so they apply to a stored-verbatim payload
        // exactly as they do to a compressed one. They used to be skipped here,
        // which left uncompressed ANIMATION_DATA with no ceiling at all (#618
        // review, MAJOR 4). The bytes are already read at this point, but the
        // staging structures built from them are not, and a writer never produces
        // an uncompressed chunk larger than its corroborated size.
        const uint64_t raw_size = uint64_t(raw.size());
        ERR_FAIL_COND_V_MSG(raw_size > corroborated_max_size, ERR_FILE_CORRUPT,
                vformat("Uncompressed %s chunk is %d bytes, past the %d bytes the file corroborates.",
                        chunk_name, (int64_t)raw_size, (int64_t)MIN(corroborated_max_size, uint64_t(INT64_MAX))));
        const uint64_t memory_budget = get_load_allocation_budget_bytes();
        ERR_FAIL_COND_V_MSG(raw_size > memory_budget, ERR_FILE_CORRUPT,
                vformat("Uncompressed %s chunk is %d bytes, past this load's %d-byte memory budget.",
                        chunk_name, (int64_t)raw_size, (int64_t)MIN(memory_budget, uint64_t(INT64_MAX))));
        r_payload = raw;
        return OK;
    }

    ERR_FAIL_COND_V_MSG(raw.size() < (int)sizeof(uint32_t), ERR_FILE_CORRUPT,
            vformat("Compressed %s chunk is too small to carry its decompressed-size field.", chunk_name));

    uint32_t original_size = 0;
    const uint8_t *r = raw.ptr();
    memcpy(&original_size, r, sizeof(uint32_t));

    PackedByteArray compressed_payload;
    compressed_payload.resize(raw.size() - sizeof(uint32_t));
    if (!compressed_payload.is_empty()) {
        memcpy(compressed_payload.ptrw(), r + sizeof(uint32_t), compressed_payload.size());
    }

    r_payload = _decompress_data(compressed_payload, original_size, type, corroborated_max_size);
    // _decompress_data fails closed (empty result) on a rejected/failed decode;
    // surface that as corruption instead of parsing garbage.
    ERR_FAIL_COND_V_MSG(r_payload.is_empty() && original_size > 0, ERR_FILE_CORRUPT,
            vformat("Failed to decompress %s chunk.", chunk_name));
    return OK;
}

uint32_t GaussianSceneSerializer::_calculate_checksum(const PackedByteArray &data) const {
    if (data.is_empty()) {
        return 0;
    }
    return _fnv1a(data.ptr(), data.size());
}

bool GaussianSceneSerializer::_verify_checksum(const PackedByteArray &data, uint32_t expected_checksum) const {
    // THE single checksum policy for the read path (#618 review, MAJOR 3).
    //
    // `enable_checksum` is a WRITER setting. On read it may only ever ADD
    // verification, never remove verification the FILE asked for. Verification is
    // therefore skipped only when this instance has checksums disabled AND nothing
    // anywhere in the file claims checksum protection (`checksum_claimed_by_file`,
    // computed once per parse by the _file_claims_checksum_protection prescan).
    //
    // That prescan is unchanged; what changed is WHERE it runs. It used to gate
    // only validate_file()'s legacy fallback, so validate_file() was strictly
    // stricter than load_scene(): a checksum-disabled loader would happily load a
    // tampered protected file that validate_file() on the same instance rejected.
    // Running it at the start of every parse makes the two agree by construction,
    // and does so by raising the load path to the validate path's strength rather
    // than lowering the validate path.
    if (!enable_checksum && !checksum_claimed_by_file) {
        return true;
    }
    return _calculate_checksum(data) == expected_checksum;
}

Error GaussianSceneSerializer::_write_gaussian_data_chunk(Ref<FileAccess> file, const ::GaussianData *gaussian_data) {
    ERR_FAIL_NULL_V(gaussian_data, ERR_INVALID_PARAMETER);

    const LocalVector<Gaussian> &storage = gaussian_data->get_gaussian_storage();
    PackedByteArray payload;
    payload.resize(sizeof(uint32_t) + storage.size() * sizeof(Gaussian));
    uint8_t *w = payload.ptrw();
    uint32_t count = storage.size();
    memcpy(w, &count, sizeof(uint32_t));
    if (count > 0) {
        memcpy(w + sizeof(uint32_t), storage.ptr(), storage.size() * sizeof(Gaussian));
    }

    bool used_compression = false;
    PackedByteArray compressed = _compress_data(payload, compression_type, used_compression);
    PackedByteArray final_payload = payload;
    uint32_t flags = 0;
    if (used_compression) {
        final_payload.resize(sizeof(uint32_t) + compressed.size());
        uint8_t *dst = final_payload.ptrw();
        uint32_t original_size = payload.size();
        memcpy(dst, &original_size, sizeof(uint32_t));
        memcpy(dst + sizeof(uint32_t), compressed.ptr(), compressed.size());
        flags |= CHUNK_FLAG_COMPRESSED;
        flags |= (uint32_t)compression_type << 8;
    }
    pending_chunk_checksum = enable_checksum ? _calculate_checksum(final_payload) : 0;
    Error err = _write_chunk_header(file, ChunkType::GAUSSIAN_DATA, final_payload.size(), flags);
    if (err != OK) {
        return err;
    }
    file->store_buffer(final_payload);
    return _ensure_file_write_ok(file, "_write_gaussian_data_chunk");
}

Error GaussianSceneSerializer::_write_animation_data_chunk(Ref<FileAccess> file, const GaussianAnimationStateMachine *animation) {
    ERR_FAIL_NULL_V(animation, ERR_INVALID_PARAMETER);

    Dictionary dict = animation->to_dict();
    int len = 0;
    Error err = encode_variant(dict, nullptr, len, true);
    if (err != OK) {
        return err;
    }

    PackedByteArray payload;
    payload.resize(len);
    err = encode_variant(dict, payload.ptrw(), len, true);
    if (err != OK) {
        return err;
    }

    bool used_compression = false;
    PackedByteArray compressed = _compress_data(payload, compression_type, used_compression);
    PackedByteArray final_payload = payload;
    uint32_t flags = 0;
    if (used_compression) {
        final_payload.resize(sizeof(uint32_t) + compressed.size());
        uint8_t *dst = final_payload.ptrw();
        uint32_t original_size = payload.size();
        memcpy(dst, &original_size, sizeof(uint32_t));
        memcpy(dst + sizeof(uint32_t), compressed.ptr(), compressed.size());
        flags |= CHUNK_FLAG_COMPRESSED;
        flags |= (uint32_t)compression_type << 8;
    }
    pending_chunk_checksum = enable_checksum ? _calculate_checksum(final_payload) : 0;
    err = _write_chunk_header(file, ChunkType::ANIMATION_DATA, final_payload.size(), flags);
    if (err != OK) {
        return err;
    }
    file->store_buffer(final_payload);
    return _ensure_file_write_ok(file, "_write_animation_data_chunk");
}

Error GaussianSceneSerializer::_write_metadata_chunk(Ref<FileAccess> file, const Dictionary &p_metadata) {
    int len = 0;
    Error err = encode_variant(p_metadata, nullptr, len, true);
    if (err != OK) {
        return err;
    }
    PackedByteArray payload;
    payload.resize(len);
    err = encode_variant(p_metadata, payload.ptrw(), len, true);
    if (err != OK) {
        return err;
    }

    pending_chunk_checksum = enable_checksum ? _calculate_checksum(payload) : 0;
    err = _write_chunk_header(file, ChunkType::METADATA, payload.size());
    if (err != OK) {
        return err;
    }
    file->store_buffer(payload);
    return _ensure_file_write_ok(file, "_write_metadata_chunk");
}

Error GaussianSceneSerializer::_write_asset_refs_chunk(Ref<FileAccess> file) {
    PackedByteArray payload;
    uint32_t count = asset_references.size();
    if (count == 0) {
        return OK;
    }

    // Encode as array of dictionaries for flexibility.
    Array refs;
    refs.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        Dictionary dict;
        dict["path"] = asset_references[i].path;
        dict["type"] = asset_references[i].type;
        dict["checksum"] = (int64_t)asset_references[i].checksum;
        dict["file_size"] = (int64_t)asset_references[i].file_size;
        dict["modified"] = (int64_t)asset_references[i].modification_time;
        refs[i] = dict;
    }

    int len = 0;
    Error err = encode_variant(refs, nullptr, len, true);
    if (err != OK) {
        return err;
    }
    payload.resize(len);
    err = encode_variant(refs, payload.ptrw(), len, true);
    if (err != OK) {
        return err;
    }

    pending_chunk_checksum = enable_checksum ? _calculate_checksum(payload) : 0;
    err = _write_chunk_header(file, ChunkType::ASSET_REFS, payload.size());
    if (err != OK) {
        return err;
    }
    file->store_buffer(payload);
    return _ensure_file_write_ok(file, "_write_asset_refs_chunk");
}

Error GaussianSceneSerializer::_read_chunk_header(Ref<FileAccess> file, ChunkHeader &header) const {
    ERR_FAIL_COND_V(file.is_null(), ERR_INVALID_PARAMETER);
    if (file->eof_reached()) {
        return ERR_FILE_EOF;
    }

    // A chunk header is exactly GSF_CHUNK_HEADER_SIZE bytes on disk. FileAccess
    // does NOT report a short read: get_32() zero-fills the bytes it could not
    // read and returns normally, so a file ending mid-header used to yield a
    // fully-formed-looking header made of trailing zeros. That let a file
    // truncated inside its final 16-byte END_OF_FILE header still satisfy both
    // the terminator check and the declared chunk count, i.e. a truncated file
    // was accepted as complete. Require the whole header to actually be present.
    const uint64_t header_offset = file->get_position();
    const uint64_t file_length = file->get_length();
    if (header_offset >= file_length) {
        return ERR_FILE_EOF;
    }
    ERR_FAIL_COND_V_MSG(file_length - header_offset < uint64_t(GSF_CHUNK_HEADER_SIZE), ERR_FILE_CORRUPT,
            vformat("GSF chunk header is truncated: %d of %d bytes remain.",
                    (int64_t)(file_length - header_offset), (int64_t)GSF_CHUNK_HEADER_SIZE));

    header.type = (ChunkType)file->get_32();
    header.size = file->get_32();
    header.checksum = file->get_32();
    header.flags = file->get_32();
    return OK;
}

Error GaussianSceneSerializer::_read_scene_header(Ref<FileAccess> file, SceneHeader &header) const {
    ChunkHeader chunk_header;
    Error err = _read_chunk_header(file, chunk_header);
    if (err != OK) {
        return err;
    }
    ERR_FAIL_COND_V(chunk_header.type != ChunkType::HEADER, ERR_FILE_CORRUPT);
    // Accept headers at least as large as the v1 layout (56 bytes).
    ERR_FAIL_COND_V(chunk_header.size < SCENE_HEADER_V1_SIZE, ERR_FILE_CORRUPT);
    ERR_FAIL_COND_V(chunk_header.size > MAX_SCENE_HEADER_CHUNK_SIZE, ERR_FILE_CORRUPT);

    const uint64_t read_offset = file->get_position();
    const uint64_t file_length = file->get_length();
    ERR_FAIL_COND_V(file_length < read_offset, ERR_FILE_CORRUPT);
    const uint64_t remaining_bytes = file_length - read_offset;
    ERR_FAIL_COND_V(uint64_t(chunk_header.size) > remaining_bytes, ERR_FILE_CORRUPT);

    PackedByteArray buffer = file->get_buffer(chunk_header.size);
    ERR_FAIL_COND_V_MSG(uint64_t(buffer.size()) != uint64_t(chunk_header.size), ERR_FILE_CORRUPT,
            "GSF scene header payload is shorter than its declared size (truncated).");

    if (!_verify_checksum(buffer, chunk_header.checksum)) {
        return ERR_FILE_CORRUPT;
    }

    header = _unpack_scene_header(buffer);
    return OK;
}

Error GaussianSceneSerializer::_read_gaussian_data_chunk(Ref<FileAccess> file, const ChunkHeader &header,
        uint32_t p_declared_splat_count, LocalVector<Gaussian> &r_gaussians) const {
    PackedByteArray buffer = file->get_buffer(header.size);
    ERR_FAIL_COND_V_MSG(uint64_t(buffer.size()) != uint64_t(header.size), ERR_FILE_CORRUPT,
            "GAUSSIAN_DATA chunk payload is shorter than its declared size (truncated).");
    if (!_verify_checksum(buffer, header.checksum)) {
        return ERR_FILE_CORRUPT;
    }

    // Corroboration for the decompression bound: the scene header independently
    // declared how many splats this file holds, and it was read (and checksum
    // verified) before this chunk. That fixes the exact number of decompressed
    // bytes this chunk is entitled to, so the chunk's own declared size can no
    // longer authorise an allocation by itself (#603a).
    const uint64_t corroborated_max_size = uint64_t(sizeof(uint32_t)) + uint64_t(p_declared_splat_count) * uint64_t(sizeof(Gaussian));

    PackedByteArray payload;
    const Error decode_err = _decode_chunk_payload(buffer, header, corroborated_max_size, "GAUSSIAN_DATA", payload);
    if (decode_err != OK) {
        return decode_err;
    }

    ERR_FAIL_COND_V(payload.size() < (int)sizeof(uint32_t), ERR_FILE_CORRUPT);
    const uint8_t *r = payload.ptr();
    uint32_t count = 0;
    memcpy(&count, r, sizeof(uint32_t));

    // The GAUSSIAN_DATA schema is EXACT, not a lower bound. The old `>` test
    // accepted trailing decoded bytes and accepted a chunk count smaller than the
    // header's splat_count, so a file could disagree with itself about how many
    // splats it holds and still load "successfully" (#618 review, MAJOR 4).
    const uint64_t expected_payload_size = sizeof(uint32_t) + uint64_t(count) * sizeof(Gaussian);
    ERR_FAIL_COND_V_MSG(expected_payload_size != uint64_t(payload.size()), ERR_FILE_CORRUPT,
            vformat("GAUSSIAN_DATA payload is %d bytes but its declared %d splats imply exactly %d.",
                    (int64_t)payload.size(), (int64_t)count, (int64_t)expected_payload_size));
    ERR_FAIL_COND_V_MSG(count != p_declared_splat_count, ERR_FILE_CORRUPT,
            vformat("GAUSSIAN_DATA declares %d splats but the scene header declares %d.",
                    (int64_t)count, (int64_t)p_declared_splat_count));

    // Decode into the caller's staging vector; the canonical bulk commit happens
    // in load_scene() only once the whole file has parsed (transactional, #601).
    //
    // Format limitation (#600): the GAUSSIAN_DATA chunk carries only the per-splat
    // `Gaussian` struct bytes (which embed the first-order SH triplet `sh_1[3]`).
    // The high-order SH sidecar (`sh_high_order_coefficients`) is NOT persisted
    // by this format, and the 2D-mode flag is not persisted either. After load
    // both reset to defaults (sh_high_order_count == 0, is_2d_mode == false); the
    // writer emits a runtime warning when a save would drop them.
    //
    // Peak-memory note (#618 review, MAJOR 5): `payload` and `r_gaussians` are
    // both live across this copy, so a chunk of B decompressed bytes costs ~2B at
    // peak. get_load_allocation_budget_bytes() already halves the machine's
    // available memory to account for exactly this, so peak <= available memory.
    r_gaussians.clear();
    if (count > 0) {
        r_gaussians.resize(count);
        memcpy(r_gaussians.ptr(), r + sizeof(uint32_t), uint64_t(count) * sizeof(Gaussian));
    }

    return OK;
}

Error GaussianSceneSerializer::_read_animation_data_chunk(Ref<FileAccess> file, const ChunkHeader &header, Dictionary &r_animation_dict) const {
    PackedByteArray buffer = file->get_buffer(header.size);
    ERR_FAIL_COND_V_MSG(uint64_t(buffer.size()) != uint64_t(header.size), ERR_FILE_CORRUPT,
            "ANIMATION_DATA chunk payload is shorter than its declared size (truncated).");
    if (!_verify_checksum(buffer, header.checksum)) {
        return ERR_FILE_CORRUPT;
    }

    // Nothing else in the file corroborates an animation payload's decompressed
    // size, so it gets the hard absolute ceiling instead (see the policy comment
    // at the top of this file). Real clip dictionaries are kilobytes.
    PackedByteArray payload;
    const Error decode_err = _decode_chunk_payload(buffer, header,
            GSF_MAX_UNCORROBORATED_DECOMPRESSED_CHUNK_SIZE, "ANIMATION_DATA", payload);
    if (decode_err != OK) {
        return decode_err;
    }

    Variant var;
    Error err = decode_variant(var, payload.ptr(), payload.size(), nullptr, true);
    if (err != OK) {
        return err;
    }
    // Fail closed on a payload that does not decode into a Dictionary rather than
    // silently applying an empty clip set (staged; committed in load_scene()).
    ERR_FAIL_COND_V_MSG(var.get_type() != Variant::DICTIONARY, ERR_FILE_CORRUPT,
            "ANIMATION_DATA chunk did not decode into a Dictionary.");
    r_animation_dict = var;
    return OK;
}

Error GaussianSceneSerializer::_read_metadata_chunk(Ref<FileAccess> file, const ChunkHeader &header, Dictionary &r_metadata) const {
    PackedByteArray buffer = file->get_buffer(header.size);
    if (!_verify_checksum(buffer, header.checksum)) {
        return ERR_FILE_CORRUPT;
    }

    Variant var;
    Error err = decode_variant(var, buffer.ptr(), buffer.size(), nullptr, true);
    if (err != OK) {
        return err;
    }
    ERR_FAIL_COND_V_MSG(var.get_type() != Variant::DICTIONARY, ERR_FILE_CORRUPT,
            "METADATA chunk did not decode into a Dictionary.");
    r_metadata = var;
    return OK;
}

Error GaussianSceneSerializer::_read_asset_refs_chunk(Ref<FileAccess> file, const ChunkHeader &header, LocalVector<AssetReference> &r_refs) const {
    PackedByteArray buffer = file->get_buffer(header.size);
    if (!_verify_checksum(buffer, header.checksum)) {
        return ERR_FILE_CORRUPT;
    }

    Variant var;
    Error err = decode_variant(var, buffer.ptr(), buffer.size(), nullptr, true);
    if (err != OK) {
        return err;
    }
    ERR_FAIL_COND_V_MSG(var.get_type() != Variant::ARRAY, ERR_FILE_CORRUPT,
            "ASSET_REFS chunk did not decode into an Array.");

    Array refs = var;
    // Decode into the caller's staging vector; asset_references + the path index
    // are committed in load_scene() only after the whole file validates (#601).
    r_refs.clear();
    r_refs.resize(refs.size());
    for (int i = 0; i < refs.size(); i++) {
        Dictionary dict = refs[i];
        r_refs[i].path = dict.get("path", "");
        r_refs[i].type = dict.get("type", "");
        r_refs[i].checksum = dict.get("checksum", (int64_t)0);
        r_refs[i].file_size = dict.get("file_size", (int64_t)0);
        r_refs[i].modification_time = dict.get("modified", (int64_t)0);
    }

    return OK;
}

void GaussianSceneSerializer::_track_asset(const String &path, const String &type) {
    if (asset_path_to_index.has(path)) {
        int index = asset_path_to_index[path];
        asset_references[index].type = type;
        return;
    }

    AssetReference ref;
    ref.path = path;
    ref.type = type;

    Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_valid()) {
        ref.file_size = file->get_length();
        ref.modification_time = file->get_modified_time(path);
    }

    asset_path_to_index[path] = asset_references.size();
    asset_references.push_back(ref);
}

bool GaussianSceneSerializer::_is_asset_modified(const AssetReference &ref) const {
    Ref<FileAccess> file = FileAccess::open(ref.path, FileAccess::READ);
    if (file.is_null()) {
        return true;
    }
    uint64_t mtime = file->get_modified_time(ref.path);
    return mtime != ref.modification_time;
}

Error GaussianSceneSerializer::save_scene(const String &file_path, const ::GaussianData *gaussian_data, const GaussianAnimationStateMachine *animation, const Dictionary &p_metadata) {
    ERR_FAIL_NULL_V(gaussian_data, ERR_INVALID_PARAMETER);

    // Atomic write: a crash or write error mid-save must not truncate an existing
    // scene file (worst case the baseline the incremental system depends on).
    return gs_atomic_file_write(file_path, [&](const Ref<FileAccess> &file) -> Error {
        return _write_scene_to_file(file, gaussian_data, animation, p_metadata);
    });
}

Error GaussianSceneSerializer::_write_scene_to_file(const Ref<FileAccess> &file, const ::GaussianData *gaussian_data, const GaussianAnimationStateMachine *animation, const Dictionary &p_metadata) {
    // Honest lossy-save warning (#600). The GAUSSIAN_DATA chunk persists only the
    // per-splat `Gaussian` struct bytes (which embed first-order SH). The
    // high-order SH sidecar and the 2D-mode flag are NOT part of the .gsf format
    // yet, so they will be dropped on load. Warn instead of losing data silently.
    // A lossless versioned schema is deferred to the ADR for #600.
    if (gaussian_data != nullptr) {
        const uint32_t dropped_sh_high_order = gaussian_data->get_sh_high_order_count();
        const bool dropped_2d_mode = gaussian_data->get_2d_mode();
        if (dropped_sh_high_order > 0 || dropped_2d_mode) {
            WARN_PRINT(vformat(
                    "GaussianSceneSerializer: the .gsf format does not persist high-order "
                    "spherical-harmonic coefficients (%d per splat) or the 2D-mode flag "
                    "(is_2d_mode=%s); these will reset to defaults on load. Lossless "
                    "persistence is tracked by issue #600 (deferred to a format ADR).",
                    (int64_t)dropped_sh_high_order, dropped_2d_mode ? "true" : "false"));
        }
    }

    SceneHeader header = {};
    header.magic = GAUSSIAN_SCENE_MAGIC;
    header.version = GAUSSIAN_SCENE_VERSION;
    header.minimum_reader_version = GAUSSIAN_SCENE_MIN_READER_VERSION;
    header._reserved_v2 = 0;
    header.flags = 0;
    if (incremental_mode) {
        header.flags |= SCENE_FLAG_INCREMENTAL;
    }
    if (enable_checksum) {
        header.flags |= SCENE_FLAG_CHECKSUM_ENABLED;
    }
    uint32_t chunk_count = 1; // Scene header chunk.
    header.splat_count = gaussian_data->get_count();
    AABB bounds = gaussian_data->get_aabb();
    header.bounds_min[0] = bounds.position.x;
    header.bounds_min[1] = bounds.position.y;
    header.bounds_min[2] = bounds.position.z;
    header.bounds_max[0] = bounds.position.x + bounds.size.x;
    header.bounds_max[1] = bounds.position.y + bounds.size.y;
    header.bounds_max[2] = bounds.position.z + bounds.size.z;

    double now = Time::get_singleton()->get_unix_time_from_system();
    header.creation_time = (uint64_t)now;
    header.modification_time = (uint64_t)now;

    if (gaussian_data->get_count() > 0) {
        chunk_count++;
    }
    if (animation != nullptr && animation->get_clip_count() > 0) {
        chunk_count++;
    }
    if (!p_metadata.is_empty()) {
        chunk_count++;
    }
    if (!asset_references.is_empty()) {
        chunk_count++;
    }
    chunk_count += unknown_chunks.size(); // Round-trip preserved unknown chunks.
    chunk_count++; // EOF marker
    header.total_chunks = chunk_count;

    Error err = _write_scene_header(file, header);
    if (err != OK) {
        return err;
    }

    if (gaussian_data->get_count() > 0) {
        err = _write_gaussian_data_chunk(file, gaussian_data);
        if (err != OK) {
            return err;
        }
    }

    if (animation != nullptr && animation->get_clip_count() > 0) {
        err = _write_animation_data_chunk(file, animation);
        if (err != OK) {
            return err;
        }
    }

    if (!p_metadata.is_empty()) {
        err = _write_metadata_chunk(file, p_metadata);
        if (err != OK) {
            return err;
        }
    }

    if (!asset_references.is_empty()) {
        err = _write_asset_refs_chunk(file);
        if (err != OK) {
            return err;
        }
    }

    // Round-trip any unknown chunks that were preserved during load.
    for (int i = 0; i < unknown_chunks.size(); i++) {
        const UnknownChunk &unk = unknown_chunks[i];
        pending_chunk_checksum = unk.checksum;
        file->store_32(unk.type_raw);
        file->store_32(unk.payload.size());
        file->store_32(unk.checksum);
        file->store_32(unk.flags);
        if (!unk.payload.is_empty()) {
            file->store_buffer(unk.payload);
        }
        err = _ensure_file_write_ok(file, "write_unknown_chunk");
        if (err != OK) {
            return err;
        }
    }

    // End of file marker for extensibility.
    pending_chunk_checksum = 0;
    err = _write_chunk_header(file, ChunkType::END_OF_FILE, 0);
    if (err != OK) {
        return err;
    }
    return _ensure_file_write_ok(file, "save_scene");
}

Error GaussianSceneSerializer::_read_scene_body(const Ref<FileAccess> &file, const String &file_path, const SceneHeader &header, LoadStaging &r_staging) const {
    const uint64_t file_length = file->get_length();

    bool done = false;
    while (!done && !file->eof_reached()) {
        ChunkHeader chunk;
        Error err = _read_chunk_header(file, chunk);
        if (err != OK) {
            if (err == ERR_FILE_EOF) {
                break;
            }
            return err;
        }

        // Bound every chunk payload against the actual remaining file bytes
        // BEFORE reading it. This (a) rejects truncated files (#601) and
        // (b) caps get_buffer() so a hostile 32-bit chunk size cannot drive a
        // multi-GiB allocation (#603a).
        const uint64_t payload_offset = file->get_position();
        ERR_FAIL_COND_V_MSG(payload_offset > file_length, ERR_FILE_CORRUPT,
                "GSF chunk header extends past end of file: " + file_path);
        ERR_FAIL_COND_V_MSG(uint64_t(chunk.size) > file_length - payload_offset, ERR_FILE_CORRUPT,
                "GSF chunk payload extends past end of file (truncated?): " + file_path);

        r_staging.chunks_parsed++;

        switch (chunk.type) {
            case ChunkType::GAUSSIAN_DATA:
                err = _read_gaussian_data_chunk(file, chunk, header.splat_count, r_staging.gaussians);
                if (err == OK) {
                    r_staging.has_gaussian_chunk = true;
                }
                break;
            case ChunkType::ANIMATION_DATA:
                // Always decode (validates the chunk); commit is deferred and only
                // applied to the caller's animation object if one was provided.
                err = _read_animation_data_chunk(file, chunk, r_staging.animation_dict);
                if (err == OK) {
                    r_staging.has_animation_chunk = true;
                }
                break;
            case ChunkType::METADATA:
                err = _read_metadata_chunk(file, chunk, r_staging.metadata);
                if (err == OK) {
                    r_staging.has_metadata_chunk = true;
                }
                break;
            case ChunkType::ASSET_REFS:
                err = _read_asset_refs_chunk(file, chunk, r_staging.asset_references);
                if (err == OK) {
                    r_staging.has_asset_refs_chunk = true;
                }
                break;
            case ChunkType::END_OF_FILE:
                // #700: the terminator carries no payload. The writer always
                // emits it with size 0 (_write_chunk_header(..., END_OF_FILE, 0)),
                // so a non-zero size is a file the writer cannot have produced.
                // Accepting it let a crafted header hide bytes behind the
                // terminator: the loop stops here, so the declared payload was
                // never read and never counted, and `header.total_chunks` still
                // matched because the EOF header is not checksummed.
                ERR_FAIL_COND_V_MSG(chunk.size != 0, ERR_FILE_CORRUPT,
                        vformat("GSF END_OF_FILE chunk declares a %d-byte payload; the terminator must be empty: %s",
                                (int64_t)chunk.size, file_path));
                r_staging.saw_eof_chunk = true;
                done = true;
                break;
            default: {
                // Forward compatibility: skip unknown chunk types and preserve
                // them so they survive a round-trip (load -> re-save).
                WARN_PRINT(vformat(
                        "Unknown chunk type 0x%08X encountered in '%s', skipping %d bytes.",
                        (uint32_t)chunk.type, file_path, chunk.size));
                UnknownChunk unk;
                unk.type_raw = (uint32_t)chunk.type;
                unk.flags = chunk.flags;
                unk.checksum = chunk.checksum;
                if (chunk.size > 0) {
                    unk.payload = file->get_buffer(chunk.size);
                    ERR_FAIL_COND_V_MSG(unk.payload.size() != (int)chunk.size,
                            ERR_FILE_CORRUPT,
                            "Failed to read unknown chunk payload.");
                    // #700: verify under the SAME policy as every known chunk
                    // (_verify_checksum is the single read-path policy). Not
                    // verifying here did not merely tolerate corruption, it
                    // LAUNDERED it: the re-save path writes `unk.checksum` back
                    // over the possibly-tampered payload, so a later save would
                    // hand out a file whose checksum certifies the wrong bytes.
                    ERR_FAIL_COND_V_MSG(!_verify_checksum(unk.payload, chunk.checksum),
                            ERR_FILE_CORRUPT,
                            vformat("GSF unknown chunk 0x%08X failed checksum verification in '%s'.",
                                    (uint32_t)chunk.type, file_path));
                }
                r_staging.unknown_chunks.push_back(unk);
                err = OK;
            } break;
        }

        if (err != OK) {
            return err;
        }
    }

    // Structural-completeness contract (#601): a well-formed file MUST terminate
    // with an END_OF_FILE chunk and MUST contain exactly the chunk count the
    // header declared. A truncated file (loop ended on EOF-of-stream, not on the
    // EOF chunk) or a file with a missing/extra chunk is rejected instead of
    // silently loading as OK.
    ERR_FAIL_COND_V_MSG(!r_staging.saw_eof_chunk, ERR_FILE_CORRUPT,
            "GSF file is missing its END_OF_FILE chunk (truncated or incomplete): " + file_path);
    // #700: and the terminator must actually terminate. Without this, bytes
    // appended after the EOF chunk load as OK -- the loop stops at the
    // terminator, so appended data is never parsed, never counted against
    // `header.total_chunks`, and never checksummed. The writer leaves the
    // stream exactly at end-of-file after emitting the terminator, so any
    // remainder is content the writer did not produce.
    ERR_FAIL_COND_V_MSG(file->get_position() != file_length, ERR_FILE_CORRUPT,
            vformat("GSF file has %d byte(s) after its END_OF_FILE chunk: %s",
                    (int64_t)(file_length - file->get_position()), file_path));
    const uint64_t chunks_seen = uint64_t(r_staging.chunks_parsed) + 1; // + the HEADER chunk read before the body.
    ERR_FAIL_COND_V_MSG(chunks_seen != uint64_t(header.total_chunks), ERR_FILE_CORRUPT,
            vformat("GSF chunk count mismatch in '%s': header declares %d, parsed %d.",
                    file_path, (int64_t)header.total_chunks, (int64_t)chunks_seen));
    ERR_FAIL_COND_V_MSG(header.splat_count > 0 && !r_staging.has_gaussian_chunk, ERR_FILE_CORRUPT,
            vformat("GSF header declares %d splats but the file contains no GAUSSIAN_DATA chunk: %s",
                    (int64_t)header.splat_count, file_path));
    return OK;
}

Error GaussianSceneSerializer::_read_scene_into_staging(const Ref<FileAccess> &file, const String &file_path, LoadStaging &r_staging) const {
    // Resolve the file's checksum claim ONCE, before anything is parsed, and never
    // inherit it from a previous file read through this instance. Only needed when
    // this instance has checksums disabled -- otherwise everything is verified
    // anyway and the prescan would be wasted I/O (#618 review, MAJOR 3).
    checksum_claimed_by_file = false;
    if (!enable_checksum) {
        checksum_claimed_by_file = _file_claims_checksum_protection(file);
    }
    file->seek(0);

    SceneHeader scene_header;
    Error err = _read_scene_header(file, scene_header);
    if (err != OK) {
        return err;
    }
    ERR_FAIL_COND_V_MSG(scene_header.magic != GAUSSIAN_SCENE_MAGIC, ERR_FILE_UNRECOGNIZED, "Invalid Gaussian scene file: " + file_path);
    ERR_FAIL_COND_V_MSG(scene_header.version == 0, ERR_FILE_UNRECOGNIZED,
            "Invalid Gaussian scene version 0 in file: " + file_path);

    // Version negotiation: if the file is from a newer writer, check whether
    // this reader is still allowed to open it via minimum_reader_version.
    if (scene_header.version > GAUSSIAN_SCENE_VERSION) {
        if (scene_header.minimum_reader_version <= GAUSSIAN_SCENE_VERSION) {
            WARN_PRINT(vformat(
                    "Gaussian scene file '%s' is version %d (this reader supports up to %d). "
                    "Forward-compatible mode enabled (minimum reader version %d). "
                    "Unknown chunk types will be skipped.",
                    file_path, (int)scene_header.version,
                    (int)GAUSSIAN_SCENE_VERSION,
                    (int)scene_header.minimum_reader_version));
        } else {
            ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED, vformat(
                    "Gaussian scene file '%s' requires reader version %d or later "
                    "(this reader is version %d, file version %d).",
                    file_path, (int)scene_header.minimum_reader_version,
                    (int)GAUSSIAN_SCENE_VERSION, (int)scene_header.version));
        }
    }

    return _read_scene_body(file, file_path, scene_header, r_staging);
}

Error GaussianSceneSerializer::load_scene(const String &file_path, ::GaussianData *gaussian_data, GaussianAnimationStateMachine *animation, Dictionary *r_metadata) {
    ERR_FAIL_NULL_V(gaussian_data, ERR_INVALID_PARAMETER);
    Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
    ERR_FAIL_COND_V_MSG(file.is_null(), ERR_FILE_NOT_FOUND, "Unable to open Gaussian scene file: " + file_path);

    // Transactional load (#601): parse the ENTIRE file into a staging sink first.
    // Only when the structural parse fully succeeds do we commit to the caller's
    // objects, so a late chunk error can never leave the target half-mutated.
    LoadStaging staging;
    Error err = _read_scene_into_staging(file, file_path, staging);
    if (err != OK) {
        return err; // Caller targets are untouched.
    }

    // Commit staging -> caller targets. Committing gaussians through the canonical
    // bulk setter rebuilds octree/overlay/SH-metadata/content-revision state in one
    // place.
    //
    // This commit is UNCONDITIONAL (#618 review, BLOCKER 2). The writer omits the
    // GAUSSIAN_DATA chunk entirely for a 0-splat scene, so `staging.gaussians` is
    // empty and committing it correctly CLEARS the target. Guarding the commit on
    // `has_gaussian_chunk`, as this path used to, meant that saving an empty scene
    // and loading it back left the PREVIOUS scene's splats in the target while
    // still returning OK -- silent data corruption from an ordinary user action.
    // _read_scene_body already guarantees the two agree: a header declaring >0
    // splats with no GAUSSIAN_DATA chunk is rejected, and _read_gaussian_data_chunk
    // rejects a chunk whose count differs from the header's.
    gaussian_data->set_gaussians(staging.gaussians);
    if (animation && staging.has_animation_chunk) {
        animation->from_dict(staging.animation_dict);
    }
    if (r_metadata && staging.has_metadata_chunk) {
        *r_metadata = staging.metadata;
    }
    if (staging.has_asset_refs_chunk) {
        asset_references = staging.asset_references;
        asset_path_to_index.clear();
        for (uint32_t i = 0; i < asset_references.size(); i++) {
            asset_path_to_index[asset_references[i].path] = i;
        }
    }
    // Unknown chunks are always refreshed from this load so a subsequent re-save
    // round-trips exactly the chunks this file carried (and no stale ones).
    unknown_chunks = staging.unknown_chunks;

    return OK;
}

Error GaussianSceneSerializer::save_incremental(const String &file_path, const String &base_file_path, const ::GaussianData *gaussian_data, const GaussianAnimationStateMachine *animation) {
    ERR_FAIL_NULL_V(gaussian_data, ERR_INVALID_PARAMETER);

    Ref<GaussianIncrementalSaver> saver;
    Ref<GaussianIncrementalSaver> existing = gaussian_data->get_incremental_saver();
    if (existing.is_valid()) {
        saver = existing;
    } else {
        saver.instantiate();
        const_cast<::GaussianData *>(gaussian_data)->set_incremental_saver(saver);
    }

    if (!saver->is_tracking_enabled()) {
        saver->start_tracking(base_file_path);
    } else if (saver->get_baseline_file() != base_file_path) {
        saver->start_tracking(base_file_path);
    }

    if (animation != nullptr) {
        const_cast<GaussianAnimationStateMachine *>(animation)->set_incremental_saver(saver);
    }

    return saver->save_changes(file_path);
}

Error GaussianSceneSerializer::load_incremental(const String &file_path, const String &base_file_path, ::GaussianData *gaussian_data, GaussianAnimationStateMachine *animation) {
    ERR_FAIL_NULL_V(gaussian_data, ERR_INVALID_PARAMETER);
    Ref<GaussianIncrementalSaver> saver;
    Ref<GaussianIncrementalSaver> existing = gaussian_data->get_incremental_saver();
    if (existing.is_valid()) {
        saver = existing;
    } else {
        saver.instantiate();
        gaussian_data->set_incremental_saver(saver);
    }

    if (!saver->is_tracking_enabled()) {
        saver->start_tracking(base_file_path);
    }

    return saver->load_and_apply_changes(file_path, gaussian_data, animation);
}

Error GaussianSceneSerializer::save_scene_bind(const String &file_path, const Ref<::GaussianData> &gaussian_data,
        const Ref<GaussianAnimationStateMachine> &animation, Dictionary p_metadata) {
    ERR_FAIL_COND_V_MSG(gaussian_data.is_null(), ERR_INVALID_PARAMETER,
            "GaussianSceneSerializer::save_scene requires a valid GaussianData resource");

    const ::GaussianData *data_ptr = gaussian_data.ptr();
    const GaussianAnimationStateMachine *anim_ptr = animation.is_valid() ? animation.ptr() : nullptr;
    return save_scene(file_path, data_ptr, anim_ptr, p_metadata);
}

Error GaussianSceneSerializer::load_scene_bind(const String &file_path, const Ref<::GaussianData> &gaussian_data,
        const Ref<GaussianAnimationStateMachine> &animation, Variant p_metadata) {
    ERR_FAIL_COND_V_MSG(gaussian_data.is_null(), ERR_INVALID_PARAMETER,
            "GaussianSceneSerializer::load_scene requires a valid GaussianData resource");

    ::GaussianData *data_ptr = gaussian_data.ptr();
    GaussianAnimationStateMachine *anim_ptr = animation.is_valid() ? animation.ptr() : nullptr;

    Dictionary metadata_dict;
    Dictionary *metadata_ptr = nullptr;
    if (p_metadata.get_type() != Variant::NIL) {
        ERR_FAIL_COND_V_MSG(p_metadata.get_type() != Variant::DICTIONARY, ERR_INVALID_PARAMETER,
                "GaussianSceneSerializer::load_scene expects 'metadata' to be a Dictionary when provided");
        metadata_dict = p_metadata;
        metadata_ptr = &metadata_dict;
    }

    return load_scene(file_path, data_ptr, anim_ptr, metadata_ptr);
}

Error GaussianSceneSerializer::save_incremental_bind(const String &file_path, const String &base_file_path,
        const Ref<::GaussianData> &gaussian_data, const Ref<GaussianAnimationStateMachine> &animation) {
    ERR_FAIL_COND_V_MSG(gaussian_data.is_null(), ERR_INVALID_PARAMETER,
            "GaussianSceneSerializer::save_incremental requires a valid GaussianData resource");

    const ::GaussianData *data_ptr = gaussian_data.ptr();
    const GaussianAnimationStateMachine *anim_ptr = animation.is_valid() ? animation.ptr() : nullptr;
    return save_incremental(file_path, base_file_path, data_ptr, anim_ptr);
}

Error GaussianSceneSerializer::load_incremental_bind(const String &file_path, const String &base_file_path,
        const Ref<::GaussianData> &gaussian_data, const Ref<GaussianAnimationStateMachine> &animation) {
    ERR_FAIL_COND_V_MSG(gaussian_data.is_null(), ERR_INVALID_PARAMETER,
            "GaussianSceneSerializer::load_incremental requires a valid GaussianData resource");

    ::GaussianData *data_ptr = gaussian_data.ptr();
    GaussianAnimationStateMachine *anim_ptr = animation.is_valid() ? animation.ptr() : nullptr;
    return load_incremental(file_path, base_file_path, data_ptr, anim_ptr);
}

void GaussianSceneSerializer::add_asset_reference(const String &path, const String &type) {
    _track_asset(path, type);
}

void GaussianSceneSerializer::remove_asset_reference(const String &path) {
    if (!asset_path_to_index.has(path)) {
        return;
    }
    int index = asset_path_to_index[path];
    asset_path_to_index.erase(path);
    asset_references.remove_at(index);
    for (int i = index; i < (int)asset_references.size(); i++) {
        asset_path_to_index[asset_references[i].path] = i;
    }
}

bool GaussianSceneSerializer::has_asset_reference(const String &path) const {
    return asset_path_to_index.has(path);
}

Array GaussianSceneSerializer::get_asset_references() const {
    Array refs;
    refs.resize(asset_references.size());
    for (uint32_t i = 0; i < asset_references.size(); i++) {
        Dictionary dict;
        dict["path"] = asset_references[i].path;
        dict["type"] = asset_references[i].type;
        dict["checksum"] = (int64_t)asset_references[i].checksum;
        dict["file_size"] = (int64_t)asset_references[i].file_size;
        dict["modified"] = (int64_t)asset_references[i].modification_time;
        refs[i] = dict;
    }
    return refs;
}

bool GaussianSceneSerializer::validate_assets() const {
    for (uint32_t i = 0; i < asset_references.size(); i++) {
        if (_is_asset_modified(asset_references[i])) {
            return false;
        }
    }
    return true;
}

bool GaussianSceneSerializer::_file_claims_checksum_protection(const Ref<FileAccess> &file) const {
    file->seek(0);
    const uint64_t file_length = file->get_length();

    ChunkHeader head;
    if (_read_chunk_header(file, head) != OK) {
        return true; // Unreadable header -> treat as claiming protection (conservative).
    }
    if (head.type != ChunkType::HEADER) {
        return true;
    }
    if (head.checksum != 0) {
        return true; // The HEAD chunk carries a checksum.
    }
    const uint64_t head_payload_offset = file->get_position();
    if (head_payload_offset > file_length || uint64_t(head.size) > file_length - head_payload_offset) {
        return true;
    }
    if (head.size < SCENE_HEADER_V1_SIZE) {
        return true;
    }
    // Scene flags live at payload offset +6 (see _pack_scene_header).
    file->seek(head_payload_offset + 6);
    const uint16_t scene_flags = file->get_16();
    if (scene_flags & SCENE_FLAG_CHECKSUM_ENABLED) {
        return true;
    }

    // Any trailing chunk that carries a checksum also counts as claiming
    // protection. Scan the raw chunk headers up to the END_OF_FILE marker.
    file->seek(head_payload_offset + uint64_t(head.size));
    while (file->get_position() + uint64_t(GSF_CHUNK_HEADER_SIZE) <= file_length) {
        ChunkHeader ch;
        if (_read_chunk_header(file, ch) != OK) {
            return true;
        }
        if (ch.checksum != 0) {
            return true;
        }
        const uint64_t payload_offset = file->get_position();
        if (payload_offset > file_length || uint64_t(ch.size) > file_length - payload_offset) {
            return true;
        }
        if (ch.type == ChunkType::END_OF_FILE) {
            return false; // Reached EOF with no checksum claim anywhere.
        }
        file->seek(payload_offset + uint64_t(ch.size));
    }
    return true; // Never reached the EOF chunk -> conservative reject.
}

Error GaussianSceneSerializer::validate_file(const String &file_path) const {
    Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
    ERR_FAIL_COND_V(file.is_null(), ERR_FILE_NOT_FOUND);

    // #602: run the SAME transactional parser load_scene uses, into a throwaway
    // staging sink, so validate_file can never be weaker than an actual load.
    // The old checksum-success path returned after only the header and never
    // scanned the trailing chunks, so a file could pass validate_file and then
    // fail load_scene.
    //
    // #618 review, MAJOR 3: there is now exactly ONE probe, configured exactly as
    // this instance is. The previous strict-probe-then-legacy-fallback ladder gave
    // validate_file a STRICTER checksum policy than load_scene: a
    // checksum-disabled loader accepted a tampered, originally-protected file that
    // validate_file rejected. The _file_claims_checksum_protection prescan that
    // used to gate only this function's fallback now runs at the top of EVERY
    // parse (_read_scene_into_staging), so this single probe answers exactly the
    // question "would load_scene accept this file?" -- by raising the load path to
    // this function's strength, never by lowering this function's.
    GaussianSceneSerializer probe;
    probe.set_enable_checksum(enable_checksum);
    file->seek(0);
    LoadStaging staging;
    return probe._read_scene_into_staging(file, file_path, staging);
}

Dictionary GaussianSceneSerializer::get_file_info(const String &file_path) const {
    Dictionary info;
    Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::READ);
    if (file.is_null()) {
        info["valid"] = false;
        return info;
    }

    SceneHeader header;
    Error err = _read_scene_header(file, header);
    if (err != OK) {
        info["valid"] = false;
        return info;
    }

    info["valid"] = true;
    info["version"] = header.version;
    info["minimum_reader_version"] = header.minimum_reader_version;
    info["splat_count"] = header.splat_count;
    info["chunks"] = header.total_chunks;
    info["created"] = (int64_t)header.creation_time;
    info["modified"] = (int64_t)header.modification_time;
    return info;
}

uint64_t GaussianSceneSerializer::get_file_size_estimate(const ::GaussianData *gaussian_data, const GaussianAnimationStateMachine *animation) const {
    ERR_FAIL_NULL_V(gaussian_data, 0);
    uint64_t size = SCENE_HEADER_PACKED_SIZE + sizeof(ChunkHeader);
    size += sizeof(ChunkHeader) + sizeof(uint32_t) + gaussian_data->get_count() * sizeof(Gaussian);
    if (animation && animation->get_clip_count() > 0) {
        size += sizeof(ChunkHeader) + 4096; // Rough estimate for animation payloads.
    }
    return size;
}

bool GaussianSceneSerializer::is_gaussian_scene_file(const String &file_path) {
    if (!file_path.ends_with("." + get_file_extension())) {
        return false;
    }
    GaussianSceneSerializer serializer;
    return serializer.validate_file(file_path) == OK;
}

Array GaussianSceneSerializer::get_supported_compression_types() {
    Array arr;
    arr.push_back((int)CompressionType::NONE);
    arr.push_back((int)CompressionType::ZSTD);
    arr.push_back((int)CompressionType::LZ4);
    return arr;
}

PackedByteArray GaussianSceneSerializer::_migrate_chunk_v1_to_v2(uint32_t /*chunk_type*/, const PackedByteArray &data) {
    // Migration stub — currently returns data unchanged.
    // Future migrations can inspect chunk_type and transform the payload
    // from v1 layout to v2 layout here.
    return data;
}

} // namespace GaussianSplatting
