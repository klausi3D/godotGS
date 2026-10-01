# Startup Trace

The Gaussian Splatting module ships a lightweight runtime trace that summarises
the cost of opening an asset, from module registration through to the first
rendered frame, in a single log line per asset open. It is intended for users
tuning startup time, comparing builds, or filing performance bug reports.

Related docs: [Performance Dashboard](index.md),
[Memory Subsystem Guide](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md),
[Build / Test / CI Reference](../reference/build-test-ci.md).

## Enabling and Disabling

| Setting | Type | Default |
| --- | --- | --- |
| `rendering/gaussian_splatting/diagnostics/startup_trace` | bool | `true` |

The setting is read once at module init and re-checked at every asset-open
boundary, so toggling it from a tool script and then opening a new asset will
honour the change without a restart.

When disabled, every `GS_STARTUP_SCOPE(...)` macro short-circuits on a relaxed
atomic load. Cost is a single non-blocking load per scope; leaving the trace
on in development and benchmark builds is the recommended default.

## Where the Trace Arms

The trace is armed only at the four `GaussianSplatNode3D` asset-load entry
points in `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp`:
`_set()` (scene-load migration of the deprecated `ply_file_path` property), `set_splat_asset()`, `_load_asset()` and the
editor drag-and-drop handler `_drop_data_fw()`.
Leaf loaders (`ply_loader`, `spz_loader`, `GaussianSplatAsset::load_from_file`,
the editor import path, headless test fixtures, direct `GaussianSplatAsset`
consumers) do **not** call `begin_asset_open()`. This is intentional: those
contexts have no renderer to consume a queued trace, so arming there would
leak phases into the next real asset open and produce a misleading line.

If you write a new high-level open path, call
`GSStartupTrace::get_singleton()->begin_asset_open()` from it, and make sure
your path eventually drives a frame through `GaussianSplatRenderer` so the
queued trace gets flushed.

## Output Format

One line per asset open, emitted on the first frame rendered after that open.
Format is stable and intended to be machine-parseable:

```
[StartupTrace] <phase1>=<ms> <phase2>=<ms> ... total=<ms>
```

Illustrative sample (not a measurement):

```
[StartupTrace] module_register=0.42ms manager_construct=1.1ms device_request_primary=8.31ms device_request_shared=0.05ms renderer_construct=2.04ms gpu_buffer_manager_init=0.71ms shader_compile_binning=143.22ms shader_compile_prefix=58.4ms shader_compile_raster=212.18ms sorter_create_variant=4.66ms streaming_persistent_buffer_alloc=11.93ms streaming_atlas_build_cpu=27.55ms streaming_atlas_sync_gpu=9.04ms first_frame_raster_pipeline_create=0.0ms ply_payload_parse=361.42ms asset_populate_gaussian_data=84.27ms total=925.3ms
```

Field rules:

- Phases appear in the order they were first recorded for that open. Phases
  that record multiple sub-scopes (e.g. shader compile across variants) are
  summed into one entry; the value is total time across all sub-scopes.
- `total=` is measured from the `begin_asset_open()` call to the moment the
  flush emits, not the sum of the listed phases. The gap captures phases that
  ran outside any instrumented scope.
- All values are milliseconds with up to two decimal places. They are printed with
  `String::num(value, 2)`, which drops trailing zeros but keeps one digit after the point:
  `1.1ms`, `0.0ms`, not `1.10ms`, `0.00ms`.
- Back-to-back opens that arrive before the renderer drains the prior trace
  are sealed and emitted in arrival order, one line each.

## Phase Reference

The phase set is fixed in the module sources; new phases are added by wrapping
new init code in `GS_STARTUP_SCOPE("name")`. Current phases (roughly ordered):

| Phase | Source location | What it measures |
| --- | --- | --- |
| `module_register` | `register_types.cpp` | ClassDB registration and project-setting defaults. |
| `manager_construct` | `core/gaussian_splat_manager.cpp` | Manager singleton construction. |
| `device_request_primary` | `core/gaussian_splat_manager.cpp` | Acquiring the primary `RenderingDevice`. |
| `device_request_shared` | `core/gaussian_splat_manager.cpp` | Acquiring a shared device (multi-window / editor). |
| `renderer_construct` | `renderer/gaussian_splat_renderer.cpp` | Renderer object construction. |
| `gpu_buffer_manager_init` | `renderer/render_resource_orchestrator.cpp` | Resident-path buffer allocation. |
| `shader_compile_binning` | `renderer/shader_compilation_helper.cpp` | Tile-binning compute compile (SPIR-V cache hit makes this near-zero). |
| `shader_compile_prefix` | `renderer/shader_compilation_helper.cpp` | Prefix-sum compute compile. |
| `shader_compile_raster` | `renderer/shader_compilation_helper.cpp` | Raster shader compile (compute and graphics variants). |
| `sorter_create_variant` | `renderer/gpu_sorter.cpp` | Per-config sorter shader instantiation. |
| `streaming_persistent_buffer_alloc` | `core/gaussian_streaming.cpp` | Right-sized persistent storage buffer allocation. |
| `streaming_atlas_build_cpu` | `core/gaussian_streaming.cpp` | CPU-side atlas layout. |
| `streaming_atlas_sync_gpu` | `core/gaussian_streaming.cpp` | Atlas GPU upload. |
| `first_frame_raster_pipeline_create` | `renderer/tile_render_rasterizer_stage.cpp` | Should be ~0 with `init/eager_raster_pipeline=true`. |
| `ply_payload_parse` | `io/ply_loader.cpp` | PLY-side parse. |
| `asset_prefetch_parallel` | `core/gaussian_splat_asset.cpp` | `GaussianSplatAsset::prefetch_gaussian_data_parallel()`: materializing several assets' runtime data on worker threads. |
| `asset_populate_gaussian_data` | `core/gaussian_splat_asset.cpp` | Filling the asset's runtime arrays. |

## Reading the Trace

Two patterns cover most use cases:

1. **Eyeball the slow phase.** Pick the largest value, then check whether it is
   a one-time cost (`shader_compile_*` on first launch before the SPIR-V disk
   cache warms) or a per-load cost (`ply_payload_parse`, atlas build,
   persistent buffer alloc).

2. **Diff across runs.** The format is grep-friendly; piping logs through
   `grep '^\[StartupTrace\]'` gives you one row per open. For a cold-vs-warm
   comparison, run twice with the SPIR-V disk cache disabled then enabled
   (`rendering/gaussian_splatting/cache/spirv_cache_enabled`) and diff the
   `shader_compile_*` columns.

For invocation details and CI lanes, see
[Build / Test / CI Reference](../reference/build-test-ci.md).

## Caveats

- The trace measures only what is wrapped in a `GS_STARTUP_SCOPE`. Engine-side
  costs outside the module (e.g. main-loop boot, scene tree setup) are not
  included; `total=` will be larger than the sum of phases by exactly that gap.
- The first asset open in a process carries pre-asset-open module-init phases
  (recorded before any `begin_asset_open()` call). Subsequent opens start
  clean.
- Headless contexts (tests, the importer, direct `GaussianSplatAsset` users)
  never emit a line because they never reach a rendered frame to flush the
  queued trace. This is by design.
