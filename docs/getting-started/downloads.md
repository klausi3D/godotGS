# Downloads

Download the godotGS editor for your platform, extract the archive, and open the executable. You do not need to compile the engine on Windows or Linux. This fork's editor includes Gaussian Splatting; a stock Godot editor cannot render splats.

## Latest Nightly

These are **nightly prereleases for evaluation and prototypes**. No stable `v*` release is published yet. The buttons below download the editor archive directly.

<div id="gs-latest-nightlies" data-repo="klausi3D/godotGS" role="region" aria-label="Editor downloads" hidden></div>

**macOS:** no prebuilt editor is published. Use [Build from Source](../BUILDING.md).

??? note "Manual downloads — if the buttons do not load"
    [Open GitHub Releases](https://github.com/klausi3D/godotGS/releases), expand **Assets**, and choose the newest release containing your platform's **editor** archive:

    - **Windows:** `godotgs-windows-x86_64-<tag>.zip`.
    - **Linux:** `godotgs-linux-x86_64-<tag>.tar.xz` — unoptimized; see [Linux nightly speed](#linux-nightly-speed).

    The newest nightly can be Linux-only. On Windows, check earlier releases until you find the Windows editor ZIP. The export template ZIP is for shipping games; GitHub's **Source code** archives contain no editor executable.

    Download the matching `.sha256` file if you want to [verify the archive](#verify-the-download). `BUILD-INFO.txt` records build metadata. Use the release list rather than GitHub's latest-release shortcut, which excludes nightlies. [Release Channels](../development/release-channels.md) explains publication.

## Run It

### Windows

1. Download the **Windows editor ZIP** above.
2. Right-click the ZIP and choose **Extract All…**. Open the extracted folder.
3. Double-click **`godot.windows.editor.x86_64.exe`**. The Project Manager opens; continue with [Your First Splat](quick-start.md).

Run the executable from the extracted folder, rather than from inside the ZIP. No installer or export template is needed to open the editor.

No godotGS binary is code-signed, so Windows SmartScreen may warn that the app is unrecognized. [Check the archive's SHA-256 checksum](#verify-the-download) before running it. A matching checksum checks the downloaded file against the published archive; it does not provide code signing.

For terminal output, the console wrapper is `godot.windows.editor.x86_64.console.exe`, when available. Current nightly packaging omits it ([#829](https://github.com/klausi3D/godotGS/issues/829)); start the GUI executable above.

### Linux

1. Download the **Linux editor TAR.XZ** above.
2. Extract it with your archive manager, or run the commands below in the download folder. Replace `<tag>` with the tag shown on the download card.
3. Open **`godot.linuxbsd.editor.dev.x86_64`** from the extracted folder. Continue with [Your First Splat](quick-start.md).

```bash
tar -xJf godotgs-linux-x86_64-<tag>.tar.xz
chmod +x godot.linuxbsd.editor.dev.x86_64
./godot.linuxbsd.editor.dev.x86_64
```

You need a working Vulkan driver; see the [Compatibility Matrix](../reference/compatibility-matrix.md). This nightly is unoptimized and unsuitable for performance measurements; see [Linux nightly speed](#linux-nightly-speed).

### Exporting a game

You only need an export template when shipping a game, after you have the editor running.

<div id="gs-export-template-download" hidden></div>

To export a game on Windows, also download `godotgs-export-template-windows-x86_64-<tag>.zip`. Unzip it and set `custom_template/release` in your export preset to the absolute path of `windows_release_x86_64.exe`. Keep the `.console.exe` next to it. Without that setting the export silently uses a stock template that renders no splats. Then clear **Export With Debug** in the export file dialog; it is on by default. A debug export, like the editor's one-click remote-debug run, reads `custom_template/debug` instead, and no godotGS debug template is published yet. No Linux export template is published; build one from source. See [Export Templates](../development/export-templates.md).

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

## Verify the Download

Match the published checksum against your local file:

```bash
# Linux: hash the archive you downloaded
sha256sum godotgs-linux-x86_64-<tag>.tar.xz
# compare to the contents of godotgs-linux-x86_64-<tag>.sha256
```

```powershell
# Windows PowerShell
Get-FileHash -Algorithm SHA256 .\godotgs-windows-x86_64-<tag>.zip
# compare to the contents of godotgs-windows-x86_64-<tag>.sha256
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
