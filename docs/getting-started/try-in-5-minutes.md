# Try in 5 Minutes

This is the shortest honest path to a visible result: download the latest nightly editor for your platform, open the sample project, and confirm the public evaluator in the viewport. macOS still starts with [Build from Source](../BUILDING.md).

## 1. Get the Nightly Editor

Open the [GitHub Releases](https://github.com/klausi3D/godotGS/releases) page, pick the most recent `nightly-YYYYMMDD` entry at the top of the list, and download the archive that matches your platform:

- **Linux:** `godotgs-linux-x86_64-<date>.tar.xz`
- **Windows:** `godotgs-windows-x86_64-<date>.zip` (contains both the GUI editor and the console wrapper — pick whichever fits your workflow)
- **macOS:** no published binary; stop here and use [Build from Source](../BUILDING.md).

See the [Downloads page](downloads.md) for verification and integrity-check details.

!!! warning "This editor is an unoptimized `-O0` build"
    Nightlies are compiled with `dev_build=yes`, i.e. `-O0`, which inflates CPU-side
    frame cost by roughly an order of magnitude. This page is the fastest way to see
    godotGS work; it is **not** a way to see how fast it is. For that, build with
    `optimize=speed_trace` ([Build Flavors](../BUILDING.md#build-flavors)) and read
    the [Performance Dashboard](../performance/index.md#measurement-environment).

## 2. Get the Sample Project

The sample project lives in this repository, not in the download. A shallow clone is
enough and is far quicker than the full engine history:

```bash
git clone --depth 1 https://github.com/klausi3D/godotGS.git
cd godotGS
```

## 3. Open the Project

Point `GODOT_BINARY` at the editor you downloaded, then open the sample project
from the clone (paths below are relative to the repository root):

```bash
export GODOT_BINARY=/absolute/path/to/godot.linuxbsd.editor.dev.x86_64
$GODOT_BINARY --path tests/examples/godot/test_project
```

```powershell
$env:GODOT_BINARY="C:\absolute\path\to\godot.windows.editor.dev.x86_64.exe"
& $env:GODOT_BINARY --path .\tests\examples\godot\test_project
```

## 4. Verify the Public Evaluator

Press Play. The sample project opens `res://scenes/public_evaluator.tscn` by default.

You should see:

- a visible splat in the viewport
- the sample project remains open and interactive

## If It Fails

- Read [Public Evaluator](quick-start.md) for the slower canonical flow.
- Check [Recurring Issues](../troubleshooting/recurring-issues.md).
- On macOS, stop using this page and build an editor from this fork with [Build from Source](../BUILDING.md) before retrying.
