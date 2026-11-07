# Recommended Reading Order

Related docs: [ARCHITECTURE](ARCHITECTURE.md), [ABBREVIATIONS](ABBREVIATIONS.md), [README](README.md)

For new contributors to understand the Gaussian Splatting module.

> **Map, not checklist.** Several entries are 3,000–5,000-line sources. For a task,
> open only the entries it touches. In large files, search for the symbol and read
> the surrounding range rather than the whole file.

## Level 1: Core Concepts (Start Here)
1. `README.md` - Overview and runtime data flow
2. `core/gaussian_data.h` - Data structures (Gaussian, PackedGaussian)
3. `nodes/gaussian_splat_node_3d.h` - Main node API

## Level 2: Rendering Pipeline
4. `renderer/gaussian_splat_renderer.h` - Renderer interface (implements `IRenderer`)
5. `renderer/render_pipeline_stages.cpp` - Cull -> Sort -> Raster -> Composite
6. `renderer/tile_renderer.cpp` - Tile-based rasterization
7. `renderer/tile_render_resources.h` - Owns the graphics raster pipeline (pre-created at module init, PR #345)
8. `interfaces/output_compositor.cpp` - Scratch-copy composite path. Read the comments around `copy_to_render_target` and the `u_destination_scratch` binding before touching the compute path - the workgroup R/W hazard that motivated this design (#256, fixed by #332) will silently recur if the destination is sampled in-place.
9. `shaders/viewport_blit.glsl` - Companion shader; note `u_destination_image` is `writeonly` and reads go through `u_destination_scratch` (binding 4).

## Level 3: Streaming System
10. `core/gaussian_streaming.h` - Streaming state structs (T8)
11. `core/gaussian_streaming.cpp` - Chunk management, VRAM budget, right-sized persistent buffer (PR #344)
12. `core/streaming_atlas.cpp` - Atlas allocator + `resize_preserve` path used by the persistent buffer
13. `renderer/gpu_memory_stream.cpp` - GPU upload mechanics
14. `MEMORY_SUBSYSTEM.md` - Resident vs streaming memory paths and budget flow

## Level 4: GPU Sorting
15. `renderer/gpu_sorter.h` - Sorter interface
16. `renderer/gpu_sorting_constants.h` - Algorithm thresholds
17. `renderer/gpu_sorter.cpp` - Bitonic, Radix, OneSweep implementations

## Level 5: Shader Compilation and Diagnostics
18. `renderer/shader_compilation_helper.cpp` - Entry point for compiling module shaders
19. `renderer/spirv_disk_cache.h` - Persistent SPIR-V cache; read the comments on `try_load`/`store` taking `RenderingDevice` to understand the per-device-dir invariant (PR #343)
20. `logger/startup_trace.h` - `GS_STARTUP_SCOPE(name)` instrumentation; read this before adding new init-time work, so the new phase shows up in `[StartupTrace]` lines (PR #342)

## Level 6: Advanced Topics
21. `interfaces/` - Dependency-inverted interfaces (see `renderer_interfaces.h` for `IRenderer`, `output_compositor_interfaces.h` for the compositor contract)
22. `lod/` - Level-of-detail management
23. `painterly/` - Artistic rendering effects
24. [`docs/architecture/renderer-lifetime-ownership.md`](../../docs/architecture/renderer-lifetime-ownership.md) - Per-owner GPU resource lifetime contract (create/destroy/idempotency/threading) at file:line precision. Read before changing any teardown path (work package #352).

If you are investigating refactor outcomes, scan `render_*_orchestrator.*` (T9) after Level 2 to see where long-method responsibilities were split.
