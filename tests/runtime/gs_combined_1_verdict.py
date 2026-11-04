#!/usr/bin/env python3
"""GS-COMBINED-1 verdict: pure, fail-closed evaluation of a captured run record.

Spec: `docs/architecture/adr-one-route-many-producers.md` section 9 (#1306). The
GPU harness (`tests/runtime/gs_combined_1_harness.gd`) renders the combined
game scene -- a streamed world, 32 movable splat objects, a pick target, an
animated skinned mesh character, one directional and four omni lights with
shadows -- for 600 fixed-timestep frames and writes one JSON *record* per
variant. This module turns a record into a verdict. It never touches a GPU,
so every rule here is unit-tested (`tests/runtime/test_gs_combined_1_verdict.py`).

Fail-closed rule (ADR 9.3). A missing key, a stale frame serial, a NaN, an
empty frame list or a frame count other than 600 is a failure, never a skip.
Each assertion must have *evaluated* on its coverage minimum (590 frames for
the every-frame families), so a run that executes nothing cannot pass.

Per-assertion states:

``live-pass``
    Not listed as blocked; evaluated with full coverage; no failing frame.
``live-fail``
    A failure the expectations file does not explain. That is a regression (or
    a new defect) and is reported as such.
``blocked``
    Listed in the expectations file with an issue, and every failure code seen
    is one the entry names. The assertion is still RED: blocked is never green.
``stale-block``
    Listed as blocked, but it passed. The gate is not met until a maintainer
    removes the entry, so an expectations file cannot keep a live assertion
    out of view.

The gate is **met** only when every assertion of the spec is ``live-pass`` and
the expectations file blocks nothing. Until then it reports exactly which
assertions are blocked and why: that is the honest status of decision 1.

Forcing controls (ADR 9.4). Controls 4, 5 and 6 mutate the *captured* record
(delete a stats key, freeze one timestamp, make pick return no hit) and must
turn their target assertion from passing to failing on the same data. Controls
1, 2 and 3 alter the engine (exclusivity, sort input, caster pass); their
record-level equivalents are applied the same way, and the runtime switches
they need are reported as not available. A control whose target assertion is
already failing proves nothing and is reported ``unproven``, never ``proven``.
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import sys
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple

SCHEMA = "gs-combined-1/record/v1"
VERDICT_SCHEMA = "gs-combined-1/verdict/v1"
EXPECTATIONS_SCHEMA = "gs-combined-1/expectations/v1"
VARIANTS = ("A", "B")
FRAMES_REQUIRED = 600
FAMILY_MIN_FRAMES = 590
FROZEN_SEGMENT_FRAMES = 120
CHARACTER_POSES = ("front", "inside", "behind")
CHARACTER_POSE_MIN_FRAMES = 190

# ADR 9.3 thresholds. They are the spec; tests pin them so a change is a diff.
A4_TOLERANCE = 3.0 / 255.0
A5_TOLERANCE_PX = 0.5
A4_PROBES = 8
# A probe whose CPU reference is undefined (coplanar layers, a third layer on the ray)
# is excluded; a frame counts towards A4 coverage only with this many defined probes.
A4_MIN_INCLUDED = 6
L_PEARSON_MIN = 0.95
S_RATIO_MAX = 0.6
B1_LEDGER_TOLERANCE = 0.02
P_TIMING_COMPLETE_MIN = 0.99
P_MAX_FRAMES_BEHIND = 3
# Not fixed by the ADR (it names the reference, not a tolerance): the share of
# reference-mask pixels whose rendered visibility must agree. Pre-registered by the
# maintainer on 2026-11-05 ("ADR §9 left it unspecified") and recorded in the
# expectations file; load_expectations() refuses a file that disagrees.
C_AGREEMENT_MIN = 0.95
PREREGISTERED_THRESHOLDS = {"C_AGREEMENT_MIN": (C_AGREEMENT_MIN, ("C1", "C2", "C3"))}
# GR-15 frame targets (ADR section 8). Report-only until the BV calibration.
GR15_TARGETS_MS = {
    "rtx3080": {"p95": 25.0, "p99": 33.3},
    "rtx3090": {"p95": 16.7, "p99": 20.0},
}

LOG_FORBIDDEN = ("frame skipped", "not_exclusive", "identity_shim")
OMNI_LIGHTS = ("omni_0", "omni_1", "omni_2", "omni_3")
SHADOWED_LIGHTS = ("directional",) + OMNI_LIGHTS
SHADOW_SETUPS = {"S2": "mesh_floor_under_splat_object", "S3": "splat_ground_under_mesh",
                 "S4": "splat_object_over_splat_world"}

# (id, family, coverage minimum, one-line statement). Order is the report order.
ASSERTIONS: Tuple[Tuple[str, str, int, str], ...] = (
    ("A1", "sources", FAMILY_MIN_FRAMES, "world_drawn_splats > 0 and object_drawn_splats[i] > 0 for every object in the CPU frustum mask"),
    ("A2", "one_sort", FAMILY_MIN_FRAMES, "one sort dispatch per view, sort_input_sources >= {world, objects}, no second splat composite pass"),
    ("A3", "route", FAMILY_MIN_FRAMES, "route_skip_reason == none, hlod_reason == active on B, dropped_sources == 0, no forbidden log line"),
    ("A4", "interpenetration", FAMILY_MIN_FRAMES, "8 probes where world and object splats alternate in depth match the CPU-sorted reference within 3/255"),
    ("A5", "transforms", FAMILY_MIN_FRAMES, "tracer centres within 0.5 px of the CPU projection"),
    ("A6", "picking", FAMILY_MIN_FRAMES, "pick returns the target whenever it is under the cursor; partial only for non-resident regions"),
    ("C1", "compositing", CHARACTER_POSE_MIN_FRAMES, "character in front of foliage matches the depth reference"),
    ("C2", "compositing", CHARACTER_POSE_MIN_FRAMES, "character inside foliage matches the depth reference"),
    ("C3", "compositing", CHARACTER_POSE_MIN_FRAMES, "character behind foliage matches the depth reference"),
    ("L1", "lights", FAMILY_MIN_FRAMES, "directional light ROI luminance follows its intensity ramp (Pearson r >= 0.95)"),
    ("L2", "lights", FAMILY_MIN_FRAMES, "each omni light ROI luminance follows its intensity ramp (Pearson r >= 0.95)"),
    ("L3", "lights", FAMILY_MIN_FRAMES, "lights_dropped == 0"),
    ("S1", "shadows", FAMILY_MIN_FRAMES, "shadow_casters_rastered[light] > 0 for every shadowed light, NO_CASTER_RASTERED == 0"),
    ("S2", "shadows", FAMILY_MIN_FRAMES, "mesh floor under a splat object: receiver ROI luminance ratio <= 0.6"),
    ("S3", "shadows", FAMILY_MIN_FRAMES, "splat ground under a mesh: receiver ROI luminance ratio <= 0.6"),
    ("S4", "shadows", FAMILY_MIN_FRAMES, "splat object over the splat world: receiver ROI luminance ratio <= 0.6"),
    ("B1", "budget", FAMILY_MIN_FRAMES, "ledger sum equals device allocation within 2 %, within budget, silent_truncation_frames == 0"),
    ("T1", "stability", FROZEN_SEGMENT_FRAMES, "120-frame frozen segment, TAA off: 0 frames differ from the first; input digests equal"),
    ("P1", "performance", FAMILY_MIN_FRAMES, "frame-coherent p95 against GR-15 (report-only)"),
    ("P2", "performance", FAMILY_MIN_FRAMES, "frame-coherent p99 against GR-15 (report-only)"),
    ("P3", "performance", FAMILY_MIN_FRAMES, "HARD: timing complete for >= 99 % of frames, no stale timestamp reported valid"),
    ("V1", "multi_view", 1, "split screen: A1-A4 per half (variant job)"),
)
ASSERTION_IDS = tuple(a[0] for a in ASSERTIONS)
REPORT_ONLY = frozenset({"P1", "P2"})

STATES = ("live-pass", "live-fail", "blocked", "stale-block")


class RecordError(ValueError):
    """The record or the expectations file is malformed; the verdict cannot be computed."""


# ---------------------------------------------------------------------------
# Small fail-closed accessors. Each returns (value, failure_code_or_None).


def _number(value: Any) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return float(value)


def _stat(frame: Dict[str, Any], key: str) -> Tuple[Any, Optional[str]]:
    stats = frame.get("stats")
    if not isinstance(stats, dict) or key not in stats:
        return None, f"missing:{key}"
    return stats[key], None


def _stat_number(frame: Dict[str, Any], key: str) -> Tuple[Optional[float], Optional[str]]:
    value, code = _stat(frame, key)
    if code:
        return None, code
    number = _number(value)
    if number is None:
        return None, f"nan_or_type:{key}"
    return number, None


def _field(frame: Dict[str, Any], key: str) -> Tuple[Any, Optional[str]]:
    if key not in frame or frame[key] is None:
        return None, f"missing:{key}"
    return frame[key], None


# ---------------------------------------------------------------------------
# Record shape and freshness.


def validate_record_shape(record: Any) -> List[str]:
    """Record-level failures. Any entry fails every assertion (fail-closed)."""
    problems: List[str] = []
    if not isinstance(record, dict):
        return ["record_not_object"]
    if record.get("schema") != SCHEMA:
        problems.append(f"schema:{record.get('schema')!r}")
    if record.get("variant") not in VARIANTS:
        problems.append(f"variant:{record.get('variant')!r}")
    frames = record.get("frames")
    if not isinstance(frames, list) or not frames:
        problems.append("frames:empty")
        return problems
    if len(frames) != FRAMES_REQUIRED:
        problems.append(f"frames:count={len(frames)}!={FRAMES_REQUIRED}")
    for index, frame in enumerate(frames):
        if not isinstance(frame, dict) or frame.get("frame") != index:
            problems.append(f"frames:index_gap_at={index}")
            break
    return problems


def fresh_frames(frames: Sequence[Dict[str, Any]]) -> List[bool]:
    """A frame is fresh when its stats serial advanced by exactly one.

    ADR 9.3: "frame serial = current frame". The renderer's `frame_count` must
    advance once per harness frame; a repeated or skipped serial means the stats
    describe some other frame and nothing read from them may count.
    """
    result: List[bool] = []
    previous: Optional[float] = None
    for frame in frames:
        serial, code = _stat_number(frame, "frame_count")
        ok = code is None and (previous is None or serial == previous + 1)
        result.append(bool(ok))
        if code is None:
            previous = serial
    return result


# ---------------------------------------------------------------------------
# Per-frame evaluators: frame -> list of failure codes ([] = pass), or None
# when the assertion does not apply to this frame (it then does not count
# towards coverage).

FrameEval = Callable[[Dict[str, Any], Dict[str, Any]], Optional[List[str]]]


def _eval_a1(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    codes: List[str] = []
    world, code = _stat_number(frame, "world_drawn_splats")
    if code:
        codes.append(code)
    elif world <= 0:
        codes.append("value:world_drawn_splats<=0")
    mask, mcode = _field(frame, "frustum_objects")
    if mcode or not isinstance(mask, list):
        codes.append("missing:frustum_objects")
        mask = []
    drawn, dcode = _stat(frame, "object_drawn_splats")
    if dcode:
        codes.append(dcode)
    elif not isinstance(drawn, list):
        codes.append("nan_or_type:object_drawn_splats")
    else:
        for index in mask:
            value = _number(drawn[index]) if isinstance(index, int) and 0 <= index < len(drawn) else None
            if value is None:
                codes.append("missing:object_drawn_splats[i]")
                break
            if value <= 0:
                codes.append("value:object_drawn_splats[i]<=0")
                break
    if record.get("variant") == "B":
        # Proxy from the PR1b counters: rows, not splats, so it can only fail A1,
        # never satisfy it -- the spec keys above are still required.
        rows, code = _stat_number(frame, "hlod_world_draw_records")
        if code:
            codes.append(code)
        elif rows <= 0:
            codes.append("value:hlod_world_draw_records<=0")
        inst, code = _stat_number(frame, "hlod_instance_draw_records")
        if code:
            codes.append(code)
        elif inst < len(mask):
            codes.append("value:hlod_instance_draw_records<frustum_objects")
    return codes


def _eval_a2(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    codes: List[str] = []
    dispatches, code = _stat_number(frame, "sort_dispatches_frame")
    if code:
        codes.append(code)
    elif dispatches != 1:
        codes.append("value:sort_dispatches_frame!=1")
    serial, code = _stat_number(frame, "frame_count")
    dispatch_frame, dcode = _stat_number(frame, "sort_dispatch_frame")
    if code or dcode:
        codes.append(code or dcode)
    elif dispatch_frame != serial - 1:
        codes.append("stale:sort_dispatch_frame")
    sources, code = _stat(frame, "sort_input_sources")
    if code:
        codes.append(code)
    elif not isinstance(sources, list) or not {"world", "objects"} <= set(map(str, sources)):
        codes.append("value:sort_input_sources")
    passes, code = _stat_number(frame, "splat_composite_passes_frame")
    if code:
        codes.append(code)
    elif passes != 1:
        codes.append("value:splat_composite_passes_frame!=1")
    return codes


def _eval_a3(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    codes: List[str] = []
    reason, code = _stat(frame, "route_skip_reason")
    if code:
        codes.append(code)
    elif reason != "none":
        codes.append("value:route_skip_reason")
    if record.get("variant") == "B":
        hlod, code = _stat(frame, "hlod_reason")
        if code:
            codes.append(code)
        elif hlod != "active":
            codes.append("value:hlod_reason")
    dropped, code = _stat_number(frame, "dropped_sources")
    if code:
        codes.append(code)
    elif dropped != 0:
        codes.append("value:dropped_sources")
    stats = frame.get("stats") if isinstance(frame.get("stats"), dict) else {}
    for key, value in stats.items():
        text = f"{key}={value}"
        if "not_exclusive" in text or "identity_shim" in text:
            codes.append("value:stats_not_exclusive_or_identity_shim")
            break
    return codes


def _close(a: Sequence[Any], b: Sequence[Any], tolerance: float) -> Optional[bool]:
    if not isinstance(a, list) or not isinstance(b, list) or len(a) != 3 or len(b) != 3:
        return None
    values = [_number(v) for v in list(a) + list(b)]
    if any(v is None for v in values):
        return None
    return all(abs(values[i] - values[i + 3]) <= tolerance for i in range(3))


def _eval_a4(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    probes, code = _field(frame, "a4_probes")
    if code or not isinstance(probes, list):
        return ["missing:a4_probes"]
    if len(probes) != A4_PROBES:
        return [f"missing:a4_probe_count={len(probes)}"]
    codes: List[str] = []
    included = 0
    for probe in probes:
        if not isinstance(probe, dict):
            codes.append("missing:a4_probe")
            continue
        front = probe.get("cpu_front")
        if front == "excluded":
            # The CPU reference is undefined here (the layers are coplanar, or another
            # layer sits on the ray). It is not a pass; the frame needs enough others.
            continue
        if front not in ("object", "world"):
            codes.append("missing:a4_cpu_front")
            continue
        included += 1
        reference = probe.get("object_ref") if front == "object" else probe.get("world_ref")
        verdict = _close(probe.get("pixel"), reference, A4_TOLERANCE)
        if verdict is None:
            codes.append("missing:a4_pixel")
        elif not verdict:
            codes.append(f"value:a4_{front}_front_mismatch")
    if not codes and included < A4_MIN_INCLUDED:
        return None  # too few defined probes: this frame does not count towards coverage
    return sorted(set(codes))


def _eval_a5(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    tracers, code = _field(frame, "a5_tracers")
    if code or not isinstance(tracers, dict):
        return ["missing:a5_tracers"]
    errors = tracers.get("errors_px")
    if not isinstance(errors, list):
        return ["missing:a5_errors_px"]
    numbers = [_number(e) for e in errors]
    if any(n is None for n in numbers):
        return ["nan_or_type:a5_errors_px"]
    if not numbers:
        return None  # no tracer was measurable this frame: not evaluated
    if max(numbers) > A5_TOLERANCE_PX:
        return ["value:a5_tracer_error>0.5px"]
    return []


def _eval_a6(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    pick, code = _field(frame, "pick")
    if code or not isinstance(pick, dict):
        return ["missing:pick"]
    if pick.get("api_available") is not True:
        return ["missing:pick_api"]
    under = pick.get("target_under_cursor")
    if not isinstance(under, bool):
        return ["missing:target_under_cursor"]
    hit = pick.get("hit_key")
    partial = pick.get("partial")
    if not isinstance(partial, bool):
        return ["missing:pick_partial"]
    codes: List[str] = []
    if under and hit != pick.get("target_key"):
        codes.append("value:pick_missed_target")
    if not under and hit == pick.get("target_key") and hit is not None:
        codes.append("value:pick_hit_target_off_cursor")
    if partial and pick.get("ray_region_resident") is not False:
        codes.append("value:pick_partial_in_resident_region")
    return codes


def _make_c_eval(pose: str) -> FrameEval:
    def _eval(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
        character, code = _field(frame, "character")
        if code or not isinstance(character, dict):
            return ["missing:character"]
        if character.get("pose") not in CHARACTER_POSES:
            return ["missing:character_pose"]
        if character.get("pose") != pose:
            return None
        reference = _number(character.get("reference_pixels"))
        agree = _number(character.get("agreeing_pixels"))
        if reference is None or agree is None:
            return ["missing:character_mask"]
        if reference <= 0:
            return ["value:character_reference_empty"]
        if agree / reference < C_AGREEMENT_MIN:
            return ["value:character_mask_disagrees"]
        return []
    return _eval


def _eval_l3(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    dropped, code = _stat_number(frame, "lights_dropped")
    if code:
        return [code]
    return [] if dropped == 0 else ["value:lights_dropped"]


def _eval_s1(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    codes: List[str] = []
    casters, code = _stat(frame, "shadow_casters_rastered")
    if code:
        codes.append(code)
    elif not isinstance(casters, dict):
        codes.append("nan_or_type:shadow_casters_rastered")
    else:
        for light in SHADOWED_LIGHTS:
            value = _number(casters.get(light))
            if value is None:
                codes.append("missing:shadow_casters_rastered[light]")
                break
            if value <= 0:
                codes.append("value:shadow_casters_rastered[light]<=0")
                break
    no_caster, code = _stat_number(frame, "shadow_no_caster_rastered")
    if code:
        codes.append(code)
    elif no_caster != 0:
        codes.append("value:NO_CASTER_RASTERED")
    return codes


def _make_s_eval(setup_id: str) -> FrameEval:
    setup = SHADOW_SETUPS[setup_id]

    def _eval(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
        rois, code = _field(frame, "shadow_rois")
        if code or not isinstance(rois, dict) or not isinstance(rois.get(setup), dict):
            return [f"missing:shadow_roi:{setup}"]
        shadow = _number(rois[setup].get("shadow_luma"))
        control = _number(rois[setup].get("control_luma"))
        if shadow is None or control is None:
            return [f"missing:shadow_roi:{setup}"]
        if control <= 0:
            return ["value:shadow_control_dark"]
        return [] if shadow / control <= S_RATIO_MAX else ["value:shadow_ratio>0.6"]
    return _eval


def _eval_b1(frame: Dict[str, Any], record: Dict[str, Any]) -> Optional[List[str]]:
    codes: List[str] = []
    ledger, lcode = _stat_number(frame, "ledger_bytes")
    device, dcode = _stat_number(frame, "device_allocated_bytes")
    budget, bcode = _stat_number(frame, "ledger_budget_bytes")
    for c in (lcode, dcode, bcode):
        if c:
            codes.append(c)
    if not (lcode or dcode):
        if device <= 0 or abs(ledger - device) > B1_LEDGER_TOLERANCE * device:
            codes.append("value:ledger_vs_device")
    if not (lcode or bcode) and ledger > budget:
        codes.append("value:ledger_over_budget")
    truncation, code = _stat_number(frame, "silent_truncation_frames")
    if code:
        codes.append(code)
    elif truncation != 0:
        codes.append("value:silent_truncation_frames")
    return codes


FRAME_EVALUATORS: Dict[str, FrameEval] = {
    "A1": _eval_a1, "A2": _eval_a2, "A3": _eval_a3, "A4": _eval_a4, "A5": _eval_a5,
    "A6": _eval_a6, "C1": _make_c_eval("front"), "C2": _make_c_eval("inside"),
    "C3": _make_c_eval("behind"), "L3": _eval_l3, "S1": _eval_s1,
    "S2": _make_s_eval("S2"), "S3": _make_s_eval("S3"), "S4": _make_s_eval("S4"),
    "B1": _eval_b1,
}


# ---------------------------------------------------------------------------
# Whole-run evaluators.


def pearson(xs: Sequence[float], ys: Sequence[float]) -> Optional[float]:
    if len(xs) != len(ys) or len(xs) < 3:
        return None
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


def _light_series(frames: Sequence[Dict[str, Any]], fresh: Sequence[bool], light: str):
    xs: List[float] = []
    ys: List[float] = []
    missing = 0
    for frame, ok in zip(frames, fresh):
        if not ok:
            continue
        lights = frame.get("lights")
        entry = lights.get(light) if isinstance(lights, dict) else None
        x = _number(entry.get("intensity")) if isinstance(entry, dict) else None
        y = _number(entry.get("roi_luma")) if isinstance(entry, dict) else None
        if x is None or y is None:
            missing += 1
            continue
        xs.append(x)
        ys.append(y)
    return xs, ys, missing


def _eval_lights(frames, fresh, lights: Sequence[str]) -> Tuple[int, List[str], Dict[str, Any]]:
    codes: List[str] = []
    detail: Dict[str, Any] = {}
    evaluated = FRAMES_REQUIRED
    for light in lights:
        xs, ys, missing = _light_series(frames, fresh, light)
        evaluated = min(evaluated, len(xs))
        if missing:
            codes.append(f"missing:light_roi:{light}")
        r = pearson(xs, ys)
        detail[light] = {"pearson_r": r, "frames": len(xs)}
        if r is None:
            codes.append(f"value:pearson_undefined:{light}")
        elif r < L_PEARSON_MIN:
            codes.append("value:pearson<0.95")
    return evaluated, sorted(set(codes)), detail


def _eval_t1(record, frames, fresh) -> Tuple[int, List[str], Dict[str, Any]]:
    segment = record.get("segments", {}).get("frozen") if isinstance(record.get("segments"), dict) else None
    if not (isinstance(segment, list) and len(segment) == 2 and all(isinstance(v, int) for v in segment)):
        return 0, ["missing:frozen_segment"], {}
    start, end = segment
    if end - start != FROZEN_SEGMENT_FRAMES or start < 0 or end > len(frames):
        return 0, ["value:frozen_segment_length"], {"segment": segment}
    codes: List[str] = []
    if record.get("taa_enabled") is not False:
        codes.append("value:taa_not_off")
    seg = frames[start:end]
    if not all(fresh[start:end]):
        codes.append("stale:frame_count")
    inputs = [f.get("input_digest") for f in seg]
    images = [f.get("frame_digest") for f in seg]
    frozen_flags = [f.get("frozen") for f in seg]
    if any(not isinstance(v, str) or not v for v in inputs):
        return 0, ["missing:input_digest"], {}
    if any(not isinstance(v, str) or not v for v in images):
        return 0, ["missing:frame_digest"], {}
    if any(flag is not True for flag in frozen_flags):
        codes.append("value:frozen_flag")
    if len(set(inputs)) != 1:
        # The segment's inputs changed: INVALID, not a pass (ADR 9.3).
        codes.append("invalid:inputs_not_frozen")
    differing = sum(1 for digest in images if digest != images[0])
    if differing:
        codes.append("value:frames_differ")
    return len(seg), sorted(set(codes)), {"segment": segment, "frames_differing": differing}


def _percentile(sorted_values: Sequence[float], q: float) -> float:
    index = max(0, min(len(sorted_values) - 1, int(math.ceil(q * len(sorted_values))) - 1))
    return sorted_values[index]


def timing_series(frames, fresh) -> Dict[str, Any]:
    """P-family source: per-frame `gpu_frame_ms` with its serial, validity and lag.

    Complete: fresh stats, reported valid, finite time, lag in [0, 3], and a
    serial that advanced past the previous frame's. Stale: reported valid with a
    serial that did NOT advance -- a sticky/frozen timestamp claiming to be new
    (#530). Incomplete frames are honest gaps (allowed up to 1 %); one stale
    frame fails the hard check.
    """
    complete: List[float] = []
    stale = 0
    missing = 0
    previous_serial: Optional[float] = None
    for frame, ok in zip(frames, fresh):
        valid, vcode = _stat(frame, "gpu_frame_valid")
        ms, mcode = _stat_number(frame, "gpu_frame_ms")
        serial, scode = _stat_number(frame, "gpu_timing_frame_serial")
        behind, bcode = _stat_number(frame, "gpu_timing_frames_behind")
        if vcode or mcode or scode or bcode:
            missing += 1
            continue
        advanced = previous_serial is None or serial > previous_serial
        if valid is True and not advanced:
            stale += 1
        elif ok and valid is True and 0 <= behind <= P_MAX_FRAMES_BEHIND and ms > 0:
            complete.append(ms)
        previous_serial = serial if previous_serial is None else max(previous_serial, serial)
    return {"complete_ms": complete, "stale": stale, "missing": missing}


def _eval_p(assertion: str, record, frames, fresh) -> Tuple[int, List[str], Dict[str, Any]]:
    series = timing_series(frames, fresh)
    complete = series["complete_ms"]
    ratio = len(complete) / FRAMES_REQUIRED
    detail: Dict[str, Any] = {"complete_frames": len(complete), "stale_frames": series["stale"],
                              "missing_frames": series["missing"], "complete_ratio": ratio}
    if assertion == "P3":
        codes: List[str] = []
        if series["missing"]:
            codes.append("missing:gpu_timing_keys")
        if series["stale"]:
            codes.append("stale:gpu_timing_frame_serial")
        if ratio < P_TIMING_COMPLETE_MIN:
            codes.append("value:timing_complete<99%")
        return FRAMES_REQUIRED - series["missing"], codes, detail
    if not complete:
        return 0, ["missing:gpu_frame_ms"], detail
    gpu = str(record.get("gpu_class", ""))
    targets = GR15_TARGETS_MS.get(gpu)
    values = sorted(complete)
    key = "p95" if assertion == "P1" else "p99"
    measured = _percentile(values, 0.95 if key == "p95" else 0.99)
    detail.update({"gpu_class": gpu, "measured_ms": measured, "target_ms": targets[key] if targets else None,
                   "build_optimized": record.get("build_optimized")})
    codes = []
    if targets is None:
        codes.append("missing:gr15_target_for_gpu")
    elif measured > targets[key]:
        # Report-only until the BV calibration: shown, never gating.
        codes.append(f"report:{key}_over_target")
    if record.get("build_optimized") is not True:
        # A dev (-O0) build measures the build, not the renderer.
        codes.append("invalid:build_not_optimized")
    if series["stale"] or ratio < P_TIMING_COMPLETE_MIN:
        codes.append("invalid:timing_not_frame_coherent")
    return len(complete), codes, detail


def _eval_v1(record, frames, fresh) -> Tuple[int, List[str], Dict[str, Any]]:
    split = record.get("split_screen")
    if not isinstance(split, dict):
        return 0, ["missing:split_screen_job"], {}
    return 0, ["missing:split_screen_evaluator"], {}


def _log_codes(log_text: Optional[str]) -> List[str]:
    if log_text is None:
        return ["missing:log"]
    lowered = log_text.lower()
    return [f"value:log:{pattern.replace(' ', '_')}" for pattern in LOG_FORBIDDEN if pattern in lowered]


# ---------------------------------------------------------------------------
# Evaluation and classification.


def evaluate_assertions(record: Dict[str, Any], log_text: Optional[str]) -> Dict[str, Dict[str, Any]]:
    """Raw per-assertion results: evaluated frame count, failure codes, detail."""
    results: Dict[str, Dict[str, Any]] = {}
    shape = validate_record_shape(record)
    if shape:
        for aid, _family, minimum, _text in ASSERTIONS:
            results[aid] = {"evaluated": 0, "required": minimum, "codes": ["record:" + p for p in shape],
                            "failing_frames": 0, "detail": {}}
        return results
    frames: List[Dict[str, Any]] = record["frames"]
    fresh = fresh_frames(frames)
    stale_frames = fresh.count(False)
    for aid, _family, minimum, _text in ASSERTIONS:
        codes: List[str] = []
        detail: Dict[str, Any] = {}
        failing = 0
        if aid in FRAME_EVALUATORS:
            evaluator = FRAME_EVALUATORS[aid]
            evaluated = 0
            counts: Dict[str, int] = {}
            for frame, ok in zip(frames, fresh):
                if not ok:
                    continue  # stale stats never count, in either direction
                frame_codes = evaluator(frame, record)
                if frame_codes is None:
                    continue
                evaluated += 1
                if frame_codes:
                    failing += 1
                    for c in frame_codes:
                        counts[c] = counts.get(c, 0) + 1
            codes = sorted(counts)
            detail["code_frames"] = counts
        elif aid in ("L1", "L2"):
            evaluated, codes, detail = _eval_lights(frames, fresh, ("directional",) if aid == "L1" else OMNI_LIGHTS)
        elif aid == "T1":
            evaluated, codes, detail = _eval_t1(record, frames, fresh)
        elif aid in ("P1", "P2", "P3"):
            evaluated, codes, detail = _eval_p(aid, record, frames, fresh)
        elif aid == "V1":
            evaluated, codes, detail = _eval_v1(record, frames, fresh)
        else:  # pragma: no cover - registry and dispatch are tested to match
            raise AssertionError(aid)
        if aid == "A3":
            codes = sorted(set(codes) | set(_log_codes(log_text)))
        if stale_frames and aid not in ("V1",):
            codes = sorted(set(codes) | {"stale:frame_count"})
        if evaluated < minimum:
            codes = sorted(set(codes) | {"coverage:below_minimum"})
        results[aid] = {"evaluated": evaluated, "required": minimum, "codes": codes,
                        "failing_frames": failing, "detail": detail}
    return results


def load_expectations(data: Any) -> Dict[str, Dict[str, Dict[str, Any]]]:
    """variant -> assertion -> {issue, reason, expected_failures}. Malformed raises."""
    if not isinstance(data, dict) or data.get("schema") != EXPECTATIONS_SCHEMA:
        raise RecordError("expectations: wrong or missing schema")
    registered = data.get("preregistered_thresholds")
    if not isinstance(registered, dict) or set(registered) != set(PREREGISTERED_THRESHOLDS):
        raise RecordError("expectations: preregistered_thresholds must name exactly "
                          f"{sorted(PREREGISTERED_THRESHOLDS)}")
    for name, (value, assertions) in PREREGISTERED_THRESHOLDS.items():
        entry = registered[name]
        if not isinstance(entry, dict) or entry.get("value") != value \
                or entry.get("assertions") != list(assertions) \
                or not entry.get("preregistered") or not entry.get("reason"):
            raise RecordError(f"expectations: pre-registered threshold {name} must be {value} for "
                              f"{list(assertions)} with its date and reason; it is frozen")
    blocked = data.get("blocked")
    if not isinstance(blocked, dict) or set(blocked) != set(VARIANTS):
        raise RecordError("expectations: 'blocked' must name exactly the variants A and B")
    out: Dict[str, Dict[str, Dict[str, Any]]] = {}
    for variant, entries in blocked.items():
        if not isinstance(entries, dict):
            raise RecordError(f"expectations: variant {variant} must map assertion ids")
        out[variant] = {}
        for aid, entry in entries.items():
            if aid not in ASSERTION_IDS:
                raise RecordError(f"expectations: unknown assertion {aid!r}")
            if not isinstance(entry, dict):
                raise RecordError(f"expectations: {variant}.{aid} must be an object")
            issue = entry.get("issue")
            if not (isinstance(issue, str) and issue.startswith("#") and issue[1:].isdigit()):
                raise RecordError(f"expectations: {variant}.{aid} needs an issue like '#123'")
            reason = entry.get("reason")
            if not (isinstance(reason, str) and reason.strip()):
                raise RecordError(f"expectations: {variant}.{aid} needs a reason")
            expected = entry.get("expected_failures")
            if not (isinstance(expected, list) and expected and all(isinstance(c, str) and c for c in expected)):
                raise RecordError(f"expectations: {variant}.{aid} needs a non-empty expected_failures list")
            out[variant][aid] = {"issue": issue, "reason": reason, "expected_failures": list(expected),
                                 "followup": entry.get("followup")}
    return out


def load_notes(data: Any) -> Dict[str, Dict[str, str]]:
    """variant -> assertion -> note. Informational only: a note never changes a state."""
    notes = data.get("notes", {}) if isinstance(data, dict) else {}
    if not isinstance(notes, dict) or not set(notes) <= set(VARIANTS):
        raise RecordError("expectations: 'notes' must map variants A/B")
    out: Dict[str, Dict[str, str]] = {}
    for variant, entries in notes.items():
        if not isinstance(entries, dict):
            raise RecordError(f"expectations: notes.{variant} must map assertion ids")
        out[variant] = {}
        for aid, entry in entries.items():
            if aid not in ASSERTION_IDS or not isinstance(entry, dict) or not str(entry.get("reason", "")).strip():
                raise RecordError(f"expectations: notes.{variant}.{aid} needs a known id and a reason")
            out[variant][aid] = entry["reason"]
    return out


def _code_matches(code: str, expected: Iterable[str]) -> bool:
    # An expected entry ending in '*' matches a prefix (e.g. "missing:light_roi:*").
    for pattern in expected:
        if pattern.endswith("*") and code.startswith(pattern[:-1]):
            return True
        if code == pattern:
            return True
    return False


def classify(results: Dict[str, Dict[str, Any]], blocked: Dict[str, Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    out: Dict[str, Dict[str, Any]] = {}
    for aid, _family, _minimum, text in ASSERTIONS:
        result = results[aid]
        report_codes = [c for c in result["codes"] if c.startswith("report:")]
        codes = [c for c in result["codes"] if not c.startswith("report:")]
        entry = blocked.get(aid)
        row: Dict[str, Any] = {"statement": text, "evaluated": result["evaluated"], "required": result["required"],
                               "failing_frames": result["failing_frames"], "codes": codes,
                               "detail": result["detail"], "report_only": aid in REPORT_ONLY,
                               "report_codes": report_codes}
        if entry is None:
            row["state"] = "live-pass" if not codes else "live-fail"
        elif not codes:
            row["state"] = "stale-block"
        else:
            unexplained = [c for c in codes if not _code_matches(c, entry["expected_failures"])]
            row["state"] = "blocked" if not unexplained else "live-fail"
            row["unexplained_codes"] = unexplained
        if entry is not None:
            row["issue"] = entry["issue"]
            row["reason"] = entry["reason"]
            if entry.get("followup"):
                row["followup"] = entry["followup"]
        out[aid] = row
    return out


def verdict(record: Dict[str, Any], expectations: Dict[str, Dict[str, Dict[str, Any]]],
            log_text: Optional[str], notes: Optional[Dict[str, Dict[str, str]]] = None) -> Dict[str, Any]:
    variant = record.get("variant") if isinstance(record, dict) else None
    blocked = expectations.get(variant, {}) if variant in VARIANTS else {}
    rows = classify(evaluate_assertions(record, log_text), blocked)
    for aid, note in ((notes or {}).get(variant) or {}).items():
        rows[aid]["note"] = note
    counts = {state: sum(1 for r in rows.values() if r["state"] == state) for state in STATES}
    # P1/P2 are report-only for their GR-15 thresholds ("report:" codes, shown
    # but not gating) and hard for timing validity, like P3.
    met = counts["live-pass"] == len(rows) and not blocked
    return {"schema": VERDICT_SCHEMA, "variant": variant, "gate_met": met, "counts": counts,
            "regressions": sorted(aid for aid, r in rows.items() if r["state"] == "live-fail"),
            "blocked": sorted(aid for aid, r in rows.items() if r["state"] == "blocked"),
            "stale_blocks": sorted(aid for aid, r in rows.items() if r["state"] == "stale-block"),
            "assertions": rows}


# ---------------------------------------------------------------------------
# Forcing controls (ADR 9.4), applied to a captured record.


def _ctl_restore_exclusivity(record):
    # What `instance_store.is_empty()` exclusivity does to a frame: the world row
    # vanishes whenever objects are present.
    for frame in record["frames"]:
        stats = frame.get("stats")
        if isinstance(stats, dict):
            for key in ("world_drawn_splats", "hlod_world_draw_records"):
                if key in stats:
                    stats[key] = 0
            if "hlod_reason" in stats:
                stats["hlod_reason"] = "not_exclusive"


def _ctl_drop_objects_from_sort(record):
    for frame in record["frames"]:
        stats = frame.get("stats")
        if isinstance(stats, dict):
            if isinstance(stats.get("sort_input_sources"), list):
                stats["sort_input_sources"] = [s for s in stats["sort_input_sources"] if s != "objects"]
            if "sort_dispatches_frame" in stats:
                stats["sort_dispatches_frame"] = 2
        for probe in frame.get("a4_probes") or []:
            if isinstance(probe, dict) and probe.get("cpu_front") == "object":
                probe["pixel"] = probe.get("world_ref")


def _ctl_disable_caster_pass(record):
    for frame in record["frames"]:
        stats = frame.get("stats")
        if isinstance(stats, dict):
            if isinstance(stats.get("shadow_casters_rastered"), dict):
                stats["shadow_casters_rastered"] = {k: 0 for k in stats["shadow_casters_rastered"]}
            if "shadow_no_caster_rastered" in stats:
                stats["shadow_no_caster_rastered"] = 1


def _ctl_delete_object_drawn(record):
    frames = record["frames"]
    victim = frames[len(frames) // 2]
    if isinstance(victim.get("stats"), dict):
        victim["stats"].pop("object_drawn_splats", None)


def _ctl_freeze_timestamp(record):
    frames = record["frames"]
    index = len(frames) // 2
    if index < 1:
        return
    previous = frames[index - 1].get("stats") or {}
    current = frames[index].get("stats")
    if isinstance(current, dict):
        for key in ("gpu_timing_frame_serial", "gpu_frame_ms"):
            if key in previous:
                current[key] = previous[key]
        current["gpu_frame_valid"] = True


def _ctl_pick_no_hit(record):
    for frame in record["frames"]:
        pick = frame.get("pick")
        if isinstance(pick, dict):
            pick["hit_key"] = None


# number -> (name, target assertions, record mutation, runtime switch available today)
CONTROLS: Dict[int, Tuple[str, Tuple[str, ...], Callable[[Dict[str, Any]], None], bool]] = {
    1: ("restore_instance_store_exclusivity", ("A1", "A3"), _ctl_restore_exclusivity, False),
    2: ("drop_objects_from_sort_input", ("A2", "A4"), _ctl_drop_objects_from_sort, False),
    3: ("disable_shadow_caster_pass", ("S1",), _ctl_disable_caster_pass, False),
    4: ("delete_stats_key_object_drawn_splats", ("A1",), _ctl_delete_object_drawn, True),
    5: ("freeze_one_pass_timestamp", ("P3",), _ctl_freeze_timestamp, True),
    6: ("pick_returns_no_hit", ("A6",), _ctl_pick_no_hit, True),
}


def run_controls(record: Dict[str, Any], log_text: Optional[str]) -> Dict[str, Any]:
    """Each control must turn a passing target into a failing one on this data.

    ``proven``: at least one target passed on the original record and fails
    under the control. ``unproven``: every target already fails, so this record
    cannot show the control bites. ``broken``: a target passed and still passes
    under the control -- the assertion cannot see the defect it exists for.
    """
    base = evaluate_assertions(record, log_text)
    out: Dict[str, Any] = {}
    for number, (name, targets, mutate, runtime) in CONTROLS.items():
        forced = copy.deepcopy(record)
        if validate_record_shape(forced):
            out[str(number)] = {"name": name, "targets": list(targets), "state": "unproven",
                                "why": "record malformed", "runtime_switch": runtime}
            continue
        mutate(forced)
        after = evaluate_assertions(forced, log_text)
        passing_before = [t for t in targets if not base[t]["codes"]]
        if not passing_before:
            state, why = "unproven", "every target already fails on this record"
        elif any(after[t]["codes"] for t in passing_before):
            state, why = "proven", "target turned red under the control"
        else:
            state, why = "broken", "target stayed green under the control"
        out[str(number)] = {"name": name, "targets": list(targets), "state": state, "why": why,
                            "runtime_switch": runtime,
                            "target_codes_after": {t: after[t]["codes"] for t in targets}}
    return out


# ---------------------------------------------------------------------------
# Report rendering and CLI.


def markdown_table(verdicts: Sequence[Dict[str, Any]]) -> str:
    lines = ["| ID | " + " | ".join(f"Variant {v['variant']}" for v in verdicts) + " |",
             "|---|" + "---|" * len(verdicts)]
    for aid in ASSERTION_IDS:
        cells = []
        for v in verdicts:
            row = v["assertions"][aid]
            cell = row["state"]
            if row.get("issue"):
                cell += f" ({row['issue']})"
            if row["state"] in ("live-fail", "blocked"):
                shown = row.get("unexplained_codes") or row["codes"]
                cell += ": " + ", ".join(shown[:3])
            if row.get("note") and row["state"] != "live-pass":
                cell += f" [{row['note'].split(':')[0]}]"
            cells.append(cell)
        lines.append(f"| {aid} | " + " | ".join(cells) + " |")
    for v in verdicts:
        lines.append("")
        lines.append(f"Variant {v['variant']}: gate_met={v['gate_met']} counts={v['counts']} "
                     f"regressions={v['regressions']}")
    return "\n".join(lines)


def _main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--record", action="append", required=True, type=Path)
    parser.add_argument("--log", action="append", type=Path, default=[],
                        help="engine log for the matching --record (same order)")
    parser.add_argument("--expectations", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--controls", action="store_true")
    args = parser.parse_args(argv)
    raw_expectations = json.loads(args.expectations.read_text(encoding="utf-8"))
    expectations = load_expectations(raw_expectations)
    notes = load_notes(raw_expectations)
    verdicts = []
    for index, path in enumerate(args.record):
        record = json.loads(path.read_text(encoding="utf-8"))
        log_text = args.log[index].read_text(encoding="utf-8", errors="replace") if index < len(args.log) else None
        result = verdict(record, expectations, log_text, notes)
        if args.controls:
            result["controls"] = run_controls(record, log_text)
        verdicts.append(result)
    print(markdown_table(verdicts))
    if args.out:
        args.out.write_text(json.dumps(verdicts, indent=2, sort_keys=True), encoding="utf-8")
    return 0 if verdicts and all(v["gate_met"] for v in verdicts) else 1


if __name__ == "__main__":
    sys.exit(_main())
