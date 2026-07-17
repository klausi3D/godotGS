# Test quarantine

Production-readiness item C3 / exit criterion G5 (program ledger #458). This
page explains how known-failing module tests are tracked in-repo instead of in
memory, and how the CI harness treats them.

> **Not authoritative.** This page is a human-readable mirror. The authoritative
> source is the JSON manifest [`tests/ci/quarantine_manifest.json`](../../tests/ci/quarantine_manifest.json)
> and the harness logic in `tests/ci/run_module_tests.py`. When this page and the
> JSON/harness disagree, the JSON and harness win. The design rationale lives in
> the ADR [`docs/architecture/adr-test-quarantine-manifest.md`](../architecture/adr-test-quarantine-manifest.md).

## Two non-overlapping manifests

There are two separate homes for excluded/known-failing tests. They do not
overlap:

| Test kind | Home | Enforced by |
| --- | --- | --- |
| Headless doctest lanes (`MODULE_TEST_FILTERS` in `run_module_tests.py`) | `tests/ci/quarantine_manifest.json` | `_run_quarantine_manifest_guard` + the doctest-lane wiring in `run_module_tests.py` |
| GPU `[SceneTree]` / `[Importer]` tests deferred because no GPU runner is available (issue #329) | `renderer_release_gate_manifest.json:deferred_requires_gpu_waivers` | `tests/ci/check_renderer_release_gates.py` (its `closure_policy`) |

A headless lane that is known-failing goes in the quarantine manifest. A
GPU-only test that cannot run in the current CI environment goes in the
release-gate waiver array. Never duplicate an entry across both.

## Status: ships empty (Slice 1)

The manifest currently ships **empty** (`{"schema_version": 1, "entries": []}`).
An empty manifest is behaviorally inert: no lane is treated as quarantined, and
the harness behaves exactly as it did before the mechanism existed (no gate,
lane outcome, or exit code changes). Populating the manifest is Slice 2 and is
individually human-gated (see below).

## Schema

Each object in `entries` describes one quarantined lane.

| Field | Required | Meaning |
| --- | --- | --- |
| `lane` | yes | Must equal a lane name in `MODULE_TEST_FILTERS` (e.g. `GaussianSplatting [Synthetic]`). The guard rejects unknown lanes. |
| `reason` | yes | Short human explanation of the failure. |
| `issue_url` | yes | Link to the tracking issue for the failure. |
| `base_sha_proven_failing` | yes | The base commit SHA on which the failure was reproduced (charter section 2.7). |
| `owner` | yes | Who owns getting the lane back to green. |
| `risk` | yes | Risk class of the quarantined area (e.g. `R3`). |
| `expires_utc` | yes | ISO-8601 UTC timestamp. The guard fails once this is in the past, forcing re-verification or removal. |
| `test_case` | yes | Doctest-style wildcard (`*` and `?` only; use `*...*` for a substring) matched against the failing doctest case name(s). A lane bundles many cases, so this narrows the quarantine to the specific known failure - only a failure whose case name matches is tolerated; any other case failing in the same lane fails the run. Required. (On a whole-lane crash the match cannot be applied - see below.) |
| `mitigation` | no | Optional note on interim mitigation. |

`schema_version` at the top level must be `1`.

## Harness semantics (no gate weakened)

The quarantine mechanism is purely additive. It never makes a passing lane fail
into success, and it never silences a lane that is not in the manifest.

A quarantine entry is honored **only when the lane actually ran and failed a
real test**. The harness tolerates only a genuine failure signal (a
nonzero/crash exit, or a doctest summary with failed tests or assertions);
every other exit-0 result fails, so a stale or misconfigured entry cannot hide.

- **Lane absent from the manifest:** today's exact behavior. Strict lanes still
  block; advisory lanes still advise.
- **Quarantined lane that RAN and reported per-case failures** (a doctest summary
  with failed tests or assertions): the failing doctest case names are extracted
  and matched against the entry's `test_case`. The failure is tolerated **only if
  every failing case matches** `test_case`:
  `[module-tests][QUARANTINE] '<lane>' failed as expected in matched case(s) [...] (test_case '<pattern>', issue <url>, base <sha>); tolerating.`
  If any **other** case failed in the same lane, that is a new regression and the
  run fails:
  `[module-tests][QUARANTINE-UNEXPECTED] '<lane>' quarantines test_case '<pattern>' but other case(s) failed: [...]; new regression - failing (issue <url>).`
  If failures are reported but no failing case name can be parsed, the run fails
  closed (the failure cannot be confirmed to be the quarantined one):
  `[module-tests][QUARANTINE-UNVERIFIED] ...`. Tolerated runs increment
  `quarantined_failing` (surfaced in the totals line).
  A quarantine tolerates ONLY its exact known failure, never a NEW skipped test:
  if a matched-failure lane ALSO printed a `Skipping test - ...` marker, the same
  strict-CI skipped-marker policy that applies to non-quarantined lanes applies
  here and the run fails:
  `[module-tests][QUARANTINE-UNEXPECTED] '<lane>' is quarantined but introduced newly skipped coverage (N skipped marker(s)) in strict CI - failing (issue <url>).`
  When a lane IS tolerated, its skipped-marker counts are still folded into the
  totals (`lanes_with_skips` / `skipped_markers`) so newly-skipped coverage is
  never hidden behind the quarantine.
- **Quarantined lane that CRASHED** (nonzero exit with no per-case doctest
  summary): a crash takes down the whole lane, so per-case matching is impossible
  and the lane is tolerated as a whole:
  `[module-tests][QUARANTINE] '<lane>' crashed as expected (no per-case doctest summary; tolerating the whole lane ...).`
  **Limitation:** a crash-quarantine can mask a NEW crash in the same lane until
  the entry expires. Mitigate by targeting the **narrowest possible lane filter**
  so the tolerated blast radius is minimal. (BDD `SCENARIO` cases, whose name
  doctest prints without a `TEST CASE:` prefix, are likewise treated as
  unparseable and fail closed rather than being silently tolerated.)
- **Quarantined lane that PASSES with real executed coverage:** the run fails
  (anti-rot):
  `[module-tests][QUARANTINE-STALE] '<lane>' is quarantined but PASSED - delete its manifest entry (issue <url>).`
  A quarantine entry cannot outlive the failure it tracks.
- **Quarantined lane that exits 0 with zero executed coverage** (its filter no
  longer matches any test after a rename/removal): the run fails - the entry is
  stale/misconfigured and lost its coverage, so it is not mistaken for an
  expected failure:
  `[module-tests][QUARANTINE] '<lane>' is quarantined but exercised no failing test (0 coverage; stale/misconfigured entry) - failing (issue <url>).`
- **Quarantined lane that exits 0 with no doctest summary at all:** treated as a
  harness error, not an expected failure, and the run fails. This prevents a
  silently broken harness from masquerading as a tolerated failure.
- **Schema guard:** runs in the fast `--guard-only` lane. It fails on a missing
  required field, an unknown `lane`, or a past `expires_utc`, and then runs the
  mechanism's own unit test (`tests/ci/test_quarantine_manifest.py`) so the
  tolerate / stale / coverage-lost / harness-error logic is exercised in CI. An
  empty manifest passes trivially.

## Adding an entry (Slice 2 - human-gated)

Every quarantine entry is an individually human-approved decision (charter section 6).
Agents may draft entries but never self-approve. Before an entry is added:

1. Reproduce the failure and record the exact `base_sha_proven_failing`.
2. Open a tracking issue and put its URL in `issue_url`.
3. Fill every required field, set a bounded `expires_utc`, and get explicit
   owner approval.
4. Set `test_case` to the narrowest wildcard that matches the known failing
   case(s) and nothing else, so a new failure elsewhere in the lane still fails
   the run. If the failure is a whole-lane crash (no per-case granularity),
   choose the narrowest `lane` filter available to bound what the entry masks.

Removing an entry is expected as soon as the underlying failure is fixed; the
stale-pass check will fail the build if a fixed lane is left quarantined.
