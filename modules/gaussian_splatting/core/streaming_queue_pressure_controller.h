#ifndef STREAMING_QUEUE_PRESSURE_CONTROLLER_H
#define STREAMING_QUEUE_PRESSURE_CONTROLLER_H

#include "core/string/ustring.h"
#include <cstdint>

class StreamingQueuePressureController {
public:
    // Invariants:
    // 1) inactive state must serialize as source=none, reason=none.
    // 2) active state must expose a known source and reason token.
    // 3) summary source flags must match sampled queue/cap inputs.
    struct ScanBudgetInput {
        uint32_t base_scan_budget = 0;
        bool throttle_enabled = false;
        uint32_t throttle_min_queue_depth = 0;
        uint32_t observed_queue_depth = 0;
        uint32_t throttle_scan_cap = 0;
        uint32_t scanned_this_frame = 0;
        uint32_t enqueue_headroom = UINT32_MAX;
    };

    struct ScanBudgetResult {
        uint32_t scan_budget = 0;
        bool throttle_active = false;
        uint32_t effective_queue_depth = 0;
    };

    struct PressureSample {
        uint32_t pack_queue_depth = 0;
        uint32_t upload_queue_depth = 0;
        uint32_t sync_fallback_queue_depth = 0;
        uint32_t pack_jobs_in_flight = 0;
        bool pack_inflight_saturated = false;
        bool upload_frame_cap_hit = false;
        bool upload_bandwidth_cap_hit = false;
        bool chunk_load_cap_hit = false;
        bool vram_chunk_cap_hit = false;
        bool sync_backpressure = false;
        bool visible_eviction_active = false;
    };

    struct PressureSummary {
        bool active = false;
        bool cap_active = false;
        bool pack_source_active = false;
        bool upload_source_active = false;
        bool sync_source_active = false;
        uint32_t backlog_depth = 0;
        uint32_t total_pending_chunks = 0;
        String source = "none";
        String reason = "none";
    };

    static constexpr uint32_t ENQUEUE_HEADROOM_TARGET = 4u;

    static constexpr const char *SOURCE_NONE = "none";
    static constexpr const char *SOURCE_PACK = "pack";
    static constexpr const char *SOURCE_UPLOAD = "upload";
    static constexpr const char *SOURCE_SYNC = "sync";
    static constexpr const char *SOURCE_CAP = "cap";
    static constexpr const char *SOURCE_COMBINED = "combined";

    static constexpr const char *REASON_NONE = "none";
    static constexpr const char *REASON_QUEUE_BACKLOG = "queue_backlog";
    static constexpr const char *REASON_PACK_QUEUE_BACKLOG = "pack_queue_backlog";
    static constexpr const char *REASON_UPLOAD_QUEUE_BACKLOG = "upload_queue_backlog";
    static constexpr const char *REASON_SYNC_QUEUE_BACKLOG = "sync_queue_backlog";
    static constexpr const char *REASON_QUEUE_AND_CAPS = "queue_and_caps";
    static constexpr const char *REASON_UPLOAD_FRAME_CAP = "upload_frame_cap";
    static constexpr const char *REASON_UPLOAD_BANDWIDTH_CAP = "upload_bandwidth_cap";
    static constexpr const char *REASON_UPLOAD_CAP_COMBINED = "upload_cap_combined";
    static constexpr const char *REASON_CHUNK_LOAD_CAP = "chunk_load_cap";
    static constexpr const char *REASON_VRAM_CHUNK_CAP = "vram_chunk_cap";
    static constexpr const char *REASON_PACK_INFLIGHT_CAP = "pack_inflight_cap";
    static constexpr const char *REASON_SYNC_FALLBACK_PRESSURE = "sync_fallback_pressure";
    static constexpr const char *REASON_SYNC_QUEUE_CAP = "sync_queue_cap";
    static constexpr const char *REASON_CAP_COMBINED = "cap_combined";

    // #1086: forward-progress signals that separate a FAILURE from designed
    // backpressure. The "needed set" is the visible chunks inside the load distance
    // (exactly the set _load_visible_chunks treats as load candidates).
    //
    // Scan starvation: needed chunks sit unserved (not loaded, not upload-pending)
    // while the visible scan had capacity to enqueue (enqueue headroom left and the
    // queue-pressure throttle NOT engaged), yet the scan found no load candidate --
    // i.e. the scan window never reached the demand. A scan cut to one chunk because
    // pack jobs in flight hit max_pack_jobs_in_flight (headroom 0, throttle engaged)
    // is the throttle doing its job, not starvation.
    struct VisibleScanStarvationInput {
        uint32_t needed_unserved_chunks = 0;
        bool scan_had_capacity = false;
        uint32_t load_candidates = 0;
    };
    static bool is_visible_scan_starved(const VisibleScanStarvationInput &p_input);
    // Whether the visible scan could have enqueued work this frame. p_scan_ran is
    // false when the scan returned before scanning anything.
    static bool visible_scan_had_capacity(bool p_scan_ran, uint32_t p_enqueue_headroom, bool p_throttle_active);

    // Needed-set stall: wall time (sum of streaming frame deltas) for which the needed
    // set has been incomplete while no chunk load completed. Frame-rate independent:
    // a pipeline completing ~0.2 chunks per frame at 200 fps never accumulates more
    // than a few frame deltas. Reset by any completion or by a complete needed set.
    // The threshold is the engine's STALL_THRESHOLD_FRAMES (30) at the 60 fps
    // reference frame delta, expressed in seconds so it no longer depends on the
    // frame rate.
    static constexpr float NEEDED_SET_STALL_THRESHOLD_SECONDS = 0.5f;
    static float advance_needed_set_stall_seconds(float p_previous_stall_seconds,
            uint32_t p_needed_chunks, uint32_t p_needed_resident_chunks,
            uint32_t p_chunks_completed_this_frame, float p_frame_delta_seconds);

    static ScanBudgetResult compute_candidate_scan_budget(const ScanBudgetInput &p_input);
    static PressureSummary summarize(const PressureSample &p_sample);

    // Diagnostics wrappers (relocated from gaussian_streaming.cpp's anonymous
    // namespace): pure guards over summarize() / validate_latched_state_invariants()
    // that log and self-heal on an invariant violation.
    static PressureSummary summarize_checked(const PressureSample &p_sample, const char *p_context);
    static void validate_latched_state(bool &r_active, String &r_source, String &r_reason, const char *p_context);

    static void reset_latched_state(bool &r_active, String &r_source, String &r_reason);
    static void mark_latched_state(bool &r_active, String &r_source, String &r_reason,
            const char *p_source, const char *p_reason);
    static void latch_summary(const PressureSummary &p_summary, bool &r_active, String &r_source, String &r_reason);

    static bool is_known_source(const String &p_source);
    static bool is_known_reason(const String &p_reason);
    static bool validate_summary_invariants(const PressureSummary &p_summary,
            const PressureSample &p_sample, String *r_error = nullptr);
    static bool validate_latched_state_invariants(bool p_active,
            const String &p_source, const String &p_reason, String *r_error = nullptr);
};

#endif // STREAMING_QUEUE_PRESSURE_CONTROLLER_H
