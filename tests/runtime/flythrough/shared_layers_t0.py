#!/usr/bin/env python3
"""Runner orchestration for the report-only D3-6 paired synthetic T0 experiment.

No C++ observation is inferred from a requested setting or a missing key. Native
route-labelled diagnostics supply the bounded owners and publication counters;
an absent observation remains explicit. This module runs only through
dispatch_lane on the dedicated runner; unit tests inject producers.
"""

from __future__ import annotations

import copy
import hashlib
import json
import math
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import probe_summary as ps
import run_probe as rp
import world_cache as wc

PRE_D3 = "f2f19bcb580cc131615b3fa262a11d99e6abc70c"
SCHEMA = "godotgs-shared-layers-t0-experiment/1"


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected an object")
    return value


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")


def missing(reason: str) -> dict:
    return {"measured": False, "reason": reason}


def sha256(path: Path) -> str:
    return rp.binary_identity(path)["sha256"]


def object_sha(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def float32_values(value):
    """Canonicalize engine real_t precision; this is not an error threshold."""
    if isinstance(value, dict):
        return {key: float32_values(item) for key, item in value.items()}
    if isinstance(value, list):
        return [float32_values(item) for item in value]
    return f32(value) if type(value) is float else value


def camera_path_sha256(segments) -> str:
    """Match the probe's typed JSON/f32 digest, including signed-zero normalization.

    JSON number spellings are not camera identity: Godot serializes -0.0 as
    0.0. All path numbers use engine f32 precision; type tags keep strings,
    arrays and objects distinct. No coordinate tolerance is applied.
    """
    def encode(value):
        if isinstance(value, dict):
            return ["object", *[[key.encode("utf-8").hex(), encode(item)] for key, item in sorted(value.items())]]
        if isinstance(value, list):
            return ["array", *[encode(item) for item in value]]
        if type(value) in (int, float):
            number = f32(value)
            return ["number_f32", struct.pack("<f", number if number != 0 else 0.0).hex()]
        if value is None:
            return ["null"]
        return ["bool", value] if type(value) is bool else ["string", value.encode("utf-8").hex()]
    return hashlib.sha256(json.dumps(encode(segments), ensure_ascii=False,
                                    separators=(",", ":")).encode("utf-8")).hexdigest()


def camera_application(trace, path: dict) -> dict:
    """Check engine camera coordinates at every traced path row, exactly at f32.

    Orientation follows Node3D::set_rotation_degrees/get_rotation_degrees and
    Math's float overloads, including their float32 degree/radian round-trip.
    The trace remains authoritative and is retained for independent inspection.
    """
    if trace is None or not trace.rows:
        return missing("no trace camera observations")
    fields = ("px", "py", "pz", "yaw_deg", "pitch_deg")
    deg_factor = f32(f32(math.pi) / 180.0)
    rad_factor = f32(180.0 / f32(math.pi))
    checked = 0
    for index, row in enumerate(trace.rows):
        segment = int(trace.get(row, "segment"))
        if not 0 <= segment < len(path["segments"]):
            return {"measured": True, "matches": False, "row": index, "reason": "trace has invalid path segment"}
        seg = path["segments"][segment]
        pose = (rp.probe_path.interpolate(seg["table"], trace.get(row, "s_m"))
                if seg["kind"] == "move" else seg["pose"])
        expected = [f32(v) for v in pose[:3]] + [f32(f32(f32(v) * deg_factor) * rad_factor) for v in pose[3:]]
        actual = [f32(trace.get(row, key)) for key in fields]
        checked += 1
        if actual != expected:
            return {"measured": True, "matches": False, "row": index, "expected_f32": expected,
                    "actual_f32": actual, "rows_checked": checked, "reason": "trace camera pose differs from applied path"}
    return {"measured": True, "matches": True, "rows_checked": checked,
            "precision": "exact engine float32 positions and Math degree/radian round-trip; no tolerance"}


def verified_binary(binary: Path, build_info: Path, expected: str) -> dict:
    """Require both the downloaded artifact digest and its immutable commit."""
    info = read_json(build_info)
    provenance = info.get("provenance", {})
    actual = rp.binary_identity(binary)
    if (provenance.get("commit") != expected or info.get("sha256") != actual["sha256"]
            or provenance.get("sha256") != actual["sha256"]
            or provenance.get("binary") != binary.name
            or not str(info.get("version", "")).endswith("." + expected[:9])):
        raise ValueError(f"binary identity mismatch for {expected}: {build_info}")
    return {"commit": expected, "sha256": actual["sha256"], "version": info.get("version")}


def prepared_receipt(repo: Path, plain: Path, tree: Path, report: dict) -> dict:
    source = read_json(plain.with_suffix(".manifest.json"))
    derived = read_json(tree.with_suffix(".manifest.json"))
    world = source["world"]
    rect = world["extent_m"]
    codec = dict(source["writer"]["codec"])
    codec.pop("codec", None)
    derived_codec = dict(derived["codec"])
    derived_codec.pop("codec", None)
    if float32_values(codec) != float32_values(derived_codec):
        raise ValueError("T0 plain/tree codec parameters differ")
    if len(rect) != 4:
        raise ValueError("T0 manifest extent is not the generator's [x0,z0,x1,z1] rectangle")
    return {
        "tier": source["generator_params"]["tier"],
        "splat_count": world["splat_count"], "extent_m": [rect[2] - rect[0], rect[3] - rect[1]],
        "input_file_sha256": sha256(plain),
        "generator_identity": {"gen_sha": source["gen_sha"],
                               "generator": source["generator"],
                               "generator_params_sha": source["generator_params_sha"],
                               "source_fingerprint": wc.source_fingerprint(repo, wc.GENERATOR_SOURCES)},
        "codec": {"encoding": "gsq", "sh_bands": world["sh_degree"],
                  "per_chunk_quantization": False, "params": float32_values(codec)},
        "tree_file_sha256": sha256(tree),
        "tree_identity": {"tree_sha": derived["tree_sha"],
                          "source_fingerprint": derived["source_fingerprint"],
                          "content_generation": derived["content_generation"]},
        "w13": report["w13"],
        "builder_oracle": {"drawn_budget_splats": report["drawn_budget_splats"],
                           "drawn_budget_source": report["drawn_budget_source"],
                           "budget_frozen": False, "scope": "D2 builder oracle only; runtime uses the common 4M node cap"},
    }


def requested_settings(plan: dict, prepared: dict) -> dict:
    return float32_values({"camera_path_sha256": camera_path_sha256(plan["path"]["segments"]),
            "camera_poses": plan["path"]["segments"], "resolution": plan["resolution"],
            "camera": plan["camera"], "codec": prepared["codec"],
            "sh_bands": prepared["codec"]["sh_bands"],
            "visible_splats": plan["node"]["max_splat_count"],
            "drawn_splats": plan["node"]["max_splat_count"],
            "timing_windows": {"moving_passes": plan["passes"], "settle": plan["settle"]},
            "node_readback": {"max_splat_count": plan["node"]["max_splat_count"],
                              "max_render_distance": plan["node"]["max_render_distance_m"], "lod_enabled": True,
                              "hlod_tau_px": plan["node"].get("hlod_tau_px", 0.0),
                              "lod_bias": plan["node"].get("lod_bias", 1.0)},
            "project_settings_readback": plan["project_settings"]})


def process_settings(result: dict) -> dict:
    # Only process output can populate this record. The path digest is calculated
    # from the process's parsed path, never copied from the requested plan hash.
    settings = float32_values(result.get("shared_layers_settings", {}))
    if "camera_poses" in settings:
        # Earlier producers emitted the complete parsed path without its digest.
        # Preserve a native digest when present: a wrong process digest must fail.
        settings.setdefault("camera_path_sha256", camera_path_sha256(settings["camera_poses"]))
    return settings


def forward_progress_windows(result: dict) -> list:
    """Retain producer settle receipts, including cold holds without checkpoints."""
    windows, seen = [], set()
    for source, entries in (("segment", result.get("segments", [])), ("checkpoint", result.get("checkpoints", []))):
        for entry in entries:
            settle = entry.get("settle")
            if not isinstance(settle, dict):
                continue
            identity = (entry.get("pass_index"), entry.get("id") if source == "segment" else entry.get("segment"))
            if identity in seen:
                continue
            seen.add(identity)
            receipt = copy.deepcopy(settle.get("forward_progress", missing("producer did not capture this settle window's progress")))
            receipt.update(pass_index=entry.get("pass_index"), segment=identity[1],
                           checkpoint=entry.get("checkpoint"), settled=settle.get("settled"),
                           source=source)
            windows.append(receipt)
    return windows


def observations(result: dict, exit_code: int) -> dict:
    final = result.get("final_render_stats", {})
    peaks = result.get("shared_layers_peaks", {})
    payload_peaks = result.get("shared_layers_payload_peaks", {})
    payload = result.get("payload_telemetry") or {}
    route = result.get("route")
    world = result.get("world_payload_readback", {})

    def value(key, source=None):
        source = final if source is None else source
        return source[key] if key in source else missing(f"process did not export {key}")

    def peak(key):
        return value(key, peaks)

    def payload_peak(key):
        values = [source[key] for source in (payload_peaks, payload) if key in source]
        if values and all(type(item) in (int, float) for item in values):
            return max(values)
        return value(key, payload_peaks if key in payload_peaks else payload)

    def hlod_peak(key):
        return peak(key) if route == "hlod" else {"measured": False, "applicable": False, "scope": "chunk",
            "reason": "chunk route has no HLOD node-residency owner"}

    resident = value("hlod_stream_drawn_nodes") if route == "hlod" else missing(
        "chunk route exposes resident chunks, not GPU-resident drawn nodes")
    mode = world.get("mode")
    fallback = (mode == "resident_only") if isinstance(mode, str) else missing("no world payload mode readback")
    native_queues = payload.get("queue_observations") or {}
    queues = {}
    for owner in ("retained", "ready"):
        queues[owner] = copy.deepcopy(native_queues[owner]) if owner in native_queues else {
            "bytes": value(f"{owner}_bytes", payload), "peak_bytes": value(f"{owner}_bytes_peak", payload),
            "byte_bound": missing(f"process did not export {owner} byte bound"),
            "jobs": missing(f"process did not export {owner} jobs"),
            "peak_jobs": missing(f"process did not export {owner} job high-water mark"),
            "job_bound": missing(f"process did not export {owner} job bound"),
        }
    failures = {
        "stale_generation_publications": payload_peak("stale_generation_publications"),
        "nonresident_drawn_nodes": hlod_peak("hlod_stream_nonresident_drawn"),
        "overflow_events": peak("instance_count_overflow_events"),
        "drop_events": missing("drop totals require native per-frame summary"),
        "digest_verification_failures": payload_peak("digest_verification_failures"),
        "hlod_stream_failed_total": hlod_peak("hlod_stream_failed_total"),
        "hlod_stream_late_dropped_total": hlod_peak("hlod_stream_late_dropped_total"),
    }
    return {"settings": process_settings(result), "source_generation": result.get("source_generation"),
            "final_marker": result.get("ok") is True and result.get("reason") == "plan completed",
            "exit_code": exit_code,
            "runtime": {"hlod_stream_paged": value("hlod_stream_paged"),
                        "hlod_stream_page_store_active": value("hlod_stream_page_store_active"),
                        "resident_materialized_fallback": fallback, "gpu_resident_drawn_nodes": resident},
            "failures": failures, "queues": queues,
            "saturation": {"reads_refused_busy": payload_peak("reads_refused_busy"),
                           "admission_waits": payload_peak("admission_waits"),
                           "upload_hold_frames": (payload_peak("upload_hold_frames") if route == "hlod" else {
                               "measured": False, "applicable": False, "scope": "chunk",
                               "reason": "chunk route has no HLOD upload-hold test seam"})},
            "convergence": {"hlod_stream_stall_frames": hlod_peak("hlod_stream_stall_frames"),
                            "hlod_stream_blocked_groups": hlod_peak("hlod_stream_blocked_groups")},
            "forward_progress": forward_progress_windows(result)}


def cold_prepare(world: Path) -> dict:
    paths = [world, world.with_suffix(".manifest.json")]
    try:
        dropped = wc.drop_from_page_cache(paths)
        residency = wc.residency(paths)
        return {"label": "cold" if residency.get("cold") is True else "warm",
                "independently_prepared": residency.get("cold") is True, "repeat_label": "repeat",
                "dropped_bytes": dropped, "residency": residency,
                "host_cache": "present (guest-cold only; host cache is not disproved)"}
    except (OSError, wc.CacheError) as exc:
        return {"label": "NOT_RUN", "independently_prepared": False, "repeat_label": "repeat", "reason": str(exc)}


def native_metrics(result: dict, run: dict, trace, route: str, probe: dict) -> dict:
    """Expose existing measurements; do not replace an absent route counter by 0."""
    stats = result.get("final_render_stats", {})
    payload = result.get("payload_telemetry") or {}
    checkpoints = result.get("checkpoints", [])
    segment_kinds = {(entry.get("pass_index"), entry.get("id")): entry.get("kind")
                     for entry in result.get("segments", [])}
    convergence = {"stop": [], "teleport": []}
    for cp in checkpoints:
        kind = segment_kinds.get((cp.get("pass_index"), cp.get("segment")))
        if kind in convergence:
            convergence[kind].append({"checkpoint": cp.get("checkpoint"), "pass_index": cp.get("pass_index"),
                                      "settle": cp.get("settle"), "actual_pose": cp.get("pose"),
                                      "hlod_desired_nodes": cp.get("hlod_desired_nodes"),
                                      "hlod_resident_nodes": cp.get("hlod_resident_nodes"),
                                      "scope": "route diagnostic observation; no G6/G6t acceptance claim"})
    metrics = {
        "os_peak_rss_bytes": run.get("rss", {}).get("peak_rss_bytes"),
        "device_vram_ledger": stats.get("device_vram_ledger"),
        "gpu_memory_usage_mb": missing("#1441 renderer placeholder is not measured VRAM"),
        "stop_convergence": convergence["stop"] or missing("no stop checkpoint observations"),
        "teleport_convergence": convergence["teleport"] or missing("no teleport checkpoint observations"),
        "drawn_splats": ([{"checkpoint": cp.get("checkpoint"), "pass_index": cp.get("pass_index"),
                            "drawn_splats": cp.get("drawn_splats")} for cp in checkpoints]
                          or missing("no checkpoint drawn observations")),
        "drawn_fraction": missing("HLOD cut-selection denominator has no exported acceptance oracle"),
        "cancellations": payload.get("cancellations", stats.get("hlod_stream_cancelled_total",
            missing("route does not export cancellation count"))),
        "unique_bytes": missing("uploads have no common unique-payload identity observation"),
        "staging_allowance_bytes": payload.get("staging_allowance_bytes",
            missing("route does not export upload staging allowance")),
        "retained_completion_peak_bytes": payload.get("retained_completion_peak_bytes",
            missing("transport completion retention high-water mark is not exported")),
        "oversized_node_events": payload.get("oversized_node_events",
            missing("oversized reservation event count is not exported")),
    }
    first = (probe.get("passes") or [{}])[0]
    if route == "chunk":
        fraction = first.get("moving_coverage", {}).get("drawn_fraction", {})
        metrics["drawn_fraction"] = (ps.metric(fraction) if fraction.get("status") == ps.OK
                                     else ps.missing(fraction.get("reason", "no drawn fraction")))
        metrics["total_bytes"] = probe.get("streaming_whole_run", {}).get("upload_bytes", missing("no upload bytes"))
    stored, wall = payload.get("stored_bytes_read"), run.get("wall_s")
    metrics["throughput_bytes_s"] = ({"status": ps.OK, "value": stored / wall,
        "scope": "achieved stored payload bytes per whole probe process wall second, including holds; not IO service bandwidth"}
        if type(stored) in (int, float) and type(wall) in (int, float) and wall > 0
        else missing("no stored bytes and whole-process wall time"))
    metrics["first_complete_frame"] = missing("no trace row proves route completeness")
    if trace is not None:
        complete = []
        for row in trace.rows:
            if trace.get(row, "drawn_splats") <= 0:
                continue
            if route == "chunk":
                ready = (trace.get(row, "render_ready") == 1 and trace.get(row, "needed_not_resident_chunks") == 0
                         and trace.get(row, "needed_chunks") > 0)
            else:
                names = ("hlod_stream_desired_nodes", "hlod_stream_resident_nodes", "hlod_stream_loading_nodes",
                         "hlod_stream_nonresident_drawn", "hlod_stream_failed_nodes", "hlod_stream_drawn_nodes",
                         "hlod_active", "hlod_stream_blocked_groups", "hlod_stream_fragmented_groups")
                ready = all(name in trace.index for name in names)
                if ready:
                    ready = (trace.get(row, names[0]) == 0
                             and all(trace.get(row, name) == 0 for name in names[2:5]) and trace.get(row, names[5]) > 0
                             and trace.get(row, names[6]) == 1 and all(trace.get(row, name) == 0 for name in names[7:]))
            if ready:
                complete.append(trace.get(row, "engine_frame"))
        if complete:
            metrics["first_complete_frame"] = {"status": ps.OK, "value": complete[0],
                                               "scope": "first observed route-ready trace row; diagnostic convergence only"}
    return metrics


def run_arm(binary: Path, world: Path, route: str, prepared: dict, path: dict, truth: dict,
            manifest: dict, out: Path, process=rp.run_godot, prepare_cold=cold_prepare,
            expected_commit: str | None = None, control: str = "none") -> dict:
    out.mkdir(parents=True, exist_ok=True)
    plan = rp.path_plan(world, path, passes=2, screenshots={"dir": (out / "screenshots").as_posix(),
                                                         "moving_every_n": 100})
    plan["shared_layers_t0"] = True
    if control == "saturate":
        # The only caller setting this is the additional, post-measurement
        # control arm. The probe binds the existing TESTS_ENABLED upload hold
        # and waits for its native ready-work observation before releasing it.
        plan["shared_layers_control"] = control
    # Both routes use precisely the same runtime policy. D2's provisional 1M
    # builder oracle budget must not silently become a runtime setting.
    plan["node"]["lod_enabled"] = True
    plan["node"]["hlod_tau_px"] = 0.0
    plan["node"]["lod_bias"] = 1.0
    request = requested_settings(plan, prepared)
    cache = prepare_cold(world)
    run = process(binary, plan, out, 1800.0)
    result = run.get("result") or {}
    trace, integrity = rp.trace_integrity(run)
    probe = (rp.summarise_path(trace, run, path, truth, 2, manifest) if trace is not None else {
        "payload_telemetry": ps.route_telemetry_summary(result.get("payload_telemetry"),
            route=result.get("route"), source_generation=result.get("source_generation")),
        "screenshots": rp.screenshot_record(result.get("screenshots"))})
    settings_samples = float32_values(result.get("shared_layers_settings_samples"))
    if route == "hlod" and settings_samples is not None and trace is not None:
        # HLOD has no chunk VRAM regulator revision. Preserve that raw absence,
        # then use actual per-row node/camera/project readbacks for this paired
        # diagnostic's settings invariance instead.
        for measured_pass in probe.get("passes", []):
            measured_pass["chunk_settings_measured"] = measured_pass.get("settings_measured")
            measured_pass["settings_measured"] = ps.shared_layers_settings_summary(
                settings_samples, ps.measured_rows(trace, measured_pass["pass_index"]))
    observed = observations(result, run.get("exit_code"))
    observed["camera_application"] = camera_application(trace, path)
    observed["actual_checkpoint_poses"] = [{"pass_index": cp.get("pass_index"), "checkpoint": cp.get("checkpoint"),
                                            "pose": cp.get("pose")} for cp in result.get("checkpoints", [])]
    observed["settings_scope"] = result.get("shared_layers_settings_scope", {})
    if route == "chunk" and trace is not None:
        resident_rows = [row for row in trace.rows if trace.get(row, "render_ready") == 1
                         and trace.get(row, "needed_not_resident_chunks") == 0
                         and trace.get(row, "needed_chunks") > 0 and trace.get(row, "drawn_splats") > 0]
        observed["runtime"]["gpu_resident_drawn_splats"] = (max(trace.get(row, "drawn_splats") for row in resident_rows)
            if resident_rows else missing("no drawn trace row with render readiness and all needed chunks GPU resident"))
        observed["runtime"]["gpu_residency_scope"] = "drawn_splats in actual trace rows with render_ready=1 and needed_not_resident_chunks=0"
    runtime_hash = result.get("engine_version", {}).get("hash")
    if expected_commit is not None and runtime_hash != expected_commit:
        # The build-info digest proves the artifact; the process's own engine
        # version independently proves what actually ran.
        observed["final_marker"] = False
    drops = probe.get("overlap_drops_whole_run", {})
    if drops.get("status") == ps.OK:
        observed["failures"]["drop_events"] = drops.get("drop_events", missing("native summary omitted drop_events"))
    source = {key: copy.deepcopy(prepared[key]) for key in (
        "input_file_sha256", "generator_identity", "codec", "tree_file_sha256", "tree_identity")}
    source["loaded_file_sha256"] = result.get("loaded_file_sha256")
    arm = {"execution": "RAN", "source": source, "requested": request, "observed": observed,
           "probe": probe, "trace": {"file": run["trace"], "integrity": integrity},
           "artifact_root": str(out), "cache": cache,
           "optional_measurements": native_metrics(result, run, trace, route, probe),
           "native": {"result": str(out / "result.json"), "process_failure": rp.process_failure(run),
                      "payload_telemetry": copy.deepcopy(result.get("payload_telemetry")),
                      "shared_layers_control": copy.deepcopy(result.get("shared_layers_control")),
                      "shared_layers_settings_samples": settings_samples,
                      "shared_layers_payload_peaks": copy.deepcopy(result.get("shared_layers_payload_peaks")),
                      "forward_progress": copy.deepcopy(observed["forward_progress"]),
                      "cold_pass_io": rp.cold_pass_io(result), "requested_route": route,
                      "engine_version": result.get("engine_version"), "expected_binary_commit": expected_commit,
                      "binary_identity_matches": runtime_hash == expected_commit if expected_commit is not None else None},
           "payload_behavior": payload_behavior(result, trace)}
    write_json(out / "arm.json", arm)
    return arm


def payload_behavior(result: dict, trace) -> dict:
    stats = result.get("final_render_stats", {})
    # All three flags exist on immutable pre-D3. Resource shape alone is not
    # proof that the process served HLOD streaming rather than a fallback.
    actual_hlod = all(stats.get(key) is True for key in ("hlod_streaming", "hlod_active", "hlod_stream_paged"))
    actual_chunk = (result.get("route") == "chunk" and isinstance(stats.get("streaming_state"), dict))
    return {"route": "hlod" if actual_hlod else "chunk" if actual_chunk else None,
            "route_evidence": {key: stats.get(key) for key in ("hlod_streaming", "hlod_active", "hlod_stream_paged")},
            "loaded_file_sha256": result.get("loaded_file_sha256"),
            "world_payload": copy.deepcopy(result.get("world_payload_readback")),
            "native_payload": {key: copy.deepcopy(value) for key, value in stats.items()
                               if key.startswith("hlod_stream_")},
            "decoded_bytes": missing("no route-common decoded byte counter exists on pre-D3 and candidate"),
            "decoded_digest": missing("immutable pre-D3 exposes no decoded-payload digest"),
            "decoded_splats": missing("immutable pre-D3 exposes no decoded-splat counter"),
            "decoded_pages": missing("resident atlas pages are not decoded payload pages; pre-D3 has no decode-page counter"),
            "stored_bytes_read": stats.get("hlod_stream_payload_bytes_read_total",
                                             missing("HLOD stored-byte counter absent")),
            "drawn_counts": [{"checkpoint": cp.get("checkpoint"), "pass_index": cp.get("pass_index"),
                              "drawn_splats": cp.get("drawn_splats"), "drawn_nodes": cp.get("hlod_drawn_nodes")}
                             for cp in result.get("checkpoints", [])],
            "trace_drawn_splats_max": max(trace.col("drawn_splats")) if trace and trace.rows else missing("no trace"),
            "D3_5_payload_stages": missing("not a pre-D3 common measurement; use per-route candidate summary")}


def migration_arm(arm: dict, identity: dict) -> dict:
    return {"execution": arm["execution"], "binary_commit": identity["commit"],
            "binary_sha256": identity["sha256"], "exit_code": arm["observed"]["exit_code"],
            "final_marker": arm["observed"]["final_marker"], "trace_file": arm["trace"]["file"],
            "input_file_sha256": arm["source"]["loaded_file_sha256"],
            "settings_readback": arm["observed"]["settings"], "payload_behavior": arm["payload_behavior"],
            "camera_application": arm["observed"].get("camera_application"),
            "actual_checkpoint_poses": arm["observed"].get("actual_checkpoint_poses"),
            "settings_scope": arm["observed"].get("settings_scope"), "native": arm.get("native"),
            "cache": arm["cache"], "legacy_unmeasured": missing(
                "pre-D3 has no D3-5 payload telemetry or queue/publication exports; all arms use shipped capacity settings")}


def capture_arm(binary: Path, world: Path, route: str, prepared: dict, path: dict, truth: dict,
                manifest: dict, out: Path, expected_commit: str, control: str = "none") -> dict:
    """A failed arm is recorded independently; later arms still get to run."""
    try:
        return run_arm(binary, world, route, prepared, path, truth, manifest, out,
                       expected_commit=expected_commit, control=control)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, ps.TraceError) as exc:
        artifact = out / "result.json"
        arm = {"execution": "RAN" if artifact.exists() else "NOT_RUN",
               "source": {"loaded_file_sha256": None},
               "observed": {"exit_code": None, "final_marker": False, "settings": {}},
               "requested": {}, "probe": {}, "trace": {"file": str(out / "trace.bin"), "integrity": {}},
               "artifact_root": str(out), "cache": {"label": "NOT_RUN", "independently_prepared": False},
               "optional_measurements": {}, "payload_behavior": {}, "orchestration_error": str(exc)}
        write_json(out / "arm.json", arm)
        return arm


def corrupt_copy(source: Path, target: Path) -> dict:
    """Flip a stored-payload byte in EVERY V4 node, preserving index/header.

    Index entries and their digests are untouched, so a demanded node must reach
    the real stored-byte verification path. Never modify the clean input used
    for either measured route or the migration control.
    """
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    shutil.copyfile(source.with_suffix(".manifest.json"), target.with_suffix(".manifest.json"))
    mutations = []
    with target.open("r+b") as stream:
        header = stream.read(4096)
        if len(header) != 4096 or struct.unpack_from("<II", header) != (0x57505347, 4):
            raise ValueError("corruption control requires a V4 file")
        node_count = struct.unpack_from("<I", header, 40)[0]
        if not 0 < node_count <= 1 << 20:
            raise ValueError("corruption control requires a bounded nonempty node directory")
        for index in range(node_count):
            entry = 4096 + (index // 255) * 65536 + 64 + (index % 255) * 256
            stream.seek(entry + 128)
            offset, stored, _extent = struct.unpack("<3Q", stream.read(24))
            if stored <= 64 or offset + stored > target.stat().st_size:
                raise ValueError("corruption control encountered an invalid payload extent")
            position = offset + stored - 1
            stream.seek(position)
            original = stream.read(1)
            stream.seek(position)
            stream.write(bytes([original[0] ^ 1]))
            mutations.append({"node": index, "offset": position})
    return {"status": "FORCED", "source_sha256": sha256(source), "corrupt_sha256": sha256(target),
            "mutations": mutations, "forcing": "stored payload byte flip; header and node-directory digests unchanged"}


def run(repo: Path, binary: Path, base_binary: Path, base_build_info: Path, cache_root: Path,
        out: Path, run_id: str, control: str = "none") -> int:
    import dispatch_lane as lane

    out.mkdir(parents=True, exist_ok=True)
    experiment = {"schema": SCHEMA, "run_id": run_id, "control": control, "prepared": {},
                  "arms": {}, "migration": {"base_commit": PRE_D3, "candidate_commit": None, "arms": {}}}
    write_json(out / "experiment.json", experiment)
    try:
        candidate_commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                                          capture_output=True, text=True, check=True).stdout.strip()
        candidate = verified_binary(binary, out / "build_info.json", candidate_commit)
        base = verified_binary(base_binary, base_build_info, PRE_D3)
        experiment["migration"]["candidate_commit"] = candidate_commit
        cache_root.parent.mkdir(parents=True, exist_ok=True)
        # Same runner cache lock/scratch parent convention as D2. No cached key
        # or real-scan path is read, written, copied into artifacts or uploaded.
        with wc.cache_lock(cache_root, exclusive=True), tempfile.TemporaryDirectory(
                prefix="bvw-shared-layers-t0-", dir=cache_root.parent) as scratch:
            paths = lane.prepare_auto_tree_t0(repo, binary, Path(scratch), out / "preparation")
            plain, tree = paths["plain"], paths["tree"]
            prepared = prepared_receipt(repo, plain, tree, paths["report"])
            experiment["prepared"] = prepared
            manifest = read_json(plain.with_suffix(".manifest.json"))
            path = rp.probe_path.resolve_path(manifest)
            truth = rp.probe_groundtruth.build_groundtruth(manifest, path, rp.probe_path.checkpoints(path), rp.CAMERA, rp.ASPECT)
            write_json(out / "path.resolved.json", path)
            write_json(out / "world.groundtruth.json", truth)
            for route, world in (("chunk", plain), ("hlod", tree)):
                experiment["arms"][route] = capture_arm(binary, world, route, prepared, path, truth, manifest, out / route,
                                                         expected_commit=candidate_commit)
                write_json(out / "experiment.json", experiment)
            for name, executable, identity in (("base", base_binary, base), ("candidate", binary, candidate)):
                arm = capture_arm(executable, tree, "hlod", prepared, path, truth, manifest, out / "migration" / name,
                                  expected_commit=identity["commit"])
                experiment["migration"]["arms"][name] = migration_arm(arm, identity)
                write_json(out / "experiment.json", experiment)
            # Negative controls execute only AFTER every independently prepared
            # cold measurement; all measured source receipts remain clean.
            if control == "corrupt":
                corrupt = Path(scratch) / "corrupt" / "world.gsplatworld"
                receipt = corrupt_copy(tree, corrupt)
                arm = capture_arm(binary, corrupt, "hlod", prepared, path, truth, manifest, out / "controls" / "corrupt",
                                  expected_commit=candidate_commit)
                receipt["arm"] = arm
                receipt["observed"] = {"process_failure": arm.get("native", {}).get("process_failure", arm.get("orchestration_error")),
                                       "payload_telemetry": arm.get("native", {}).get("payload_telemetry")}
                experiment["control_receipt"] = receipt
            elif control == "saturate":
                arm = capture_arm(binary, tree, "hlod", prepared, path, truth, manifest,
                                  out / "controls" / "saturate", expected_commit=candidate_commit, control="saturate")
                native_control = arm.get("native", {}).get("shared_layers_control") or {}
                experiment["control_receipt"] = {
                    "status": "FORCED" if native_control.get("hook_applied") is True else "FAILED",
                    "forcing": "TESTS_ENABLED HlodNodeStreamer upload hold, applied only in this additional probe",
                    "arm": arm, "observed": copy.deepcopy(native_control)}
            elif control == "skip_compare":
                experiment["control_receipt"] = {"status": "FORCED", "reason": "workflow omits comparison invocation; separate final verdict requires its report"}
        write_json(out / "experiment.json", experiment)
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, lane.LaneError, wc.CacheError) as exc:
        experiment["orchestration_error"] = str(exc)
        write_json(out / "experiment.json", experiment)
        print(f"shared-layers-t0: NOT_RUN/UNUSABLE: {exc}")
        return 3
