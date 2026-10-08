---
title: Start Here
hide:
  - toc
---

<div class="gs-landing-header" markdown>

# Start Here

**For:** Godot users who want to put Gaussian splat captures in a Godot scene.

**At the end of this section:** you have a godotGS editor, a splat on screen in the sample project, and a route to importing your own capture.

godotGS is a fork of Godot 4.5 with Gaussian splatting built in, so it runs as its own editor, not as a plugin for stock Godot. It is alpha software: nightly editors are published for Windows and Linux, macOS means building from source, and the [known limitations](../development/known-public-alpha-limitations.md) list what this alpha ships with.

</div>

<div class="gs-section-shell" markdown>

## The Path

<div class="grid cards" markdown>

- __1. Get an editor__

    ---

    Download a nightly editor for Windows or Linux, and learn which build is fast and which is not.

    [Open Downloads](downloads.md)

- __2. Your first splat__

    ---

    Open the sample project, press Play, and see a splat in the viewport.

    [Open Your First Splat](quick-start.md)

- __3. Import your own capture__

    ---

    Bring a `.ply` or `.spz` capture into your project and render it.

    [Open the Import Workflow](../workflows/importing.md)

- __4. Keep going__

    ---

    Concepts, quality presets, lighting and the feature guides.

    [Open the Guides](../user/index.md)

</div>

</div>

<div class="gs-section-shell gs-section-shell--compact" markdown>

## Building From Source

On macOS, or if you want representative speed on Linux or a custom build, build the editor yourself: [Build Your Own Editor](installation.md) lists the prerequisites, and [Build from Source](../BUILDING.md) has the commands.

## When Something Goes Wrong

- [Troubleshooting](../troubleshooting/index.md): nothing renders, black splats, the wrong editor, exports without splats.
- [FAQ](../user/manual/faq.md): short answers to common questions.
- [Compatibility Matrix](../reference/compatibility-matrix.md): what is tested on which platform.

## Reference

- [Migration Guide](../migration/index.md)
- [Project Settings Reference](../reference/project-settings.md)
- [API Reference](../api/index.md)

</div>
