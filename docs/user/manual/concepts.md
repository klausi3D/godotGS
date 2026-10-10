# User Manual: Concepts

## What This Module Does

Gaussian splatting renders scenes from splat data (`.ply` / `.spz`) instead of traditional meshes.

## Glossary

Each concept has one name in these docs. The "Not" column lists names to avoid.

| Term | Meaning | Not |
| --- | --- | --- |
| Splat | One 3D Gaussian: a position, size, rotation, opacity and colour. A captured scene is made of millions of them. | point, particle |
| Source file | The `.ply` or `.spz` file you copy into the project. | asset |
| Splat asset (`GaussianSplatAsset`) | The resource Godot imports from a source file. Assign it in Inspector › Asset › Splat Asset. The `.gsplatcache` file next to a `.ply` is a load cache, not the asset. | splat file, PLY asset, model, cloud |
| `GaussianSplatNode3D` | The scene node that draws one splat asset. After first use on a page, "splat node". | GS node, splat object |
| World file (`.gsplatworld`) | A file the [world bake](../../workflows/GSPLATWORLD_BAKE.md) writes by merging several source files, or the splat nodes of one scene, into one chunked world. | baked asset, world cache |
| World (`GaussianSplatWorld`) | The resource Godot loads from a world file. Write "world" alone only where "world file" or "world node" already set the context. | scene, level; Godot's `World3D` is unrelated |
| World node (`GaussianSplatWorld3D`) | The scene node that draws a world. Assign it in Inspector › World. | streaming node, world renderer |
| Quality preset | Per splat node: Inspector › Quality › Preset, one of Performance, Balanced, Quality or Custom. See [choosing a quality preset](performance-presets.md). | performance preset, quality level, tier |
| Quality tier | Project-wide hardware defaults, such as streaming budgets: `rendering/gaussian_splatting/quality/tier_preset`, for example `low`, `medium`, `high` or `steam_deck`. The default, `custom`, applies none. | quality preset, hardware preset |
| Import preset | Per source file: Import dock › Quality › Preset, one of `mobile`, `desktop`, `high`, `ultra`, `development` or `custom`. Sets import options such as the splat cap and quantization. | quality preset |
| Other presets | Always qualified: sorting preset (`rendering/gaussian_splatting/gpu_sorting/gpu_preset`), painterly style preset (Style Preset on a `PainterlyMaterial`), export preset (Godot's Export dialog). | "preset" on its own |
| Painterly | An optional stylised look for one splat node that draws brush strokes. Turn it on with Inspector › Painterly › Enabled; a `PainterlyMaterial` in Inspector › Painterly › Material shapes it. | NPR mode, paint mode, stylized mode |
| SH (spherical harmonics) | Per-splat colour coefficients that let a splat's colour change with the viewing direction. | harmonics |
| DC | SH band 0: a splat's base colour, the same from every direction. In a PLY file, the `f_dc_*` properties. | diffuse, albedo |
| SH bands | How many SH bands are rendered. Band 0 is DC alone; each higher band adds finer view-dependent colour and costs memory. Set with `rendering/gaussian_splatting/rendering/sh_bands`; [Project Settings](../../reference/project-settings.md) lists the range and default. `SHn` means bands 0 to n. | SH level, SH quality, SH order |
| Resident route | The content is uploaded to the GPU as a whole, with no chunk loading. Splat nodes always use it. A world node uses it when `rendering/gaussian_splatting/streaming/route_policy` is `0` and its world is held in memory, as a compressed world file is. If the content is larger than the GPU memory set aside for it (at most 60% of VRAM and 2 GB, or less with `rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb`), only the most important splats are kept and the content looks thinner. | resident mode, in-memory path |
| Streaming route | A world node loads and evicts chunks of its world around the camera to stay within a VRAM budget. The default for world nodes (`route_policy` `1`). An uncompressed world file loaded the normal way always uses it, whatever `route_policy` says. | streaming mode, streamed path |
| Sorting | Ordering splats by depth, so transparency blends correctly. | |
| Nightly | A prerelease tagged `nightly-YYYYMMDD`. The release workflow publishes one from `master` every night; a maintainer can also publish one by hand, from any branch. | snapshot, dev build (`dev_build` is a build option) |
| Release channel | How a build is published: CI artifact, nightly or stable tag. See [Release Channels](../../development/release-channels.md). | track |
| Your First Splat | The Get Started page that takes you from an editor to a visible splat: [Your First Splat](../../getting-started/quick-start.md). | Quick Start, Public Evaluator, First Run |

## When to Tune

- Use the quality preset first.
- Only change advanced settings when you see visible artifacts or performance problems.

## Behavior Guides

- [Runtime behavior](runtime-behavior.md) for expected behavior and the first safe controls.
- [Lighting behavior](lighting-behavior.md) for the lighting controls that matter first.

## Related

- [Guides home and workflow details](../index.md#workflow-details)
- [Performance presets](performance-presets.md)
- [FAQ](faq.md)
- [Architecture overview](../../architecture/overview.md) if you need the engine-level model.
