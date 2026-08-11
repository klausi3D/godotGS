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

If a run is interrupted, a `export_presets.cfg.smoke-backup` may be left behind.
The next run **refuses to start** in that case, with or without
`--overwrite-preset`: one of the two files is your original and the tool cannot
tell which. Inspect them, keep the one you want as `export_presets.cfg`, delete
the backup, and re-run.

It fails fast if the exported binary carries no `GaussianSplat` symbols. Treat
that scan as a **pre-check only**: their absence rules a stock template out,
but their presence proves nothing on its own — any file containing those
literals would pass it. What actually proves the module is present and working
is the next step, inside the exported binary: the probe resolves the GS types
through `ClassDB` and then requires a live `RenderingDevice`, a renderer,
visible splats and a successful GPU raster pass. The game is therefore run with
a real Vulkan rendering device rather than `--headless` — a headless run has no
`RenderingDevice`, which would make any "it rendered" assertion vacuous.

The final assertion reads pixels back from the game window, so it needs an
interactive desktop session — the same requirement as the in-repo
`Canonical Node Asset Render` proof. In a session without a composited desktop
the window reads back blank; that is reported as its own status
(`failed_visual_evidence`, probe exit code 4) and can be downgraded with
`--allow-blank-viewport`. That flag downgrades **only** that one case: the
module check, the `RenderingDevice` check and the "a GPU raster pass actually
ran over the fixture" check all still have to pass.

### In CI

The `export_smoke_windows` job in `release_builds.yml` runs this against the
Windows template built by the same run, on the self-hosted GPU runner, on
`push`/tag/schedule/dispatch. It downloads the two published archives rather
than reading a local `bin/`, so what it exercises is the bytes a user gets.

Two flags matter there and are worth repeating, because getting either wrong
turns the lane into a check that cannot fail:

- **`--require-binaries` is always passed.** Without it `_skip()` returns exit 0,
  so a missing binary, a failed artifact download or a non-Windows host reports
  the lane green. That is the "skip encoded as pass" shape this lane exists to
  retire.
- **`--allow-blank-viewport` is never passed.** The GPU pool reads back real
  pixels. If it stops, that is a finding to file, not a flag to add.

The job then runs the same script a second time with
`--expect-stock-template-failure`. That is the negative control: it writes the
preset with an **empty** `custom_template/release` and requires the run *not* to
end in a working GS export. It reports one of three outcomes on its
`[EXPORT_SMOKE_METRICS]` line:

| Outcome | Meaning | Verdict |
| --- | --- | --- |
| `export_rejected` | The editor refused to export; there was no stock template to fall back to. | pass |
| `stock_template_detected` | The export succeeded and produced a binary with no GS symbols. A stock upstream template is installed on that machine — this is #825 reproduced, and only the byte-scan caught it. | pass, but report it |
| `undetected` | A GS-enabled binary came out of a preset that named no template. Then the positive run is not discriminating. | fail |

**What CI does not check:** whether the exported game *looks right*. The probe
asserts visible splats, four pipeline stage statuses and a count of
non-background pixel samples; it does not compare against a reference image.
That still needs one human look — run the script with `--keep`, launch the
exported binary, and look at it.

## Related

- [Release channels](release-channels.md)
- [Build from source](../BUILDING.md)
- [Workflow overview](../../.github/workflows/README.md)
