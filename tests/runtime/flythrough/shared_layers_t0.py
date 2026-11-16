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
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import probe_summary as ps
import run_probe as rp
import world_cache as wc
import verify_world_manifest as vm

# Immutable provenance base: original pre-#1453 source
# f2f19bcb580cc131615b3fa262a11d99e6abc70c plus only #1453's count provenance.
PRE_D3 = "5a438363cd6939bca1041d738e2bfa10f2b024f2"
SCHEMA = "godotgs-shared-layers-t0-experiment/1"
# Viewport readbacks stall the single GPU runner; quarter the moving captures.
# The probe still captures every checkpoint independently of this cadence.
MOVING_SCREENSHOT_EVERY_N = 400

# #1471: the unconstrained T0 keeps the whole HLOD tree resident, so eviction, budget-bound
# selection and teleport convergence cannot fail there. The pressure variant caps the streamed
# atlas and the drawn budget through settings that already exist; it adds no renderer knob.
VARIANTS = ("unconstrained", "pressure")
# Read by the HLOD publisher for the streamed atlas (DeviceVramLedger::atlas_limit); whole MiB.
PRESSURE_ATLAS_SETTING = "rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb"
PRESSURE_ATLAS_FRACTION = (0.20, 0.25)  # Of the tree's pages (#1471).
PRESSURE_DRAWN_BUDGET_SPLATS = 1_000_000  # SCRATCH #1483 A 1M proof only; never merged.
PRESSURE_PAGE_SPLATS = 1024  # HlodNodeStreamer::Config::DEFAULT_PAGE_SPLATS
PRESSURE_RECORD_BYTES = 176  # sizeof(PackedGaussian); the lane keeps per-chunk quantization off.


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


def content_references(plain: Path, source: dict, derived: dict, w13: dict) -> dict:
    """Read references from preparation, independently of every runtime arm.

    Chunk keys are GSQ page-table ordinals. Tree keys are V4 node ordinals;
    W13 produced each sidecar digest from its separate reopened-node decode.
    These maps have distinct partitions and do not assert cross-route equality.
    The preparation pipeline verifies both files before this extraction.
    """
    nodes = derived.get("nodes")
    if (not isinstance(nodes, list) or not nodes or w13.get("available") is not True
            or w13.get("passed") is not True or type(w13.get("nodes_verified")) is not int
            or type(w13.get("node_count")) is not int
            or w13["nodes_verified"] != len(nodes) or w13["node_count"] != len(nodes)):
        raise ValueError("content reference requires complete independent W13 node verification")
    hlod = {}
    for index, node in enumerate(nodes):
        digest = node.get("decoded_sha256") if isinstance(node, dict) else None
        if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
            raise ValueError(f"content reference has invalid HLOD node digest: {index}")
        hlod[str(index)] = digest
    codec = source.get("codec", {})
    with plain.open("rb") as stream:
        header = stream.read(vm.PREFIX_BYTES + vm.PAGED_EXTENSION_BYTES)
        if len(header) != vm.PREFIX_BYTES + vm.PAGED_EXTENSION_BYTES:
            raise ValueError("content reference GSQ header truncated")
        # Use the complete GSQ extension layout used by the independent
        # verifier: page_count precedes alignment; the table offset follows it.
        extension = struct.unpack_from("<I4BfiIIQ", header, vm.PREFIX_BYTES)
        count, alignment, offset = extension[-3:]
        if (extension[0] != vm.CODEC_GSQ_V1 or alignment != vm.PAGE_ALIGNMENT
                or type(codec.get("page_count")) is not int or count != codec["page_count"] or count <= 0
                or offset < len(header) or offset + count * vm.PAGED_PAGE_RECORD_BYTES > plain.stat().st_size):
            raise ValueError("content reference GSQ page directory invalid")
        stream.seek(offset)
        chunk, digest_set = {}, hashlib.sha256()
        for index in range(count):
            record = stream.read(vm.PAGED_PAGE_RECORD_BYTES)
            if len(record) != vm.PAGED_PAGE_RECORD_BYTES:
                raise ValueError("content reference GSQ page directory truncated")
            digest = record[48:80]
            digest_set.update(digest)
            chunk[str(index)] = digest.hex()
    if digest_set.hexdigest() != codec.get("decoded_digest_set_sha256"):
        raise ValueError("content reference GSQ decoded digest set differs from prepared manifest")
    return {"chunk": chunk, "hlod": hlod}


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
        "content_references": content_references(plain, source, derived, report["w13"]),
        "content_reference_sources": {
            "algorithm": "gs_gsq::decoded_digest SHA-256 (decoded gaussians and SH)",
            "chunk": "prepared GSQ page-directory decoded digest, bound by the verified manifest digest set",
            "hlod": "independent prepare/W13 reopened V4 node digests from the tree sidecar",
            "scope": "per-route page/node content against preparation; no exact cross-route partition correspondence"},
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
            "static_sort_cache": copy.deepcopy(result.get("static_sort_cache")),
            "checkpoint_evidence": ps.checkpoint_evidence_summary([
                ps.checkpoint_evidence(cp, route=route, static_sort_cache=result.get("static_sort_cache"))
                for cp in result.get("checkpoints", [])]),
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


def sync_cut_measurements(result: dict) -> dict:
    """Report native cut waits, including absent/incomplete event coverage honestly."""
    source = result.get("overlap_sync_cuts", {})
    absent = {"status": "not_measured", "reason": "sync-cut counter or complete per-cut observations unavailable"}
    if not isinstance(source, dict):
        return absent
    count, events = source.get("count"), source.get("events")
    if type(count) is not int or count < 0 or not isinstance(events, list) or len(events) != count:
        return absent
    for index, event in enumerate(events, 1):
        if (not isinstance(event, dict) or event.get("count") != index
                or type(event.get("frame_serial")) is not int or event["frame_serial"] <= 0
                or type(event.get("cpu_wall_ms")) not in (int, float)
                or not math.isfinite(event["cpu_wall_ms"]) or event["cpu_wall_ms"] < 0):
            return absent
    value = {"count": count, "frames": [e["frame_serial"] for e in events],
            "windows": [e.get("window", "not_measured") for e in events],
            "cpu_wall_ms_per_cut": [e["cpu_wall_ms"] for e in events], "events": copy.deepcopy(events)}
    return {"status": ps.OK, "value": value, **value}


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
        "overlap_sync_cuts": sync_cut_measurements(result),
        "os_peak_rss_bytes": run.get("rss", {}).get("peak_rss_bytes"),
        # #1466 (report-only): where the peak sits. Absent sampler or clock data stays not measured.
        "os_rss_timeline": run.get("rss", {}).get("timeline", missing("the RSS sampler exported no timeline")),
        "os_rss_peak_location": ps.rss_peak_location(run.get("rss"), result, trace),
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
        "content_digests": copy.deepcopy(payload.get("content_digests")),
        # #1457/#1461 (report-only): the sampled dropping frames (per-tile record maximum, its
        # tile, the record allocation) and the checkpoint drops split into settle window and
        # settled pose. Neither replaces the drop_events failure; absent values stay not measured.
        "overlap_drop_frames": ps.drop_frame_summary(result.get("overlap_drop_frames")),
        "checkpoint_drop_split": ps.checkpoint_drop_split(probe.get("passes")),
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


def tree_page_count(tree: Path) -> int:
    """Atlas pages the whole V4 tree needs: every node's splats rounded up to pages.

    Read from the file's node directory (the layout corrupt_copy and verify_v4 read), so the
    pressure cap's denominator is independent of anything the runtime reports.

    The directory offsets are this harness's assumption about the writer. They are checked on
    every run against the producer's own sidecar totals (node count and leaf + interior splat
    count): a wrong offset fails closed here instead of producing a wrong cap."""
    totals = read_json(tree.with_suffix(".manifest.json")).get("totals")
    expected = [totals.get(key) for key in ("node_count", "leaf_splat_count", "interior_splat_count")] \
        if isinstance(totals, dict) else []
    if len(expected) != 3 or any(type(value) not in (int, float) or value != int(value) or value < 0 for value in expected):
        raise ValueError("pressure variant: the tree sidecar carries no producer totals to check the directory against")
    pages = splat_sum = 0
    with tree.open("rb") as stream:
        header = stream.read(4096)
        if len(header) != 4096 or struct.unpack_from("<II", header) != (0x57505347, 4):
            raise ValueError("pressure variant requires a V4 tree")
        node_count = struct.unpack_from("<I", header, 40)[0]
        if not 0 < node_count <= 1 << 20:
            raise ValueError("pressure variant requires a bounded nonempty node directory")
        for index in range(node_count):
            stream.seek(4096 + (index // 255) * 65536 + 64 + (index % 255) * 256 + 88)
            entry = stream.read(12)
            if len(entry) != 12:
                raise ValueError("pressure variant: V4 node directory truncated")
            splats = struct.unpack("<QI", entry)[1]
            if splats <= 0:
                raise ValueError(f"pressure variant: V4 node {index} has no payload")
            splat_sum += splats
            pages += (splats + PRESSURE_PAGE_SPLATS - 1) // PRESSURE_PAGE_SPLATS
    if node_count != expected[0] or splat_sum != expected[1] + expected[2]:
        raise ValueError(f"pressure variant: V4 directory read ({node_count} nodes, {splat_sum} splats) differs from the "
                         f"producer sidecar totals ({expected[0]} nodes, {expected[1] + expected[2]} splats)")
    return pages


def pressure_request(tree: Path) -> dict:
    """The pressure variant's settings for this tree. A request, never an observation: the
    comparison requires the process readback and the native atlas capacity separately."""
    low, high = PRESSURE_ATLAS_FRACTION
    total = tree_page_count(tree)
    page_bytes = PRESSURE_PAGE_SPLATS * PRESSURE_RECORD_BYTES
    megabytes = int((low + high) / 2 * total * page_bytes) >> 20
    pages = (megabytes << 20) // page_bytes
    if megabytes <= 0 or not low * total <= pages <= high * total:
        raise ValueError(f"pressure variant: no whole-MiB atlas override caps a {total}-page tree to "
                         f"{low:.0%}-{high:.0%} of its pages")
    return {"name": "pressure", "issue": "#1471", "atlas_setting": PRESSURE_ATLAS_SETTING,
            "atlas_override_mb": megabytes, "tree_pages": total, "requested_atlas_pages": pages,
            "atlas_fraction_band": [low, high], "drawn_budget_splats": PRESSURE_DRAWN_BUDGET_SPLATS,
            "page_splats": PRESSURE_PAGE_SPLATS, "record_bytes": PRESSURE_RECORD_BYTES,
            "tree_pages_source": "V4 node directory of the prepared tree, read before any arm runs",
            "chunk_arm": {"role": "unconstrained_reference", "reason": (
                "every arm gets the same settings and the same 250k node cap, but the atlas override is read "
                "only by the resident/HLOD publisher: the chunk streaming system has its own VRAM regulator "
                "and no page cap that corresponds to a fraction of an HLOD tree, so no equivalent chunk "
                "constraint is claimed and no pressure evidence is required of the chunk arm")}}


def pressure_observations(result: dict, trace, route: str, pressure: dict) -> dict:
    """What the process reported about pressure. An absent counter stays not measured."""
    stats = result.get("final_render_stats", {})
    payload = result.get("payload_telemetry") or {}

    def stat(key):
        return stats[key] if key in stats else missing(f"process did not export {key}")

    kinds = {(entry.get("pass_index"), entry.get("id")): entry.get("kind") for entry in result.get("segments", [])}
    settles = []
    for cp in result.get("checkpoints", []):
        kind = kinds.get((cp.get("pass_index"), cp.get("segment")))
        if kind not in ("stop", "teleport"):
            continue
        settle = cp.get("settle") if isinstance(cp.get("settle"), dict) else {}
        progress = settle.get("forward_progress") if isinstance(settle.get("forward_progress"), dict) else {}
        usec = settle.get("settle_usec")
        row = {"checkpoint": cp.get("checkpoint"), "pass_index": cp.get("pass_index"), "kind": kind,
               "settled": settle.get("settled", missing("checkpoint has no settle receipt")),
               "settle_ms": (usec / 1000.0 if type(usec) in (int, float) and usec >= 0
                             else missing("no settle time: the window never settled or was not captured")),
               "streaming_work": (copy.deepcopy(progress["progress_deltas"])
                                  if progress.get("measured") is True and isinstance(progress.get("progress_deltas"), dict)
                                  else missing(progress.get("reason", "producer did not capture this settle window's progress"))),
               "drawn_splats": cp.get("drawn_splats", missing("checkpoint has no drawn count")),
               "drawn_nodes": cp.get("hlod_drawn_nodes", missing("checkpoint has no drawn-node count"))}
        # #1483: report-only, never one of the pressure acceptance requirements.
        row["hlod_pressure"] = copy.deepcopy(cp.get("hlod_pressure", missing("probe has no pressure diagnostics")))
        if "frames" in settle:
            row["frames"] = settle["frames"]
        settles.append(row)
    out = {"route": route, "settles": settles,
           "scope": "final native counters and each stop/teleport checkpoint's settle receipt; report inputs only"}
    if route != "hlod":
        out.update(constraint="not_applied", role=pressure["chunk_arm"]["role"], reason=pressure["chunk_arm"]["reason"])
        evicted = (trace.col("chunks_evicted_this_frame")
                   if trace is not None and "chunks_evicted_this_frame" in trace.index else None)
        out["evicted_chunks"] = (sum(evicted) if evicted and all(value >= 0 for value in evicted)
                                 else missing("no complete per-frame chunk eviction column in the trace"))
        return out
    reads, digests = stats.get("hlod_stream_payload_reads_total"), payload.get("content_digests")
    out.update(constraint="applied", atlas_pages_capacity=stat("hlod_stream_pages_capacity"),
               pages_resident_final=stat("hlod_stream_pages_resident"),
               evicted_total=stat("hlod_stream_evicted_total"),
               budget_bound_frames=stat("hlod_budget_bound_frames"),
               payload_reads_total=stat("hlod_stream_payload_reads_total"),
               rereads=(max(reads - len(digests), 0) if type(reads) is int and isinstance(digests, dict) and digests
                        else missing("needs the native read total and the decoded-node digest map")),
               rereads_scope="payload reads beyond one per distinct decoded node")
    return out


def run_arm(binary: Path, world: Path, route: str, prepared: dict, path: dict, truth: dict,
            manifest: dict, out: Path, process=rp.run_godot, prepare_cold=cold_prepare,
            expected_commit: str | None = None, control: str = "none", pressure: dict | None = None) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    plan = rp.path_plan(world, path, passes=2, screenshots={"dir": (out / "screenshots").as_posix(),
                                                         "moving_every_n": MOVING_SCREENSHOT_EVERY_N})
    plan["shared_layers_t0"] = True
    plan["project_settings"]["rendering/gaussian_splatting/diagnostics/payload_content_digests"] = True
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
    if pressure is not None:
        # #1471 pressure variant only: an existing atlas limit and the node's drawn budget,
        # identical for every arm. Nothing else differs from the unconstrained plan.
        plan["project_settings"][pressure["atlas_setting"]] = pressure["atlas_override_mb"]
        plan["node"]["max_splat_count"] = pressure["drawn_budget_splats"]
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
           "content_digests": copy.deepcopy(result.get("payload_telemetry", {}).get("content_digests"))
                if isinstance(result.get("payload_telemetry"), dict) else None,
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
    if pressure is not None:
        arm["pressure"] = pressure_observations(result, trace, route, pressure)
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
            "content_digests": copy.deepcopy((result.get("payload_telemetry") or {}).get("content_digests")),
            "stored_bytes_read": stats.get("hlod_stream_payload_bytes_read_total",
                                             missing("HLOD stored-byte counter absent")),
            "drawn_counts": [{**ps.checkpoint_evidence(cp, route=result.get("route"),
                                                      static_sort_cache=result.get("static_sort_cache")),
                              "drawn_nodes": cp.get("hlod_drawn_nodes")}
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
            "checkpoint_evidence": copy.deepcopy(arm["observed"].get("checkpoint_evidence")),
            "static_sort_cache": copy.deepcopy(arm["observed"].get("static_sort_cache")),
            # #1445: the driver script is the same for both binaries, so the pre-D3 arm reports its
            # wall-clock and game-relevant frame times too (report-only).
            "frame_time": ps.frame_time_passes(arm.get("probe")),
            "cache": arm["cache"], "legacy_unmeasured": missing(
                "pre-D3 has no D3-5 payload telemetry or queue/publication exports; all arms use shipped capacity settings")}


def capture_arm(binary: Path, world: Path, route: str, prepared: dict, path: dict, truth: dict,
                manifest: dict, out: Path, expected_commit: str, control: str = "none",
                pressure: dict | None = None) -> dict:
    """A failed arm is recorded independently; later arms still get to run."""
    try:
        return run_arm(binary, world, route, prepared, path, truth, manifest, out,
                       expected_commit=expected_commit, control=control,
                       **({} if pressure is None else {"pressure": pressure}))
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
        out: Path, run_id: str, control: str = "none", variant: str = "unconstrained") -> int:
    import dispatch_lane as lane

    out.mkdir(parents=True, exist_ok=True)
    experiment = {"schema": SCHEMA, "run_id": run_id, "control": control, "prepared": {},
                  "arms": {}, "migration": {"base_commit": PRE_D3, "candidate_commit": None, "arms": {}}}
    write_json(out / "experiment.json", experiment)
    extra = {}  # Stays empty for the unconstrained variant: its arms and its record are unchanged.
    try:
        if variant not in VARIANTS:
            raise ValueError(f"unknown shared-layers variant: {variant}")
        if variant == "pressure" and control != "none":
            raise ValueError("the pressure variant runs without a negative control; dispatch the control unconstrained")
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
            if variant == "pressure":
                extra["pressure"] = pressure_request(tree)
                experiment["variant"] = copy.deepcopy(extra["pressure"])
                if isinstance(prepared.get("builder_oracle"), dict):
                    prepared["builder_oracle"]["scope"] = (
                        "D2 builder oracle only (1M); separate from the pressure variant's 250k runtime node cap")
            manifest = read_json(plain.with_suffix(".manifest.json"))
            path = rp.probe_path.resolve_path(manifest)
            truth = rp.probe_groundtruth.build_groundtruth(manifest, path, rp.probe_path.checkpoints(path), rp.CAMERA, rp.ASPECT)
            write_json(out / "path.resolved.json", path)
            write_json(out / "world.groundtruth.json", truth)
            for route, world in (("chunk", plain), ("hlod", tree)):
                experiment["arms"][route] = capture_arm(binary, world, route, prepared, path, truth, manifest, out / route,
                                                         expected_commit=candidate_commit, **extra)
                write_json(out / "experiment.json", experiment)
            for name, executable, identity in (("base", base_binary, base), ("candidate", binary, candidate)):
                arm = capture_arm(executable, tree, "hlod", prepared, path, truth, manifest, out / "migration" / name,
                                  expected_commit=identity["commit"], **extra)
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
