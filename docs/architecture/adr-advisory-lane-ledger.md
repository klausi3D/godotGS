# ADR: Per-lane result ledger for `run_module_tests.py` (#705, slice 1)

- **Status:** Proposed (filed before implementation, as required for R3). Slice 1 is the
  *measurement* slice: it reports and gates nothing.
- **Risk class:** R3 — the change edits `tests/ci/run_module_tests.py` lane-execution
  logic, which `.agentic/policy.json` classifies as CI deterministic-check machinery
  (`adr_required = true`).
- **Tracking:** #705 (advisory lanes claim coverage they do not gate), #519 (streaming
  lifecycle lanes are advisory). **Precedent:**
  [`adr-test-quarantine-manifest.md`](adr-test-quarantine-manifest.md).

## Status note: the measurement falsified the motivating hypothesis

A full lane run has since been executed against a `tests=yes` binary on the self-hosted
runner. **It recorded no advisory failures**, so the premise this slice was built on —
that advisory lanes are swallowing red — is **not supported by the measurement**. That
finding stands on the record rather than being quietly dropped for being inconvenient.

What the run did surface is **absent coverage, not concealed failure**: one advisory lane
(`GPU Memory Stream`) reporting green while executing zero assertions, and a substantial
block of self-skipped coverage concentrated in one lane (`Streaming Pipeline`).

That changes what GS-705-2 should arm:

- a gate on advisory *failures* would pin zero and defend nothing, because there is nothing
  there to defend against;
- the defensible ratchets are on **executed coverage** (a lane with 0 assertions must not
  count as green) and on the **skip-marker counts** (shrink-only), each pinned at the value
  the ledger measures at arming time;
- flipping the six lanes to `strict`, as #705 literally proposes, is cheap for most of them
  and would immediately fail the zero-coverage lane. That is the correct outcome, to be
  resolved by giving that lane real coverage — never by re-hiding it.

**The coverage ratchet depends on the round-4 `zero_coverage` definition (§4a).** Armed on
the superseded passed-count formula, a lane in which every test failed would have reported
`zero_coverage=1`, indistinguishable from a lane that ran nothing, and the ratchet would
have read that catastrophe as improvement. GS-705-2 must pin against `zero_coverage` as
defined in §4a, and against `gating_failures` — round 3 renamed `strict_failures`, so any
older transcript uses a field name that no longer exists.

Neither the round-3 renames nor the round-4 `zero_coverage` fix changes the numbers that run
measured: it recorded no failing lane at all, and the superseded formulas differ from the
current ones only where failed counts are nonzero.

> The per-lane transcript, exact invocation and binary provenance for that run live in the
> pull request, not here. An ADR records *why* a decision was made and *what would change
> it*; evidence valid for one machine on one afternoon belongs with the change that produced
> it (`AGENTS.md` — no ephemeral agent artifacts in the repository).

## Context

`MODULE_TEST_FILTERS` declares 26 lanes: **20 strict, 6 advisory** (`GaussianSplatting
[Synthetic]`, `GaussianSplatting [untagged]`, `GaussianSplatting [Renderer]`,
`TileRenderer`, `GPU Memory Stream`, `Streaming Pipeline`).

For an advisory lane, `_run_doctest_lanes()` routes a nonzero exit or a crash into
`_report_failed_lane()`, which prints one free-text line and returns `True`, so the loop
continues and `main()` still returns 0. `_handle_no_executed_coverage()` /
`_report_advisory_no_coverage()` do the same for a lane that executed nothing. **On an
advisory lane, a nonzero exit, a crash, zero coverage and self-skipped coverage are all
tolerated**, and since doctest exits nonzero whenever a test fails, that covers the normal
shape of an advisory failure.

Two outcomes are **not** tolerated, on any lane, `strict` or not, and it is worth being
exact about them rather than writing the tidier absolute claim: a lane that exits 0 while
its doctest summary **reports failures**, and a lane that exits 0 with **no doctest summary
at all**, both go through `_validate_successful_lane()` → `_report_lane_failure()` → `return
1`. Both are harness anomalies rather than ordinary test failures. "Advisory" is therefore
not an unconditional exemption from the exit code, and this ADR's own first draft summarised
it as though it were, while the accompanying table and test had it right.

The end-of-run summary `_print_doctest_totals()` reports `lanes`,
`lanes_with_coverage`, `lanes_with_skips`, `lanes_unavailable`, `quarantined_failing` and
`skipped_markers`, but has **no field for advisory failures**. Nothing in the repository
records, per lane, what happened.

Two consequences are already measurable at the branch base:

- `tests/ci/check_test_lane_coverage.py` reports, un-gated, that **387 registered cases
  reach no strict module lane and no GPU batch** (59 `[RequiresGPU]`, 328 ordinary).
- The `GPU Memory Stream` lane reports `1 passed | 0 failed` with **0 assertions** — one
  case selected, nothing executed, lane green. `Streaming Pipeline` runs 62 tests /
  213 assertions with **38 skip markers**.

#705 asks for these lanes to be made strict. At the time this ADR was filed that flip could
not be made responsibly, because **no run of this repository had ever recorded whether an
advisory lane passed, failed, crashed or executed nothing.** Arming a gate on a guessed
number is as bad as having no gate. The status note above records what the first such run
actually measured, and it is not what this section predicted.

## Decision

Add a **per-lane result ledger** to `run_module_tests.py`, plus an optional
`--lane-report <path>` JSON output. It **reports; it gates nothing.**

### 1. Exit-code parity is the primary invariant

For every reachable lane outcome, the value returned by `main()` is identical to the
baseline. The ledger observes; it never decides. This is asserted directly, per outcome
class, in `tests/ci/test_run_module_tests_lane_ledger.py` — and asserted *structurally*,
by running each scenario a second time with the ledger neutered and requiring the two exit
codes to be equal. A pinned constant alone would only prove that the code still does what
it does; the neutered-run comparison proves the ledger changed no decision.

The single exception is a **harness-integrity failure** (below), which is unreachable in a
correct build.

### 2. Totality: a lane missing from the ledger is impossible by construction

The ledger is **pre-seeded** from the lane list before the first lane runs: every lane
starts with outcome `NOT-RUN` and unknown (`-1`) counts. A control-flow path that forgets
to record therefore cannot produce an *absent* lane — only a visibly `NOT-RUN` one, which
the integrity check turns into a failure when the run was not aborted earlier.

This is deliberate: "did not run" silently reading as "passed" is the exact defect class
this whole phase exists to remove, and the ledger must not reproduce it in miniature.

Mechanically:

- the per-lane body is extracted into `_execute_lane()`, whose return type is
  `tuple[int | None, LaneResult]` — every `return` must carry a result, and a bare
  `return 1` is a loud unpacking error rather than a silent omission;
- `LaneLedger.record()` refuses to overwrite an already-recorded lane (an overwrite is how
  a FAIL would become a PASS) and reports the attempt as an integrity error;
- `main()` asserts that the built run list covers `MODULE_TEST_FILTERS` itself, so a lane
  that disappears between the table and the loop fails rather than vanishing.

### 3. Unknown is `-1`, never `0`

`_parse_doctest_results()` returns `0` for every count when no doctest summary was found,
which makes "the binary crashed before printing anything" indistinguishable from "the lane
ran and passed nothing". The ledger records `-1` for all four counts whenever the summary
is absent. `skipped_markers` is counted directly from the output and is `-1` only for a
lane that was never attempted.

### 4. Record grammar is a stable contract

Per lane, one line in lane order:

```
[module-tests][lane-result] lane=<name> strict=<0|1> outcome=<OUTCOME> passed_tests=<n> passed_assertions=<n> failed_tests=<n> failed_assertions=<n> skipped_markers=<n> exit_code=<n> summary_reported=<0|1> zero_coverage=<0|1|-1>
```

`OUTCOME` is one of `PASS`, `FAIL`, `ADVISORY-FAIL`, `ADVISORY-NO-COVERAGE`,
`UNAVAILABLE`, `QUARANTINE-TOLERATED`, `QUARANTINE-REJECTED`, `NOT-RUN`.

The first eight fields are the grammar named in the task contract. `exit_code`,
`summary_reported` and `zero_coverage` are an **additive suffix**: the contract's prefix is
unchanged, and a superset is not a weakening. They are present because the process exit
code, "did doctest report at all" and "did the lane execute any coverage" are the three
facts a reader needs to tell a crash from a pass from an empty lane, and dropping them
would make the ledger unable to answer the question it was built for.

`summary_reported` was called `executed` until round 4 (§4a). The rename is recorded **in
this grammar**, not only in the code: renaming a field so that a stale parser breaks loudly
accomplishes nothing if the documented grammar still advertises the old name.

`NOT-RUN` is likewise additive. When a strict lane aborts the run, the remaining lanes are
printed as `NOT-RUN` rather than omitted, for the same reason as §2.

After the lane block, unconditionally — including when `advisory_failures=0`, so that
absence of output can never be read as absence of failures:

```
[module-tests][lane-ledger] lanes=<n> strict_lanes=<n> advisory_lanes=<n> advisory_failures=<n> advisory_zero_coverage=<n> quarantine_tolerated=<n> unavailable=<n> quarantine_rejected=<n> gating_failures=<n> passed=<n> not_run=<n>
[module-tests][lane-ledger] ADVISORY-RED lane=<name> reason=<failed|crashed|nonzero-exit-no-test-failures|no-coverage>
```

### 4a. Every field names only what it can support

Four fields were reworked because each over-claimed, and for a measurement tool
**a confidently wrong field is worse than an absent one — someone will quote it.** The rule
adopted here: prefer the narrower claim the data actually supports. Every one of these was a
field summarising across a boundary the data does not cross — outcome → lane class,
summary-exists → tests-failed, this-lane → whole-run, and passed-count → executed-at-all.

- `strict_failures` → **`gating_failures`**. The old field counted `FAIL` outcomes, but an
  *advisory* lane also records `FAIL` (exit 0 with a missing or failing summary), so the
  aggregate could read `strict_lanes=0 strict_failures=1` — an advisory harness anomaly
  charged to a strict lane. The name now matches what is counted, and the strict/advisory
  split is derived from `record.strict`, never from the outcome
  (`gating_failures_on_strict_lanes` / `gating_failures_on_advisory_lanes`, JSON).
- **`reason=`** is derived from the failed counts *and* the exit status, not from "did a
  summary exist". A clean all-pass summary followed by a nonzero exit is a teardown/harness
  failure — `_classify_quarantined_lane_outcome()` has always known this — and calling it
  `failed` would report a test failure where no test failed. It is now
  `nonzero-exit-no-test-failures`, named for the observation rather than a guessed cause.
- **`baseline_note`** no longer asserts that CI exited 0. The loop continues past an
  `ADVISORY-FAIL`, so a *later* strict lane can fail the run while the same report claims
  success. The note now describes the advisory *result* ("did not itself fail the run"),
  which is true and stable, and the JSON carries `lane_loop_exit_code` — narrowly named
  because the harness-integrity check and the report write itself can still change the
  process exit code after the report is on disk.
- **`zero_coverage`** is derived from **passed + failed**, not from the passed counts alone
  (round 4), and **`executed` → `summary_reported`**. The old formula emitted
  `zero_coverage=1` for a lane in which *every* test failed — both passed counts are zero
  there — so the same record said "nothing ran" and "these ran and failed". That is the
  inverse of the defect this ledger exists to expose, in the one field that exposes it, and
  it mattered strategically: the ratchet this ADR recommends below is armed on **executed
  coverage**, so a catastrophically failing lane would have registered as *improvement*.
  `executed` was renamed in the same pass because it only ever meant "doctest printed a
  summary" — a `0 passed | 0 failed` summary is reported and executes nothing.

  The invariant now stands on its own, checked over every record rather than case by case:
  **`zero_coverage=1` and a nonzero failed count are mutually exclusive**, and a record
  holding both is a harness-integrity failure. The removed formula passed every case-by-case
  test written for it while violating that property, which is the argument for stating
  invariants rather than examples.

  One place still derives coverage from passed counts, correctly:
  `_handle_no_executed_coverage()` decides the `ADVISORY-NO-COVERAGE` *outcome*, and it is
  reached only after `_validate_successful_lane()` has established zero failures — so passed
  *is* the executed total there. That is baseline gating logic and changing it would break
  exit-code parity.

### 5. `--lane-report <path>` writes the same records as JSON

`{schema_version, baseline_note, generated_utc, lanes: [...], totals: {...}}`. The option
is optional; omitting it changes nothing. The file is a build output and stays **untracked**.
It is rejected in combination with `--guard-only`, where it could only ever produce an
empty report that a reader would mistake for "no lanes failed".

**The report is evidence, so writing it must never destroy evidence.** The writability
preflight probes a **sibling** temp file, never the destination, and the write is
serialize → temp file → `os.replace()`. The destination is therefore always either the
previous report or the complete new one — never empty, never partial. The first
implementation of the preflight opened the destination in `"w"` mode, which truncated the
last valid measurement at second zero; a run interrupted after that point left behind
exactly the empty report the rest of the runner treats as a red flag, with the real
measurement gone. (`os.replace`, not `os.rename`: it is atomic on Windows and overwrites.
The repo's recorded non-atomic-rename hazard is Godot's `DirAccess::rename`, which is
engine code and does not apply here.)

### 6. Fail closed on anything the ledger cannot determine

An unwritable report path, a lane recorded twice, a self-contradictory record, or a lane the
runner **attempted** that was left `NOT-RUN` is a **harness-integrity failure**: the runner
prints `[module-tests][lane-ledger][INTEGRITY] ...` and returns nonzero. This is not a test
gate — it cannot be reached by any test outcome — but silently omitting a lane would recreate
the bug being fixed, so it must be loud.

"Attempted" is counted, not inferred from an abort flag. Validation used to be skipped
entirely whenever the run aborted, which excused every record collected *before* the abort —
reopening the "did not run reads as passed" hole for exactly the runs where something had
already gone wrong. Only lanes after the aborting one may be `NOT-RUN`.

**A ledger that fails its own integrity check is not written to `--lane-report`.** The
atomic write guarantees the destination is never empty or partial; it does not decide
whether the content deserves to replace what is there. Overwriting the last valid
measurement with a known-untrustworthy one destroys good evidence in favour of bad — the
same trade the preflight fix in §5 refused — and the full block is on stdout regardless. The
refusal is printed, so a missing report is never silent.

## Explicitly not decided here

- **No lane's `strict` flag changes.** No lane is promoted or demoted.
- **No entry, include or exclude pattern in `MODULE_TEST_FILTERS` / `REQUIRES_RD_TEST_FILTERS`
  changes.**
- **No gate is armed.** No threshold, ratchet or new failure condition on lane outcomes is
  introduced. Arming is GS-705-2, and it must be a **shrink-only ratchet pinned at a
  measured value** taken from this ledger — never a guessed number.
- **No test case is added, deleted, renamed, retagged, excluded or quarantined.** Reducing
  a stranded or no-strict-lane count by removing tests is prohibited.
- **No workflow change.** Uploading the JSON as a CI artifact is out of scope; stdout is
  sufficient for this slice.

## Consequences

- The first honest, per-lane measurement of which advisory lanes are red becomes available
  from a single run, and is the pinned input for GS-705-2. It has now been taken, and it
  redirected GS-705-2 from "gate advisory failures" (measured 0, would defend nothing) to
  "gate executed coverage and skip counts" — see the status note.
- CI output grows by ~30 lines per run.
- Turning previously hidden state visibly RED is an accepted and expected outcome. The red
  must **not** be resolved by reverting this change, by re-hiding the lane, or by removing
  the affected tests.

## Alternatives rejected

- **Flip the six advisory lanes to strict now (#705 as literally filed).** Rejected: with
  no measurement, this either breaks CI for an unknown number of unrelated reasons or, if
  it happens to pass, proves nothing about the other outcome classes. Measure first.
- **Report only failures.** Rejected: it makes an empty ledger ambiguous between "no
  failures" and "the ledger stopped running", which is the same silent-absence bug.
- **Append to the existing `_print_doctest_totals()` line.** Rejected: the aggregate cannot
  express per-lane outcomes, and #705 needs the per-lane record.
