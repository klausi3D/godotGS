#include "gaussian_streaming.h"

#include "gs_project_settings.h"
#include "residency_budget_controller.h"
#include "streaming_analytics_frame.h"
#include "streaming_chunk_invariants.h"
#include "streaming_layout_hint.h"
#include "streaming_queue_pressure_controller.h"
#include "streaming_telemetry_adapter.h"
#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "gaussian_splat_manager.h"
#include "../renderer/gaussian_gpu_layout.h"
#include "../renderer/gpu_memory_stream.h"
#include "../logger/gs_logger.h"
#include "../lod/lod_config.h"
#include <cstdint>
#include <memory>
#include <utility>

// Streaming diagnostics surface: telemetry logging, end-of-frame analytics
// assembly, the diagnostics snapshot, and the public debug/analytics/culling/
// VRAM stat getters. These GaussianStreamingSystem member definitions were
// moved verbatim out of gaussian_streaming.cpp (declarations remain in
// gaussian_streaming.h). The layout-hint validation helpers used by these
// bodies live in streaming_layout_hint.{h,cpp}; the queue-pressure summary /
// latch guards live on StreamingQueuePressureController.
using namespace gs_layout_hint;

void GaussianStreamingSystem::_log_streaming_telemetry() {
    static uint64_t debug_frame = 0;
    const bool telemetry_enabled = debug_logging_enabled;
    const uint32_t log_freq = debug_frame_log_frequency > 0 ? debug_frame_log_frequency : 60;
    if (!telemetry_enabled || (++debug_frame % log_freq) != 1) {
        return;
    }

    GS_LOG_STREAMING_DEBUG(vformat("[STREAM-DBG] frame=%d chunks=%d loaded=%d vis=%d",
            total_frame_count, chunks.size(), budget.loaded_chunks_count,
            frame_data[current_frame_idx].visible_chunks.size()));

    uint32_t pack_queue_size = 0;
    uint32_t upload_queue_size = 0;
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_size, upload_queue_size);

    const StreamingUploadPipeline::PackTelemetry::Snapshot tsnap = upload_pipeline.telemetry.read_current();

    const double pack_avg_ms = tsnap.pack_jobs > 0
            ? double(tsnap.pack_time_total) / (USEC_PER_MS * double(tsnap.pack_jobs))
            : 0.0;
    const double pack_max_ms = double(tsnap.pack_time_max) / USEC_PER_MS;
    const double upload_mb = double(tsnap.upload_bytes) / double(BYTES_PER_MB);
    const double upload_mb_per_frame = upload_mb / double(log_freq);

    GS_LOG_STREAMING_DEBUG(vformat(
            "[STREAM-PACK] frame=%d async=%s threads=%d inflight=%d pack_avg=%.2fms pack_max=%.2fms pack_jobs=%d "
            "upload=%.2fMB (%.2fMB/f) upload_chunks=%d queues=(pack=%d upload=%d)",
            total_frame_count,
            upload_pipeline.async_pack_enabled ? "yes" : "no",
            upload_pipeline.pack_worker_threads,
            upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed),
            pack_avg_ms,
            pack_max_ms,
            tsnap.pack_jobs,
            upload_mb,
            upload_mb_per_frame,
            tsnap.upload_chunks,
            pack_queue_size,
            upload_queue_size));
}

void GaussianStreamingSystem::_log_streaming_frame_stats(uint32_t effective_max) {
    uint32_t visible_count = get_visible_count();
    visibility.prev_visible_count = visible_count;
    uint32_t effective_splats = get_effective_splat_count();

    if (visible_count == 0) {
        return;
    }

    const FrameData &frame = frame_data[current_frame_idx];
    if (debug_logging_enabled && debug_frame_log_frequency > 0 &&
            (total_frame_count % static_cast<uint64_t>(debug_frame_log_frequency) == 0)) {
        GS_LOG_STREAMING_DEBUG(vformat("[Streaming] Frame %d: %d visible chunks, %d visible splats (max: %d, usage: %.1f%%)",
                current_frame_idx, frame.visible_chunks.size(), visible_count,
                effective_max,
                budget.vram_regulator.is_valid() ? budget.vram_regulator->get_debug_stats().usage_percent : 0.0f));
    }

    static uint64_t lod_log_counter = 0;
    const LODConfig &lod_config = _get_lod_config();
    if (lod_config.enabled && lod_config.diagnostic_logging && (++lod_log_counter % 300) == 0) {
        float reduction_pct = visible_count > 0
                ? (1.0f - float(effective_splats) / float(visible_count)) * 100.0f
                : 0.0f;
        GS_LOG_STREAMING_INFO(vformat("[LOD] Splats: %d -> %d (%.1f%% reduction), Levels: %d, MaxDist: %.1f",
                visible_count, effective_splats, reduction_pct,
                lod_config.num_levels, lod_config.max_distance));
    }
}

void GaussianStreamingSystem::end_frame() {
    end_frame_with_overlay(AnalyticsOverlay());
}

void GaussianStreamingSystem::end_frame_with_overlay(AnalyticsOverlay p_overlay) {
    if (memory_stream_proxy.is_valid()) {
        memory_stream_proxy->end_frame();
    }
    std::shared_ptr<StreamingAnalyticsFrame> frame = std::make_shared<StreamingAnalyticsFrame>();
    frame->task_debug_state = memory_stream_proxy.is_valid() ? memory_stream_proxy->get_task_debug_state() : Dictionary();
    frame->overlay = std::move(p_overlay);
    StreamingAnalyticsFrame::AnalyticsValues &analytics = frame->analytics;
    using AK = StreamingAnalyticsFrame::AnalyticsKey;

    // Add VRAM usage stats to analytics. vram_mb mirrors the regulator-facing
    // total (full persistent buffer allocation + auxiliary overhead), so the
    // reported figure is not under-counted by the unfilled buffer remainder.
    const uint64_t payload_vram_bytes = budget.vram_usage;
    const uint64_t overhead_vram_bytes = _get_auxiliary_vram_overhead_bytes();
    // Mirror the live-RID gate in _get_total_vram_usage_bytes(): a failed
    // storage_buffer_create() leaves persistent_buffer_size nonzero with an invalid
    // RID, so report the allocation only when it actually exists. Otherwise vram_mb
    // (gated) and vram_persistent_buffer_mb (ungated) would disagree on the
    // allocation-failure path. (Codex #411)
    const uint64_t allocated_persistent_bytes = persistent_buffer.is_valid() ? uint64_t(persistent_buffer_size) : 0;
    analytics[AK::vram_mb] = double(_get_total_vram_usage_bytes()) / (1024.0 * 1024.0);
    analytics[AK::vram_payload_mb] = double(payload_vram_bytes) / (1024.0 * 1024.0);
    analytics[AK::vram_overhead_mb] = double(overhead_vram_bytes) / (1024.0 * 1024.0);
    analytics[AK::vram_persistent_buffer_mb] = double(allocated_persistent_bytes) / (1024.0 * 1024.0);
    analytics[AK::loaded_chunks] = get_loaded_chunks();
    analytics[AK::atlas_published_chunks] = global_atlas_registry.get_atlas_published_chunks();
    analytics[AK::atlas_generation] = static_cast<int64_t>(global_atlas_registry.get_atlas_generation());
    analytics[AK::visible_splats] = get_visible_count();
    analytics[AK::effective_max_chunks] = get_effective_max_chunks();
    analytics[AK::streaming_initial_capacity] = static_cast<int64_t>(streaming_initial_capacity);
    analytics[AK::streaming_current_capacity] = static_cast<int64_t>(streaming_current_capacity);
    analytics[AK::streaming_grow_count] = static_cast<int64_t>(streaming_grow_count);
    // #1088: page-granular atlas occupancy. Capacity fields above are in pages too.
    {
        uint64_t resident_splats = 0;
        for (uint32_t asset_id : asset_registry.atlas_asset_order) {
            const AtlasAssetState *asset = _get_asset_state(asset_id);
            if (!asset) {
                continue;
            }
            for (const StreamingChunk &chunk : _get_asset_chunks(*asset)) {
                if (chunk.is_loaded) {
                    resident_splats += chunk.count;
                }
            }
        }
        const uint32_t used_pages = atlas_allocator.get_used_page_count();
        analytics[AK::atlas_page_splats] = static_cast<int64_t>(ATLAS_PAGE_SPLATS);
        analytics[AK::atlas_page_bytes] = static_cast<int64_t>(_atlas_page_bytes());
        analytics[AK::atlas_pages_capacity] = static_cast<int64_t>(atlas_allocator.get_capacity());
        analytics[AK::atlas_pages_used] = static_cast<int64_t>(used_pages);
        analytics[AK::atlas_occupancy_target_pages] = static_cast<int64_t>(_atlas_occupancy_target_pages());
        analytics[AK::atlas_largest_free_run_pages] = static_cast<int64_t>(atlas_allocator.get_largest_free_run());
        analytics[AK::atlas_free_run_count] = static_cast<int64_t>(atlas_allocator.get_free_run_count());
        analytics[AK::atlas_resident_splats] = static_cast<int64_t>(resident_splats);
        // Resident splats over the splats the allocated runs could hold (1.0 = no tail waste).
        // Pending uploads hold runs too, so this is a lower bound while uploads are in flight.
        analytics[AK::atlas_fill_ratio] = used_pages > 0
                ? double(resident_splats) / (double(used_pages) * double(ATLAS_PAGE_SPLATS))
                : 0.0;
        analytics[AK::atlas_fit_extra_evictions] = static_cast<int64_t>(diagnostics.atlas_fit_extra_evictions);
    }
    analytics[AK::chunks_loaded_this_frame] = budget.chunks_loaded_this_frame;
    analytics[AK::chunks_evicted_this_frame] = eviction_controller.get_chunks_evicted_this_frame();
    analytics[AK::pending_upload_reserved_bytes] = static_cast<int64_t>(_get_pending_upload_bytes_for_diagnostics());
    analytics[AK::pending_upload_reserved_slots] = static_cast<int64_t>(get_pending_upload_retirement_slots());
    analytics[AK::pending_upload_retirement_tickets] = static_cast<int64_t>(pending_upload_retirements.size());
    analytics[AK::retired_upload_bytes_this_frame] = static_cast<int64_t>(budget.retired_upload_bytes_this_frame);
    analytics[AK::retired_upload_slots_this_frame] = static_cast<int64_t>(budget.retired_upload_slots_this_frame);
    analytics[AK::failed_upload_retirements] = static_cast<int64_t>(budget.failed_upload_retirements);
    analytics[AK::stride_flip_dropped_upload_retirements] = static_cast<int64_t>(budget.stride_flip_dropped_upload_retirements);
    // #1177: stale (cancelled-then-superseded) uploads dropped at pack completion and retirement.
    analytics[AK::stale_sequence_dropped_uploads] = static_cast<int64_t>(budget.stale_sequence_dropped_uploads);
    analytics[AK::stale_sequence_dropped_upload_retirements] =
            static_cast<int64_t>(budget.stale_sequence_dropped_upload_retirements);
    analytics[AK::last_upload_completion_mode] = last_upload_completion_mode;
    // #1087: the distance bound on chunk demand (0 = unbounded) and what it removed this frame.
    analytics[AK::load_distance_limit] = visibility.load_distance_limit;
    analytics[AK::distance_culled_chunks] = static_cast<int64_t>(visibility.culling_stats.distance_culled_chunks);
    analytics[AK::zero_visible_consecutive_frames] = visibility.zero_visible_recovery.zero_visible_consecutive_frames;
    analytics[AK::zero_visible_recoveries_triggered] = (int)visibility.zero_visible_recovery.recoveries_triggered;
    analytics[AK::zero_visible_stall_detections] = (int)visibility.zero_visible_recovery.stall_detections;
    analytics[AK::zero_visible_recovery_mode] =
            visibility.zero_visible_recovery.mode == ZeroVisibleRecoveryMode::PERSISTENT ? "persistent" : "startup_only";
    analytics[AK::runtime_ready] = is_runtime_ready();
    analytics[AK::runtime_capacity_zero] = is_runtime_capacity_zero();
    analytics[AK::runtime_buffer_invalid] = is_persistent_buffer_invalid();
    analytics[AK::invalid_camera_input_events] = (int)invalid_camera_input_events;
    analytics[AK::cap_tier_preset] = upload_pipeline.cap_tier_preset;
    analytics[AK::cap_tier_active] = upload_pipeline.cap_tier_active;
    analytics[AK::effective_upload_cap_mb_per_frame] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_frame);
    analytics[AK::effective_upload_cap_mb_per_slice] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_slice);
    analytics[AK::effective_upload_cap_mb_per_second] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_second);
    analytics[AK::cap_source_upload_mb_per_frame] = upload_pipeline.cap_source_upload_mb_per_frame;
    analytics[AK::cap_source_upload_mb_per_slice] = upload_pipeline.cap_source_upload_mb_per_slice;
    analytics[AK::cap_source_upload_mb_per_second] = upload_pipeline.cap_source_upload_mb_per_second;
    analytics[AK::upload_frame_cap_hit] = upload_pipeline.upload_frame_cap_hit_this_frame;
    analytics[AK::upload_slice_cap_hit] = upload_pipeline.upload_slice_cap_hit_this_frame;
    analytics[AK::upload_bandwidth_cap_hit] = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    analytics[AK::chunk_load_cap_hit] = upload_pipeline.chunk_load_cap_hit_this_frame;
    analytics[AK::vram_chunk_cap_hit] = budget.vram_chunk_cap_hit_this_frame;

    // Add VRAM regulator stats if available
    if (budget.vram_regulator.is_valid()) {
        VRAMDebugStats vram_stats = budget.vram_regulator->get_debug_stats();
        const VRAMBudgetConfig &active_budget_config = budget.vram_regulator->get_config();
        analytics[AK::vram_budget_mb] = float(vram_stats.budget_bytes) / (1024.0f * 1024.0f);
        analytics[AK::vram_usage_percent] = vram_stats.usage_percent;
        analytics[AK::vram_budget_warning] = vram_stats.budget_warning_active;
        analytics[AK::vram_regulation_adjustments] = vram_stats.regulation_adjustments;
        analytics[AK::vram_thrashing_events] = vram_stats.thrashing_events;
        analytics[AK::lod_distance_multiplier] = budget.vram_regulator->get_lod_distance_multiplier();
        // BV-04a (ADR D-6): regulator events by kind.
        analytics[AK::vram_auto_regulate_enabled] = active_budget_config.auto_regulate_enabled;
        analytics[AK::vram_regulated_max_chunks] = static_cast<int64_t>(budget.vram_regulator->get_current_max_chunks());
        analytics[AK::vram_thrash_reductions] = static_cast<int64_t>(vram_stats.thrash_reductions);
        analytics[AK::vram_visible_refetches_total] = static_cast<int64_t>(vram_stats.visible_refetches_total);
        analytics[AK::vram_budget_reductions] = static_cast<int64_t>(vram_stats.budget_reductions);
        analytics[AK::vram_headroom_increases] = static_cast<int64_t>(vram_stats.headroom_increases);
        analytics[AK::vram_lod_multiplier_adjustments] = static_cast<int64_t>(vram_stats.lod_multiplier_adjustments);
        analytics[AK::vram_last_regulation_frame] = static_cast<int64_t>(vram_stats.last_regulation_frame);
        analytics[AK::vram_last_regulation_reason] = String(vram_stats.last_regulation_reason);
        analytics[AK::effective_vram_budget_mb] = static_cast<int64_t>(active_budget_config.budget_mb);
        analytics[AK::effective_vram_min_chunks] = static_cast<int64_t>(active_budget_config.min_chunks);
        analytics[AK::effective_vram_max_chunks] = static_cast<int64_t>(active_budget_config.max_chunks);
        analytics[AK::requested_vram_budget_mb] = static_cast<int64_t>(active_budget_config.requested_budget_mb);
        analytics[AK::cap_source_vram_budget_mb] = active_budget_config.source_budget_mb;
        analytics[AK::requested_cap_source_vram_budget_mb] = active_budget_config.requested_source_budget_mb;
        analytics[AK::cap_source_vram_min_chunks] = active_budget_config.source_min_chunks;
        analytics[AK::cap_source_vram_max_chunks] = active_budget_config.source_max_chunks;
        analytics[AK::vram_budget_capacity_verified] = active_budget_config.budget_capacity_verified;
        analytics[AK::vram_budget_unknown_capacity_fallback] = active_budget_config.budget_uses_unknown_capacity_fallback;
        analytics[AK::vram_budget_unverified] = active_budget_config.budget_unverified;
        analytics[AK::cap_tier_preset] = active_budget_config.cap_tier_preset;
        analytics[AK::cap_tier_active] = active_budget_config.cap_tier_active;
    }
    // BV-04a: the chunk load distance the LOD multiplier produces (base / multiplier), and the
    // revision of the effective settings a measured run freezes (see DiagnosticsState).
    analytics[AK::lod_effective_load_distance] = _get_needed_set_load_threshold();
    {
        DiagnosticsState::EffectiveSettingsSample settings_sample;
        if (budget.vram_regulator.is_valid()) {
            const VRAMBudgetConfig &settings_config = budget.vram_regulator->get_config();
            settings_sample.vram_budget_mb = static_cast<int64_t>(settings_config.budget_mb);
            settings_sample.regulated_max_chunks = budget.vram_regulator->get_current_max_chunks();
            settings_sample.lod_distance_multiplier = budget.vram_regulator->get_lod_distance_multiplier();
            settings_sample.auto_regulate_enabled = settings_config.auto_regulate_enabled;
        }
        settings_sample.effective_max_chunks = get_effective_max_chunks();
        diagnostics.observe_effective_settings(settings_sample);
        analytics[AK::effective_settings_revision] = static_cast<int64_t>(diagnostics.effective_settings_revision);
    }

    // Add distance-based LOD stats (Octree-GS)
    const LODConfig &lod_config = _get_lod_config();
    analytics[AK::lod_enabled] = lod_config.enabled;
    analytics[AK::effective_splat_count] = get_effective_splat_count();
    if (lod_config.enabled) {
        Dictionary lod_stats = get_lod_debug_stats();
        analytics[AK::lod_reduction_ratio] = lod_stats.get("reduction_ratio", 0.0f);
    }

    // Sample pack/upload timing metrics (every frame for performance monitors).
    // PackTelemetry::exchange_and_reset() atomically drains all counters in one
    // cache-line-local sweep, replacing 15 scattered atomic exchanges.
    const StreamingUploadPipeline::PackTelemetry::Snapshot tsnap = upload_pipeline.telemetry.exchange_and_reset();

    upload_pipeline.last_pack_avg_ms = tsnap.pack_jobs > 0 ? double(tsnap.pack_time_total) / (1000.0 * double(tsnap.pack_jobs)) : 0.0;
    upload_pipeline.last_pack_max_ms = double(tsnap.pack_time_max) / 1000.0;
    upload_pipeline.last_pack_jobs = tsnap.pack_jobs;
    upload_pipeline.last_upload_mb = double(tsnap.upload_bytes) / (1024.0 * 1024.0);
    upload_pipeline.last_upload_chunks = tsnap.upload_chunks;
    upload_pipeline.last_upload_bytes = tsnap.upload_bytes;
    upload_pipeline.last_read_bytes = tsnap.read_bytes;
    upload_pipeline.last_decode_bytes = tsnap.decode_bytes;
    upload_pipeline.upload_bytes_cumulative += tsnap.upload_bytes;
    upload_pipeline.upload_chunks_cumulative += tsnap.upload_chunks;
    upload_pipeline.read_bytes_cumulative += tsnap.read_bytes;
    upload_pipeline.decode_bytes_cumulative += tsnap.decode_bytes;
    upload_pipeline.last_pack_queue_latency_avg_ms = tsnap.pack_queue_lat_samples > 0
            ? double(tsnap.pack_queue_lat_total) / (USEC_PER_MS * double(tsnap.pack_queue_lat_samples))
            : 0.0;
    upload_pipeline.last_pack_queue_latency_max_ms = double(tsnap.pack_queue_lat_max) / USEC_PER_MS;
    upload_pipeline.last_upload_queue_latency_avg_ms = tsnap.upload_queue_lat_samples > 0
            ? double(tsnap.upload_queue_lat_total) / (USEC_PER_MS * double(tsnap.upload_queue_lat_samples))
            : 0.0;
    upload_pipeline.last_upload_queue_latency_max_ms = double(tsnap.upload_queue_lat_max) / USEC_PER_MS;
    upload_pipeline.last_pack_mutex_wait_avg_ms = tsnap.mutex_wait_samples > 0
            ? double(tsnap.mutex_wait_total) / (USEC_PER_MS * double(tsnap.mutex_wait_samples))
            : 0.0;
    upload_pipeline.last_pack_mutex_wait_max_ms = double(tsnap.mutex_wait_max) / USEC_PER_MS;
    upload_pipeline.last_thread_wakes = tsnap.thread_wakes;
    upload_pipeline.last_thread_dequeues = tsnap.thread_dequeues;
    upload_pipeline.last_thread_snapshots = tsnap.thread_snapshots;
    upload_pipeline.last_thread_enqueue_uploads = tsnap.thread_enqueue_uploads;

    analytics[AK::pack_avg_ms] = upload_pipeline.last_pack_avg_ms;
    analytics[AK::pack_max_ms] = upload_pipeline.last_pack_max_ms;
    analytics[AK::pack_jobs_completed] = (int)upload_pipeline.last_pack_jobs;
    analytics[AK::upload_mb_this_frame] = upload_pipeline.last_upload_mb;
    analytics[AK::upload_chunks_this_frame] = (int)upload_pipeline.last_upload_chunks;
    // BV-04a: release-build byte counters (per frame, and cumulative for diffing) plus the
    // cumulative read/decode/pack/upload latency histograms. See PackTelemetry.
    analytics[AK::upload_bytes_this_frame] = static_cast<int64_t>(upload_pipeline.last_upload_bytes);
    analytics[AK::read_bytes_this_frame] = static_cast<int64_t>(upload_pipeline.last_read_bytes);
    analytics[AK::decode_bytes_this_frame] = static_cast<int64_t>(upload_pipeline.last_decode_bytes);
    analytics[AK::upload_bytes_total] = static_cast<int64_t>(upload_pipeline.upload_bytes_cumulative);
    analytics[AK::upload_chunks_total] = static_cast<int64_t>(upload_pipeline.upload_chunks_cumulative);
    analytics[AK::read_bytes_total] = static_cast<int64_t>(upload_pipeline.read_bytes_cumulative);
    analytics[AK::decode_bytes_total] = static_cast<int64_t>(upload_pipeline.decode_bytes_cumulative);
    {
        const StreamingUploadPipeline::PackTelemetry &telemetry = upload_pipeline.telemetry;
        frame->read_latency = telemetry.read_latency.read();
        frame->decode_latency = telemetry.decode_latency.read();
        frame->pack_latency = telemetry.pack_latency.read();
        frame->upload_latency = telemetry.upload_latency.read();
    }
    analytics[AK::pack_queue_latency_avg_ms] = upload_pipeline.last_pack_queue_latency_avg_ms;
    analytics[AK::pack_queue_latency_max_ms] = upload_pipeline.last_pack_queue_latency_max_ms;
    analytics[AK::upload_queue_latency_avg_ms] = upload_pipeline.last_upload_queue_latency_avg_ms;
    analytics[AK::upload_queue_latency_max_ms] = upload_pipeline.last_upload_queue_latency_max_ms;
    analytics[AK::pack_mutex_wait_avg_ms] = upload_pipeline.last_pack_mutex_wait_avg_ms;
    analytics[AK::pack_mutex_wait_max_ms] = upload_pipeline.last_pack_mutex_wait_max_ms;
    analytics[AK::pack_thread_wakes] = (int)upload_pipeline.last_thread_wakes;
    analytics[AK::pack_thread_dequeues] = (int)upload_pipeline.last_thread_dequeues;
    analytics[AK::pack_thread_snapshots] = (int)upload_pipeline.last_thread_snapshots;
    analytics[AK::pack_thread_enqueue_uploads] = (int)upload_pipeline.last_thread_enqueue_uploads;
    analytics[AK::pack_jobs_in_flight] = (int)upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    analytics[AK::avg_chunk_load_time_ms] = upload_pipeline.last_pack_avg_ms;
    analytics[AK::camera_velocity] = visibility.camera_tracker.velocity.length();
    analytics[AK::prefetch_hits] = (int)scheduler.last_prefetch_enqueued_count;
    const int64_t prefetch_miss_count =
            int64_t(scheduler.last_prefetch_candidate_count) - int64_t(scheduler.last_prefetch_enqueued_count);
    analytics[AK::prefetch_misses] = (int)(prefetch_miss_count > 0 ? prefetch_miss_count : 0);
    analytics[AK::evicted_bytes_total_mb] = double(budget.evicted_bytes_total) / (1024.0 * 1024.0);
    const float cull_effectiveness = visibility.culling_stats.total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks)
            : 0.0f;
    analytics[AK::cull_effectiveness] = cull_effectiveness;
    analytics[AK::primary_spatial_chunking_enabled] = primary_chunk_layout_metrics.spatial_partition_enabled;
    analytics[AK::primary_chunk_source_index_count] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    analytics[AK::primary_chunk_avg_radius_ratio] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    analytics[AK::primary_chunk_max_radius_ratio] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    analytics[AK::primary_chunk_bounds_volume_ratio] = primary_chunk_layout_metrics.bounds_volume_ratio;
    frame->layout_hint_strict_mode = _layout_hint_strict_validation_enabled();
    if (const LayoutHintFailureState *layout_hint_state = _layout_hint_get_state(this)) {
        frame->layout_hint_counters = layout_hint_state->counters;
        frame->layout_hint_counters_valid = true;
    }

    uint32_t pack_queue_depth = 0;
    uint32_t upload_queue_depth = 0;
    const uint32_t sync_fallback_queue_depth = _get_sync_fallback_queue_depth();
    upload_pipeline.get_pending_queue_depths_cached(pack_queue_depth, upload_queue_depth);
    const uint32_t analytics_pack_jobs_in_flight = upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    const bool pack_inflight_saturated = upload_pipeline.max_pack_jobs_in_flight > 0 &&
            analytics_pack_jobs_in_flight >= upload_pipeline.max_pack_jobs_in_flight;
    const bool sync_backpressure =
            scheduler.last_sync_fallback_dropped_count > 0 || scheduler.last_sync_fallback_stalled_count > 0;
    const bool analytics_visible_eviction_active = diagnostics.visible_evict_fallback_attempts > 0 &&
            diagnostics.visible_evict_fallback_successes < diagnostics.visible_evict_fallback_attempts;
    StreamingQueuePressureController::PressureSample queue_pressure_sample;
    queue_pressure_sample.pack_queue_depth = pack_queue_depth;
    queue_pressure_sample.upload_queue_depth = upload_queue_depth;
    queue_pressure_sample.sync_fallback_queue_depth = sync_fallback_queue_depth;
    queue_pressure_sample.pack_jobs_in_flight = analytics_pack_jobs_in_flight;
    queue_pressure_sample.pack_inflight_saturated = pack_inflight_saturated;
    queue_pressure_sample.upload_frame_cap_hit = upload_pipeline.upload_frame_cap_hit_this_frame;
    queue_pressure_sample.upload_bandwidth_cap_hit = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    queue_pressure_sample.chunk_load_cap_hit = upload_pipeline.chunk_load_cap_hit_this_frame;
    queue_pressure_sample.vram_chunk_cap_hit = budget.vram_chunk_cap_hit_this_frame;
    queue_pressure_sample.sync_backpressure = sync_backpressure;
    queue_pressure_sample.visible_eviction_active = analytics_visible_eviction_active;
    const StreamingQueuePressureController::PressureSummary queue_pressure_summary =
            StreamingQueuePressureController::summarize_checked(queue_pressure_sample, "GaussianStreamingSystem::end_frame.analytics");
    StreamingQueuePressureController::latch_summary(queue_pressure_summary,
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason);
    StreamingQueuePressureController::validate_latched_state(
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason,
            "GaussianStreamingSystem::end_frame.analytics");
    analytics[AK::scheduler_pack_queue_depth] = (int)pack_queue_depth;
    analytics[AK::scheduler_upload_queue_depth] = (int)upload_queue_depth;
    analytics[AK::pending_uploads] = (int)upload_queue_depth;
    analytics[AK::pending_upload_reserved_bytes] = static_cast<int64_t>(_get_pending_upload_bytes_for_diagnostics());
    analytics[AK::pending_upload_reserved_slots] = static_cast<int64_t>(get_pending_upload_retirement_slots());
    analytics[AK::pending_upload_retirement_tickets] = static_cast<int64_t>(pending_upload_retirements.size());
    analytics[AK::scheduler_visible_scan_chunks] = (int)scheduler.last_visible_scan_count;
    analytics[AK::scheduler_load_candidates] = (int)scheduler.last_load_candidate_count;
    analytics[AK::scheduler_primary_eviction_scan_chunks] = (int)scheduler.last_primary_eviction_scan_count;
    analytics[AK::scheduler_primary_eviction_candidates] = (int)scheduler.last_primary_eviction_candidate_count;
    analytics[AK::scheduler_non_primary_scan_chunks] = (int)scheduler.last_non_primary_scan_count;
    analytics[AK::scheduler_non_primary_eviction_candidates] = (int)scheduler.last_non_primary_eviction_candidate_count;
    analytics[AK::scheduler_prefetch_scan_chunks] = (int)scheduler.last_prefetch_scan_count;
    analytics[AK::scheduler_prefetch_candidates] = (int)scheduler.last_prefetch_candidate_count;
    analytics[AK::scheduler_prefetch_upload_pending_skips] =
            static_cast<int64_t>(scheduler.last_prefetch_upload_pending_skip_count);
    analytics[AK::scheduler_prefetch_enqueue_headroom_stalls] =
            static_cast<int64_t>(scheduler.last_prefetch_enqueue_headroom_stall_count);
    scheduler.last_cpu_total_attributed_ms =
            scheduler.last_visibility_cpu_ms +
            scheduler.last_load_cpu_ms +
            scheduler.last_build_visible_cpu_ms +
            scheduler.last_prefetch_cpu_ms;
    scheduler.last_cpu_unattributed_ms = MAX(
            0.0, scheduler.last_update_cpu_ms - scheduler.last_cpu_total_attributed_ms);
    analytics[AK::scheduler_update_cpu_ms] = scheduler.last_update_cpu_ms;
    analytics[AK::scheduler_visibility_cpu_ms] = scheduler.last_visibility_cpu_ms;
    analytics[AK::scheduler_load_cpu_ms] = scheduler.last_load_cpu_ms;
    analytics[AK::scheduler_build_visible_cpu_ms] = scheduler.last_build_visible_cpu_ms;
    analytics[AK::scheduler_prefetch_cpu_ms] = scheduler.last_prefetch_cpu_ms;
    analytics[AK::scheduler_cpu_total_attributed_ms] = scheduler.last_cpu_total_attributed_ms;
    analytics[AK::scheduler_cpu_unattributed_ms] = scheduler.last_cpu_unattributed_ms;
    analytics[AK::scheduler_visible_scan_budget_effective] =
            static_cast<int64_t>(scheduler.last_visible_scan_budget_effective);
    // #1086 needed-set / forward-progress telemetry (see streaming_queue_pressure_controller.h).
    analytics[AK::needed_set_measured] = scheduler.last_needed_set_measured;
    analytics[AK::needed_chunks_completed] = static_cast<int64_t>(scheduler.last_needed_chunks_completed);
    analytics[AK::needed_set_net_progress] = static_cast<int64_t>(scheduler.last_needed_set_net_progress);
    analytics[AK::needed_set_displacement_debt] = static_cast<int64_t>(scheduler.needed_set_displacement_debt);
    // #1178: failed chunk payload reads (cumulative). A failing chunk stays needed and unserved
    // while it backs off, so this names the cause when needed_unserved_chunks does not drain.
    analytics[AK::chunk_payload_read_failures] = static_cast<int64_t>(diagnostics.chunk_payload_read_failures);
    analytics[AK::scheduler_visible_scan_starvation_eligible] = scheduler.last_visible_scan_starvation_eligible;
    analytics[AK::needed_chunks] = static_cast<int64_t>(scheduler.last_needed_chunk_count);
    analytics[AK::needed_resident_chunks] = static_cast<int64_t>(scheduler.last_needed_resident_chunk_count);
    analytics[AK::needed_unserved_chunks] = static_cast<int64_t>(scheduler.last_needed_unserved_chunk_count);
    analytics[AK::needed_set_stall_seconds] = scheduler.needed_set_stall_seconds;
    analytics[AK::needed_set_stalled] = scheduler.needed_set_stall_seconds >=
            StreamingQueuePressureController::NEEDED_SET_STALL_THRESHOLD_SECONDS;
    analytics[AK::scheduler_visible_scan_had_capacity] = scheduler.last_visible_scan_had_capacity;
    analytics[AK::scheduler_visible_scan_starved] = scheduler.last_visible_scan_starved;
    analytics[AK::scheduler_prefetch_scan_budget_effective] =
            static_cast<int64_t>(scheduler.last_prefetch_scan_budget_effective);
    analytics[AK::scheduler_queue_pressure_scan_throttle_active] =
            scheduler.queue_pressure_candidate_scan_throttle_active;
    analytics[AK::scheduler_queue_pressure_scan_throttle_queue_depth] =
            static_cast<int64_t>(scheduler.queue_pressure_candidate_scan_throttle_queue_depth);
    analytics[AK::scheduler_queue_pressure_scan_throttle_enabled] =
            scheduler.queue_pressure_candidate_scan_throttle_enabled;
    analytics[AK::scheduler_force_sync_fallback_due_to_async_stall] =
            scheduler.force_sync_fallback_due_to_async_stall;
    analytics[AK::scheduler_sync_fallback_queue_depth] = (int)sync_fallback_queue_depth;
    analytics[AK::scheduler_sync_fallback_enqueued] = (int)scheduler.last_sync_fallback_enqueued_count;
    analytics[AK::scheduler_sync_fallback_drained] = (int)scheduler.last_sync_fallback_drained_count;
    analytics[AK::scheduler_sync_fallback_dropped] = (int)scheduler.last_sync_fallback_dropped_count;
    analytics[AK::scheduler_sync_fallback_stalled] = (int)scheduler.last_sync_fallback_stalled_count;
    analytics[AK::scheduler_sync_fallback_load_budget] = (int)scheduler.max_sync_fallback_loads_per_frame;
    analytics[AK::scheduler_sync_fallback_cpu_ms] = scheduler.last_sync_fallback_cpu_ms;
    analytics[AK::sync_promoted_pack_jobs_this_frame] = (int)upload_pipeline.last_sync_promoted_pack_jobs;
    analytics[AK::sync_promoted_pack_jobs_total] = static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total);
    StreamingTelemetryAdapter::QueuePressureSnapshot queue_pressure_snapshot;
    queue_pressure_snapshot.active = upload_pipeline.queue_pressure_active;
    queue_pressure_snapshot.source = upload_pipeline.queue_pressure_source;
    queue_pressure_snapshot.reason = upload_pipeline.queue_pressure_reason;
    queue_pressure_snapshot.backlog_depth = queue_pressure_summary.backlog_depth;
    queue_pressure_snapshot.total_pending_chunks = queue_pressure_summary.total_pending_chunks;
    queue_pressure_snapshot.pack_source_active = queue_pressure_summary.pack_source_active;
    queue_pressure_snapshot.upload_source_active = queue_pressure_summary.upload_source_active;
    queue_pressure_snapshot.sync_source_active = queue_pressure_summary.sync_source_active;
    frame->analytics_queue_pressure = queue_pressure_snapshot;

    analytics[AK::visible_evict_fallback_attempts] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    analytics[AK::visible_evict_fallback_successes] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);
    analytics[AK::prefetch_visible_eviction_refusals] =
            static_cast<int64_t>(diagnostics.prefetch_visible_eviction_refusals);

    _capture_streaming_diagnostics(*frame, pack_queue_depth, upload_queue_depth, sync_fallback_queue_depth);

    MutexLock lock(analytics_frame_mutex);
    analytics_frame = std::move(frame);
}

void GaussianStreamingSystem::_capture_streaming_diagnostics(StreamingAnalyticsFrame &r_frame,
        uint32_t pack_queue_depth, uint32_t upload_queue_depth, uint32_t sync_fallback_queue_depth) {
    StreamingAnalyticsFrame::DiagnosticsValues &diagnostics_values = r_frame.diagnostics;
    using DK = StreamingAnalyticsFrame::DiagnosticsKey;

    String runtime_reason;
    const bool runtime_ready = is_runtime_ready(&runtime_reason);
    const uint32_t total_chunks = visibility.culling_stats.total_chunks;
    const uint32_t visible_chunks = visibility.culling_stats.visible_chunks;
    const uint32_t loaded_chunks = budget.loaded_chunks_count;
    const uint32_t reserved_upload_slots = get_pending_upload_retirement_slots();
    const uint64_t reserved_upload_bytes = _get_pending_upload_bytes_for_diagnostics();
    const uint32_t load_candidates = scheduler.last_load_candidate_count;
    const uint32_t loaded_this_frame = budget.chunks_loaded_this_frame;
    const uint32_t upload_chunks_this_frame = upload_pipeline.last_upload_chunks;
    const uint32_t visible_splats = get_visible_count();
    const float cull_effectiveness = total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(total_chunks)
            : 0.0f;
    const bool upload_frame_cap_hit = upload_pipeline.upload_frame_cap_hit_this_frame;
    const bool upload_slice_cap_hit = upload_pipeline.upload_slice_cap_hit_this_frame;
    const bool upload_bandwidth_cap_hit = upload_pipeline.upload_bandwidth_cap_hit_this_frame;
    const bool chunk_load_cap_hit = upload_pipeline.chunk_load_cap_hit_this_frame;
    const bool vram_chunk_cap_hit = budget.vram_chunk_cap_hit_this_frame;
    const uint32_t current_pack_jobs_in_flight = upload_pipeline.pack_jobs_in_flight.load(std::memory_order_relaxed);
    const bool pack_inflight_saturated = upload_pipeline.max_pack_jobs_in_flight > 0 &&
            current_pack_jobs_in_flight >= upload_pipeline.max_pack_jobs_in_flight;
    const bool sync_backpressure =
            scheduler.last_sync_fallback_dropped_count > 0 || scheduler.last_sync_fallback_stalled_count > 0;
    const bool visible_eviction_active = diagnostics.visible_evict_fallback_attempts > 0 &&
            diagnostics.visible_evict_fallback_successes < diagnostics.visible_evict_fallback_attempts;
    StreamingQueuePressureController::PressureSample queue_pressure_sample;
    queue_pressure_sample.pack_queue_depth = pack_queue_depth;
    queue_pressure_sample.upload_queue_depth = upload_queue_depth;
    queue_pressure_sample.sync_fallback_queue_depth = sync_fallback_queue_depth;
    queue_pressure_sample.pack_jobs_in_flight = current_pack_jobs_in_flight;
    queue_pressure_sample.pack_inflight_saturated = pack_inflight_saturated;
    queue_pressure_sample.upload_frame_cap_hit = upload_frame_cap_hit;
    queue_pressure_sample.upload_bandwidth_cap_hit = upload_bandwidth_cap_hit;
    queue_pressure_sample.chunk_load_cap_hit = chunk_load_cap_hit;
    queue_pressure_sample.vram_chunk_cap_hit = vram_chunk_cap_hit;
    queue_pressure_sample.sync_backpressure = sync_backpressure;
    queue_pressure_sample.visible_eviction_active = visible_eviction_active;
    const StreamingQueuePressureController::PressureSummary queue_pressure_summary =
            StreamingQueuePressureController::summarize_checked(queue_pressure_sample,
                    "GaussianStreamingSystem::_capture_streaming_diagnostics");
    StreamingQueuePressureController::latch_summary(queue_pressure_summary,
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason);
    StreamingQueuePressureController::validate_latched_state(
            upload_pipeline.queue_pressure_active, upload_pipeline.queue_pressure_source, upload_pipeline.queue_pressure_reason,
            "GaussianStreamingSystem::_capture_streaming_diagnostics");
    const bool queue_pressure_active = upload_pipeline.queue_pressure_active;
    const String queue_pressure_source = upload_pipeline.queue_pressure_source;
    const String queue_pressure_reason = upload_pipeline.queue_pressure_reason;
    VRAMBudgetConfig active_vram_caps;
    if (budget.vram_regulator.is_valid()) {
        active_vram_caps = budget.vram_regulator->get_config();
    }
    const int64_t effective_vram_budget_mb = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.budget_mb) : 0;
    const int64_t effective_vram_min_chunks = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.min_chunks) : 0;
    const int64_t effective_vram_max_chunks = budget.vram_regulator.is_valid() ? static_cast<int64_t>(active_vram_caps.max_chunks) : 0;

    if (!runtime_ready) {
        diagnostics.init_invalid_frames++;
    } else {
        diagnostics.init_invalid_frames = 0;
    }

    if (runtime_ready && total_chunks > 0 && visible_chunks == 0) {
        diagnostics.culling_empty_frames++;
    } else {
        diagnostics.culling_empty_frames = 0;
    }

    const bool loaded_progress = loaded_this_frame > 0 || loaded_chunks > diagnostics.last_loaded_chunks;
    const bool scheduler_stalled = runtime_ready &&
            total_chunks > 0 &&
            visible_chunks > 0 &&
            load_candidates > 0 &&
            !loaded_progress &&
            pack_queue_depth == 0 &&
            upload_queue_depth == 0 &&
            sync_fallback_queue_depth == 0;
    if (scheduler_stalled) {
        diagnostics.scheduler_stall_frames++;
    } else {
        diagnostics.scheduler_stall_frames = 0;
    }

    const bool upload_stalled = runtime_ready &&
            (pack_queue_depth > 0 || upload_queue_depth > 0 || reserved_upload_slots > 0) &&
            upload_chunks_this_frame == 0 &&
            loaded_this_frame == 0;
    if (upload_stalled) {
        diagnostics.upload_stall_frames++;
    } else {
        diagnostics.upload_stall_frames = 0;
    }
    diagnostics.update_upload_stall_wall_clock(upload_stalled, OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0);

    const bool sync_fallback_stalled = runtime_ready &&
            !upload_pipeline.pack_thread_running.load() &&
            sync_fallback_queue_depth > 0 &&
            loaded_this_frame == 0;
    if (sync_fallback_stalled) {
        diagnostics.sync_fallback_stall_frames++;
    } else {
        diagnostics.sync_fallback_stall_frames = 0;
    }
    if (queue_pressure_active) {
        diagnostics.queue_pressure_frames++;
    } else {
        diagnostics.queue_pressure_frames = 0;
    }
    if (vram_chunk_cap_hit) {
        diagnostics.vram_cap_hit_frames++;
    } else {
        diagnostics.vram_cap_hit_frames = 0;
    }

    // Sustained pressure categorization: classify the dominant bottleneck
    // so long-soak diagnostics can distinguish bandwidth limits, residency
    // limits, churn, and pipeline stalls.
    {
        const uint32_t chunks_evicted = eviction_controller.get_chunks_evicted_this_frame();
        const uint32_t visible_evicted = eviction_controller.get_visible_chunks_evicted_this_frame();
        const bool bandwidth_signal = upload_frame_cap_hit || upload_bandwidth_cap_hit || chunk_load_cap_hit;
        const bool residency_signal = vram_chunk_cap_hit && chunks_evicted > 0;
        const bool churn_signal = visible_evicted > 0 && loaded_this_frame > 0;
        const bool stall_signal = upload_stalled || sync_fallback_stalled || scheduler_stalled;

        const uint32_t active_signals = uint32_t(bandwidth_signal) + uint32_t(residency_signal) +
                uint32_t(churn_signal) + uint32_t(stall_signal);

        if (active_signals > 1) {
            diagnostics.pressure_category = "combined";
            diagnostics.sustained_pressure_frames++;
        } else if (churn_signal) {
            diagnostics.pressure_category = "churn";
            diagnostics.sustained_pressure_frames++;
        } else if (residency_signal) {
            diagnostics.pressure_category = "residency_limited";
            diagnostics.sustained_pressure_frames++;
        } else if (bandwidth_signal) {
            diagnostics.pressure_category = "bandwidth_limited";
            diagnostics.sustained_pressure_frames++;
        } else if (stall_signal) {
            diagnostics.pressure_category = "pipeline_stall";
            diagnostics.sustained_pressure_frames++;
        } else {
            if (diagnostics.sustained_pressure_frames > 0) {
                if (diagnostics.sustained_pressure_frames <= DiagnosticsState::SUSTAINED_PRESSURE_COOLDOWN_FRAMES) {
                    diagnostics.sustained_pressure_frames = 0;
                    diagnostics.pressure_category = "none";
                } else {
                    diagnostics.sustained_pressure_frames--;
                }
            } else {
                diagnostics.pressure_category = "none";
            }
        }
    }

    String category = "ok";
    String reason = "healthy";
    if (!runtime_ready) {
        category = "init_invalid";
        reason = runtime_reason;
    } else if (budget.failed_upload_retirements > 0) {
        category = "upload_retirement_failed";
        reason = diagnostics.last_invariant_message.is_empty() ?
                vformat("upload retirements failed=%d.", static_cast<int64_t>(budget.failed_upload_retirements)) :
                diagnostics.last_invariant_message;
    } else if (diagnostics.integrity_mismatch_count > 0) {
        category = "integrity_mismatch";
        reason = diagnostics.last_integrity_mismatch_message.is_empty() ?
                vformat("integrity mismatches detected=%d.", static_cast<int64_t>(diagnostics.integrity_mismatch_count)) :
                diagnostics.last_integrity_mismatch_message;
    } else if (diagnostics.culling_empty_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "culling_empty";
        reason = vformat("visible_chunks=0 for %d frames while total_chunks=%d.",
                diagnostics.culling_empty_frames, total_chunks);
    } else if (diagnostics.upload_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "upload_stalled";
        reason = vformat("upload queue stalled for %d frames (pack_queue=%d upload_queue=%d pending_retire=%d).",
                diagnostics.upload_stall_frames, pack_queue_depth, upload_queue_depth, reserved_upload_slots);
    } else if (diagnostics.sync_fallback_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "sync_fallback_stalled";
        reason = vformat("sync fallback queue stalled for %d frames (queue=%d).",
                diagnostics.sync_fallback_stall_frames, sync_fallback_queue_depth);
    } else if (diagnostics.queue_pressure_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "queue_pressure";
        reason = vformat("queue pressure active for %d frames (source=%s reason=%s pack=%d upload=%d sync=%d).",
                diagnostics.queue_pressure_frames,
                queue_pressure_source,
                queue_pressure_reason,
                pack_queue_depth,
                upload_queue_depth,
                sync_fallback_queue_depth);
    } else if (diagnostics.vram_cap_hit_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "vram_cap_hit";
        reason = vformat("VRAM chunk cap blocked loads for %d frames (loaded=%d max=%d).",
                diagnostics.vram_cap_hit_frames, loaded_chunks, effective_vram_max_chunks);
    } else if (diagnostics.scheduler_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES) {
        category = "scheduler_stalled";
        reason = vformat("load candidates present but no chunk progress for %d frames.",
                diagnostics.scheduler_stall_frames);
    }

    const bool has_failure = category != "ok";
    const String fingerprint = vformat(
            "%s|ready=%s|chunks=%d/%d/%d|visible_splats=%d|cand=%d|pack_q=%d|upload_q=%d|retire_q=%d|sync_q=%d|pack_flight=%d|total_pend=%d|load_frame=%d|upload_frame=%d|caps=%d/%d/%d|vram=%d/%d|hits=%d%d%d%d%d%d|qsrc=%s|qwhy=%s|atlas=%d|req=%d|inv=%d/%d/%d|sync_promo=%d",
            category,
            runtime_ready ? "1" : "0",
            loaded_chunks,
            visible_chunks,
            total_chunks,
            visible_splats,
            load_candidates,
            pack_queue_depth,
            upload_queue_depth,
            reserved_upload_slots,
            sync_fallback_queue_depth,
            current_pack_jobs_in_flight,
            queue_pressure_summary.total_pending_chunks,
            loaded_this_frame,
            upload_chunks_this_frame,
            upload_pipeline.effective_upload_cap_mb_per_frame,
            upload_pipeline.effective_upload_cap_mb_per_slice,
            upload_pipeline.effective_upload_cap_mb_per_second,
            effective_vram_budget_mb,
            effective_vram_max_chunks,
            upload_frame_cap_hit ? 1 : 0,
            upload_bandwidth_cap_hit ? 1 : 0,
            chunk_load_cap_hit ? 1 : 0,
            vram_chunk_cap_hit ? 1 : 0,
            queue_pressure_active ? 1 : 0,
            visible_eviction_active ? 1 : 0,
            queue_pressure_source,
            queue_pressure_reason,
            static_cast<int64_t>(global_atlas_registry.get_atlas_generation()),
            static_cast<int64_t>(asset_registry.request_generation),
            static_cast<int64_t>(diagnostics.invariant_slot_ownership_violations),
            static_cast<int64_t>(diagnostics.invariant_upload_lifecycle_violations),
            static_cast<int64_t>(diagnostics.invariant_generation_violations),
            static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total));

    if (has_failure) {
        const bool fingerprint_changed = diagnostics.last_logged_fingerprint != fingerprint;
        const bool log_due = diagnostics.last_fingerprint_log_frame == 0 ||
                (total_frame_count - diagnostics.last_fingerprint_log_frame) >= DiagnosticsState::LOG_INTERVAL_FRAMES;
        if (fingerprint_changed || log_due) {
            WARN_PRINT(vformat("[Streaming][Diag:%s] fingerprint=%s reason=%s",
                    category, fingerprint, reason));
            diagnostics.last_logged_fingerprint = fingerprint;
            diagnostics.last_fingerprint_log_frame = total_frame_count;
        }
    } else if (diagnostics.active_category != "ok") {
        WARN_PRINT(vformat("[Streaming][Diag:ok] recovered_from=%s", diagnostics.active_category));
    }

    diagnostics.active_category = category;
    diagnostics.active_reason = reason;
    diagnostics.active_fingerprint = fingerprint;
    diagnostics.last_total_chunks = total_chunks;
    diagnostics.last_visible_chunks = visible_chunks;
    diagnostics.last_loaded_chunks = loaded_chunks;

    diagnostics_values[DK::category] = category;
    diagnostics_values[DK::reason] = reason;
    diagnostics_values[DK::fingerprint] = fingerprint;
    diagnostics_values[DK::has_failure] = has_failure;
    diagnostics_values[DK::runtime_ready] = runtime_ready;
    diagnostics_values[DK::total_chunks] = static_cast<int64_t>(total_chunks);
    diagnostics_values[DK::visible_chunks] = static_cast<int64_t>(visible_chunks);
    diagnostics_values[DK::loaded_chunks] = static_cast<int64_t>(loaded_chunks);
    diagnostics_values[DK::visible_splats] = static_cast<int64_t>(visible_splats);
    diagnostics_values[DK::cull_effectiveness] = cull_effectiveness;
    diagnostics_values[DK::primary_spatial_chunking_enabled] = primary_chunk_layout_metrics.spatial_partition_enabled;
    diagnostics_values[DK::primary_chunk_source_index_count] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    diagnostics_values[DK::primary_chunk_avg_radius_ratio] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    diagnostics_values[DK::primary_chunk_max_radius_ratio] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    diagnostics_values[DK::primary_chunk_bounds_volume_ratio] = primary_chunk_layout_metrics.bounds_volume_ratio;
    diagnostics_values[DK::load_candidates] = static_cast<int64_t>(load_candidates);
    diagnostics_values[DK::scheduler_primary_eviction_scan_chunks] =
            static_cast<int64_t>(scheduler.last_primary_eviction_scan_count);
    diagnostics_values[DK::scheduler_primary_eviction_candidates] =
            static_cast<int64_t>(scheduler.last_primary_eviction_candidate_count);
    diagnostics_values[DK::scheduler_non_primary_scan_chunks] =
            static_cast<int64_t>(scheduler.last_non_primary_scan_count);
    diagnostics_values[DK::scheduler_non_primary_eviction_candidates] =
            static_cast<int64_t>(scheduler.last_non_primary_eviction_candidate_count);
    diagnostics_values[DK::pack_queue_depth] = static_cast<int64_t>(pack_queue_depth);
    diagnostics_values[DK::upload_queue_depth] = static_cast<int64_t>(upload_queue_depth);
    diagnostics_values[DK::pending_upload_reserved_slots] = static_cast<int64_t>(reserved_upload_slots);
    diagnostics_values[DK::pending_upload_reserved_bytes] = static_cast<int64_t>(reserved_upload_bytes);
    diagnostics_values[DK::pending_upload_retirement_tickets] = static_cast<int64_t>(pending_upload_retirements.size());
    diagnostics_values[DK::sync_fallback_queue_depth] = static_cast<int64_t>(sync_fallback_queue_depth);
    diagnostics_values[DK::pack_jobs_in_flight] = static_cast<int64_t>(current_pack_jobs_in_flight);
    diagnostics_values[DK::total_pending_chunks] = static_cast<int64_t>(queue_pressure_summary.total_pending_chunks);
    diagnostics_values[DK::visible_eviction_active] = visible_eviction_active;
    StreamingTelemetryAdapter::QueuePressureSnapshot queue_pressure_snapshot;
    queue_pressure_snapshot.active = queue_pressure_active;
    queue_pressure_snapshot.source = queue_pressure_source;
    queue_pressure_snapshot.reason = queue_pressure_reason;
    queue_pressure_snapshot.backlog_depth = queue_pressure_summary.backlog_depth;
    queue_pressure_snapshot.total_pending_chunks = queue_pressure_summary.total_pending_chunks;
    queue_pressure_snapshot.pack_source_active = queue_pressure_summary.pack_source_active;
    queue_pressure_snapshot.upload_source_active = queue_pressure_summary.upload_source_active;
    queue_pressure_snapshot.sync_source_active = queue_pressure_summary.sync_source_active;
    r_frame.diagnostics_queue_pressure = queue_pressure_snapshot;
    diagnostics_values[DK::scheduler_visible_scan_budget_effective] =
            static_cast<int64_t>(scheduler.last_visible_scan_budget_effective);
    diagnostics_values[DK::scheduler_prefetch_scan_budget_effective] =
            static_cast<int64_t>(scheduler.last_prefetch_scan_budget_effective);
    diagnostics_values[DK::scheduler_prefetch_upload_pending_skips] =
            static_cast<int64_t>(scheduler.last_prefetch_upload_pending_skip_count);
    diagnostics_values[DK::scheduler_prefetch_enqueue_headroom_stalls] =
            static_cast<int64_t>(scheduler.last_prefetch_enqueue_headroom_stall_count);
    diagnostics_values[DK::scheduler_queue_pressure_scan_throttle_active] =
            scheduler.queue_pressure_candidate_scan_throttle_active;
    diagnostics_values[DK::scheduler_queue_pressure_scan_throttle_queue_depth] =
            static_cast<int64_t>(scheduler.queue_pressure_candidate_scan_throttle_queue_depth);
    diagnostics_values[DK::scheduler_queue_pressure_scan_throttle_enabled] =
            scheduler.queue_pressure_candidate_scan_throttle_enabled;
    diagnostics_values[DK::scheduler_force_sync_fallback_due_to_async_stall] =
            scheduler.force_sync_fallback_due_to_async_stall;
    diagnostics_values[DK::chunks_loaded_this_frame] = static_cast<int64_t>(loaded_this_frame);
    diagnostics_values[DK::chunks_uploaded_this_frame] = static_cast<int64_t>(upload_chunks_this_frame);
    diagnostics_values[DK::retired_upload_bytes_this_frame] = static_cast<int64_t>(budget.retired_upload_bytes_this_frame);
    diagnostics_values[DK::retired_upload_slots_this_frame] = static_cast<int64_t>(budget.retired_upload_slots_this_frame);
    diagnostics_values[DK::failed_upload_retirements] = static_cast<int64_t>(budget.failed_upload_retirements);
    diagnostics_values[DK::stride_flip_dropped_upload_retirements] = static_cast<int64_t>(budget.stride_flip_dropped_upload_retirements);
    diagnostics_values[DK::chunk_payload_read_failures] = static_cast<int64_t>(diagnostics.chunk_payload_read_failures);
    diagnostics_values[DK::stale_sequence_dropped_uploads] = static_cast<int64_t>(budget.stale_sequence_dropped_uploads);
    diagnostics_values[DK::stale_sequence_dropped_upload_retirements] =
            static_cast<int64_t>(budget.stale_sequence_dropped_upload_retirements);
    diagnostics_values[DK::last_upload_completion_mode] = last_upload_completion_mode;
    diagnostics_values[DK::last_completed_upload_ticket_id] = static_cast<int64_t>(last_completed_upload_ticket_id);
    diagnostics_values[DK::sync_promoted_pack_jobs_this_frame] = static_cast<int64_t>(upload_pipeline.last_sync_promoted_pack_jobs);
    diagnostics_values[DK::sync_promoted_pack_jobs_total] = static_cast<int64_t>(upload_pipeline.sync_promoted_pack_jobs_total);
    diagnostics_values[DK::pack_thread_wakes] = static_cast<int64_t>(upload_pipeline.last_thread_wakes);
    diagnostics_values[DK::pack_thread_dequeues] = static_cast<int64_t>(upload_pipeline.last_thread_dequeues);
    diagnostics_values[DK::pack_thread_snapshots] = static_cast<int64_t>(upload_pipeline.last_thread_snapshots);
    diagnostics_values[DK::pack_thread_enqueue_uploads] = static_cast<int64_t>(upload_pipeline.last_thread_enqueue_uploads);
    diagnostics_values[DK::cap_tier_preset] = budget.vram_regulator.is_valid()
            ? active_vram_caps.cap_tier_preset
            : upload_pipeline.cap_tier_preset;
    diagnostics_values[DK::cap_tier_active] = budget.vram_regulator.is_valid()
            ? active_vram_caps.cap_tier_active
            : upload_pipeline.cap_tier_active;
    diagnostics_values[DK::effective_upload_cap_mb_per_frame] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_frame);
    diagnostics_values[DK::effective_upload_cap_mb_per_slice] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_slice);
    diagnostics_values[DK::effective_upload_cap_mb_per_second] = static_cast<int64_t>(upload_pipeline.effective_upload_cap_mb_per_second);
    diagnostics_values[DK::effective_vram_budget_mb] = effective_vram_budget_mb;
    diagnostics_values[DK::effective_vram_min_chunks] = effective_vram_min_chunks;
    diagnostics_values[DK::effective_vram_max_chunks] = effective_vram_max_chunks;
    diagnostics_values[DK::requested_vram_budget_mb] = budget.vram_regulator.is_valid()
            ? static_cast<int64_t>(active_vram_caps.requested_budget_mb)
            : int64_t(0);
    diagnostics_values[DK::cap_source_upload_mb_per_frame] = upload_pipeline.cap_source_upload_mb_per_frame;
    diagnostics_values[DK::cap_source_upload_mb_per_slice] = upload_pipeline.cap_source_upload_mb_per_slice;
    diagnostics_values[DK::cap_source_upload_mb_per_second] = upload_pipeline.cap_source_upload_mb_per_second;
    diagnostics_values[DK::cap_source_vram_budget_mb] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_budget_mb
            : String("project_default");
    diagnostics_values[DK::requested_cap_source_vram_budget_mb] = budget.vram_regulator.is_valid()
            ? active_vram_caps.requested_source_budget_mb
            : String("project_default");
    diagnostics_values[DK::cap_source_vram_min_chunks] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_min_chunks
            : String("project_default");
    diagnostics_values[DK::cap_source_vram_max_chunks] = budget.vram_regulator.is_valid()
            ? active_vram_caps.source_max_chunks
            : String("project_default");
    diagnostics_values[DK::vram_budget_capacity_verified] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_capacity_verified
            : false;
    diagnostics_values[DK::vram_budget_unknown_capacity_fallback] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_uses_unknown_capacity_fallback
            : false;
    diagnostics_values[DK::vram_budget_unverified] = budget.vram_regulator.is_valid()
            ? active_vram_caps.budget_unverified
            : false;
    diagnostics_values[DK::upload_frame_cap_hit] = upload_frame_cap_hit;
    diagnostics_values[DK::upload_slice_cap_hit] = upload_slice_cap_hit;
    diagnostics_values[DK::upload_bandwidth_cap_hit] = upload_bandwidth_cap_hit;
    diagnostics_values[DK::chunk_load_cap_hit] = chunk_load_cap_hit;
    diagnostics_values[DK::vram_chunk_cap_hit] = vram_chunk_cap_hit;
    diagnostics_values[DK::queue_pressure_frames] = static_cast<int64_t>(diagnostics.queue_pressure_frames);
    diagnostics_values[DK::vram_cap_hit_frames] = static_cast<int64_t>(diagnostics.vram_cap_hit_frames);
    diagnostics_values[DK::init_invalid_frames] = static_cast<int64_t>(diagnostics.init_invalid_frames);
    diagnostics_values[DK::culling_empty_frames] = static_cast<int64_t>(diagnostics.culling_empty_frames);
    diagnostics_values[DK::scheduler_stall_frames] = static_cast<int64_t>(diagnostics.scheduler_stall_frames);
    diagnostics_values[DK::upload_stall_frames] = static_cast<int64_t>(diagnostics.upload_stall_frames);
    diagnostics_values[DK::upload_stall_wall_ms] = double(diagnostics.upload_stall_wall_usec) / 1000.0;
    diagnostics_values[DK::sync_fallback_stall_frames] = static_cast<int64_t>(diagnostics.sync_fallback_stall_frames);
    diagnostics_values[DK::sustained_pressure_frames] = static_cast<int64_t>(diagnostics.sustained_pressure_frames);
    diagnostics_values[DK::pressure_category] = diagnostics.pressure_category;
    diagnostics_values[DK::scheduler_update_cpu_ms] = scheduler.last_update_cpu_ms;
    diagnostics_values[DK::scheduler_visibility_cpu_ms] = scheduler.last_visibility_cpu_ms;
    diagnostics_values[DK::scheduler_load_cpu_ms] = scheduler.last_load_cpu_ms;
    diagnostics_values[DK::scheduler_build_visible_cpu_ms] = scheduler.last_build_visible_cpu_ms;
    diagnostics_values[DK::scheduler_prefetch_cpu_ms] = scheduler.last_prefetch_cpu_ms;
    diagnostics_values[DK::scheduler_sync_fallback_cpu_ms] = scheduler.last_sync_fallback_cpu_ms;
    diagnostics_values[DK::scheduler_cpu_total_attributed_ms] = scheduler.last_cpu_total_attributed_ms;
    diagnostics_values[DK::scheduler_cpu_unattributed_ms] = scheduler.last_cpu_unattributed_ms;
    diagnostics_values[DK::pack_queue_latency_avg_ms] = upload_pipeline.last_pack_queue_latency_avg_ms;
    diagnostics_values[DK::pack_queue_latency_max_ms] = upload_pipeline.last_pack_queue_latency_max_ms;
    diagnostics_values[DK::upload_queue_latency_avg_ms] = upload_pipeline.last_upload_queue_latency_avg_ms;
    diagnostics_values[DK::upload_queue_latency_max_ms] = upload_pipeline.last_upload_queue_latency_max_ms;
    diagnostics_values[DK::pack_mutex_wait_avg_ms] = upload_pipeline.last_pack_mutex_wait_avg_ms;
    diagnostics_values[DK::pack_mutex_wait_max_ms] = upload_pipeline.last_pack_mutex_wait_max_ms;
    diagnostics_values[DK::visible_evict_fallback_attempts] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    diagnostics_values[DK::visible_evict_fallback_successes] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);
    diagnostics_values[DK::prefetch_visible_eviction_refusals] =
            static_cast<int64_t>(diagnostics.prefetch_visible_eviction_refusals);
    diagnostics_values[DK::invariant_slot_ownership_violations] =
            static_cast<int64_t>(diagnostics.invariant_slot_ownership_violations);
    diagnostics_values[DK::invariant_upload_lifecycle_violations] =
            static_cast<int64_t>(diagnostics.invariant_upload_lifecycle_violations);
    diagnostics_values[DK::invariant_generation_violations] =
            static_cast<int64_t>(diagnostics.invariant_generation_violations);
    diagnostics_values[DK::integrity_mismatch_count] =
            static_cast<int64_t>(diagnostics.integrity_mismatch_count);
    diagnostics_values[DK::last_invariant_context] = diagnostics.last_invariant_context;
    diagnostics_values[DK::last_invariant_message] = diagnostics.last_invariant_message;
    diagnostics_values[DK::last_integrity_mismatch_message] = diagnostics.last_integrity_mismatch_message;
    diagnostics_values[DK::atlas_generation] = static_cast<int64_t>(global_atlas_registry.get_atlas_generation());
    diagnostics_values[DK::request_generation] = static_cast<int64_t>(asset_registry.request_generation);
}

Dictionary GaussianStreamingSystem::get_task_debug_state() const {
    if (memory_stream_proxy.is_null()) {
        return Dictionary();
    }
    return memory_stream_proxy->get_task_debug_state();
}

Dictionary GaussianStreamingSystem::get_streaming_analytics() const {
    const std::shared_ptr<StreamingAnalyticsFrame> frame = get_analytics_frame();
    return frame ? frame->resolve() : Dictionary();
}

std::shared_ptr<StreamingAnalyticsFrame> GaussianStreamingSystem::get_analytics_frame() const {
    MutexLock lock(analytics_frame_mutex);
    return analytics_frame;
}

Dictionary GaussianStreamingSystem::get_chunk_culling_stats() const {
    Dictionary stats;
    const bool strict_layout_validation = _layout_hint_strict_validation_enabled();
    const Dictionary layout_hint_validation = _layout_hint_build_snapshot(this, strict_layout_validation);
    stats["total_chunks"] = visibility.culling_stats.total_chunks;
    stats["visible_chunks"] = visibility.culling_stats.visible_chunks;
    stats["frustum_culled_chunks"] = visibility.culling_stats.frustum_culled_chunks;
    stats["distance_culled_chunks"] = visibility.culling_stats.distance_culled_chunks;
    stats["load_distance_limit"] = visibility.load_distance_limit;
    stats["loaded_chunks"] = visibility.culling_stats.loaded_chunks;
    stats["resident_chunks"] = visibility.culling_stats.resident_chunks;
    stats["visibility_flag_reset_scan_count"] = visibility.culling_stats.visibility_flag_reset_scan_count;
    stats["lod_parameter_update_scan_count"] = visibility.culling_stats.lod_parameter_update_scan_count;
    stats["lod_blend_update_scan_count"] = visibility.culling_stats.lod_blend_update_scan_count;
    stats["primary_eviction_scan_count"] = static_cast<int64_t>(scheduler.last_primary_eviction_scan_count);
    stats["primary_eviction_candidate_count"] = static_cast<int64_t>(scheduler.last_primary_eviction_candidate_count);
    stats["non_primary_eviction_scan_count"] = static_cast<int64_t>(scheduler.last_non_primary_scan_count);
    stats["non_primary_eviction_candidate_count"] = static_cast<int64_t>(scheduler.last_non_primary_eviction_candidate_count);
    stats["atlas_published_chunks"] = global_atlas_registry.get_atlas_published_chunks();
    stats["culling_enabled"] = visibility.chunk_frustum_culling_enabled;
    stats["frustum_padding"] = visibility.chunk_frustum_padding;
    stats["zero_visible_consecutive_frames"] = visibility.zero_visible_recovery.zero_visible_consecutive_frames;
    stats["zero_visible_recoveries_triggered"] = (int)visibility.zero_visible_recovery.recoveries_triggered;
    stats["zero_visible_stall_detections"] = (int)visibility.zero_visible_recovery.stall_detections;
    stats["zero_visible_recovery_mode"] =
            visibility.zero_visible_recovery.mode == ZeroVisibleRecoveryMode::PERSISTENT ? "persistent" : "startup_only";
    stats["runtime_capacity_zero"] = is_runtime_capacity_zero();
    stats["runtime_buffer_invalid"] = is_persistent_buffer_invalid();
    stats["invalid_camera_input_events"] = (int)invalid_camera_input_events;
    stats["visible_evict_fallback_attempts"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_attempts);
    stats["visible_evict_fallback_successes"] =
            static_cast<int64_t>(diagnostics.visible_evict_fallback_successes);
    stats["layout_hint_validation"] = layout_hint_validation;
    stats["layout_hint_strict_mode_enabled"] = strict_layout_validation;
    stats["layout_hint_fallback_total"] = layout_hint_validation.get("fallback_total", int64_t(0));
    stats["layout_hint_fallback_io_total"] = layout_hint_validation.get("fallback_io_total", int64_t(0));
    stats["layout_hint_fallback_primary_total"] = layout_hint_validation.get("fallback_primary_total", int64_t(0));
    stats["layout_hint_strict_failure_total"] = layout_hint_validation.get("strict_failure_total", int64_t(0));
    stats["layout_hint_last_reason"] = layout_hint_validation.get("last_reason", String("none"));
    stats["layout_hint_last_reason_category"] = layout_hint_validation.get("last_reason_category", String("other"));
    stats["layout_hint_last_context"] = layout_hint_validation.get("last_context", String("none"));
    stats["layout_hint_last_usage"] = layout_hint_validation.get("last_usage", String("none"));
    stats["vram_payload_mb"] = double(budget.vram_usage) / (1024.0 * 1024.0);
    stats["vram_overhead_mb"] = double(_get_auxiliary_vram_overhead_bytes()) / (1024.0 * 1024.0);
    stats["cull_effectiveness"] = visibility.culling_stats.total_chunks > 0
            ? float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks)
            : 0.0f;
    stats["primary_spatial_chunking_enabled"] = primary_chunk_layout_metrics.spatial_partition_enabled;
    stats["primary_chunk_source_index_count"] = static_cast<int64_t>(primary_chunk_layout_metrics.source_index_count);
    stats["primary_chunk_avg_radius_ratio"] = primary_chunk_layout_metrics.avg_chunk_radius_ratio;
    stats["primary_chunk_max_radius_ratio"] = primary_chunk_layout_metrics.max_chunk_radius_ratio;
    stats["primary_chunk_bounds_volume_ratio"] = primary_chunk_layout_metrics.bounds_volume_ratio;

    // Calculate culling efficiency
    if (visibility.culling_stats.total_chunks > 0) {
        stats["cull_ratio"] = float(visibility.culling_stats.frustum_culled_chunks) / float(visibility.culling_stats.total_chunks);
    } else {
        stats["cull_ratio"] = 0.0f;
    }

    return stats;
}

Dictionary GaussianStreamingSystem::get_vram_debug_stats() const {
    return budget.get_vram_debug_stats();
}
