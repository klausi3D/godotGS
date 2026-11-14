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

PRE_D3 = "f2f19bcb580cc131615b3fa262a11d99e6abc70c"
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
                    "device_ledger_vram_bytes", "external_vram_bytes"),
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
        # A same-binary cold/repeat difference disproves rendered-count identity
        # at that checkpoint. Keep it visible; stable cross-binary differences
        # still fail. Never use this exception to excuse different payload bytes.
        for key in sorted(drawn_counts[0].keys() & drawn_counts[1].keys()):
            for metric in sorted(drawn_counts[0][key].keys() | drawn_counts[1][key].keys()):
                values = [counts[key].get(metric) for counts in drawn_counts]
                varies = any(len({row[metric] for (_, cp), row in counts.items()
                                  if cp == key[1] and metric in row}) > 1 for counts in drawn_counts)
                if values[0] != values[1] and not varies:
                    problem("migration_identity_mismatch", f"self-consistent checkpoint {key[1]} {metric} differs")
        for key in ("decoded_bytes", "decoded_digest", "decoded_pages", "decoded_splats"):
            values = [_value(b.get(key)) for b in behaviors]
            if all(v is not None for v in values) and values[0] != values[1]:
                problem("migration_identity_mismatch", f"migration {key} differs")
    control = exp.get("control", "none")
    receipt = _dict(exp.get("control_receipt"))
    if control not in ("none", "corrupt", "saturate", "skip_compare"):
        problem("control_not_observed", "unknown negative control")
    if control in ("corrupt", "saturate"):
        wanted = "payload_failure" if control == "corrupt" else "queue_saturation"
        if receipt.get("status") == "NOT_RUN":
            problem("control_not_run", receipt.get("reason", "forcing hook unavailable"), not_run=True)
        elif "arm" in receipt or not any(r["code"] == wanted for r in reasons):
            control_arm = _dict(receipt.get("arm"))
            control_telemetry = (_dict(_dict(control_arm.get("native")).get("payload_telemetry"))
                                 or _dict(_dict(control_arm.get("probe")).get("payload_telemetry")))
            corruption_source = ("corrupt_sha256" not in receipt or (
                _is_sha(receipt.get("corrupt_sha256")) and receipt.get("source_sha256") == prepared.get("tree_file_sha256")
                and receipt["corrupt_sha256"] != receipt["source_sha256"]
                and _dict(control_arm.get("source")).get("loaded_file_sha256") == receipt["corrupt_sha256"]
                and control_telemetry.get("route") == "hlod" and _count(control_telemetry.get("read_requests"))
                and control_telemetry["read_requests"] > 0))
            if control == "corrupt" and corruption_source and any(_count(control_telemetry.get(k)) and control_telemetry[k] > 0
                                           for k in ("page_failures", "captures_failed", "digest_verification_failures")):
                problem("payload_failure", "corrupted V4 control produced a native payload verification failure", "control")
            elif control == "saturate" and _saturation_evidence(control_arm):
                problem("queue_saturation", "separate HLOD control observed bounded-pipeline pressure", "control")
            else:
                problem("control_not_observed", f"{control} label has no attributable observed failure")
    if control == "skip_compare":
        problem("comparison_skipped", "comparison invocation is deliberately removed", not_run=True)
    if exp.get("orchestration_problem") or exp.get("orchestration_error"):
        problem("orchestration_failed", str(exp.get("orchestration_problem", exp.get("orchestration_error"))))
    verdict = "NOT_RUN" if absent_execution else "UNUSABLE" if reasons else "VALID"
    return {"schema": SCHEMA, "run_id": exp.get("run_id"), "control": control, "report_only": True,
            "verdict": verdict, "exit_code": EXIT_CODES[verdict], "reasons": reasons,
            "prepared": copy.deepcopy(prepared), "migration": copy.deepcopy(migration),
            "migration_observations": _migration_observations(migration_arms),
            "control_receipt": copy.deepcopy(receipt),
            "artifact_digests": _artifact_digests(arms, migration_arms),
            "passes": {route: copy.deepcopy(_dict(_dict(arms.get(route)).get("probe")).get("passes", []))
                       for route in ("chunk", "hlod")},
            "forward_progress": {route: _progress_summary(arms.get(route), route) for route in ("chunk", "hlod")},
            "measurement_scopes": {"decode_pack": "worker_cpu_sum; not zstd wall time",
                                   "io_latency": "reader_wall_sum and read queue sum",
                                   "frame_intervals": "time-weighted nearest rank, cold and repeat reported separately"},
            "metrics": {route: _route_metrics(arms.get(route)) for route in ("chunk", "hlod")}}


def _migration_observations(arms):
    behaviors = {name: _dict(_dict(arms.get(name)).get("payload_behavior")) for name in ("base", "candidate")}
    native = {name: _dict(behavior.get("native_payload")) for name, behavior in behaviors.items()}
    metrics = {}
    for key in sorted(set(native["base"]) | set(native["candidate"])):
        values = [native[name].get(key) for name in ("base", "candidate")]
        metrics[key] = {name: measurement(native[name].get(key)) for name in native}
        metrics[key]["candidate_minus_base"] = (measurement(values[1] - values[0])
            if all(_number(v) for v in values) else not_measured("no two numeric common observations"))
    counts, changes = {}, []
    for name, behavior in behaviors.items():
        rows = behavior.get("drawn_counts")
        rows = rows if isinstance(rows, list) else []
        counts[name] = {(row["pass_index"], row["checkpoint"]): row for row in rows if isinstance(row, dict)
                        and _count(row.get("pass_index")) and isinstance(row.get("checkpoint"), str)}
        for checkpoint in sorted({key[1] for key in counts[name]}):
            for metric in ("drawn_splats", "drawn_nodes"):
                samples = [{"pass_index": index, "value": row[metric]} for (index, cp), row in counts[name].items()
                           if cp == checkpoint and _count(row.get(metric))]
                if len({sample["value"] for sample in samples}) > 1:
                    changes.append({"arm": name, "checkpoint": checkpoint, "metric": metric, "samples": samples})
    differences = []
    for key in sorted(counts["base"].keys() & counts["candidate"].keys()):
        for metric in ("drawn_splats", "drawn_nodes"):
            values = [counts[name][key].get(metric) for name in ("base", "candidate")]
            if all(_count(v) for v in values) and values[0] != values[1]:
                differences.append({"pass_index": key[0], "checkpoint": key[1], "metric": metric,
                    "base": values[0], "candidate": values[1], "candidate_minus_base": values[1] - values[0],
                    "within_binary_variation_observed": any(c["checkpoint"] == key[1] and c["metric"] == metric for c in changes)})
    return {"scope": "same V4 source digest, actual common payload work and failure observations; aggregate work is scheduling-dependent",
            "native_payload": metrics, "draw_differences": differences, "within_binary_changes": changes,
            "decode": {name: {key: measurement(behavior.get(key)) for key in (
                "decoded_bytes", "decoded_digest", "decoded_pages", "decoded_splats")} for name, behavior in behaviors.items()}}


def render_markdown(report: dict) -> str:
    """Keep measurement scopes and unavailable values visible."""
    lines = [f"# Shared layers T0: {report.get('verdict', 'NOT_RUN')}", "",
             "Report-only diagnostic; this is not a Tier A verdict. No performance regression threshold is applied.", "",
             "Worker stage times are CPU sums, not wall latency or separately measured zstd time.", ""]
    for reason in report.get("reasons", []):
        lines.append(f"- {reason.get('arm', 'experiment')}: {reason.get('code')}: {reason.get('detail')}")
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
                         f"{cell(measurement(intervals.get('p99_time_weighted')))}.")
    return "\n".join(lines)


def _safe_json(value):
    if isinstance(value, dict):
        return {k: _safe_json(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_safe_json(v) for v in value]
    return None if isinstance(value, float) and not math.isfinite(value) else value


def _canonical(value):
    return json.dumps(_safe_json(value), sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _require_report(path, run_id, control):
    if not path.is_file():
        print("NOT_RUN: comparison report absent; comparison invocation may have been removed", file=sys.stderr)
        return EXIT_CODES["NOT_RUN"]
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
        if (report.get("schema") != SCHEMA or report.get("run_id") != run_id or report.get("control") != control
                or report.get("report_only") is not True):
            raise ValueError("report is malformed or belongs to a different run/control")
        if report.get("verdict") != "VALID":
            return EXIT_CODES.get(report.get("verdict"), EXIT_CODES["UNUSABLE"])
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
    print("VALID: current report-only paired T0 evidence")
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
    args = parser.parse_args(argv)
    if args.require_report:
        if not args.run_id:
            parser.error("--require-report needs --run-id")
        return _require_report(args.require_report, args.run_id, args.control)
    if not args.out:
        parser.error("--experiment needs --out")
    try:
        experiment = json.loads(args.experiment.read_text(encoding="utf-8"))
        report = compare(experiment)
        if args.run_id and report.get("run_id") != args.run_id:
            report["reasons"].append({"code": "run_identity_mismatch", "detail": "experiment is from another workflow attempt"})
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
