# Build / Test / CI Command Reference

Use this page when you already know you need a build, test, or CI command.
For the main build walkthrough, start with [Build from Source](../BUILDING.md).

## Build

- Base editor builds: use the canonical [Build from Source](../BUILDING.md) page.
- First visible result after a successful build: use [First Run](../getting-started/quick-start.md).

For test-enabled editor builds:

```bash
scons platform=<platform> target=editor dev_build=yes tests=yes -j<jobs>
```

> **Binary naming:** `dev_build=yes` adds a `.dev` segment to the output binary name.
> For example, Windows produces `bin/godot.windows.editor.dev.x86_64.exe`
> (not `bin/godot.windows.editor.x86_64.exe`).

## Test Runners

- Baseline QA:
  - `python3 tests/ci/run_baseline_qa.py --godot <module-built-binary>`
- Module checks/tests:
  - `python3 tests/ci/run_module_tests.py --guard-only`
  - `python3 tests/ci/run_module_tests.py --godot-binary <module-built-binary>`
  - `--guard-only` includes the renderer release-gate contract check.
  - `--lane-report <path>` additionally writes the per-lane ledger below as JSON (see [Per-lane result ledger](#per-lane-result-ledger)).
- Runtime validation:
  - `python3 tests/runtime/run_runtime_validation.py --godot-binary <module-built-binary> --gd-mode headless`
- Benchmark suite:
  - `python3 tests/runtime/run_benchmark.py --godot-binary <module-built-binary> --profile everything`
- GPU test harness (visual gate):
  - `python3 tests/ci/run_gpu_harness.py --batch CompositorHazard --godot <module-built-binary>`
  - Direct doctest invocation: `<module-built-binary> --gs-gpu-test --test-case="*HazardRepro*"`

For module-only build commands and SCons targets, see [Gaussian Splatting Build and Test Guide](../../modules/gaussian_splatting/docs/BUILD_AND_TEST.md). For test-runner overviews, see [Tests Overview](../../tests/README.md).

## Per-lane result ledger

`tests/ci/run_module_tests.py` declares 26 doctest lanes in `MODULE_TEST_FILTERS`:
**20 strict, 6 advisory** (`strict=False`). An advisory lane's failure, crash, zero
coverage and self-skipped coverage **cannot change the runner's exit code by any path**.
Since #705 the runner records what each lane actually did. **The ledger reports; it gates
nothing** — see [`docs/architecture/adr-advisory-lane-ledger.md`](../architecture/adr-advisory-lane-ledger.md).

One line per lane, in lane order:

```
[module-tests][lane-result] lane=<name> strict=<0|1> outcome=<OUTCOME> passed_tests=<n> passed_assertions=<n> failed_tests=<n> failed_assertions=<n> skipped_markers=<n> exit_code=<n> executed=<0|1> zero_coverage=<0|1|-1>
```

| `OUTCOME` | Meaning | Exit code effect |
| --- | --- | --- |
| `PASS` | exit 0 with real executed coverage | none |
| `FAIL` | strict lane failed/crashed, or any lane exited 0 with a missing/failing summary | run fails |
| `ADVISORY-FAIL` | advisory lane failed or crashed | **none — CI still exits 0** |
| `ADVISORY-NO-COVERAGE` | advisory lane executed nothing (0 passed tests or 0 passed assertions) | **none — CI still exits 0** |
| `UNAVAILABLE` | binary has no `--test` support | fails only in strict tests-unavailable mode |
| `QUARANTINE-TOLERATED` | known failure tolerated per `tests/ci/quarantine_manifest.json` | none |
| `QUARANTINE-REJECTED` | quarantine entry stale/misconfigured | run fails |
| `NOT-RUN` | the runner never reached this lane (an earlier lane aborted the run) | none |

A count of `-1` means **not known** (the lane produced no doctest summary), never zero.
`NOT-RUN` lanes are printed rather than omitted: an absent lane reading as a passed lane is
the defect this ledger exists to remove.

After the per-lane block, printed unconditionally — including when `advisory_failures=0`, so
that absence of output can never be read as absence of failures:

```
[module-tests][lane-ledger] lanes=<n> strict_lanes=<n> advisory_lanes=<n> advisory_failures=<n> advisory_zero_coverage=<n> quarantine_tolerated=<n> unavailable=<n> quarantine_rejected=<n> strict_failures=<n> passed=<n> not_run=<n>
[module-tests][lane-ledger] ADVISORY-RED lane=<name> reason=<failed|crashed|no-coverage>
```

**An `ADVISORY-RED` line means that lane FAILED and CI still exited 0.** It is not a
warning about a future problem; it is a test failure that nothing gates on today. Arming
those lanes is #705 / #519, and must be done against the measured values this ledger
produces — not a guessed threshold.

`--lane-report <path>` writes the same records as JSON
(`{schema_version, baseline_note, generated_utc, lanes, totals}`). The file is a build
output and must stay untracked. It is rejected together with `--guard-only`, where it could
only produce an empty report. An unwritable path fails the run rather than being skipped.

The ledger's own unit test (`tests/ci/test_run_module_tests_lane_ledger.py`) runs in the
`--guard-only` lane; it asserts exit-code parity per outcome class and ledger completeness
against `MODULE_TEST_FILTERS`.

## GPU Test Harness and Visual Gate

The `--gs-gpu-test` entrypoint in `main/main.cpp` is a second doctest runner that boots `RenderingDevice` offscreen (no window) for tests tagged `[RequiresGPU]`. `tests/ci/run_gpu_harness.py` is the Python supervisor that drives it in per-batch subprocesses so a driver hang or GPU OOM in one batch can't corrupt the next.

Since #329 the harness also registers the mock `DisplayServer` driver, so `[SceneTree]`-tagged `[RequiresGPU]` cases **do** get a full `SceneTree` — they run in the `NodeSceneTree` / `WorldSceneTree` / `SceneDirectorSceneTree` batches, which pass explicit `--test-case=` filters. (A bare `--gs-gpu-test` with no filter still excludes `*[SceneTree]*` as a conservative convenience default, which is why the harness can look `SceneTree`-less when invoked by hand.) This page previously said the harness has "no `SceneTree`"; that has not been true since #329, and the stale line was cited as evidence that a device-plus-`SceneTree` lane still had to be built (#675).

- Canonical detail (per-batch table, contracts, troubleshooting): [Testing Setup Guide — GPU Test Harness](../testing/setup-guide.md#gpu-test-harness-gs-gpu-test).
- Per-batch filter table and listener semantics: [`modules/gaussian_splatting/tests/README.md`](../../modules/gaussian_splatting/tests/README.md).
- Seeded golden captures and recapture workflow: [`tests/visual_baselines/README.md`](../../tests/visual_baselines/README.md).

Required-batch contract: `REQUIRED_BATCHES = {"CompositorHazard", "RendererPipeline", "Lifetime", "OutputCompositor", "RendererSceneTree", "WorldSceneTree", "SceneDirectorSceneTree"}` is asserted at import in `tests/ci/run_gpu_harness.py`. A required batch whose doctest filter matches zero test cases fails the gate — this prevents a silently-green CI when a rename empties the canonical `#256` regression batch, `#351`'s route/stage cascade coverage, `#352`'s GPU-resource lifetime proof, or the SceneTree/OutputCompositor coverage promoted in #724. `NodeSceneTree` is deliberately NOT required — its wall time is only ~1.6× under budget on the shared self-hosted runner and #630's contention variance would make it a flaky gate; it stays advisory until #630 is resolved.

## CI Source of Truth

- [Workflow overview](../../.github/workflows/README.md)
- [Production gate workflow](../../.github/workflows/gaussian_production_gates.yml)
- [Baseline QA workflow (gpu-tests + gpu-harness visual gate)](../../.github/workflows/baseline_qa.yml)
- [Renderer release gate contract](renderer-release-gates.md)

Fork-PR safety gate: the `gpu-tests` and `gpu-harness` jobs in `baseline_qa.yml` both guard on `github.event.pull_request.head.repo.full_name == github.repository`, so untrusted fork-PR code never executes on the self-hosted Windows GPU runner. Same-repo branch PRs and the merge queue still exercise the visual gate.

External advisory checks: `qlty check` is not part of the local renderer
release gate while `master` branch protection has no required status checks and
the repo has no tracked qlty configuration. Treat qlty as a non-blocking signal
unless branch protection or `docs/reference/renderer_release_gate_manifest.json`
is changed to require it.

## Common Failure Modes

- Wrong binary (stock Godot instead of module-enabled build)
- Missing toolchain dependencies (`scons`, shader compiler toolchain)
- Build path mismatch (for example using a stale editor outside this fork's `bin/` output)

Use recurring fixes:

- [Recurring issues](../troubleshooting/recurring-issues.md)
