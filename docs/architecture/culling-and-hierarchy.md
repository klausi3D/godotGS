# Culling and Hierarchy

This page describes how coarse culling works in the Gaussian Splatting module today, and which
spatial-hierarchy code is a fallback rather than the production path. It was re-verified against
the code on 2026-10-01.

There is no cluster-culling stage in the render path, and no project setting for one. The cluster
stack (`ClusterCuller`, `ClusterBuilder`, `cluster_cull.glsl`) was deleted; see
[Removed cluster stack](#removed-cluster-stack).

## Current coarse culling path: GPU instance/chunk frustum cull

Both production routes publish the same atlas-shaped instance contract and run the same cull
stage. The resident route reaches it through `RenderInstancingOrchestrator::render_instanced`, once
per instance pass. The streaming route reaches it through
`RenderStreamingOrchestrator::render_streaming_frame` and `RenderPipelineStages::execute_frame_entry`
(see [Render pipeline architecture](render-pipeline.md)). The coarse cull is a GPU frustum test
over the chunks of the published contract (see
[Resident-instanced renderer contract](gaussian-resident-instanced-contract.md)).

Call chain from the cull stage:

`RenderPipelineStages::execute_cull_stage` -> `RenderPipelineStages::cull_for_view` ->
`GaussianSplatRenderer::_cull_for_view` -> `RenderQualityOrchestrator::cull_for_view` ->
`GPUCuller::cull_for_view` -> `GPUCuller::_gpu_frustum_cull_instance` -> sort stage
(`RenderPipelineStages::execute_sort_stage`).

- The cull stage requires the instance cull buffers. It checks
  `GaussianSplatRenderer::has_instance_pipeline_buffers()` and
  `InstancePipelineContract::has_cull_buffers()`, then hands the buffers to the culler with
  `GPUCuller::set_instance_pipeline_inputs()`. If the buffers are missing, the stage is skipped
  with route `COMMON.SKIP.NO_DATA` ("Culling skipped: instance buffers missing"). No other culler
  runs in that case.
- `GPUCuller::cull_for_view` tries the instance path first. `_gpu_frustum_cull_instance` dispatches
  [`compute/frustum_cull.glsl`](../../modules/gaussian_splatting/compute/frustum_cull.glsl), which
  tests each chunk's bounds, transformed by its instance transform, against the frustum planes. It
  writes visible chunk references into `visible_chunk_buffer` and updates the chunk counters. The
  summary reports route `INSTANCE.CULL.GPU` and the output domain is chunk references
  (`IndexDomain::CHUNK_REF`).
- Per-splat work happens in the sort stage, not in the culler. `GPUSortingPipeline` expands the
  visible chunks with
  [`instance_chunk_dispatch.glsl`](../../modules/gaussian_splatting/compute/instance_chunk_dispatch.glsl),
  [`depth_compute.glsl`](../../modules/gaussian_splatting/compute/depth_compute.glsl) (which
  emits `splat_ref_buffer` entries and sort keys) and
  [`instance_count_clamp.glsl`](../../modules/gaussian_splatting/compute/instance_count_clamp.glsl).

Sources: [`interfaces/gpu_culler.h`](../../modules/gaussian_splatting/interfaces/gpu_culler.h) /
[`.cpp`](../../modules/gaussian_splatting/interfaces/gpu_culler.cpp),
[`renderer/render_pipeline_stages.cpp`](../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp),
[`renderer/render_instancing_orchestrator.cpp`](../../modules/gaussian_splatting/renderer/render_instancing_orchestrator.cpp),
[`interfaces/gpu_sorting_pipeline.cpp`](../../modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp).

## Fallback: CPU cull over `GaussianData` (static chunks or octree)

`GPUCuller::cull_for_view` still contains an older CPU cull, marked "Legacy path" in the source.
It works on the renderer's primary `GaussianData` (`SceneState::gaussian_data`) or on test
positions, and outputs global gaussian indices (`IndexDomain::GAUSSIAN_GLOBAL`).

When it runs:

1. **The instance cull failed inside the frame pipeline.** The instance buffers were present, but
   `_gpu_frustum_cull_instance` returned false: an invalid device or buffer, or no cull
   shader/pipeline. The summary route becomes `INSTANCE.CULL.CPU_FALLBACK`. The CPU cull then runs
   only if `SceneState::gaussian_data` is valid. Otherwise the route is `COMMON.SKIP.NO_DATA` with
   reason `instance_pipeline_failed_no_fallback`.
2. **Outside the frame pipeline, with no instance inputs set.** One caller is
   `RenderSortingOrchestrator::force_sort_for_view` (`GaussianSplatRenderer::force_sort_for_view`, a
   runtime-validation entry point). It falls through to cull + sort when the streaming route is not
   ready, or when the resident route is preferred. The other caller is the test hook
   `GaussianSplatRenderer::test_cull_visible_count`, which clears `gaussian_data` and culls test
   positions.

Inside the fallback there are two candidate sources:

- **Static chunks.** When `GPUCuller::CullingState::static_chunks` is non-empty, chunk bounding
  spheres are frustum-tested first.
- **Octree.** Otherwise `GPUCuller::ensure_hierarchical_structure()` builds a
  `GaussianSplatting::HierarchicalSplatStructure`
  ([`lod/hierarchical_splat_structure.h`](../../modules/gaussian_splatting/lod/hierarchical_splat_structure.h)),
  stored as `GPUCuller::CullingState::hierarchical_structure`. It is queried with
  `query_visible_splats(frustum, camera, lod_bias, max_query)`, and the result
  (`QueryResult { visible_indices, lod_weights }`) supplies the candidates. The octree is built with
  `culling_state.culling_octree_max_depth` (default `8`) and `culling_state.culling_min_gaussians`
  (default `32`), using `culling_config.lod_bias` from the LOD config
  ([`lod/lod_config.h`](../../modules/gaussian_splatting/lod/lod_config.h)). If the structure is
  not ready, every gaussian is a candidate.

The candidates are then filtered per splat on the CPU (frustum, distance, screen size,
importance). The results go to `culling_state.culled_indices`, `culled_distances_sq` and
`culled_importance_weights`.

What has not been measured: whether the fallback is ever hit in a default production scene. The
analysis above is structural. The `cull_route_uid` render stat (`INSTANCE.CULL.GPU` vs
`INSTANCE.CULL.CPU_FALLBACK`) is the runtime signal to check.

### Note on `GaussianData::octree`

`GaussianData` carries its own octree (`GaussianData::build_octree(...)` /
`GaussianData::query_octree(AABB)` in
[`core/gaussian_data.h`](../../modules/gaussian_splatting/core/gaussian_data.h)). It is a separate
query utility for asset-side spatial lookups. It is not the culler and not the same structure as
`HierarchicalSplatStructure`. Do not conflate the two.

## Removed cluster stack

The cluster stack was deleted in #307 (`2a1fa29ddd1`) after it was found to be dormant: nothing
in the render path constructed or invoked it. The deleted pieces were the `ClusterCuller`
resource (`interfaces/cluster_culler.*`), the CPU Morton helper `ClusterBuilder`
(`lod/cluster_builder.*`), the `compute/cluster_cull.glsl` shader, the unused
`GPUCuller::cluster_culler` field, the `ClusterCullIndirectDispatchLayout` struct, the
`GDREGISTER_CLASS(ClusterCuller)` registration, and the `cluster_*` fields of
`GPUCuller::CullingConfig`, which only fed a pipeline-state hash. The project settings it read
(`culling/cluster_culling_enabled`, `cluster_target_size`, `cluster_frustum_slack`) were removed
with it. [Project settings](../reference/project-settings.md) lists them as deleted.

## Possible future cluster culling

Cluster culling inside chunks is an unimplemented proposal:
[tier2_cluster_culling_spec.md](tier2_cluster_culling_spec.md), added in
[#246](https://github.com/klausi3D/godotGS/pull/246). It would add a stage between the chunk
frustum cull and `depth_compute.glsl`, with chunk-local cluster records and four new shaders. None
of that code exists. Any implementation must be written against the tier-2 instance contract; the
deleted stack worked on whole-scene `GaussianData` and cannot be revived.
