#!/usr/bin/env python3
"""CPU contracts of the HLOD pop metric (hlod_pop_metric.py) on constructed dollies; no GPU, no captures.

The images are synthetic: a textured frame that slides one pixel per step (the camera motion), and inside one
node's footprint either a step change of the mean colour in one frame (a hard switch) or the same change spread
over many frames (a fade). The metric must flag the first, not the second, and must ignore frustum events.
"""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("hlod_pop_metric", HERE / "hlod_pop_metric.py")
metric = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metric)

W, H = 96, 64
STEPS = 40
SWITCH = 20
# A footprint mean-colour step of 4 levels: about the 95th percentile of the hard switches measured on the reef dolly.
POP_LEVEL = 4.0


def nodes():
    # Root 0 over children 1 and 2. Node 1 sits in front of a camera at the origin looking down -Z; node 2 is far
    # off to the side, outside the frame. Only parent links and AABBs matter to the metric.
    return {
        0: (-1, 1, (-2.0, -1.0, -6.0), (4.0, 2.0, 2.0)),
        1: (0, 0, (-1.0, -0.5, -5.5), (2.0, 1.0, 1.0)),
        2: (0, 0, (40.0, 0.0, -6.0), (1.0, 1.0, 1.0)),
    }


def frame(step, level_at):
    rng = np.random.default_rng(7)
    texture = rng.uniform(40, 200, size=(H, W + STEPS, 3)).astype(np.float32)
    img = texture[:, step:step + W, :].copy()  # Camera motion: one pixel per step.
    rect = metric.project_rect(*nodes()[0][2:], (0.0, 0.0, 0.0), (0.0, 0.0, -1.0), 75.0, W, H, 0.05)
    x0, y0, x1, y1 = rect
    img[y0:y1, x0:x1, :] += level_at(step)  # The representation's mean colour inside the node's footprint.
    return img


def write_run(directory, weights_at, level_at):
    rows = []
    images = {}
    for i in range(STEPS):
        w = weights_at(i)
        rows.append({"step": i, "cam_pos": [0.0, 0.0, 0.0], "look_at": [0.0, 0.0, -1.0], "shot": f"frames/{i:04d}.png",
                     "drawn_node_ids": sorted(w), "drawn_node_fades": [w[n] for n in sorted(w)]})
        images[f"frames/{i:04d}.png"] = frame(i, level_at)
    Path(directory, "dolly.json").write_text(json.dumps({"rows": rows}), encoding="utf-8")
    Path(directory, "meta.json").write_text(json.dumps({"camera": {"fov_deg": 75.0, "near_m": 0.05}}), encoding="utf-8")
    return lambda path: images[Path(path).relative_to(directory).as_posix()]


class PopMetricTests(unittest.TestCase):
    def run_metric(self, weights_at, level_at):
        with tempfile.TemporaryDirectory() as d:
            loader = write_run(d, weights_at, level_at)
            events, rows = metric.score(d, nodes(), image_loader=loader)
            return events, metric.summarize(events, rows)

    def test_hard_switch_is_a_pop_and_controls_stay_quiet(self):
        events, summary = self.run_metric(
            lambda i: {0: 1.0} if i < SWITCH else {1: 1.0, 2: 1.0},
            lambda i: 0.0 if i < SWITCH else POP_LEVEL)
        self.assertEqual(summary["lod_events"], 1)
        self.assertEqual(summary["hard_switch_events"], 1)
        self.assertEqual(summary["pops"], 1)
        self.assertEqual(events[0]["step"], SWITCH)
        self.assertEqual(events[0]["parent"], 0)
        self.assertTrue(events[0]["scored"])
        self.assertEqual(summary["control_pops"], 0)

    def test_the_same_change_spread_over_a_fade_is_not_a_pop(self):
        frames = 12

        def t(i):
            return min(1.0, max(0.0, (i - SWITCH + 1) / frames))
        events, summary = self.run_metric(
            lambda i: {n: w for n, w in ((0, 1.0 - t(i)), (1, t(i)), (2, t(i))) if w > 0.0},
            lambda i: POP_LEVEL * t(i))
        self.assertGreaterEqual(summary["lod_events"], frames - 1)
        self.assertEqual(summary["hard_switch_events"], 0)
        self.assertGreater(summary["scored_events"], 0)
        self.assertEqual(summary["pops"], 0)

    def test_camera_motion_alone_is_not_a_pop(self):
        _, summary = self.run_metric(lambda i: {0: 1.0}, lambda i: 0.0)
        self.assertEqual(summary["lod_events"], 0)
        self.assertEqual(summary["pops"], 0)

    def test_frustum_entry_is_not_a_lod_event(self):
        # Child 2 enters (no node leaves): nothing replaced anything.
        _, summary = self.run_metric(lambda i: {1: 1.0} if i < SWITCH else {1: 1.0, 2: 1.0}, lambda i: 0.0)
        self.assertEqual(summary["lod_events"], 0)

    def test_event_without_a_full_baseline_window_is_unscored_not_passed(self):
        # A switch in the second step has no full window before it: unscored, never a pass.
        events, summary = self.run_metric(
            lambda i: {0: 1.0} if i < 2 else {1: 1.0, 2: 1.0},
            lambda i: 0.0 if i < 2 else POP_LEVEL)
        self.assertEqual(summary["lod_events"], 1)
        self.assertEqual(summary["scored_events"], 0)
        self.assertEqual(summary["unscored_events"], 1)
        self.assertEqual(summary["pops"], 0)
        self.assertIsNone(summary["pop_rate"])

    def test_a_second_switch_in_the_window_does_not_hide_a_pop(self):
        # Two hard switches three steps apart (out, then back): both stay pops (the baseline is a median).
        events, summary = self.run_metric(
            lambda i: {1: 1.0, 2: 1.0} if SWITCH <= i < SWITCH + 3 else {0: 1.0},
            lambda i: POP_LEVEL if SWITCH <= i < SWITCH + 3 else 0.0)
        self.assertEqual(summary["lod_events"], 2)
        self.assertEqual(summary["pops"], 2)

    def test_projection_matches_a_centred_box(self):
        rect = metric.project_rect((-1.0, -1.0, -11.0), (2.0, 2.0, 2.0), (0.0, 0.0, 0.0), (0.0, 0.0, -1.0), 90.0, 100, 100, 0.05)
        # Half-size 1 at depth 9 (the near face) with tan(45 deg) = 1: +-1/9 of the half screen around the centre.
        self.assertEqual(rect, (44, 44, 56, 56))
        self.assertIsNone(metric.project_rect((50.0, 0.0, -2.0), (1.0, 1.0, 1.0), (0.0, 0.0, 0.0), (0.0, 0.0, -1.0), 90.0, 100, 100, 0.05))


if __name__ == "__main__":
    unittest.main()
