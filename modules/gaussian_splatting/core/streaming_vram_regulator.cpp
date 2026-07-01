#include "streaming_vram_regulator.h"
#include "gs_project_settings.h"
#include "quality_tier_config.h"
#include "streaming_tier_cap_policy.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_method_pointer.h"
#include "../logger/gs_logger.h"
#include "../renderer/gpu_debug_utils.h"

using namespace gs_tier_cap;

namespace {

static constexpr uint32_t STREAMING_DEFAULT_VRAM_BUDGET_MB = STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB;
static constexpr uint32_t STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM = 4;
static constexpr uint32_t STREAMING_DEFAULT_MAX_CHUNKS_IN_VRAM = 128;

} // anonymous namespace

// ==============================================================================
// LODBlendConfig Implementation (LODGE technique)
// ==============================================================================

LODBlendConfig LODBlendConfig::load_from_project_settings() {
    LODBlendConfig config;
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return config;
    }

    // Load blend enabled setting
    if (ps->has_setting("rendering/gaussian_splatting/lod/blend_enabled")) {
        Variant value = ps->get_setting_with_override("rendering/gaussian_splatting/lod/blend_enabled");
        if (value.get_type() == Variant::BOOL) {
            config.blend_enabled = value.operator bool();
        }
    }

    // Load blend distance setting
    if (ps->has_setting("rendering/gaussian_splatting/lod/blend_distance")) {
        Variant value = ps->get_setting_with_override("rendering/gaussian_splatting/lod/blend_distance");
        if (value.get_type() == Variant::FLOAT) {
            config.blend_distance = MAX(0.1f, (float)value.operator double());
        } else if (value.get_type() == Variant::INT) {
            config.blend_distance = MAX(0.1f, (float)value.operator int64_t());
        }
    }

    // Load hysteresis zone setting
    if (ps->has_setting("rendering/gaussian_splatting/lod/hysteresis_zone")) {
        Variant value = ps->get_setting_with_override("rendering/gaussian_splatting/lod/hysteresis_zone");
        if (value.get_type() == Variant::FLOAT) {
            config.hysteresis_zone = MAX(0.0f, (float)value.operator double());
        } else if (value.get_type() == Variant::INT) {
            config.hysteresis_zone = MAX(0.0f, (float)value.operator int64_t());
        }
    }

    return config;
}

// ==============================================================================
// VRAMBudgetConfig Implementation
// ==============================================================================

VRAMBudgetConfig VRAMBudgetConfig::load_from_project_settings() {
    VRAMBudgetConfig config;
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return config;
    }

    const StreamingTierCapPolicy tier_policy = _resolve_streaming_tier_cap_policy(ps);
    config.cap_tier_preset = tier_policy.tier_preset;
    config.cap_tier_active = tier_policy.active;

    config.budget_mb = _resolve_tiered_cap_uint(ps,
            "rendering/gaussian_splatting/streaming/vram_budget_mb",
            STREAMING_DEFAULT_VRAM_BUDGET_MB,
            tier_policy.active,
            tier_policy.vram_budget_mb,
            config.source_budget_mb);
    config.auto_regulate_enabled = gs::settings::get_bool(ps, "rendering/gaussian_splatting/streaming/auto_regulate_enabled", config.auto_regulate_enabled);
    config.warning_threshold_percent = gs::settings::get_uint(ps, "rendering/gaussian_splatting/streaming/vram_warning_threshold_percent", config.warning_threshold_percent);
    config.min_chunks = _resolve_tiered_cap_uint(ps,
            "rendering/gaussian_splatting/streaming/min_chunks_in_vram",
            STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM,
            tier_policy.active,
            tier_policy.min_chunks_in_vram,
            config.source_min_chunks);
    config.max_chunks = _resolve_tiered_cap_uint(ps,
            "rendering/gaussian_splatting/streaming/max_chunks_in_vram",
            STREAMING_DEFAULT_MAX_CHUNKS_IN_VRAM,
            tier_policy.active,
            tier_policy.max_chunks_in_vram,
            config.source_max_chunks);
    config.regulation_step_percent = gs::settings::get_float(ps, "rendering/gaussian_splatting/streaming/regulation_step_percent", config.regulation_step_percent);

    // Sanitize values
    config.min_chunks = MAX(1u, config.min_chunks);
    config.max_chunks = MAX(config.min_chunks, config.max_chunks);
    config.warning_threshold_percent = CLAMP(config.warning_threshold_percent, 50u, 99u);
    config.regulation_step_percent = CLAMP(config.regulation_step_percent, 0.1f, 10.0f);

    return config;
}

// ==============================================================================
// VRAMBudgetRegulator Implementation
// ==============================================================================

VRAMBudgetRegulator::VRAMBudgetRegulator() {
    reload_config();
}

void VRAMBudgetRegulator::_bind_methods() {
    ClassDB::bind_method(D_METHOD("initialize", "rendering_device"), &VRAMBudgetRegulator::initialize);
    ClassDB::bind_method(D_METHOD("reload_config"), &VRAMBudgetRegulator::reload_config);
    ClassDB::bind_method(D_METHOD("get_current_max_chunks"), &VRAMBudgetRegulator::get_current_max_chunks);
    ClassDB::bind_method(D_METHOD("get_lod_distance_multiplier"), &VRAMBudgetRegulator::get_lod_distance_multiplier);
    ClassDB::bind_method(D_METHOD("can_load_more_chunks", "current_loaded"), &VRAMBudgetRegulator::can_load_more_chunks);
    ClassDB::bind_method(D_METHOD("should_trigger_eviction", "current_usage"), &VRAMBudgetRegulator::should_trigger_eviction);
    ClassDB::bind_method(D_METHOD("get_debug_stats_dictionary"), &VRAMBudgetRegulator::get_debug_stats_dictionary);
    ClassDB::bind_method(D_METHOD("is_budget_warning_active"), &VRAMBudgetRegulator::is_budget_warning_active);
}

void VRAMBudgetRegulator::initialize(RenderingDevice *p_rd) {
    rd = p_rd;
    _query_device_memory();  // Query device first so device memory info is available when applying config
    if (config_override_active) {
        _apply_config();
    } else {
        reload_config();
    }

    GS_LOG_STREAMING_INFO(vformat("[VRAM Regulator] Initialized with budget: %d MB, auto-regulate: %s, max chunks: %d",
            config.budget_mb, config.auto_regulate_enabled ? "enabled" : "disabled", current_max_chunks));
}

void VRAMBudgetRegulator::_capture_requested_budget() {
    // Snapshot the caller's original ask BEFORE any clamp/fallback mutates
    // config.budget_mb. _apply_config() rewrites budget_mb in place, so it can no
    // longer recover the pre-clamp request on a second pass — initialize() applies
    // an already-mutated config a second time. Capturing the request here, at the
    // genuine config-establishment points, keeps requested_budget_mb stable across
    // those re-applications (Codex #411).
    config.requested_budget_mb = config.budget_mb;
    config.requested_source_budget_mb = config.source_budget_mb;
}

void VRAMBudgetRegulator::_apply_config() {
    config.min_chunks = MAX(1u, config.min_chunks);
    config.max_chunks = MAX(config.min_chunks, config.max_chunks);
    config.warning_threshold_percent = CLAMP(config.warning_threshold_percent, 50u, 99u);
    config.regulation_step_percent = CLAMP(config.regulation_step_percent, 0.1f, 10.0f);
    // requested_budget_mb / requested_source_budget_mb are captured by the
    // config-establishment entry points (reload_config / set_config_override)
    // before the clamp below runs; do NOT overwrite them here or a second
    // _apply_config() pass would replace the original ask with the clamped value.
    config.budget_capacity_verified = stats.device_capacity_known;
    config.budget_uses_unknown_capacity_fallback = false;
    config.budget_unverified = false;

    // RenderingDevice::MEMORY_TOTAL reports current total memory usage, not hardware capacity.
    // Clamp configured budget only when true capacity is known from a dedicated query path.
    if (stats.device_capacity_known && stats.device_capacity_bytes > 0) {
        const uint32_t capacity_mb = uint32_t(stats.device_capacity_bytes / (1024u * 1024u));
        if (capacity_mb > 0 && config.budget_mb > capacity_mb) {
            WARN_PRINT(vformat("[VRAM Budget] Clamping configured budget from %d MB to detected device capacity %d MB.",
                    config.budget_mb, capacity_mb));
            config.budget_mb = capacity_mb;
            config.source_budget_mb = "detected_capacity_clamp";
        }
    } else if (config.source_budget_mb == "project_default") {
        config.budget_mb = MIN(config.budget_mb, STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB);
        config.source_budget_mb = "unknown_capacity_fallback";
        config.budget_uses_unknown_capacity_fallback = true;
    } else {
        config.budget_unverified = true;
        if (config.budget_mb > STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB) {
            const String warning_key = vformat("%s:%d", config.source_budget_mb, config.budget_mb);
            if (warning_key != last_unverified_budget_warning_key) {
                WARN_PRINT(vformat("[VRAM Budget] Using unverified %d MB budget from %s while device capacity is unknown; "
                                   "fresh projects default to %d MB until true capacity can be queried.",
                        config.budget_mb,
                        config.source_budget_mb,
                        STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB));
                last_unverified_budget_warning_key = warning_key;
            }
        }
    }

    // Initialize current_max_chunks to configured max (will be regulated down if needed)
    current_max_chunks = config.max_chunks;

    // Update budget in stats
    stats.budget_bytes = uint64_t(config.budget_mb) * 1024 * 1024;
    stats.current_max_chunks = current_max_chunks;
}

void VRAMBudgetRegulator::reload_config() {
    if (config_override_active) {
        return;
    }
    config = VRAMBudgetConfig::load_from_project_settings();
    // Re-derive device capacity from the freshly loaded config. clear_config_override()
    // reaches here after dropping an override that may have carried a trusted capacity;
    // without a re-query, stats.device_capacity_* would stay populated from the cleared
    // override and a now-unknown-capacity project would be falsely capacity-verified or
    // clamped to the stale capacity instead of following the unknown-capacity path
    // (Codex #411).
    _query_device_memory();
    _capture_requested_budget();
    _apply_config();
}

void VRAMBudgetRegulator::set_config_override(const VRAMBudgetConfig &p_config) {
    config_override_active = true;
    config = p_config;
    if (config.source_budget_mb.is_empty() || config.source_budget_mb == "project_default") {
        config.source_budget_mb = "runtime_override";
    }
    if (config.source_min_chunks.is_empty() || config.source_min_chunks == "project_default") {
        config.source_min_chunks = "runtime_override";
    }
    if (config.source_max_chunks.is_empty() || config.source_max_chunks == "project_default") {
        config.source_max_chunks = "runtime_override";
    }
    // Pick up any trusted capacity carried on the override before applying.
    _query_device_memory();
    _capture_requested_budget();
    _apply_config();
}

void VRAMBudgetRegulator::clear_config_override() {
    if (!config_override_active) {
        return;
    }
    config_override_active = false;
    reload_config();
}

void VRAMBudgetRegulator::_query_device_memory() {
    // A trusted device VRAM capacity may be supplied out-of-band (config override
    // or an explicit project pin resolved in load_from_project_settings()). When
    // present it is authoritative and must survive a re-query — RenderingDevice
    // exposes no portable capacity API, so the only way the capacity-known path
    // (and its precise budget clamp) becomes reachable is to honor that value
    // instead of unconditionally forcing "unknown".
    const bool config_capacity_known = config.device_capacity_known && config.device_capacity_bytes > 0;

    if (!rd) {
        stats.device_reported_usage_bytes = 0;
        stats.device_available_bytes = 0;
        stats.device_memory_queryable = false;
        stats.device_capacity_bytes = config_capacity_known ? config.device_capacity_bytes : 0;
        stats.device_capacity_known = config_capacity_known;
        return;
    }

    // RenderingDevice::MEMORY_TOTAL is current usage, not total capacity.
    stats.device_reported_usage_bytes = rd->get_memory_usage(RenderingDevice::MEMORY_TOTAL);
    stats.device_memory_queryable = (stats.device_reported_usage_bytes > 0);

    // Capacity precedence: a trusted out-of-band value (config override / explicit
    // project pin) is authoritative; otherwise query the real device-local VRAM budget
    // now that RenderingDevice exposes it (GS #321; Vulkan-backed, other backends report
    // 0 and fall through to the conservative capacity-unknown path).
    if (config_capacity_known) {
        stats.device_capacity_bytes = config.device_capacity_bytes;
        stats.device_capacity_known = true;
    } else {
        // Static device-local VRAM capacity from the driver (GS #321). Only trust it as
        // "known" above a sane floor: a degenerate/garbage small-positive value would
        // otherwise clamp the budget to nonsense AND mark it capacity-verified, which is
        // strictly worse than the conservative unknown-capacity fallback. The floor also
        // closes the capacity_mb == 0 (< 1 MiB) truncation hole in the _apply_config clamp.
        constexpr uint64_t MIN_TRUSTED_DEVICE_CAPACITY_BYTES = uint64_t(256) * 1024 * 1024;
        const uint64_t queried_capacity_bytes = rd->get_device_memory_budget();
        const bool capacity_plausible = queried_capacity_bytes >= MIN_TRUSTED_DEVICE_CAPACITY_BYTES;
        stats.device_capacity_bytes = capacity_plausible ? queried_capacity_bytes : 0;
        stats.device_capacity_known = capacity_plausible;
    }
    stats.device_available_bytes = 0;

    if (GaussianSplatting::is_debug_frame_logging_enabled()) {
        if (stats.device_capacity_known) {
            GS_LOG_STREAMING_INFO(vformat("[VRAM Regulator] Device capacity known: %d MB (reported usage %d MB)",
                    stats.device_capacity_bytes / (1024 * 1024),
                    stats.device_reported_usage_bytes / (1024 * 1024)));
        } else if (stats.device_memory_queryable) {
            GS_LOG_STREAMING_INFO(vformat("[VRAM Regulator] Device usage queryable: %d MB reported usage (capacity unknown)",
                    stats.device_reported_usage_bytes / (1024 * 1024)));
        } else {
            GS_LOG_STREAMING_INFO("[VRAM Regulator] Device usage query unavailable; capacity unknown");
        }
    }
}

void VRAMBudgetRegulator::update(uint64_t p_reported_usage, uint64_t p_decision_usage, uint32_t loaded_chunks,
                                  uint32_t loads_this_frame, uint32_t evictions_this_frame,
                                  uint64_t current_frame) {
    // p_reported_usage is the allocation-inclusive footprint (persistent buffer + payload +
    // auxiliary overhead) — it mirrors get_vram_usage()/end-frame vram_mb and is what the debug
    // overlay and the budget warning must show. p_decision_usage is the reclaimable/evictable
    // subset that admission and auto-regulation act on; gating those on the non-reclaimable
    // allocation would deny loads or ratchet the chunk cap down while no VRAM can be freed.
    // Keep the two separate so reporting is never under-counted by the decision basis. (Codex #411)
    stats.current_usage_bytes = p_reported_usage;
    stats.loaded_chunks = loaded_chunks;
    stats.loaded_this_frame = loads_this_frame;
    stats.evicted_this_frame = evictions_this_frame;
    stats.current_max_chunks = current_max_chunks;

    decision_usage_bytes = p_decision_usage;
    if (stats.budget_bytes > 0) {
        stats.usage_percent = (float(p_reported_usage) / float(stats.budget_bytes)) * 100.0f;
        decision_usage_percent = (float(p_decision_usage) / float(stats.budget_bytes)) * 100.0f;
    } else {
        stats.usage_percent = 0.0f;
        decision_usage_percent = 0.0f;
    }

    // Warn on the true footprint approaching budget (reported, not the reclaimable subset).
    bool was_warning = stats.budget_warning_active;
    stats.budget_warning_active = (stats.usage_percent >= config.warning_threshold_percent);

    // Log warning transition
    if (stats.budget_warning_active && !was_warning) {
        WARN_PRINT(vformat("[VRAM Budget] Approaching VRAM limit: %.1f%% of %d MB budget used (%d MB)",
                stats.usage_percent, config.budget_mb,
                p_reported_usage / (1024 * 1024)));
    }

    // Record activity for thrashing detection
    _record_activity(loads_this_frame, evictions_this_frame);

    // Auto-regulation chases the budget by raising/lowering the chunk cap; reducing the cap only
    // frees payload, so it must act on the reclaimable decision usage, not the full allocation.
    if (config.auto_regulate_enabled) {
        _update_regulation(p_decision_usage, current_frame);
    }
}

void VRAMBudgetRegulator::_update_regulation(uint64_t current_usage, uint64_t current_frame) {
    // Don't adjust too frequently (prevent oscillation)
    if (current_frame - last_adjustment_frame < MIN_FRAMES_BETWEEN_ADJUSTMENTS) {
        return;
    }

    // Regulate against the reclaimable basis the caller passes (decision usage), NOT
    // stats.usage_percent which now reflects the full allocation-inclusive footprint:
    // lowering the chunk cap frees only payload, never the persistent buffer. (Codex #411)
    const float decision_percent = stats.budget_bytes > 0
            ? (float(current_usage) / float(stats.budget_bytes)) * 100.0f
            : 0.0f;

    // Check for thrashing - if detected, reduce chunks more aggressively
    if (_detect_thrashing()) {
        stats.thrashing_events++;
        uint32_t reduction = MAX(1u, current_max_chunks / 10); // 10% reduction on thrashing
        uint32_t new_max = MAX(config.min_chunks, current_max_chunks - reduction);
        if (new_max != current_max_chunks) {
            current_max_chunks = new_max;
            last_adjustment_frame = current_frame;
            stats.regulation_adjustments++;

            // Also increase LOD distance to reduce quality demands
            lod_distance_multiplier = MIN(2.0f, lod_distance_multiplier * 1.1f);

            GS_LOG_STREAMING_INFO(vformat("[VRAM Regulator] Thrashing detected! Reduced max chunks to %d, LOD multiplier: %.2f",
                    current_max_chunks, lod_distance_multiplier));
        }
        return;
    }

    // Calculate step size based on config
    uint32_t step_size = MAX(1u, uint32_t(float(config.max_chunks) * config.regulation_step_percent / 100.0f));

    // If over budget, reduce chunks
    if (decision_percent > float(config.warning_threshold_percent)) {
        uint32_t new_max = MAX(config.min_chunks, current_max_chunks - step_size);
        if (new_max != current_max_chunks) {
            current_max_chunks = new_max;
            last_adjustment_frame = current_frame;
            stats.regulation_adjustments++;

            GS_LOG_STREAMING_DEBUG(vformat("[VRAM Regulator] Reducing max chunks: %d -> %d (usage: %.1f%%)",
                    current_max_chunks + step_size, current_max_chunks, decision_percent));
        }
    }
    // If well under budget and not at max, increase chunks
    else if (decision_percent < float(config.warning_threshold_percent) * 0.7f) {
        if (current_max_chunks < config.max_chunks) {
            uint32_t new_max = MIN(config.max_chunks, current_max_chunks + step_size);
            if (new_max != current_max_chunks) {
                current_max_chunks = new_max;
                last_adjustment_frame = current_frame;
                stats.regulation_adjustments++;

                // Also recover LOD quality if we have headroom
                if (lod_distance_multiplier > 1.0f) {
                    lod_distance_multiplier = MAX(1.0f, lod_distance_multiplier * 0.95f);
                }

                GS_LOG_STREAMING_DEBUG(vformat("[VRAM Regulator] Increasing max chunks: %d -> %d (usage: %.1f%%)",
                        current_max_chunks - step_size, current_max_chunks, decision_percent));
            }
        }
    }

    stats.current_max_chunks = current_max_chunks;
}

bool VRAMBudgetRegulator::_detect_thrashing() const {
    // Thrashing = high load AND eviction activity simultaneously
    // Check if both loads and evictions are consistently high in recent history
    uint32_t high_activity_frames = 0;
    for (uint32_t i = 0; i < THRASHING_HISTORY_SIZE; i++) {
        // Both loading and evicting in same frame = potential thrashing
        if (load_history[i] > 0 && evict_history[i] > 0) {
            high_activity_frames++;
        }
    }

    // If more than half the recent frames show load+evict, we're thrashing
    return high_activity_frames > THRASHING_HISTORY_SIZE / 2;
}

void VRAMBudgetRegulator::_record_activity(uint32_t loads, uint32_t evictions) {
    history_index = (history_index + 1) % THRASHING_HISTORY_SIZE;
    load_history[history_index] = loads;
    evict_history[history_index] = evictions;
}

bool VRAMBudgetRegulator::can_load_more_chunks(uint32_t current_loaded) const {
    // Never exceed dynamic max
    if (current_loaded >= current_max_chunks) {
        return false;
    }

    // If approaching budget, be more conservative. Gate on the reclaimable decision usage,
    // not the reported allocation-inclusive percentage: loading a chunk into an existing slot
    // grows payload, not the persistent-buffer allocation, so the non-reclaimable allocation
    // must not block admission. Buffer growth has its own allocation-inclusive gate. (Codex #411)
    if (decision_usage_percent >= float(config.warning_threshold_percent)) {
        // Only allow loading if we have significant headroom
        return current_loaded < current_max_chunks / 2;
    }

    return true;
}

bool VRAMBudgetRegulator::should_trigger_eviction(uint64_t current_usage) const {
    // Trigger early eviction if we're approaching the warning threshold
    float early_threshold = float(config.warning_threshold_percent) * 0.9f;
    float current_percent = stats.budget_bytes > 0
            ? (float(current_usage) / float(stats.budget_bytes)) * 100.0f
            : 0.0f;

    return current_percent >= early_threshold;
}

Dictionary VRAMBudgetRegulator::get_debug_stats_dictionary() const {
    Dictionary d;
    d["current_usage_bytes"] = stats.current_usage_bytes;
    d["current_usage_mb"] = float(stats.current_usage_bytes) / (1024.0f * 1024.0f);
    d["budget_bytes"] = stats.budget_bytes;
    d["budget_mb"] = float(stats.budget_bytes) / (1024.0f * 1024.0f);
    d["device_reported_usage_bytes"] = stats.device_reported_usage_bytes;
    d["device_reported_usage_mb"] = float(stats.device_reported_usage_bytes) / (1024.0f * 1024.0f);
    d["device_capacity_bytes"] = stats.device_capacity_bytes;
    d["device_capacity_mb"] = float(stats.device_capacity_bytes) / (1024.0f * 1024.0f);
    d["device_capacity_known"] = stats.device_capacity_known;
    // Backward-compatible aliases retained for existing tooling.
    d["device_total_bytes"] = stats.device_reported_usage_bytes;
    d["device_total_mb"] = float(stats.device_reported_usage_bytes) / (1024.0f * 1024.0f);
    d["device_total_known"] = stats.device_capacity_known;
    d["device_reported_bytes"] = stats.device_reported_usage_bytes;
    d["device_total_semantics"] = "reported_usage";
    d["current_max_chunks"] = stats.current_max_chunks;
    d["loaded_chunks"] = stats.loaded_chunks;
    d["evicted_this_frame"] = stats.evicted_this_frame;
    d["loaded_this_frame"] = stats.loaded_this_frame;
    d["usage_percent"] = stats.usage_percent;
    d["budget_warning_active"] = stats.budget_warning_active;
    d["device_memory_queryable"] = stats.device_memory_queryable;
    d["regulation_adjustments"] = stats.regulation_adjustments;
    d["thrashing_events"] = stats.thrashing_events;
    d["lod_distance_multiplier"] = lod_distance_multiplier;
    d["auto_regulate_enabled"] = config.auto_regulate_enabled;
    d["cap_tier_preset"] = config.cap_tier_preset;
    d["cap_tier_active"] = config.cap_tier_active;
    d["requested_budget_mb"] = int64_t(config.requested_budget_mb);
    d["requested_source_budget_mb"] = config.requested_source_budget_mb;
    d["source_budget_mb"] = config.source_budget_mb;
    d["source_min_chunks"] = config.source_min_chunks;
    d["source_max_chunks"] = config.source_max_chunks;
    d["budget_capacity_verified"] = config.budget_capacity_verified;
    d["budget_uses_unknown_capacity_fallback"] = config.budget_uses_unknown_capacity_fallback;
    d["budget_unverified"] = config.budget_unverified;
    return d;
}
