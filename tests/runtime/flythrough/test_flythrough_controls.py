#!/usr/bin/env python3
"""BV-13a forcing controls (ADR 8.5): each control's tuple, the harness-failure rule, the wiring.

Input is the captured M1 run (`fixtures/bv09/`, see its README). Its T0 world is the
control world: at the tuned settings it evicted nothing, which is exactly what C-off
forces. Every other control is shown on that run with the one signal its forcing
moves, and shown to be INVALID without it, so no control can pass by construction.

Run directly or through ``python tests/ci/run_module_tests.py --guard-only``.
"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))

import dispatch_lane as lane  # noqa: E402
import flythrough_controls as fc  # noqa: E402
import flythrough_verdict as fv  # noqa: E402
import run_probe as rp  # noqa: E402
from test_flythrough_verdict import _INPUTS, _THRESHOLDS, CONTENTION_OK  # noqa: E402

CHECKPOINTS_G6 = ("C1", "C2", "C3", "C5", "C6")


def _t0_summary() -> dict:
    """The captured summary as a control process would leave it: T0 only, one pass."""
    summary = copy.deepcopy(_INPUTS["summary"])
    del summary["worlds"]["A"]
    fl = summary["worlds"]["T0"]["flythrough"]
    for p in fl["passes"]:
        p["settings_measured"] = copy.deepcopy(p["settings"])  # predates BV-13a, see test_flythrough_verdict
    fl["passes"] = fl["passes"][:1]
    return summary


def _starve(summary: dict) -> dict:
    for c in summary["worlds"]["T0"]["flythrough"]["passes"][0]["checkpoints"]:
        c["settled"] = False
        c["settle_s"] = {"status": "MISSING", "value": None, "reason": fv.SETTLE_DEADLINE_MISSED}
        c["settle_deadline_s"] = 30.0 if c["checkpoint"] in ("C4", "C7") else 15.0
    return summary


def _drift_in_hold(summary: dict) -> dict:
    s = summary["worlds"]["T0"]["flythrough"]["passes"][0]["settings_measured"]
    s.update(status="INVALID", settings_drifted=True, effective_max_chunks=[400, 480],
             reason="V7 INVALID settings_drifted: chunk cap, settings revision changed in the measured rows")
    return summary


def _base() -> dict:
    inputs = {k: copy.deepcopy(v) for k, v in _INPUTS.items() if k != "summary"}
    inputs["steps"]["contention"] = dict(CONTENTION_OK)
    return inputs


def _judge(cid: str, summary: dict) -> dict:
    inputs = dict(_base(), summary=summary, gate_world=fc.CONTROL_WORLD)
    return fc.judge(cid, fv.evaluate(_THRESHOLDS, fc.THRESHOLDS_TIER, "flat", inputs))


class Tuples(unittest.TestCase):
    def test_c_off_fires_on_the_captured_t0(self) -> None:
        rec = _judge("C-off", _t0_summary())
        self.assertEqual(rec["observed"], ["V9-evictions", fv.INVALID, "not_streaming"])
        self.assertEqual(rec["status"], "PASS", rec)

    def test_c_off_differs_when_t0_streams(self) -> None:
        summary = _t0_summary()
        summary["worlds"]["T0"]["flythrough"]["streaming_whole_run"]["chunks_evicted_sampled"]["value"] = 5000
        rec = _judge("C-off", summary)
        self.assertEqual(rec["status"], fv.INVALID)
        self.assertIn("expected", rec["reason"])

    def test_c_starve_needs_the_missed_deadlines(self) -> None:
        self.assertEqual(_judge("C-starve", _t0_summary())["status"], fv.INVALID, "T0 settles at the tuned budget")
        rec = _judge("C-starve", _starve(_t0_summary()))
        self.assertEqual(rec["observed"], ["G6", fv.FAIL, "settle_deadline_missed"])
        self.assertEqual(rec["status"], "PASS", rec)

    def test_c_settings_needs_the_drift(self) -> None:
        self.assertEqual(_judge("C-settings", _t0_summary())["status"], fv.INVALID)
        rec = _judge("C-settings", _drift_in_hold(_t0_summary()))
        self.assertEqual(rec["observed"], ["V7", fv.INVALID, "settings_drifted"])
        self.assertEqual(rec["status"], "PASS", rec)

    def test_lost_hold_evidence_never_certifies_c_settings(self) -> None:
        """Codex on #1388: a summary without settings_measured is MISSING, not settings_drifted."""
        summary = _t0_summary()
        del summary["worlds"]["T0"]["flythrough"]["passes"][0]["settings_measured"]
        rec = _judge("C-settings", summary)
        self.assertEqual(rec["observed"], ["V7", fv.INVALID, "MISSING"])
        self.assertEqual(rec["status"], fv.INVALID)
        summary = _t0_summary()
        summary["worlds"]["T0"]["flythrough"]["passes"][0]["settings_measured"].update(
            status="INVALID", settings_drifted=False, rows_without_signal=3)
        self.assertEqual(_judge("C-settings", summary)["observed"], ["V7", fv.INVALID, "MISSING"])

    def test_a_wrong_reason_is_a_differing_tuple(self) -> None:
        summary = _drift_in_hold(_t0_summary())
        summary["worlds"]["T0"]["driver"]["node_readback"] = None  # V7 now INVALID MISSING
        self.assertEqual(_judge("C-settings", summary)["status"], fv.INVALID)

    def test_another_entry_without_its_signal_makes_the_control_invalid(self) -> None:
        summary = _t0_summary()
        summary["worlds"]["T0"]["rss"]["peak_rss_bytes"] = {"status": "MISSING", "value": None, "reason": "no VmHWM"}
        rec = _judge("C-off", summary)
        self.assertEqual(rec["status"], fv.INVALID)
        self.assertTrue(any(n.startswith("G4") for n in rec["not_evaluable"]), rec)

    def test_missing_checkpoint_cap_fit_never_lets_c_off_pass(self) -> None:
        """#1390 P1 mutation: without the checkpoint cap_fit records M1-settle has no evidence."""
        for mutate in ("delete", "missing_status"):
            summary = _t0_summary()
            for c in summary["worlds"]["T0"]["flythrough"]["passes"][0]["checkpoints"]:
                if mutate == "delete":
                    del c["cap_fit"]
                else:
                    c["cap_fit"] = {"status": "MISSING", "needed_chunks": None, "chunk_cap": None,
                                    "reason": "needed_chunks absent at the checkpoint"}
            rec = _judge("C-off", summary)
            self.assertEqual(rec["observed"], ["V9-evictions", fv.INVALID, "not_streaming"], mutate)
            self.assertEqual(rec["status"], fv.INVALID, (mutate, rec))
            self.assertTrue(any(n.startswith("M1-settle") for n in rec["not_evaluable"]), (mutate, rec))

    def test_an_entry_without_a_classification_is_not_evaluable(self) -> None:
        """The controls read the structured field, never the reason text; no field fails closed."""
        result = fv.evaluate(_THRESHOLDS, fc.THRESHOLDS_TIER, "flat",
                             dict(_base(), summary=_t0_summary(), gate_world=fc.CONTROL_WORLD))
        self.assertEqual(fc.judge("C-off", result)["status"], "PASS")
        g4 = next(r for r in result["entries"] if r["id"] == "G4")
        self.assertEqual(g4["evidence"], fv.EVIDENCE_PRESENT)
        del g4["evidence"]
        rec = fc.judge("C-off", result)
        self.assertEqual(rec["status"], fv.INVALID)
        self.assertTrue(any(n.startswith("G4") for n in rec["not_evaluable"]), rec)

    def test_reason_code(self) -> None:
        self.assertEqual(fc.reason_code("not_streaming: 0 evictions"), "not_streaming")
        self.assertEqual(fc.reason_code("MISSING: x"), "MISSING")
        self.assertEqual(fc.reason_code(None), "")

    def test_no_control_is_not_run_never_pass(self) -> None:
        self.assertEqual(fc.combine([])[0], fv.NOT_RUN)


def _runner_writing(summaries: dict, code: int = 2):
    calls = []

    def runner(cmd, **kwargs):
        calls.append(cmd)
        out = Path(cmd[cmd.index("--out") + 1])
        cid = cmd[cmd.index("--control") + 1]
        if cid in summaries:
            out.mkdir(parents=True, exist_ok=True)
            (out / "summary.json").write_text(json.dumps(summaries[cid]), encoding="utf-8")
        return subprocess.CompletedProcess(cmd, code)

    return runner, calls


class Harness(unittest.TestCase):
    def run_all(self, runner) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            return fc.run_controls(Path("run_probe.py"), Path("godot"), Path("t0"), Path(tmp), _base(),
                                   runner=runner, thresholds=_THRESHOLDS)

    def test_a_stub_that_always_exits_2_makes_the_run_invalid(self) -> None:
        """ADR 8.5: a harness stub that always exits 2 must yield run INVALID."""
        runner, calls = _runner_writing({}, code=2)
        rep = self.run_all(runner)
        self.assertEqual(rep["verdict"], fv.INVALID)
        self.assertEqual(len(calls), len(fc.CONTROLS))
        for rec in rep["controls"]:
            self.assertEqual(rec["status"], fv.INVALID)
            self.assertIn("harness failure", rec["reason"])

    def test_a_crashed_probe_is_a_harness_failure_even_with_a_summary(self) -> None:
        good = {"C-off": _t0_summary(), "C-starve": _starve(_t0_summary()), "C-settings": _drift_in_hold(_t0_summary())}
        runner, _ = _runner_writing(good, code=1)
        rep = self.run_all(runner)
        self.assertEqual(rep["verdict"], fv.INVALID)
        self.assertTrue(all("probe exited 1" in r["reason"] for r in rep["controls"]))

    def test_every_control_firing_passes_and_the_command_is_one_t0_pass(self) -> None:
        good = {"C-off": _t0_summary(), "C-starve": _starve(_t0_summary()), "C-settings": _drift_in_hold(_t0_summary())}
        runner, calls = _runner_writing(good, code=2)
        rep = self.run_all(runner)
        self.assertEqual(rep["verdict"], "PASS", rep["reason"])
        for cmd in calls:
            self.assertIn("--no-sweep", cmd)
            self.assertEqual(cmd[cmd.index("--passes") + 1], "1")
            self.assertTrue(cmd[cmd.index("--world") + 1].startswith("T0="))
        self.assertIn("C-blind", rep["absent"])
        self.assertIn("C-thrash", rep["doctest"])


class Producer(unittest.TestCase):
    def test_the_probe_applies_each_control(self) -> None:
        path = {"segments": [{"pose": [0, 0, 0, 0, 0]}]}
        self.assertEqual(rp.path_plan(Path("w"), path, 1)["project_settings"], rp.TUNED_SETTINGS)
        starve = rp.path_plan(Path("w"), path, 1, control="C-starve")
        self.assertEqual(starve["project_settings"][fc.BUDGET_KEY], 256)
        self.assertNotIn("inject", starve)
        settings = rp.path_plan(Path("w"), path, 1, control="C-settings")
        self.assertEqual(settings["inject"]["project_settings"], {fc.CAP_KEY: 400})
        self.assertEqual(settings["project_settings"], rp.TUNED_SETTINGS)

    def test_the_driver_injects_after_the_checkpoint(self) -> None:
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn("await _maybe_inject(str(seg[\"checkpoint\"]), pass_index)", gd)
        self.assertIn('ProjectSettings.emit_signal("settings_changed")', gd.split("func _maybe_inject", 1)[1])


class Wiring(unittest.TestCase):
    def test_the_lane_records_the_controls_step(self) -> None:
        self.assertIn("controls", lane.STEP_NAMES)
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            t0 = Path(tmp) / "cache" / "T0"
            t0.mkdir(parents=True)
            (t0 / lane.MANIFEST_NAME).write_text("{}", encoding="utf-8")
            cw = Path(tmp) / lane.CACHE_WORLDS_FILE
            cw.write_text(json.dumps({"worlds": {"T0": str(t0)}, "cache_root": str(Path(tmp) / "cache")}),
                          encoding="utf-8")
            runner, _ = _runner_writing({}, code=2)
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = lane.controls(ROOT, Path("godot"), out, cw, runner=runner,
                                     lock=lambda *a: contextlib.nullcontext())
            record = json.loads((out / "steps" / "controls.json").read_text(encoding="utf-8"))
            self.assertTrue((out / "controls" / fc.CONTROLS_FILE).is_file())
        self.assertEqual(code, lane.EXIT_INVALID)
        self.assertEqual(record["status"], "INVALID")
        self.assertEqual(sorted(c["control"] for c in record["data"]["controls"]), sorted(fc.CONTROLS))

    def test_the_workflow_runs_the_controls_after_the_cold_probe(self) -> None:
        text = (ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml").read_text(encoding="utf-8")
        probe = text.index("dispatch_lane.py probe ")
        controls = text.index("dispatch_lane.py controls ")
        self.assertLess(probe, controls)
        self.assertLess(controls, text.index("runner_gpu_contention.py postflight", controls))
        self.assertIn("&& !inputs.controls_only }}", text[:probe].rsplit("- name:", 1)[1])
        self.assertIn("      controls_only:\n", text)

    def test_this_test_runs_in_the_guard_lane(self) -> None:
        text = (ROOT / "tests" / "ci" / "run_module_tests.py").read_text(encoding="utf-8")
        self.assertIn('"test_flythrough_controls.py"', text)


if __name__ == "__main__":
    unittest.main()
