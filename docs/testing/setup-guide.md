# Testing Setup Guide

## Purpose

Run Gaussian Splatting test lanes through maintained runners in `tests/ci/` and `tests/runtime/`.

## Usage

| Task | Command | Reference |
| --- | --- | --- |
| Build a test-enabled Godot binary | `scons platform=<platform> target=editor dev_build=yes tests=yes -j<jobs>` | [Repository README](../../README.md), [root build notes](../../BUILDING.md), `modules/gaussian_splatting/SCsub:69` |
| Set default Godot binary for Python runners | `export GODOT_BINARY=/path/to/godot` | `tests/ci/run_baseline_qa.py:763`, `tests/ci/run_module_tests.py:350`, `tests/runtime/run_runtime_validation.py:98` |
| Run quick baseline subset | `python3 tests/ci/run_baseline_qa.py --quick` | `tests/ci/run_baseline_qa.py:725`, `tests/ci/run_baseline_qa.py:284` |
| Run one baseline category | `python3 tests/ci/run_baseline_qa.py --category <ply|pipeline|sorting|runtime|module|qa>` | `tests/ci/run_baseline_qa.py:730`, `tests/ci/run_baseline_qa.py:281` |
| Run unified benchmark scene (report-only) | `godot --path tests/examples/godot/test_project --scene res://scenes/benchmark_unified.tscn --benchmark-headless-summary` | `tests/examples/godot/test_project/scenes/benchmark_unified.gd` |
| Run small baseline benchmark scene (high-FPS reference) | `godot --path tests/examples/godot/test_project --scene res://scenes/benchmark_small_baseline.tscn --benchmark-headless-summary` | `tests/examples/godot/test_project/scenes/benchmark_small_baseline.gd` |
| Run benchmark lane suite (profile runner) | `python3 tests/runtime/run_benchmark.py --profile quick --generate-dummy-assets` | `tests/runtime/run_benchmark.py`, `tests/examples/godot/test_project/scenes/benchmark_suite/` |
| Compare QA output against stored baseline (strict) | `python3 tests/ci/run_baseline_qa.py --category qa --qa-baseline tests/ci/baselines/qa_results.json --require-qa-baseline` | `tests/ci/run_baseline_qa.py:730`, `tests/ci/run_baseline_qa.py:735`, `tests/ci/run_baseline_qa.py:745`, `tests/ci/run_baseline_qa.py:796` |
| Refresh QA baseline snapshot from current run | `python3 tests/ci/run_baseline_qa.py --category qa --update-qa-baseline` | `tests/ci/run_baseline_qa.py:740`, `tests/ci/run_baseline_qa.py:456` |
| Run renderer/static guards without Godot tests | `python3 tests/ci/run_module_tests.py --guard-only` | `tests/ci/run_module_tests.py:354`, `tests/ci/run_module_tests.py:439` |
| Run module doctests after guards | `python3 tests/ci/run_module_tests.py --godot-binary "$GODOT_BINARY"` | `tests/ci/run_module_tests.py:350`, `tests/ci/run_module_tests.py:454` |
| Run runtime validation harnesses | `python3 tests/runtime/run_runtime_validation.py --godot-binary "$GODOT_BINARY" --gd-mode headless` | `tests/runtime/run_runtime_validation.py:98`, `tests/runtime/run_runtime_validation.py:103`, `tests/runtime/run_runtime_validation.py:957` |

| Baseline category | Executed target | Reference |
| --- | --- | --- |
| `ply` | `tests/ci/test_ply_loader_ci.gd` | `tests/ci/run_baseline_qa.py:233` |
| `pipeline` | `tests/ci/test_ply_pipeline_ci.gd` | `tests/ci/run_baseline_qa.py:239` |
| `sorting` | `tests/ci/test_gpu_sorting_ci.gd` | `tests/ci/run_baseline_qa.py:245` |
| `runtime` | `python3 tests/runtime/run_runtime_validation.py` | `tests/ci/run_baseline_qa.py:251` |
| `module` | `python3 tests/ci/run_module_tests.py` | `tests/ci/run_baseline_qa.py:258` |
| `qa` | `godot --headless --path tests/examples/godot/test_project --script res://scripts/qa_test_runner.gd --qa-output tests/ci/qa_results.json` | `tests/ci/run_baseline_qa.py:269`, `tests/ci/run_baseline_qa.py:271`, `tests/ci/run_baseline_qa.py:273` |

## API

| Entry point | Key options | Behavior | Artifacts | Reference |
| --- | --- | --- | --- | --- |
| `tests/ci/run_baseline_qa.py` | `--godot`, `--quick`, `--category`, `--qa-baseline`, `--update-qa-baseline`, `--require-qa-baseline`, `--baseline-report`, `--baseline-summary` | Orchestrates baseline categories and QA baseline compare/update flow. | `baseline_qa_results.json`, `tests/ci/qa_results.json`, `baseline_qa_regression_report.json`, `baseline_qa_regression_summary.md` | `tests/ci/run_baseline_qa.py:723`, `tests/ci/run_baseline_qa.py:735`, `tests/ci/run_baseline_qa.py:750` |
| `tests/ci/run_module_tests.py` | `--godot-binary`, `--base-ref`, `--guard-only`, `--skip-render-guards`, `--skip-static-guards`, `--tests-unavailable-mode`, `--allow-tests-unavailable` | Runs renderer guards and module doctests filtered by `*GaussianSplatting*`. | Console output | `tests/ci/run_module_tests.py:350`, `tests/ci/run_module_tests.py:354`, `tests/ci/run_module_tests.py:453` |
| `tests/runtime/run_runtime_validation.py` | `--godot-binary`, `--gd-mode`, `--skip-cpp`, `--skip-gd`, `--fail-on-skip`, `--allow-skips` | Runs C++ runtime harnesses and GDScript runtime harnesses with skip/fail policy. | `tests/runtime/runtime_validation_report.json` | `tests/runtime/run_runtime_validation.py:95`, `tests/runtime/run_runtime_validation.py:98`, `tests/runtime/run_runtime_validation.py:970` |
| Scripted entry point | Command surface | Reference |
| --- | --- | --- |
| `Makefile` | `make test` | `Makefile:29` |
| `package.json` | `npm run test` | `package.json:6` |
| `run_tests.bat` | Windows batch full module run | `run_tests.bat:24`, `run_tests.bat:31` |
| `ci/scripts/run_module_tests.bat` | Windows quick guard + module run | `ci/scripts/run_module_tests.bat:23`, `ci/scripts/run_module_tests.bat:47` |

## GPU Test Harness (gs-gpu-test)

The `--gs-gpu-test` entrypoint is a second doctest runner specifically for tests tagged `[RequiresGPU]`. It bootstraps `RenderingDevice` offscreen via `Main::test_setup()` + `RenderingContextDriverVulkan` / `RenderingContextDriverD3D12` (no `SceneTree`, no window) and is therefore distinct from the `--test` lane driven by `tests/ci/run_module_tests.py`.

| Task | Command | Reference |
| --- | --- | --- |
| Run the canonical compositor hazard regression | `python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot ./bin/godot.linuxbsd.editor.dev.x86_64` | `tests/ci/run_gpu_harness.py`, `modules/gaussian_splatting/tests/test_output_compositor_composite_hazard.h` |
| Run every catalogued batch | `python3 tests/ci/run_gpu_harness.py --godot ./bin/godot.linuxbsd.editor.dev.x86_64` | `tests/ci/run_gpu_harness.py` |
| Direct doctest invocation with a custom filter | `./bin/godot.linuxbsd.editor.dev.x86_64 --gs-gpu-test --test-case="*HazardRepro*"` | `main/main.cpp:947`, `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp` |
| Force a specific GPU driver | `./bin/godot.linuxbsd.editor.dev.x86_64 --gs-gpu-test --gs-gpu-driver=d3d12` | `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp` |

Windows examples:

```powershell
python tests\ci\run_gpu_harness.py --batch CompositorHazard --godot bin\godot.windows.editor.dev.x86_64.console.exe
bin\godot.windows.editor.dev.x86_64.console.exe --gs-gpu-test --test-case="*HazardRepro*"
```

Key contracts:

- Default filter when `--test-case` is omitted: `--test-case=*[RequiresGPU]* --test-case-exclude=*[SceneTree]*,*[Importer]*`. The `[SceneTree][RequiresGPU]` exclusion is tracked as Issue #329.
- Active batches: `CompositorHazard`, `OutputCompositor`, `TileRenderer`, `Integration`, `RendererPipeline`, `Lifetime`, and the `[SceneTree]` batches (`NodeSceneTree`, `WorldSceneTree`, `SceneDirectorSceneTree`, `RendererSceneTree`) — the `[SceneTree]` corpus (#329) now runs in these dedicated batches rather than being excluded. `GpuSorting` carries active coverage (its `*Sort*][RequiresGPU]*` filter matches the #508 `[GPUSortPipeline][RequiresGPU]` sticky-overflow test) but is currently advisory — promotion tracked in #744. Still genuinely catalogued-but-empty: `ComputeInfrastructure`, `MemoryStream`, `Streaming`.
- `REQUIRED_BATCHES = {"CompositorHazard", "RendererPipeline", "Lifetime", "OutputCompositor", "RendererSceneTree", "WorldSceneTree", "SceneDirectorSceneTree"}` (the last four promoted in #724) — asserted at import; an empty filter on a required batch fails the gate, and any hollow (zero-assertion) case in a required batch fails it per-case (#696). `NodeSceneTree` stays advisory (flaky under runner contention, #630).
- CI strict mode (`GITHUB_ACTIONS=true` or `CI=true`): `--godot` is required and `bin/` fallback is refused.
- The supervisor writes a JSON report atomically (`tempfile.mkstemp` + `os.replace`) and streams subprocess output with one thread per pipe (Windows-compatible).
- The doctest listener prints `[GS-GPU][RID-LEAK?] bytes=N test=advisory` above 4 MiB residual (raised from 1 MiB in #341); the supervisor folds non-zero values into a gate failure.

See `modules/gaussian_splatting/tests/README.md#gpu-test-harness` for the per-batch filter table and full listener semantics. The `.github/workflows/baseline_qa.yml` workflow's `gpu-harness` job runs after `gpu-tests` on the self-hosted Windows runner; both jobs are gated against fork-PR code via `head.repo.full_name == github.repository`.

## Examples

```bash
python3 tests/ci/run_baseline_qa.py --help
python3 tests/ci/run_module_tests.py --help
python3 tests/runtime/run_runtime_validation.py --help
python3 tests/ci/run_module_tests.py --guard-only
python3 tests/ci/run_gpu_harness.py --help
python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot "$GODOT_BINARY"
```

## Troubleshooting

| Symptom | Cause | Action | Reference |
| --- | --- | --- | --- |
| `Could not find Godot binary` | `GODOT_BINARY` is unset and no `godot` binary is on `PATH`. | Set `GODOT_BINARY` or pass `--godot`/`--godot-binary`. | `tests/ci/run_baseline_qa.py:776` |
| `--quick` is ignored | `--category` was also passed. | Run only one selector mode at a time. | `tests/ci/run_baseline_qa.py:781` |
| Module tests report disabled test runner | Binary was not built with `tests=yes`. | Rebuild the editor from repository root with `tests=yes` so the output in `bin/` includes the in-tree module and test runner. | `tests/ci/run_module_tests.py:459`, `modules/gaussian_splatting/SCsub:69` |
| Need to run only `module` or `qa` checks | Those categories are exposed only through the maintained baseline runner. | Use `tests/ci/run_baseline_qa.py --category module` or `--category qa`. | `tests/ci/run_baseline_qa.py:730`, `tests/ci/run_baseline_qa.py:281` |
| Runtime run fails on skips in non-headless mode | Default skip policy is strict outside headless mode. | Add `--allow-skips` or switch to `--gd-mode headless`. | `tests/runtime/run_runtime_validation.py:916`, `tests/runtime/run_runtime_validation.py:920` |
| `run_gpu_harness.py` refuses to discover a binary | `GITHUB_ACTIONS=true` or `CI=true` is set; CI strict mode requires explicit `--godot`. | Pass `--godot <path>` explicitly when running under CI envs. | `tests/ci/run_gpu_harness.py` |
| GPU harness gate fails on an apparently-empty batch | The batch is in `REQUIRED_BATCHES` and its filter matched no tests. | Either add tests for that batch or remove it from `REQUIRED_BATCHES`. | `tests/ci/run_gpu_harness.py` |
| `FATAL: batch '<name>' TIMED OUT` / batch `rc=124`, `status=TIMEOUT`, `cases=0/0` | The batch exceeded its wall-clock budget, so doctest never printed a summary: its case counts parse as `0` and its leak bytes are partial. This is an absence of measurement, not a clean batch — `totals_authoritative` in the report goes `false` and the run totals exclude whatever it never reached. | First establish budget vs hang by re-running that batch alone with a large `--timeout`. If it completes, raise that batch's `BatchSpec.timeout_seconds` (this is why `NodeSceneTree` sets `180`; it measures ~82 s and was being cut off at 9 of 18 cases). If it never completes, it is a hang — diagnose it, do not budget around it. | `tests/ci/run_gpu_harness.py` |
| `[GS-GPU][RID-LEAK?] bytes=N test=advisory` fails the gate | A test left more than 4 MiB resident; the supervisor folds non-zero advisories into a failure. | Audit the offending test's RID lifetimes; ensure `REQUIRE_GPU_DEVICE()` cleanup runs. | `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp`, `modules/gaussian_splatting/tests/test_macros.h` |
