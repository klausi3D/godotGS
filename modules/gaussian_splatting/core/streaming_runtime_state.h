#ifndef STREAMING_RUNTIME_STATE_H
#define STREAMING_RUNTIME_STATE_H

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"
#include "core/variant/dictionary.h"
#include "streaming_asset_types.h"
#include "streaming_vram_regulator.h"
#include <cstdint>

namespace GaussianStreamingTypes {

struct FrameData {
    LocalVector<uint32_t> visible_chunks;
    uint64_t frame_number = 0;
};

struct BudgetState {
    Ref<VRAMBudgetRegulator> vram_regulator;
    uint32_t loaded_chunks_count = 0;
    uint64_t vram_usage = 0;
    uint64_t evicted_bytes_total = 0;
    uint32_t chunks_loaded_this_frame = 0;
    // BV-08a: visible chunks whose upload began within the regulator's thrash window of their eviction.
    uint32_t visible_refetches_this_frame = 0;
    bool vram_chunk_cap_hit_this_frame = false;
    uint64_t pending_upload_bytes = 0;
    uint32_t pending_upload_slots = 0;
    uint64_t retired_upload_bytes_this_frame = 0;
    uint32_t retired_upload_slots_this_frame = 0;
    uint64_t failed_upload_retirements = 0;
    // #757 (extends #513): count of pending upload retirements dropped fail-closed because the
    // EFFECTIVE atlas byte stride flipped (per-chunk quantization DC-compatibility toggled by a
    // mixed-DC asset registration) between the chunk being packed/staged and its frame-delayed
    // retirement. #513's resident-chunk eviction cannot cover these (they are upload_pending,
    // is_loaded == false), so completing them would mark an old-stride payload resident and
    // read/account it at the new stride -> GPU corruption. Cumulative (never per-frame reset) so
    // the degraded path is explicit and observable, matching the renderer's timing-honesty rule.
    uint64_t stride_flip_dropped_upload_retirements = 0;
    // #766: async pack jobs dropped fail-closed in process_upload_queue() because the effective
    // atlas stride flipped between pack time and the buffer write. Distinct from the retirement
    // drop above: this one fires BEFORE the write, so no neighbor slot is corrupted. Cumulative,
    // like the retirement counter, so the pre-write degraded path is explicit and observable.
    uint64_t stride_flip_dropped_prewrite_uploads = 0;
    // #1177: completed uploads dropped at pack completion (process_upload_queue) and retirement
    // tickets dropped at retirement because their upload_sequence no longer matched the chunk's:
    // the chunk was cancelled while a worker held the job and has been re-queued since.
    // Cumulative, and kept apart so each guard stays individually observable.
    uint64_t stale_sequence_dropped_uploads = 0;
    uint64_t stale_sequence_dropped_upload_retirements = 0;
    uint64_t stale_generation_publications = 0; // Successful retirement boundary, not rejected/cancelled work.

    Dictionary get_vram_debug_stats() const;
    bool is_vram_budget_warning_active() const;
    uint32_t get_effective_max_chunks() const;
};

struct SchedulerState {
    static constexpr uint32_t DEFAULT_PREFETCH_LOADS_PER_FRAME = 6;
    static constexpr uint32_t MAX_PREFETCH_LOADS_PER_FRAME = 64;
    static constexpr uint32_t DEFAULT_SYNC_FALLBACK_LOADS_PER_FRAME = 1;
    static constexpr uint32_t MAX_SYNC_FALLBACK_LOADS_PER_FRAME = 8;
    static constexpr uint32_t DEFAULT_SYNC_FALLBACK_QUEUE_SIZE = 2048;
    static constexpr uint32_t SYNC_FALLBACK_QUEUE_COMPACT_MIN_PREFIX = 128;
    static constexpr bool DEFAULT_QUEUE_PRESSURE_CANDIDATE_SCAN_THROTTLE_ENABLED = false;
    static constexpr uint32_t DEFAULT_QUEUE_PRESSURE_CANDIDATE_SCAN_THROTTLE_MIN_QUEUE_DEPTH = 1;
    static constexpr uint32_t DEFAULT_QUEUE_PRESSURE_VISIBLE_SCAN_BUDGET = 1024;
    static constexpr uint32_t DEFAULT_QUEUE_PRESSURE_PREFETCH_SCAN_BUDGET = 1024;
    uint32_t max_visible_chunk_scan_per_frame = 4096;
    uint32_t max_prefetch_chunk_scan_per_frame = 4096;
    uint32_t max_prefetch_loads_per_frame = DEFAULT_PREFETCH_LOADS_PER_FRAME;
    uint32_t max_sync_fallback_loads_per_frame = DEFAULT_SYNC_FALLBACK_LOADS_PER_FRAME;
    uint32_t max_sync_fallback_queue_size = DEFAULT_SYNC_FALLBACK_QUEUE_SIZE;
    bool queue_pressure_candidate_scan_throttle_enabled =
            DEFAULT_QUEUE_PRESSURE_CANDIDATE_SCAN_THROTTLE_ENABLED;
    uint32_t queue_pressure_candidate_scan_throttle_min_queue_depth =
            DEFAULT_QUEUE_PRESSURE_CANDIDATE_SCAN_THROTTLE_MIN_QUEUE_DEPTH;
    uint32_t queue_pressure_candidate_scan_throttle_visible_scan_cap =
            DEFAULT_QUEUE_PRESSURE_VISIBLE_SCAN_BUDGET;
    uint32_t queue_pressure_candidate_scan_throttle_prefetch_scan_cap =
            DEFAULT_QUEUE_PRESSURE_PREFETCH_SCAN_BUDGET;
    uint32_t visible_scan_cursor = 0;
    uint32_t prefetch_scan_cursor = 0;
    uint32_t last_visible_scan_count = 0;
    uint32_t last_visible_scan_budget_effective = 0;
    uint32_t last_load_candidate_count = 0;
    uint32_t last_primary_eviction_scan_count = 0;
    uint32_t last_primary_eviction_candidate_count = 0;
    uint32_t last_non_primary_scan_count = 0;
    uint32_t last_non_primary_eviction_candidate_count = 0;
    uint32_t last_prefetch_scan_count = 0;
    uint32_t last_prefetch_scan_budget_effective = 0;
    uint32_t last_prefetch_candidate_count = 0;
    uint32_t last_prefetch_upload_pending_skip_count = 0;
    uint32_t last_prefetch_enqueued_count = 0;
    uint32_t last_prefetch_enqueue_headroom_stall_count = 0;
    uint32_t last_sync_fallback_queue_depth = 0;
    uint32_t last_sync_fallback_enqueued_count = 0;
    uint32_t last_sync_fallback_drained_count = 0;
    uint32_t last_sync_fallback_dropped_count = 0;
    // #1087: sync-fallback entries the drain tried to admit/load this frame (passed relevance).
    uint32_t last_sync_fallback_attempted_count = 0;
    uint32_t last_sync_fallback_stalled_count = 0;
    // #1086 needed-set accounting, written by _build_visible_chunk_list. "Needed" =
    // visible and inside the load distance; "resident" = GPU-resident and renderable
    // (the frame's visible chunk list); "unserved" = neither loaded nor upload-pending.
    uint32_t last_needed_chunk_count = 0;
    uint32_t last_needed_resident_chunk_count = 0;
    uint32_t last_needed_unserved_chunk_count = 0;
    // Needed but not GPU-resident: unserved, upload-pending (queued or in flight) and
    // loaded-but-not-yet-GPU-resident alike. A view is complete only when this is 0;
    // "unserved" above reads an in-flight chunk as served.
    uint32_t last_needed_not_resident_chunk_count = 0;
    // The upload-pending part of the needed set (queued or in flight, not yet resident).
    uint32_t last_needed_upload_pending_chunk_count = 0;
    uint32_t last_padding_chunk_count = 0; // BV-PRIO: visible only through the frustum padding
    uint32_t last_padding_resident_chunk_count = 0;
    bool last_visible_scan_had_capacity = false;
    bool last_visible_scan_starvation_eligible = false;
    bool last_visible_scan_starved = false;
    // True only on frames where _build_visible_chunk_list ran, so "no demand"
    // (needed_chunks == 0) is distinguishable from "not measured this frame".
    bool last_needed_set_measured = false;
    // Needed chunks whose upload retired this frame (counted in _process_upload_retirements).
    uint32_t last_needed_chunks_completed = 0;
    // Net-progress completions this frame (needed completions not repaying a displacement).
    uint32_t last_needed_set_net_progress = 0;
    // Cumulative across frames (not per-frame reset); see
    // StreamingQueuePressureController::advance_needed_set_progress.
    float needed_set_stall_seconds = 0.0f;
    uint32_t needed_set_displacement_debt = 0;
    uint32_t prefetch_loads_remaining_this_frame = DEFAULT_PREFETCH_LOADS_PER_FRAME;
    uint32_t prefetch_scan_budget_remaining_this_frame = 0;
    bool queue_pressure_candidate_scan_throttle_active = false;
    uint32_t queue_pressure_candidate_scan_throttle_queue_depth = 0;
    bool force_sync_fallback_due_to_async_stall = false;
    static constexpr uint32_t SYNC_FALLBACK_STALENESS_LIMIT_FRAMES = 120;
    uint32_t sync_fallback_no_progress_frames = 0;
    LocalVector<uint64_t> sync_fallback_chunk_load_queue;
    HashSet<uint64_t> sync_fallback_chunk_load_set;
    uint32_t sync_fallback_chunk_load_queue_read_idx = 0;
    double last_update_cpu_ms = 0.0;
    double last_visibility_cpu_ms = 0.0;
    double last_load_cpu_ms = 0.0;
    double last_build_visible_cpu_ms = 0.0;
    double last_prefetch_cpu_ms = 0.0;
    double last_sync_fallback_cpu_ms = 0.0;
    double last_cpu_total_attributed_ms = 0.0;
    double last_cpu_unattributed_ms = 0.0;
};

struct DiagnosticsState {
    static constexpr uint32_t STALL_THRESHOLD_FRAMES = 30;
    // PR #1397: the async-stall sync fallback also needs this much wall clock of the same
    // upload-stall predicate. 30 frames alone is ~60 ms at the 2-8 ms frames of a VRAM-resident
    // pool, while the first pack after a teleport measured 270-350 ms; the frame count fired on
    // a healthy pipeline and pinned the main thread to one ~107 ms GSQ sync load per frame until
    // the staleness flush. 2 s is ~6x the worst measured post-teleport pack latency, so only a
    // pipeline that really stopped completing uploads falls back. Diag:upload_stalled keeps
    // using the frame count alone.
    static constexpr uint64_t ASYNC_STALL_MIN_WALL_USEC = 2000000;
    static constexpr uint32_t LOG_INTERVAL_FRAMES = 120;

    uint32_t init_invalid_frames = 0;
    uint32_t culling_empty_frames = 0;
    uint32_t scheduler_stall_frames = 0;
    uint32_t upload_stall_frames = 0;
    uint32_t sync_fallback_stall_frames = 0;
    uint32_t queue_pressure_frames = 0;
    uint32_t vram_cap_hit_frames = 0;
    uint64_t visible_evict_fallback_attempts = 0;
    uint64_t visible_evict_fallback_successes = 0;
    // #1088: evictions beyond the first that admission needed before a contiguous page run
    // existed for the chunk (fragmentation or a victim smaller than the incoming chunk).
    uint64_t atlas_fit_extra_evictions = 0;
    // #1176: admissions of a predictive prefetch that found only visible chunks to evict and
    // were skipped instead of taking the visible-eviction fallback. Cumulative.
    uint64_t prefetch_visible_eviction_refusals = 0;
    // #1178: chunk payload reads that failed (async pack snapshot or sync pack), cumulative.
    // Each failure backs its chunk off exponentially, so a permanently unreadable chunk adds
    // O(log frames) here, not one per frame; the open-world proof can assert it stays 0.
    uint64_t chunk_payload_read_failures = 0;

    uint32_t last_total_chunks = 0;
    uint32_t last_visible_chunks = 0;
    uint32_t last_loaded_chunks = 0;

    uint64_t invariant_slot_ownership_violations = 0;
    uint64_t invariant_upload_lifecycle_violations = 0;
    uint64_t invariant_generation_violations = 0;
    uint64_t integrity_mismatch_count = 0;
    String last_invariant_context;
    String last_invariant_message;
    String last_integrity_mismatch_message;

    String active_category = "ok";
    String active_reason = "healthy";
    String active_fingerprint = "ok";
    String last_logged_fingerprint;
    uint64_t last_fingerprint_log_frame = 0;

    // Sustained pressure categorization for long-soak diagnostics.
    // Distinguishes bandwidth limits, residency limits, churn, and pipeline stalls.
    static constexpr uint32_t SUSTAINED_PRESSURE_COOLDOWN_FRAMES = 60;
    uint32_t sustained_pressure_frames = 0;
    String pressure_category = "none";   // none, bandwidth_limited, residency_limited, churn, pipeline_stall, combined

    // BV-04a (ADR adr-beyond-vram-worlds.md D-5): wall-clock companion of upload_stall_frames.
    // The frame-count predicate is unchanged; this measures how long the same predicate has
    // held in microseconds, so a gate does not depend on the frame rate.
    uint64_t upload_stall_start_usec = 0;
    uint64_t upload_stall_wall_usec = 0;

    void update_upload_stall_wall_clock(bool p_upload_stalled, uint64_t p_now_usec) {
        if (!p_upload_stalled) {
            upload_stall_start_usec = 0;
            upload_stall_wall_usec = 0;
            return;
        }
        if (upload_stall_start_usec == 0) {
            // A stall that starts this frame has lasted 0 us. A 0 timestamp (no OS) stays 0.
            upload_stall_start_usec = p_now_usec;
        }
        upload_stall_wall_usec = p_now_usec >= upload_stall_start_usec ? p_now_usec - upload_stall_start_usec : 0;
    }

    // BV-04a (ADR D-6 / V7): the effective settings a measured run freezes. The revision counts
    // end_frame() samples whose tuple differs from the previous sample's, so a reader comparing
    // two revisions sees a change even if it was reverted between the frames it sampled.
    struct EffectiveSettingsSample {
        int64_t vram_budget_mb = 0;
        uint32_t regulated_max_chunks = 0;
        uint32_t effective_max_chunks = 0;
        float lod_distance_multiplier = 1.0f;
        bool auto_regulate_enabled = false;

        bool operator==(const EffectiveSettingsSample &p_other) const {
            return vram_budget_mb == p_other.vram_budget_mb &&
                    regulated_max_chunks == p_other.regulated_max_chunks &&
                    effective_max_chunks == p_other.effective_max_chunks &&
                    lod_distance_multiplier == p_other.lod_distance_multiplier &&
                    auto_regulate_enabled == p_other.auto_regulate_enabled;
        }
        bool operator!=(const EffectiveSettingsSample &p_other) const { return !(*this == p_other); }
    };
    EffectiveSettingsSample last_effective_settings;
    bool effective_settings_sampled = false;
    uint64_t effective_settings_revision = 0;

    void observe_effective_settings(const EffectiveSettingsSample &p_sample) {
        if (effective_settings_sampled && p_sample != last_effective_settings) {
            effective_settings_revision++;
        }
        last_effective_settings = p_sample;
        effective_settings_sampled = true;
    }
};

// Inputs of the async-stall sync-fallback decision, one per frame. All values come from the
// previous end_frame() snapshot (last_upload_chunks, upload_stall_frames and
// upload_stall_wall_usec are captured together there) plus the live queue depths.
struct AsyncStallFallbackInputs {
    bool currently_forced = false;
    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    uint32_t sync_fallback_queue_depth = 0;
    // Async uploads completed in the last frame (telemetry upload_chunks; the sync path does
    // not count here).
    uint32_t last_upload_chunks = 0;
    uint32_t upload_stall_frames = 0;
    uint64_t upload_stall_wall_usec = 0;
};

// Returns the new value of SchedulerState::force_sync_fallback_due_to_async_stall. Pure, so the
// decision is testable without threads or a GPU.
inline bool evaluate_async_stall_sync_fallback(const AsyncStallFallbackInputs &p_in) {
    // An async upload completed: the pipeline is alive. Clear a forced fallback at once instead
    // of waiting for every queue to drain, and do not force one this frame.
    if (p_in.last_upload_chunks > 0) {
        return false;
    }
    bool forced = p_in.currently_forced;
    if (forced && p_in.pack_queue_depth == 0 && p_in.upload_queue_depth == 0 &&
            p_in.sync_fallback_queue_depth == 0) {
        forced = false;
    }
    if (forced) {
        return true;
    }
    // Both the frame count and the wall clock of the same stall predicate must pass, so the
    // decision does not depend on the frame rate (DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC).
    const bool stalled_long_enough =
            p_in.upload_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES &&
            p_in.upload_stall_wall_usec >= DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC;
    if (!stalled_long_enough) {
        return false;
    }
    // Original narrow signature: the pack queue has work but nothing reaches the upload queue,
    // so the pack thread may be wedged.
    if (p_in.pack_queue_depth > 0 && p_in.upload_queue_depth == 0) {
        return true;
    }
    // Broader signature: the upload queue has work but nothing completes, so the GPU upload
    // path may be blocked or starved (last_upload_chunks == 0 is established above).
    return p_in.upload_queue_depth > 0;
}

struct PrimaryChunkLayoutMetrics {
    bool spatial_partition_enabled = false;
    uint32_t source_index_count = 0;
    float avg_chunk_radius_ratio = 0.0f;
    float max_chunk_radius_ratio = 0.0f;
    float bounds_volume_ratio = 0.0f;

    void reset() {
        spatial_partition_enabled = false;
        source_index_count = 0;
        avg_chunk_radius_ratio = 0.0f;
        max_chunk_radius_ratio = 0.0f;
        bounds_volume_ratio = 0.0f;
    }
};

} // namespace GaussianStreamingTypes

#endif // STREAMING_RUNTIME_STATE_H
