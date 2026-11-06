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
- `gpu_binning_valid`
- `gpu_cull_ms`, `gpu_cull_valid`
- `gpu_projection_ms`, `gpu_projection_valid`
- `gpu_presort_ms`, `gpu_presort_valid`
- `gpu_composite_ms`, `gpu_composite_valid`
- `gpu_coherent_frame_serial`, `gpu_coherent_pass_mask`, `gpu_coherent_total_ms`
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
- `gpu_tile_binning_time_valid`
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

## GPU timing completeness (BV-04b)

ADR `docs/architecture/adr-beyond-vram-worlds.md` (D-4, D-5) needs the fixed cost F of a frame. Before BV-04b only
the tile passes had GPU timestamps. Cull, projection, the depth pre-sort and the composite were untimed.

**Passes.** Every pass of a pipeline pass is bracketed by two RenderingDevice timestamps,
`<prefix><frame serial>_Begin` and `_End` (`renderer/gpu_pass_timing.h`):

| Pass | Prefix | What it brackets |
| --- | --- | --- |
| `cull` | `GSCull_` | frustum / instance-chunk cull (`GPUCuller`) |
| `projection` | `GSProjection_` | depth-key projection: instance chunk dispatch, depth compute, count clamp |
| `presort` | `GSPreSort_` | depth pre-sort of the visible splats (`GPUSortingPipeline` radix sort) |
| `overlap_count` | `TileOverlapCount_` | tile_binning COUNT |
| `prefix` | `TilePrefix_` | tile prefix scan |
| `binning` | `TileBinning_` | tile_binning EMIT (also reported as overlap emit) |
| `overlap_sort` | `TileOverlapSort_` | overlap-record radix sort |
| `raster` | `TileRaster_` | tile raster |
| `resolve` | `TileResolve_` | tile resolve |
| `composite` | `GSComposite_` | compute composite into the engine frame |

Not bracketed: the graphics-fallback composite, the CPU HLOD cut (it has no GPU cull), and passes recorded on a
RenderingDevice other than the tile renderer's submission device. They read as not measured, never as 0 ms.

**Frame serial.** `RenderPipelineStages::prepare_frame_context` allocates one serial per pipeline pass, and every
pass of that pass carries it. The tile renderer takes it through `RenderParams::frame_serial`. Before BV-04b the
depth pre-sort wrote `TileOverlapSort_0`, which the parser could report as the overlap-sort time; serial 0 is now never
a frame.

**Per-frame-serial validity.** RenderingDevice flushes discard some markers on most frames, so per-pass values stay
"sticky" until a pass has not been measured for 120 resolves. Sticky values of different passes can therefore come
from different frames. Each pass publishes the frame serial it was measured in, and the coherent fields describe the
newest frame alone:

- `gpu_coherent_frame_serial`: newest frame with at least one measured pass (0 = none, or aged out);
- `gpu_coherent_pass_mask`: bit i = pass i (table order above, `cull` = bit 0) was measured in that frame;
- `gpu_coherent_total_ms`: the sum of exactly those passes.

To measure F, sum only passes from one frame: the coherent fields, or per-pass values whose `frame_serial`s agree.
Never add sticky values blindly.

**Freshness.** Timestamps are resolved only while the Gaussian pipeline runs. When nothing is drawn (for example, the
node is outside the view), every value above is held unchanged and still reads valid. The nested dictionary carries
`last_resolve_usec` and `coherent_resolve_usec` (`OS.get_ticks_usec()` at the resolve; compare with
`Time.get_ticks_usec()`). If they are older than a frame or two, the values are held, not measured this frame.

**Binning validity.** `binning` is the EMIT pass and shares its timestamp range with overlap emit, so
`gpu_binning_valid` / `gpu_tile_binning_time_valid` equal the emit validity. `gpu_pass_breakdown_available` no
longer counts a non-zero binning time without its flag.

**Where.** Production metrics carry the keys listed above. `get_render_stats()` and the streaming `streaming_state`
carry flat `gpu_<pass>_time_ms`, `gpu_<pass>_time_valid` and `gpu_<pass>_frame_serial` keys for cull, projection,
presort and composite. They also carry a nested `gpu_pass_timing` dictionary with every pass
(`passes.<name>.{time_ms, valid, frame_serial}`) and `coherent_frame_serial`, `coherent_passes`,
`coherent_pass_mask`, `coherent_total_ms`, `coherent_age_resolves`, `coherent_has_all_bv04b_passes`,
`resolve_count`, `last_resolve_usec` and `coherent_resolve_usec`. All of these are available in every build flavour. They are gated only by
`rendering/gaussian_splatting/gpu_sorting/enable_stage_timestamps` (default on).

**Records per splat per HLOD level.** On the HLOD route, each draw record carries its node's level
(`DrawnNodeGPU.level`, the tree height, 0 = leaf). The tile_binning EMIT pass adds the overlap records it emitted, and
1, to the level's slot of a 128-byte counter buffer (binding 24). An always-on async readback samples it.
`get_render_stats()` reports, per slot:

- `hlod_level_records` and `hlod_level_emitted_splats` (GPU, last sampled frame `hlod_level_stats_frame_serial`;
  `hlod_level_stats_samples` counts the samples);
- `hlod_level_records_per_emitted_splat` (-1 where no splat reached the emit loop);
- `hlod_level_drawn_nodes` and `hlod_level_drawn_splats` (CPU, the last main view's world draw records).

There are 16 slots. Slot i is level i for levels 0..13, slot 14 holds level 14 and above, and slot 15 holds object
(instance) records. Records include those later dropped at capacity, which is the demand the frame model's k depends
on.
