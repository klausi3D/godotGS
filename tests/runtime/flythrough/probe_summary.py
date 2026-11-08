#!/usr/bin/env python3
"""Trace parser and summary arithmetic for the BV-07 probe lane.

ADR: docs/architecture/adr-beyond-vram-worlds.md D-4 (frame model F + k*N), D-6
(regulator events, ``settings_drifted``), sections 7.3 (passes), 8.2 (validity) and
8.3 (P1 percentile method: time-weighted, nearest rank).

Report-only, but absence of a signal is never a pass: every number this module puts
into the summary is a ``metric()`` record carrying ``status`` OK / MISSING / INVALID
and a reason. A metric that could not be computed is MISSING with the reason, never
0 and never omitted.

Freshness (BV-04b). The pass-timing snapshot is held while nothing resolves, so a
row's GPU numbers count only when the coherent frame was refreshed for that row:
``coherent_resolve_usec`` advanced since the previous row, all four BV-04b passes
are in the coherent mask, and the coherent serial is non-zero. A held sample is
counted as stale, never as a measurement.

Standard library only.
"""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

TRACE_MAGIC = b"GSBV7TR1"
TRACE_SCHEMA = "godotgs-bv07-probe-trace/1"
RESULT_SCHEMA = "godotgs-bv07-probe-result/1"
SUMMARY_SCHEMA = "godotgs-bv07-probe-summary/1"

PASS_NAMES = (
    "cull", "projection", "presort", "overlap_count", "prefix", "binning",
    "overlap_sort", "raster", "resolve", "composite",
)
#: renderer/gpu_pass_timing.h BV04B_PASSES_MASK: cull | projection | presort | composite.
BV04B_PASSES_MASK = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 9)

STATE_MOVING = 0
STATE_SETTLING = 1
STATE_HOLD = 2
STATE_MEASURE = 3
STATE_WARMUP = 4

OK = "OK"
MISSING = "MISSING"
INVALID = "INVALID"

#: ADR G15 / G14 frame-interval bounds, used here only to count frames (report-only).
SLOW_FRAME_MS = 50.0
HITCH_FRAME_MS = 250.0


class TraceError(ValueError):
    """The trace file is not a well-formed BV-07 trace."""


def metric(value, status: str = OK, reason: str = "", unit: str = "") -> Dict[str, object]:
    if status == OK and (value is None or (isinstance(value, float) and not math.isfinite(value))):
        status, reason = MISSING, reason or "no finite value"
    rec: Dict[str, object] = {"value": value if status == OK else None, "status": status}
    if unit:
        rec["unit"] = unit
    if reason:
        rec["reason"] = reason
    return rec


def missing(reason: str, unit: str = "") -> Dict[str, object]:
    return metric(None, MISSING, reason, unit)


# --------------------------------------------------------------------------
# Binary trace
# --------------------------------------------------------------------------


class Trace:
    def __init__(self, fields: List[str], types: str, rows: List[tuple], trailing_bytes: int) -> None:
        self.fields = fields
        self.types = types
        self.rows = rows
        self.trailing_bytes = trailing_bytes
        self.index = {name: i for i, name in enumerate(fields)}

    def col(self, name: str) -> List:
        i = self.index[name]
        return [r[i] for r in self.rows]

    def get(self, row: tuple, name: str):
        return row[self.index[name]]


def read_trace(path: Path) -> Trace:
    data = Path(path).read_bytes()
    if len(data) < 12 or data[:8] != TRACE_MAGIC:
        raise TraceError(f"{path}: not a BV-07 trace (bad magic)")
    (hlen,) = struct.unpack_from("<I", data, 8)
    if 12 + hlen > len(data):
        raise TraceError(f"{path}: header length {hlen} runs past the file")
    try:
        header = json.loads(data[12:12 + hlen].decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise TraceError(f"{path}: header is not JSON: {exc}") from exc
    if header.get("schema") != TRACE_SCHEMA:
        raise TraceError(f"{path}: schema {header.get('schema')!r}, expected {TRACE_SCHEMA}")
    fields = list(header.get("fields", []))
    types = str(header.get("types", ""))
    if not fields or len(fields) != len(types) or set(types) - {"q", "d"}:
        raise TraceError(f"{path}: field list and type string disagree")
    fmt = "<" + types
    size = struct.calcsize(fmt)
    body = data[12 + hlen:]
    n = len(body) // size
    rows = [struct.unpack_from(fmt, body, i * size) for i in range(n)]
    trace = Trace(fields, types, rows, len(body) - n * size)
    for i, r in enumerate(rows):
        if trace.get(r, "row") != i:
            raise TraceError(f"{path}: row {i} carries index {trace.get(r, 'row')}")
    return trace


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------


def nearest_rank(values: Sequence[float], q: float) -> Optional[float]:
    if not values:
        return None
    s = sorted(values)
    k = max(1, math.ceil(q * len(s)))
    return s[min(k, len(s)) - 1]


def time_weighted_percentile(intervals_ms: Sequence[float], q: float) -> Optional[float]:
    """Nearest-rank percentile of the frame interval where each frame weighs its duration.

    The value v such that the frames with interval <= v cover at least q of the wall
    time (ADR section 8.3, P1: "time-weighted, nearest-rank").
    """
    vals = [v for v in intervals_ms if v > 0.0 and math.isfinite(v)]
    if not vals:
        return None
    total = sum(vals)
    acc = 0.0
    for v in sorted(vals):
        acc += v
        if acc >= q * total - 1e-9:
            return v
    return max(vals)


def mean(values: Sequence[float]) -> Optional[float]:
    return sum(values) / len(values) if values else None


def linear_fit(xs: Sequence[float], ys: Sequence[float]) -> Optional[Dict[str, float]]:
    """Ordinary least squares y = a + b x. None with fewer than two distinct x."""
    n = len(xs)
    if n < 2 or len(set(xs)) < 2:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    a = my - b * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a + b * x)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0
    return {"intercept": a, "slope": b, "r2": r2, "points": n,
            "max_abs_residual": max(abs(y - (a + b * x)) for x, y in zip(xs, ys))}


# --------------------------------------------------------------------------
# Row selections
# --------------------------------------------------------------------------


def frame_intervals(trace: Trace, rows: Sequence[int]) -> List[float]:
    """Wall-clock interval (ms) ending at each selected row; the first row of the trace has none."""
    t = trace.col("t_usec")
    out = []
    for i in rows:
        if i > 0:
            out.append((t[i] - t[i - 1]) / 1000.0)
    return out


def fresh_gpu_rows(trace: Trace, rows: Sequence[int]) -> Tuple[List[int], Dict[str, int]]:
    """Rows whose coherent GPU breakdown was refreshed for that row (BV-04b freshness)."""
    res = trace.col("coherent_resolve_usec")
    mask = trace.col("coherent_pass_mask")
    serial = trace.col("coherent_frame_serial")
    fresh: List[int] = []
    counts = {"rows": 0, "fresh": 0, "held": 0, "incomplete_mask": 0, "no_serial": 0}
    for i in rows:
        counts["rows"] += 1
        if serial[i] == 0:
            counts["no_serial"] += 1
            continue
        if (mask[i] & BV04B_PASSES_MASK) != BV04B_PASSES_MASK:
            counts["incomplete_mask"] += 1
            continue
        if i == 0 or res[i] <= res[i - 1] or serial[i] == serial[i - 1]:
            counts["held"] += 1
            continue
        fresh.append(i)
        counts["fresh"] += 1
    return fresh, counts


def gpu_breakdown(trace: Trace, rows: Sequence[int]) -> Dict[str, object]:
    fresh, counts = fresh_gpu_rows(trace, rows)
    out: Dict[str, object] = {"freshness": counts}
    if not fresh:
        out["coherent_total_ms"] = missing("no row with a fresh coherent GPU breakdown carrying all BV-04b passes", "ms")
        out["passes"] = {p: missing("no fresh coherent frame", "ms") for p in PASS_NAMES}
        return out
    tot = [trace.get(trace.rows[i], "coherent_total_ms") for i in fresh]
    out["coherent_total_ms"] = {
        "mean": metric(mean(tot), unit="ms"),
        "p50": metric(nearest_rank(tot, 0.5), unit="ms"),
        "p95": metric(nearest_rank(tot, 0.95), unit="ms"),
        "samples": len(tot),
    }
    passes = {}
    for bit, name in enumerate(PASS_NAMES):
        vals = [
            trace.get(trace.rows[i], f"{name}_ms")
            for i in fresh
            if (trace.get(trace.rows[i], "coherent_pass_mask") >> bit) & 1
        ]
        passes[name] = (
            {"mean": metric(mean(vals), unit="ms"), "samples": len(vals)}
            if vals
            else missing(f"pass {name} not in the coherent mask of any fresh row", "ms")
        )
    out["passes"] = passes
    return out


def interval_stats(intervals: Sequence[float]) -> Dict[str, object]:
    if not intervals:
        return {"frames": 0, "mean": missing("no frame interval", "ms")}
    n = len(intervals)
    return {
        "frames": n,
        "mean": metric(mean(intervals), unit="ms"),
        "p95_time_weighted": metric(time_weighted_percentile(intervals, 0.95), unit="ms"),
        "p99_time_weighted": metric(time_weighted_percentile(intervals, 0.99), unit="ms"),
        "max": metric(max(intervals), unit="ms"),
        "frames_over_50ms": sum(1 for v in intervals if v > SLOW_FRAME_MS),
        "frames_over_50ms_pct": metric(100.0 * sum(1 for v in intervals if v > SLOW_FRAME_MS) / n, unit="%"),
        "frames_over_250ms": sum(1 for v in intervals if v > HITCH_FRAME_MS),
    }


def _signal_rows(trace: Trace, rows: Sequence[int], name: str) -> List[int]:
    """Rows in which the render stats carried ``name`` (the driver writes -1 when absent)."""
    return [i for i in rows if trace.get(trace.rows[i], name) >= 0]


def settings_drift(trace: Trace, rows: Sequence[int]) -> Dict[str, object]:
    """ADR V7 / D-6: the frozen settings must hold in every measured frame.

    ``rows`` are the measured rows. The verdict is INVALID, never a bare OK, when

    - the effective settings changed: the settings revision or the regulator adjustment
      count moved, or the effective budget, chunk cap or ``lod_distance_multiplier`` took
      more than one value (``settings_drifted``, ADR section 8.3 C-settings);
    - any measured row lacks the signal (the driver writes -1): V7 could not see that
      frame, and absence of a signal is no pass (#1356).

    If no measured row carries the signal the result is MISSING.
    """
    if not rows:
        return {"settings_drifted": None, "status": MISSING, "reason": "no measured rows"}
    have = [i for i in _signal_rows(trace, rows, "effective_settings_revision")
            if trace.get(trace.rows[i], "vram_regulation_adjustments") >= 0]
    if not have:
        return {"settings_drifted": None, "status": MISSING,
                "reason": "effective_settings_revision / vram_regulation_adjustments absent from every measured row"}
    rev = [trace.get(trace.rows[i], "effective_settings_revision") for i in have]
    adj = [trace.get(trace.rows[i], "vram_regulation_adjustments") for i in have]
    budget = sorted(set(trace.get(trace.rows[i], "effective_vram_budget_mb") for i in have))
    cap = sorted(set(trace.get(trace.rows[i], "effective_max_chunks") for i in have))
    lod = sorted(set(trace.get(trace.rows[i], "lod_distance_multiplier") for i in have))
    changed = [name for name, values in (("budget", budget), ("chunk cap", cap), ("lod_distance_multiplier", lod))
               if len(values) > 1]
    if max(rev) != min(rev):
        changed.append("settings revision")
    if max(adj) != min(adj):
        changed.append("regulator adjustments")
    drifted = bool(changed)
    without = len(rows) - len(have)
    reasons = []
    if drifted:
        reasons.append(f"V7 INVALID settings_drifted: {', '.join(changed)} changed in the measured rows")
    if without:
        reasons.append(f"V7 INVALID rows_without_signal: {without} of {len(rows)} measured rows "
                       "carry no settings signal")
    rec: Dict[str, object] = {
        "status": INVALID if reasons else OK,
        "settings_drifted": drifted,
        "label": "settings_drifted" if drifted else "",
        "rows": len(rows),
        "rows_without_signal": without,
        "revision_first": rev[0], "revision_last": rev[-1],
        "revision_changes": sum(1 for a, b in zip(rev, rev[1:]) if a != b),
        "regulation_adjustments_delta": adj[-1] - adj[0],
        "effective_vram_budget_mb": budget,
        "effective_max_chunks": cap,
        "lod_distance_multiplier": lod,
    }
    if reasons:
        rec["reason"] = "; ".join(reasons)
    return rec


#: The cumulative streaming counters start at 0 when the process starts and are never
#: reset (docs/features/streaming.md, BV-04a).
COUNTER_INITIAL_VALUE = 0


def streaming_counters(trace: Trace, rows: Sequence[int]) -> Dict[str, object]:
    """Counter totals over a contiguous window of rows.

    A cumulative counter's window total is (last valid sample in the window) minus the
    baseline: the last valid sample BEFORE the window (the preceding boundary), or the
    known initial value 0 when there is none. Using the first sample inside the window
    as the baseline would drop whatever the counter did before that sample (Codex on
    #1333: 4.3 MB of reads before the first sampled frame of the captured path).
    """
    if not rows:
        return {"status": MISSING, "reason": "no rows"}
    first = min(rows)

    def delta(name: str) -> Dict[str, object]:
        have = _signal_rows(trace, rows, name)
        if not have:
            return missing(f"{name} absent from the streaming state in every row")
        before = [i for i in range(first - 1, -1, -1) if trace.get(trace.rows[i], name) >= 0][:1]
        base = trace.get(trace.rows[before[0]], name) if before else COUNTER_INITIAL_VALUE
        rec = metric(trace.get(trace.rows[have[-1]], name) - base)
        rec["baseline"] = "preceding_sample" if before else "initial_zero"
        rec["rows_without_signal"] = len(rows) - len(have)
        return rec

    def peak(name: str, unit: str = "") -> Dict[str, object]:
        have = _signal_rows(trace, rows, name)
        if not have:
            return missing(f"{name} absent from the streaming state in every row", unit)
        return metric(max(trace.get(trace.rows[i], name) for i in have), unit=unit)

    ev_rows = _signal_rows(trace, rows, "chunks_evicted_this_frame")
    return {
        "upload_bytes": delta("upload_bytes_total"),
        "upload_chunks": delta("upload_chunks_total"),
        "read_bytes": delta("read_bytes_total"),
        "evicted_mb": delta("evicted_bytes_total_mb"),
        "chunks_evicted_sampled": metric(sum(trace.get(trace.rows[i], "chunks_evicted_this_frame") for i in ev_rows))
        if ev_rows else missing("chunks_evicted_this_frame absent in every row"),
        "peak_loaded_chunks": peak("loaded_chunks"),
        "peak_vram_mb": peak("vram_mb", "MiB"),
        "upload_stall_wall_ms_max": peak("upload_stall_wall_ms", "ms"),
        "probe_stats_usec_mean": metric(mean([trace.get(trace.rows[i], "stats_usec") for i in rows]), unit="usec"),
    }


def rows_where(trace: Trace, **conds) -> List[int]:
    idx = {k: trace.index[k] for k in conds}
    out = []
    for i, r in enumerate(trace.rows):
        ok = True
        for k, v in conds.items():
            val = r[idx[k]]
            if isinstance(v, (set, frozenset, tuple, list)):
                if val not in v:
                    ok = False
                    break
            elif val != v:
                ok = False
                break
        if ok:
            out.append(i)
    return out


# --------------------------------------------------------------------------
# F / k
# --------------------------------------------------------------------------


def fit_frame_model(points: Sequence[Dict[str, object]], y_key: str) -> Dict[str, object]:
    """Fit mean ms = F + k * N (N in millions of drawn splats) over the sweep points.

    ``points`` carry ``drawn_mean`` and ``y_key`` (a metric record). Points whose
    y is not OK are excluded and listed; fewer than two usable points is MISSING.
    """
    xs, ys, used, excluded = [], [], [], []
    for p in points:
        y = p.get(y_key) or {}
        drawn = p.get("drawn_mean")
        if p.get("valid_for_fit") is False:
            excluded.append({"point": p.get("id"), "reason": "not settled: measured with chunks missing"})
            continue
        if not isinstance(y, dict) or y.get("status") != OK or drawn is None or drawn <= 0:
            excluded.append({"point": p.get("id"), "reason": (y or {}).get("reason", "no drawn count") if isinstance(y, dict) else "no value"})
            continue
        xs.append(float(drawn) / 1e6)
        ys.append(float(y["value"]))
        used.append(p.get("id"))
    fit = linear_fit(xs, ys)
    if fit is None:
        return {"status": MISSING, "reason": f"fewer than two usable sweep points ({len(xs)})",
                "excluded": excluded}
    return {
        "status": OK,
        "F_intercept_ms": fit["intercept"],
        "k_ms_per_million": fit["slope"],
        "r2": fit["r2"],
        "max_abs_residual_ms": fit["max_abs_residual"],
        "points_used": used,
        "excluded": excluded,
        "x_drawn_millions": xs,
        "y_ms": ys,
    }
