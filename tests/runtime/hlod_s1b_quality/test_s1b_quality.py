#!/usr/bin/env python3
"""CPU-only tests for the S1b quality harness (no GPU, no Godot binary).

Every test is written to be able to fail: boundaries are probed on both sides, and
``ValidateRowGuards`` injects each known benchmark trap (missing fixture / zero splats,
stale fixture, black frame, flicker, count mismatch, ...) one at a time and requires the
specific guard to fire. Telemetry fixtures are built by ``good_telemetry`` whose key set
is checked against the keys the producer (``s1b_render_arm.gd``) actually writes.
"""

from __future__ import annotations

import copy
import importlib.util
import re
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("s1b_quality", HERE / "s1b_quality.py")
q = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(q)

W, H = q.EXPECTED_VIEWPORT


def blank() -> np.ndarray:
    return np.zeros((H, W, 3), dtype=np.uint8)


def with_block(image: np.ndarray, x0: int, y0: int, x1: int, y1: int, value: int) -> np.ndarray:
    out = image.copy()
    out[y0:y1, x0:x1] = value
    return out


class Frozen(unittest.TestCase):
    def test_spec_constants_are_unchanged(self):
        # bounded-acceptance.md / ADR section 8 test 12: changing any of these is a
        # contract change that needs its own review, not a fix to make a row pass.
        self.assertEqual(q.ROI_DIFF_THRESHOLD_8BIT, 8)
        self.assertEqual(q.ROI_PAD_PX, 8)
        self.assertEqual(q.DECISION_MARGIN_DB, 0.5)
        self.assertEqual(q.ARMS, ("sat", "a_area", "s1b"))
        self.assertEqual(q.EXPECTED_VIEWPORT, (960, 540))
        self.assertEqual(q.EXPECTED_FOV, 60.0)


class Roi(unittest.TestCase):
    def test_threshold_is_strictly_greater_than_8_of_255_per_channel(self):
        ref = blank()
        for delta, expected in ((8, False), (9, True)):
            for channel in range(3):
                arm = ref.copy()
                arm[10, 10, channel] = delta
                self.assertEqual(bool(q.changed_mask(ref, arm)[10, 10]), expected, (delta, channel))

    def test_bounding_box_of_union_is_padded_and_same_for_all_arms(self):
        ref = blank()
        a = with_block(ref, 100, 100, 110, 110, 200)  # 10x10
        b = with_block(ref, 300, 200, 305, 205, 200)  # elsewhere
        roi = q.common_roi(ref, {"a": a, "b": b})
        self.assertEqual(roi, (100 - 8, 100 - 8, 305 + 8, 205 + 8))
        # A single arm's ROI is smaller: the union genuinely widens the crop.
        self.assertEqual(q.common_roi(ref, {"a": a}), (92, 92, 118, 118))

    def test_padding_clamps_to_the_image(self):
        ref = blank()
        arm = with_block(ref, 0, 0, 2, 2, 255)
        self.assertEqual(q.common_roi(ref, {"a": arm}), (0, 0, 10, 10))
        arm = with_block(ref, W - 2, H - 2, W, H, 255)
        self.assertEqual(q.common_roi(ref, {"a": arm}), (W - 10, H - 10, W, H))

    def test_no_change_is_none_not_a_full_frame_crop(self):
        ref = blank()
        self.assertIsNone(q.common_roi(ref, {"a": ref.copy()}))
        sub = ref.copy()
        sub[5, 5] = 8  # at, not above, the threshold
        self.assertIsNone(q.common_roi(ref, {"a": sub}))

    def test_shape_mismatch_raises(self):
        with self.assertRaises(q.HarnessError):
            q.common_roi(blank(), {"a": np.zeros((H, W - 1, 3), np.uint8)})
        with self.assertRaises(q.HarnessError):
            q.common_roi(blank(), {})


class Decision(unittest.TestCase):
    def test_boundary_is_inclusive_at_exactly_half_a_db(self):
        base = {"sat": 30.0, "a_area": 29.0}
        self.assertEqual(q.decide({**base, "s1b": 29.5})["verdict"], "PASS")
        self.assertEqual(q.decide({**base, "s1b": 29.4999})["verdict"], "FAIL")

    def test_uses_the_better_baseline_whichever_it_is(self):
        # S1b beats SAT but is 1 dB under A-area: must fail; a min() rule would pass.
        r = q.decide({"sat": 28.0, "a_area": 30.0, "s1b": 29.0})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertEqual(r["best_baseline_arm"], "a_area")
        r = q.decide({"sat": 30.0, "a_area": 28.0, "s1b": 29.0})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertEqual(r["best_baseline_arm"], "sat")

    def test_better_than_both_passes(self):
        self.assertEqual(q.decide({"sat": 30.0, "a_area": 29.0, "s1b": 35.0})["verdict"], "PASS")

    def test_missing_or_nonfinite_psnr_is_invalid_never_pass(self):
        ok = {"sat": 30.0, "a_area": 29.0, "s1b": 35.0}
        for arm in q.ARMS:
            for bad in (None, float("nan"), float("inf"), "30", True):
                self.assertEqual(q.decide({**ok, arm: bad})["verdict"], "INVALID", (arm, bad))
            missing = dict(ok)
            del missing[arm]
            self.assertEqual(q.decide(missing)["verdict"], "INVALID", arm)


def good_manifest():
    def entry(name, splats):
        return {"path": f"/x/{name}.gsplatworld", "sha256": f"sha-{name}", "splats": splats}

    return {
        "id": "holzbank_d6_tau4.0",
        "cut_splats": 11146,
        "domain_splats": 252326,
        "sh_first_order_count": 3,
        "sh_high_order_count": 12,
        "files": {
            "reference": entry("reference", 252326),
            "sat": entry("sat", 11146),
            "a_area": entry("a_area", 11146),
            "s1b": entry("s1b", 11146),
        },
    }


def good_view():
    return {
        "asset": "holzbank",
        "distance_label": "d6",
        "tau_pixels": 4,
        "position": [0.0, 1.0, 46.0],
        "target": [0.0, 1.0, -1.0],
        "basis_rows": [[1, 0, 0], [0, 1, 0], [0, 0, 1]],
    }


def live_frame(serial):
    return {
        "live_sampled_drop_frame_serial": serial,
        "live_sampled_dropped_records": 0,
        "live_sampled_raster_truncated_tiles": 0,
        "live_overflow_drop_events": 0,
        "live_raster_tile_cap_drop_events": 0,
        "live_clamped_records": 0,
        "live_overflow_tile_count": 0,
        "live_aggregated_records": 123456,
    }


def good_telemetry(manifest, view, far=1000.0):
    def one(name):
        n = manifest["files"][name]["splats"]
        return {
            "status": "ok",
            "reason": "rendered",
            "world_sha256": manifest["files"][name]["sha256"],
            "world_splat_count": n,
            "data_count": n,
            "sh_degree": 3,
            "sh_first_order_count": 3,
            "sh_high_order_count": 12,
            "world_has_hlod_tree": False,
            "nonblack_pixels": 5000,
            "distinct_frame_hashes": 1,
            "drawn": {"visible_splats": n // 2, "sorted_splats": n // 2, "total_splats": n},
            "camera": {"viewport": [960, 540], "fov": 60.0, "near": 0.01, "far": far},
            "clip_proof": {"min_depth": 30.0, "max_depth": 60.0, "max_corner_distance": 61.0, "near": 0.01, "far": far},
            "camera_job": {"position": view["position"], "target": view["target"], "basis_rows": view["basis_rows"]},
            "project_settings": {"a": "1"},
            "engine_version": {"hash": "abc"},
            "adapter": "gpu0",
            "frames": [live_frame(100 + 2 * i) for i in range(4)],
        }

    return {name: one(name) for name in ("reference",) + q.ARMS}


class ProducerContract(unittest.TestCase):
    def test_fixture_keys_are_keys_the_producer_writes(self):
        produced = set(re.findall(r'_telemetry\["(\w+)"\]', (HERE / "s1b_render_arm.gd").read_text()))
        fixture = good_telemetry(good_manifest(), good_view())["sat"]
        self.assertEqual(set(fixture) - produced, set(), "fixture invents telemetry keys")


class ValidateRowGuards(unittest.TestCase):
    def setUp(self):
        self.manifest = good_manifest()
        self.view = good_view()
        self.tel = good_telemetry(self.manifest, self.view)

    def check(self, tel=None, manifest=None):
        return q.validate_row(manifest or self.manifest, self.view, tel if tel is not None else self.tel, 1000.0)

    def assertFires(self, failures, needle):
        self.assertTrue(any(needle in f for f in failures), f"{needle!r} not in {failures}")

    def test_valid_row_has_no_failures(self):
        self.assertEqual(self.check(), [])

    def test_missing_arm(self):
        for name in ("reference",) + q.ARMS:
            tel = dict(self.tel)
            tel[name] = None
            self.assertFires(self.check(tel), f"{name}: no telemetry")
            tel = dict(self.tel)
            del tel[name]
            self.assertFires(self.check(tel), f"{name}: no telemetry")

    def test_failed_render_status(self):
        self.tel["s1b"]["status"] = "failed"
        self.tel["s1b"]["reason"] = "boom"
        self.assertFires(self.check(), "s1b: render status")

    def test_stale_or_substituted_fixture(self):
        self.tel["a_area"]["world_sha256"] = "other"
        self.assertFires(self.check(), "a_area: world sha256 differs")

    def test_zero_splat_fixture_and_render(self):
        self.manifest["files"]["sat"]["splats"] = 0
        self.assertFires(self.check(), "zero-splat fixture")
        self.manifest = good_manifest()
        self.tel["sat"]["drawn"]["visible_splats"] = 0
        self.assertFires(self.check(), "zero-splat render")

    def test_thinned_import(self):
        # The recorded trap: a stale import cache thinned 10000 -> 410.
        self.tel["reference"]["world_splat_count"] = 410
        self.assertFires(self.check(), "reference: world_splat_count=410")
        self.tel = good_telemetry(self.manifest, self.view)
        self.tel["s1b"]["drawn"]["total_splats"] = 410
        self.assertFires(self.check(), "s1b: renderer total_splats=410")

    def test_black_frame(self):
        self.tel["s1b"]["nonblack_pixels"] = 0
        self.assertFires(self.check(), "black frame")
        self.tel["s1b"]["nonblack_pixels"] = None
        self.assertFires(self.check(), "black frame")

    def test_flicker(self):
        self.tel["sat"]["distinct_frame_hashes"] = 2
        self.assertFires(self.check(), "unstable static frames")

    def test_unequal_counts_between_arms(self):
        self.tel["a_area"]["drawn"]["total_splats"] = 11145
        self.manifest["files"]["a_area"]["splats"] = 11145
        for k in ("world_splat_count", "data_count"):
            self.tel["a_area"][k] = 11145
        self.assertFires(self.check(), "arms are not equal-count")

    def test_arm_count_differs_from_manifest_cut(self):
        self.manifest["cut_splats"] = 99
        self.assertFires(self.check(), "arms are not equal-count")

    def test_reference_is_not_the_full_domain(self):
        self.manifest["domain_splats"] = 252325
        self.assertFires(self.check(), "reference total_splats != manifest domain_splats")

    def test_sh_degree_and_terms(self):
        self.tel["s1b"]["sh_degree"] = 1
        self.assertFires(self.check(), "s1b: SH degree differs")
        self.tel = good_telemetry(self.manifest, self.view)
        self.tel["sat"]["sh_high_order_count"] = 0
        self.assertFires(self.check(), "SH term counts differ")

    def test_arm_must_not_carry_a_tree(self):
        self.tel["s1b"]["world_has_hlod_tree"] = True
        self.assertFires(self.check(), "carries an HLOD tree")
        self.tel["s1b"]["world_has_hlod_tree"] = None
        self.assertFires(self.check(), "carries an HLOD tree")

    def test_settings_camera_engine_and_adapter_must_match_across_arms(self):
        for key, value in (("project_settings", {"a": "2"}), ("engine_version", {"hash": "z"}), ("adapter", "gpu1")):
            tel = copy.deepcopy(self.tel)
            tel["a_area"][key] = value
            self.assertFires(self.check(tel), f"a_area: {key} differs")

    def test_camera_must_be_the_frozen_view(self):
        for t in self.tel.values():
            t["camera_job"]["position"] = [0.0, 1.0, 47.0]
        self.assertFires(self.check(), "camera position is not the frozen view's")
        self.tel = good_telemetry(self.manifest, self.view)
        for t in self.tel.values():
            t["camera_job"]["basis_rows"] = [[0, 1, 0], [1, 0, 0], [0, 0, 1]]
        self.assertFires(self.check(), "camera basis")

    def test_viewport_fov_and_far(self):
        self.tel["s1b"]["camera"]["viewport"] = [640, 360]
        self.assertFires(self.check(), "frozen 960x540")
        self.tel = good_telemetry(self.manifest, self.view)
        self.tel["s1b"]["camera"]["far"] = 1e6
        self.assertFires(self.check(), "far plane")

    def test_clip_proof(self):
        self.tel["reference"]["clip_proof"]["max_depth"] = 1500.0
        self.assertFires(self.check(), "clip proof")
        self.tel = good_telemetry(self.manifest, self.view)
        # min_depth below near is an interior camera: accepted (near is the frozen intrinsic).
        self.tel["reference"]["clip_proof"]["min_depth"] = 0.005
        self.assertEqual(self.check(), [])
        self.tel["reference"]["clip_proof"]["min_depth"] = float("nan")
        self.assertFires(self.check(), "clip proof")
        self.tel = good_telemetry(self.manifest, self.view)
        del self.tel["reference"]["clip_proof"]
        self.assertFires(self.check(), "clip proof")


class LiveLoss(unittest.TestCase):
    def tel(self, serials=(100, 102, 104, 106)):
        return {"frames": [live_frame(v) for v in serials]}

    def test_clean_and_advancing_passes(self):
        self.assertEqual(q.live_loss_failures("x", self.tel()), [])

    def test_non_advancing_or_unpopulated_serial_is_unproven_not_zero(self):
        for serials in ((100, 100, 100, 100), (0, 0, 0, 0), (100, 99, 98, 97)):
            f = q.live_loss_failures("x", self.tel(serials))
            self.assertTrue(any("did not advance" in m for m in f), serials)
        t = self.tel()
        t["frames"][0]["live_sampled_drop_frame_serial"] = None
        self.assertTrue(q.live_loss_failures("x", t))

    def test_each_counter_nonzero_missing_or_wrong_type_fails(self):
        for key in q.LIVE_ZERO_COUNTERS:
            for bad in (1, None, "0", 0.0):
                t = self.tel()
                t["frames"][2]["live_" + key] = bad
                f = q.live_loss_failures("x", t)
                self.assertTrue(any(key in m for m in f), (key, bad, f))
            t = self.tel()
            del t["frames"][1]["live_" + key]
            self.assertTrue(any(key in m for m in q.live_loss_failures("x", t)))

    def test_unpopulated_tile_record_total_is_unproven(self):
        for bad in (0, None):
            t = self.tel()
            t["frames"][-1]["live_aggregated_records"] = bad
            self.assertTrue(any("unpopulated" in m for m in q.live_loss_failures("x", t)), bad)

    def test_no_frames_fails(self):
        self.assertTrue(q.live_loss_failures("x", {}))
        self.assertTrue(q.live_loss_failures("x", {"frames": [live_frame(1)]}))

    def test_validate_row_applies_it(self):
        manifest, view = good_manifest(), good_view()
        tel = good_telemetry(manifest, view)
        tel["a_area"]["frames"][3]["live_sampled_dropped_records"] = 7
        self.assertTrue(any("loss observed" in f for f in q.validate_row(manifest, view, tel, 1000.0)))


class ColorSpace(unittest.TestCase):
    def test_identical_is_infinite(self):
        a = np.full((20, 20, 3), 90, np.uint8)
        self.assertEqual(q.psnr_array(a, a.copy(), True), float("inf"))

    def test_srgb_known_value(self):
        a = np.zeros((10, 10, 3), np.uint8)
        b = a + 51  # 0.2 error everywhere -> mse 0.04 -> 13.98 dB
        self.assertAlmostEqual(q.psnr_array(a, b, False), 13.9794, places=3)

    def test_linear_light_differs_and_is_correct(self):
        a = np.zeros((10, 10, 3), np.uint8)
        b = a + 255
        self.assertAlmostEqual(q.psnr_array(a, b, True), 0.0, places=6)  # linear 1.0 error
        c = a + 128  # sRGB 128 -> linear 0.2158
        self.assertAlmostEqual(q.psnr_array(a, c, True), -20 * np.log10(0.21586), places=2)
        self.assertNotAlmostEqual(q.psnr_array(a, c, True), q.psnr_array(a, c, False), places=1)


class Clip(unittest.TestCase):
    def test_exterior_camera(self):
        self.assertTrue(q.far_clamp_clips_nothing(30, 60, 61, 0.01, 1000))
        self.assertFalse(q.far_clamp_clips_nothing(30, 1000, 61, 0.01, 1000))
        self.assertFalse(q.far_clamp_clips_nothing(30, 60, 1000, 0.01, 1000))
        self.assertFalse(q.far_clamp_clips_nothing(30, 60, float("nan"), 0.01, 1000))
        self.assertFalse(q.far_clamp_clips_nothing(30, 60, 61, 0.0, 1000))

    def test_interior_camera_accepted_when_far_covers_bounds(self):
        # Camera inside the bounds: nearest corner is behind the camera (negative depth).
        self.assertTrue(q.far_clamp_clips_nothing(-40.0, 55.0, 80.0, 0.01, 1000))

    def test_interior_camera_refused_when_far_too_small(self):
        self.assertFalse(q.far_clamp_clips_nothing(-40.0, 55.0, 80.0, 0.01, 60.0))  # corner at 80 > far
        self.assertFalse(q.far_clamp_clips_nothing(-40.0, 55.0, 80.0, 0.01, 80.0))  # boundary is refused
        self.assertFalse(q.far_clamp_clips_nothing(-40.0, 1500.0, 1600.0, 0.01, 1000.0))

    def test_interior_row_is_validated_end_to_end(self):
        manifest, view = good_manifest(), good_view()
        tel = good_telemetry(manifest, view)
        for t in tel.values():
            t["clip_proof"].update(min_depth=-40.0, max_depth=55.0, max_corner_distance=80.0)
        self.assertEqual(q.validate_row(manifest, view, tel, 1000.0), [])
        for t in tel.values():
            t["clip_proof"]["max_corner_distance"] = 1200.0
        self.assertTrue(any("clip proof" in f for f in q.validate_row(manifest, view, tel, 1000.0)))


class RowVerdict(unittest.TestCase):
    def setUp(self):
        self.ref = blank()
        self.arms = {a: with_block(self.ref, 100, 100, 140, 140, 200) for a in q.ARMS}

    def scores(self, sat, a_area, s1b):
        return {"sat": {"psnr": sat}, "a_area": {"psnr": a_area}, "s1b": {"psnr": s1b}}

    def test_validation_failures_win_over_good_scores(self):
        v = q.row_verdict(["x"], self.ref, self.arms, self.scores(30, 30, 40))
        self.assertEqual(v["verdict"], "INVALID")

    def test_pass_and_fail_flow_through_with_roi(self):
        v = q.row_verdict([], self.ref, self.arms, self.scores(30, 29, 29.6))
        self.assertEqual((v["verdict"], v["roi"]), ("PASS", [92, 92, 148, 148]))
        self.assertEqual(q.row_verdict([], self.ref, self.arms, self.scores(30, 29, 29.4))["verdict"], "FAIL")

    def test_empty_roi_identical_is_not_a_pass(self):
        same = {a: self.ref.copy() for a in q.ARMS}
        v = q.row_verdict([], self.ref, same, None)
        self.assertEqual(v["verdict"], "IDENTICAL_NOT_SCORED")
        self.assertFalse(v["informative"])
        # Even with perfect scores supplied, an absent crop never scores.
        self.assertNotEqual(q.row_verdict([], self.ref, same, self.scores(100, 100, 100))["verdict"], "PASS")

    def test_empty_roi_with_subthreshold_differences_is_invalid(self):
        sub = self.ref.copy()
        sub[3, 3] = 8
        v = q.row_verdict([], self.ref, {a: sub.copy() for a in q.ARMS}, None)
        self.assertEqual(v["verdict"], "INVALID")

    def test_scores_missing_is_invalid(self):
        self.assertEqual(q.row_verdict([], self.ref, self.arms, None)["verdict"], "INVALID")

    def test_perturbation_control_is_a_real_change(self):
        stripes = self.ref.copy()
        stripes[:, ::7] = 255
        shifted = q.perturb(stripes)
        self.assertFalse(np.array_equal(stripes, shifted))
        self.assertIsNotNone(q.common_roi(stripes, {"s1b": shifted}))


class FindView(unittest.TestCase):
    VIEWS = {
        "rows": [
            {"asset": "holzbank", "distance_label": "d2", "tau_pixels": 4},
            {"asset": "holzbank", "distance_label": "d6", "tau_pixels": 1},
            {"asset": "holzbank", "distance_label": "d6", "tau_pixels": 4},
        ]
    }

    def test_exact_match_by_all_three_fields(self):
        self.assertEqual(q.find_view(self.VIEWS, "holzbank_d6_tau1.0")["tau_pixels"], 1)
        self.assertEqual(q.find_view(self.VIEWS, "holzbank_d6_tau4.0")["tau_pixels"], 4)

    def test_no_match_or_ambiguous_raises(self):
        for bad in ("holzbank_d9_tau4.0", "baum3_d6_tau4.0", "garbage"):
            with self.assertRaises(q.HarnessError):
                q.find_view(self.VIEWS, bad)
        dup = {"rows": self.VIEWS["rows"] + [self.VIEWS["rows"][0]]}
        with self.assertRaises(q.HarnessError):
            q.find_view(dup, "holzbank_d2_tau4.0")


if __name__ == "__main__":
    unittest.main()
