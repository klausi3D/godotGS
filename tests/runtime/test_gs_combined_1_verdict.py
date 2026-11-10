#!/usr/bin/env python3
"""Unit tests for tests/runtime/gs_combined_1_verdict.py (GS-COMBINED-1, ADR section 9).

The record built by ``passing_record()`` is a constructed fixture, not a capture of the
harness: it exists to exercise the verdict's rules (the deterministic construction is
the point, tests/AGENTS.md). Every assertion is shown to pass on it, to fail on a bad
value, and to fail when its signal is missing; the fail-closed rule, the coverage floor,
the four classification states and the six forcing controls are each pinned.

Run directly or via ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import math
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tests" / "runtime" / "gs_combined_1_verdict.py"
EXPECTATIONS_PATH = ROOT / "tests" / "runtime" / "gs_combined_1_expectations.json"

_spec = importlib.util.spec_from_file_location("gs_combined_1_verdict", MODULE_PATH)
V = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
_spec.loader.exec_module(V)

NO_BLOCKS = {"A": {}, "B": {}}
CLEAN_LOG = "engine started\nall good\n"


def _frame(i: int, variant: str) -> dict:
    t = i / 60.0
    frozen = 240 <= i < 360
    stats = {
        "frame_count": 1000 + i,
        "world_drawn_splats": 50000,
        "object_drawn_splats": [500] * 32,
        "sort_input_sources": ["world", "objects"],
        "splat_composite_passes_frame": 1,
        "route_skip_reason": "none",
        "dropped_sources": 0,
        "lights_dropped": 0,
        "shadow_casters_rastered": {light: 100 for light in V.SHADOWED_LIGHTS},
        "shadow_no_caster_rastered": 0,
        "ledger_bytes": 1_000_000,
        "device_allocated_bytes": 1_010_000,
        "ledger_budget_bytes": 2_000_000,
        "silent_truncation_frames": 0,
        "sort_dispatches_frame": 1,
        "sort_dispatch_frame": 1000 + i - 1,
        "gpu_frame_ms": 8.0 + (i % 7) * 0.1,
        "gpu_frame_valid": True,
        "gpu_timing_frame_serial": 5000 + i,
        "gpu_timing_frames_behind": 2,
    }
    if variant == "B":
        stats.update({"hlod_reason": "active", "hlod_world_draw_records": 1, "hlod_instance_draw_records": 32})
    ramp = 1.0 + 0.25 * math.sin(2 * math.pi * (240 / 60.0 if frozen else t) / 5.0)
    lights = {"directional": {"intensity": ramp, "roi_luma": 0.1 + 0.3 * ramp}}
    for k, light in enumerate(V.OMNI_LIGHTS):
        omni = 1.0 + 0.8 * math.sin(2 * math.pi * (t if not frozen else 4.0) / 3.3 + k)
        lights[light] = {"intensity": omni, "roi_luma": 0.05 + 0.2 * omni}
    pose = "front" if i < 200 else ("inside" if i < 400 else "behind")
    probes = []
    for k in range(8):
        front = "object" if (i + k) % 2 else "world"
        obj, world = [0.1, 0.35, 1.0], [0.9, 0.1, 0.1]
        probes.append({"probe": k, "cpu_front": front, "pixel": list(obj if front == "object" else world),
                       "object_ref": obj, "world_ref": world})
    return {
        "frame": i, "t": t, "frozen": frozen, "stats": stats,
        "frustum_objects": list(range(32)),
        "a4_probes": probes,
        "a5_tracers": {"errors_px": [0.1, 0.2, 0.3], "excluded": {}},
        "pick": {"api_available": True, "target_under_cursor": i % 3 == 0, "target_key": "42",
                 "hit_key": "42" if i % 3 == 0 else None, "partial": False, "ray_region_resident": True},
        "character": {"pose": pose, "reference_pixels": 400, "agreeing_pixels": 396},
        "lights": lights,
        "shadow_rois": {name: {"shadow_luma": 0.1, "control_luma": 0.4} for name in V.SHADOW_SETUPS.values()},
        "frame_digest": "frozen-image" if frozen else f"img{i}",
        "input_digest": "frozen-input" if frozen else f"in{i}",
    }


def passing_record(variant: str = "B") -> dict:
    return {
        "schema": V.SCHEMA, "variant": variant, "frames_requested": 600,
        "segments": {"frozen": [240, 360]}, "taa_enabled": False,
        "gpu_class": "rtx3090", "build_optimized": True,
        "split_screen": None,
        "warmup": {"ready": True, "frames": 40, "msec": 500, "timeout_msec": 240000},
        "frames": [_frame(i, variant) for i in range(600)],
    }


def _states(record, blocked=None, log=CLEAN_LOG):
    v = V.verdict(record, blocked if blocked is not None else NO_BLOCKS, log)
    return {aid: row["state"] for aid, row in v["assertions"].items()}, v


def _all_frames(record, fn):
    for frame in record["frames"]:
        fn(frame)
    return record


class RegistryTests(unittest.TestCase):
    def test_every_assertion_is_dispatched(self):
        record = passing_record()
        results = V.evaluate_assertions(record, CLEAN_LOG)
        self.assertEqual(tuple(results), V.ASSERTION_IDS)

    def test_spec_ids_are_all_present(self):
        expected = {"A1", "A2", "A3", "A4", "A5", "A6", "C1", "C2", "C3", "L1", "L2", "L3",
                    "S1", "S2", "S3", "S4", "B1", "T1", "P1", "P2", "P3", "V1"}
        self.assertEqual(set(V.ASSERTION_IDS), expected)

    def test_thresholds_are_the_spec(self):
        self.assertEqual(V.FRAMES_REQUIRED, 600)
        self.assertEqual(V.FAMILY_MIN_FRAMES, 590)
        self.assertEqual(V.FROZEN_SEGMENT_FRAMES, 120)
        self.assertAlmostEqual(V.A4_TOLERANCE, 3 / 255)
        self.assertEqual(V.A5_TOLERANCE_PX, 0.5)
        self.assertEqual(V.L_PEARSON_MIN, 0.95)
        self.assertEqual(V.S_RATIO_MAX, 0.6)
        self.assertEqual(V.B1_LEDGER_TOLERANCE, 0.02)
        self.assertEqual(V.P_TIMING_COMPLETE_MIN, 0.99)
        # Measurability floors (PR #1315 review): not tolerances on a passing value.
        self.assertEqual(V.A4_REF_MIN_SEPARATION, 0.1)
        self.assertEqual(V.A6_MIN_OPPORTUNITIES, 1)
        # Pre-registered 2026-11-05 (ADR §9 left it unspecified); frozen.
        self.assertEqual(V.C_AGREEMENT_MIN, 0.95)
        self.assertEqual(V.GR15_TARGETS_MS["rtx3080"], {"p95": 25.0, "p99": 33.3})
        self.assertEqual(V.GR15_TARGETS_MS["rtx3090"], {"p95": 16.7, "p99": 20.0})


class PassingRecordTests(unittest.TestCase):
    def test_constructed_passing_record_meets_everything_but_v1(self):
        states, v = _states(passing_record())
        not_live = {aid: s for aid, s in states.items() if s != "live-pass"}
        # V1 (split screen) has no evaluator yet, so it can never pass: the gate cannot be met
        # without it, which is the point.
        self.assertEqual(not_live, {"V1": "live-fail"})
        self.assertFalse(v["gate_met"])

    def test_gate_met_requires_every_row_and_no_blocks(self):
        record = passing_record()
        record["split_screen"] = {"halves": []}
        original = V._eval_v1
        try:
            V._eval_v1 = lambda record, frames, fresh: (1, [], {})
            states, v = _states(record)
            self.assertTrue(v["gate_met"], states)
            blocked = {"A": {}, "B": {"A1": {"issue": "#1", "reason": "x", "expected_failures": ["missing:x"]}}}
            _, v2 = _states(record, blocked)
            self.assertFalse(v2["gate_met"])
            self.assertEqual(v2["stale_blocks"], ["A1"])
        finally:
            V._eval_v1 = original

    def test_variant_a_does_not_require_hlod_keys(self):
        states, _ = _states(passing_record("A"))
        self.assertEqual(states["A3"], "live-pass")
        self.assertEqual(states["A1"], "live-pass")


class FailClosedTests(unittest.TestCase):
    def _assert_all_fail(self, record):
        states, _ = _states(record)
        self.assertTrue(all(s == "live-fail" for s in states.values()), states)

    def test_empty_frame_list_fails_everything(self):
        record = passing_record()
        record["frames"] = []
        self._assert_all_fail(record)

    def test_short_run_fails_everything(self):
        record = passing_record()
        record["frames"] = record["frames"][:599]
        self._assert_all_fail(record)

    def test_index_gap_fails_everything(self):
        record = passing_record()
        record["frames"][10]["frame"] = 11
        self._assert_all_fail(record)

    def test_warmup_not_ready_fails_everything(self):
        record = passing_record()
        record["warmup"]["ready"] = False
        self._assert_all_fail(record)
        states, v = _states(record)
        self.assertIn("record:warmup:not_ready", v["assertions"]["A1"]["codes"])

    def test_warmup_missing_fails_everything(self):
        record = passing_record()
        del record["warmup"]
        self._assert_all_fail(record)

    def test_late_warmup_readiness_fails_everything(self):
        # Readiness observed past the deadline is late, not ready (tests/AGENTS.md).
        record = passing_record()
        record["warmup"]["msec"] = record["warmup"]["timeout_msec"] + 1
        self._assert_all_fail(record)
        record = passing_record()
        del record["warmup"]["timeout_msec"]
        self._assert_all_fail(record)

    def test_wrong_schema_fails_everything(self):
        record = passing_record()
        record["schema"] = "other"
        self._assert_all_fail(record)

    def test_stale_serial_fails_and_does_not_count(self):
        record = passing_record()
        record["frames"][300]["stats"]["frame_count"] = record["frames"][299]["stats"]["frame_count"]
        states, v = _states(record)
        for aid in ("A1", "A4", "S3", "B1", "T1", "P3"):
            self.assertEqual(states[aid], "live-fail", aid)
            self.assertIn("stale:frame_count", v["assertions"][aid]["codes"])

    def test_missing_frame_count_is_stale(self):
        record = passing_record()
        del record["frames"][5]["stats"]["frame_count"]
        self.assertEqual(V.fresh_frames(record["frames"]).count(False), 2)

    def test_nan_fails(self):
        record = passing_record()
        record["frames"][50]["stats"]["world_drawn_splats"] = float("nan")
        states, v = _states(record)
        self.assertEqual(states["A1"], "live-fail")
        self.assertIn("nan_or_type:world_drawn_splats", v["assertions"]["A1"]["codes"])

    def test_bool_is_not_a_number(self):
        record = passing_record()
        record["frames"][50]["stats"]["dropped_sources"] = False
        states, _ = _states(record)
        self.assertEqual(states["A3"], "live-fail")


class PerAssertionTests(unittest.TestCase):
    """Each assertion: a bad value fails it, a missing signal fails it."""

    CASES = {
        "A1": [("value", lambda f: f["stats"].__setitem__("world_drawn_splats", 0)),
               ("value", lambda f: f["stats"]["object_drawn_splats"].__setitem__(3, 0)),
               ("missing", lambda f: f["stats"].pop("world_drawn_splats")),
               ("missing", lambda f: f["stats"].pop("object_drawn_splats")),
               ("missing", lambda f: f.pop("frustum_objects"))],
        "A2": [("value", lambda f: f["stats"].__setitem__("sort_dispatches_frame", 2)),
               ("value", lambda f: f["stats"].__setitem__("sort_input_sources", ["world"])),
               ("value", lambda f: f["stats"].__setitem__("splat_composite_passes_frame", 2)),
               ("stale", lambda f: f["stats"].__setitem__("sort_dispatch_frame", 0)),
               ("missing", lambda f: f["stats"].pop("sort_input_sources"))],
        "A3": [("value", lambda f: f["stats"].__setitem__("route_skip_reason", "frame_skipped")),
               ("value", lambda f: f["stats"].__setitem__("hlod_reason", "not_exclusive")),
               ("value", lambda f: f["stats"].__setitem__("dropped_sources", 1)),
               ("value", lambda f: f["stats"].__setitem__("x", "identity_shim")),
               ("missing", lambda f: f["stats"].pop("dropped_sources")),
               ("missing", lambda f: f["stats"].pop("hlod_reason"))],
        "A4": [("value", lambda f: f["a4_probes"][0].__setitem__("pixel", [0.0, 0.0, 0.0])),
               ("value", lambda f: f["a4_probes"][1].__setitem__(
                   "pixel", [v + 4 / 255 for v in f["a4_probes"][1]["pixel"]])),
               ("missing", lambda f: f.pop("a4_probes")),
               ("missing", lambda f: f["a4_probes"].pop()),
               ("missing", lambda f: f["a4_probes"][2].pop("pixel"))],
        "A5": [("value", lambda f: f["a5_tracers"].__setitem__("errors_px", [0.1, 0.51])),
               ("missing", lambda f: f.pop("a5_tracers")),
               ("missing", lambda f: f["a5_tracers"].__setitem__("errors_px", [float("nan")]))],
        "A6": [("value", lambda f: f["pick"].update({"target_under_cursor": True, "hit_key": None})),
               ("value", lambda f: f["pick"].update({"target_under_cursor": False, "hit_key": "42"})),
               ("value", lambda f: f["pick"].update({"partial": True})),
               ("missing", lambda f: f["pick"].__setitem__("api_available", False)),
               ("missing", lambda f: f.pop("pick"))],
        "L3": [("value", lambda f: f["stats"].__setitem__("lights_dropped", 1)),
               ("missing", lambda f: f["stats"].pop("lights_dropped"))],
        "S1": [("value", lambda f: f["stats"]["shadow_casters_rastered"].__setitem__("omni_2", 0)),
               ("value", lambda f: f["stats"].__setitem__("shadow_no_caster_rastered", 3)),
               ("missing", lambda f: f["stats"]["shadow_casters_rastered"].pop("directional")),
               ("missing", lambda f: f["stats"].pop("shadow_no_caster_rastered"))],
        "S2": [("value", lambda f: f["shadow_rois"]["mesh_floor_under_splat_object"].__setitem__("shadow_luma", 0.3)),
               ("missing", lambda f: f["shadow_rois"].pop("mesh_floor_under_splat_object"))],
        "S3": [("value", lambda f: f["shadow_rois"]["splat_ground_under_mesh"].__setitem__("control_luma", 0.0)),
               ("missing", lambda f: f["shadow_rois"]["splat_ground_under_mesh"].__setitem__("shadow_luma", None))],
        "S4": [("value", lambda f: f["shadow_rois"]["splat_object_over_splat_world"].__setitem__("shadow_luma", 0.25)),
               ("missing", lambda f: f.pop("shadow_rois"))],
        "B1": [("value", lambda f: f["stats"].__setitem__("device_allocated_bytes", 1_030_000)),
               ("value", lambda f: f["stats"].__setitem__("ledger_budget_bytes", 10)),
               ("value", lambda f: f["stats"].__setitem__("silent_truncation_frames", 1)),
               ("missing", lambda f: f["stats"].pop("ledger_bytes")),
               ("missing", lambda f: f["stats"].pop("silent_truncation_frames"))],
    }

    def test_each_case_turns_the_assertion_red(self):
        for aid, cases in self.CASES.items():
            for index, (kind, mutate) in enumerate(cases):
                with self.subTest(assertion=aid, case=index, kind=kind):
                    record = passing_record()
                    mutate(record["frames"][100])
                    states, v = _states(record)
                    self.assertEqual(states[aid], "live-fail", v["assertions"][aid])
                    codes = v["assertions"][aid]["codes"]
                    if kind == "missing":
                        self.assertTrue(any(c.startswith(("missing:", "nan_or_type:")) for c in codes), codes)

    def test_character_pose_assertions_are_separate(self):
        record = passing_record()
        record["frames"][250]["character"]["agreeing_pixels"] = 100  # inside pose
        states, _ = _states(record)
        self.assertEqual((states["C1"], states["C2"], states["C3"]), ("live-pass", "live-fail", "live-pass"))
        record = passing_record()
        record["frames"][450]["character"].pop("reference_pixels")
        states, _ = _states(record)
        self.assertEqual(states["C3"], "live-fail")
        record = passing_record()
        record["frames"][10]["character"]["reference_pixels"] = 0
        self.assertEqual(_states(record)[0]["C1"], "live-fail")

    def test_character_pose_coverage(self):
        record = passing_record()
        for f in record["frames"][0:15]:
            f["character"]["pose"] = "inside"  # front now has 185 < 190 frames
        states, v = _states(record)
        self.assertEqual(states["C1"], "live-fail")
        self.assertIn("coverage:below_minimum", v["assertions"]["C1"]["codes"])

    def test_lights_correlation(self):
        record = passing_record()
        for f in record["frames"]:
            f["lights"]["omni_1"]["roi_luma"] = 0.3  # constant: r undefined
        states, _ = _states(record)
        self.assertEqual(states["L2"], "live-fail")
        record = passing_record()
        for i, f in enumerate(record["frames"]):
            f["lights"]["directional"]["roi_luma"] = (i * 7919 % 101) / 100.0  # uncorrelated
        self.assertEqual(_states(record)[0]["L1"], "live-fail")
        record = passing_record()
        record["frames"][30]["lights"]["omni_3"].pop("roi_luma")
        states, v = _states(record)
        self.assertEqual(states["L2"], "live-fail")
        self.assertIn("missing:light_roi:omni_3", v["assertions"]["L2"]["codes"])

    def test_log_lines(self):
        for line in ("Frame skipped: no route", "route not_exclusive", "identity_shim engaged"):
            with self.subTest(line=line):
                states, _ = _states(passing_record(), log=CLEAN_LOG + line)
                self.assertEqual(states["A3"], "live-fail")
        states, v = _states(passing_record(), log=None)
        self.assertEqual(states["A3"], "live-fail")
        self.assertIn("missing:log", v["assertions"]["A3"]["codes"])


class CoverageTests(unittest.TestCase):
    def test_a5_with_no_measurable_tracer_fails_on_coverage(self):
        record = _all_frames(passing_record(), lambda f: f["a5_tracers"].__setitem__("errors_px", []))
        states, v = _states(record)
        self.assertEqual(states["A5"], "live-fail")
        self.assertEqual(v["assertions"]["A5"]["codes"], ["coverage:below_minimum"])

    def test_a5_coverage_floor_is_590(self):
        record = passing_record()
        for f in record["frames"][:10]:
            f["a5_tracers"]["errors_px"] = []
        self.assertEqual(_states(record)[0]["A5"], "live-pass")
        record["frames"][10]["a5_tracers"]["errors_px"] = []
        self.assertEqual(_states(record)[0]["A5"], "live-fail")

    def test_a4_excluded_probes(self):
        record = passing_record()
        for f in record["frames"][:10]:
            for p in f["a4_probes"][:3]:
                p["cpu_front"] = "excluded"  # 5 defined < 6: frame not counted
        self.assertEqual(_states(record)[0]["A4"], "live-pass")
        record["frames"][10]["a4_probes"][0]["cpu_front"] = "excluded"
        record["frames"][10]["a4_probes"][1]["cpu_front"] = "excluded"
        record["frames"][10]["a4_probes"][2]["cpu_front"] = "excluded"
        self.assertEqual(_states(record)[0]["A4"], "live-fail")

    def test_a4_uniform_image_is_not_a_pass(self):
        # Both layers gone (or a uniform composite): probe and both references read the
        # same colour. Agreement is meaningless there; it must fail, never live-pass.
        def black(frame):
            for p in frame["a4_probes"]:
                p["pixel"] = [0.0, 0.0, 0.0]
                p["object_ref"] = [0.0, 0.0, 0.0]
                p["world_ref"] = [0.0, 0.0, 0.0]
        record = _all_frames(passing_record(), black)
        states, v = _states(record)
        self.assertEqual(states["A4"], "live-fail")
        self.assertIn("coverage:below_minimum", v["assertions"]["A4"]["codes"])

    def test_a4_indistinct_references_do_not_count(self):
        record = passing_record()
        for f in record["frames"][:11]:
            for p in f["a4_probes"][:3]:
                p["world_ref"] = list(p["object_ref"])  # 5 distinct < 6: frame not counted
        self.assertEqual(_states(record)[0]["A4"], "live-fail")
        record = passing_record()
        for f in record["frames"][:10]:
            for p in f["a4_probes"][:3]:
                p["world_ref"] = list(p["object_ref"])
        self.assertEqual(_states(record)[0]["A4"], "live-pass")

    def test_a4_missing_reference_fails(self):
        record = passing_record()
        del record["frames"][5]["a4_probes"][1]["world_ref"]
        states, v = _states(record)
        self.assertEqual(states["A4"], "live-fail")
        self.assertIn("missing:a4_reference", v["assertions"]["A4"]["codes"])

    def test_a6_without_a_pick_opportunity_fails_on_coverage(self):
        # A picker that never hits passes every frame without the target under the cursor.
        def no_target(frame):
            frame["pick"]["target_under_cursor"] = False
            frame["pick"]["hit_key"] = None
        record = _all_frames(passing_record(), no_target)
        states, v = _states(record)
        self.assertEqual(states["A6"], "live-fail")
        self.assertIn("coverage:no_pick_opportunity", v["assertions"]["A6"]["codes"])
        self.assertEqual(v["assertions"]["A6"]["detail"]["pick_opportunities"], 0)

    def test_a6_one_opportunity_is_enough_coverage(self):
        record = passing_record()
        for i, f in enumerate(record["frames"]):
            f["pick"]["target_under_cursor"] = i == 7
            f["pick"]["hit_key"] = "42" if i == 7 else None
        self.assertEqual(_states(record)[0]["A6"], "live-pass")
        record["frames"][7]["pick"]["hit_key"] = None
        self.assertEqual(_states(record)[0]["A6"], "live-fail")

    def test_a4_excluded_frame_still_reports_a_mismatch(self):
        record = passing_record()
        f = record["frames"][20]
        for p in f["a4_probes"][:4]:
            p["cpu_front"] = "excluded"
        f["a4_probes"][5]["pixel"] = [0.0, 0.0, 0.0]
        self.assertEqual(_states(record)[0]["A4"], "live-fail")


class StabilityTests(unittest.TestCase):
    def test_one_differing_frame_fails(self):
        record = passing_record()
        record["frames"][300]["frame_digest"] = "other"
        states, v = _states(record)
        self.assertEqual(states["T1"], "live-fail")
        self.assertEqual(v["assertions"]["T1"]["detail"]["frames_differing"], 1)

    def test_changed_inputs_are_invalid_not_a_pass(self):
        record = passing_record()
        record["frames"][300]["input_digest"] = "moved"
        states, v = _states(record)
        self.assertEqual(states["T1"], "live-fail")
        self.assertIn("invalid:inputs_not_frozen", v["assertions"]["T1"]["codes"])

    def test_taa_and_segment_shape(self):
        record = passing_record()
        record["taa_enabled"] = True
        self.assertEqual(_states(record)[0]["T1"], "live-fail")
        record = passing_record()
        record["segments"]["frozen"] = [240, 300]
        self.assertEqual(_states(record)[0]["T1"], "live-fail")
        record = passing_record()
        del record["segments"]
        self.assertEqual(_states(record)[0]["T1"], "live-fail")
        record = passing_record()
        record["frames"][250]["frame_digest"] = None
        self.assertEqual(_states(record)[0]["T1"], "live-fail")
        record = passing_record()
        record["frames"][250]["frozen"] = False
        self.assertEqual(_states(record)[0]["T1"], "live-fail")


class PerformanceTests(unittest.TestCase):
    def test_over_target_is_report_only(self):
        record = _all_frames(passing_record(), lambda f: f["stats"].__setitem__("gpu_frame_ms", 40.0))
        states, v = _states(record)
        self.assertEqual(states["P1"], "live-pass")
        self.assertEqual(states["P2"], "live-pass")
        self.assertEqual(v["assertions"]["P1"]["report_codes"], ["report:p95_over_target"])

    def test_dev_build_invalidates_p1_p2(self):
        record = passing_record()
        record["build_optimized"] = False
        states, _ = _states(record)
        self.assertEqual((states["P1"], states["P2"], states["P3"]), ("live-fail", "live-fail", "live-pass"))

    def test_unknown_gpu_has_no_target(self):
        record = passing_record()
        record["gpu_class"] = "unknown"
        self.assertEqual(_states(record)[0]["P1"], "live-fail")

    def test_one_stale_timestamp_fails_p3(self):
        record = passing_record()
        frames = record["frames"]
        frames[300]["stats"]["gpu_timing_frame_serial"] = frames[299]["stats"]["gpu_timing_frame_serial"]
        states, v = _states(record)
        self.assertEqual(states["P3"], "live-fail")
        self.assertIn("stale:gpu_timing_frame_serial", v["assertions"]["P3"]["codes"])
        self.assertEqual(states["P1"], "live-fail")  # validity binds P1/P2 too

    def test_incomplete_timing_up_to_one_percent(self):
        record = passing_record()
        for f in record["frames"][:6]:
            f["stats"]["gpu_frame_valid"] = False
        self.assertEqual(_states(record)[0]["P3"], "live-pass")
        record["frames"][6]["stats"]["gpu_frame_valid"] = False
        states, v = _states(record)
        self.assertEqual(states["P3"], "live-fail")
        self.assertIn("value:timing_complete<99%", v["assertions"]["P3"]["codes"])

    def test_lag_beyond_bound_is_incomplete(self):
        record = _all_frames(passing_record(), lambda f: f["stats"].__setitem__("gpu_timing_frames_behind", 4))
        self.assertEqual(_states(record)[0]["P3"], "live-fail")

    def test_missing_timing_key_fails(self):
        record = passing_record()
        del record["frames"][3]["stats"]["gpu_timing_frame_serial"]
        states, v = _states(record)
        self.assertEqual(states["P3"], "live-fail")
        self.assertIn("missing:gpu_timing_keys", v["assertions"]["P3"]["codes"])


class ClassificationTests(unittest.TestCase):
    def _record_without_object_drawn(self):
        return _all_frames(passing_record(), lambda f: f["stats"].pop("object_drawn_splats"))

    def test_blocked_when_every_code_is_expected(self):
        blocked = {"A": {}, "B": {"A1": {"issue": "#788", "reason": "no per-object counter",
                                         "expected_failures": ["missing:object_drawn_splats"]}}}
        states, v = _states(self._record_without_object_drawn(), blocked)
        self.assertEqual(states["A1"], "blocked")
        self.assertEqual(v["assertions"]["A1"]["issue"], "#788")
        self.assertEqual(v["blocked"], ["A1"])
        self.assertFalse(v["gate_met"])

    def test_unexplained_code_under_a_block_is_a_regression(self):
        record = self._record_without_object_drawn()
        record["frames"][7]["stats"]["world_drawn_splats"] = 0
        blocked = {"A": {}, "B": {"A1": {"issue": "#788", "reason": "x",
                                         "expected_failures": ["missing:object_drawn_splats"]}}}
        states, v = _states(record, blocked)
        self.assertEqual(states["A1"], "live-fail")
        self.assertEqual(v["assertions"]["A1"]["unexplained_codes"], ["value:world_drawn_splats<=0"])
        self.assertIn("A1", v["regressions"])

    def test_prefix_patterns(self):
        self.assertTrue(V._code_matches("missing:light_roi:omni_2", ["missing:light_roi:*"]))
        self.assertFalse(V._code_matches("missing:other", ["missing:light_roi:*"]))

    def test_stale_block(self):
        blocked = {"A": {}, "B": {"S3": {"issue": "#1095", "reason": "x", "expected_failures": ["value:shadow_ratio>0.6"]}}}
        states, v = _states(passing_record(), blocked)
        self.assertEqual(states["S3"], "stale-block")
        self.assertEqual(v["stale_blocks"], ["S3"])

    def test_blocks_are_per_variant(self):
        blocked = {"A": {"A1": {"issue": "#788", "reason": "x", "expected_failures": ["missing:object_drawn_splats"]}}, "B": {}}
        states, _ = _states(self._record_without_object_drawn(), blocked)
        self.assertEqual(states["A1"], "live-fail")


class ExpectationsLoaderTests(unittest.TestCase):
    def _good(self):
        return {"schema": V.EXPECTATIONS_SCHEMA,
                "preregistered_thresholds": {"C_AGREEMENT_MIN": {
                    "value": 0.95, "assertions": ["C1", "C2", "C3"], "preregistered": "2026-11-05",
                    "reason": "ADR §9 left it unspecified"}},
                "blocked": {"A": {}, "B": {
            "A6": {"issue": "#788", "reason": "pick is a stub", "expected_failures": ["missing:pick_api"]}}}}

    def test_good(self):
        self.assertIn("A6", V.load_expectations(self._good())["B"])

    def test_rejections(self):
        mutations = [
            lambda d: d.__setitem__("schema", "x"),
            lambda d: d["blocked"].pop("A"),
            lambda d: d["blocked"]["B"].__setitem__("Z9", d["blocked"]["B"]["A6"]),
            lambda d: d["blocked"]["B"]["A6"].__setitem__("issue", "788"),
            lambda d: d["blocked"]["B"]["A6"].__setitem__("issue", "#abc"),
            lambda d: d["blocked"]["B"]["A6"].__setitem__("reason", " "),
            lambda d: d["blocked"]["B"]["A6"].__setitem__("expected_failures", []),
            lambda d: d["blocked"]["B"]["A6"].pop("expected_failures"),
            lambda d: d.pop("preregistered_thresholds"),
            lambda d: d["preregistered_thresholds"]["C_AGREEMENT_MIN"].__setitem__("value", 0.9),
            lambda d: d["preregistered_thresholds"]["C_AGREEMENT_MIN"].__setitem__("assertions", ["C1"]),
            lambda d: d["preregistered_thresholds"]["C_AGREEMENT_MIN"].pop("preregistered"),
            lambda d: d["preregistered_thresholds"]["C_AGREEMENT_MIN"].pop("reason"),
            lambda d: d["preregistered_thresholds"].__setitem__("OTHER", {}),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(case=index):
                data = self._good()
                mutate(data)
                with self.assertRaises(V.RecordError):
                    V.load_expectations(data)

    def test_notes_never_change_a_state(self):
        record = _all_frames(passing_record(), lambda f: f["a5_tracers"].__setitem__("errors_px", [0.9]))
        with_note = V.verdict(record, NO_BLOCKS, CLEAN_LOG, {"B": {"A5": "needs triage"}})
        without = V.verdict(record, NO_BLOCKS, CLEAN_LOG)
        self.assertEqual(with_note["assertions"]["A5"]["state"], without["assertions"]["A5"]["state"])
        self.assertEqual(with_note["assertions"]["A5"]["note"], "needs triage")
        self.assertEqual(with_note["gate_met"], without["gate_met"])
        with self.assertRaises(V.RecordError):
            V.load_notes({"notes": {"B": {"Z9": {"reason": "x"}}}})
        with self.assertRaises(V.RecordError):
            V.load_notes({"notes": {"B": {"A5": {"reason": " "}}}})

    def test_the_repository_expectations_file_loads(self):
        data = json.loads(EXPECTATIONS_PATH.read_text(encoding="utf-8"))
        loaded = V.load_expectations(data)
        V.load_notes(data)
        self.assertEqual(set(loaded), {"A", "B"})
        # Blocked is never green: every assertion the file blocks must be listed with an issue.
        for variant in loaded.values():
            for entry in variant.values():
                self.assertRegex(entry["issue"], r"^#\d+$")


class ControlTests(unittest.TestCase):
    def test_every_control_is_proven_on_a_passing_record(self):
        controls = V.run_controls(passing_record(), CLEAN_LOG)
        self.assertEqual(set(controls), {"1", "2", "3", "4", "5", "6"})
        for number, result in controls.items():
            with self.subTest(control=number):
                self.assertEqual(result["state"], "proven", result)

    def test_runtime_switch_availability_is_reported(self):
        controls = V.run_controls(passing_record(), CLEAN_LOG)
        self.assertEqual({n for n, r in controls.items() if not r["runtime_switch"]}, {"1", "2", "3"})

    def test_control_on_an_already_failing_target_is_unproven(self):
        record = _all_frames(passing_record(), lambda f: f["pick"].__setitem__("api_available", False))
        self.assertEqual(V.run_controls(record, CLEAN_LOG)["6"]["state"], "unproven")

    def test_a_blind_assertion_is_reported_broken(self):
        original = V.FRAME_EVALUATORS["A6"]
        try:
            V.FRAME_EVALUATORS["A6"] = lambda frame, record: []
            self.assertEqual(V.run_controls(passing_record(), CLEAN_LOG)["6"]["state"], "broken")
        finally:
            V.FRAME_EVALUATORS["A6"] = original

    def test_controls_do_not_mutate_the_input(self):
        record = passing_record()
        before = copy.deepcopy(record)
        V.run_controls(record, CLEAN_LOG)
        self.assertEqual(record, before)


class ReportTests(unittest.TestCase):
    def test_markdown_lists_every_assertion(self):
        _, v = _states(passing_record())
        table = V.markdown_table([v])
        for aid in V.ASSERTION_IDS:
            self.assertIn(f"| {aid} |", table)


if __name__ == "__main__":
    unittest.main(verbosity=2)
