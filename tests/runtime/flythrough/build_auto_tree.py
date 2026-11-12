#!/usr/bin/env python3
"""Build a V4 auto tree and CPU oracle report with a tests-capable producer.

Usage: --godot-binary B --input W --job-manifest J --out V4 --report R
Exit 0: complete eight-pose oracle; 1: failed job (reason in R); 2: CLI usage.
Source identity is supplied by the job, not computed from a guessed TREE_SOURCES
list: D2-5 owns that list. The budget is provisional until BV-26a is merged.
Standard library only. Generated worlds stay outside the temporary res:// project.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import probe_path  # noqa: E402

JSON_LIMIT = 16 * 1024 * 1024
MAX_JSON_INTEGER = (1 << 53) - 1  # Godot's JSON reader uses doubles.
JOB_KEYS = {"schema", "codec", "worker_count", "max_tile_splats", "limits",
            "source_fingerprint", "source_fingerprint_source", "drawn_budget_splats",
            "drawn_budget_source", "oracle"}
LIMIT_KEYS = {"max_ram_bytes", "max_scratch_bytes", "max_output_bytes", "deadline_sec"}
CAMERA_KEYS = {"width", "height", "fov_deg", "keep", "near_m", "far_m", "tau_pixels"}
SELECT_STATUSES = {"COMPLETE", "EMPTY", "INVALID", "UNAVAILABLE", "BOOTSTRAP_EXHAUSTED",
                   "BOOTSTRAP_ARITHMETIC", "BOOTSTRAP_MISSING_RESIDENCY", "BOOTSTRAP_INFEASIBLE"}


class BuildError(ValueError):
    def __init__(self, reason):
        self.reason = reason
        super().__init__(reason)


def absent(reason):
    return {"available": False, "reason": reason}


def new_report():
    report = dict.fromkeys(("wall_time_sec", "output_bytes", "node_count", "height", "levels",
        "content_generation", "output_file_sha256", "tree_sha", "input_file_sha256",
        "job_manifest_sha256", "source_fingerprint", "source_fingerprint_source",
        "drawn_budget_splats", "drawn_budget_source", "oracle", "leaves_only"))
    report.update(schema="godotgs-auto-tree-build-report/1", status="failed", reason="not_started",
        budget_frozen=False, oracle_complete_all_poses=False,
        peak_rss_bytes=absent("process RSS not available"),
        scratch_bytes=absent("builder does not measure scratch usage; directory samples are not a peak"))
    return report


def _keys(value, expected):
    if type(value) is not dict:
        raise BuildError("job_manifest_type")
    if value.keys() - expected:
        raise BuildError("job_manifest_unknown_key")
    if expected - value.keys():
        raise BuildError("job_manifest_missing_key")


def _number(value, *, integer=False, low=0, high=MAX_JSON_INTEGER):
    if type(value) not in ((int,) if integer else (int, float)):
        raise BuildError("job_manifest_type")
    if not low < value <= high or not math.isfinite(value):
        raise BuildError("job_manifest_value")


def validate_job_manifest(job):
    _keys(job, JOB_KEYS)
    _keys(job["limits"], LIMIT_KEYS)
    _keys(job["oracle"], CAMERA_KEYS)
    for key in ("schema", "codec", "source_fingerprint", "source_fingerprint_source", "drawn_budget_source"):
        if type(job[key]) is not str:
            raise BuildError("job_manifest_type")
        if not job[key].strip():
            raise BuildError("job_manifest_value")
    if (job["schema"] != "godotgs-auto-tree-job/1" or job["codec"] not in ("none", "gsq")
            or re.fullmatch(r"[0-9a-f]{64}", job["source_fingerprint"]) is None):
        raise BuildError("job_manifest_value")
    _number(job["worker_count"], integer=True, high=64)
    _number(job["max_tile_splats"], integer=True, low=16384, high=(1 << 32) - 1)
    _number(job["drawn_budget_splats"], integer=True)
    for key, value in job["limits"].items():
        _number(value, integer=key != "deadline_sec")
    camera = job["oracle"]
    for key in ("width", "height"):
        _number(camera[key], integer=True, high=(1 << 31) - 1)
    if type(camera["keep"]) is not str:
        raise BuildError("job_manifest_type")
    for key in ("fov_deg", "near_m", "far_m", "tau_pixels"):
        _number(camera[key], high=1.0e100)
    if camera["keep"] != "width" or camera["fov_deg"] >= 180 or camera["far_m"] <= camera["near_m"]:
        raise BuildError("job_manifest_value")
    return job


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def _read_json(path, reason, *, with_sha=False):
    try:
        with path.open("rb") as stream:
            data = stream.read(JSON_LIMIT + 1)
        if len(data) > JSON_LIMIT:
            raise ValueError("JSON too large")
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
        return (value, hashlib.sha256(data).hexdigest()) if with_sha else value
    except (OSError, ValueError, RecursionError) as exc:
        raise BuildError(reason) from exc


def load_job_manifest(path, *, with_sha=False):
    if not path.is_file():
        raise BuildError("job_manifest_missing")
    job, digest = _read_json(path, "job_manifest_corrupt", with_sha=True)
    validate_job_manifest(job)
    return (job, digest) if with_sha else job


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def verify_output_sha256(path, expected):
    actual = file_sha256(path)
    if actual != expected:
        raise BuildError("output_sha_mismatch")
    return actual


def process_peak_rss(builder_report):
    value = builder_report.get("process_peak_memory_bytes")
    if (builder_report.get("process_peak_memory_available") is True
            and builder_report.get("process_peak_memory_source") == "process_rss"
            and type(value) is int and value > 0):
        return {"available": True, "value": value, "source": "process_rss",
                "scope": "producer process high-water RSS through builder completion"}
    return absent("builder has no OS process RSS; reserved bytes and godot_static are not process RSS")


def _oracle_block(value, poses, budget, *, control=False):
    if type(value) is not dict or type(value.get("error")) is not int:
        raise BuildError("oracle_report_invalid")
    if value["error"] != 0:
        reason = value.get("reason")
        raise BuildError(reason if type(reason) is str and reason else "oracle_report_invalid")
    rows = value.get("poses")
    if type(rows) is not list or len(rows) != len(poses):
        raise BuildError("oracle_report_invalid")
    for row, expected in zip(rows, poses):
        if (type(row) is not dict or row.get("checkpoint") != expected["checkpoint"]
                or row.get("pose") != expected["pose"] or type(row.get("status")) is not str
                or row["status"] not in SELECT_STATUSES
                or type(row.get("selected_splats")) is not int or row["selected_splats"] < 0
                or type(row.get("selected_count")) is not int or row["selected_count"] < 0
                or type(row.get("budget")) is not int or row["budget"] != budget
                or type(row.get("budget_bound")) is not bool
                or row.get("drawn_le_budget") is not (row["selected_splats"] <= budget)):
            raise BuildError("oracle_report_invalid")
        if ((row["status"] == "COMPLETE" and (row["selected_count"] == 0 or row["selected_splats"] == 0))
                or (row["status"] != "COMPLETE" and (row["selected_count"] != 0 or row["selected_splats"] != 0))):
            raise BuildError("oracle_report_invalid")
        if control and (type(row.get("only_leaves")) is not bool or
                        (row["status"] in ("COMPLETE", "EMPTY") and not row["only_leaves"])):
            raise BuildError("leaves_control_invalid")
    if control and value.get("mechanism") != "stale_interiors_unbounded":
        raise BuildError("leaves_control_invalid")
    return value


def _collect_output(report, output, job, builder):
    sidecar_path = output.with_suffix(".manifest.json")
    if not output.is_file() or not sidecar_path.is_file():
        raise BuildError("output_missing")
    sidecar = _read_json(sidecar_path, "output_manifest_invalid")
    try:
        if sidecar["schema"] != "godotgs-auto-tree-manifest/1":
            raise ValueError("schema")
        report["output_file_sha256"] = verify_output_sha256(output, sidecar["output_file_sha256"])
        if (sidecar["input_file_sha256"] != report["input_file_sha256"]
                or sidecar["source_fingerprint"] != job["source_fingerprint"]
                or sidecar["tree_sha"] != builder["tree_sha"]):
            raise BuildError("output_identity_mismatch")
        levels = sidecar["levels"]
        if type(levels) is not list or not levels:
            raise ValueError("levels")
        for level in levels:
            if type(level["depth"]) is not int or level["depth"] < 0 or type(level["node_count"]) is not int or level["node_count"] <= 0:
                raise ValueError("level counts")
            eps = [level[key] for key in ("eps_min", "eps_median", "eps_max")]
            if any(type(x) not in (int, float) or not math.isfinite(x) or x < 0 for x in eps) or eps != sorted(eps):
                raise ValueError("level errors")
        count = sidecar["totals"]["node_count"]
        generation = sidecar["content_generation"]
        if (type(count) is not int or count <= 0 or count != sum(level["node_count"] for level in levels)
                or type(generation) is not str or not generation.isdecimal() or not 0 < int(generation) < (1 << 64)
                or re.fullmatch(r"[0-9a-f]{64}", sidecar["tree_sha"]) is None):
            raise ValueError("identity/counts")
        report.update(node_count=count, height=max(level["depth"] for level in levels), levels=levels,
                      content_generation=generation, tree_sha=sidecar["tree_sha"],
                      output_bytes=output.stat().st_size + sidecar_path.stat().st_size)
    except (KeyError, TypeError, ValueError) as exc:
        if isinstance(exc, BuildError):
            raise
        raise BuildError("output_manifest_invalid") from exc


def _build(args, report):
    # Bind the digest to the exact bytes parsed, not a second read of a job
    # that another process might have replaced between parsing and hashing.
    job, report["job_manifest_sha256"] = load_job_manifest(args.job_manifest, with_sha=True)
    for key in ("source_fingerprint", "source_fingerprint_source", "drawn_budget_splats", "drawn_budget_source"):
        report[key] = job[key]
    if not args.input.is_file():
        raise BuildError("input_not_found")
    report["input_file_sha256"] = file_sha256(args.input)
    source_manifest = _read_json(args.input.with_suffix(".manifest.json"), "input_manifest_invalid")
    try:
        poses = [{"checkpoint": name, "pose": pose} for name, pose in
                 probe_path.checkpoints(probe_path.resolve_path(source_manifest))]
    except (ValueError, KeyError, TypeError, IndexError) as exc:
        raise BuildError("oracle_path_invalid") from exc
    if [p["checkpoint"] for p in poses] != [f"C{i}" for i in range(8)]:
        raise BuildError("oracle_path_invalid")
    binary = shutil.which(str(args.godot_binary))
    if not binary:
        raise BuildError("binary_missing")
    script = HERE / "build_auto_tree.gd"
    if not script.is_file():
        raise BuildError("driver_script_missing")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gs_auto_tree_project_") as directory:
        project = Path(directory)
        (project / "project.godot").write_text('[application]\nconfig/name="auto-tree"\n', encoding="utf-8")
        scratch = project / "scratch"
        scratch.mkdir()
        request = project / "request.json"
        response = project / "response.json"
        request.write_text(json.dumps({"input": args.input.as_posix(), "output": args.out.as_posix(),
            "scratch": scratch.as_posix(), "job": job, "poses": poses}, allow_nan=False), encoding="utf-8")
        command = [str(Path(binary).resolve()), "--headless", "--path", str(project), "--script", str(script),
                   "--", "--request", str(request), "--report", str(response)]
        print("build_auto_tree: mode=CPU-only " + " ".join(command), flush=True)
        try:
            child = subprocess.run(command, check=False, timeout=job["limits"]["deadline_sec"] + 60)
        except subprocess.TimeoutExpired as exc:
            raise BuildError("producer_timeout") from exc
        if not response.is_file():
            raise BuildError("producer_report_missing")
        envelope = _read_json(response, "producer_report_invalid")
        if type(envelope) is not dict:
            raise BuildError("producer_report_invalid")
        if "reason" in envelope and type(envelope["reason"]) is not str:
            raise BuildError("producer_report_invalid")
        if envelope.get("reason"):
            raise BuildError(envelope["reason"])
        builder = envelope.get("builder")
        if type(builder) is not dict or type(builder.get("error")) is not int or type(builder.get("reason")) is not str:
            raise BuildError("builder_report_invalid")
        report["peak_rss_bytes"] = process_peak_rss(builder)
        if builder["error"] != 0:
            raise BuildError(builder["reason"] or "builder_error_without_reason")
        if builder["reason"]:
            raise BuildError("builder_report_invalid")
        if child.returncode != 0:
            raise BuildError("producer_exit_nonzero")
        _collect_output(report, args.out, job, builder)
        report["oracle"] = _oracle_block(envelope.get("oracle"), poses, job["drawn_budget_splats"])
        report["oracle"].update(camera=job["oracle"],
            focal_pixels=0.5 * job["oracle"]["width"] / math.tan(math.radians(job["oracle"]["fov_deg"]) / 2))
        rows = report["oracle"]["poses"]
        report["oracle_complete_all_poses"] = all(row["status"] == "COMPLETE" and row["drawn_le_budget"] for row in rows)
        report["leaves_only"] = _oracle_block(envelope.get("leaves_only"), poses, job["drawn_budget_splats"], control=True)
        report["leaves_only"]["role"] = "diagnostic control only; excluded from the main oracle verdict"
        if not report["oracle_complete_all_poses"]:
            reason = "oracle_budget_infeasible" if any(row["status"] == "BOOTSTRAP_INFEASIBLE" for row in rows) else "oracle_incomplete"
            raise BuildError(reason)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for flag in ("godot-binary", "input", "job-manifest", "out", "report"):
        parser.add_argument("--" + flag, required=True, type=Path)
    args = parser.parse_args(argv)
    for name in ("input", "job_manifest", "out", "report"):
        setattr(args, name, getattr(args, name).resolve())
    # Never let report publication or builder output overwrite an input or each other.
    paths = [args.input, args.input.with_suffix(".manifest.json"), args.job_manifest,
             args.out, args.out.with_suffix(".manifest.json"), args.report]
    if any(a == b or (a.exists() and b.exists() and os.path.samefile(a, b))
           for i, a in enumerate(paths) for b in paths[i + 1:]):
        print("build_auto_tree: path_collision (report not written)", file=sys.stderr)
        return 1
    report = new_report()
    started = time.monotonic()
    try:
        _build(args, report)
        report.update(status="complete", reason="")
    except BuildError as exc:
        report.update(status="failed", reason=exc.reason)
    except OSError as exc:
        report.update(status="failed", reason="driver_io_error", detail=str(exc))
    finally:
        report["wall_time_sec"] = time.monotonic() - started
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except OSError as exc:
        print(f"build_auto_tree: report_write_failed: {exc}", file=sys.stderr)
        return 1
    print(f"build_auto_tree: {report['status']} {report['reason']}", flush=True)
    return 0 if report["status"] == "complete" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
