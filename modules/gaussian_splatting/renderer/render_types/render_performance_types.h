/**
 * @file render_performance_types.h
 * @brief Performance metrics and settings type definitions.
 *
 * Standalone types for performance monitoring and quality configuration.
 * Extracted from GaussianSplatRenderer so orchestrators and diagnostic
 * systems can depend on narrow contracts.
 */

#ifndef GAUSSIAN_RENDER_PERFORMANCE_TYPES_H
#define GAUSSIAN_RENDER_PERFORMANCE_TYPES_H

#include "core/string/string_name.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "render_pipeline_io_types.h"
#include <cstdint>

namespace GaussianRenderPerformance {

struct PerformanceSettings {
	int max_splats = 5000000;
};

/**
 * @struct SortFrameMetrics
 * @brief Per-frame sorting performance metrics.
 *
 * Captures timing and algorithm selection data for a single frame's
 * depth sorting pass. Used for performance monitoring and debugging.
 */
struct SortFrameMetrics {
	uint32_t frame_index = 0;       ///< Frame number for correlation.
	uint32_t element_count = 0;     ///< Number of splats sorted.
	float total_ms = 0.0f;          ///< Total sorting time in milliseconds.
	float gpu_ms = 0.0f;            ///< GPU-side sorting time.
	float cpu_ms = 0.0f;            ///< CPU-side sorting time (if fallback).
	float cpu_selection_ms = 0.0f;  ///< Time spent preparing sort input buffers (ms).
	StringName algorithm;           ///< Name of the sorting algorithm used.
	bool used_gpu = false;          ///< True if GPU sorting was used.
	bool used_cpu_fallback = false; ///< True if CPU fallback was triggered.
	bool used_hybrid = false;       ///< Reserved for future hybrid GPU/CPU sorting.
};

struct PerformanceMetrics {
	float buffer_upload_time_ms = 0.0f;
	float culling_time_ms = 0.0f;
	float gpu_memory_usage_mb = 0.0f;
	uint32_t uploaded_splat_count = 0;
	uint32_t rendered_splat_count = 0;
	bool using_real_data = false;
	String data_source = GaussianRenderPipeline::SplatDataSource::kSourceNone;
	String data_source_error;
	String raster_path = "unknown";
	String raster_path_reason;
	bool raster_compute_allowed = false;
	// Per-splat scene-depth clip active this frame (compositing slice D). Distinguishes
	// "clipping mid-cloud" from "whole-pixel composite only" (off by setting, no scene
	// depth, multiview, or degraded at the binding site).
	bool raster_scene_clip_active = false;
	uint32_t raster_total_tiles = 0;
	uint32_t raster_empty_tiles = 0;
	uint32_t raster_overflow_tiles = 0;
	uint32_t raster_max_splats_per_tile = 0;
	float raster_avg_splats_per_tile = 0.0f;
	float raster_occupancy_ratio = 0.0f;
	float raster_dense_ratio = 0.0f;
	float raster_overflow_ratio = 0.0f;
	uint32_t raster_overlap_records = 0;
	uint32_t raster_overlap_record_budget = 0;
	uint32_t raster_overlap_record_budget_effective = 0;
	uint32_t raster_overlap_record_budget_configured = 0;
	float raster_overlap_thinning_keep_ratio = 1.0f;
	bool raster_feature_global_sort = false;
	bool raster_feature_packed_stage_data = false;
	bool raster_feature_tighter_bounds = false;
	bool raster_feature_sh_amortization = false;
	uint32_t raster_sh_amortization_divisor = 1;
	bool raster_feature_quantized_storage = false;
	bool raster_feature_debug_counters = false;
	uint32_t raster_tile_splat_capacity = 0;
	uint32_t raster_max_raster_splats_per_tile = 0;
	uint64_t raster_shader_defines_hash = 0;
	uint64_t total_frames_rendered = 0;
	float avg_frame_time_ms = 0.0f;
	float peak_frame_time_ms = 0.0f;
	float sort_submission_time_ms = 0.0f;
	float sort_wait_time_ms = 0.0f;
	float sort_input_build_time_ms = 0.0f;
	uint64_t instance_sort_sync_fallback_count = 0;
	uint64_t tile_sort_sync_fallback_count = 0;
	uint64_t sort_cached_fallback_count = 0;
	uint64_t sort_identity_fallback_count = 0;
	uint64_t sort_cull_order_fallback_count = 0;
	bool async_sort_used = false;
	bool async_sort_waited = false;
	float async_overlap_efficiency = 0.0f;
	uint32_t culled_frustum_count = 0;
	uint32_t culled_distance_count = 0;
	uint32_t culled_screen_count = 0;
	uint32_t culled_importance_count = 0;
	uint32_t culling_candidate_count = 0;
	uint32_t visible_after_culling = 0;
	String cull_route_uid;
	String cull_route_reason;
	bool used_hierarchical_culling = false;
	Dictionary streaming_state;
	uint64_t sort_cache_hits = 0;
	uint64_t sort_cache_misses = 0;
	float gpu_utilization = 0.0f;
	float gpu_frame_time_ms = 0.0f;
	bool gpu_frame_time_valid = false;
	float gpu_tile_overlap_count_time_ms = 0.0f;
	bool gpu_tile_overlap_count_time_valid = false;
	float gpu_tile_binning_time_ms = 0.0f;
	float gpu_tile_overlap_emit_time_ms = 0.0f;
	bool gpu_tile_overlap_emit_time_valid = false;
	float gpu_tile_overlap_sort_time_ms = 0.0f;
	bool gpu_tile_overlap_sort_time_valid = false;
	float tile_overlap_sort_cpu_dispatch_ms = 0.0f;
	bool tile_overlap_sort_cpu_dispatch_valid = false;
	float gpu_tile_raster_time_ms = 0.0f;
	bool gpu_tile_raster_time_valid = false;
	float gpu_tile_prefix_time_ms = 0.0f;
	bool gpu_tile_prefix_time_valid = false;
	float tile_prefix_cpu_sync_fallback_ms = 0.0f;
	bool tile_prefix_cpu_sync_fallback_valid = false;
	float gpu_tile_resolve_time_ms = 0.0f;
	bool gpu_tile_resolve_time_valid = false;
	uint64_t gpu_timing_frame_serial = 0;
	uint64_t gpu_timing_frames_behind = 0;
	uint32_t gpu_timeline_inflight_frames = 0;
	uint32_t gpu_timeline_completed_frames = 0;
	uint32_t gpu_timeline_stall_count = 0;
	float gpu_timeline_stall_ms = 0.0f;
	uint64_t gpu_timeline_last_value = 0;
	uint64_t last_frame_start_usec = 0;
	float frame_to_frame_time_ms = 0.0f;
	float avg_frame_to_frame_ms = 0.0f;
	uint64_t cull_projection_contract_mismatch_count = 0;
	uint32_t raster_pipeline_reformats = 0;
};

struct PerformanceState {
	PerformanceMetrics metrics;
	LocalVector<SortFrameMetrics> sort_metrics_history;
};

} // namespace GaussianRenderPerformance

#endif // GAUSSIAN_RENDER_PERFORMANCE_TYPES_H
