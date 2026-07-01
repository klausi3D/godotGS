/**
 * @file streaming_tier_cap_policy.h
 * @brief Shared resolution of streaming tier-cap policy from ProjectSettings.
 *
 * This header centralizes the StreamingTierCapPolicy struct and the three
 * resolver helpers that were previously duplicated, near-identically, in
 * streaming_upload_pipeline.cpp and streaming_vram_regulator.cpp. All
 * ProjectSettings reads route through the canonical gs::settings accessors.
 */

#ifndef GS_STREAMING_TIER_CAP_POLICY_H
#define GS_STREAMING_TIER_CAP_POLICY_H

#include "gs_project_settings.h"
#include "quality_tier_config.h"
#include "core/config/project_settings.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

#include <cstdint>

namespace gs_tier_cap {

struct StreamingTierCapPolicy {
    String tier_preset = "custom";
    bool active = false;
    uint32_t upload_mb_per_frame = 0;
    uint32_t upload_mb_per_slice = 0;
    uint32_t upload_mb_per_second = 0;
    uint32_t vram_budget_mb = 0;
    uint32_t min_chunks_in_vram = 0;
    uint32_t max_chunks_in_vram = 0;
};

inline bool _project_setting_has_override(ProjectSettings *ps, const StringName &name) {
    if (!ps || !ps->has_setting(name) || !ps->property_can_revert(name)) {
        return false;
    }
    return ps->get_setting_with_override(name) != ps->property_get_revert(name);
}

inline uint32_t _resolve_tiered_cap_uint(ProjectSettings *ps, const StringName &name, uint32_t fallback,
        bool tier_active, uint32_t tier_value, String &r_source) {
    const uint32_t configured_value = gs::settings::get_uint(ps, name, fallback);
    const bool has_project_override = _project_setting_has_override(ps, name);
    if (tier_active && !has_project_override) {
        r_source = "tier_preset";
        return tier_value;
    }
    r_source = has_project_override ? "project_override" : "project_default";
    return configured_value;
}

inline StreamingTierCapPolicy _resolve_streaming_tier_cap_policy(ProjectSettings *ps) {
    StreamingTierCapPolicy policy;
    if (!ps) {
        return policy;
    }

    const StringName tier_preset_setting = "rendering/gaussian_splatting/quality/tier_preset";
    const Variant tier_preset_value = ps->has_setting(tier_preset_setting)
            ? ps->get_setting_with_override(tier_preset_setting)
            : Variant("custom");
    policy.tier_preset = String(tier_preset_value)
                                 .strip_edges()
                                 .to_lower();
    const bool apply_tier_budgets =
            gs::settings::get_bool(ps, "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", true);
    if (!apply_tier_budgets) {
        return policy;
    }

    QualityTierConfig tier_config;
    if (!get_quality_tier_config(policy.tier_preset, tier_config)) {
        return policy;
    }

    policy.active = true;
    policy.upload_mb_per_frame = tier_config.streaming_upload_mb_per_frame;
    policy.upload_mb_per_slice = tier_config.streaming_upload_mb_per_slice;
    policy.upload_mb_per_second = tier_config.streaming_upload_mb_per_second;
    policy.vram_budget_mb = tier_config.streaming_vram_budget_mb;
    policy.min_chunks_in_vram = tier_config.streaming_min_chunks_in_vram;
    policy.max_chunks_in_vram = tier_config.streaming_max_chunks_in_vram;
    return policy;
}

} // namespace gs_tier_cap

#endif // GS_STREAMING_TIER_CAP_POLICY_H
