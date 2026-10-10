# Recurring Issues

**For:** anyone whose godotGS editor or game does not do what [Your First Splat](../getting-started/quick-start.md) says it should.

**At the end:** you have found your symptom, its likely cause and a fix, or you know what to put in a bug report.

Some problems are known defects of the alpha rather than something in your setup. [Known Public Alpha Limitations](../development/known-public-alpha-limitations.md) lists them, each with its symptom and a workaround where one exists. If your symptom matches an entry there, check the conditions it describes against your project before you debug further.

## The Stock Godot Editor Opened Instead of godotGS

**Symptoms:** the editor reports unknown or missing types such as `GaussianSplatWorld3D` or `GaussianSplatNode3D`; `.ply` and `.spz` source files are not imported as splat assets; the Create New Node dialog has no `GaussianSplatNode3D`.

**Cause:** the editor you started is not built from this fork. godotGS keeps Godot's name and version number, so the two are easy to confuse. A shortcut, a file association or a `godot` on your `PATH` can still point at a stock install.

**Fix:** start the editor you downloaded from [Downloads](../getting-started/downloads.md) or built yourself, by its full path. To check, search for `GaussianSplatNode3D` in the Create New Node dialog: only a godotGS editor has it.

## Nothing Renders

Work through these in order:

1. **Wrong editor or template.** A stock Godot editor or export template runs the scene without splats. See [the section above](#the-stock-godot-editor-opened-instead-of-godotgs) and [Exported Game Renders No Splats](#exported-game-renders-no-splats).
2. **Compatibility renderer.** Splats are drawn with Vulkan compute through Godot's `RenderingDevice`, which the Compatibility (OpenGL) renderer does not have. Set **Project Settings → Rendering → Renderer → Rendering Method** to `forward_plus`, the renderer godotGS is tested with, and restart the editor.
3. **A world node and a splat node in one scene.** With a `GaussianSplatWorld3D` and a `GaussianSplatNode3D` in the same scene, one of them renders nothing. This is a known limitation; see [GaussianSplatWorld3D](../development/known-public-alpha-limitations.md#gaussiansplatworld3d) for the workaround.
4. **No splat asset assigned.** A `GaussianSplatNode3D` draws only after a splat asset is assigned in Inspector › Asset › Splat Asset. Check that the import succeeded; [Import Workflow](../workflows/importing.md) lists the common import failures.
5. **Silent fallback to OpenGL.** godotGS needs Vulkan 1.1 or newer. When Vulkan cannot start, Godot falls back to the Compatibility renderer on its own (`rendering/rendering_device/fallback_to_opengl3` is on by default), and splats then do not render, as in item 2. At startup the editor prints a line naming its renderer: `Vulkan … - Forward+ - Using Device …` is correct, `OpenGL API … - Compatibility - …` means the fallback happened. Update your graphics driver.

## Black or Dark Splats

- **No light, and indirect light switched off.** The project setting `rendering/gaussian_splatting/lighting/indirect_sh_scale` scales each splat's captured color. Its default is `1.0`. At `0.0` the captured color is multiplied by zero, and only lights in the scene add color, so a scene without a light renders black splats. The sample project sets it to `0.0` and adds a `DirectionalLight3D`; if you delete that light, the splats go black. Set the value back to `1.0`, or add a light.
- **Black rectangles in far views.** Dense content seen from far away can show black, tile-shaped holes. This is a known limitation with a project-setting workaround; see [Far views of dense content can show 16 px black tiles](../development/known-public-alpha-limitations.md#rendering).
- **Splats darken themselves under a shadow-casting light (older nightlies only).** On nightlies up to `nightly-20261003`, a splat node with `rendering/cast_shadow` on, lit by a `DirectionalLight3D` with shadows, rendered dark, and the starter template rendered its splats almost black. Nightlies from `nightly-20261004` fix this: download a newer one. Splats still cast no shadow of their own; see [Splats cast no shadows](../development/known-public-alpha-limitations.md#rendering).

## Exported Game Renders No Splats

**Cause:** the export used a stock Godot export template. The export still succeeds, with no error and no warning, but the game has no splat renderer.

**Fix:** set `custom_template/release` in your export preset to a godotGS export template, and clear **Export With Debug** in the export file dialog; it is on by default. A debug export, like the editor's one-click remote-debug run, reads `custom_template/debug` instead, and no godotGS debug template is published yet, so it never gets a godotGS template. Windows templates come with nightlies whose Windows build passed ([Downloads](../getting-started/downloads.md)); for Linux, build one yourself. [Export Templates](../development/export-templates.md) has the steps and a one-line check that tells you which template an export used.

## Slow on the Linux Nightly

The Linux nightly editor is an unoptimized build, so it runs far slower than godotGS really does. That is expected: [Downloads](../getting-started/downloads.md#linux-nightly-speed) explains it and how to get an optimized editor. The Windows nightly editor is optimized.

On any build, a lower [quality preset](../user/manual/performance-presets.md) is the first thing to try when a scene is slow.

## Crash or Error When the Game Exits

On nightlies up to `nightly-20261004`, a project that renders on a separate render thread (the starter template does, through `rendering/driver/threads/thread_model=2`) could end with an error or crash when you closed the game. Nightlies from `nightly-20261005` fix this: download a newer one.

One crash is still open: polling `GaussianSplatNode3D.get_statistics()` from a script every frame can crash the engine. See [get_statistics() can crash](../development/known-public-alpha-limitations.md#scripting-api) for the workaround.

## Windows Warns About the Download

No godotGS binary is code-signed, so Windows SmartScreen may warn about the editor and about games exported with the godotGS template. Check each download against its `.sha256` file as shown in [Verify the Download](../getting-started/downloads.md#verify-the-download).

## No Nightly Works on Your Machine

Build an editor from this fork yourself: [Build Your Own Editor](../getting-started/installation.md) lists the prerequisites. macOS has no nightly at all.

## Getting Help

Open an issue on [GitHub](https://github.com/klausi3D/godotGS/issues) and include:

- what you did, step by step, and the exact command if you used one;
- your platform, GPU and driver version;
- the nightly tag (from `BUILD-INFO.txt`) or the commit you built from;
- the relevant part of the editor's Output panel or console log;
- whether the editor came from a godotGS nightly, your own build of this fork, or possibly a stock Godot install.

## For Contributors

The contributor runbook that used to be on this page (sorting, tile overflow, shader contract and build-path failures, and which test runner narrows each one down) is now in the [Testing Setup Guide](../testing/setup-guide.md#narrowing-a-renderer-symptom).
