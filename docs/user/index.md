---
title: Guides
hide:
  - toc
---

<div class="gs-landing-header" markdown>

# Guides

Use godotGS day to day: import splat content, understand behavior, and tune quality, lighting, and performance without digging through maintainer docs first. These guides are written for artists, technical artists, and non-programmers as much as for engineers.

The import, colour grading, artist pipeline, runtime behavior and world bake guides include real editor screenshots and rendered captures. Short workflow clips are still pending.

</div>

<div class="gs-section-shell" markdown>

## Recommended Order

1. Confirm the editor and the sample scene work with [Your First Splat](../getting-started/quick-start.md).
2. Bring in a real asset with the [Gaussian Splat Asset Import Workflow](../workflows/importing.md).
3. Use [Performance Presets](manual/performance-presets.md) before touching advanced knobs.
4. Move into the [Gaussian Splat Artist Pipeline](../features/artist_pipeline.md) when you need brush tools, hot reload, or bake actions.
5. Use the [Color Grading Quick Start](../features/color-grading-quick-start.md) when the scene is readable and you are tuning look, not import correctness.

## Most Common Tasks

<div class="grid cards" markdown>

- __Learn the core concepts__

    ---

    Get the minimum shared vocabulary for assets, nodes, streaming, and sorting.

    [Open concepts](manual/concepts.md)

- __Import a splat asset__

    ---

    Use the maintained import workflow for `.ply` and `.spz` content.

    [Open import workflow](../workflows/importing.md)

- __Bake a world__

    ---

    Merge several source assets into one runtime `.gsplatworld` resource.

    [Open world bake workflow](../workflows/GSPLATWORLD_BAKE.md)

- __Browse the feature guides__

    ---

    Import, colour grading, the artist pipeline, streaming and animation, each with its own guide.

    [Open features](../features/index.md)

- __Color grade a scene__

    ---

    Use the scoped feature quick start when you are tuning look, not debugging import.

    [Open color grading quick start](../features/color-grading-quick-start.md)

- __Tune runtime behavior__

    ---

    Start with the first safe knobs for runtime quality and stability.

    [Open runtime behavior](manual/runtime-behavior.md)

- __Choose a performance preset__

    ---

    Pick the right quality and speed trade-off before doing fine-grained tuning.

    [Open performance presets](manual/performance-presets.md)

- __Export a game__

    ---

    Point your export preset at the godotGS export template, or the exported game renders no splats.

    [Open export templates](../development/export-templates.md)

</div>

</div>

<div class="gs-section-shell" markdown>

## Workflow Details

Use these once you already know the job and need the next supporting page.

### Import

- Put source files inside the project.
- Assign the imported asset to `GaussianSplatNode3D`.
- Verify a first render before you tune quality or lighting.

Related pages: [Import workflow](../workflows/importing.md), [PLY loader reference](../features/ply-loader.md).

### Tuning

- Start with the `Balanced` preset (`GaussianSplatNode3D` property `quality/preset`).
- Adjust render distance (`quality/max_render_distance`) and max splat count (`quality/max_splat_count`) only after the scene is visible.
- Validate image quality and frame stability before going deeper.

Related pages: [Artist pipeline reference](../features/artist_pipeline.md), [Performance presets](manual/performance-presets.md).

### Runtime and Lighting Checks

1. Confirm base runtime stability first.
2. Tune the lighting controls that change readability the most.
3. Use troubleshooting only after the normal controls fail.

Related pages: [Runtime behavior](manual/runtime-behavior.md), [Lighting behavior](manual/lighting-behavior.md), [Recurring issues](../troubleshooting/recurring-issues.md).

### Bake and World Workflows

Use the bake workflow when you want one merged runtime world resource instead of several source assets: [Gaussian Splat World Bake Workflow](../workflows/GSPLATWORLD_BAKE.md).

### Effectors

A scene-authored `SphereEffector3D` node can displace splats (`affect_position`) and change their opacity (`affect_opacity`) inside its `radius`. The [Sphere Effector Workflow](../api/sphere_effector_workflow.md) describes the runtime surface, and the [Effector System Consumer Guide](../api/effector_system_consumer_guide.md) covers correct use from game code.

</div>

<div class="gs-section-shell gs-section-shell--compact" markdown>

## All Guides

- [Concepts](manual/concepts.md)
- [Runtime behavior](manual/runtime-behavior.md)
- [Lighting behavior](manual/lighting-behavior.md)
- [Performance presets](manual/performance-presets.md)
- [Export templates](../development/export-templates.md)
- [FAQ](manual/faq.md)

## If Something Looks Wrong

- [Recurring issues and fixes](../troubleshooting/recurring-issues.md)

## Related References

- [Project settings reference](../reference/project-settings.md)
- [Compatibility matrix](../reference/compatibility-matrix.md)

</div>

<div class="gs-section-shell gs-section-shell--compact" markdown>

## Technical Flow Reference

<figure markdown="1">
![Diagram of the artist workflow handoff from the first splat to import, presets, iteration tools, and finishing tasks](../assets/images/artist-workflow-lane.svg){ .gs-diagram }
<figcaption>The artist lane starts only after the first splat is visible, then moves through import, safe preset selection, iteration tools, and finishing tasks like bake or color grading.</figcaption>
</figure>

</div>
