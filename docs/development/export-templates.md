# Export Templates

## Purpose

Explain how to export a playable game from this fork, and why an export can
succeed and still produce a binary that renders no splats.

## The one thing you must not skip

**Set `custom_template/release` explicitly in your export preset.** If you leave
it empty, Godot falls back to a stock (upstream) export template, which contains
no Gaussian Splatting module. **The export still succeeds. The resulting binary
silently renders nothing.** There is no error, no warning, and no missing-file
dialog — the game just runs with your splat nodes doing nothing.

This is not hypothetical. A `GrandmasHouse` export made on 2026-01-21 with
`custom_template/release=""` produced an `.exe` containing zero `GaussianSplat`
symbols (issue #825). A correct export made on 2026-07-01 —
`exhibition/splat_kabinett`, with `custom_template/release` pointed at a locally
built template — produced a binary that does contain them.

Check any export you make:

```bash
# Should print matches. Zero matches means a stock template was used.
grep -a -c GaussianSplat path/to/your_game.exe
```

## Where to get templates

Templates are built by the `Release Builds` workflow
(`.github/workflows/release_builds.yml`) and uploaded as **CI artifacts**:

| Job | Artifact | Contains |
| --- | --- | --- |
| `build_windows_export_template` | `godotgs-export-template-windows-<channel>-<run>` | `windows_release_x86_64.exe`, `windows_release_x86_64.console.exe` |
| `build_linux_export_template` | `godotgs-export-template-linux-<channel>-<run>` | `linux_release.x86_64` |

Both artifacts also carry a `.sha256` checksum and an `EXPORT-TEMPLATE-INFO.txt`
recording the channel, commit and build flags.

To build one yourself instead:

```bash
# Windows
scons platform=windows target=template_release arch=x86_64 gs_native_arch=no
# Linux
scons platform=linuxbsd target=template_release arch=x86_64 gs_native_arch=no
```

Do **not** add `dev_build=yes`. A `dev_build` template compiles at `-O0` and is
not shippable.

## Scope: release templates only, for now

Only `target=template_release` is built and published. `template_debug` is a
follow-up; until it lands:

- Leave `custom_template/debug` empty and export with the **release** preset.
- "Deploy with remote debug" / one-click debug runs from the editor are not
  covered by a published template.

## Where Godot expects the files

Godot resolves a template in one of two ways.

**1. Custom template path (recommended here).** Set the absolute path in the
preset. This is what the smoke test and the `splat_kabinett` reference preset
do:

```ini
[preset.0.options]

custom_template/debug=""
custom_template/release="C:/projects/godotgs-clean/bin/godot.windows.template_release.x86_64.exe"
```

On Windows, if `debug/export_console_wrapper` is enabled, the exporter also
looks for a `<template basename>.console.exe` next to the template
(`EditorExportPlatformWindows::modify_template`). Keep the two files together.

**2. Installed export templates directory.** If `custom_template/release` is
empty, Godot looks up a file **by name** in its export templates directory
(`%APPDATA%/Godot/export_templates/<version>/` on Windows,
`~/.local/share/godot/export_templates/<version>/` on Linux). Dropping the
artifacts there under the expected names also works — but it is easy to end up
with an upstream Godot template of the same version already installed, which is
exactly the silent failure above. Prefer the explicit custom path.

## The file names differ per platform

The expected names come from each platform's export plugin and are **not**
consistent with each other:

| Platform | Expected name | Source |
| --- | --- | --- |
| Windows | `windows_release_x86_64.exe` (+ `windows_release_x86_64.console.exe`) | `EditorExportPlatformWindows::get_template_file_name()` — `"windows_" + target + "_" + arch + ".exe"` |
| Linux | `linux_release.x86_64` | `EditorExportPlatformLinuxBSD::get_template_file_name()` — `"linux_" + target + "." + arch` |

Windows separates the architecture with `_`, Linux with `.`. That is upstream
Godot behaviour. Do not "normalise" it — renaming makes the template invisible
to the exporter.

## Verifying an export

`tests/runtime/run_export_smoke.py` exports
`tests/examples/godot/test_project` against a locally built template, runs the
result, and asserts that splats actually render in the exported binary:

```bash
python tests/runtime/run_export_smoke.py \
    --editor-binary bin/godot.windows.editor.x86_64.exe \
    --template bin/godot.windows.template_release.x86_64.exe
```

The run generates an `export_presets.cfg` in that project. If you already have
one there, the run **refuses to start** rather than overwriting a file that is
gitignored and would therefore vanish without showing up in `git status`. Pass
`--overwrite-preset` to let it move yours to `export_presets.cfg.smoke-backup`
and put it back afterwards.

It fails if the exported binary carries no `GaussianSplat` symbols, and it runs
the game with a real Vulkan rendering device rather than `--headless` — a
headless run has no `RenderingDevice`, which would make any "it rendered"
assertion vacuous.

The final assertion reads pixels back from the game window, so it needs an
interactive desktop session — the same requirement as the in-repo
`Canonical Node Asset Render` proof. In a session without a composited desktop
the window reads back blank; that is reported as its own status
(`failed_visual_evidence`, probe exit code 4) and can be downgraded with
`--allow-blank-viewport`. That flag downgrades **only** that one case: the
module check, the `RenderingDevice` check and the "a GPU raster pass actually
ran over the fixture" check all still have to pass.

## Related

- [Release channels](release-channels.md)
- [Build from source](../BUILDING.md)
- [Workflow overview](../../.github/workflows/README.md)
