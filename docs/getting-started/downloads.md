# Downloads

Public binaries for godotGS are published as nightly prereleases on GitHub. There is no stable `v*` release yet — see [Release Channels](../development/release-channels.md) for the full publishing model.

## Latest Nightly

[**Open the Releases page**](https://github.com/klausi3D/godotGS/releases) and pick the most recent `nightly-YYYYMMDD` entry at the top. (There is no stable `v*` release yet, so GitHub's "latest release" shortcut does not resolve to a nightly; always use the list.)

Each nightly contains the editor for both supported platforms, the Windows export template, and integrity files. The Windows assets are attached only when the Windows build and its export smoke test succeeded for that run:

| Asset | Platform | Contents |
| --- | --- | --- |
| `godotgs-linux-x86_64-<tag>.tar.xz` | Linux x86_64 | Editor binary (`dev_build=yes`, `-O0`) |
| `godotgs-windows-x86_64-<tag>.zip` | Windows x86_64 | **Editor**, optimized: GUI editor (`.exe`) + console wrapper (`.console.exe`) |
| `godotgs-export-template-windows-x86_64-<tag>.zip` | Windows x86_64 | **Export template** for shipping a game: `windows_release_x86_64.exe` + `windows_release_x86_64.console.exe`. Not an editor. See [Export Templates](../development/export-templates.md). |
| `*.sha256` | both | SHA-256 checksum sidecars |
| `BUILD-INFO.txt` | shared | Channel, commit hash, binary names, generation timestamp |

macOS is not yet covered by a published binary — [Build from Source](../BUILDING.md).

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

- `godot.windows.editor.x86_64.exe` — GUI editor with no console window. Use this for the normal editor experience.
- `godot.windows.editor.x86_64.console.exe` — same editor with a console window attached for stdout/stderr. Use this when debugging or when a script needs to capture editor output.

Both binaries ship in the same zip; you can keep just one or both side-by-side.

To export a game, also download `godotgs-export-template-windows-x86_64-<tag>.zip`. Unzip it and set `custom_template/release` in your export preset to the absolute path of `windows_release_x86_64.exe`. Keep the `.console.exe` next to it. Without that setting the export silently uses a stock template that renders no splats. See [Export Templates](../development/export-templates.md).

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

Nightlies are prereleases by design — they may break at any time. They are intended for evaluation, prototypes, and contributor work, not production. See the [stability column in Release Channels](../development/release-channels.md#channels) for the per-channel guarantees.

The Linux nightly is also **not performance-representative**: it is a `dev_build=yes`
/ `-O0` binary, so anything you measure on it is an artifact of the build flavor
rather than of godotGS. Use the optimized Windows nightly, or build an optimized
editor ([Build Flavors](../BUILDING.md#build-flavors)), before drawing any conclusion
about speed.

## Building From Source

Use [Build from Source](../BUILDING.md) when:

- you are on macOS,
- you are on Linux and want representative performance rather than the `-O0` Linux nightly (see [Build Flavors](../BUILDING.md#build-flavors)),
- you need a custom build flavor (release-stripped, different optimizer settings, debug symbols, etc.),
- or you want to reproduce a specific commit's binary.
