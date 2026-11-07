#ifndef STREAMING_VRAM_REGULATOR_H
#define STREAMING_VRAM_REGULATOR_H

#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"
#include <cstdint>

// LOD blending configuration for smooth LOD transitions (LODGE technique)
struct LODBlendConfig {
    bool blend_enabled = true;             // Enable LOD boundary blending
    float blend_distance = 5.0f;           // Transition zone width in world units
    float hysteresis_zone = 0.5f;          // Hysteresis zone to prevent rapid LOD switching

    static LODBlendConfig load_from_project_settings();
};

// Conservative budget used when the renderer cannot query true device VRAM.
// A fresh project must not assume a 12 GB desktop GPU; explicit project,
// runtime, or quality-tier overrides may still opt into larger budgets.
static constexpr uint32_t STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB = 2048;

// VRAM budget configuration loaded from project settings
struct VRAMBudgetConfig {
    uint32_t budget_mb = STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB; // Effective VRAM budget in MB
    uint32_t requested_budget_mb = STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB; // Pre-clamp/override budget in MB
    bool auto_regulate_enabled = true;     // Enable automatic chunk limit adjustment
    uint32_t warning_threshold_percent = 85; // Warn when usage exceeds this percentage
    uint32_t min_chunks = 4;               // Minimum chunks to keep loaded
    uint32_t max_chunks = 64;              // Maximum chunks allowed
    float regulation_step_percent = 1.0f;  // Step size for adjustments (1% recommended)
    String cap_tier_preset = "custom";
    bool cap_tier_active = false;
    String source_budget_mb = "project_default";
    String requested_source_budget_mb = "project_default";
    String source_min_chunks = "project_default";
    String source_max_chunks = "project_default";
    bool budget_capacity_verified = false;
    bool budget_uses_unknown_capacity_fallback = false;
    bool budget_unverified = false;

    // Real device VRAM capacity, when the caller can supply it from a trusted
    // source (e.g. a platform query or an explicit project pin). 0 / false means
    // capacity is genuinely unknown and the conservative fallback path applies.
    uint64_t device_capacity_bytes = 0;
    bool device_capacity_known = false;

    static VRAMBudgetConfig load_from_project_settings();
};

// Debug statistics for VRAM usage monitoring
struct VRAMDebugStats {
    uint64_t current_usage_bytes = 0;      // Current tracked VRAM usage
    uint64_t budget_bytes = 0;             // Total budget in bytes
    uint64_t device_reported_usage_bytes = 0; // RenderingDevice::MEMORY_TOTAL (current usage, not capacity)
    uint64_t device_capacity_bytes = 0;    // True device VRAM capacity when available
    uint64_t device_available_bytes = 0;   // Available device VRAM (if queryable)
    uint32_t current_max_chunks = 0;       // Current dynamic chunk limit
    uint32_t loaded_chunks = 0;            // Currently loaded chunks
    uint32_t evicted_this_frame = 0;       // Chunks evicted this frame
    uint32_t loaded_this_frame = 0;        // Chunks loaded this frame
    float usage_percent = 0.0f;            // Current usage as percentage of budget
    bool budget_warning_active = false;    // True if approaching budget limit
    bool device_memory_queryable = false;  // True if usage reporting is available
    bool device_capacity_known = false;    // True when true VRAM capacity is known
    uint32_t regulation_adjustments = 0;   // Total adjustments made this session
    uint32_t thrashing_events = 0;         // Detected thrashing occurrences
    // BV-04a (ADR adr-beyond-vram-worlds.md D-6): regulator events by kind, so a probe can tell
    // whether and why the regulator fired. regulation_adjustments is their sum.
    uint32_t thrash_reductions = 0;        // Thrash ratchet: chunk cap cut by 10 %
    uint32_t budget_reductions = 0;        // Over the warning threshold: chunk cap stepped down
    uint32_t headroom_increases = 0;       // Well under the threshold: chunk cap stepped up
    uint32_t lod_multiplier_adjustments = 0; // Times lod_distance_multiplier changed value
    uint64_t last_regulation_frame = 0;    // Frame of the last adjustment (valid when regulation_adjustments > 0)
    const char *last_regulation_reason = "none"; // none, thrash_reduction, budget_reduction, headroom_increase
};

// VRAM budget auto-regulator for graceful degradation (H3DGS-style)
class VRAMBudgetRegulator : public RefCounted {
    GDCLASS(VRAMBudgetRegulator, RefCounted);

private:
    VRAMBudgetConfig config;
    VRAMDebugStats stats;
    bool config_override_active = false;
    String last_unverified_budget_warning_key;

    // Dynamic chunk limit (adjusted based on VRAM pressure)
    uint32_t current_max_chunks = 32;

    // Decision-basis usage (reclaimable/evictable), kept distinct from the reported
    // allocation-inclusive usage published in `stats`. Eviction, admission, and
    // auto-regulation act on what they can actually free; the overlay + budget warning
    // report the true footprint. Gating decisions on the non-reclaimable persistent-buffer
    // allocation would ratchet limits down or deny loads while no VRAM can be freed, and
    // publishing the decision basis as the reported usage would under-count the overlay. (Codex #411)
    uint64_t decision_usage_bytes = 0;
    float decision_usage_percent = 0.0f;

    // Thrashing prevention state
    static constexpr uint32_t THRASHING_HISTORY_SIZE = 16;
    uint32_t load_history[THRASHING_HISTORY_SIZE] = {};
    uint32_t evict_history[THRASHING_HISTORY_SIZE] = {};
    uint32_t history_index = 0;
    uint64_t last_adjustment_frame = 0;
    static constexpr uint32_t MIN_FRAMES_BETWEEN_ADJUSTMENTS = 30;

    // LOD distance multiplier for quality degradation
    float lod_distance_multiplier = 1.0f;

    // Rendering device reference for memory queries
    RenderingDevice *rd = nullptr;

    // Internal methods
    void _query_device_memory();
    void _update_regulation(uint64_t current_usage, uint64_t current_frame);
    bool _detect_thrashing() const;
    void _record_activity(uint32_t loads, uint32_t evictions);
    void _capture_requested_budget();
    void _apply_config();

protected:
    static void _bind_methods();

public:
    VRAMBudgetRegulator();
    ~VRAMBudgetRegulator() = default;

    // Initialize with rendering device and config
    void initialize(RenderingDevice *p_rd);
    void reload_config();
    void set_config_override(const VRAMBudgetConfig &p_config);
    void clear_config_override();

    // Called each frame with current usage stats. p_reported_usage is the
    // allocation-inclusive footprint (overlay + warning); p_decision_usage is the
    // reclaimable/evictable subset that admission + auto-regulation act on. (Codex #411)
    void update(uint64_t p_reported_usage, uint64_t p_decision_usage, uint32_t loaded_chunks,
                uint32_t loads_this_frame, uint32_t evictions_this_frame,
                uint64_t current_frame);

    // Get current regulated limits
    uint32_t get_current_max_chunks() const { return current_max_chunks; }
    float get_lod_distance_multiplier() const { return lod_distance_multiplier; }

    // Check if we should allow loading more chunks
    bool can_load_more_chunks(uint32_t current_loaded) const;

    // Check if we need to trigger early eviction
    bool should_trigger_eviction(uint64_t current_usage) const;

    // Get debug statistics
    VRAMDebugStats get_debug_stats() const { return stats; }
    Dictionary get_debug_stats_dictionary() const;

    // Check if budget warning is active
    bool is_budget_warning_active() const { return stats.budget_warning_active; }
    bool is_config_override_active() const { return config_override_active; }

    // Get config for external use
    const VRAMBudgetConfig &get_config() const { return config; }
};

#endif // STREAMING_VRAM_REGULATOR_H
