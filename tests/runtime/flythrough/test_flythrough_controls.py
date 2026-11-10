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
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Optional, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))

import dispatch_lane as lane  # noqa: E402
import flythrough_controls as fc  # noqa: E402
import flythrough_verdict as fv  # noqa: E402
import run_probe as rp  # noqa: E402
import world_cache as wc  # noqa: E402
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


T0_STORED = 1207959552  # the captured T0 oracle: worlds.T0.flythrough.expected_unique_upload_flat.stored_bytes


def _cold(resident: Tuple[int, int] = (0, 100), read: Optional[int] = None,
          summary: Optional[dict] = None) -> dict:
    """The D-14 record a control process writes, built by its producer (world_cache), not by hand."""
    res = wc.residency([Path("T0")], lambda path: resident)
    return wc.cold_cache_record(res, T0_STORED if read is None else read, summary or _t0_summary(),
                                [fc.CONTROL_WORLD], "present")


def _warm() -> dict:
    """What C-warm leaves: every T0 page resident before the probe, no block read during it."""
    return _cold(resident=(100, 100), read=0)


def _base() -> dict:
    inputs = {k: copy.deepcopy(v) for k, v in _INPUTS.items() if k != "summary"}
    inputs["steps"]["contention"] = dict(CONTENTION_OK)
    inputs["cold_cache"] = _cold()
    return inputs


def _judge(cid: str, summary: dict, cold: Any = "cold") -> dict:
    inputs = dict(_base(), summary=summary, gate_world=fc.CONTROL_WORLD,
                  cold_cache=_cold(summary=summary) if cold == "cold" else cold)
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

    def test_c_warm_fires_on_a_warm_t0(self) -> None:
        rec = _judge("C-warm", _t0_summary(), _warm())
        self.assertEqual(rec["observed"], ["V10", fv.INVALID, "cache_warm"])
        self.assertEqual(rec["status"], "PASS", rec)

    def test_c_warm_differs_when_t0_is_cold(self) -> None:
        """Discrimination: a proven-cold T0 passes V10, so C-warm's tuple differs and the control is INVALID."""
        rec = _judge("C-warm", _t0_summary())
        self.assertEqual(rec["observed"], ["V10", fv.PASS, "cold"])
        self.assertEqual(rec["status"], fv.INVALID)

    def test_either_d14_condition_alone_fires_c_warm(self) -> None:
        resident_only = _cold(resident=(2, 100))  # D-14 item 2, reads complete
        reads_only = _cold(read=int(T0_STORED * 0.89))  # D-14 item 3, nothing resident
        for name, cold in (("residency", resident_only), ("read_bytes", reads_only)):
            rec = _judge("C-warm", _t0_summary(), cold)
            self.assertEqual(rec["observed"], ["V10", fv.INVALID, "cache_warm"], name)
            self.assertEqual(rec["status"], "PASS", (name, rec))
        self.assertEqual(_judge("C-warm", _t0_summary(), _cold(resident=(1, 100), read=int(T0_STORED * 0.9) + 1))
                         ["observed"], ["V10", fv.PASS, "cold"], "the limits themselves are inside")

    def test_no_measurement_is_missing_never_cache_warm(self) -> None:
        """A control without its own D-14 record, or one without the oracle, cannot certify C-warm."""
        no_oracle = _t0_summary()
        del no_oracle["worlds"]["T0"]["flythrough"]["expected_unique_upload_flat"]["stored_bytes"]
        unmeasured = _cold(resident=(100, 100))
        unmeasured["v10"] = wc.v10(None, T0_STORED)
        for name, cold in (("no record", None), ("no oracle", _cold(resident=(100, 100), read=0, summary=no_oracle)),
                           ("no read bytes", unmeasured)):
            rec = _judge("C-warm", _t0_summary(), cold)
            self.assertEqual(rec["observed"], ["V10", fv.INVALID, "MISSING"], name)
            self.assertEqual(rec["status"], fv.INVALID, name)
            off = _judge("C-off", _t0_summary(), cold)
            self.assertEqual(off["status"], fv.INVALID, name)
            self.assertTrue(any(n.startswith("V10") for n in off["not_evaluable"]), (name, off))

    def test_a_warm_t0_is_evaluable_for_every_other_control(self) -> None:
        """T0 is warm after the main probe: V10 is then a present-evidence INVALID, not a missing signal."""
        self.assertEqual(_judge("C-off", _t0_summary(), _warm())["status"], "PASS")
        self.assertEqual(_judge("C-starve", _starve(_t0_summary()), _warm())["status"], "PASS")

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


class _PageCache:
    """A fake page cache for T0: `warm` makes every page resident; a probe on a cold T0 reads the oracle's bytes."""

    def __init__(self, warm_works: bool = True) -> None:
        self.warm_works = warm_works
        self.warmed = False
        self.reads = 0
        self.warm_calls = 0

    def warm(self, files) -> int:
        self.warm_calls += 1
        self.warmed = self.warmed or self.warm_works
        return 0

    def residency(self, path: Path) -> Tuple[int, int]:
        return (100, 100) if self.warmed else (0, 100)

    def read_bytes(self) -> int:
        return self.reads

    def wrap(self, runner):
        def run(cmd, **kwargs):
            if not self.warmed:
                self.reads += T0_STORED
            return runner(cmd, **kwargs)

        return run


GOOD = {"C-off": _t0_summary, "C-starve": lambda: _starve(_t0_summary()),
        "C-settings": lambda: _drift_in_hold(_t0_summary()), "C-warm": _t0_summary}


class Harness(unittest.TestCase):
    def run_all(self, runner, cache: Optional[_PageCache] = None, **kw) -> dict:
        cache = cache or _PageCache()
        kw.setdefault("residency_probe", cache.residency)
        with tempfile.TemporaryDirectory() as tmp:
            return fc.run_controls(Path("run_probe.py"), Path("godot"), Path("t0"), Path(tmp), _base(),
                                   runner=cache.wrap(runner), thresholds=_THRESHOLDS,
                                   read_bytes=cache.read_bytes, warm=cache.warm,
                                   world_files=lambda d: [d / "world.gsplatworld"], **kw)

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
        runner, _ = _runner_writing({k: v() for k, v in GOOD.items()}, code=1)
        rep = self.run_all(runner)
        self.assertEqual(rep["verdict"], fv.INVALID)
        self.assertTrue(all("probe exited 1" in r["reason"] for r in rep["controls"]))

    def test_c_warm_warms_t0_and_only_c_warm_does(self) -> None:
        cache = _PageCache()
        runner, _ = _runner_writing({k: v() for k, v in GOOD.items()}, code=2)
        rep = self.run_all(runner, cache, only=["C-off", "C-warm"])
        self.assertEqual(rep["verdict"], "PASS", rep["reason"])
        self.assertEqual(cache.warm_calls, 1)
        by_id = {r["control"]: r for r in rep["controls"]}
        self.assertTrue(by_id["C-off"]["cold_cache"]["cold_proven"], "C-off ran on a cold T0")
        warm = by_id["C-warm"]["cold_cache"]
        self.assertEqual((warm["residency"]["resident_fraction"], warm["v10"]["read_bytes"]), (1.0, 0))

    def test_c_warm_without_its_forcing_is_invalid(self) -> None:
        """Mutation: a warm-up that warms nothing leaves T0 cold, V10 passes, C-warm's tuple differs."""
        runner, _ = _runner_writing({k: v() for k, v in GOOD.items()}, code=2)
        rep = self.run_all(runner, _PageCache(warm_works=False), only=["C-warm"])
        self.assertEqual(rep["verdict"], fv.INVALID)
        self.assertEqual(rep["controls"][0]["observed"], ["V10", fv.PASS, "cold"])

    def test_unmeasurable_residency_makes_every_control_invalid(self) -> None:
        def no_mincore(path: Path) -> Tuple[int, int]:
            raise wc.CacheError("mincore residency is only implemented for Linux")

        runner, _ = _runner_writing({k: v() for k, v in GOOD.items()}, code=2)
        rep = self.run_all(runner, residency_probe=no_mincore)
        self.assertEqual(rep["verdict"], fv.INVALID)
        for rec in rep["controls"]:
            self.assertEqual(rec["status"], fv.INVALID, rec["control"])
            self.assertIn("residency unmeasured", rec["cold_cache"]["error"])

    def test_every_control_firing_passes_and_the_command_is_one_t0_pass(self) -> None:
        runner, calls = _runner_writing({k: v() for k, v in GOOD.items()}, code=2)
        rep = self.run_all(runner)
        self.assertEqual(rep["verdict"], "PASS", rep["reason"])
        self.assertEqual(sorted(GOOD), sorted(fc.CONTROLS), "every control has a firing summary here")
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


DOCTEST_FIXTURES = ROOT / "tests" / "ci" / "fixtures"


def _captured_doctest(name: str) -> str:
    """Verbatim stdout of a real doctest run (tests/ci/fixtures, captured for #817 and the GPU harness)."""
    return (DOCTEST_FIXTURES / name).read_text(encoding="utf-8")


# The positive input is CAPTURED from the producer, not constructed: the verbatim stdout of
#   bin/godot.linuxbsd.editor.x86_64 --headless --test "--test-case=<fc.DOCTEST['C-thrash']['case']>"
# on the release-tests binary (dev_build=no tests=yes) built from ccccd7b5880dcd86076ae6f294ffa9a8cc6da6e4,
# from GPU lane run 42827730602 (artifact beyond-vram-flythrough-A-42827730602-1, logs/doctest_C-thrash.log,
# sha256 2a55edcace328f6f2776347920b26371a63641bb134b47e631ff3aa444b49c2b). The lane judged it PASS with
# 6 assertions. Re-capture it from a lane run if the producer changes; never type it out.
C_THRASH_CAPTURE = "doctest_c_thrash_release_tests_sample.txt"


def _one_case(text: str, failed: int = 0, assertions: int = 6) -> str:
    """A MUTATION for negative cases only: the summary lines rewritten to a one-case run's counts."""
    text = re.sub(r"\[doctest\] test cases:[^\n]*",
                  f"[doctest] test cases: 1 | {1 - failed} passed | {failed} failed | 2186 skipped", text)
    text = re.sub(r"\[doctest\] assertions:[^\n]*",
                  f"[doctest] assertions: {assertions} | {assertions - failed} passed | {failed} failed |", text)
    return re.sub(r"\[doctest\] Status: \w+", f"[doctest] Status: {'FAILURE' if failed else 'SUCCESS'}!", text)


class DoctestProofs(unittest.TestCase):
    CASE = fc.DOCTEST["C-thrash"]["case"]

    def judge(self, text: str, rc: int = 0) -> dict:
        return fc.judge_doctest("C-thrash", self.CASE, rc, text)

    def test_captured_outputs_never_pass(self) -> None:
        """Real producer output: 9 cases with self-skips, 3 cases with zero-assert skips. Neither is one proof."""
        for name in ("doctest_env_skip_sample.txt", "gpu_harness_zero_assert_sample.txt"):
            rec = self.judge(_captured_doctest(name))
            self.assertEqual(rec["status"], fv.INVALID, name)
            self.assertIn("needs exactly this one", rec["reason"])

    def test_the_capture_has_the_shape_it_claims(self) -> None:
        real = _captured_doctest(C_THRASH_CAPTURE)
        self.assertIn("[doctest] test cases: 1 | 1 passed | 0 failed | 2667 skipped", real)
        self.assertIn("[doctest] assertions: 6 | 6 passed | 0 failed |", real)
        self.assertIn("[doctest] Status: SUCCESS!", real)
        self.assertNotIn("Skipping test", real)

    def test_the_captured_passing_run_passes_and_every_other_shape_does_not(self) -> None:
        real = _captured_doctest(C_THRASH_CAPTURE)
        rec = self.judge(real)
        self.assertEqual(rec["status"], "PASS", rec)
        self.assertEqual((rec["assertions"], rec["cases_run"], rec["cases_passed"]), (6, 1, 1), rec)
        # Negative cases: mutations of the real capture (constructed on purpose, never a positive input).
        self.assertEqual(self.judge(real, rc=1)["status"], fv.FAIL)
        self.assertEqual(self.judge(_one_case(real, failed=1), rc=1)["status"], fv.FAIL)
        self.assertEqual(self.judge(_one_case(real, assertions=0))["status"], fv.INVALID)
        self.assertEqual(self.judge(real.replace("test cases: 1 |", "test cases: 2 |"))["status"], fv.INVALID,
                         "two cases are not this one proof")
        self.assertEqual(self.judge(_one_case(_captured_doctest("gpu_harness_zero_assert_sample.txt")))["status"],
                         fv.INVALID, "a self-skip counted as passed is not a proof")
        self.assertEqual(self.judge("")["status"], fv.INVALID)
        self.assertEqual(self.judge(real.split("[doctest] Status")[0])["status"], fv.INVALID, "truncated summary")

    def test_combine(self) -> None:
        self.assertEqual(fc.combine_doctests([])[0], fv.NOT_RUN)
        ok, bad, broken = {"proof": "a", "status": "PASS"}, {"proof": "b", "status": fv.FAIL, "reason": "x"}, \
            {"proof": "c", "status": fv.INVALID, "reason": "y"}
        self.assertEqual(fc.combine_doctests([ok])[0], "PASS")
        self.assertEqual(fc.combine_doctests([ok, bad])[0], fv.FAIL)
        self.assertEqual(fc.combine_doctests([ok, bad, broken])[0], fv.INVALID)
        self.assertEqual(fc.combine_doctests([ok, {"proof": "d"}])[0], fv.INVALID)

    def test_every_case_is_defined_once_by_exact_name_and_needs_no_gpu(self) -> None:
        for proof, spec in fc.DOCTEST.items():
            source = (ROOT / spec["file"]).read_text(encoding="utf-8")
            self.assertEqual(source.count(f'TEST_CASE("{spec["case"]}")'), 1, proof)
            self.assertFalse(set("*?,") & set(spec["case"]), "a wildcard or a comma would widen the filter")
            self.assertNotIn("RequiresGPU", spec["case"])
        self.assertTrue({"G7", "G8", "G9", "G3", "G2a"} <= set(fc.DOCTEST_ABSENT))
        self.assertFalse(set(fc.DOCTEST) & set(fc.DOCTEST_ABSENT))

    def step(self, binary: Path, output: Optional[str], rc: int = 0, raise_exc: Optional[Exception] = None):
        calls = []

        def runner(cmd, **kw):
            calls.append(cmd)
            if raise_exc is not None:
                raise raise_exc
            kw["stdout"].write(output)
            return subprocess.CompletedProcess(cmd, rc)

        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            out = Path(tmp) / "out"
            if binary.name == "godot":
                binary = Path(tmp) / "godot"
                binary.write_text("", encoding="utf-8")
            code = lane.doctest_proofs(binary, out, runner=runner)
            record = json.loads((out / "steps" / "doctest_proofs.json").read_text(encoding="utf-8"))
        return code, record, calls

    def test_the_step_runs_each_case_by_name_on_the_binary(self) -> None:
        code, record, calls = self.step(Path("godot"), _captured_doctest(C_THRASH_CAPTURE))
        self.assertEqual(code, lane.EXIT_OK, record)
        self.assertEqual(calls[0][1:], ["--headless", "--test", f"--test-case={self.CASE}"])
        self.assertEqual(record["data"]["verdict"], "PASS")
        self.assertEqual(record["data"]["proofs"][0]["assertions"], 6)
        self.assertIn("G7", record["data"]["absent"])
        code, record, _ = self.step(Path("godot"), _captured_doctest("doctest_env_skip_sample.txt"))
        self.assertEqual(code, lane.EXIT_INVALID)
        code, record, _ = self.step(Path("godot"), None, raise_exc=subprocess.TimeoutExpired("godot", 900))
        self.assertEqual((code, record["status"]), (lane.EXIT_INVALID, "INVALID"))

    def test_no_binary_is_not_run(self) -> None:
        for binary in (Path("does-not-exist"), Path("")):
            code, record, calls = self.step(binary, "")
            self.assertEqual((code, record["status"], calls), (lane.EXIT_NOT_RUN, "NOT_RUN", []))
            self.assertIn("no release-tests binary", record["reason"])

    def test_the_workflow_runs_the_proofs_on_the_installed_binary_in_every_mode(self) -> None:
        text = (ROOT / ".github" / "workflows" / "beyond_vram_flythrough.yml").read_text(encoding="utf-8")
        job = text[text.index("\n  flythrough:\n"):]
        step = job[job.index("- name: Doctest proofs"):]
        step = step[:step.index("\n      - name:", 1)]
        self.assertIn('dispatch_lane.py doctest-proofs --godot-binary "${{ steps.godot_bin.outputs.path }}"', step)
        self.assertNotIn("controls_only", step)
        self.assertNotIn("continue-on-error", step)
        self.assertLess(job.index("dispatch_lane.py build-info "), job.index("dispatch_lane.py doctest-proofs "))
        self.assertIn("doctest_proofs", lane.STEP_NAMES)
        self.assertIn("dev_build=no tests=yes", text[:text.index("jobs:")])


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
