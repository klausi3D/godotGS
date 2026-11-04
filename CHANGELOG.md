# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Versioned GitHub Pages docs pipeline with media LFS support
- Single-process benchmark orchestrator for unified scene benchmarking
- Visual metrics to synthetic benchmark scenes
- Shader dependency guard and async readback test hardening
- Tile shared-memory raster contract enforcement in editor
- RWLock migration for gaussian core data
- Streaming queue pressure controller and scan throttle
- LOD distance-based chunk reduction (Octree-GS approach)
- Multi-asset atlas registration for streaming
- Per-chunk quantization (Unity-inspired 4x compression)
- Animation state machine with keyframe interpolation and clip blending
- Color grading bake/restore workflow on GaussianSplatNode3D
- Documentation: streaming feature guide, animation feature guide, expanded performance presets
- Renderer diagnostics destroy hook plus cull/sort short-circuit on empty frames (#331)
- Visual-compare helper for compositor regression captures (#332)
- GPU test harness Phases 1-3 with `--gs-gpu-test` entrypoint, `tests/ci/run_gpu_harness.py` supervisor, doctest visual lane in `.github/workflows/baseline_qa.yml`, and seeded captures under `tests/visual_baselines/` (#333)
- Per-phase startup-time trace via `GS_STARTUP_SCOPE` RAII timer and `[StartupTrace]` log lines, gated by `rendering/gaussian_splatting/diagnostics/startup_trace` (default on) (#342)
- Persistent SPIR-V disk cache (`modules/gaussian_splatting/renderer/spirv_disk_cache.{cpp,h}`) (#343)
- Pre-created graphics raster pipeline at init to eliminate first-frame stall (#345)

### Changed

- **Behaviour change: a world with an HLOD tree takes the HLOD route automatically** (ADR
  `docs/architecture/adr-one-route-many-producers.md` §4.7, decision 5).
  - A `GaussianSplatWorld3D` whose world carries an HLOD tree now draws on the HLOD route together with its
    `GaussianSplatNode3D` instances, with no project setting changed. The tree is one over a resident payload (from
    `GaussianSplatWorld.bake_hlod()` or a resident load) or a V4 paged package.
  - **Imported `.gsplatworld` worlds keep their route until the importer writes resident or V4 paged packages
    (follow-up, R3).** The world importer writes a file-backed (streamable) payload, and the runtime cut needs a
    resident payload or a V4 package, so an imported world is not changed by this entry.
  - `rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled` now defaults to `true` (it was `false`). Set it to
    `false` to opt out: the routing is then exactly as before, and a V4 paged package loads its payload into memory.
  - `rendering/gaussian_splatting/streaming/route_policy` keeps its default (streaming). It still picks the backend of
    every world that does not take the HLOD route, but it no longer keeps a world with an HLOD tree on the
    chunk-streaming route. It cannot be the opt-out: a value equal to its default cannot be told from an unset one.
  - Worlds without an HLOD tree are unchanged.
  - Some setups the HLOD route cannot serve: LOD off, wind or sphere effectors, painterly, the shadow instance filter,
    a moved (non-identity) world, or a tree the runtime does not admit. If the world node sees the setup when it
    submits the world, the world keeps the route `route_policy` gives it (chunk streaming by default). It reports the
    cause in the render stat `hlod_reason` (`hlod_auto_route_unavailable:<cause>`, `hlod_world_transform_unsupported`
    or the admission reason) and prints a warning. A V4 paged package loads its payload into memory instead, as
    before. Under `route_policy = resident`, the HLOD route draws the canonical leaves for LOD off, effectors, painterly
    and the shadow filter, as it did with the setting enabled before.
  - Sticky demotion: some refusals only appear once the world is on the HLOD route, for example an orthographic,
    multi-view (XR) or painterly view. Others are a domain that starts later (painterly, the shadow instance filter,
    effector nodes, wind), or quantization over a tree with mixed DC encodings. If the world has no copy that serves
    the frame (a streamed or V4 paged world, or one too large for its whole tree), the first such refusal demotes it to
    the route it has without the automatic route: `route_policy`'s backend, and a V4 paged world loads its payload into
    memory. The world reports `hlod_auto_route_demoted:<cause>` in `hlod_reason`, the renderer stat
    `hlod_auto_route_demotions` counts the demotions, and the world node warns once. The world stays demoted until
    `apply_world()` runs again; re-promotion on the fly is a follow-up. Quantization over a mixed-DC tree is known
    when the world is submitted and demotes it right away. The frame that detects a refusal still draws only the
    objects; the world draws from the next frames on.
  - Memory: an HLOD world now holds its runtime snapshot on the CPU and its HLOD bundles on the GPU, in place of the
    chunk streamer's atlas.
- Benchmark scenes unified under single-process orchestrator
- Streaming path uses shared `gs::settings` helpers (ISSUE-018)
- Removed friend-only access in streaming orchestrator (ISSUE-019)
- Instance sort cache preserved across strict-mode toggles
- Aligned scons arguments between `gpu-tests` and `gpu-harness` lanes (#338)
- `REQUIRE_GPU_DEVICE()` owns its fallback `RenderingDevice` lifetime via `ScopedFallbackRD` (#340)
- RID-leak listener threshold raised to 4 MiB and the gate now fails on leaks (#341)
- Right-sized streaming persistent buffer (#344)

### Fixed

- Duplicated mike version/alias in docs deploy workflow
- GDScript const compatibility in benchmark scripts
- Missing `ColorGradingResource` include and `batches_completed` getter
- Trailing whitespace in sorting pipeline
- CPU fallback and OneSweep support in sorting validation
- Compositor R/W hazard from compute `imageLoad` on live destination (scratch-copy path) — #256 black-blocks regression (#332)
- doctest argv ownership in GPU test harness via RAII (`gs_gpu_test_runner.cpp`, `tests/test_main.cpp`) (#339)
