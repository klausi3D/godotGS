---
title: Features
hide:
  - toc
---

<p class="gs-eyebrow">Features</p>

# From capture to running scene

From a raw capture to a running scene, godotGS keeps the splatting workflow inside the engine, with no separate viewer. godotGS is alpha software. Each guide below is maintained in this repository.

<div class="grid cards" markdown>

-   __Import PLY and SPZ captures__

    ---

    Import supported `.ply` and `.spz` splat captures as native Godot resources. Each imports as a `GaussianSplatAsset`; assign it to a `GaussianSplatNode3D`, or drag it into the 3D viewport and the editor creates the node for you.

    [PLY loader technical details →](ply-loader.md)

    <span class="gs-card-meta">ply · spz</span>

-   __Grade in the editor__

    ---

    Exposure, contrast, saturation and white balance (temperature and tint) through a `ColorGradingResource`, tuned in the Inspector. An optional, reversible bake (`bake_color_grading()`, undone by `restore_color_grading()`) writes the grade into the node's loaded splat colors; it does not rewrite the saved asset.

    [Color grading quick start →](color-grading-quick-start.md)

    <span class="gs-card-meta">exposure · white balance · bake</span>

-   __Author the pipeline__

    ---

    The day-to-day artist flow: from a raw capture to a baked `.gsplatworld`, with the editor tools you already use.

    [Gaussian splat artist pipeline →](artist_pipeline.md)

    <span class="gs-card-meta">import · bake · residency</span>

-   __Stream millions of splats__

    ---

    Streaming in fixed-size chunks, distance-based LOD, and per-chunk quantization are built for multi-million-splat scenes. Frame rates at that density are not yet interactive; see [Performance](../performance/index.md) for the measured numbers.

    [Streaming system →](streaming.md)

    <span class="gs-card-meta">lod · quantization · fixed-size chunks</span>

-   __Animate splats__

    ---

    Keyframe per-splat position, color, opacity, scale and rotation with `GaussianAnimationStateMachine`, then advance and sample it from script. It is opt-in: the renderer does not play these clips on its own, and there is no `AnimationPlayer` timeline integration.

    [Animation system →](animation.md)

    <span class="gs-card-meta">keyframes · per-splat · scripted</span>

-   __Version-controlled media__

    ---

    How screenshots and captures are stored, budgeted, and referenced so the docs stay reproducible.

    [Media guidance →](media.md)

    <span class="gs-card-meta">screenshots · budgets · reproducible</span>

</div>

## Related

<div class="grid cards" markdown>

-   __Canonical import workflow__

    ---

    The end-to-end route for bringing a capture into a project.

    [Import workflow →](../workflows/importing.md)

-   __Baking workflow__

    ---

    How a scene is baked into a `.gsplatworld` asset.

    [Gaussian splat world bake workflow →](../workflows/GSPLATWORLD_BAKE.md)

-   __API reference__

    ---

    GDScript and node API surfaces for the module.

    [API index →](../api/index.md)

-   __Color grading reference__

    ---

    Field ranges, method contracts, and GPU mapping details.

    [Color grading reference →](../reference/color-grading.md)

</div>
