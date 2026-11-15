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


def shared_layers_settings_summary(samples: object, rows: Sequence[int]) -> Dict[str, object]:
    """D3-6 readbacks for the HLOD route, which has no chunk regulator.

    These samples are taken by the probe in the same row as the trace. Every
    measured row must have complete, stable process readbacks; a plan or a
    one-time initial readback cannot substitute for the samples.
    """
    if not rows or not isinstance(samples, list):
        return {"status": MISSING, "settings_drifted": None, "reason": "no measured settings samples"}
    by_row = {}
    dictionaries = ("project_settings_readback", "node_readback", "camera")
    counters = ("sh_bands", "visible_splats", "drawn_splats")
    keys = dictionaries + ("resolution",) + counters
    for sample in samples:
        if not isinstance(sample, dict) or type(sample.get("row")) is not int or sample["row"] in by_row:
            return {"status": INVALID, "settings_drifted": None, "reason": "invalid or duplicate settings sample row"}
        by_row[sample["row"]] = sample
    baseline = None
    for row in rows:
        sample = by_row.get(row)
        if (not isinstance(sample, dict) or any(not sample.get(key) for key in dictionaries)
                or any(not isinstance(sample.get(key), dict) for key in dictionaries)
                or not isinstance(sample.get("resolution"), list) or len(sample["resolution"]) != 2
                or any(type(sample.get(key)) not in (int, float) or not math.isfinite(sample[key])
                       or sample[key] < 0 for key in counters)):
            return {"status": MISSING, "settings_drifted": None,
                    "reason": f"measured row {row} lacks complete route settings readback"}
        current = {key: sample[key] for key in keys}
        if baseline is None:
            baseline = current
        elif current != baseline:
            return {"status": INVALID, "settings_drifted": True,
                    "reason": f"route settings changed in measured row {row}"}
    return {"status": OK, "settings_drifted": False, "rows": len(rows), "rows_without_signal": 0,
            "scope": "per-row process ProjectSettings, node, camera and viewport readbacks", "readback": baseline}


def checkpoint_cap_fit(needed: object, cap: object) -> Dict[str, object]:
    """ADR M1 settle-where-fits criterion: does a checkpoint's needed set fit the chunk cap?

    ``needed`` is the engine's ``needed_chunks`` at the checkpoint, ``cap`` the chunk cap
    the engine enforced in the same frame (trace ``effective_max_chunks``).

    - needed <= cap: OK; the checkpoint must settle (missing chunks = 0, section 7.3).
    - needed > cap: the needed set can never be resident at once, so settle is undefined.
      The checkpoint needs a priority proof instead (the most important chunks resident
      and drawn first). No such check exists yet, so the verdict is INVALID, never a pass.
    - either number absent (or not positive): MISSING. Absence of a signal is no pass.
    """
    ok_needed = isinstance(needed, int) and not isinstance(needed, bool) and needed > 0
    ok_cap = isinstance(cap, int) and not isinstance(cap, bool) and cap > 0
    rec: Dict[str, object] = {"needed_chunks": needed if ok_needed else None,
                              "chunk_cap": cap if ok_cap else None}
    if not ok_needed or not ok_cap:
        absent = [n for n, ok in (("needed_chunks", ok_needed), ("chunk cap", ok_cap)) if not ok]
        rec.update(status=MISSING, settle_required=None,
                   reason=f"{' and '.join(absent)} absent at the checkpoint: cannot tell whether it must settle")
        return rec
    if needed > cap:
        rec.update(status=INVALID, settle_required=False,
                   reason=f"needed {needed} > cap {cap}: the needed set cannot be resident at once; "
                          "the checkpoint requires a priority proof, which does not exist yet")
        return rec
    rec.update(status=OK, settle_required=True)
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


#: Gate repair: per-frame completeness columns of the trace (probe_world.gd FIELDS; -1 where
#: the streaming state did not publish the key). `needed_not_resident_chunks` counts every
#: needed chunk that is not GPU-resident in that frame, queued and in-flight ones included.
COVERAGE_COLUMNS = ("needed_not_resident_chunks", "needed_upload_pending_chunks",
                    "visible_chunks_evicted_this_frame")
#: ADR 8.2 V9 pre-registers "at every checkpoint, drawn >= 50 % of the manifest's expected
#: count". The expected count is the no-LOD upper bound (probe_groundtruth), so LOD thinning
#: falls below it as well as missing chunks do.
V9_DRAWN_FLOOR = 0.5
#: The expected count of a pose needs a frustum test of every chunk of the world (about 10 ms
#: per pose on tier A in Python), so the drawn fraction is sampled on every Nth moving row.
#: The per-frame columns are read on every moving row.
DRAWN_FRACTION_STRIDE = 10
POSE_COLUMNS = ("px", "py", "pz", "yaw_deg", "pitch_deg")


def moving_coverage(trace: Trace, rows: Sequence[int], expected_at=None, stride: int = DRAWN_FRACTION_STRIDE,
                    floor: float = V9_DRAWN_FLOOR) -> Dict[str, object]:
    """How complete the view was while moving, per frame (gate repair, report-only).

    * ``needed_not_resident``: frames in which at least one needed chunk was not GPU-resident.
    * ``upload_pending`` / ``visible_evicted``: the upload-pending part of those chunks, and
      visible chunks evicted in the frame.
    * ``drawn_fraction``: drawn splats over ``expected_at(pose)`` (the no-LOD upper bound of
      the frustum chunks at the row's pose) on every ``stride``-th row, and how many sampled
      frames fell below ``floor``. The render stats can lag the pose by a frame or two; at
      moving speeds that shifts the expected count by a few chunks at most.

    A column the trace does not have (an older driver), or a moving row without the signal,
    makes the record MISSING: a frame without the signal could hide a hole.
    """
    if not rows:
        return {"status": MISSING, "reason": "no moving rows"}
    absent = [c for c in COVERAGE_COLUMNS if c not in trace.index]
    if absent:
        return {"status": MISSING, "moving_frames": len(rows),
                "reason": f"the trace has no {absent} column (an older probe driver)"}
    out: Dict[str, object] = {"moving_frames": len(rows)}
    seg_ix = trace.index["segment"]
    per_seg: Dict[str, Dict[str, int]] = {}
    for i in rows:
        rec = per_seg.setdefault(str(trace.rows[i][seg_ix]), {"frames": 0, "needed_not_resident_frames": 0,
                                                               "drawn_samples": 0, "drawn_below_floor": 0})
        rec["frames"] += 1
        if trace.get(trace.rows[i], "needed_not_resident_chunks") > 0:
            rec["needed_not_resident_frames"] += 1
    missing_signal = []

    def per_frame(name: str) -> Dict[str, object]:
        vals = [trace.get(trace.rows[i], name) for i in rows if trace.get(trace.rows[i], name) >= 0]
        gap = len(rows) - len(vals)
        if gap:
            missing_signal.append(f"{name} absent in {gap} of {len(rows)} moving rows")
        if not vals:
            return {"frames_with_signal": 0, "rows_without_signal": gap}
        gt0 = sum(1 for v in vals if v > 0)
        return {"frames_with_signal": len(vals), "rows_without_signal": gap, "frames_gt0": gt0,
                "share_gt0": gt0 / len(vals), "max": max(vals), "mean": mean(vals), "total": sum(vals)}

    out["needed_not_resident"] = per_frame("needed_not_resident_chunks")
    out["upload_pending"] = per_frame("needed_upload_pending_chunks")
    out["visible_evicted"] = per_frame("visible_chunks_evicted_this_frame")
    if missing_signal:
        out.update(status=MISSING, reason="; ".join(missing_signal))
    else:
        out["status"] = OK

    step = max(1, int(stride))
    drawn: Dict[str, object] = {"stride": step, "floor": floor,
                                "expected": "no-LOD upper bound: the splats of the frustum chunks at the row's pose"}
    if expected_at is None:
        drawn.update(status=MISSING, reason="no manifest: the expected count of a moving pose cannot be computed")
    else:
        fractions: List[float] = []
        no_expected = no_signal = 0
        for i in rows[::step]:
            row = trace.rows[i]
            got = trace.get(row, "drawn_splats")
            if got < 0:
                no_signal += 1
                continue
            exp = expected_at([trace.get(row, c) for c in POSE_COLUMNS])
            if exp <= 0:
                no_expected += 1
                continue
            frac = got / exp
            fractions.append(frac)
            rec = per_seg[str(row[seg_ix])]
            rec["drawn_samples"] += 1
            rec["drawn_below_floor"] += 1 if frac < floor else 0
        below = sum(1 for f in fractions if f < floor)
        drawn.update(samples=len(fractions), rows_without_signal=no_signal, rows_without_expected=no_expected,
                     frames_below_floor=below)
        if no_signal or not fractions:
            drawn.update(status=MISSING, reason=f"{no_signal} sampled rows without drawn_splats, "
                                                f"{len(fractions)} usable samples")
        else:
            drawn.update(status=OK, share_below_floor=below / len(fractions), min=min(fractions),
                         p05=nearest_rank(fractions, 0.05), p50=nearest_rank(fractions, 0.5))
    out["drawn_fraction"] = drawn
    out["per_segment"] = per_seg
    return out


#: #1393: the renderer's cumulative overlap-record drop counters, one trace column each
#: (probe_world.gd DROP_FIELDS; -1 where the renderer did not publish the key).
DROP_COUNTERS = (
    "drop_signal_samples", "overflow_drop_events", "binning_drop_events",
    "binning_tile_range_drop_events", "binning_budget_drop_events", "raster_tile_cap_drop_events",
    "sampled_dropped_records_total", "sampled_overlap_records_total", "sampled_overlap_splats_total",
)


def _counter_increments(trace: Trace, name: str) -> Tuple[Dict[int, int], List[int]]:
    """Per-row increments of one cumulative counter: row -> value minus the previous valid sample
    (the first valid sample counts from 0, the renderer's initial value). Rows without the signal
    (-1) get no increment. The second list names the rows where the counter went backwards."""
    inc: Dict[int, int] = {}
    backwards: List[int] = []
    prev = 0
    for i, row in enumerate(trace.rows):
        v = trace.get(row, name)
        if v < 0:
            continue
        if v < prev:
            backwards.append(i)
        inc[i] = v - prev
        prev = v
    return inc, backwards


def overlap_drops(trace: Trace, rows: Sequence[int]) -> Dict[str, object]:
    """#1393: overlap records the tile renderer dropped (missing splats), over the selected rows.

    The counters are cumulative per renderer and sampled by an async readback, so each row is
    credited with what its counters advanced since the previous row that carried them: a drop
    lands in the row whose stats saw the readback, a few frames after the frame that dropped.
    Statuses:

    - MISSING when the trace has no drop columns (an older probe), no selected row carries them
      (the renderer did not publish them), or ``drop_signal_samples`` did not advance in the rows:
      no readback completed, so zero drops would be unproven (absence of a signal is no pass).
    - INVALID when a counter went backwards (the renderer was recreated): the window cannot be summed.
    - OK otherwise. ``drop_events`` counts readback intervals with a drop on any channel;
      ``sampled_dropped_records`` is a lower bound (frames between two samples are not counted).
      ``records_per_splat`` is the sampled frames' attempted records over their splats.
    """
    absent = [n for n in DROP_COUNTERS if n not in trace.index]
    if absent:
        return {"status": MISSING, "reason": f"the trace has no drop telemetry columns {absent} (older probe)"}
    if not rows:
        return {"status": MISSING, "reason": "no rows"}
    selected = set(rows)
    sums: Dict[str, int] = {}
    with_signal = 0
    for name in DROP_COUNTERS:
        inc, backwards = _counter_increments(trace, name)
        bad = [i for i in backwards if i in selected]
        if bad:
            return {"status": INVALID, "reason": f"{name} went backwards at row {bad[0]} (renderer recreated?)"}
        sums[name] = sum(v for i, v in inc.items() if i in selected)
        if name == "drop_signal_samples":
            with_signal = sum(1 for i in selected if i in inc)
    if with_signal == 0:
        return {"status": MISSING, "reason": "no selected row carries the drop telemetry (the renderer did not publish it)"}
    if sums["drop_signal_samples"] <= 0:
        return {"status": MISSING, "rows_with_signal": with_signal,
                "reason": "the drop readback did not complete in these rows: zero drops would be unproven"}
    drop_inc, _ = _counter_increments(trace, "overflow_drop_events")
    drop_rows = sorted(i for i, v in drop_inc.items() if i in selected and v > 0)
    records, splats = sums["sampled_overlap_records_total"], sums["sampled_overlap_splats_total"]
    rec: Dict[str, object] = {
        "status": OK,
        "rows": len(rows),
        "rows_with_signal": with_signal,
        "readback_samples": sums["drop_signal_samples"],
        "drop_events": sums["overflow_drop_events"],
        "binning_drop_events": sums["binning_drop_events"],
        "binning_tile_range_drop_events": sums["binning_tile_range_drop_events"],
        "binning_budget_drop_events": sums["binning_budget_drop_events"],
        "raster_tile_cap_drop_events": sums["raster_tile_cap_drop_events"],
        "sampled_dropped_records": sums["sampled_dropped_records_total"],
        "sampled_overlap_records": records,
        "sampled_splats": splats,
        "records_per_splat": metric(records / splats) if splats > 0
        else missing("no sampled frame carried a binning splat count"),
        "sampled_dropped_fraction": metric(sums["sampled_dropped_records_total"] / records) if records > 0
        else missing("no sampled frame carried overlap records"),
        "rows_with_drop": len(drop_rows),
        "first_drop_row": drop_rows[0] if drop_rows else None,
    }
    if drop_rows:
        r = trace.rows[drop_rows[0]]
        rec["first_drop"] = {k: trace.get(r, k) for k in ("pass_index", "segment", "view", "state", "s_m")}
    return rec


#: The exported spelling of a value that was not measured (the renderer's drop-frame export and
#: probe_world.gd use the same string). Never replaced by 0.
NOT_MEASURED = "not_measured"

#: #1457: the two parts of a pass's checkpoint drop window, by the probe's row state. Together
#: they are exactly ``overlap_drops_checkpoints`` (every settle and hold row of the pass).
CHECKPOINT_DROP_WINDOWS = (
    ("settle_window", "overlap_drops_checkpoints_settle_window", STATE_SETTLING),
    ("settled_pose", "overlap_drops_checkpoints_settled_pose", STATE_HOLD),
)
CHECKPOINT_DROP_SPLIT_SCOPE = (
    "settle_window = rows while the probe waits for a checkpoint to settle (the frames after a stop or a "
    "teleport); settled_pose = the hold rows after the settle. Both are dropped records (missing splats) and "
    "both stay failures; the split only says where they were observed. A drop is credited to the row whose "
    "stats saw the readback, a frame or more after the frame that dropped, so a drop in the last frames of "
    "one window can be credited to the next.")
_DROP_WINDOW_KEYS = ("rows", "readback_samples", "drop_events", "binning_tile_range_drop_events",
                     "binning_budget_drop_events", "raster_tile_cap_drop_events", "sampled_dropped_records")


def checkpoint_drop_windows(trace: Trace, pass_index: int) -> Dict[str, object]:
    """#1457: one pass's checkpoint drops, split into the settle window and the settled pose."""
    return {key: overlap_drops(trace, rows_where(trace, phase=0, pass_index=pass_index, state=state))
            for _label, key, state in CHECKPOINT_DROP_WINDOWS}


def checkpoint_drop_split(passes: object) -> Dict[str, object]:
    """#1457 (report-only): the checkpoint drops of each pass, settle window beside settled pose.

    Reads the two windows `checkpoint_drop_windows` put into a pass record. A window that is not OK
    (no readback completed in its rows, an older summary without the split) is ``not_measured``
    with its reason, never zero drops. Nothing here changes a verdict: the comparator and G10
    keep failing on drops in either window.
    """
    out = []
    for record in passes if isinstance(passes, list) else []:
        if not isinstance(record, dict):
            continue
        entry: Dict[str, object] = {"pass_index": record.get("pass_index")}
        for label, key, _state in CHECKPOINT_DROP_WINDOWS:
            window = record.get(key)
            if isinstance(window, dict) and window.get("status") == OK:
                entry[label] = {"status": OK, **{name: window.get(name) for name in _DROP_WINDOW_KEYS}}
            else:
                reason = (window.get("reason", "window not OK") if isinstance(window, dict)
                          else "the pass summary carries no split checkpoint window")
                entry[label] = {"status": NOT_MEASURED, "reason": reason}
        out.append(entry)
    if not out:
        return missing("no pass summary to split")
    value = {"scope": CHECKPOINT_DROP_SPLIT_SCOPE, "passes": out}
    return {"status": OK, "value": value, **value}


_DROP_FRAME_INT_KEYS = ("frame_serial", "dropped_records", "raster_cap_dropped_records", "raster_truncated_tiles",
                        "overlap_records_attempted", "raster_tile_cap")
_DROP_FRAME_OPTIONAL_KEYS = ("max_tile_records", "max_tile_x", "max_tile_y", "overlap_allocation_records")
DROP_FRAME_SCOPE = (
    "sampled frames whose own counters show lost overlap records, from the renderer's always-on drop readback "
    "(no extra readback). max_tile_records is the most records any tile reserved in that frame, max_tile its "
    "tile, overlap_allocation_records the record capacity of that frame, overlap_records_attempted the demand. "
    "A drop event whose dropping frame was not the sampled one has no entry; observed_* is the probe row that "
    "saw the sample, a frame or more after the frame itself.")


def _count(value: object) -> Optional[int]:
    return value if type(value) is int and value >= 0 else None


def drop_frame_summary(source: object) -> Dict[str, object]:
    """#1457/#1461: the sampled frames that lost overlap records (probe_world.gd ``overlap_drop_frames``).

    - MISSING when the probe result has no such block or the renderer did not publish the counter
      (an older binary): there is no number, and none is invented.
    - INVALID when the block contradicts itself (events that do not add up to the counter, a frame
      without its required counters): its numbers are not reported.
    - OK otherwise, also with zero drop frames. Per-frame values the renderer exported as
      ``not_measured`` stay ``not_measured``; maxima are taken over measured values only and are
      MISSING when there is none.
    """
    if not isinstance(source, dict):
        return {"status": MISSING, "reason": "the probe result has no overlap_drop_frames block (older probe)"}
    count, events, unrecorded = _count(source.get("count")), source.get("events"), _count(source.get("events_not_recorded"))
    if count is None:
        return {"status": MISSING, "reason": "the renderer did not publish drop_frame_samples (older binary)"}
    if not isinstance(events, list) or unrecorded is None or len(events) + unrecorded != count:
        return {"status": INVALID, "reason": "drop-frame events and events_not_recorded do not add up to the counter"}
    frames: List[Dict[str, object]] = []
    for index, event in enumerate(events):
        frame = event.get("frame") if isinstance(event, dict) else None
        if not isinstance(frame, dict) or any(_count(frame.get(key)) is None for key in _DROP_FRAME_INT_KEYS):
            return {"status": INVALID, "reason": f"drop-frame event {index} lacks its frame counters"}
        if frame["dropped_records"] <= 0 or frame["raster_cap_dropped_records"] > frame["dropped_records"]:
            return {"status": INVALID, "reason": f"drop-frame event {index} has inconsistent lost-record counters"}
        entry: Dict[str, object] = {key: frame[key] for key in _DROP_FRAME_INT_KEYS}
        for key in _DROP_FRAME_OPTIONAL_KEYS:
            value = _count(frame.get(key))
            entry[key] = value if value is not None else NOT_MEASURED
        entry["binning_dropped_records"] = frame["dropped_records"] - frame["raster_cap_dropped_records"]
        entry["max_tile_records_over_cap"] = (
            entry["max_tile_records"] - frame["raster_tile_cap"]
            if entry["max_tile_records"] != NOT_MEASURED and frame["raster_tile_cap"] > 0 else NOT_MEASURED)
        entry["overlap_records_over_allocation"] = (
            frame["overlap_records_attempted"] - entry["overlap_allocation_records"]
            if entry["overlap_allocation_records"] != NOT_MEASURED else NOT_MEASURED)
        for key in ("observed_row", "observed_phase", "observed_pass_index", "observed_segment", "observed_state"):
            value = event.get(key)
            entry[key] = value if type(value) is int else NOT_MEASURED
        frames.append(entry)

    def peak(selected: List[Dict[str, object]], key: str, reason: str) -> Dict[str, object]:
        values = [f[key] for f in selected if f[key] != NOT_MEASURED]
        return metric(max(values), unit="records") if values else missing(reason, "records")

    cap_frames = [f for f in frames if f["raster_cap_dropped_records"] > 0]
    binning_frames = [f for f in frames if f["binning_dropped_records"] > 0]
    value = {
        "scope": DROP_FRAME_SCOPE,
        "drop_frames": count,
        "drop_frames_described": len(frames),
        "drop_frames_not_described": unrecorded,
        "max_tile_records_peak": peak(frames, "max_tile_records", "no sampled dropping frame carried a per-tile record maximum"),
        "raster_cap_frames": len(cap_frames),
        "raster_cap_max_tile_records_peak": peak(
            cap_frames, "max_tile_records", "no sampled frame lost records at the per-tile raster cap"),
        "raster_cap_dropped_records": sum(f["raster_cap_dropped_records"] for f in frames),
        "binning_frames": len(binning_frames),
        "binning_dropped_records": sum(f["binning_dropped_records"] for f in frames),
        "binning_overlap_records_over_allocation_peak": peak(
            binning_frames, "overlap_records_over_allocation",
            "no sampled frame with a binning drop carried its record allocation"),
        "frames": frames,
    }
    return {"status": OK, "value": value, **value}


def forward_progress_summary(records: object, *, route: str, windows: dict) -> Dict[str, object]:
    """Recompute progress over existing probe settle windows, without a new limit.

    ``windows`` is the process-read-back settle configuration (run_probe.SETTLE:
    cold/stop/teleport deadlines). A shorter successful settle need not prove a
    full stall window. A missed window needs complete native samples before it
    can establish that demand persisted and no read/decode/upload progressed.
    """
    if not isinstance(records, list) or not records:
        return {**missing("no native settle-window progress observations"), "windows": []}

    def invalid(reason):
        return {"status": INVALID, "reason": reason, "windows": []}

    result = []
    counter_keys = {"read_completions", "pages_decoded", "upload_completions", "upload_bytes"}
    for record in records:
        if not isinstance(record, dict):
            return invalid("invalid forward-progress window")
        deadline = windows.get(record.get("window"))
        if (type(deadline) not in (int, float) or not math.isfinite(deadline) or deadline <= 0
                or record.get("deadline_s") != deadline):
            return invalid("forward-progress window differs from existing settle configuration")
        if record.get("measured") is not True:
            result.append({**record, "measured": False, "no_forward_progress": None,
                           "reason": record.get("reason", "native progress snapshots unavailable")})
            continue
        samples = record.get("samples")
        if not isinstance(samples, list) or len(samples) < 2:
            return invalid("forward-progress window lacks native endpoints")
        if (type(record.get("elapsed_usec")) not in (int, float) or not math.isfinite(record["elapsed_usec"])
                or record["elapsed_usec"] < 0 or type(record.get("deadline_missed")) is not bool):
            return invalid("forward-progress window lacks elapsed time or deadline outcome")
        previous = None
        for sample in samples:
            if (not isinstance(sample, dict) or sample.get("measured") is not True
                    or sample.get("route") != route or type(sample.get("row")) is not int
                    or type(sample.get("t_usec")) is not int or type(sample.get("demand_outstanding")) is not bool
                    or not isinstance(sample.get("progress_counts"), dict) or set(sample["progress_counts"]) != counter_keys
                    or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 or v != int(v)
                           for v in sample["progress_counts"].values())):
                return invalid("missing or invalid native progress sample")
            if previous is not None and (sample["row"] != previous["row"] + 1 or sample["t_usec"] <= previous["t_usec"]
                    or any(sample["progress_counts"][key] < previous["progress_counts"][key] for key in counter_keys)):
                return invalid("progress samples omit a row, regress a counter or cross an epoch")
            previous = sample
        elapsed = samples[-1]["t_usec"] - samples[0]["t_usec"]
        deltas = {key: samples[-1]["progress_counts"][key] - samples[0]["progress_counts"][key] for key in counter_keys}
        stalled = (record.get("deadline_missed") is True and record["elapsed_usec"] >= deadline * 1_000_000
                   and elapsed >= deadline * 1_000_000
                   and all(sample["demand_outstanding"] for sample in samples) and not any(deltas.values()))
        if record.get("no_forward_progress") is not stalled:
            return invalid("claimed forward-progress outcome differs from native samples")
        result.append({**record, "measured": True, "no_forward_progress": stalled,
                       "observed_usec": elapsed, "progress_deltas": deltas})
    return {"status": OK, "scope": "existing run_probe.SETTLE deadlines; native read/decode/upload progress", "windows": result}


def queue_observations_summary(block: object, *, route: str) -> Dict[str, object]:
    """Validate native bounded-owner observations without inventing limits.

    HLOD output shares byte admission; chunk IO has byte admission and chunk
    upload has job admission. An absent *independent* limit is explicitly null,
    with a reason. Occupancy, peaks and every limit that actually exists remain
    mandatory. This validates observations, not whether saturation is acceptable.
    """
    if not isinstance(block, dict) or not block:
        return missing("no native bounded payload queue observations")
    owners = {
        "hlod": {"retained": "hlod_shared_reservation_ledger", "ready": "hlod_shared_reservation_ledger"},
        "chunk": {"retained": "payload_read_service", "ready": "chunk_upload_window"},
    }
    if route not in owners:
        return {"status": INVALID, "reason": "unknown queue route"}
    result = {"status": OK}
    for name, owner in owners[route].items():
        queue = block.get(name)
        if not isinstance(queue, dict):
            return missing(f"missing {route} {name} queue")
        if queue.get("owner") != owner:
            return {"status": INVALID, "reason": f"wrong {route} {name} queue owner"}
        for key in ("bytes", "peak_bytes", "jobs", "peak_jobs", "byte_bound", "job_bound"):
            value = queue.get(key)
            no_independent_limit = (
                key == "job_bound" and (route == "hlod" or name == "retained")
                or key == "byte_bound" and route == "chunk" and name == "ready")
            if (no_independent_limit and key in queue and value is None
                    and isinstance(queue.get(f"{key}_reason"), str) and queue[f"{key}_reason"].strip()):
                continue
            if type(value) not in (int, float) or not math.isfinite(value):
                return missing(f"missing {route} {name} {key}")
            if value < 0 or value != int(value) or (key.endswith("bound") and value <= 0):
                return {"status": INVALID, "reason": f"invalid {route} {name} {key}"}
        for current, peak in (("bytes", "peak_bytes"), ("jobs", "peak_jobs")):
            if queue[current] > queue[peak]:
                return {"status": INVALID, "reason": f"{route} {name} {current} exceeds peak"}
        if route == "hlod":
            for key in ("peak_byte_bound", "bound_violations"):
                value = queue.get(key)
                if type(value) not in (int, float) or not math.isfinite(value):
                    return missing(f"missing {route} {name} {key}")
                if value < 0 or value != int(value) or (key == "peak_byte_bound" and value == 0):
                    return {"status": INVALID, "reason": f"invalid {route} {name} {key}"}
        result[name] = dict(queue)
    return result


def route_telemetry_summary(block: object, *, route: str, source_generation: int) -> Dict[str, object]:
    """Validate required producer evidence against an independently observed source.

    CPU stage sums are not wall latency or separately measured zstd time. This
    report does not change any flythrough verdict or release threshold.
    """
    if not isinstance(block, dict) or not block:
        return missing("no route-labelled payload telemetry")

    def invalid(reason):
        return {"status": INVALID, "reason": reason, "route": route}

    def numeric(value):
        return ((isinstance(value, int) and not isinstance(value, bool))
                or (isinstance(value, float) and math.isfinite(value)))

    if route not in ("hlod", "chunk"):
        return invalid("unknown required route")
    if "route" in block and block["route"] != route:
        return invalid("payload telemetry belongs to another route")
    if not numeric(source_generation) or source_generation == 0:
        return missing("live source generation is unavailable")
    for key in ("source_generation", "collection_epoch"):
        if key in block and numeric(block[key]) and block[key] != source_generation:
            return invalid(f"stale {key}: {block[key]} != {source_generation}")
    numbers = (
        "source_generation", "collection_epoch", "captures", "captures_failed",
        "captures_on_worker_threads", "captures_on_main_thread", "pages_decoded", "splats_decoded",
        "pages_decoded_on_worker_threads", "pages_decoded_on_main_thread", "page_failures",
        "stored_bytes_read", "read_requests", "read_failures", "read_latency_nsec", "read_queue_nsec",
        "verify_nsec", "decode_nsec", "decode_remainder_nsec", "pages_decoded_digest_checked",
        "pages_decoded_digest_skipped",
    )
    owners = ("packed", "ready", "mailbox", "partial_transfer", "active", "stored", "shadow")
    hlod_numbers = (
        "pack_calls", "pack_nsec", "queue_residence_nsec", "queue_residence_samples",
        "upload_bytes", "upload_bytes_this_frame", "upload_slices", "upload_completions", "upload_completion_nsec",
    ) + tuple(f"{owner}_bytes{suffix}" for owner in owners for suffix in ("", "_peak"))
    required_numbers = numbers + (hlod_numbers if route == "hlod" else ())
    absent = [key for key in required_numbers if not numeric(block.get(key))]
    for key in ("route", "last_failure_reason", "verify_scope", "stage_time_scope", "digest_status"):
        if not isinstance(block.get(key), str):
            absent.append(key)
    if "digest_nsec" not in block:
        absent.append("digest_nsec")
    if absent:
        return missing(f"required {route} telemetry absent: {absent}")
    # Missing work outranks consistency checks: zero worker captures never proves
    # that a producer's worker path ran, even if other counters claim work.
    for key in ("captures", "captures_on_worker_threads", "pages_decoded", "splats_decoded",
                "pages_decoded_on_worker_threads", "read_requests", "stored_bytes_read", "decode_nsec"):
        if block[key] == 0:
            return missing(f"no measured {key}")
    unsigned = set(required_numbers) - {"source_generation", "collection_epoch"}
    if any(block[key] < 0 for key in unsigned):
        return invalid("negative measurement")
    if any(block[key] != int(block[key]) for key in required_numbers):
        return invalid("non-integer counter or nanosecond duration")
    if any(block[key] != 0 for key in ("captures_failed", "page_failures", "read_failures")) or block["last_failure_reason"]:
        return invalid("producer reported a payload failure")
    if not block["verify_scope"] or block["stage_time_scope"] != "worker_cpu_sum":
        return invalid("unknown verification or CPU timing scope")
    for total, worker, main in (
        ("captures", "captures_on_worker_threads", "captures_on_main_thread"),
        ("pages_decoded", "pages_decoded_on_worker_threads", "pages_decoded_on_main_thread"),
    ):
        if block[total] != block[worker] + block[main]:
            return invalid(f"inconsistent {total} thread counts")
    checked, skipped = block["pages_decoded_digest_checked"], block["pages_decoded_digest_skipped"]
    # Historical checked/skipped captures predate this counter. Only those
    # statuses can interpret its absence as zero; unavailable work needs its
    # own explicit producer counter and cannot become a policy skip.
    unavailable = block.get("pages_decoded_digest_unavailable", 0)
    if (not numeric(unavailable) or isinstance(unavailable, bool)
            or unavailable < 0 or unavailable != int(unavailable)):
        return invalid("invalid pages_decoded_digest_unavailable")
    if checked + skipped + unavailable != block["pages_decoded"]:
        return invalid("digest counts do not cover decoded pages")
    digest_status, digest_nsec = block["digest_status"], block["digest_nsec"]
    if digest_status == "skipped":
        if checked != 0 or skipped <= 0 or unavailable != 0 or digest_nsec is not None:
            return invalid("skipped digest must have skipped pages and no measured duration")
        measured_digest = 0
    elif digest_status == "not_available":
        if checked != 0 or skipped != 0 or unavailable <= 0 or digest_nsec is not None:
            return invalid("unavailable digest must have unavailable pages and no measured duration")
        measured_digest = 0
    elif digest_status in ("checked", "mixed"):
        kinds = sum(value > 0 for value in (checked, skipped, unavailable))
        if ((digest_status == "checked" and (checked <= 0 or kinds != 1))
                or (digest_status == "mixed" and kinds < 2)
                or (checked > 0 and (not numeric(digest_nsec) or digest_nsec <= 0))
                or (checked == 0 and digest_nsec is not None)):
            return invalid("checked digest lacks performed work")
        measured_digest = digest_nsec if checked > 0 else 0
    else:
        return invalid("unknown decoded-digest status")
    if block["decode_nsec"] != block["verify_nsec"] + block["decode_remainder_nsec"] + measured_digest:
        return invalid("decode parts do not sum to the measured span")
    if route == "hlod":
        for key in ("pack_calls", "queue_residence_samples", "upload_bytes", "upload_slices", "upload_completions"):
            if block[key] == 0:
                return missing(f"no measured {key}")
        for owner in owners:
            if block[f"{owner}_bytes"] > block[f"{owner}_bytes_peak"]:
                return invalid(f"{owner} bytes exceed their high-water mark")
    if block.get("gpu_memory_usage_mb") is not None:
        if block.get("gpu_memory_usage_mb") == 0 or block.get("gpu_memory_status") != "measured":
            return invalid("unmeasured VRAM cannot be reported as zero")
    if "queue_observations" in block:
        queues = queue_observations_summary(block["queue_observations"], route=route)
        if queues["status"] != OK:
            return queues
        counters = ("stale_generation_publications", "digest_verification_failures", "reads_refused_busy", "admission_waits")
        if route == "hlod":
            counters += ("upload_hold_frames",)
        for key in counters:
            if not numeric(block.get(key)) or isinstance(block.get(key), bool):
                return missing(f"required {route} queue/publication counter absent: {key}")
            if block[key] < 0 or block[key] != int(block[key]):
                return invalid(f"invalid {key}")
        if any(block[key] != 0 for key in ("stale_generation_publications", "digest_verification_failures")):
            return invalid("producer reported a publication or verification failure")
    return {**block, "status": OK, "digest": {"status": digest_status, "nsec": digest_nsec}}


def gsq_decode_split(stats: object) -> Dict[str, object]:
    """BV-K9b: the GSQ decode span (the pipeline's "decode" stage) split into its parts, as
    per-page means in ms and shares of the span: the stored-page verify (header + stored
    SHA-256), decode (the remainder: zstd frame decompression, LUTs, dequantisation and the
    page-table checks) and the decoded digest (SHA-256 over the decoded splats). zstd is not
    timed apart: that would change gsq_codec.*, a world-cache generator source.
    Counters are whole-run totals of the world's source."""
    if not isinstance(stats, dict):
        return missing("no GSQ decode counters (not a paged-GSQ world, or an older probe)")
    keys = ("pages_decoded", "splats_decoded", "decode_nsec", "verify_nsec", "digest_nsec")
    vals = {k: stats.get(k) for k in keys}
    # New route telemetry represents skipped work as null. Keep the historical
    # K9 arithmetic/outputs while retaining the explicit skip label below.
    if vals["digest_nsec"] is None and (stats.get("digest_status") in ("skipped", "not_available")
            or (stats.get("digest_status") == "mixed" and stats.get("pages_decoded_digest_checked") == 0)):
        vals["digest_nsec"] = 0
    absent = [k for k, v in vals.items() if not isinstance(v, (int, float)) or isinstance(v, bool)]
    if absent:
        return missing(f"decode counters absent: {absent}")
    pages, splats, total = int(vals["pages_decoded"]), int(vals["splats_decoded"]), int(vals["decode_nsec"])
    verify, digest = int(vals["verify_nsec"]), int(vals["digest_nsec"])
    if pages <= 0 or total <= 0:
        return missing("no page was decoded")
    decode = total - verify - digest
    if verify < 0 or digest < 0 or decode < 0:
        return {"status": INVALID,
                "reason": f"parts exceed the span: verify {verify} + digest {digest} > decode {total} ns"}
    parts = {"verify": verify, "decode": decode, "digest": digest}
    out: Dict[str, object] = {
        "status": OK,
        "pages": pages,
        "splats": splats,
        "ms_per_page": {k: v / pages / 1e6 for k, v in parts.items()},
        "share": {k: v / total for k, v in parts.items()},
        "decode_span_ms_per_page": total / pages / 1e6,
        "splats_per_s_per_worker": splats / (total / 1e9),
    }
    out["dominant"] = max(parts, key=lambda k: parts[k])
    # BV-K9c: zero is the historical arithmetic sum for skipped work. D3-5
    # publishes null for its duration and an explicit label, kept here too.
    verified = stats.get("decoded_digest_verified")
    checked = stats.get("pages_decoded_digest_checked")
    out["decoded_digest_verified"] = verified if isinstance(verified, bool) else None
    out["pages_decoded_digest_checked"] = (
        int(checked) if isinstance(checked, (int, float)) and not isinstance(checked, bool) else None)
    for key in ("pages_decoded_digest_skipped", "pages_decoded_digest_unavailable"):
        count = stats.get(key)
        out[key] = int(count) if isinstance(count, (int, float)) and not isinstance(count, bool) else None
    out["digest_status"] = stats.get("digest_status", "skipped" if verified is False else None)
    return out


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


#: The load interval: the first pass's cold settle at the path's first hold (S0), during
#: which the world loads and the effective settings arrive (M1: cap 256 -> 480 there).
LOAD_PASS_INDEX = 0
LOAD_SEGMENT = 0


def measured_rows(trace: Trace, pass_index: int) -> List[int]:
    """#1383 item 2: one pass's measured rows for V7, i.e. its moving rows plus every
    checkpoint settle and hold, without the load interval (pass 0's S0 settle)."""
    seg, state = trace.index["segment"], trace.index["state"]
    return [
        i for i in rows_where(trace, phase=0, pass_index=pass_index)
        if not (pass_index == LOAD_PASS_INDEX and trace.rows[i][seg] == LOAD_SEGMENT
                and trace.rows[i][state] == STATE_SETTLING)
    ]


# --------------------------------------------------------------------------
# Frame-time breakdown (#1445)
# --------------------------------------------------------------------------

#: The four parts of the wall-clock interval that ENDS at a row, in the order they happen
#: (probe_world.gd BREAKDOWN_FIELDS). They add up to ``t_usec[i] - t_usec[i - 1]`` exactly.
BREAKDOWN_PARTS = (
    "frame_harness_telemetry_usec", "frame_harness_capture_usec", "frame_harness_script_usec",
    "frame_engine_usec",
)
BREAKDOWN_COLUMNS = BREAKDOWN_PARTS + ("frame_captures", "render_cpu_ms", "renderer_frames_total")
BREAKDOWN_SCOPE = (
    "per row: the wall-clock interval since the previous row = the driver's telemetry (stats query "
    "and trace row) + its viewport captures (readback, PNG encode, write) + its remaining script "
    "work + the engine frame. game_frame is the engine frame alone: everything between the driver "
    "handing control back and the next frame_post_draw (scene step, render CPU, submit, present), "
    "which is what a game without the probe pays per frame. wall_frame is the unchanged "
    "moving_frame_interval quantity. renderer_cpu is the renderer's own frame_time_ms, counted once "
    "per finished renderer frame; engine_other is the engine frame outside it. GPU time runs in "
    "parallel with the CPU parts and is reported beside them, never added to them."
)
#: Slow game frames listed per block, longest first (the counts cover all of them).
HITCH_LIST_LIMIT = 20
#: Cumulative counters whose per-row advance says what a slow frame did (absent columns are left out).
HITCH_DELTA_COLUMNS = (
    "upload_bytes_total", "upload_chunks_total", "read_bytes_total", "hlod_stream_uploaded_bytes_total",
)
HITCH_VALUE_COLUMNS = (
    "loaded_chunks", "needed_not_resident_chunks", "needed_upload_pending_chunks", "upload_stall_wall_ms",
    "chunks_evicted_this_frame", "hlod_stream_loading_nodes", "hlod_stream_desired_nodes",
    "hlod_stream_uploaded_bytes_this_frame", "drawn_splats",
)


def frame_time_distribution(values_ms: Sequence[float]) -> Dict[str, object]:
    """Frame-time percentiles two ways: time-weighted (ADR 8.3, the share of wall time spent in
    frames at least this long) and per frame (the share of frames)."""
    if not values_ms:
        return {"frames": 0, "mean": missing("no frame interval", "ms")}
    out: Dict[str, object] = {"frames": len(values_ms), "mean": metric(mean(values_ms), unit="ms")}
    for q in (50, 95, 99):
        out[f"p{q}_time_weighted"] = metric(time_weighted_percentile(values_ms, q / 100.0), unit="ms")
    for q in (50, 95, 99):
        out[f"p{q}_frames"] = metric(nearest_rank(values_ms, q / 100.0), unit="ms")
    out["max"] = metric(max(values_ms), unit="ms")
    out["frames_over_50ms"] = sum(1 for v in values_ms if v > SLOW_FRAME_MS)
    out["frames_over_250ms"] = sum(1 for v in values_ms if v > HITCH_FRAME_MS)
    return out


def frame_breakdown(trace: Trace, rows: Sequence[int]) -> Dict[str, object]:
    """#1445: split the selected rows' frame intervals into the engine's frame and the probe's work.

    Statuses (absence of a signal is never a pass, and never a zero):

    - MISSING when the trace has no breakdown columns (a driver older than #1445), no selected row
      has an interval, or any selected row carries the driver's -1 (it could not split that
      interval). No number is reported then; ``moving_frame_interval`` is unaffected.
    - INVALID when a row's parts do not add up to its wall-clock interval, or a part is negative.
    - OK otherwise. The renderer's CPU share additionally needs ``frame_time_ms`` and
      ``total_frames_rendered`` on every row; without them that share and ``engine_other`` are
      MISSING while the engine/harness split stays OK.
    """
    absent = [n for n in BREAKDOWN_COLUMNS if n not in trace.index]
    if absent:
        return {"status": MISSING,
                "reason": f"the trace has no frame breakdown columns {absent} (driver older than #1445)"}
    sel = [i for i in rows if i > 0]
    if not sel:
        return {"status": MISSING, "reason": "no frame interval in the selected rows"}
    t = trace.col("t_usec")
    parts = {name: trace.col(name) for name in BREAKDOWN_PARTS}
    unsplit = [i for i in sel if any(parts[name][i] < 0 for name in BREAKDOWN_PARTS)]
    if unsplit:
        negative = [i for i in unsplit if any(parts[name][i] < -1 for name in BREAKDOWN_PARTS)]
        if negative:
            return {"status": INVALID, "reason": f"negative frame part at row {negative[0]}"}
        return {"status": MISSING, "reason": f"{len(unsplit)} of {len(sel)} selected frames carry no "
                f"breakdown (first at row {unsplit[0]}): the driver could not split them"}
    for i in sel:
        if sum(parts[name][i] for name in BREAKDOWN_PARTS) != t[i] - t[i - 1]:
            return {"status": INVALID, "reason": f"the frame parts of row {i} add up to "
                    f"{sum(parts[name][i] for name in BREAKDOWN_PARTS)} usec, its interval is {t[i] - t[i - 1]} usec"}
    telemetry, capture, script, engine = (parts[name] for name in BREAKDOWN_PARTS)
    wall_ms = [(t[i] - t[i - 1]) / 1000.0 for i in sel]
    game_ms = [engine[i] / 1000.0 for i in sel]
    harness_ms = [(telemetry[i] + capture[i] + script[i]) / 1000.0 for i in sel]
    totals = {
        "wall": sum(wall_ms), "game_frame": sum(game_ms), "harness": sum(harness_ms),
        "telemetry": sum(telemetry[i] for i in sel) / 1000.0, "capture": sum(capture[i] for i in sel) / 1000.0,
        "script": sum(script[i] for i in sel) / 1000.0,
    }
    wall_total = totals["wall"]

    def share(value_ms: Optional[float], reason: str = "") -> Dict[str, object]:
        if value_ms is None:
            return missing(reason, "%")
        if wall_total <= 0.0:
            return missing("no wall time in the selected rows", "%")
        return metric(100.0 * value_ms / wall_total, unit="%")

    # The renderer's own CPU time. A row whose renderer frame counter did not advance repeats the
    # previous frame's number (a held sample): counted once, with the frame that produced it.
    cpu, frames_total = trace.col("render_cpu_ms"), trace.col("renderer_frames_total")
    no_cpu = [i for i in sel if cpu[i] < 0.0 or frames_total[i] < 0 or frames_total[i - 1] < 0]
    renderer: Dict[str, object] = {}
    if no_cpu:
        reason = (f"{len(no_cpu)} of {len(sel)} selected rows (first {no_cpu[0]}) lack the renderer's "
                  "frame_time_ms / total_frames_rendered, on this row or the one before")
        renderer["cpu_ms"] = missing(reason, "ms")
        cpu_share = engine_other_share = missing(reason, "%")
        fresh_cpu: Dict[int, float] = {}
    else:
        fresh_cpu = {i: float(cpu[i]) for i in sel if frames_total[i] > frames_total[i - 1]}
        values = list(fresh_cpu.values())
        renderer["cpu_ms"] = ({"mean": metric(mean(values), unit="ms"),
                               "p50_frames": metric(nearest_rank(values, 0.5), unit="ms"),
                               "p95_frames": metric(nearest_rank(values, 0.95), unit="ms"),
                               "max": metric(max(values), unit="ms"), "samples": len(values)}
                              if values else missing("the renderer finished no frame in the selected rows", "ms"))
        renderer["cpu_fresh_frames"] = len(values)
        totals["renderer_cpu"] = sum(values)
        totals["engine_other"] = totals["game_frame"] - totals["renderer_cpu"]
        cpu_share = share(totals["renderer_cpu"])
        engine_other_share = (share(totals["engine_other"]) if totals["engine_other"] >= 0.0 else metric(
            None, INVALID, "the renderer's CPU time exceeds the engine frame time of these rows", "%"))
    if "gpu_frame_ms" in trace.index and "gpu_frame_valid" in trace.index:
        gpu, gpu_valid = trace.col("gpu_frame_ms"), trace.col("gpu_frame_valid")
        values = [float(gpu[i]) for i in sel if gpu_valid[i] == 1]
        renderer["gpu_frame_ms"] = ({
            "mean": metric(mean(values), unit="ms"), "p50_frames": metric(nearest_rank(values, 0.5), unit="ms"),
            "p95_frames": metric(nearest_rank(values, 0.95), unit="ms"), "max": metric(max(values), unit="ms"),
            "samples": len(values),
            "scope": "the renderer's last valid GPU frame time as each row saw it (a held value repeats); "
                     "parallel to the CPU parts, not a share of them",
        } if values else missing("no selected row carries a valid GPU frame time", "ms"))
    else:
        renderer["gpu_frame_ms"] = missing("the trace has no gpu_frame_ms column", "ms")

    captures = trace.col("frame_captures")
    slow_game = [i for i in sel if engine[i] / 1000.0 > SLOW_FRAME_MS]
    slow_wall = [i for i in sel if (t[i] - t[i - 1]) / 1000.0 > SLOW_FRAME_MS]
    listed = []
    for i in sorted(slow_game, key=lambda i: (-engine[i], i))[:HITCH_LIST_LIMIT]:
        row = trace.rows[i]
        entry: Dict[str, object] = {
            "row": i, "game_frame_ms": engine[i] / 1000.0, "wall_frame_ms": (t[i] - t[i - 1]) / 1000.0,
            "harness_telemetry_ms": telemetry[i] / 1000.0, "harness_capture_ms": capture[i] / 1000.0,
            "harness_script_ms": script[i] / 1000.0, "captures_in_interval": captures[i],
            "render_cpu_ms": fresh_cpu.get(i),
        }
        for name in ("pass_index", "segment", "view", "state", "s_m", "engine_frame"):
            if name in trace.index:
                entry[name] = trace.get(row, name)
        if "gpu_frame_ms" in trace.index and "gpu_frame_valid" in trace.index:
            entry["gpu_frame_ms"] = trace.get(row, "gpu_frame_ms") if trace.get(row, "gpu_frame_valid") == 1 else None
        for name in HITCH_DELTA_COLUMNS:
            if name in trace.index:
                now, before = trace.get(row, name), trace.get(trace.rows[i - 1], name)
                entry[name.replace("_total", "") + "_delta"] = now - before if now >= 0 and before >= 0 else None
        for name in HITCH_VALUE_COLUMNS:
            if name in trace.index:
                value = trace.get(row, name)
                entry[name] = value if value >= 0 else None
        listed.append(entry)
    return {
        "status": OK,
        "scope": BREAKDOWN_SCOPE,
        "frames": len(sel),
        "wall_frame": frame_time_distribution(wall_ms),
        "game_frame": frame_time_distribution(game_ms),
        "harness_frame": frame_time_distribution(harness_ms),
        "totals_ms": totals,
        "wall_share_pct": {
            "game_frame": share(totals["game_frame"]), "harness": share(totals["harness"]),
            "renderer_cpu": cpu_share, "engine_other": engine_other_share,
            "script": share(totals["script"]), "capture": share(totals["capture"]),
            "telemetry": share(totals["telemetry"]),
        },
        "captures": sum(captures[i] for i in sel),
        "frames_with_capture": sum(1 for i in sel if captures[i] > 0),
        "renderer": renderer,
        "slow_frames": {
            "threshold_ms": SLOW_FRAME_MS, "hitch_threshold_ms": HITCH_FRAME_MS,
            "wall_over_50ms": len(slow_wall),
            # Slow by the wall clock although the engine frame itself was not: the probe's own work.
            "wall_over_50ms_harness_only": sum(1 for i in slow_wall if engine[i] / 1000.0 <= SLOW_FRAME_MS),
            "game_over_50ms": len(slow_game),
            "game_over_250ms": sum(1 for i in slow_game if engine[i] / 1000.0 > HITCH_FRAME_MS),
            # The driver read the viewport back just before this engine frame: the stall may be its echo.
            "game_over_50ms_with_capture_in_interval": sum(1 for i in slow_game if captures[i] > 0),
            "listed_limit": HITCH_LIST_LIMIT,
            "listed": listed,
        },
    }


FRAME_TIME_PASS_KEYS = ("moving_frame_interval", "moving_frame_breakdown", "measured_frame_breakdown")


def frame_time_passes(probe: object) -> List[Dict[str, object]]:
    """The frame-time blocks of a path summary's passes, for reports that carry nothing else of it."""
    passes = probe.get("passes") if isinstance(probe, dict) else None
    out = []
    for record in passes if isinstance(passes, list) else []:
        if isinstance(record, dict):
            out.append({"pass_index": record.get("pass_index"), "label": record.get("label"),
                        **{key: record.get(key) for key in FRAME_TIME_PASS_KEYS}})
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
