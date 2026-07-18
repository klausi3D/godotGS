# Gaussian Splatting Module

This directory contains the Godot engine module implementation.

Related docs: [ARCHITECTURE](ARCHITECTURE.md), [READING_ORDER](READING_ORDER.md), [MEMORY_SUBSYSTEM](MEMORY_SUBSYSTEM.md), [ABBREVIATIONS](ABBREVIATIONS.md).

## Core Entry Points

- Registration and module lifecycle: `register_types.cpp`
- Scene node APIs: `nodes/`
- Renderer pipeline: `renderer/`
- Data and manager systems: `core/`
- Import/load/save paths: `io/`
- GLSL shader sources: `shaders/`
- Diagnostics and tracing: `logger/`
- Runtime and integration tests: `tests/`

## Key Features

- **Tile-based rasterizer** with cull -> sort -> raster -> composite stages (`renderer/render_pipeline_stages.*`). Graphics raster pipeline is pre-created eagerly during `TileRenderer::initialize()` (probable-format guess, lazily reformatted if wrong) to avoid first-frame stalls in the common case. See `ARCHITECTURE.md`.
- **Scratch-copy output compositor** (`interfaces/output_compositor.*`) - eliminates the workgroup R/W hazard that caused the #256 black-blocks regression. See `ARCHITECTURE.md` for details.
- **Persistent SPIR-V disk cache** (`renderer/spirv_disk_cache.*`) - module reloads skip the SPIR-V compile step on cache hit, keyed per-device.
- **Right-sized streaming persistent buffer** - sized to actual scene need rather than a fixed upper bound (`core/gaussian_streaming.*`).
- **GPU sorters** - Bitonic, Radix, and OneSweep variants in `renderer/gpu_sorter.*`.
- **Startup trace** (`logger/startup_trace.{cpp,h}`) - RAII `GS_STARTUP_SCOPE(name)` instrumentation that attributes module-init time across 17 scope sites. Each `[StartupTrace]` line emits at the first rendered frame of an asset open. Gated by `rendering/gaussian_splatting/diagnostics/startup_trace` (default on).

## Build Integration

Build through the bundled Godot source tree:

```bash
scons -C ../.. platform=<windows|linuxbsd|macos> target=editor dev_build=yes modules/gaussian_splatting
```

Use repository-level build docs for full platform details:

- `../../BUILDING.md`
- `../../docs/BUILDING.md`
- `docs/BUILD_AND_TEST.md`

## Validation

Run baseline and module guards from repository root:

```bash
python3 modules/gaussian_splatting/tests/check_build_metadata_consistency.py
python3 tests/ci/run_baseline_qa.py
python3 tests/ci/run_module_tests.py --guard-only
```

For full build/test instructions, the GPU test harness, and integration-test layout, see:

- `docs/BUILD_AND_TEST.md` - build matrix and headless test invocation
- `tests/README.md` - test directory layout and harness usage
- `tests/INTEGRATION_TESTS.md` - integration test coverage and gating
