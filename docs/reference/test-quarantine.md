# Test quarantine

Production-readiness item C3 / exit criterion G5 (program ledger #458). This
page explains how known-failing module tests are tracked in-repo instead of in
memory, and how the CI harness treats them.

> **Not authoritative.** This page is a human-readable mirror. The authoritative
> source is the JSON manifest [`tests/ci/quarantine_manifest.json`](../../tests/ci/quarantine_manifest.json)
> and the harness logic in `tests/ci/run_module_tests.py`. When this page and the
> JSON/harness disagree, the JSON and harness win. The design rationale lives in
> the ADR [`docs/architecture/adr-test-quarantine-manifest.md`](../architecture/adr-test-quarantine-manifest.md).

## Three non-overlapping homes

There are three separate homes for excluded/known-failing tests. They do not
overlap:

| Test kind | Home | Enforced by |
| --- | --- | --- |
| Headless doctest lanes (`MODULE_TEST_FILTERS` in `run_module_tests.py`) that are known-**failing** | `tests/ci/quarantine_manifest.json` -> `entries` | `_run_quarantine_manifest_guard` + the doctest-lane wiring in `run_module_tests.py` |
| Test cases that match **no lane at all** and therefore never run (issue #520) | `tests/ci/quarantine_manifest.json` -> `unlaned_tests` | `tests/ci/check_test_lane_coverage.py` |
| GPU `[SceneTree]` / `[Importer]` tests deferred because no GPU runner is available (issue #329) | `renderer_release_gate_manifest.json:deferred_requires_gpu_waivers` | `tests/ci/check_renderer_release_gates.py` (its `closure_policy`) |

A headless lane that runs and fails goes in `entries`. A test that no lane ever
selects goes in `unlaned_tests`. A GPU-only test that cannot run in the current
CI environment goes in the release-gate waiver array. Never duplicate an entry
across two of them - they answer different questions ("this fails", "this never
runs", "this cannot run here").

## Status of `entries`: ships empty (Slice 1)

The `entries` array currently ships **empty**. An empty `entries` is
behaviorally inert: no lane is treated as quarantined, and the harness behaves
exactly as it did before the mechanism existed (no gate, lane outcome, or exit
code changes). Populating it is Slice 2 and is individually human-gated (see
below).

`unlaned_tests` is **not** empty - see its own section below.

### The retired prose baseline (#650, measured 2026-08)

Until #650 the module's known-failure baseline existed only as prose in PR
descriptions and issue bodies: "3 known failures". That number was never
checked by anything, and it had already drifted. Measured on a headless
`*GaussianSplatting*` run excluding `[RequiresGPU]`:

| claimed | measured | disposition |
| --- | --- | --- |
| 3 known failures | **1** | 2 were fixed before anyone re-read the prose |

- one was fixed by **#652** (retagged `[Thumbnail][Editor]` / `[Thumbnail][SceneTree]`
  into real lanes),
- one was fixed by **#653 / #655** (the test itself was wrong),
- one **survives**: `[GaussianSplatting][Thumbnail] Generator caches deterministic
  asset+settings keys` fails at
  `modules/gaussian_splatting/tests/test_gaussian_importer.h:1181`, where
  `CHECK(misses >= 2)` reports `CHECK( 0 >= 2 )`. Tracked in **#814**.

**The survivor cannot be enrolled in `entries`.** An entry names a lane in
`MODULE_TEST_FILTERS`, and the harness fails a quarantined lane that does not
actually run and fail (`QUARANTINE-STALE` / `coverage_lost`). No `[Thumbnail]`
lane exists, so the failure runs nowhere and there is no lane to quarantine. The
repo has **no mechanism that makes a known-failing test both RUN and be
tolerated while it is unlaned** - until `[Thumbnail]` gets a lane (#819), its
`unlaned_tests` declaration documents *non-execution*, not
*disposition-of-failure*. That gap is stated here rather than papered over.

## Ratchet: the manifest cannot grow silently

The baseline lives in the **guard**, `tests/ci/test_quarantine_manifest.py`,
never in the manifest the guard checks. A manifest that is its own baseline is
not a ratchet: before #650 a PR could append a quarantine entry, or an 11th
`unlaned_tests` declaration, in a single hunk and go green. (This is the same
hole `tests/ci/test_gpu_harness_deferred_contract.py` closed for
`unbatched_requires_gpu_backlog`; its own comment records why the first attempt,
which read the allowed backlog out of the manifest, was not a ratchet either.)

Both arrays are pinned three ways, because each catches something the others
miss:

| pin | catches |
| --- | --- |
| `QUARANTINE_ENTRIES_MAX`, `UNLANED_MAX_DECLARATIONS`, `UNLANED_MAX_TOTAL_COUNT` | growth in size, including raising an existing `count` by one |
| `QUARANTINE_ENTRIES_BASELINE`, `UNLANED_BASELINE` | additions by **set inclusion** - a same-size swap or a fix-one/add-one trade nets zero and still fails |
| `QUARANTINE_ENTRIES_FINGERPRINT`, `UNLANED_FINGERPRINT` | any edit at all, so a re-pin is a deliberate two-file diff instead of a one-liner |

Both fingerprints hash the **complete objects** - every field of every element,
in the order committed - and both arrays get identical treatment. Two properties
follow deliberately:

- **Totality.** Nothing enumerates field names, so a field added by a future
  schema change is hashed the day it appears. A hash over a hand-listed subset
  of fields is the same class of defect as an invariant guarded by a
  hand-written list. (Round 2 of #650 found the `unlaned_tests` hash covering
  only `test_case` and `count`, so a rewritten `owner`, `reason`, `risk` or
  `expires_utc` - and an `issue_url` swapped between two allowlisted open issues
  - were all invisible. That is the closed-issue orphaning failure in reverse,
  in the guard built to catch it.)
- **Order.** `check_test_lane_coverage.py` attributes stranded cases
  **first-match-wins** (#664), so moving a catch-all above a narrow family
  silently re-attributes cases while every count stays put. Declaration order is
  therefore pinned content, not cosmetics.

Two further pins close the ways a declaration can become permanent without ever
growing the count:

- `MAX_EXPIRY_UTC` - an absolute ceiling on `expires_utc`, on top of the
  relative `EXPIRY_HORIZON_DAYS = 180` rule. The relative horizon alone never
  stops **serial** renewal: a PR could push every expiry out by 179 days
  forever. The ceiling makes each renewal a guard edit.
- `MANIFEST_TOP_LEVEL_KEYS` - the manifest's legitimate homes are pinned, so a
  new top-level array cannot be introduced as a fresh unratcheted place to park
  declarations.

**The ratchet turns one way.** Counts may go DOWN, never UP. Raising any
constant is a review red flag: it means a test was newly stranded, or a new
failure was quarantined, instead of being given a lane.

### Re-pin procedure (SHRINK only)

1. Give the case a lane in `tests/ci/run_module_tests.py` (or a batch in
   `run_gpu_harness.py`) so it actually runs.
2. Delete or lower its declaration in `tests/ci/quarantine_manifest.json`.
3. Re-pin the constants with
   `python tests/ci/test_quarantine_manifest.py --print-fingerprint`.

That tool **prints; it never writes**. It refuses to emit anything when the
current manifest contains a declaration that is not in the pinned baseline, or a
count above its pinned value - decided by set inclusion, not by net totals.
There is no path in this repo that regenerates the pinned block from the current
tree, and deleting the manifest does not bootstrap a fresh one: the guard fails
closed on a missing, unreadable or unparseable manifest.

### Tracking-issue liveness, checked offline

Every `issue_url` in either array must reference an issue that a human has
verified **OPEN**, listed in `ISSUES_VERIFIED_OPEN` in the guard. A declaration
whose tracking issue has been closed is a **silent expiry**: the work stops
being tracked while the declaration still looks blessed.

This is not hypothetical. #650 found that **9 of the 10** `unlaned_tests`
declarations pointed at closed issues - eight at **#520** and the 59-case
`[RequiresGPU]` catch-all at **#329** - and nothing had noticed, because nothing
had ever checked. They were re-pointed at live successors (#819, #820, #814).

The check is deliberately **offline**. A guard that needs the GitHub API is a
guard that fails when the API does, and CI would then block on rate limits or
fail open. The allowlist is fail-closed in the useful direction: an issue nobody
has verified is rejected, so pointing a declaration at a new tracking issue is a
deliberate two-file diff. `ISSUES_VERIFIED_OPEN_UTC` plus
`ISSUE_VERIFICATION_MAX_AGE_DAYS` bound how stale that human verification may
get; the horizon sits later than `MAX_EXPIRY_UTC`, so re-checking issue state
falls due as part of the renewal every declaration already needs.

### Per-declaration content rules

Field *presence* was already checked; presence is not hygiene. Both arrays are
additionally checked for: a `reason` under 40 characters or consisting of a
placeholder token (`TODO` / `TBD` / `FIXME` / `N/A` / `unknown` / `none` / ...);
an `issue_url` outside `https://github.com/klausi3D/godotGS/issues/<number>`; an
`expires_utc` in the past, beyond the 180-day horizon, or beyond
`MAX_EXPIRY_UTC`; a `risk` outside `{R0, R1, R2, R3}`; and, for `entries` only,
a `base_sha_proven_failing` that is not exactly 40 lowercase hex characters.

## Schema

Each object in `entries` describes one quarantined lane.

| Field | Required | Meaning |
| --- | --- | --- |
| `lane` | yes | Must equal a lane name in `MODULE_TEST_FILTERS` (e.g. `GaussianSplatting [Synthetic]`). The guard rejects unknown lanes. A lane may appear in more than one entry (see below). |
| `reason` | yes | Short human explanation of the failure. |
| `issue_url` | yes | Link to the tracking issue for the failure. |
| `base_sha_proven_failing` | yes | The base commit SHA on which the failure was reproduced (charter section 2.7). |
| `owner` | yes | Who owns getting the lane back to green. |
| `risk` | yes | Risk class of the quarantined area (e.g. `R3`). |
| `expires_utc` | yes | ISO-8601 UTC timestamp. The guard fails once this is in the past, forcing re-verification or removal. |
| `test_case` | yes | Doctest-style wildcard (`*` and `?` only; use `*...*` for a substring) matched against the failing doctest case name(s). A lane bundles many cases, so this narrows the quarantine to the specific known failure - only a failure whose case name matches is tolerated; any other case failing in the same lane fails the run. Required. (On a whole-lane crash the match cannot be applied - see below.) |
| `mitigation` | no | Optional note on interim mitigation. |

`schema_version` at the top level must be `1`.

## `unlaned_tests` - tests that match no lane (#520)

`tests/ci/check_test_lane_coverage.py` parses every `TEST_CASE` name out of the
test sources, imports the lane definitions from `run_module_tests.py`
(`MODULE_TEST_FILTERS`) and `run_gpu_harness.py` (`BATCHES`), and fails when a
registered case matches **none** of them.

`REQUIRES_RD_TEST_FILTERS` is deliberately not counted as coverage: the runner
only appends that lane under `--gpu`/`GS_RUN_GPU_TESTS=1`, which the blocking
workflow does not pass, and it is `strict=False` regardless. It is a catalogue,
not a lane, so crediting it would report coverage that does not exist. Such a
case compiles, links and registers, but no lane ever selects it: it can never
run and can never fail CI.

Both sides are derived, never transcribed. #520 catalogued three stranded
families by hand and missed a fourth (`[World]`) that existed at filing time; a
hand-maintained list is the same class of artifact as the bug.

Matching uses **doctest's** wildcard semantics, ported from
`thirdparty/doctest/doctest.h` (`wildcmp`): only `*` and `?` are special, `[` and
`]` are literal, and matching is case-insensitive because doctest's
`case_sensitive` option defaults to false. Python's `fnmatch` treats `[...]` as a
character class and answers a different question - an `fnmatch`-based lane audit
is unreliable.

| Field | Required | Meaning |
| --- | --- | --- |
| `test_case` | yes | Doctest-style wildcard matched against stranded case names. Keep it as narrow as the family allows. |
| `count` | yes | Exactly how many stranded cases this declaration covers. Family wildcards are open-ended, so without a count a brand-new stranded case joining an already-declared family would pass silently — the declaration would amnesty tests written long after anyone agreed to it. |
| `reason` | yes | Why these cases have no lane, and what it would take to give them one. |
| `issue_url` | yes | Tracking issue. |
| `owner` | yes | Who owns getting them laned. |
| `expires_utc` | yes | ISO-8601 UTC. The guard fails once this is in the past. |
| `risk` | no | Risk class, for consistency with `entries`. |

Declarations are verified in **both** directions and on the count: an entry
matching zero stranded cases fails as stale; an entry matching **more** than it
declares fails, naming the newcomer; an entry matching **fewer** fails with an
instruction to lower the count so the slack cannot be reoccupied. The list can
neither rot into a permanent amnesty nor quietly widen.

### Strict-coverage contracts - a promotion cannot quietly unwind (#846)

Reaching *a* lane is not the same as reaching a lane that can fail CI. Promoting
a corpus out of the advisory `[untagged]` safety net takes **two** coupled edits
in `run_module_tests.py` - the tag joins `HEADLESS_GAUSSIAN_SCOPED_TAGS`, and a
`strict=True` lane joins `MODULE_TEST_FILTERS`. Undo **both** and every case
falls back to the advisory net; retag **some** of the cases and those fall back
while the strict lane stays green and non-empty. Neither shape strands anything,
so neither is caught by the check above.

`STRICT_COVERAGE_CONTRACTS` in `check_test_lane_coverage.py` gates the property
directly: for each declared corpus - named by its source file(s) **and** by a tag
pattern - every case must be executed by at least one lane whose `strict` flag is
true. The cases are derived from the sources and the lanes from
`MODULE_TEST_FILTERS`, so no case list or lane list is maintained by hand; the
only thing written down is which corpora are load-bearing, which is exactly the
fact a tree that has already lost the lane can no longer tell you.

Both keys must match at least one case **on their own**. That is deliberate: a
contract whose file was renamed, or whose tag was misspelled, would otherwise
enumerate nothing, find nothing uncovered, and pass. An empty
`STRICT_COVERAGE_CONTRACTS` fails for the same reason. Measured on PR #850, all
four undo shapes are red - deleting both halves (11 uncovered), retagging four
cases (4 uncovered, in either the new-tag or dropped-tag form), and flipping the
lane's `strict` flag to `False` (11 uncovered) - while all four are green without
the contract.

### What the guard does not check

It does not fail on cases outside a strict-coverage contract that reach only a
**non-strict** lane. 381 of 856 registered cases reach no strict module lane and
no GPU batch, most of them legitimately (GPU harness, advisory safety nets).
Gating that globally would demand hundreds of declarations, turning this manifest
into the rubber stamp it exists to prevent. The number is printed on every run so
it stays visible, and the contracts above are how it is ratcheted deliberately,
one corpus at a time.

It also does not detect a case that matches a lane and then early-returns past
every assertion. That is vacuity, not stranding - a different defect that no lane
configuration can see.

### Multiple entries per lane

A single lane bundles many doctest cases, so it may need to quarantine more than
one known failure. **Multiple entries may share the same `lane`, one per approved
`test_case`.** The guard allows repeated lanes and rejects only an exact-duplicate
`(lane, test_case)` pair. At runtime a lane's **approved patterns are the union**
of the `test_case` globs across all of its entries: a runnable failure is
tolerated only if every failing case matches at least one approved pattern, and
any failing case matching none fails the run. (One entry per lane is the common
case and behaves exactly as before.)

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
  and matched against the lane's **approved patterns** (the union of `test_case`
  globs across all of the lane's entries). The failure is tolerated **only if
  every failing case matches at least one approved pattern**:
  `[module-tests][QUARANTINE] '<lane>' failed as expected in matched case(s) [...] (matched patterns [...], issue <url>); tolerating.`
  If any **other** case failed in the same lane (matching none of the approved
  patterns), that is a new regression and the run fails:
  `[module-tests][QUARANTINE-UNEXPECTED] '<lane>' quarantines patterns [...] but other case(s) failed: [...]; new regression - failing (issue <url>).`
  If failures are reported but no failing case name can be parsed, the run fails
  closed (the failure cannot be confirmed to be the quarantined one):
  `[module-tests][QUARANTINE-UNVERIFIED] ...`. Tolerated runs increment
  `quarantined_failing` (surfaced in the totals line).
  When a multi-entry lane IS tolerated, any entry whose `test_case` matched **no**
  current failing case is surfaced as a **WARN** (the lane is still tolerated - rc
  unchanged):
  `[module-tests][QUARANTINE-STALE-ENTRY] lane '<lane>' entry for test_case '<pattern>' matched no current failing case (fixed, or did not run this run); review/remove (issue <url>).`
  This prompts a human to re-verify or remove the entry so a fixed quarantine
  cannot silently re-tolerate a future regression of that case. It is a WARN, not
  a hard fail, because the harness cannot distinguish "fixed" from "did not run
  this pass" (env-skipped / filtered) without parsing passed-case names; the
  `expires_utc` field remains the hard backstop that forces re-verification.
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
  doctest prints without a `TEST CASE:` prefix, are named positionally from the
  test-start header, so a scenario failure is matched against the approved
  `test_case` patterns like any other case instead of inheriting the previously
  named one. A failure that still cannot be named leaves the result unparseable,
  which fails closed rather than being silently tolerated.)
- **Quarantined lane that PASSES with real executed coverage:** the run fails
  (anti-rot), **regardless of the process exit code**. The doctest summary is
  authoritative about what ran, so an all-pass summary means the tracked failure
  is gone:
  `[module-tests][QUARANTINE-STALE] '<lane>' is quarantined but PASSED - delete its manifest entry (issue <url>).`
  If the tests all pass but the process then exits nonzero (a teardown/harness
  crash after the tests), that is a stale quarantine *and* a new crash - both
  reasons to fail:
  `[module-tests][QUARANTINE-STALE] '<lane>' passed all tests (nonzero exit indicates a teardown/harness failure) - delete the entry / investigate the crash (issue <url>).`
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
