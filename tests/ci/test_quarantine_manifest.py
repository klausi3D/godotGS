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


def _valid_entry(**overrides) -> dict:
    entry = {
        "lane": VALID_LANE,
        "test_case": "descriptive optional case",
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


def _fail_output() -> str:
    return (
        "[doctest] test cases: 3 | 2 passed | 1 failed\n"
        "[doctest] assertions: 10 | 9 passed | 1 failed\n"
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

    def test_committed_manifest_is_empty_and_inert(self) -> None:
        committed = ROOT / "tests" / "ci" / "quarantine_manifest.json"
        self.assertTrue(committed.is_file(), "committed manifest must exist")
        data = json.loads(committed.read_text(encoding="utf-8"))
        self.assertEqual(data.get("schema_version"), 1)
        self.assertEqual(data.get("entries"), [])
        # The loader on the real committed file yields an empty map (inert).
        self.assertEqual(harness._load_quarantine(committed), {})
        # And the schema guard passes against the committed file.
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

    def test_malformed_json_fails_guard(self) -> None:
        with _raw_manifest("{not valid json"):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertFalse(ok)
            self.assertTrue(any("not valid JSON" in m for m in messages), messages)

    def test_valid_future_entry_passes_guard_and_loads(self) -> None:
        with _manifest([_valid_entry()]):
            ok, messages = harness._validate_quarantine_manifest_schema()
            self.assertTrue(ok, messages)
            loaded = harness._load_quarantine()
            self.assertIn(VALID_LANE, loaded)

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
        # this test never forks. Schema passes on the committed empty manifest.
        with mock.patch.dict(
            os.environ, {harness.QUARANTINE_UNITTEST_ACTIVE_ENV: "1"}
        ):
            ok, messages = harness._run_quarantine_manifest_guard()
        self.assertTrue(ok, messages)
        self.assertTrue(
            any("nested guard invocation" in m for m in messages), messages
        )


class QuarantineLaneWiringTests(unittest.TestCase):
    def test_quarantined_fail_is_tolerated(self) -> None:
        with _manifest([_valid_entry()]):
            rc, out = _run_lane(VALID_LANE, strict=True, godot_result=(False, False, _fail_output()))
        self.assertEqual(rc, 0, out)
        self.assertIn("[module-tests][QUARANTINE]", out)
        self.assertIn("failed as expected", out)
        self.assertIn("quarantined_failing=1", out)

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


if __name__ == "__main__":
    unittest.main()
