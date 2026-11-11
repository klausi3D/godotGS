#ifndef STREAMING_ANALYTICS_FRAME_H
#define STREAMING_ANALYTICS_FRAME_H

#include "core/os/mutex.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"
#include "streaming_latency_histogram.h"
#include "streaming_layout_hint.h"
#include "streaming_telemetry_adapter.h"

#include <cstdint>
#include <functional>

// One frame's streaming analytics. GaussianStreamingSystem::end_frame() fills the values, which costs no
// hashing and no Dictionary allocation, and the Dictionary that scripts, monitors and the gates read is
// built by resolve() on the first read only. Values are captured at end_frame() rather than computed at
// read time because readers on other threads must keep seeing the end-of-frame snapshot, never the
// streamer's state in the middle of the next frame.
//
// The tables are in the Dictionary's insertion order; adding a key means adding it here.

#define GS_STREAMING_ANALYTICS_KEYS(X) \
    X(vram_mb) \
    X(vram_payload_mb) \
    X(vram_overhead_mb) \
    X(vram_persistent_buffer_mb) \
    X(loaded_chunks) \
    X(atlas_published_chunks) \
    X(atlas_generation) \
    X(visible_splats) \
    X(effective_max_chunks) \
    X(streaming_initial_capacity) \
    X(streaming_current_capacity) \
    X(streaming_grow_count) \
    X(pool_placement_known) \
    X(pool_device_local) \
    X(pool_memory_type_index) \
    X(pool_memory_heap_index) \
    X(pool_memory_property_flags) \
    X(pool_allocations) \
    X(pool_non_device_local_allocations) \
    X(pool_grow_refusals_non_device_local) \
    X(pool_grow_device_clamps) \
    X(pool_last_grow_old_mb) \
    X(pool_last_grow_new_mb) \
    X(pool_last_grow_ms) \
    X(pool_last_grow_pre_device_local_ext_usage_mb) \
    X(pool_last_grow_pre_device_local_ext_budget_mb) \
    X(pool_last_grow_pre_device_local_allocator_mb) \
    X(pool_last_grow_post_device_local_ext_usage_mb) \
    X(pool_last_grow_post_device_local_allocator_mb) \
    X(pool_last_grow_post_host_allocator_mb) \
    X(device_local_heap_known) \
    X(device_local_heap_mb) \
    X(device_local_allocator_block_mb) \
    X(device_local_allocator_allocation_mb) \
    X(host_allocator_block_mb) \
    X(device_local_ext_known) \
    X(device_local_ext_usage_mb) \
    X(device_local_ext_budget_mb) \
    X(atlas_page_splats) \
    X(atlas_page_bytes) \
    X(atlas_pages_capacity) \
    X(atlas_pages_used) \
    X(atlas_occupancy_target_pages) \
    X(atlas_largest_free_run_pages) \
    X(atlas_free_run_count) \
    X(atlas_resident_splats) \
    X(atlas_fill_ratio) \
    X(atlas_fit_extra_evictions) \
    X(chunks_loaded_this_frame) \
    X(chunks_evicted_this_frame) \
    X(pending_upload_reserved_bytes) \
    X(pending_upload_reserved_slots) \
    X(pending_upload_retirement_tickets) \
    X(retired_upload_bytes_this_frame) \
    X(retired_upload_slots_this_frame) \
    X(failed_upload_retirements) \
    X(stride_flip_dropped_upload_retirements) \
    X(stale_sequence_dropped_uploads) \
    X(stale_sequence_dropped_upload_retirements) \
    X(last_upload_completion_mode) \
    X(load_distance_limit) \
    X(distance_culled_chunks) \
    X(zero_visible_consecutive_frames) \
    X(zero_visible_recoveries_triggered) \
    X(zero_visible_stall_detections) \
    X(zero_visible_recovery_mode) \
    X(runtime_ready) \
    X(runtime_capacity_zero) \
    X(runtime_buffer_invalid) \
    X(invalid_camera_input_events) \
    X(cap_tier_preset) \
    X(cap_tier_active) \
    X(effective_upload_cap_mb_per_frame) \
    X(upload_staging_clamp_bytes_per_frame) \
    X(max_ready_upload_jobs) \
    X(effective_upload_cap_mb_per_slice) \
    X(effective_upload_cap_mb_per_second) \
    X(cap_source_upload_mb_per_frame) \
    X(cap_source_upload_mb_per_slice) \
    X(cap_source_upload_mb_per_second) \
    X(upload_frame_cap_hit) \
    X(upload_slice_cap_hit) \
    X(upload_bandwidth_cap_hit) \
    X(chunk_load_cap_hit) \
    X(vram_chunk_cap_hit) \
    X(vram_budget_mb) \
    X(vram_usage_percent) \
    X(vram_budget_warning) \
    X(vram_regulation_adjustments) \
    X(vram_thrashing_events) \
    X(lod_distance_multiplier) \
    X(vram_auto_regulate_enabled) \
    X(vram_regulated_max_chunks) \
    X(vram_thrash_reductions) \
    X(vram_visible_refetches_total) \
    X(vram_budget_reductions) \
    X(vram_headroom_increases) \
    X(vram_lod_multiplier_adjustments) \
    X(vram_last_regulation_frame) \
    X(vram_last_regulation_reason) \
    X(effective_vram_budget_mb) \
    X(effective_vram_min_chunks) \
    X(effective_vram_max_chunks) \
    X(requested_vram_budget_mb) \
    X(cap_source_vram_budget_mb) \
    X(requested_cap_source_vram_budget_mb) \
    X(cap_source_vram_min_chunks) \
    X(cap_source_vram_max_chunks) \
    X(vram_budget_capacity_verified) \
    X(vram_budget_unknown_capacity_fallback) \
    X(vram_budget_unverified) \
    X(lod_effective_load_distance) \
    X(effective_settings_revision) \
    X(lod_enabled) \
    X(effective_splat_count) \
    X(lod_reduction_ratio) \
    X(pack_avg_ms) \
    X(pack_max_ms) \
    X(pack_jobs_completed) \
    X(upload_mb_this_frame) \
    X(upload_chunks_this_frame) \
    X(upload_bytes_this_frame) \
    X(read_bytes_this_frame) \
    X(decode_bytes_this_frame) \
    X(upload_bytes_total) \
    X(upload_chunks_total) \
    X(read_bytes_total) \
    X(decode_bytes_total) \
    X(streaming_latency_histograms) \
    X(pack_queue_latency_avg_ms) \
    X(pack_queue_latency_max_ms) \
    X(upload_queue_latency_avg_ms) \
    X(upload_queue_latency_max_ms) \
    X(pack_mutex_wait_avg_ms) \
    X(pack_mutex_wait_max_ms) \
    X(pack_thread_wakes) \
    X(pack_thread_dequeues) \
    X(pack_thread_snapshots) \
    X(pack_thread_enqueue_uploads) \
    X(pack_jobs_in_flight) \
    X(avg_chunk_load_time_ms) \
    X(camera_velocity) \
    X(prefetch_hits) \
    X(prefetch_misses) \
    X(evicted_bytes_total_mb) \
    X(cull_effectiveness) \
    X(primary_spatial_chunking_enabled) \
    X(primary_chunk_source_index_count) \
    X(primary_chunk_avg_radius_ratio) \
    X(primary_chunk_max_radius_ratio) \
    X(primary_chunk_bounds_volume_ratio) \
    X(layout_hint_validation) \
    X(layout_hint_strict_mode_enabled) \
    X(layout_hint_fallback_total) \
    X(layout_hint_fallback_io_total) \
    X(layout_hint_fallback_primary_total) \
    X(layout_hint_strict_failure_total) \
    X(layout_hint_last_reason) \
    X(layout_hint_last_reason_category) \
    X(layout_hint_last_context) \
    X(layout_hint_last_usage) \
    X(scheduler_pack_queue_depth) \
    X(scheduler_upload_queue_depth) \
    X(pending_uploads) \
    X(scheduler_visible_scan_chunks) \
    X(scheduler_load_candidates) \
    X(scheduler_primary_eviction_scan_chunks) \
    X(scheduler_primary_eviction_candidates) \
    X(scheduler_non_primary_scan_chunks) \
    X(scheduler_non_primary_eviction_candidates) \
    X(scheduler_prefetch_scan_chunks) \
    X(scheduler_prefetch_candidates) \
    X(scheduler_prefetch_upload_pending_skips) \
    X(scheduler_prefetch_enqueue_headroom_stalls) \
    X(scheduler_update_cpu_ms) \
    X(scheduler_visibility_cpu_ms) \
    X(scheduler_load_cpu_ms) \
    X(scheduler_build_visible_cpu_ms) \
    X(scheduler_prefetch_cpu_ms) \
    X(scheduler_cpu_total_attributed_ms) \
    X(scheduler_cpu_unattributed_ms) \
    X(scheduler_visible_scan_budget_effective) \
    X(needed_set_measured) \
    X(needed_chunks_completed) \
    X(needed_set_net_progress) \
    X(needed_set_displacement_debt) \
    X(chunk_payload_read_failures) \
    X(scheduler_visible_scan_starvation_eligible) \
    X(needed_chunks) \
    X(needed_resident_chunks) \
    X(needed_unserved_chunks) \
    X(needed_not_resident_chunks) \
    X(needed_upload_pending_chunks) \
    X(visible_chunks_evicted_this_frame) \
    X(padding_chunks) \
    X(padding_resident_chunks) \
    X(needed_set_stall_seconds) \
    X(needed_set_stalled) \
    X(scheduler_visible_scan_had_capacity) \
    X(scheduler_visible_scan_starved) \
    X(scheduler_prefetch_scan_budget_effective) \
    X(scheduler_queue_pressure_scan_throttle_active) \
    X(scheduler_queue_pressure_scan_throttle_queue_depth) \
    X(scheduler_queue_pressure_scan_throttle_enabled) \
    X(scheduler_force_sync_fallback_due_to_async_stall) \
    X(scheduler_sync_fallback_queue_depth) \
    X(scheduler_sync_fallback_enqueued) \
    X(scheduler_sync_fallback_drained) \
    X(scheduler_sync_fallback_dropped) \
    X(scheduler_sync_fallback_stalled) \
    X(scheduler_sync_fallback_load_budget) \
    X(scheduler_sync_fallback_cpu_ms) \
    X(sync_promoted_pack_jobs_this_frame) \
    X(sync_promoted_pack_jobs_total) \
    X(cap_sources) \
    X(visible_evict_fallback_attempts) \
    X(visible_evict_fallback_successes) \
    X(prefetch_visible_eviction_refusals) \
    X(diagnostics) \
    X(diagnostics_category) \
    X(diagnostics_reason) \
    X(diagnostics_fingerprint) \
    X(diagnostics_has_failure)

#define GS_STREAMING_DIAGNOSTICS_KEYS(X) \
    X(category) \
    X(reason) \
    X(fingerprint) \
    X(has_failure) \
    X(runtime_ready) \
    X(total_chunks) \
    X(visible_chunks) \
    X(loaded_chunks) \
    X(visible_splats) \
    X(cull_effectiveness) \
    X(primary_spatial_chunking_enabled) \
    X(primary_chunk_source_index_count) \
    X(primary_chunk_avg_radius_ratio) \
    X(primary_chunk_max_radius_ratio) \
    X(primary_chunk_bounds_volume_ratio) \
    X(layout_hint_validation) \
    X(layout_hint_strict_mode_enabled) \
    X(layout_hint_fallback_total) \
    X(layout_hint_fallback_io_total) \
    X(layout_hint_fallback_primary_total) \
    X(layout_hint_strict_failure_total) \
    X(layout_hint_last_reason) \
    X(layout_hint_last_reason_category) \
    X(layout_hint_last_context) \
    X(layout_hint_last_usage) \
    X(load_candidates) \
    X(scheduler_primary_eviction_scan_chunks) \
    X(scheduler_primary_eviction_candidates) \
    X(scheduler_non_primary_scan_chunks) \
    X(scheduler_non_primary_eviction_candidates) \
    X(pack_queue_depth) \
    X(upload_queue_depth) \
    X(pending_upload_reserved_slots) \
    X(pending_upload_reserved_bytes) \
    X(pending_upload_retirement_tickets) \
    X(sync_fallback_queue_depth) \
    X(pack_jobs_in_flight) \
    X(total_pending_chunks) \
    X(visible_eviction_active) \
    X(scheduler_visible_scan_budget_effective) \
    X(scheduler_prefetch_scan_budget_effective) \
    X(scheduler_prefetch_upload_pending_skips) \
    X(scheduler_prefetch_enqueue_headroom_stalls) \
    X(scheduler_queue_pressure_scan_throttle_active) \
    X(scheduler_queue_pressure_scan_throttle_queue_depth) \
    X(scheduler_queue_pressure_scan_throttle_enabled) \
    X(scheduler_force_sync_fallback_due_to_async_stall) \
    X(chunks_loaded_this_frame) \
    X(chunks_uploaded_this_frame) \
    X(retired_upload_bytes_this_frame) \
    X(retired_upload_slots_this_frame) \
    X(failed_upload_retirements) \
    X(stride_flip_dropped_upload_retirements) \
    X(chunk_payload_read_failures) \
    X(stale_sequence_dropped_uploads) \
    X(stale_sequence_dropped_upload_retirements) \
    X(last_upload_completion_mode) \
    X(last_completed_upload_ticket_id) \
    X(sync_promoted_pack_jobs_this_frame) \
    X(sync_promoted_pack_jobs_total) \
    X(pack_thread_wakes) \
    X(pack_thread_dequeues) \
    X(pack_thread_snapshots) \
    X(pack_thread_enqueue_uploads) \
    X(cap_tier_preset) \
    X(cap_tier_active) \
    X(effective_upload_cap_mb_per_frame) \
    X(effective_upload_cap_mb_per_slice) \
    X(effective_upload_cap_mb_per_second) \
    X(effective_vram_budget_mb) \
    X(effective_vram_min_chunks) \
    X(effective_vram_max_chunks) \
    X(requested_vram_budget_mb) \
    X(cap_source_upload_mb_per_frame) \
    X(cap_source_upload_mb_per_slice) \
    X(cap_source_upload_mb_per_second) \
    X(cap_source_vram_budget_mb) \
    X(requested_cap_source_vram_budget_mb) \
    X(cap_source_vram_min_chunks) \
    X(cap_source_vram_max_chunks) \
    X(vram_budget_capacity_verified) \
    X(vram_budget_unknown_capacity_fallback) \
    X(vram_budget_unverified) \
    X(upload_frame_cap_hit) \
    X(upload_slice_cap_hit) \
    X(upload_bandwidth_cap_hit) \
    X(chunk_load_cap_hit) \
    X(vram_chunk_cap_hit) \
    X(queue_pressure_frames) \
    X(vram_cap_hit_frames) \
    X(init_invalid_frames) \
    X(culling_empty_frames) \
    X(scheduler_stall_frames) \
    X(upload_stall_frames) \
    X(upload_stall_wall_ms) \
    X(sync_fallback_stall_frames) \
    X(sustained_pressure_frames) \
    X(pressure_category) \
    X(scheduler_update_cpu_ms) \
    X(scheduler_visibility_cpu_ms) \
    X(scheduler_load_cpu_ms) \
    X(scheduler_build_visible_cpu_ms) \
    X(scheduler_prefetch_cpu_ms) \
    X(scheduler_sync_fallback_cpu_ms) \
    X(scheduler_cpu_total_attributed_ms) \
    X(scheduler_cpu_unattributed_ms) \
    X(pack_queue_latency_avg_ms) \
    X(pack_queue_latency_max_ms) \
    X(upload_queue_latency_avg_ms) \
    X(upload_queue_latency_max_ms) \
    X(pack_mutex_wait_avg_ms) \
    X(pack_mutex_wait_max_ms) \
    X(visible_evict_fallback_attempts) \
    X(visible_evict_fallback_successes) \
    X(prefetch_visible_eviction_refusals) \
    X(invariant_slot_ownership_violations) \
    X(invariant_upload_lifecycle_violations) \
    X(invariant_generation_violations) \
    X(integrity_mismatch_count) \
    X(last_invariant_context) \
    X(last_invariant_message) \
    X(last_integrity_mismatch_message) \
    X(atlas_generation) \
    X(request_generation)

class StreamingAnalyticsFrame {
public:
#define GS_KEY_ENUM(m_key) m_key,
    enum class AnalyticsKey : uint16_t {
        GS_STREAMING_ANALYTICS_KEYS(GS_KEY_ENUM)
        COUNT,
    };
    enum class DiagnosticsKey : uint16_t {
        GS_STREAMING_DIAGNOSTICS_KEYS(GS_KEY_ENUM)
        COUNT,
    };
#undef GS_KEY_ENUM

    // A fixed slot per key. A slot nothing wrote stays out of the Dictionary, like a key never inserted.
    template <typename TKey>
    class KeyedValues {
        static constexpr int COUNT = static_cast<int>(TKey::COUNT);
        Variant values[COUNT];
        bool present[COUNT] = {};

    public:
        Variant &operator[](TKey p_key) {
            const int index = static_cast<int>(p_key);
            present[index] = true;
            return values[index];
        }
        Variant get(TKey p_key, const Variant &p_default) const {
            const int index = static_cast<int>(p_key);
            return present[index] ? values[index] : p_default;
        }
        void append_range(Dictionary &r_out, const char *const *p_names, TKey p_first, TKey p_end) const {
            for (int i = static_cast<int>(p_first); i < static_cast<int>(p_end); i++) {
                if (present[i]) {
                    r_out[Variant(p_names[i])] = values[i];
                }
            }
        }
    };
    using AnalyticsValues = KeyedValues<AnalyticsKey>;
    using DiagnosticsValues = KeyedValues<DiagnosticsKey>;
    // Runs once on the resolved Dictionary. The renderer adds its per-frame keys through this.
    using Overlay = std::function<void(Dictionary &)>;

    AnalyticsValues analytics;
    DiagnosticsValues diagnostics;
    Dictionary task_debug_state;
    StreamingLatencyHistogram::Snapshot read_latency;
    StreamingLatencyHistogram::Snapshot decode_latency;
    StreamingLatencyHistogram::Snapshot pack_latency;
    StreamingLatencyHistogram::Snapshot upload_latency;
    gs_layout_hint::LayoutHintFailureCounters layout_hint_counters;
    bool layout_hint_counters_valid = false;
    bool layout_hint_strict_mode = false;
    StreamingTelemetryAdapter::QueuePressureSnapshot analytics_queue_pressure;
    StreamingTelemetryAdapter::QueuePressureSnapshot diagnostics_queue_pressure;
    Overlay overlay;

    // Fields above are written by end_frame() before the frame is published and are read-only after that.
    // The first call builds the Dictionary; every later call returns the same one, so a key a reader
    // writes into it stays visible to later readers, as with the former analytics_snapshot member.
    Dictionary resolve();

private:
    Dictionary _build_diagnostics();
    Dictionary _build_layout_hint_validation() const;

    BinaryMutex resolve_mutex; // Guards resolved_* only.
    bool resolved = false;
    Dictionary resolved_dictionary;
};

#endif // STREAMING_ANALYTICS_FRAME_H
