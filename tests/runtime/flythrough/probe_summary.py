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
    if checked + skipped != block["pages_decoded"]:
        return invalid("digest counts do not cover decoded pages")
    digest_status, digest_nsec = block["digest_status"], block["digest_nsec"]
    if digest_status == "skipped":
        if checked != 0 or skipped <= 0 or digest_nsec is not None:
            return invalid("skipped digest must have skipped pages and no measured duration")
        measured_digest = 0
    elif digest_status in ("checked", "mixed"):
        if (checked <= 0 or (digest_status == "checked" and skipped != 0)
                or (digest_status == "mixed" and skipped <= 0) or not numeric(digest_nsec) or digest_nsec <= 0):
            return invalid("checked digest lacks performed work")
        measured_digest = digest_nsec
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
    if vals["digest_nsec"] is None and stats.get("digest_status") == "skipped":
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
