# Downloads

**For:** anyone who wants a godotGS editor without compiling one.

**At the end:** you have a godotGS editor running on Windows or Linux, and you know whether it is the right build for what you want to do.

A stock Godot editor cannot render splats. You need an editor built from this fork, and there are two ways to get one:

| Route | Use it when | Where |
| --- | --- | --- |
| **Nightly editor** | You are on Windows or Linux and want to try godotGS today. | This page |
| **Build from source** | You are on macOS, you want representative speed on Linux, or you need a custom build. | [Build from Source](../BUILDING.md) |

There is no stable `v*` release yet: nightlies are the only published binaries. [Release Channels](../development/release-channels.md) explains the publishing model.

## Latest Nightly

[**Open the Releases page**](https://github.com/klausi3D/godotGS/releases) and pick the most recent `nightly-YYYYMMDD` entry at the top. Because there is no stable release yet, GitHub's "latest release" shortcut does not lead to a nightly, so always use the list.

Every nightly contains the Linux editor, `BUILD-INFO.txt`, and checksum files. The Windows editor and the Windows export template are attached **only when that night's Windows build succeeds**. If the Windows build fails, the nightly is Linux-only, and several in a row can be. If the Windows build succeeds but the export template build or its export smoke test fails, no nightly is published that night at all. On Windows, pick the most recent nightly whose asset list includes `godotgs-windows-x86_64-<tag>.zip`; it is not always the newest entry.

<div id="gs-latest-nightlies" data-repo="klausi3D/godotGS" hidden></div>

| Asset | Platform | Contents |
| --- | --- | --- |
| `godotgs-linux-x86_64-<tag>.tar.xz` | Linux x86_64 | **Editor**, unoptimized (see [Linux nightly speed](#linux-nightly-speed)) |
| `godotgs-windows-x86_64-<tag>.zip` | Windows x86_64 (only when the Windows build passed) | **Editor**, optimized: GUI editor (`.exe`) + console wrapper (`.console.exe`) |
| `godotgs-export-template-windows-x86_64-<tag>.zip` | Windows x86_64 (only when the Windows build passed) | **Export template** for shipping a game: `windows_release_x86_64.exe` + `windows_release_x86_64.console.exe`. Not an editor. See [Export Templates](../development/export-templates.md). |
| `*.sha256` | per archive | SHA-256 checksum for each archive in that nightly |
| `BUILD-INFO.txt` | shared | Channel, commit hash, binary names, generation timestamp |

macOS has no published binary: use [Build from Source](../BUILDING.md).

## Linux Nightly Speed

!!! warning "The Linux nightly is an unoptimized `-O0` build"
    The published Linux nightly editor is compiled with `dev_build=yes`, which means
    `-O0`: no optimization at all. That inflates CPU-side frame cost by roughly an
    order of magnitude compared with an optimized build. The `.dev` segment in its
    binary name is exactly this flag. **Do not judge godotGS performance from the
    Linux nightly, and do not benchmark it.** The Windows nightly editor is built
    without `dev_build`, with `optimize=speed_trace` (`/O2`), so its binary has no
    `.dev` segment. For representative speed on Linux, build from source with
    `target=editor optimize=speed_trace` (see
    [Build Flavors](../BUILDING.md#build-flavors)), and read the
    [Performance Dashboard](../performance/index.md#measurement-environment) for the
    numbers an optimized build actually produces.

## Run It

### Linux

```bash
tar -xJf godotgs-linux-x86_64-<tag>.tar.xz
chmod +x godot.linuxbsd.editor.dev.x86_64
./godot.linuxbsd.editor.dev.x86_64
```

### Windows

Unzip and pick the variant that fits how you want to run the editor:

- `godot.windows.editor.x86_64.exe`: the GUI editor with no console window. Use this for normal work.
- `godot.windows.editor.x86_64.console.exe`: the same editor with a console window for stdout/stderr. Use this when debugging or when a script needs the editor's output.

Both ship in the same zip; keep one or both.

No godotGS binary is code-signed, so Windows SmartScreen may warn that the app is unrecognized. Check the download against its `.sha256` file (below) before you run it.

### Exporting a game

To export a game on Windows, also download `godotgs-export-template-windows-x86_64-<tag>.zip`. Unzip it and set `custom_template/release` in your export preset to the absolute path of `windows_release_x86_64.exe`. Keep the `.console.exe` next to it. Without that setting the export silently uses a stock template that renders no splats. Then clear **Export With Debug** in the export file dialog; it is on by default. A debug export, like the editor's one-click remote-debug run, reads `custom_template/debug` instead, and no godotGS debug template is published yet. No Linux export template is published; build one from source. See [Export Templates](../development/export-templates.md).

## Verify the Download

Match the published checksum against your local file:

```bash
# Linux / WSL / git-bash
sha256sum godotgs-windows-x86_64-<tag>.zip
# compare to the contents of godotgs-windows-x86_64-<tag>.sha256
```

```powershell
# Windows PowerShell
Get-FileHash -Algorithm SHA256 .\godotgs-windows-x86_64-<tag>.zip
```

## Stability Expectations

Nightlies are prereleases: any one of them may break. They are for evaluation, prototypes and contributor work, not production. See the [stability column in Release Channels](../development/release-channels.md#channels) for what each channel promises.

## When to Build From Source Instead

Use [Build from Source](../BUILDING.md) when:

- you are on macOS,
- you are on Linux and want representative performance (see [Linux nightly speed](#linux-nightly-speed)),
- you need a custom build flavor (release-stripped, different optimizer settings, debug symbols),
- or you want to reproduce a specific commit's binary.

[Build Your Own Editor](installation.md) lists the prerequisites.

## Next Step

You have an editor. Continue with [Your First Splat](quick-start.md) to open the sample project and see a splat.
