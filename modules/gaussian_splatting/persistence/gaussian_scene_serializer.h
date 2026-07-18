#ifndef GAUSSIAN_SCENE_SERIALIZER_H
#define GAUSSIAN_SCENE_SERIALIZER_H

#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/variant/variant.h"
#include "core/templates/local_vector.h"
#include "core/templates/hash_map.h"
#include "core/string/ustring.h"
#include "../core/gaussian_data.h"
#include "../animation/animation_state_machine.h"

namespace GaussianSplatting {

// Binary format constants
static const uint32_t GAUSSIAN_SCENE_MAGIC = 0x47534346; // "GSCF" - Gaussian Scene File
static const uint16_t GAUSSIAN_SCENE_VERSION = 2;
// Minimum reader version that can open files written by this version.
// A v1 reader can still open v2 files by skipping unknown chunks.
static const uint16_t GAUSSIAN_SCENE_MIN_READER_VERSION = 1;

// Chunk types for extensible binary format
enum class ChunkType : uint32_t {
    HEADER = 0x48454144,        // "HEAD"
    GAUSSIAN_DATA = 0x47415553,  // "GAUS"
    ANIMATION_DATA = 0x414E494D, // "ANIM"
    METADATA = 0x4D455441,       // "META"
    ASSET_REFS = 0x41535354,     // "ASST"
    COMPRESSION = 0x434F4D50,    // "COMP"
    END_OF_FILE = 0x454F4600     // "EOF\0"
};

// Compression types
enum class CompressionType : uint8_t {
    NONE = 0,
    ZSTD = 1,
    LZ4 = 2
};

struct ChunkHeader {
    ChunkType type;
    uint32_t size;
    uint32_t checksum;
    uint32_t flags;
};

struct SceneHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t total_chunks;
    uint32_t splat_count;
    float bounds_min[3];
    float bounds_max[3];
    uint64_t creation_time;
    uint64_t modification_time;
    // v2+: minimum reader version required to open this file.  Readers whose
    // GAUSSIAN_SCENE_VERSION is at least this value may open the file and
    // gracefully skip any chunk types they do not recognize.
    uint16_t minimum_reader_version;
    uint16_t _reserved_v2; // Padding / reserved for future use.
};

// On-disk packed size of SceneHeader (may differ from sizeof(SceneHeader)
// because of compiler struct padding).
static const uint32_t SCENE_HEADER_PACKED_SIZE = 60;

// On-disk size of a ChunkHeader (4 x uint32). Spelled out rather than taken from
// sizeof(ChunkHeader) so the wire format never depends on compiler padding.
static const uint32_t GSF_CHUNK_HEADER_SIZE = 16;

// Absolute ceiling for a compressed chunk whose decompressed size nothing else in
// the file independently corroborates (currently ANIMATION_DATA, a Variant-encoded
// clip dictionary measured in kilobytes). Chunks that ARE corroborated -- today
// GAUSSIAN_DATA, bounded by the scene header's splat_count -- use that instead.
// See the anti-decompress-bomb policy comment in gaussian_scene_serializer.cpp.
static const uint64_t GSF_MAX_UNCORROBORATED_DECOMPRESSED_CHUNK_SIZE = 64ull * 1024 * 1024;

// Floor for the load-path memory budget (see get_load_allocation_budget_bytes).
// A platform that cannot report its memory must not silently become MORE
// permissive than one that can, and every ordinary scene must still load.
static const uint64_t GSF_MIN_LOAD_ALLOCATION_BUDGET_BYTES = 256ull * 1024 * 1024;

struct AssetReference {
    String path;
    String type;
    uint64_t checksum;
    uint64_t file_size;
    uint64_t modification_time;
};

class GaussianSceneSerializer : public Resource {
    GDCLASS(GaussianSceneSerializer, Resource);

private:
    // Serialization settings
    CompressionType compression_type = CompressionType::ZSTD;
    int compression_level = 3;
    bool enable_checksum = true;
    bool incremental_mode = false;

    // Asset tracking
    LocalVector<AssetReference> asset_references;
    HashMap<String, int> asset_path_to_index;

    // Forward-compatibility: unknown chunks encountered during load are stored
    // here so they survive a round-trip (load -> re-save).  Each entry holds
    // the full raw chunk (ChunkHeader + payload) as a PackedByteArray.
    struct UnknownChunk {
        uint32_t type_raw; // Original ChunkType uint32_t value.
        uint32_t flags;
        uint32_t checksum;
        PackedByteArray payload;
    };
    Vector<UnknownChunk> unknown_chunks;

    // Staging sink for a transactional load (#601). The scene is parsed in full
    // into this structure; nothing is committed to the caller's GaussianData /
    // animation / metadata (or to this serializer's asset/unknown-chunk state)
    // until the ENTIRE structural parse succeeds. A late chunk error therefore
    // never leaves a half-mutated target. validate_file() (#602) reuses the exact
    // same parser with a throwaway staging sink so it can never be weaker than
    // load_scene().
    struct LoadStaging {
        LocalVector<Gaussian> gaussians;
        bool has_gaussian_chunk = false;
        Dictionary animation_dict;
        bool has_animation_chunk = false;
        Dictionary metadata;
        bool has_metadata_chunk = false;
        LocalVector<AssetReference> asset_references;
        bool has_asset_refs_chunk = false;
        Vector<UnknownChunk> unknown_chunks;
        // Chunks read inside the body loop (every chunk after HEADER, INCLUDING
        // the END_OF_FILE chunk). Used to enforce header.total_chunks on load.
        uint32_t chunks_parsed = 0;
        bool saw_eof_chunk = false;
    };

    // Cached checksum for the most recently written chunk. This keeps the
    // public API of `_write_chunk_header` simple while still allowing callers
    // to pre-compute the checksum once the payload has been assembled.
    mutable uint32_t pending_chunk_checksum = 0;

    // Internal serialization methods
    Error _write_chunk_header(Ref<FileAccess> file, ChunkType type, uint32_t size, uint32_t flags = 0);
    Error _write_scene_header(Ref<FileAccess> file, const SceneHeader& header);
    Error _write_gaussian_data_chunk(Ref<FileAccess> file, const ::GaussianData* gaussian_data);
    Error _write_animation_data_chunk(Ref<FileAccess> file, const GaussianAnimationStateMachine* animation);
    Error _write_metadata_chunk(Ref<FileAccess> file, const Dictionary& p_metadata);
    Error _write_asset_refs_chunk(Ref<FileAccess> file);
    // Streams the full scene to an already-open file. Invoked inside the atomic
    // write in save_scene() so a mid-write failure never truncates the target.
    Error _write_scene_to_file(const Ref<FileAccess>& file, const ::GaussianData* gaussian_data,
            const GaussianAnimationStateMachine* animation, const Dictionary& p_metadata);

    Error _read_chunk_header(Ref<FileAccess> file, ChunkHeader& header) const;
    Error _read_scene_header(Ref<FileAccess> file, SceneHeader& header) const;
    // Chunk readers decode into caller-owned staging out-params and never mutate
    // the target objects or this serializer, so the same code path drives both
    // load_scene() (which commits) and validate_file() (which discards).
    // p_declared_splat_count comes from the already-verified scene header and
    // caps how many decompressed bytes this chunk may claim (#603a).
    Error _read_gaussian_data_chunk(Ref<FileAccess> file, const ChunkHeader& header, uint32_t p_declared_splat_count, LocalVector<Gaussian>& r_gaussians) const;
    Error _read_animation_data_chunk(Ref<FileAccess> file, const ChunkHeader& header, Dictionary& r_animation_dict) const;
    Error _read_metadata_chunk(Ref<FileAccess> file, const ChunkHeader& header, Dictionary& r_metadata) const;
    Error _read_asset_refs_chunk(Ref<FileAccess> file, const ChunkHeader& header, LocalVector<AssetReference>& r_refs) const;

    // Transactional read pipeline (#601/#602). _read_scene_into_staging reads the
    // scene header, negotiates the version, then parses the full body into
    // `r_staging`, enforcing the declared chunk count and a terminating
    // END_OF_FILE chunk. It mutates nothing outside `r_staging`.
    Error _read_scene_body(const Ref<FileAccess>& file, const String& file_path, const SceneHeader& header, LoadStaging& r_staging) const;
    Error _read_scene_into_staging(const Ref<FileAccess>& file, const String& file_path, LoadStaging& r_staging) const;

    // True when the file advertises checksum protection (scene flag set, or any
    // non-zero chunk checksum anywhere in the file), so a checksum-disabled parse
    // never rubber-stamps a tampered file that was originally written
    // checksum-protected.
    bool _file_claims_checksum_protection(const Ref<FileAccess>& file) const;

    // Set at the start of every parse from _file_claims_checksum_protection().
    // When true, checksums are verified even on an instance with
    // `enable_checksum == false`. This is what makes load_scene() and
    // validate_file() reach the same verdict; the prescan used to run on the
    // validate path ONLY, so a checksum-disabled load accepted tampered protected
    // files that validate_file() rejected (#618 review, MAJOR 3).
    mutable bool checksum_claimed_by_file = false;

    // Compression helpers
    PackedByteArray _compress_data(const PackedByteArray& data, CompressionType type, bool& r_used_compression) const;
    PackedByteArray _decompress_data(const PackedByteArray& compressed_data, uint32_t original_size, CompressionType type,
            uint64_t corroborated_max_size) const;
    // The ONE place a chunk's compression contract is applied. Every compressible
    // chunk reader goes through this, and the codec is resolved purely from the
    // chunk's on-disk flags (never from this instance's compression_type), so
    // validate_file() and load_scene() cannot diverge (#602).
    Error _decode_chunk_payload(const PackedByteArray& raw, const ChunkHeader& header,
            uint64_t corroborated_max_size, const char* chunk_name, PackedByteArray& r_payload) const;

    // Script bindings
    void set_compression_type_bind(int type);
    int get_compression_type_bind() const;

    // Checksum calculation
    uint32_t _calculate_checksum(const PackedByteArray& data) const;
    bool _verify_checksum(const PackedByteArray& data, uint32_t expected_checksum) const;

    // Asset management
    void _track_asset(const String& path, const String& type);
    bool _is_asset_modified(const AssetReference& ref) const;

protected:
    static void _bind_methods();

public:
    GaussianSceneSerializer();
    ~GaussianSceneSerializer();

    // Main serialization interface
    Error save_scene(const String& file_path, const ::GaussianData* gaussian_data,
                     const GaussianAnimationStateMachine* animation = nullptr,
                     const Dictionary& p_metadata = Dictionary());

    Error load_scene(const String& file_path, ::GaussianData* gaussian_data,
                     GaussianAnimationStateMachine* animation = nullptr,
                     Dictionary* r_metadata = nullptr);

    // Scripting helpers that work with Ref<> types.
    Error save_scene_bind(const String& file_path, const Ref<::GaussianData>& gaussian_data,
            const Ref<GaussianAnimationStateMachine>& animation, Dictionary p_metadata = Dictionary());
    Error load_scene_bind(const String& file_path, const Ref<::GaussianData>& gaussian_data,
            const Ref<GaussianAnimationStateMachine>& animation = Ref<GaussianAnimationStateMachine>(),
            Variant p_metadata = Variant());
    Error save_incremental_bind(const String& file_path, const String& base_file_path,
            const Ref<::GaussianData>& gaussian_data,
            const Ref<GaussianAnimationStateMachine>& animation);
    Error load_incremental_bind(const String& file_path, const String& base_file_path,
            const Ref<::GaussianData>& gaussian_data,
            const Ref<GaussianAnimationStateMachine>& animation);

    // Incremental save/load for large scenes
    Error save_incremental(const String& file_path, const String& base_file_path,
                          const ::GaussianData* gaussian_data,
                          const GaussianAnimationStateMachine* animation = nullptr);

    Error load_incremental(const String& file_path, const String& base_file_path,
                          ::GaussianData* gaussian_data,
                          GaussianAnimationStateMachine* animation = nullptr);

    // Asset management
    void add_asset_reference(const String& path, const String& type);
    void remove_asset_reference(const String& path);
    bool has_asset_reference(const String& path) const;
    Array get_asset_references() const;
    bool validate_assets() const;

    // Settings
    void set_compression_type(CompressionType type) { compression_type = type; }
    CompressionType get_compression_type() const { return compression_type; }

    void set_compression_level(int level) { compression_level = CLAMP(level, 1, 22); }
    int get_compression_level() const { return compression_level; }

    void set_enable_checksum(bool enable) { enable_checksum = enable; }
    bool get_enable_checksum() const { return enable_checksum; }

    void set_incremental_mode(bool enable) { incremental_mode = enable; }
    bool get_incremental_mode() const { return incremental_mode; }

    // Utility methods
    Error validate_file(const String& file_path) const;
    Dictionary get_file_info(const String& file_path) const;
    uint64_t get_file_size_estimate(const ::GaussianData* gaussian_data,
                                   const GaussianAnimationStateMachine* animation = nullptr) const;

    // Unknown chunk round-trip accessors
    int get_unknown_chunk_count() const { return unknown_chunks.size(); }
    void clear_unknown_chunks() { unknown_chunks.clear(); }

    // Migration registry stub — provides the hook point for future per-chunk
    // data migrations.  Currently returns ``data`` unchanged.
    static PackedByteArray _migrate_chunk_v1_to_v2(uint32_t chunk_type, const PackedByteArray& data);

    // Static helpers
    static bool is_gaussian_scene_file(const String& file_path);
    static String get_file_extension() { return "gsf"; } // Gaussian Scene File
    static Array get_supported_compression_types();

    // Largest number of bytes codec `type` can PHYSICALLY emit from
    // `compressed_size` input bytes, given its container format (Zstd: one 4-byte
    // RLE block header per 128 KiB block; FastLZ level 2: one length byte per 255
    // copied bytes). Saturates instead of overflowing. Because this is a property
    // of the format, not of the data, it can never reject a stream our writer
    // produced. See the derivation in gaussian_scene_serializer.cpp.
    static uint64_t max_codec_expansion_bytes(CompressionType type, uint64_t compressed_size);

    // Absolute ceiling, in bytes, that a single chunk of a load may claim. Unlike
    // the codec/corroboration bounds -- which bound a chunk against the FILE --
    // this bounds it against the MACHINE, and is the only thing that stops an
    // internally CONSISTENT hostile file from forcing a multi-GiB allocation.
    // Derived from OS::get_memory_info() rather than being a tuned constant, so it
    // cannot re-introduce the "rejects legitimately large assets" regression.
    static uint64_t get_load_allocation_budget_bytes();
    // Test / embedder seam: pin the budget to an exact value (0 restores the
    // OS-derived default). Process-global and NOT thread-safe; intended to be set
    // once, from the main thread, around a load.
    static void set_load_allocation_budget_override(uint64_t bytes);

    // Anti-decompress-bomb bound for a compressed chunk (#603a): true when a chunk
    // declaring `original_size` decompressed bytes from `compressed_size` bytes is
    // acceptable. Bounded by (1) the uint32 on-disk field width, (2) the
    // corroborated ceiling the surrounding file structure justifies, (3) the
    // codec's physical maximum expansion, and (4) `memory_budget`, the absolute
    // allocation ceiling for this load. (1)-(3) are NOT tunable ratio heuristics,
    // which previously rejected this serializer's own highly-compressible output.
    // Pure (the budget is an explicit parameter, not read from global state) and
    // public + static so it can be unit-tested at the boundary without allocating
    // multi-GiB buffers. Not a bound (script) method.
    static bool is_decompressed_chunk_size_plausible(uint64_t original_size, uint64_t compressed_size,
            CompressionType type, uint64_t corroborated_max_size, uint64_t memory_budget);
};

} // namespace GaussianSplatting

#endif // GAUSSIAN_SCENE_SERIALIZER_H
