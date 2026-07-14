#include "pipeline_feature_set.h"

#include "servers/rendering/rendering_device.h"
#include "../core/effective_config_snapshot.h"
#include "../core/gs_project_settings.h"
#include "../core/quality_tier_config.h"
#include "../logger/gs_logger.h"

const String PipelineFeatureSet::SECTION_PATH = "rendering/gaussian_splatting/pipeline/";
const String PipelineFeatureSet::ENABLE_PACKED_STAGE_DATA_PATH = SECTION_PATH + "enable_packed_stage_data";
const String PipelineFeatureSet::ENABLE_TIGHTER_BOUNDS_PATH = SECTION_PATH + "enable_tighter_bounds";
const String PipelineFeatureSet::ENABLE_FAST_RASTER_PATH = SECTION_PATH + "enable_fast_raster";
const String PipelineFeatureSet::ENABLE_SH_AMORTIZATION_PATH = SECTION_PATH + "enable_sh_amortization";
const String PipelineFeatureSet::SH_AMORTIZATION_DIVISOR_PATH = SECTION_PATH + "sh_amortization_divisor";
const String PipelineFeatureSet::ENABLE_ALL_PIPELINE_EXPERIMENTAL_PATH = SECTION_PATH + "enable_all_pipeline_experimental";
const String PipelineFeatureSet::DEPRECATED_ENABLE_ALL_EXPERIMENTAL_PATH = SECTION_PATH + "enable_all_experimental";

PipelineFeatureSet g_pipeline_feature_set;

// Resolve pipeline/enable_all_pipeline_experimental (#169) honoring the
// deprecated pipeline/enable_all_experimental alias. Precedence: an explicit
// canonical override wins; else the deprecated alias (with a one-time WARN);
// else the canonical registered default. A default project (only the builtin
// canonical present, unset) resolves to false with no warning, so the rename is
// behavior-neutral. Same accepted limitation as the other alias helpers: a value
// set EQUAL to the code default reads as unset.
static bool _load_enable_all_pipeline_experimental(ProjectSettings *p_ps) {
    if (!p_ps) {
        return false;
    }
    const String canonical = PipelineFeatureSet::ENABLE_ALL_PIPELINE_EXPERIMENTAL_PATH;
    const String deprecated = PipelineFeatureSet::DEPRECATED_ENABLE_ALL_EXPERIMENTAL_PATH;
    if (p_ps->has_setting(canonical) && p_ps->property_can_revert(canonical) &&
            p_ps->get_setting_with_override(canonical) != p_ps->property_get_revert(canonical)) {
        return gs::settings::get_bool(p_ps, canonical, false);
    }
    if (p_ps->has_setting(deprecated)) {
        WARN_PRINT_ONCE(vformat("[GaussianSplatting] Project setting '%s' is deprecated; use '%s' instead. Deprecated alias support is read-only compatibility and will be removed after project migration.",
                deprecated, canonical));
        return gs::settings::get_bool(p_ps, deprecated, false);
    }
    if (p_ps->has_setting(canonical)) {
        return gs::settings::get_bool(p_ps, canonical, false);
    }
    return false;
}

static void _describe_project_setting_source(ProjectSettings *p_ps, const String &p_path,
        String &r_source, String &r_source_label) {
    // Detect an explicit project.godot override by the effective value differing
    // from the registered default (property_get_revert), NOT by !is_builtin_setting.
    // GLOBAL_DEF calls set_builtin_order() on EVERY key at registration -- including
    // keys already loaded from project.godot -- so is_builtin_setting() is true for
    // persisted user values too and would misclassify a real startup override as a
    // code default (and, since #175, let the tier wrongly overwrite it). Comparing
    // the effective value against the registered revert value is order-independent
    // and matches gs_tier_cap::_project_setting_has_override() used for the streaming
    // budgets. Limitation shared with streaming: a value explicitly set EQUAL to the
    // code default is indistinguishable from unset and is treated as code_default.
    if (p_ps != nullptr && p_ps->has_setting(p_path) && p_ps->property_can_revert(p_path) &&
            p_ps->get_setting_with_override(p_path) != p_ps->property_get_revert(p_path)) {
        r_source = "project_override";
        r_source_label = "project override";
        return;
    }
    r_source = "code_default";
    r_source_label = "code default";
}

static void _register_pipeline_project_settings() {
    GLOBAL_DEF(
            PropertyInfo(Variant::BOOL, PipelineFeatureSet::ENABLE_PACKED_STAGE_DATA_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.enable_packed_stage_data);
    GLOBAL_DEF(
            PropertyInfo(Variant::BOOL, PipelineFeatureSet::ENABLE_TIGHTER_BOUNDS_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.enable_tighter_bounds);
    GLOBAL_DEF(
            PropertyInfo(Variant::BOOL, PipelineFeatureSet::ENABLE_FAST_RASTER_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.enable_fast_raster);
    GLOBAL_DEF(
            PropertyInfo(Variant::BOOL, PipelineFeatureSet::ENABLE_SH_AMORTIZATION_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.enable_sh_amortization);
    GLOBAL_DEF(
            PropertyInfo(Variant::INT, PipelineFeatureSet::SH_AMORTIZATION_DIVISOR_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.sh_amortization_divisor);
    // Canonical key (#169). The old enable_all_experimental spelling is
    // intentionally NOT registered: it is read only as a deprecated alias when a
    // project.godot explicitly sets it (see _load_enable_all_pipeline_experimental).
    GLOBAL_DEF(
            PropertyInfo(Variant::BOOL, PipelineFeatureSet::ENABLE_ALL_PIPELINE_EXPERIMENTAL_PATH,
                    PROPERTY_HINT_NONE, String(),
                    PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_STORAGE),
            g_pipeline_feature_set.enable_all_pipeline_experimental);
}

void PipelineFeatureSet::load_from_project_settings() {
    // Precedence for pipeline/* settings (issue #175 — EXPLICIT GRANULAR WINS):
    //   1. Code defaults — the values of g_pipeline_feature_set.* members at
    //      module init time. Registered as the default arg of GLOBAL_DEF.
    //   2. Quality tier — when quality/tier_apply_pipeline_toggles=true and a real
    //      tier is active, the tier supplies the value for any key NOT explicitly
    //      set in project.godot.
    //   3. Explicit project.godot value — an entry actually present in
    //      project.godot (source == "project_override") WINS over the tier for
    //      that key; a WARN is logged when the tier value would have differed.
    //
    // Implication: an explicit pipeline/enable_* entry always takes effect, even
    // when a tier preset is active (the tier only fills keys left at their code
    // default). This mirrors the sentinel/explicit-wins model already used for
    // the streaming budgets and sh/quantization "Auto" settings. The keys are
    // marked PROPERTY_USAGE_NO_EDITOR so they stay hidden from the default editor
    // inspector tree but remain readable/writable via ProjectSettings.
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    // Read every value the feature-tag-aware way (get_setting_with_override, via the
    // gs::settings accessors) so the LOADED value matches what
    // _describe_project_setting_source() detects and what the streaming half uses.
    // Otherwise a platform override such as enable_tighter_bounds.windows=true would
    // be seen by the override detector (get_setting_with_override) but the base value
    // would be loaded (plain get_setting), dropping both the feature override and the
    // tier value on the matching platform.
    enable_packed_stage_data = gs::settings::get_bool(ps, ENABLE_PACKED_STAGE_DATA_PATH, false);
    enable_tighter_bounds = gs::settings::get_bool(ps, ENABLE_TIGHTER_BOUNDS_PATH, false);
    enable_fast_raster = gs::settings::get_bool(ps, ENABLE_FAST_RASTER_PATH, false);
    enable_sh_amortization = gs::settings::get_bool(ps, ENABLE_SH_AMORTIZATION_PATH, false);
    sh_amortization_divisor = gs::settings::get_int(ps, SH_AMORTIZATION_DIVISOR_PATH, sh_amortization_divisor);
    enable_all_pipeline_experimental = _load_enable_all_pipeline_experimental(ps);

    const String tier_preset = ps->has_setting("rendering/gaussian_splatting/quality/tier_preset")
            ? String(ps->get_setting_with_override("rendering/gaussian_splatting/quality/tier_preset"))
            : String("custom");
    const bool apply_tier_toggles = gs::settings::get_bool(ps, "rendering/gaussian_splatting/quality/tier_apply_pipeline_toggles", true);

    String packed_stage_source;
    String packed_stage_source_label;
    String tighter_bounds_source;
    String tighter_bounds_source_label;
    String fast_raster_source;
    String fast_raster_source_label;
    String sh_amortization_source;
    String sh_amortization_source_label;
    String sh_amortization_divisor_source;
    String sh_amortization_divisor_source_label;

    _describe_project_setting_source(ps, ENABLE_PACKED_STAGE_DATA_PATH, packed_stage_source, packed_stage_source_label);
    _describe_project_setting_source(ps, ENABLE_TIGHTER_BOUNDS_PATH, tighter_bounds_source, tighter_bounds_source_label);
    _describe_project_setting_source(ps, ENABLE_FAST_RASTER_PATH, fast_raster_source, fast_raster_source_label);
    _describe_project_setting_source(ps, ENABLE_SH_AMORTIZATION_PATH, sh_amortization_source, sh_amortization_source_label);
    _describe_project_setting_source(ps, SH_AMORTIZATION_DIVISOR_PATH, sh_amortization_divisor_source, sh_amortization_divisor_source_label);

    if (apply_tier_toggles) {
        QualityTierConfig tier_config;
        if (get_quality_tier_config(tier_preset, tier_config)) {
            const String tier_label = vformat("tier preset '%s'", tier_preset);

            // Issue #175: an EXPLICITLY-set granular pipeline key (present in
            // project.godot -> source == "project_override") WINS over the tier
            // preset; only keys left at their code default inherit the tier value.
            // A WARN fires only when the tier value would actually DIFFER from the
            // kept explicit value (a no-op match, e.g. deck fixtures, stays quiet).
            auto apply_bool = [&](const char *p_key, bool &r_value, bool p_tier_value,
                    String &r_source, String &r_source_label) {
                if (r_source == "project_override") {
                    if (r_value != p_tier_value) {
                        GS_LOG_WARN_DEFAULT(vformat(
                                "[Pipeline Feature Set] %s: explicit project override (%s) kept; %s value (%s) NOT applied.",
                                p_key, r_value ? "true" : "false", tier_label,
                                p_tier_value ? "true" : "false"));
                    }
                    return; // keep explicit value AND its "project_override" source
                }
                r_value = p_tier_value;
                r_source = "tier_preset";
                r_source_label = tier_label;
            };

            apply_bool("enable_packed_stage_data", enable_packed_stage_data,
                    tier_config.enable_packed_stage_data, packed_stage_source, packed_stage_source_label);
            apply_bool("enable_tighter_bounds", enable_tighter_bounds,
                    tier_config.enable_tighter_bounds, tighter_bounds_source, tighter_bounds_source_label);
            apply_bool("enable_fast_raster", enable_fast_raster,
                    tier_config.enable_fast_raster, fast_raster_source, fast_raster_source_label);
            apply_bool("enable_sh_amortization", enable_sh_amortization,
                    tier_config.enable_sh_amortization, sh_amortization_source, sh_amortization_source_label);

            // sh_amortization_divisor is an int; same explicit-wins rule.
            if (sh_amortization_divisor_source == "project_override") {
                if (sh_amortization_divisor != tier_config.sh_amortization_divisor) {
                    GS_LOG_WARN_DEFAULT(vformat(
                            "[Pipeline Feature Set] sh_amortization_divisor: explicit project override (%d) kept; %s value (%d) NOT applied.",
                            sh_amortization_divisor, tier_label, tier_config.sh_amortization_divisor));
                }
            } else {
                sh_amortization_divisor = tier_config.sh_amortization_divisor;
                sh_amortization_divisor_source = "tier_preset";
                sh_amortization_divisor_source_label = tier_label;
            }

            GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] Applying quality tier preset: %s", tier_config.name));
        }
    }

    Dictionary snapshot;
    GaussianEffectiveConfig::set_entry(snapshot, StringName("pipeline_packed_stage_data"),
            enable_packed_stage_data, packed_stage_source, packed_stage_source_label);
    GaussianEffectiveConfig::set_entry(snapshot, StringName("pipeline_tighter_bounds"),
            enable_tighter_bounds, tighter_bounds_source, tighter_bounds_source_label);
    GaussianEffectiveConfig::set_entry(snapshot, StringName("pipeline_fast_raster"),
            enable_fast_raster, fast_raster_source, fast_raster_source_label);
    GaussianEffectiveConfig::set_entry(snapshot, StringName("pipeline_sh_amortization"),
            enable_sh_amortization, sh_amortization_source, sh_amortization_source_label);
    GaussianEffectiveConfig::set_entry(snapshot, StringName("pipeline_sh_amortization_divisor"),
            int64_t(sh_amortization_divisor), sh_amortization_divisor_source, sh_amortization_divisor_source_label);
    loaded_provenance_snapshot = snapshot;
    effective_provenance_snapshot = Dictionary();
    effective_provenance_snapshot_valid = false;

    if (enable_all_pipeline_experimental || enable_packed_stage_data ||
            enable_tighter_bounds || enable_fast_raster || enable_sh_amortization) {
        print_config_summary();
    }
}

void PipelineFeatureSet::save_to_project_settings() const {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    ps->set_setting(ENABLE_PACKED_STAGE_DATA_PATH, enable_packed_stage_data);
    ps->set_setting(ENABLE_TIGHTER_BOUNDS_PATH, enable_tighter_bounds);
    ps->set_setting(ENABLE_FAST_RASTER_PATH, enable_fast_raster);
    ps->set_setting(ENABLE_SH_AMORTIZATION_PATH, enable_sh_amortization);
    ps->set_setting(SH_AMORTIZATION_DIVISOR_PATH, sh_amortization_divisor);
    ps->set_setting(ENABLE_ALL_PIPELINE_EXPERIMENTAL_PATH, enable_all_pipeline_experimental);
    // Migrate away from the deprecated alias so a persisted config does not keep
    // re-triggering the read-time deprecation warning.
    if (ps->has_setting(DEPRECATED_ENABLE_ALL_EXPERIMENTAL_PATH)) {
        ps->clear(DEPRECATED_ENABLE_ALL_EXPERIMENTAL_PATH);
    }

    ps->save();

    GS_LOG_INFO_DEFAULT("[Pipeline Feature Set] Configuration saved to project settings");
}

void PipelineFeatureSet::reset_to_defaults() {
    enable_packed_stage_data = false;
    enable_tighter_bounds = false;
    enable_fast_raster = false;
    enable_sh_amortization = false;
    sh_amortization_divisor = 10;
    enable_all_pipeline_experimental = false;

    GS_LOG_INFO_DEFAULT("[Pipeline Feature Set] Reset to default configuration");
}

bool PipelineFeatureSet::validate(uint32_t p_total_gaussians) const {
    return get_validation_errors(p_total_gaussians).is_empty();
}

String PipelineFeatureSet::get_validation_errors(uint32_t p_total_gaussians) const {
    PackedStringArray errors;
    const bool packed_stage_requested = enable_all_pipeline_experimental || enable_packed_stage_data;
    const bool sh_amortization_requested = enable_all_pipeline_experimental || enable_sh_amortization;

    if (packed_stage_requested && p_total_gaussians > PACKED_STAGE_MAX_TOTAL_SPLATS) {
        errors.push_back(vformat(
                "Packed stage data requires <= %d total splats, got %d.",
                int(PACKED_STAGE_MAX_TOTAL_SPLATS),
                int(p_total_gaussians)));
    }

    if (sh_amortization_requested && sh_amortization_divisor <= 1) {
        errors.push_back("SH amortization divisor must be > 1.");
    }

    return String("\n").join(errors);
}

PipelineFeatureSet PipelineFeatureSet::get_effective(RenderingDevice *p_device,
        bool p_compute_raster_enabled,
        bool /*p_global_sort_enabled*/,
        String *r_warnings) const {
    PipelineFeatureSet effective = *this;
    Dictionary provenance_snapshot = loaded_provenance_snapshot.duplicate(true);

    if (enable_all_pipeline_experimental) {
        effective.enable_packed_stage_data = true;
        effective.enable_tighter_bounds = true;
        effective.enable_fast_raster = true;
        effective.enable_sh_amortization = true;
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_packed_stage_data"),
                true, "project_override", "project override");
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_tighter_bounds"),
                true, "project_override", "project override");
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_fast_raster"),
                true, "project_override", "project override");
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_sh_amortization"),
                true, "project_override", "project override");
    }

    auto warn = [&](const String &p_msg) {
        if (r_warnings) {
            *r_warnings += p_msg + "\n";
        }
    };

    if (!p_compute_raster_enabled) {
        if (effective.enable_fast_raster) {
            warn("Fast raster path requires compute raster; disabling feature.");
            effective.enable_fast_raster = false;
            GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_fast_raster"),
                    false, "runtime_requirement", "disabled by runtime requirement");
        }
    }

    if (effective.enable_sh_amortization && effective.sh_amortization_divisor <= 1) {
        warn("SH amortization requires divisor > 1; disabling feature.");
        effective.enable_sh_amortization = false;
        effective.sh_amortization_divisor = 1;
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_sh_amortization"),
                false, "invalid_setting", "disabled by invalid setting");
        GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_sh_amortization_divisor"),
                int64_t(1), "invalid_setting", "disabled by invalid setting");
    }
    if (!effective.enable_sh_amortization) {
        effective.sh_amortization_divisor = 1;
        Dictionary divisor_entry = GaussianEffectiveConfig::get_entry(provenance_snapshot, StringName("pipeline_sh_amortization_divisor"));
        if (divisor_entry.is_empty()) {
            GaussianEffectiveConfig::set_entry(provenance_snapshot, StringName("pipeline_sh_amortization_divisor"),
                    int64_t(1), "project_setting", "project setting");
        } else {
            divisor_entry[StringName("value")] = int64_t(1);
            divisor_entry[StringName("display_value")] = String("1");
            provenance_snapshot[StringName("pipeline_sh_amortization_divisor")] = divisor_entry;
        }
    }
    if (!p_device) {
        warn("No RenderingDevice available to validate pipeline feature capabilities.");
        effective_provenance_snapshot = provenance_snapshot;
        effective_provenance_snapshot_valid = true;
        return effective;
    }

    uint64_t subgroup_ops = p_device->limit_get(RenderingDevice::LIMIT_SUBGROUP_OPERATIONS);
    uint64_t subgroup_stages = p_device->limit_get(RenderingDevice::LIMIT_SUBGROUP_IN_SHADERS);
    bool has_basic = (subgroup_ops & RenderingDevice::SUBGROUP_BASIC_BIT) != 0;
    bool has_ballot = (subgroup_ops & RenderingDevice::SUBGROUP_BALLOT_BIT) != 0;
    bool has_compute = (subgroup_stages & RenderingDevice::SHADER_STAGE_COMPUTE_BIT) != 0;
    bool subgroups_available = has_basic && has_ballot && has_compute;

    if (!subgroups_available && effective.enable_fast_raster) {
        warn("Fast raster path requested but subgroup operations are unavailable; expect reduced gains.");
    }

    effective_provenance_snapshot = provenance_snapshot;
    effective_provenance_snapshot_valid = true;

    return effective;
}

Dictionary PipelineFeatureSet::get_effective_config_snapshot() const {
	if (effective_provenance_snapshot_valid) {
		return effective_provenance_snapshot.duplicate(true);
	}
	Dictionary snapshot = loaded_provenance_snapshot.duplicate(true);
	GaussianEffectiveConfig::mark_snapshot_limited(snapshot, "runtime capability validation pending");
	return snapshot;
}

void PipelineFeatureSet::print_config_summary() const {
    GS_LOG_INFO_DEFAULT("[Pipeline Feature Set] ========== Configuration Summary ==========");
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] enable_all_pipeline_experimental: %s", enable_all_pipeline_experimental ? "enabled" : "disabled"));
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] packed_stage_data: %s", enable_packed_stage_data ? "enabled" : "disabled"));
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] tighter_bounds: %s", enable_tighter_bounds ? "enabled" : "disabled"));
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] fast_raster: %s", enable_fast_raster ? "enabled" : "disabled"));
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] sh_amortization: %s", enable_sh_amortization ? "enabled" : "disabled"));
    GS_LOG_INFO_DEFAULT(vformat("[Pipeline Feature Set] sh_amortization_divisor: %d", sh_amortization_divisor));
    GS_LOG_INFO_DEFAULT("[Pipeline Feature Set] ================================================");
}

void PipelineFeatureSet::clear_provenance_snapshots() {
    loaded_provenance_snapshot.clear();
    effective_provenance_snapshot.clear();
    effective_provenance_snapshot_valid = false;
}

void initialize_pipeline_feature_set() {
    _register_pipeline_project_settings();
    g_pipeline_feature_set.load_from_project_settings();

    if (!g_pipeline_feature_set.validate()) {
        GS_LOG_WARN_DEFAULT("[Pipeline Feature Set] Invalid configuration detected:");
        GS_LOG_WARN_DEFAULT(g_pipeline_feature_set.get_validation_errors());
        GS_LOG_INFO_DEFAULT("[Pipeline Feature Set] Resetting to defaults...");
        g_pipeline_feature_set.reset_to_defaults();
        g_pipeline_feature_set.save_to_project_settings();
    }
}

void release_pipeline_feature_set_module_strings() {
    // The global feature-set Dictionary entries cache module-owned
    // StringName keys (pipeline_packed_stage_data, ..., value, source, ...)
    // that the engine's exit-time orphan StringName report would otherwise
    // flag. Dropping the snapshot Dictionaries decrements those refcounts
    // so the keys leave the StringName table cleanly at unregister.
    g_pipeline_feature_set.clear_provenance_snapshots();
}
