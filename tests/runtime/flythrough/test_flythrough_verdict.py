#!/usr/bin/env python3
"""BV-09 gate verdict: every rule turns red under its own mutation.

Input is the captured output of the first valid M1 run (tier A, run
42512864344, `fixtures/bv09/`, see its README): the probe summary verbatim
(gzipped), `build_info.json` and the step records. Each test mutates exactly
one signal of that run, or one entry of the committed thresholds, and asserts
the verdict the ADR demands. A rule whose mutation stayed green would be
decorative; the legal routes (a value inside its limit, a hard entry that
passes) are tested too, so the module cannot pass by refusing everything.

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import contextlib
import copy
import gzip
import io
import json
import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))

import dispatch_lane as lane  # noqa: E402
import flythrough_verdict as fv  # noqa: E402

FIXTURE = HERE / "fixtures" / "bv09"
COMMIT = "46e1c556aa0f5980123e22cde6b419df5c9af5d6"
GIB = 1024**3


def _fixture_inputs() -> dict:
    with gzip.open(FIXTURE / "summary.json.gz", "rt", encoding="utf-8") as handle:
        summary = json.load(handle)
    steps = {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in sorted((FIXTURE / "steps").glob("*.json"))}
    build_info = json.loads((FIXTURE / "build_info.json").read_text(encoding="utf-8"))
    return {"summary": summary, "build_info": build_info, "steps": steps, "sha": COMMIT}


_INPUTS = _fixture_inputs()
_THRESHOLDS = fv.load_thresholds()
CONTENTION_OK = {"step": "contention", "status": "OK", "exit_code": 0, "reason": "contention postflight CLEAN"}


class _Case(unittest.TestCase):
    def setUp(self) -> None:
        self.inputs = copy.deepcopy(_INPUTS)
        # The M1 run predates the contention record (BV-06b); add the one the lane writes now.
        self.inputs["steps"]["contention"] = dict(CONTENTION_OK)
        self.th = copy.deepcopy(_THRESHOLDS)

    @property
    def a(self) -> dict:
        return self.inputs["summary"]["worlds"]["A"]

    def passes(self) -> list:
        return self.a["flythrough"]["passes"]

    def run_verdict(self, tier: str = "A", arm: str = "flat") -> dict:
        return fv.evaluate(self.th, tier, arm, self.inputs)

    def row(self, result: dict, gid: str) -> dict:
        rows = [r for r in result["entries"] if r["id"] == gid]
        self.assertEqual(len(rows), 1, gid)
        return rows[0]

    def assert_entry(self, gid: str, status: str, reason_part: str = "") -> dict:
        result = self.run_verdict()
        row = self.row(result, gid)
        self.assertEqual(row["status"], status, row)
        self.assertIn(reason_part, row["reason"])
        return result


class CapturedRun(_Case):
    def test_the_captured_run_as_it_is_is_invalid_only_for_want_of_a_contention_record(self) -> None:
        del self.inputs["steps"]["contention"]
        result = self.run_verdict()
        self.assertEqual(result["verdict"], fv.INVALID)
        self.assertEqual(self.row(result, "V1")["status"], fv.INVALID)
        others = {r["id"]: r["status"] for r in result["entries"] if r["section"] == "validity" and r["id"] != "V1"}
        self.assertEqual(set(others.values()), {fv.PASS}, others)

    def test_with_contention_clean_every_validity_holds_and_the_stop_and_teleport_settles_fail(self) -> None:
        result = self.run_verdict()
        statuses = {r["id"]: r["status"] for r in result["entries"]}
        self.assertEqual(statuses, {
            "V1": "PASS", "V2": "PASS", "V3": "PASS", "V5": "PASS", "V7": "PASS", "V8": "PASS",
            "V9-evictions": "PASS", "V9-peak-resident": "PASS", "M1-settle": "PASS",
            "G1": "PASS", "G2a": "PASS", "G4": "PASS", "G6": "FAIL", "G6t": "FAIL",
        })
        self.assertEqual(result["verdict"], fv.FAIL)
        self.assertEqual(self.row(result, "G6")["observed"], 4.151)
        self.assertEqual(self.row(result, "G6t")["observed"], 10.797)

    def test_startup_rows_are_reported_and_never_counted(self) -> None:
        row = self.row(self.run_verdict(), "V7")
        self.assertEqual(row["status"], fv.PASS)
        startup = row["observed"]["startup_rows"]
        self.assertEqual(startup["status"], "INVALID")
        self.assertIn("settings_drifted", startup["reason"])
        self.assertIs(startup["counted_in_verdict"], False)

    def test_report_only_until_a_hard_entry_exists(self) -> None:
        result = self.run_verdict()
        self.assertEqual(result["enforced_verdict"], fv.NOT_RUN)
        self.assertTrue(result["report_only"])


class Validity(_Case):
    def test_v1_void_voids_the_run(self) -> None:
        self.inputs["steps"]["contention"] = {"step": "contention", "status": "VOID"}
        self.assertEqual(self.run_verdict()["verdict"], fv.VOID)

    def test_v1_absent_or_not_ok(self) -> None:
        self.inputs["steps"]["contention"]["status"] = "FAIL"
        self.assert_entry("V1", fv.INVALID)

    def test_v2_world_digest_differs_from_the_committed_one(self) -> None:
        self.inputs["summary"]["worlds"]["T0"]["world_file_sha256"] = "0" * 64
        result = self.assert_entry("V2", fv.INVALID, "world_digest_mismatch")
        self.assertEqual(result["verdict"], fv.INVALID)

    def test_v2_reads_the_committed_digests_not_the_worlds_own(self) -> None:
        self.th["globals"]["worlds"]["A"]["generator_params_sha"] = "f" * 64
        self.assert_entry("V2", fv.INVALID, "world_digest_mismatch")

    def test_v2_loaded_counts(self) -> None:
        self.a["splat_count"] -= 1
        self.assert_entry("V2", fv.INVALID, "splat_count")

    def test_v2_needs_a_verified_file(self) -> None:
        del self.inputs["steps"]["generate"]
        self.assert_entry("V2", fv.INVALID, "not verified")
        self.inputs["steps"]["cache_check"] = {"step": "cache_check", "status": "OK"}
        self.assert_entry("V2", fv.PASS, "cache_check")

    def test_v3_wrong_device(self) -> None:
        self.a["driver"]["adapter"] = "NVIDIA GeForce RTX 3090"
        self.assert_entry("V3", fv.INVALID, "tier_device_mismatch")

    def test_v3_world_too_small(self) -> None:
        self.th["globals"]["min_vram_ratio"] = 3.4
        self.assert_entry("V3", fv.INVALID, "world_too_small")

    def test_v5_rows_written_differs_from_frames_rendered(self) -> None:
        self.a["trace"]["rows_written"] -= 1
        self.assert_entry("V5", fv.INVALID, "rows_written")

    def test_v5_short_segment(self) -> None:
        self.passes()[1]["segments"]["S4"]["coverage_m"]["value"] -= 1.0
        self.assert_entry("V5", fv.INVALID, "S4")

    def test_v5_missing_checkpoint_or_repetition(self) -> None:
        self.passes()[0]["checkpoints"].pop()
        self.assert_entry("V5", fv.INVALID, "checkpoints")
        self.setUp()
        self.passes().pop()
        self.assert_entry("V5", fv.INVALID, "repetitions")

    def test_v5_other_path(self) -> None:
        self.a["path_sha256"] = "1" * 64
        self.assert_entry("V5", fv.INVALID, "path_sha256")

    def test_v7_drift_in_measured_rows(self) -> None:
        self.passes()[1]["settings"]["effective_max_chunks"] = [256, 480]
        self.assert_entry("V7", fv.INVALID, "effective_max_chunks")

    def test_v7_measured_rows_not_ok(self) -> None:
        self.passes()[0]["settings"]["status"] = "INVALID"
        self.assert_entry("V7", fv.INVALID, "measured rows")

    def test_v7_tuned_setting_or_readback_differs(self) -> None:
        key = "rendering/gaussian_splatting/streaming/max_chunks_in_vram"
        self.inputs["summary"]["settings"]["tuned"][key] = 320
        self.assert_entry("V7", fv.INVALID, "tuned")
        self.setUp()
        self.a["driver"]["project_settings_readback"][key] = 320.0
        self.assert_entry("V7", fv.INVALID, "readback")

    def test_v8_binary_identity(self) -> None:
        self.inputs["build_info"]["sha256"] = "0" * 64
        self.assert_entry("V8", fv.INVALID, "sha256")
        self.setUp()
        self.inputs["build_info"]["scons_args"] = self.inputs["build_info"]["scons_args"].replace("dev_build=no", "dev_build=yes")
        self.assert_entry("V8", fv.INVALID, "dev_build=no")
        self.setUp()
        self.inputs["sha"] = "a" * 40
        self.assert_entry("V8", fv.INVALID, "run commit")
        self.setUp()
        self.inputs["build_info"] = None
        self.assert_entry("V8", fv.INVALID, "MISSING")

    def test_v9_evictions(self) -> None:
        self.a["flythrough"]["streaming_whole_run"]["chunks_evicted_sampled"]["value"] = 999
        self.assert_entry("V9-evictions", fv.INVALID, "not_streaming")
        self.a["flythrough"]["streaming_whole_run"]["chunks_evicted_sampled"] = {"status": "MISSING", "value": None}
        self.assert_entry("V9-evictions", fv.INVALID, "MISSING")

    def test_v9_peak_resident(self) -> None:
        self.a["flythrough"]["streaming_whole_run"]["peak_vram_mb"]["value"] = 6000.0
        self.assert_entry("V9-peak-resident", fv.INVALID, "resident_too_large")

    def test_m1_needed_above_cap_is_invalid_settled_or_not(self) -> None:
        cp = self.passes()[0]["checkpoints"][1]
        self.assertTrue(cp["settled"])
        cp["cap_fit"] = {"status": "INVALID", "chunk_cap": 320, "needed_chunks": 399, "settle_required": False,
                         "reason": "needed 399 > cap 320: priority proof missing"}
        self.assert_entry("M1-settle", fv.INVALID, "needed 399 > cap 320")

    def test_m1_missed_deadline_or_absent_record(self) -> None:
        self.passes()[1]["checkpoints"][4]["settled"] = False
        self.assert_entry("M1-settle", fv.INVALID, "settle_deadline_missed")
        self.setUp()
        del self.passes()[1]["checkpoints"][2]["cap_fit"]
        self.assert_entry("M1-settle", fv.INVALID, "no needed/cap record")


class Gates(_Case):
    def test_g1(self) -> None:
        self.a["process"]["exit_code"] = 1
        self.assert_entry("G1", fv.FAIL, "exit code")
        self.setUp()
        self.a["process"]["timed_out"] = True
        self.assert_entry("G1", fv.FAIL, "timed_out")
        self.setUp()
        self.a["driver"]["reason"] = "aborted"
        self.assert_entry("G1", fv.FAIL, "final marker")

    def test_g2a(self) -> None:
        self.a["flythrough"]["streaming_whole_run"]["peak_vram_mb"]["value"] = 4100.0
        result = self.assert_entry("G2a", fv.FAIL)
        self.assertEqual(result["verdict"], fv.FAIL)
        self.setUp()
        self.a["flythrough"]["settings_whole_run"]["effective_vram_budget_mb"] = []
        self.assert_entry("G2a", fv.INVALID, "MISSING")

    def test_g4(self) -> None:
        self.a["rss"]["peak_rss_bytes"]["value"] = 13 * GIB
        self.assert_entry("G4", fv.FAIL)
        self.a["rss"]["peak_rss_bytes"] = {"status": "MISSING", "value": None, "reason": "no VmHWM"}
        self.assert_entry("G4", fv.INVALID, "no VmHWM")

    def test_g6_legal_route_and_absent_checkpoint(self) -> None:
        for p in self.passes():
            for c in p["checkpoints"]:
                c["settle_s"]["value"] = min(c["settle_s"]["value"], 2.9)
        self.assert_entry("G6", fv.PASS)
        self.assert_entry("G6t", fv.PASS)
        self.assertEqual(self.run_verdict()["verdict"], fv.PASS)
        self.passes()[0]["checkpoints"][2]["settle_s"] = {"status": "MISSING", "value": None}
        self.assert_entry("G6", fv.INVALID, "MISSING")

    def test_g6t_only_reads_its_checkpoints(self) -> None:
        for p in self.passes():
            for c in p["checkpoints"]:
                if c["checkpoint"] in ("C4", "C7"):
                    c["settle_s"]["value"] = 5.9
        self.assert_entry("G6t", fv.PASS)
        self.assert_entry("G6", fv.FAIL)


class Combination(_Case):
    def test_no_summary_is_not_run(self) -> None:
        self.inputs["summary"] = None
        result = self.run_verdict()
        self.assertEqual((result["verdict"], result["enforced_verdict"]), (fv.NOT_RUN, fv.NOT_RUN))

    def test_unreadable_thresholds_are_invalid(self) -> None:
        self.th = {"schema": "something-else"}
        self.assertEqual(self.run_verdict()["verdict"], fv.INVALID)

    def test_a_tier_without_thresholds_is_not_run(self) -> None:
        self.assertEqual(self.run_verdict(tier="T0")["verdict"], fv.NOT_RUN)

    def test_an_entry_without_an_evaluator_is_invalid(self) -> None:
        self.th["gates"]["G99"] = dict(self.th["gates"]["G4"])
        self.assert_entry("G99", fv.INVALID, "no evaluator")

    def test_an_entry_without_the_run_tier_is_invalid(self) -> None:
        del self.th["gates"]["G4"]["tiers"]["A"]
        self.assert_entry("G4", fv.INVALID, "no value for tier A")

    def test_an_unknown_status_is_invalid(self) -> None:
        original = fv.EVALUATORS["G4"]
        fv.EVALUATORS["G4"] = lambda entry, limit, ctx: ("OK", 1, "a status this module does not know")
        try:
            self.assert_entry("G4", fv.INVALID, "unknown status")
        finally:
            fv.EVALUATORS["G4"] = original

    def test_validity_dominates_gate_failures(self) -> None:
        self.a["rss"]["peak_rss_bytes"]["value"] = 13 * GIB
        self.a["trace"]["rows_written"] -= 1
        self.assertEqual(self.run_verdict()["verdict"], fv.INVALID)

    def test_an_entry_of_the_other_arm_is_not_counted(self) -> None:
        self.th["gates"]["G4"]["arm"] = "hlod"
        result = self.run_verdict()
        self.assertEqual(self.row(result, "G4")["status"], fv.NOT_APPLICABLE)
        self.assertEqual(self.row(fv.evaluate(self.th, "A", "hlod", self.inputs), "G4")["status"], fv.PASS)

    def test_enforced_verdict_covers_hard_entries_only(self) -> None:
        for gid in ("G1", "G2a", "G4"):
            self.th["gates"][gid]["mode"] = "hard"
        result = self.run_verdict()
        self.assertEqual((result["verdict"], result["enforced_verdict"]), (fv.FAIL, fv.PASS))
        self.assertFalse(result["report_only"])
        self.a["rss"]["peak_rss_bytes"]["value"] = 13 * GIB
        self.assertEqual(self.run_verdict()["enforced_verdict"], fv.FAIL)


class Wiring(unittest.TestCase):
    """Deleting a wire must turn something red (tests/AGENTS.md, evidence integrity practice 3)."""

    def test_summarize_writes_the_gate_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "probe").mkdir()
            with gzip.open(FIXTURE / "summary.json.gz", "rb") as src, (out / "probe" / "summary.json").open("wb") as dst:
                shutil.copyfileobj(src, dst)
            shutil.copy(FIXTURE / "build_info.json", out / "build_info.json")
            shutil.copytree(FIXTURE / "steps", out / "steps")
            with contextlib.redirect_stdout(io.StringIO()):
                lane.summarize(out, contention_outcome="success", meta={"tier": "A", "sha": COMMIT})
            verdict = json.loads((out / lane.VERDICT_FILE).read_text(encoding="utf-8"))
            gates = json.loads((out / fv.GATE_VERDICT_FILE).read_text(encoding="utf-8"))
        self.assertEqual(gates["verdict"], fv.FAIL)
        self.assertEqual(verdict["gate_verdict"]["verdict"], fv.FAIL)
        self.assertTrue(verdict["gate_verdict"]["report_only"])
        self.assertEqual(gates["thresholds_source_run"], "42512864344")

    def test_summarize_without_tier_reports_not_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(io.StringIO()):
                lane.summarize(Path(tmp))
            verdict = json.loads((Path(tmp) / lane.VERDICT_FILE).read_text(encoding="utf-8"))
        self.assertEqual(verdict["gate_verdict"]["verdict"], fv.NOT_RUN)

    def test_the_guard_and_this_test_run_in_the_guard_lane(self) -> None:
        text = (ROOT / "tests" / "ci" / "run_module_tests.py").read_text(encoding="utf-8")
        for needle in ('"check_flythrough_thresholds.py"', '"test_flythrough_verdict.py"'):
            self.assertTrue(needle in text, f"run_module_tests.py no longer names {needle}")
        self.assertTrue(re.search(r"\n\s+_run_flythrough_thresholds_guard,\n", text),
                        "the thresholds guard is not in the guard-only table of run_module_tests.py")

    def test_the_workflow_passes_the_tier_to_summarize(self) -> None:
        text = (ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml").read_text(encoding="utf-8")
        self.assertIn('dispatch_lane.py summarize --out "$BVW_OUT" --tier "$BVW_TIER"', text)


if __name__ == "__main__":
    unittest.main()
