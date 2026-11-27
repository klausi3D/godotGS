"""Constructed verdict inputs, NOT producer captures. ADR dynamics §7."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("gs_game_1_verdict", HERE / "gs_game_1_verdict.py")
V = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(V)


def frames(ms=12.0, count=20):
    return [{"sample": i, "input_digest": f"pose-{i}", "frame": i + 1,
             "gpu_frame": i + 1, "frame_ms": ms, "gpu_frame_ms": ms,
             "gpu_frame_valid": True, "spawn": i == 10} for i in range(count)]


def p1_timeline(engine_ms=12.0, harness_ms=0.0, count=20, start=1_000_000, drop=None):
    """A P1 measurement in the post_draw-to-post_draw shape (#1552).

    Simulates the sampling loop: each interval holds the engine's frame plus the
    harness bookkeeping that ran after the previous boundary. `drop` removes one
    sample, as a producer that lost an interval would.
    """
    rows, boundary = [], start
    for i in range(count):
        interval = round((engine_ms + harness_ms) * 1000)
        boundary += interval
        rows.append({"sample": i, "input_digest": f"pose-{i}", "frame": i + 1, "gpu_frame": i + 1,
                     "frame_ms": interval / 1000, "boundary_usec": boundary, "drawn_frames": 1,
                     "harness_ms": harness_ms, "script_ms": 0.5,
                     "engine_ms": interval / 1000 - harness_ms - 0.5,
                     "gpu_frame_ms": engine_ms, "gpu_frame_valid": True,
                     "viewport_gpu_ms": engine_ms + 1, "viewport_cpu_ms": 2.0, "frame_setup_cpu_ms": 0.5,
                     "process_ms": 1.0, "physics_ms": 0.25, "spawn": i == 10})
    if drop is not None:
        del rows[drop]
    return {"frames": rows, "frame_basis": "post_draw_to_post_draw",
            "capture_start_usec": start, "capture_end_usec": boundary}


def identity(changed=0):
    return {"arms": [{"divisor": d, "wind_time_scale": 0, "wind_nonzero": True,
                      "hysteresis": "disabled", "settled": True,
                      "drawn_nodes_before": [2], "evicted_nodes": [2], "preoccupied_pages": [3],
                      "pages_before": [3], "pages_after": [7],
                      "drawn_digest_before": "abc", "drawn_digest_after": "abc",
                      "frames_compared": 2, "baseline_sha256": "baseline",
                      "comparisons": [{"changed_pixels": changed, "image_sha256": "image"}, {"changed_pixels": 0, "image_sha256": "image"}], "changed_frames": changed} for d in (8, 0)]}


def record():
    measurements = {
        "P1": p1_timeline(),
        "P2": {"on": frames(13), "off": frames(12)},
        "H1": {"settled": True, "nonresident_drawn": 0, "missing_nodes": 0, "expected_nodes": 8},
        "W1": identity(),
        "W2": {"errors_px": [0.1], "samples": 1, "children_displacement_px": 2.0, "distance_fade": 1.0},
        "W3": {"errors_shadow_texels": [0.1], "samples": 1},
        "W4": {"weight_zero_displacement_px": [0.0], "samples": 1},
        "C1": {"native_first_frame": {"frame": 20, "absent_at_arm": [2], "first_readmitted_nodes": [2], "drawn_digest": "abc"},
               "native_reference_frame": {"frame": 21, "drawn_digest": "abc"}, "changed_pixels": [0], "first_frame_after_admission": True,
               "divisor": 8, "reference_divisor": 1, "same_pose": True, "same_drawn_digest": True},
        "R1": {"lods": [{"level": 0, "region_pixels": 8, "tinted_pixels": 8, "foreign_pixels": 0}],
               "expected_lods": [0], "expected_source": "tree_region_volume"},
        "R2": {"set_frame": 20, "same_frame_effect": True, "survives_far_evict_near": True},
        "R3": {"hidden_region_sort_records": 0, "hidden_region_shadow_pixels": 0,
               "visible_region_records": 4},
        "S1": {"before_drawn_nodes": 8, "spawn_drawn_nodes": [8, 8], "root_nodes": 1, "new_asset": True},
        "O2": {"object_offsets_px": [0.1], "caster_colour_offsets_texels": [0.1]},
        "L1": {"lit_pixels": 8, "lighting_verified": True},
        "L2": {"hlod": True, "omni_splat_shadow_pixels": 8, "spot_splat_shadow_pixels": 8},
        "M1": {"ledger_bytes": 100, "external_vram_bytes": 120, "upload_bytes_per_second": 10},
        "F3": {"region_keyed_interior_bytes": 110, "single_region_interior_bytes": 100,
               "world": "synthetic T0", "producer": "B2"},
    }
    bad = copy.deepcopy(measurements)
    bad["P1"] = p1_timeline(45) | {"calibration": {"attempts": 2,
        "bounded_attempts": 8, "calibrated": True, "iterations": 4096,
        "samples": [{"iterations": 1024, "measurement": {"stall_ms": 10, "stall_valid": True}},
                    {"iterations": 4096, "measurement": {"stall_ms": 31, "stall_valid": True}}]}, "stalled_frame_count": 2,
        "stalls": [{"frame": i + 1, "stall_ms": 31, "stall_valid": True} for i in (0, 10)]}
    bad["H1"]["nonresident_drawn"] = 1
    bad["W1"] = identity(1)
    bad["W2"]["errors_px"] = [1]
    bad["W3"]["errors_shadow_texels"] = [2]
    bad["W4"]["weight_zero_displacement_px"] = [1]
    bad["C1"]["changed_pixels"] = [1]
    bad["R1"]["lods"][0]["foreign_pixels"] = 1
    bad["R2"]["same_frame_effect"] = False
    bad["R3"]["hidden_region_sort_records"] = 2
    bad["S1"]["spawn_drawn_nodes"] = [1]
    bad["O2"]["object_offsets_px"] = [1]
    return {"schema": "gs-game-1/raw/v1", "job_id": "constructed-job",
            "hardware": "RTX 3080", "ledger_cap_bytes": 8_000_000_000,
            "approximation": V.O6_LABEL, "subject_run": "subject",
            "rows": {k: {"measurement": m, "control": {"job_id": "constructed-job",
                "run": "control-" + k, "switch_observed": True,
                "measurement": bad[k]}} for k, m in measurements.items()}}


class VerdictTests(unittest.TestCase):
    def evaluate(self, r=None):
        return V.verdict(record() if r is None else r)

    def test_all_rows_and_o6(self):
        result = self.evaluate()
        self.assertEqual(set(result["rows"]), set(V.ROW_IDS))
        self.assertIn(V.O6_LABEL, V.markdown(result))
        self.assertIn("16.7", V.markdown(result))
        self.assertEqual(result["rows"]["O1"]["state"], V.O1_STATE)
        for row in V.CONTROLLED:
            expected = V.MEASURED if row in ("R2", "S1") else "PASS"
            self.assertEqual(result["rows"][row]["state"], expected, row)

    def test_each_controlled_row_needs_its_own_red_same_job_control(self):
        for row in V.CONTROLLED:
            for mutation in ("missing", "green", "other_job", "same_run", "unobserved"):
                with self.subTest(row=row, mutation=mutation):
                    r = record()
                    item = r["rows"][row]
                    c = item["control"]
                    if mutation == "missing": del item["control"]
                    if mutation == "green": c["measurement"] = copy.deepcopy(item["measurement"])
                    if mutation == "other_job": c["job_id"] = "other"
                    if mutation == "same_run": c["run"] = r["subject_run"]
                    if mutation == "unobserved": c["switch_observed"] = False
                    self.assertNotIn(self.evaluate(r)["rows"][row]["state"],
                                     ("PASS", V.NO_CONTROL_PASS, V.MEASURED))

    def test_missing_row_or_control_is_not_run(self):
        for row in V.ROW_IDS:
            if row == "O1": continue
            r = record()
            del r["rows"][row]
            self.assertEqual(self.evaluate(r)["rows"][row]["state"], "NOT_RUN", row)
        for row in V.CONTROLLED:
            r = record()
            del r["rows"][row]["control"]
            self.assertEqual(self.evaluate(r)["rows"][row]["state"], "NOT_RUN", row)

    def test_named_not_run_reason_reaches_the_report_and_never_passes(self):
        reason = "churn cannot reach full eviction: 44/63; 13 non-drawn non-ancestor nodes stay resident"
        for row in ("W1", "C1", "R2"):
            r = record()
            r["rows"][row]["measurement"]["not_run_reason"] = reason
            out = self.evaluate(r)["rows"][row]
            self.assertEqual(out["state"], "NOT_RUN", row)
            self.assertEqual(out["measurement"], {"reason": reason}, row)
            self.assertIn(reason, V.markdown(self.evaluate(r)))
        # An empty or non-string reason is not a reason: the row is judged as before.
        baseline = self.evaluate()["rows"]["W1"]["state"]
        for value in ("", None, 0):
            r = record()
            r["rows"]["W1"]["measurement"]["not_run_reason"] = value
            self.assertEqual(self.evaluate(r)["rows"]["W1"]["state"], baseline)

    def test_w1_digest_mismatch_invalid_in_either_arm(self):
        for divisor in (0, 8):
            r = record()
            for arm in r["rows"]["W1"]["measurement"]["arms"]:
                if arm["divisor"] == divisor: arm["drawn_digest_after"] = "different"
            self.assertEqual(self.evaluate(r)["rows"]["W1"]["state"], "INVALID")

    def test_w1_requires_the_entire_oracle(self):
        for field, value in (("preoccupied_pages", []), ("pages_after", [3]),
                             ("drawn_nodes_before", [2, 3]), ("baseline_sha256", ""),
                             ("comparisons", [{"changed_pixels": -1, "image_sha256": ""}]),
                             ("wind_time_scale", 1), ("wind_nonzero", False),
                             ("hysteresis", "unknown"), ("settled", False)):
            r = record()
            r["rows"]["W1"]["measurement"]["arms"][0][field] = value
            self.assertNotEqual(self.evaluate(r)["rows"]["W1"]["state"], "PASS", field)

    def test_w2_vacuous_probe_is_not_run_in_either_arm(self):
        # Run 45231549548: a probe 270 m from pose A compared 0 px with 0 px and
        # "passed"; its control could not be red either. Neither arm is evidence.
        for arm in ("subject", "control"):
            for field, value in (("children_displacement_px", 0.0), ("children_displacement_px", 0.999),
                                 ("distance_fade", 0.0), ("children_displacement_px", None)):
                with self.subTest(arm=arm, field=field, value=value):
                    r = record()
                    item = r["rows"]["W2"]
                    m = item["measurement"] if arm == "subject" else item["control"]["measurement"]
                    if value is None:
                        del m[field]
                    else:
                        m[field] = value
                    self.assertEqual(self.evaluate(r)["rows"]["W2"]["state"], "NOT_RUN")
        # The floor itself is a pass condition, not a threshold change.
        r = record()
        r["rows"]["W2"]["measurement"]["children_displacement_px"] = 1.0
        self.assertEqual(self.evaluate(r)["rows"]["W2"]["state"], "PASS")

    def test_c1_needs_native_first_readmission_and_reference(self):
        for field in ("native_first_frame", "native_reference_frame"):
            r = record()
            del r["rows"]["C1"]["measurement"][field]
            self.assertEqual(self.evaluate(r)["rows"]["C1"]["state"], "NOT_RUN")

    def test_postflight_void_overrides_all_evaluated_rows(self):
        r = record()
        r["invalid_reason"] = "VOID: contention"
        result = self.evaluate(r)
        self.assertEqual(result["state"], "INVALID")
        for row in V.ROW_IDS:
            self.assertEqual(result["rows"][row]["state"], V.O1_STATE if row == "O1" else "INVALID")

    def test_p1_calibration_sampling_and_separation(self):
        for field, value in (("stalled_frame_count", 1), ("stalls", []),
                             ("calibration", {"calibrated": False})):
            r = record()
            r["rows"]["P1"]["control"]["measurement"][field] = value
            self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")
        r = record()
        r["rows"]["P1"]["control"]["measurement"]["calibration"]["samples"][-1]["measurement"]["stall_valid"] = False
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")
        for field, value in (("stall_ms", 29.9), ("stall_valid", False)):
            r = record()
            r["rows"]["P1"]["control"]["measurement"]["stalls"][0][field] = value
            self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")

    def test_p1_old_window_frames_are_not_evidence(self):
        # #1552: frames timed from the loop top to post_draw leave the harness's own
        # per-sample work out of P1. A record without the post_draw-to-post_draw
        # basis cannot say what its frame_ms covers, so it never passes.
        for key in ("frame_basis", "capture_start_usec", "capture_end_usec"):
            with self.subTest(missing=key):
                r = record()
                del r["rows"]["P1"]["measurement"][key]
                self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")
        r = record()
        r["rows"]["P1"]["measurement"]["frame_basis"] = "loop_top_to_post_draw"
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")

    def test_p1_frames_must_tile_the_capture_window(self):
        # Run 45497531161: 1,200 samples summed to 23.373 s in a 24.214 s window.
        # A lost interval, or a frame_ms that is not its boundary interval, is a
        # population that does not cover the window: INVALID, never PASS.
        r = record()
        r["rows"]["P1"]["measurement"] = p1_timeline(drop=5)
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "INVALID")
        r = record()
        r["rows"]["P1"]["measurement"]["frames"][3]["frame_ms"] -= 0.8  # old window: bookkeeping cut out
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "INVALID")
        r = record()
        r["rows"]["P1"]["measurement"]["capture_end_usec"] += 841
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "INVALID")
        result = self.evaluate()["rows"]["P1"]["measurement"]
        self.assertAlmostEqual(result["interval_sum_ms"], result["capture_window_ms"], places=6)
        self.assertEqual(result["window_tolerance_ms"], V.P1_WINDOW_TOLERANCE_MS)

    def test_p1_out_of_window_delay_worsens_p1_and_is_reported_as_harness_overhead(self):
        # The forcing control's arithmetic: 15 ms of harness work after the boundary
        # (outside the old window) must move P1 from PASS to FAIL, and must show up
        # as harness overhead, not vanish.
        clean = self.evaluate()["rows"]["P1"]
        self.assertEqual(clean["state"], "PASS")
        r = record()
        r["rows"]["P1"]["measurement"] = p1_timeline(harness_ms=15.0)
        forced = self.evaluate(r)["rows"]["P1"]
        self.assertEqual(forced["measurement"]["p95_ms"], 27.0)
        self.assertNotEqual(forced["state"], "PASS")
        self.assertEqual(forced["measurement"]["harness_overhead_ms"]["total"], 15.0 * 20)
        self.assertEqual(clean["measurement"]["harness_overhead_ms"]["total"], 0.0)

    def test_p1_reports_each_timing_source_separately_and_names_what_is_absent(self):
        values = self.evaluate()["rows"]["P1"]["measurement"]
        breakdown = values["breakdown_p95_ms"]
        for key in ("gs_gpu_passes", "engine_gpu_viewport", "engine_cpu_viewport", "engine_cpu_frame_setup",
                    "script", "harness", "engine_main_loop"):
            self.assertIn(key, breakdown)
        self.assertEqual(breakdown["gs_gpu_passes"], 12.0)
        self.assertIn("present", values["not_measured"])
        # A source the producer did not record is named, never reported as zero.
        r = record()
        for f in r["rows"]["P1"]["measurement"]["frames"]:
            del f["viewport_cpu_ms"]
        values = self.evaluate(r)["rows"]["P1"]["measurement"]
        self.assertNotIn("engine_cpu_viewport", values["breakdown_p95_ms"])
        self.assertIn("engine_cpu_viewport", values["not_measured"])

    def test_p1_breakdown_never_decides_the_state(self):
        r = record()
        for f in r["rows"]["P1"]["measurement"]["frames"]:
            f.update(viewport_gpu_ms=500.0, viewport_cpu_ms=500.0, frame_setup_cpu_ms=500.0,
                     process_ms=500.0, physics_ms=500.0)
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "PASS")

    def test_uncontrolled_rows_need_measurement_proof(self):
        for row, field in (("P2", "off"), ("L1", "lit_pixels"),
                           ("L2", "spot_splat_shadow_pixels"), ("M1", "external_vram_bytes"),
                           ("F3", "single_region_interior_bytes")):
            r = record()
            del r["rows"][row]["measurement"][field]
            self.assertNotIn(self.evaluate(r)["rows"][row]["state"], ("PASS", V.NO_CONTROL_PASS))
        r = record()
        r["rows"]["P2"]["measurement"]["off"][0]["input_digest"] = "other"
        self.assertEqual(self.evaluate(r)["rows"]["P2"]["state"], "NOT_RUN")
        r["rows"]["P2"]["measurement"]["off"] = frames()
        r["rows"]["P2"]["measurement"]["off"][0]["gpu_frame_valid"] = False
        self.assertEqual(self.evaluate(r)["rows"]["P2"]["state"], "NOT_RUN")

    def test_r1_skipped_level_never_passes(self):
        # PR #1532 review: a level the sweep never reached must stay missing coverage.
        good = {"level": 0, "region_pixels": 8, "tinted_pixels": 8, "foreign_pixels": 0}
        # (a) The tree expects 0, 5 and 7; only 0 was measured: NOT_RUN naming 5 and 7.
        r = record()
        r["rows"]["R1"]["measurement"] = {"lods": [good], "expected_lods": [0, 5, 7], "expected_source": "tree_region_volume"}
        out = self.evaluate(r)["rows"]["R1"]
        self.assertEqual(out["state"], "NOT_RUN")
        self.assertIn("[5, 7]", out["measurement"]["reason"])
        # (b) An expected set that is not tree-derived (the circular sweep-derived one)
        # cannot pass, even when it equals what was measured.
        for source in (None, "sweep"):
            r = record()
            m = {"lods": [good], "expected_lods": [0]}
            if source:
                m["expected_source"] = source
            r["rows"]["R1"]["measurement"] = m
            self.assertEqual(self.evaluate(r)["rows"]["R1"]["state"], "NOT_RUN", source)
        # (c) A measured level outside the tree's set is not coverage either.
        r = record()
        r["rows"]["R1"]["measurement"]["lods"].append(dict(good, level=3))
        self.assertEqual(self.evaluate(r)["rows"]["R1"]["state"], "NOT_RUN")

    def test_l2_missing_shadow_is_fail(self):
        r = record()
        r["rows"]["L2"]["measurement"]["spot_splat_shadow_pixels"] = 0
        self.assertEqual(self.evaluate(r)["rows"]["L2"]["state"], "FAIL")

    def test_no_adr_number_never_passes(self):
        for row in ("L1", "L2", "M1", "R2", "S1"):
            self.assertEqual(self.evaluate()["rows"][row]["state"], V.MEASURED, row)
        self.assertEqual(self.evaluate()["rows"]["P2"]["state"], V.NO_CONTROL_PASS)
        self.assertEqual(self.evaluate()["rows"]["F3"]["state"], V.NO_CONTROL_PASS)

    def test_numeric_boundaries_and_missing_validity(self):
        for row, field, bad in (("W2", "errors_px", [0.5001]),
                                ("W3", "errors_shadow_texels", [1.0001]),
                                ("W4", "weight_zero_displacement_px", [0.0001])):
            r = record()
            r["rows"][row]["measurement"][field] = bad
            self.assertEqual(self.evaluate(r)["rows"][row]["state"], "FAIL")
        r = record()
        r["rows"]["P1"]["measurement"]["frames"][0]["gpu_frame_valid"] = False
        self.assertEqual(self.evaluate(r)["rows"]["P1"]["state"], "NOT_RUN")

    def test_threshold_quotes_are_verbatim(self):
        text = (HERE.parents[1] / "docs/architecture/adr-dynamics-on-hlod.md").read_text(encoding="utf-8")
        result = self.evaluate()
        for row in V.ROW_IDS:
            line = next(s for s in text.splitlines() if s.startswith(f"| {row} |"))
            quote = line.split("|")[3].strip()
            self.assertEqual(result["rows"][row]["adr_threshold"], quote)
        expectations = json.loads((HERE / "gs_game_1_expectations.json").read_text(encoding="utf-8"))
        self.assertNotIn("upload_threshold", expectations)
        # The control column too: a rewritten ADR control must reach the contract.
        for row, contract in expectations["rows"].items():
            line = next(s for s in text.splitlines() if s.startswith(f"| {row} |"))
            self.assertEqual(contract["control_verbatim"], line.split("|")[4].strip(), row)


if __name__ == "__main__":
    unittest.main()
