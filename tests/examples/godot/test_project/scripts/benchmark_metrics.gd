class_name BenchmarkMetrics
extends RefCounted

const RENDER_PERF_ROW_SCHEMA_VERSION := 7

const RENDER_PERF_CONTEXT_FIELDS := [
	"lane_id",
	"lane_name",
	"lane_preset",
	"phase",
	"sample_index",
	"elapsed_s",
	"frame_ms",
	"fps",
	"camera_mode",
	"capture_tag",
]

const RENDER_PERF_STATS_FIELDS := [
	"frame",
	"frame_count",
	"total_frames_rendered",
	"route_uid",
	"sort_route_uid",
	"cull_route_uid",
	"cull_route_reason",
	"requested_route_policy",
	"requested_route_policy_source",
	"instance_backend_policy",
	"backend_selection_reason",
	"instance_contract_shape",
	"instance_contract_ready",
	"data_source",
	"data_source_error",
	"raster_path",
	"raster_path_reason",
	"raster_compute_allowed",
	"raster_total_tiles",
	"raster_empty_tiles",
	"raster_overflow_tiles",
	"raster_max_splats_per_tile",
	"raster_avg_splats_per_tile",
	# Derived counts that disambiguate per-active-tile load from per-total-tile.
	# raster_avg_splats_per_tile is divided by ALL tiles (including empties),
	# which understates the per-active-tile cost ~15x at typical occupancy.
	"raster_active_tiles",
	"raster_avg_splats_per_active_tile",
	"raster_splat_pixel_ops_estimate",
	"raster_occupancy_ratio",
	"raster_dense_ratio",
	"raster_overflow_ratio",
	"raster_overlap_records",
	"raster_overlap_record_budget",
	"raster_overlap_record_budget_effective",
	"raster_overlap_record_budget_configured",
	"raster_overlap_sort_capacity",
	"raster_overlap_sort_capacity_high_water",
	"raster_overlap_sort_bytes",
	"raster_overlap_sort_resize_peak_bytes",
	"raster_overlap_sort_resize_preflight_refusals",
	"raster_overlap_thinning_keep_ratio",
	"raster_feature_global_sort",
	"raster_feature_packed_stage_data",
	"raster_feature_tighter_bounds",
	"raster_feature_sh_amortization",
	"raster_sh_amortization_divisor",
	"raster_feature_quantized_storage",
	"raster_feature_debug_counters",
	"raster_tile_splat_capacity",
	"raster_max_raster_splats_per_tile",
	"raster_shader_defines_hash",
	"raster_profile",
	# Per-pixel shader counters from gs_rasterize_pixel(). Populated only when
	# perf-capture flipped set_perf_capture_raster_shader_counters(true), which
	# adds GS_COLLECT_RASTER_STATS to the raster shader and enables the runtime
	# sample_raster_stats flag. Reads zero in production runs.
	# - raster_sample_count: total per-pixel rasterize_pixel invocations
	# - raster_splats_iterated: total inner-loop iterations across all pixels
	#   (= effective work; divide by sample_count for avg per pixel)
	# - raster_splats_contributed: iterations that actually wrote color
	# - raster_reject_*: per-iteration rejects (sorted-idx OOB, gaussian-idx
	#   OOB, base opacity, NaN/Inf, weight, alpha)
	# - raster_break_*: per-pixel early-exit reasons (remaining alpha, final
	#   alpha, subgroup early-exit)
	# - raster_alpha_sum_q10: sum of final alpha (Q10 fixed) -- avg = sum / (1024 * sample_count)
	"raster_sample_count",
	"raster_splats_iterated",
	"raster_splats_contributed",
	"raster_reject_sorted_idx_oob",
	"raster_reject_gaussian_idx_oob",
	"raster_reject_base_opacity",
	"raster_reject_nan_inf",
	"raster_reject_weight",
	"raster_reject_alpha",
	"raster_reject_index_mismatch",
	# Previously-uninstrumented continue paths in the inner loop. Without these,
	# (a) conic spatial-extent rejects, (b) LOD-modulated opacity rejects, and
	# (c) zero blend-alpha rejects all lumped together as "iterated but didn't
	# contribute or reject". reject_quadratic is the spatial-extent gate; if
	# this dominates, splats are too spatially wide. reject_lod_opacity fires
	# when LOD demotion zeros out the splat's effective opacity; if this
	# dominates, more aggressive LOD bias would help. reject_blend_alpha fires
	# when remaining-alpha multiply zeros the contribution; the right read is
	# usually small except in very-saturated scenes.
	"raster_reject_quadratic",
	"raster_reject_lod_opacity",
	"raster_reject_blend_alpha",
	"raster_break_remaining_alpha",
	"raster_break_final_alpha",
	"raster_break_subgroup_early_exit",
	"raster_has_depth",
	"raster_alpha_sum_q10",
	"render_mode",
	"using_real_data",
	"visible_splats",
	"total_splats",
	"sorted_splats",
	"rendered_splat_count",
	"uploaded_splat_count",
	"visible_after_culling",
	"culling_candidate_count",
	"culled_by_frustum",
	"culled_by_distance",
	"culled_by_screen",
	"culled_by_importance",
	"cull_ms",
	"sort_ms",
	"raster_ms",
	"composite_ms",
	"stage_total_ms",
	"render_ms",
	"render_time_ms",
	"sort_time_ms",
	"frame_time_ms",
	"avg_frame_time_ms",
	"peak_frame_time_ms",
	"stage_metrics_valid",
	"stage_cull_status",
	"stage_sort_status",
	"stage_raster_status",
	"stage_composite_status",
	"stage_cull_reason",
	"stage_sort_reason",
	"stage_raster_reason",
	"stage_composite_reason",
	"stage_cull_time_ms",
	"stage_sort_time_ms",
	"stage_raster_time_ms",
	"stage_composite_time_ms",
	"stage_cull_has_visible",
	"stage_cull_visible_count",
	"stage_sort_sorted_count",
	"stage_sort_input_count",
	"stage_raster_cached",
	"sort_active_algorithm",
	"sort_switch_reason",
	"sort_submission_time_ms",
	"sort_wait_time_ms",
	"sort_input_build_time_ms",
	"sort_sync_fallback_count",
	"instance_sort_sync_fallback_count",
	"tile_sort_sync_fallback_count",
	"sort_cached_fallback_count",
	"sort_identity_fallback_count",
	"sort_cull_order_fallback_count",
	"sort_total_route_fallback_count",
	"async_sort_used",
	"async_sort_waited",
	"async_overlap_efficiency",
	"gpu_sorter_algorithm",
	"gpu_sorter_ready",
	"gpu_sorter_max_elements",
	"gpu_sorter_last_sort_ms",
	"gpu_sorter_total_sorts",
	"gpu_sorter_async_sorts",
	"gpu_sorter_total_elements",
	"gpu_sorter_last_element_count",
	"gpu_sorter_last_element_count_known",
	"gpu_sorter_last_key_bits",
	"gpu_sorter_last_radix_bits",
	"gpu_sorter_last_pass_count",
	"gpu_sorter_last_sort_indirect",
	"gpu_sorter_last_sort_async",
	"sorting_pipeline_sorter_last_element_count",
	"sorting_pipeline_sorter_last_element_count_known",
	"sorting_pipeline_sorter_last_key_bits",
	"sorting_pipeline_sorter_last_radix_bits",
	"sorting_pipeline_sorter_last_pass_count",
	"sorting_pipeline_sorter_last_sort_indirect",
	"sorting_pipeline_sorter_last_sort_async",
	# Explicit overlap-sort metadata. These reflect the sorter that actually
	# produced the most recent gpu_overlap_sort_ms timestamp — the resident
	# instance pipeline uses sorting_pipeline; the standalone tile path uses
	# gpu_sorter. Reading either of those upstream fields directly is a trap
	# because which one is populated depends on the active route.
	"overlap_sort_element_count",
	"overlap_sort_element_count_known",
	"overlap_sort_key_bits",
	"overlap_sort_radix_bits",
	"overlap_sort_pass_count",
	"overlap_sort_indirect",
	"overlap_sort_async",
	# Tile overlap-record counters. visible_splats reports unique splats; an
	# overlap record exists per (splat × tile) pair, so the ratio
	# overlap_records / visible_splats is the per-frame tile-coverage factor.
	# overlap_record_budget is the GPU-side capacity; budget hits indicate
	# overflow / thinning fallback.
	"overlap_records",
	"overlap_record_budget",
	"overlap_record_budget_effective",
	"overlap_thinning_keep_ratio",
	"gpu_frame_ms",
	"gpu_frame_valid",
	"gpu_frame_time_ms",
	"gpu_frame_time_valid",
	"gpu_frame_estimate_ms",
	"gpu_frame_time_source",
	"gpu_timing_available",
	"gpu_overlap_count_ms",
	"gpu_overlap_count_valid",
	"gpu_overlap_emit_ms",
	"gpu_overlap_emit_valid",
	"gpu_overlap_sort_ms",
	"gpu_overlap_sort_valid",
	"overlap_sort_cpu_dispatch_ms",
	"overlap_sort_cpu_dispatch_valid",
	"gpu_binning_ms",
	"gpu_prefix_ms",
	"gpu_prefix_valid",
	"prefix_cpu_sync_fallback_ms",
	"prefix_cpu_sync_fallback_valid",
	"gpu_raster_ms",
	"gpu_raster_valid",
	"gpu_resolve_ms",
	"gpu_resolve_valid",
	"gpu_tile_overlap_count_time_ms",
	"gpu_tile_overlap_count_time_valid",
	"gpu_tile_binning_time_ms",
	"gpu_tile_overlap_emit_time_ms",
	"gpu_tile_overlap_emit_time_valid",
	"gpu_tile_overlap_sort_time_ms",
	"gpu_tile_overlap_sort_time_valid",
	"tile_overlap_sort_cpu_dispatch_ms",
	"tile_overlap_sort_cpu_dispatch_valid",
	"gpu_tile_prefix_time_ms",
	"gpu_tile_prefix_time_valid",
	"tile_prefix_cpu_sync_fallback_ms",
	"tile_prefix_cpu_sync_fallback_valid",
	"gpu_tile_raster_time_ms",
	"gpu_tile_raster_time_valid",
	"gpu_tile_resolve_time_ms",
	"gpu_tile_resolve_time_valid",
	"gpu_pass_breakdown_available",
	"gpu_timing_frame_serial",
	"gpu_timing_frames_behind",
	"gpu_timeline_inflight_frames",
	"gpu_timeline_completed_frames",
	"gpu_timeline_stall_count",
	"gpu_timeline_stall_ms",
	"instance_pipeline_execution_mode",
	"instance_pipeline_execution_path",
	"instance_pipeline_execution_reason",
	"instance_pipeline_true_single_pass_enabled",
	"instance_pipeline_instance_count",
	"instance_pipeline_content_generation",
	"effective_quality_preset",
	"effective_max_splats",
	"effective_lod_enabled",
	"effective_lod_bias",
	"effective_lod_max_distance",
	"effective_distance_cull_enabled",
	"effective_distance_cull_start",
	"effective_distance_cull_max_rate",
	"effective_tiny_splat_screen_radius",
	"effective_overflow_autotune_enabled",
	"streaming_cap_tier_preset",
	"streaming_cap_tier_active",
	"streaming_effective_upload_cap_mb_per_frame",
	"streaming_effective_upload_cap_mb_per_slice",
	"streaming_effective_upload_cap_mb_per_second",
	"streaming_effective_vram_budget_mb",
	"streaming_effective_vram_min_chunks",
	"streaming_effective_vram_max_chunks",
	"streaming_requested_vram_budget_mb",
	"streaming_cap_source_upload_mb_per_frame",
	"streaming_cap_source_upload_mb_per_slice",
	"streaming_cap_source_upload_mb_per_second",
	"streaming_cap_source_vram_budget_mb",
	"streaming_requested_cap_source_vram_budget_mb",
	"streaming_cap_source_vram_min_chunks",
	"streaming_cap_source_vram_max_chunks",
	"streaming_vram_budget_capacity_verified",
	"streaming_vram_budget_unknown_capacity_fallback",
	"streaming_vram_budget_unverified",
	"streaming_queue_pressure_active",
	"streaming_queue_pressure_frames",
	"streaming_vram_cap_hit_frames",
	"streaming_upload_bandwidth_cap_hit",
	"streaming_chunk_load_cap_hit",
	"streaming_vram_chunk_cap_hit",
]

const RENDER_PERF_STREAMING_STATE_FIELDS := [
	"streaming_render_readiness_state",
	"streaming_render_readiness_reason",
	"streaming_total_chunks",
	"streaming_visible_chunks",
	"streaming_loaded_chunks",
	"streaming_resident_chunks",
	"streaming_atlas_published_chunks",
	"streaming_chunks_loaded_this_frame",
	"streaming_chunks_evicted_this_frame",
	"streaming_vram_usage_mb",
	"streaming_visible_count",
	"streaming_effective_splat_count",
	"streaming_buffer_capacity_splats",
	"streaming_visible_change_ratio",
	"streaming_lod_blend_factor",
	"streaming_sh_band_level",
	"streaming_bytes_uploaded_mb",
]

const RENDER_PERF_SORT_METRIC_FIELDS := [
	"last_sort_frame",
	"last_sort_elements",
	"last_sort_total_ms",
	"last_sort_gpu_ms",
	"last_sort_cpu_ms",
	"last_sort_cpu_selection_ms",
	"last_sort_algorithm",
	"last_sort_used_gpu",
	"last_sort_cpu_fallback",
	"last_sort_hybrid",
	"last_sort_target_ms",
	"last_sort_active_algorithm",
	"last_sort_switch_reason",
	"last_sort_override_force_cpu",
	"last_sort_override_force_algorithm",
	"last_sort_override_forced_algorithm",
	"last_sort_sorter_last_element_count",
	"last_sort_sorter_last_element_count_known",
	"last_sort_sorter_last_key_bits",
	"last_sort_sorter_last_radix_bits",
	"last_sort_sorter_last_pass_count",
	"last_sort_sorter_last_sort_indirect",
	"last_sort_sorter_last_sort_async",
	"last_sort_pipeline_last_element_count",
	"last_sort_pipeline_last_element_count_known",
	"last_sort_pipeline_last_key_bits",
	"last_sort_pipeline_last_radix_bits",
	"last_sort_pipeline_last_pass_count",
	"last_sort_pipeline_last_sort_indirect",
	"last_sort_pipeline_last_sort_async",
]

const RENDER_PERF_TRACE_FIELDS := [
	"pipeline_trace_available",
	"pipeline_trace_frame",
	"pipeline_trace_dump_frame",
	"pipeline_trace_enabled",
	"pipeline_trace_events_valid",
	"pipeline_trace_fresh",
	"pipeline_trace_generation",
	"pipeline_trace_event_count",
	"pipeline_trace_route_uid",
	"pipeline_trace_sort_route_uid",
	"pipeline_trace_dump",
]

static func _sorted_copy(samples: Array) -> Array:
	var sorted: Array = samples.duplicate()
	sorted.sort()
	return sorted

static func _append_fields(target: Array[String], fields: Array) -> void:
	for field in fields:
		target.append(str(field))

static func render_perf_row_fields() -> Array[String]:
	var fields: Array[String] = [
		"row_schema_version",
		"sample_time_unix",
		"sample_time_msec",
		"renderer_available",
		"render_stats_available",
		"sort_metrics_available",
	]
	_append_fields(fields, RENDER_PERF_CONTEXT_FIELDS)
	_append_fields(fields, RENDER_PERF_STATS_FIELDS)
	_append_fields(fields, RENDER_PERF_STREAMING_STATE_FIELDS)
	_append_fields(fields, RENDER_PERF_SORT_METRIC_FIELDS)
	_append_fields(fields, RENDER_PERF_TRACE_FIELDS)
	return fields

static func _csv_scalar(value: Variant) -> Variant:
	match typeof(value):
		TYPE_NIL, TYPE_BOOL, TYPE_INT, TYPE_FLOAT, TYPE_STRING:
			return value
		TYPE_STRING_NAME, TYPE_NODE_PATH:
			return str(value)
		_:
			return JSON.stringify(value)

static func _copy_scalar_key(source: Dictionary, target: Dictionary, key: String, target_key: String = "") -> void:
	if not source.has(key):
		return
	var out_key := target_key if not target_key.is_empty() else key
	target[out_key] = _csv_scalar(source.get(key))

static func _copy_scalar_keys(source: Dictionary, target: Dictionary, keys: Array) -> void:
	for key_variant in keys:
		_copy_scalar_key(source, target, str(key_variant))

static func _copy_scalar_keys_if_missing(source: Dictionary, target: Dictionary, keys: Array) -> void:
	for key_variant in keys:
		var key := str(key_variant)
		if target.has(key) and target[key] != null:
			continue
		_copy_scalar_key(source, target, key)

static func _copy_nested_metric_source(stats: Dictionary, row: Dictionary, source_key: String) -> void:
	var nested = stats.get(source_key, {})
	if nested is Dictionary:
		_copy_scalar_keys_if_missing(nested, row, RENDER_PERF_STATS_FIELDS)

static func _set_raster_profile_from_row(row: Dictionary) -> void:
	var profile := {}
	for key_variant in RENDER_PERF_STATS_FIELDS:
		var key := str(key_variant)
		if not key.begins_with("raster_"):
			continue
		if key == "raster_profile":
			continue
		var value = row.get(key, null)
		if value != null:
			profile[key] = value
	if not profile.is_empty():
		row["raster_profile"] = JSON.stringify(profile)

static func _copy_streaming_state(stats: Dictionary, row: Dictionary) -> void:
	var state: Dictionary = {}
	if stats.has("streaming_state") and stats["streaming_state"] is Dictionary:
		state = stats["streaming_state"]
	for field_variant in RENDER_PERF_STREAMING_STATE_FIELDS:
		var target_key := str(field_variant)
		var source_key := target_key
		if source_key.begins_with("streaming_"):
			source_key = source_key.substr("streaming_".length())
		if state.has(source_key):
			_copy_scalar_key(state, row, source_key, target_key)
		elif stats.has(target_key):
			_copy_scalar_key(stats, row, target_key)

static func _copy_sort_metrics(renderer: Object, row: Dictionary) -> void:
	if renderer == null or not renderer.has_method("get_last_sort_metrics"):
		return
	var sort_metrics = renderer.get_last_sort_metrics()
	if not (sort_metrics is Dictionary):
		return
	row["sort_metrics_available"] = true
	_copy_scalar_key(sort_metrics, row, "frame", "last_sort_frame")
	_copy_scalar_key(sort_metrics, row, "elements", "last_sort_elements")
	_copy_scalar_key(sort_metrics, row, "total_ms", "last_sort_total_ms")
	_copy_scalar_key(sort_metrics, row, "gpu_ms", "last_sort_gpu_ms")
	_copy_scalar_key(sort_metrics, row, "cpu_ms", "last_sort_cpu_ms")
	_copy_scalar_key(sort_metrics, row, "cpu_selection_ms", "last_sort_cpu_selection_ms")
	_copy_scalar_key(sort_metrics, row, "algorithm", "last_sort_algorithm")
	_copy_scalar_key(sort_metrics, row, "used_gpu", "last_sort_used_gpu")
	_copy_scalar_key(sort_metrics, row, "cpu_fallback", "last_sort_cpu_fallback")
	_copy_scalar_key(sort_metrics, row, "hybrid", "last_sort_hybrid")
	_copy_scalar_key(sort_metrics, row, "target_ms", "last_sort_target_ms")
	_copy_scalar_key(sort_metrics, row, "active_algorithm", "last_sort_active_algorithm")
	_copy_scalar_key(sort_metrics, row, "switch_reason", "last_sort_switch_reason")
	_copy_scalar_key(sort_metrics, row, "override_force_cpu", "last_sort_override_force_cpu")
	_copy_scalar_key(sort_metrics, row, "override_force_algorithm", "last_sort_override_force_algorithm")
	_copy_scalar_key(sort_metrics, row, "override_forced_algorithm", "last_sort_override_forced_algorithm")
	_copy_scalar_key(sort_metrics, row, "sorter_last_element_count", "last_sort_sorter_last_element_count")
	_copy_scalar_key(sort_metrics, row, "sorter_last_element_count_known", "last_sort_sorter_last_element_count_known")
	_copy_scalar_key(sort_metrics, row, "sorter_last_key_bits", "last_sort_sorter_last_key_bits")
	_copy_scalar_key(sort_metrics, row, "sorter_last_radix_bits", "last_sort_sorter_last_radix_bits")
	_copy_scalar_key(sort_metrics, row, "sorter_last_pass_count", "last_sort_sorter_last_pass_count")
	_copy_scalar_key(sort_metrics, row, "sorter_last_sort_indirect", "last_sort_sorter_last_sort_indirect")
	_copy_scalar_key(sort_metrics, row, "sorter_last_sort_async", "last_sort_sorter_last_sort_async")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_element_count", "last_sort_pipeline_last_element_count")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_element_count_known", "last_sort_pipeline_last_element_count_known")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_key_bits", "last_sort_pipeline_last_key_bits")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_radix_bits", "last_sort_pipeline_last_radix_bits")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_pass_count", "last_sort_pipeline_last_pass_count")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_sort_indirect", "last_sort_pipeline_last_sort_indirect")
	_copy_scalar_key(sort_metrics, row, "sorting_pipeline_sorter_last_sort_async", "last_sort_pipeline_last_sort_async")

static func _copy_pipeline_trace(renderer: Object, row: Dictionary, include_dump: bool) -> void:
	if renderer == null or not renderer.has_method("get_pipeline_trace_snapshot"):
		return
	var trace = renderer.get_pipeline_trace_snapshot()
	if not (trace is Dictionary):
		return
	row["pipeline_trace_available"] = true
	_copy_scalar_key(trace, row, "frame", "pipeline_trace_frame")
	_copy_scalar_key(trace, row, "dump_frame", "pipeline_trace_dump_frame")
	_copy_scalar_key(trace, row, "trace_enabled", "pipeline_trace_enabled")
	_copy_scalar_key(trace, row, "events_valid", "pipeline_trace_events_valid")
	_copy_scalar_key(trace, row, "trace_fresh", "pipeline_trace_fresh")
	_copy_scalar_key(trace, row, "trace_generation", "pipeline_trace_generation")
	_copy_scalar_key(trace, row, "route_uid", "pipeline_trace_route_uid")
	_copy_scalar_key(trace, row, "sort_route_uid", "pipeline_trace_sort_route_uid")
	var events = trace.get("events", [])
	row["pipeline_trace_event_count"] = events.size() if events is Array else 0
	if include_dump:
		row["pipeline_trace_dump"] = JSON.stringify(trace)

static func capture_render_perf_row(renderer: Object, context: Dictionary = {}, options: Dictionary = {}) -> Dictionary:
	var row: Dictionary = {}
	for field in render_perf_row_fields():
		row[field] = null
	row["row_schema_version"] = RENDER_PERF_ROW_SCHEMA_VERSION
	row["sample_time_unix"] = Time.get_unix_time_from_system()
	row["sample_time_msec"] = Time.get_ticks_msec()
	row["renderer_available"] = renderer != null
	row["render_stats_available"] = false
	row["sort_metrics_available"] = false
	row["pipeline_trace_available"] = false
	_copy_scalar_keys(context, row, RENDER_PERF_CONTEXT_FIELDS)

	if renderer == null:
		return row
	if renderer.has_method("get_render_stats"):
		var stats = renderer.get_render_stats()
		if stats is Dictionary:
			row["render_stats_available"] = true
			_copy_scalar_keys(stats, row, RENDER_PERF_STATS_FIELDS)
			_copy_nested_metric_source(stats, row, "production_metrics")
			_copy_nested_metric_source(stats, row, "telemetry")
			_set_raster_profile_from_row(row)
			_copy_streaming_state(stats, row)
	_copy_sort_metrics(renderer, row)
	_copy_pipeline_trace(renderer, row, bool(options.get("include_pipeline_trace_dump", false)))
	return row

static func percentile(samples: Array, percentile_value: float) -> float:
	if samples.is_empty():
		return 0.0
	var sorted := _sorted_copy(samples)
	var pct := clampf(percentile_value, 0.0, 100.0)
	var idx := int(round((pct / 100.0) * float(sorted.size() - 1)))
	return float(sorted[idx])

static func summarize_samples(frame_ms_samples: Array, fps_samples: Array) -> Dictionary:
	var total_frame_ms := 0.0
	var avg_frame_ms := 0.0
	for frame_ms in frame_ms_samples:
		total_frame_ms += float(frame_ms)
	if not frame_ms_samples.is_empty():
		avg_frame_ms = total_frame_ms / float(frame_ms_samples.size())
	# Use harmonic mean: total_frames / total_time. This gives true throughput
	# and avoids inflated averages when frame times are bimodal.
	var avg_fps := 0.0
	if total_frame_ms > 0.0:
		avg_fps = float(frame_ms_samples.size()) / (total_frame_ms / 1000.0)

	var min_fps := 0.0 if fps_samples.is_empty() else float(fps_samples.min())
	var max_fps := 0.0 if fps_samples.is_empty() else float(fps_samples.max())
	var p1_fps := percentile(fps_samples, 1.0)
	var p5_fps := percentile(fps_samples, 5.0)
	var p95_frame_ms := percentile(frame_ms_samples, 95.0)
	var p99_frame_ms := percentile(frame_ms_samples, 99.0)
	var max_frame_ms := 0.0 if frame_ms_samples.is_empty() else float(frame_ms_samples.max())

	var stability := 0.0
	if avg_fps > 0.0:
		stability = clampf(p1_fps / avg_fps, 0.0, 1.0)

	return {
		"sample_count": frame_ms_samples.size(),
		"avg_fps": avg_fps,
		"min_fps": min_fps,
		"max_fps": max_fps,
		"p1_fps": p1_fps,
		"p5_fps": p5_fps,
		"avg_frame_ms": avg_frame_ms,
		"p95_frame_ms": p95_frame_ms,
		"p99_frame_ms": p99_frame_ms,
		"max_frame_ms": max_frame_ms,
		"stability": stability,
	}

static func has_samples(summary: Dictionary) -> bool:
	return int(summary.get("sample_count", 0)) > 0

static func compute_score(overall_summary: Dictionary, monitor_max: Dictionary) -> float:
	var avg_fps: float = float(overall_summary.get("avg_fps", 0.0))
	var p1_fps: float = float(overall_summary.get("p1_fps", 0.0))
	var p99_frame_ms: float = maxf(float(overall_summary.get("p99_frame_ms", 0.0)), 0.001)
	var stability: float = float(overall_summary.get("stability", 0.0))

	var fps_component: float = clampf(avg_fps / 90.0, 0.0, 1.0) * 45.0
	var low_percentile_component: float = clampf(p1_fps / 60.0, 0.0, 1.0) * 25.0
	var frame_component: float = clampf(16.6 / p99_frame_ms, 0.0, 1.0) * 20.0
	var stability_component: float = clampf(stability, 0.0, 1.0) * 10.0

	var penalty: float = 0.0
	if float(monitor_max.get("streaming_queue_pressure_active", 0.0)) > 0.0:
		penalty += 3.0
	if float(monitor_max.get("streaming_upload_bandwidth_cap_hit", 0.0)) > 0.0:
		penalty += 2.0
	if float(monitor_max.get("streaming_chunk_load_cap_hit", 0.0)) > 0.0:
		penalty += 2.0

	return clampf(fps_component + low_percentile_component + frame_component + stability_component - penalty, 0.0, 100.0)

static func _setting_value(settings: Dictionary, key: String, fallback: Variant) -> Variant:
	if settings.has(key):
		return settings[key]
	return fallback

static func build_recommendations(report: Dictionary) -> Array[Dictionary]:
	var recommendations: Array[Dictionary] = []
	var overall: Dictionary = report.get("overall", {})
	var monitor_max: Dictionary = report.get("monitor_max", {})
	var settings: Dictionary = report.get("project_settings", {})

	var avg_fps: float = float(overall.get("avg_fps", 0.0))
	var p1_fps: float = float(overall.get("p1_fps", 0.0))
	var p99_frame_ms: float = float(overall.get("p99_frame_ms", 0.0))
	var streaming_vram_usage_mb: float = float(monitor_max.get("streaming_vram_usage_mb", 0.0))
	var streaming_visible_change_ratio: float = float(monitor_max.get("streaming_visible_change_ratio", 0.0))
	var queue_pressure: float = float(monitor_max.get("streaming_queue_pressure_active", 0.0))
	var upload_cap_hit: float = float(monitor_max.get("streaming_upload_bandwidth_cap_hit", 0.0))
	var chunk_load_cap_hit: float = float(monitor_max.get("streaming_chunk_load_cap_hit", 0.0))
	var lod_reduction_ratio: float = float(monitor_max.get("lod_reduction_ratio_pct", 0.0))
	var lod_transitions: float = float(monitor_max.get("lod_transitions_this_frame", 0.0))

	if avg_fps < 45.0 or p1_fps < 28.0 or p99_frame_ms > 33.3:
		recommendations.append({
			"setting": "rendering/gaussian_splatting/lod/max_distance",
			"current": _setting_value(settings, "rendering/gaussian_splatting/lod/max_distance", 50.0),
			"suggested": 35.0,
			"reason": "Frame consistency indicates splat density is too high at distance.",
			"tradeoff": "Slightly earlier distance culling for far splats.",
		})
		recommendations.append({
			"setting": "rendering/gaussian_splatting/lod/bias",
			"current": _setting_value(settings, "rendering/gaussian_splatting/lod/bias", 1.0),
			"suggested": 1.15,
			"reason": "Higher LOD bias reduces per-frame shading/raster pressure.",
			"tradeoff": "Marginal detail loss on medium/far splats.",
		})

	if queue_pressure > 0.0 or upload_cap_hit > 0.0 or chunk_load_cap_hit > 0.0 or streaming_visible_change_ratio > 0.8:
		recommendations.append({
			"setting": "rendering/gaussian_splatting/streaming/max_chunk_loads_per_frame",
			"current": _setting_value(settings, "rendering/gaussian_splatting/streaming/max_chunk_loads_per_frame", 8),
			"suggested": 12,
			"reason": "Streaming queue pressure/cap hits suggest chunk ingress is rate-limited.",
			"tradeoff": "Higher upload bursts can increase transient frame spikes.",
		})
		recommendations.append({
			"setting": "rendering/gaussian_splatting/streaming/pack_worker_threads",
			"current": _setting_value(settings, "rendering/gaussian_splatting/streaming/pack_worker_threads", 4),
			"suggested": 6,
			"reason": "Packing throughput appears to be the bottleneck during streaming transitions.",
			"tradeoff": "Additional CPU thread pressure on low-core systems.",
		})

	if streaming_vram_usage_mb > 1800.0:
		recommendations.append({
			"setting": "rendering/gaussian_splatting/streaming/vram_budget_mb",
			"current": _setting_value(settings, "rendering/gaussian_splatting/streaming/vram_budget_mb", 2048),
			"suggested": 1536,
			"reason": "VRAM usage approached high-water mark, increasing eviction risk on smaller GPUs.",
			"tradeoff": "More aggressive streaming may increase pop-in if camera moves quickly.",
		})

	if p1_fps < 35.0:
		recommendations.append({
			"setting": "rendering/gaussian_splatting/animation/wind_strength",
			"current": _setting_value(settings, "rendering/gaussian_splatting/animation/wind_strength", 0.0),
			"suggested": 0.4,
			"reason": "Low percentile FPS indicates animation deformation cost is too high for current hardware.",
			"tradeoff": "Reduced wind motion amplitude.",
		})
		recommendations.append({
			"setting": "rendering/gaussian_splatting/effects/max_effectors",
			"current": _setting_value(settings, "rendering/gaussian_splatting/effects/max_effectors", 0),
			"suggested": 2,
			"reason": "Effector-heavy frames are likely contributing to frame-time outliers.",
			"tradeoff": "Fewer concurrent procedural effect regions.",
		})

	if lod_transitions > 12.0 and lod_reduction_ratio > 40.0:
		recommendations.append({
			"setting": "rendering/gaussian_splatting/lod/hysteresis_zone",
			"current": _setting_value(settings, "rendering/gaussian_splatting/lod/hysteresis_zone", 0.5),
			"suggested": 0.8,
			"reason": "Frequent LOD swaps indicate transition hysteresis is too tight for motion profile.",
			"tradeoff": "Slightly slower response to rapid camera distance changes.",
		})

	if recommendations.is_empty():
		recommendations.append({
			"setting": "none",
			"current": "n/a",
			"suggested": "keep current settings",
			"reason": "Benchmark stayed within target envelope for this workload.",
			"tradeoff": "No change required.",
		})

	return recommendations
