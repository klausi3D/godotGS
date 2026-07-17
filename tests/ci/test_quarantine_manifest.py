#!/usr/bin/env python3
"""Unit tests for the test-quarantine mechanism in run_module_tests.py.

Production-readiness C3 / exit criterion G5 (ledger #458). These tests exercise
the schema guard, the loader, and the doctest-lane wiring against temporary
manifest fixtures. They never write a non-empty manifest into the repo: every
fixture lives in a TemporaryDirectory and the module's QUARANTINE_MANIFEST_PATH
global is patched to point at it.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_module_tests.py"
spec = importlib.util.spec_from_file_location("run_module_tests", SCRIPT)
assert spec and spec.loader
harness = importlib.util.module_from_spec(spec)
# Register in sys.modules before exec: run_module_tests uses
# `from __future__ import annotations` + @dataclass, and dataclasses resolves the
# stringized annotations via sys.modules[cls.__module__] at class-creation time.
sys.modules[spec.name] = harness
spec.loader.exec_module(harness)


# A real lane name from MODULE_TEST_FILTERS so entries pass the unknown-lane check.
VALID_LANE = harness.MODULE_TEST_FILTERS[0][0]


def _future_iso(days: int = 365) -> str:
    return (datetime.now(timezone.utc) + timedelta(days=days)).isoformat()


def _past_iso(days: int = 1) -> str:
    return (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()


# The doctest case that _fail_output() reports as failing, and a test_case glob
# that matches it (and not other cases in the lane).
FAILING_CASE = "[GaussianSplatting][Animation] plays a clip"
MATCHING_TEST_CASE = "*plays a clip*"
# A second, distinct approved failing case in the SAME lane (multiple entries).
SECOND_FAILING_CASE = "[GaussianSplatting][Animation] loops the clip"
SECOND_TEST_CASE = "*loops the clip*"


def _valid_entry(**overrides) -> dict:
    entry = {
        "lane": VALID_LANE,
        "test_case": MATCHING_TEST_CASE,
        "reason": "known failure reproduced on base SHA",
        "issue_url": "https://github.com/example/repo/issues/999",
        "base_sha_proven_failing": "7c5b79aacbe2188da22f837cb37824a829d8e074",
        "owner": "alexander-klaus",
        "risk": "R3",
        "expires_utc": _future_iso(),
        "mitigation": "optional mitigation note",
    }
    entry.update(overrides)
    return entry


def _pass_output(tests: int = 3, asserts: int = 10) -> str:
    return (
        f"[doctest] test cases: {tests} | {tests} passed | 0 failed\n"
        f"[doctest] assertions: {asserts} | {asserts} passed | 0 failed\n"
    )


def _case_failure_block(case: str) -> str:
    # Mirrors doctest's ConsoleReporter failure block: a lazily-printed
    # "TEST CASE:  <name>" header followed by a "<file>(<line>): ERROR: ..." line.
    return (
        "===============================================================================\n"
        "modules/gaussian_splatting/tests/test_animation.h(42):\n"
        f"TEST CASE:  {case}\n"
        "\n"
        "modules/gaussian_splatting/tests/test_animation.h(50): ERROR: CHECK( a == b ) is NOT correct!\n"
        "  values: CHECK( 1 == 2 )\n"
        "\n"
    )


def _fail_output(case: str = FAILING_CASE, total_cases: int = 3) -> str:
    # A realistic doctest failing run: one failing case block + the summary.
    return (
        _case_failure_block(case)
        + f"[doctest] test cases: {total_cases} | {total_cases - 1} passed | 1 failed\n"
        + "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _multi_fail_output(case_names: list[str]) -> str:
    blocks = "".join(_case_failure_block(case) for case in case_names)
    n = len(case_names)
    return (
        blocks
        + f"[doctest] test cases: {n + 1} | 1 passed | {n} failed\n"
        + f"[doctest] assertions: 10 | {10 - n} passed | {n} failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _failing_summary_no_case_output() -> str:
    # Summary reports failures but no TEST CASE header -> case name unparseable.
    return (
        "[doctest] test cases: 3 | 2 passed | 1 failed\n"
        "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        "[doctest] Status: FAILURE!\n"
    )


def _fail_output_with_skip(case: str = FAILING_CASE) -> str:
    # The matching failing case PLUS a NEW "Skipping test - ..." marker (one
    # skipped doctest marker). doctest's skip line matches DOCTEST_SKIP_MARKER_RE.
    return (
        _case_failure_block(case)
        + "Skipping test - GPU device unavailable\n"
        + "[doctest] test cases: 3 | 1 passed | 1 failed | 1 skipped\n"
        + "[doctest] assertions: 10 | 9 passed | 1 failed\n"
        + "[doctest] Status: FAILURE!\n"
    )


def _no_coverage_output() -> str:
    return (
        "[doctest] test cases: 0 | 0 passed | 0 failed\n"
        "[doctest] assertions: 0 | 0 passed | 0 failed\n"
    )


def _no_summary_output() -> str:
    return "engine started\nfilter matched nothing meaningful\nengine exited\n"


@contextlib.contextmanager
def _manifest(entries, schema_version=1):
    """Write a temporary manifest and patch the module global to point at it."""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quarantine_manifest.json"
        payload: dict = {"schema_version": schema_version, "entries": entries}
        path.write_text(json.dumps(payload), encoding="utf-8")
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", path):
            yield path


@contextlib.contextmanager
def _raw_manifest(text: str):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "quarantine_manifest.json"
        path.write_text(text, encoding="utf-8")
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", path):
            yield path


def _run_lane(lane_name: str, strict: bool, godot_result):
    """Run the doctest-lane loop for a single lane with _run_godot stubbed."""
    buffer = io.StringIO()
    with mock.patch.object(harness, "_run_godot", return_value=godot_result):
        with contextlib.redirect_stdout(buffer):
            rc = harness._run_doctest_lanes(
                "godot",
                [(lane_name, ["--headless", "--test"], strict)],
                "warn-only",
                False,
            )
    return rc, buffer.getvalue()


class QuarantineGuardTests(unittest.TestCase):
    def test_empty_manifest_passes_guard_and_loads_empty(self) -> None:
        with _manifest([]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            self.assertEqual(harness._load_quarantine(), {})

    def test_committed_manifest_is_schema_valid(self) -> None:
        # The guard runs this unit test in CI, so this must assert only that the
        # committed manifest is SCHEMA-VALID - never that it is specifically
        # EMPTY. Asserting empty here would block the Slice 2 population path the
        # moment a valid human-approved entry is committed. An empty manifest and
        # a valid populated manifest both satisfy this.
        committed = ROOT / "tests" / "ci" / "quarantine_manifest.json"
        self.assertTrue(committed.is_file(), "committed manifest must exist")
        data = json.loads(committed.read_text(encoding="utf-8"))
        self.assertEqual(data.get("schema_version"), 1)
        self.assertIsInstance(data.get("entries"), list)
        ok, messages = harness._validate_quarantine_manifest_schema()
        self.assertTrue(ok, messages)

    def test_missing_file_loads_empty_and_guard_passes(self) -> None:
        missing = ROOT / "tests" / "ci" / "does_not_exist_quarantine.json"
        self.assertFalse(missing.is_file())
        with mock.patch.object(harness, "QUARANTINE_MANIFEST_PATH", missing):
            self.assertEqual(harness._load_quarantine(), {})
            ok, _ = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok)

    def test_expired_entry_fails_guard(self) -> None:
        with _manifest([_valid_entry(expires_utc=_past_iso())]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(any("past its expires_utc" in m for m in messages), messages)

    def test_unknown_lane_fails_guard(self) -> None:
        with _manifest([_valid_entry(lane="No Such Lane")]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("not present in MODULE_TEST_FILTERS" in m for m in messages), messages
            )

    def test_missing_required_field_fails_guard(self) -> None:
        entry = _valid_entry()
        del entry["issue_url"]
        with _manifest([entry]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("missing required field 'issue_url'" in m for m in messages), messages
            )

    def test_missing_test_case_fails_guard(self) -> None:
        # test_case is REQUIRED (round-3): a lane-only quarantine would tolerate
        # any failure in the lane, so the schema guard rejects an entry without it.
        entry = _valid_entry()
        del entry["test_case"]
        with _manifest([entry]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("missing required field 'test_case'" in m for m in messages), messages
            )

    def test_two_entries_same_lane_different_test_case_accepted(self) -> None:
        # Round-6: multiple entries per lane are allowed, one per approved case.
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            loaded = harness._load_quarantine()
            self.assertEqual(len(loaded[VALID_LANE]), 2)

    def test_exact_duplicate_lane_test_case_rejected(self) -> None:
        # Same (lane, test_case) twice is a real duplicate and is rejected.
        entries = [_valid_entry(), _valid_entry()]
        with _manifest(entries):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(
                any("(lane, test_case)" in m for m in messages), messages
            )

    def test_malformed_json_fails_guard(self) -> None:
        with _raw_manifest("{not valid json"):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(any("not valid JSON" in m for m in messages), messages)

    def test_valid_populated_manifest_is_accepted_and_honored(self) -> None:
        # Forward-compat (Slice 2): a populated manifest carrying a valid,
        # human-approved entry must be ACCEPTED by the guard and honored by the
        # loader - never blocked. Proves the documented population path works.
        entry = _valid_entry()
        with _manifest([entry]):
            # Schema validation accepts the populated manifest.
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            # The full guard also accepts it (recursion env set so the nested
            # unit-test step short-circuits instead of re-spawning the suite).
            with mock.patch.dict(
                os.environ, {harness.QUARANTINE_UNITTEST_ACTIVE_ENV: "1"}
            ):
                guard_ok, _ = harness._run_quarantine_manifest_guard()
            self.assertTrue(guard_ok)
            # The loader returns a LIST of entries keyed by lane.
            loaded = harness._load_quarantine()
            self.assertIn(VALID_LANE, loaded)
            self.assertEqual(len(loaded[VALID_LANE]), 1)
            self.assertEqual(loaded[VALID_LANE][0]["issue_url"], entry["issue_url"])

    def test_guard_messages_are_ascii(self) -> None:
        # A non-ASCII byte has crashed CI's cp1252 stdout before; keep it clean.
        with _manifest([_valid_entry(lane="No Such Lane", expires_utc=_past_iso())]):
            _, messages = harness._validate_quarantine_manifest_schema()
            for message in messages:
                message.encode("ascii")  # raises UnicodeEncodeError on failure

    def test_full_guard_runs_unittest_and_recursion_guard_short_circuits(self) -> None:
        # The full guard runs schema validation AND the mechanism's unit test.
        # With the recursion-guard env var set (as it is in the spawned child),
        # the unit-test step short-circuits instead of re-spawning the suite, so
        # this test never forks. Schema passes on the committed manifest (empty
        # or validly populated).
        with mock.patch.dict(
            os.environ, {harness.QUARANTINE_UNITTEST_ACTIVE_ENV: "1"}
        ):
            ok, messages = harness._run_quarantine_manifest_guard()
        self.assertTrue(ok, messages)
        self.assertTrue(
            any("nested guard invocation" in m for m in messages), messages
        )


class QuarantineLaneWiringTests(unittest.TestCase):
    def test_quarantined_matching_case_failure_is_tolerated(self) -> None:
        # The only failing case matches the entry's test_case -> tolerated.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _fail_output()))
        self.assertEqual(rc, 0, out)
        self.assertIn("[module-tests][QUARANTINE]", out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_different_case_in_same_lane_fails_the_run(self) -> None:
        # Round-3 core: a NEW/unrelated case failing in the same quarantined lane
        # must NOT be tolerated - it is a regression and fails the run.
        other = "[GaussianSplatting][Animation] a different unrelated test"
        with _manifest([_valid_entry(test_case=MATCHING_TEST_CASE)]):
            rc, out = _run_lane(
                VALID_LANE, strict=True, godot_result=(False, False, _fail_output(case=other))
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(other, out)

    def test_quarantined_two_entries_same_lane_both_cases_tolerated(self) -> None:
        # Round-6: two entries for the same lane (distinct approved cases). A run
        # where BOTH approved cases fail is tolerated (union of patterns).
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(False, False, _multi_fail_output([FAILING_CASE, SECOND_FAILING_CASE])),
            )
        self.assertEqual(rc, 0, out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_two_entries_third_unapproved_case_fails_the_run(self) -> None:
        # Round-6 core: with two approved entries, a THIRD non-approved failing
        # case in the same lane still fails the run.
        third = "[GaussianSplatting][Animation] a third unapproved test"
        entries = [
            _valid_entry(test_case=MATCHING_TEST_CASE, issue_url="https://x/issues/1"),
            _valid_entry(test_case=SECOND_TEST_CASE, issue_url="https://x/issues/2"),
        ]
        with _manifest(entries):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(
                    False,
                    False,
                    _multi_fail_output([FAILING_CASE, SECOND_FAILING_CASE, third]),
                ),
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(third, out)

    def test_quarantined_mixed_matching_and_unexpected_fails_the_run(self) -> None:
        # A lane where the quarantined case AND another case both fail: the
        # unexpected one still forces the run to fail.
        other = "[GaussianSplatting][Animation] a different unrelated test"
        with _manifest([_valid_entry(test_case=MATCHING_TEST_CASE)]):
            rc, out = _run_lane(
                VALID_LANE,
                strict=True,
                godot_result=(False, False, _multi_fail_output([FAILING_CASE, other])),
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn(other, out)

    def test_quarantined_crash_is_whole_lane_tolerated(self) -> None:
        # A crash (nonzero exit, no per-case summary) cannot be narrowed; the
        # whole lane is tolerated (documented limitation).
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _no_summary_output()))
        self.assertEqual(rc, 0, out)
        self.assertIn("crashed as expected", out)
        self.assertIn("quarantined_failing=1", out)

    def test_quarantined_runnable_failure_without_case_name_fails(self) -> None:
        # A runnable failure whose failing case name cannot be parsed fails closed
        # (we cannot confirm it is the quarantined case).
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(
                VALID_LANE, strict=True, godot_result=(False, False, _failing_summary_no_case_output())
            )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNVERIFIED]", out)

    def test_quarantined_matching_case_with_new_skip_marker_fails_in_strict_ci(self) -> None:
        # Round-4: the approved failing case is present, but the lane ALSO printed
        # a new "Skipping test" marker. In strict CI a quarantine must apply the
        # same skipped-marker policy as a normal lane and FAIL - it tolerates only
        # its exact known failure, never newly skipped coverage.
        with mock.patch.dict(os.environ, {"CI": "1"}):
            with _manifest([_valid_entry()]):
                rc, out = _run_lane(
                    VALID_LANE, strict=True, godot_result=(False, False, _fail_output_with_skip())
                )
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-UNEXPECTED]", out)
        self.assertIn("newly skipped coverage", out)

    def test_quarantined_skip_marker_tolerated_updates_totals_when_not_ci(self) -> None:
        # Outside strict CI the skipped-marker policy does not fail the lane, but
        # the skip counts must still be folded into the totals so they are not
        # silently reported as 0 behind the quarantine.
        with mock.patch.dict(os.environ, {"CI": ""}):
            with _manifest([_valid_entry()]):
                rc, out = _run_lane(
                    VALID_LANE, strict=True, godot_result=(False, False, _fail_output_with_skip())
                )
        self.assertEqual(rc, 0, out)
        self.assertIn("failed as expected in matched case(s)", out)
        self.assertIn("quarantined_failing=1", out)
        self.assertIn("lanes_with_skips=1", out)
        self.assertIn("skipped_markers=1", out)

    def test_quarantined_zero_coverage_is_coverage_lost_and_nonzero(self) -> None:
        # Codex P2 (comment 3601513465): a quarantined lane that exits 0 with a
        # summary but zero executed coverage (its filter stopped matching any
        # test) must NOT be tolerated - it means the entry is stale/misconfigured
        # and lost its coverage. It fails the run instead of being counted as an
        # expected failure.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _no_coverage_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("exercised no failing test", out)
        self.assertNotIn("failed as expected", out)

    def test_quarantined_pass_is_stale_and_nonzero(self) -> None:
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _pass_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-STALE]", out)
        self.assertIn("PASSED", out)

    def test_quarantined_clean_summary_with_nonzero_exit_fails(self) -> None:
        # Round-5: a CLEAN all-pass summary that then exits nonzero (a teardown /
        # harness crash after all tests passed) must NOT be tolerated as a crash.
        # The summary is authoritative: the tracked failure is gone (stale) AND
        # there is a new teardown crash - both reasons to fail the run.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _pass_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("[module-tests][QUARANTINE-STALE]", out)
        self.assertIn("passed all tests", out)
        self.assertNotIn("crashed as expected", out)
        self.assertNotIn("failed as expected", out)

    def test_quarantined_harness_error_is_nonzero(self) -> None:
        # exit 0 with no doctest summary must NOT be read as an expected failure.
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _no_summary_output()))
        self.assertEqual(rc, 1, out)
        self.assertIn("no doctest summary", out)

    def test_empty_manifest_is_a_noop_for_lanes(self) -> None:
        # With an empty manifest a passing lane behaves exactly as today: rc 0,
        # no quarantine output at all.
        with _manifest([]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(True, False, _pass_output()))
        self.assertEqual(rc, 0, out)
        self.assertNotIn("[QUARANTINE", out)
        self.assertIn("quarantined_failing=0", out)


class QuarantineClassificationTests(unittest.TestCase):
    def test_classify_clean_pass(self) -> None:
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _pass_output()),
            "clean_pass",
        )

    def test_classify_clean_pass_even_with_nonzero_exit(self) -> None:
        # Summary-first: an all-pass summary is clean_pass regardless of exit code.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _pass_output()),
            "clean_pass",
        )

    def test_classify_expected_fail_on_crash(self) -> None:
        # Nonzero exit with no summary is a genuine crash-failure.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _no_summary_output()),
            "expected_fail",
        )

    def test_classify_expected_fail_on_failing_summary(self) -> None:
        # Nonzero exit with a failing summary is a genuine failure signal.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(False, _fail_output()),
            "expected_fail",
        )

    def test_classify_coverage_lost_on_exit0_zero_coverage(self) -> None:
        # Exit 0 with a summary but zero executed coverage -> coverage_lost.
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _no_coverage_output()),
            "coverage_lost",
        )

    def test_classify_harness_error_on_clean_exit_no_summary(self) -> None:
        self.assertEqual(
            harness._classify_quarantined_lane_outcome(True, _no_summary_output()),
            "harness_error",
        )


class DoctestCaseParsingTests(unittest.TestCase):
    def test_parses_single_failing_case(self) -> None:
        self.assertEqual(
            harness._parse_failing_doctest_cases(_fail_output()),
            [FAILING_CASE],
        )

    def test_parses_two_failing_cases_in_order(self) -> None:
        other = "[GaussianSplatting][Animation] second failing case"
        self.assertEqual(
            harness._parse_failing_doctest_cases(_multi_fail_output([FAILING_CASE, other])),
            [FAILING_CASE, other],
        )

    def test_ignores_warning_and_message_only_cases(self) -> None:
        # A case that only emits WARNING/MESSAGE (no ERROR) is not a failure.
        output = (
            "===============================================================================\n"
            "test.h(10):\n"
            "TEST CASE:  [GaussianSplatting][Animation] warns but passes\n"
            "\n"
            "test.h(12): WARNING: CHECK( x ) is NOT correct!\n"
            "test.h(13): MESSAGE: some note\n"
            "\n"
            "[doctest] test cases: 2 | 2 passed | 0 failed\n"
            "[doctest] assertions: 5 | 5 passed | 0 failed\n"
        )
        self.assertEqual(harness._parse_failing_doctest_cases(output), [])

    def test_no_case_header_yields_no_names(self) -> None:
        self.assertEqual(
            harness._parse_failing_doctest_cases(_failing_summary_no_case_output()), []
        )


class DoctestCaseMatchingTests(unittest.TestCase):
    def test_substring_glob_matches(self) -> None:
        self.assertTrue(harness._test_case_matches("*plays a clip*", FAILING_CASE))

    def test_tag_prefixed_glob_matches_literally(self) -> None:
        # '[' ']' are literal (not fnmatch char classes).
        self.assertTrue(
            harness._test_case_matches("[GaussianSplatting][Animation]*", FAILING_CASE)
        )

    def test_non_matching_pattern_does_not_match(self) -> None:
        self.assertFalse(harness._test_case_matches("*unrelated*", FAILING_CASE))

    def test_question_mark_wildcard(self) -> None:
        self.assertTrue(harness._test_case_matches("*play? a clip*", FAILING_CASE))


if __name__ == "__main__":
    unittest.main()
