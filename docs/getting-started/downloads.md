# Downloads

Public binaries for godotGS are published as nightly prereleases on GitHub. There is no stable `v*` release yet — see [Release Channels](../development/release-channels.md) for the full publishing model.

## Latest Nightly

[**Open the Releases page**](https://github.com/klausi3D/godotGS/releases) and pick the most recent `nightly-YYYYMMDD` entry at the top. (There is no stable `v*` release yet, so GitHub's "latest release" shortcut does not resolve to a nightly; always use the list.)

Each nightly contains the editor for both supported platforms plus integrity files:

| Asset | Platform | Contents |
| --- | --- | --- |
| `godotgs-linux-x86_64-<tag>.tar.xz` | Linux x86_64 | Editor binary |
| `godotgs-windows-x86_64-<tag>.zip` | Windows x86_64 | GUI editor (`.exe`) + console wrapper (`.console.exe`) |
| `*.sha256` | both | SHA-256 checksum sidecars |
| `BUILD-INFO.txt` | shared | Channel, commit hash, binary names, generation timestamp |

macOS is not yet covered by a published binary — [Build from Source](../BUILDING.md).

!!! warning "Nightly binaries are unoptimized `-O0` builds"
    Every published nightly is compiled with `dev_build=yes`, which means `-O0` — no
    optimization at all. That inflates CPU-side frame cost by roughly an order of
    magnitude compared with an optimized build. The `.dev` segment in the binary
    names below is exactly this flag. **Do not judge godotGS performance from a
    nightly download, and do not benchmark one.** For representative speed, build
    from source with `target=editor optimize=speed_trace` — see
    [Build Flavors](../BUILDING.md#build-flavors) — and read the
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

- `godot.windows.editor.dev.x86_64.exe` — GUI editor with no console window. Use this for the normal editor experience.
- `godot.windows.editor.dev.x86_64.console.exe` — same editor with a console window attached for stdout/stderr. Use this when debugging or when a script needs to capture editor output.

Both binaries ship in the same zip; you can keep just one or both side-by-side.

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

They are also **not performance-representative**: every nightly is a `dev_build=yes`
/ `-O0` binary, so anything you measure on one is an artifact of the build flavor
rather than of godotGS. Build an optimized editor ([Build Flavors](../BUILDING.md#build-flavors))
before drawing any conclusion about speed.

## Building From Source

Use [Build from Source](../BUILDING.md) when:

- you are on macOS,
- you want representative performance rather than an `-O0` nightly (see [Build Flavors](../BUILDING.md#build-flavors)),
- you need a custom build flavor (release-stripped, different optimizer settings, debug symbols, etc.),
- or you want to reproduce a specific commit's binary.
