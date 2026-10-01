# Testing Setup Guide

## Purpose

Run Gaussian Splatting test lanes through maintained runners in `tests/ci/` and `tests/runtime/`.

References on this page name a file and a symbol (function, constant or block), not a line number.
Run any runner with `--help` for its current option list.

## Usage

| Task | Command | Reference |
| --- | --- | --- |
| Build a test-enabled Godot binary | `scons platform=<platform> target=editor dev_build=yes tests=yes -j<jobs>` | [Repository README](../../README.md), [root build notes](../../BUILDING.md), `modules/gaussian_splatting/SCsub` (the `if env.get("tests", False)` block) |
| Set default Godot binary for Python runners | `export GODOT_BINARY=/path/to/godot` | `tests/ci/run_baseline_qa.py` `main()`, and the `--godot-binary` defaults in `tests/ci/run_module_tests.py` and `tests/runtime/run_runtime_validation.py`. `tests/ci/run_gpu_harness.py` does not read `GODOT_BINARY`; pass `--godot`. |
| Run quick baseline subset (`ply` + `sorting`) | `python3 tests/ci/run_baseline_qa.py --quick` | `BaselineQARunner.run_all_tests()` in `tests/ci/run_baseline_qa.py` |
| Run one baseline category | `python3 tests/ci/run_baseline_qa.py --category <all|ply|pipeline|sorting|runtime|module|qa|renderer>` | `TEST_CATEGORIES`, `CATEGORY_ALIASES` and `normalize_test_category()` in `tests/ci/run_baseline_qa.py` |
| Run several baseline categories | `python3 tests/ci/run_baseline_qa.py --categories ply,pipeline,runtime,module` | `BaselineQARunner.run_all_tests()`: a requested category that selects no tests fails the run |
| Run unified benchmark scene (report-only) | `godot --path tests/examples/godot/test_project --scene res://scenes/benchmark_unified.tscn --benchmark-headless-summary` | `tests/examples/godot/test_project/scenes/benchmark_unified.gd` |
| Run small baseline benchmark scene (high-FPS reference) | `godot --path tests/examples/godot/test_project --scene res://scenes/benchmark_small_baseline.tscn --benchmark-headless-summary` | `tests/examples/godot/test_project/scenes/benchmark_small_baseline.gd` |
| Run benchmark lane suite (profile runner) | `python3 tests/runtime/run_benchmark.py --profile quick --generate-dummy-assets` | `tests/runtime/run_benchmark.py`, `tests/examples/godot/test_project/scenes/benchmark_suite/` |
| Compare QA output against stored baseline (strict) | `python3 tests/ci/run_baseline_qa.py --category qa --qa-require-capture --qa-baseline tests/ci/baselines/qa_results.json --require-qa-baseline` | `BaselineQARunner.compare_qa_baseline()` and `main()` in `tests/ci/run_baseline_qa.py` |
| Refresh QA baseline snapshot from current run | `python3 tests/ci/run_baseline_qa.py --category qa --qa-require-capture --update-qa-baseline` | `BaselineQARunner.compare_qa_baseline()`, `validate_baseline_candidate()`. A baseline change is a contract change and needs its own justification and review ([tests/AGENTS.md](../../tests/AGENTS.md)). |
| Run renderer/static guards without Godot tests | `python3 tests/ci/run_module_tests.py --guard-only` | `main()` in `tests/ci/run_module_tests.py` |
| Run module doctests after guards | `python3 tests/ci/run_module_tests.py --godot-binary "$GODOT_BINARY"` | `MODULE_TEST_FILTERS` in `tests/ci/run_module_tests.py` |
| Run runtime validation harnesses | `python3 tests/runtime/run_runtime_validation.py --godot-binary "$GODOT_BINARY" --profile headless-ci` | `default_profile` and `profiles` in `tests/runtime/runtime_scenarios.json`; `main()` in `tests/runtime/run_runtime_validation.py` |

Always pass `--profile` to `run_runtime_validation.py`. Without it the runner uses the scenario
config's `default_profile`, which is `release-ci`: the non-headless GDScript suite with a required
renderer proof, which needs a GPU and a display. `headless-ci` is the profile baseline QA runs.
`--gd-mode` defaults to the selected profile's mode, so it is only needed to override it.
`--list-profiles` prints every profile and marks the default.

| Baseline category | Executed target | Reference |
| --- | --- | --- |
| `ply` | `tests/ci/test_ply_loader_ci.gd` (headless) | `BaselineQARunner._build_test_table()` |
| `pipeline` | `tests/ci/test_ply_pipeline_ci.gd` (headless) | `BaselineQARunner._build_test_table()` |
| `sorting` | `tests/ci/test_gpu_sorting_ci.gd`, launched on the real display (`--display-driver windows --rendering-driver vulkan --render-thread safe`) because the entry sets `requires_gpu` | `BaselineQARunner._build_test_table()`, `gpu_display_args()` |
| `runtime` | `python3 tests/runtime/run_runtime_validation.py --profile headless-ci --godot-binary <binary>` | `BaselineQARunner._build_test_table()` |
| `module` | `python3 tests/ci/run_module_tests.py --godot-binary <binary>` | `BaselineQARunner._build_test_table()` |
| `qa` | `godot <display flags> --path tests/examples/godot/test_project --script res://scripts/qa_test_runner.gd --qa-output tests/ci/qa_results.json` — runs headless by default, but on the real display (`--display-driver windows --rendering-driver vulkan --render-thread safe`) when `--qa-require-capture` / `GS_CI_GPU_REQUIRED=1` is set. Headless cannot create a RenderingDevice, so the scenes capture nothing and the category degrades to a skip; the blocking lane always passes `--qa-require-capture`. | `BaselineQARunner._build_test_table()`, `gpu_display_args()` |
| `renderer` | `res://tests/test_render_thread_dispatch.gd` inside `tests/examples/godot/test_project`, launched with `--render-thread separate` and classified by its verdict markers, not its exit code. See [Render-thread dispatch characterization](../reference/build-test-ci.md#render-thread-dispatch-characterization-live-renderingserver). | `BaselineQARunner._build_test_table()`, `BaselineQARunner.run_test()` |

The table also holds an `Importer Zero-Init Tests` entry (`tests/ci/test_importer_zero_init_ci.gd`,
category `import`). `import` is not a `--category` choice, so that entry runs only when no category
is selected (the full suite, or `--category all`).

## API

| Entry point | Key options | Behavior | Artifacts | Reference |
| --- | --- | --- | --- | --- |
| `tests/ci/run_baseline_qa.py` | `--godot`, `--quick`, `--category`, `--categories`, `--qa-baseline`, `--update-qa-baseline`, `--require-qa-baseline`, `--qa-require-capture`, `--baseline-report`, `--baseline-summary` | Orchestrates baseline categories and QA baseline compare/update flow. | `baseline_qa_results.json`, `tests/ci/qa_results.json`, `baseline_qa_regression_report.json`, `baseline_qa_regression_summary.md` | `main()`, `BaselineQARunner._save_json_report()`, `DEFAULT_BASELINE_REPORT_PATH`, `DEFAULT_BASELINE_SUMMARY_PATH` |
| `tests/ci/run_module_tests.py` | `--godot-binary`, `--base-ref`, `--guard-only`, `--skip-render-guards`, `--skip-static-guards`, `--skip-build-metadata-guard`, `--tests-unavailable-mode`, `--allow-tests-unavailable`, `--gpu`, `--lane-report` | Runs the guards, then the module doctest lanes declared in `MODULE_TEST_FILTERS` (plus `REQUIRES_RD_TEST_FILTERS` with `--gpu`), and prints the per-lane ledger. | Console output; `--lane-report <path>` writes the ledger as JSON | `main()`, `MODULE_TEST_FILTERS`; ledger format in [Build / Test / CI reference](../reference/build-test-ci.md#per-lane-result-ledger) |
| `tests/runtime/run_runtime_validation.py` | `--godot-binary`, `--profile`, `--list-profiles`, `--gd-mode`, `--skip-cpp`, `--skip-gd`, `--gd-test`, `--gd-script`, `--cpp-test`, `--fail-on-skip`, `--allow-skips`, `--allow-skip-test`, `--report-path` | Runs the C++ and GDScript runtime harnesses of the selected profile with its skip/fail policy. | `tests/runtime/runtime_validation_report.json` (or `--report-path`) | `main()`, `tests/runtime/runtime_scenarios.json` |

| Scripted entry point | Command surface | Reference |
| --- | --- | --- |
| `Makefile` | `make test` (= `guard` + `test-baseline`, which builds with `tests=yes` first); also `make test-module`, `make test-runtime`, `make test-quick` | `Makefile` targets `test`, `guard`, `test-baseline`, `test-runtime` |
| `package.json` | `npm run test` (calls `make test`) | `package.json` `scripts.test` |
| `run_tests.bat` | Windows wrapper that forwards to `ci/scripts/run_module_tests.bat` | `run_tests.bat` |
| `ci/scripts/run_module_tests.bat` | Windows: guard-only run, incremental `tests=yes` build, then module doctests | `ci/scripts/run_module_tests.bat` |

## GPU Test Harness (gs-gpu-test)

The `--gs-gpu-test` entrypoint is a second doctest runner specifically for tests tagged `[RequiresGPU]`. It bootstraps `RenderingDevice` offscreen via `Main::test_setup()` + `RenderingContextDriverVulkan` / `RenderingContextDriverD3D12` (no window) and is therefore distinct from the `--test` lane driven by `tests/ci/run_module_tests.py`. Since #329 it also registers the mock `DisplayServer` driver, so `[SceneTree]`-tagged `[RequiresGPU]` cases get a full `SceneTree` and run in the four SceneTree batches listed below.

| Task | Command | Reference |
| --- | --- | --- |
| Run the canonical compositor hazard regression | `python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot ./bin/godot.linuxbsd.editor.dev.x86_64` | `tests/ci/run_gpu_harness.py`, `modules/gaussian_splatting/tests/test_output_compositor_composite_hazard.h` |
| List the batch catalogue | `python3 tests/ci/run_gpu_harness.py --list-batches` | `BATCHES` in `tests/ci/run_gpu_harness.py` |
| Run every catalogued batch | `python3 tests/ci/run_gpu_harness.py --godot ./bin/godot.linuxbsd.editor.dev.x86_64` | `tests/ci/run_gpu_harness.py` |
| Direct doctest invocation with a custom filter | `./bin/godot.linuxbsd.editor.dev.x86_64 --gs-gpu-test --test-case="*HazardRepro*"` | `--gs-gpu-test` dispatch to `gs_gpu_test_main()` in `main/main.cpp`; `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp` |
| Force a specific GPU driver | `./bin/godot.linuxbsd.editor.dev.x86_64 --gs-gpu-test --gs-gpu-driver=d3d12` | `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp` |

Windows examples:

```powershell
python tests\ci\run_gpu_harness.py --batch CompositorHazard --godot bin\godot.windows.editor.dev.x86_64.console.exe
bin\godot.windows.editor.dev.x86_64.console.exe --gs-gpu-test --test-case="*HazardRepro*"
```

Key contracts:

- Default filter when `--test-case` is omitted: `--test-case=*[RequiresGPU]* --test-case-exclude=*[SceneTree]*,*[Importer]*`. This is only the bare-invocation default. The supervisor's batches pass explicit `--test-case=` filters, and the `[SceneTree]` corpus (#329) runs in the `NodeSceneTree`, `WorldSceneTree`, `SceneDirectorSceneTree` and `RendererSceneTree` batches.
- Batches (`python3 tests/ci/run_gpu_harness.py --list-batches`): `CompositorHazard`, `OutputCompositor`, `TileRenderer`, `GpuSorting`, `MemoryStream`, `Streaming`, `Integration`, `RendererPipeline`, `Lifetime`, `NodeSceneTree`, `WorldSceneTree`, `SceneDirectorSceneTree`, `RendererSceneTree`.
  - `GpuSorting` is **required** (#744). Its `*Sort*][RequiresGPU]*` filter matches the #508 `[GPUSortPipeline][RequiresGPU]` sticky-overflow case and, since #622, three sort-order oracles in `test_gpu_sorting.h` that compare GPU output against a reference order. `tests/ci/check_gpu_sorting_order_coverage.py` pins that the batch selects them. The separate order tests in `test_gpu_sorting.cpp` are still linker-dropped (#622/#631), and the "GPU Sorting Performance" timing benchmark is not gated.
  - `MemoryStream` (the two #798 cases) and `Streaming` are advisory. `Streaming` runs two of the three `[Streaming]` cases retagged in #908; "GPU Memory Streaming Performance" is excluded under a waiver.
  - `TileRenderer` excludes one case under a waiver (#643). Every `BatchSpec.excludes` entry must have a matching entry in `deferred_requires_gpu_waivers` in `docs/reference/renderer_release_gate_manifest.json`; `tests/ci/test_gpu_harness_deferred_contract.py` checks that.
  - The structurally empty `ComputeInfrastructure` batch was deleted in #909; its `[ComputeInfra]` cases run in the strict headless `[ComputeInfra]` lane.
- `REQUIRED_BATCHES = {"CompositorHazard", "RendererPipeline", "Lifetime", "OutputCompositor", "RendererSceneTree", "WorldSceneTree", "SceneDirectorSceneTree", "GpuSorting"}` (the four SceneTree/OutputCompositor batches promoted in #724, `GpuSorting` in #744). An import-time assertion checks every name is a defined batch; an empty filter on a required batch fails the gate, and any hollow (zero-assertion) case in a required batch fails it per case (#696). `NodeSceneTree` stays advisory (flaky under runner contention, #630).
- CI strict mode (`GITHUB_ACTIONS=true` or `CI=true`): `--godot` is required and `bin/` fallback is refused.
- The supervisor writes a JSON report atomically (`tempfile.mkstemp` + `os.replace`) and streams subprocess output with one thread per pipe (Windows-compatible).
- The doctest listener prints `[GS-GPU][RID-LEAK?] bytes=N test=advisory` above 4 MiB residual (`LEAK_BYTES_THRESHOLD`, raised from 1 MiB in #341); the supervisor folds non-zero values into a gate failure.

See `modules/gaussian_splatting/tests/README.md#gpu-test-harness` for the per-batch filter table and full listener semantics. The `.github/workflows/baseline_qa.yml` workflow's `gpu-harness` job runs after `gpu-tests` on the self-hosted Windows runner; both jobs are gated against fork-PR code via `head.repo.full_name == github.repository`.

## Examples

```bash
python3 tests/ci/run_baseline_qa.py --help
python3 tests/ci/run_module_tests.py --help
python3 tests/runtime/run_runtime_validation.py --help
python3 tests/runtime/run_runtime_validation.py --list-profiles
python3 tests/ci/run_module_tests.py --guard-only
python3 tests/ci/run_gpu_harness.py --help
python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot "$GODOT_BINARY"
```

## Troubleshooting

| Symptom | Cause | Action | Reference |
| --- | --- | --- | --- |
| `Could not find Godot binary` | `GODOT_BINARY` is unset and no `godot` binary is on `PATH`. | Set `GODOT_BINARY` or pass `--godot`/`--godot-binary`. | `main()` in `tests/ci/run_baseline_qa.py` |
| `--quick` is ignored | `--category` or `--categories` was also passed; they take precedence. | Run only one selector mode at a time. | `BaselineQARunner.run_all_tests()` |
| Module tests report disabled test runner | Binary was not built with `tests=yes`. | Rebuild the editor from repository root with `tests=yes` so the output in `bin/` includes the in-tree module and test runner. | `_test_runner_is_unavailable()` and `_tests_unavailable()` in `tests/ci/run_module_tests.py`; `modules/gaussian_splatting/SCsub` |
| Need to run only `module` or `qa` checks | Those categories are exposed only through the maintained baseline runner. | Use `tests/ci/run_baseline_qa.py --category module` or `--category qa`. | `TEST_CATEGORIES` in `tests/ci/run_baseline_qa.py` |
| Runtime run fails on skips | Every profile in `runtime_scenarios.json` sets `fail_on_skip: true`, and a profile's setting overrides the mode-based default, so switching `--gd-mode` does not relax it. | Pass `--allow-skips`, or `--allow-skip-test <name>` for a single test. A skip that a CI lane must not tolerate is a real failure there. | `main()` in `tests/runtime/run_runtime_validation.py` (`fail_on_skip` resolution) |
| Runtime run without `--profile` needs a GPU/display or fails a renderer proof | The default profile is `release-ci` (non-headless, `requires_renderer_proof`). | Pass `--profile headless-ci` for the headless lane. | `default_profile` in `tests/runtime/runtime_scenarios.json` |
| `run_gpu_harness.py` refuses to discover a binary | `GITHUB_ACTIONS=true` or `CI=true` is set; CI strict mode requires explicit `--godot`. | Pass `--godot <path>` explicitly when running under CI envs. | `tests/ci/run_gpu_harness.py` |
| GPU harness gate fails on an apparently-empty batch | The batch is in `REQUIRED_BATCHES` and its filter matched no tests. | Find the rename or tag change that emptied it and restore the coverage. Removing a batch from `REQUIRED_BATCHES` is a contract change that needs its own justification and review. | `REQUIRED_BATCHES` in `tests/ci/run_gpu_harness.py` |
| `FATAL: batch '<name>' TIMED OUT` / batch `rc=124`, `status=TIMEOUT`, `cases=0/0` | The batch exceeded its wall-clock budget, so doctest never printed a summary: its case counts parse as `0` and its leak bytes are partial. This is an absence of measurement, not a clean batch — `totals_authoritative` in the report goes `false` and the run totals exclude whatever it never reached. | First establish budget vs hang by re-running that batch alone with a large `--timeout`. If it completes, raise that batch's `BatchSpec.timeout_seconds`. `NodeSceneTree` went from the 60 s default to 180 s (#677) and then to 300 s as its corpus grew; the `BatchSpec` comment records the measured wall times. If it never completes, it is a hang — diagnose it, do not budget around it. | `BatchSpec` in `tests/ci/run_gpu_harness.py` |
| `[GS-GPU][RID-LEAK?] bytes=N test=advisory` fails the gate | A test left more than 4 MiB resident; the supervisor folds non-zero advisories into a failure. | Audit the offending test's RID lifetimes; ensure `REQUIRE_GPU_DEVICE()` cleanup runs. | `modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp`, `modules/gaussian_splatting/tests/test_macros.h` |
