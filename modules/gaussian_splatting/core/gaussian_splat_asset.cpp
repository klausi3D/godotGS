#include "gaussian_splat_asset.h"
#include "../io/ply_loader.h"
#include "../io/spz_loader.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_importance.h" // ResidentAtlasBudget::gaussian_importance / select_top_k_indices (prune ranking)
#include "../core/gs_vector_alloc.h" // #798: gs_resize_or_fail() for resize-then-ptrw() outputs
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/math/quaternion.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/variant/typed_array.h"
#include "../logger/gs_logger.h"
#include "../logger/startup_trace.h"
#include "scene/resources/image_texture.h"

namespace {

static double _elapsed_msec(uint64_t p_start_usec) {
	const uint64_t now = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : p_start_usec;
	return double(now - p_start_usec) / 1000.0;
}

static GaussianDCEncoding _resolve_dc_encoding_from_metadata(const Dictionary &p_import_metadata) {
	if (p_import_metadata.has(StringName("dc_encoding"))) {
		const String dc_encoding = String(p_import_metadata[StringName("dc_encoding")]).to_lower();
		if (dc_encoding == "legacy_bias") {
			return GAUSSIAN_DC_ENCODING_LEGACY_BIAS;
		}
	}
	return GAUSSIAN_DC_ENCODING_LINEAR_RGB;
}

static Dictionary _build_runtime_load_timing(const String &p_stage,
		bool p_cache_hit,
		double p_source_ms,
		double p_materialize_ms,
		double p_total_ms,
		const Dictionary &p_source_stats) {
	Dictionary timing;
	timing[StringName("stage")] = p_stage;
	timing[StringName("cache_hit")] = p_cache_hit;
	timing[StringName("source_stage_ms")] = p_source_ms;
	timing[StringName("asset_materialization_ms")] = p_materialize_ms;
	timing[StringName("total_load_ms")] = p_total_ms;
	if (!p_source_stats.is_empty()) {
		timing[StringName("source_stats")] = p_source_stats;
	}
	return timing;
}

} // namespace

std::atomic<uint32_t> GaussianSplatAsset::instance_count{ 0 };

void GaussianSplatAsset::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_asset_type", "type"), &GaussianSplatAsset::set_asset_type);
    ClassDB::bind_method(D_METHOD("get_asset_type"), &GaussianSplatAsset::get_asset_type);

    ClassDB::bind_method(D_METHOD("is_loaded"), &GaussianSplatAsset::is_loaded);

    ClassDB::bind_method(D_METHOD("set_splat_count", "count"), &GaussianSplatAsset::set_splat_count);
    ClassDB::bind_method(D_METHOD("get_splat_count"), &GaussianSplatAsset::get_splat_count);

    // Getters
    ClassDB::bind_method(D_METHOD("get_positions"), &GaussianSplatAsset::get_positions);
    ClassDB::bind_method(D_METHOD("get_position_vectors"), &GaussianSplatAsset::get_position_vectors);
    ClassDB::bind_method(D_METHOD("get_colors"), &GaussianSplatAsset::get_colors);
    ClassDB::bind_method(D_METHOD("get_scales"), &GaussianSplatAsset::get_scales);
    ClassDB::bind_method(D_METHOD("get_scale_vectors"), &GaussianSplatAsset::get_scale_vectors);
    ClassDB::bind_method(D_METHOD("get_rotations"), &GaussianSplatAsset::get_rotations);
    ClassDB::bind_method(D_METHOD("get_rotation_quaternions"), &GaussianSplatAsset::get_rotation_quaternions);
    ClassDB::bind_method(D_METHOD("get_sh_dc_coefficients"), &GaussianSplatAsset::get_sh_dc_coefficients);
    ClassDB::bind_method(D_METHOD("get_sh_first_order_coefficients"), &GaussianSplatAsset::get_sh_first_order_coefficients);
    ClassDB::bind_method(D_METHOD("get_sh_high_order_coefficients"), &GaussianSplatAsset::get_sh_high_order_coefficients);
    ClassDB::bind_method(D_METHOD("get_spherical_harmonics_buffer"), &GaussianSplatAsset::get_spherical_harmonics_buffer);
    ClassDB::bind_method(D_METHOD("get_opacity_logits"), &GaussianSplatAsset::get_opacity_logits);
    ClassDB::bind_method(D_METHOD("get_opacities"), &GaussianSplatAsset::get_opacities);
    ClassDB::bind_method(D_METHOD("get_palette_ids"), &GaussianSplatAsset::get_palette_ids);
    ClassDB::bind_method(D_METHOD("get_palette_ids_buffer"), &GaussianSplatAsset::get_palette_ids_buffer);
    ClassDB::bind_method(D_METHOD("get_painterly_flags"), &GaussianSplatAsset::get_painterly_flags);
    ClassDB::bind_method(D_METHOD("get_painterly_flags_buffer"), &GaussianSplatAsset::get_painterly_flags_buffer);
    ClassDB::bind_method(D_METHOD("get_brush_override_ids"), &GaussianSplatAsset::get_brush_override_ids);
    ClassDB::bind_method(D_METHOD("get_brush_override_ids_buffer"), &GaussianSplatAsset::get_brush_override_ids_buffer);
    ClassDB::bind_method(D_METHOD("get_normals"), &GaussianSplatAsset::get_normals);
    ClassDB::bind_method(D_METHOD("get_normal_vectors"), &GaussianSplatAsset::get_normal_vectors);
    ClassDB::bind_method(D_METHOD("get_brush_axes"), &GaussianSplatAsset::get_brush_axes);
    ClassDB::bind_method(D_METHOD("get_brush_axes_vector2"), &GaussianSplatAsset::get_brush_axes_vector2);
    ClassDB::bind_method(D_METHOD("get_stroke_ages"), &GaussianSplatAsset::get_stroke_ages);
    ClassDB::bind_method(D_METHOD("get_stroke_ages_buffer"), &GaussianSplatAsset::get_stroke_ages_buffer);
    ClassDB::bind_method(D_METHOD("get_sh_first_order_terms"), &GaussianSplatAsset::get_sh_first_order_terms);
    ClassDB::bind_method(D_METHOD("get_sh_high_order_terms"), &GaussianSplatAsset::get_sh_high_order_terms);

    // Setters - needed for loaders to populate data
    ClassDB::bind_method(D_METHOD("set_positions", "positions"), &GaussianSplatAsset::set_positions);
    ClassDB::bind_method(D_METHOD("set_colors", "colors"), &GaussianSplatAsset::set_colors);
    ClassDB::bind_method(D_METHOD("set_scales", "scales"), &GaussianSplatAsset::set_scales);
    ClassDB::bind_method(D_METHOD("set_rotations", "rotations"), &GaussianSplatAsset::set_rotations);
    ClassDB::bind_method(D_METHOD("set_sh_dc_coefficients", "coefficients"), &GaussianSplatAsset::set_sh_dc_coefficients);
    ClassDB::bind_method(D_METHOD("set_sh_first_order_coefficients", "coefficients"), &GaussianSplatAsset::set_sh_first_order_coefficients);
    ClassDB::bind_method(D_METHOD("set_sh_high_order_coefficients", "coefficients"), &GaussianSplatAsset::set_sh_high_order_coefficients);
    ClassDB::bind_method(D_METHOD("set_opacity_logits", "opacity_logits"), &GaussianSplatAsset::set_opacity_logits);
    ClassDB::bind_method(D_METHOD("set_palette_ids", "palette_ids"), &GaussianSplatAsset::set_palette_ids);
    ClassDB::bind_method(D_METHOD("set_painterly_flags", "painterly_flags"), &GaussianSplatAsset::set_painterly_flags);
    ClassDB::bind_method(D_METHOD("set_brush_override_ids", "brush_override_ids"), &GaussianSplatAsset::set_brush_override_ids);
    ClassDB::bind_method(D_METHOD("set_normals", "normals"), &GaussianSplatAsset::set_normals);
    ClassDB::bind_method(D_METHOD("set_brush_axes", "brush_axes"), &GaussianSplatAsset::set_brush_axes);
    ClassDB::bind_method(D_METHOD("set_stroke_ages", "stroke_ages"), &GaussianSplatAsset::set_stroke_ages);
    ClassDB::bind_method(D_METHOD("set_sh_component_terms", "first_order_terms", "high_order_terms"), &GaussianSplatAsset::set_sh_component_terms);

    ClassDB::bind_method(D_METHOD("set_import_metadata", "metadata"), &GaussianSplatAsset::set_import_metadata);
    ClassDB::bind_method(D_METHOD("get_import_metadata"), &GaussianSplatAsset::get_import_metadata);
    ClassDB::bind_method(D_METHOD("set_import_quality_preset", "preset"), &GaussianSplatAsset::set_import_quality_preset);
    ClassDB::bind_method(D_METHOD("get_import_quality_preset"), &GaussianSplatAsset::get_import_quality_preset);
    ClassDB::bind_method(D_METHOD("set_compression_flags", "flags"), &GaussianSplatAsset::set_compression_flags);
    ClassDB::bind_method(D_METHOD("get_compression_flags"), &GaussianSplatAsset::get_compression_flags);
    ClassDB::bind_method(D_METHOD("set_preview_image", "image"), &GaussianSplatAsset::set_preview_image);
    ClassDB::bind_method(D_METHOD("get_preview_image"), &GaussianSplatAsset::get_preview_image);
    ClassDB::bind_method(D_METHOD("get_preview_texture"), &GaussianSplatAsset::get_preview_texture);
    ClassDB::bind_method(D_METHOD("set_thumbnail", "texture"), &GaussianSplatAsset::set_thumbnail);
    ClassDB::bind_method(D_METHOD("get_thumbnail"), &GaussianSplatAsset::get_thumbnail);
    ClassDB::bind_method(D_METHOD("set_source_path", "path"), &GaussianSplatAsset::set_source_path);
    ClassDB::bind_method(D_METHOD("get_source_path"), &GaussianSplatAsset::get_source_path);
    ClassDB::bind_method(D_METHOD("load_from_file", "path"), &GaussianSplatAsset::load_from_file);
    ClassDB::bind_method(D_METHOD("save_to_file", "path"), &GaussianSplatAsset::save_to_file);

    ClassDB::bind_method(D_METHOD("set_streaming_chunk_records", "records"), &GaussianSplatAsset::set_streaming_chunk_records);
    ClassDB::bind_method(D_METHOD("get_streaming_chunk_records"), &GaussianSplatAsset::get_streaming_chunk_records);
    ClassDB::bind_method(D_METHOD("set_streaming_primary_source_indices", "indices"), &GaussianSplatAsset::set_streaming_primary_source_indices);
    ClassDB::bind_method(D_METHOD("get_streaming_primary_source_indices"), &GaussianSplatAsset::get_streaming_primary_source_indices);
    ClassDB::bind_method(D_METHOD("set_streaming_quantization_records", "records"), &GaussianSplatAsset::set_streaming_quantization_records);
    ClassDB::bind_method(D_METHOD("get_streaming_quantization_records"), &GaussianSplatAsset::get_streaming_quantization_records);
    ClassDB::bind_method(D_METHOD("set_streaming_chunk_size_used", "size"), &GaussianSplatAsset::set_streaming_chunk_size_used);
    ClassDB::bind_method(D_METHOD("get_streaming_chunk_size_used"), &GaussianSplatAsset::get_streaming_chunk_size_used);

    ClassDB::bind_static_method("GaussianSplatAsset", D_METHOD("get_instance_count"), &GaussianSplatAsset::get_instance_count);
    ClassDB::bind_static_method("GaussianSplatAsset", D_METHOD("prefetch_parallel", "assets"), &GaussianSplatAsset::prefetch_parallel);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "asset_type", PROPERTY_HINT_ENUM, "Static,Dynamic"), "set_asset_type", "get_asset_type");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "splat_count"), "set_splat_count", "get_splat_count");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "import/quality_preset", PROPERTY_HINT_ENUM, "low,medium,high,ultra,custom"),
            "set_import_quality_preset", "get_import_quality_preset");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "import/compression_flags", PROPERTY_HINT_FLAGS, "Positions,Colors,Scales,Rotations"),
            "set_compression_flags", "get_compression_flags");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "import/metadata"), "set_import_metadata", "get_import_metadata");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "import/source_path", PROPERTY_HINT_FILE, "*.ply,*.spz"),
            "set_source_path", "get_source_path");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/positions"), "set_positions", "get_positions");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_COLOR_ARRAY, "data/colors"), "set_colors", "get_colors");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/scales"), "set_scales", "get_scales");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/rotations"), "set_rotations", "get_rotations");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/sh_dc"), "set_sh_dc_coefficients", "get_sh_dc_coefficients");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/sh_first_order"), "set_sh_first_order_coefficients", "get_sh_first_order_coefficients");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/sh_high_order"), "set_sh_high_order_coefficients", "get_sh_high_order_coefficients");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/opacity_logits"), "set_opacity_logits", "get_opacity_logits");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "data/palette_ids"), "set_palette_ids", "get_palette_ids");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "data/painterly_flags"), "set_painterly_flags", "get_painterly_flags");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/normals"), "set_normals", "get_normals");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/brush_axes"), "set_brush_axes", "get_brush_axes");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "data/stroke_ages"), "set_stroke_ages", "get_stroke_ages");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data/streaming_chunk_records", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE),
            "set_streaming_chunk_records", "get_streaming_chunk_records");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_INT32_ARRAY, "data/streaming_primary_source_indices", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE),
            "set_streaming_primary_source_indices", "get_streaming_primary_source_indices");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data/streaming_quantization_records", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE),
            "set_streaming_quantization_records", "get_streaming_quantization_records");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "data/streaming_chunk_size_used", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE),
            "set_streaming_chunk_size_used", "get_streaming_chunk_size_used");

    BIND_ENUM_CONSTANT(ASSET_TYPE_STATIC);
    BIND_ENUM_CONSTANT(ASSET_TYPE_DYNAMIC);
    BIND_ENUM_CONSTANT(COMPRESSION_NONE);
    BIND_ENUM_CONSTANT(COMPRESSION_POSITIONS);
    BIND_ENUM_CONSTANT(COMPRESSION_COLORS);
    BIND_ENUM_CONSTANT(COMPRESSION_SCALES);
    BIND_ENUM_CONSTANT(COMPRESSION_ROTATIONS);
}

bool GaussianSplatAsset::_set(const StringName &p_name, const Variant &p_value) {
    if (p_name == StringName("import/thumbnail")) {
        if (p_value.get_type() == Variant::NIL) {
            set_preview_image(Ref<Image>());
            return true;
        }

        Ref<Image> image = p_value;
        if (image.is_valid()) {
            set_preview_image(image);
            return true;
        }

        Ref<Texture2D> texture = p_value;
        if (texture.is_valid()) {
            ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), false,
                    "Legacy GaussianSplatAsset thumbnails must be converted on the main thread.");
            Ref<Image> texture_image = texture->get_image();
            if (texture_image.is_null()) {
                return true;
            }
            set_preview_image(texture_image);
            return true;
        }

        return false;
    }

    return false;
}

bool GaussianSplatAsset::_get(const StringName &p_name, Variant &r_ret) const {
    if (p_name == StringName("import/thumbnail")) {
        r_ret = preview_image;
        return true;
    }

    return false;
}

void GaussianSplatAsset::_get_property_list(List<PropertyInfo> *p_list) const {
    p_list->push_back(PropertyInfo(Variant::OBJECT, "import/thumbnail", PROPERTY_HINT_RESOURCE_TYPE, "Image"));
}

GaussianSplatAsset::GaussianSplatAsset() {
    instance_count++;
}

GaussianSplatAsset::~GaussianSplatAsset() {
    instance_count--;
}

void GaussianSplatAsset::_invalidate_gaussian_data_cache() {
    // Lock paired with get_gaussian_data() / has_gaussian_data_cached(). Setters
    // and populate_from_gaussian_data() are expected to run on the asset's owner
    // thread (the payload_sealed gate enforces that contract), but a worker
    // could still observe a torn cache ref without this synchronization.
    MutexLock cache_lock(populate_mutex);
    gaussian_data_cache.unref();
}

void GaussianSplatAsset::_invalidate_streaming_bake() {
    streaming_chunk_records = PackedByteArray();
    streaming_primary_source_indices = PackedInt32Array();
    streaming_quantization_records = PackedByteArray();
    streaming_chunk_size_used = 0;
}

bool GaussianSplatAsset::_runtime_mutation_permitted(const char *p_method) const {
    if (!payload_sealed) {
        return true;
    }
    ERR_PRINT(vformat(
            "[GaussianSplatAsset] %s() rejected: asset payload is sealed "
            "(runtime-authoritative). The Packed setters are not a supported "
            "runtime mutation API. Use GaussianData for runtime edits, or "
            "rebuild/populate a fresh asset before handing it out again.",
            p_method));
    return false;
}

Error GaussianSplatAsset::copy_from(const Ref<Resource> &p_resource) {
    // ResourceLoader's CACHE_MODE_REPLACE path applies replacements via
    // Resource::copy_from(), which iterates storage properties and writes
    // them back through set(...) (core/io/resource.cpp:225-252). Our packed
    // setters refuse mutations once payload_sealed is true (post first
    // get_gaussian_data()), so a replace-load on a previously hot asset
    // would silently drop every data/* property and the engine-driven
    // hot-reload would be a no-op. Unseal here so the engine's reload
    // semantics still work; the next get_gaussian_data() call re-seals
    // naturally on the next runtime hand-out.
    //
    // Hold populate_mutex across the entire unseal/copy/reseal cycle so a
    // concurrent prefetch worker cannot read torn source arrays while the
    // base Resource::copy_from() is rewriting them via the packed setters.
    MutexLock cache_lock(populate_mutex);
    const bool previous_seal = payload_sealed;
    payload_sealed = false;
    const Error err = Resource::copy_from(p_resource);
    if (err != OK) {
        // Restore seal on failure so a rejected copy (null/incompatible
        // resource) does not leave a runtime-authoritative asset mutable.
        payload_sealed = previous_seal;
    }
    return err;
}
void GaussianSplatAsset::_invalidate_bounds_metadata() {
    import_metadata.erase(StringName("bounds"));
    import_metadata[StringName("bounds_dirty")] = true;
}

void GaussianSplatAsset::set_asset_type(AssetType p_type) {
    if (asset_type != p_type) {
        asset_type = p_type;
        emit_changed();
    }
}

void GaussianSplatAsset::set_splat_count(uint32_t p_count) {
    // Hold populate_mutex across the seal check and the array resize so a
    // concurrent prefetch worker cannot observe torn source arrays.
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_splat_count")) {
        return;
    }
    if (splat_count != p_count) {
        splat_count = p_count;
        _ensure_buffer_sizes();
        import_metadata[StringName("splat_count")] = (int)p_count;
        _invalidate_bounds_metadata();
        _invalidate_gaussian_data_cache();
        // Bake describes per-chunk geometry; any splat-layout mutation stales it.
        // Safe to clear during deserialization because data/streaming_* properties
        // are registered AFTER data/* arrays in _bind_methods, so the bake install
        // setters run last and re-populate.
        _invalidate_streaming_bake();
        emit_changed();
    }
}

uint32_t GaussianSplatAsset::get_splat_count() const {
    // Lock pairs with set_splat_count() / copy_from() so concurrent readers
    // (prefetch worker filter, get_gaussian_data() early-out) see either the
    // pre- or post-update value, never a torn one.
    MutexLock cache_lock(populate_mutex);
    return splat_count;
}

GaussianSplatAsset::PayloadSnapshot GaussianSplatAsset::capture_payload_snapshot() const {
    MutexLock cache_lock(populate_mutex);
    PayloadSnapshot snapshot;
    snapshot.splat_count = splat_count;
    snapshot.sh_first_order_terms = sh_first_order_terms;
    snapshot.sh_high_order_terms = sh_high_order_terms;
    snapshot.compression_flags = compression_flags;
    snapshot.import_quality_preset = import_quality_preset;
    snapshot.import_metadata = import_metadata;
    snapshot.preview_image = preview_image;
    snapshot.positions = positions;
    snapshot.colors = colors;
    snapshot.scales = scales;
    snapshot.rotations = rotations;
    snapshot.sh_dc_coefficients = has_sh_dc_coefficients ? sh_dc_coefficients : PackedFloat32Array();
    snapshot.sh_first_order_coefficients = sh_first_order_coefficients;
    snapshot.sh_high_order_coefficients = sh_high_order_coefficients;
    snapshot.opacity_logits = opacity_logits;
    snapshot.palette_ids = palette_ids;
    snapshot.painterly_flags = painterly_flags;
    snapshot.normals = normals;
    snapshot.brush_axes = brush_axes;
    snapshot.stroke_ages = stroke_ages;
    snapshot.streaming_chunk_records = streaming_chunk_records;
    snapshot.streaming_primary_source_indices = streaming_primary_source_indices;
    snapshot.streaming_quantization_records = streaming_quantization_records;
    snapshot.streaming_chunk_size_used = streaming_chunk_size_used;
    snapshot.has_sh_dc_coefficients = has_sh_dc_coefficients;
    return snapshot;
}

// ---------------------------------------------------------------------------
// Raw-array getters: warn once when the asset has no loaded data so that
// callers can distinguish "empty because unloaded" from "legitimately empty".
// These use WARN_PRINT_ONCE because they may be called per-frame. Multi-field
// readers must prefer capture_payload_snapshot() so they take populate_mutex once
// and cannot observe fields from different copy_from()/reload generations.
// ---------------------------------------------------------------------------

PackedFloat32Array GaussianSplatAsset::get_positions() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && positions.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_positions() called on unloaded asset; returning empty array.");
    }
    return positions;
}

PackedColorArray GaussianSplatAsset::get_colors() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && colors.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_colors() called on unloaded asset; returning empty array.");
    }
    return colors;
}

PackedFloat32Array GaussianSplatAsset::get_scales() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && scales.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_scales() called on unloaded asset; returning empty array.");
    }
    return scales;
}

PackedFloat32Array GaussianSplatAsset::get_rotations() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && rotations.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_rotations() called on unloaded asset; returning empty array.");
    }
    return rotations;
}

PackedFloat32Array GaussianSplatAsset::get_sh_dc_coefficients() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && !has_sh_dc_coefficients) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_sh_dc_coefficients() called on unloaded asset; returning empty array.");
    }
    return has_sh_dc_coefficients ? sh_dc_coefficients : PackedFloat32Array();
}

PackedFloat32Array GaussianSplatAsset::get_sh_first_order_coefficients() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && sh_first_order_coefficients.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_sh_first_order_coefficients() called on unloaded asset; returning empty array.");
    }
    return sh_first_order_coefficients;
}

PackedFloat32Array GaussianSplatAsset::get_sh_high_order_coefficients() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && sh_high_order_coefficients.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_sh_high_order_coefficients() called on unloaded asset; returning empty array.");
    }
    return sh_high_order_coefficients;
}

PackedFloat32Array GaussianSplatAsset::get_opacity_logits() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && opacity_logits.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_opacity_logits() called on unloaded asset; returning empty array.");
    }
    return opacity_logits;
}

PackedInt32Array GaussianSplatAsset::get_palette_ids() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && palette_ids.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_palette_ids() called on unloaded asset; returning empty array.");
    }
    return palette_ids;
}

PackedInt32Array GaussianSplatAsset::get_painterly_flags() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && painterly_flags.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_painterly_flags() called on unloaded asset; returning empty array.");
    }
    return painterly_flags;
}

PackedInt32Array GaussianSplatAsset::get_brush_override_ids() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && painterly_flags.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_brush_override_ids() called on unloaded asset; returning empty array.");
    }
    return painterly_flags;
}

PackedFloat32Array GaussianSplatAsset::get_normals() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && normals.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_normals() called on unloaded asset; returning empty array.");
    }
    return normals;
}

PackedFloat32Array GaussianSplatAsset::get_brush_axes() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && brush_axes.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_brush_axes() called on unloaded asset; returning empty array.");
    }
    return brush_axes;
}

PackedFloat32Array GaussianSplatAsset::get_stroke_ages() const {
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0 && stroke_ages.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_stroke_ages() called on unloaded asset; returning empty array.");
    }
    return stroke_ages;
}

// ---------------------------------------------------------------------------
// Structured getters: these convert raw data into higher-level types and
// silently fill fallback values when individual splat data is missing.
// They use WARN_PRINT_ONCE for the "asset not loaded at all" case.
//
// #798: every one of these sizes its output to splat_count (or a multiple of it)
// and then writes through a raw ptrw() with the loop bounded by splat_count, NOT
// by result.size(). Vector::resize() reports OOM only through its return value and
// leaves the vector empty, so ptrw() would hand back nullptr and the loop would
// write the whole asset through address 0 -- no CRASH_BAD_INDEX, no diagnostic.
// splat_count is file-derived and unbounded, so these are exactly the allocations
// that can realistically fail. The failure result is the empty array each getter
// already returns for an unloaded asset (the splat_count == 0 branch above it);
// consumers such as gaussian_splat_merge_sources() already index these defensively
// against the returned size() and substitute per-field defaults.
// ---------------------------------------------------------------------------

PackedVector3Array GaussianSplatAsset::get_position_vectors() const {
    MutexLock cache_lock(populate_mutex);
    PackedVector3Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_position_vectors() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_position_vectors")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    Vector3 *write = result.ptrw();
    const float *read = positions.ptr();
    const int available = positions.size();

    if (available >= int(splat_count) * 3 && read != nullptr) {
        for (uint32_t i = 0; i < splat_count; i++) {
            const uint32_t base = i * 3u;
            write[i] = Vector3(read[base + 0], read[base + 1], read[base + 2]);
        }
        return result;
    }

    for (uint32_t i = 0; i < splat_count; i++) {
        const uint32_t base = i * 3u;
        if (available >= int(base + 3u) && read != nullptr) {
            write[i] = Vector3(read[base + 0], read[base + 1], read[base + 2]);
        } else {
            write[i] = Vector3();
        }
    }

    return result;
}

PackedVector3Array GaussianSplatAsset::get_scale_vectors() const {
    MutexLock cache_lock(populate_mutex);
    PackedVector3Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_scale_vectors() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_scale_vectors")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    Vector3 *write = result.ptrw();
    const float *read = scales.ptr();
    const int available = scales.size();

    if (available >= int(splat_count) * 3 && read != nullptr) {
        for (uint32_t i = 0; i < splat_count; i++) {
            const uint32_t base = i * 3u;
            write[i] = Vector3(read[base + 0], read[base + 1], read[base + 2]);
        }
        return result;
    }

    for (uint32_t i = 0; i < splat_count; i++) {
        const uint32_t base = i * 3u;
        if (available >= int(base + 3u) && read != nullptr) {
            write[i] = Vector3(read[base + 0], read[base + 1], read[base + 2]);
        } else {
            write[i] = Vector3(1.0f, 1.0f, 1.0f);
        }
    }

    return result;
}

TypedArray<Quaternion> GaussianSplatAsset::get_rotation_quaternions() const {
    MutexLock cache_lock(populate_mutex);
    TypedArray<Quaternion> result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_rotation_quaternions() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    result.resize(splat_count);
    const int available = rotations.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        if (available >= int(i * 4 + 4)) {
            const float w = rotations[i * 4 + 0];
            const float x = rotations[i * 4 + 1];
            const float y = rotations[i * 4 + 2];
            const float z = rotations[i * 4 + 3];
            const float len_sq = w * w + x * x + y * y + z * z;
            if (Math::is_zero_approx(len_sq)) {
                result[i] = Quaternion();
            } else {
                result[i] = Quaternion(x, y, z, w);
            }
        } else {
            result[i] = Quaternion();
        }
    }

    return result;
}

PackedFloat32Array GaussianSplatAsset::get_spherical_harmonics_buffer() const {
    MutexLock cache_lock(populate_mutex);
    PackedFloat32Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_spherical_harmonics_buffer() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    const uint32_t first_terms = sh_first_order_terms;
    const uint32_t high_terms = sh_high_order_terms;
    const uint32_t total_terms = 1 + first_terms + high_terms;

#ifdef TESTS_ENABLED
    // Arm-once allocation-failure injection; see GaussianSplatAsset::TestGetterFailure.
    // Placed exactly where the real gs_resize_or_fail() failure returns below, so the
    // injected result is byte-identical to the production one.
    if (test_getter_failure == TEST_GETTER_FAILURE_SPHERICAL_HARMONICS) {
        test_getter_failure = TEST_GETTER_FAILURE_NONE;
        return result;
    }
#endif
    // #798: splat_count * total_terms * 3 is the largest output of this family (SH bands
    // multiply the splat count), so it is the most likely to fail and the write loop is
    // bounded by splat_count/total_terms rather than result.size().
    if (!gs_resize_or_fail(result, int64_t(splat_count) * int64_t(total_terms) * 3,
                "GaussianSplatAsset::get_spherical_harmonics_buffer")) {
        return result; // empty == the unloaded-asset result; see the block comment above.
    }
    float *write = result.ptrw();
    const float *dc_read = has_sh_dc_coefficients ? sh_dc_coefficients.ptr() : nullptr;
    const float *first_read = sh_first_order_coefficients.ptr();
    const float *high_read = sh_high_order_coefficients.ptr();
    const Color *color_read = colors.ptr();

    const int dc_available = has_sh_dc_coefficients ? sh_dc_coefficients.size() : 0;
    const int first_available = sh_first_order_coefficients.size();
    const int high_available = sh_high_order_coefficients.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        int offset = int(i * total_terms * 3);

        if (dc_available >= int(i * 3 + 3) && dc_read != nullptr) {
            write[offset + 0] = dc_read[i * 3 + 0];
            write[offset + 1] = dc_read[i * 3 + 1];
            write[offset + 2] = dc_read[i * 3 + 2];
        } else if (i < (uint32_t)colors.size() && color_read != nullptr) {
            const Color color = color_read[i];
            write[offset + 0] = color.r;
            write[offset + 1] = color.g;
            write[offset + 2] = color.b;
        } else {
            write[offset + 0] = 1.0f;
            write[offset + 1] = 1.0f;
            write[offset + 2] = 1.0f;
        }

        offset += 3;

        if (first_terms > 0) {
            const int stride = int(first_terms * 3);
            const int base = int(i) * stride;
            for (uint32_t term = 0; term < first_terms; term++) {
                if (first_available >= base + int(term * 3 + 3) && first_read != nullptr) {
                    write[offset + 0] = first_read[base + term * 3 + 0];
                    write[offset + 1] = first_read[base + term * 3 + 1];
                    write[offset + 2] = first_read[base + term * 3 + 2];
                } else {
                    write[offset + 0] = 0.0f;
                    write[offset + 1] = 0.0f;
                    write[offset + 2] = 0.0f;
                }
                offset += 3;
            }
        }

        if (high_terms > 0) {
            const int stride = int(high_terms * 3);
            const int base = int(i) * stride;
            for (uint32_t term = 0; term < high_terms; term++) {
                if (high_available >= base + int(term * 3 + 3) && high_read != nullptr) {
                    write[offset + 0] = high_read[base + term * 3 + 0];
                    write[offset + 1] = high_read[base + term * 3 + 1];
                    write[offset + 2] = high_read[base + term * 3 + 2];
                } else {
                    write[offset + 0] = 0.0f;
                    write[offset + 1] = 0.0f;
                    write[offset + 2] = 0.0f;
                }
                offset += 3;
            }
        }
    }

    return result;
}

PackedFloat32Array GaussianSplatAsset::get_opacities() const {
    MutexLock cache_lock(populate_mutex);
    PackedFloat32Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_opacities() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_opacities")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    float *write = result.ptrw();
    const float *logit_read = opacity_logits.ptr();
    const Color *color_read = colors.ptr();

    const int logit_available = opacity_logits.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        float opacity_value = 1.0f;
        if (logit_available > int(i) && logit_read != nullptr) {
            const float logit = logit_read[i];
            const float exp_value = Math::exp(-logit);
            opacity_value = 1.0f / (1.0f + exp_value);
        } else if (i < (uint32_t)colors.size() && color_read != nullptr) {
            opacity_value = color_read[i].a;
        }

        write[i] = CLAMP(opacity_value, 0.0f, 1.0f);
    }

    return result;
}

PackedInt32Array GaussianSplatAsset::get_palette_ids_buffer() const {
    MutexLock cache_lock(populate_mutex);
    PackedInt32Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_palette_ids_buffer() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_palette_ids_buffer")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    int32_t *write = result.ptrw();
    const int available = palette_ids.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        int32_t value = (available > int(i)) ? palette_ids[i] : 0;
        write[i] = CLAMP(value, 0, 65535);
    }

    return result;
}

PackedInt32Array GaussianSplatAsset::get_painterly_flags_buffer() const {
    MutexLock cache_lock(populate_mutex);
    PackedInt32Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_painterly_flags_buffer() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_painterly_flags_buffer")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    int32_t *write = result.ptrw();
    const int available = painterly_flags.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        int32_t value = (available > int(i)) ? painterly_flags[i] : 0;
        write[i] = CLAMP(value, 0, 65535);
    }

    return result;
}

PackedInt32Array GaussianSplatAsset::get_brush_override_ids_buffer() const {
    return get_painterly_flags_buffer();
}

PackedVector3Array GaussianSplatAsset::get_normal_vectors() const {
    MutexLock cache_lock(populate_mutex);
    PackedVector3Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_normal_vectors() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

#ifdef TESTS_ENABLED
    // Arm-once allocation-failure injection; see GaussianSplatAsset::TestGetterFailure.
    // Placed exactly where the real gs_resize_or_fail() failure returns, so the injected
    // result is byte-identical to the production one.
    if (test_getter_failure == TEST_GETTER_FAILURE_NORMALS) {
        test_getter_failure = TEST_GETTER_FAILURE_NONE;
        return result;
    }
#endif
    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_normal_vectors")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    Vector3 *write = result.ptrw();
    const float *read = normals.ptr();
    const int available = normals.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        if (available >= int(i * 3 + 3) && read != nullptr) {
            write[i] = Vector3(read[i * 3 + 0], read[i * 3 + 1], read[i * 3 + 2]);
        } else {
            write[i] = Vector3(0.0f, 1.0f, 0.0f);
        }
    }

    return result;
}

PackedVector2Array GaussianSplatAsset::get_brush_axes_vector2() const {
    MutexLock cache_lock(populate_mutex);
    PackedVector2Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_brush_axes_vector2() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_brush_axes_vector2")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    Vector2 *write = result.ptrw();
    const float *read = brush_axes.ptr();
    const int available = brush_axes.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        if (available >= int(i * 2 + 2) && read != nullptr) {
            write[i] = Vector2(read[i * 2 + 0], read[i * 2 + 1]);
        } else {
            write[i] = Vector2(1.0f, 1.0f);
        }
    }

    return result;
}

PackedFloat32Array GaussianSplatAsset::get_stroke_ages_buffer() const {
    MutexLock cache_lock(populate_mutex);
    PackedFloat32Array result;
    if (splat_count == 0) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] get_stroke_ages_buffer() called on unloaded asset (splat_count == 0); returning empty array.");
        return result;
    }

    if (!gs_resize_or_fail(result, splat_count, "GaussianSplatAsset::get_stroke_ages_buffer")) {
        return result; // #798: empty == the unloaded-asset result; see the block comment above.
    }
    float *write = result.ptrw();
    const int available = stroke_ages.size();

    for (uint32_t i = 0; i < splat_count; i++) {
        write[i] = (available > int(i)) ? stroke_ages[i] : 0.0f;
    }

    return result;
}

uint32_t GaussianSplatAsset::get_sh_first_order_terms() const {
    MutexLock cache_lock(populate_mutex);
    return sh_first_order_terms;
}

uint32_t GaussianSplatAsset::get_sh_high_order_terms() const {
    MutexLock cache_lock(populate_mutex);
    return sh_high_order_terms;
}

void GaussianSplatAsset::set_positions(const PackedFloat32Array &p_positions) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_positions")) {
        return;
    }
    positions = p_positions;
    // Always update splat count based on position array size (3 floats per splat)
    uint32_t new_count = p_positions.size() / 3;
    if (splat_count != new_count) {
        splat_count = new_count;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("splat_count")] = (int)splat_count;
    _invalidate_bounds_metadata();
    _invalidate_gaussian_data_cache();
    // Any splat-layout mutation stales the bake; data/streaming_* properties
    // are registered AFTER data/* arrays in _bind_methods, so deserialization
    // re-installs the bake after the array setters clear it here.
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_colors(const PackedColorArray &p_colors) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_colors")) {
        return;
    }
    colors = p_colors;
    // Update splat count if not already set by positions
    if (splat_count == 0 && !p_colors.is_empty()) {
        splat_count = p_colors.size();
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("splat_count")] = (int)splat_count;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_scales(const PackedFloat32Array &p_scales) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_scales")) {
        return;
    }
    scales = p_scales;
    // Update splat count if not already set
    if (splat_count == 0 && !p_scales.is_empty()) {
        splat_count = p_scales.size() / 3;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("splat_count")] = (int)splat_count;
    _invalidate_bounds_metadata();
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_rotations(const PackedFloat32Array &p_rotations) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_rotations")) {
        return;
    }
    rotations = p_rotations;
    // Update splat count if not already set
    if (splat_count == 0 && !p_rotations.is_empty()) {
        splat_count = p_rotations.size() / 4;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("splat_count")] = (int)splat_count;
    _invalidate_bounds_metadata();
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_sh_dc_coefficients(const PackedFloat32Array &p_coefficients) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_sh_dc_coefficients")) {
        return;
    }
    sh_dc_coefficients = p_coefficients;
    has_sh_dc_coefficients = !p_coefficients.is_empty();
    if (splat_count == 0 && p_coefficients.size() >= 3) {
        splat_count = p_coefficients.size() / 3;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("splat_count")] = (int)splat_count;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_sh_first_order_coefficients(const PackedFloat32Array &p_coefficients) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_sh_first_order_coefficients")) {
        return;
    }
    sh_first_order_coefficients = p_coefficients;
    if (splat_count > 0 && !p_coefficients.is_empty()) {
        sh_first_order_terms = MIN<uint32_t>(p_coefficients.size() / (splat_count * 3), 3u);
    } else if (p_coefficients.is_empty()) {
        sh_first_order_terms = 0;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("sh_first_order_terms")] = (int)sh_first_order_terms;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_sh_high_order_coefficients(const PackedFloat32Array &p_coefficients) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_sh_high_order_coefficients")) {
        return;
    }
    sh_high_order_coefficients = p_coefficients;
    if (splat_count > 0 && !p_coefficients.is_empty()) {
        sh_high_order_terms = p_coefficients.size() / (splat_count * 3);
    } else if (p_coefficients.is_empty()) {
        sh_high_order_terms = 0;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("sh_high_order_terms")] = (int)sh_high_order_terms;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_opacity_logits(const PackedFloat32Array &p_opacity_logits) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_opacity_logits")) {
        return;
    }
    opacity_logits = p_opacity_logits;
    if (splat_count == 0 && !p_opacity_logits.is_empty()) {
        splat_count = p_opacity_logits.size();
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("opacity_encoding")] = StringName("logit");
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_palette_ids(const PackedInt32Array &p_palette_ids) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_palette_ids")) {
        return;
    }
    palette_ids = p_palette_ids;
    if (splat_count == 0 && !p_palette_ids.is_empty()) {
        splat_count = p_palette_ids.size();
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("has_palette_ids")] = palette_ids.size() == splat_count;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_painterly_flags(const PackedInt32Array &p_flags) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_painterly_flags")) {
        return;
    }
    painterly_flags = p_flags;
    if (splat_count == 0 && !p_flags.is_empty()) {
        splat_count = p_flags.size();
    }
    _ensure_buffer_sizes();
    const bool has_painterly_lane = painterly_flags.size() == splat_count;
    import_metadata[StringName("has_painterly_flags")] = has_painterly_lane;
    import_metadata[StringName("has_brush_override_ids")] = has_painterly_lane;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_brush_override_ids(const PackedInt32Array &p_override_ids) {
    // set_painterly_flags() performs its own gate check and bake invalidation.
    set_painterly_flags(p_override_ids);
}

void GaussianSplatAsset::set_normals(const PackedFloat32Array &p_normals) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_normals")) {
        return;
    }
    normals = p_normals;
    if (splat_count == 0 && p_normals.size() >= 3) {
        splat_count = p_normals.size() / 3;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("has_normals")] = normals.size() >= splat_count * 3;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_brush_axes(const PackedFloat32Array &p_brush_axes) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_brush_axes")) {
        return;
    }
    brush_axes = p_brush_axes;
    if (splat_count == 0 && p_brush_axes.size() >= 2) {
        splat_count = p_brush_axes.size() / 2;
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("has_brush_axes")] = brush_axes.size() >= splat_count * 2;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_stroke_ages(const PackedFloat32Array &p_stroke_ages) {
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("set_stroke_ages")) {
        return;
    }
    stroke_ages = p_stroke_ages;
    if (splat_count == 0 && !p_stroke_ages.is_empty()) {
        splat_count = p_stroke_ages.size();
    }
    _ensure_buffer_sizes();
    import_metadata[StringName("has_stroke_age")] = stroke_ages.size() == splat_count;
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
}

void GaussianSplatAsset::set_streaming_chunk_records(const PackedByteArray &p_records) {
    MutexLock cache_lock(populate_mutex);
    streaming_chunk_records = p_records;
    _invalidate_gaussian_data_cache();
}

PackedByteArray GaussianSplatAsset::get_streaming_chunk_records() const {
    MutexLock cache_lock(populate_mutex);
    return streaming_chunk_records;
}

void GaussianSplatAsset::set_streaming_primary_source_indices(const PackedInt32Array &p_indices) {
    MutexLock cache_lock(populate_mutex);
    streaming_primary_source_indices = p_indices;
    _invalidate_gaussian_data_cache();
}

PackedInt32Array GaussianSplatAsset::get_streaming_primary_source_indices() const {
    MutexLock cache_lock(populate_mutex);
    return streaming_primary_source_indices;
}

void GaussianSplatAsset::set_streaming_quantization_records(const PackedByteArray &p_records) {
    MutexLock cache_lock(populate_mutex);
    streaming_quantization_records = p_records;
    _invalidate_gaussian_data_cache();
}

PackedByteArray GaussianSplatAsset::get_streaming_quantization_records() const {
    MutexLock cache_lock(populate_mutex);
    return streaming_quantization_records;
}

void GaussianSplatAsset::set_streaming_chunk_size_used(uint32_t p_size) {
    MutexLock cache_lock(populate_mutex);
    streaming_chunk_size_used = p_size;
    _invalidate_gaussian_data_cache();
}

uint32_t GaussianSplatAsset::get_streaming_chunk_size_used() const {
    MutexLock cache_lock(populate_mutex);
    return streaming_chunk_size_used;
}

void GaussianSplatAsset::set_sh_component_terms(uint32_t p_first_order_terms, uint32_t p_high_order_terms) {
    MutexLock cache_lock(populate_mutex);
    if (sh_first_order_terms == p_first_order_terms && sh_high_order_terms == p_high_order_terms) {
        return;
    }
    if (!_runtime_mutation_permitted("set_sh_component_terms")) {
        return;
    }
    sh_first_order_terms = MIN<uint32_t>(p_first_order_terms, 3u);
    sh_high_order_terms = p_high_order_terms;
    _ensure_buffer_sizes();
    import_metadata[StringName("sh_first_order_terms")] = (int)sh_first_order_terms;
    import_metadata[StringName("sh_high_order_terms")] = (int)sh_high_order_terms;
    _invalidate_gaussian_data_cache();
    emit_changed();
}

void GaussianSplatAsset::_recalculate_sh_component_counts() {
    if (splat_count > 0) {
        if (!sh_first_order_coefficients.is_empty()) {
            sh_first_order_terms = MIN<uint32_t>(sh_first_order_coefficients.size() / (splat_count * 3), 3u);
        } else {
            sh_first_order_terms = 0;
        }
        if (!sh_high_order_coefficients.is_empty()) {
            sh_high_order_terms = sh_high_order_coefficients.size() / (splat_count * 3);
        } else {
            sh_high_order_terms = 0;
        }
    } else {
        if (sh_first_order_coefficients.is_empty()) {
            sh_first_order_terms = 0;
        }
        if (sh_high_order_coefficients.is_empty()) {
            sh_high_order_terms = 0;
        }
    }
}

void GaussianSplatAsset::_ensure_buffer_sizes() {
    const uint32_t count = splat_count;
    const int old_scale_size = scales.size();
    const int old_rotation_size = rotations.size();
    // #798: unchecked by the own-size rule. Every write below is bounded by the vector's
    // OWN size() and gated on size() > old_size, so a failed resize (which leaves size()
    // at the old value) skips the fill entirely instead of walking off a null ptrw().
    positions.resize(count * 3);
    colors.resize(count);
    scales.resize(count * 3);
    rotations.resize(count * 4);
    if (scales.size() > old_scale_size) {
        float *scale_ptr = scales.ptrw();
        int start = old_scale_size;
        if (start < 0) {
            start = 0;
        }
        for (int i = start; i < scales.size(); i += 3) {
            scale_ptr[i + 0] = 1.0f;
            if (i + 1 < scales.size()) {
                scale_ptr[i + 1] = 1.0f;
            }
            if (i + 2 < scales.size()) {
                scale_ptr[i + 2] = 1.0f;
            }
        }
    }
    if (rotations.size() > old_rotation_size) {
        float *rot_ptr = rotations.ptrw();
        int start = old_rotation_size;
        if (start < 0) {
            start = 0;
        }
        for (int i = start; i < rotations.size(); i += 4) {
            rot_ptr[i + 0] = 1.0f; // w
            if (i + 1 < rotations.size()) {
                rot_ptr[i + 1] = 0.0f;
            }
            if (i + 2 < rotations.size()) {
                rot_ptr[i + 2] = 0.0f;
            }
            if (i + 3 < rotations.size()) {
                rot_ptr[i + 3] = 0.0f;
            }
        }
    }
    // Use resize_initialized() (not resize()) for POD vectors to force
    // zero-fill on extension. Vector<T>::resize() skips zero-init when
    // std::is_trivially_constructible_v<T> is true (floats, ints), which
    // leaves the new slots holding whatever the allocator returned —
    // on Windows dev builds that is the 0xC0C0C0C0 poison pattern
    // (as float: -6.023529, as int32: -1061109568). Those uninitialized
    // bytes were being serialized straight into the .tres cache, causing
    // the runtime to read a full-count buffer of -6.023529 opacity logits
    // for every splat — which the opacity-aware tile-binning pass then
    // rejected as "effectively transparent", producing 0 overlap records
    // and a black viewport on any freshly-imported .ply.
    if (has_sh_dc_coefficients) {
        sh_dc_coefficients.resize_initialized(count * 3);
    } else {
        sh_dc_coefficients.resize_initialized(0);
    }
    sh_first_order_coefficients.resize_initialized(count * sh_first_order_terms * 3);
    sh_high_order_coefficients.resize_initialized(count * sh_high_order_terms * 3);
    opacity_logits.resize_initialized(count);
    palette_ids.resize_initialized(count);
    painterly_flags.resize_initialized(count);
    normals.resize_initialized(count * 3);
    brush_axes.resize_initialized(count * 2);
    stroke_ages.resize_initialized(count);

    _recalculate_sh_component_counts();
}

void GaussianSplatAsset::set_import_metadata(const Dictionary &p_metadata) {
    // Public/bound setter: take populate_mutex across the Dictionary write so a
    // concurrent prefetch worker holding the lock in populate_gaussian_data()
    // cannot observe a torn metadata read (dc_encoding / gaussian_2d_mode) and
    // cache GaussianData built from inconsistent state. Recursive mutex permits
    // the nested _invalidate_gaussian_data_cache() acquire.
    MutexLock cache_lock(populate_mutex);
    import_metadata = p_metadata;
    import_metadata[StringName("splat_count")] = (int)splat_count;
    import_metadata[StringName("quality_preset")] = import_quality_preset;
    import_metadata[StringName("compression_flags")] = (int)compression_flags;
    _invalidate_gaussian_data_cache();
    emit_changed();
}

Dictionary GaussianSplatAsset::get_import_metadata() const {
    MutexLock cache_lock(populate_mutex);
    return import_metadata;
}

// Invariant: every bound setter below that mutates `import_metadata` acquires
// `populate_mutex` on entry. A concurrent prefetch_parallel() worker reads
// metadata under the same lock in populate_gaussian_data(), so writers must
// hold the lock or snapshot before dispatch.
void GaussianSplatAsset::set_import_quality_preset(const String &p_preset) {
    MutexLock cache_lock(populate_mutex);
    String lower = p_preset.to_lower();
    if (import_quality_preset == lower) {
        return;
    }
    import_quality_preset = lower;
    import_metadata[StringName("quality_preset")] = import_quality_preset;
    emit_changed();
}

String GaussianSplatAsset::get_import_quality_preset() const {
    MutexLock cache_lock(populate_mutex);
    return import_quality_preset;
}

void GaussianSplatAsset::set_compression_flags(uint32_t p_flags) {
    MutexLock cache_lock(populate_mutex);
    if (compression_flags == p_flags) {
        return;
    }
    compression_flags = p_flags;
    import_metadata[StringName("compression_flags")] = (int)compression_flags;
    emit_changed();
}

uint32_t GaussianSplatAsset::get_compression_flags() const {
    MutexLock cache_lock(populate_mutex);
    return compression_flags;
}

void GaussianSplatAsset::set_preview_image(const Ref<Image> &p_image) {
    MutexLock cache_lock(populate_mutex);
    if (preview_image == p_image) {
        return;
    }

    preview_image = p_image;
    preview_texture_cache.unref();
    import_metadata[StringName("has_thumbnail")] = preview_image.is_valid() && !preview_image->is_empty();
    emit_changed();
}

Ref<Image> GaussianSplatAsset::get_preview_image() const {
    MutexLock cache_lock(populate_mutex);
    return preview_image;
}

Ref<Texture2D> GaussianSplatAsset::get_preview_texture() const {
    ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), Ref<Texture2D>(),
            "GaussianSplatAsset::get_preview_texture() creates an ImageTexture and must run on the main thread. Use get_preview_image() or capture_payload_snapshot() from worker threads.");

    Ref<Image> image;
    {
        MutexLock cache_lock(populate_mutex);
        if (preview_texture_cache.is_valid()) {
            return preview_texture_cache;
        }
        image = preview_image;
    }

    if (image.is_null() || image->is_empty()) {
        return Ref<Texture2D>();
    }

    // Texture creation crosses RenderingServer state. Keep worker/import paths
    // CPU-only and expose the stored Image through get_preview_image() instead.
    Ref<ImageTexture> texture = ImageTexture::create_from_image(image);
    MutexLock cache_lock(populate_mutex);
    if (preview_texture_cache.is_null() && preview_image == image) {
        preview_texture_cache = texture;
    }
    return preview_texture_cache.is_valid() ? preview_texture_cache : texture;
}

void GaussianSplatAsset::set_thumbnail(const Ref<Texture2D> &p_thumbnail) {
    if (p_thumbnail.is_null()) {
        set_preview_image(Ref<Image>());
        return;
    }

    ERR_FAIL_COND_MSG(!Thread::is_main_thread(),
            "GaussianSplatAsset::set_thumbnail() can only convert textures on the main thread. Use set_preview_image() during threaded import.");
    Ref<Image> image = p_thumbnail->get_image();
    set_preview_image(image);
}

void GaussianSplatAsset::set_source_path(const String &p_path) {
    MutexLock cache_lock(populate_mutex);
    if (import_metadata.has(StringName("source_path")) && (String)import_metadata[StringName("source_path")] == p_path) {
        return;
    }
    import_metadata[StringName("source_path")] = p_path;
    emit_changed();
}

String GaussianSplatAsset::get_source_path() const {
    MutexLock cache_lock(populate_mutex);
    if (import_metadata.has(StringName("source_path"))) {
        return (String)import_metadata[StringName("source_path")];
    }
    return String();
}

Error GaussianSplatAsset::load_from_file(const String &p_path) {
    if (!FileAccess::exists(p_path)) {
        GS_LOG_ERROR_DEFAULT("Gaussian splat file not found: " + p_path);
        return ERR_FILE_NOT_FOUND;
    }

    // GSStartupTrace is armed by render-context callers (the
    // GaussianSplatNode3D load sites) so importers, tests, and other
    // headless flows that call load_from_file directly do not queue
    // pending traces that will never be drained.

	uint64_t total_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
	uint64_t source_start_usec = total_start_usec;

	Ref<::GaussianData> gaussian_data;
	PackedStringArray missing_required;
	PackedStringArray missing_optional;
	Dictionary source_stats;
	String file_label = p_path.get_extension().to_upper();
	String source_stage = "raw";
	bool cache_hit = false;

	const String extension = p_path.get_extension().to_lower();
	Error err = OK;
	if (extension == "ply") {
		Ref<PLYLoader> ply_loader;
		ply_loader.instantiate();

		err = ply_loader->load_file(p_path);
		if (err == OK) {
			ply_loader->get_property_deficiencies(missing_required, missing_optional);
			source_stats = ply_loader->get_load_statistics();
			cache_hit = source_stats.get(StringName("cache_hit"), false);
			source_stage = cache_hit ? "cache" : "raw";
			gaussian_data = ply_loader->get_gaussian_data();
			file_label = "PLY";
		}
	} else if (extension == "spz") {
		Ref<SPZLoader> spz_loader;
		spz_loader.instantiate();

		err = spz_loader->load_file(p_path);
		if (err == OK) {
			source_stats = spz_loader->get_load_statistics();
			gaussian_data = spz_loader->get_gaussian_data();
			file_label = "SPZ";
			source_stage = "raw";
		}
	} else {
		GS_LOG_ERROR_DEFAULT(vformat(
				"Unsupported Gaussian splat raw format '%s' for path: %s. Supported extensions: .ply, .spz.",
				extension, p_path));
		return ERR_FILE_UNRECOGNIZED;
	}

	if (err != OK) {
		GS_LOG_ERROR_DEFAULT("Failed to load splat file: " + p_path);
		return err;
	}

	if (file_label == "PLY") {
		if (!missing_optional.is_empty()) {
			for (int i = 0; i < missing_optional.size(); i++) {
				GS_LOG_STREAMING_DEBUG(vformat("PLY load: missing optional data %s", missing_optional[i]));
			}
		}

		if (!missing_required.is_empty()) {
			String missing_required_text;
			for (int i = 0; i < missing_required.size(); i++) {
				if (i > 0) {
					missing_required_text += ", ";
				}
				missing_required_text += missing_required[i];
			}
			GS_LOG_ERROR_DEFAULT(vformat("PLY file missing required properties: %s", missing_required_text));
			return ERR_FILE_CORRUPT;
		}
	}

	if (gaussian_data.is_null() || gaussian_data->get_count() <= 0) {
		GS_LOG_ERROR_DEFAULT("Loaded splat file produced no Gaussian data: " + p_path);
		return ERR_FILE_CORRUPT;
	}

	// Full-asset finiteness sweep for the RAW/runtime load path. The
	// ResourceImporterPLY/SPZ importers validate at import time, but node
	// legacy-path migration, reload, and drag-drop reach the renderer through
	// this method (GaussianSplatNode3D::_set / reload_asset / drag-drop, plus
	// ResourceFormatLoaderGaussianSplat::load, all -> load_from_file) and never
	// touch the importers (#518). A NaN/Inf anywhere past splat 0 otherwise
	// poisons covariance/projection math on the GPU behind a clean splat 0.
	// This is the single chokepoint for those paths; the importers do NOT route
	// through load_from_file, so this shares the validator without double-running.
	int nonfinite_splat_index = -1;
	if (!gaussian_data->all_render_fields_finite(&nonfinite_splat_index)) {
		GS_LOG_ERROR_DEFAULT(vformat(
				"Gaussian splat load rejected: Non-finite (NaN/Inf) position/scale/rotation/opacity at splat %d in %s",
				nonfinite_splat_index, p_path));
		return ERR_FILE_CORRUPT;
	}

	const double source_stage_ms = _elapsed_msec(source_start_usec);

	uint64_t materialize_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : source_start_usec;
	Error populate_err = populate_from_gaussian_data(gaussian_data);
	const double materialize_ms = _elapsed_msec(materialize_start_usec);
	if (populate_err != OK) {
		return populate_err;
	}

	const double total_load_ms = _elapsed_msec(total_start_usec);
	// Preserve the authoritative GaussianData loaded from disk and stamp the
	// runtime-load metadata under the same lock so a concurrent prefetch
	// worker cannot observe a partial metadata write paired with the cached
	// GaussianData. Lock pairs with get_gaussian_data() and
	// populate_gaussian_data().
	{
		MutexLock cache_lock(populate_mutex);
		gaussian_data_cache = gaussian_data;
		import_metadata[StringName("runtime_load_timing")] = _build_runtime_load_timing(
				source_stage, cache_hit, source_stage_ms, materialize_ms, total_load_ms, source_stats);
		import_metadata[StringName("runtime_load_source")] = source_stage;
		import_metadata[StringName("runtime_load_cache_hit")] = cache_hit;
		import_metadata[StringName("runtime_load_source_path")] = p_path;
	}

	GS_LOG_STREAMING_INFO(vformat(
			"[LoadTiming][GaussianSplatAsset] file=%s path=%s splats=%d source_stage=%s cache_hit=%s source_ms=%.2f materialize_ms=%.2f total_ms=%.2f",
			file_label,
			p_path,
			splat_count,
			source_stage,
			cache_hit ? "yes" : "no",
			source_stage_ms,
			materialize_ms,
			total_load_ms));
	return OK;
}

Error GaussianSplatAsset::save_to_file(const String &p_path) const {
    Ref<::GaussianData> data = get_gaussian_data();
    if (data.is_null()) {
        return ERR_INVALID_DATA;
    }
    return data->save_to_file(p_path);
}

// Contract: holding populate_mutex across the read of the source SoA arrays is
// required so concurrent set_*()/copy_from()/populate_from_gaussian_data() on
// the owner thread cannot resize or rewrite the arrays mid-build. Sealing
// before any read also prevents new setters from being accepted while a worker
// is still in populate_gaussian_data(). Per-asset mutex preserves cross-asset
// parallelism in prefetch_gaussian_data_parallel.
Ref<::GaussianData> GaussianSplatAsset::get_gaussian_data() const {
    MutexLock cache_lock(populate_mutex);
    ERR_FAIL_COND_V_MSG(splat_count == 0, Ref<::GaussianData>(),
            "[GaussianSplatAsset] get_gaussian_data() called on unloaded asset (splat_count == 0); returning null.");

    if (gaussian_data_cache.is_valid()) {
        // Cache already handed out — keep payload sealed so external code
        // cannot mutate it behind the consumer's back.
        payload_sealed = true;
        return gaussian_data_cache;
    }

    // Seal BEFORE the read so any setter currently waiting on the mutex will
    // observe seal=true once it acquires and reject the mutation. The build
    // below then runs against immutable source arrays.
    payload_sealed = true;

    Ref<::GaussianData> data;
    uint64_t rebuild_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    if (!populate_gaussian_data(data)) {
        return Ref<::GaussianData>();
    }
    const double rebuild_ms = _elapsed_msec(rebuild_start_usec);

    gaussian_data_cache = data;
    GS_LOG_STREAMING_INFO(vformat(
            "[LoadTiming][GaussianSplatAsset] rebuilt GaussianData from asset arrays: splats=%d rebuild_ms=%.2f",
            splat_count,
            rebuild_ms));
    return gaussian_data_cache;
}

bool GaussianSplatAsset::has_gaussian_data_cached() const {
    MutexLock cache_lock(populate_mutex);
    return gaussian_data_cache.is_valid();
}

// Contract: callers must not invoke any of GaussianSplatAsset's packed setters,
// populate_from_gaussian_data(), or copy_from() concurrently with this call —
// the per-asset populate_mutex serializes worker reads against same-thread
// mutation, but only the seal state machine prevents intentional cross-thread
// mutation. Each asset's source SoA arrays are read on the worker thread; the
// payload is sealed before the read, so any subsequent setter on the owner
// thread will be rejected with a loud diagnostic.
void GaussianSplatAsset::prefetch_gaussian_data_parallel(const LocalVector<Ref<GaussianSplatAsset>> &p_assets) {
    GS_STARTUP_SCOPE("asset_prefetch_parallel");
    // Collect only assets that still need materialization. Already-cached and
    // null/unloaded entries are filtered here so the worker callback can be a
    // plain pointer-indexed dispatch with no per-task null/valid checks.
    LocalVector<GaussianSplatAsset *> pending;
    pending.reserve(p_assets.size());
    for (uint32_t i = 0; i < p_assets.size(); ++i) {
        const Ref<GaussianSplatAsset> &ref = p_assets[i];
        if (ref.is_null()) {
            continue;
        }
        if (ref->get_splat_count() == 0) {
            continue;
        }
        if (ref->has_gaussian_data_cached()) {
            continue;
        }
        pending.push_back(ref.ptr());
    }

    if (pending.is_empty()) {
        return;
    }

    if (pending.size() == 1) {
        // Single asset — skip pool overhead and run inline on the calling thread.
        pending[0]->get_gaussian_data();
        return;
    }

    // Pure CPU SoA->AoS conversion across distinct assets — no shared mutation,
    // no RenderingDevice work. Each worker calls get_gaussian_data() on its
    // assigned asset; the per-asset populate_mutex covers the cache store.
    struct WorkCtx {
        GaussianSplatAsset *const *items;
    };
    WorkCtx ctx{ pending.ptr() };
    auto worker = [](void *p_userdata, uint32_t p_index) {
        WorkCtx *c = static_cast<WorkCtx *>(p_userdata);
        c->items[p_index]->get_gaussian_data();
    };
    WorkerThreadPool::GroupID gid = WorkerThreadPool::get_singleton()->add_native_group_task(
            worker, &ctx, int(pending.size()), -1, true, String("GSAssetMaterialize"));
    WorkerThreadPool::get_singleton()->wait_for_group_task_completion(gid);
}

void GaussianSplatAsset::prefetch_parallel(const TypedArray<GaussianSplatAsset> &p_assets) {
    LocalVector<Ref<GaussianSplatAsset>> refs;
    refs.reserve(p_assets.size());
    for (int i = 0; i < p_assets.size(); i++) {
        Ref<GaussianSplatAsset> asset = p_assets[i];
        if (asset.is_valid()) {
            refs.push_back(asset);
        }
    }
    prefetch_gaussian_data_parallel(refs);
}

bool GaussianSplatAsset::populate_gaussian_data(Ref<::GaussianData> &r_data) const {
    GS_STARTUP_SCOPE("asset_populate_gaussian_data");
    // Lock so external callers (scene_director DYNAMIC path, tests) also see
    // a consistent snapshot of the SoA source arrays. Recursive: get_gaussian_data()
    // already holds populate_mutex when it calls into here.
    MutexLock cache_lock(populate_mutex);
    if (splat_count == 0) {
        return false;
    }

    // #798 review round 7: build into a payload the caller cannot reach, and PUBLISH it
    // only once every lane has landed.
    //
    // Round 6 made the failure loud; it did not make it transactional. The lanes below are
    // materialized in sequence, so set_positions()/set_scales()/set_rotations() have already
    // rewritten the destination by the time the SH getter is validated. When the destination
    // was the caller's own object -- this is an in/out Ref, and the previous code reused a
    // non-null one in place -- clearing r_data on failure dropped only THIS function's
    // reference: any other live Ref to the same payload kept observing it resized to
    // splat_count and rewritten with three asset lanes over GaussianData::resize()'s defaults
    // for the other seven. That is a half-built payload handed out through the back door of a
    // call that returned false.
    //
    // Staging removes the failure paths' write set entirely: on every `return false` below,
    // `staged` is the only reference and dies with the frame, and r_data is left EXACTLY as
    // the caller passed it. Note this replaces round 6's "clear r_data on failure" -- with
    // nothing half-written to hide, clearing would itself be the one mutation the failure path
    // still performed. Callers gate on the bool return (get_gaussian_data(),
    // prune_by_importance(), InstanceStore::_populate_gaussian_data_from_asset()), never on
    // the out-param's nullity, so the observable behaviour of every in-tree call site -- all
    // of which pass a fresh, null Ref -- is unchanged: null in, null out on failure.
    //
    // Consequence on SUCCESS: a caller-supplied non-null payload is no longer rewritten in
    // place, it is REPLACED. No in-tree caller passes one -- the three named above are the only
    // ones, and each declares a fresh local Ref -- and the out-param is documented as an output,
    // so nothing depends on that identity.
    Ref<::GaussianData> staged;
    staged.instantiate();

    // #798 review round 6: MATERIALIZATION must fail closed too, exactly like the merge
    // and rewrite paths already do.
    //
    // Every structured getter called below reports its own allocation failure by returning
    // the EMPTY array (see the block comment above get_position_vectors()). Every
    // GaussianData setter it feeds is `void` and rejects a size mismatch with a bare
    // ERR_FAIL_COND -- gaussian_data.cpp:789/799/809/819/834/862, and set_spherical_harmonics()
    // bails at `floats_per_gaussian < 3` for the empty input. So the setter simply returns,
    // the next one runs, and this function used to return true at the bottom regardless.
    // The result was a payload still carrying GaussianData::resize()'s defaults for the
    // failed lane -- sh_dc = Color(1,1,1,1), normal = (0,1,0), scale = (1,1,1) -- reported
    // as a SUCCESS. get_gaussian_data() then CACHES it (so every later call returns the
    // defaulted payload without retrying) and the scene director's DYNAMIC path installs it
    // as the record's data. The SH buffer is the one to worry about first: it is the largest
    // output here (splat_count * total_terms * 3 floats), hence the first to fail.
    //
    // The check is derived, not a hand-maintained list: each getter sizes its output to
    // splat_count, or -- for the SH buffer alone -- to splat_count * total_terms * 3, and it
    // does so unconditionally, padding with per-field defaults when the SOURCE lane is short.
    // So on any successful call the size is exactly that, and anything else is an allocation
    // failure. EXACT equality, for the same reason the lane guard below uses it: a failed
    // SHRINK leaves a CowData at its previous, LARGER length (cowdata.h), which a `>=` test
    // waves through.
    //
    // On failure nothing is published, so no caller can mistake an out-param for a usable
    // payload; get_gaussian_data() consequently caches nothing and the next call retries the
    // build.
    staged->resize(splat_count);
    // resize() is void and ERR_FAIL_CONDs on a negative count -- splat_count is file-derived
    // and unbounded, so a value past INT_MAX truncates negative and leaves the storage at its
    // previous length, after which every setter below would reject its (correctly sized) input
    // and this function would again report success over an untouched payload.
    if (int64_t(staged->get_count()) != int64_t(splat_count)) {
        const int64_t got = int64_t(staged->get_count());
        ERR_FAIL_V_MSG(false, vformat("[GaussianSplatAsset] populate_gaussian_data: GaussianData::resize(%d) "
                                      "left %d gaussians, so the payload storage could not be sized. "
                                      "Refusing to materialize.",
                                      int64_t(splat_count), got));
    }

    const int64_t sh_expected = int64_t(splat_count) *
            int64_t(1u + sh_first_order_terms + sh_high_order_terms) * 3;

#define GS_MATERIALIZE_LANE(m_getter, m_expected, m_setter)                                              \
    {                                                                                                    \
        const int64_t expected_size = int64_t(m_expected);                                               \
        auto lane = m_getter();                                                                          \
        if (int64_t(lane.size()) != expected_size) {                                                     \
            const int64_t got_size = int64_t(lane.size());                                               \
            ERR_FAIL_V_MSG(false, vformat("[GaussianSplatAsset] populate_gaussian_data: " #m_getter      \
                                          "() returned %d elements but this payload needs exactly %d, "  \
                                          "so its output allocation failed. Refusing to materialize "    \
                                          "instead of reporting success over a partially "               \
                                          "default-initialized payload.",                                \
                                          got_size, expected_size));                                     \
        }                                                                                                \
        staged->m_setter(lane);                                                                          \
    }

    GS_MATERIALIZE_LANE(get_position_vectors, splat_count, set_positions)
    GS_MATERIALIZE_LANE(get_scale_vectors, splat_count, set_scales)
    GS_MATERIALIZE_LANE(get_rotation_quaternions, splat_count, set_rotations)
    GS_MATERIALIZE_LANE(get_spherical_harmonics_buffer, sh_expected, set_spherical_harmonics)
    GS_MATERIALIZE_LANE(get_opacities, splat_count, set_opacities)
    GS_MATERIALIZE_LANE(get_palette_ids_buffer, splat_count, set_palette_ids)
    GS_MATERIALIZE_LANE(get_brush_override_ids_buffer, splat_count, set_brush_override_ids)
    GS_MATERIALIZE_LANE(get_normal_vectors, splat_count, set_normals)
    GS_MATERIALIZE_LANE(get_brush_axes_vector2, splat_count, set_brush_axes)
    GS_MATERIALIZE_LANE(get_stroke_ages_buffer, splat_count, set_stroke_ages)

#undef GS_MATERIALIZE_LANE

    Dictionary asset_metadata = get_import_metadata();
    if (asset_metadata.has(StringName("gaussian_2d_mode"))) {
        staged->set_2d_mode((bool)asset_metadata[StringName("gaussian_2d_mode")]);
    }
    const GaussianDCEncoding staged_dc_encoding = _resolve_dc_encoding_from_metadata(asset_metadata);
    for (int i = 0; i < staged->get_count(); i++) {
        Gaussian g = staged->get_gaussian(i);
        g.render_meta = gaussian_set_dc_encoding(g.render_meta, staged_dc_encoding);
        staged->set_gaussian(i, g);
    }

    staged->set_streaming_chunk_bake(streaming_chunk_records,
            streaming_primary_source_indices,
            streaming_quantization_records,
            streaming_chunk_size_used);

    // Publish. This is the ONLY write to r_data in the whole function, and it is
    // unreachable from every failure path above.
    r_data = staged;
    return true;
}

namespace {
// #798: a lane pointer that fails closed on a SHORT lane.
//
// _ensure_buffer_sizes() sizes every SoA lane with a bare Vector::resize(), which reports
// OOM only through its return value and -- crucially -- leaves the vector at its PREVIOUS
// size rather than empty. populate_from_gaussian_data() is the asset REWRITE path, so that
// previous size is routinely non-zero: a failed grow leaves a live, non-empty, TOO SHORT
// heap block. `is_empty() ? nullptr : ptrw()` does not catch that, and the write loop
// indexes by the NEW splat count, so it would run past the end of a real allocation -- a
// silent heap overflow, strictly worse than the null-ptrw case this class of bug usually
// produces. Separate the two cases: an EMPTY lane is a legitimately absent optional lane
// (nullptr, skipped by the has_* flags), while a short non-empty lane is an allocation
// failure and clears r_ok so the caller aborts the whole population. Degrading a short
// lane to "absent" is deliberately NOT an option -- that would silently drop a field
// (the opacity_logits class of data-loss bug) instead of reporting it.
//
// #798 review: an EMPTY lane is only legitimately absent when NOTHING was supposed to be
// allocated for it. _ensure_buffer_sizes() sizes each lane to exactly the length the write
// loop indexes it to, so callers pass that same length here -- which makes the test derivable
// rather than a hand-maintained required/optional list:
//
//     p_required == 0  -> the lane is genuinely absent (nullptr, skipped by its has_* flag)
//     p_required  > 0  -> the lane MUST hold p_required elements; empty means the sizing
//                         failed outright (a fresh/CoW-forked lane whose _alloc failed leaves
//                         _ptr null, hence empty rather than short), so fail closed.
//
// #798 review round 5: the test is EXACT EQUALITY, not ">= required". A resize that SHRINKS
// can fail too, and CowData::_fork_allocate() bails out of its in-place branch before writing
// the new size when _realloc() fails ("Out of memory; the current array is still valid though",
// core/templates/cowdata.h) -- so a failed shrink leaves the lane at its previous, LARGER
// length. `size() < required` waves that through and populate_from_gaussian_data() seals an
// asset whose lane lengths contradict its own splat_count: the has_normals / has_palette_ids /
// has_brush_axes metadata flags below are written as `lane.size() == splat_count * stride` and
// silently flip to false, dropping fields that are physically still there, and the oversized
// lane is what gets serialized. There is no legitimate over-length lane here --
// _ensure_buffer_sizes() sizes every lane to exactly the length the write loop indexes it to --
// so "longer than required" is an allocation failure just as much as "shorter" is.
//
// Without that second case a failed allocation of a required lane returned nullptr with r_ok
// still true, and populate_from_gaussian_data() went on to seal the asset, bump its version
// and return OK with a nonzero splat_count and no payload for that lane.
template <typename T>
T *_gs_lane_ptrw_or_fail(Vector<T> &p_lane, int64_t p_required, const char *p_name, bool &r_ok) {
    if (p_lane.is_empty()) {
        if (p_required > 0) {
            r_ok = false;
            ERR_PRINT(vformat("[GaussianSplatAsset] populate_from_gaussian_data: lane '%s' is empty "
                              "but this payload needs %d elements, so its buffer allocation failed. "
                              "Aborting the population instead of sealing an asset with a missing lane.",
                    String(p_name), p_required));
        }
        return nullptr;
    }
    if (int64_t(p_lane.size()) != p_required) {
        r_ok = false;
        ERR_PRINT(vformat("[GaussianSplatAsset] populate_from_gaussian_data: lane '%s' holds %d "
                          "elements but this payload needs exactly %d, so its buffer resize failed. "
                          "Aborting the population instead of sealing an asset whose lane lengths "
                          "disagree with its splat count.",
                String(p_name), int64_t(p_lane.size()), p_required));
        return nullptr;
    }
    return p_lane.ptrw();
}
} // namespace

Error GaussianSplatAsset::populate_from_gaussian_data(const Ref<::GaussianData> &p_gaussian_data) {
    if (p_gaussian_data.is_null()) {
        GS_LOG_ERROR_DEFAULT("populate_from_gaussian_data called with invalid GaussianData reference");
        return ERR_INVALID_PARAMETER;
    }

    // This is the one legitimate runtime-to-asset persistence writer. It
    // bypasses the public Packed setter gate because it mutates the internal
    // fields directly under the class's own control. Snapshot the prior seal
    // so any early-failure return below restores it — otherwise a failed
    // populate on a previously sealed (runtime-authoritative) asset would
    // silently re-enable packed setters and let arrays diverge from already
    // handed-out GaussianData. The success path re-asserts seal=true at the
    // end of the function.
    //
    // Hold populate_mutex across the entire unseal/rewrite/reseal cycle so a
    // concurrent prefetch worker cannot read torn source arrays. Recursive
    // mutex permits the nested _invalidate_gaussian_data_cache() acquire.
    MutexLock cache_lock(populate_mutex);
    const bool previous_seal = payload_sealed;
    payload_sealed = false;
    _invalidate_gaussian_data_cache();
    // The persisted bake describes the prior source-array layout; rewriting
    // the asset arrays invalidates it. Without clearing, a same-count rewrite
    // would let has_baked_streaming_chunks() short-circuit chunk rebuild
    // against stale bounds/centers.
    _invalidate_streaming_bake();

    int count = p_gaussian_data->get_count();
    if (count <= 0) {
        GS_LOG_ERROR_DEFAULT("GaussianData contains no splats");
        payload_sealed = previous_seal;
        return ERR_FILE_CORRUPT;
    }

    splat_count = count;
    // #798 review round 5: keep the REQUESTED term counts in locals. `sh_*_terms` are about
    // to become untrustworthy: _ensure_buffer_sizes() ends in _recalculate_sh_component_counts(),
    // which DERIVES both counts back out of the lane SIZES. That derivation is only equal to
    // what was asked for when the resize actually landed -- on a failed resize it reports the
    // term count of the SURVIVING lane, so every length derived from it afterwards describes
    // the corrupted lane instead of the payload. The source's own counts are the ground truth
    // for both the lane requirements and the stride into p_gaussian_data's arrays.
    const uint32_t requested_first_order_terms = MIN<uint32_t>(p_gaussian_data->get_sh_first_order_count(), 3u);
    const uint32_t requested_high_order_terms = p_gaussian_data->get_sh_high_order_count();
    sh_first_order_terms = requested_first_order_terms;
    sh_high_order_terms = requested_high_order_terms;
#ifdef TESTS_ENABLED
    // Snapshot for TEST_LANE_FAILURE_OVERSIZED below: a failed shrink leaves the lane at
    // exactly its PREVIOUS length, so the injection has to know that length.
    const int test_prev_high_order_size = sh_high_order_coefficients.size();
#endif
    _ensure_buffer_sizes();

#ifdef TESTS_ENABLED
    // Arm-once allocation-failure injection; see GaussianSplatAsset::TestLaneFailure.
    // _ensure_buffer_sizes() ignores every resize() return, so a failure there is visible
    // ONLY as the lane's resulting size (plus whatever _recalculate_sh_component_counts()
    // then derives from it). Reproducing that state is therefore a complete simulation of
    // the failure, not an approximation of it.
    if (test_lane_failure != TEST_LANE_FAILURE_NONE) {
        const TestLaneFailure mode = test_lane_failure;
        test_lane_failure = TEST_LANE_FAILURE_NONE;
        if (mode == TEST_LANE_FAILURE_EMPTY) {
            positions.clear();
        } else if (mode == TEST_LANE_FAILURE_SHORT) {
            // Non-empty but short: keep one splat's worth, which is < count * 3 for any
            // count > 1. Shrinking never allocates, so this cannot itself fail.
            positions.resize(3);
        } else {
            // TEST_LANE_FAILURE_OVERSIZED: a failed SHRINK. CowData::_fork_allocate()
            // returns early when _realloc() fails and never writes the new size, so the
            // lane keeps its previous, LARGER length with its previous contents
            // (core/templates/cowdata.h). Deliberately applied to
            // sh_high_order_coefficients, the one lane whose length feeds a derived term
            // count -- restore its pre-resize length and re-run the derivation exactly as
            // _ensure_buffer_sizes() would have on the real failure.
            if (test_prev_high_order_size > sh_high_order_coefficients.size()) {
                sh_high_order_coefficients.resize_initialized(test_prev_high_order_size);
            }
            _recalculate_sh_component_counts();
        }
    }
#endif

    // #798 review round 5: undo any lane-derived term count before it can be used. Placed
    // AFTER the injection block on purpose, so an injected shape gets exactly the same
    // treatment a real failure would. On the success path this is a no-op --
    // _recalculate_sh_component_counts() reproduces the requested counts exactly from a lane
    // of length count * terms * 3 -- but on a failed shrink it is what stops the inflated
    // count from (a) being accepted as the lane's own requirement below and (b) becoming the
    // stride used to walk p_gaussian_data's SHORTER high-order array in the write loop.
    sh_first_order_terms = requested_first_order_terms;
    sh_high_order_terms = requested_high_order_terms;

    const Vector3 *high_order_ptr = p_gaussian_data->get_sh_high_order_coefficients_ptr();

    // #798: each lane states the length the write loop below will index it to, right where
    // its pointer is produced, so the requirement cannot drift away from the indexing.
    // See _gs_lane_ptrw_or_fail() for why is_empty() alone is not a sufficient guard here.
    const int64_t need_1 = int64_t(count);
    const int64_t need_2 = int64_t(count) * 2;
    const int64_t need_3 = int64_t(count) * 3;
    const int64_t need_4 = int64_t(count) * 4;
    bool lanes_ok = true;
    float *positions_ptr = _gs_lane_ptrw_or_fail(positions, need_3, "positions", lanes_ok);
    Color *colors_ptr = _gs_lane_ptrw_or_fail(colors, need_1, "colors", lanes_ok);
    float *scales_ptr = _gs_lane_ptrw_or_fail(scales, need_3, "scales", lanes_ok);
    float *rotations_ptr = _gs_lane_ptrw_or_fail(rotations, need_4, "rotations", lanes_ok);
    // _ensure_buffer_sizes() sizes this lane to 0 when the asset has no DC coefficients, so
    // pass the length IT produces -- not need_3 -- or a legitimate absence reads as a failure.
    const int64_t need_sh_dc = has_sh_dc_coefficients ? need_3 : int64_t(0);
    float *sh_dc_ptr = _gs_lane_ptrw_or_fail(sh_dc_coefficients, need_sh_dc, "sh_dc_coefficients", lanes_ok);
    float *sh_first_order_ptr = _gs_lane_ptrw_or_fail(sh_first_order_coefficients,
            int64_t(count) * int64_t(sh_first_order_terms) * 3, "sh_first_order_coefficients", lanes_ok);
    float *sh_high_order_ptr = _gs_lane_ptrw_or_fail(sh_high_order_coefficients,
            int64_t(count) * int64_t(sh_high_order_terms) * 3, "sh_high_order_coefficients", lanes_ok);
    float *opacity_logits_ptr = _gs_lane_ptrw_or_fail(opacity_logits, need_1, "opacity_logits", lanes_ok);
    int32_t *palette_ids_ptr = _gs_lane_ptrw_or_fail(palette_ids, need_1, "palette_ids", lanes_ok);
    int32_t *painterly_flags_ptr = _gs_lane_ptrw_or_fail(painterly_flags, need_1, "painterly_flags", lanes_ok);
    float *normals_ptr = _gs_lane_ptrw_or_fail(normals, need_3, "normals", lanes_ok);
    float *brush_axes_ptr = _gs_lane_ptrw_or_fail(brush_axes, need_2, "brush_axes", lanes_ok);
    float *stroke_ages_ptr = _gs_lane_ptrw_or_fail(stroke_ages, need_1, "stroke_ages", lanes_ok);
    if (!lanes_ok) {
        // #798 review round 2: restoring only `payload_sealed` was not enough. By this point the
        // function has already replaced splat_count and the SH term counts, invalidated the
        // gaussian-data cache and the streaming bake, and run _ensure_buffer_sizes() over every
        // lane -- so a bare return leaves a MIXED asset: a new count, new term layout, and lanes
        // at a mixture of old, newly-sized and empty lengths. That is observable, because callers
        // such as _register_instance_in_director() log the Error and then register runtime_asset
        // anyway.
        //
        // Reset to a coherent EMPTY state instead: splat_count 0 with cleared lanes is exactly
        // the shape every consumer already handles (each getter early-outs on splat_count == 0),
        // whereas "mixed" is a shape none of them handle.
        //
        // A true transactional rollback is deliberately NOT attempted. Preserving the previous
        // payload would mean snapshotting every lane BEFORE the rewrite, i.e. allocating a full
        // copy of the asset on the path whose whole premise is that allocation is failing. Losing
        // the payload is the honest outcome here; silently keeping half of it is not.
        splat_count = 0;
        sh_first_order_terms = 0;
        sh_high_order_terms = 0;
        positions.clear();
        colors.clear();
        scales.clear();
        rotations.clear();
        sh_dc_coefficients.clear();
        sh_first_order_coefficients.clear();
        sh_high_order_coefficients.clear();
        opacity_logits.clear();
        palette_ids.clear();
        painterly_flags.clear();
        normals.clear();
        brush_axes.clear();
        stroke_ages.clear();
        _recalculate_sh_component_counts();

        // #798 review round 4: the reset has to invalidate the DERIVED bounds too, not
        // just the lanes. The success path below writes import_metadata["bounds"] from
        // the freshly accumulated min/max and clears "bounds_dirty"; clearing positions
        // without touching either leaves the PRE-reset AABB behind, still flagged clean.
        // GaussianSplatNodeHelpers::update_asset() reads exactly that pair
        // (gaussian_splat_node_helpers.cpp): a false "bounds_dirty" plus a non-degenerate
        // cached AABB sets used_cached_bounds = true and SKIPS the recompute from
        // `positions` -- which is now empty -- so the node and the editor keep reporting
        // the old asset's extents for a zero-splat asset. Culling, the LOD distance
        // metric and the editor gizmo all read that AABB. Every other lane-clearing
        // mutator on this class already invalidates here (set_splat_count(),
        // set_positions(), set_scales(), prune) -- this branch was the one that did not.
        _invalidate_bounds_metadata();

        payload_sealed = previous_seal;

        // #798 review round 3: the reset above is a PAYLOAD CHANGE, so it has to be
        // announced exactly like the success path announces one. Without this, the reset
        // is invisible to every consumer that caches a materialized copy and gates the
        // rebuild on payload_version: InstanceStore::refresh_asset() returns true
        // unconditionally when the version has not moved
        // (gaussian_splat_scene_director.cpp), and retain_asset() likewise skips the
        // rebuild -- so the renderer keeps drawing the OLD geometry from its cached
        // GaussianData while every direct reader of the asset sees splat_count == 0.
        // "Empty here, old geometry there" is a worse divergence than the mixed state
        // this branch was added to prevent.
        //
        // Bumping instead makes the next retain/refresh attempt the rebuild, which then
        // fails loudly (_populate_gaussian_data_from_asset() returns false on
        // splat_count == 0) and propagates false to its caller -- the failure becoming
        // observable is the point.
        payload_version++;
        emit_changed();
        return ERR_OUT_OF_MEMORY;
    }

    const bool has_positions = positions_ptr != nullptr;
    const bool has_colors = colors_ptr != nullptr;
    const bool has_scales = scales_ptr != nullptr;
    const bool has_rotations = rotations_ptr != nullptr;
    const bool has_sh_dc = sh_dc_ptr != nullptr;
    const bool has_first_order = sh_first_order_terms > 0 && sh_first_order_ptr != nullptr;
    const bool has_high_order = sh_high_order_terms > 0 && high_order_ptr != nullptr && sh_high_order_ptr != nullptr;
    const bool has_opacity_logits = opacity_logits_ptr != nullptr;
    const bool has_normals = normals_ptr != nullptr;
    const bool has_brush_axes = brush_axes_ptr != nullptr;
    const bool has_stroke_ages = stroke_ages_ptr != nullptr;
    const bool has_palette_ids = palette_ids_ptr != nullptr;
    const bool has_painterly_flags = painterly_flags_ptr != nullptr;

    bool bounds_initialized = false;
    Vector3 min_pos;
    Vector3 max_pos;
    GaussianDCEncoding asset_dc_encoding = GAUSSIAN_DC_ENCODING_LINEAR_RGB;
    bool asset_dc_encoding_initialized = false;
    bool mixed_dc_encoding = false;

    for (int i = 0; i < count; i++) {
        Gaussian g = p_gaussian_data->get_gaussian(i);
        GaussianDCEncoding gaussian_dc_encoding = gaussian_get_dc_encoding(g.render_meta);
        if (!asset_dc_encoding_initialized) {
            asset_dc_encoding = gaussian_dc_encoding;
            asset_dc_encoding_initialized = true;
        } else if (asset_dc_encoding != gaussian_dc_encoding) {
            mixed_dc_encoding = true;
        }
        const uint32_t base3 = uint32_t(i) * 3u;
        const uint32_t base4 = uint32_t(i) * 4u;
        const int first_base = i * int(sh_first_order_terms) * 3;
        const int high_base = i * int(sh_high_order_terms) * 3;
        const size_t high_order_base = size_t(i) * size_t(sh_high_order_terms);
        const uint32_t brush_base = uint32_t(i) * 2u;

        // Rotation-aware AABB extent for anisotropic Gaussian scales:
        // extent = abs(R) * sigma, then expand to 3-sigma coverage.
        const Vector3 sigma(Math::abs(g.scale.x), Math::abs(g.scale.y), Math::abs(g.scale.z));
        const Basis rotation_basis(g.rotation);
        const Vector3 axis_x = rotation_basis.get_column(0) * sigma.x;
        const Vector3 axis_y = rotation_basis.get_column(1) * sigma.y;
        const Vector3 axis_z = rotation_basis.get_column(2) * sigma.z;
        Vector3 extent(
                Math::abs(axis_x.x) + Math::abs(axis_y.x) + Math::abs(axis_z.x),
                Math::abs(axis_x.y) + Math::abs(axis_y.y) + Math::abs(axis_z.y),
                Math::abs(axis_x.z) + Math::abs(axis_y.z) + Math::abs(axis_z.z));
        extent *= 3.0f;
        Vector3 local_min = g.position - extent;
        Vector3 local_max = g.position + extent;
        if (!bounds_initialized) {
            min_pos = local_min;
            max_pos = local_max;
            bounds_initialized = true;
        } else {
            min_pos = min_pos.min(local_min);
            max_pos = max_pos.max(local_max);
        }

        if (has_positions) {
            positions_ptr[base3 + 0] = g.position.x;
            positions_ptr[base3 + 1] = g.position.y;
            positions_ptr[base3 + 2] = g.position.z;
        }

        if (has_colors) {
            Color color = g.sh_dc;
            color.a = g.opacity;
            colors_ptr[i] = color;
        }

        if (has_scales) {
            scales_ptr[base3 + 0] = g.scale.x;
            scales_ptr[base3 + 1] = g.scale.y;
            scales_ptr[base3 + 2] = g.scale.z;
        }

        if (has_rotations) {
            rotations_ptr[base4 + 0] = g.rotation.w;
            rotations_ptr[base4 + 1] = g.rotation.x;
            rotations_ptr[base4 + 2] = g.rotation.y;
            rotations_ptr[base4 + 3] = g.rotation.z;
        }

        // SH coefficients
        if (has_sh_dc) {
            sh_dc_ptr[base3 + 0] = g.sh_dc.r;
            sh_dc_ptr[base3 + 1] = g.sh_dc.g;
            sh_dc_ptr[base3 + 2] = g.sh_dc.b;
        }

        if (has_first_order) {
            for (uint32_t term = 0; term < sh_first_order_terms; term++) {
                const Vector3 &coeff = g.sh_1[term];
                const int term_base = first_base + int(term) * 3;
                sh_first_order_ptr[term_base + 0] = coeff.x;
                sh_first_order_ptr[term_base + 1] = coeff.y;
                sh_first_order_ptr[term_base + 2] = coeff.z;
            }
        }

        if (has_high_order) {
            for (uint32_t term = 0; term < sh_high_order_terms; term++) {
                const Vector3 &coeff = high_order_ptr[high_order_base + term];
                const int term_base = high_base + int(term) * 3;
                sh_high_order_ptr[term_base + 0] = coeff.x;
                sh_high_order_ptr[term_base + 1] = coeff.y;
                sh_high_order_ptr[term_base + 2] = coeff.z;
            }
        }

        float clamped_opacity = CLAMP(g.opacity, 0.0001f, 0.9999f);
        if (has_opacity_logits) {
            opacity_logits_ptr[i] = Math::log(clamped_opacity / (1.0f - clamped_opacity));
        }

        if (has_normals) {
            normals_ptr[base3 + 0] = g.normal.x;
            normals_ptr[base3 + 1] = g.normal.y;
            normals_ptr[base3 + 2] = g.normal.z;
        }

        if (has_brush_axes) {
            brush_axes_ptr[brush_base + 0] = g.brush_axes.x;
            brush_axes_ptr[brush_base + 1] = g.brush_axes.y;
        }

        if (has_stroke_ages) {
            stroke_ages_ptr[i] = g.stroke_age;
        }

        if (has_palette_ids) {
            palette_ids_ptr[i] = (int)gaussian_get_palette_id(g.painterly_meta);
        }

        if (has_painterly_flags) {
            painterly_flags_ptr[i] = (int)gaussian_get_brush_override_id(g.painterly_meta);
        }
    }

    import_metadata[StringName("splat_count")] = count;
    import_metadata[StringName("sh_first_order_terms")] = (int)sh_first_order_terms;
    import_metadata[StringName("sh_high_order_terms")] = (int)sh_high_order_terms;
    import_metadata[StringName("sh_degree")] = (int)p_gaussian_data->get_sh_degree();
    if (asset_dc_encoding_initialized && !mixed_dc_encoding) {
        import_metadata[StringName("dc_encoding")] = asset_dc_encoding == GAUSSIAN_DC_ENCODING_LINEAR_RGB
                ? String("linear_rgb")
                : String("legacy_bias");
    } else {
        import_metadata.erase(StringName("dc_encoding"));
    }
    import_metadata[StringName("has_normals")] = normals.size() == splat_count * 3;
    import_metadata[StringName("has_palette_ids")] = palette_ids.size() == splat_count;
    import_metadata[StringName("has_painterly_flags")] = painterly_flags.size() == splat_count;
    import_metadata[StringName("has_brush_override_ids")] = painterly_flags.size() == splat_count;
    import_metadata[StringName("has_brush_axes")] = brush_axes.size() == splat_count * 2;
    import_metadata[StringName("has_stroke_age")] = stroke_ages.size() == splat_count;
    import_metadata[StringName("opacity_encoding")] = StringName("logit");
    import_metadata[StringName("gaussian_2d_mode")] = p_gaussian_data->get_2d_mode();
    if (bounds_initialized) {
        import_metadata[StringName("bounds")] = AABB(min_pos, max_pos - min_pos);
        import_metadata[StringName("bounds_dirty")] = false;
    } else {
        _invalidate_bounds_metadata();
    }

    // The packed arrays now mirror an authoritative GaussianData payload.
    // Seal so that subsequent external set_* calls must first re-seat via
    // set_splat_count() or call populate_from_gaussian_data() again.
    payload_sealed = true;

    // DATA-001: the payload just changed. Bump the live version (still under
    // populate_mutex) so a consumer caching a materialized copy -- notably the scene
    // director's per-asset AssetRecord -- can detect the re-population and rebuild, since
    // Resource::get_edited_version() is TOOLS-only and never moves here.
    payload_version++;

    emit_changed();

    return OK;
}

uint32_t GaussianSplatAsset::get_payload_version() const {
    MutexLock cache_lock(populate_mutex);
    return payload_version;
}

namespace {
// Forward in-place compaction of a strided packed array by an ASCENDING keep-index list.
// keep[j] >= j (ascending, keep <= count), so writing slot j never clobbers an as-yet
// unread source slot. Copies raw elements -- survivors are byte-identical (no activation
// / sigmoid-logit round-trip). Empty (optional) lanes and zero stride are skipped.
void _gs_prune_compact_f32(PackedFloat32Array &r_arr, const LocalVector<uint32_t> &p_keep, uint32_t p_stride) {
    if (r_arr.is_empty() || p_stride == 0u) {
        return;
    }
    float *p = r_arr.ptrw();
    for (uint32_t j = 0; j < p_keep.size(); j++) {
        const uint32_t src = p_keep[j];
        if (src != j) {
            memmove(p + size_t(j) * p_stride, p + size_t(src) * p_stride, size_t(p_stride) * sizeof(float));
        }
    }
    r_arr.resize(int(p_keep.size() * p_stride));
}
void _gs_prune_compact_color(PackedColorArray &r_arr, const LocalVector<uint32_t> &p_keep) {
    if (r_arr.is_empty()) {
        return;
    }
    Color *p = r_arr.ptrw();
    for (uint32_t j = 0; j < p_keep.size(); j++) {
        const uint32_t src = p_keep[j];
        if (src != j) {
            p[j] = p[src];
        }
    }
    r_arr.resize(int(p_keep.size()));
}
void _gs_prune_compact_i32(PackedInt32Array &r_arr, const LocalVector<uint32_t> &p_keep) {
    if (r_arr.is_empty()) {
        return;
    }
    int32_t *p = r_arr.ptrw();
    for (uint32_t j = 0; j < p_keep.size(); j++) {
        const uint32_t src = p_keep[j];
        if (src != j) {
            p[j] = p[src];
        }
    }
    r_arr.resize(int(p_keep.size()));
}
} // namespace

uint32_t GaussianSplatAsset::prune_by_importance(double p_keep_ratio, float p_importance_threshold) {
    // Lossless default: leave the SoA arrays completely untouched so a default import is
    // byte-identical to base (Ultra regression). Matches GaussianData::prune_by_importance's
    // no-op guard so the two agree on what "off" means; no cache churn / seal transition here.
    if (p_keep_ratio >= 1.0 && p_importance_threshold <= 0.0f) {
        return get_splat_count();
    }

    // RANK ONLY via an AoS copy. populate_gaussian_data() takes populate_mutex internally, so
    // build the ranking copy BEFORE we take the lock. We deliberately do NOT write the AoS copy
    // back (populate_from_gaussian_data round-trips opacity through sigmoid/logit and CLAMPS
    // survivors to 0.0001..0.9999 -- lossy). Instead we compute the keep-index set here and
    // compact the raw SoA arrays in place, so every surviving splat stays byte-identical across
    // all lanes (opacity_logits, SH, colors, scales, rotations, palette/painterly, normals, ...).
    Ref<::GaussianData> ranking;
    if (!populate_gaussian_data(ranking) || ranking.is_null()) {
        return get_splat_count();
    }
    const uint32_t count = uint32_t(ranking->get_count());
    if (count == 0u) {
        return 0u;
    }
    const LocalVector<Gaussian> &g = ranking->get_gaussian_storage();
    LocalVector<float> importance;
    importance.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        importance[i] = ResidentAtlasBudget::gaussian_importance(g[i]);
    }

    // Keep-set: ratio top-k (keep-top-1 floor) intersected with the importance threshold.
    // Mirrors GaussianData::prune_by_importance exactly. select_top_k_indices returns ASCENDING
    // source indices, so filtering keeps them ascending (forward-compaction safe: keep[j] >= j).
    uint32_t ratio_keep = count;
    if (p_keep_ratio < 1.0) {
        const double clamped_ratio = p_keep_ratio > 0.0 ? p_keep_ratio : 0.0;
        int64_t rounded = int64_t(Math::round(double(count) * clamped_ratio));
        rounded = CLAMP(rounded, int64_t(1), int64_t(count));
        ratio_keep = uint32_t(rounded);
    }
    LocalVector<uint32_t> ratio_indices;
    ResidentAtlasBudget::select_top_k_indices(importance.ptr(), count, ratio_keep, ratio_indices);

    LocalVector<uint32_t> keep_indices;
    if (p_importance_threshold <= 0.0f) {
        keep_indices = ratio_indices;
    } else {
        keep_indices.reserve(ratio_indices.size());
        for (uint32_t j = 0; j < ratio_indices.size(); j++) {
            const uint32_t idx = ratio_indices[j];
            if (importance[idx] >= p_importance_threshold) {
                keep_indices.push_back(idx);
            }
        }
    }
    if (keep_indices.is_empty()) {
        WARN_PRINT_ONCE("[GaussianSplatAsset] prune_by_importance: ratio/threshold would have pruned "
                        "every splat; keeping the single highest-importance splat instead.");
        ResidentAtlasBudget::select_top_k_indices(importance.ptr(), count, 1u, keep_indices);
    }

    const uint32_t keep = keep_indices.size();
    if (keep >= count) {
        // No drop (ratio rounded to full AND threshold kept all): SoA already matches; untouched.
        return count;
    }

    // Compact the raw SoA arrays in place. Hold populate_mutex across the seal check + resize,
    // exactly like set_splat_count(), so a concurrent reader cannot observe torn arrays.
    MutexLock cache_lock(populate_mutex);
    if (!_runtime_mutation_permitted("prune_by_importance")) {
        return splat_count;
    }
    if (splat_count != count) {
        // SoA arrays changed between the unlocked ranking copy and here; bail without touching.
        return splat_count;
    }

    _gs_prune_compact_f32(positions, keep_indices, 3u);
    _gs_prune_compact_color(colors, keep_indices);
    _gs_prune_compact_f32(scales, keep_indices, 3u);
    _gs_prune_compact_f32(rotations, keep_indices, 4u);
    _gs_prune_compact_f32(sh_dc_coefficients, keep_indices, 3u);
    _gs_prune_compact_f32(sh_first_order_coefficients, keep_indices, sh_first_order_terms * 3u);
    _gs_prune_compact_f32(sh_high_order_coefficients, keep_indices, sh_high_order_terms * 3u);
    _gs_prune_compact_f32(opacity_logits, keep_indices, 1u);
    _gs_prune_compact_i32(palette_ids, keep_indices);
    _gs_prune_compact_i32(painterly_flags, keep_indices);
    _gs_prune_compact_f32(normals, keep_indices, 3u);
    _gs_prune_compact_f32(brush_axes, keep_indices, 2u);
    _gs_prune_compact_f32(stroke_ages, keep_indices, 1u);

    splat_count = keep;
    _ensure_buffer_sizes(); // no-op here (arrays already sized to keep*stride); mirrors set_splat_count()
    import_metadata[StringName("splat_count")] = int(keep);
    _invalidate_bounds_metadata();
    _invalidate_gaussian_data_cache();
    _invalidate_streaming_bake();
    emit_changed();
    return keep;
}
