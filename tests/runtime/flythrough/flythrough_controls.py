#!/usr/bin/env python3
"""Forcing controls of the beyond-VRAM fly-through (BV-13a, ADR sections 7.3 pass 1 and 8.5).

A control forces one gate family to fire and asserts the exact `(gate, verdict,
reason)` tuple the BV-09 verdict module (`flythrough_verdict.py`) reports for it.
The run is INVALID when any control's observed tuple differs from its expected
one: a wrong verdict, a wrong reason, an unexpected INVALID, or a harness failure
or crash (a probe that wrote no summary has no tuple, so it differs). A control
whose expected verdict is INVALID passes when it ends with exactly that INVALID
and reason: it proves the validity check fires.

ADR 8.5 also asks that every *other* entry in the control process be evaluable.
Here that means its signal was present: the verdict module classifies every entry
(`evidence`, #1390), and an entry whose evidence is not "present" (an absent or
unreadable signal, a checkpoint without its cap_fit record, no evaluator, no tier
value, an unknown status, or no classification at all) is not evaluable and the
control is INVALID. A FAIL or an INVALID the module computed from present
signals is evaluable (the forced setting itself makes V7 INVALID, for example).
V1 is exempt: contention is judged once for the whole job, at postflight.

Every control runs the BV-07 probe (`run_probe.py --control ID`) on T0, one
repetition, no sweep, and is judged against tier A's frozen values with T0 as
the gate world. The forcing lives in `CONTROLS` below, the only place that
defines it; `run_probe.py` reads it. Controls whose gate or forcing prerequisite
has not landed are listed in `ABSENT` with the package that brings them; they
are never faked (ADR 8.1: a gate without its prerequisite is absent).

Pure except `run_controls()`, which runs the probe per control through an
injectable runner. Standard library only.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import flythrough_verdict as fv  # noqa: E402

SCHEMA = "godotgs-flythrough-controls/1"
CONTROLS_FILE = "controls.json"
#: Controls are judged against the only gated tier's frozen values (ADR 8.3: A on the 3080).
THRESHOLDS_TIER = "A"
CONTROL_WORLD = "T0"
CONTROL_PASSES = 1
#: Contention is one record for the whole job, written at postflight, after the controls.
EXEMPT_FROM_EVALUABLE = ("V1",)

BUDGET_KEY = "rendering/gaussian_splatting/streaming/vram_budget_mb"
CAP_KEY = "rendering/gaussian_splatting/streaming/max_chunks_in_vram"

#: id -> how it is forced, what the probe changes, and the expected tuple.
#: `project_settings` override the tuned settings from the first frame; `inject` changes
#: project settings at runtime once the named checkpoint of the named pass has settled
#: and keeps them for `hold_s` seconds of held frames (they are not restored).
CONTROLS: Dict[str, Dict[str, Any]] = {
    "C-starve": {
        "how": "T0, effective budget 256 MB (ADR 8.5)",
        "project_settings": {BUDGET_KEY: 256},
        "expected": ("G6", fv.FAIL, "settle_deadline_missed"),
    },
    "C-off": {
        "how": "T0 at the tuned settings: the whole world fits the budget and the chunk cap, so the streamer "
               "never evicts (M1 run 42512864344: T0 0 evictions); the flat arm's streaming is idle",
        "expected": ("V9-evictions", fv.INVALID, "not_streaming"),
    },
    "C-settings": {
        "how": "T0, chunk cap 480 -> 400 at runtime while C3 holds (a drift inside a checkpoint hold, #1383 item 2)",
        "inject": {"pass_index": 0, "checkpoint": "C3", "hold_s": 2.0, "project_settings": {CAP_KEY: 400}},
        "expected": ("V7", fv.INVALID, "settings_drifted"),
    },
}

#: Controls of ADR 8.5 that do not run here, and why. Never counted as passing.
ABSENT: Dict[str, str] = {
    "C-blind": "V9 drawn floor absent from the thresholds: the manifest's checkpoint count is a no-LOD upper bound "
               "(M1 drawn/expected 0.14-0.82), the LOD-aware expected count comes with BV-13b's settle oracle",
    "C-lod": "G11 / G11q need T-ref and the quality module (BV-13b, BV-26a)",
    "C-drop": "G6 missing_chunks needs the manifest's expected set as the settle oracle (BV-13b); "
              "the flat settle uses the engine's own needed set",
    "C-shift/C-flip": "G12 beacons (BV-13b)",
    "C-read": "G9 absent: the flat chunk route publishes no read-error counter yet",
    "C-decode": "G9 absent: the flat chunk route publishes no gsq_* decode-error counter yet",
    "C-cap": "G10 judges overlap-record drops at the checkpoints since #1393, but the instance-count clamp a "
             "100k visible cap forces (instance_count_overflow_events) is not in G10's signal yet",
    "C-cap-h": "HLOD arm, G10h (M3, BV-16c)",
    "C-revisit": "G13 needs pass B captures (BV-13b)",
    "C-warm": "V10 absent from the thresholds until the cold-pass proof lands (BV-13a-2); the lane's "
              "cache_warm refusal (dispatch_lane.py probe) exists already",
    "C-codec-D": "codec arms (BV-C6)",
}

#: Controls proven outside the lane (ADR 8.5): name -> where.
DOCTEST: Dict[str, str] = {
    "C-thrash": "modules/gaussian_splatting/tests/test_vram_budget_regulator.h, "
                "'BV-08a thrash is visible-set refetch in a wall-clock window (C-thrash)'",
}

_REASON_CODE = re.compile(r"^([A-Za-z0-9_]+)")


def reason_code(reason: Any) -> str:
    """The leading token of a verdict reason (`not_streaming: 0 evictions` -> `not_streaming`)."""
    match = _REASON_CODE.match(str(reason or ""))
    return match.group(1) if match else ""


def _evaluated(row: dict) -> bool:
    """#1390: the module's structured classification, never the wording of the reason. Fails closed."""
    return row.get("evidence") == fv.EVIDENCE_PRESENT


def judge(control_id: str, result: Optional[dict], harness_problem: Optional[str] = None) -> Dict[str, Any]:
    """Compare one control's gate verdict with its expected tuple. Pure."""
    gate, verdict, reason = CONTROLS[control_id]["expected"]
    record: Dict[str, Any] = {"control": control_id, "how": CONTROLS[control_id]["how"],
                              "expected": [gate, verdict, reason], "observed": None, "not_evaluable": []}
    problems: List[str] = []
    if harness_problem:
        problems.append(harness_problem)
    rows = (result or {}).get("entries") or []
    match = [r for r in rows if r.get("id") == gate]
    if len(match) != 1:
        problems.append(f"no {gate} entry in the gate verdict ({(result or {}).get('reason', 'no verdict')})")
    else:
        row = match[0]
        observed = [gate, row.get("status"), reason_code(row.get("reason"))]
        record["observed"] = observed
        record["observed_reason"] = row.get("reason")
        if observed != [gate, verdict, reason]:
            problems.append(f"observed {tuple(observed)}, expected {(gate, verdict, reason)}")
    for row in rows:
        if row.get("id") in EXEMPT_FROM_EVALUABLE or row.get("id") == gate or row.get("status") == fv.NOT_APPLICABLE:
            continue
        if not _evaluated(row):
            record["not_evaluable"].append(f"{row.get('id')}: {row.get('reason')}")
    if record["not_evaluable"]:
        problems.append(f"{len(record['not_evaluable'])} other entries not evaluable")
    record["status"] = "PASS" if not problems else fv.INVALID
    record["reason"] = "observed tuple equals expected" if not problems else "; ".join(problems)
    return record


def combine(records: Sequence[dict]) -> Tuple[str, str]:
    """INVALID if any control differs from its tuple; with no control run, NOT_RUN, never PASS."""
    if not records:
        return fv.NOT_RUN, "no control ran"
    bad = [r for r in records if r.get("status") != "PASS"]
    if bad:
        return fv.INVALID, "; ".join(f"{r['control']}: {r['reason']}" for r in bad)
    return "PASS", f"{len(records)} controls fired with their expected tuples"


def report(records: Sequence[dict]) -> Dict[str, Any]:
    verdict, reason = combine(records)
    return {"schema": SCHEMA, "verdict": verdict, "reason": reason, "controls": list(records),
            "absent": dict(ABSENT), "doctest": dict(DOCTEST)}


Runner = Callable[..., subprocess.CompletedProcess]


def run_controls(
    probe_entry: Path,
    godot_binary: Path,
    world_dir: Path,
    out: Path,
    base_inputs: dict,
    runner: Runner = subprocess.run,
    only: Optional[Sequence[str]] = None,
    thresholds: Any = None,
    timeout_s: float = 2400.0,
) -> Dict[str, Any]:
    """Run every control's probe, judge it, write `out/controls.json`. Returns the report.

    `base_inputs` are the lane's verdict inputs (build_info, step records, sha); each
    control's own probe summary replaces the summary in them.
    """
    thresholds = fv.load_thresholds() if thresholds is None else thresholds
    records = []
    for cid in (only or list(CONTROLS)):
        cout = out / cid
        probe_out = cout / "probe"
        probe_out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(probe_entry), "--godot-binary", str(godot_binary),
               "--world", f"{CONTROL_WORLD}={world_dir}", "--out", str(probe_out),
               "--no-sweep", "--passes", str(CONTROL_PASSES), "--control", cid]
        harness = None
        try:
            with (cout / "probe.log").open("w", encoding="utf-8") as log:
                rc = runner(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=timeout_s, check=False).returncode
        except (OSError, subprocess.TimeoutExpired) as exc:
            rc, harness = None, f"harness failure: {type(exc).__name__}: {exc}"
        summary, summary_error = fv._read_json(probe_out / "summary.json")  # noqa: SLF001
        if rc not in (0, 2) and harness is None:
            # 2 is INVALID, which a control is meant to provoke; 1 is a crashed or failed Godot
            # process and 3 is NOT_RUN: both are harness failures.
            harness = f"harness failure: probe exited {rc}"
        if summary is None and harness is None:
            harness = f"harness failure: {summary_error}"
        inputs = dict(base_inputs, summary=summary, summary_error=summary_error, gate_world=CONTROL_WORLD)
        result = fv.evaluate(thresholds, THRESHOLDS_TIER, "flat", inputs)
        (cout / fv.GATE_VERDICT_FILE).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        record = judge(cid, result, harness)
        record["probe_exit_code"] = rc
        records.append(record)
    rep = report(records)
    out.mkdir(parents=True, exist_ok=True)
    (out / CONTROLS_FILE).write_text(json.dumps(rep, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return rep


def markdown(rep: dict) -> str:
    lines = [f"Controls (ADR 8.5): **{rep.get('verdict')}** ({rep.get('reason', '')})", "",
             "| Control | Expected | Observed | Status |", "| --- | --- | --- | --- |"]
    for r in rep.get("controls", []):
        lines.append(f"| {r['control']} | {' '.join(map(str, r['expected']))} | "
                     f"{' '.join(map(str, r['observed'] or ['-']))} | {r['status']} |")
    for cid, why in sorted(rep.get("absent", {}).items()):
        lines.append(f"| {cid} | absent | {why.replace('|', '/')} | NOT_RUN |")
    return "\n".join(lines) + "\n"
