#!/usr/bin/env python3
"""GS-GAME-1, dispatch/report only. Pure evaluation of actual producer evidence.

Threshold quotations are the ADR dynamics §7 table, not a second specification.
Missing evidence is NOT_RUN; malformed evidence is INVALID. Forcing controls are
separate producer runs in the same job, never mutations of a captured record.
The coordinator's 2026-11-21 decisions supplement measurement prerequisites,
not ADR performance thresholds. No number is assigned to M1's upload rate.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
O6_LABEL = "APPROXIMATION: RTX 3080 with an 8 GB ledger cap; not the mid-range result"
O1_STATE = "NOT EVALUATED: no triaged tolerance contract (Refs #1520)"
MEASURED = "MEASURED (no ADR threshold)"
NO_CONTROL_PASS = "PASS (no forcing control, ADR §7)"
ROW_IDS = ("P1", "P2", "H1", "W1", "W2", "W3", "W4", "C1", "R1", "R2", "R3",
           "S1", "O1", "O2", "L1", "L2", "M1", "F3")
CONTROLLED = ("P1", "H1", "W1", "W2", "W3", "W4", "C1", "R1", "R2", "R3", "S1", "O2")


class Missing(ValueError):
    pass


class Invalid(ValueError):
    pass


def field(obj, key):
    if not isinstance(obj, dict) or key not in obj or obj[key] is None:
        raise Missing(key)
    return obj[key]


def number(obj, key, *, positive=False):
    value = field(obj, key)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise Invalid(f"{key}: expected finite nonnegative number")
    if positive and value == 0:
        raise Missing(f"{key}: no measured samples")
    return value


def sequence(obj, key):
    value = field(obj, key)
    if not isinstance(value, list):
        raise Invalid(f"{key}: not an array")
    if not value:
        raise Missing(f"{key}: empty")
    return value


def numbers(obj, key):
    return [number({key: v}, key) for v in sequence(obj, key)]


def truth(obj, key):
    value = field(obj, key)
    if not isinstance(value, bool):
        raise Invalid(f"{key}: not boolean")
    return value


def percentile(values, p):
    """Nearest-rank percentile, including every sampled frame."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p * len(ordered)) - 1)]


def timed_frames(obj, key="frames"):
    rows = sequence(obj, key)
    seen = set()
    for f in rows:
        frame = number(f, "frame")
        if frame in seen or number(f, "gpu_frame") != frame:
            raise Missing("non-unique or incoherent GPU timestamp frame")
        seen.add(frame)
        if not truth(f, "gpu_frame_valid"):
            raise Missing("gpu_frame_valid=false")
        number(f, "frame_ms", positive=True)
        number(f, "gpu_frame_ms", positive=True)
    return rows


def p1_control_ready(m):
    frames = timed_frames(m)
    calibration = field(m, "calibration")
    if not truth(calibration, "calibrated"):
        raise Missing("P1 stall calibration failed")
    attempts = number(calibration, "attempts", positive=True)
    bound = number(calibration, "bounded_attempts", positive=True)
    if attempts > bound or bound > 8:
        raise Missing("P1 calibration exceeded its bounded attempts")
    samples = sequence(calibration, "samples")
    if len(samples) != attempts or number(samples[-1], "iterations", positive=True) != number(calibration, "iterations", positive=True):
        raise Missing("P1 winning calibration sample absent")
    measured = field(samples[-1], "measurement")
    if not truth(measured, "stall_valid") or number(measured, "stall_ms") < 30:
        raise Missing("P1 calibration timestamp not valid/measured >= 30 ms")
    stalls = sequence(m, "stalls")
    stalled_frames = {number(s, "frame") for s in stalls}
    if len(stalled_frames) != len(stalls) or not stalled_frames <= {f["frame"] for f in frames}:
        raise Missing("P1 stalled frames do not belong to the separate control pass")
    if number(m, "stalled_frame_count") != len(stalls) or len(stalls) * 10 < len(frames):
        raise Missing("P1 stalled fewer than 10 % of sampled control frames")
    for stall in stalls:
        if not truth(stall, "stall_valid") or number(stall, "stall_ms") < 30:
            raise Missing("P1 per-frame stall not measured >= 30 ms with valid GPU timestamps")


def judge(row, m, cap):
    """Return measurement state + raw-derived statistics; all limits cite §7 row."""
    if row == "P1":
        fs = timed_frames(m)
        ms = [f["frame_ms"] for f in fs]
        spawn = [f["frame_ms"] for f in fs if truth(f, "spawn")]
        if not spawn:
            raise Missing("P1 spawn samples absent")
        values = {"p95_ms": percentile(ms, .95), "p99_ms": percentile(ms, .99),
                  "spawn_max_ms": max(spawn), "target_p95_ms": 16.7}
        # ADR §7 P1: hard gate 25/33.3, max 50; target 16.7 is always reported.
        ok = values["p95_ms"] <= 25 and values["p99_ms"] <= 33.3 and max(spawn) <= 50
        return ("PASS" if ok else "FAIL"), values
    if row == "P2":
        on, off = timed_frames(m, "on"), timed_frames(m, "off")
        if len(on) != len(off):
            raise Missing("P2 arms have different samples")
        ratios, overhead = [], []
        for a, b in zip(on, off):
            if field(a, "sample") != field(b, "sample") or not field(a, "input_digest") \
                    or field(a, "input_digest") != field(b, "input_digest"):
                raise Missing("P2 arms have nonidentical input samples")
            ratio = a["frame_ms"] / b["frame_ms"]
            ratios.append(ratio)
            overhead.append((ratio - 1) * b["frame_ms"])
        # ADR §7 P2: ≤ +1.5 ms p95; paired ratios, never subtract percentiles.
        return (NO_CONTROL_PASS if percentile(overhead, .95) <= 1.5 else "FAIL"), {
            "paired_ratio_p95": percentile(ratios, .95),
            "paired_overhead_p95_ms": percentile(overhead, .95), "samples": len(on)}
    if row == "H1":
        if not truth(m, "settled"):
            raise Missing("H1 not settled")
        nonresident = number(m, "nonresident_drawn")
        fraction = number(m, "missing_nodes") / number(m, "expected_nodes", positive=True)
        # ADR §7 H1: 0 / 0 %.
        return ("PASS" if nonresident == 0 and fraction == 0 else "FAIL"), {
            "nonresident_drawn": nonresident, "missing_node_fraction": fraction}
    if row == "W1":
        arms = sequence(m, "arms")
        if sorted(number(a, "divisor") for a in arms) != [0, 8]:
            raise Missing("W1 needs divisor 8 and amortization off")
        changed = []
        for a in arms:
            if field(a, "drawn_digest_before") != field(a, "drawn_digest_after"):
                raise Invalid("W1 drawn digest differs; path-dependent cut, ADR §7 oracle step 4")
            if not field(a, "drawn_digest_before"):
                raise Missing("W1 empty drawn digest")
            if number(a, "wind_time_scale") != 0 or not truth(a, "wind_nonzero") or not truth(a, "settled"):
                raise Missing("W1 requires frozen non-zero wind and resettlement")
            if set(sequence(a, "evicted_nodes")) != set(sequence(a, "drawn_nodes_before")):
                raise Missing("W1 full drawn-set eviction unproven")
            if not set(sequence(a, "pages_before")) <= set(sequence(a, "preoccupied_pages")):
                raise Missing("W1 all freed runs must be preoccupied")
            if sequence(a, "pages_before") == sequence(a, "pages_after"):
                raise Missing("W1 readmission did not relocate pages")
            hysteresis = field(a, "hysteresis")
            if hysteresis != "disabled" and (hysteresis != "after >= 2 frames" or number(a, "frames_after_readmission") < 2):
                raise Missing("W1 hysteresis rule unproven")
            compared = number(a, "frames_compared", positive=True)
            count = number(a, "changed_frames")
            if count > compared:
                raise Invalid("W1 changed_frames exceeds comparisons")
            comparisons = sequence(a, "comparisons")
            if len(comparisons) != compared or not field(a, "baseline_sha256"):
                raise Missing("W1 image comparisons/baseline absent")
            observed_changed = sum(number(c, "changed_pixels") > 0 for c in comparisons)
            if any(not field(c, "image_sha256") for c in comparisons):
                raise Missing("W1 image readback absent")
            if observed_changed != count:
                raise Invalid("W1 changed-frame counter disagrees with image comparisons")
            changed.append(count)
        # ADR §7 W1: **0 changed frames**, on both required arms.
        return ("PASS" if max(changed) == 0 else "FAIL"), {"changed_frames": changed, "arms": arms}
    if row in ("W2", "W3", "W4", "O2"):
        # ADR §7 W2 ≤ 0.5 px; W3 ≤ 1 shadow texel; W4 0 px;
        # O2 ≤ 0.5 px object offset, moving caster vs colour ≤ 1 texel.
        limits = {"W2": [("errors_px", .5)], "W3": [("errors_shadow_texels", 1)],
                  "W4": [("weight_zero_displacement_px", 0)],
                  "O2": [("object_offsets_px", .5), ("caster_colour_offsets_texels", 1)]}
        maxima = {key: max(numbers(m, key)) for key, _ in limits[row]}
        return ("PASS" if all(maxima[k] <= limit for k, limit in limits[row]) else "FAIL"), maxima
    if row == "C1":
        if not all(truth(m, key) for key in ("first_frame_after_admission", "same_pose", "same_drawn_digest")) \
                or number(m, "divisor") != 8 or number(m, "reference_divisor") != 1:
            raise Missing("C1 first-admission/divisor-1 identical-pose/drawn-set proof absent")
        first = field(m, "native_first_frame")
        reference = field(m, "native_reference_frame")
        sequence(first, "absent_at_arm")
        if not set(sequence(first, "first_readmitted_nodes")) <= set(first["absent_at_arm"]):
            raise Invalid("C1 readmission not a previously absent target")
        number(first, "frame", positive=True)
        if number(reference, "frame", positive=True) <= first["frame"]:
            raise Missing("C1 reference did not follow the first readmission frame")
        if not field(first, "drawn_digest") or first["drawn_digest"] != field(reference, "drawn_digest"):
            raise Missing("C1 native first/reference drawn digest mismatch")
        changed = numbers(m, "changed_pixels")
        # ADR §7 C1: 0 changed pixels in the first frame after admission.
        return ("PASS" if max(changed) == 0 else "FAIL"), {"changed_pixels": changed}
    if row == "R1":
        lods = sequence(m, "lods")
        if sorted(number(l, "level") for l in lods) != sorted(numbers(m, "expected_lods")):
            raise Missing("R1 missing a LOD level")
        ok = True
        for l in lods:
            eligible = number(l, "region_pixels", positive=True)
            ok &= number(l, "tinted_pixels") == eligible and number(l, "foreign_pixels") == 0
        # ADR §7 R1: 100 % of region pixels tinted, 0 foreign pixels.
        return ("PASS" if ok else "FAIL"), {"lods": lods}
    if row == "R2":
        number(m, "set_frame", positive=True)
        ok = truth(m, "same_frame_effect") and truth(m, "survives_far_evict_near")
        # ADR §7 R2 gives a qualitative requirement, no numeric threshold.
        return (MEASURED if ok else "FAIL"), m
    if row == "R3":
        number(m, "visible_region_records", positive=True)
        # ADR §7 R3: 0 sort records for the hidden region; no shadow.
        ok = number(m, "hidden_region_sort_records") == 0 and number(m, "hidden_region_shadow_pixels") == 0
        return ("PASS" if ok else "FAIL"), m
    if row == "S1":
        before, root = number(m, "before_drawn_nodes", positive=True), number(m, "root_nodes", positive=True)
        if before <= root or not truth(m, "new_asset"):
            raise Missing("S1 no refined world/new asset opportunity")
        # ADR §7 S1 qualitative requirement; no new numeric threshold.
        return (MEASURED if min(numbers(m, "spawn_drawn_nodes")) > root else "FAIL"), m
    if row == "L1":
        number(m, "lit_pixels", positive=True)
        if not truth(m, "lighting_verified"):
            raise Missing("L1 lighting not verified")
        return MEASURED, m  # ADR §7 L1: pass (no ADR number).
    if row == "L2":
        # Coordinator: missing omni/spot shadows on HLOD are FAIL, never skipped.
        if not truth(m, "hlod"):
            return "FAIL", m
        omni, spot = number(m, "omni_splat_shadow_pixels"), number(m, "spot_splat_shadow_pixels")
        return (MEASURED if omni > 0 and spot > 0 else "FAIL"), m  # ADR §7 L2 has no ADR number.
    if row == "M1":
        ledger, external = number(m, "ledger_bytes"), number(m, "external_vram_bytes")
        upload = number(m, "upload_bytes_per_second")
        # ADR §7 M1: within budget; upload ≤ a threshold fixed before the first run.
        # Coordinator: absent ADR upload number => MEASURED, never invent one.
        return (MEASURED if ledger <= cap and external <= cap else "FAIL"), {
            "ledger_bytes": ledger, "external_vram_bytes": external, "budget_bytes": cap,
            "upload_bytes_per_second": upload, "upload_state": MEASURED}
    if row == "F3":
        if field(m, "world") != "synthetic T0" or field(m, "producer") != "B2":
            raise Missing("F3 requires the B2 bake counters on synthetic T0")
        keyed = number(m, "region_keyed_interior_bytes", positive=True)
        single = number(m, "single_region_interior_bytes", positive=True)
        # ADR §7 F3: interior bytes ≤ +10 % versus single-region.
        return (NO_CONTROL_PASS if keyed <= single * 1.1 else "FAIL"), {
            "region_keyed_interior_bytes": keyed, "single_region_interior_bytes": single,
            "interior_byte_overhead_percent": 100 * (keyed / single - 1)}
    raise Invalid(f"unknown row {row}")


def attempt(row, measurement, cap):
    try:
        return judge(row, measurement, cap)
    except Missing as exc:
        return "NOT_RUN", {"reason": str(exc)}
    except (Invalid, TypeError, KeyError, ValueError) as exc:
        return "INVALID", {"reason": str(exc)}


def verdict(record, expectations=None):
    if expectations is None:
        expectations = json.loads((HERE / "gs_game_1_expectations.json").read_text(encoding="utf-8"))
    result = {"schema": "gs-game-1/verdict/v1", "hardware": record.get("hardware", "NOT_RUN"),
              "approximation": O6_LABEL, "report_only": True, "rows": {}}
    header_ok = (record.get("schema") == "gs-game-1/raw/v1" and bool(record.get("job_id"))
                 and record.get("hardware") == "RTX 3080" and record.get("ledger_cap_bytes") == 8_000_000_000
                 and record.get("approximation") == O6_LABEL and bool(record.get("subject_run")))
    for row in ROW_IDS:
        contract = expectations["rows"][row]
        out = {"state": "NOT_RUN", "adr_source": f"ADR §7 {row}",
               "adr_threshold": contract["threshold_verbatim"],
               "control_required": row in CONTROLLED}
        result["rows"][row] = out
        if row == "O1":
            out["state"] = O1_STATE
            continue
        if record.get("invalid_reason"):
            out.update(state="INVALID", reason=record["invalid_reason"])
            continue
        if not header_ok:
            out["reason"] = "hardware/job/schema/O6 provenance absent or incorrect"
            continue
        item = record.get("rows", {}).get(row, {})
        state, details = attempt(row, item.get("measurement", {}), record["ledger_cap_bytes"])
        out.update(state=state, measurement=details)
        if row not in CONTROLLED:
            continue
        c = item.get("control", {})
        if (c.get("job_id") != record["job_id"] or not c.get("run")
                or c.get("run") == record["subject_run"] or c.get("switch_observed") is not True):
            if state != "INVALID":
                out.update(state="NOT_RUN", reason="missing observable separate control in the same job")
            continue
        if row == "P1":
            try:
                p1_control_ready(c.get("measurement", {}))
            except (Missing, Invalid, TypeError, KeyError, ValueError) as exc:
                out.update(state="NOT_RUN", reason=str(exc), control_state="NOT_RUN")
                continue
        control_state, control_details = attempt(row, c.get("measurement", {}), record["ledger_cap_bytes"])
        out.update(control_state=control_state, control=control_details)
        if control_state in ("NOT_RUN", "INVALID") and state != "INVALID":
            out["state"] = control_state
        elif control_state != "FAIL" and state not in ("NOT_RUN", "INVALID"):
            out.update(state="FAIL", reason="forcing control did not turn its own row red")
    states = [r["state"] for r in result["rows"].values()]
    result["state"] = next((s for s in ("INVALID", "FAIL", "NOT_RUN") if s in states), MEASURED)
    return result


def markdown(result):
    lines = ["# GS-GAME-1 — report-only", "", f"Hardware: {result['hardware']}",
             O6_LABEL, "", "This lane does not establish game alpha.",
             "P1 hard gate: p95 ≤ 25 ms; p99 ≤ 33.3 ms; max at spawn ≤ 50 ms. Target: p95 ≤ 16.7 ms.",
             "", f"Report state: **{result['state']}**", "",
             "| Row | State | Control | ADR §7 threshold (verbatim) | Evidence |",
             "| --- | --- | --- | --- | --- |"]
    for row, value in result["rows"].items():
        details = json.dumps(value.get("measurement", {"reason": value.get("reason", "")}), ensure_ascii=False)
        details = details.replace("|", "\\|").replace("\n", " ")
        if len(details) > 500:
            details = "See raw.json rows." + row + " and verdict.json for full measurement; " + details[:400] + "…"
        lines.append(f"| {row} | {value['state']} | {value.get('control_state', 'none' if not value['control_required'] else 'NOT_RUN')} | {value['adr_threshold']} ({value['adr_source']}) | {details} |")
    return "\n".join(lines) + "\n"


def write_report(record, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    result = verdict(record)
    (out / "verdict.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    data = markdown(result).encode("utf-8")
    (out / "verdict.md").write_bytes(data)
    (out / "verdict.md.sha256").write_text(hashlib.sha256(data).hexdigest() + "  verdict.md\n", encoding="ascii")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("raw", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = write_report(json.loads(args.raw.read_text(encoding="utf-8")), args.out)
    return {"INVALID": 2, "FAIL": 1, "NOT_RUN": 3}.get(result["state"], 0)


if __name__ == "__main__":
    raise SystemExit(main())
