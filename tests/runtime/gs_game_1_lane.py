#!/usr/bin/env python3
"""Dispatch-only synthetic GS-GAME-1 orchestration. Never opens real-scan inputs.

Worlds remain in the runner cache. Artifacts are an explicit report allowlist.
Exit 0 measured; 1 failure; 2 invalid; 3 not run; 75 contention void (workflow).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE / "flythrough"))
import dispatch_lane as dispatch
import world_cache as cache
import shared_layers_t0 as shared_layers
import gs_game_1_verdict as verdict

WORLD = "world.gsplatworld"
GIB = 1024 ** 3


def save(path, obj):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(obj, indent=2, ensure_ascii=False), encoding="utf-8")


def semantic_job():
    """Existing T0 limits, raw V4, B2 volume descriptors, no generated sources."""
    job = dispatch.auto_tree_job("T0")
    job.update(codec="none", drawn_budget_splats=500_000,
               drawn_budget_source="ADR dynamics §7 T0 drawn budget 500k",
               measure_region_compression=True)
    volumes = []
    # The generator's T0 is 512 m wide. Three adjoining named Z bands.
    for name, z, weight in (("near", -512 / 3, 1.0), ("mid", 0.0, 0.6), ("far", 512 / 3, 0.0)):
        transform = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 32, z]
        for kind in ("region", "wind"):
            volumes.append({"kind": kind, "path": f"/GSGame1/{name}/{kind}", "name": name,
                            "size": [512, 128, 512 / 3], "transform": transform,
                            **({"wind_weight": weight} if kind == "wind" else {})})
    job["volumes"] = volumes
    return job


def prepare_world(binary, cache_root, out, prime):
    """Use the existing flock/auto-tree producer and immutable complete cache key."""
    job = semantic_job()
    fingerprint = cache.source_fingerprint(ROOT, tuple(cache.GENERATOR_SOURCES) + tuple(cache.TREE_SOURCES))
    key = hashlib.sha256((fingerprint + json.dumps(job, sort_keys=True)).encode()).hexdigest()
    cache_root.mkdir(parents=True, exist_ok=True)
    directory = cache_root / "gs-game-1-T0-v4-raw" / key[:12]
    with cache.cache_lock(cache_root, exclusive=True):
        cache.ensure_format(cache_root, create=prime)
        marker = directory / ".complete"
        if not marker.is_file():
            if not prime:
                raise dispatch.LaneError("NOT_RUN: GS-GAME-1 T0 cache absent/stale; dispatch prime_world_cache=true", 3)
            # Existing T0 producer's 2 GiB output/scratch caps plus plain 1.5 GB
            # and the existing D-14 free margin, never a raised cap.
            required = dispatch.WORLD_BUDGET_BYTES["T0"] + 4 * GIB + dispatch.FREE_MARGIN_BYTES
            if shutil.disk_usage(cache_root).free < required:
                raise dispatch.LaneError("NOT_RUN: disk below T0 + existing producer caps + 25 GiB margin", 3)
            directory.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix="gs-game-1-prime-", dir=directory.parent) as temporary:
                staging = Path(temporary)
                plain = staging / "plain"
                command = [sys.executable, str(HERE / "flythrough/generate_world.py"),
                           "--godot-binary", str(binary), "--tier", "T0",
                           "--encoding", "plain_v3_uncompressed", "--out", str(plain)]
                rc = dispatch._run_logged(command, out / "logs/generate.log", subprocess.run, 600)
                if rc:
                    raise dispatch.LaneError(f"T0 generator failed: {rc}")
                tree = staging / "tree" / WORLD
                job_path = staging / "job.json"
                report_path = staging / "build.json"
                save(job_path, job)
                command = [sys.executable, str(HERE / "flythrough/build_auto_tree.py"),
                           "--godot-binary", str(binary), "--input", str(plain / WORLD),
                           "--job-manifest", str(job_path), "--out", str(tree), "--report", str(report_path)]
                rc = dispatch._run_logged(command, out / "logs/build.log", subprocess.run, 1500)
                if rc or not report_path.is_file():
                    raise dispatch.LaneError(f"raw T0 auto-tree producer failed: {rc}")
                report = json.loads(report_path.read_text(encoding="utf-8"))
                if (report.get("status") != "complete" or report.get("oracle_complete_all_poses") is not True
                        or report.get("drawn_budget_splats") != 500_000
                        or report.get("w13", {}).get("passed") is not True
                        or not dispatch._verify_world(tree).get("ok")):
                    raise dispatch.LaneError("INVALID: raw T0 producer/oracle/reopen verification incomplete", 2)
                sidecar = json.loads(tree.with_suffix(".manifest.json").read_text(encoding="utf-8"))
                if sidecar.get("codec") != {"codec": "none"}:
                    raise dispatch.LaneError("INVALID: expected raw V4 payload", 2)
                save(staging / "bake.json", report)
                # Re-prime completion follows actual producer verification and flush.
                files = cache.world_files(staging)
                cache.drop_from_page_cache(files)
                hashes = {str(p.relative_to(staging)): dispatch._sha256(p)
                          for p in staging.rglob("*") if p.is_file()}
                save(staging / ".complete", {"key": key, "source_fingerprint": fingerprint,
                                               "job": job, "sha256": hashes})
                # No previous complete key is modified or deleted.
                if directory.exists():
                    raise dispatch.LaneError("INVALID: incomplete cache key exists; inspect it before re-prime", 2)
                staging.rename(directory)
        complete = json.loads(marker.read_text(encoding="utf-8"))
        if complete.get("key") != key:
            raise dispatch.LaneError("INVALID: cache provenance mismatch", 2)
        hashes = complete.get("sha256")
        actual_files = {str(p.relative_to(directory)) for p in directory.rglob("*") if p.is_file() and p != marker}
        required = {"tree/" + WORLD, "tree/world.manifest.json", "bake.json"}
        if not isinstance(hashes, dict) or not hashes or set(hashes) != actual_files or not required <= set(hashes):
            raise dispatch.LaneError("INVALID: cache digest coverage incomplete", 2)
        for relative, expected in hashes.items():
            path = (directory / relative).resolve()
            if not path.is_relative_to(directory.resolve()) or not path.is_file() \
                    or dispatch._sha256(path) != expected:
                raise dispatch.LaneError("INVALID: cached synthetic payload digest mismatch", 2)
        if not dispatch._verify_world(directory / "tree" / WORLD).get("ok"):
            raise dispatch.LaneError("INVALID: cached world reopen verification failed", 2)
        return directory / "tree" / WORLD, json.loads((directory / "bake.json").read_text(encoding="utf-8")), complete


def run_arm(binary, project, plan, output, name):
    run = output / "runs" / name
    run.mkdir(parents=True, exist_ok=True)
    (run / "raw.json").unlink(missing_ok=True)
    save(run / "plan.json", plan | {"output": str(run / "raw.json"), "run": name})
    # External device measurement covers the run, on the contention-checked GPU.
    external = []
    stop = threading.Event()

    def sample_vram():
        while not stop.is_set():
            try:
                query = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                                       capture_output=True, text=True, timeout=10)
                lines = query.stdout.strip().splitlines()
                if query.returncode == 0 and len(lines) == 1:
                    external.append({"monotonic": time.monotonic(), "bytes": int(lines[0]) * 1024 ** 2})
            except (OSError, ValueError, subprocess.TimeoutExpired):
                pass  # Missing remains missing; no zero placeholder.
            stop.wait(.5)

    sampler = threading.Thread(target=sample_vram, daemon=True)
    sampler.start()
    try:
        rc = dispatch._run_logged([str(binary), "--path", str(project), "--rendering-method", "forward_plus",
                                   "--rendering-driver", "vulkan", "--script", str(HERE / "gs_game_1_harness.gd"),
                                   "--", "--plan", str(run / "plan.json")],
                                  run / "godot.log", subprocess.run, 1200)
    finally:
        stop.set()
        sampler.join(timeout=12)
    if not (run / "raw.json").is_file():
        return {"rows": {}, "producer_exit": rc, "reason": "producer raw JSON absent"}
    raw = json.loads((run / "raw.json").read_text(encoding="utf-8"))
    raw["producer_exit"] = rc
    error = producer_error(raw, plan, name, rc)
    if error:
        raw["rejected_rows"] = raw.pop("rows", {})
        raw["rows"] = {}
        raw["frames"] = []
        raw["rejection"] = error
    raw["external_vram_samples"] = external
    if external and "M1" in raw.get("rows", {}):
        raw["rows"]["M1"]["external_vram_bytes"] = max(s["bytes"] for s in external)
    return raw


def producer_error(raw, plan, name, rc):
    """Validate a constructed arm envelope; partial/stale captures are not evidence."""
    if rc != 0 or raw.get("completed") is not True:
        return "NOT_RUN: producer did not complete successfully"
    for key, expected in (("job_id", plan["job_id"]), ("run", name),
                          ("control", plan["control"]), ("dynamics", plan["dynamics"]),
                          ("tests_enabled", True), ("ledger_cap_verified", True)):
        if raw.get(key) != expected:
            return f"INVALID: producer {key} provenance mismatch"
    return ""


def finalize(out, postflight):
    path = out / "raw.json"
    if not path.is_file():
        return 3
    raw = json.loads(path.read_text(encoding="utf-8"))
    raw["contention_postflight"] = postflight
    if postflight != "success":
        raw["invalid_reason"] = "VOID: contention postflight did not succeed: " + postflight
    save(path, raw)
    result = verdict.write_report(raw, out)
    return {"FAIL": 1, "INVALID": 2, "NOT_RUN": 3}.get(result["state"], 0)


def execute(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    raw = {"schema": "gs-game-1/raw/v1", "job_id": args.job_id,
           "subject_run": "subject", "hardware": "NOT_RUN", "approximation": verdict.O6_LABEL,
           "ledger_cap_bytes": 8_000_000_000, "rows": {}}
    # Write a NOT_RUN report before any external process, including a cache miss.
    save(out / "raw.json", raw)
    verdict.write_report(raw, out)
    try:
        if not args.godot_binary.is_file():
            raise dispatch.LaneError("NOT_RUN: optimized tests=yes binary absent", 3)
        world, bake, complete = prepare_world(args.godot_binary.resolve(), args.cache_root.resolve(), out,
                                              args.prime_world_cache)
        with tempfile.TemporaryDirectory(prefix="gs-game-1-project-") as temporary:
            project = Path(temporary)
            shutil.copyfile(HERE / "gs_game_1_project/project.godot", project / "project.godot")
            plan = {"world": str(world), "control": "none", "dynamics": True,
                    "job_id": args.job_id, "samples": 1200, "ledger_cap_bytes": 8_000_000_000,
                    "tree_pages": shared_layers.tree_page_count(world),
                    "atlas_mb": int(.35 * shared_layers.tree_page_count(world) * 1024 * 176) // (1024 ** 2)}
            subject = run_arm(args.godot_binary.resolve(), project, plan, out, "subject")
            raw["subject"] = subject
            adapter = subject.get("adapter", "")
            if "RTX 3080" in adapter and subject.get("tests_enabled") is True:
                raw["hardware"] = "RTX 3080"
            raw["rows"] = {row: {"measurement": m} for row, m in subject.get("rows", {}).items()}
            off = run_arm(args.godot_binary.resolve(), project, plan | {"dynamics": False}, out, "dynamics-off")
            raw["dynamics_off"] = off
            raw["rows"]["P2"] = {"measurement": {"on": subject.get("frames", []), "off": off.get("frames", [])}}
            for row in verdict.CONTROLLED:
                arm = run_arm(args.godot_binary.resolve(), project, plan | {"control": row}, out, "control-" + row)
                raw.setdefault("controls", {})[row] = arm
                raw["rows"].setdefault(row, {})["control"] = {
                    "job_id": arm.get("job_id"), "run": arm.get("run"),
                    "switch_observed": arm.get("switch", {}).get("switch_observed") is True,
                    "measurement": arm.get("rows", {}).get(row, {})}
                save(out / "raw.json", raw)
        counters = bake.get("builder_raw") or {}
        raw["rows"]["F3"] = {"measurement": {"world": "synthetic T0", "producer": "B2",
            **{k: counters[k] for k in ("region_keyed_interior_bytes", "single_region_interior_bytes") if k in counters}}}
        raw["bake"] = bake
        raw["cache_provenance"] = complete
    except (dispatch.LaneError, cache.CacheError, OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as exc:
        raw["reason"] = str(exc)
        if "INVALID:" in str(exc):
            raw["invalid_reason"] = str(exc)
    save(out / "raw.json", raw)
    result = verdict.write_report(raw, out)
    print(f"GS-GAME-1 report-only: {result['state']}; {verdict.O6_LABEL}")
    return {"FAIL": 1, "INVALID": 2, "NOT_RUN": 3}.get(result["state"], 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot-binary", type=Path)
    parser.add_argument("--cache-root", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--job-id")
    parser.add_argument("--prime-world-cache", action="store_true")
    parser.add_argument("--finalize-postflight", choices=("success", "failure", "cancelled", "skipped"))
    args = parser.parse_args()
    if args.finalize_postflight:
        return finalize(args.out, args.finalize_postflight)
    if not args.godot_binary or not args.cache_root or not args.job_id:
        parser.error("execution requires --godot-binary, --cache-root and --job-id")
    return execute(args)


if __name__ == "__main__":
    raise SystemExit(main())
