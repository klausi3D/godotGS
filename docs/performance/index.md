# Performance Dashboard

This page surfaces the current published benchmark snapshot and the suite lanes that are expected to grow it.

Charts use `assets/data/benchmark_latest.json` generated during docs build.
The current public dataset contains five committed result rows, and the coverage table below shows the remaining user-relevant benchmark lanes already defined in the suite.

## Current Public Snapshot

### Measurement environment

Every number on this page comes from one machine, one build, and one commit. Read the table with this context; results on other hardware will differ.

| Item | Value |
| --- | --- |
| Capture date | 2026-07-19 (UTC) |
| Commit measured | `9161d92f349326e2004088638a5ab43eb4773123` (`master`) |
| Build flags | `scons platform=windows target=editor dev_build=no optimize=speed debug_symbols=no tests=yes` |
| Build type | **Optimized** (`/O2`). Not a `dev_build` binary — see the warning below. |
| GPU | NVIDIA GeForce RTX 3090, 24 GiB, driver 591.86 |
| CPU | AMD Ryzen 7 5800X (8C/16T) |
| RAM | 64 GiB |
| OS | Windows 11 Pro, build 26200 |
| Renderer | Vulkan 1.4.325, Forward+ |
| Profile | `run_benchmark.py --profile performance` |
| Window | Steady-state only (first 3 s of warmup excluded) |

!!! warning "Optimized builds only"
    A `dev_build=yes` binary compiles at `-O0` and inflates CPU-side frame cost by roughly an order of magnitude on this hardware. Numbers from such a build are not performance evidence. The previously published row on this page (`static_baseline`, 74.0 avg FPS) was captured on 2026-03-19 from a `bin/godot.linuxbsd.editor.dev.x86_64` binary under the `quick` profile, on different hardware, with a different fixture size. **It has been replaced rather than compared against** — the two rows do not measure the same thing.

### Results

| Lane | Scene shape | Instances | Visible splats | Avg FPS | Avg frame (ms) | P99 frame (ms) | GPU frame (ms) | GPU mem delta (MiB) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `static_baseline` | Single asset, orbit camera | 1 | 10,000 | 455.1 | 2.20 | 3.05 | 1.69 | ~318 |
| `city_flyover` | High-altitude visibility churn | 16 | 160,000 | 128.7 | 7.77 | 8.33 | 6.83 | ~1,079 |
| `lighting_stress` | Animated light and shading | 9 | 90,000 | 72.6 | 13.78 | 15.15 | 13.90 | ~612 |
| `instance_storm` | Many-instance submission pressure | 36 | 360,000 | 31.5 | 31.75 | 31.82 | 30.09 | ~1,128 |
| `dense_resident_2m` | Dense resident path | 196 | 4,900,000 | 12.3 | 81.02 | 87.47 | 51.93 | ~2,388 |

**Read `dense_resident_2m` as a negative result.** At 4.9M visible splats this configuration renders at ~12 FPS — far below interactive. It is published here because it is the honest ceiling of the current resident path on a high-end desktop GPU, not because it is a good number. It is a zero-weight support lane and is excluded from the aggregate suite score.

Note that the lane's name and its manifest metadata ("2M visible splats", "81 x synthetic_spiral") are stale: as configured today it instantiates 196 nodes and reports 4.9M visible splats. The measured column above is the ground truth; the lane name is not.

`instance_storm` at ~31 FPS is likewise borderline rather than comfortable.

### Where the GPU time goes

Per-pass GPU timestamps resolve on all five lanes and sum to the reported frame total. Values are the mean of three runs.

| Lane | Overlap count | Prefix | Overlap emit | Sort | Raster | Resolve | Total | Sort share |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `static_baseline` | 0.095 | 0.017 | 0.133 | 1.123 | 0.313 | 0.014 | 1.69 | 66% |
| `city_flyover` | 0.237 | 0.017 | 0.351 | 4.844 | 1.767 | 0.014 | 7.23 | 67% |
| `lighting_stress` | 0.459 | 0.017 | 0.601 | 11.745 | 1.123 | 0.015 | 13.96 | 84% |
| `instance_storm` | 1.091 | 0.017 | 1.516 | 26.051 | 1.407 | 0.016 | 30.10 | 87% |
| `dense_resident_2m` | 2.095 | 0.017 | 3.039 | 33.458 | 13.553 | 0.014 | 52.18 | 64% |

The depth sort dominates GPU cost on every lane measured, from 64% to 87% of GPU frame time. That is the single largest optimization target in the current pipeline.

### Measured vs derived

Be precise about what was sampled and what was computed:

- **Measured:** per-frame wall-clock delta (`delta * 1000` in the lane script); per-pass GPU timestamp ranges resolved from the tile renderer.
- **Derived:** `Avg FPS` is the harmonic mean `frame_count / total_time`, not an average of per-frame FPS values. `Avg frame (ms)` is the arithmetic mean of the same deltas. `P99 frame (ms)` is the 99th percentile of measured deltas.
- **Externally measured:** `GPU mem delta (MiB)` is whole-device `nvidia-smi` memory-used at peak minus an idle baseline taken immediately before each run. It is **not** a per-process figure and carries roughly ±200 MiB of noise from other GPU clients on the machine. Treat it as an order-of-magnitude indication, not an allocation accounting.

### Run-to-run variance

Each lane was run three times. Spread in `Avg FPS` (max−min as a share of the mean) was: `static_baseline` 2.6%, `city_flyover` 0.7%, `lighting_stress` 0.4%, `instance_storm` 0.2%, `dense_resident_2m` 0.5%. These lanes are stable enough that a single run is representative. The committed row set is run 2 of 3.

The one metric that moves more is `city_flyover` GPU frame time (~16% spread), because the lane's camera path crosses regions of very different visible density.

### Caveats

- **Synthetic assets, not real captures.** Every lane here resolves to a generated fixture, classified `lightweight_smoke` in the asset manifest. These lanes characterize pipeline cost against known splat counts; they are not a substitute for real-scan content, and per the repo's own asset policy they must not be cited as large-scene evidence.
- **`gpu_frame_time_source` reads `unavailable` on every lane.** This is a reporting gap, not a bad measurement: no code in the module publishes that key, so the benchmark script's default string is always used. The companion flag `gpu_frame_time_valid` is `true`, and the per-pass timings sum exactly to the reported frame total, so the GPU numbers are real. The same applies to `gpu_timing_available`, which reads `false` for the same reason.
- **These lanes use the resident path**, not the instance pipeline (`instance_pipeline_execution_path` is empty). Per-pass GPU timing has previously been observed not to resolve on the instance-pipeline route; that limitation does not apply to the rows above, but it does mean these numbers do not characterize the instance-pipeline route.
- **Not a release gate.** These rows are benchmark evidence. Blocking streaming/runtime readiness is enforced by the runtime validation profile `streaming-gpu-ci`; open-world benchmark proof surfaces are review evidence and remain non-blocking unless the workflow contract changes.

### Reproducing these numbers

```bash
# 1. Optimized build (a dev_build binary will not reproduce these numbers)
scons platform=windows target=editor dev_build=no optimize=speed debug_symbols=no tests=yes

# 2. Generate the benchmark fixtures. Passing --godot-binary is REQUIRED:
#    without it the script falls back to lightweight Python generators and
#    test_splats.ply is written with 1024 splats instead of 10000, which
#    changes every lane that depends on it.
python tests/runtime/prepare_synthetic_assets.py \
  --godot-binary ./bin/godot.windows.editor.x86_64.console.exe

# 3. Run the five published lanes
python tests/runtime/run_benchmark.py \
  --godot-binary ./bin/godot.windows.editor.x86_64.console.exe \
  --project-path ./tests/examples/godot/test_project \
  --profile performance \
  --lane static_baseline --lane dense_resident_2m --lane city_flyover \
  --lane instance_storm --lane lighting_stress \
  --output-dir tests/output/benchmark_suite --no-captures
```

!!! note "`test_splats.ply` is generated, not committed"
    Most benchmark lanes resolve to `res://tests/fixtures/test_splats.ply`, which is gitignored and produced by step 2. If you skip that step the lanes still exit 0, but they instantiate **zero** splat nodes and report a meaningless several-thousand FPS with a passing recommendation. Always confirm a lane's reported visible-splat count is non-zero before trusting its numbers.

### Raw data

The full suite report backing this table, including per-lane telemetry, is committed at [`assets/data/benchmark_suite_report.json`](../assets/data/benchmark_suite_report.json). Host-specific paths in it are normalized to repo-relative form; nothing else is edited. The chart dataset [`assets/data/benchmark_latest.json`](../assets/data/benchmark_latest.json) is generated from it by `scripts/export_benchmark_vegalite.py` and carries only the metric fields the charts consume.

## Coverage Map

| Lane | Purpose | Status |
| --- | --- | --- |
| `static_baseline` | Low-noise raster baseline | Published in `benchmark_latest.json` |
| `city_flyover` | High-altitude visibility-change stress | Published in `benchmark_latest.json` |
| `lighting_stress` | Animated light and shading stress | Published in `benchmark_latest.json` |
| `instance_storm` | Many-instance submission pressure | Published in `benchmark_latest.json` |
| `dense_resident_2m` | Dense resident path, ~4.9M visible splats | Published in `benchmark_latest.json` (zero-weight support lane) |
| `streaming_corridor` | Camera sweep stressing chunk turnover | Defined in the benchmark suite, not yet published |
| `unified_composite` | Integrated all-systems composite lane | Defined in the benchmark suite, not yet published |
| `open_world_corridor_proof` | Chunked large-world proof lane | Defined in the benchmark suite, not yet published |

Do not cite suite-only or unpublished lanes as public performance results. They become public claims only after a real benchmark suite report is exported to `assets/data/benchmark_latest.json` and the snapshot table above is updated.

## Lane Scores Overview

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "cornerRadiusEnd": 4, "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "score", "type": "quantitative", "title": "Score"},
    "color": {"value": "#355caa"},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "lane_name", "title": "Description"},
      {"field": "score", "title": "Score", "format": ".1f"},
      {"field": "weight", "title": "Weight", "format": ".1f"}
    ]
  },
  "width": "container",
  "height": 250,
  "title": "Weighted Lane Scores"
}
```

## Frame Timing

```vegalite
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "data": {"url": "../assets/data/benchmark_latest.json"},
  "mark": {"type": "bar", "cornerRadiusEnd": 4, "tooltip": true},
  "encoding": {
    "y": {"field": "lane_id", "type": "nominal", "sort": "-x", "title": "Lane"},
    "x": {"field": "p99_frame_ms", "type": "quantitative", "title": "Frame Time (ms)"},
    "color": {"value": "#355caa"},
    "tooltip": [
      {"field": "lane_id", "title": "Lane"},
      {"field": "p99_frame_ms", "title": "P99 Frame (ms)", "format": ".2f"},
      {"field": "avg_fps", "title": "Avg FPS", "format": ".1f"},
      {"field": "gpu_time_frame_ms", "title": "GPU Time (ms)", "format": ".2f"}
    ]
  },
  "width": "container",
  "height": 250,
  "title": "P99 Frame Time by Lane (lower is better)"
}
```

## How to Update

1. Run a benchmark: `python tests/runtime/run_benchmark.py --profile everything`
2. Export data: `python scripts/export_benchmark_vegalite.py`
3. Update the current snapshot table above when the published lane set changes.
4. Build docs: `python scripts/build_docs_site.py --strict`

See [Benchmark Suite Runner](../testing/benchmark-suite.md) for full benchmark documentation.
For benchmark invocation flags and CI lanes, see [Build / Test / CI Reference](../reference/build-test-ci.md).

## Runtime Diagnostics and Caching

The module ships a handful of runtime knobs that are intended for users tuning
startup cost and warm-cache behaviour. All of them are surfaced as
ProjectSettings under `rendering/gaussian_splatting/...` and ship with sensible
defaults; nothing here needs to be enabled to render correctly.

### Startup trace

`rendering/gaussian_splatting/diagnostics/startup_trace` (bool, default `true`).

When enabled, each `GaussianSplatNode3D` asset open emits one `[StartupTrace]`
log line on the first rendered frame. The line itemises the cost of init in
roughly fifteen named phases (module register, device request, shader compile,
streaming buffer alloc, atlas build, first-frame raster pipeline create, payload
parse, etc.) plus a `total=` end-to-end duration.

When disabled, the macro is a static-atomic short-circuit with no measurable
overhead, so leaving it on is the default for development builds and benchmarks.

Full output format, phase reference, and consumer-script guidance:
[Startup Trace](startup-trace.md).

### SPIR-V disk cache

The module persists compiled shader binaries to disk so subsequent module
loads can skip the GLSL-to-SPIR-V compile step on a cache hit. The cache is
keyed on shader source, sorted preprocessor defines, and a device fingerprint
(vendor, device name, API/version, pipeline-cache UUID), so driver upgrades
and GPU swaps invalidate automatically without manual housekeeping.

Settings:

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `rendering/gaussian_splatting/cache/spirv_cache_enabled` | bool | `true` | Master switch. |
| `rendering/gaussian_splatting/cache/spirv_cache_max_mb` | int | `64` | LRU cap across all device subdirs (range 4-1024). |

Storage: `user://gsplat_spirv_cache/<device-hash>/<key>.spv` (one subdir per
GPU). On module init the cache is pruned to fit `spirv_cache_max_mb` using an
LRU policy keyed on file mtime; cache hits also touch the file so frequently
loaded shaders are not evicted ahead of cold entries. Stores go through a
`.tmp` write + rename with `.bak` rollback, so a crash mid-write cannot lose
a previously cached blob.

When to disable: only if you suspect a stale or corrupt blob is being served
(e.g. after an out-of-tree shader patch that did not bump the cache version).
Toggle the setting to `false`, restart, and the next compile will run from
source. The cache is safe to delete by hand at any time.

### Streaming persistent buffer sizing

The streaming path allocates a single persistent GPU storage buffer sized
once at asset init. It is now sized from the actual asset's chunk count plus
a 25 percent headroom (with a floor of `STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM`
and a ceiling at the regulated `effective_max_chunks`), rather than always
allocating the full regulated maximum. Eviction pressure can grow the buffer
on demand via `_grow_persistent_buffer()`, which copies the live region to a
larger allocation in-place.

Surfaced metrics (read via `RenderDiagnosticsOrchestrator`):

- `streaming_initial_capacity` - chunks reserved on init
- `streaming_current_capacity` - chunks the persistent buffer currently fits
- `streaming_grow_count` - times the buffer has grown since init

See [Memory Subsystem Guide](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
for the budget regulator and eviction policy that drive growth.

### First-frame raster pipeline pre-create

`rendering/gaussian_splatting/init/eager_raster_pipeline` (bool, default `true`).

The graphics raster pipeline is now built at `TileRenderer` init rather than
lazily on the first dispatch, removing a ~tens-of-ms first-frame stall. The
savings show up in the startup trace as a missing or near-zero
`first_frame_raster_pipeline_create` phase.

If the eager pre-create binds the wrong framebuffer format (rare; only when
the caller-provided format hint disagrees with the real framebuffer built on
first frame), the lazy reformat path inside `dispatch_tile_rasterizer()` frees
the eager pipeline and rebuilds it, so correctness is preserved. Disable the
setting only if you are debugging that fallback path.
