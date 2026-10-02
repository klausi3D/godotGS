# GPU Timestamp Profiling and Monitor Semantics

This document defines how Gaussian Splatting timing monitors should be interpreted.

## Custom monitor coverage

The module registers its custom monitors with Godot's `Performance` singleton under the `gaussian_splatting/`
prefix (`GaussianSplattingPerformanceMonitors::_register_monitor_definitions()`). The timing-related monitors are:

- `telemetry_active`: lifecycle flag (`1` when at least one Gaussian renderer is registered, `0` when telemetry is inactive).
- `gpu_time_cull_ms`: cull stage duration from stage metrics (fallback: culling summary).
- `gpu_time_sort_ms`: the GPU timestamp of the tile overlap sort when the current sample is valid
  (`is_last_gpu_overlap_sort_time_valid()`); otherwise falls back to the stage-metrics sort time, or the frame sort
  time when stage metrics are invalid. The fallback values are CPU submit times and under-report the GPU sort.
- `gpu_time_overlap_count_ms`: tile overlap-COUNT pass GPU timestamp duration.
- `gpu_time_binning_ms`: tile binning GPU timestamp duration.
- `gpu_time_prefix_ms`: tile prefix-sum GPU timestamp duration.
- `gpu_time_raster_ms`: tile raster GPU timestamp duration.
- `gpu_time_resolve_ms`: tile resolve GPU timestamp duration.
- `gpu_time_frame_ms`: total GPU frame duration for the tile renderer path.

The registered set also contains non-timing monitors (counts, memory, streaming state); the authoritative list is
`_register_monitor_definitions()`.

`route_uid` and `sort_route_uid` are **not** `Performance` monitors. They are keys of the production metrics
snapshot (below) and of `GaussianSplatRenderer.get_render_stats()`.

## Route and stage contracts

Each rendered frame carries a `RenderRouteDecision` through `RenderFramePlan`. Stage results for cull, sort,
raster, and composite carry the selected route UID, input/output domains, input/output counts, first failure,
skip cause, and degradation flags.

Downstream stages that did not execute because an upstream stage failed are `skipped`, not `failed`. The first
executed failing stage keeps `failed`, and skipped stages publish `stage_skip_cause` so diagnostics can attribute
the cascade without treating derived empty output as a second failure.

Composite depth degradation is part of the stage contract. When `depth_test_honored` is false, relaxed policy
reports a composite degradation; strict policy reports a composite contract violation/failure.

## Telemetry availability

Use `telemetry_active` to distinguish unavailable monitor samples from valid runtime zeros.
When `telemetry_active == 0`, monitor consumers should treat route/timing/stat counters as inactive.

## Non-placeholder behavior

Timing values now preserve the last valid sample until a newer timestamp sample is available.
This prevents per-frame resets to `0.0` from being interpreted as real timings when timestamp
readback is temporarily delayed.

Expected `0.0` cases:

- No active renderer is registered.
- The pass did not run (for example, no visible splats).
- No valid sample has ever been captured yet.

## Freshness fields

Runtime diagnostics expose:

- `gpu_timing_frame_serial`
- `gpu_timing_frames_behind`

Use these with timing values to decide whether a sample is fresh for the current frame.

## Pipeline Trace Data-Flow Recent Window

When `rendering/gaussian_splatting/debug/enable_pipeline_trace` is enabled, pipeline trace snapshots now include
`data_flow.recent_window` alongside lifetime counters.

- `capacity`: fixed ring-buffer capacity (bounded, no unbounded growth).
- `frames_recorded`: number of recent per-frame deltas currently retained.
- `frame_deltas`: ordered per-frame telemetry deltas (for near-term regression localization).
- aggregate counters (`pack_sh_samples`, `pack_range_calls`, etc.) over the recent window.

## Production metrics validation

Production metrics now include:

- `gpu_frame_ms`
- `gpu_frame_valid`
- `gpu_overlap_count_ms`
- `gpu_overlap_count_valid`
- `gpu_overlap_emit_ms`
- `gpu_overlap_emit_valid`
- `gpu_overlap_sort_ms`
- `gpu_overlap_sort_valid`
- `overlap_sort_cpu_dispatch_ms`
- `overlap_sort_cpu_dispatch_valid`
- `gpu_binning_ms`
- `gpu_prefix_ms`
- `gpu_prefix_valid`
- `prefix_cpu_sync_fallback_ms`
- `prefix_cpu_sync_fallback_valid`
- `gpu_raster_ms`
- `gpu_raster_valid`
- `gpu_resolve_ms`
- `gpu_resolve_valid`
- `gpu_timing_frame_serial`
- `gpu_timing_frames_behind`
- `gpu_pass_breakdown_available`

The runtime telemetry snapshot also exposes the same timing sample with the longer renderer-internal names:

- `gpu_frame_time_ms`
- `gpu_frame_time_valid`
- `gpu_tile_overlap_count_time_ms`
- `gpu_tile_overlap_count_time_valid`
- `gpu_tile_binning_time_ms`
- `gpu_tile_overlap_emit_time_ms`
- `gpu_tile_overlap_emit_time_valid`
- `gpu_tile_overlap_sort_time_ms`
- `gpu_tile_overlap_sort_time_valid`
- `tile_overlap_sort_cpu_dispatch_ms`
- `tile_overlap_sort_cpu_dispatch_valid`
- `gpu_tile_prefix_time_ms`
- `gpu_tile_prefix_time_valid`
- `tile_prefix_cpu_sync_fallback_ms`
- `tile_prefix_cpu_sync_fallback_valid`
- `gpu_tile_raster_time_ms`
- `gpu_tile_raster_time_valid`
- `gpu_tile_resolve_time_ms`
- `gpu_tile_resolve_time_valid`

The production metrics names are the stable perf-capture contract for external consumers; the telemetry names are
kept for renderer diagnostics and cross-checking.

`TileOverlapCount` is the COUNT pass. `TileBinning` is the overlap EMIT pass. The tile overlap sort has its own
GPU timestamp: `gpu_overlap_sort_ms` / `gpu_tile_overlap_sort_time_ms` is that GPU duration (resolved with the other
tile-renderer timestamps), and `overlap_sort_cpu_dispatch_ms` / `tile_overlap_sort_cpu_dispatch_ms` is the separate
CPU dispatch time. Read each with its `_valid` flag. Prefix sync fallback timing is
CPU wall-clock and is valid only when deterministic readback forces a blocking path.

Validation checks:

- all timing fields must be finite and non-negative;
- cull/sort/raster stage timings must be non-placeholder for meaningful successful workloads;
- when a GPU timing frame sample is available, GPU pass breakdown must not collapse to all-zero placeholders.
