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

    # The implemented blend (CutRequest::fade_frames, hlod_runtime_selection.h fade_cover): overlapping weights, the
    # incoming side at min(1, 2t), the outgoing side at min(1, 2(1 - t)), t = age / FADE_FRAMES. The children rise while
    # the parent stays at 1, then the parent falls while the children stay at 1: never opposite in one frame pair.
    FADE_FRAMES = 16

    @staticmethod
    def fade_cover(p):
        return min(1.0, max(0.0, 2.0 * p))

    def fade_t(self, i):
        return min(1.0, max(0.0, (i - SWITCH + 1) / self.FADE_FRAMES))

    def fade_weights(self, i):
        t = self.fade_t(i)
        w = {0: self.fade_cover(1.0 - t), 1: self.fade_cover(t), 2: self.fade_cover(t)}
        return {n: v for n, v in w.items() if v > 0.0}

    def test_every_step_of_a_complete_overlapping_fade_is_a_lod_event(self):
        # Codex P2 on #1542: the opposite-direction predicate found no event in a complete fade of the implemented curve,
        # so nothing during a fade was ever scored. Every selection of the fade changes a weight: each is an event.
        events = []
        for i in range(SWITCH, SWITCH + self.FADE_FRAMES):
            events.extend(metric.events_between(nodes(), self.fade_weights(i - 1), self.fade_weights(i))[0])
        self.assertEqual(len(events), self.FADE_FRAMES)
        self.assertTrue(all(e["parent"] == 0 for e in events))  # Grouped under the transition's parent: its footprint.
        self.assertTrue(all(e["max_dw"] < 0.999 for e in events))

    def test_the_same_change_spread_over_a_fade_is_scored_and_not_a_pop(self):
        events, summary = self.run_metric(self.fade_weights, lambda i: POP_LEVEL * self.fade_t(i))
        self.assertEqual(summary["lod_events"], self.FADE_FRAMES)
        self.assertEqual(summary["hard_switch_events"], 0)
        self.assertEqual(summary["scored_events"], self.FADE_FRAMES)
        self.assertEqual(summary["pops"], 0)

    def test_a_discontinuity_inside_a_fade_is_scored_as_a_pop(self):
        # The same fade, but the image jumps by a hard switch's worth in one frame mid-fade (what the fade exists to
        # prevent). The metric must see it: it scores the fade's own frames.
        jump = SWITCH + self.FADE_FRAMES // 2
        events, summary = self.run_metric(self.fade_weights, lambda i: POP_LEVEL * self.fade_t(i) + (POP_LEVEL if i >= jump else 0.0))
        self.assertEqual(summary["hard_switch_events"], 0)
        pops = [e for e in events if e["pop"]]
        self.assertEqual([e["step"] for e in pops], [jump])  # Only the jump; the fade's smooth steps stay quiet.

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
