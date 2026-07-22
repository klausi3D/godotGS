---
title: Features
hide:
  - toc
---

<p class="gs-eyebrow">Features</p>

# Everything you need to ship reality

From a raw capture to a running scene, godotGS keeps the whole splatting workflow inside the engine — no exporting, no separate viewer, no guesswork. Each guide below is maintained in this repository.

<div class="grid cards" markdown>

-   __Import any capture__

    ---

    Load `.ply` / `.splat` files and COLMAP camera poses. They land as a `GaussianSplat3D` node in your scene tree, ready to orbit.

    [PLY loader technical details →](ply-loader.md)

    <span class="gs-card-meta">ply · splat · colmap</span>

-   __Grade in the editor__

    ---

    Exposure, white balance, and a non-destructive color-grading bake — tuned live in the Inspector, then baked into the resource.

    [Color grading quick start →](color-grading-quick-start.md)

    <span class="gs-card-meta">exposure · white balance · bake</span>

-   __Author the pipeline__

    ---

    The day-to-day artist flow: from a raw capture to a baked `.gsplatworld`, with the editor tools you already use.

    [Gaussian splat artist pipeline →](artist_pipeline.md)

    <span class="gs-card-meta">import · bake · residency</span>

-   __Stream millions of splats__

    ---

    Distance-based LOD, per-chunk quantization, and a streaming queue keep multi-million-splat scenes fluid at runtime — not just in a demo.

    [Streaming system →](streaming.md)

    <span class="gs-card-meta">lod · quantization · octree chunks</span>

-   __Animate splats__

    ---

    Drive splat transforms and node properties from the timeline, alongside the rest of your Godot scene.

    [Animation system →](animation.md)

    <span class="gs-card-meta">timeline · transforms · properties</span>

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
