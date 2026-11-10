#!/usr/bin/env python3
"""Gate verdict of the beyond-VRAM fly-through (BV-09, ADR section 8).

ADR: docs/architecture/adr-beyond-vram-worlds.md, sections 8.1-8.4 and the M1
checkpoint criterion in section 10. Pure: `evaluate()` reads a thresholds
document and the lane's outputs (already parsed) and returns the verdict; it
runs nothing and writes nothing. `evaluate_out_dir()` loads the outputs of one
lane run from disk; `dispatch_lane.py summarize` calls it and writes
`gate_verdict.json` next to the lane's own `verdict.json`.

Every entry of `flythrough_thresholds.json` (`validity` and `gates`) has one
evaluator here, keyed by its ID. The rules:

* An absent, unreadable or non-OK signal is INVALID with a reason, never PASS
  (`docs/governance/evidence-integrity.md`, "fail-open on absence").
* An entry without an evaluator, an entry that does not list the run's tier,
  and any status this module does not know are INVALID.
* A validity condition that does not hold makes the run INVALID (ADR 8.2); a
  contention record of VOID makes it VOID.
* Precedence: VOID > INVALID (validity) > FAIL (gates) > INVALID (gates) >
  PASS. Without a summary there is nothing to judge: NOT_RUN.
* `verdict` covers every entry. `enforced_verdict` covers only entries whose
  `mode` is `hard`; with none it is NOT_RUN, never PASS. Until BV-14 makes the
  correctness gates hard every entry is `report_only`, so the lane reports
  this verdict and does not exit on it.

V7 is judged on the measured rows of each repetition
(`passes[].settings`). The load rows before the first pass
(`settings_whole_run`) are reported under `startup_rows` and never counted:
M1 run 42512864344 has them INVALID (cap 256 -> 480 while loading, one row
without a signal) while every measured row is OK, and the verdict keeps the
two apart instead of blending them. Since BV-13a (#1383) V7 also judges
`passes[].settings_measured` (moving rows plus every checkpoint settle and
hold, without the load interval) and the node's own readback of the visible
cap and far plane. G6/G6t report a checkpoint that never settled within a
deadline at or above the limit as FAIL `settle_deadline_missed` (C-starve).
`inputs["gate_world"]` judges another world than the tier's gate world: the
BV-13a controls (`flythrough_controls.py`) run on T0.
V10 (BV-13a-2b) judges the D-14 record of the probe step (`steps/probe.json`
`data.cache`); `inputs["cold_cache"]` replaces it with a control's own record.

CLI: ``python3 tests/runtime/flythrough/flythrough_verdict.py --out DIR --tier A``
writes ``DIR/gate_verdict.json`` and exits with the section 9.1 code of the
enforced verdict (PASS 0, FAIL 1, INVALID 2, NOT_RUN 3, VOID 75).
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
THRESHOLDS_FILE = HERE / "flythrough_thresholds.json"
THRESHOLDS_SCHEMA = "godotgs-flythrough-thresholds/1"
SCHEMA = "godotgs-flythrough-gate-verdict/1"
GATE_VERDICT_FILE = "gate_verdict.json"
SUMMARY_PATH = Path("probe") / "summary.json"
BUILD_INFO_FILE = "build_info.json"

PASS, FAIL, INVALID, NOT_RUN, VOID = "PASS", "FAIL", "INVALID", "NOT_RUN", "VOID"
NOT_APPLICABLE = "NOT_APPLICABLE"
VERDICT_EXIT = {PASS: 0, FAIL: 1, INVALID: 2, NOT_RUN: 3, VOID: 75}
OK = "OK"
GIB = 1024**3
MIB = 1024**2
FINAL_MARKER = "plan completed"
SECTIONS = ("validity", "gates")
ARMS = ("flat", "hlod", "both")


class Missing(Exception):
    """A signal the entry needs is absent or unusable: the entry is INVALID."""


# --------------------------------------------------------------------------
# signal access
# --------------------------------------------------------------------------


def _get(obj: Any, path: str) -> Any:
    cur = obj
    for key in path.split("."):
        if not isinstance(cur, dict) or key not in cur:
            raise Missing(f"{path}: absent")
        cur = cur[key]
    return cur


def _finite(value: Any, what: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise Missing(f"{what}: not a finite number ({value!r})")
    return float(value)


def _metric(obj: Any, what: str) -> float:
    """A probe metric {"status": "OK", "value": x}. Any other status is not a value."""
    if not isinstance(obj, dict):
        raise Missing(f"{what}: absent")
    status = obj.get("status")
    if status != OK:
        raise Missing(f"{what}: status {status!r} ({obj.get('reason', 'no reason given')})")
    return _finite(obj.get("value"), what)


def _int(value: Any, what: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise Missing(f"{what}: not an integer ({value!r})")
    return value


def _compare(op: str, observed: float, limit: float) -> bool:
    if op == "<=":
        return observed <= limit
    if op == ">=":
        return observed >= limit
    if op == "==":
        return observed == limit
    raise Missing(f"unknown op {op!r}")


class Ctx:
    def __init__(self, thresholds: dict, tier: str, arm: str, inputs: dict) -> None:
        self.th = thresholds
        self.tier = tier
        self.arm = arm
        self.summary = inputs.get("summary")
        self.build_info = inputs.get("build_info")
        self.steps: Dict[str, Any] = inputs.get("steps") or {}
        self.sha: Optional[str] = inputs.get("sha")
        self.glob = thresholds.get("globals") or {}
        self.tier_cfg = (self.glob.get("tiers") or {}).get(tier)
        # BV-13a controls (ADR 8.5) run on T0 and are judged against tier A's frozen values.
        self.gate_world_override: Optional[str] = inputs.get("gate_world")
        # V10 (BV-13a-2b): a control process passes its own D-14 record; the lane's is in
        # the probe step record.
        self._cold_cache_given = "cold_cache" in inputs
        self._cold_cache = inputs.get("cold_cache")

    def cold_cache(self) -> Any:
        if self._cold_cache_given:
            return self._cold_cache
        rec = self.steps.get("probe")
        data = rec.get("data") if isinstance(rec, dict) else None
        return data.get("cache") if isinstance(data, dict) else None

    def gate_world_name(self) -> str:
        if self.gate_world_override:
            return str(self.gate_world_override)
        if not isinstance(self.tier_cfg, dict) or not self.tier_cfg.get("gate_world"):
            raise Missing(f"thresholds: no gate world for tier {self.tier}")
        return str(self.tier_cfg["gate_world"])

    def world(self, name: Optional[str] = None) -> dict:
        name = name or self.gate_world_name()
        ws = _get(self.summary, f"worlds.{name}")
        if not isinstance(ws, dict):
            raise Missing(f"worlds.{name}: not an object")
        return ws

    def passes(self) -> List[dict]:
        fl = _get(self.world(), "flythrough")
        if not isinstance(fl, dict) or fl.get("status") not in (None, OK):
            raise Missing(f"flythrough: status {fl.get('status') if isinstance(fl, dict) else fl!r} "
                          f"({fl.get('reason', '') if isinstance(fl, dict) else ''})")
        passes = fl.get("passes")
        if not isinstance(passes, list) or not passes:
            raise Missing("flythrough.passes: absent or empty")
        return passes

    def step_status(self, name: str) -> Optional[str]:
        rec = self.steps.get(name)
        if not isinstance(rec, dict):
            return None
        return str(rec.get("status"))


Result = Tuple[str, Any, str]
Evaluator = Callable[[dict, Any, Ctx], Result]


# --------------------------------------------------------------------------
# validity (ADR 8.2): a condition that does not hold is INVALID
# --------------------------------------------------------------------------


def v1_contention(entry: dict, limit: Any, ctx: Ctx) -> Result:
    status = ctx.step_status("contention")
    if status is None:
        return INVALID, None, "no contention record: contention CLEAN is not proven"
    if status == "VOID":
        return VOID, status, "contention postflight voided the run"
    if status != limit:
        return INVALID, status, f"contention record {status}, expected {limit}"
    return PASS, status, "contention CLEAN"


WORLD_DIGEST_FIELDS = (
    ("file_sha256", "world_file_sha256"),
    ("generator_params_sha", "generator_params_sha"),
    ("splat_count", "splat_count"),
    ("chunk_count", "chunk_count"),
    ("file_bytes", "file_bytes"),
)


def v2_committed_digests(entry: dict, limit: Any, ctx: Ctx) -> Result:
    """#1376 item 3: against the digests committed in the thresholds, not the world's own manifest."""
    committed_all = ctx.glob.get("worlds")
    if not isinstance(committed_all, dict):
        raise Missing("thresholds: globals.worlds absent")
    names = (ctx.tier_cfg or {}).get("worlds") or []
    if ctx.gate_world_override:
        # A control process (BV-13a) loads its one world only.
        names = [ctx.gate_world_override]
    if not names:
        raise Missing(f"thresholds: no worlds for tier {ctx.tier}")
    problems, observed = [], {}
    for name in names:
        committed = committed_all.get(name)
        if not isinstance(committed, dict):
            raise Missing(f"thresholds: no committed digests for world {name}")
        ws = ctx.world(name)
        for frozen_key, summary_key in WORLD_DIGEST_FIELDS:
            if summary_key not in ws:
                raise Missing(f"worlds.{name}.{summary_key}: absent")
            if ws[summary_key] != committed.get(frozen_key):
                problems.append(f"{name}.{frozen_key} {ws[summary_key]!r} != committed {committed.get(frozen_key)!r}")
        observed[name] = str(ws.get("world_file_sha256"))[:12]
    # The file itself must have been checked against the manifest the summary echoes:
    # the per-run cache verify (BV-06b) or, on a run that generated its worlds, the generator.
    verified_by = next((s for s in ("cache_check", "generate") if ctx.step_status(s) == OK), None)
    if problems:
        return INVALID, observed, "world_digest_mismatch: " + "; ".join(problems)
    if verified_by is None:
        return INVALID, observed, "no OK cache_check or generate record: the world file was not verified this run"
    return PASS, observed, f"every world matches the committed digests (file verified by {verified_by})"


def v3_vram_ratio(entry: dict, limit: Any, ctx: Ctx) -> Result:
    device = (ctx.glob.get("devices") or {}).get((ctx.tier_cfg or {}).get("device"))
    if not isinstance(device, dict):
        raise Missing(f"thresholds: no device for tier {ctx.tier}")
    min_ratio = _finite(ctx.glob.get("min_vram_ratio"), "globals.min_vram_ratio")
    per_splat = _finite(ctx.glob.get("gpu_bytes_per_splat"), "globals.gpu_bytes_per_splat")
    vram = _finite(device.get("vram_bytes"), "device vram_bytes")
    ws = ctx.world()
    adapter = _get(ws, "driver.adapter")
    splats = _int(ws.get("splat_count"), "splat_count")
    ratio = splats * per_splat / vram
    observed = {"adapter": adapter, "ratio": round(ratio, 4)}
    if adapter != device.get("adapter"):
        return INVALID, observed, f"tier_device_mismatch: adapter {adapter!r}, tier {ctx.tier} needs {device.get('adapter')!r}"
    if ratio < min_ratio:
        return INVALID, observed, f"world_too_small: {ratio:.3f} x VRAM < {min_ratio}"
    return PASS, observed, f"{ratio:.2f} x VRAM (GPU-equivalent) >= {min_ratio} on {adapter}"


def v5_path_coverage(entry: dict, limit: Any, ctx: Ctx) -> Result:
    ws = ctx.world()
    committed = (ctx.glob.get("worlds") or {}).get(ctx.gate_world_name()) or {}
    problems = []
    if ws.get("path_sha256") != committed.get("path_sha256"):
        problems.append(f"path_sha256 {ws.get('path_sha256')!r} != committed {committed.get('path_sha256')!r}")
    trace = _get(ws, "trace")
    if trace.get("status") != OK:
        problems.append(f"trace {trace.get('status')!r}: {trace.get('reason', '')}")
    rows = _int(trace.get("rows_written"), "trace.rows_written")
    frames = _int(trace.get("frames_rendered"), "trace.frames_rendered")
    in_file = _int(trace.get("rows_in_file"), "trace.rows_in_file")
    if not rows == frames == in_file:
        problems.append(f"rows_written {rows}, frames_rendered {frames}, rows_in_file {in_file}")
    passes = ctx.passes()
    if len(passes) != limit:
        problems.append(f"{len(passes)} repetitions, expected {limit}")
    expected_cps = entry.get("checkpoints")
    for p in passes:
        got = [c.get("checkpoint") for c in p.get("checkpoints") or []]
        if got != expected_cps:
            problems.append(f"pass {p.get('pass_index')}: checkpoints {got}, expected {expected_cps}")
        segments = p.get("segments")
        if not isinstance(segments, dict) or not segments:
            raise Missing(f"pass {p.get('pass_index')}: no segments")
        for sid, seg in sorted(segments.items()):
            length = _finite(seg.get("length_m"), f"segment {sid} length_m")
            cov = _metric(seg.get("coverage_m"), f"pass {p.get('pass_index')} segment {sid} coverage_m")
            if length <= 0 or cov < length * (1.0 - 1e-9):
                problems.append(f"pass {p.get('pass_index')} {sid}: coverage {cov} of {length} m")
    observed = {"repetitions": len(passes), "rows": rows}
    if problems:
        return INVALID, observed, "; ".join(problems)
    return PASS, observed, f"committed path, {len(passes)} repetitions, coverage 1.0, rows_written == frames_rendered == {rows}"


def _same_setting(observed: Any, frozen: Any) -> bool:
    if isinstance(frozen, bool) or isinstance(observed, bool):
        return isinstance(observed, bool) and isinstance(frozen, bool) and observed == frozen
    if isinstance(frozen, (int, float)) and isinstance(observed, (int, float)):
        return math.isfinite(observed) and float(observed) == float(frozen)
    return observed == frozen


#: #1383 item 1: (key in `driver.node_readback`, key in `globals.settings`).
NODE_READBACK_KEYS = (("max_splat_count", "visible_cap"), ("max_render_distance", "far_m"))


def v7_settings(entry: dict, limit: Any, ctx: Ctx) -> Result:
    frozen = ctx.glob.get("settings")
    if not isinstance(frozen, dict) or not isinstance(frozen.get("tuned"), dict):
        raise Missing("thresholds: globals.settings.tuned absent")
    tuned = frozen["tuned"]
    run_settings = _get(ctx.summary, "settings")
    problems = []
    run_tuned = run_settings.get("tuned")
    if not isinstance(run_tuned, dict):
        raise Missing("settings.tuned: absent")
    for key in sorted(set(tuned) | set(run_tuned)):
        if key not in run_tuned or key not in tuned or not _same_setting(run_tuned[key], tuned[key]):
            problems.append(f"tuned {key}: run {run_tuned.get(key)!r}, frozen {tuned.get(key)!r}")
    for key in ("visible_cap", "far_m"):
        if not _same_setting(run_settings.get(key), frozen.get(key)):
            problems.append(f"{key}: run {run_settings.get(key)!r}, frozen {frozen.get(key)!r}")
    # #1383 item 1: the cap and far plane the node reports back, not only the requested ones.
    node = _get(ctx.world(), "driver.node_readback")
    if not isinstance(node, dict):
        raise Missing("driver.node_readback: not an object")
    absent: List[str] = []
    for key, frozen_key in NODE_READBACK_KEYS:
        if key not in node:
            absent.append(f"node readback {key}: absent")
        elif not _same_setting(node[key], frozen.get(frozen_key)):
            problems.append(f"node readback {key}: {node.get(key)!r}, frozen {frozen_key} {frozen.get(frozen_key)!r}")
    readback = _get(ctx.world(), "driver.project_settings_readback")
    for key, value in tuned.items():
        if key not in readback:
            absent.append(f"readback {key}: absent")
        elif not _same_setting(readback[key], value):
            problems.append(f"readback {key}: {readback.get(key)!r}, frozen {value!r}")
    cap = tuned.get("rendering/gaussian_splatting/streaming/max_chunks_in_vram")
    budget = tuned.get("rendering/gaussian_splatting/streaming/vram_budget_mb")
    lod = frozen.get("lod_distance_multiplier")
    for p in ctx.passes():
        # `settings`: the moving rows. `settings_measured` (#1383 item 2, BV-13a): the moving
        # rows plus every checkpoint settle and hold, without the load interval. Both are
        # judged. Without the second the holds go unjudged, which is INVALID, never a pass.
        # Absent evidence is never reported as drift: `settings_drifted` is C-settings'
        # expected reason, so a lost signal must not satisfy that control (Codex on #1388).
        for field in ("settings", "settings_measured"):
            s = p.get(field)
            label = f"pass {p.get('pass_index')} {field}"
            if not isinstance(s, dict):
                absent.append(f"{label}: absent, the rows it covers were not judged")
                continue
            if s.get("rows_without_signal") != 0 or (s.get("status") != OK and s.get("settings_drifted") is not True):
                absent.append(f"{label}: measured rows {s.get('status')!r}, rows_without_signal "
                              f"{s.get('rows_without_signal')!r} ({s.get('reason', 'no reason')})")
                continue
            if s.get("status") != OK:
                problems.append(f"{label}: measured rows {s.get('status')!r} ({s.get('reason', 'no reason')})")
                continue
            if s.get("settings_drifted") is not False:
                problems.append(f"{label}: settings_drifted {s.get('settings_drifted')!r}")
            for key, want in (("effective_max_chunks", cap), ("effective_vram_budget_mb", budget),
                              ("lod_distance_multiplier", lod)):
                got = s.get(key)
                if not isinstance(got, list) or len(got) != 1 or not _same_setting(got[0], want):
                    problems.append(f"{label}: {key} {got!r}, frozen [{want!r}]")
    startup = ctx.world().get("flythrough", {}).get("settings_whole_run")
    observed = {
        "startup_rows": {
            "status": startup.get("status") if isinstance(startup, dict) else "MISSING",
            "reason": startup.get("reason", "") if isinstance(startup, dict) else "absent",
            "counted_in_verdict": False,
            "why": "ADR V7 applies to measured frames; the load rows before the first pass are reported, never counted",
        }
    }
    if absent:
        raise Missing("; ".join(absent))
    if problems:
        return INVALID, observed, "settings_drifted: " + "; ".join(problems)
    return PASS, observed, "measured rows of every repetition carry the frozen settings"


_HEX40 = re.compile(r"^[0-9a-f]{40}$")


def v8_binary(entry: dict, limit: Any, ctx: Ctx) -> Result:
    frozen = ctx.glob.get("binary")
    if not isinstance(frozen, dict):
        raise Missing("thresholds: globals.binary absent")
    bi = ctx.build_info
    if not isinstance(bi, dict):
        raise Missing(f"{BUILD_INFO_FILE}: absent")
    problems = []
    if bi.get("binary_kind") != frozen.get("kind"):
        problems.append(f"binary_kind {bi.get('binary_kind')!r}, frozen {frozen.get('kind')!r}")
    args = str(bi.get("scons_args", "")).split()
    for arg in frozen.get("required_scons_args") or []:
        if arg not in args:
            problems.append(f"scons arg {arg} missing")
    sha = bi.get("sha256")
    prov = bi.get("provenance") or {}
    run_sha = _get(ctx.summary, "binary.sha256")
    if not sha or sha != run_sha or sha != prov.get("sha256"):
        problems.append(f"sha256 build_info {sha!r}, probe {run_sha!r}, provenance {prov.get('sha256')!r}")
    commit = str(prov.get("commit", ""))
    if not _HEX40.match(commit):
        problems.append(f"provenance commit {commit!r} is not a full sha")
    elif ctx.sha and commit != ctx.sha:
        problems.append(f"provenance commit {commit} != run commit {ctx.sha}")
    observed = {"kind": bi.get("binary_kind"), "sha256": str(sha)[:12], "commit": commit[:12]}
    if problems:
        return INVALID, observed, "binary_identity: " + "; ".join(problems)
    return PASS, observed, f"{bi.get('binary_kind')} {str(sha)[:12]} at {commit[:12]}"


def v9_evictions(entry: dict, limit: Any, ctx: Ctx) -> Result:
    value = _metric(_get(ctx.world(), "flythrough.streaming_whole_run.chunks_evicted_sampled"), "chunks_evicted_sampled")
    if not _compare(entry["op"], value, _finite(limit, "limit")):
        return INVALID, value, f"not_streaming: {value:.0f} evictions, needs {entry['op']} {limit}"
    return PASS, value, f"{value:.0f} evictions"


def v9_peak_resident(entry: dict, limit: Any, ctx: Ctx) -> Result:
    ws = ctx.world()
    peak_mib = _metric(_get(ws, "flythrough.streaming_whole_run.peak_vram_mb"), "peak_vram_mb")
    per_splat = _finite(ctx.glob.get("gpu_bytes_per_splat"), "globals.gpu_bytes_per_splat")
    world_bytes = _int(ws.get("splat_count"), "splat_count") * per_splat
    fraction = peak_mib * MIB / world_bytes
    if not _compare(entry["op"], fraction, _finite(limit, "limit")):
        return INVALID, round(fraction, 4), f"resident_too_large: peak {fraction:.1%} of the world"
    return PASS, round(fraction, 4), f"peak resident {fraction:.1%} of the world"


def m1_settle(entry: dict, limit: Any, ctx: Ctx) -> Result:
    # #1390: a checkpoint without its needed/cap evidence is MISSING (raised below, so the
    # entry carries evidence "missing"), never a judged INVALID. A cap_fit of INVALID is
    # present evidence: the needed set was measured and does not fit the cap.
    problems, absent, seen = [], [], 0
    for p in ctx.passes():
        for c in p.get("checkpoints") or []:
            seen += 1
            label = f"pass {p.get('pass_index')} {c.get('checkpoint')}"
            fit = c.get("cap_fit")
            if not isinstance(fit, dict):
                absent.append(f"{label}: no needed/cap record")
                continue
            if fit.get("status") not in (OK, INVALID):
                absent.append(f"{label}: cap_fit {fit.get('status')!r} ({fit.get('reason', 'no reason')})")
                continue
            if fit.get("status") != OK:
                problems.append(f"{label}: cap_fit {fit.get('status')!r} ({fit.get('reason', 'no reason')})")
                continue
            try:
                needed = _int(fit.get("needed_chunks"), "needed_chunks")
                cap = _int(fit.get("chunk_cap"), "chunk_cap")
            except Missing as exc:
                absent.append(f"{label}: {exc}")
                continue
            if needed > cap:
                problems.append(f"{label}: priority_proof_missing (needed {needed} > cap {cap})")
            elif c.get("settled") is not True:
                problems.append(f"{label}: settle_deadline_missed (needed {needed} <= cap {cap})")
    if seen == 0:
        raise Missing("no checkpoint in any repetition")
    if absent:
        raise Missing("; ".join(absent + problems))
    if problems:
        return INVALID, seen, "; ".join(problems)
    return PASS, seen, f"{seen} checkpoints: needed <= cap and settled"


CACHE_WARM = "cache_warm"


def v10_cold_cache(entry: dict, limit: Any, ctx: Ctx) -> Result:
    """ADR V10 / D-14 items 2-4 (BV-13a-2b): the cold pass was proven cold.

    Judged from the D-14 record with the frozen values, not from the record's own
    `status`: residency <= `residency_max_fraction` before the probe, read bytes
    >= the limit x the path oracle's expected unique stored bytes, and a recorded
    `host_cache` label. A record without either number is MISSING (evidence
    "missing"), never a pass and never a judged cache_warm.
    """
    cache = ctx.cold_cache()
    if not isinstance(cache, dict):
        raise Missing("no D-14 cold-cache record: the probe did not run on the cached worlds")
    max_resident = _finite(entry.get("residency_max_fraction"), "residency_max_fraction")
    resident = _finite(_get(cache, "residency.resident_fraction"), "residency.resident_fraction")
    rec = cache.get("v10")
    if not isinstance(rec, dict):
        raise Missing("cache.v10 absent")
    if rec.get("read_bytes") is None:
        raise Missing(f"read_bytes unmeasured ({rec.get('why', 'no reason')})")
    if rec.get("expected_unique_stored_bytes") is None:
        raise Missing(f"no expected unique stored bytes ({rec.get('expected_source', rec.get('why', 'no reason'))})")
    read = _int(rec.get("read_bytes"), "read_bytes")
    expected = _int(rec.get("expected_unique_stored_bytes"), "expected_unique_stored_bytes")
    if read < 0 or expected <= 0:
        raise Missing(f"read_bytes {read} / expected {expected}: not a measurement")
    host = cache.get("host_cache")
    if not isinstance(host, str) or not host:
        raise Missing("host_cache label not recorded (D-14 item 4)")
    fraction = read / expected
    observed = {"resident_fraction": resident, "read_fraction": round(fraction, 4), "host_cache": host}
    problems = []
    if resident > max_resident:
        problems.append(f"{resident:.2%} of the world pages resident before the probe > {max_resident:.0%}")
    if not _compare(entry["op"], fraction, _finite(limit, "limit")):
        problems.append(f"read {read} B = {fraction:.3f} x the oracle's {expected} B, needs {entry['op']} {limit}")
    if problems:
        return INVALID, observed, f"{CACHE_WARM}: " + "; ".join(problems)
    return PASS, observed, (f"cold: {resident:.2%} resident before, read {fraction:.2f} x the oracle; "
                            f"host_cache {host} (present: guest-cold only)")


# --------------------------------------------------------------------------
# gates (ADR 8.3)
# --------------------------------------------------------------------------


def g1_exit(entry: dict, limit: Any, ctx: Ctx) -> Result:
    ws = ctx.world()
    code = _int(_get(ws, "process.exit_code"), "process.exit_code")
    timed_out = _get(ws, "process.timed_out")
    marker = _get(ws, "driver.reason")
    if timed_out is not False:
        return FAIL, code, f"timed_out {timed_out!r}"
    if code != limit:
        return FAIL, code, f"exit code {code}"
    if marker != FINAL_MARKER:
        return FAIL, code, f"no final marker (driver reason {marker!r})"
    return PASS, code, f"exit {code}, final marker '{FINAL_MARKER}'"


def g2a_budget(entry: dict, limit: Any, ctx: Ctx) -> Result:
    fl = _get(ctx.world(), "flythrough")
    peak = _metric(_get(fl, "streaming_whole_run.peak_vram_mb"), "peak_vram_mb")
    budgets = _get(fl, "settings_whole_run.effective_vram_budget_mb")
    if not isinstance(budgets, list) or not budgets:
        raise Missing("settings_whole_run.effective_vram_budget_mb: absent or empty")
    budget = min(_finite(b, "effective_vram_budget_mb") for b in budgets)
    pct = 100.0 * peak / budget
    status = PASS if _compare(entry["op"], pct, _finite(limit, "limit")) else FAIL
    return status, round(pct, 3), f"peak {peak:.1f} MiB of {budget:.0f} MiB effective budget ({pct:.2f} %)"


def g4_rss(entry: dict, limit: Any, ctx: Ctx) -> Result:
    rss = _metric(_get(ctx.world(), "rss.peak_rss_bytes"), "peak_rss_bytes") / GIB
    status = PASS if _compare(entry["op"], rss, _finite(limit, "limit")) else FAIL
    return status, round(rss, 3), f"peak RSS {rss:.2f} GiB"


SETTLE_DEADLINE_MISSED = "settle_deadline_missed"


def g6_settle(entry: dict, limit: Any, ctx: Ctx) -> Result:
    wanted = entry.get("checkpoints")
    if not isinstance(wanted, list) or not wanted:
        raise Missing("entry lists no checkpoints")
    if entry.get("op") != "<=":
        raise Missing(f"op {entry.get('op')!r}: a settle gate is an upper bound")
    bound = _finite(limit, "limit")
    worst, worst_at, found, missed = None, "", set(), []
    for p in ctx.passes():
        for c in p.get("checkpoints") or []:
            if c.get("checkpoint") not in wanted:
                continue
            label = f"pass {p.get('pass_index')} {c.get('checkpoint')}"
            found.add(c.get("checkpoint"))
            settle = c.get("settle_s")
            if (c.get("settled") is False and isinstance(settle, dict)
                    and settle.get("reason") == SETTLE_DEADLINE_MISSED):
                # BV-13a (C-starve, ADR 8.5): never settled within a deadline at or above the
                # limit is a measured failure. A deadline inside the limit proves nothing.
                deadline = _finite(c.get("settle_deadline_s"), f"{label} settle_deadline_s")
                if deadline < bound:
                    raise Missing(f"{label}: deadline {deadline:g} s is inside the limit {bound:g} s")
                missed.append(f"{label} (deadline {deadline:g} s)")
                continue
            value = _metric(settle, f"{label} settle_s")
            if worst is None or value > worst:
                worst, worst_at = value, label
    absent = [cp for cp in wanted if cp not in found]
    if absent:
        raise Missing(f"checkpoints {absent} absent")
    if missed:
        return FAIL, None, f"{SETTLE_DEADLINE_MISSED}: never settled at {', '.join(missed)}"
    if worst is None:
        raise Missing("no settle time")
    status = PASS if _compare(entry["op"], worst, bound) else FAIL
    return status, round(worst, 3), f"worst settle {worst:.2f} s at {worst_at}"


EVALUATORS: Dict[str, Evaluator] = {
    "V1": v1_contention,
    "V2": v2_committed_digests,
    "V3": v3_vram_ratio,
    "V5": v5_path_coverage,
    "V7": v7_settings,
    "V8": v8_binary,
    "V9-evictions": v9_evictions,
    "V9-peak-resident": v9_peak_resident,
    "M1-settle": m1_settle,
    "V10": v10_cold_cache,
    "G1": g1_exit,
    "G2a": g2a_budget,
    "G4": g4_rss,
    "G6": g6_settle,
    "G6t": g6_settle,
}


# --------------------------------------------------------------------------
# verdict
# --------------------------------------------------------------------------


def _limit(entry: dict, tier: str, glob: dict) -> Any:
    tiers = entry.get("tiers")
    if not isinstance(tiers, dict) or tier not in tiers:
        raise Missing(f"no value for tier {tier}")
    value = tiers[tier]
    if isinstance(value, str) and value.startswith("globals."):
        return _get(glob, value[len("globals."):])
    return value


#: `row["evidence"]` (#1390): whether the entry's signals were present. "missing" for every
#: row the module could not judge (absent or unreadable signal, no evaluator, no value for
#: the tier, unknown arm or status); "present" when an evaluator judged present signals,
#: whatever the status; "not_applicable" for an entry of another arm. Consumers (the
#: BV-13a controls) read this field, never the wording of `reason`.
EVIDENCE_PRESENT, EVIDENCE_MISSING, EVIDENCE_NOT_APPLICABLE = "present", "missing", "not_applicable"


def _evaluate_entry(section: str, gid: str, entry: Any, ctx: Ctx) -> dict:
    row: Dict[str, Any] = {"id": gid, "section": section, "evidence": EVIDENCE_MISSING}
    if not isinstance(entry, dict):
        row.update(status=INVALID, reason="entry is not an object")
        return row
    row.update({k: entry.get(k) for k in ("class", "mode", "op", "arm", "unit")})
    arm = entry.get("arm")
    if arm not in ARMS:
        row.update(status=INVALID, reason=f"unknown arm {arm!r}")
        return row
    if arm not in ("both", ctx.arm):
        row.update(status=NOT_APPLICABLE, reason=f"arm {arm}, run arm {ctx.arm}", evidence=EVIDENCE_NOT_APPLICABLE)
        return row
    evaluator = EVALUATORS.get(gid)
    if evaluator is None:
        row.update(status=INVALID, reason=f"no evaluator for {gid}: an entry the module cannot judge never passes")
        return row
    evidence = EVIDENCE_MISSING
    try:
        limit = _limit(entry, ctx.tier, ctx.glob)
        row["limit"] = limit if not isinstance(limit, (dict, list)) else entry["tiers"][ctx.tier]
        status, observed, reason = evaluator(entry, limit, ctx)
        evidence = EVIDENCE_PRESENT
    except Missing as exc:
        status, observed, reason = INVALID, None, f"MISSING: {exc}"
    except (TypeError, ValueError, KeyError, AttributeError) as exc:
        status, observed, reason = INVALID, None, f"unreadable signal: {type(exc).__name__}: {exc}"
    if status not in (PASS, FAIL, INVALID, VOID):
        reason = f"unknown status {status!r}: {reason}"
        status, evidence = INVALID, EVIDENCE_MISSING
    if section == "validity" and status == FAIL:
        status = INVALID  # ADR 8.2: a validity failure is INVALID, never FAIL
    row.update(status=status, observed=observed, reason=reason, evidence=evidence)
    return row


def combine(rows: Sequence[dict]) -> Tuple[str, str]:
    """VOID > INVALID (validity) > FAIL (gates) > INVALID (gates) > PASS. Nothing counted: NOT_RUN."""
    counted = [r for r in rows if r.get("status") != NOT_APPLICABLE]
    if not counted:
        return NOT_RUN, "no entry applies"
    validity = [r for r in counted if r["section"] == "validity"]
    gates = [r for r in counted if r["section"] != "validity"]
    for r in counted:
        if r["status"] == VOID:
            return VOID, f"{r['id']}: {r.get('reason', '')}"
    bad = [r for r in validity if r["status"] != PASS]
    if bad:
        return INVALID, "; ".join(f"{r['id']} {r['status']}" for r in bad)
    failed = [r for r in gates if r["status"] == FAIL]
    if failed:
        return FAIL, "; ".join(f"{r['id']} FAIL" for r in failed)
    bad = [r for r in gates if r["status"] != PASS]
    if bad:
        return INVALID, "; ".join(f"{r['id']} {r['status']}" for r in bad)
    return PASS, f"{len(counted)} entries PASS"


def evaluate(thresholds: Any, tier: str, arm: str, inputs: dict) -> dict:
    """The gate verdict of one run. Pure."""
    out: Dict[str, Any] = {"schema": SCHEMA, "tier": tier, "arm": arm, "report_only": True, "entries": []}
    if not isinstance(thresholds, dict) or thresholds.get("schema") != THRESHOLDS_SCHEMA:
        out.update(verdict=INVALID, reason="thresholds unreadable or of an unknown schema",
                   enforced_verdict=INVALID, enforced_reason="thresholds unreadable")
        return out
    out["thresholds_source_run"] = (thresholds.get("provenance") or {}).get("source_run", {}).get("run_id")
    if inputs.get("summary") is None:
        reason = inputs.get("summary_error") or "the probe wrote no summary"
        out.update(verdict=NOT_RUN, reason=f"nothing to judge: {reason}",
                   enforced_verdict=NOT_RUN, enforced_reason="nothing to judge")
        return out
    ctx = Ctx(thresholds, tier, arm, inputs)
    if ctx.tier_cfg is None:
        out.update(verdict=NOT_RUN, reason=f"tier {tier} has no thresholds (ADR 8.3 gates tier A and AB)",
                   enforced_verdict=NOT_RUN, enforced_reason="no thresholds for this tier")
        return out
    rows = []
    for section in SECTIONS:
        entries = thresholds.get(section)
        if not isinstance(entries, dict):
            rows.append({"id": section, "section": section, "status": INVALID, "reason": f"{section} absent"})
            continue
        for gid, entry in entries.items():
            rows.append(_evaluate_entry(section, gid, entry, ctx))
    out["entries"] = rows
    out["verdict"], out["reason"] = combine(rows)
    hard = [r for r in rows if r.get("mode") == "hard"]
    if hard:
        out["enforced_verdict"], out["enforced_reason"] = combine(hard)
    else:
        out["enforced_verdict"] = NOT_RUN
        out["enforced_reason"] = "no entry is hard yet (BV-14 makes the correctness gates hard); report-only"
    out["report_only"] = not hard
    return out


# --------------------------------------------------------------------------
# I/O
# --------------------------------------------------------------------------


def _read_json(path: Path) -> Tuple[Optional[Any], Optional[str]]:
    try:
        return json.loads(path.read_text(encoding="utf-8")), None
    except FileNotFoundError:
        return None, f"{path.name} absent"
    except (OSError, ValueError) as exc:
        return None, f"{path.name} unreadable: {exc}"


def load_thresholds(path: Path = THRESHOLDS_FILE) -> Any:
    data, _ = _read_json(path)
    return data


def load_inputs(out: Path, sha: Optional[str] = None) -> dict:
    summary, summary_error = _read_json(out / SUMMARY_PATH)
    build_info, _ = _read_json(out / BUILD_INFO_FILE)
    steps: Dict[str, Any] = {}
    steps_dir = out / "steps"
    if steps_dir.is_dir():
        for path in sorted(steps_dir.glob("*.json")):
            record, _ = _read_json(path)
            steps[path.stem] = record
    return {"summary": summary, "summary_error": summary_error, "build_info": build_info, "steps": steps, "sha": sha}


def evaluate_out_dir(out: Path, tier: str, arm: str = "flat", sha: Optional[str] = None,
                     thresholds_path: Path = THRESHOLDS_FILE) -> dict:
    result = evaluate(load_thresholds(thresholds_path), tier, arm, load_inputs(out, sha))
    (out / GATE_VERDICT_FILE).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return result


def markdown(result: dict) -> str:
    lines = [
        "## Beyond-VRAM gate verdict (BV-09, report-only until BV-14)",
        "",
        f"Gate verdict: **{result.get('verdict')}** ({result.get('reason', '')}); "
        f"enforced: **{result.get('enforced_verdict')}** ({result.get('enforced_reason', '')})",
        "",
        "| Entry | Class | Mode | Status | Observed | Limit | Detail |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for r in result.get("entries", []):
        observed = r.get("observed")
        if isinstance(observed, (dict, list)):
            observed = json.dumps(observed, sort_keys=True)[:80]
        lines.append(
            f"| `{r.get('id')}` | {r.get('class', '')} | {r.get('mode', '')} | {r.get('status')} | "
            f"{observed} | {r.get('op', '')} {r.get('limit', '')} | {str(r.get('reason', '')).replace('|', '/')} |"
        )
    return "\n".join(lines) + "\n"


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--out", type=Path, required=True, help="the lane's output directory")
    parser.add_argument("--tier", required=True)
    parser.add_argument("--arm", default="flat", choices=("flat", "hlod"))
    parser.add_argument("--sha", default=None, help="the commit the run built (GITHUB_SHA)")
    parser.add_argument("--thresholds", type=Path, default=THRESHOLDS_FILE)
    args = parser.parse_args(argv)
    result = evaluate_out_dir(args.out, args.tier, args.arm, args.sha, args.thresholds)
    print(markdown(result))
    return VERDICT_EXIT[result["enforced_verdict"]]


if __name__ == "__main__":
    sys.exit(main())
