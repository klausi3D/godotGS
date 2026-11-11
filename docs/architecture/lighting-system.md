# Lighting System Architecture

This document explains how lighting and shadow controls flow from settings and node state into GPU parameters and shader execution.

## Canonical Sources

- Lighting defaults and renderer integration: [../../modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp](../../modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp)
- Lighting/shadow parameter assembly in stage setup: [../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp](../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp)
- GPU param packing (`TileRenderParamsGPU`): [../../modules/gaussian_splatting/renderer/tile_render_stages.cpp](../../modules/gaussian_splatting/renderer/tile_render_stages.cpp)
- GPU param layout contract: [../../modules/gaussian_splatting/renderer/gaussian_gpu_layout.h](../../modules/gaussian_splatting/renderer/gaussian_gpu_layout.h)
- Lighting bridge and shadow sampling shaders: [../../modules/gaussian_splatting/shaders/includes/gs_lighting_bridge.glsl](../../modules/gaussian_splatting/shaders/includes/gs_lighting_bridge.glsl), [../../modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl](../../modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl), [../../modules/gaussian_splatting/shaders/includes/gs_directional_shadow.glsl](../../modules/gaussian_splatting/shaders/includes/gs_directional_shadow.glsl)
- Resolve pass shader: [../../modules/gaussian_splatting/shaders/tile_resolve.glsl](../../modules/gaussian_splatting/shaders/tile_resolve.glsl)

## Control Surfaces

### Project Settings (Global)

Primary global lighting controls are initialized under `rendering/gaussian_splatting/lighting/*`:

- `direct_light_scale`
- `indirect_sh_scale`
- `shadow_strength`
- `shadow_receiver_bias_scale`
- `shadow_receiver_bias_min`
- `shadow_receiver_bias_max`

Defaults are set by the file-static `_initialize_lighting_project_settings_defaults()` in `gaussian_splat_renderer.cpp`. The values are read through `gs::settings::get_lighting_settings()` in [gs_project_settings.h](../../modules/gaussian_splatting/core/gs_project_settings.h).

### Node Controls (Per-Node)

Node-level controls that directly affect lighting/shadow-related behavior:

- `rendering/cast_shadow`
- `rendering/color_grading`
- `rendering/wind_*` override controls for animation deformation inputs

Source: [../../modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp](../../modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp)

## Data Flow: CPU to GPU

1. The raster stage in `render_pipeline_stages.cpp` (`RenderPipelineStages::RasterStage`) and the painterly producer (`PainterlyRenderer::populate_painterly_gbuffer`) both fill the lighting, shadow and cluster fields of `TileRenderParams` through one shared applier, `apply_lighting_to_render_params()` in [tile_render_types.h](../../modules/gaussian_splatting/renderer/tile_render_types.h). It takes the settings from `gs::settings::get_lighting_settings()` plus the gathered scene-lighting inputs (#851).
2. `tile_render_stages.cpp` packs these values into `TileRenderParamsGPU`.
3. `gaussian_gpu_layout.h` defines the authoritative memory layout and semantic fields: `lighting_config`, `shadow_strength`, `shadow_bias_config`, `lighting_mode`, and `light_counts`.
4. The binning and resolve shaders consume these fields.

## Direct Lighting Path

Production direct lighting is per splat and runs in the binning pass,
[tile_binning.glsl](../../modules/gaussian_splatting/shaders/tile_binning.glsl). Each splat is lit
at its own position, using scene data, light buffers, shadow helpers and a radius-scaled receiver
bias. The result is baked into the splat colour before resolve.

`lighting_mode.x` (`direct_lighting_mode`) selects which pass adds direct light. The two passes
own disjoint modes, and there is no "both" mode:

- `1`: per-splat. The binning pass bakes direct light and the resolve pass adds none. This is the
  only mode any route uses: `apply_lighting_to_render_params()` always writes
  `direct_lighting_mode = 1`. One dormant caller does not call the applier:
  `TileRasterizer::render` leaves the struct default `0`. Its only caller is
  `PainterlyRenderer::render`, which no production route calls today.
- `0`: resolve-time. [tile_resolve.glsl](../../modules/gaussian_splatting/shaders/tile_resolve.glsl)
  adds direct light per pixel and binning bakes none. No route selects this mode since #1078, and
  it has known defects. It lights at the blended depth and normal, which gives black silhouette
  contours, and it has no shadow input because its receiver bias is fixed at 0. It is kept only
  for evaluation against mode 1 (#1083). `0` is also the struct default of
  `TileRenderParams::direct_lighting_mode`, which is why the applier writes the mode explicitly.

Field semantics are defined on `TileRenderParamsGPU::lighting_mode` in
[gaussian_gpu_layout.h](../../modules/gaussian_splatting/renderer/gaussian_gpu_layout.h), and
mirrored in `shaders/includes/gs_render_params.glsl`. The
[resolve-mode lighting redesign spec](resolve_lighting_redesign_spec.md) targets mode 0, which
production does not run.

## Clustered vs Unclustered Lights

Cluster usage is determined by cluster buffer/config availability and `light_counts.z` semantics in GPU params.

- Cluster decision helper in shader: `gs_use_clustered_lights()` in [../../modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl](../../modules/gaussian_splatting/shaders/includes/gs_lighting_common.glsl)
- Debug override (force unclustered): [../../modules/gaussian_splatting/renderer/gpu_debug_utils.h](../../modules/gaussian_splatting/renderer/gpu_debug_utils.h)

## Shadow System

### Runtime Shadow Sampling

Shadow factors are computed per light type in [../../modules/gaussian_splatting/shaders/includes/gs_directional_shadow.glsl](../../modules/gaussian_splatting/shaders/includes/gs_directional_shadow.glsl):

- directional: `gs_directional_shadow`
- omni: `gs_omni_shadow_factor`
- spot: `gs_spot_shadow_factor`

### Directional Shadow Atlas Path

Directional shadow maps are rendered and blitted through:

- `GaussianSplatRenderer::render_shadow_depth_map`
- shadow output compositor setup
- shadow blit pipeline resources

Source: [../../modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp](../../modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp)

**Fail closed (#1089, #1095 slice 1).** A shadow pass blits only a depth image it rasterized itself: at least one splat (`RasterStageOutput::rastered_splat_count`), not a cached reuse or painterly output, in the rasterizer's current depth target, at exactly the atlas-rect size. Anything else returns `SHADOW_SKIP_NO_CASTER_RASTERED` and writes nothing, for directional, spot, omni dual-paraboloid and omni cube passes alike; an omni cube light's cubemap is re-copied into the atlas only if a splat was written for that light. On the canonical routes the shadow raster rasterizes no splats, so **splats cast no shadows there**; before this guard the pass copied the main camera's splat depth into every atlas rect. Splats still receive shadows.

### HLOD route: splat shadow casters (#1095, #1163)

A renderer that owns an HLOD world submission never runs the colour pipeline for a light view. Its shadow passes draw real casters instead (`GaussianSplatRenderer::_render_hlod_shadow_casters`, [gs_shadow_caster.cpp](../../modules/gaussian_splatting/renderer/gs_shadow_caster.cpp), [gs_shadow_caster.glsl](../../modules/gaussian_splatting/shaders/gs_shadow_caster.glsl), planning in [hlod_shadow_caster_plan.h](../../modules/gaussian_splatting/renderer/hlod_shadow_caster_plan.h)):

- **What casts:** the HLOD draw list the last active main view uploaded: the world cut plus the admitted node instances. Forward+ renders a frame's shadow passes before its GS main view, so a pass draws the previous main view's list, with offsets formed against that view's camera. Rows whose node has `cast_shadow` off are masked; objects the budget rule refused cast nothing. Nothing outside the main view's cut casts (a light-view cut is a later slice).
- **How:** one instanced, depth-only quad per splat, drawn into the atlas rect Godot hands the pass with Godot's own clip transform and depth convention (`set_depth_correction(!flip_y)` with the pass's `flip_y`, which is `true` for directional passes exactly as Godot hands its mesh casters; reverse-Z; `GREATER_OR_EQUAL` against the mesh casters already there) and the pancake clamp. A fragment occludes where the Gaussian's alpha reaches 0.5; its depth follows the Gaussian's mean depth across the footprint, so a tilted sheet does not shadow itself. No sort, no blit. Mesh receivers sample it unchanged; splat receivers already sample the atlas in binning.
- **Refusals, explicit and counted:** spot and omni passes (`shadow_caster_light_type_not_directional`), no active HLOD main view since the bundle was (re)built, a quantized payload, an empty list. A refused pass writes nothing.
- **Observable:** `get_render_stats()` reports `shadow_caster_frame`, `shadow_casters_rastered` (splats submitted to the light-view draws of that frame, summed over cascades; a CPU count, not a fragment count), `shadow_caster_passes_cast`, `shadow_caster_passes_refused` and `shadow_caster_last_refusal`.
- **Per-light accounting (#1313):** `shadow_casters_rastered_per_light` maps integer scene instance RID IDs (`Light3D.get_instance().get_id()`, the scene-cull `Instance::self`, distinct from the LightStorage instance RID) to integer caster totals across every shadow-hook route and early exit. Only successful atlas writes contribute casters; a refused light is present with zero, while a light with no pass is absent. `shadow_no_caster_rastered` counts lights whose total is zero, not individual refused passes. Entries reset on a change of `RSG::rasterizer->get_frame_number()`, advanced once by RenderingServer before viewport rendering and read on the same render thread. `shadow_light_frame` captures the renderer counter at the first identified pass in that RS frame (`-1` before any identified pass). On HLOD it equals `shadow_caster_frame`, or `frame_count - 1` for fresh data. On the canonical route it is the counter before the first identified shadow pass; subsequent shadow rasterization may advance that counter without resetting per-light entries. Accounting never changes rendering counters. Standalone calls without an RS compositor share reset key 0. Invalid RIDs from direct hook calls are ignored, including the frame stamp. Storage is capped at 256 distinct lights per frame; `shadow_light_overflow_passes` counts omitted passes for additional IDs, while already recorded lights continue accumulating. The existing scalar stays HLOD-only; with identified HLOD passes below the cap, its sum equals the per-light totals without 32-bit clamping.
- **GS-COMBINED-1:** the harness maps those IDs to `directional` and `omni_0` through `omni_3` for S1, preserving the scalar as `shadow_casters_rastered_total`. Missing lights stay absent. Variant B's HLOD omni passes are refused (`NOT_DIRECTIONAL`), so reported omnis have zero casters and `shadow_no_caster_rastered >= 4` when all four receive passes; S1 fails on those real values. This accounting does not add omni caster support.

### Transitional Shadow Pass State Contract

The canonical route still routes shadow maps through the normal sorted-splat raster path, but the mutable renderer state is owned by one scoped shadow-pass guard. The guard is the only code allowed to temporarily override:

- `ViewState` light-view fields (`manual_viewport_override`, `manual_viewport_format_override`, camera transform/projection, and `using_scene_data`)
- `SubsystemState::output_compositor`, swapped to the dedicated shadow output compositor for the pass
- `shadow_instance_filter_enabled`, set to shadow-caster filtering for the pass

The invariant is that every return after the guard is constructed, including missing rasterizer, invalid depth texture, owner-alias failure, and blit failure, restores the main view state, main output compositor, and previous shadow-filter mode before control returns to Forward+. `ShadowRenderResult` records shadow-specific route labels such as `SHADOW_FAIL_DEPTH_INVALID` and `SHADOW_FAIL_BLIT` so these failures are distinguishable from main-view render failures.

Production target: replace this scoped mutation with a fully pass-local `ShadowRenderContext` and depth-only shadow raster contract. At that point shadow passes should pass their compositor/filter/depth target through `RenderFrameContext::FrameDeps` or a shadow-specific context without swapping renderer-wide state, and shadow cull/sort/raster metrics should be namespaced away from main-view frame metrics.

## Current Constraint: Soft Shadows in Compute Path

`gs_lighting_bridge.glsl` provides compute-shader stand-ins for Godot's forward-lighting
specialization constants. Light-size (penumbra) soft shadows are disabled there:
`sc_use_light_soft_shadows()` returns `false`, and that is the switch that turns them off. The
filter sample counts are not zero. `sc_soft_shadow_samples()` and
`sc_directional_soft_shadow_samples()` return `4u`, to match Godot's `SHADOW_QUALITY_SOFT_LOW`
filtering. Only the penumbra sample counts (`sc_penumbra_shadow_samples()`,
`sc_directional_penumbra_shadow_samples()`) return `0u`. Projector lights are also unavailable
(`sc_use_light_projector()` returns `false`). This is a current design constraint, not a
documentation omission.

## Related Docs

- [Architecture overview](overview.md)
- [Render pipeline architecture](render-pipeline.md)
- [Project settings reference](../reference/project-settings.md)
- [Generated shader inventory](../api/shader_reference.md)
