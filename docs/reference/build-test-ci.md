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
**20 strict, 6 advisory** (`strict=False`).

When an advisory lane fails the ordinary way — the lane process exits nonzero, or crashes —
the failure is **tolerated and does not itself fail the run**. The same holds when it executes
nothing or self-skips its coverage. Since doctest exits nonzero whenever a test fails, that
covers the normal shape of an advisory failure.

**Two outcomes still fail the run for _any_ lane, `strict` or not**, and the claim above
does not extend to them: a lane that exits 0 while its doctest summary reports failures, and
a lane that exits 0 with no doctest summary at all. Both are harness anomalies rather than
ordinary test failures, and both are recorded as `FAIL` (see the table). Read "advisory" as
"failures of the ordinary shape are tolerated", not as an unconditional exemption.

Since #705 the runner records what each lane actually did. **The ledger reports; it gates
nothing** — see [`docs/architecture/adr-advisory-lane-ledger.md`](../architecture/adr-advisory-lane-ledger.md).

One line per lane, in lane order:

```
[module-tests][lane-result] lane=<name> strict=<0|1> outcome=<OUTCOME> passed_tests=<n> passed_assertions=<n> failed_tests=<n> failed_assertions=<n> skipped_markers=<n> exit_code=<n> summary_reported=<0|1> zero_coverage=<0|1|-1>
```

`summary_reported=1` means doctest printed a summary — **not** that anything ran; a
`0 passed | 0 failed` summary is reported and executes nothing. (It was called `executed`
until #822 round 4; renamed rather than redefined, because a rename breaks a parser loudly
while a silent change of meaning does not.)

`zero_coverage=1` means the lane executed no coverage, derived from **passed + failed**
counts. A lane in which every test fails has both *passed* counts at zero while having
executed the most coverage of any shape there is, so a passed-only derivation would file
the maximally-informative case under "nothing ran" — the exact inverse of what this field
exists to expose. `zero_coverage=1` together with a nonzero failed count is a
self-contradictory record and is reported as a harness-integrity failure.

| `OUTCOME` | Meaning | Exit code effect |
| --- | --- | --- |
| `PASS` | exit 0 with real executed coverage | none |
| `FAIL` | strict lane failed/crashed, or any lane exited 0 with a missing/failing summary | run fails |
| `ADVISORY-FAIL` | advisory lane failed or crashed | **none — does not itself fail the run** |
| `ADVISORY-NO-COVERAGE` | advisory lane executed nothing (0 passed tests or 0 passed assertions — this path is only reached once failures are known to be 0, so passed *is* the executed total here) | **none — does not itself fail the run** |
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
[module-tests][lane-ledger] lanes=<n> strict_lanes=<n> advisory_lanes=<n> advisory_failures=<n> advisory_zero_coverage=<n> quarantine_tolerated=<n> unavailable=<n> quarantine_rejected=<n> gating_failures=<n> passed=<n> not_run=<n>
[module-tests][lane-ledger] ADVISORY-RED lane=<name> reason=<failed|crashed|nonzero-exit-no-test-failures|no-coverage>
```

`gating_failures` counts lanes whose outcome was `FAIL`, i.e. that failed the run. It is
deliberately **not** called `strict_failures`: an advisory lane also records `FAIL` when it
exits 0 with a missing or failing summary, so counting `FAIL` outcomes as strict failures
could print `strict_lanes=0 strict_failures=1` and charge an advisory harness anomaly to a
strict lane. The strict/advisory split is derived from each lane's declared `strict` flag
and is in the JSON as `gating_failures_on_strict_lanes` /
`gating_failures_on_advisory_lanes`.

**An `ADVISORY-RED` line means that lane failed, crashed, or executed nothing, and that
outcome did not itself fail the run.** (It does *not* mean the run passed: the loop
continues after an advisory failure, so a later strict lane can still fail — read
`gating_failures` for that.) It is not a warning about a future problem; it is a lane
outcome that nothing gates on today, and `reason=` says which kind:

| `reason=` | What was observed |
| --- | --- |
| `failed` | the doctest summary reports failed tests or assertions |
| `crashed` | no doctest summary at all — the lane died before reporting |
| `nonzero-exit-no-test-failures` | every test passed and the process still exited nonzero (teardown/harness failure, not a test failure) |
| `no-coverage` | the lane executed nothing |

`nonzero-exit-no-test-failures` exists because "a summary was printed, therefore tests
failed" is wrong, and reporting it as `failed` would announce a test failure where none
occurred. `_classify_quarantined_lane_outcome()` already draws the same distinction.

Arming those lanes is #705 / #519, and must be done against the measured values this ledger
produces — not a guessed threshold. The first full measured run (2026-08-03) reported
`advisory_failures=0` with one `ADVISORY-RED … reason=no-coverage`; see the ADR for what
that does and does not license.

`--lane-report <path>` writes the same records as JSON
(`{schema_version, baseline_note, generated_utc, lane_loop_exit_code, lanes, totals}`).
`lane_loop_exit_code` is narrowly named on purpose: the report is written before the
harness-integrity check and before the write itself can fail, either of which can still make
the process exit nonzero afterwards. The file is a build
output and must stay untracked. It is rejected together with `--guard-only`, where it could
only produce an empty report. An unwritable path fails the run rather than being skipped —
checked before the lanes run, using a sibling probe file so an existing report is never
truncated. The write itself is serialize-then-temp-then-`os.replace`, so the destination is
either the previous report or the complete new one, never empty or partial.

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
