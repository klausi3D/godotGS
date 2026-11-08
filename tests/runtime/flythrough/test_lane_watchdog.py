#!/usr/bin/env python3
"""BV-06b liveness watchdog: the rules of both modes and the API reading, with a fake API.

Discriminating cases: no run, only VOID/NOT_RUN/verdict-less runs, a run
queued too long and a run outside the window each fail the pre-M2 watchdog;
one FAIL/INVALID/PASS in the window passes it. In M2 mode a FAIL alone, two
consecutive VOIDs and a cancelled run fail. An unreadable API answer is a
watchdog failure, never "nothing found".

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, List, Optional
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import lane_watchdog as wd  # noqa: E402

NOW = datetime(2026, 11, 16, 6, 0, tzinfo=timezone.utc)


def _t(delta: timedelta) -> str:
    return (NOW - delta).strftime("%Y-%m-%dT%H:%M:%SZ")


def run(i: int, verdict: Optional[str], age: timedelta = timedelta(days=2), status: str = "completed",
        conclusion: str = "success") -> Dict[str, object]:
    return {"id": i, "status": status, "conclusion": conclusion, "created_at": _t(age), "verdict": verdict}


class PreM2(unittest.TestCase):
    def ok(self, runs: List[Dict[str, object]]) -> bool:
        return wd.evaluate(runs, NOW, "pre-m2")[0]

    def test_no_run_is_not_alive(self) -> None:
        self.assertFalse(self.ok([]))

    def test_a_measured_verdict_in_the_window_is_alive(self) -> None:
        for verdict in ("PASS", "FAIL", "INVALID"):
            self.assertTrue(self.ok([run(1, verdict)]), verdict)

    def test_void_not_run_and_no_verdict_are_not_alive(self) -> None:
        for verdict in ("VOID", "NOT_RUN", None):
            self.assertFalse(self.ok([run(1, verdict)]), verdict)

    def test_a_run_older_than_nine_days_does_not_count(self) -> None:
        self.assertFalse(self.ok([run(1, "PASS", age=timedelta(days=9, seconds=1))]))
        self.assertTrue(self.ok([run(1, "PASS", age=timedelta(days=9))]))

    def test_a_run_queued_over_twenty_hours_fails(self) -> None:
        queued = run(2, None, age=timedelta(hours=20, seconds=1), status="queued", conclusion="")
        self.assertFalse(self.ok([queued, run(1, "PASS")]))
        queued = run(2, None, age=timedelta(hours=19), status="queued", conclusion="")
        self.assertTrue(self.ok([queued, run(1, "PASS")]))


class M2(unittest.TestCase):
    def ok(self, runs: List[Dict[str, object]]) -> bool:
        return wd.evaluate(runs, NOW, "m2")[0]

    def test_pass_is_required(self) -> None:
        self.assertFalse(self.ok([run(1, "FAIL")]))
        self.assertTrue(self.ok([run(1, "PASS")]))

    def test_two_consecutive_voids_fail(self) -> None:
        self.assertFalse(self.ok([run(3, "VOID"), run(2, "VOID"), run(1, "PASS", age=timedelta(days=3))]))
        self.assertTrue(self.ok([run(3, "VOID"), run(2, "PASS"), run(1, "VOID", age=timedelta(days=3))]))

    def test_a_cancelled_run_fails(self) -> None:
        self.assertFalse(self.ok([run(2, None, conclusion="cancelled"), run(1, "PASS")]))

    def test_unknown_mode_is_refused(self) -> None:
        with self.assertRaises(wd.WatchdogError):
            wd.evaluate([], NOW, "later")


def _zip(record: dict) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        archive.writestr("verdict.json", json.dumps(record))
    return buf.getvalue()


class FakeApi:
    def __init__(self, runs: Dict[str, list], artifacts: Dict[int, list], blobs: Dict[int, bytes]) -> None:
        self.runs, self.artifacts, self.blobs = runs, artifacts, blobs
        self.paths: List[str] = []

    def __call__(self, path: str) -> bytes:
        self.paths.append(path)
        if "/actions/workflows/" in path:
            event = path.split("event=", 1)[1].split("&", 1)[0]
            return json.dumps({"workflow_runs": self.runs.get(event, [])}).encode()
        if path.endswith("/zip"):
            return self.blobs[int(path.split("/artifacts/", 1)[1].split("/", 1)[0])]
        run_id = int(path.split("/runs/", 1)[1].split("/", 1)[0])
        return json.dumps({"artifacts": self.artifacts.get(run_id, [])}).encode()


def _api_run(i: int, title: str = "Beyond-VRAM fly-through A", age: timedelta = timedelta(days=1)) -> dict:
    return {"id": i, "status": "completed", "conclusion": "failure", "created_at": _t(age),
            "run_attempt": 1, "display_title": title}


class ReadsTheApi(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name)
        self._stdout = mock.patch("sys.stdout", new_callable=io.StringIO)
        self._stdout.start()

    def tearDown(self) -> None:
        self._stdout.stop()
        self._tmp.cleanup()

    def watch(self, api: FakeApi, events=("schedule",)) -> int:
        with mock.patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": ""}):
            return wd.run_watchdog("o/r", "beyond_vram_flythrough.yml", "pre-m2", list(events), self.out, api, NOW)

    def report(self) -> dict:
        return json.loads((self.out / "watchdog.json").read_text(encoding="utf-8"))

    def test_the_latest_attempts_verdict_is_read(self) -> None:
        api = FakeApi(
            {"schedule": [_api_run(10)]},
            {10: [{"id": 1, "name": "bvw-verdict-10-1"}, {"id": 2, "name": "bvw-verdict-10-2"},
                  {"id": 3, "name": "beyond-vram-flythrough-A-10-2"}]},
            {1: _zip({"verdict": "VOID"}), 2: _zip({"verdict": "FAIL"})},
        )
        self.assertEqual(self.watch(api), 0)
        self.assertEqual(self.report()["runs"][0]["verdict"], "FAIL")

    def test_a_run_without_a_verdict_artifact_is_not_alive(self) -> None:
        api = FakeApi({"schedule": [_api_run(10)]}, {10: []}, {})
        self.assertEqual(self.watch(api), 1)
        self.assertIsNone(self.report()["runs"][0]["verdict"])

    def test_the_watchdogs_own_runs_are_skipped(self) -> None:
        api = FakeApi({"schedule": [_api_run(11, title=wd.WATCHDOG_TITLE)]},
                      {11: [{"id": 5, "name": "bvw-verdict-11-1"}]}, {5: _zip({"verdict": "PASS"})})
        self.assertEqual(self.watch(api), 1)
        self.assertEqual(self.report()["runs"], [])

    def test_dispatch_runs_count_only_when_asked(self) -> None:
        api = FakeApi({"workflow_dispatch": [_api_run(12)]},
                      {12: [{"id": 6, "name": "bvw-verdict-12-1"}]}, {6: _zip({"verdict": "INVALID"})})
        self.assertEqual(self.watch(api), 1)
        self.assertEqual(self.watch(api, ("schedule", "workflow_dispatch")), 0)

    def test_an_unreadable_answer_fails_the_watchdog(self) -> None:
        def broken(path: str) -> bytes:
            raise wd.WatchdogError("gh api exited 1: HTTP 403")

        self.assertEqual(self.watch(broken), 1)
        self.assertIn("could not read the lane", self.report()["problems"][0])
        api = FakeApi({"schedule": [_api_run(10)]}, {10: [{"id": 1, "name": "bvw-verdict-10-1"}]},
                      {1: b"not a zip"})
        self.assertEqual(self.watch(api), 1)


if __name__ == "__main__":
    unittest.main()
