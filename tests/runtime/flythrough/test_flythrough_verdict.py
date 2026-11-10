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
import math
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
import probe_summary as ps  # noqa: E402
import world_cache as wc  # noqa: E402

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


def _flat_m1_world_a(thresholds: dict) -> dict:
    """Tier A's committed digests before the GSQ re-prime (#1389), from the record's `replaces`.

    The captured M1 run is the uncompressed world. Since BV-C7 the committed digests are the GSQ
    world, so the fixture is judged against the digests it was captured with; the record keeps
    them. Built from the committed file, never hand-typed.
    """
    record = next(r for r in thresholds["reprime_records"] if r["world"] == "A" and "replaces" in r)
    world = dict(thresholds["globals"]["worlds"]["A"])
    world.update({k: v for k, v in record["replaces"].items() if k in world})
    return world


CONTENTION_OK = {"step": "contention", "status": "OK", "exit_code": 0, "reason": "contention postflight CLEAN"}
WORLDS_A = ("T0", "A")


def cold_record(summary: dict, resident: tuple = (0, 1000), read_fraction: float = 1.0,
                host_cache: str = "present") -> dict:
    """The probe step's D-14 record (dispatch_lane.probe) for this summary, built by world_cache."""
    expected, _ = wc.expected_unique_stored_bytes(summary, WORLDS_A)
    res = wc.residency([Path("w")], lambda path: resident)
    read = None if expected is None else math.ceil(expected * read_fraction)
    return wc.cold_cache_record(res, read, summary, WORLDS_A, host_cache)


def drop_trace(increments: list) -> "ps.Trace":
    """An in-memory trace of the drop counters: one row per entry, each entry the counters' increments
    in that row (None: the row carries no drop telemetry, the -1 the driver writes)."""
    fields = ["row", "pass_index", "segment", "view", "state", "s_m", *ps.DROP_COUNTERS]
    totals = dict.fromkeys(ps.DROP_COUNTERS, 0)
    rows = []
    for i, inc in enumerate(increments):
        if inc is None:
            values = [-1] * len(ps.DROP_COUNTERS)
        else:
            for key, value in inc.items():
                totals[key] += value
            values = [totals[k] for k in ps.DROP_COUNTERS]
        rows.append((i, 0, 0, -1, ps.STATE_HOLD, 0.0, *values))
    return ps.Trace(fields, "qqqqqd" + "q" * len(ps.DROP_COUNTERS), rows, 0)


def drop_window(drops: int = 0, samples: int = 4) -> dict:
    """A checkpoint window as the producer (probe_summary.overlap_drops) reports it: `samples`
    readbacks of 800 records over 100 splats, `drops` of them with a binning-budget drop."""
    assert 0 <= drops <= samples
    increments = []
    for i in range(samples):
        inc = {"drop_signal_samples": 1, "sampled_overlap_records_total": 800, "sampled_overlap_splats_total": 100}
        if i < drops:
            inc.update(overflow_drop_events=1, binning_drop_events=1, binning_budget_drop_events=1,
                       sampled_dropped_records_total=10)
        increments.append(inc)
    trace = drop_trace(increments)
    return ps.overlap_drops(trace, list(range(len(trace.rows))))


class _Case(unittest.TestCase):
    def setUp(self) -> None:
        self.inputs = copy.deepcopy(_INPUTS)
        # The M1 run predates the contention record (BV-06b); add the one the lane writes now.
        self.inputs["steps"]["contention"] = dict(CONTENTION_OK)
        # It predates the D-14 record of the cached probe as well (BV-06b, V10 BV-13a-2b): add a
        # proven-cold one, built by its producer. CapturedRun shows the run without it.
        self.inputs["steps"]["probe"]["data"]["cache"] = cold_record(self.inputs["summary"])
        # The M1 run also predates `settings_measured` (#1383 item 2, BV-13a). Its moving-row
        # record stands in; CapturedRun.test_the_pre_bv13a_producer_leaves_the_holds_unjudged
        # shows the captured run without it is INVALID.
        for world in self.inputs["summary"]["worlds"].values():
            for p in world["flythrough"]["passes"]:
                p["settings_measured"] = copy.deepcopy(p["settings"])
                # It predates the drop telemetry too (#1393): a zero-drop window from the producer
                # stands in; CapturedRun.test_the_pre_1393_producer_leaves_g10_unjudged shows the
                # captured run without it is INVALID.
                p["overlap_drops_checkpoints"] = drop_window()
        self.th = copy.deepcopy(_THRESHOLDS)
        # The M1 run predates the GSQ re-prime of Tier A (#1389); Reprime proves the committed
        # GSQ digests and that this flat run is INVALID against them.
        self.th["globals"]["worlds"]["A"] = _flat_m1_world_a(_THRESHOLDS)

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
    def test_the_captured_run_as_it_is_is_invalid_only_for_want_of_contention_and_cold_cache_records(self) -> None:
        self.inputs = copy.deepcopy(_INPUTS)
        for world in self.inputs["summary"]["worlds"].values():
            for p in world["flythrough"]["passes"]:
                p["settings_measured"] = copy.deepcopy(p["settings"])
        result = self.run_verdict()
        self.assertEqual(result["verdict"], fv.INVALID)
        self.assertEqual(self.row(result, "V1")["status"], fv.INVALID)
        v10 = self.row(result, "V10")
        self.assertEqual((v10["status"], v10["evidence"]), (fv.INVALID, fv.EVIDENCE_MISSING))
        self.assertIn("no D-14 cold-cache record", v10["reason"])
        others = {r["id"]: r["status"] for r in result["entries"]
                  if r["section"] == "validity" and r["id"] not in ("V1", "V10")}
        self.assertEqual(set(others.values()), {fv.PASS}, others)

    def test_with_contention_clean_every_validity_holds_and_the_stop_and_teleport_settles_fail(self) -> None:
        result = self.run_verdict()
        statuses = {r["id"]: r["status"] for r in result["entries"]}
        self.assertEqual(statuses, {
            "V1": "PASS", "V2": "PASS", "V3": "PASS", "V5": "PASS", "V7": "PASS", "V8": "PASS",
            "V9-evictions": "PASS", "V9-peak-resident": "PASS", "M1-settle": "PASS", "V10": "PASS",
            "G1": "PASS", "G2a": "PASS", "G4": "PASS", "G6": "FAIL", "G6t": "FAIL", "G10": "PASS",
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

    def test_the_pre_1393_producer_leaves_g10_unjudged(self) -> None:
        for p in self.passes():
            del p["overlap_drops_checkpoints"]
        result = self.assert_entry("G10", fv.INVALID, "overlap_drops_checkpoints absent")
        self.assertEqual(self.row(result, "G10")["evidence"], fv.EVIDENCE_MISSING)

    def test_the_pre_bv13a_producer_leaves_the_holds_unjudged(self) -> None:
        for p in self.passes():
            del p["settings_measured"]
        self.assert_entry("V7", fv.INVALID, "settings_measured: absent")

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

    def test_v7_drift_during_a_hold_only(self) -> None:
        """#1383 item 2: the moving rows are clean, a checkpoint hold is not."""
        held = self.passes()[0]["settings_measured"]
        held.update(status="INVALID", settings_drifted=True, effective_max_chunks=[400, 480],
                    reason="V7 INVALID settings_drifted: chunk cap changed in the measured rows")
        self.assertEqual(self.passes()[0]["settings"]["status"], "OK")
        self.assert_entry("V7", fv.INVALID, "pass 0 settings_measured: measured rows 'INVALID'")
        self.setUp()
        self.passes()[1]["settings_measured"]["effective_vram_budget_mb"] = [256.0]
        self.assert_entry("V7", fv.INVALID, "settings_measured: effective_vram_budget_mb [256.0]")

    def test_v7_node_readback_of_cap_and_far(self) -> None:
        """#1383 item 1: the node's readback, not the request, must equal the frozen cap and far plane."""
        self.a["driver"]["node_readback"]["max_splat_count"] = 3_999_999
        self.assert_entry("V7", fv.INVALID, "node readback max_splat_count: 3999999")
        self.setUp()
        del self.a["driver"]["node_readback"]["max_render_distance"]
        self.assert_entry("V7", fv.INVALID, "MISSING: node readback max_render_distance: absent")
        self.setUp()
        self.a["driver"]["node_readback"] = None
        self.assert_entry("V7", fv.INVALID, "MISSING")

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
        result = self.assert_entry("M1-settle", fv.INVALID, "no needed/cap record")
        self.assertEqual(self.row(result, "M1-settle")["evidence"], fv.EVIDENCE_MISSING)
        self.setUp()
        self.passes()[0]["checkpoints"][0]["cap_fit"] = {"status": "MISSING", "needed_chunks": None,
                                                          "chunk_cap": None, "reason": "needed_chunks absent"}
        result = self.assert_entry("M1-settle", fv.INVALID, "MISSING")
        self.assertEqual(self.row(result, "M1-settle")["evidence"], fv.EVIDENCE_MISSING)

    def test_m1_measured_overflow_is_present_evidence(self) -> None:
        cp = self.passes()[0]["checkpoints"][1]
        cp["cap_fit"] = {"status": "INVALID", "chunk_cap": 320, "needed_chunks": 399, "settle_required": False,
                         "reason": "needed 399 > cap 320: priority proof missing"}
        result = self.assert_entry("M1-settle", fv.INVALID, "needed 399 > cap 320")
        self.assertEqual(self.row(result, "M1-settle")["evidence"], fv.EVIDENCE_PRESENT)

    def set_cold(self, **kw) -> dict:
        self.inputs["steps"]["probe"]["data"]["cache"] = cache = cold_record(self.inputs["summary"], **kw)
        return cache

    def test_v10_cold_passes_with_the_label(self) -> None:
        result = self.assert_entry("V10", fv.PASS, "host_cache present")
        row = self.row(result, "V10")
        self.assertEqual(row["observed"], {"resident_fraction": 0.0, "read_fraction": 1.0, "host_cache": "present"})
        self.assertEqual(row["limit"], 0.9)

    def test_v10_resident_pages_before_the_probe(self) -> None:
        self.set_cold(resident=(10, 1000))  # exactly 1 %
        self.assert_entry("V10", fv.PASS)
        self.set_cold(resident=(11, 1000))
        result = self.assert_entry("V10", fv.INVALID, "cache_warm: 1.10% of the world pages resident")
        self.assertEqual(self.row(result, "V10")["evidence"], fv.EVIDENCE_PRESENT)

    def test_v10_too_few_bytes_read(self) -> None:
        self.set_cold(read_fraction=0.9)
        self.assert_entry("V10", fv.PASS)
        self.set_cold(read_fraction=0.89)
        result = self.assert_entry("V10", fv.INVALID, "cache_warm: read")
        self.assertEqual(result["verdict"], fv.INVALID, "a validity condition, never a FAIL")

    def test_v10_judges_with_the_frozen_values_not_the_records_status(self) -> None:
        cache = self.set_cold(read_fraction=0.5)
        cache["v10"]["status"], cache["cold_proven"] = "OK", True
        self.assert_entry("V10", fv.INVALID, "cache_warm")
        self.th["validity"]["V10"]["tiers"]["A"] = 0.4
        self.assert_entry("V10", fv.PASS)
        self.th["validity"]["V10"]["residency_max_fraction"] = 0.0
        self.set_cold(resident=(1, 1000))
        self.assert_entry("V10", fv.INVALID, "cache_warm")

    def test_v10_without_a_measurement_is_missing(self) -> None:
        mutations = {
            "no record": lambda c: self.inputs["steps"]["probe"]["data"].pop("cache"),
            "no oracle": lambda c: c["v10"].update(expected_unique_stored_bytes=None),
            "no read bytes": lambda c: c["v10"].update(read_bytes=None),
            "no residency": lambda c: c.pop("residency"),
            "no label": lambda c: c.pop("host_cache"),
        }
        for name, mutate in mutations.items():
            mutate(self.set_cold())
            row = self.row(self.run_verdict(), "V10")
            self.assertEqual((row["status"], row["evidence"]), (fv.INVALID, fv.EVIDENCE_MISSING), name)
            self.assertTrue(row["reason"].startswith("MISSING"), (name, row["reason"]))

    def test_v10_a_partial_oracle_is_never_summed(self) -> None:
        del self.inputs["summary"]["worlds"]["T0"]["flythrough"]["expected_unique_upload_flat"]["stored_bytes"]
        cache = self.set_cold()
        self.assertEqual(cache["v10"]["status"], "NOT_EVALUATED")
        self.assert_entry("V10", fv.INVALID, "MISSING: read_bytes unmeasured")

    def test_v10_frozen_values_equal_the_lane_constants(self) -> None:
        """The lane's own refusal (dispatch_lane probe) and the verdict use one pair of limits."""
        entry = _THRESHOLDS["validity"]["V10"]
        self.assertEqual(entry["tiers"]["A"], wc.V10_MIN_READ_FRACTION)
        self.assertEqual(entry["residency_max_fraction"], wc.RESIDENCY_MAX_FRACTION)
        self.assertNotIn("V10", (_THRESHOLDS["provenance"].get("absent_until_prerequisite") or {}))


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

    def test_g10_drops_at_a_checkpoint_fail(self) -> None:
        self.passes()[1]["overlap_drops_checkpoints"] = drop_window(drops=2)
        result = self.assert_entry("G10", fv.FAIL, "overflow_at_checkpoint: 2 drop intervals")
        row = self.row(result, "G10")
        self.assertEqual((row["observed"], row["evidence"]), (2, fv.EVIDENCE_PRESENT))
        self.assertIn("binning budget 2", row["reason"])
        self.assertIn(">= 20 records dropped", row["reason"])

    def test_g10_zero_drops_need_a_completed_readback(self) -> None:
        """Absence is never a pass: a window without a readback, or without the columns, is MISSING."""
        self.assert_entry("G10", fv.PASS, "0 drop intervals")
        # Rows carry the counters, but no readback landed in the window.
        self.passes()[0]["overlap_drops_checkpoints"] = ps.overlap_drops(drop_trace([{}, {}]), [0, 1])
        self.assert_entry("G10", fv.INVALID, "zero drops would be unproven")
        self.setUp()
        no_columns = ps.Trace(["row"], "q", [(0,)], 0)
        self.passes()[0]["overlap_drops_checkpoints"] = ps.overlap_drops(no_columns, [0])
        self.assert_entry("G10", fv.INVALID, "older probe")
        self.setUp()
        self.passes()[0]["overlap_drops_checkpoints"] = ps.overlap_drops(drop_trace([None, None]), [0, 1])
        self.assert_entry("G10", fv.INVALID, "did not publish")

    def test_g10_a_counter_that_went_backwards_is_not_summed(self) -> None:
        trace = drop_trace([{"drop_signal_samples": 1, "overflow_drop_events": 3}, {"drop_signal_samples": 1}])
        rows = list(trace.rows)
        idx = trace.index["overflow_drop_events"]
        rows[1] = rows[1][:idx] + (0,) + rows[1][idx + 1:]  # the renderer was recreated
        trace = ps.Trace(trace.fields, trace.types, rows, 0)
        self.passes()[0]["overlap_drops_checkpoints"] = ps.overlap_drops(trace, [0, 1])
        self.assert_entry("G10", fv.INVALID, "went backwards")

    def _miss(self, checkpoint: str, deadline) -> None:
        for p in self.passes():
            for c in p["checkpoints"]:
                if c["checkpoint"] == checkpoint:
                    c["settled"] = False
                    c["settle_s"] = {"status": "MISSING", "value": None, "reason": fv.SETTLE_DEADLINE_MISSED}
                    if deadline is not None:
                        c["settle_deadline_s"] = deadline

    def test_g6_a_missed_deadline_is_a_measured_fail(self) -> None:
        """C-starve (ADR 8.5): never settled within a deadline >= the limit is FAIL settle_deadline_missed."""
        for p in self.passes():
            for c in p["checkpoints"]:
                c["settle_s"]["value"] = min(c["settle_s"]["value"], 2.9)
        self._miss("C2", 15.0)
        self.assert_entry("G6", fv.FAIL, "settle_deadline_missed: never settled at pass 0 C2 (deadline 15 s)")
        self.assert_entry("G6t", fv.PASS)

    def test_g6_a_deadline_inside_the_limit_or_unknown_proves_nothing(self) -> None:
        self._miss("C2", 2.0)
        self.assert_entry("G6", fv.INVALID, "inside the limit")
        self.setUp()
        self._miss("C2", None)
        self.assert_entry("G6", fv.INVALID, "settle_deadline_s")
        self.setUp()
        self._miss("C2", 15.0)
        self.passes()[0]["checkpoints"][2]["settle_s"]["reason"] = "pose never matched"
        self.assert_entry("G6", fv.INVALID, "MISSING")

    def test_gate_world_override_judges_that_world_alone(self) -> None:
        """BV-13a controls judge T0 against tier A's frozen values."""
        self.inputs["gate_world"] = "T0"
        del self.inputs["summary"]["worlds"]["A"]
        result = self.run_verdict()
        self.assertEqual(self.row(result, "V2")["status"], fv.PASS)
        self.assertIn("world_too_small", self.row(result, "V3")["reason"])
        self.assertIn("not_streaming: 0 evictions", self.row(result, "V9-evictions")["reason"])

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


class Reprime(_Case):
    """#1389: Tier A's committed digests are the GSQ world (ADR 8.4 re-prime path)."""

    def setUp(self) -> None:
        super().setUp()
        self.th = copy.deepcopy(_THRESHOLDS)  # the committed digests, not the flat M1 ones
        self.record = next(r for r in _THRESHOLDS["reprime_records"] if r["world"] == "A")

    def test_the_committed_digests_are_the_record(self) -> None:
        committed = _THRESHOLDS["globals"]["worlds"]["A"]
        self.assertEqual(self.record["encoding"], "plain_v3_gsq")
        for key in ("gen_sha", "file_sha256", "file_bytes"):
            self.assertEqual(committed[key], self.record[key], key)
        # `replaces` is the captured M1 world, so the flat run is still traceable.
        self.assertEqual(self.record["replaces"]["file_sha256"], self.a["world_file_sha256"])
        self.assertEqual(self.record["replaces"]["file_bytes"], self.a["file_bytes"])

    def test_the_flat_m1_world_is_invalid_against_the_gsq_digests(self) -> None:
        self.assert_entry("V2", fv.INVALID, "A.file_sha256")

    def test_a_world_with_the_recorded_gsq_digests_passes_v2(self) -> None:
        self.a["world_file_sha256"] = self.record["file_sha256"]
        self.a["file_bytes"] = self.record["file_bytes"]
        self.assert_entry("V2", fv.PASS, "every world matches")
        self.a["file_bytes"] += 1
        self.assert_entry("V2", fv.INVALID, "A.file_bytes")


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
        # The captured summary predates `settings_measured` (BV-13a), so V7 makes it INVALID.
        self.assertEqual(gates["verdict"], fv.INVALID)
        self.assertEqual(verdict["gate_verdict"]["verdict"], fv.INVALID)
        v7 = [r for r in gates["entries"] if r["id"] == "V7"][0]
        self.assertIn("settings_measured: absent", v7["reason"])
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
