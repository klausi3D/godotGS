# User Manual: Runtime Behavior

This page explains what artists usually observe while navigating a splat scene, and which controls are safest to touch first.

<figure markdown="1">
![Diagram of the normal runtime progression from warmup through stabilization and safe tuning](../../assets/images/runtime-behavior-checkpoints.svg){ .gs-diagram }
<figcaption>Most runtime sessions follow a short warmup, stabilize, then show expected streaming or sorting changes before any tuning is needed.</figcaption>
</figure>

## What You Should Expect Visually

- First load can take longer while data streams and GPU resources warm up.
- Camera movement should feel stable after load settles.
- Some distant detail popping can happen as streaming updates visible chunks.
- Transparent splat content can shift slightly as sort order updates.

## The Performance Overlay

The sample project's main scene and scenes made from the project template include a performance overlay, a `GaussianSplatPerformanceOverlay` node. It reports frame timing, the time each GPU pass took, visibility, device memory, and the splat node or world it watches (Inspector › Target Path). Press F3 to hide it. Inspector › Hide Key changes the key, and Inspector › Sections picks what it shows.

<figure markdown="1">
![The performance overlay over a captured scene of a white bicycle leaning on a park bench. Left column: FRAME (FPS 25.2), CAMERA, GPU PASSES (overlap sort 22.556 ms, rasterize 8.798 ms, pass total 36.340 ms) and HOST STAGES. Right column: VISIBILITY, DEVICE VRAM (6909.6 MB), LOD, STREAMING, NODE (total splats 6.08M) and MANAGER. The footer reads F3: hide.](../../assets/images/screenshots/runtime-overlay-bicycle.webp){ .gs-shot width="1280" height="720" loading="lazy" }
<figcaption markdown="span">The overlay at runtime, cropped from a 1920×1080 frame: the Mip-NeRF 360 "bicycle" scene, 6.08M splats, on an RTX 3090 (godotGS built from [PR #1226](https://github.com/klausi3D/godotGS/pull/1226)). Most of the 36 ms GPU time goes to the overlap sort. Known bug: the VISIBILITY section's "Total splats (source): 0" line is wrong on this route; the NODE section shows the real count ([#1215](https://github.com/klausi3D/godotGS/issues/1215)). Scan: Barron et al., [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), trained with gsplat by the godotGS project. Not the default settings: black background, scene lighting off, tile cap 65,536, Custom quality. See [Image credits](../../reference/index.md#image-credits).</figcaption>
</figure>

## Common Confusion Points

- "It looked better in one scene than another": light setup and exposure strongly change perceived splat quality.
- "Parts are missing": often `Render Distance` or max splat budget is too low.
- "It stutters only at scene start": this is often startup/streaming work, not a permanent runtime state.
- "Editor view and play mode differ": post-processing and camera setup can differ between scenes.

## Which Setting Knobs Are Safe to Tweak First

1. Preset (`Balanced`, then move toward performance or quality)
2. Max splat count (raise/lower visible density)
3. Render distance (trade distant detail for stability)
4. Only then advanced streaming/sorting options

Quick references:
- [Performance presets](performance-presets.md)
- [Workflow details](../index.md#workflow-details)

## When to Use Troubleshooting Docs

Use troubleshooting pages when behavior is persistent, not a one-time warmup:

- repeated heavy flicker or mis-ordered transparency
- splats consistently fail to appear
- shader/pipeline errors, or runtime starts with no visible result

Start here:
- [Recurring issues](../../troubleshooting/recurring-issues.md)

## Deeper Architecture (Engineers)

For internals and stage-level details:
- [Render pipeline architecture](../../architecture/render-pipeline.md)
- [Architecture overview](../../architecture/overview.md)
