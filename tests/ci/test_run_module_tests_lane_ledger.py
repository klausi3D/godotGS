#!/usr/bin/env python3
"""Pins the per-lane result ledger in run_module_tests.py (#705, slice 1).

Three properties are asserted here, and the order matters:

1. **EXIT-CODE PARITY.** The ledger observes; it never decides. For every
   outcome class the runner's exit code must be what the baseline produced.
   That is asserted twice over, deliberately:
     - against a pinned constant, documented per class with *why* the baseline
       returns it (a pinned constant alone only proves the code still does what
       it does), and
     - against the SAME scenario re-run with the ledger neutered, which is the
       property-shaped form: whatever the decision is, adding the ledger did not
       change it. A future path that lets the ledger gate a lane outcome fails
       here even if someone also "fixes" the pinned constant.

2. **TOTALITY.** Every lane the runner attempts produces exactly one record, in
   every outcome path. A lane that is absent from the ledger is the same class
   of defect as the bug being fixed - "did not run" reading as "passed" - so the
   ledger is pre-seeded and a lane can only ever be visibly NOT-RUN, never gone.
   Completeness is asserted mechanically against `MODULE_TEST_FILTERS` itself,
   not against a hand-written list of lane names, because a hand-maintained list
   of the things a guard must cover is an invariant that is already broken.

3. **UNKNOWN != ZERO.** A lane that crashed before printing a doctest summary
   records -1, never 0. `_parse_doctest_results()` returns 0 for a missing
   summary, and propagating that would make a crash indistinguishable from a
   lane that ran and passed nothing.

Every test method drives `_run_doctest_lanes()` and asserts on the emitted
records, so stubbing the lane loop out makes the whole file fail rather than
pass vacuously. The one exception is the CLI-argument test, which by
construction never reaches the lane loop; it is mutation-proven separately by
removing the `parser.error()` it pins.

`CI` is patched explicitly in every scenario rather than inherited, because
`_enforce_skipped_marker_policy` and `_tolerate_quarantined_lane` sit behind
`_is_ci()`: a file that behaves differently when CI happens to be set is a file
whose local green says nothing about CI.
"""

from __future__ import annotations

import contextlib
import importlib.util
import inspect
import io
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_module_tests.py"


def _load_harness():
    spec = importlib.util.spec_from_file_location("run_module_tests", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_module_tests"] = module
    spec.loader.exec_module(module)
    return module


harness = _load_harness()


# --------------------------------------------------------------------------
# Baseline exit codes, per outcome class, read off e9ddb27c285's
# _run_doctest_lanes() and unchanged by the two stacked branches beneath this
# one. Each constant carries the control-flow reason it holds, so a future
# change that alters a DECISION has to argue with the reason rather than just
# retype the number.
# --------------------------------------------------------------------------
# strict lane, exit 0, real coverage -> falls through to `return 0`.
BASELINE_RC_PASS = 0
# advisory lane, nonzero exit -> _report_failed_lane() returns True -> `continue`.
BASELINE_RC_ADVISORY_FAIL = 0
# advisory lane, exit 0, zero executed coverage -> _handle_no_executed_coverage()
# returns stats (not None) for strict=False -> loop continues.
BASELINE_RC_ADVISORY_NO_COVERAGE = 0
# strict lane, nonzero exit -> _report_failed_lane() returns False -> `return 1`.
BASELINE_RC_STRICT_FAIL = 1
# exit 0 with no doctest summary -> _validate_successful_lane() returns None ->
# `return 1`. This is the ONE advisory outcome that still gates at baseline.
BASELINE_RC_NO_SUMMARY = 1
# tests-unavailable binary under warn-only -> _report_unavailable_lane() returns
# True -> lanes_unavailable += 1, `continue`.
BASELINE_RC_UNAVAILABLE_WARN = 0
# ... and under strict mode without the opt-out -> returns False -> `return 1`.
BASELINE_RC_UNAVAILABLE_STRICT = 1
# quarantined lane whose failing cases all match an approved pattern ->
# _tolerate_quarantined_lane() returns None -> `continue`.
BASELINE_RC_QUARANTINE_TOLERATED = 0
# quarantined lane that PASSED -> QUARANTINE-STALE -> `return 1`.
BASELINE_RC_QUARANTINE_REJECTED = 1

FAILING_CASE = "[GaussianSplatting][Animation] plays a clip"

LANE_RESULT_RE = re.compile(
    r"^\[module-tests\]\[lane-result\] lane=(?P<lane>.+?) strict=(?P<strict>[01]) "
    r"outcome=(?P<outcome>\S+) passed_tests=(?P<passed_tests>-?\d+) "
    r"passed_assertions=(?P<passed_assertions>-?\d+) failed_tests=(?P<failed_tests>-?\d+) "
    r"failed_assertions=(?P<failed_assertions>-?\d+) skipped_markers=(?P<skipped_markers>-?\d+) "
    r"exit_code=(?P<exit_code>-?\d+) executed=(?P<executed>[01]) "
    r"zero_coverage=(?P<zero_coverage>-?[01])$"
)


# --------------------------------------------------------------------------
# doctest output fixtures, in the framing a real ConsoleReporter emits.
# --------------------------------------------------------------------------
def _summary(passed_tests: int, failed_tests: int, passed_asserts: int, failed_asserts: int) -> str:
    return (
        f"[doctest] test cases: {passed_tests + failed_tests} | {passed_tests} passed "
        f"| {failed_tests} failed\n"
        f"[doctest] assertions: {passed_asserts + failed_asserts} | {passed_asserts} passed "
        f"| {failed_asserts} failed\n"
    )


def _case_failure_block(case: str = FAILING_CASE) -> str:
    return (
        "===============================================================================\n"
        "modules/gaussian_splatting/tests/test_animation.h(42):\n"
        f"TEST CASE:  {case}\n"
        "\n"
        "modules/gaussian_splatting/tests/test_animation.h(50): ERROR: CHECK( a == b ) "
        "is NOT correct!\n"
        "  values: CHECK( 1 == 2 )\n"
        "\n"
    )


def _skip_marker_line() -> str:
    # The real framing: doctest's log_message() calls file_line_to_stream()
    # first, so a marker can never start a line.
    return (
        "modules/gaussian_splatting/tests/test_animation.h(61): MESSAGE: "
        "GS_ENV_SKIP: RenderingDevice unavailable\n"
    )


PASS_OUTPUT = _summary(5, 0, 120, 0)
FAIL_OUTPUT = _case_failure_block() + _summary(2, 1, 9, 1) + "[doctest] Status: FAILURE!\n"
# The measured shape of the real `GPU Memory Stream` lane: one case selected,
# nothing executed, lane green.
NO_COVERAGE_OUTPUT = _summary(1, 0, 0, 0)
CRASH_OUTPUT = "engine booted\nAccess violation\n"
NO_SUMMARY_OUTPUT = "engine started\nfilter matched nothing\nengine exited\n"
UNAVAILABLE_OUTPUT = "Unknown option '--test'.\n"
PASS_WITH_SKIP_OUTPUT = _skip_marker_line() + _summary(5, 0, 120, 0)


def _godot(ok: bool, skipped: bool, output: str, returncode: int | None):
    factory = getattr(harness, "GodotRunResult", None)
    if factory is None:
        # Pre-#705 the runner had no returncode-carrying result and every caller
        # unpacked a plain 3-tuple. Degrading here (rather than raising) is what
        # keeps the revert-proof honest: a reverted run must fail on a MISSING
        # RECORD assertion, not on an AttributeError, which would only prove
        # that this file measures import success.
        return (ok, skipped, output)
    return factory(ok, skipped, output, returncode)


def _call_lane_loop(test_runs, mode, allow_unavailable, lane_report):
    """Call `_run_doctest_lanes`, tolerating a signature without --lane-report."""
    accepts_report = "lane_report_path" in inspect.signature(
        harness._run_doctest_lanes
    ).parameters
    if accepts_report:
        return harness._run_doctest_lanes(
            "godot", test_runs, mode, allow_unavailable, lane_report
        )
    return harness._run_doctest_lanes("godot", test_runs, mode, allow_unavailable)


@contextlib.contextmanager
def _neutered_ledger():
    """Disable the ledger completely, leaving baseline control flow alone.

    This is the parity oracle: whatever the runner decides with the ledger, it
    must decide the same thing without it.
    """
    if not hasattr(harness, "LaneLedger"):
        yield
        return
    with contextlib.ExitStack() as stack:
        stack.enter_context(
            mock.patch.object(harness.LaneLedger, "record", lambda self, *a, **k: None)
        )
        stack.enter_context(
            mock.patch.object(
                harness.LaneLedger, "print_block", lambda self: harness.LaneLedgerTotals()
            )
        )
        stack.enter_context(
            mock.patch.object(harness.LaneLedger, "check_integrity", lambda self, **k: [])
        )
        yield


def _drive(
    lanes,
    results,
    *,
    mode: str = "warn-only",
    allow_unavailable: bool = False,
    quarantine: dict | None = None,
    lane_report: Path | None = None,
    ci: bool = False,
    neuter_ledger: bool = False,
):
    """Run the real lane loop over `lanes` with `_run_godot` stubbed.

    `lanes` is [(name, strict)]; `results` is either one GodotRunResult reused
    for every lane, or a list of one per lane.
    """
    test_runs = [(name, ["--headless", "--test"], strict) for name, strict in lanes]
    if isinstance(results, list):
        godot_stub = mock.patch.object(harness, "_run_godot", side_effect=list(results))
    else:
        godot_stub = mock.patch.object(harness, "_run_godot", return_value=results)

    buffer = io.StringIO()
    with contextlib.ExitStack() as stack:
        stack.enter_context(mock.patch.dict(os.environ, {"CI": "1" if ci else ""}))
        stack.enter_context(godot_stub)
        stack.enter_context(
            mock.patch.object(harness, "_load_quarantine", return_value=dict(quarantine or {}))
        )
        if neuter_ledger:
            stack.enter_context(_neutered_ledger())
        stack.enter_context(contextlib.redirect_stdout(buffer))
        rc = _call_lane_loop(test_runs, mode, allow_unavailable, lane_report)
    return rc, buffer.getvalue()


def _records(output: str) -> dict[str, dict[str, str]]:
    found: dict[str, dict[str, str]] = {}
    for line in output.splitlines():
        match = LANE_RESULT_RE.match(line)
        if match:
            found[match.group("lane")] = match.groupdict()
    return found


def _record_order(output: str) -> list[str]:
    return [
        match.group("lane")
        for match in (LANE_RESULT_RE.match(line) for line in output.splitlines())
        if match
    ]


def _aggregate(output: str) -> dict[str, int]:
    for line in output.splitlines():
        if line.startswith("[module-tests][lane-ledger] lanes="):
            body = line.split("] ", 1)[1]
            return {k: int(v) for k, v in (pair.split("=") for pair in body.split(" "))}
    raise AssertionError(
        "no [module-tests][lane-ledger] aggregate line was printed; it must be emitted "
        "unconditionally so that absence of output can never read as absence of failures"
    )


def _advisory_red(output: str) -> dict[str, str]:
    red = {}
    for line in output.splitlines():
        if line.startswith("[module-tests][lane-ledger] ADVISORY-RED "):
            fields = line.split("ADVISORY-RED ", 1)[1]
            lane, reason = fields.rsplit(" reason=", 1)
            red[lane[len("lane="):]] = reason
    return red


class LaneLedgerOutcomeTests(unittest.TestCase):
    """One test per outcome class: exact record, aggregate counts, exit code."""

    maxDiff = None

    def _assert_parity(self, rc, expected_baseline_rc, scenario_kwargs, lanes, results):
        """rc must equal the baseline value AND the value the same run produces
        with the ledger disabled."""
        self.assertEqual(
            rc,
            expected_baseline_rc,
            f"exit code changed from the baseline value {expected_baseline_rc}",
        )
        rc_without_ledger, _ = _drive(lanes, results, neuter_ledger=True, **scenario_kwargs)
        self.assertEqual(
            rc,
            rc_without_ledger,
            "the ledger changed a decision: exit code differs with and without it",
        )

    def _assert_record(self, output, lane, **expected):
        records = _records(output)
        self.assertIn(
            lane,
            records,
            f"lane '{lane}' has no [module-tests][lane-result] record; an unrecorded "
            f"lane reads as a passed lane",
        )
        record = records[lane]
        for key, value in expected.items():
            self.assertEqual(
                record[key],
                str(value),
                f"lane '{lane}' record field {key}: {record[key]!r} != {str(value)!r}",
            )

    def test_pass_outcome(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self.assertIn(
            "[module-tests][lane-result] lane=StrictLane strict=1 outcome=PASS "
            "passed_tests=5 passed_assertions=120 failed_tests=0 failed_assertions=0 "
            "skipped_markers=0 exit_code=0 executed=1 zero_coverage=0",
            output,
        )
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], 1)
        self.assertEqual(totals["strict_lanes"], 1)
        self.assertEqual(totals["advisory_failures"], 0)
        self.assertEqual(totals["advisory_zero_coverage"], 0)
        self.assertEqual(totals["not_run"], 0)
        self._assert_parity(rc, BASELINE_RC_PASS, {}, lanes, results)

    def test_advisory_failure_is_recorded_and_still_exits_zero(self) -> None:
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            strict=0,
            outcome="ADVISORY-FAIL",
            passed_tests=2,
            failed_tests=1,
            passed_assertions=9,
            failed_assertions=1,
            exit_code=1,
            executed=1,
        )
        totals = _aggregate(output)
        self.assertEqual(
            totals["advisory_failures"],
            1,
            "a failed advisory lane must be counted in advisory_failures",
        )
        self.assertEqual(_advisory_red(output), {"AdvisoryLane": "failed"})
        # NO SUPPRESSION: the baseline's own line is still printed.
        self.assertIn("'AdvisoryLane' crashed or failed", output)
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_advisory_crash_records_unknown_counts_not_zero(self) -> None:
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, CRASH_OUTPUT, 3221225477)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "AdvisoryLane",
            outcome="ADVISORY-FAIL",
            passed_tests=-1,
            failed_tests=-1,
            passed_assertions=-1,
            failed_assertions=-1,
            executed=0,
            zero_coverage=-1,
            exit_code=3221225477,
        )
        self.assertEqual(
            _advisory_red(output),
            {"AdvisoryLane": "crashed"},
            "a lane that never printed a summary crashed; it did not 'fail'",
        )
        self._assert_parity(rc, BASELINE_RC_ADVISORY_FAIL, {}, lanes, results)

    def test_strict_failure_records_fail_and_gates(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(output, "StrictLane", strict=1, outcome="FAIL", failed_tests=1)
        totals = _aggregate(output)
        self.assertEqual(totals["strict_failures"], 1)
        self.assertEqual(
            totals["advisory_failures"], 0, "a strict failure is not an advisory failure"
        )
        self.assertEqual(_advisory_red(output), {})
        self._assert_parity(rc, BASELINE_RC_STRICT_FAIL, {}, lanes, results)

    def test_advisory_zero_coverage_is_recorded(self) -> None:
        # The measured `GPU Memory Stream` shape: 1 passed | 0 failed, 0 assertions.
        lanes = [("GPU Memory Stream", False)]
        results = _godot(True, False, NO_COVERAGE_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output,
            "GPU Memory Stream",
            outcome="ADVISORY-NO-COVERAGE",
            passed_tests=1,
            passed_assertions=0,
            executed=1,
            zero_coverage=1,
        )
        totals = _aggregate(output)
        self.assertEqual(totals["advisory_zero_coverage"], 1)
        self.assertEqual(_advisory_red(output), {"GPU Memory Stream": "no-coverage"})
        self._assert_parity(rc, BASELINE_RC_ADVISORY_NO_COVERAGE, {}, lanes, results)

    def test_exit_zero_without_summary_records_fail(self) -> None:
        # The one advisory outcome that gates at baseline: a harness error.
        lanes = [("AdvisoryLane", False)]
        results = _godot(True, False, NO_SUMMARY_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output, "AdvisoryLane", outcome="FAIL", passed_tests=-1, executed=0
        )
        self.assertIn("missing doctest summary", output)
        self._assert_parity(rc, BASELINE_RC_NO_SUMMARY, {}, lanes, results)

    def test_unavailable_binary_warn_only(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, True, UNAVAILABLE_OUTPUT, 1)
        rc, output = _drive(lanes, results)
        self._assert_record(output, "StrictLane", outcome="UNAVAILABLE", executed=0)
        self.assertEqual(_aggregate(output)["unavailable"], 1)
        self._assert_parity(rc, BASELINE_RC_UNAVAILABLE_WARN, {}, lanes, results)

    def test_unavailable_binary_strict_mode_records_and_gates(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(True, True, UNAVAILABLE_OUTPUT, 1)
        kwargs = {"mode": "strict"}
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="UNAVAILABLE", executed=0)
        self.assertEqual(
            _aggregate(output)["unavailable"],
            1,
            "the lane that ABORTED the run must still be in the ledger",
        )
        self._assert_parity(rc, BASELINE_RC_UNAVAILABLE_STRICT, kwargs, lanes, results)

    def test_quarantine_tolerated_is_recorded(self) -> None:
        lanes = [("StrictLane", True)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        kwargs = {
            "quarantine": {
                "StrictLane": [
                    {"test_case": "*plays a clip*", "issue_url": "https://example/issues/1"}
                ]
            }
        }
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="QUARANTINE-TOLERATED", failed_tests=1)
        self.assertEqual(_aggregate(output)["quarantine_tolerated"], 1)
        self.assertIn("[module-tests][QUARANTINE]", output)
        self._assert_parity(rc, BASELINE_RC_QUARANTINE_TOLERATED, kwargs, lanes, results)

    def test_quarantine_rejected_is_recorded(self) -> None:
        # Quarantined lane that PASSED -> stale entry -> the run fails.
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        kwargs = {
            "quarantine": {
                "StrictLane": [
                    {"test_case": "*plays a clip*", "issue_url": "https://example/issues/1"}
                ]
            }
        }
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(output, "StrictLane", outcome="QUARANTINE-REJECTED")
        self.assertEqual(_aggregate(output)["quarantine_rejected"], 1)
        self.assertIn("[module-tests][QUARANTINE-STALE]", output)
        self._assert_parity(rc, BASELINE_RC_QUARANTINE_REJECTED, kwargs, lanes, results)

    def test_strict_ci_skip_policy_failure_is_recorded_with_its_skip_count(self) -> None:
        # Runs with CI=1, because _enforce_skipped_marker_policy is behind
        # _is_ci(): a local green says nothing about this path.
        lanes = [("StrictLane", True)]
        results = _godot(True, False, PASS_WITH_SKIP_OUTPUT, 0)
        kwargs = {"ci": True}
        rc, output = _drive(lanes, results, **kwargs)
        self._assert_record(
            output, "StrictLane", outcome="FAIL", skipped_markers=1, passed_tests=5
        )
        self.assertIn("skipped doctest coverage is not allowed in CI", output)
        self._assert_parity(rc, BASELINE_RC_STRICT_FAIL, kwargs, lanes, results)

    def test_skip_markers_are_reported_not_dropped(self) -> None:
        # Same output, advisory lane, no CI: the lane passes and the ledger must
        # still carry the skip count.
        lanes = [("AdvisoryLane", False)]
        results = _godot(True, False, PASS_WITH_SKIP_OUTPUT, 0)
        rc, output = _drive(lanes, results)
        self._assert_record(
            output, "AdvisoryLane", outcome="PASS", skipped_markers=1, passed_tests=5
        )
        self._assert_parity(rc, BASELINE_RC_PASS, {}, lanes, results)


class LaneLedgerTotalityTests(unittest.TestCase):
    maxDiff = None

    def test_every_module_test_filter_lane_is_recorded(self) -> None:
        """Completeness against MODULE_TEST_FILTERS itself, not a copied list."""
        lanes = [(name, strict) for name, _f, _e, strict in harness.MODULE_TEST_FILTERS]
        rc, output = _drive(lanes, _godot(True, False, PASS_OUTPUT, 0))
        recorded = _record_order(output)
        expected = [name for name, _f, _e, _s in harness.MODULE_TEST_FILTERS]
        self.assertEqual(
            recorded,
            expected,
            "the ledger must carry exactly one record per MODULE_TEST_FILTERS lane, in order",
        )
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], len(harness.MODULE_TEST_FILTERS))
        self.assertEqual(
            totals["strict_lanes"] + totals["advisory_lanes"], len(harness.MODULE_TEST_FILTERS)
        )
        self.assertEqual(totals["not_run"], 0)
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_mixed_outcomes_are_each_recorded_once(self) -> None:
        lanes = [
            ("PassLane", True),
            ("AdvisoryFailLane", False),
            ("NoCoverageLane", False),
            ("UnavailableLane", True),
        ]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, NO_COVERAGE_OUTPUT, 0),
            _godot(True, True, UNAVAILABLE_OUTPUT, 1),
        ]
        rc, output = _drive(lanes, results)
        self.assertEqual(
            _record_order(output),
            ["PassLane", "AdvisoryFailLane", "NoCoverageLane", "UnavailableLane"],
        )
        records = _records(output)
        self.assertEqual(records["PassLane"]["outcome"], "PASS")
        self.assertEqual(records["AdvisoryFailLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(records["NoCoverageLane"]["outcome"], "ADVISORY-NO-COVERAGE")
        self.assertEqual(records["UnavailableLane"]["outcome"], "UNAVAILABLE")
        totals = _aggregate(output)
        self.assertEqual(totals["lanes"], 4)
        self.assertEqual(totals["advisory_failures"], 1)
        self.assertEqual(totals["advisory_zero_coverage"], 1)
        self.assertEqual(totals["unavailable"], 1)
        self.assertEqual(totals["passed"], 1)
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_lanes_after_an_aborting_strict_failure_are_recorded_not_run(self) -> None:
        lanes = [("PassLane", True), ("StrictFailLane", True), ("NeverReached", False)]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        rc, output = _drive(lanes, results)
        records = _records(output)
        self.assertEqual(records["StrictFailLane"]["outcome"], "FAIL")
        self.assertEqual(
            records["NeverReached"]["outcome"],
            "NOT-RUN",
            "a lane the runner never reached must be printed as NOT-RUN, never omitted",
        )
        self.assertEqual(records["NeverReached"]["passed_tests"], "-1")
        totals = _aggregate(output)
        self.assertEqual(totals["not_run"], 1)
        self.assertEqual(totals["lanes"], 3)
        self.assertEqual(rc, BASELINE_RC_STRICT_FAIL)

    def test_aggregate_line_is_printed_when_nothing_is_red(self) -> None:
        """TRUTHFUL ZERO: absence of output must never read as absence of failures."""
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        totals = _aggregate(output)
        self.assertEqual(totals["advisory_failures"], 0)
        self.assertEqual(_advisory_red(output), {})
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_missing_record_is_an_integrity_failure(self) -> None:
        """A path that forgets to record must fail, not pass quietly.

        Simulated at the seam a real regression would hit: a lane result that
        never reaches the ledger.
        """
        original = harness.LaneLedger.record

        def skip_unavailable(self, index, result, *, ended_run):
            if result.outcome == harness.LANE_OUTCOME_UNAVAILABLE:
                return
            original(self, index, result, ended_run=ended_run)

        lanes = [("UnavailableLane", True), ("PassLane", True)]
        results = [
            _godot(True, True, UNAVAILABLE_OUTPUT, 1),
            _godot(True, False, PASS_OUTPUT, 0),
        ]
        with mock.patch.object(harness.LaneLedger, "record", skip_unavailable):
            rc, output = _drive(lanes, results)
        self.assertEqual(_records(output)["UnavailableLane"]["outcome"], "NOT-RUN")
        self.assertIn("[module-tests][lane-ledger][INTEGRITY]", output)
        self.assertIn("'UnavailableLane' was attempted but produced no ledger record", output)
        self.assertNotEqual(rc, 0, "an incomplete ledger must not report success")

    def test_a_lane_cannot_be_recorded_twice(self) -> None:
        """An overwrite is how a FAIL would become a PASS."""
        lanes = [("AdvisoryLane", False)]
        results = _godot(False, False, FAIL_OUTPUT, 1)
        buffer = io.StringIO()
        with mock.patch.dict(os.environ, {"CI": ""}):
            with mock.patch.object(harness, "_run_godot", return_value=results):
                with mock.patch.object(harness, "_load_quarantine", return_value={}):
                    ledger = harness.LaneLedger([("AdvisoryLane", False)])
                    ledger.record(
                        0,
                        harness.LaneResult(outcome=harness.LANE_OUTCOME_ADVISORY_FAIL),
                        ended_run=False,
                    )
                    ledger.record(
                        0,
                        harness.LaneResult(outcome=harness.LANE_OUTCOME_PASS),
                        ended_run=False,
                    )
                    with contextlib.redirect_stdout(buffer):
                        ledger.print_block()
        self.assertEqual(_records(buffer.getvalue())["AdvisoryLane"]["outcome"], "ADVISORY-FAIL")
        errors = ledger.check_integrity(aborted=False)
        self.assertTrue(errors, "recording a lane twice must be an integrity error")
        self.assertIn("recorded twice", errors[0])
        # Non-vacuous under the anti-vacuous mutation too: also drive the loop.
        rc, output = _drive(lanes, results)
        self.assertEqual(_records(output)["AdvisoryLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_declared_lanes_absent_from_the_run_list_are_reported(self) -> None:
        full = [(name, [], strict) for name, _f, _e, strict in harness.MODULE_TEST_FILTERS]
        self.assertEqual(harness._lane_runs_missing_from_module_filters(full), [])
        dropped = full[1:]
        errors = harness._lane_runs_missing_from_module_filters(dropped)
        self.assertTrue(errors, "a lane that vanished from the run list must be reported")
        self.assertIn(harness.MODULE_TEST_FILTERS[0][0], errors[0])
        # Drive the loop as well, so this method cannot pass with the lane loop
        # stubbed out.
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")
        self.assertEqual(rc, BASELINE_RC_PASS)


class LaneReportTests(unittest.TestCase):
    maxDiff = None

    def test_json_report_matches_the_printed_records(self) -> None:
        lanes = [("PassLane", True), ("AdvisoryFailLane", False)]
        results = [
            _godot(True, False, PASS_OUTPUT, 0),
            _godot(False, False, FAIL_OUTPUT, 1),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=report)
            self.assertTrue(report.is_file(), "--lane-report must write the file")
            payload = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(payload["schema_version"], harness.LANE_LEDGER_SCHEMA_VERSION)
        self.assertIn("baseline_note", payload)
        self.assertEqual([lane["lane"] for lane in payload["lanes"]], _record_order(output))
        by_lane = {lane["lane"]: lane for lane in payload["lanes"]}
        self.assertEqual(by_lane["AdvisoryFailLane"]["outcome"], "ADVISORY-FAIL")
        self.assertEqual(by_lane["AdvisoryFailLane"]["failed_tests"], 1)
        self.assertTrue(by_lane["AdvisoryFailLane"]["advisory_red"])
        self.assertEqual(by_lane["PassLane"]["outcome"], "PASS")
        self.assertEqual(payload["totals"], _aggregate(output))
        self.assertEqual(rc, BASELINE_RC_ADVISORY_FAIL)

    def test_unwritable_report_path_fails_the_run(self) -> None:
        lanes = [("PassLane", True)]
        results = _godot(True, False, PASS_OUTPUT, 0)
        with tempfile.TemporaryDirectory() as tmp:
            unwritable = Path(tmp) / "no-such-dir" / "lane_ledger.json"
            rc, output = _drive(lanes, results, lane_report=unwritable)
        self.assertNotEqual(
            rc, 0, "a ledger that could not be persisted must not report success"
        )
        self.assertIn("--lane-report could not be written", output)
        self.assertEqual(
            _records(output)["PassLane"]["outcome"],
            "PASS",
            "the lane records are still printed when the report write fails",
        )

    def test_preflight_rejects_an_unwritable_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "ok.json"
            self.assertEqual(harness._preflight_lane_report_path(good), [])
            bad = Path(tmp) / "missing" / "nope.json"
            errors = harness._preflight_lane_report_path(bad)
            self.assertTrue(errors, "an unwritable --lane-report path must be rejected early")
            self.assertIn("not writable", errors[0])
        # Also drive the loop, so this method is not vacuous when the lane loop
        # is stubbed out.
        rc, output = _drive([("PassLane", True)], _godot(True, False, PASS_OUTPUT, 0))
        self.assertEqual(_records(output)["PassLane"]["outcome"], "PASS")
        self.assertEqual(rc, BASELINE_RC_PASS)

    def test_lane_report_is_rejected_with_guard_only(self) -> None:
        """An empty ledger from a lane-less run reads as 'nothing failed'.

        This method never reaches the lane loop by construction; it is
        mutation-proven by deleting the parser.error() it pins.
        """
        argv = ["run_module_tests.py", "--guard-only", "--lane-report", "x.json"]
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", argv):
            with contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as caught:
                    harness._parse_args()
        self.assertNotEqual(caught.exception.code, 0)
        # Named reason, not just "some nonzero exit": on a tree without the flag
        # argparse also exits nonzero, for the unrelated reason that the option
        # does not exist.
        self.assertIn("--lane-report cannot be combined with --guard-only", stderr.getvalue())
        # ... and the flag is accepted on its own.
        with mock.patch.object(sys, "argv", ["run_module_tests.py", "--lane-report", "x.json"]):
            self.assertEqual(harness._parse_args().lane_report, "x.json")


if __name__ == "__main__":
    unittest.main()
