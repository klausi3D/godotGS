#ifndef GAUSSIAN_SPLAT_ASSET_H
#define GAUSSIAN_SPLAT_ASSET_H

#include "core/io/resource.h"
#include "core/io/image.h"
#include "scene/resources/texture.h"
#include "core/io/resource_loader.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/typed_array.h"

#include <atomic>

class GaussianData;
class ImageTexture;

class GaussianSplatAsset : public Resource {
    GDCLASS(GaussianSplatAsset, Resource);
    RES_BASE_EXTENSION("gaussiansplat");

public:
    enum AssetType {
        ASSET_TYPE_STATIC,  // Immutable, optimized for GPU
        ASSET_TYPE_DYNAMIC  // Editable, supports runtime modifications
    };

    enum CompressionFlags {
        COMPRESSION_NONE = 0,
        COMPRESSION_POSITIONS = 1 << 0,
        COMPRESSION_COLORS = 1 << 1,
        COMPRESSION_SCALES = 1 << 2,
        COMPRESSION_ROTATIONS = 1 << 3
    };

#ifdef TESTS_ENABLED
    // #798 review round 3: the allocation-failure contracts on this class (the
    // populate_from_gaussian_data() reset, and the merge refusal driven by the
    // structured getters) could not be exercised by any test, because a real
    // Vector::resize() failure cannot be provoked from a unit test at any size the
    // test itself can afford to allocate. Their "failure arm" assertions therefore
    // never ran -- a green test that proves nothing, which is precisely the defect
    // shape these guards exist to prevent.
    //
    // These arm-once hooks reproduce the exact post-failure buffer SHAPES that
    // cowdata.h leaves behind, so the production failure branches execute for real.
    // TESTS_ENABLED only: absent from editor and export builds (see #725).
    enum TestLaneFailure {
        TEST_LANE_FAILURE_NONE,
        // A fresh or CoW-forked lane whose _alloc() failed: _ptr stays null, so the
        // lane reads back EMPTY rather than short.
        TEST_LANE_FAILURE_EMPTY,
        // A failed GROW of an already-populated lane: the lane keeps its previous,
        // shorter size. This is the shape that would silently overflow the heap.
        TEST_LANE_FAILURE_SHORT,
        // A failed SHRINK of an already-populated lane: CowData::_fork_allocate()
        // returns before writing the new size when _realloc() fails, so the lane keeps
        // its previous, LARGER size. Applied to sh_high_order_coefficients, because that
        // lane's LENGTH is what _recalculate_sh_component_counts() derives the SH term
        // count from -- an oversized lane therefore inflates the term count, which the
        // write loop uses as the stride into the SOURCE GaussianData's (correctly sized,
        // hence shorter) high-order array. That is an out-of-bounds READ on the way to
        // sealing a corrupted asset, and unlike the SHORT shape a `size() < required`
        // test cannot see it.
        TEST_LANE_FAILURE_OVERSIZED,
    };
    enum TestGetterFailure {
        TEST_GETTER_FAILURE_NONE,
        // get_normal_vectors() output allocation failed. Deliberately a lane that
        // _ensure_buffer_sizes() sizes UNCONDITIONALLY to splat_count, so an empty
        // result can only mean OOM -- the case the merge guard must refuse rather
        // than paper over with the (0,1,0) default.
        TEST_GETTER_FAILURE_NORMALS,
    };
#endif

private:
    AssetType asset_type = ASSET_TYPE_STATIC;
    PackedFloat32Array positions;  // x,y,z packed
    PackedColorArray colors;        // RGBA colors
    PackedFloat32Array scales;      // 3D scales
    PackedFloat32Array rotations;   // Quaternions
    PackedFloat32Array sh_dc_coefficients;          // RGB coefficients for SH DC band
    PackedFloat32Array sh_first_order_coefficients; // First-order SH coefficients (variable count)
    PackedFloat32Array sh_high_order_coefficients;  // Higher-order SH coefficients
    PackedFloat32Array opacity_logits;              // Opacity logits per splat
    PackedInt32Array palette_ids;                   // Palette identifiers per splat
    PackedInt32Array painterly_flags;               // Shared storage for painterly flags / brush override IDs per splat
    PackedFloat32Array normals;                     // Optional per-splat normals
    PackedFloat32Array brush_axes;                  // Painterly brush axes
    PackedFloat32Array stroke_ages;                 // Painterly stroke age metadata

    // Streaming chunk bake (Phase B.1 startup optimization). Baked at import
    // so GaussianStreamingSystem::_build_chunks_for_data can skip the per-
    // splat center / bounds loop. Serialized as raw byte blobs of POD records
    // (see io/streaming_chunk_bake.h). streaming_chunk_size_used == 0 means
    // "no bake present" so old caches fall through to the runtime compute path.
    PackedByteArray streaming_chunk_records;
    PackedInt32Array streaming_primary_source_indices;
    PackedByteArray streaming_quantization_records;
    uint32_t streaming_chunk_size_used = 0;

    uint32_t splat_count = 0;
    // DATA-001: monotonic version bumped every time the payload is (re)populated via
    // populate_from_gaussian_data(). Unlike Resource::get_edited_version() (TOOLS-only, so
    // a compile-time 0 in exported builds and never bumped by procedural repopulation) it
    // is always live, letting the scene director detect that a DYNAMIC asset was
    // re-populated at runtime and rebuild its cached GaussianData. Guarded by populate_mutex.
    uint32_t payload_version = 0;
    uint32_t sh_first_order_terms = 0;
    uint32_t sh_high_order_terms = 0;
    uint32_t compression_flags = COMPRESSION_NONE;
    String import_quality_preset = "high";
    Dictionary import_metadata;
    Ref<Image> preview_image;
    mutable Ref<ImageTexture> preview_texture_cache;
    bool has_sh_dc_coefficients = false;
    mutable Ref<::GaussianData> gaussian_data_cache;
    // Serializes payload snapshots, hot-reload copy_from(), preview image/cache
    // state, and get_gaussian_data() materialization. Multi-field readers should
    // use capture_payload_snapshot() to copy a coherent asset generation under
    // one lock and then release it before doing heavier work.
    mutable Mutex populate_mutex;
    // Once the asset has been "handed out" as runtime authority (via
    // get_gaussian_data() or populate_from_gaussian_data()), the packed
    // arrays become read-only from external callers. Public setters,
    // including set_splat_count(), hard-fail while sealed so a live
    // GaussianData can never diverge from rewritten asset arrays.
    // Freshly loaded / newly instantiated assets start unsealed, so
    // normal import and deserialization paths still populate through
    // the public property setters before first runtime promotion.
    mutable bool payload_sealed = false;

#ifdef TESTS_ENABLED
    // Both are consumed (and cleared) by the single call they arm; see the enum
    // declarations above.
    TestLaneFailure test_lane_failure = TEST_LANE_FAILURE_NONE;
    mutable TestGetterFailure test_getter_failure = TEST_GETTER_FAILURE_NONE;
#endif

    static std::atomic<uint32_t> instance_count;

    void _recalculate_sh_component_counts();
    void _ensure_buffer_sizes();
    void _invalidate_gaussian_data_cache();
    void _invalidate_streaming_bake();
    void _invalidate_bounds_metadata();
    // Returns true when runtime mutation of the packed payload is still
    // allowed. Emits a loud diagnostic when the payload is sealed and the
    // caller is not the internal persistence path.
    bool _runtime_mutation_permitted(const char *p_method) const;

protected:
    static void _bind_methods();
    bool _set(const StringName &p_name, const Variant &p_value);
    bool _get(const StringName &p_name, Variant &r_ret) const;
    void _get_property_list(List<PropertyInfo> *p_list) const;

public:
    struct PayloadSnapshot {
        uint32_t splat_count = 0;
        uint32_t sh_first_order_terms = 0;
        uint32_t sh_high_order_terms = 0;
        uint32_t compression_flags = COMPRESSION_NONE;
        String import_quality_preset;
        Dictionary import_metadata;
        Ref<Image> preview_image;
        PackedFloat32Array positions;
        PackedColorArray colors;
        PackedFloat32Array scales;
        PackedFloat32Array rotations;
        PackedFloat32Array sh_dc_coefficients;
        PackedFloat32Array sh_first_order_coefficients;
        PackedFloat32Array sh_high_order_coefficients;
        PackedFloat32Array opacity_logits;
        PackedInt32Array palette_ids;
        PackedInt32Array painterly_flags;
        PackedFloat32Array normals;
        PackedFloat32Array brush_axes;
        PackedFloat32Array stroke_ages;
        PackedByteArray streaming_chunk_records;
        PackedInt32Array streaming_primary_source_indices;
        PackedByteArray streaming_quantization_records;
        uint32_t streaming_chunk_size_used = 0;
        bool has_sh_dc_coefficients = false;

        bool is_loaded() const { return splat_count > 0; }
    };

    GaussianSplatAsset();
    ~GaussianSplatAsset();

    // Returns true when the asset has been populated with splat data (splat_count > 0).
    bool is_loaded() const { return splat_count > 0; }

    // Override Resource::copy_from so engine-driven hot-reload via
    // ResourceLoader::load(..., CACHE_MODE_REPLACE) can repopulate a sealed
    // asset. The base implementation iterates storage properties and writes
    // them back through set(...) (core/io/resource.cpp:225-252); without
    // this override, a sealed GaussianSplatAsset would silently drop every
    // data/* setter and reload would appear to succeed while leaving stale
    // arrays in place.
    virtual Error copy_from(const Ref<Resource> &p_resource) override;

    void set_asset_type(AssetType p_type);
    AssetType get_asset_type() const { return asset_type; }
    // DATA-001: current payload version (see the member comment). Read under populate_mutex.
    uint32_t get_payload_version() const;

    void set_splat_count(uint32_t p_count);
    // Takes populate_mutex so concurrent prefetch workers cannot observe a torn
    // read against set_splat_count() / copy_from(). Mutex is recursive, so it
    // is safe to call from within a setter that already holds the lock.
    uint32_t get_splat_count() const;

    // Copies all payload arrays and metadata while holding populate_mutex once.
    // Use this for node/editor/runtime code that needs more than one field from
    // an asset; individual getters are for single-field compatibility reads.
    PayloadSnapshot capture_payload_snapshot() const;

    // Getters
    PackedFloat32Array get_positions() const;
    PackedVector3Array get_position_vectors() const;
    PackedColorArray get_colors() const;
    PackedFloat32Array get_scales() const;
    PackedVector3Array get_scale_vectors() const;
    PackedFloat32Array get_rotations() const;
    TypedArray<Quaternion> get_rotation_quaternions() const;
    PackedFloat32Array get_sh_dc_coefficients() const;
    PackedFloat32Array get_sh_first_order_coefficients() const;
    PackedFloat32Array get_sh_high_order_coefficients() const;
    PackedFloat32Array get_spherical_harmonics_buffer() const;
    PackedFloat32Array get_opacity_logits() const;
    PackedFloat32Array get_opacities() const;
    PackedInt32Array get_palette_ids() const;
    PackedInt32Array get_palette_ids_buffer() const;
    PackedInt32Array get_painterly_flags() const;
    PackedInt32Array get_painterly_flags_buffer() const;
    PackedInt32Array get_brush_override_ids() const;
    PackedInt32Array get_brush_override_ids_buffer() const;
    PackedFloat32Array get_normals() const;
    PackedVector3Array get_normal_vectors() const;
    PackedFloat32Array get_brush_axes() const;
    PackedVector2Array get_brush_axes_vector2() const;
    PackedFloat32Array get_stroke_ages() const;
    PackedFloat32Array get_stroke_ages_buffer() const;
    uint32_t get_sh_first_order_terms() const;
    uint32_t get_sh_high_order_terms() const;

    // Setters - needed for loaders to populate data
    void set_positions(const PackedFloat32Array &p_positions);
    void set_colors(const PackedColorArray &p_colors);
    void set_scales(const PackedFloat32Array &p_scales);
    void set_rotations(const PackedFloat32Array &p_rotations);
    void set_sh_dc_coefficients(const PackedFloat32Array &p_coefficients);
    void set_sh_first_order_coefficients(const PackedFloat32Array &p_coefficients);
    void set_sh_high_order_coefficients(const PackedFloat32Array &p_coefficients);
    void set_opacity_logits(const PackedFloat32Array &p_opacity_logits);
    void set_palette_ids(const PackedInt32Array &p_palette_ids);
    void set_painterly_flags(const PackedInt32Array &p_flags);
    void set_brush_override_ids(const PackedInt32Array &p_override_ids);
    void set_normals(const PackedFloat32Array &p_normals);
    void set_brush_axes(const PackedFloat32Array &p_brush_axes);
    void set_stroke_ages(const PackedFloat32Array &p_stroke_ages);
    void set_sh_component_terms(uint32_t p_first_order_terms, uint32_t p_high_order_terms);

    static uint32_t get_instance_count() { return instance_count.load(); }

    void set_import_metadata(const Dictionary &p_metadata);
    Dictionary get_import_metadata() const;

    void set_import_quality_preset(const String &p_preset);
    String get_import_quality_preset() const;

    void set_compression_flags(uint32_t p_flags);
    uint32_t get_compression_flags() const;

    void set_preview_image(const Ref<Image> &p_image);
    Ref<Image> get_preview_image() const;
    Ref<Texture2D> get_preview_texture() const;

    void set_thumbnail(const Ref<Texture2D> &p_thumbnail);
    Ref<Texture2D> get_thumbnail() const { return get_preview_texture(); }

    void set_source_path(const String &p_path);
    String get_source_path() const;

    Error load_from_file(const String &p_path);
    Ref<::GaussianData> get_gaussian_data() const;
    bool populate_gaussian_data(Ref<::GaussianData> &r_data) const;
    // True iff get_gaussian_data() would return immediately from cache. Used by the
    // parallel prefetch path to skip already-materialized assets.
    bool has_gaussian_data_cached() const;
    // Parallel batch materialization. Each WorkerThreadPool task calls
    // get_gaussian_data() on a distinct asset; assets that are already cached
    // are no-ops. INVARIANT: the assets MUST be stable (not mutated by other
    // threads) for the duration of this call. populate_gaussian_data() is pure
    // CPU SoA->AoS conversion — no RenderingDevice / RID work — and therefore
    // safe to run off the main thread per Godot's thread-safe APIs contract.
    static void prefetch_gaussian_data_parallel(const LocalVector<Ref<GaussianSplatAsset>> &p_assets);
    // GDScript-callable wrapper around prefetch_gaussian_data_parallel(). Accepts
    // a TypedArray and forwards valid entries to the LocalVector overload.
    static void prefetch_parallel(const TypedArray<GaussianSplatAsset> &p_assets);
    Error populate_from_gaussian_data(const Ref<::GaussianData> &p_gaussian_data);
    Error save_to_file(const String &p_path) const;

    // Import-time importance pruning (GS-PERF-PRUNE, issue #456). Thin adapter
    // that reuses GaussianData::prune_by_importance (slice 2a) on a materialized
    // copy of this asset's SoA payload, then writes the compacted arrays back so
    // the saved .res and any subsequent streaming-chunk bake both reflect the
    // pruned set. Returns the number of splats kept.
    //
    // No-op fast path: when p_keep_ratio >= 1.0 AND p_importance_threshold <= 0.0
    // the SoA arrays are left completely untouched (byte-identical, no seal, no
    // cache churn) -- the lossless default that keeps Ultra imports unchanged.
    uint32_t prune_by_importance(double p_keep_ratio, float p_importance_threshold);

    // Streaming-chunk bake accessors (Phase B.1).
    void set_streaming_chunk_records(const PackedByteArray &p_records);
    PackedByteArray get_streaming_chunk_records() const;
    void set_streaming_primary_source_indices(const PackedInt32Array &p_indices);
    PackedInt32Array get_streaming_primary_source_indices() const;
    void set_streaming_quantization_records(const PackedByteArray &p_records);
    PackedByteArray get_streaming_quantization_records() const;
    void set_streaming_chunk_size_used(uint32_t p_size);
    uint32_t get_streaming_chunk_size_used() const;

    bool has_baked_streaming_chunks() const {
        MutexLock cache_lock(populate_mutex);
        return streaming_chunk_size_used > 0 && !streaming_chunk_records.is_empty();
    }

#ifdef TESTS_ENABLED
    // Arms the NEXT populate_from_gaussian_data() to see p_mode's buffer shape, then
    // clears itself. EMPTY/SHORT are applied to the `positions` lane; OVERSIZED to
    // `sh_high_order_coefficients` (see TestLaneFailure for why the lane differs).
    void _test_force_next_populate_lane_failure(TestLaneFailure p_mode) { test_lane_failure = p_mode; }
    // Arms the NEXT matching structured getter to return its allocation-failure result
    // (the empty array), then clears itself. See TestGetterFailure.
    void _test_force_next_getter_failure(TestGetterFailure p_mode) const { test_getter_failure = p_mode; }
#endif
};

VARIANT_ENUM_CAST(GaussianSplatAsset::AssetType);
VARIANT_ENUM_CAST(GaussianSplatAsset::CompressionFlags);

#endif // GAUSSIAN_SPLAT_ASSET_H
