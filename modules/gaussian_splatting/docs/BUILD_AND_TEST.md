# Gaussian Splatting Build and Test Guide

This module supports the same desktop targets declared in `modules/gaussian_splatting/config.py`:

- `windows`
- `linuxbsd`
- `macos`

## Build (SCons)

Run from repository root (`/mnt/c/projects/godotgs` in this workspace):

### Windows

```powershell
scons platform=windows target=editor dev_build=yes modules/gaussian_splatting
```

### Linux

```bash
scons platform=linuxbsd target=editor dev_build=yes modules/gaussian_splatting
```

### macOS

```bash
scons platform=macos target=editor dev_build=yes modules/gaussian_splatting
```

## Validation Guards (Fast, Cross-Platform)

These checks do not require a full editor build and should be run before opening a PR:

```bash
python3 modules/gaussian_splatting/tests/check_build_metadata_consistency.py
python3 tests/ci/run_module_tests.py --guard-only
```

The metadata guard enforces:

- SCsub source directories stay aligned with CMake IDE metadata.
- CMake does not reference missing source directories.
- `config.py` doc contracts map to real XML files in `doc_classes/`.

## Module Test Runner

`tests/ci/run_module_tests.py` is the canonical module test/guard entry point used by CI.

### Auto-detect `godot` in PATH

```bash
python3 tests/ci/run_module_tests.py
```

### Explicit binary path

```bash
python3 tests/ci/run_module_tests.py --godot-binary /path/to/godot-editor
```

On Windows, the explicit path is typically `bin\\godot.windows.editor.dev.x86_64.exe` (the `.dev` segment is added by `dev_build=yes`).

## GPU Test Harness (`--gs-gpu-test`)

In addition to the classic `--test` lane, the module ships a second doctest entrypoint specifically for `[RequiresGPU]` tests. It bootstraps `RenderingDevice` offscreen via `Main::test_setup()` + `RenderingContextDriverVulkan` / `RenderingContextDriverD3D12` (no `SceneTree`), so it isolates GPU-tagged regressions from scene-tree-dependent fixtures.

- Entrypoint: `<godot-binary> --gs-gpu-test [--test-case=<filter>] [--gs-gpu-driver=vulkan|d3d12]`
- Runner source: `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp` (`gs_gpu_test_main`, dispatched from `main/main.cpp:947`).
- Default filter when `--test-case` is omitted: `--test-case=*[RequiresGPU]* --test-case-exclude=*[SceneTree]*,*[Importer]*`. The `[SceneTree][RequiresGPU]` exclusion is tracked as Issue #329.
- `REQUIRE_GPU_DEVICE()` (`modules/gaussian_splatting/tests/test_macros.h`) owns its fallback `RenderingDevice` via `ScopedFallbackRD`; tests do not need to teardown manually.
- RID leak listener prints `[GS-GPU][RID-LEAK?] bytes=N test=advisory` when a test leaves more than 4 MiB resident (raised from 1 MiB in #341). The Python supervisor folds any non-zero advisory into a gate failure.

### Supervisor

`tests/ci/run_gpu_harness.py` drives `--gs-gpu-test` in **per-batch subprocesses** so a driver hang or OOM in one batch cannot corrupt the next. The batch catalogue and the `REQUIRED_BATCHES` set live in that file's `BATCHES`/`REQUIRED_BATCHES` definitions — ask the source (`python tests/ci/run_gpu_harness.py --list-batches`) or see the per-batch table in [`tests/README.md`](../tests/README.md#gpu-test-harness) rather than a list transcribed here; an earlier version of this paragraph hand-listed the batches and had drifted badly by the time T4 (#909) deleted the structurally-empty `ComputeInfrastructure` batch it still advertised. An empty filter on a required batch fails the gate (asserted at import), and a `--batch` name matching no catalogued batch is rejected by the supervisor.

```powershell
python tests\ci\run_gpu_harness.py --batch CompositorHazard --godot bin\godot.windows.editor.dev.x86_64.console.exe
```

```bash
python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot ./bin/godot.linuxbsd.editor.dev.x86_64
```

The supervisor enforces CI strict mode when `GITHUB_ACTIONS=true` or `CI=true` (passing `--godot` is required; `bin/` discovery is refused), writes a JSON report atomically via `tempfile.mkstemp` + `os.replace`, and streams subprocess output with one thread per pipe (the older `selectors`-based path would fail on Windows pipes with WSAStartup).

The canonical regression is `tests/test_output_compositor_composite_hazard.h` — a 256x256 compositor write-hazard repro from incident #256 with ~20 assertions running in <1 s.

### CI Integration

The `.github/workflows/baseline_qa.yml` workflow runs a new `gpu-harness` job after `gpu-tests` on the self-hosted Windows runner. Both jobs are gated against fork-PR code via `head.repo.full_name == github.repository`. The path filter is rendering-aware and covers `modules/gaussian_splatting/`, `servers/rendering/`, `drivers/vulkan/`, `drivers/d3d12/`, `core/`, `platform/`, `thirdparty/doctest/`, `SConstruct`, `methods.py`, the visual baselines, and the workflow itself.

Visual baselines live in `tests/visual_baselines/`. Compare-mode is the default for PRs/pushes. The recapture-PR flow (nightly schedule or `workflow_dispatch baseline_mode=update`) writes regenerated baselines plus a `.provenance.json` audit via the SHA-pinned `peter-evans/create-pull-request@c5a7806660adbe173f04e3e038b0ccdcd758773c`; update-mode requires `BASELINE_UPDATE_PAT`.

## Convenience Wrappers

- `run_tests.bat` (repository root) remains a Windows convenience wrapper for the `--test` lane.
- `tests/ci/run_module_tests.py` remains the cross-platform canonical command for the `--test` lane.
- `tests/ci/run_gpu_harness.py` is the canonical entry for the `--gs-gpu-test` lane (see above).
