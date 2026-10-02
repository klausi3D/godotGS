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
    // while the visible scan had enqueue headroom, yet the scan found no load
    // candidate -- the scan window never reached the demand. This is the case a
    // throttled or capped scan produces when it restarts at a nearest prefix that is
    // already resident. A scan cut to one chunk because pack jobs in flight hit
    // max_pack_jobs_in_flight (headroom 0) is designed backpressure, not starvation.
    // A frame is ELIGIBLE (could have starved) when the scan had capacity and needed
    // chunks were unserved; with an uncapped, unthrottled scan every eligible frame
    // finds candidates, so starvation needs a scan budget below the visible count.
    struct VisibleScanStarvationInput {
        uint32_t needed_unserved_chunks = 0;
        bool scan_had_capacity = false;
        uint32_t load_candidates = 0;
    };
    static bool is_visible_scan_starvation_eligible(const VisibleScanStarvationInput &p_input);
    static bool is_visible_scan_starved(const VisibleScanStarvationInput &p_input);
    // Whether the visible scan could have enqueued work this frame: it ran and had
    // enqueue headroom. p_scan_ran is false when the scan returned before scanning.
    static bool visible_scan_had_capacity(bool p_scan_ran, uint32_t p_enqueue_headroom);

    // Needed-set progress. A frame makes progress when a NEEDED chunk completes that
    // did not merely replace another needed chunk: completions of non-needed chunks
    // (prefetch) do not count, and a completion first pays off "displacement debt"
    // -- one unit per needed (visible) chunk evicted to make room. Evict/reload
    // churn of the needed set therefore makes no progress. Debt no pending load can
    // repay is dropped (capped at the in-flight load count).
    //
    // The stall is the sum of streaming frame deltas (each clamped by
    // _resolve_frame_delta_seconds to [0.0005, 0.25] s) for which the needed set has
    // been incomplete without progress; it resets on progress or a complete needed
    // set. The threshold is the engine's STALL_THRESHOLD_FRAMES (30) at the 60 fps
    // reference delta, in seconds so the onset does not depend on the frame rate.
    static constexpr float NEEDED_SET_STALL_THRESHOLD_SECONDS = 0.5f;
    struct NeededSetProgressInput {
        uint32_t needed_chunks = 0;
        uint32_t needed_resident_chunks = 0;
        uint32_t needed_chunks_completed = 0;
        uint32_t needed_chunks_evicted = 0;
        uint32_t in_flight_loads = 0;
        float frame_delta_seconds = 0.0f;
    };
    struct NeededSetProgressState {
        float stall_seconds = 0.0f;
        uint32_t displacement_debt = 0;
    };
    // Returns the number of net-progress completions this frame.
    static uint32_t advance_needed_set_progress(NeededSetProgressState &r_state, const NeededSetProgressInput &p_input);

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
