#!/usr/bin/env python3
"""Report-only D3-6 paired T0 evidence validation; no Tier A scoring.

Consume native D3-5 telemetry and BV-07 summaries with independent process and
source receipts. Missing execution is NOT_RUN (3), unusable executed evidence
is UNUSABLE (2), usable diagnostic evidence is VALID (0).

--experiment FILE --out DIR writes comparison.json and comparison.md.
--require-report FILE --run-id ID --control CONTROL requires a current report
and revalidates its bound experiment, including on-disk traces/screenshots.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Optional, Sequence

import probe_summary as ps

# Pre-D3 source f2f19bcb580cc131615b3fa262a11d99e6abc70c plus #1453
# count/camera provenance only, published on bv-pre-d3-provenance.
PRE_D3 = "5a438363cd6939bca1041d738e2bfa10f2b024f2"
SCHEMA = "godotgs-shared-layers-t0-comparison/1"
EXPERIMENT_SCHEMA = "godotgs-shared-layers-t0-experiment/1"
EXIT_CODES = {"VALID": 0, "UNUSABLE": 2, "NOT_RUN": 3}

# Reporting policy: absent optional measurements still occupy a table slot.
METRICS = {
    "correctness": ("exit_code", "final_marker", "actual_route", "read_failures", "decode_failures",
                    "stale_generation_publications", "nonresident_drawn_nodes", "overflow_events", "drop_events"),
    "io": ("stored_bytes", "requests", "queue_latency_ns", "read_latency_ns", "throughput_bytes_s",
           "cancellations", "retained_completion_peak_bytes", "admission_waits", "reads_refused_busy"),
    "decode_pack": ("verify_ns", "decode_span_ns", "digest_ns", "decode_remainder_ns", "pack_ns",
                    "splats", "pages", "worker_captures", "main_thread_captures"),
    "cpu_memory": ("os_peak_rss_bytes", "active_bytes", "stored_bytes", "mailbox_bytes", "ready_bytes",
                   "shadow_bytes", "retained_bytes", "active_bytes_peak", "stored_bytes_peak",
                   "mailbox_bytes_peak", "ready_bytes_peak", "shadow_bytes_peak", "retained_bytes_peak",
                   "oversized_node_events"),
    "upload": ("bytes_per_frame", "total_bytes", "unique_bytes", "slices", "queue_residence_ns",
               "completion_latency_ns", "staging_allowance_bytes"),
    "rendering": ("first_complete_frame", "stop_convergence", "teleport_convergence", "drawn_nodes",
                  "drawn_splats", "drawn_fraction", "screenshots"),
    "performance": ("moving_frame_p95_ms", "moving_frame_p99_ms", "per_segment", "frame_coherent_gpu",
                    "device_ledger_vram_bytes", "external_vram_bytes", "overlap_sync_cuts"),
}
SETTINGS = ("camera_path_sha256", "camera_poses", "resolution", "codec", "sh_bands", "visible_splats",
            "drawn_splats", "timing_windows", "node_readback", "project_settings_readback")
FAILURES = ("stale_generation_publications", "nonresident_drawn_nodes", "overflow_events", "drop_events",
            "digest_verification_failures", "hlod_stream_failed_total", "hlod_stream_late_dropped_total")
BACKPRESSURE = ("reads_refused_busy", "admission_waits")
SATURATION = ("upload_hold_frames",)
# These are the shipped HLOD Config inputs, not new diagnostic budgets.
HLOD_CONFIG = {"configured_read_byte_cap": 8 * 1024 * 1024,
               "configured_max_jobs_in_flight": 16,
               "configured_upload_bytes_per_frame": 32 * 1024 * 1024,
               "normal_retained_byte_bound": 8 * 1024 * 1024}
HLOD_ONLY_FAILURES = ("nonresident_drawn_nodes", "hlod_stream_failed_total", "hlod_stream_late_dropped_total")
# These observations exist on immutable pre-D3 as well as the candidate.
# Aggregate work varies with scheduling/rereads; positive work and zero failures
# establish transport activity, not equality of rendered LOD demand.
MIGRATION_WORK = ("hlod_stream_payload_reads_total", "hlod_stream_payload_bytes_read_total",
                  "hlod_stream_uploaded_bytes_total", "hlod_stream_pages_resident")
MIGRATION_FAILURES = ("hlod_stream_payload_read_failures_total", "hlod_stream_failed_total",
                      "hlod_stream_late_dropped_total", "hlod_stream_nonresident_drawn")
# #1471 pressure variant: comparison-owned policy, never read from the experiment. The
# unconstrained T0 holds the whole tree, so its eviction/budget checks cannot fail; this
# variant is usable only with evidence that the HLOD route was actually under pressure.
VARIANTS = ("unconstrained", "pressure")
PRESSURE_ATLAS_SETTING = "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb"
PRESSURE_ATLAS_BAND = (0.20, 0.25)
PRESSURE_DRAWN_BUDGET_SPLATS = 1_000_000
PRESSURE_WORK = ("upload_completions", "upload_bytes", "read_completions", "pages_decoded")


def _dict(value):
    return value if isinstance(value, dict) else {}


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _count(value, *, signed=False):
    return _number(value) and value == int(value) and (signed or value >= 0)


def _value(value):
    if isinstance(value, dict):
        if value.get("measured") is False or value.get("available") is False:
            return None
        if "status" in value and value["status"] != ps.OK:
            return None
        return value.get("value")
    return value


def _chunk_hlod_inapplicable(value, route, key):
    """Only HLOD-owned observations can be inapplicable on the chunk route."""
    return (route == "chunk" and key in HLOD_ONLY_FAILURES and isinstance(value, dict)
            and value.get("measured") is False and value.get("applicable") is False
            and value.get("scope") == "chunk" and bool(value.get("reason")))


def _queue_escape(queue):
    """Full occupancy is bounded back-pressure; only an actual escape fails."""
    queue = _dict(queue)
    if _count(queue.get("bound_violations")) and queue["bound_violations"] > 0:
        return "native owner observed a bound violation"
    for key, bound in (("bytes", "byte_bound"),
                       ("peak_bytes", "peak_byte_bound" if "peak_byte_bound" in queue else "byte_bound"),
                       ("jobs", "job_bound"), ("peak_jobs", "job_bound")):
        if _count(queue.get(key)) and _count(queue.get(bound)) and queue[key] > queue[bound]:
            return f"{key} above its observed {bound}"
    return None


def _saturation_evidence(arm):
    """Observe bound escape or held work; ordinary admission waits are IO data."""
    arm = _dict(arm)
    payload = (_dict(_dict(arm.get("native")).get("payload_telemetry"))
               or _dict(_dict(arm.get("probe")).get("payload_telemetry")))
    observed = _dict(arm.get("observed"))
    counters = _dict(observed.get("saturation"))
    if "queue_observations" in payload:
        queues = ps.queue_observations_summary(payload["queue_observations"], route=payload.get("route"))
        if payload.get("route") != "hlod" or queues.get("status") != ps.OK:
            return False
        ready = _dict(queues.get("ready"))
        if (_count(payload.get("upload_hold_frames")) and payload["upload_hold_frames"] > 0
                and ready.get("peak_bytes", 0) > 0 and ready.get("peak_jobs", 0) > 0):
            return True
        return any(_queue_escape(queues.get(owner)) for owner in ("retained", "ready"))
    if any(_count(counters.get(key)) and counters[key] > 0 for key in SATURATION):
        return True
    return any(_queue_escape(queue) for queue in _dict(observed.get("queues")).values())


def _progress_summary(arm, route):
    observed = _dict(_dict(arm).get("observed"))
    settings = _dict(observed.get("settings"))
    windows = _dict(_dict(settings.get("timing_windows")).get("settle"))
    return ps.forward_progress_summary(observed.get("forward_progress"), route=route, windows=windows)


def _known(value, *, nested=False):
    if value is None:
        # Path segments legitimately contain null optional fields. The whole
        # required readback still has to exist and equal the independent plan.
        return nested
    if isinstance(value, dict):
        if value.get("measured") is False or value.get("available") is False:
            return False
        if value.get("status") in (ps.MISSING, ps.INVALID, "NOT_RUN"):
            return False
        return (nested or bool(value)) and all(_known(v, nested=True) for v in value.values())
    if isinstance(value, (list, tuple)):
        return (nested or bool(value)) and all(_known(v, nested=True) for v in value)
    return not isinstance(value, float) or math.isfinite(value)


def not_measured(reason):
    return {"measured": False, "value": None, "reason": reason}


def measurement(value, reason="not exported by this route or probe"):
    metadata = {}
    if isinstance(value, dict) and ("status" in value or "measured" in value or "available" in value):
        metadata = {key: value[key] for key in ("scope", "unit") if key in value}
        reason = value.get("reason", reason)
        value = _value(value)
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return {**not_measured(reason), **metadata}
    return {"measured": True, "value": value, **metadata}


def _sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _is_sha(value, length=64):
    return isinstance(value, str) and re.fullmatch(rf"[0-9a-f]{{{length}}}", value) is not None


def _digest_map(value):
    return (isinstance(value, dict) and bool(value)
            and all(isinstance(node, str) and bool(node) and _is_sha(digest) for node, digest in value.items()))


def _check_content(actual, reference, arm, problem):
    """Compare decoded demand to preparation, never to that run's own output."""
    if not _digest_map(reference):
        problem("content_reference_unproven", "independent preparation node digest map is missing or malformed", arm)
        return {"status": "NOT_MEASURED", "reason": "independent reference unavailable"}
    if isinstance(actual, dict) and "conflicting_runtime_digests" in actual.values():
        problem("content_mismatch", "same node produced conflicting decoded digests during runtime", arm)
        return {"status": "MISMATCH", "reason": "conflicting runtime observations"}
    if not _digest_map(actual):
        problem("content_unproven", "runtime decoded-node digest map is missing, empty or malformed", arm)
        return {"status": "NOT_MEASURED", "reason": "runtime content unavailable"}
    mismatches = [node for node, digest in actual.items() if reference.get(node) != digest]
    if mismatches:
        problem("content_mismatch", f"unknown node or decoded content differs from preparation: {', '.join(sorted(mismatches))}", arm)
    return {"status": "MISMATCH" if mismatches else "MATCHED", "nodes_checked": len(actual),
            "reference_nodes": len(reference), "mismatched_nodes": sorted(mismatches),
            "scope": "canonical decoded Gaussian and SH digest by route-local node identity; decoded nodes only"}


def _draw_assessment(counts, key, metric):
    base = [row[metric] for (_, checkpoint), row in counts[0].items()
            if checkpoint == key[1] and metric in row]
    candidate = [row.get(metric) for (_, checkpoint), row in counts[1].items() if checkpoint == key[1]]
    if (len(set(base)) > 1 and all(_count(value) and min(base) <= value <= max(base) for value in candidate)):
        return "inconclusive"
    return "mismatch"


def _admissible_draw_counts(rows):
    # Keep the original numeric count shape. Optional unmeasured node counts
    # must not enter a range as None, nor hide a one-sided metric difference.
    return {(row["pass_index"], row["checkpoint"]):
            {metric: row[metric] for metric in ("drawn_splats", "drawn_nodes") if _count(row.get(metric))}
            for row in rows if row.get("evidence_verdict") == "VALID"
            and _count(row.get("pass_index")) and isinstance(row.get("checkpoint"), str)
            and _count(row.get("drawn_splats")) and row["drawn_splats"] > 0}


def _checkpoint_reason(record, *, reference=False):
    verdict = record.get("evidence_verdict")
    if verdict == "VALID":
        return None
    if verdict in ("static_sort_cache_unsupported", "static_sort_cache_state_unavailable"):
        return verdict
    return "migration_reference_unsettled" if reference else "checkpoint_unsettled"


def _checkpoint_observations(arms, migration_arms, problem):
    """Recompute admissibility from receipts, never from a cached VALID token.

    Inadmissible observations remain visible but cannot establish a reference
    range or a mismatch. Process completion is independent of this evidence.
    """
    records, migration_rows = [], {name: [] for name in ("base", "candidate")}

    def capture(row, arm, route, cache):
        record = ps.checkpoint_evidence(row, route=route, static_sort_cache=cache)
        record["arm"] = arm
        records.append(record)
        reason = _checkpoint_reason(record, reference=arm == "migration_base")
        if reason:
            problem(reason, f"pass {record.get('pass_index')} checkpoint {record.get('checkpoint')}: "
                    f"{record.get('evidence_verdict')}", arm)
        for exclusion in record.get("evidence_reasons", []):
            if exclusion in ("static_sort_cache_unsupported", "static_sort_cache_state_unavailable"):
                problem(exclusion, f"checkpoint {record.get('checkpoint')}: actual chunk sort cache state", arm)
        return record

    for route in ("chunk", "hlod"):
        arm = _dict(arms.get(route))
        cache = _dict(arm.get("observed")).get("static_sort_cache")
        passes = _dict(arm.get("probe")).get("passes")
        for measured_pass in passes if isinstance(passes, list) else []:
            measured_pass = _dict(measured_pass)
            rows = measured_pass.get("checkpoints")
            if not isinstance(rows, list) or not rows:
                problem("checkpoint_unsettled", "no checkpoint evidence in measured pass", route)
                continue
            for row in rows:
                capture({**_dict(row), "pass_index": measured_pass.get("pass_index")}, route, route, cache)
    for name in ("base", "candidate"):
        arm = _dict(migration_arms.get(name))
        rows = _dict(arm.get("payload_behavior")).get("drawn_counts")
        for row in rows if isinstance(rows, list) else []:
            migration_rows[name].append(capture(_dict(row), "migration_" + name, "hlod", arm.get("static_sort_cache")))
    return ps.checkpoint_evidence_summary(records), migration_rows


def _trace_problem(path, expected=None):
    if not isinstance(path, str) or not Path(path).is_file():
        return "missing_trace", "trace file is absent"
    try:
        trace = ps.read_trace(Path(path))
        if trace.trailing_bytes or not trace.rows:
            return "invalid_trace", "empty trace or torn final row"
        frames, rows = trace.col("engine_frame"), trace.col("row")
        if rows != list(range(len(rows))) or any(b - a != 1 for a, b in zip(frames, frames[1:])):
            return "invalid_trace", "trace omits an engine frame or row"
        if isinstance(expected, dict):
            for key in ("rows_in_file", "rows_written", "frames_rendered"):
                if expected.get(key) != len(rows):
                    return "invalid_trace", f"{key} differs from the actual trace"
            if expected.get("status") != ps.OK:
                return "invalid_trace", "probe reported invalid trace coverage"
    except (OSError, ValueError, KeyError, ps.TraceError) as exc:
        return "invalid_trace", str(exc)
    return None


def _route_metrics(arm):
    arm = _dict(arm)
    probe, obs = _dict(arm.get("probe")), _dict(arm.get("observed"))
    telemetry = _dict(_dict(arm.get("native")).get("payload_telemetry")) or _dict(probe.get("payload_telemetry"))
    optional = _dict(arm.get("optional_measurements"))
    failures, runtime = _dict(obs.get("failures")), _dict(obs.get("runtime"))
    passes = probe.get("passes") if isinstance(probe.get("passes"), list) else []
    first = _dict(passes[0]) if passes else {}
    intervals = _dict(first.get("moving_frame_interval"))
    out = {area: {key: not_measured("not exported by this route or probe") for key in keys}
           for area, keys in METRICS.items()}

    def put(area, key, value, reason="not exported by this route or probe"):
        out[area][key] = measurement(value, reason)

    for key in ("exit_code", "final_marker"):
        put("correctness", key, obs.get(key))
    put("correctness", "actual_route", telemetry.get("route"))
    for key, source in (("read_failures", "read_failures"), ("decode_failures", "page_failures")):
        put("correctness", key, telemetry.get(source))
    for key in ("stale_generation_publications", "nonresident_drawn_nodes", "overflow_events", "drop_events"):
        put("correctness", key, failures.get(key))
    for key, source in (("stored_bytes", "stored_bytes_read"), ("requests", "read_requests"),
                        ("queue_latency_ns", "read_queue_nsec"), ("read_latency_ns", "read_latency_nsec")):
        put("io", key, telemetry.get(source))
    # A CPU sum is not wall-clock IO throughput.
    for key in ("throughput_bytes_s", "cancellations", "retained_completion_peak_bytes"):
        put("io", key, optional.get(key))
    for key in BACKPRESSURE:
        put("io", key, telemetry.get(key, _dict(obs.get("saturation")).get(key)))
    for key, source in (("verify_ns", "verify_nsec"), ("decode_span_ns", "decode_nsec"),
                        ("decode_remainder_ns", "decode_remainder_nsec"), ("pack_ns", "pack_nsec"),
                        ("splats", "splats_decoded"), ("pages", "pages_decoded"),
                        ("worker_captures", "captures_on_worker_threads"), ("main_thread_captures", "captures_on_main_thread")):
        put("decode_pack", key, telemetry.get(source))
    put("decode_pack", "digest_ns", telemetry.get("digest_nsec"),
        f"decoded digest {telemetry.get('digest_status', 'not observed')}; no measured duration")
    for key in METRICS["cpu_memory"]:
        put("cpu_memory", key, optional.get(key, telemetry.get(key)))
    queues = _dict(obs.get("queues"))
    for key, source in (("retained_bytes", "bytes"), ("retained_bytes_peak", "peak_bytes")):
        retained = _dict(queues.get("retained"))
        put("cpu_memory", key, {"status": ps.OK, "value": retained.get(source),
                                "scope": retained.get("owner", "comparison-observed retained payload owner")})
    for key, source in (("bytes_per_frame", "upload_bytes_this_frame"), ("total_bytes", "upload_bytes"),
                        ("slices", "upload_slices"), ("queue_residence_ns", "queue_residence_nsec"),
                        ("completion_latency_ns", "upload_completion_nsec")):
        put("upload", key, telemetry.get(source, optional.get(key)))
    for key in ("unique_bytes", "staging_allowance_bytes"):
        put("upload", key, optional.get(key))
    for key in METRICS["rendering"]:
        put("rendering", key, optional.get(key))
    put("rendering", "drawn_nodes", runtime.get("gpu_resident_drawn_nodes"))
    if "gpu_resident_drawn_splats" in runtime:
        put("rendering", "drawn_splats", runtime["gpu_resident_drawn_splats"])
    put("rendering", "screenshots", _dict(probe.get("screenshots")).get("files"))
    for q in (95, 99):
        put("performance", f"moving_frame_p{q}_ms", intervals.get(f"p{q}_time_weighted"))
    put("performance", "per_segment", first.get("segments"))
    put("performance", "overlap_sync_cuts", optional.get("overlap_sync_cuts"),
        "not_measured: sync-cut counter or per-cut CPU wall times unavailable")
    if not out["performance"]["overlap_sync_cuts"]["measured"]:
        out["performance"]["overlap_sync_cuts"]["status"] = "not_measured"
    gpu = _dict(first.get("moving_gpu"))
    if _dict(_dict(gpu.get("coherent_total_ms")).get("mean")).get("status") == ps.OK:
        put("performance", "frame_coherent_gpu", gpu)
    ledger = _dict(optional.get("device_vram_ledger"))
    if ledger.get("device_observed") is True and _number(ledger.get("device_bytes")) and ledger["device_bytes"] > 0:
        put("performance", "device_ledger_vram_bytes", ledger["device_bytes"])
    else:
        out["performance"]["device_ledger_vram_bytes"] = not_measured(
            "#1441: no observed positive device ledger; gpu_memory_usage_mb=0 is not a measurement")
    external = optional.get("external_vram_bytes")
    if _number(_value(external)) and _value(external) > 0:
        put("performance", "external_vram_bytes", external)
    else:
        out["performance"]["external_vram_bytes"] = not_measured("external VRAM was not independently sampled")
    return out


def _artifact_digests(arms, migration_arms):
    """Bind the report to artifact bytes, not just a previously OK summary."""
    result = {}
    for route, arm in arms.items():
        arm = _dict(arm)
        trace = _dict(arm.get("trace")).get("file")
        entry = {"trace": _sha256(trace) if isinstance(trace, str) and Path(trace).is_file() else None,
                 "screenshots": {}}
        screenshots = _dict(_dict(arm.get("probe")).get("screenshots"))
        root = Path(arm.get("artifact_root", ".")).resolve()
        for file in screenshots.get("files") or []:
            if not isinstance(file, str):
                continue
            path = (root / screenshots.get("dir", "screenshots") / file).resolve()
            if path.is_relative_to(root) and path.is_file():
                entry["screenshots"][file] = _sha256(path)
        result[route] = entry
    result["migration"] = {}
    for name, arm in migration_arms.items():
        trace = _dict(arm).get("trace_file")
        result["migration"][name] = _sha256(trace) if isinstance(trace, str) and Path(trace).is_file() else None
    return result


def _pressure_assessment(exp, arms, problem):
    """#1471: require evidence that the pressure variant put the HLOD route under pressure.

    Every requirement has its own reason code, and "not measured" is a different code from
    "measured zero". The chunk arm is the reference: it gets the same settings, no claim of an
    equivalent constraint, and no pressure requirement. Nothing here relaxes another check."""
    request = exp.get("variant")
    low, high = PRESSURE_ATLAS_BAND
    valid_request = (isinstance(request, dict) and request.get("name") == "pressure"
                     and request.get("atlas_setting") == PRESSURE_ATLAS_SETTING
                     and _count(request.get("atlas_override_mb")) and request["atlas_override_mb"] > 0
                     and _count(request.get("tree_pages")) and request["tree_pages"] > 0
                     and request.get("atlas_fraction_band") == [low, high]
                     and request.get("drawn_budget_splats") == PRESSURE_DRAWN_BUDGET_SPLATS)
    if not valid_request:
        problem("pressure_variant_invalid", "pressure request is malformed or differs from the comparison policy "
                f"({low:.0%}-{high:.0%} of the tree pages, {PRESSURE_DRAWN_BUDGET_SPLATS} drawn splats)")
        return {"hlod": {"requirements": [{"name": "request", "satisfied": False, "observed": None}]}, "chunk": {}}
    cap, tree_pages = request["atlas_override_mb"], request["tree_pages"]
    not_applied = []
    for route in ("chunk", "hlod"):
        arm = _dict(arms.get(route))
        for label, settings in (("requested", _dict(arm.get("requested"))),
                                ("process readback", _dict(_dict(arm.get("observed")).get("settings")))):
            if (_dict(settings.get("project_settings_readback")).get(PRESSURE_ATLAS_SETTING) != cap
                    or _dict(settings.get("node_readback")).get("max_splat_count") != PRESSURE_DRAWN_BUDGET_SPLATS
                    or settings.get("visible_splats") != PRESSURE_DRAWN_BUDGET_SPLATS
                    or settings.get("drawn_splats") != PRESSURE_DRAWN_BUDGET_SPLATS):
                not_applied.append(f"{route} {label}")
                problem("pressure_not_applied", f"{label} lacks the {cap} MiB atlas cap or the "
                        f"{PRESSURE_DRAWN_BUDGET_SPLATS} drawn budget", route)
    hlod = _dict(arms.get("hlod"))
    observed = hlod.get("pressure")
    requirements = [{"name": "settings applied", "satisfied": not not_applied,
                     "observed": not_applied or f"{cap} MiB atlas cap and {PRESSURE_DRAWN_BUDGET_SPLATS} drawn budget, "
                                                "requested and read back on both routes"}]

    def require(name, satisfied, value, code, detail):
        requirements.append({"name": name, "satisfied": bool(satisfied), "observed": copy.deepcopy(value)})
        if not satisfied:
            problem(code, detail, "hlod")

    chunk = _dict(_dict(arms.get("chunk")).get("pressure"))
    chunk_report = {"role": chunk.get("role", "unconstrained_reference"), "constraint": chunk.get("constraint", "not_applied"),
                    "reason": chunk.get("reason", _dict(request.get("chunk_arm")).get("reason")),
                    "evicted_chunks": copy.deepcopy(chunk.get("evicted_chunks", not_measured("chunk arm carries no pressure record"))),
                    "settles": copy.deepcopy(chunk.get("settles", []))}
    if not isinstance(observed, dict) or observed.get("route") != "hlod":
        require("pressure record", False, None, "pressure_evidence_missing",
                "the HLOD arm carries no pressure observations")
        return {"hlod": {"requirements": requirements}, "chunk": chunk_report}
    capacity = observed.get("atlas_pages_capacity")
    fraction = capacity / tree_pages if _count(capacity) else None
    if fraction is None:
        require("atlas cap", False, capacity, "pressure_atlas_cap_unobserved", "native atlas page capacity not measured")
    else:
        require("atlas cap", low <= fraction <= high, capacity, "pressure_atlas_cap_out_of_band",
                f"native atlas capacity {capacity} of {tree_pages} tree pages is {fraction:.1%}, outside {low:.0%}-{high:.0%}")
    for name, key, absent, zero in (("evictions", "evicted_total", "pressure_evictions_unmeasured", "pressure_no_evictions"),
                                    ("budget-bound frames", "budget_bound_frames", "pressure_budget_bound_unmeasured",
                                     "pressure_no_budget_bound_frames")):
        value = observed.get(key)
        if not _count(value):
            require(name, False, value, absent, f"{key} not measured")
        else:
            require(name, value > 0, value, zero, f"{key} is 0: the route was never under this pressure")
    teleports = [row for row in observed.get("settles") if isinstance(row, dict) and row.get("kind") == "teleport"] \
        if isinstance(observed.get("settles"), list) else []
    measured = bool(teleports) and all(
        _count(row.get("frames")) and row["frames"] > 0 and _number(row.get("settle_ms")) and row["settle_ms"] >= 0
        and isinstance(row.get("streaming_work"), dict) and row["streaming_work"].get("measured") is not False
        and all(_count(row["streaming_work"].get(key)) for key in PRESSURE_WORK)
        for row in teleports if row.get("settled") is True)
    unsettled = [row.get("checkpoint") for row in teleports if row.get("settled") is not True]
    working = [row for row in teleports if row.get("settled") is True and measured
               and any(row["streaming_work"][key] > 0 for key in PRESSURE_WORK)]
    summary = [{key: row.get(key) for key in ("checkpoint", "pass_index", "settled", "frames", "settle_ms")}
               for row in teleports]
    if unsettled:
        require("teleport settle", False, summary, "pressure_teleport_not_converged",
                f"teleport checkpoint(s) did not settle: {', '.join(str(name) for name in unsettled)}")
    elif not measured:
        require("teleport settle", False, summary, "pressure_teleport_settle_unmeasured",
                "no teleport settle, or one without frames, time or native streaming-work deltas")
    else:
        require("teleport settle", bool(working), summary, "pressure_teleport_settle_trivial",
                "no teleport settle window read, decoded or uploaded anything: nothing had to stream")
    queues = _dict(_dict(hlod.get("observed")).get("queues"))
    violations = {owner: _dict(queues.get(owner)).get("bound_violations") for owner in ("retained", "ready")}
    require("bound violations measured", all(_count(value) for value in violations.values()), violations,
            "pressure_bound_unmeasured", "native queue bound_violations not measured")
    telemetry = _dict(_dict(hlod.get("native")).get("payload_telemetry")) or _dict(_dict(hlod.get("probe")).get("payload_telemetry"))
    failures = {key: telemetry.get(key) for key in ("read_failures", "page_failures")}
    require("read/decode failures measured", all(_count(value) for value in failures.values()), failures,
            "pressure_failures_unmeasured", "native read/decode failure counters not measured")
    return {"hlod": {**{key: copy.deepcopy(observed.get(key)) for key in (
                        "constraint", "atlas_pages_capacity", "pages_resident_final", "evicted_total",
                        "budget_bound_frames", "payload_reads_total", "rereads", "rereads_scope", "settles")},
                     "atlas_fraction_observed": fraction, "bound_violations": violations,
                     "read_decode_failures": failures, "requirements": requirements},
            "chunk": chunk_report}


def compare(experiment: dict) -> dict:
    """Validate required evidence; retain all reasons and optional measurement gaps."""
    exp = _dict(experiment)
    reasons, absent_execution = [], False

    def problem(code, detail, arm=None, *, not_run=False):
        nonlocal absent_execution
        absent_execution |= not_run
        if not any(r["code"] == code and r.get("arm") == arm for r in reasons):
            reasons.append({"code": code, "detail": detail, **({"arm": arm} if arm else {})})

    prepared, arms = _dict(exp.get("prepared")), _dict(exp.get("arms"))
    checkpoint_evidence, migration_rows = _checkpoint_observations(
        arms, _dict(_dict(exp.get("migration")).get("arms")), problem)
    references = _dict(prepared.get("content_references"))
    content_identity = {}
    if exp.get("schema") != EXPERIMENT_SCHEMA or not exp.get("run_id"):
        problem("experiment_invalid", "experiment schema or run identity is absent")
    if prepared.get("tier") != "T0" or prepared.get("splat_count") != 4_194_304 or prepared.get("extent_m") != [512, 512]:
        problem("wrong_t0", "required T0 is 4,194,304 splats over 512 by 512 m")
    w13 = _dict(prepared.get("w13"))
    if (w13.get("available") is not True or w13.get("passed") is not True
            or not _count(w13.get("node_count")) or w13.get("node_count", 0) <= 0
            or w13.get("nodes_verified") != w13.get("node_count")):
        problem("w13_unproven", "producer W13 reopen and complete decoded-node verification are required")
    for key in ("input_file_sha256", "tree_file_sha256"):
        if not _is_sha(prepared.get(key)):
            problem("provenance_mismatch", f"prepared {key} is absent or invalid")
    for key in ("generator_identity", "codec", "tree_identity"):
        if not _known(prepared.get(key)) or not isinstance(prepared.get(key), dict):
            problem("provenance_mismatch", f"prepared {key} is unavailable")
    if _dict(prepared.get("codec")).get("encoding") != "gsq":
        problem("provenance_mismatch", "both prepared inputs must use GSQ")
    for route in ("chunk", "hlod"):
        arm = _dict(arms.get(route))
        if not arm or arm.get("execution") != "RAN":
            problem("missing_arm", "required arm did not execute", route, not_run=True)
            continue
        content_identity[route] = _check_content(arm.get("content_digests"), references.get(route), route, problem)
        raw_content = _dict(_dict(arm.get("native")).get("payload_telemetry")).get("content_digests")
        if raw_content is not None and raw_content != arm.get("content_digests"):
            problem("content_mismatch", "comparison content differs from native producer telemetry", route)
        source = _dict(arm.get("source"))
        for key in ("input_file_sha256", "generator_identity", "codec", "tree_file_sha256", "tree_identity"):
            if not _known(source.get(key)) or source.get(key) != prepared.get(key):
                problem("provenance_mismatch", f"{key} differs from the one prepared T0", route)
        loaded = prepared.get("input_file_sha256" if route == "chunk" else "tree_file_sha256")
        if source.get("loaded_file_sha256") != loaded:
            problem("provenance_mismatch", "actual loaded file is not the prepared route input", route)
        observed = _dict(arm.get("observed"))
        if not observed:
            problem("missing_observation", "no independent process observations", route)
        requested, settings = _dict(arm.get("requested")), _dict(observed.get("settings"))
        for key in sorted(set(SETTINGS) | set(requested)):
            if not _known(settings.get(key)) or not _known(requested.get(key)) or settings.get(key) != requested.get(key):
                problem("settings_mismatch", f"missing or different process readback: {key}", route)
        if _dict(observed.get("camera_application")).get("matches") is not True:
            problem("settings_mismatch", "actual trace camera poses do not prove application of the shared path", route)
        other = _dict(_dict(arms.get("hlod" if route == "chunk" else "chunk")).get("observed"))
        if _dict(other.get("settings")) and settings != other["settings"]:
            problem("settings_mismatch", "process settings differ between routes", route)
        if type(observed.get("exit_code")) is not int or observed["exit_code"] != 0 or observed.get("final_marker") is not True:
            problem("process_failure", "nonzero process exit or missing successful final marker", route)
        trace = _dict(arm.get("trace"))
        trace_issue = _trace_problem(trace.get("file"), trace.get("integrity"))
        if trace_issue:
            problem(*trace_issue, route)
        probe = _dict(arm.get("probe"))
        passes = probe.get("passes")
        if not isinstance(passes, list) or not passes:
            problem("missing_metric", "no measured path passes", route)
        else:
            for record in passes:
                settings_measurement = _dict(_dict(record).get("settings_measured"))
                if settings_measurement.get("status") != ps.OK or settings_measurement.get("settings_drifted") is not False:
                    problem("settings_mismatch", "settings missing or drifted during measured frames", route)
                intervals = _dict(_dict(record).get("moving_frame_interval"))
                if any(_dict(intervals.get(f"p{q}_time_weighted")).get("status") != ps.OK for q in (95, 99)):
                    problem("missing_metric", "time-weighted moving frame intervals are unavailable", route)
        summary_telemetry = _dict(probe.get("payload_telemetry"))
        telemetry = _dict(_dict(arm.get("native")).get("payload_telemetry")) or summary_telemetry
        if not summary_telemetry:
            problem("missing_telemetry", "route telemetry is absent", route)
        else:
            if telemetry.get("route") != route:
                problem("wrong_route", "actual process telemetry reports another route", route)
            generation = observed.get("source_generation")
            if (not _count(generation, signed=True) or generation == 0
                    or telemetry.get("source_generation") != generation or telemetry.get("collection_epoch") != generation):
                problem("stale_generation", "epoch/source generation differs from the independently queried source", route)
            for key in ("read_failures", "page_failures", "captures_failed"):
                value = telemetry.get(key)
                if value is not None and not _count(value):
                    problem("invalid_metric", f"invalid counter: {key}", route)
                elif _count(value) and value > 0:
                    problem("payload_failure", f"producer reported {key}", route)
            if telemetry.get("last_failure_reason"):
                problem("payload_failure", str(telemetry["last_failure_reason"]), route)
            for key in ("stored_bytes_read", "read_requests", "pages_decoded", "splats_decoded", "captures_on_worker_threads"):
                if _number(telemetry.get(key)) and telemetry[key] == 0:
                    problem("no_payload_work", f"no measured {key}", route)
            native = ps.route_telemetry_summary(telemetry, route=route, source_generation=generation)
            if native.get("status") == ps.MISSING:
                problem("missing_metric", native.get("reason", "missing telemetry measurement"), route)
            elif native.get("status") != ps.OK:
                problem("invalid_metric", native.get("reason", "invalid telemetry measurement"), route)
        runtime = _dict(observed.get("runtime"))
        if route == "hlod" and (runtime.get("hlod_stream_paged") is not True
                                or runtime.get("hlod_stream_page_store_active") is not True
                                or runtime.get("resident_materialized_fallback") is not False):
            problem("resident_fallback", "paged HLOD page-store streaming was not observed", route)
        drawn = runtime.get("gpu_resident_drawn_nodes")
        drawn_splats = runtime.get("gpu_resident_drawn_splats")
        chunk_draw_proof = route == "chunk" and _count(drawn_splats) and drawn_splats > 0
        if not chunk_draw_proof and (not _count(drawn) or drawn <= 0):
            problem("no_gpu_residency", "no GPU-resident drawn data was observed", route)
        for key in FAILURES:
            value = _dict(observed.get("failures")).get(key)
            if _chunk_hlod_inapplicable(value, route, key):
                continue
            if not _count(value):
                problem("missing_metric", f"required failure observation not measured: {key}", route)
            elif value != 0:
                problem("correctness_failure", f"nonzero {key}", route)
        native_queues = telemetry.get("queue_observations")
        scoped_queues = ps.queue_observations_summary(native_queues, route=route) if native_queues is not None else None
        if scoped_queues is not None and scoped_queues.get("status") != ps.OK:
            problem("missing_metric", scoped_queues.get("reason", "invalid native queue observations"), route)
        if scoped_queues is not None:
            for key in ("stale_generation_publications", "digest_verification_failures"):
                if telemetry.get(key) != _dict(observed.get("failures")).get(key):
                    problem("invalid_metric", f"{key} disagrees with native failure observation", route)
            if route == "hlod":
                samples = _dict(arm.get("native")).get("shared_layers_settings_samples")
                try:
                    actual_trace = ps.read_trace(Path(trace["file"]))
                    for record in passes if isinstance(passes, list) else []:
                        measured = ps.shared_layers_settings_summary(samples, ps.measured_rows(actual_trace, record["pass_index"]))
                        if measured.get("status") != ps.OK:
                            problem("settings_mismatch", measured.get("reason", "no per-row HLOD settings proof"), route)
                        else:
                            for key, value in _dict(measured.get("readback")).items():
                                if settings.get(key) != value:
                                    problem("settings_mismatch", f"per-row HLOD {key} differs from initial readback", route)
                except (OSError, ValueError, KeyError, ps.TraceError) as exc:
                    problem("settings_mismatch", f"cannot validate HLOD settings rows: {exc}", route)
                for key, shipped in HLOD_CONFIG.items():
                    if telemetry.get(key) != shipped:
                        problem("settings_mismatch", f"native HLOD {key} differs from its shipped Config default", route)
                oversized, live = telemetry.get("oversized_reservation_bytes"), telemetry.get("live_reservations")
                if not _count(oversized) or not _count(live):
                    problem("missing_metric", "native oversized reservation ownership is unavailable", route)
                else:
                    if oversized != 0 and live != 1:
                        problem("queue_saturation", "oversized reservation is not the sole live owner", route)
                    effective = max(HLOD_CONFIG["normal_retained_byte_bound"], oversized)
                    for owner in ("retained", "ready"):
                        if _dict(scoped_queues.get(owner)).get("byte_bound") != effective:
                            problem("settings_mismatch", f"{owner} bound is neither shipped admission nor its single-node exception", route)
        for key in BACKPRESSURE + SATURATION:
            value = _dict(observed.get("saturation")).get(key)
            # Upload holds are a HLOD-only test seam. Chunk has no such owner;
            # its real service/ready-window bounds remain mandatory below.
            if (key == "upload_hold_frames" and route == "chunk" and isinstance(value, dict)
                    and value.get("measured") is False and value.get("applicable") is False
                    and value.get("scope") == "chunk" and value.get("reason")):
                continue
            if not _count(value):
                problem("missing_metric", f"required saturation observation not measured: {key}", route)
            elif scoped_queues is not None and telemetry.get(key) != value:
                problem("invalid_metric", f"{key} disagrees with native queue observation", route)
            elif key in SATURATION and value != 0:
                problem("queue_saturation", f"nonzero {key}", route)
        for owner in ("retained", "ready"):
            queue = _dict(_dict(observed.get("queues")).get(owner))
            native_queue = _dict(_dict(scoped_queues).get(owner))
            if scoped_queues is not None and queue != native_queue:
                problem("invalid_metric", f"{owner} queue disagrees with native owner observation", route)
            for key in ("bytes", "peak_bytes", "byte_bound", "jobs", "peak_jobs", "job_bound"):
                if (scoped_queues is not None and scoped_queues.get("status") == ps.OK
                        and key.endswith("bound") and key in native_queue and native_queue[key] is None):
                    continue  # No independent bound at this owner; its real bound is checked below.
                if not _count(queue.get(key)):
                    problem("missing_metric", f"{owner} {key} not measured", route)
            escape = _queue_escape(queue)
            if escape:
                problem("queue_saturation", f"{owner} {escape}", route)
        if "forward_progress" in observed:
            if (_dict(arm.get("native")).get("forward_progress") is not None
                    and arm["native"]["forward_progress"] != observed["forward_progress"]):
                problem("invalid_metric", "forward-progress records disagree with native probe receipts", route)
            progress = _progress_summary(arm, route)
            if progress.get("status") == ps.INVALID:
                problem("invalid_metric", progress.get("reason", "invalid forward-progress observations"), route)
            for window in progress.get("windows", []):
                if window.get("no_forward_progress") is True:
                    problem("queue_saturation", "demand outstanding without progress for the existing settle deadline", route)
                elif window.get("deadline_missed") is True and window.get("measured") is not True:
                    problem("missing_metric", "deadline expired without complete native forward-progress observations", route)
        screenshots = _dict(probe.get("screenshots"))
        files = screenshots.get("files")
        if screenshots.get("status") != ps.OK or not isinstance(files, list) or not files:
            problem("missing_screenshots", "synthetic checkpoint/movement screenshots are absent or failed", route)
        else:
            root = Path(arm.get("artifact_root", ".")).resolve()
            for file in files:
                if not isinstance(file, str):
                    problem("missing_screenshots", "invalid screenshot path", route)
                    continue
                path = (root / screenshots.get("dir", "screenshots") / file).resolve()
                if not path.is_relative_to(root) or not path.is_file():
                    problem("missing_screenshots", "claimed synthetic screenshot does not exist under arm output", route)
        cache = _dict(arm.get("cache"))
        if cache.get("label") != "cold" or cache.get("independently_prepared") is not True or cache.get("repeat_label") != "repeat":
            problem("cache_unproven", "independent cold preparation and repeat labels are required", route)
    migration = _dict(exp.get("migration"))
    migration_arms = _dict(migration.get("arms"))
    if not migration or any(not _dict(migration_arms.get(name)) for name in ("base", "candidate")):
        problem("missing_migration", "both immutable pre-D3 and candidate migration executions are required", not_run=True)
    else:
        if migration.get("base_commit") != PRE_D3 or not _is_sha(migration.get("candidate_commit"), 40):
            problem("migration_identity_mismatch", "migration binary commit identities are not immutable")
        for name in ("base", "candidate"):
            arm = _dict(migration_arms[name])
            if arm.get("execution") != "RAN":
                problem("migration_unproven", "migration arm did not execute", name, not_run=True)
            elif arm.get("exit_code") != 0 or arm.get("final_marker") is not True or _trace_problem(arm.get("trace_file")):
                problem("migration_unproven", "migration process failed or has no usable trace", name)
            expected_commit = PRE_D3 if name == "base" else migration.get("candidate_commit")
            if arm.get("binary_commit") != expected_commit or not _is_sha(arm.get("binary_sha256")):
                problem("migration_identity_mismatch", "binary identity is missing or differs from required commit", name)
            if (arm.get("input_file_sha256") != prepared.get("tree_file_sha256")
                    or arm.get("settings_readback") != _dict(_dict(arms.get("hlod")).get("observed")).get("settings")):
                problem("migration_identity_mismatch", "migration needs the same V4 bytes and process settings", name)
            if _dict(arm.get("camera_application")).get("matches") is not True:
                problem("migration_unproven", "migration camera application is unproven", name)
        if migration_arms["base"].get("binary_sha256") == migration_arms["candidate"].get("binary_sha256"):
            problem("migration_identity_mismatch", "pre-D3 and candidate binary digests are identical")
        # Pre-D3 shares native payload/route behavior, not D3-5 decode stages.
        behaviors = [_dict(_dict(migration_arms[n]).get("payload_behavior")) for n in ("base", "candidate")]
        drawn_counts = []
        for name, behavior in zip(("base", "candidate"), behaviors):
            rows = behavior.get("drawn_counts")
            if behavior.get("route") != "hlod" or not isinstance(rows, list) or not rows:
                problem("migration_unproven", "native HLOD route/drawn behavior is unavailable", name)
                rows = []
            counts = {}
            for row in rows:
                row = _dict(row)
                key = (row.get("pass_index"), row.get("checkpoint"))
                splats = row.get("drawn_splats")
                if (not _count(key[0]) or not isinstance(key[1], str) or not key[1]
                        or not _count(splats) or splats <= 0 or key in counts):
                    problem("migration_unproven", "invalid, repeated or empty checkpoint draw observation", name)
                    continue
                counts[key] = {"drawn_splats": splats}
                if _count(row.get("drawn_nodes")):
                    counts[key]["drawn_nodes"] = row["drawn_nodes"]
            settings = _dict(migration_arms[name].get("settings_readback"))
            passes = _dict(settings.get("timing_windows")).get("moving_passes")
            segments = settings.get("camera_poses")
            segments = segments if isinstance(segments, list) else []
            checkpoints = {s["checkpoint"] for s in segments if isinstance(s, dict)
                           and isinstance(s.get("checkpoint"), str) and s["checkpoint"]}
            if not _count(passes) or passes != 2 or not checkpoints:
                problem("migration_unproven", "planned cold/repeat checkpoint coverage is missing", name)
            elif set(counts) != {(index, cp) for index in range(int(passes)) for cp in checkpoints}:
                problem("migration_unproven", "migration lacks a planned cold/repeat checkpoint", name)
            drawn_counts.append(counts)
            if behavior.get("loaded_file_sha256") != prepared.get("tree_file_sha256"):
                problem("migration_identity_mismatch", "migration process payload digest differs", name)
            native = _dict(behavior.get("native_payload"))
            for key in MIGRATION_WORK:
                if not _count(native.get(key)) or native[key] <= 0:
                    problem("migration_unproven", f"missing or nonpositive common payload work: {key}", name)
            for key in MIGRATION_FAILURES:
                if not _count(native.get(key)):
                    problem("migration_unproven", f"missing common payload failure observation: {key}", name)
                elif native[key] != 0:
                    problem("payload_failure", f"migration nonzero {key}", name)
            capacity, resident = native.get("hlod_stream_pages_capacity"), native.get("hlod_stream_pages_resident")
            if not _count(capacity) or capacity <= 0:
                problem("migration_unproven", "common resident-page capacity is missing", name)
            elif _count(resident) and resident > capacity:
                problem("payload_failure", "migration resident atlas pages exceed capacity", name)
            world = _dict(behavior.get("world_payload"))
            if (world.get("has_hlod_paged_source") is not True or world.get("mode") != "hlod_paged_v4"
                    or world.get("splat_count") != prepared.get("splat_count")
                    or world.get("sh_degree") != _dict(prepared.get("codec")).get("sh_bands")
                    or any(_dict(behavior.get("route_evidence")).get(key) is not True
                           for key in ("hlod_streaming", "hlod_active", "hlod_stream_paged"))):
                problem("migration_unproven", "common process V4 payload shape/route is unproven", name)
        if behaviors[0].get("world_payload") != behaviors[1].get("world_payload"):
            problem("migration_identity_mismatch", "migration V4 payload shape differs")
        if _dict(behaviors[0].get("native_payload")).get("hlod_stream_pages_capacity") != _dict(
                behaviors[1].get("native_payload")).get("hlod_stream_pages_capacity"):
            problem("migration_identity_mismatch", "migration resident-page capacity differs")
        if drawn_counts[0].keys() != drawn_counts[1].keys():
            problem("migration_unproven", "migration checkpoint coverage differs")
    migration_observations = _migration_observations(migration_arms, migration_rows)
    usable_counts = [_admissible_draw_counts(migration_rows[name]) for name in ("base", "candidate")]
    # Preserve #1448's comparison, including one-sided optional measurements.
    # Only admissible samples participate in either side of its range test.
    for key in sorted(usable_counts[0].keys() & usable_counts[1].keys()):
        for metric in sorted(usable_counts[0][key].keys() | usable_counts[1][key].keys()):
            values = [counts[key].get(metric) for counts in usable_counts]
            if values[0] != values[1] and _draw_assessment(usable_counts, key, metric) == "mismatch":
                problem("migration_identity_mismatch", f"checkpoint {key[1]} {metric} differs outside observed base range")
    candidate_content = _check_content(_dict(_dict(migration_arms.get("candidate")).get("payload_behavior")).get(
        "content_digests"), references.get("hlod"), "candidate", problem)
    migration_identity = {"status": "NOT_MEASURED",
        "reason": "immutable pre-D3 V4 decoded streaming payload is native-private; existing script APIs expose no decoded nodes",
        "base": {"status": "NOT_MEASURED"}, "candidate": candidate_content,
        "scope": "candidate decoded nodes checked against independent W13 reference; no base/candidate decoded-content equality claim"}
    content_identity["cross_arm"] = {"status": "NOT_COMPARABLE",
        "reason": "HLOD repartitions/reorders and re-quantizes node-relative source content; chunk pages and HLOD nodes have no exact digest correspondence",
        "scope": "each route's decoded demand matches its own independent preparation reference"}
    # #1471: only an experiment that records a variant is assessed; the unconstrained report is unchanged.
    pressure = _pressure_assessment(exp, arms, problem) if "variant" in exp else None
    control = exp.get("control", "none")
    receipt = _dict(exp.get("control_receipt"))
    control_verdict = "NOT_REQUESTED"
    if control not in ("none", "corrupt", "saturate", "skip_compare"):
        problem("control_not_observed", "unknown negative control")
    if control in ("corrupt", "saturate"):
        control_verdict = "NOT_DETECTED"
        if receipt.get("status") == "NOT_RUN":
            control_verdict = "NOT_RUN"
            problem("control_not_run", receipt.get("reason", "forcing hook unavailable"), not_run=True)
        else:
            control_arm = _dict(receipt.get("arm"))
            control_observed = _dict(control_arm.get("observed"))
            control_trace = _dict(control_arm.get("trace"))
            usable = (receipt.get("status") == "FORCED" and control_arm.get("execution") == "RAN"
                      and type(control_observed.get("exit_code")) is int and control_observed["exit_code"] == 0
                      and control_observed.get("final_marker") is True
                      and _trace_problem(control_trace.get("file"), control_trace.get("integrity")) is None)
            control_telemetry = (_dict(_dict(control_arm.get("native")).get("payload_telemetry"))
                                 or _dict(_dict(control_arm.get("probe")).get("payload_telemetry")))
            corruption_source = (
                _is_sha(receipt.get("corrupt_sha256")) and receipt.get("source_sha256") == prepared.get("tree_file_sha256")
                and receipt["corrupt_sha256"] != receipt["source_sha256"]
                and _dict(control_arm.get("source")).get("loaded_file_sha256") == receipt["corrupt_sha256"]
                and control_telemetry.get("route") == "hlod" and _count(control_telemetry.get("read_requests"))
                and control_telemetry["read_requests"] > 0)
            if usable and control == "corrupt" and corruption_source and any(_count(control_telemetry.get(k)) and control_telemetry[k] > 0
                                           for k in ("page_failures", "captures_failed", "digest_verification_failures")):
                problem("payload_failure", "corrupted V4 control produced a native payload verification failure", "control")
                control_verdict = "DETECTED"
            elif (usable and control == "saturate" and control_telemetry.get("route") == "hlod"
                  and _dict(control_arm.get("source")).get("loaded_file_sha256") == prepared.get("tree_file_sha256")
                  and ps.queue_observations_summary(control_telemetry.get("queue_observations"), route="hlod").get("status") == ps.OK
                  and _saturation_evidence(control_arm)):
                problem("queue_saturation", "separate HLOD control observed bounded-pipeline pressure", "control")
                control_verdict = "DETECTED"
            else:
                problem("control_not_observed", f"{control} lacks a successful control process/trace or attributable observed failure", "control")
    if control == "skip_compare":
        problem("comparison_skipped", "comparison invocation is deliberately removed", not_run=True)
    if exp.get("orchestration_problem") or exp.get("orchestration_error"):
        problem("orchestration_failed", str(exp.get("orchestration_problem", exp.get("orchestration_error"))))
    unexpected = [r for r in reasons if not (control_verdict == "DETECTED" and r.get("arm") == "control"
                  and r["code"] == ("payload_failure" if control == "corrupt" else "queue_saturation"))]
    verdict = "NOT_RUN" if absent_execution else "UNUSABLE" if unexpected else "VALID"
    return {"schema": SCHEMA, "run_id": exp.get("run_id"), "control": control, "report_only": True,
            **({} if pressure is None else {"variant": copy.deepcopy(exp["variant"]), "pressure": pressure}),
            "verdict": verdict, "exit_code": EXIT_CODES[verdict], "reasons": reasons,
            "prepared": copy.deepcopy(prepared), "migration": {**copy.deepcopy(migration), "content_identity": migration_identity},
            "content_identity": content_identity, "control_verdict": control_verdict,
            "migration_observations": migration_observations,
            "checkpoint_evidence": checkpoint_evidence,
            "control_receipt": copy.deepcopy(receipt),
            "artifact_digests": _artifact_digests({**arms, **({"control": receipt["arm"]} if "arm" in receipt else {})}, migration_arms),
            "passes": {route: copy.deepcopy(_dict(_dict(arms.get(route)).get("probe")).get("passes", []))
                       for route in ("chunk", "hlod")},
            "forward_progress": {route: _progress_summary(arms.get(route), route) for route in ("chunk", "hlod")},
            "measurement_scopes": {"decode_pack": "worker_cpu_sum; not zstd wall time",
                                   "io_latency": "reader_wall_sum and read queue sum",
                                   "frame_intervals": "time-weighted nearest rank, cold and repeat reported separately",
                                   "frame_time": FRAME_TIME_SCOPE},
            "frame_time": _frame_time(arms, migration_arms),
            "overlap_drop_report": _overlap_drop_report(arms),
            "metrics": {route: _route_metrics(arms.get(route)) for route in ("chunk", "hlod")}}


def _migration_observations(arms, checkpoint_rows):
    behaviors = {name: _dict(_dict(arms.get(name)).get("payload_behavior")) for name in ("base", "candidate")}
    native = {name: _dict(behavior.get("native_payload")) for name, behavior in behaviors.items()}
    metrics = {}
    for key in sorted(set(native["base"]) | set(native["candidate"])):
        values = [native[name].get(key) for name in ("base", "candidate")]
        metrics[key] = {name: measurement(native[name].get(key)) for name in native}
        metrics[key]["candidate_minus_base"] = (measurement(values[1] - values[0])
            if all(_number(v) for v in values) else not_measured("no two numeric common observations"))
    counts, admissible, changes = {}, {}, []
    for name, rows in checkpoint_rows.items():
        counts[name] = {(row["pass_index"], row["checkpoint"]): row for row in rows if isinstance(row, dict)
                        and _count(row.get("pass_index")) and isinstance(row.get("checkpoint"), str)}
        admissible[name] = _admissible_draw_counts(rows)
        for checkpoint in sorted({key[1] for key in admissible[name]}):
            for metric in ("drawn_splats", "drawn_nodes"):
                samples = [{"pass_index": index, "value": row[metric]} for (index, cp), row in admissible[name].items()
                           if cp == checkpoint and _count(row.get(metric))]
                if len({sample["value"] for sample in samples}) > 1:
                    changes.append({"arm": name, "checkpoint": checkpoint, "metric": metric, "samples": samples})
    differences, pairs = [], []
    for key in sorted(counts["base"].keys() | counts["candidate"].keys()):
        base, candidate = counts["base"].get(key, {}), counts["candidate"].get(key, {})
        reason = _checkpoint_reason(base, reference=True) or _checkpoint_reason(candidate)
        pair = {"pass_index": key[0], "checkpoint": key[1],
                "assessment": "INCONCLUSIVE" if reason else "equal"}
        if reason:
            pair["reason"] = reason
        pairs.append(pair)
        for metric in ("drawn_splats", "drawn_nodes"):
            values = [row.get(metric) for row in (base, candidate)]
            if values[0] != values[1]:
                # #1448 is unchanged for usable observations. Only admissible
                # base samples establish its range; invalid candidate samples
                # cannot poison the all-candidate range assessment either.
                usable = not reason and key in admissible["base"] and key in admissible["candidate"]
                assessment = _draw_assessment([admissible["base"], admissible["candidate"]], key, metric) if usable else "INCONCLUSIVE"
                if usable:
                    pair["assessment"] = assessment if pair["assessment"] != "mismatch" else "mismatch"
                if not all(_count(v) for v in values):
                    continue
                differences.append({"pass_index": key[0], "checkpoint": key[1], "metric": metric,
                    "base": values[0], "candidate": values[1], "candidate_minus_base": values[1] - values[0],
                    "assessment": assessment, **({"reason": reason} if reason else {}),
                    "within_binary_variation_observed": any(c["checkpoint"] == key[1] and c["metric"] == metric for c in changes)})
    return {"scope": "same V4 source digest, actual common payload work and failure observations; aggregate work is scheduling-dependent",
            "native_payload": metrics, "draw_differences": differences, "within_binary_changes": changes,
            "checkpoint_pairs": pairs,
            "decode": {name: {key: measurement(behavior.get(key)) for key in (
                "decoded_bytes", "decoded_digest", "decoded_pages", "decoded_splats")} for name, behavior in behaviors.items()}}


FRAME_TIME_ARMS = ("chunk", "hlod", "migration_base", "migration_candidate")
FRAME_TIME_ROWS = (("moving", "moving_frame_breakdown"),
                   ("moving + checkpoint settle/hold", "measured_frame_breakdown"))
FRAME_TIME_SCOPE = ("wall = interval between trace rows, the probe's own work included (unchanged metric); "
                    "game-relevant = the engine frame alone, without the probe's telemetry, captures and script; "
                    "both time-weighted nearest rank; report-only, no threshold")
STATE_NAMES = {ps.STATE_MOVING: "moving", ps.STATE_SETTLING: "settling", ps.STATE_HOLD: "hold"}
HITCH_LINES_PER_PASS = 5


DROP_FRAME_LINES_PER_ARM = 40


def _overlap_drop_report(arms):
    """#1457/#1461: where each arm dropped overlap records and how large the dropping frames were.
    Report-only: the verdict keeps reading `drop_events` (correctness), which counts drops in the
    settle window and at the settled pose alike. An absent block is not measured, never zero."""
    out = {}
    for route in ("chunk", "hlod"):
        arm = _dict(arms.get(route))
        optional = _dict(arm.get("optional_measurements"))
        whole = _dict(_dict(arm.get("probe")).get("overlap_drops_whole_run"))
        split, frames = optional.get("checkpoint_drop_split"), optional.get("overlap_drop_frames")
        out[route] = {
            "whole_run": ({key: whole.get(key) for key in ("status", "readback_samples", "drop_events",
                           "binning_tile_range_drop_events", "binning_budget_drop_events",
                           "raster_tile_cap_drop_events", "sampled_dropped_records")}
                          if whole.get("status") == ps.OK else
                          {"status": ps.NOT_MEASURED, "reason": whole.get("reason", "the arm carries no whole-run drop window")}),
            "checkpoint_drop_split": (copy.deepcopy(split) if isinstance(split, dict) and split.get("status") == ps.OK else
                                      {"status": ps.NOT_MEASURED, "reason": _dict(split).get(
                                          "reason", "the arm carries no checkpoint drop split")}),
            "drop_frames": (copy.deepcopy(frames) if isinstance(frames, dict) and frames.get("status") == ps.OK else
                            {"status": ps.NOT_MEASURED, "reason": _dict(frames).get(
                                "reason", "the arm carries no drop-frame record")}),
        }
    return out


def _overlap_drop_lines(report):
    def clean(value):
        if value is None or value == ps.NOT_MEASURED:
            return "not measured"
        return str(value).replace("|", "/").replace("\n", " ")

    def window(record):
        record = _dict(record)
        if record.get("status") != ps.OK:
            return "not measured: " + clean(record.get("reason", "no window"))
        return f"{clean(record.get('drop_events'))} / {clean(record.get('sampled_dropped_records'))}"

    lines = ["", "## Overlap drops: where and how large (#1457, #1461)", "",
             "Report-only. `drop_events` in the correctness table decides, and it counts drops in the settle "
             "window and at the settled pose alike: both are missing splats. Cells are drop intervals / lost "
             "records in the sampled frames (a lower bound; each lost record once). " + ps.CHECKPOINT_DROP_SPLIT_SCOPE, "",
             "| arm, pass | whole run | checkpoint settle window | checkpoint settled pose |", "| --- | --- | --- | --- |"]
    report = _dict(report)
    for route in ("chunk", "hlod"):
        entry = _dict(report.get(route))
        whole, split = window(entry.get("whole_run")), _dict(entry.get("checkpoint_drop_split"))
        passes = split.get("passes") if split.get("status") == ps.OK else None
        if not isinstance(passes, list) or not passes:
            reason = "not measured: " + clean(split.get("reason", "no checkpoint drop split"))
            lines.append(f"| {route} | {whole} | {reason} | {reason} |")
            continue
        for index, record in enumerate(passes):
            record = _dict(record)
            lines.append(f"| {route}, {'cold' if index == 0 else 'repeat'} | {whole if index == 0 else 'see cold row'} | "
                         f"{window(record.get('settle_window'))} | {window(record.get('settled_pose'))} |")
    lines += ["", ps.DROP_FRAME_SCOPE.capitalize(), "",
              "| arm | observed at (pass, segment, state) | frame | lost records (raster-cap part) | "
              "max records in one tile | tile x, y | raster tile cap | max minus cap | records attempted | "
              "record allocation | attempted minus allocation |",
              "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    summary = []
    for route in ("chunk", "hlod"):
        frames = _dict(_dict(report.get(route)).get("drop_frames"))
        if frames.get("status") != ps.OK:
            lines.append(f"| {route} | " + " | ".join(
                ["not measured: " + clean(frames.get("reason", "no drop-frame record"))] * 10) + " |")
            continue
        described = frames.get("frames") if isinstance(frames.get("frames"), list) else []
        if not described:
            lines.append(f"| {route} | " + " | ".join(
                [f"no sampled frame lost records ({clean(frames.get('drop_frames'))} drop frames)"] * 10) + " |")
        for frame in described[:DROP_FRAME_LINES_PER_ARM]:
            frame = _dict(frame)
            state = STATE_NAMES.get(frame.get("observed_state"), clean(frame.get("observed_state")))
            lines.append(
                f"| {route} | pass {clean(frame.get('observed_pass_index'))}, segment "
                f"{clean(frame.get('observed_segment'))}, {state} | {clean(frame.get('frame_serial'))} | "
                f"{clean(frame.get('dropped_records'))} ({clean(frame.get('raster_cap_dropped_records'))}) | "
                f"{clean(frame.get('max_tile_records'))} | {clean(frame.get('max_tile_x'))}, {clean(frame.get('max_tile_y'))} | "
                f"{clean(frame.get('raster_tile_cap'))} | {clean(frame.get('max_tile_records_over_cap'))} | "
                f"{clean(frame.get('overlap_records_attempted'))} | {clean(frame.get('overlap_allocation_records'))} | "
                f"{clean(frame.get('overlap_records_over_allocation'))} |")
        peak = _dict(frames.get("raster_cap_max_tile_records_peak"))
        summary.append(
            f"{route}: {clean(frames.get('drop_frames'))} sampled dropping frames "
            f"({clean(frames.get('drop_frames_not_described'))} not described, "
            f"{max(len(described) - DROP_FRAME_LINES_PER_ARM, 0)} more in the JSON); raster-cap frames "
            f"{clean(frames.get('raster_cap_frames'))}, their largest per-tile record count "
            f"{clean(peak.get('value')) if peak.get('status') == ps.OK else 'not measured'}.")
    return lines + [""] + summary


def _frame_time(arms, migration_arms):
    """#1445: each arm's wall-clock and game-relevant frame time, side by side. Report-only:
    nothing here feeds the verdict, and an absent block stays absent (rendered as not measured)."""
    out = {route: ps.frame_time_passes(_dict(arms.get(route)).get("probe")) for route in ("chunk", "hlod")}
    for name in ("base", "candidate"):
        record = _dict(migration_arms.get(name)).get("frame_time")
        out["migration_" + name] = copy.deepcopy(record) if isinstance(record, list) else []
    return out


def _frame_time_lines(frame_time):
    def num(record, digits=2):
        value = _value(record) if isinstance(record, dict) else None
        return f"{value:.{digits}f}" if _number(value) else "not measured"

    def triple(dist):
        dist = _dict(dist)
        return " / ".join(num(dist.get(f"p{q}_time_weighted")) for q in (50, 95, 99))

    def clean(text):
        return "not measured" if text is None else str(text).replace("|", "/").replace("\n", " ")

    lines = ["", "## Frame time: wall clock and game-relevant (#1445)", "",
             "Wall clock is the interval between trace rows (the unchanged moving-frame metric) and includes the "
             "probe's own work. Game-relevant is the engine frame alone: the same interval without the probe's "
             "telemetry rows, viewport captures (readback, PNG encode, write) and script. p50 / p95 / p99 are "
             "time-weighted nearest rank; shares are of wall time; GPU time runs in parallel and is no share. "
             "Report-only, no threshold.", "",
             "| arm, pass, rows | frames | wall ms p50 / p95 / p99 | game-relevant ms p50 / p95 / p99 | "
             "harness ms p50 / p95 / p99 | wall share %: renderer CPU / engine other / script / capture / telemetry | "
             "GPU frame ms mean / p95 | frames over 50 ms: wall / harness-only / game (game over 250 ms) |",
             "| --- | --- | --- | --- | --- | --- | --- | --- |"]
    hitches = []
    frame_time = _dict(frame_time)
    for arm in FRAME_TIME_ARMS:
        passes = frame_time.get(arm)
        if not isinstance(passes, list) or not passes:
            lines.append(f"| {arm} | " + " | ".join(
                ["not measured: the arm carries no frame-time record"] * 7) + " |")
            continue
        for index, record in enumerate(passes):
            record = _dict(record)
            label = "cold" if index == 0 else "repeat"
            for rows_label, key in FRAME_TIME_ROWS:
                breakdown = _dict(record.get(key))
                name = f"{arm}, {label}, {rows_label}"
                if breakdown.get("status") != ps.OK:
                    reason = "not measured: " + clean(breakdown.get("reason", "the summary carries no frame breakdown"))
                    wall = reason
                    if key == "moving_frame_breakdown":
                        # The old wall-clock block does not depend on the breakdown columns.
                        wall = triple(record.get("moving_frame_interval"))
                    lines.append(f"| {name} | {reason} | {wall} | " + " | ".join([reason] * 5) + " |")
                    continue
                share = _dict(breakdown.get("wall_share_pct"))
                gpu = _dict(_dict(breakdown.get("renderer")).get("gpu_frame_ms"))
                slow = _dict(breakdown.get("slow_frames"))
                lines.append(
                    f"| {name} | {breakdown.get('frames')} | {triple(breakdown.get('wall_frame'))} | "
                    f"{triple(breakdown.get('game_frame'))} | {triple(breakdown.get('harness_frame'))} | "
                    + " / ".join(num(share.get(k), 1) for k in (
                        "renderer_cpu", "engine_other", "script", "capture", "telemetry"))
                    + f" | {num(gpu.get('mean'))} / {num(gpu.get('p95_frames'))} | "
                    f"{slow.get('wall_over_50ms')} / {slow.get('wall_over_50ms_harness_only')} / "
                    f"{slow.get('game_over_50ms')} ({slow.get('game_over_250ms')}) |")
                if key != "measured_frame_breakdown":
                    continue
                listed = slow.get("listed") if isinstance(slow.get("listed"), list) else []
                hitches.append(f"{arm} {label}: {slow.get('game_over_50ms')} game-relevant frame(s) over 50 ms in the "
                               f"moving + checkpoint rows, {slow.get('game_over_50ms_with_capture_in_interval')} of them "
                               "right after a viewport capture"
                               + (f"; the longest {min(len(listed), HITCH_LINES_PER_PASS)}:" if listed else "."))
                for entry in listed[:HITCH_LINES_PER_PASS]:
                    entry = _dict(entry)
                    ms = {k: (f"{entry[k]:.1f}" if _number(entry.get(k)) else "not measured")
                          for k in ("game_frame_ms", "wall_frame_ms", "render_cpu_ms", "gpu_frame_ms")}
                    hitches.append(
                        f"- row {entry.get('row')} (segment {entry.get('segment')}, "
                        f"{STATE_NAMES.get(entry.get('state'), entry.get('state'))}): engine frame {ms['game_frame_ms']} ms, "
                        f"wall {ms['wall_frame_ms']} ms, renderer CPU {ms['render_cpu_ms']} ms, GPU {ms['gpu_frame_ms']} ms, "
                        f"chunk upload bytes {clean(entry.get('upload_bytes_delta'))}, "
                        f"HLOD upload bytes {clean(entry.get('hlod_stream_uploaded_bytes_delta'))}, "
                        f"captures just before {entry.get('captures_in_interval')}")
    return lines + [""] + hitches


def _pressure_lines(report):
    """#1471: the pressure variant's request, its required evidence and each settle window."""
    pressure, request = _dict(report.get("pressure")), _dict(report.get("variant"))

    def clean(value):
        if isinstance(value, dict) and value.get("measured") is False:
            return "not measured: " + str(value.get("reason", "no observation")).replace("|", "/").replace("\n", " ")
        if value is None:
            return "not measured"
        return (json.dumps(value, sort_keys=True) if isinstance(value, (dict, list)) else str(value)).replace("|", "/")

    hlod, chunk = _dict(pressure.get("hlod")), _dict(pressure.get("chunk"))
    fraction = hlod.get("atlas_fraction_observed")
    lines = ["", "## Pressure variant (#1471)", "",
             f"Requested: `{request.get('atlas_setting')}` = {clean(request.get('atlas_override_mb'))} MiB "
             f"({clean(request.get('requested_atlas_pages'))} of {clean(request.get('tree_pages'))} tree pages) and a "
             f"drawn budget of {clean(request.get('drawn_budget_splats'))} splats, for every arm. "
             "A request is not evidence: each row below is a process observation, and a missing or zero one "
             "makes the run UNUSABLE with its own reason.", "",
             f"Chunk arm: {str(chunk.get('role', 'unconstrained_reference')).replace('_', ' ')}, constraint "
             f"{str(chunk.get('constraint', 'not_applied')).replace('_', ' ')}. {clean(chunk.get('reason'))} "
             f"Chunk evictions observed: {clean(chunk.get('evicted_chunks'))}.", "",
             "| HLOD pressure evidence | observed | required | satisfied |", "| --- | --- | --- | --- |"]
    needs = {"settings applied": "requested and read back on both routes", "atlas cap": "native capacity within 20-25 % of the tree pages", "evictions": "> 0",
             "budget-bound frames": "> 0", "teleport settle": "every teleport settles, at least one with streaming work",
             "bound violations measured": "measured (nonzero fails as queue_saturation)",
             "read/decode failures measured": "measured (nonzero fails as payload_failure)"}
    for item in hlod.get("requirements") or []:
        item = _dict(item)
        lines.append(f"| {item.get('name')} | {clean(item.get('observed'))} | {needs.get(item.get('name'), 'present')} | "
                     f"{'yes' if item.get('satisfied') is True else 'NO'} |")
    lines += ["", f"HLOD atlas: capacity {clean(hlod.get('atlas_pages_capacity'))} pages "
              f"({f'{fraction:.1%}' if _number(fraction) else 'not measured'} of the tree), resident at exit "
              f"{clean(hlod.get('pages_resident_final'))}; payload reads {clean(hlod.get('payload_reads_total'))}, "
              f"re-reads {clean(hlod.get('rereads'))} ({clean(hlod.get('rereads_scope'))}).", "",
              "| arm | settle window | settled | frames | settle ms | streaming work in the window | drawn splats / nodes |",
              "| --- | --- | --- | --- | --- | --- | --- |"]
    for route, entry in (("chunk", chunk), ("hlod", hlod)):
        rows = entry.get("settles") if isinstance(entry.get("settles"), list) else []
        if not rows:
            lines.append(f"| {route} | " + " | ".join(["not measured: no settle receipts"] * 6) + " |")
        for row in rows:
            row = _dict(row)
            lines.append(f"| {route} | {row.get('kind')} {row.get('checkpoint')}, "
                         f"{'cold' if row.get('pass_index') == 0 else 'repeat'} | {clean(row.get('settled'))} | "
                         f"{clean(row.get('frames'))} | {clean(row.get('settle_ms'))} | {clean(row.get('streaming_work'))} | "
                         f"{clean(row.get('drawn_splats'))} / {clean(row.get('drawn_nodes'))} |")
    return lines


def render_markdown(report: dict) -> str:
    """Keep measurement scopes and unavailable values visible."""
    lines = [f"# Shared layers T0: {report.get('verdict', 'NOT_RUN')}", "",
             "Report-only diagnostic; this is not a Tier A verdict. No performance regression threshold is applied.", "",
             "Worker stage times are CPU sums, not wall latency or separately measured zstd time.", ""]
    lines += [f"Control verdict: {report.get('control_verdict', 'NOT_REQUESTED')}. "
              "A detected control succeeds only when non-control arms are otherwise usable.", ""]
    for route, identity in _dict(report.get("content_identity")).items():
        lines.append(f"Content identity {route}: {json.dumps(identity, sort_keys=True)}.")
    migration_identity = _dict(_dict(report.get("migration")).get("content_identity"))
    lines += [f"Migration content identity: {migration_identity.get('status', 'NOT_MEASURED')}. "
              f"{migration_identity.get('reason', 'No decoded content observation')}. "
              f"{migration_identity.get('scope', '')}",
              f"Migration candidate content check: {json.dumps(migration_identity.get('candidate', {}), sort_keys=True)}.", ""]
    for reason in report.get("reasons", []):
        lines.append(f"- {reason.get('arm', 'experiment')}: {reason.get('code')}: {reason.get('detail')}")
    evidence = _dict(report.get("checkpoint_evidence"))
    lines += ["", "## Checkpoint evidence (#1454)", "",
              f"Checkpoint evidence: {evidence.get('verdict', 'NOT_MEASURED')}. "
              "Process completion does not establish checkpoint admissibility.", "",
              "| arm | pass | checkpoint | verdict / reasons | pose first / count source / render frame | cache enabled / applicable |",
              "| --- | --- | --- | --- | --- | --- |"]
    for record in evidence.get("checkpoints", []):
        record = _dict(record)
        cache = _dict(record.get("static_sort_cache"))
        values = (record.get("arm"), record.get("pass_index"), record.get("checkpoint"),
                  f"{record.get('evidence_verdict')} / {', '.join(record.get('evidence_reasons', []))}",
                  f"{record.get('pose_first_frame')} / {record.get('rendered_splat_count_source_frame')} / "
                  f"{record.get('render_frame_counter')}", f"{cache.get('enabled')} / {cache.get('applicable')}")
        lines.append("| " + " | ".join(str(value).replace("|", "/").replace("\n", " ") for value in values) + " |")
    for pair in _dict(report.get("migration_observations")).get("checkpoint_pairs", []):
        lines.append(f"Migration checkpoint pair: {json.dumps(pair, sort_keys=True)}.")
    lines += ["", "| Area / metric | chunk | hlod |", "| --- | --- | --- |"]

    def cell(rec):
        rec = _dict(rec)
        if rec.get("measured") is not True:
            return "not measured: " + str(rec.get("reason", "measurement unavailable")).replace("|", "/").replace("\n", " ")
        value = json.dumps(rec.get("value"), ensure_ascii=True)
        if rec.get("scope"):
            value += f" ({rec['scope']})"
        return value.replace("|", "/").replace("\n", " ")

    for area, keys in METRICS.items():
        for key in keys:
            columns = [cell(_dict(_dict(_dict(report.get("metrics")).get(route)).get(area)).get(key)) for route in ("chunk", "hlod")]
            lines.append(f"| {area} / {key} | {columns[0]} | {columns[1]} |")
    lines += ["", f"Migration base: `{_dict(report.get('migration')).get('base_commit', PRE_D3)}`.",
              "Pre-D3 measurements are limited to common native payload/route/draw observations; missing stages are not measured.", ""]
    migration = _dict(report.get("migration_observations"))
    lines += ["Aggregate payload work can differ with scheduling, cancellation, eviction and rereads.",
              "The loaded-file SHA identifies source bytes; it is not a decoded-payload digest.", "",
              "| Common payload observation | pre-D3 | candidate | candidate minus pre-D3 |", "| --- | --- | --- | --- |"]
    for key, values in _dict(migration.get("native_payload")).items():
        lines.append(f"| {key} | {cell(values.get('base'))} | {cell(values.get('candidate'))} | {cell(values.get('candidate_minus_base'))} |")
    for difference in migration.get("draw_differences", []):
        lines.append(f"Migration draw observation: {json.dumps(difference, sort_keys=True)}.")
    for change in migration.get("within_binary_changes", []):
        lines.append(f"Same-binary cold/repeat draw observation: {json.dumps(change, sort_keys=True)}.")
    for name, values in _dict(migration.get("decode")).items():
        for key, value in values.items():
            lines.append(f"Migration {name} {key}: {cell(value)}.")
    for route, passes in _dict(report.get("passes")).items():
        for index, record in enumerate(passes):
            intervals = _dict(_dict(record).get("moving_frame_interval"))
            lines.append(f"{route} {'cold' if index == 0 else 'repeat'}: moving frame p95/p99 ms = "
                         f"{cell(measurement(intervals.get('p95_time_weighted')))} / "
                         f"{cell(measurement(intervals.get('p99_time_weighted')))} "
                         "(wall clock, probe work included; see the frame-time table).")
    if "pressure" in report:
        lines += _pressure_lines(report)
    lines += _frame_time_lines(report.get("frame_time"))
    lines += _overlap_drop_lines(report.get("overlap_drop_report"))
    return "\n".join(lines)


def _safe_json(value):
    if isinstance(value, dict):
        return {k: _safe_json(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_safe_json(v) for v in value]
    return None if isinstance(value, float) and not math.isfinite(value) else value


def _canonical(value):
    return json.dumps(_safe_json(value), sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _report_variant(report):
    """The variant a report was assessed as. A report without the block is the unconstrained one."""
    if "variant" not in report:
        return "unconstrained"
    return _dict(report["variant"]).get("name") if _dict(report["variant"]).get("name") != "unconstrained" else None


def _require_report(path, run_id, control, variant="unconstrained"):
    if not path.is_file():
        print("NOT_RUN: comparison report absent; comparison invocation may have been removed", file=sys.stderr)
        return EXIT_CODES["NOT_RUN"]
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
        if (report.get("schema") != SCHEMA or report.get("run_id") != run_id or report.get("control") != control
                or report.get("report_only") is not True):
            raise ValueError("report is malformed or belongs to a different run/control")
        if _report_variant(report) != variant:
            # #1471: an unconstrained VALID report must never satisfy a pressure dispatch, or the reverse.
            raise ValueError(f"report was not assessed as the dispatched {variant} variant")
        if report.get("verdict") != "VALID":
            return EXIT_CODES.get(report.get("verdict"), EXIT_CODES["UNUSABLE"])
        evidence = _dict(report.get("checkpoint_evidence"))
        if evidence.get("verdict") != "VALID" or not evidence.get("checkpoints"):
            raise ValueError("checkpoint admissibility validation is absent or unusable")
        if (control in ("corrupt", "saturate") and report.get("control_verdict") != "DETECTED") or control == "skip_compare":
            raise ValueError("requested negative control was not detected")
        bound = dict(report)
        seal = bound.pop("report_sha256", None)
        if seal != hashlib.sha256(_canonical(bound)).hexdigest():
            raise ValueError("report was changed or not written by the comparison invocation")
        source = Path(bound.pop("experiment_file"))
        source_digest = bound.pop("experiment_sha256")
        if _sha256(source) != source_digest:
            raise ValueError("bound experiment changed after comparison")
        if "contention_file" in bound:
            contention_file = Path(bound.pop("contention_file"))
            contention_digest = bound.pop("contention_sha256")
            contention = bound.pop("contention")
            if _sha256(contention_file) != contention_digest or contention.get("verdict") != "CLEAN":
                raise ValueError("contention evidence is not current and CLEAN")
        fresh = compare(json.loads(source.read_text(encoding="utf-8")))
        if _canonical(fresh) != _canonical(bound) or fresh["verdict"] != "VALID":
            raise ValueError("current evidence does not reproduce the usable report")
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        print(f"UNUSABLE: {exc}", file=sys.stderr)
        return EXIT_CODES["UNUSABLE"]
    print(f"VALID: current report-only paired T0 evidence; control {report.get('control_verdict', 'NOT_REQUESTED')}")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--experiment", type=Path)
    mode.add_argument("--require-report", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--contention-report", type=Path)
    parser.add_argument("--control", choices=("none", "corrupt", "saturate", "skip_compare"), default="none")
    parser.add_argument("--variant", choices=VARIANTS, default="unconstrained",
                        help="#1471: the dispatched variant; the experiment and the report must be of this variant")
    args = parser.parse_args(argv)
    if args.require_report:
        if not args.run_id:
            parser.error("--require-report needs --run-id")
        return _require_report(args.require_report, args.run_id, args.control, args.variant)
    if not args.out:
        parser.error("--experiment needs --out")
    try:
        experiment = json.loads(args.experiment.read_text(encoding="utf-8"))
        report = compare(experiment)
        if args.run_id and report.get("run_id") != args.run_id:
            report["reasons"].append({"code": "run_identity_mismatch", "detail": "experiment is from another workflow attempt"})
            report.update(verdict="UNUSABLE", exit_code=2)
        if _report_variant(report) != args.variant:
            report["reasons"].append({"code": "variant_mismatch", "detail": (
                f"dispatched variant is {args.variant}; the experiment records "
                f"{_report_variant(report) or 'an invalid variant'}")})
            if report["verdict"] != "NOT_RUN":
                report.update(verdict="UNUSABLE", exit_code=2)
        if args.contention_report:
            try:
                contention = json.loads(args.contention_report.read_text(encoding="utf-8"))
                report.update(contention=contention, contention_file=str(args.contention_report.resolve()),
                              contention_sha256=_sha256(args.contention_report))
                clean = _dict(contention).get("verdict") == "CLEAN"
            except (OSError, ValueError):
                clean = False
            if not clean:
                report["reasons"].append({"code": "contention_unproven", "detail": "postflight contention evidence is absent or not CLEAN"})
                if report["verdict"] != "NOT_RUN":
                    report.update(verdict="UNUSABLE", exit_code=2)
        report.update(experiment_file=str(args.experiment.resolve()), experiment_sha256=_sha256(args.experiment))
    except (OSError, ValueError, TypeError, KeyError) as exc:
        verdict = "NOT_RUN" if not args.experiment.is_file() else "UNUSABLE"
        report = {"schema": SCHEMA, "run_id": args.run_id, "control": args.control, "report_only": True,
                  "verdict": verdict, "exit_code": EXIT_CODES[verdict], "metrics": {},
                  "reasons": [{"code": "experiment_unreadable", "detail": str(exc)}]}
    report = _safe_json(report)
    report["report_sha256"] = hashlib.sha256(_canonical(report)).hexdigest()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "comparison.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    (args.out / "comparison.md").write_text(render_markdown(report), encoding="utf-8")
    print(f"{report['verdict']}: report-only T0 comparison ({len(report['reasons'])} reason(s))")
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
