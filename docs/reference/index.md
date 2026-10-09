---
title: Reference
hide:
  - toc
---

<div class="gs-landing-header" markdown>

# Reference

Use this section when you need exact names, settings, compatibility evidence, or migration guidance for godotGS.

</div>

<div class="gs-section-shell" markdown>

## Most Common Tasks

<div class="grid cards" markdown>

- __Browse the API surface__

    ---

    Start from the maintained API index before diving into individual classes.

    [Open API index](../api/index.md)

- __Read the node API__

    ---

    Check the primary node-level contract used by most scene integrations.

    [Open GaussianSplatNode3D](../api/gaussian_splat_node3d.md)

- __Look up project settings__

    ---

    Review module settings, defaults, and operational controls.

    [Open project settings](project-settings.md)

- __Check compatibility status__

    ---

    Confirm current platform support and the evidence behind it.

    [Open compatibility matrix](compatibility-matrix.md)

- __Inspect generated script docs__

    ---

    Use the generated GDScript reference when you need a script-level inventory.

    [Open GDScript API](../api/gdscript_reference.md)

- __Inspect shader docs__

    ---

    Review documented shader functions and uniform blocks without opening GLSL directly.

    [Open shader reference](../api/shader_reference.md)

</div>

</div>

<div class="gs-section-shell gs-section-shell--compact" markdown>

## Related References

- [Color grading reference](color-grading.md)
- [Migration guide](../migration/index.md)
- [Performance dashboard](../performance/index.md)
- [Timing metrics reference](../timing_metrics_reference.md)
- [Engine patch report](engine-patch.md)
- [Build / Test / CI reference](build-test-ci.md)
- [Renderer release gates](renderer-release-gates.md)
- [Recurring issues](../troubleshooting/recurring-issues.md)

</div>

<div class="gs-section-shell gs-section-shell--compact" markdown>

## Image Credits

The screenshots on the homepage and in the guides show real captured scenes rendered by godotGS. The scans are third-party work, used under their licences:

- **Mip-NeRF 360 "bicycle" and "garden"** by Jonathan T. Barron, Ben Mildenhall, Dor Verbin, Pratul P. Srinivasan and Peter Hedman ("Mip-NeRF 360: Unbounded Anti-Aliased Neural Radiance Fields", CVPR 2022), licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The godotGS project trained the splats from the published photos with gsplat 1.4.0 (`simple_trainer`, default settings, 30,000 steps, quarter-resolution images): 6,076,956 splats for "bicycle" and 5,824,664 for "garden".
    - "bicycle" is on the homepage and in [Runtime Behavior](../user/manual/runtime-behavior.md).
    - "garden" is in [Your First Splat](../getting-started/quick-start.md), [Import Workflow](../workflows/importing.md), [Color Grading Quick Start](../features/color-grading-quick-start.md) and [Artist Pipeline](../features/artist_pipeline.md).
- **[Knock Community Hall](https://superspl.at/scene/0ff2e6dc)** by scbenoit, licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). We converted the published SOG file to PLY with PlayCanvas `splat-transform`: 1,935,275 splats. SOG is lossy, so this is not the original training output. It is in [World Bake](../workflows/GSPLATWORLD_BAKE.md).

The images are our adaptations of these works: we trained or converted the splats, rendered them, and cropped or resized the frames.

How the images were made:

- **Build:** an optimized godotGS editor (`optimize=speed_trace`) built from [PR #1226](https://github.com/klausi3D/godotGS/pull/1226) before it merged, based on `master` at `ab74e332`. The PR's head, `762fbe1282e`, differs from the built commit only in a template README. The editor's version label reads 4.5.rc.
- **Hardware:** NVIDIA RTX 3090, Vulkan Forward+, Windows 11.
- **Project:** made from the repository's project template, `templates/gaussian_splat_template`, with the scene's directional light and ground hidden.
- **Settings that differ from the defaults:** a black background with fog off; `rendering/gaussian_splatting/gpu_sorting/max_raster_splats_per_tile` 65,536 (default 0, auto); `rendering/gaussian_splatting/lighting/direct_light_scale` and `rendering/gaussian_splatting/lighting/shadow_strength` 0; `rendering/gaussian_splatting/lod/min_screen_size_pixels` 0; and on the splat node, Quality preset Custom with LOD bias 0.1, maximum render distance 100,000 m and maximum splat count 100,000,000. Imports used the default `ultra` preset, which keeps every splat.

</div>
