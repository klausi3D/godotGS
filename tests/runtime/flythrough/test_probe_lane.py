#!/usr/bin/env python3
"""Unit tests for the BV-07 probe lane (run_probe.py, probe_path.py, probe_groundtruth.py,
probe_summary.py).

Producer fixtures are CAPTURED, never authored (tests/AGENTS.md):

* ``fixtures/bv07/tsmoke.manifest.json`` is the sidecar manifest BV-05's generator wrote
  for Tier T-smoke (``generate_world.py --tier T-smoke``).
* ``fixtures/bv07/tsmoke_path.*`` and ``tsmoke_sweep.*`` are the binary trace (gzip) and
  the result JSON probe_world.gd wrote when run_probe.py drove it on that world on an
  RTX 3090 (the path at a 15 fps cap so the fixture stays small; one sweep plan). How they
  were captured is in ``fixtures/bv07/README.md``.

Constructed inputs are used only where the construction is the point: malformed traces
(a torn tail, a bad magic, a skipped row index), the held-sample and settings-drift cases
(one field of a captured row changed, to show the check can fire), and the closed-form
statistics.

Run directly, through pytest, or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import gzip
import importlib.util
import json
import copy
import math
import os
import re
import subprocess
import time
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIX = HERE / "fixtures" / "bv07"
sys.path.insert(0, str(HERE))


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


pp = _load("probe_path")
gt = _load("probe_groundtruth")
ps = _load("probe_summary")
rp = _load("run_probe")


def manifest() -> dict:
    return json.loads((FIX / "tsmoke.manifest.json").read_text(encoding="utf-8"))


def captured(name: str, tmp: Path):
    """(trace path, result dict) of a captured run, the trace unpacked into tmp."""
    trace = tmp / f"{name}.trace.bin"
    trace.write_bytes(gzip.decompress((FIX / f"{name}.trace.bin.gz").read_bytes()))
    result = json.loads((FIX / f"{name}.result.json").read_text(encoding="utf-8"))
    return trace, result


def rewrite_rows(src: Path, dst: Path, edit) -> None:
    """Copy a captured trace, letting edit(row_index, values) change values of a row."""
    trace = ps.read_trace(src)
    data = src.read_bytes()
    (hlen,) = struct.unpack_from("<I", data, 8)
    head = data[: 12 + hlen]
    fmt = "<" + trace.types
    body = bytearray()
    for i, row in enumerate(trace.rows):
        values = list(row)
        edit(i, values, trace.index)
        body += struct.pack(fmt, *values)
    dst.write_bytes(head + bytes(body))


# --------------------------------------------------------------------------
# Path
# --------------------------------------------------------------------------


class Path_(unittest.TestCase):
    def setUp(self) -> None:
        self.m = manifest()
        self.path = pp.resolve_path(self.m)

    def test_segment_table_follows_the_adr(self) -> None:
        segs = self.path["segments"]
        ids = [s["id"] for s in segs]
        for sid in ("S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10", "S11", "S12"):
            self.assertIn(sid, ids)
        speeds = {s["id"]: s["speed_m_s"] for s in segs if s["kind"] == pp.KIND_MOVE}
        self.assertEqual((speeds["S1"], speeds["S2"], speeds["S4"], speeds["S5"], speeds["S7"], speeds["S8"],
                          speeds["S11"]), (6.0, 25.0, 60.0, 120.0, 60.0, 10.0, 15.0))
        self.assertEqual([c for c, _ in pp.checkpoints(self.path)], [f"C{i}" for i in range(8)])
        climb = next(s for s in segs if s["id"] == "S10")
        self.assertAlmostEqual(climb["length_m"] / climb["speed_m_s"], pp.CLIMB_SECONDS, places=3)
        self.assertAlmostEqual(climb["table"][-1][2], pp.CLIMB_HEIGHT_M, places=3)
        self.assertAlmostEqual(climb["table"][-1][5], pp.CLIMB_PITCH_DEG, places=3)

    def test_table_is_arc_length(self) -> None:
        """Consecutive rows are one table step apart along the curve, so s is path distance."""
        for seg in self.path["segments"]:
            if seg["kind"] != pp.KIND_MOVE:
                continue
            table = seg["table"]
            self.assertAlmostEqual(table[-1][0], seg["length_m"], places=3)
            step = seg["length_m"] / (len(table) - 1)
            self.assertLessEqual(step, pp.TABLE_STEP_M + 1e-9)
            chords = 0.0
            for a, b in zip(table, table[1:]):
                chord = math.dist(a[1:4], b[1:4])
                # A chord is never longer than the arc between its rows.
                self.assertLessEqual(chord, (b[0] - a[0]) + 2e-4, seg["id"])
                chords += chord
                # Yaw is unwrapped: interpolation never spins round.
                self.assertLess(abs(b[4] - a[4]), 30.0, seg["id"])
            # The polyline through the rows is the curve to within 1 %: s is distance travelled.
            self.assertGreater(chords, 0.99 * seg["length_m"], seg["id"])

    def test_town_segment_turns_the_camera_a_full_circle(self) -> None:
        """S8: 360 degrees net camera yaw at 24 deg/s, whatever the ring's own turning."""
        for m in (manifest(),):
            s8 = next(s for s in pp.resolve_path(m)["segments"] if s["id"] == "S8")
            net = s8["table"][-1][4] - s8["table"][0][4]
            self.assertAlmostEqual(net, 360.0, delta=0.5)
            self.assertAlmostEqual(s8["length_m"] / s8["speed_m_s"], 360.0 / pp.TOWN_YAW_RATE_DEG_S, delta=0.5)
            # Monotone: the camera never turns back while sweeping.
            for a, b in zip(s8["table"], s8["table"][1:]):
                self.assertGreaterEqual(b[4], a[4])

    def test_spline_length_is_exact_on_a_line(self) -> None:
        sp = pp.ArcLengthSpline([(0.0, 0.0, 0.0), (5.0, 0.0, 0.0), (10.0, 0.0, 0.0)])
        self.assertAlmostEqual(sp.length, 10.0, places=9)
        self.assertAlmostEqual(sp.point_at(2.5)[0], 2.5, places=9)

    def test_spline_never_backtracks_on_uneven_spacing(self) -> None:
        """The T0 S4 lead-in shape: a short climb, then a long run. A uniform Catmull-Rom
        overshoots backwards there (x first grows away from the target); the centripetal
        one does not."""
        sp = pp.ArcLengthSpline([(52.0, 1.7, 8.0), (36.3, 12.0, -5.4), (-232.7, 12.0, -236.0)])
        xs = [sp.point_at(sp.length * i / 400)[0] for i in range(401)]
        for a, b in zip(xs, xs[1:]):
            self.assertLessEqual(b, a + 1e-9)

    def test_interpolate_is_what_the_driver_does(self) -> None:
        seg = next(s for s in self.path["segments"] if s["id"] == "S4")
        mid = seg["length_m"] * 0.37
        p = pp.interpolate(seg["table"], mid)
        self.assertEqual(len(p), 5)
        self.assertEqual(pp.interpolate(seg["table"], -5.0), seg["table"][0][1:])
        self.assertEqual(pp.interpolate(seg["table"], 1e9), seg["table"][-1][1:])
        driver = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn("func _interp(table: Array, s: float) -> Array:", driver)
        self.assertNotIn("Curve3D", driver, "the driver must not evaluate its own spline")

    def test_path_sha_is_deterministic_and_bound_to_the_world(self) -> None:
        again = pp.resolve_path(manifest())
        self.assertEqual(again["path_sha256"], self.path["path_sha256"])
        self.assertEqual(pp.path_sha256(self.path), self.path["path_sha256"])
        moved = manifest()
        town = next(f for f in moved["features"] if f["class"] == "town")
        town["footprint"] = [v + 8 for v in town["footprint"]]
        self.assertNotEqual(pp.resolve_path(moved)["path_sha256"], self.path["path_sha256"])

    def test_missing_layout_is_a_typed_error(self) -> None:
        m = manifest()
        m["features"] = [f for f in m["features"] if f["class"] != "wall"]
        with self.assertRaises(pp.PathError):
            pp.resolve_path(m)


# --------------------------------------------------------------------------
# Ground truth
# --------------------------------------------------------------------------


CAM = {"fov_deg": 90.0, "keep": "width", "near_m": 0.1, "far_m": 100.0}


class GroundTruth(unittest.TestCase):
    def test_frustum_orientation_matches_godot(self) -> None:
        # Godot: rotation_degrees.y = -90 looks along +X; pitch -45 looks down.
        planes = gt.frustum_planes([0, 0, 0, -90.0, 0.0], CAM, 1.0)
        self.assertTrue(gt.point_inside(planes, (10, 0, 0)))
        self.assertFalse(gt.point_inside(planes, (-10, 0, 0)))
        self.assertTrue(gt.point_inside(planes, (10, 0, 9.9)))
        self.assertFalse(gt.point_inside(planes, (10, 0, 10.1)))
        self.assertFalse(gt.point_inside(planes, (101, 0, 0)), "beyond the far plane")
        down = gt.frustum_planes([0, 0, 0, 0.0, -45.0], CAM, 1.0)
        self.assertTrue(gt.point_inside(down, (0, -10, -10)))
        self.assertTrue(gt.point_inside(down, (0, -20, -1)))
        self.assertFalse(gt.point_inside(down, (0, 10, -10)))

    def test_checkpoint_truth_on_a_real_manifest(self) -> None:
        m = manifest()
        path = pp.resolve_path(m)
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        self.assertEqual(truth["generator_params_sha"], m["generator_params_sha"])
        self.assertEqual(truth["path_sha256"], path["path_sha256"])
        self.assertEqual(truth["oracle_cut"]["status"], "NOT_APPLICABLE")
        counts = {int(c["index"]): int(c["count"]) for c in m["chunks"]}
        for name, cp in truth["checkpoints"].items():
            self.assertEqual(cp["expected_drawn_upper_bound"], sum(counts[i] for i in cp["frustum_chunks"]), name)
            self.assertEqual(cp["tref_spec"]["within_tref_cap"], cp["tref_spec"]["splats"] <= gt.TREF_MAX_SPLATS)
        self.assertGreater(truth["checkpoints"]["C0"]["frustum_chunk_count"], 0)
        # C7 is a teleport back to P0: same pose, same truth.
        self.assertEqual(truth["checkpoints"]["C7"]["frustum_chunks"], truth["checkpoints"]["C0"]["frustum_chunks"])
        upload = truth["expected_unique_upload"]["flat"]
        self.assertLessEqual(upload["splats"], m["world"]["splat_count"])
        self.assertGreater(upload["stored_bytes"], 0)
        # Flat: the stored sections are the decoded bytes.
        self.assertEqual(upload["decoded_bytes"], upload["stored_bytes"])

    def test_gsq_chunks_count_their_page_run_and_their_decoded_bytes(self) -> None:
        # BV-C7: a GSQ chunk has no section byte counts; stored is its page run, the upload
        # oracle is the decoded size.
        chunk = {"count": 10, "pages_bytes": 700}
        self.assertEqual(gt.chunk_stored_bytes(chunk), 700)
        self.assertEqual(gt.chunk_decoded_bytes(chunk, 12), 10 * (144 + 12 * 12))
        flat = {"count": 10, "gaussian_bytes": 1440, "sh_bytes": 1440}
        self.assertEqual(gt.chunk_stored_bytes(flat), gt.chunk_decoded_bytes(flat, 12))

    def test_chunk_behind_the_camera_is_out(self) -> None:
        m = {"chunks": [
            {"index": 0, "plan_index": 0, "count": 5, "center": [10, 0, 0], "radius": 1.0, "aabb": [9, -1, -1, 11, 1, 1]},
            {"index": 1, "plan_index": 1, "count": 7, "center": [-10, 0, 0], "radius": 1.0, "aabb": [-11, -1, -1, -9, 1, 1]},
        ], "features": []}
        truth = gt.checkpoint_truth(m, [0, 0, 0, -90.0, 0.0], CAM, 1.0)
        self.assertEqual(truth["frustum_chunks"], [0])
        self.assertEqual(truth["expected_drawn_upper_bound"], 5)


# --------------------------------------------------------------------------
# Trace + summary (captured)
# --------------------------------------------------------------------------


def admissible_checkpoint_policy(result):
    """Construct only the newer admissibility inputs for legacy policy replays.

    The captured trace, counts, settle receipts and fixture files stay unchanged;
    these frame scalars are deliberately transplanted from run43717101341 chunk
    C2. This is test policy construction, not provenance for the historical run.
    """
    result = copy.deepcopy(result)
    result["route"] = "chunk"
    result["static_sort_cache"] = {"enabled": False, "applicable": True}
    for checkpoint in result["checkpoints"]:
        checkpoint.update(actual_route="chunk", pose_first_frame=6100,
                          rendered_splat_count_source_frame=6103, render_frame_counter=6105)
    return result


class CapturedTrace(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.path_trace, self.path_result = captured("tsmoke_path", self.tmp)
        self.sweep_trace, self.sweep_result = captured("tsmoke_sweep", self.tmp)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_of(self, trace: Path, result: dict) -> dict:
        return {"exit_code": 0, "timed_out": False, "result": result, "trace": str(trace)}

    def test_trace_parses_and_rows_equal_frames(self) -> None:
        tr, info = rp.trace_integrity(self.run_of(self.path_trace, self.path_result))
        self.assertIsNotNone(tr)
        self.assertEqual(info["status"], ps.OK, info)
        self.assertEqual(len(tr.rows), self.path_result["rows_written"])
        self.assertEqual(len(tr.rows), self.path_result["frames_rendered"])
        self.assertEqual(tr.fields[:3], ["row", "engine_frame", "t_usec"])
        self.assertEqual(info["engine_frame_gaps"], 0)

    def test_frames_rendered_mismatch_is_invalid(self) -> None:
        result = dict(self.path_result, frames_rendered=self.path_result["frames_rendered"] + 1)
        _, info = rp.trace_integrity(self.run_of(self.path_trace, result))
        self.assertEqual(info["status"], ps.INVALID)

    def test_torn_trace_is_invalid(self) -> None:
        torn = self.tmp / "torn.bin"
        torn.write_bytes(self.path_trace.read_bytes()[:-7])
        _, info = rp.trace_integrity(self.run_of(torn, self.path_result))
        self.assertEqual(info["status"], ps.INVALID)

    def test_skipped_engine_frame_is_invalid(self) -> None:
        gap = self.tmp / "gap.bin"
        rewrite_rows(self.path_trace, gap,
                     lambda i, v, ix: v.__setitem__(ix["engine_frame"], v[ix["engine_frame"]] + (1 if i >= 5 else 0)))
        _, info = rp.trace_integrity(self.run_of(gap, self.path_result))
        self.assertEqual(info["status"], ps.INVALID)
        self.assertEqual(info["engine_frame_gaps"], 1)

    def test_malformed_traces_are_refused(self) -> None:
        bad = self.tmp / "bad.bin"
        bad.write_bytes(b"NOTATRACE" + self.path_trace.read_bytes()[9:])
        with self.assertRaises(ps.TraceError):
            ps.read_trace(bad)
        renum = self.tmp / "renum.bin"
        rewrite_rows(self.path_trace, renum, lambda i, v, ix: v.__setitem__(ix["row"], i + (1 if i == 3 else 0)))
        with self.assertRaises(ps.TraceError):
            ps.read_trace(renum)

    def test_path_summary_from_a_real_run(self) -> None:
        m = manifest()
        path = pp.resolve_path(m)
        self.assertEqual([s["id"] for s in self.path_result["segments"]], [s["id"] for s in path["segments"]],
                         "the fixture was captured on this path; re-capture it when the path changes")
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        tr = ps.read_trace(self.path_trace)
        s = rp.summarise_path(tr, self.run_of(self.path_trace, self.path_result), path, truth, 1)
        p0 = s["passes"][0]
        self.assertGreater(p0["moving_frame_interval"]["frames"], 0)
        self.assertEqual(p0["moving_frame_interval"]["mean"]["status"], ps.OK)
        self.assertEqual(p0["settings"]["status"], ps.OK)
        self.assertFalse(p0["settings"]["settings_drifted"])
        self.assertEqual(p0["streaming"]["upload_bytes"]["status"], ps.OK)
        self.assertGreater(p0["streaming"]["upload_bytes"]["value"], 0)
        self.assertEqual(s["regulator"]["status"], ps.OK)
        self.assertEqual([c["checkpoint"] for c in p0["checkpoints"]], [f"C{i}" for i in range(8)])
        self.assertTrue(all(c["settled"] for c in p0["checkpoints"]))
        self.assertEqual(s["records_per_splat"]["status"], ps.MISSING)
        # The fixture predates the drop telemetry (#1393): every window is MISSING, never zero drops.
        for rec in (s["overlap_drops_whole_run"], p0["overlap_drops"], p0["overlap_drops_moving"],
                    p0["overlap_drops_checkpoints"]):
            self.assertEqual(rec["status"], ps.MISSING, rec)
            self.assertIn("older probe", rec["reason"])
        self.assertIn("older probe", s["records_per_splat"]["reason"])
        # The fixture predates BV-POOL: the absent placement is MISSING, never device-local.
        self.assertEqual(s["pool_placement"]["status"], ps.MISSING)
        self.assertIsNone(s["pool_placement"]["device_local"])
        # The raw capture predates count provenance: settled alone is no longer
        # admissible checkpoint evidence, although its other metrics still parse.
        self.assertEqual(s["checkpoint_evidence"]["verdict"], "UNUSABLE")
        self.assertTrue(any("checkpoint_unsettled" in problem for problem in
                            rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}})))

    def _with_coverage_columns(self, needed_not_resident) -> "ps.Trace":
        """The captured trace with the gate-repair columns appended: needed_not_resident(i, row)
        per row, no upload pending beyond it, no visible eviction. Every other value is real."""
        tr = ps.read_trace(self.path_trace)
        rows = []
        for i, r in enumerate(tr.rows):
            nnr = needed_not_resident(i, r)
            rows.append(tuple(r) + (nnr, min(nnr, 1), 0))
        return ps.Trace(tr.fields + list(ps.COVERAGE_COLUMNS), tr.types + "qqq", rows, 0)

    def test_the_captured_run_predates_the_coverage_columns(self) -> None:
        """An older driver's trace has no per-frame completeness: MISSING, never complete. It
        adds no coverage problems line: G16 is report-only. Its missing checkpoint
        provenance independently makes the historical evidence unusable."""
        m = manifest()
        path = pp.resolve_path(m)
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        s = rp.summarise_path(ps.read_trace(self.path_trace), self.run_of(self.path_trace, self.path_result),
                              path, truth, 1, m)
        mc = s["passes"][0]["moving_coverage"]
        self.assertEqual(mc["status"], ps.MISSING)
        self.assertIn("older probe driver", mc["reason"])
        self.assertEqual(s["screenshots"]["status"], ps.MISSING)
        self.assertIsNone(s["passes"][0]["checkpoints"][0]["needed_not_resident_chunks"])
        self.assertEqual(s["checkpoint_evidence"]["verdict"], "UNUSABLE")
        self.assertTrue(any("checkpoint_unsettled" in problem for problem in
                            rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}})))

    def test_moving_coverage_counts_every_frame_with_a_chunk_in_flight(self) -> None:
        m = manifest()
        tr = self._with_coverage_columns(lambda i, r: 2 if i % 7 == 0 else 0)
        moving = ps.rows_where(tr, phase=0, pass_index=0, state=ps.STATE_MOVING)
        self.assertGreater(len(moving), 20, "the fixture has moving rows")
        expected_at = lambda pose: gt.expected_drawn_upper_bound(m, pose, rp.CAMERA, rp.ASPECT)  # noqa: E731
        mc = ps.moving_coverage(tr, moving, expected_at, stride=1)
        self.assertEqual(mc["status"], ps.OK, mc)
        holes = sum(1 for i in moving if i % 7 == 0)
        self.assertEqual(mc["needed_not_resident"]["frames_gt0"], holes)
        self.assertEqual(mc["needed_not_resident"]["frames_with_signal"], len(moving))
        self.assertEqual(sum(v["needed_not_resident_frames"] for v in mc["per_segment"].values()), holes)
        # The drawn fraction is the row's real drawn count over the real manifest's expected count
        # at the row's real pose.
        d = mc["drawn_fraction"]
        self.assertEqual(d["status"], ps.OK, d)
        self.assertEqual(d["samples"] + d["rows_without_expected"], len(moving))
        row = tr.rows[moving[len(moving) // 2]]
        exp = expected_at([tr.get(row, c) for c in ps.POSE_COLUMNS])
        self.assertGreater(exp, 0)
        self.assertGreaterEqual(d["min"], 0.0)
        self.assertLessEqual(d["min"], tr.get(row, "drawn_splats") / exp)
        # A complete trace has no hole frame, and every sampled row is counted once per stride.
        clean = ps.moving_coverage(self._with_coverage_columns(lambda i, r: 0), moving, expected_at, stride=10)
        self.assertEqual(clean["needed_not_resident"]["frames_gt0"], 0)
        self.assertEqual(clean["drawn_fraction"]["samples"] + clean["drawn_fraction"]["rows_without_expected"],
                         len(moving[::10]))

    def test_a_moving_row_without_the_signal_is_missing(self) -> None:
        captured_moving = ps.rows_where(ps.read_trace(self.path_trace), phase=0, pass_index=0, state=ps.STATE_MOVING)
        gap = captured_moving[len(captured_moving) // 2]
        tr = self._with_coverage_columns(lambda i, r: -1 if i == gap else 0)
        moving = ps.rows_where(tr, phase=0, pass_index=0, state=ps.STATE_MOVING)
        self.assertEqual(moving, captured_moving)
        mc = ps.moving_coverage(tr, moving, lambda pose: 1)
        self.assertEqual(mc["status"], ps.MISSING)
        self.assertIn("needed_not_resident_chunks absent in 1 of", mc["reason"])

    def test_the_moving_expected_count_is_the_checkpoint_oracle(self) -> None:
        """One oracle: at a checkpoint pose the per-pose count equals the checkpoint's expected count."""
        m = manifest()
        path = pp.resolve_path(m)
        for name, pose in pp.checkpoints(path):
            self.assertEqual(gt.expected_drawn_upper_bound(m, pose, rp.CAMERA, rp.ASPECT),
                             gt.checkpoint_truth(m, pose, rp.CAMERA, rp.ASPECT)["expected_drawn_upper_bound"], name)

    def test_screenshot_record(self) -> None:
        ok = rp.screenshot_record({"dir": "/x/screenshots", "moving_every_n": 0,
                                   "taken": [{"name": "p0_C0", "file": "p0_C0.png"}]})
        self.assertEqual((ok["status"], ok["files"]), (ps.OK, ["p0_C0.png"]))
        bad = rp.screenshot_record({"dir": "/x", "moving_every_n": 0, "taken": [{"name": "p0_C1", "error": "no viewport image"}]})
        self.assertEqual(bad["status"], ps.INVALID)
        self.assertEqual(rp.screenshot_record({"dir": None, "taken": []})["status"], ps.MISSING)
        self.assertEqual(rp.screenshot_record(None)["status"], ps.MISSING)
        self.assertIn("perturb", rp.screenshot_record({"dir": "/x", "moving_every_n": 30, "taken": []})["note"])

    def test_pool_placement_reports_only_a_known_placement(self) -> None:
        full = {k: 0 for k in rp.POOL_PLACEMENT_KEYS}
        full.update(pool_placement_known=True, pool_device_local=1)
        self.assertEqual(rp.pool_placement(full)["status"], ps.OK)
        self.assertIs(rp.pool_placement(full)["device_local"], True)
        self.assertIs(rp.pool_placement(dict(full, pool_device_local=0))["device_local"], False)
        unknown = rp.pool_placement(dict(full, pool_placement_known=False, pool_device_local=-1))
        self.assertEqual((unknown["status"], unknown["device_local"]), (ps.MISSING, None))
        partial = dict(full)
        del partial["device_local_ext_usage_mb"]
        self.assertEqual(rp.pool_placement(partial)["status"], ps.MISSING)

    def test_measured_rows_are_moving_plus_holds_without_the_load_settle(self) -> None:
        """#1383 item 2: V7's measured rows include every checkpoint settle and hold, not the load."""
        tr = ps.read_trace(self.path_trace)
        seg, state = tr.index["segment"], tr.index["state"]
        measured = set(ps.measured_rows(tr, 0))
        pass_rows = ps.rows_where(tr, phase=0, pass_index=0)
        load = {i for i in pass_rows if tr.rows[i][seg] == 0 and tr.rows[i][state] == ps.STATE_SETTLING}
        self.assertTrue(load, "the fixture has a load settle")
        self.assertEqual(measured, set(pass_rows) - load)
        self.assertTrue(set(ps.rows_where(tr, phase=0, pass_index=0, state=ps.STATE_MOVING)) < measured)
        holds = {i for i in measured if tr.rows[i][state] in (ps.STATE_SETTLING, ps.STATE_HOLD)}
        self.assertTrue(holds, "the later checkpoints' settles and holds are measured")
        m = manifest()
        path = pp.resolve_path(m)
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        s = rp.summarise_path(tr, self.run_of(self.path_trace, self.path_result), path, truth, 1)
        self.assertEqual(s["passes"][0]["settings_measured"]["rows"], len(measured))
        self.assertEqual({c["settle_deadline_s"] for c in s["passes"][0]["checkpoints"]} - {None},
                         {c["settle"]["deadline_s"] for c in self.path_result["checkpoints"]} - {None})

    def test_counter_totals_keep_activity_before_the_first_sample(self) -> None:
        """Whole run: last value minus the initial 0. A later window: minus the last sample
        before it. Never minus the window's own first sample."""
        tr = ps.read_trace(self.path_trace)
        rows = list(range(len(tr.rows)))
        reads = [v for v in tr.col("read_bytes_total") if v >= 0]
        self.assertGreater(reads[0], 0, "the fixture has activity before its first sample")
        whole = ps.streaming_counters(tr, rows)["read_bytes"]
        self.assertEqual(whole["value"], reads[-1] - ps.COUNTER_INITIAL_VALUE)
        self.assertEqual(whole["baseline"], "initial_zero")
        cut = len(rows) // 2
        head = ps.streaming_counters(tr, rows[:cut])["read_bytes"]["value"]
        tail = ps.streaming_counters(tr, rows[cut:])
        self.assertEqual(tail["read_bytes"]["baseline"], "preceding_sample")
        self.assertEqual(head + tail["read_bytes"]["value"], whole["value"], "windows add up to the whole run")

    def test_missed_checkpoint_deadlines_make_the_run_invalid(self) -> None:
        """Replay of the captured run with every checkpoint marked settle_deadline_missed."""
        result = copy.deepcopy(self.path_result)
        for cp in result["checkpoints"]:
            cp["settle"] = {"settled": False, "settle_usec": -1, "frames": 1, "deadline_s": 15.0,
                            "reason": "settle_deadline_missed"}
        m = manifest()
        path = pp.resolve_path(m)
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        s = rp.summarise_path(ps.read_trace(self.path_trace), self.run_of(self.path_trace, result), path, truth, 1)
        problems = rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}})
        self.assertEqual(len([p for p in problems if "settle_deadline_missed" in p]), 8, problems)
        # A dropped checkpoint is a problem too.
        s["passes"][0]["checkpoints"] = s["passes"][0]["checkpoints"][:-1]
        problems = rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}})
        self.assertTrue(any("expected" in p for p in problems), problems)

    def _replay_checkpoints(self, edit) -> tuple:
        """Replay the captured run with edit(cp, cap) applied to each checkpoint record."""
        # Supply legal new preconditions before the negative edit, so missing
        # needed/cap fields still test their original property independently.
        result = admissible_checkpoint_policy(self.path_result)
        tr = ps.read_trace(self.path_trace)
        for cp in result["checkpoints"]:
            edit(cp, tr.get(tr.rows[cp["row"]], "effective_max_chunks"))
        m = manifest()
        path = pp.resolve_path(m)
        truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
        s = rp.summarise_path(tr, self.run_of(self.path_trace, result), path, truth, 1)
        return s, rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}})

    def test_needed_within_cap_and_settled_is_ok(self) -> None:
        """ADR M1 settle-where-fits: needed <= cap (boundary needed == cap) and settled passes."""
        s, problems = self._replay_checkpoints(lambda cp, cap: cp.__setitem__("needed_chunks", cap))
        self.assertEqual(problems, [])
        fits = [c["cap_fit"] for c in s["passes"][0]["checkpoints"]]
        self.assertEqual(len(fits), 8)
        self.assertTrue(all(f["status"] == ps.OK and f["settle_required"] for f in fits), fits)
        self.assertTrue(all(f["needed_chunks"] == f["chunk_cap"] > 0 for f in fits), fits)

    def test_needed_over_cap_is_invalid_even_when_settled(self) -> None:
        """needed > cap needs a priority proof that does not exist yet: INVALID, never a silent pass."""
        def over(cp, cap):
            if cp["checkpoint"] == "C3":
                cp["needed_chunks"] = cap + 1
        s, problems = self._replay_checkpoints(over)
        c3 = s["passes"][0]["checkpoints"][3]
        self.assertTrue(c3["settled"], "the replayed checkpoint settled; the cap rule alone must fire")
        self.assertEqual(c3["cap_fit"]["status"], ps.INVALID)
        cap = c3["cap_fit"]["chunk_cap"]
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(f"C3: needed {cap + 1} > cap {cap}", problems[0])
        self.assertIn("priority proof", problems[0])

    def test_absent_needed_or_cap_is_not_a_pass(self) -> None:
        s, problems = self._replay_checkpoints(lambda cp, cap: cp.pop("needed_chunks"))
        self.assertEqual(len(problems), 8, problems)
        self.assertTrue(all("needed_chunks absent" in p for p in problems), problems)
        self.assertEqual(ps.checkpoint_cap_fit(10, -1)["status"], ps.MISSING)
        self.assertEqual(ps.checkpoint_cap_fit(10, None)["status"], ps.MISSING)
        self.assertEqual(ps.checkpoint_cap_fit(449, 480)["status"], ps.OK)
        self.assertEqual(ps.checkpoint_cap_fit(481, 480)["status"], ps.INVALID)

    def test_tuned_cap_fits_the_largest_m1_needed_set(self) -> None:
        """The tuned cap admits the largest needed set M1 tier A met (449, run 42427296940)."""
        key = "rendering/gaussian_splatting/streaming/max_chunks_in_vram"
        self.assertGreaterEqual(rp.TUNED_SETTINGS[key], 449)

    def test_unsettled_sweep_view_is_not_fk_evidence(self) -> None:
        result = copy.deepcopy(self.sweep_result)
        result["sweep"][0]["settle"] = {"settled": False, "settle_usec": -1, "reason": "settle_deadline_missed"}
        tr = ps.read_trace(self.sweep_trace)
        views = rp.summarise_sweep_point(tr, self.run_of(self.sweep_trace, result), "x", 1)
        bad = views[result["sweep"][0]["view"]]
        self.assertFalse(bad["valid_for_fit"])
        fit = ps.fit_frame_model([dict(bad, id="x"), dict(bad, id="y", drawn_mean=2 * bad["drawn_mean"], valid_for_fit=True),
                                  dict(bad, id="z", drawn_mean=3 * bad["drawn_mean"], valid_for_fit=True)],
                                 "gpu_pipeline_mean_ms")
        self.assertEqual(fit["points_used"], ["y", "z"])
        self.assertEqual(fit["excluded"][0]["point"], "x")

    def test_settled_checkpoint_was_observed_at_its_pose(self) -> None:
        """The driver settles only on stats that describe the pose (pose error <= 1 mm)."""
        for cp in self.path_result["checkpoints"]:
            self.assertTrue(cp["settle"]["settled"], cp["checkpoint"])
            self.assertLessEqual(cp["settle"]["pose_error_m"], 0.001, cp["checkpoint"])

    def test_freshness_counts_only_refreshed_rows(self) -> None:
        tr = ps.read_trace(self.sweep_trace)
        rows = list(range(len(tr.rows)))
        fresh, counts = ps.fresh_gpu_rows(tr, rows)
        self.assertGreater(counts["fresh"], 0)
        res = tr.col("coherent_resolve_usec")
        for i in fresh:
            self.assertGreater(res[i], res[i - 1])
        # Hold the coherent sample: every row repeats row 10's resolve tick and serial.
        held = self.tmp / "held.bin"
        r10 = tr.rows[10]

        def hold(i, v, ix):
            if i > 10:
                v[ix["coherent_resolve_usec"]] = r10[ix["coherent_resolve_usec"]]
                v[ix["coherent_frame_serial"]] = r10[ix["coherent_frame_serial"]]

        rewrite_rows(self.sweep_trace, held, hold)
        th = ps.read_trace(held)
        fresh_h, counts_h = ps.fresh_gpu_rows(th, list(range(11, len(th.rows))))
        self.assertEqual(fresh_h, [])
        self.assertEqual(counts_h["held"], len(th.rows) - 11)
        self.assertEqual(ps.gpu_breakdown(th, list(range(11, len(th.rows))))["coherent_total_ms"]["status"], ps.MISSING)

    def test_incomplete_bv04b_mask_is_not_a_measurement(self) -> None:
        part = self.tmp / "part.bin"
        rewrite_rows(self.sweep_trace, part,
                     lambda i, v, ix: v.__setitem__(ix["coherent_pass_mask"], v[ix["coherent_pass_mask"]] & ~(1 << 9)))
        tr = ps.read_trace(part)
        fresh, counts = ps.fresh_gpu_rows(tr, list(range(len(tr.rows))))
        self.assertEqual(fresh, [])
        self.assertGreater(counts["incomplete_mask"], 0)

    def test_settings_drift_fires_on_a_revision_change(self) -> None:
        tr = ps.read_trace(self.sweep_trace)
        # The measured rows (ADR V7 "in every measured frame"); the load frames before
        # them legitimately raise the revision from 0 to 1.
        rows = ps.rows_where(tr, state=ps.STATE_MEASURE)
        self.assertTrue(rows)
        base = ps.settings_drift(tr, rows)
        self.assertEqual(base["status"], ps.OK)
        self.assertFalse(base["settings_drifted"])
        drift = self.tmp / "drift.bin"
        last = len(tr.rows) - 1
        rewrite_rows(self.sweep_trace, drift,
                     lambda i, v, ix: v.__setitem__(ix["effective_settings_revision"],
                                                    v[ix["effective_settings_revision"]] + (1 if i == last else 0)))
        d = ps.settings_drift(ps.read_trace(drift), rows)
        self.assertTrue(d["settings_drifted"])
        self.assertEqual(d["label"], "settings_drifted")
        # ADR V7 / C-settings: drift in a measured frame is INVALID, never a labelled OK.
        self.assertEqual(d["status"], ps.INVALID)
        self.assertIn("V7 INVALID settings_drifted", d["reason"])
        absent = self.tmp / "absent.bin"
        rewrite_rows(self.sweep_trace, absent,
                     lambda i, v, ix: v.__setitem__(ix["effective_settings_revision"], -1))
        self.assertEqual(ps.settings_drift(ps.read_trace(absent), rows)["status"], ps.MISSING)

    def test_v7_cap_change_without_a_revision_change_is_invalid(self) -> None:
        """V7 names the chunk cap itself: a cap that moves while the revision holds still drifted."""
        tr = ps.read_trace(self.sweep_trace)
        rows = ps.rows_where(tr, state=ps.STATE_MEASURE)
        last = rows[-1]
        cap = self.tmp / "cap.bin"
        rewrite_rows(self.sweep_trace, cap,
                     lambda i, v, ix: v.__setitem__(ix["effective_max_chunks"],
                                                    v[ix["effective_max_chunks"]] - (1 if i == last else 0)))
        d = ps.settings_drift(ps.read_trace(cap), rows)
        self.assertEqual(d["revision_changes"], 0)
        self.assertTrue(d["settings_drifted"])
        self.assertEqual(d["status"], ps.INVALID)
        self.assertIn("chunk cap", d["reason"])

    def test_v7_measured_row_without_signal_is_no_plain_ok(self) -> None:
        """#1356: a measured row V7 could not see is INVALID even when the others hold still."""
        tr = ps.read_trace(self.sweep_trace)
        rows = ps.rows_where(tr, state=ps.STATE_MEASURE)
        blind_row = rows[len(rows) // 2]
        blind = self.tmp / "blind.bin"
        rewrite_rows(self.sweep_trace, blind,
                     lambda i, v, ix: v.__setitem__(ix["effective_settings_revision"],
                                                    -1 if i == blind_row else v[ix["effective_settings_revision"]]))
        d = ps.settings_drift(ps.read_trace(blind), rows)
        self.assertFalse(d["settings_drifted"])
        self.assertEqual(d["rows_without_signal"], 1)
        self.assertEqual(d["status"], ps.INVALID)
        self.assertIn("V7 INVALID rows_without_signal", d["reason"])

    def test_sweep_point_summary_from_a_real_run(self) -> None:
        tr = ps.read_trace(self.sweep_trace)
        views = rp.summarise_sweep_point(tr, self.run_of(self.sweep_trace, self.sweep_result), "T-smoke", 1)
        self.assertEqual(set(views), {v["view"] for v in self.sweep_result["sweep"]})
        for v in views.values():
            self.assertTrue(v["settled"])
            self.assertGreater(v["drawn_mean"], 0)
            self.assertEqual(v["gpu_pipeline_mean_ms"]["status"], ps.OK)
            self.assertEqual(v["wall_interval_mean_ms"]["status"], ps.OK)
            self.assertEqual(v["gpu"]["passes"]["composite"]["mean"]["status"], ps.OK)
            self.assertEqual(v["overlap_drops"]["status"], ps.MISSING)  # predates #1393
            self.assertEqual(v["overlap_drops_whole_run"]["status"], ps.MISSING)


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------


def _drop_trace(increments):
    """Constructed drop-counter rows (the arithmetic is the point): each entry is that row's
    counter increments, None a row without the telemetry (the driver's -1)."""
    fields = ["row", "pass_index", "segment", "view", "state", "s_m", *ps.DROP_COUNTERS]
    totals = dict.fromkeys(ps.DROP_COUNTERS, 0)
    rows = []
    for i, inc in enumerate(increments):
        if inc is None:
            values = [-1] * len(ps.DROP_COUNTERS)
        else:
            for k, v in inc.items():
                totals[k] += v
            values = [totals[k] for k in ps.DROP_COUNTERS]
        rows.append((i, 0, 3, -1, ps.STATE_MOVING, 1.5 * i, *values))
    return ps.Trace(fields, "qqqqqd" + "q" * len(ps.DROP_COUNTERS), rows, 0)


class OverlapDrops(unittest.TestCase):
    """#1393: dropped overlap records and records per splat from the renderer's cumulative counters."""

    ROWS = [
        None,
        {"drop_signal_samples": 1, "sampled_overlap_records_total": 1000, "sampled_overlap_splats_total": 100},
        {"drop_signal_samples": 1, "overflow_drop_events": 1, "binning_drop_events": 1,
         "binning_tile_range_drop_events": 1, "sampled_dropped_records_total": 7,
         "sampled_overlap_records_total": 1200, "sampled_overlap_splats_total": 100},
        None,
        {"drop_signal_samples": 1, "sampled_overlap_records_total": 800, "sampled_overlap_splats_total": 100},
    ]

    def test_each_row_is_credited_with_its_increment(self) -> None:
        tr = _drop_trace(self.ROWS)
        rec = ps.overlap_drops(tr, list(range(5)))
        self.assertEqual(rec["status"], ps.OK, rec)
        self.assertEqual((rec["readback_samples"], rec["drop_events"], rec["binning_drop_events"]), (3, 1, 1))
        self.assertEqual((rec["binning_tile_range_drop_events"], rec["binning_budget_drop_events"]), (1, 0))
        self.assertEqual((rec["sampled_dropped_records"], rec["sampled_overlap_records"], rec["sampled_splats"]),
                         (7, 3000, 300))
        self.assertEqual(rec["records_per_splat"]["value"], 10.0)
        self.assertAlmostEqual(rec["sampled_dropped_fraction"]["value"], 7 / 3000)
        self.assertEqual((rec["rows_with_drop"], rec["first_drop_row"]), (1, 2))
        self.assertEqual(rec["first_drop"]["segment"], 3)
        # A later window starts after the drop: the cumulative value is not re-counted.
        later = ps.overlap_drops(tr, [3, 4])
        self.assertEqual((later["status"], later["drop_events"], later["readback_samples"]), (ps.OK, 0, 1))
        self.assertEqual(later["records_per_splat"]["value"], 8.0)

    def test_absent_signal_is_missing_never_zero(self) -> None:
        tr = _drop_trace(self.ROWS)
        self.assertIn("did not publish", ps.overlap_drops(tr, [0, 3])["reason"])
        quiet = _drop_trace([{"drop_signal_samples": 1}, {}, {}])
        rec = ps.overlap_drops(quiet, [1, 2])
        self.assertEqual(rec["status"], ps.MISSING)
        self.assertIn("unproven", rec["reason"])
        self.assertEqual(ps.overlap_drops(tr, [])["status"], ps.MISSING)
        no_cols = ps.Trace(["row"], "q", [(0,)], 0)
        self.assertIn("older probe", ps.overlap_drops(no_cols, [0])["reason"])

    def test_a_counter_that_went_backwards_is_invalid(self) -> None:
        tr = _drop_trace(self.ROWS)
        idx = tr.index["sampled_overlap_records_total"]
        rows = list(tr.rows)
        rows[4] = rows[4][:idx] + (5,) + rows[4][idx + 1:]
        rec = ps.overlap_drops(ps.Trace(tr.fields, tr.types, rows, 0), list(range(5)))
        self.assertEqual(rec["status"], ps.INVALID)
        self.assertIn("went backwards at row 4", rec["reason"])

    def test_the_driver_and_the_renderer_publish_every_counter(self) -> None:
        """Wiring: probe_world.gd traces exactly DROP_COUNTERS, and the renderer writes each one
        into get_render_stats() (deleting a key or the call turns this red)."""
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        block = gd[gd.index("const DROP_FIELDS"):]
        block = block[:block.index("\n]\n")]
        self.assertEqual(tuple(re.findall(r'\["([a-z_]+)", "q"\]', block)), ps.DROP_COUNTERS)
        self.assertIn("] + DROP_FIELDS", gd)
        self.assertRegex(gd, r"for f in DROP_FIELDS:\n\t\tvalues\.append\(int\(_g\(stats, f\[0\], -1\)\)\)")
        renderer = ROOT / "modules" / "gaussian_splatting" / "renderer"
        src = (renderer / "render_debug_state_orchestrator.cpp").read_text(encoding="utf-8")
        body = src[src.index("void RenderDebugStateOrchestrator::append_drop_telemetry"):]
        body = body[:body.index("\n}\n")]
        for name in ps.DROP_COUNTERS:
            self.assertIn(f'r_stats["{name}"]', body, name)
        stats = (renderer / "render_diagnostics_orchestrator.cpp").read_text(encoding="utf-8")
        self.assertIn("debug_state_orchestrator->append_drop_telemetry(stats);", stats)


def _checkpoint_trace(rows):
    """Constructed rows of one pass (the arithmetic is the point): each entry is (row state,
    counter increments or None for a row without the telemetry)."""
    fields = ["row", "phase", "pass_index", "segment", "view", "state", "s_m", *ps.DROP_COUNTERS]
    totals = dict.fromkeys(ps.DROP_COUNTERS, 0)
    out = []
    for i, (state, inc) in enumerate(rows):
        if inc is None:
            values = [-1] * len(ps.DROP_COUNTERS)
        else:
            for k, v in inc.items():
                totals[k] += v
            values = [totals[k] for k in ps.DROP_COUNTERS]
        out.append((i, 0, 0, 3, -1, state, 1.5 * i, *values))
    return ps.Trace(fields, "qqqqqqd" + "q" * len(ps.DROP_COUNTERS), out, 0)


class CheckpointDropSplit(unittest.TestCase):
    """#1457: the checkpoint drop window split into settle window and settled pose (report-only)."""

    SAMPLE = {"drop_signal_samples": 1, "sampled_overlap_records_total": 1000, "sampled_overlap_splats_total": 100}
    ROWS = [
        (ps.STATE_MOVING, dict(SAMPLE, overflow_drop_events=1, raster_tile_cap_drop_events=1,
                               sampled_dropped_records_total=11)),
        (ps.STATE_SETTLING, dict(SAMPLE, overflow_drop_events=1, binning_drop_events=1,
                                 binning_budget_drop_events=1, sampled_dropped_records_total=245)),
        (ps.STATE_SETTLING, dict(SAMPLE)),
        (ps.STATE_HOLD, dict(SAMPLE)),
        (ps.STATE_HOLD, dict(SAMPLE, overflow_drop_events=1, raster_tile_cap_drop_events=1,
                             sampled_dropped_records_total=5)),
    ]

    def test_the_two_parts_are_exactly_the_checkpoint_window(self) -> None:
        trace = _checkpoint_trace(self.ROWS)
        windows = ps.checkpoint_drop_windows(trace, 0)
        self.assertEqual(set(windows), {key for _label, key, _state in ps.CHECKPOINT_DROP_WINDOWS})
        settle = windows["overlap_drops_checkpoints_settle_window"]
        settled = windows["overlap_drops_checkpoints_settled_pose"]
        self.assertEqual((settle["status"], settle["rows"], settle["drop_events"], settle["sampled_dropped_records"],
                          settle["binning_budget_drop_events"]), (ps.OK, 2, 1, 245, 1))
        self.assertEqual((settled["status"], settled["rows"], settled["drop_events"], settled["sampled_dropped_records"],
                          settled["raster_tile_cap_drop_events"]), (ps.OK, 2, 1, 5, 1))
        # The moving row's drop belongs to neither part, and the parts add up to the window G10 reads.
        union = ps.overlap_drops(trace, ps.rows_where(trace, phase=0, pass_index=0,
                                                      state={ps.STATE_SETTLING, ps.STATE_HOLD}))
        for key in ("rows", "readback_samples", "drop_events", "sampled_dropped_records"):
            self.assertEqual(settle[key] + settled[key], union[key], key)

    def test_the_report_keeps_both_parts_and_never_turns_absence_into_zero(self) -> None:
        trace = _checkpoint_trace(self.ROWS)
        record = {"pass_index": 0, **ps.checkpoint_drop_windows(trace, 0)}
        split = ps.checkpoint_drop_split([record])
        self.assertEqual(split["status"], ps.OK)
        entry = split["passes"][0]
        self.assertEqual((entry["settle_window"]["drop_events"], entry["settled_pose"]["drop_events"]), (1, 1))
        self.assertEqual((entry["settle_window"]["sampled_dropped_records"],
                          entry["settled_pose"]["sampled_dropped_records"]), (245, 5))
        self.assertIn("both stay failures", split["scope"])
        # No readback landed in the hold rows: that part is not measured, with the producer's reason.
        quiet = _checkpoint_trace([(ps.STATE_SETTLING, dict(self.SAMPLE)), (ps.STATE_HOLD, {}), (ps.STATE_HOLD, {})])
        entry = ps.checkpoint_drop_split([{"pass_index": 0, **ps.checkpoint_drop_windows(quiet, 0)}])["passes"][0]
        self.assertEqual(entry["settle_window"]["status"], ps.OK)
        self.assertEqual(entry["settled_pose"]["status"], ps.NOT_MEASURED)
        self.assertIn("unproven", entry["settled_pose"]["reason"])
        self.assertNotIn("drop_events", entry["settled_pose"])
        # A summary that predates the split has neither part.
        entry = ps.checkpoint_drop_split([{"pass_index": 1}])["passes"][0]
        self.assertEqual((entry["settle_window"]["status"], entry["settled_pose"]["status"]),
                         (ps.NOT_MEASURED, ps.NOT_MEASURED))
        self.assertEqual(ps.checkpoint_drop_split(None)["status"], ps.MISSING)

    def test_the_path_summary_publishes_the_split(self) -> None:
        """Wiring: run_probe puts both windows into every pass record."""
        src = (HERE / "run_probe.py").read_text(encoding="utf-8")
        body = src[src.index("def summarise_path"):]
        body = body[:body.index("\ndef ")]
        self.assertIn("**ps.checkpoint_drop_windows(trace, pass_index),", body)


def _drop_frame(**overrides):
    """A CONSTRUCTED drop frame for the arithmetic and malformed-input cases below (the numbers are
    the point). The producer's real format is covered by the captured run in
    DropFrames.test_the_captured_runner_export_parses_with_its_real_numbers."""
    frame = {"frame_serial": 900, "dropped_records": 3000, "raster_cap_dropped_records": 3000,
             "raster_truncated_tiles": 2, "overlap_records_attempted": 250000, "raster_tile_cap": 16384,
             "max_tile_records": 18000, "max_tile_x": 47, "max_tile_y": 33, "overlap_allocation_records": 400000}
    frame.update(overrides)
    return frame


def _drop_event(count, frame, **observed):
    event = {"count": count, "observed_row": 10 * count, "observed_phase": 0, "observed_pass_index": 0,
             "observed_segment": 4, "observed_state": ps.STATE_MOVING, "frame": frame}
    event.update(observed)
    return event


class DropFrames(unittest.TestCase):
    """#1457/#1461: the sampled dropping frames (per-tile record maximum, tile, allocation)."""

    def test_the_captured_runner_export_parses_with_its_real_numbers(self) -> None:
        """The producer's format, captured: the result.json blocks probe_world.gd wrote from the
        renderer's drop_frame export on the RTX 3080 runner (fixtures/d3_6/README.md, run
        43717101341). Not authored and not edited."""
        capture = json.loads((HERE / "fixtures" / "d3_6" / "run43717101341_drop_frames.json").read_text(encoding="utf-8"))
        hlod = ps.drop_frame_summary(capture["arms"]["hlod"]["overlap_drop_frames"])
        self.assertEqual(hlod["status"], ps.OK, hlod)
        self.assertEqual((hlod["drop_frames"], hlod["drop_frames_described"], hlod["drop_frames_not_described"]), (5, 5, 0))
        self.assertEqual((hlod["raster_cap_frames"], hlod["raster_cap_dropped_records"], hlod["binning_frames"]), (5, 6637, 0))
        self.assertEqual(hlod["raster_cap_max_tile_records_peak"]["value"], 18311)
        self.assertEqual([f["max_tile_records"] for f in hlod["frames"]], [17188, 18182, 18311, 16901, 17220])
        self.assertEqual([(f["max_tile_x"], f["max_tile_y"]) for f in hlod["frames"]],
                         [(23, 35), (22, 36), (23, 35), (22, 35), (22, 36)])
        for frame in hlod["frames"]:
            # Every captured value is a measured integer; none fell back to not_measured.
            self.assertTrue(all(type(frame[key]) is int for key in ps._DROP_FRAME_INT_KEYS + ps._DROP_FRAME_OPTIONAL_KEYS), frame)
            self.assertEqual((frame["raster_tile_cap"], frame["observed_state"]), (16384, ps.STATE_MOVING))
            self.assertGreater(frame["max_tile_records"], frame["raster_tile_cap"])
        chunk = ps.drop_frame_summary(capture["arms"]["chunk"]["overlap_drop_frames"])
        self.assertEqual((chunk["status"], chunk["drop_frames"], chunk["frames"]), (ps.OK, 0, []))
        self.assertEqual(chunk["max_tile_records_peak"]["status"], ps.MISSING)

    def test_raster_cap_and_budget_frames_are_reported_with_their_numbers(self) -> None:
        budget = _drop_frame(frame_serial=1500, dropped_records=245, raster_cap_dropped_records=0,
                             raster_truncated_tiles=0, overlap_records_attempted=205000,
                             overlap_allocation_records=160000, max_tile_records=700,
                             max_tile_x=ps.NOT_MEASURED, max_tile_y=ps.NOT_MEASURED)
        rec = ps.drop_frame_summary({"count": 3, "events_not_recorded": 1, "events": [
            _drop_event(1, _drop_frame()), _drop_event(3, budget, observed_state=ps.STATE_SETTLING)]})
        self.assertEqual(rec["status"], ps.OK, rec)
        self.assertEqual((rec["drop_frames"], rec["drop_frames_described"], rec["drop_frames_not_described"]), (3, 2, 1))
        self.assertEqual((rec["raster_cap_frames"], rec["raster_cap_dropped_records"]), (1, 3000))
        self.assertEqual(rec["raster_cap_max_tile_records_peak"]["value"], 18000)
        self.assertEqual(rec["max_tile_records_peak"]["value"], 18000)
        self.assertEqual((rec["binning_frames"], rec["binning_dropped_records"]), (1, 245))
        self.assertEqual(rec["binning_overlap_records_over_allocation_peak"]["value"], 45000)
        cap, cut = rec["frames"]
        self.assertEqual((cap["max_tile_records"], cap["max_tile_x"], cap["max_tile_y"], cap["max_tile_records_over_cap"]),
                         (18000, 47, 33, 1616))
        self.assertEqual((cut["max_tile_x"], cut["max_tile_y"]), (ps.NOT_MEASURED, ps.NOT_MEASURED))
        self.assertEqual((cut["overlap_records_over_allocation"], cut["observed_state"]), (45000, ps.STATE_SETTLING))

    def test_not_measured_values_stay_not_measured(self) -> None:
        frame = _drop_frame(max_tile_records=ps.NOT_MEASURED, max_tile_x=ps.NOT_MEASURED, max_tile_y=ps.NOT_MEASURED,
                            overlap_allocation_records=ps.NOT_MEASURED)
        rec = ps.drop_frame_summary({"count": 1, "events_not_recorded": 0, "events": [_drop_event(1, frame)]})
        self.assertEqual(rec["status"], ps.OK, rec)
        only = rec["frames"][0]
        for key in ("max_tile_records", "max_tile_records_over_cap", "overlap_allocation_records",
                    "overlap_records_over_allocation"):
            self.assertEqual(only[key], ps.NOT_MEASURED, key)
        for key in ("max_tile_records_peak", "raster_cap_max_tile_records_peak"):
            self.assertEqual((rec[key]["status"], rec[key]["value"]), (ps.MISSING, None), key)
        # No dropping frame at all: a count of 0 frames, and still no invented maximum.
        none = ps.drop_frame_summary({"count": 0, "events_not_recorded": 0, "events": []})
        self.assertEqual((none["status"], none["drop_frames"], none["frames"]), (ps.OK, 0, []))
        self.assertEqual(none["max_tile_records_peak"]["status"], ps.MISSING)

    def test_absent_or_contradictory_telemetry_is_never_a_number(self) -> None:
        self.assertEqual(ps.drop_frame_summary(None)["status"], ps.MISSING)
        older = ps.drop_frame_summary({"count": ps.NOT_MEASURED, "events": [], "events_not_recorded": 0})
        self.assertEqual(older["status"], ps.MISSING)
        self.assertIn("older binary", older["reason"])
        lost = ps.drop_frame_summary({"count": 2, "events_not_recorded": 0, "events": [_drop_event(1, _drop_frame())]})
        self.assertEqual(lost["status"], ps.INVALID)
        self.assertNotIn("frames", lost)
        bare = ps.drop_frame_summary({"count": 1, "events_not_recorded": 0,
                                      "events": [_drop_event(1, {"frame_serial": 3})]})
        self.assertEqual(bare["status"], ps.INVALID)
        more_cap_than_total = _drop_frame(dropped_records=10, raster_cap_dropped_records=11)
        self.assertEqual(ps.drop_frame_summary({"count": 1, "events_not_recorded": 0,
                                                "events": [_drop_event(1, more_cap_than_total)]})["status"], ps.INVALID)

    def test_every_stage_carries_the_drop_frame(self) -> None:
        """Wiring, renderer to comparator: deleting a key, the export call or a hand-over turns this red."""
        renderer = ROOT / "modules" / "gaussian_splatting" / "renderer"
        types = (renderer / "tile_render_types.h").read_text(encoding="utf-8")
        export = types[types.index("void append_drop_frame_telemetry(Dictionary &r_stats) const"):]
        export = export[:export.index("\n\t}\n")]
        for name in ps._DROP_FRAME_INT_KEYS + ps._DROP_FRAME_OPTIONAL_KEYS:
            self.assertIn(f'out["{name}"] = static_cast<int64_t>(frame.{name});', export, name)
        for name in ("drop_frame_samples", "drop_frame"):
            self.assertIn(f'r_stats["{name}"] = ', export, name)
        for name in ps._DROP_FRAME_OPTIONAL_KEYS:  # every optional value has a not-measured branch
            self.assertIn(f'out["{name}"] = not_measured;', export, name)
        self.assertIn('r_stats["drop_frame"] = not_measured;', export)
        src = (renderer / "render_debug_state_orchestrator.cpp").read_text(encoding="utf-8")
        body = src[src.index("void RenderDebugStateOrchestrator::append_drop_telemetry"):]
        self.assertIn("tr->append_drop_frame_telemetry(r_stats);", body[:body.index("\n}\n")])
        callback = (renderer / "tile_render_debug_stats.cpp").read_text(encoding="utf-8")
        self.assertIn("owner.diagnostics.record_overflow_sample(sample, context);", callback)
        self.assertIn("diagnostics.last_overlap_binning_splats, diagnostics.last_prefix_scan_capacity);",
                      (renderer / "tile_renderer.cpp").read_text(encoding="utf-8"))
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        sample = gd[gd.index("func _sample_and_write() -> void:"):]
        self.assertIn("\t_sample_drop_frames(stats)\n", sample[:sample.index("\nfunc ")])
        self.assertIn('_result["overlap_drop_frames"] = {"count": _drop_frame_count, "events": _drop_frame_events,', gd)
        for key in ("observed_row", "observed_phase", "observed_pass_index", "observed_segment", "observed_state"):
            self.assertIn(f'"{key}": ', gd, key)
        self.assertIn('out["overlap_drop_frames"] = ps.drop_frame_summary(result.get("overlap_drop_frames"))',
                      (HERE / "run_probe.py").read_text(encoding="utf-8"))
        paired = (HERE / "shared_layers_t0.py").read_text(encoding="utf-8")
        self.assertIn('"overlap_drop_frames": ps.drop_frame_summary(result.get("overlap_drop_frames")),', paired)
        self.assertIn('"checkpoint_drop_split": ps.checkpoint_drop_split(probe.get("passes")),', paired)
        compare = (HERE / "compare_shared_layers_t0.py").read_text(encoding="utf-8")
        self.assertIn('"overlap_drop_report": _overlap_drop_report(arms),', compare)
        self.assertIn('lines += _overlap_drop_lines(report.get("overlap_drop_report"))', compare)


def _breakdown_trace(frames, *, columns=True):
    """Constructed frame-breakdown rows (the arithmetic is the point): each entry is the interval
    ENDING at that row, in usec per part; ``unsplit`` a row the driver could not split (its -1).
    Row 0 is the first frame of a trace: no interval, no breakdown."""
    fields = ["row", "t_usec", "pass_index", "segment", "view", "state", "s_m",
              "gpu_frame_ms", "gpu_frame_valid", "upload_bytes_total", "upload_chunks_total",
              "read_bytes_total", "loaded_chunks", "needed_not_resident_chunks", "upload_stall_wall_ms"]
    types = "qqqqqqd" + "dqqqqqqd"
    if columns:
        fields += list(ps.BREAKDOWN_COLUMNS)
        types += "qqqqqdq"
    rows = []
    t = 1_000_000
    frames_total = 10
    for i, f in enumerate([None] + list(frames)):
        f = dict(f or {})
        split = i > 0 and not f.get("unsplit")
        parts = [f.get(k, 0) for k in ("telemetry", "capture", "script", "engine")] if split else [-1] * 4
        if i > 0:
            t += f["wall_usec"] if "wall_usec" in f else sum(parts)
        frames_total += f.get("renderer_frames", 1)
        row = [i, t, 0, f.get("segment", 2), -1, f.get("state", ps.STATE_MOVING), 1.5 * i,
               f.get("gpu_ms", 3.0), f.get("gpu_valid", 1), f.get("upload_bytes_total", 4096 * i), i,
               0, 5, f.get("not_resident", 0), 0.0]
        if columns:
            row += parts + [(1 if f.get("capture") else 0) if split else -1,
                            f.get("cpu_ms", 1.0), -1 if f.get("no_renderer") else frames_total]
        rows.append(tuple(row))
    return ps.Trace(fields, types, rows, 0)


class FrameBreakdown(unittest.TestCase):
    """#1445: each frame interval split into the engine's frame and the probe driver's own work, so
    the game-relevant frame time is reported apart from the harness overhead."""

    #: 8 clean 4 ms frames, then one whose interval holds a 400 ms screenshot readback.
    HARNESS = [{"telemetry": 300, "script": 100, "engine": 3600}] * 8 + [
        {"telemetry": 300, "script": 100, "capture": 400_000, "engine": 3600}]

    def test_harness_work_is_excluded_from_the_game_frame_and_kept_in_the_wall_frame(self) -> None:
        tr = _breakdown_trace(self.HARNESS)
        rows = list(range(len(tr.rows)))
        rec = ps.frame_breakdown(tr, rows)
        self.assertEqual(rec["status"], ps.OK, rec)
        self.assertEqual(rec["frames"], 9)
        # The wall-clock numbers are the existing metric, value for value.
        old = ps.interval_stats(ps.frame_intervals(tr, rows))
        for key in ("mean", "p95_time_weighted", "p99_time_weighted", "max"):
            self.assertEqual(rec["wall_frame"][key], old[key], key)
        self.assertEqual(rec["wall_frame"]["p95_time_weighted"]["value"], 404.0)
        self.assertEqual(rec["wall_frame"]["frames_over_250ms"], 1)
        # The engine needed 3.6 ms for every one of them.
        game = rec["game_frame"]
        for key in ("p50_time_weighted", "p95_time_weighted", "p99_time_weighted", "p50_frames", "p95_frames",
                    "p99_frames", "max"):
            self.assertEqual(game[key], ps.metric(3.6, unit="ms"), key)
        self.assertEqual((game["frames_over_50ms"], game["frames_over_250ms"]), (0, 0))
        self.assertEqual(rec["harness_frame"]["max"]["value"], 400.4)
        totals = rec["totals_ms"]
        self.assertAlmostEqual(totals["wall"], 436.0)
        self.assertAlmostEqual(totals["game_frame"] + totals["telemetry"] + totals["capture"] + totals["script"],
                               totals["wall"])
        share = rec["wall_share_pct"]
        self.assertAlmostEqual(share["capture"]["value"], 100.0 * 400.0 / 436.0)
        self.assertAlmostEqual(share["game_frame"]["value"], 100.0 * 32.4 / 436.0)
        self.assertAlmostEqual(share["renderer_cpu"]["value"] + share["engine_other"]["value"],
                               share["game_frame"]["value"])
        self.assertAlmostEqual(sum(share[k]["value"] for k in (
            "renderer_cpu", "engine_other", "script", "capture", "telemetry")), 100.0)
        slow = rec["slow_frames"]
        self.assertEqual((slow["wall_over_50ms"], slow["wall_over_50ms_harness_only"], slow["game_over_50ms"]),
                         (1, 1, 0))
        self.assertEqual(slow["listed"], [])
        self.assertEqual((rec["captures"], rec["frames_with_capture"]), (1, 1))

    def test_a_slow_engine_frame_is_a_game_hitch_and_says_what_the_frame_did(self) -> None:
        frames = [dict(f) for f in self.HARNESS]
        frames[4] = {"telemetry": 300, "script": 100, "engine": 300_000, "cpu_ms": 280.0, "segment": 7,
                     "upload_bytes_total": 9_000_000, "not_resident": 3}
        tr = _breakdown_trace(frames)
        rec = ps.frame_breakdown(tr, list(range(len(tr.rows))))
        self.assertEqual(rec["status"], ps.OK, rec)
        self.assertEqual(rec["game_frame"]["max"]["value"], 300.0)
        self.assertEqual(rec["game_frame"]["frames_over_250ms"], 1)
        self.assertEqual(rec["game_frame"]["p95_time_weighted"]["value"], 300.0)
        slow = rec["slow_frames"]
        self.assertEqual((slow["wall_over_50ms"], slow["wall_over_50ms_harness_only"], slow["game_over_50ms"],
                          slow["game_over_250ms"], slow["game_over_50ms_with_capture_in_interval"]), (2, 1, 1, 1, 0))
        (hitch,) = slow["listed"]
        self.assertEqual((hitch["row"], hitch["segment"], hitch["game_frame_ms"], hitch["render_cpu_ms"]),
                         (5, 7, 300.0, 280.0))
        self.assertEqual(hitch["upload_bytes_delta"], 9_000_000 - 4096 * 4)
        self.assertEqual(hitch["needed_not_resident_chunks"], 3)
        self.assertEqual(hitch["captures_in_interval"], 0)

    def test_an_older_driver_reads_as_not_measured_never_as_zero(self) -> None:
        # A real trace captured before #1445: it has no breakdown columns.
        with tempfile.TemporaryDirectory() as tmp:
            trace, result = captured("tsmoke_path", Path(tmp))
            tr = ps.read_trace(trace)
            rec = ps.frame_breakdown(tr, ps.rows_where(tr, phase=0, state=ps.STATE_MOVING))
            self.assertEqual(rec["status"], ps.MISSING)
            self.assertIn("older than #1445", rec["reason"])
            for key in ("game_frame", "harness_frame", "wall_share_pct", "totals_ms"):
                self.assertNotIn(key, rec)
            m = manifest()
            path = pp.resolve_path(m)
            truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
            p0 = rp.summarise_path(tr, {"result": result, "trace": str(trace)}, path, truth, 1)["passes"][0]
            # The old wall-clock block is still there and still measured; the new blocks are not.
            self.assertEqual(p0["moving_frame_interval"]["p95_time_weighted"]["status"], ps.OK)
            for key in ("moving_frame_breakdown", "measured_frame_breakdown"):
                self.assertEqual(p0[key]["status"], ps.MISSING, key)
        self.assertEqual(ps.frame_breakdown(_breakdown_trace(self.HARNESS, columns=False), [1, 2])["status"], ps.MISSING)
        self.assertEqual(ps.frame_breakdown(_breakdown_trace(self.HARNESS), [])["status"], ps.MISSING)
        self.assertEqual(ps.frame_breakdown(_breakdown_trace(self.HARNESS), [0])["status"], ps.MISSING)

    def test_a_frame_the_driver_could_not_split_is_not_measured(self) -> None:
        frames = [dict(f) for f in self.HARNESS]
        frames[2] = {"unsplit": True, "wall_usec": 4000}
        tr = _breakdown_trace(frames)
        rec = ps.frame_breakdown(tr, list(range(len(tr.rows))))
        self.assertEqual(rec["status"], ps.MISSING)
        self.assertIn("1 of 9", rec["reason"])
        self.assertIn("row 3", rec["reason"])
        self.assertNotIn("game_frame", rec)

    def test_parts_that_do_not_add_up_to_the_interval_are_invalid(self) -> None:
        frames = [dict(f) for f in self.HARNESS]
        frames[5] = {"telemetry": 300, "script": 100, "engine": 3600, "wall_usec": 9000}
        tr = _breakdown_trace(frames)
        rec = ps.frame_breakdown(tr, list(range(len(tr.rows))))
        self.assertEqual(rec["status"], ps.INVALID)
        self.assertIn("row 6", rec["reason"])
        self.assertNotIn("game_frame", rec)

    def test_renderer_cpu_counts_fresh_renderer_frames_only_and_absence_is_missing(self) -> None:
        frames = [dict(f) for f in self.HARNESS]
        # A held sample: the renderer finished no frame in this interval, the stat repeats.
        frames[1] = {**frames[1], "renderer_frames": 0, "cpu_ms": 1.0}
        tr = _breakdown_trace(frames)
        rec = ps.frame_breakdown(tr, list(range(len(tr.rows))))
        self.assertEqual(rec["renderer"]["cpu_fresh_frames"], 8)
        self.assertAlmostEqual(rec["totals_ms"]["renderer_cpu"], 8.0)
        self.assertEqual(rec["renderer"]["gpu_frame_ms"]["samples"], 9)
        frames[3] = {**frames[3], "no_renderer": True}
        rec = ps.frame_breakdown(_breakdown_trace(frames), list(range(len(tr.rows))))
        self.assertEqual(rec["status"], ps.OK, "the engine/harness split does not need the renderer's stat")
        self.assertEqual(rec["game_frame"]["p95_time_weighted"]["value"], 3.6)
        for key in ("renderer_cpu", "engine_other"):
            self.assertEqual(rec["wall_share_pct"][key]["status"], ps.MISSING, key)
        self.assertEqual(rec["renderer"]["cpu_ms"]["status"], ps.MISSING)
        self.assertNotIn("renderer_cpu", rec["totals_ms"])

    def test_capture_cost_is_reported_per_run_and_absent_timing_is_missing(self) -> None:
        shots = {"dir": "/x/screenshots", "moving_every_n": 400, "taken": [
            {"name": "p0_C1", "file": "p0_C1.png", "capture_usec": 300_000},
            {"name": "p0_s01_row0000400", "file": "p0_s01_row0000400.png", "capture_usec": 100_000}]}
        rec = rp.screenshot_record(shots)
        self.assertEqual(rec["capture_ms"], {"samples": 2, "total": 400.0, "mean": 200.0, "max": 300.0})
        self.assertIn("moving_frame_breakdown", rec["note"])
        # An older driver did not time its captures: not measured, never 0 ms.
        del shots["taken"][1]["capture_usec"]
        self.assertEqual(rp.screenshot_record(shots)["capture_ms"]["status"], ps.MISSING)
        self.assertEqual(rp.screenshot_record({"dir": "/x", "moving_every_n": 0, "taken": []})["capture_ms"]["status"],
                         ps.MISSING)

    def test_the_wall_clock_block_keeps_its_meaning(self) -> None:
        self.assertEqual(set(ps.interval_stats([1.0, 2.0])), {
            "frames", "mean", "p95_time_weighted", "p99_time_weighted", "max", "frames_over_50ms",
            "frames_over_50ms_pct", "frames_over_250ms"})
        # Still time-weighted nearest rank (ADR 8.3): one 100 ms frame among twenty 1 ms frames
        # holds 5/6 of the wall time, so it is the p95 although 95 % of the frames took 1 ms.
        old = ps.interval_stats([1.0] * 20 + [100.0])
        self.assertEqual((old["p95_time_weighted"]["value"], old["p99_time_weighted"]["value"], old["max"]["value"]),
                         (100.0, 100.0, 100.0))
        self.assertAlmostEqual(old["mean"]["value"], 120.0 / 21)
        self.assertEqual((old["frames"], old["frames_over_50ms"], old["frames_over_250ms"]), (21, 1, 0))
        self.assertEqual(ps.nearest_rank([1.0] * 20 + [100.0], 0.95), 1.0)
        tr = _breakdown_trace(self.HARNESS)
        self.assertEqual(ps.frame_intervals(tr, [9]), [404.0], "row-to-row wall clock, harness work included")

    def test_the_driver_writes_every_column_and_times_its_own_work(self) -> None:
        """Wiring: probe_world.gd traces exactly BREAKDOWN_COLUMNS, stamps the hand-over to the engine
        in the one place frames are awaited, charges the readback to the capture part, and the
        renderer publishes the CPU stat (deleting any of these turns this red)."""
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        block = gd[gd.index("const BREAKDOWN_FIELDS"):]
        block = block[:block.index("\n]\n")]
        self.assertEqual(tuple(re.findall(r'\["([a-z_]+)", "[qd]"\]', block)), ps.BREAKDOWN_COLUMNS)
        self.assertIn("] + DROP_FIELDS + BREAKDOWN_FIELDS", gd)
        self.assertEqual(gd.count("await RenderingServer.frame_post_draw"), 1)
        self.assertIn("\t_iv_yield_usec = Time.get_ticks_usec()\n\tawait RenderingServer.frame_post_draw\n"
                      "\t_sample_and_write()", gd)
        shot = gd[gd.index("func _screenshot("):]
        shot = shot[:shot.index("\n\n\n")]
        self.assertIn("_iv_capture_usec += ", shot)
        self.assertIn("_iv_captures += 1", shot)
        sample = gd[gd.index("func _sample_and_write("):]
        sample = sample[:sample.index("\n\n\n")]
        self.assertIn("values.append_array(_frame_breakdown_values(t_usec, stats))", sample)
        self.assertRegex(sample, r"_iv_telemetry_usec = Time\.get_ticks_usec\(\) - t_usec$")
        stats = (ROOT / "modules" / "gaussian_splatting" / "renderer" / "render_diagnostics_orchestrator.cpp").read_text(
            encoding="utf-8")
        for key in ("frame_time_ms", "total_frames_rendered"):
            self.assertIn(f'r_metrics["{key}"]', stats, key)
            self.assertIn(f'"{key}"', gd, key)
        probe = (HERE / "run_probe.py").read_text(encoding="utf-8")
        self.assertIn('"moving_frame_breakdown": ps.frame_breakdown(trace, moving)', probe)


class CheckpointEvidence(unittest.TestCase):
    """#1454: checkpoint evidence survives successful diagnostic process completion.

    The minimal C2 records below are copied from run 43717101341's chunk,
    HLOD and migration/base result.json outputs. Only unrelated fields and
    forward_progress samples are omitted. These tests replay the records through
    the summary using the existing captured trace; they do not claim that trace
    belongs to this newer run or test its timing/cap metrics. Cache observations
    are explicitly CONSTRUCTED policy inputs: that producer did not export them.
    No test reads an external evidence directory.
    """

    CHUNK_C2 = {
        "checkpoint": "C2", "segment": "S6", "pass_index": 0,
        "actual_route": "chunk", "drawn_splats": 298066, "row": 6105,
        "pose_first_frame": 6100, "render_frame_counter": 6105,
        "rendered_splat_count_source_frame": 6103,
        "settle": {"settled": True, "settle_usec": 9469, "frames": 5,
                   "deadline_s": 15.0, "pose_error_m": 0.0},
    }
    HLOD_C2 = {
        "checkpoint": "C2", "segment": "S6", "pass_index": 0,
        "actual_route": "hlod", "drawn_splats": 204115, "row": 6767,
        "pose_first_frame": 6759, "render_frame_counter": 6767,
        "rendered_splat_count_source_frame": 6765,
        "settle": {"settled": True, "settle_usec": 21212, "frames": 8,
                   "deadline_s": 15.0, "pose_error_m": 0.0},
    }
    BASE_C2 = {
        "checkpoint": "C2", "segment": "S6", "pass_index": 0,
        "actual_route": "hlod", "drawn_splats": 204115, "row": 26752,
        "pose_first_frame": -1, "render_frame_counter": -1,
        "rendered_splat_count_source_frame": -1,
        "settle": {"settled": False, "settle_usec": -1, "frames": 4845,
                   "deadline_s": 15.0, "pose_error_m": 0.0,
                   "reason": "settle_deadline_missed"},
    }

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.trace_path, self.result = captured("tsmoke_path", Path(self._tmp.name))
        self.trace = ps.read_trace(self.trace_path)
        self.path = pp.resolve_path(manifest())
        self.truth = gt.build_groundtruth(manifest(), self.path, pp.checkpoints(self.path),
                                          rp.CAMERA, rp.ASPECT)

    def tearDown(self):
        self._tmp.cleanup()

    def replay(self, checkpoint, enabled=False):
        result = copy.deepcopy(self.result)
        result.update(ok=True, reason="plan completed", route=checkpoint["actual_route"],
                      checkpoints=[copy.deepcopy(checkpoint)])
        # CONSTRUCTED new observation, at run scope to exercise its propagation
        # to individual checkpoint receipts. Zero hits does not reveal enabled.
        result["static_sort_cache"] = {"enabled": enabled,
                                        "applicable": checkpoint["actual_route"] == "chunk"}
        result.setdefault("final_render_stats", {})["sort_cache_hits"] = 0
        run = {"exit_code": 0, "timed_out": False, "result": result,
               "trace": str(self.trace_path)}
        return result, rp.summarise_path(self.trace, run, self.path, self.truth, 1)

    def assert_evidence(self, summary, verdict, aggregate="VALID"):
        cp = summary["passes"][0]["checkpoints"][0]
        self.assertEqual(cp.get("evidence_verdict"), verdict,
                         "checkpoint evidence must remain explicit beside process completion")
        self.assertEqual(summary.get("checkpoint_evidence", {}).get("verdict"), aggregate)
        if aggregate != "VALID":
            self.assertIn(verdict, json.dumps(summary.get("checkpoint_evidence", {}).get("reasons", [])))
        return cp

    def test_deadline_missed_checkpoint_is_unusable_despite_ok_process(self):
        result, summary = self.replay(self.BASE_C2)
        self.assertTrue(result["ok"])
        self.assertEqual(result["reason"], "plan completed")
        self.assertFalse(summary["passes"][0]["checkpoints"][0]["settled"])
        cp = self.assert_evidence(summary, "checkpoint_settle_deadline_missed", "UNUSABLE")
        self.assertEqual(cp.get("settle"), self.BASE_C2["settle"])
        self.assertEqual(cp["drawn_splats"], 204115, "retain diagnostic capture after the deadline")

    def test_settled_checkpoint_preserves_provenance_and_valid_verdict(self):
        _, summary = self.replay(self.CHUNK_C2)
        cp = self.assert_evidence(summary, "VALID")
        for key in ("settle", "pose_first_frame", "render_frame_counter", "rendered_splat_count_source_frame"):
            self.assertEqual(cp.get(key), self.CHUNK_C2[key], key)

    def test_run_probe_delegates_checkpoint_normalization_to_probe_summary(self):
        normalize = getattr(ps, "checkpoint_evidence", None)
        self.assertTrue(callable(normalize), "probe_summary must own checkpoint evidence normalization")
        with mock.patch.object(ps, "checkpoint_evidence", wraps=normalize) as normalizer:
            self.replay(self.BASE_C2)
        self.assertTrue(normalizer.called, "deleting run_probe's normalization wiring must turn this red")
        receipts = [arg for call in normalizer.call_args_list for arg in (*call.args, *call.kwargs.values())
                    if isinstance(arg, dict)]
        self.assertTrue(any(all(receipt.get(key) == value for key, value in self.BASE_C2.items())
                            for receipt in receipts),
                        "the normalizer must see the producer receipt, including its failed settle")

    def test_required_path_metrics_rejects_unusable_evidence_even_when_settled(self):
        # CONSTRUCTED consumer-policy case on the complete captured path. Keep
        # every existing required metric legal so missing checkpoints, stale
        # counts, or an unrelated metric cannot manufacture the rejection.
        result = copy.deepcopy(self.result)
        result.update(route="chunk", static_sort_cache={"enabled": False, "applicable": True})
        for checkpoint in result["checkpoints"]:
            # Policy-only transplantation of a known fresh frame triple onto
            # every legacy receipt, not a capture of those old checkpoints.
            for key in ("pose_first_frame", "render_frame_counter", "rendered_splat_count_source_frame"):
                checkpoint[key] = self.CHUNK_C2[key]
            checkpoint.update(actual_route="chunk", static_sort_cache={"enabled": False, "applicable": True})
        summary = rp.summarise_path(self.trace, {"result": result}, self.path, self.truth, 1)
        world = {"flythrough": summary, "rss": {"peak_rss_bytes": ps.metric(1)}}
        self.assertEqual(rp.required_path_metrics(world), [])
        cp = summary["passes"][0]["checkpoints"][2]
        self.assertTrue(cp["settled"])
        cp.update(evidence_verdict="static_sort_cache_unsupported",
                  static_sort_cache={"enabled": True, "applicable": True})
        summary["checkpoint_evidence"] = {
            "verdict": "UNUSABLE", "reasons": ["static_sort_cache_unsupported"]}
        problems = rp.required_path_metrics(world)
        self.assertTrue(any("static_sort_cache_unsupported" in problem for problem in problems),
                        "run_probe final validation must consume checkpoint evidence, even when settled")

    def test_chunk_cache_enabled_is_unsupported_even_with_zero_hits(self):
        _, summary = self.replay(self.CHUNK_C2, enabled=True)
        cp = self.assert_evidence(summary, "static_sort_cache_unsupported", "UNUSABLE")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": True, "applicable": True})

    def test_chunk_cache_disabled_retains_valid_checkpoint_evidence(self):
        _, summary = self.replay(self.CHUNK_C2, enabled=False)
        cp = self.assert_evidence(summary, "VALID")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": False, "applicable": True})

    def test_chunk_cache_unavailable_is_explicit_despite_zero_hits(self):
        _, summary = self.replay(self.CHUNK_C2, enabled=None)
        cp = self.assert_evidence(summary, "static_sort_cache_state_unavailable", "UNUSABLE")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": None, "applicable": True})

    def test_hlod_cache_enabled_records_bypass_and_valid_evidence(self):
        _, summary = self.replay(self.HLOD_C2, enabled=True)
        cp = self.assert_evidence(summary, "VALID")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": True, "applicable": False})

    def test_hlod_cache_disabled_records_bypass_and_valid_evidence(self):
        _, summary = self.replay(self.HLOD_C2, enabled=False)
        cp = self.assert_evidence(summary, "VALID")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": False, "applicable": False})

    def test_hlod_cache_unavailable_preserves_unknown_state_and_bypass(self):
        _, summary = self.replay(self.HLOD_C2, enabled=None)
        cp = self.assert_evidence(summary, "VALID")
        self.assertEqual(cp.get("static_sort_cache"), {"enabled": None, "applicable": False})


class CheckpointRouteContext(unittest.TestCase):
    def test_a_checkpoint_cannot_claim_hlod_to_bypass_known_chunk_cache(self):
        checkpoint = copy.deepcopy(CheckpointEvidence.HLOD_C2)
        evidence = ps.checkpoint_evidence(checkpoint, route="chunk", static_sort_cache={"enabled": True})
        self.assertEqual(evidence["evidence_verdict"], "checkpoint_unsettled")
        self.assertEqual(evidence["static_sort_cache"], {"enabled": True, "applicable": True})
        self.assertIn("static_sort_cache_unsupported", evidence["evidence_reasons"])
        checkpoint.pop("actual_route")
        unknown = ps.checkpoint_evidence(checkpoint)
        self.assertEqual(unknown["evidence_verdict"], "checkpoint_unsettled")
        self.assertIsNone(unknown["static_sort_cache"]["applicable"])


class CheckpointEvidenceWiring(unittest.TestCase):
    """Textual GDScript wiring checks only; these do not execute Godot.

    Summary policy is exercised above. Actual renderer-state observation and
    checkpoint export still require a Godot runtime run in the implementation phase.
    """

    def setUp(self):
        self.gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")

    def body(self, name):
        match = re.search(rf"^func {name}\([^\n]*\)[^\n]*:\n(.*?)(?=^func |\Z)",
                          self.gd, re.M | re.S)
        self.assertIsNotNone(match, f"production helper {name} is missing")
        return match.group(1)

    def test_driver_reads_actual_static_cache_state_with_explicit_unavailability(self):
        methods = re.findall(r"^func [^\n]+\n(.*?)(?=^func |\Z)", self.gd, re.M | re.S)
        observers = [body for body in methods if 'has_method("is_static_sort_cache_enabled")' in body]
        self.assertTrue(observers, "query actual renderer cache state; zero hits cannot imply disabled")
        observer = "\n".join(observers)
        self.assertIn("get_renderer()", observer)
        self.assertRegex(observer, r'\.call\("is_static_sort_cache_enabled"\)|\.is_static_sort_cache_enabled\(\)')
        self.assertIn('"enabled"', observer)
        self.assertIn("null", observer, "an unavailable getter must retain an explicit unknown enabled state")
        self.assertNotIn("sort_cache_hits", observer)

    def test_driver_records_cache_route_exclusion_and_hlod_bypass(self):
        self.assertTrue('"static_sort_cache_unsupported"' in self.gd,
                        "enabled chunk cache must name the exclusion in checkpoint/run evidence")
        cache_methods = [body for body in re.findall(r"^func [^\n]+\n(.*?)(?=^func |\Z)",
                                                    self.gd, re.M | re.S)
                         if '"applicable"' in body and '"enabled"' in body]
        self.assertTrue(cache_methods, "record cache applicability separately from enabled state")
        self.assertTrue(any("_is_hlod_payload_route()" in body for body in cache_methods),
                        "HLOD applicability must use the actual route, which bypasses this cache")
        self.assertIn('"static_sort_cache"', self.body("_checkpoint"))
        self.assertIn('"static_sort_cache"', self.body("_finish"))

    def test_driver_exports_checkpoint_and_run_verdicts_beside_process_ok(self):
        self.assertIn('"evidence_verdict"', self.body("_checkpoint"))
        self.assertIn('"checkpoint_settle_deadline_missed"', self.gd)
        finish = self.body("_finish")
        self.assertIn('_result["ok"] = ok', finish, "process completion remains distinct")
        self.assertIn('"checkpoint_evidence"', finish)


class CountProvenance(unittest.TestCase):
    """Scalar predicate replay plus textual wiring; this does not execute GDScript.

    Translate only the small scalar helper's Python-compatible statements, rather
    than testing a second handwritten implementation. Camera/loop wiring remains
    structural and requires a Godot run for behavioural evidence.
    """

    def setUp(self):
        self.gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")

    def body(self, name):
        match = re.search(rf"^func {name}\([^\n]*\)[^\n]*:\n(.*?)(?=^func |\Z)",
                          self.gd, re.M | re.S)
        self.assertIsNotNone(match, f"production helper {name} is missing")
        return match.group(1).split("\n\n\n")[0]

    def fresh(self, stats, first=20):
        body = self.body("_count_sample_is_fresh")
        body = re.sub(r"\bvar (\w+)\s*:?=", r"\1 =", body)
        body = re.sub(r"\bfalse\b", "False", body)
        env = {"typeof": lambda v: type(v), "TYPE_INT": int}
        exec("def predicate(stats, pose_first_frame):\n" + body, env)
        return env["predicate"](stats, first)

    def test_stale_count_cannot_settle(self):
        self.assertFalse(self.fresh({"render_frame_counter": 22,
                                     "rendered_splat_count_source_frame": 19}))

    def test_missing_or_invalid_provenance_cannot_settle(self):
        for stats in ({}, {"render_frame_counter": 22},
                      {"render_frame_counter": 22, "rendered_splat_count_source_frame": -1},
                      {"render_frame_counter": 22, "rendered_splat_count_source_frame": "20"},
                      {"rendered_splat_count_source_frame": 20}):
            with self.subTest(stats=stats):
                self.assertFalse(self.fresh(stats))

    def test_fresh_count_including_boundary_can_settle(self):
        for source in (20, 22):
            self.assertTrue(self.fresh({"render_frame_counter": 22,
                                       "rendered_splat_count_source_frame": source}))
        self.assertFalse(self.fresh({"render_frame_counter": 22,
                                    "rendered_splat_count_source_frame": 20}, -1))
        self.assertFalse(self.fresh({"render_frame_counter": 22,
                                    "rendered_splat_count_source_frame": 23}))

    def test_provenance_gates_both_routes_and_is_recorded(self):
        settled = self.body("_settled")
        self.assertIn("if not _count_sample_is_fresh(_last_stats, _pose_first_frame):", settled)
        self.assertLess(settled.index("_count_sample_is_fresh"), settled.index("_is_hlod_payload_route"))
        loop = self.body("_pump_settle")
        self.assertLess(loop.index("await _frame()"), loop.index("_update_pose_first_frame()"))
        self.assertLess(loop.index("_update_pose_first_frame()"), loop.index("if _settled():"))
        tracker = self.body("_update_pose_first_frame")
        self.assertIn("_stats_pose_error() > 0.001", tracker)
        self.assertIn("not _stats_orientation_matches()", tracker)
        self.assertIn("_pose_first_frame = -1", tracker)
        self.assertIn("if _pose_first_frame < 0:", tracker)
        self.assertIn('_pose_first_frame = int(_last_stats["render_frame_counter"])', tracker)
        self.assertIn("_pose_first_frame = -1", self.body("_set_pose"))
        checkpoint = self.body("_checkpoint")
        for key in ("render_frame_counter", "rendered_splat_count_source_frame", "pose_first_frame"):
            self.assertIn(f'"{key}":', checkpoint)

    def test_native_readback_and_host_case_are_wired(self):
        module = ROOT / "modules/gaussian_splatting"
        pipeline = (module / "interfaces/gpu_sorting_pipeline.cpp").read_text(encoding="utf-8")
        callback = pipeline.split("void GPUSortingPipeline::_on_instance_count_readback", 1)[1].split("\n}", 1)[0]
        self.assertIn("if (!last_instance_count.accept(indirect->element_count, request_frame))", callback)
        self.assertIn("resolved_visible_source_frame = last_instance_count.source_frame();", pipeline)
        self.assertIn("last_published_instance_count_source_frame = resolved_visible_source_frame;", pipeline)
        anchor = (module / "tests/test_gaussian_splatting.h").read_text(encoding="utf-8")
        self.assertIn("test_splat_count_sample_cpp_force_link_anchor = test_splat_count_sample_cpp_force_link();", anchor)

    def test_native_source_and_camera_frame_reach_stats(self):
        renderer = ROOT / "modules/gaussian_splatting/renderer"
        sorting = (renderer / "render_sorting_orchestrator.cpp").read_text(encoding="utf-8")
        self.assertIn("frame_context.runtime.frame_counter = frame_state.frame_counter;", sorting)
        self.assertEqual(sorting.count("sorting_pipeline->get_published_instance_count_source_frame()"), 2)
        self.assertIn("sorting_state.sorted_splat_count_source_frame = instance_sort_cache.count_source_frame;", sorting)
        diagnostics = (renderer / "render_diagnostics_orchestrator.cpp").read_text(encoding="utf-8")
        self.assertIn('r_metrics["rendered_splat_count_source_frame"] = perf.rendered_splat_count_source_frame;', diagnostics)
        self.assertIn('stats["render_frame_counter"] = view_state.last_camera_render_frame;', diagnostics)
        self.assertIn("performance_state.metrics.rendered_splat_count_source_frame = frame_state.visible_splat_count_source_frame;", diagnostics)
        scene = (renderer / "gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        self.assertIn("get_view_state().last_camera_render_frame = get_frame_state().frame_counter;", scene)
        orientation = self.body("_stats_orientation_matches")
        for vector in ("forward", "up"):
            for axis in "xyz":
                key = f"debug_cam_{vector}_{axis}"
                self.assertIn(f'stats["{key}"]', diagnostics)
                self.assertIn(f'"{key}"', orientation)
        self.assertIn("_camera.global_transform.basis.z", orientation)
        self.assertIn("_camera.global_transform.basis.y", orientation)


class SettleStreakProvenance(unittest.TestCase):
    """#1455: replay scalar decisions and the production pump without Godot.

    Frame numbers for the count/streak boundary come from the issue's diagnosed
    candidate p1 C7 (pose 32081, render 32093, count source/streak start 32090).
    Sequences, digests and clocks below are CONSTRUCTED policy inputs, not captured
    producer receipts. The replay supplies settled route/pose state; it does not
    exercise camera matching, streaming, GPU readbacks or wall-clock timing.

    Optional pure helpers use the seams _count_sample_is_after_streak(stats,
    streak_first_frame) and _settle_digest_is_stable(stats, streak_digest). Before
    they exist, fall back to today's pose-fresh count gate and digest-blind streak.
    Thus the old predicate fails assertions, rather than a missing-helper lookup.
    Replaying the pump as well catches helpers whose callers are never wired up.
    """

    POSE_FIRST = 32081
    STREAK_FIRST = 32090

    def setUp(self):
        self.gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.streak_required = int(re.search(r"^const SETTLE_STREAK := (\d+)$", self.gd, re.M)[1])

    def compile_body(self, name, env, optional=False):
        match = re.search(rf"^func {name}\(([^\n]*)\)[^\n]*:\n(.*?)(?=^func |\Z)",
                          self.gd, re.M | re.S)
        if optional and match is None:
            return
        self.assertIsNotNone(match, f"existing production function {name} is missing")
        args = re.sub(r":\s*\w+", "", match[1])
        body = match[2].split("\n\n\n")[0]
        body = re.sub(r"\bvar (\w+)(?:\s*:\s*\w+)?\s*:?=", r"\1 =", body)
        for gd, py in (("true", "True"), ("false", "False"), ("null", "None")):
            body = re.sub(rf"\b{gd}\b", py, body)
        body = body.replace("await _frame()", "_frame()")
        exec(f"def {name}({args}):\n" + body, env)

    def helpers(self, stats=None):
        env = {"typeof": type, "TYPE_INT": int, "TYPE_STRING": str,
               "_pose_first_frame": self.POSE_FIRST, "_last_stats": stats or {}}
        self.compile_body("_count_sample_is_fresh", env)
        # These fallbacks are the current production decisions, not the fix.
        env["_count_sample_is_after_streak"] = lambda stats, first: env["_count_sample_is_fresh"](
            stats, self.POSE_FIRST)
        env["_settle_digest_is_stable"] = lambda stats, digest: True
        for name in ("_count_sample_is_after_streak", "_settle_digest_is_stable"):
            self.compile_body(name, env, optional=True)
        return env

    def sample(self, render, source, **extra):
        return {"render_frame_counter": render, "rendered_splat_count_source_frame": source, **extra}

    def replay_pump(self, samples):
        env = self.helpers()
        tick = 0
        frame = 0

        def next_frame():
            nonlocal tick, frame
            env["_last_stats"] = samples[min(frame, len(samples) - 1)]
            frame += 1
            tick += 1000

        env.update({
            "Time": mock.Mock(get_ticks_usec=lambda: tick),
            "SETTLE_STREAK": self.streak_required, "_plan": {},
            "_frame": next_frame, "_update_pose_first_frame": lambda: None,
            "_stats_pose_error": lambda: 0.0,
            "_settled": lambda: env["_last_stats"].get("demand_settled", True)
            and env["_count_sample_is_fresh"](env["_last_stats"], self.POSE_FIRST),
        })
        self.compile_body("_pump_settle", env)
        # All supplied samples finish before this deadline; the next poll misses
        # it. No real clock or arbitrary iteration cap determines acceptance.
        return env["_pump_settle"]((len(samples) + 0.5) / 1000.0)

    def verdict(self, stats, settle, cache=None):
        env = self.helpers(stats)
        self.compile_body("_checkpoint_evidence_verdict", env)
        return env["_checkpoint_evidence_verdict"](
            settle, {"applicable": False} if cache is None else cache)

    def test_count_source_must_be_strictly_after_streak_start(self):
        env = self.helpers()
        for source, expected in ((32089, False), (32090, False), (32091, True)):
            with self.subTest(source=source):
                stats = self.sample(32093, source)
                accepted = 3 >= self.streak_required \
                    and env["_count_sample_is_fresh"](stats, self.POSE_FIRST) \
                    and env["_count_sample_is_after_streak"](stats, self.STREAK_FIRST)
                self.assertEqual(accepted, expected,
                                 f"count source {source} must be > streak start 32090; pose freshness is insufficient")

    def test_pump_keeps_polling_until_count_describes_the_streak(self):
        samples = [self.sample(frame, 32090) for frame in range(32090, 32094)]
        samples.append(self.sample(32094, 32091))
        receipt = self.replay_pump(samples)
        self.assertTrue(receipt["settled"], receipt)
        self.assertEqual(receipt["frames"], 5,
                         "the pump must wait past the three-frame streak for a count sourced after frame 32090")
        self.assertEqual(receipt.get("streak_first_frame"), 32090)

    def test_old_count_cannot_settle_even_with_a_long_streak(self):
        receipt = self.replay_pump([self.sample(frame, 32090) for frame in range(32090, 32099)])
        self.assertFalse(receipt["settled"],
                         "a long settled streak cannot make a count sourced at its first frame valid")
        self.assertEqual(receipt.get("reason"), "settle_deadline_missed")
        self.assertEqual(receipt.get("streak_first_frame"), 32090)

    def test_digest_helper_rejects_a_changed_drawn_node_set(self):
        stable = self.helpers()["_settle_digest_is_stable"]
        self.assertTrue(stable({"hlod_drawn_node_digest": "cut-a"}, "cut-a"))
        self.assertFalse(stable({"hlod_drawn_node_digest": "cut-b"}, "cut-a"),
                         "a changed drawn-node digest must restart the settled streak")

    def test_digest_change_restarts_streak_at_the_changed_frame(self):
        samples = [self.sample(frame, frame - 1, hlod_drawn_node_digest=digest)
                   for frame, digest in ((32090, "cut-a"), (32091, "cut-a"),
                                         (32092, "cut-b"), (32093, "cut-b"), (32094, "cut-b"))]
        receipt = self.replay_pump(samples)
        self.assertTrue(receipt["settled"], receipt)
        self.assertEqual(receipt["frames"], 5,
                         "a digest change on frame 32092 must restart, rather than finish, the streak")
        self.assertEqual(receipt.get("streak_first_frame"), 32092)
        self.assertEqual(receipt.get("streak_digest"), "cut-b")
        self.assertEqual(receipt["settle_usec"], 3000)

    def test_nonsettled_sample_restarts_streak_and_count_barrier(self):
        samples = [self.sample(32090, 32089), self.sample(32091, 32090, demand_settled=False),
                   self.sample(32092, 32091), self.sample(32093, 32092),
                   self.sample(32094, 32092), self.sample(32095, 32093)]
        receipt = self.replay_pump(samples)
        self.assertTrue(receipt["settled"], receipt)
        self.assertEqual(receipt["frames"], 6,
                         "after demand resumes, the old streak's count barrier cannot be reused")
        self.assertEqual(receipt.get("streak_first_frame"), 32092)
        self.assertEqual(receipt["settle_usec"], 3000)

    def test_absent_or_null_digest_does_not_block_or_fabricate_identity(self):
        stable = self.helpers()["_settle_digest_is_stable"]
        for digest_stats in ({}, {"hlod_drawn_node_digest": None}):
            with self.subTest(digest_stats=digest_stats):
                self.assertTrue(stable(digest_stats, "cut-a"))
                receipt = self.replay_pump([self.sample(frame, frame - 1, **digest_stats)
                                            for frame in range(32090, 32093)])
                self.assertTrue(receipt["settled"], "the pinned base/chunk route need not export a digest")
                self.assertIsNone(receipt.get("streak_digest"), "never fabricate a missing digest")

    def test_missing_or_invalid_count_source_never_settles(self):
        for value in (None, -1, "32091", 32091.0, True, 32099):
            with self.subTest(source=value):
                receipt = self.replay_pump([self.sample(frame, value) for frame in range(32090, 32094)])
                self.assertFalse(receipt["settled"], "an invalid/future count source cannot settle")
                self.assertEqual(receipt.get("reason"), "settle_deadline_missed")
        receipt = self.replay_pump([{"render_frame_counter": frame} for frame in range(32090, 32094)])
        self.assertFalse(receipt["settled"], "an absent count source cannot settle")
        self.assertEqual(receipt.get("reason"), "settle_deadline_missed")

    def test_settle_receipt_records_streak_first_frame_and_available_digest(self):
        for digest_stats in ({}, {"hlod_drawn_node_digest": None},
                             {"hlod_drawn_node_digest": "cut-a"}, {"hlod_drawn_node_digest": ""}):
            with self.subTest(digest_stats=digest_stats):
                receipt = self.replay_pump([self.sample(frame, frame - 1, **digest_stats)
                                            for frame in range(32090, 32093)])
                self.assertTrue(receipt["settled"], receipt)
                self.assertEqual(receipt.get("streak_first_frame"), 32090,
                                 "settle receipts must export the streak's render frame, not just elapsed time")
                self.assertEqual(receipt.get("streak_digest"), digest_stats.get("hlod_drawn_node_digest"))

    def test_checkpoint_verdict_rejects_count_at_or_before_streak_start(self):
        for source in (32089, 32090):
            with self.subTest(source=source):
                self.assertEqual(self.verdict(self.sample(32093, source),
                                              {"settled": True, "streak_first_frame": 32090}),
                                 "checkpoint_unsettled",
                                 "checkpoint evidence must independently require a count after the streak start")

    def test_checkpoint_verdict_rejects_missing_or_invalid_streak_start(self):
        for settle in ({"settled": True}, *({"settled": True, "streak_first_frame": value}
                                          for value in (None, -1, "32090", 32090.0, True, 32094))):
            with self.subTest(settle=settle):
                self.assertEqual(self.verdict(self.sample(32093, 32091), settle), "checkpoint_unsettled",
                                 "without a valid streak-first render frame the count cannot prove settlement")

    def test_checkpoint_verdict_preserves_valid_deadline_and_cache_results(self):
        stats = self.sample(32093, 32091)
        settle = {"settled": True, "streak_first_frame": 32090}
        self.assertEqual(self.verdict(stats, settle), "VALID")
        for enabled, expected in ((False, "VALID"), (True, "static_sort_cache_unsupported"),
                                  (None, "static_sort_cache_state_unavailable")):
            with self.subTest(enabled=enabled):
                self.assertEqual(self.verdict(stats, settle, {"applicable": True, "enabled": enabled}), expected)
        for deadline in ({"reason": "settle_deadline_missed"},
                         {"forward_progress": {"deadline_missed": True}}):
            with self.subTest(deadline=deadline):
                self.assertEqual(self.verdict(self.sample(32093, 32090), {**settle, **deadline}),
                                 "checkpoint_settle_deadline_missed")


class CoverageWiring(unittest.TestCase):
    """Gate repair: deleting any wire of the completeness signal turns this red."""

    def test_the_driver_traces_and_settles_on_needed_not_resident(self) -> None:
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        for name in ps.COVERAGE_COLUMNS:
            self.assertIn(f'["{name}", "q"]', gd, name)
            self.assertIn(f'_g(ss, "{name}", -1)', gd, name)
        settled = gd[gd.index("func _settled() -> bool:"):]
        settled = settled[:settled.index("\n\n\n")]
        self.assertIn('int(ss["needed_not_resident_chunks"]) != 0', settled)
        self.assertIn('not ss.has("needed_not_resident_chunks")', settled)
        self.assertNotIn('ss["needed_unserved_chunks"]', settled, "the old oracle reads in-flight chunks as served")

    def test_the_driver_captures_every_checkpoint(self) -> None:
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertRegex(gd, r'var shot := _screenshot\("p%d_%s" % \[pass_index, str\(seg\["checkpoint"\]\)\]\)')
        self.assertIn("img.save_png(dir.path_join(file))", gd)
        self.assertIn('_result["screenshots"]', gd)

    def test_the_engine_publishes_every_coverage_key(self) -> None:
        core = ROOT / "modules" / "gaussian_splatting" / "core"
        keys = (core / "streaming_analytics_frame.h").read_text(encoding="utf-8")
        surface = (core / "streaming_diagnostics_surface.cpp").read_text(encoding="utf-8")
        for name in ps.COVERAGE_COLUMNS:
            self.assertIn(f"X({name})", keys, name)
            self.assertIn(f"analytics[AK::{name}] =", surface, name)
        build = (core / "gaussian_streaming.cpp").read_text(encoding="utf-8")
        body = build[build.index("void GaussianStreamingSystem::_build_visible_chunk_list()"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("scheduler.last_needed_not_resident_chunk_count = needed_not_resident_chunks;", body)

    def test_the_probe_judges_moving_rows_with_the_manifest(self) -> None:
        src = (HERE / "run_probe.py").read_text(encoding="utf-8")
        self.assertIn("summarise_path(trace, run, path, gt, args.passes, manifest)", src)
        self.assertIn('"moving_coverage": ps.moving_coverage(trace, moving, expected_at)', src)

    def test_the_workflow_uploads_the_screenshots(self) -> None:
        wf = (ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml").read_text(encoding="utf-8")
        self.assertIn("name: beyond-vram-screenshots-", wf)
        self.assertIn("path: ${{ runner.temp }}/bvw-out/**/screenshots/", wf)


def _rss_trace(times):
    """Constructed: six-column trace rows at the given t_usec; the placement arithmetic is the point."""
    fields = ["row", "engine_frame", "t_usec", "phase", "pass_index", "segment", "state"]
    rows = [(i, 100 + i, t, 0, 0, 3 if i < 2 else 4, ps.STATE_MOVING) for i, t in enumerate(times)]
    return ps.Trace(fields, "q" * len(fields), rows, 0)


class RssTimeline(unittest.TestCase):
    """#1466: the harness exported only the RSS peak, so a +1.9 GB peak could not be placed in time."""

    GIB = 1 << 30

    def test_the_os_high_water_mark_places_a_peak_no_poll_hit(self) -> None:
        # Constructed sampler rows: the mark jumps to 4 GiB between 0.4 s and 0.6 s, while no polled
        # RSS ever exceeds 2 GiB (the transient was allocated and released between two polls).
        rows = [(0.2, 1 * self.GIB, 1 * self.GIB), (0.4, 2 * self.GIB, 2 * self.GIB),
                (0.6, 2 * self.GIB, 4 * self.GIB), (0.8, 2 * self.GIB, 4 * self.GIB)]
        out = ps.rss_timeline(rows, 1000.0, 0.2)
        self.assertEqual(out["peak_window"]["status"], ps.OK)
        self.assertEqual((out["peak_window"]["after_t_s"], out["peak_window"]["by_t_s"]), (0.4, 0.6))
        self.assertEqual((out["peak_window"]["bytes"], out["peak_window"]["source"]), (4 * self.GIB, "os_high_water_mark"))
        self.assertEqual((out["peak_sampled"]["t_s"], out["peak_sampled"]["rss_bytes"]), (0.4, 2 * self.GIB))
        self.assertEqual(out["timeline"]["points"], [[t, rss] for t, rss, _ in rows])
        self.assertEqual((out["timeline"]["t0_unix_s"], out["timeline"]["source_samples"]), (1000.0, 4))
        # The mark already final at the first sample: the peak lies before it, from process start.
        early = ps.rss_timeline([(0.2, 1, 9), (0.4, 1, 9)], 1000.0, 0.2)["peak_window"]
        self.assertEqual((early["after_t_s"], early["by_t_s"]), (0.0, 0.2))

    def test_without_the_mark_only_the_largest_poll_is_claimed(self) -> None:
        rows = [(0.2, 5, None), (0.4, 9, None), (0.6, 7, None)]
        window = ps.rss_timeline(rows, 1000.0, 0.2)["peak_window"]
        self.assertEqual((window["after_t_s"], window["by_t_s"], window["bytes"], window["source"]),
                         (0.4, 0.4, 9, "polled_sample"))
        # One sample without the mark is enough to stop claiming an OS-located window.
        mixed = ps.rss_timeline([(0.2, 5, 5), (0.4, 9, None)], 1000.0, 0.2)["peak_window"]
        self.assertEqual(mixed["source"], "polled_sample")

    def test_absent_or_malformed_samples_are_missing_never_zero(self) -> None:
        for samples in ([], None, [(0.2, 5)], [(0.2, "5", 5)], [(0.4, 5, 5), (0.2, 6, 6)], [(float("nan"), 5, 5)]):
            with self.subTest(samples=samples):
                out = ps.rss_timeline(samples, 1000.0, 0.2)
                for key in ("timeline", "peak_sampled", "peak_window"):
                    self.assertEqual(out[key]["status"], ps.MISSING, key)
                    self.assertIsNone(out[key]["value"], key)
                    self.assertTrue(out[key]["reason"], key)
                self.assertNotIn("points", out["timeline"])

    def test_the_timeline_is_bounded_and_keeps_each_bucket_maximum(self) -> None:
        rows = [(round(0.2 * (i + 1), 3), 1000 + (i * 37) % 101, 2000) for i in range(5000)]
        rows[1234] = (rows[1234][0], 999_999, 2000)
        out = ps.rss_timeline(rows, 1000.0, 0.2)
        points = out["timeline"]["points"]
        self.assertLessEqual(len(points), ps.RSS_TIMELINE_MAX_POINTS)
        self.assertGreater(len(points), ps.RSS_TIMELINE_MAX_POINTS // 2)
        self.assertIn([rows[1234][0], 999_999], points)
        self.assertEqual(max(p[1] for p in points), max(r[1] for r in rows))
        self.assertEqual([p[0] for p in points], sorted(p[0] for p in points))
        self.assertEqual(out["timeline"]["source_samples"], 5000)

    def location(self, window, times, **result):
        rss = {"peak_window": {"status": ps.OK, "after_t_s": window[0], "by_t_s": window[1], "bytes": 7,
                               "source": "os_high_water_mark"},
               "timeline": {"status": ps.OK, "t0_unix_s": 1000.0, "points": [[0.2, 7]]}}
        # The probe read its clocks 0.5 s after the sampler started: ticks 2,000,000 at unix 1000.5.
        base = {"start_unix_s": 1000.5, "start_usec": 2_000_000, "apply_usec": 2_300_000}
        base.update(result)
        return ps.rss_peak_location(rss, {k: v for k, v in base.items() if v is not None}, _rss_trace(times))

    def test_the_peak_window_maps_onto_the_rows_whose_frames_overlap_it(self) -> None:
        # Sampler 1.0 s..1.2 s is probe ticks 2,500,000..2,700,000.
        times = [2_400_000, 2_550_000, 2_650_000, 2_800_000, 2_900_000]
        record = self.location((1.0, 1.2), times)
        self.assertEqual(record["status"], ps.OK)
        value = record["value"]
        self.assertEqual(value["window_t_usec"], [2_500_000, 2_700_000])
        self.assertEqual(value["position"], "within_trace")
        # Row 1's frame (2.40 s..2.55 s) and row 3's (2.65 s..2.80 s) both overlap; row 0 ended before, row 4 began after.
        self.assertEqual((value["rows"]["count"], value["rows"]["first"]["row"], value["rows"]["last"]["row"]), (3, 1, 3))
        self.assertEqual((value["rows"]["first"]["segment"], value["rows"]["last"]["segment"]), (3, 4))
        self.assertEqual(value["rows"]["first"]["engine_frame"], 101)
        self.assertEqual(value["window_s_after_apply_world"], [0.2, 0.4])
        self.assertEqual((value["peak_bytes"], value["peak_source"]), (7, "os_high_water_mark"))

    def test_a_window_outside_the_trace_is_named_not_assigned_to_a_row(self) -> None:
        times = [2_800_000, 2_900_000]
        before = self.location((1.0, 1.2), times)["value"]
        self.assertEqual((before["position"], before["rows"]), ("before_first_trace_row", {"count": 0}))
        after = self.location((3.0, 3.2), times)["value"]
        self.assertEqual((after["position"], after["rows"]), ("after_last_trace_row", {"count": 0}))
        # A window that ends inside the first row's frame belongs to that row.
        self.assertEqual(self.location((1.0, 1.35), times)["value"]["rows"]["first"]["row"], 0)
        self.assertEqual(self.location((1.0, 1.2), times, apply_usec=None)["value"]["window_s_after_apply_world"],
                         "not_measured")

    def test_no_shared_clock_or_no_rows_is_missing(self) -> None:
        times = [2_400_000, 2_550_000]
        for label, record in (
                ("probe wall clock", self.location((1.0, 1.2), times, start_unix_s=None)),
                ("probe ticks", self.location((1.0, 1.2), times, start_usec=None)),
                ("empty trace", self.location((1.0, 1.2), [])),
                ("no sampler window", ps.rss_peak_location({"peak_window": ps.missing("no RSS sample")}, {}, _rss_trace(times))),
                ("no sampler block", ps.rss_peak_location(None, None, None))):
            with self.subTest(label):
                self.assertEqual(record["status"], ps.MISSING)
                self.assertIsNone(record["value"])
                self.assertTrue(record["reason"])

    def test_the_sampler_exports_the_timeline_of_a_real_process(self) -> None:
        # The real producer: RssSampler polling a live child of this test for about a second.
        with mock.patch.object(rp, "RSS_POLL_S", 0.05):
            proc = subprocess.Popen([sys.executable, "-c", "import time; b = bytearray(32 << 20); time.sleep(1.0)"])
            sampler = rp.RssSampler(proc)
            sampler.start()
            proc.wait(timeout=60)
            sampler.join(timeout=5)
        result = sampler.result()
        if sys.platform not in ("win32",) and not sys.platform.startswith("linux"):
            self.assertEqual(result["timeline"]["status"], ps.MISSING)
            self.assertEqual(result["peak_window"]["status"], ps.MISSING)
            return
        self.assertEqual(result["peak_rss_bytes"]["status"], ps.OK)
        timeline, window = result["timeline"], result["peak_window"]
        self.assertEqual((timeline["status"], window["status"]), (ps.OK, ps.OK))
        self.assertEqual(timeline["source_samples"], result["samples"])
        self.assertGreaterEqual(len(timeline["points"]), 2)
        self.assertLess(abs(timeline["t0_unix_s"] - time.time()), 120.0)
        self.assertEqual(window["source"], "os_high_water_mark")
        self.assertEqual(window["bytes"], result["peak_rss_bytes"]["value"])
        self.assertLessEqual(window["after_t_s"], window["by_t_s"])
        self.assertGreaterEqual(window["bytes"], max(p[1] for p in timeline["points"]))

    def test_a_process_that_was_never_sampled_reports_missing(self) -> None:
        class Gone:
            pid = 0

            def poll(self):
                return 0

        result = rp.RssSampler(Gone()).result()
        self.assertEqual(result["samples"], 0)
        for key in ("peak_rss_bytes", "timeline", "peak_sampled", "peak_window"):
            self.assertEqual(result[key]["status"], ps.MISSING, key)

    def test_the_probe_exports_the_wall_clock_beside_its_ticks(self) -> None:
        # Source pin only: the engine is not run here. The runner run is what proves the value.
        source = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertRegex(source, r'"start_usec": Time\.get_ticks_usec\(\), "start_unix_s": Time\.get_unix_time_from_system\(\)')


class Statistics(unittest.TestCase):
    def test_gsq_decode_split_names_the_dominant_part(self) -> None:
        # Constructed: the split is closed-form arithmetic over four counters.
        stats = {"pages_decoded": 10, "splats_decoded": 40960, "decode_nsec": 70_000_000,
                 "verify_nsec": 8_000_000, "digest_nsec": 50_000_000}
        s = ps.gsq_decode_split(stats)
        self.assertEqual(s["status"], ps.OK)
        self.assertEqual(s["dominant"], "digest")
        self.assertAlmostEqual(s["ms_per_page"]["decode"], 1.2)
        self.assertAlmostEqual(s["ms_per_page"]["verify"], 0.8)
        self.assertAlmostEqual(s["share"]["digest"], 50 / 70)
        self.assertAlmostEqual(sum(s["share"].values()), 1.0)
        self.assertEqual(ps.gsq_decode_split(None)["status"], ps.MISSING)
        self.assertEqual(ps.gsq_decode_split({**stats, "verify_nsec": None})["status"], ps.MISSING)
        self.assertEqual(ps.gsq_decode_split({**stats, "digest_nsec": 63_000_000})["status"], ps.INVALID)
        # Older probes carry no flag; the split says so instead of guessing.
        self.assertIsNone(s["decoded_digest_verified"])
        self.assertIsNone(s["pages_decoded_digest_checked"])

    def test_gsq_decode_split_reports_a_skipped_decoded_digest(self) -> None:
        # BV-K9c: on the streaming path the digest is not computed; the split stays OK with a
        # zero digest share and carries the source's own statement that it was skipped.
        stats = {"pages_decoded": 10, "splats_decoded": 40960, "decode_nsec": 20_000_000,
                 "verify_nsec": 8_000_000, "digest_nsec": 0,
                 "decoded_digest_verified": False, "pages_decoded_digest_checked": 0}
        s = ps.gsq_decode_split(stats)
        self.assertEqual(s["status"], ps.OK)
        self.assertEqual(s["share"]["digest"], 0.0)
        self.assertEqual(s["dominant"], "decode")
        self.assertIs(s["decoded_digest_verified"], False)
        self.assertEqual(s["pages_decoded_digest_checked"], 0)

    def test_time_weighted_percentile(self) -> None:
        # 9 frames of 10 ms and one of 100 ms: the long frame is 100/190 = 53 % of the time.
        vals = [10.0] * 9 + [100.0]
        self.assertEqual(ps.time_weighted_percentile(vals, 0.95), 100.0)
        self.assertEqual(ps.time_weighted_percentile(vals, 0.40), 10.0)
        self.assertEqual(ps.nearest_rank(vals, 0.9), 10.0)
        self.assertIsNone(ps.time_weighted_percentile([], 0.95))

    def test_fit_recovers_f_and_k(self) -> None:
        pts = [{"id": str(n), "drawn_mean": n * 1e6, "y": ps.metric(4.0 + 6.5 * n)} for n in (0.03, 0.25, 1.0, 2.0)]
        fit = ps.fit_frame_model(pts, "y")
        self.assertEqual(fit["status"], ps.OK)
        self.assertAlmostEqual(fit["F_intercept_ms"], 4.0, places=9)
        self.assertAlmostEqual(fit["k_ms_per_million"], 6.5, places=9)

    def test_fit_without_two_points_is_missing(self) -> None:
        pts = [{"id": "a", "drawn_mean": 1e6, "y": ps.metric(5.0)},
               {"id": "b", "drawn_mean": 2e6, "y": ps.missing("no fresh coherent frame")}]
        fit = ps.fit_frame_model(pts, "y")
        self.assertEqual(fit["status"], ps.MISSING)
        self.assertEqual(fit["excluded"][0]["point"], "b")

    def test_metric_never_reports_a_non_finite_value(self) -> None:
        self.assertEqual(ps.metric(float("nan"))["status"], ps.MISSING)
        self.assertEqual(ps.metric(None)["status"], ps.MISSING)

    def test_subset_selection_is_uniform_and_near_target(self) -> None:
        m = manifest()
        total = sum(int(c["count"]) for c in m["chunks"])
        sel = rp.select_subset(m, total // 4)
        got = sum(int(c["count"]) for c in m["chunks"] if int(c["plan_index"]) in set(sel))
        biggest = max(int(c["count"]) for c in m["chunks"])
        self.assertLessEqual(abs(got - total // 4), biggest)
        self.assertEqual(sel, sorted(set(sel)))


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


class EntryPoint(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.binary = self.tmp / "godot.bin"
        self.binary.write_bytes(b"not really godot")
        self.world = self.tmp / "world"
        self.world.mkdir()
        (self.world / rp.WORLD_NAME).write_bytes(b"x")
        (self.world / rp.MANIFEST_NAME).write_text(json.dumps(manifest()), encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def summary(self, out: Path) -> dict:
        return json.loads((out / rp.SUMMARY_NAME).read_text(encoding="utf-8"))

    def test_no_world_is_not_run_with_a_summary(self) -> None:
        out = self.tmp / "out"
        code = rp.main(["--godot-binary", str(self.binary), "--out", str(out)])
        self.assertEqual(code, rp.EXIT_NOT_RUN)
        self.assertEqual(self.summary(out)["status"], "NOT_RUN")

    def test_world_without_manifest_is_not_run(self) -> None:
        (self.world / rp.MANIFEST_NAME).unlink()
        out = self.tmp / "out"
        code = rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}", "--out", str(out)])
        self.assertEqual(code, rp.EXIT_NOT_RUN)

    def test_a_driver_that_leaves_no_result_fails(self) -> None:
        out = self.tmp / "out"
        dead = {"exit_code": 0, "timed_out": False, "wall_s": 0.1, "result": None,
                "rss": {"peak_rss_bytes": ps.missing("x")}, "log": "", "trace": str(self.tmp / "none.bin")}
        with mock.patch.object(rp, "run_godot", return_value=dead):
            code = rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                            "--out", str(out), "--no-sweep"])
        self.assertEqual(code, rp.EXIT_FAIL)
        s = self.summary(out)
        self.assertEqual(s["status"], "FAIL")
        self.assertIn("without a result file", " ".join(s["problems"]))
        # The ground truth is still written, next to the probe output, not into the world.
        self.assertTrue((out / "T-smoke" / gt.GROUNDTRUTH_NAME).is_file())
        self.assertFalse((self.world / gt.GROUNDTRUTH_NAME).exists())

    def test_sweep_without_t0_is_reported_missing(self) -> None:
        out = self.tmp / "out"
        code = rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                        "--out", str(out), "--no-path"])
        self.assertEqual(code, rp.EXIT_INVALID)
        self.assertEqual(self.summary(out)["sweep"]["status"], ps.MISSING)

    def test_captured_path_run_gives_a_complete_summary(self) -> None:
        trace, result = captured("tsmoke_path", self.tmp)
        # Entry-point policy replay; the raw historical receipt is tested above.
        result = admissible_checkpoint_policy(result)
        run = {"exit_code": 0, "timed_out": False, "wall_s": 1.0, "result": result,
               "rss": {"peak_rss_bytes": ps.metric(123, unit="bytes"), "samples": 1}, "log": "", "trace": str(trace)}
        out = self.tmp / "out"
        with mock.patch.object(rp, "run_godot", return_value=run):
            code = rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                            "--out", str(out), "--no-sweep", "--passes", "1"])
        s = self.summary(out)
        self.assertEqual(code, rp.EXIT_OK, s["problems"])
        self.assertEqual(s["worlds"]["T-smoke"]["trace"]["status"], ps.OK)
        self.assertFalse(s["settings_drifted"])

    def test_regulator_drift_in_moving_frames_makes_the_run_invalid(self) -> None:
        """ADR V7 / C-settings: the M1 Tier A repeat (run 42427296940) labelled settings_drifted
        while every V7 status said OK and no problems line named it."""
        trace, result = captured("tsmoke_path", self.tmp)
        tr = ps.read_trace(trace)
        moving = ps.rows_where(tr, state=ps.STATE_MOVING)
        self.assertTrue(moving)
        cut = moving[len(moving) // 2]

        def regulate(i, v, ix):
            if i >= cut:
                v[ix["effective_settings_revision"]] += 1
                v[ix["vram_regulation_adjustments"]] += 1
                v[ix["effective_max_chunks"]] -= 1

        drifted = self.tmp / "drifted.bin"
        rewrite_rows(trace, drifted, regulate)
        run = {"exit_code": 0, "timed_out": False, "wall_s": 1.0, "result": result,
               "rss": {"peak_rss_bytes": ps.metric(123, unit="bytes"), "samples": 1}, "log": "", "trace": str(drifted)}
        out = self.tmp / "out"
        with mock.patch.object(rp, "run_godot", return_value=run):
            code = rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                            "--out", str(out), "--no-sweep", "--passes", "1"])
        s = self.summary(out)
        self.assertEqual(code, rp.EXIT_INVALID, s["problems"])
        self.assertTrue(s["settings_drifted"])
        self.assertEqual(s["worlds"]["T-smoke"]["flythrough"]["passes"][0]["settings"]["status"], ps.INVALID)
        self.assertTrue(any("V7 INVALID settings_drifted" in p for p in s["problems"]), s["problems"])

    def test_the_path_plan_asks_for_checkpoint_screenshots(self) -> None:
        out = self.tmp / "out"
        seen = {}

        def fake_run(binary, plan, workdir, timeout_s):
            seen.setdefault("plan", plan)
            return {"exit_code": 0, "timed_out": False, "wall_s": 0.1, "result": None,
                    "rss": {"peak_rss_bytes": ps.missing("x")}, "log": "", "trace": str(self.tmp / "none.bin")}

        with mock.patch.object(rp, "run_godot", side_effect=fake_run):
            rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                     "--out", str(out), "--no-sweep"])
        shots = seen["plan"]["screenshots"]
        self.assertEqual(Path(shots["dir"]), (out / "T-smoke" / rp.SCREENSHOT_DIR).resolve())
        self.assertEqual(shots["moving_every_n"], 0, "moving captures stall frames: off unless asked for")
        with mock.patch.object(rp, "run_godot", side_effect=fake_run):
            seen.clear()
            rp.main(["--godot-binary", str(self.binary), "--world", f"T-smoke={self.world}",
                     "--out", str(out), "--no-sweep", "--screenshot-moving-every", "30"])
        self.assertEqual(seen["plan"]["screenshots"]["moving_every_n"], 30)

    def test_dispatch_lane_contract(self) -> None:
        """BV-06a calls exactly this entry point with --godot-binary, --world TIER=DIR, --out."""
        lane = (HERE / "dispatch_lane.py").read_text(encoding="utf-8")
        self.assertIn('PROBE_ENTRY = Path("tests/runtime/flythrough/run_probe.py")', lane)
        self.assertIn('PROBE_SUMMARY_NAME = "summary.json"', lane)
        self.assertEqual(rp.SUMMARY_NAME, "summary.json")

    def _sweep_runs(self, held_point=None, unsettled_point=None, spread_drawn=False, blind=None):
        """run_godot replacement for run_sweep: every point replays the captured sweep run;
        held_point gets a trace whose coherent GPU sample never refreshes.

        spread_drawn gives each point its own drawn count, so that the fits are OK and the
        exit code rests on the per-point checks alone. blind = (point, view, column) also
        writes column as -1, what the driver records when the signal is absent, in that
        view's measured rows.
        """
        trace, result = captured("tsmoke_sweep", self.tmp)
        held = self.tmp / "held_sweep.bin"
        tr = ps.read_trace(trace)
        r0 = tr.rows[0]

        def hold(i, v, ix):
            v[ix["coherent_resolve_usec"]] = r0[ix["coherent_resolve_usec"]]
            v[ix["coherent_frame_serial"]] = r0[ix["coherent_frame_serial"]]

        rewrite_rows(trace, held, hold)

        def spread(pid):
            factor = 1 + [p for p, _ in rp.SWEEP_POINTS].index(pid)
            window = range(0)
            if blind and blind[0] == pid:
                entry = next(e for e in result["sweep"] if e["view"] == blind[1])
                window = range(entry["first_row"], entry["last_row"] + 1)

            def edit(i, v, ix):
                v[ix["drawn_splats"]] *= factor
                if i in window:
                    v[ix[blind[2]]] = -1

            dst = self.tmp / f"spread_{pid}.bin"
            rewrite_rows(trace, dst, edit)
            return dst

        def fake(binary, plan, workdir, timeout_s):
            pid = Path(workdir).name
            res = copy.deepcopy(result)
            if pid == unsettled_point:
                for e in res["sweep"]:
                    e["settle"] = {"settled": False, "settle_usec": -1, "reason": "settle_deadline_missed"}
            return {"exit_code": 0, "timed_out": False, "wall_s": 1.0, "result": res,
                    "rss": {"peak_rss_bytes": ps.metric(1, unit="bytes")}, "log": "",
                    "trace": str(spread(pid) if spread_drawn else held if pid == held_point else trace)}

        return fake

    def _run_sweep_main(self, **kw):
        out = self.tmp / "out"
        with mock.patch.object(rp, "run_godot", side_effect=self._sweep_runs(**kw)), \
                mock.patch.object(rp, "generate_subset", return_value=None):
            code = rp.main(["--godot-binary", str(self.binary), "--world", f"T0={self.world}",
                            "--out", str(out), "--no-path"])
        return code, self.summary(out)

    def test_complete_sweep_points_raise_no_point_problem(self) -> None:
        # Every point replays the same capture, so the fits have one distinct N and are
        # MISSING here; what this pins is that complete points add no per-point problem.
        _, s = self._run_sweep_main()
        self.assertEqual(rp.required_sweep_points(s["sweep"]), [])
        self.assertFalse(any("point " in p for p in s["problems"]), s["problems"])

    def test_f_small_without_coherent_samples_is_invalid(self) -> None:
        code, s = self._run_sweep_main(held_point="F_small")
        self.assertEqual(code, rp.EXIT_INVALID)
        self.assertTrue(any("F_small" in p and "gpu_pipeline_mean_ms MISSING" in p for p in s["problems"]), s["problems"])
        # The partial fit over the remaining points is kept for diagnostics.
        fit = s["sweep"]["fits"]["overview"]["gpu_pipeline"]
        self.assertIn("F_small", [e["point"] for e in fit["excluded"]])

    def test_unsettled_sweep_point_is_invalid(self) -> None:
        code, s = self._run_sweep_main(unsettled_point="1M")
        self.assertEqual(code, rp.EXIT_INVALID)
        self.assertTrue(any("1M" in p and "settle_deadline_missed" in p for p in s["problems"]), s["problems"])

    def test_sweep_view_without_settings_signal_is_invalid(self) -> None:
        """#1334: drift that cannot be evaluated is not "no drift". The control is a run the
        lane calls OK, so the verdict below can only come from the removed signal."""
        code, s = self._run_sweep_main(spread_drawn=True)
        self.assertEqual(code, rp.EXIT_OK, s["problems"])
        for column in ("effective_settings_revision", "vram_regulation_adjustments"):
            with self.subTest(column=column):
                code, s = self._run_sweep_main(spread_drawn=True, blind=("1M", "overview", column))
                self.assertEqual(code, rp.EXIT_INVALID, s["problems"])
                self.assertEqual(len(s["problems"]), 1, s["problems"])
                self.assertIn("point 1M view overview: settings status MISSING", s["problems"][0])
                self.assertIn("absent from every measured row", s["problems"][0])

    def test_relative_world_dir_reaches_godot_as_an_absolute_path(self) -> None:
        plans = []

        def fake(binary, plan, workdir, timeout_s):
            plans.append(plan)
            return {"exit_code": 0, "timed_out": False, "wall_s": 0.1, "result": None,
                    "rss": {"peak_rss_bytes": ps.missing("x")}, "log": "", "trace": ""}

        cwd = os.getcwd()
        os.chdir(self.tmp)
        try:
            with mock.patch.object(rp, "run_godot", side_effect=fake):
                rp.main(["--godot-binary", str(self.binary), "--world", "T-smoke=world",
                         "--out", str(self.tmp / "out"), "--no-sweep"])
        finally:
            os.chdir(cwd)
        self.assertTrue(plans)
        self.assertTrue(Path(plans[0]["world"]).is_absolute(), plans[0]["world"])
        self.assertEqual(Path(plans[0]["world"]), (self.world / rp.WORLD_NAME).resolve())

    def test_guard_lane_runs_this_file(self) -> None:
        text = (ROOT / "tests" / "ci" / "run_module_tests.py").read_text(encoding="utf-8")
        self.assertIn('"test_probe_lane.py"', text)
        self.assertIn("_run_beyond_vram_probe_lane_tests,", text)


class D35Telemetry(unittest.TestCase):
    """Required evidence, independent of flythrough_verdict and release gates.

    Positive cases ONLY consume the real C++ captures documented in d3_5/README.
    Constructed dictionaries below are deliberately incomplete/invalid inputs.
    These tuples prescribe the D3-5 measurement contract, not a coverage inventory.
    """

    COMMON_REQUIRED = (
        "route", "source_generation", "collection_epoch", "captures", "captures_failed",
        "captures_on_worker_threads", "captures_on_main_thread", "pages_decoded", "splats_decoded",
        "pages_decoded_on_worker_threads", "pages_decoded_on_main_thread", "page_failures",
        "last_failure_reason", "stored_bytes_read", "read_requests", "read_failures",
        "read_latency_nsec", "read_queue_nsec", "verify_nsec", "verify_scope", "decode_nsec",
        "decode_remainder_nsec", "digest_nsec", "digest_status", "pages_decoded_digest_checked",
        "pages_decoded_digest_skipped", "stage_time_scope",
    )
    HLOD_REQUIRED = (
        "pack_calls", "pack_nsec", "packed_bytes", "packed_bytes_peak", "ready_bytes", "ready_bytes_peak",
        "mailbox_bytes", "mailbox_bytes_peak", "active_bytes", "active_bytes_peak",
        "stored_bytes", "stored_bytes_peak", "shadow_bytes", "shadow_bytes_peak",
        "queue_residence_nsec", "queue_residence_samples", "upload_bytes", "upload_bytes_this_frame",
        "upload_slices", "upload_completions", "upload_completion_nsec",
    )

    def producer(self, name: str) -> dict:
        path = HERE / "fixtures" / "d3_5" / name
        self.assertTrue(path.is_file(), f"Required real producer capture absent: {path}. "
                        "Capture the strict D3Telemetry cases on CI; never synthesize or skip it.")
        return json.loads(path.read_text(encoding="utf-8"))

    def hlod(self, key="completed"):
        report = self.producer("d3_5_hlod_test_results.json")
        generation_key = "replacement_source_generation" if key == "replaced" else "source_generation"
        result = report[key]  # verbatim renderer Dictionary serialized by C++
        self.assertIn("payload_telemetry", result, "HLOD result JSON must carry its explicit telemetry block")
        return result["payload_telemetry"], report[generation_key]

    def chunk(self, digest="skipped"):
        record = self.producer("d3_5_chunk_test_results.json")[digest]
        return record["payload_telemetry"], record["source_generation"]

    def summarize(self, block, generation, route="hlod"):
        return ps.route_telemetry_summary(block, route=route, source_generation=generation)

    def test_real_hlod_result_json_is_ok_with_worker_gsq_work(self):
        block, generation = self.hlod()
        result = self.summarize(block, generation)
        self.assertEqual(result["status"], ps.OK, result)
        self.assertEqual(result["route"], "hlod")
        self.assertEqual(result["collection_epoch"], generation)
        self.assertGreater(result["pages_decoded"], 0)
        self.assertGreater(result["splats_decoded"], 0)
        self.assertGreater(result["captures_on_worker_threads"], 0)
        self.assertEqual(block["stage_time_scope"], "worker_cpu_sum")
        for key in self.COMMON_REQUIRED + self.HLOD_REQUIRED:
            with self.subTest(key=key):
                self.assertIn(key, block)

    def test_real_chunk_checked_and_skipped_blocks_remain_chunk(self):
        for digest in ("checked", "skipped"):
            with self.subTest(digest=digest):
                block, generation = self.chunk(digest)
                result = self.summarize(block, generation, "chunk")
                self.assertEqual(result["status"], ps.OK, result)
                self.assertEqual(result["route"], "chunk")
                self.assertEqual(result["digest"]["status"], digest)
                if digest == "checked":
                    self.assertGreater(result["digest"]["nsec"], 0)
                else:
                    self.assertIsNone(result["digest"]["nsec"], "skipped work is not a 0 ns measurement")

    def test_missing_block_and_missing_keys_are_missing(self):
        self.assertEqual(self.summarize(None, 7)["status"], ps.MISSING)
        self.assertEqual(self.summarize({}, 7)["status"], ps.MISSING)
        for route, get_block, required in (
            ("hlod", self.hlod, self.COMMON_REQUIRED + self.HLOD_REQUIRED),
            ("chunk", self.chunk, self.COMMON_REQUIRED),
        ):
            block, generation = get_block()
            for key in required:
                with self.subTest(route=route, missing=key):
                    mutated = copy.deepcopy(block)
                    del mutated[key]
                    self.assertEqual(self.summarize(mutated, generation, route)["status"], ps.MISSING)

    def test_null_and_nonnumeric_required_measurements_are_missing(self):
        block, generation = self.hlod()
        for key in ("captures", "pages_decoded", "read_latency_nsec", "pack_nsec", "upload_slices"):
            for value in (None, True, "0"):
                with self.subTest(key=key, value=value):
                    mutated = {**block, key: value}
                    self.assertEqual(self.summarize(mutated, generation)["status"], ps.MISSING)

    def test_stale_epoch_is_invalid_before_work_is_considered(self):
        # Malformed: stale scope may never be interpreted as current zero work.
        for block in (
            {"route": "hlod", "source_generation": 6, "collection_epoch": 6},
            {"route": "hlod", "source_generation": 7, "collection_epoch": 6},
        ):
            with self.subTest(block=block):
                self.assertEqual(self.summarize(block, 7)["status"], ps.INVALID)

    def test_replacement_epoch_cannot_relabel_previous_measurements(self):
        old, old_generation = self.hlod()
        new, new_generation = self.hlod("replaced")
        self.assertNotEqual(old_generation, new_generation)
        self.assertEqual(self.summarize(new, new_generation)["status"], ps.OK)
        self.assertEqual(self.summarize(old, new_generation)["status"], ps.INVALID)
        self.assertEqual(old["pages_decoded"], 1)
        self.assertEqual(new["pages_decoded"], 1, "old-source totals must not be relabelled as a new collection")

    def test_zero_work_or_zero_worker_captures_is_missing(self):
        block, generation = self.hlod()
        for key in ("captures", "captures_on_worker_threads", "pages_decoded", "splats_decoded",
                    "pages_decoded_on_worker_threads"):
            with self.subTest(key=key):
                self.assertEqual(self.summarize({**block, key: 0}, generation)["status"], ps.MISSING)

    def test_failures_negative_counters_and_inconsistent_stages_are_invalid(self):
        block, generation = self.hlod()
        mutations = (
            {"captures_failed": 1}, {"page_failures": 1}, {"read_failures": 1},
            {"last_failure_reason": "gsq_corrupt_page"}, {"verify_nsec": -1},
            {"decode_remainder_nsec": block["decode_nsec"] + 1},
            {"captures_on_worker_threads": block["captures"] + 1},
            {"pages_decoded_on_main_thread": block["pages_decoded"] + 1},
            {"pages_decoded_digest_checked": block["pages_decoded"] + 1},
            {"ready_bytes_peak": -1}, {"ready_bytes": block["ready_bytes_peak"] + 1},
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assertEqual(self.summarize({**block, **mutation}, generation)["status"], ps.INVALID)

    def test_skipped_digest_is_labelled_and_never_zero_ns_free(self):
        block, generation = self.chunk("skipped")
        result = self.summarize(block, generation, "chunk")
        self.assertEqual(result["status"], ps.OK, result)
        self.assertEqual(result["digest"], {"status": "skipped", "nsec": None})
        for mutation in ({"digest_status": "checked"}, {"digest_nsec": 0}):
            with self.subTest(mutation=mutation):
                self.assertEqual(self.summarize({**block, **mutation}, generation, "chunk")["status"], ps.INVALID)

    def test_unavailable_reference_digest_has_a_distinct_counter_contract(self):
        # Counter-policy unit input based on the old checked capture; these
        # alterations are not presented as a new native producer capture.
        block, generation = self.chunk("checked")
        block = {**block, "digest_status": "not_available", "digest_nsec": None,
                 "decode_remainder_nsec": block["decode_remainder_nsec"] + block["digest_nsec"],
                 "pages_decoded_digest_checked": 0, "pages_decoded_digest_skipped": 0,
                 "pages_decoded_digest_unavailable": block["pages_decoded"]}
        result = self.summarize(block, generation, "chunk")
        self.assertEqual(result["status"], ps.OK, result)
        self.assertEqual(result["digest"], {"status": "not_available", "nsec": None})
        for mutation in ({"pages_decoded_digest_unavailable": 0},
                         {"pages_decoded_digest_unavailable": True},
                         {"digest_status": "skipped"}, {"digest_nsec": 0}):
            with self.subTest(mutation=mutation):
                self.assertNotEqual(self.summarize({**block, **mutation}, generation, "chunk")["status"], ps.OK)

    def test_checked_and_unavailable_reference_digests_are_mixed(self):
        block, generation = self.chunk("checked")
        # Constructed counter contract, not a claimed producer fixture.
        block = {**block, "digest_status": "mixed", "pages_decoded_digest_unavailable": 1,
                 "pages_decoded": block["pages_decoded"] + 1,
                 "pages_decoded_on_worker_threads": block["pages_decoded_on_worker_threads"] + 1}
        self.assertEqual(self.summarize(block, generation, "chunk")["status"], ps.OK)
        self.assertEqual(self.summarize({**block, "digest_status": "checked"}, generation, "chunk")["status"], ps.INVALID)

    def test_mixed_unverified_reference_work_has_no_measured_digest_duration(self):
        block, generation = self.chunk("skipped")
        # Contract-only counter mutation: skipped and unavailable are two
        # different kinds of unperformed work, neither has measured hash time.
        block = {**block, "digest_status": "mixed", "pages_decoded_digest_unavailable": 1,
                 "pages_decoded": block["pages_decoded"] + 1,
                 "pages_decoded_on_worker_threads": block["pages_decoded_on_worker_threads"] + 1}
        result = self.summarize(block, generation, "chunk")
        self.assertEqual(result["status"], ps.OK, result)
        self.assertEqual(result["digest"], {"status": "mixed", "nsec": None})
        self.assertEqual(self.summarize({**block, "digest_nsec": 0}, generation, "chunk")["status"], ps.INVALID)

    def test_chunk_cannot_supply_required_hlod_evidence(self):
        # Malformed route identity fails even before the absent stages are read.
        self.assertEqual(self.summarize({"route": "chunk", "source_generation": 7,
                                         "collection_epoch": 7}, 7)["status"], ps.INVALID)
        hlod, generation = self.hlod()
        self.assertEqual(self.summarize(hlod, generation, "chunk")["status"], ps.INVALID)
        self.assertEqual(self.summarize(None, generation)["status"], ps.MISSING)
        # The old probe's chunk-shaped gsq_decode_stats is not an HLOD block.
        old_result = json.loads((FIX / "tsmoke_path.result.json").read_text(encoding="utf-8"))
        self.assertEqual(self.summarize(old_result.get("payload_telemetry"), generation)["status"], ps.MISSING)

    def test_unmeasured_vram_is_distinct_from_measured_zero(self):
        block, generation = self.hlod()
        self.assertEqual(self.summarize({**block, "gpu_memory_usage_mb": None}, generation)["status"], ps.OK)
        self.assertEqual(self.summarize({**block, "gpu_memory_usage_mb": 0}, generation)["status"], ps.INVALID)
        self.assertEqual(self.summarize({**block, "gpu_memory_usage_mb": 0,
                                        "gpu_memory_status": "not_measured"}, generation)["status"], ps.INVALID)

    def test_export_wiring_removal_is_missing(self):
        report = self.producer("d3_5_hlod_test_results.json")
        result = copy.deepcopy(report["completed"])
        self.assertEqual(self.summarize(result.get("payload_telemetry"), report["source_generation"])["status"], ps.OK)
        del result["payload_telemetry"]  # Remove the actual export, not a helper's validation.
        self.assertEqual(self.summarize(result.get("payload_telemetry"), report["source_generation"])["status"], ps.MISSING)

    def test_probe_summary_wires_hlod_result_to_route_validator(self):
        # Malformed-input replay of a REAL probe result: only the declared route
        # is changed; its missing HLOD block must survive as MISSING in summary.
        with tempfile.TemporaryDirectory() as tmp:
            trace_path, result = captured("tsmoke_path", Path(tmp))
            result.update(route="hlod", source_generation=7)
            result.pop("payload_telemetry", None)
            m = manifest()
            path = pp.resolve_path(m)
            truth = gt.build_groundtruth(m, path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
            with mock.patch.object(ps, "route_telemetry_summary", wraps=ps.route_telemetry_summary) as validator:
                summary = rp.summarise_path(ps.read_trace(trace_path), {"result": result}, path, truth, 1)
            self.assertTrue("payload_telemetry" in summary, "the actual probe summarizer must preserve required route evidence")
            self.assertEqual(summary["payload_telemetry"]["status"], ps.MISSING)
            validator.assert_called_once_with(None, route="hlod", source_generation=7)


class RuntimeNodeWorkTelemetryPolicy(unittest.TestCase):
    """Constructed optional-counter policy; legacy producer captures stay unchanged."""

    def sample(self):
        capture = json.loads((HERE / "fixtures/d3_5/d3_5_hlod_test_results.json").read_text(encoding="utf-8"))
        block = copy.deepcopy(capture["completed"]["payload_telemetry"])
        # These values test the consumer contract, not newly captured engine work.
        block.update(record_verify_nsec=block["decode_nsec"] + 1,
                     record_verify_scope="v4_record_envelope_sha256_and_optional_payload_validation_worker_cpu_sum",
                     node_load_nsec=3 * block["decode_nsec"],
                     node_load_scope="successful_runtime_node_load_wall_sum_including_io_wait", nodes_loaded=2)
        return block, capture["source_generation"]

    def test_optional_record_work_stays_outside_the_decode_span(self):
        block, generation = self.sample()
        result = ps.route_telemetry_summary(block, route="hlod", source_generation=generation)
        self.assertEqual(result["status"], ps.OK, result)
        for key in ("record_verify_nsec", "record_verify_scope", "node_load_nsec", "node_load_scope", "nodes_loaded"):
            self.assertEqual(result.get(key), block[key])
        without_node_work = {k: v for k, v in block.items() if k not in (
            "record_verify_nsec", "record_verify_scope", "node_load_nsec", "node_load_scope", "nodes_loaded")}
        self.assertEqual(ps.gsq_decode_split(result), ps.gsq_decode_split(without_node_work))
        legacy = ps.route_telemetry_summary(without_node_work, route="hlod", source_generation=generation)
        self.assertEqual(legacy["status"], ps.OK, legacy)
        for key in ("record_verify_nsec", "node_load_nsec", "nodes_loaded"):
            self.assertNotIn(key, legacy, "an old capture must not acquire a zero measurement")

    def test_optional_record_work_rejects_invalid_counters_and_missing_scopes(self):
        for key in ("record_verify_nsec", "node_load_nsec", "nodes_loaded"):
            for value in (-1, True, 0.5, None):
                block, generation = self.sample()
                block[key] = value
                with self.subTest(key=key, value=value):
                    self.assertEqual(ps.route_telemetry_summary(block, route="hlod", source_generation=generation)["status"], ps.INVALID)
        for key in ("record_verify_scope", "node_load_scope"):
            block, generation = self.sample()
            del block[key]
            with self.subTest(key=key):
                self.assertEqual(ps.route_telemetry_summary(block, route="hlod", source_generation=generation)["status"], ps.INVALID)


class D36ObservationPolicy(unittest.TestCase):
    """Constructed validation-policy cases, NOT captures of new C++ exports.

    Real D3-5 captures above remain untouched. C++ doctests exercise the new
    export wiring; these cases test the Python policy for bounds/row coverage.
    """

    def queues(self, route):
        result = {}
        for name in ("retained", "ready"):
            result[name] = {"owner": ("hlod_shared_reservation_ledger" if route == "hlod"
                                      else "payload_read_service" if name == "retained" else "chunk_upload_window"),
                            "bytes": 4, "peak_bytes": 8, "byte_bound": 32,
                            "jobs": 1, "peak_jobs": 2, "job_bound": 4}
            absent = "job_bound" if route == "hlod" or name == "retained" else "byte_bound"
            result[name][absent] = None
            result[name][absent + "_reason"] = "no independent limit; admission uses the other quantity"
            if route == "hlod":
                result[name].update(peak_byte_bound=32, bound_violations=0)
        return result

    def test_route_owner_limits_are_scoped_without_inventing_independent_limits(self):
        for route in ("chunk", "hlod"):
            policy = self.queues(route)
            summary = ps.queue_observations_summary(policy, route=route)
            self.assertEqual(summary["status"], ps.OK, summary)
            self.assertEqual(summary["retained"], policy["retained"])
            self.assertIsNone(summary["retained"]["job_bound"])

    def test_missing_real_bounds_or_occupancy_and_forged_owner_fail_closed(self):
        for route in ("chunk", "hlod"):
            for name in ("retained", "ready"):
                for key in (("owner", "bytes", "peak_bytes", "jobs", "peak_jobs", "byte_bound", "job_bound")
                            + (("peak_byte_bound", "bound_violations") if route == "hlod" else ())):
                    policy = self.queues(route)
                    del policy[name][key]
                    with self.subTest(route=route, owner=name, key=key):
                        self.assertNotEqual(ps.queue_observations_summary(policy, route=route)["status"], ps.OK)
            policy = self.queues(route)
            policy["retained"]["owner"] = "invented owner"
            self.assertEqual(ps.queue_observations_summary(policy, route=route)["status"], ps.INVALID)

    def test_unbounded_real_limit_and_missing_reason_are_not_evidence(self):
        for mutation in (lambda q: q["retained"].update(byte_bound=None, byte_bound_reason="configured unbounded"),
                         lambda q: q["retained"].pop("job_bound_reason"),
                         lambda q: q["ready"].update(jobs=3, peak_jobs=2)):
            policy = self.queues("hlod")
            mutation(policy)
            self.assertNotEqual(ps.queue_observations_summary(policy, route="hlod")["status"], ps.OK)

    def test_new_counter_wiring_cannot_disappear_from_the_telemetry_validator(self):
        capture = json.loads((HERE / "fixtures/d3_5/d3_5_hlod_test_results.json").read_text(encoding="utf-8"))
        # Keep the real stage capture unchanged; appended records are explicitly
        # the new observation policy under test, not a replacement engine fixture.
        block = copy.deepcopy(capture["completed"]["payload_telemetry"])
        counters = ("stale_generation_publications", "digest_verification_failures",
                    "reads_refused_busy", "admission_waits", "upload_hold_frames")
        block.update({key: 0 for key in counters}, queue_observations=self.queues("hlod"))
        generation = capture["source_generation"]
        self.assertEqual(ps.route_telemetry_summary(block, route="hlod", source_generation=generation)["status"], ps.OK)
        for key in counters:
            missing_counter = copy.deepcopy(block)
            del missing_counter[key]
            with self.subTest(key=key):
                self.assertEqual(ps.route_telemetry_summary(missing_counter, route="hlod",
                    source_generation=generation)["status"], ps.MISSING)
        for key in counters[:2]:
            broken = {**block, key: 1}
            self.assertEqual(ps.route_telemetry_summary(broken, route="hlod", source_generation=generation)["status"], ps.INVALID)

    def settings(self):
        # A settings policy with two observed rows; not a fabricated trace fixture.
        sample = {"project_settings_readback": {"fixed_policy": True},
                  "node_readback": {"max_splat_count": 4194304},
                  "camera": {"fov_deg": 107.5}, "resolution": [1280, 720],
                  "sh_bands": 3, "visible_splats": 4194304, "drawn_splats": 4194304}
        return [{"row": row, **copy.deepcopy(sample)} for row in (1, 2)]

    def test_per_row_hlod_settings_can_be_proven_without_chunk_regulator(self):
        summary = ps.shared_layers_settings_summary(self.settings(), [1, 2])
        self.assertEqual(summary["status"], ps.OK, summary)
        self.assertIs(summary["settings_drifted"], False)

    def test_missing_duplicate_or_changed_settings_rows_fail_closed(self):
        for mutation in (lambda s: s.pop(), lambda s: s.append(copy.deepcopy(s[0])),
                         lambda s: s[1].pop("node_readback"),
                         lambda s: s[1]["camera"].update(fov_deg=90),
                         lambda s: s[1].update(sh_bands=0)):
            samples = self.settings()
            mutation(samples)
            self.assertNotEqual(ps.shared_layers_settings_summary(samples, [1, 2])["status"], ps.OK)


class HlodProbe(unittest.TestCase):
    def extractor(self):
        self.assertTrue(callable(getattr(rp, "hlod_summary", None)), "HLOD extraction is not implemented")
        return rp.hlod_summary

    def test_route_label_alone_and_chunk_capture_cannot_prove_hlod(self):
        extract = self.extractor()
        report = json.loads((HERE / "fixtures/d3_5/d3_5_hlod_test_results.json").read_text())
        # Captured payload stages are real; neither these CPU stages nor a label
        # provide the native streaming flags and fresh checkpoint render evidence.
        result = {"route": "hlod", "source_generation": report["source_generation"],
                  "payload_telemetry": report["completed"]["payload_telemetry"]}
        self.assertNotEqual(extract(result, None, {})["route_evidence"]["status"], ps.OK)
        with tempfile.TemporaryDirectory() as tmp:
            path, result = captured("tsmoke_path", Path(tmp))
            self.assertNotEqual(extract(result, ps.read_trace(path), {})["route_evidence"]["status"], ps.OK)

    def test_route_policy_requires_every_native_flag_epoch_and_fresh_count(self):
        extract = self.extractor()
        report = json.loads((HERE / "fixtures/d3_5/d3_5_hlod_test_results.json").read_text())
        # Constructed consumer-policy input, not a claimed GPU capture. Native
        # payload encoding is copied verbatim from its captured producer.
        migration = json.loads((HERE / "fixtures/d3_6/run43602195987_migration.json").read_text())["arms"]["candidate"]
        drawn = migration["payload_behavior"]["drawn_counts"][0]
        cp = {"actual_route": "hlod", "drawn_splats": drawn["drawn_splats"], "hlod_drawn_nodes": drawn["drawn_nodes"],
              "pose_first_frame": 1, "rendered_splat_count_source_frame": 2, "render_frame_counter": 3}
        stats = copy.deepcopy(migration["final_render_stats"])
        result = {"route": "hlod", "source_generation": report["source_generation"],
                  "payload_telemetry": report["completed"]["payload_telemetry"],
                  "final_render_stats": stats, "checkpoints": [cp]}
        self.assertEqual(extract(result, None, {})["route_evidence"]["status"], ps.OK)
        mutations = [{"final_render_stats": {**stats, key: False}}
                     for key in ("hlod_active", "hlod_streaming", "hlod_stream_paged")]
        mutations += [{"source_generation": -1}, {"checkpoints": []},
                      {"checkpoints": [{**cp, "rendered_splat_count_source_frame": 0}]},
                      {"checkpoints": [{**cp, "hlod_drawn_nodes": 0}]},
                      {"checkpoints": [{**cp, "drawn_splats": -1}]}]
        for change in mutations:
            with self.subTest(change=change):
                self.assertNotEqual(extract({**result, **change}, None, {})["route_evidence"]["status"], ps.OK)
        diag = extract(result, None, {})["diagnostics"]
        for key in ("hlod_route_owned_bytes", "hlod_bundle_device_bytes"):
            self.assertEqual(diag[key]["status"], ps.MISSING)
        self.assertEqual(diag["configured_budget"]["value"], 4096 * 1024**2)
        self.assertIn("vram_budget_mb", diag["configured_budget"]["source"])
        self.assertEqual(diag["hlod_ledger_limit_bytes"]["status"], ps.MISSING)
        result["final_render_stats"]["device_vram_ledger"] = {"limit_bytes": 321}
        native_budget = extract(result, None, {})["diagnostics"]["hlod_ledger_limit_bytes"]
        self.assertEqual(native_budget["value"], 321)
        self.assertIn("gs_limit_for_device", native_budget["source"])
        for value in (None, -1):
            result["shared_layers_peaks"] = {"hlod_route_owned_bytes": value}
            self.assertEqual(extract(result, None, {})["diagnostics"]["hlod_route_owned_bytes"]["status"], ps.MISSING)
        result["shared_layers_peaks"] = {"hlod_route_owned_bytes": 123, "hlod_bundle_device_bytes": 456}
        self.assertEqual(extract(result, None, {})["diagnostics"]["hlod_route_owned_bytes"]["value"], 123)

    def test_chunk_pin_matches_immutable_base_without_hlod_keys(self):
        import types
        base = "9faecfb155a"
        code = subprocess.check_output(["git", "show", f"{base}:tests/runtime/flythrough/run_probe.py"], cwd=ROOT, text=True)
        old = types.ModuleType("m32_base_probe")
        old.__file__ = str(HERE / "run_probe.py")
        exec(compile(code, old.__file__, "exec"), old.__dict__)
        path = pp.resolve_path(manifest())
        plan = rp.path_plan(Path("world.gsplatworld"), path, 2)
        self.assertEqual(plan, old.path_plan(Path("world.gsplatworld"), path, 2))
        self.assertNotIn("shared_layers_t0", plan)
        with tempfile.TemporaryDirectory() as tmp:
            trace_path, result = captured("tsmoke_path", Path(tmp))
            trace = ps.read_trace(trace_path)
            truth = gt.build_groundtruth(manifest(), path, pp.checkpoints(path), rp.CAMERA, rp.ASPECT)
            run = {"result": result}
            current = rp.summarise_path(trace, run, path, truth, 1, manifest())
            self.assertEqual(current, old.summarise_path(trace, run, path, truth, 1, manifest()))
            for key in ("hlod", "hlod_route_owned_bytes", "hlod_bundle_device_bytes"):
                self.assertNotIn(key, current)
            # Replay the complete default CLI against the immutable implementation.
            tmp = Path(tmp)
            world = tmp / "world"
            world.mkdir()
            (world / rp.WORLD_NAME).write_bytes(b"consumer replay")
            (world / rp.MANIFEST_NAME).write_text(json.dumps(manifest()))
            binary = tmp / "godot"
            binary.write_bytes(b"binary")
            replay = {"exit_code": 0, "timed_out": False, "wall_s": 1., "log": "", "trace": str(trace_path),
                      "result": admissible_checkpoint_policy(result), "rss": {"peak_rss_bytes": ps.metric(123)}}
            summaries = []
            for module in (old, rp):
                with mock.patch.object(module, "run_godot", return_value=replay):
                    self.assertEqual(module.main(["--godot-binary", str(binary), "--world", f"T-smoke={world}",
                                                  "--out", str(tmp / "out"), "--passes", "1", "--no-sweep"]), 0)
                summary = json.loads((tmp / "out" / rp.SUMMARY_NAME).read_text())
                for key in ("started_utc", "finished_utc"):
                    summary.pop(key)
                self.assertNotIn("arm", summary)
                self.assertNotIn("hlod", summary["worlds"]["T-smoke"])
                summaries.append(summary)
            self.assertEqual(summaries[0], summaries[1], "default/chunk summary must acquire no new keys or values")
        self.assertIn("arm", __import__("inspect").signature(rp.path_plan).parameters,
                      "HLOD collection must be opt-in")
        hlod = rp.path_plan(Path("world.gsplatworld"), path, 2, arm="hlod")
        self.assertIs(hlod.pop("shared_layers_t0"), True)
        self.assertEqual(hlod, plan)

    def test_driver_peak_collection_is_opt_in_and_preserves_absence(self):
        text = (HERE / "probe_world.gd").read_text()
        self.assertIn('for key in ["hlod_route_owned_bytes", "hlod_bundle_device_bytes"]:', text)
        start = text.index('for key in ["hlod_route_owned_bytes"')
        block = text[start:text.index('\n\tvar ', start)]
        self.assertIn('stats.get(key)', block)
        self.assertIn('>= 0', block)
        self.assertTrue(text[:start].rfind('if bool(_plan.get("shared_layers_t0", false)):') > text[:start].rfind('func '))
        baseline = subprocess.check_output(["git", "show", "9faecfb155a:tests/runtime/flythrough/probe_world.gd"], cwd=ROOT, text=True)
        begin = text.index('\t\t# HLOD collection only.')
        end = text.index('\t\tvar payload = ', begin)
        self.assertEqual(text[:begin] + text[end:], baseline,
                         "only the opt-in collector may change; trace keys, frame loop, timing and passes are pinned")

    def test_separate_manifest_cli_uses_source_for_path_and_tree_for_render(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            source = tmp / "source"
            tree = tmp / "tree"
            source.mkdir()
            tree.mkdir()
            binary = tmp / "godot"
            binary.write_bytes(b"binary")
            leaf = manifest()
            (source / rp.WORLD_NAME).write_bytes(b"source")
            import hashlib
            digest = hashlib.sha256(b"source").hexdigest()
            leaf["world"]["file_sha256"] = digest
            (source / rp.MANIFEST_NAME).write_text(json.dumps(leaf))
            (tree / rp.WORLD_NAME).write_bytes(b"tree")
            sidecar = {"input_file_sha256": digest, "tree_sha": "ab" * 32,
                       "output_file_sha256": hashlib.sha256(b"tree").hexdigest(),
                       "totals": {"leaf_splat_count": leaf["world"]["splat_count"]}}
            (tree / rp.MANIFEST_NAME).write_text(json.dumps(sidecar))
            dead = {"exit_code": 0, "timed_out": False, "wall_s": .1, "result": None,
                    "rss": {}, "log": "", "trace": "absent"}
            with mock.patch.object(rp, "run_godot", return_value=dead) as run, \
                    mock.patch.object(rp, "binary_identity", wraps=rp.binary_identity) as identity:
                try:
                    rp.main(["--godot-binary", str(binary), "--world", f"A={tree}", "--arm", "hlod",
                             "--world-manifest", f"A={source / rp.MANIFEST_NAME}", "--no-sweep", "--out", str(tmp / "out")])
                except SystemExit as exc:
                    self.fail(f"HLOD probe options rejected: {exc.code}")
                self.assertEqual([c.args[0] for c in identity.call_args_list], [binary.resolve()],
                                 "world rehashing after mincore would warm the cold-pass payload")
            run.assert_called_once()
            self.assertEqual(run.call_args.args[1]["world"], (tree / rp.WORLD_NAME).as_posix())
            self.assertIs(run.call_args.args[1]["shared_layers_t0"], True)
            summary = json.loads((tmp / "out" / rp.SUMMARY_NAME).read_text())
            world = summary["worlds"]["A"]
            self.assertEqual(world["source_identity"]["file_sha256"], digest)
            self.assertNotEqual(world["world_file_sha256"], digest)
            self.assertEqual(world["splat_count"], leaf["world"]["splat_count"])
            # Successful consumer-policy replay. The trace and payload format are
            # captured; flag/count policy scalars below are explicitly constructed.
            trace, result = captured("tsmoke_path", tmp)
            result = admissible_checkpoint_policy(result)
            native = json.loads((HERE / "fixtures/d3_5/d3_5_hlod_test_results.json").read_text())
            result.update(route="hlod", source_generation=native["source_generation"],
                          payload_telemetry=native["completed"]["payload_telemetry"],
                          loaded_file_sha256=sidecar["output_file_sha256"])
            migration = json.loads((HERE / "fixtures/d3_6/run43602195987_migration.json").read_text())["arms"]["candidate"]
            result["final_render_stats"] = copy.deepcopy(migration["final_render_stats"])
            for cp, drawn in zip(result["checkpoints"], migration["payload_behavior"]["drawn_counts"]):
                cp.update(actual_route="hlod", hlod_drawn_nodes=drawn["drawn_nodes"], drawn_splats=drawn["drawn_splats"])
            good = {**dead, "result": result, "trace": str(trace), "rss": {"peak_rss_bytes": ps.metric(123)}}
            with mock.patch.object(rp, "run_godot", return_value=good):
                self.assertEqual(rp.main(["--godot-binary", str(binary), "--world", f"A={tree}", "--arm", "hlod",
                                         "--world-manifest", f"A={source / rp.MANIFEST_NAME}", "--no-sweep", "--passes", "1",
                                         "--out", str(tmp / "good")]), 0)
            physical = json.loads((tmp / "good" / rp.SUMMARY_NAME).read_text())["worlds"]["A"]
            self.assertEqual(physical["hlod"]["route_evidence"]["status"], ps.OK)
            self.assertEqual(physical["hlod"]["acceptance"]["status"], "NOT_RUN")
            self.assertNotIn("expected_unique_upload_flat", physical["flythrough"])
            self.assertGreater(physical["hlod"]["checkpoints"][0]["hlod_drawn_nodes"], 0)
            sidecar["input_file_sha256"] = "00" * 32
            (tree / rp.MANIFEST_NAME).write_text(json.dumps(sidecar))
            with mock.patch.object(rp, "run_godot") as run:
                self.assertEqual(rp.main(["--godot-binary", str(binary), "--world", f"A={tree}", "--arm", "hlod",
                                         "--world-manifest", f"A={source / rp.MANIFEST_NAME}", "--no-sweep",
                                         "--out", str(tmp / "bad")]), rp.EXIT_NOT_RUN)
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
