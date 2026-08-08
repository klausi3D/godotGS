#include "resource_importer_spz.h"

#ifdef TOOLS_ENABLED

#include "spz_loader.h"
#include "gaussian_import_preset.h"
#include "streaming_chunk_bake.h"
#include "../editor/gaussian_import_settings_dialog.h"
#include "core/io/file_access.h"
#include "core/io/resource_saver.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "../core/gaussian_data.h"
#include "../core/gs_vector_alloc.h"
#include "../editor/gaussian_thumbnail_generator.h"
#include "../logger/gs_logger.h"

#include <algorithm>
#include <cfloat>

namespace {

#define OPTION_ASSET_TYPE SNAME("general/asset_type")
#define OPTION_PRESET SNAME("quality/preset")
#define OPTION_MAX_SPLATS SNAME("quality/max_splats")
#define OPTION_DENSITY SNAME("quality/density_multiplier")
#define OPTION_ENABLE_LOD SNAME("quality/enable_lod")
#define OPTION_OPTIMIZE_GPU SNAME("quality/optimize_for_gpu")
#define OPTION_NORMALIZE_OPACITY SNAME("processing/normalize_opacity")
#define OPTION_SORT_OPACITY SNAME("processing/sort_by_opacity")
#define OPTION_PRUNE_RATIO SNAME("processing/prune_ratio")
#define OPTION_PRUNE_IMPORTANCE_THRESHOLD SNAME("processing/prune_importance_threshold")
#define OPTION_QUANTIZE_POSITIONS SNAME("compression/quantize_positions")
#define OPTION_QUANTIZE_COLORS SNAME("compression/quantize_colors")
#define OPTION_QUANTIZE_SCALES SNAME("compression/quantize_scales")
#define OPTION_QUANTIZE_ROTATIONS SNAME("compression/quantize_rotations")
#define OPTION_PACK_OPACITY SNAME("compression/pack_opacity")
#define OPTION_GENERATE_THUMBNAIL SNAME("preview/generate_thumbnail")
#define OPTION_THUMBNAIL_STYLE SNAME("preview/thumbnail_style")
#define OPTION_THUMBNAIL_SIZE SNAME("preview/thumbnail_size")
#define OPTION_INCLUDE_STATS SNAME("metadata/include_statistics")
#define OPTION_INCLUDE_MEMORY SNAME("metadata/include_memory_estimate")

static bool _get_bool_option(const HashMap<StringName, Variant> &p_options, const StringName &p_name, bool p_default) {
    if (const Variant *value = p_options.getptr(p_name)) {
        return bool(*value);
    }
    return p_default;
}

static int _get_int_option(const HashMap<StringName, Variant> &p_options, const StringName &p_name, int p_default) {
    if (const Variant *value = p_options.getptr(p_name)) {
        return int64_t(*value);
    }
    return p_default;
}

static double _get_double_option(const HashMap<StringName, Variant> &p_options, const StringName &p_name, double p_default) {
    if (const Variant *value = p_options.getptr(p_name)) {
        return double(*value);
    }
    return p_default;
}

static String _get_string_option(const HashMap<StringName, Variant> &p_options, const StringName &p_name, const String &p_default) {
    if (const Variant *value = p_options.getptr(p_name)) {
        return String(*value);
    }
    return p_default;
}

static int _compute_final_splat_count(int p_original_count, int p_max_splats, double p_density) {
    int final_count = p_original_count;
    if (p_max_splats > 0) {
        final_count = MIN(final_count, p_max_splats);
    }
    final_count = MIN(final_count, int(Math::round(p_original_count * p_density)));
    final_count = MAX(final_count, 1);
    return final_count;
}

static Gaussian _merge_gaussian_range(const Ref<::GaussianData> &p_data, const int *p_indices,
        int p_start, int p_end, bool p_normalize_opacity, int *r_source_index = nullptr) {
    // Density multiplier is a subsampling factor; pick a representative splat
    // instead of averaging to avoid "hole" artifacts from blended positions.
    const int count = MAX(1, p_end - p_start);
    const int sample_index = CLAMP(p_start + (count / 2), p_start, p_end - 1);
    const int source_index = p_indices[sample_index];
    if (r_source_index) {
        *r_source_index = source_index;
    }
    Gaussian g = p_data->get_gaussian(source_index);
    if (p_normalize_opacity) {
        g.opacity = CLAMP(g.opacity, 0.0f, 1.0f);
    }
    return g;
}

static uint32_t _build_compression_flags(bool p_positions, bool p_colors, bool p_scales, bool p_rotations) {
    uint32_t flags = GaussianSplatAsset::COMPRESSION_NONE;
    if (p_positions) {
        flags |= GaussianSplatAsset::COMPRESSION_POSITIONS;
    }
    if (p_colors) {
        flags |= GaussianSplatAsset::COMPRESSION_COLORS;
    }
    if (p_scales) {
        flags |= GaussianSplatAsset::COMPRESSION_SCALES;
    }
    if (p_rotations) {
        flags |= GaussianSplatAsset::COMPRESSION_ROTATIONS;
    }
    return flags;
}

} // namespace

ResourceImporterSPZ::ResourceImporterSPZ() {}

String ResourceImporterSPZ::get_importer_name() const {
    return "gaussian_splat_spz";
}

String ResourceImporterSPZ::get_visible_name() const {
    return "Gaussian Splat SPZ";
}

void ResourceImporterSPZ::get_recognized_extensions(List<String> *p_extensions) const {
    p_extensions->push_back("spz");
}

String ResourceImporterSPZ::get_save_extension() const {
    return "res";
}

String ResourceImporterSPZ::get_resource_type() const {
    return "GaussianSplatAsset";
}

int ResourceImporterSPZ::get_preset_count() const {
    return gaussian_get_import_presets().size();
}

String ResourceImporterSPZ::get_preset_name(int p_idx) const {
    const Vector<GaussianImportPresetDefinition> &presets = gaussian_get_import_presets();
    if (p_idx < 0 || p_idx >= presets.size()) {
        return String();
    }
    return presets[p_idx].display_name;
}

void ResourceImporterSPZ::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
    (void)p_path;
    const Vector<GaussianImportPresetDefinition> &presets = gaussian_get_import_presets();
    int preset_index = CLAMP(p_preset, 0, presets.size() - 1);
    const GaussianImportPresetDefinition &preset = gaussian_get_import_preset_by_index(preset_index);

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::STRING, String(OPTION_PRESET), PROPERTY_HINT_ENUM,
                    "mobile,desktop,high,ultra,development,custom"),
            preset.id));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::INT, String(OPTION_ASSET_TYPE), PROPERTY_HINT_ENUM, "Static,Dynamic"),
            preset.default_asset_type));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::INT, String(OPTION_MAX_SPLATS), PROPERTY_HINT_RANGE, "0,5000000,1000"),
            preset.max_splats));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::FLOAT, String(OPTION_DENSITY), PROPERTY_HINT_RANGE, "0.1,1.0,0.05"),
            preset.density_multiplier));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_ENABLE_LOD)), preset.enable_lod));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_OPTIMIZE_GPU)), preset.optimize_for_gpu));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_NORMALIZE_OPACITY)), true));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_SORT_OPACITY)), false));

    // GS-PERF-PRUNE (issue #456): opt-in importance pruning. Defaults are a
    // strict no-op (keep all / off). prune_ratio is the portable, scale-invariant
    // control; prune_importance_threshold is an ADVANCED, scale-DEPENDENT absolute
    // cutoff (see the ADR) that is safe to leave at 0.0 unless tuned per-asset.
    r_options->push_back(ImportOption(
            PropertyInfo(Variant::FLOAT, String(OPTION_PRUNE_RATIO), PROPERTY_HINT_RANGE, "0.01,1.0,0.01"),
            1.0));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::FLOAT, String(OPTION_PRUNE_IMPORTANCE_THRESHOLD), PROPERTY_HINT_RANGE, "0.0,10.0,0.001"),
            0.0));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_QUANTIZE_POSITIONS)), preset.quantize_positions));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_QUANTIZE_COLORS)), preset.quantize_colors));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_QUANTIZE_SCALES)), preset.quantize_scales));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_QUANTIZE_ROTATIONS)), preset.quantize_rotations));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_GENERATE_THUMBNAIL)), true));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::INT, String(OPTION_THUMBNAIL_STYLE), PROPERTY_HINT_ENUM, "Color,Density,Normals,Heatmap"),
            preset.thumbnail_style));

    r_options->push_back(ImportOption(
            PropertyInfo(Variant::INT, String(OPTION_THUMBNAIL_SIZE), PROPERTY_HINT_RANGE, "32,512,16"),
            preset.default_thumbnail_size));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_INCLUDE_STATS)), preset.include_statistics));

    r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, String(OPTION_INCLUDE_MEMORY)), preset.include_memory_estimate));
}

bool ResourceImporterSPZ::get_option_visibility(const String &p_path, const String &p_option,
        const HashMap<StringName, Variant> &p_options) const {
    (void)p_path;
    (void)p_option;
    (void)p_options;
    return true;
}

Error ResourceImporterSPZ::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path,
        const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants,
        List<String> *r_gen_files, Variant *r_metadata) {
    (void)p_source_id;
    (void)r_platform_variants;

    if (!FileAccess::exists(p_source_file)) {
        GS_LOG_ERROR_DEFAULT("SPZ file not found: " + p_source_file);
        return ERR_FILE_NOT_FOUND;
    }

    // Load SPZ file
    Ref<SPZLoader> loader;
    loader.instantiate();
    Error err = loader->load_file(p_source_file);
    if (err != OK) {
        GS_LOG_ERROR_DEFAULT(vformat("Failed to load SPZ file: %s (error %d)", p_source_file, err));
        return err;
    }

    Ref<::GaussianData> gaussian_data = loader->get_gaussian_data();
    if (!gaussian_data.is_valid() || gaussian_data->get_count() == 0) {
        GS_LOG_ERROR_DEFAULT("SPZ file contains no Gaussian data: " + p_source_file);
        return ERR_FILE_CORRUPT;
    }

    // Full-asset finiteness sweep: reject NaN/Inf anywhere in the payload rather
    // than letting corrupt splats reach the GPU. Shares the PLY importer's
    // validator so neither format can smuggle a bad splat past a clean splat 0
    // (#518).
    int spz_bad_index = -1;
    if (!gaussian_data->all_render_fields_finite(&spz_bad_index)) {
        GS_LOG_ERROR_DEFAULT(vformat(
                "SPZ validation failed: Non-finite (NaN/Inf) position/scale/rotation/opacity at splat %d in %s",
                spz_bad_index, p_source_file));
        return ERR_FILE_CORRUPT;
    }

    const int original_count = gaussian_data->get_count();

    // Get import options
    String preset_name = _get_string_option(p_options, OPTION_PRESET,
            gaussian_get_import_preset_by_index(gaussian_find_import_preset_index("desktop")).id);
    preset_name = preset_name.to_lower();
    const GaussianImportPresetDefinition &preset = gaussian_get_import_preset_by_name(preset_name);

    int asset_type_value = _get_int_option(p_options, OPTION_ASSET_TYPE, preset.default_asset_type);
    GaussianSplatAsset::AssetType asset_type = asset_type_value == 1 ? GaussianSplatAsset::ASSET_TYPE_DYNAMIC : GaussianSplatAsset::ASSET_TYPE_STATIC;

    int max_splats = _get_int_option(p_options, OPTION_MAX_SPLATS, preset.max_splats);
    double density_multiplier = _get_double_option(p_options, OPTION_DENSITY, preset.density_multiplier);
    density_multiplier = CLAMP(density_multiplier, 0.1, 1.0);
    bool enable_lod = _get_bool_option(p_options, OPTION_ENABLE_LOD, preset.enable_lod);
    bool optimize_for_gpu = _get_bool_option(p_options, OPTION_OPTIMIZE_GPU, preset.optimize_for_gpu);
    bool normalize_opacity = _get_bool_option(p_options, OPTION_NORMALIZE_OPACITY, true);
    bool sort_by_opacity = _get_bool_option(p_options, OPTION_SORT_OPACITY, false);
    // GS-PERF-PRUNE (#456): opt-in importance pruning. Clamp to the documented
    // hint ranges; the (1.0, 0.0) default is a strict no-op in prune_by_importance.
    double prune_ratio = _get_double_option(p_options, OPTION_PRUNE_RATIO, 1.0);
    prune_ratio = CLAMP(prune_ratio, 0.0, 1.0);
    double prune_threshold_opt = _get_double_option(p_options, OPTION_PRUNE_IMPORTANCE_THRESHOLD, 0.0);
    float prune_importance_threshold = float(MAX(prune_threshold_opt, 0.0));
    bool quantize_positions = _get_bool_option(p_options, OPTION_QUANTIZE_POSITIONS, preset.quantize_positions);
    bool quantize_colors = _get_bool_option(p_options, OPTION_QUANTIZE_COLORS, preset.quantize_colors);
    bool quantize_scales = _get_bool_option(p_options, OPTION_QUANTIZE_SCALES, preset.quantize_scales);
    bool quantize_rotations = _get_bool_option(p_options, OPTION_QUANTIZE_ROTATIONS, preset.quantize_rotations);
    const Variant *legacy_pack_opacity_value = p_options.getptr(OPTION_PACK_OPACITY);
    const bool legacy_pack_opacity_requested = legacy_pack_opacity_value && bool(*legacy_pack_opacity_value);
    if (legacy_pack_opacity_requested) {
        WARN_PRINT_ONCE("[ResourceImporterSPZ] compression/pack_opacity is deprecated and ignored.");
    }
    bool generate_thumbnail = _get_bool_option(p_options, OPTION_GENERATE_THUMBNAIL, true);
    int thumbnail_style = _get_int_option(p_options, OPTION_THUMBNAIL_STYLE, preset.thumbnail_style);
    int thumbnail_size = _get_int_option(p_options, OPTION_THUMBNAIL_SIZE, preset.default_thumbnail_size);
    bool include_stats = _get_bool_option(p_options, OPTION_INCLUDE_STATS, preset.include_statistics);
    bool include_memory = _get_bool_option(p_options, OPTION_INCLUDE_MEMORY, preset.include_memory_estimate);

    thumbnail_style = CLAMP(thumbnail_style, 0, 3);
    thumbnail_size = CLAMP(thumbnail_size, 32, 512);

    int final_count = _compute_final_splat_count(original_count, max_splats, density_multiplier);
    const bool merge_density = density_multiplier < 0.999 && final_count < original_count;
    const double merge_stride = merge_density ? double(original_count) / double(final_count) : 1.0;

    // Create index array for potential sorting
    Vector<int> indices;
    // #798: original_count is the SPZ's splat count -- FILE-DERIVED, so this allocation is
    // reachable from a merely large or malformed asset without any memory pressure. The
    // loop writes through the raw ptrw() pointer bounded by original_count, not by
    // indices.size(); a failed resize leaves ptrw() null (CowData::_fork_allocate(0) unrefs
    // and leaves _ptr null) and indices_ptr[0] is then a wild write with no
    // CRASH_BAD_INDEX to name it. Fail closed with this importer's own Error contract,
    // naming the allocation rather than misreporting file corruption.
    if (!gs_resize_or_fail(indices, original_count, "ResourceImporterSPZ::import indices")) {
        return ERR_OUT_OF_MEMORY;
    }
    int *indices_ptr = indices.ptrw();
    for (int i = 0; i < original_count; i++) {
        indices_ptr[i] = i;
    }

    if (sort_by_opacity) {
        std::sort(indices_ptr, indices_ptr + original_count, [&](int a, int b) {
            return gaussian_data->get_gaussian(a).opacity > gaussian_data->get_gaussian(b).opacity;
        });
    }

    // Extract data arrays
    PackedFloat32Array positions;
    PackedColorArray colors;
    PackedFloat32Array scales;
    PackedFloat32Array rotations;

    // #798: every count here derives from final_count, which derives from the SPZ's own
    // splat count -- FILE-DERIVED. The write loop below drives raw ptrw() pointers with an
    // index bounded by final_count rather than by each array's size(), so a failed resize
    // turns the very first store into a write through nullptr. Fail closed before any
    // asset is instantiated or written, reporting the allocation as the cause.
    if (!gs_resize_or_fail(positions, int64_t(final_count) * 3, "ResourceImporterSPZ::import positions") ||
            !gs_resize_or_fail(colors, final_count, "ResourceImporterSPZ::import colors") ||
            !gs_resize_or_fail(scales, int64_t(final_count) * 3, "ResourceImporterSPZ::import scales") ||
            !gs_resize_or_fail(rotations, int64_t(final_count) * 4, "ResourceImporterSPZ::import rotations")) {
        return ERR_OUT_OF_MEMORY;
    }

    // View-dependent SH bands. The SPZ loader already parsed bands 1..3 into
    // `Gaussian::sh_1[]` and `GaussianData::sh_high_order_coefficients`; mirror
    // them into the asset so the renderer evaluates beyond the DC term. Layout
    // per gaussian_splat_asset.cpp:1411-1428: per-splat, per-term, RGB triplet.
    const uint32_t sh_first_terms = gaussian_data->get_sh_first_order_count();
    const uint32_t sh_high_terms = gaussian_data->get_sh_high_order_count();
    const Vector3 *sh_high_src = gaussian_data->get_sh_high_order_coefficients_ptr();

    PackedFloat32Array sh_first_order;
    PackedFloat32Array sh_high_order;
    PackedFloat32Array opacity_logits;
    // #798: opacity_logits is written via write[] (the #794 class, which CRASH_BAD_INDEX
    // traps loudly); it is checked here only so that one logical output allocation has one
    // failure mode instead of two.
    if (!gs_resize_or_fail(opacity_logits, final_count, "ResourceImporterSPZ::import opacity_logits")) {
        return ERR_OUT_OF_MEMORY;
    }
    // #798: these two are NOT wild writes -- the loop below re-checks `sh_first_ptr` /
    // `sh_high_ptr` for null, so a failed resize skips the stores. They are checked anyway
    // because that guard converts the crash into SILENT PERMANENT DATA LOSS: the import
    // would return OK and bake a DC-only .res (set_sh_*_coefficients() maps an empty array
    // to terms=0), so the asset renders without view-dependent SH forever and nothing
    // reports why. Counts are final_count x term count, both FILE-DERIVED.
    if (sh_first_terms > 0) {
        if (!gs_resize_or_fail(sh_first_order, int64_t(final_count) * int64_t(sh_first_terms) * 3,
                    "ResourceImporterSPZ::import sh_first_order")) {
            return ERR_OUT_OF_MEMORY;
        }
    }
    if (sh_high_terms > 0 && sh_high_src != nullptr) {
        if (!gs_resize_or_fail(sh_high_order, int64_t(final_count) * int64_t(sh_high_terms) * 3,
                    "ResourceImporterSPZ::import sh_high_order")) {
            return ERR_OUT_OF_MEMORY;
        }
    }

    float *positions_ptr = positions.ptrw();
    Color *colors_ptr = colors.ptrw();
    float *scales_ptr = scales.ptrw();
    float *rotations_ptr = rotations.ptrw();
    float *sh_first_ptr = sh_first_order.ptrw();
    float *sh_high_ptr = sh_high_order.ptrw();

    for (int i = 0; i < final_count; i++) {
        int start = merge_density ? int(Math::floor(double(i) * merge_stride)) : i;
        int end = merge_density ? int(Math::floor(double(i + 1) * merge_stride)) : i + 1;
        start = CLAMP(start, 0, original_count - 1);
        end = CLAMP(end, start + 1, original_count);
        int pos_base = i * 3;
        int source_index = indices_ptr[start];
        Gaussian g = merge_density ? _merge_gaussian_range(gaussian_data, indices_ptr, start, end, normalize_opacity, &source_index)
                                   : gaussian_data->get_gaussian(source_index);

        positions_ptr[pos_base + 0] = g.position.x;
        positions_ptr[pos_base + 1] = g.position.y;
        positions_ptr[pos_base + 2] = g.position.z;

        Color color = g.sh_dc;
        color.a = normalize_opacity ? CLAMP(g.opacity, 0.0f, 1.0f) : g.opacity;
        colors_ptr[i] = color;
        // Populate opacity_logits in lockstep with color.a. _ensure_buffer_sizes
        // zero-fills opacity_logits to splat_count, and get_opacities() prefers
        // logits over color.a — leaving them unwritten yields sigmoid(0)=0.5
        // for every splat regardless of the actual stored alpha.
        const float clamped_op = CLAMP(g.opacity, 0.0001f, 0.9999f);
        opacity_logits.write[i] = Math::log(clamped_op / (1.0f - clamped_op));

        int scale_base = i * 3;
        scales_ptr[scale_base + 0] = g.scale.x;
        scales_ptr[scale_base + 1] = g.scale.y;
        scales_ptr[scale_base + 2] = g.scale.z;

        int rot_base = i * 4;
        rotations_ptr[rot_base + 0] = g.rotation.w;
        rotations_ptr[rot_base + 1] = g.rotation.x;
        rotations_ptr[rot_base + 2] = g.rotation.y;
        rotations_ptr[rot_base + 3] = g.rotation.z;

        if (sh_first_terms > 0 && sh_first_ptr != nullptr) {
            const int first_base = i * int(sh_first_terms) * 3;
            for (uint32_t term = 0; term < sh_first_terms && term < 3; term++) {
                const Vector3 &coeff = g.sh_1[term];
                const int term_base = first_base + int(term) * 3;
                sh_first_ptr[term_base + 0] = coeff.x;
                sh_first_ptr[term_base + 1] = coeff.y;
                sh_first_ptr[term_base + 2] = coeff.z;
            }
        }

        if (sh_high_terms > 0 && sh_high_src != nullptr && sh_high_ptr != nullptr) {
            const int high_base = i * int(sh_high_terms) * 3;
            const size_t src_row = size_t(source_index) * size_t(sh_high_terms);
            for (uint32_t term = 0; term < sh_high_terms; term++) {
                const Vector3 &coeff = sh_high_src[src_row + term];
                const int term_base = high_base + int(term) * 3;
                sh_high_ptr[term_base + 0] = coeff.x;
                sh_high_ptr[term_base + 1] = coeff.y;
                sh_high_ptr[term_base + 2] = coeff.z;
            }
        }
    }

    // Create asset
    Ref<GaussianSplatAsset> asset;
    asset.instantiate();
    asset->set_asset_type(asset_type);
    asset->set_positions(positions);
    asset->set_colors(colors);
    asset->set_scales(scales);
    asset->set_rotations(rotations);
    asset->set_opacity_logits(opacity_logits);
    if (sh_first_terms > 0) {
        asset->set_sh_first_order_coefficients(sh_first_order);
    }
    if (sh_high_terms > 0) {
        asset->set_sh_high_order_coefficients(sh_high_order);
    }
    asset->set_import_quality_preset(preset_name);

    // GS-PERF-PRUNE (#456): importance prune runs on the asset's post-transform
    // arrays (after density merge / sort_by_opacity / max_splats) and BEFORE the
    // thumbnail, memory estimate, bounds, metadata, and streaming-chunk bake
    // below, so the saved .res AND the baked chunks both reflect the pruned set.
    // No-op at the default (1.0 / 0.0) -> saved_count == final_count.
    const int saved_count = int(asset->prune_by_importance(prune_ratio, prune_importance_threshold));

    uint32_t compression_flags = _build_compression_flags(quantize_positions, quantize_colors, quantize_scales, quantize_rotations);
    asset->set_compression_flags(compression_flags);
    // Generate thumbnail
    Ref<Image> thumbnail;
    if (generate_thumbnail) {
        Ref<GaussianThumbnailGenerator> generator;
        generator.instantiate();
        thumbnail = generator->generate_thumbnail_image(asset, thumbnail_size,
                GaussianThumbnailGenerator::style_from_int(thumbnail_style));
        asset->set_preview_image(thumbnail);
    } else {
        asset->set_preview_image(Ref<Image>());
    }

    Dictionary option_dict;
    option_dict[OPTION_PRESET] = preset_name;
    option_dict[OPTION_ASSET_TYPE] = asset_type_value;
    option_dict[OPTION_MAX_SPLATS] = max_splats;
    option_dict[OPTION_DENSITY] = density_multiplier;
    option_dict[OPTION_ENABLE_LOD] = enable_lod;
    option_dict[OPTION_OPTIMIZE_GPU] = optimize_for_gpu;
    option_dict[OPTION_NORMALIZE_OPACITY] = normalize_opacity;
    option_dict[OPTION_SORT_OPACITY] = sort_by_opacity;
    option_dict[OPTION_PRUNE_RATIO] = prune_ratio;
    option_dict[OPTION_PRUNE_IMPORTANCE_THRESHOLD] = prune_importance_threshold;
    option_dict[OPTION_QUANTIZE_POSITIONS] = quantize_positions;
    option_dict[OPTION_QUANTIZE_COLORS] = quantize_colors;
    option_dict[OPTION_QUANTIZE_SCALES] = quantize_scales;
    option_dict[OPTION_QUANTIZE_ROTATIONS] = quantize_rotations;
    option_dict[OPTION_PACK_OPACITY] = legacy_pack_opacity_requested;
    option_dict[OPTION_GENERATE_THUMBNAIL] = generate_thumbnail;
    option_dict[OPTION_THUMBNAIL_STYLE] = thumbnail_style;
    option_dict[OPTION_THUMBNAIL_SIZE] = thumbnail_size;
    option_dict[OPTION_INCLUDE_STATS] = include_stats;
    option_dict[OPTION_INCLUDE_MEMORY] = include_memory;

    // Build metadata
    Dictionary import_metadata;
    import_metadata[StringName("source_file")] = p_source_file;
    import_metadata[StringName("source_format")] = "spz";
    import_metadata[StringName("dc_encoding")] = "linear_rgb";
    import_metadata[StringName("import_time")] = Time::get_singleton()->get_datetime_dict_from_system();
    // Dual counts: original_splat_count is the raw source count (pre-reduction);
    // splat_count is the FINAL count actually written to the .res (post density /
    // max_splats AND post importance prune). pre_prune_splat_count records the
    // count that entered pruning so an A/B can see the prune-only reduction. When
    // pruning is off, saved_count == final_count.
    import_metadata[StringName("original_splat_count")] = original_count;
    import_metadata[StringName("pre_prune_splat_count")] = final_count;
    import_metadata[StringName("splat_count")] = saved_count;
    import_metadata[StringName("prune_ratio")] = prune_ratio;
    import_metadata[StringName("prune_importance_threshold")] = prune_importance_threshold;
    import_metadata[StringName("asset_type")] = asset_type;
    import_metadata[StringName("quality_preset")] = preset_name;
    import_metadata[StringName("preset_display")] = preset.display_name;
    import_metadata[StringName("enable_lod")] = enable_lod;
    import_metadata[StringName("optimize_for_gpu")] = optimize_for_gpu;
    import_metadata[StringName("density_multiplier")] = density_multiplier;
    import_metadata[StringName("max_splats")] = max_splats;
    import_metadata[StringName("normalize_opacity")] = normalize_opacity;
    import_metadata[StringName("sort_by_opacity")] = sort_by_opacity;
    import_metadata[StringName("compression_flags")] = int(compression_flags);
    import_metadata[StringName("options")] = option_dict;
    import_metadata[StringName("thumbnail_generated")] = generate_thumbnail;
    import_metadata[StringName("thumbnail_style")] = thumbnail_style;
    import_metadata[StringName("thumbnail_size")] = thumbnail_size;
    import_metadata[StringName("thumbnail_style_name")] = GaussianThumbnailGenerator::style_to_display_name(
            GaussianThumbnailGenerator::style_from_int(thumbnail_style));

    // SPZ-specific metadata
    SPZLoader::SPZHeader spz_header = loader->get_header();
    import_metadata[StringName("spz_version")] = (int)spz_header.version;
    import_metadata[StringName("spz_sh_degree")] = (int)spz_header.sh_degree;
    import_metadata[StringName("spz_fractional_bits")] = (int)spz_header.fractional_bits;
    import_metadata[StringName("spz_antialiased")] = (spz_header.flags & SPZLoader::SPZ_FLAG_ANTIALIASED) != 0;

    if (include_stats) {
        import_metadata[StringName("loader_statistics")] = loader->get_load_statistics();
    }
    if (include_memory) {
        Ref<GaussianThumbnailGenerator> generator;
        generator.instantiate();
        Dictionary memory_stats = generator->compute_memory_statistics(saved_count, compression_flags, false);
        import_metadata[StringName("memory_estimate_mb")] = memory_stats.get(StringName("total_mb"), 0.0);
        import_metadata[StringName("memory_breakdown_mb")] = memory_stats;
    }

    // Phase B.1: bake per-chunk spatial bookkeeping so runtime
    // GaussianStreamingSystem::_build_chunks_for_data can skip the per-splat
    // pass on scene load. Materialize from the asset's post-transform arrays
    // rather than the loader's `gaussian_data`: sort_by_opacity, density
    // merge, max_splats, AND importance prune reorder/reduce splats before they
    // are written to the asset, so baking from the pre-transform layout would
    // write chunk bounds/counts that do not match the saved arrays (Codex P1 on
    // #349). Fetch it once here and reuse it for the saved bounds AABB.
    Ref<::GaussianData> post_transform_data = asset->get_gaussian_data();

    // Bounds must reflect the post-prune asset (get_aabb() is computed fresh from
    // the current gaussians); a stale loader-side AABB would still include dropped
    // splats after pruning.
    AABB bounds = post_transform_data.is_valid() ? post_transform_data->get_aabb() : gaussian_data->get_aabb();
    import_metadata[StringName("bounds")] = bounds;

    asset->set_import_metadata(import_metadata);
    asset->set_source_path(p_source_file);

    bake_streaming_chunks_for_asset(asset, post_transform_data, /*include_primary*/ false, /*quant*/ nullptr);

    // Save asset
    String save_path = p_save_path + "." + get_save_extension();
    err = ResourceSaver::save(asset, save_path);
    if (err != OK) {
        GS_LOG_ERROR_DEFAULT("Failed to save GaussianSplatAsset: " + save_path);
        return err;
    }

    if (r_gen_files) {
        r_gen_files->push_back(save_path);
    }

    if (r_metadata) {
        *r_metadata = import_metadata;
    }

    GS_LOG_STREAMING_INFO(vformat("SPZ import successful: %d/%d splats (pre-prune %d) from %s",
            saved_count, original_count, final_count, p_source_file));
    return OK;
}

bool ResourceImporterSPZ::has_advanced_options() const {
    return true;
}

void ResourceImporterSPZ::show_advanced_options(const String &p_path) {
    GaussianImportSettingsDialog *dialog = GaussianImportSettingsDialog::get_singleton();
    if (dialog) {
        dialog->open_settings(p_path);
    }
}

#endif // TOOLS_ENABLED
