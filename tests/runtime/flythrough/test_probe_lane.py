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
        self.assertEqual(rp.required_path_metrics({"flythrough": s, "rss": {"peak_rss_bytes": ps.metric(1)}}), [])

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
        result = copy.deepcopy(self.path_result)
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


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------


class Statistics(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
