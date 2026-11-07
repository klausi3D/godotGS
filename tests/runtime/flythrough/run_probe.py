#!/usr/bin/env python3
"""BV-07 probe lane: arc-length fly-through, binary trace, pumps, F/k drawn sweep.

ADR: docs/architecture/adr-beyond-vram-worlds.md, PR row BV-07 ("Probe lane:
arc-length path, binary trace, pumps, F/k drawn sweep on T0"), D-4 (frame model),
D-6 (regulator, ``settings_drifted``), section 6.4 (ground truth), 7 (fly-through)
and the M1 list ("measured F and k; whether the regulator fires").

Entry point of the BV-06a dispatch lane's ``probe`` step
(tests/runtime/flythrough/dispatch_lane.py)::

    python3 tests/runtime/flythrough/run_probe.py --godot-binary B \\
        --world TIER=DIR [--world TIER=DIR ...] --out OUT

Each DIR holds ``world.gsplatworld`` and its BV-05 sidecar ``world.manifest.json``.
The probe writes ``OUT/summary.json`` (always, once the arguments parse) and, per
world, ``OUT/<TIER>/``: the resolved path, ``world.groundtruth.json`` (ADR section
6.4: the generator manifest carries no checkpoint data), the plan, the binary trace
and the driver's result.

What runs:

1. **Fly-through, per world** (one Godot process): the ADR section 7.2 path,
   resolved against the world's manifest by probe_path.py, at the tuned settings
   (G-c: 4M visible cap, 400 m far plane, 4096 MB budget, 320 chunks; 1920x1080,
   vsync off, chunk-streaming route, quantized streaming off). Moving segments are
   paced by wall clock (pass A); stops and teleports pump until missing chunks = 0
   or a wall-clock deadline (pass B settle) and capture a checkpoint. ``--passes 2``
   (default) flies the path twice in the same process: a first and a repeat pass.
   The host page cache is not controlled here (V10 is BV-06b), so neither pass is
   labelled cold.
2. **F/k drawn sweep on T0** (one Godot process per point): T0 chunk subsets of
   about 32k (the "small drawn count" F point: F cannot be measured at 0 drawn,
   because then no Gaussian pass runs and every timing is held), 0.25, 0.5, 1 and
   2M splats, plus the whole of T0 (4.19M), each rendered from fixed views with the
   chunk route's prefix LOD off, so drawn = the resident subset in view. A subset
   is every chunk an error-diffusion walk over the Morton order picks, so it is a
   spatially uniform sample with T0's per-splat footprint; the fixed camera keeps
   records per splat constant across the sweep. ``mean ms = F + k * N`` is fitted
   per view to the fresh coherent GPU breakdown (BV-04b) and to the wall-clock
   frame interval. The subset worlds are generated with BV-05's generate_world.py
   (``--select``) into OUT/sweep_worlds and deleted afterwards.

Report-only: nothing here gates. But absence of a signal is never a pass: every
metric in the summary carries ``status`` OK / MISSING / INVALID and a reason
(probe_summary.metric). If any regulator event fires, or the effective settings
change, inside measured frames, the numbers are labelled ``settings_drifted``
(ADR D-6 item 2): a finding, not a baseline.

Exit codes (ADR section 9.1 mapping): 0 every phase ran and every required metric
is present; 1 a Godot process failed (crash, non-zero exit, timeout, no result);
2 the run completed but is INVALID (a required metric is MISSING, or
rows_written != frames_rendered); 3 NOT_RUN (no world / no binary). The summary
is written in every case but a usage error.

Standard library only.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import probe_groundtruth  # noqa: E402
import probe_path  # noqa: E402
import probe_summary as ps  # noqa: E402

DRIVER = HERE / "probe_world.gd"
GENERATOR = HERE / "generate_world.py"
WORLD_NAME = "world.gsplatworld"
MANIFEST_NAME = "world.manifest.json"
SUMMARY_NAME = "summary.json"

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_NOT_RUN = 3

RESOLUTION = (1920, 1080)
ASPECT = RESOLUTION[0] / RESOLUTION[1]

#: ADR G-c tuned settings, plus the section 7.1 configuration.
TUNED_SETTINGS: Dict[str, object] = {
    "rendering/gaussian_splatting/streaming/route_policy": 1,
    "rendering/gaussian_splatting/instance_pipeline/enabled": True,
    "rendering/gaussian_splatting/streaming/vram_budget_mb": 4096,
    "rendering/gaussian_splatting/streaming/max_chunks_in_vram": 320,
    "rendering/gaussian_splatting/compression/per_chunk_quantization": 0,
}
TUNED_VISIBLE_CAP = 4_000_000
TUNED_FAR_M = 400.0
CAMERA = {"fov_deg": 107.5, "keep": "width", "near_m": 0.05, "far_m": TUNED_FAR_M}

#: Sweep-only deviations from the tuned settings, recorded in the summary:
#: prefix LOD off (drawn = resident subset in view) and a chunk cap that admits all
#: of T0's 343 chunks, so the 4M point is fully resident.
SWEEP_SETTINGS: Dict[str, object] = dict(
    TUNED_SETTINGS,
    **{
        "rendering/gaussian_splatting/lod/enabled": False,
        "rendering/gaussian_splatting/streaming/max_chunks_in_vram": 512,
    },
)
SWEEP_TIER = "T0"
#: (id, target splats). None = the whole world.
SWEEP_POINTS: Tuple[Tuple[str, Optional[int]], ...] = (
    ("F_small", 32_768),
    ("0.25M", 250_000),
    ("0.5M", 500_000),
    ("1M", 1_000_000),
    ("2M", 2_000_000),
    ("4M", None),
)

SETTLE = {"cold_deadline_s": 90.0, "stop_deadline_s": 15.0, "teleport_deadline_s": 30.0}
RSS_POLL_S = 0.2


class ProbeError(Exception):
    def __init__(self, message: str, code: int = EXIT_FAIL) -> None:
        super().__init__(message)
        self.code = code


# --------------------------------------------------------------------------
# Process memory sampling (external: what the OS charges the process)
# --------------------------------------------------------------------------


def _rss_linux(pid: int) -> Tuple[Optional[int], Optional[int]]:
    try:
        text = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    except OSError:
        return None, None
    rss = hwm = None
    for line in text.splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1]) * 1024
        elif line.startswith("VmHWM:"):
            hwm = int(line.split()[1]) * 1024
    return rss, hwm


def _rss_windows(pid: int) -> Tuple[Optional[int], Optional[int]]:
    class PMC(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
    psapi = ctypes.windll.psapi  # type: ignore[attr-defined]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not handle:
        return None, None
    try:
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        if not psapi.GetProcessMemoryInfo(ctypes.c_void_p(handle), ctypes.byref(pmc), pmc.cb):
            return None, None
        return int(pmc.WorkingSetSize), int(pmc.PeakWorkingSetSize)
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


def sample_rss(pid: int) -> Tuple[Optional[int], Optional[int]]:
    if sys.platform.startswith("linux"):
        return _rss_linux(pid)
    if sys.platform == "win32":
        return _rss_windows(pid)
    return None, None


class RssSampler(threading.Thread):
    def __init__(self, proc: subprocess.Popen) -> None:
        super().__init__(daemon=True)
        self.proc = proc
        self.samples: List[Tuple[float, int]] = []
        self.peak_reported: Optional[int] = None
        self._t0 = time.monotonic()

    def run(self) -> None:
        while self.proc.poll() is None:
            rss, hwm = sample_rss(self.proc.pid)
            if rss is not None:
                self.samples.append((round(time.monotonic() - self._t0, 3), rss))
            if hwm is not None:
                self.peak_reported = max(self.peak_reported or 0, hwm)
            time.sleep(RSS_POLL_S)

    def result(self) -> Dict[str, object]:
        if not self.samples:
            return {"peak_rss_bytes": ps.missing(f"no RSS sample on {sys.platform}", "bytes"), "samples": 0}
        peak_sampled = max(s[1] for s in self.samples)
        peak = max(peak_sampled, self.peak_reported or 0)
        return {
            "peak_rss_bytes": ps.metric(peak, unit="bytes"),
            "peak_rss_source": "VmHWM / PeakWorkingSetSize (OS high-water mark)" if self.peak_reported else "polled",
            "peak_rss_sampled_bytes": peak_sampled,
            "last_rss_bytes": self.samples[-1][1],
            "samples": len(self.samples),
            "poll_s": RSS_POLL_S,
        }


# --------------------------------------------------------------------------
# Godot invocation
# --------------------------------------------------------------------------


def resolve_binary(binary: Path) -> Tuple[Path, str]:
    """On Windows the *.console.exe is a launcher; run (and measure) the real binary."""
    name = binary.name
    if name.endswith(".console.exe"):
        real = binary.with_name(name[: -len(".console.exe")] + ".exe")
        if real.is_file():
            return real, f"{name} is a console launcher; ran {real.name}"
    return binary, ""


def binary_identity(binary: Path) -> Dict[str, object]:
    h = hashlib.sha256()
    with binary.open("rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    st = binary.stat()
    return {"path": str(binary), "sha256": h.hexdigest(), "bytes": st.st_size,
            "mtime_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(st.st_mtime))}


def run_godot(binary: Path, plan: dict, workdir: Path, timeout_s: float) -> Dict[str, object]:
    workdir.mkdir(parents=True, exist_ok=True)
    plan_path = workdir / "plan.json"
    plan["trace"] = (workdir / "trace.bin").as_posix()
    plan["result"] = (workdir / "result.json").as_posix()
    plan_path.write_text(json.dumps(plan, indent=1), encoding="utf-8")
    result_path = workdir / "result.json"
    if result_path.exists():
        result_path.unlink()
    log_path = workdir / "godot.log"
    with tempfile.TemporaryDirectory(prefix="gs_probe_project_") as proj:
        Path(proj, "project.godot").write_text('[application]\nconfig/name="bv07probe"\n', encoding="utf-8")
        cmd = [
            str(binary), "--path", proj, "--rendering-driver", "vulkan",
            "--resolution", f"{RESOLUTION[0]}x{RESOLUTION[1]}", "--windowed",
            "--script", DRIVER.as_posix(), "--", "--plan", plan_path.as_posix(),
        ]
        print("run_probe: " + " ".join(cmd), flush=True)
        t0 = time.monotonic()
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            sampler = RssSampler(proc)
            sampler.start()
            timed_out = False
            try:
                rc = proc.wait(timeout=timeout_s)
            except subprocess.TimeoutExpired:
                timed_out = True
                proc.kill()
                rc = proc.wait()
            sampler.join(timeout=5)
        wall = time.monotonic() - t0
    result = None
    if result_path.is_file():
        try:
            result = json.loads(result_path.read_text(encoding="utf-8"))
        except ValueError:
            result = None
    return {"exit_code": rc, "timed_out": timed_out, "wall_s": round(wall, 2), "result": result,
            "rss": sampler.result(), "log": log_path.as_posix(), "trace": plan["trace"]}


def process_failure(run: Dict[str, object]) -> Optional[str]:
    if run["timed_out"]:
        return "timed out"
    if run["result"] is None:
        return f"exited {run['exit_code']} without a result file"
    if run["exit_code"] != 0:
        return f"exited {run['exit_code']}: {run['result'].get('reason')}"
    if not run["result"].get("ok"):
        return f"driver reported failure: {run['result'].get('reason')}"
    return None


def trace_integrity(run: Dict[str, object]) -> Tuple[Optional[ps.Trace], Dict[str, object]]:
    try:
        trace = ps.read_trace(Path(run["trace"]))
    except (OSError, ps.TraceError) as exc:
        return None, {"status": ps.INVALID, "reason": f"trace unreadable: {exc}"}
    result = run["result"] or {}
    frames = result.get("frames_rendered")
    rows_claimed = result.get("rows_written")
    ef = trace.col("engine_frame")
    gaps = sum(1 for a, b in zip(ef, ef[1:]) if b - a != 1)
    info = {"rows_in_file": len(trace.rows), "rows_written": rows_claimed, "frames_rendered": frames,
            "trailing_bytes": trace.trailing_bytes, "engine_frame_gaps": gaps}
    if trace.trailing_bytes or rows_claimed != len(trace.rows) or frames != len(trace.rows) or gaps:
        info.update(status=ps.INVALID,
                    reason="rows_written != frames_rendered, a skipped engine frame, or a torn trace (ADR V5)")
    else:
        info["status"] = ps.OK
    return trace, info


# --------------------------------------------------------------------------
# Fly-through
# --------------------------------------------------------------------------


def path_plan(world: Path, path: dict, passes: int, max_fps: int = 0) -> dict:
    return {
        "schema": "godotgs-bv07-probe-plan/1",
        "mode": "path",
        "world": world.as_posix(),
        "resolution": list(RESOLUTION),
        "project_settings": TUNED_SETTINGS,
        "node": {"max_splat_count": TUNED_VISIBLE_CAP, "max_render_distance_m": TUNED_FAR_M},
        "camera": CAMERA,
        "first_pose": path["segments"][0]["pose"],
        "path": path,
        "passes": passes,
        "settle": SETTLE,
        "max_fps": max_fps,
    }


def summarise_path(trace: ps.Trace, run: dict, path: dict, groundtruth: dict, passes: int) -> Dict[str, object]:
    result = run["result"] or {}
    segs = path["segments"]
    out: Dict[str, object] = {"passes": []}
    for pass_index in range(passes):
        pass_rows = ps.rows_where(trace, phase=0, pass_index=pass_index)
        moving = ps.rows_where(trace, phase=0, pass_index=pass_index, state=ps.STATE_MOVING)
        per_seg = {}
        for si, seg in enumerate(segs):
            if seg["kind"] != "move":
                continue
            rows = ps.rows_where(trace, phase=0, pass_index=pass_index, segment=si, state=ps.STATE_MOVING)
            per_seg[seg["id"]] = {
                "label": seg["label"], "speed_m_s": seg["speed_m_s"], "length_m": seg["length_m"],
                "frame_interval": ps.interval_stats(ps.frame_intervals(trace, rows)),
                "coverage_m": ps.metric(max((trace.get(trace.rows[i], "s_m") for i in rows), default=None), unit="m"),
            }
        checkpoints = []
        for cp in result.get("checkpoints", []):
            if cp.get("pass_index") != pass_index:
                continue
            truth = groundtruth["checkpoints"].get(cp["checkpoint"], {})
            exp = truth.get("expected_drawn_upper_bound")
            settle = cp.get("settle", {})
            checkpoints.append({
                "checkpoint": cp["checkpoint"],
                "segment": cp["segment"],
                "settled": settle.get("settled"),
                "settle_s": ps.metric(settle["settle_usec"] / 1e6, unit="s") if settle.get("settled")
                else ps.missing(settle.get("reason", "not settled"), "s"),
                "drawn_splats": cp.get("drawn_splats"),
                "expected_drawn_upper_bound": exp,
                "drawn_fraction_of_expected": ps.metric(cp["drawn_splats"] / exp) if exp else ps.missing("no expected count"),
                "needed_unserved_chunks": cp.get("needed_unserved_chunks"),
                "loaded_chunks": cp.get("loaded_chunks"),
                "frustum_chunks_expected": truth.get("frustum_chunk_count"),
            })
        out["passes"].append({
            "pass_index": pass_index,
            "label": "first" if pass_index == 0 else "repeat",
            "host_cache": "not_controlled (V10 cold-cache proof is BV-06b)",
            "moving_frame_interval": ps.interval_stats(ps.frame_intervals(trace, moving)),
            "moving_gpu": ps.gpu_breakdown(trace, moving),
            "segments": per_seg,
            "checkpoints": checkpoints,
            "streaming": ps.streaming_counters(trace, pass_rows),
            "settings": ps.settings_drift(trace, moving),
            "settings_all_rows": ps.settings_drift(trace, pass_rows),
        })
    all_rows = ps.rows_where(trace, phase=0)
    out["streaming_whole_run"] = ps.streaming_counters(trace, all_rows)
    out["settings_whole_run"] = ps.settings_drift(trace, all_rows)
    fs = result.get("final_streaming", {})
    out["regulator"] = {
        k: fs.get(k) for k in (
            "vram_regulation_adjustments", "vram_thrash_reductions", "vram_budget_reductions",
            "vram_headroom_increases", "vram_lod_multiplier_adjustments", "vram_last_regulation_frame",
            "vram_last_regulation_reason", "vram_thrashing_events", "vram_regulated_max_chunks",
            "vram_auto_regulate_enabled", "effective_settings_revision",
        )
    }
    missing_keys = [k for k, v in out["regulator"].items() if v is None]
    out["regulator"]["status"] = ps.MISSING if missing_keys else ps.OK
    if missing_keys:
        out["regulator"]["reason"] = f"absent from the final streaming state: {missing_keys}"
    out["regulator"]["fired"] = bool(fs.get("vram_regulation_adjustments")) if not missing_keys else None
    out["latency_histograms"] = fs.get("streaming_latency_histograms", ps.missing("no latency histograms"))
    out["expected_unique_upload_flat"] = groundtruth["expected_unique_upload"]["flat"]
    ub = out["streaming_whole_run"].get("upload_bytes", {})
    exp_b = out["expected_unique_upload_flat"]["stored_bytes"]
    out["uploaded_vs_expected_unique"] = (
        ps.metric(ub["value"] / exp_b) if ub.get("status") == ps.OK and exp_b else ps.missing("no upload bytes or no oracle")
    )
    out["records_per_splat"] = ps.missing(
        "not published on the flat chunk route (BV-04b publishes records per splat per level on the HLOD route only)")
    return out


def required_path_metrics(world_summary: dict) -> List[str]:
    problems = []
    fl = world_summary.get("flythrough", {})
    for p in fl.get("passes", []):
        if p["moving_frame_interval"].get("frames", 0) == 0:
            problems.append(f"pass {p['pass_index']}: no moving frame")
        if p["settings"].get("status") != ps.OK:
            problems.append(f"pass {p['pass_index']}: settings drift not evaluable")
        if p["moving_gpu"]["coherent_total_ms"].get("status") == ps.MISSING:
            problems.append(f"pass {p['pass_index']}: no fresh GPU breakdown while moving")
    if fl.get("regulator", {}).get("status") != ps.OK:
        problems.append("regulator counters missing")
    rss = world_summary.get("rss", {}).get("peak_rss_bytes", {})
    if rss.get("status") != ps.OK:
        problems.append("peak RSS missing")
    return problems


# --------------------------------------------------------------------------
# F/k sweep
# --------------------------------------------------------------------------


def select_subset(manifest: dict, target: int) -> List[int]:
    """Error-diffusion walk over the Morton-ordered chunks: a spatially uniform subset of ~target splats."""
    chunks = sorted(manifest["chunks"], key=lambda c: int(c["morton"]))
    total = sum(int(c["count"]) for c in chunks)
    frac = min(1.0, target / total)
    want = 0.0
    got = 0
    out = []
    for c in chunks:
        want += frac * int(c["count"])
        if got + int(c["count"]) / 2.0 <= want:
            out.append(int(c["plan_index"]))
            got += int(c["count"])
    if not out:
        out.append(int(chunks[len(chunks) // 2]["plan_index"]))
    return sorted(out)


def sweep_views(manifest: dict) -> List[dict]:
    x0, z0, x1, z1 = (float(v) for v in manifest["world"]["extent_m"])
    cx, cz = (x0 + x1) / 2.0, (z0 + z1) / 2.0
    half_w = max(x1 - x0, z1 - z0) / 2.0
    tan_h = math.tan(math.radians(CAMERA["fov_deg"] / 2.0))
    tan_v = tan_h / ASPECT
    h = half_w / tan_v * 1.04
    corner = (h * h + 2 * half_w * half_w) ** 0.5
    return [
        # Top-down, the whole world in view: the only view in which all of T0 can be drawn.
        {"id": "overview", "pose": [cx, round(h, 3), cz, 0.0, -90.0], "far_m": round(corner * 1.1, 1)},
        # Oblique from the edge at the tuned far plane: near splats large, as in the fly-through.
        {"id": "oblique", "pose": [cx, 60.0, z1 + 20.0, 0.0, -25.0], "far_m": TUNED_FAR_M},
    ]


def generate_subset(binary: Path, tier_dir: Path, select: List[int], out_dir: Path, log: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    sel = out_dir / "select.json"
    sel.write_text(json.dumps(select), encoding="utf-8")
    manifest = json.loads((tier_dir / MANIFEST_NAME).read_text(encoding="utf-8"))
    params = manifest["generator_params"]
    overrides = out_dir / "overrides.json"
    overrides.write_text(json.dumps({k: v for k, v in params.items() if k != "tier"}), encoding="utf-8")
    cmd = [sys.executable, str(GENERATOR), "--godot-binary", str(binary), "--tier", str(params["tier"]),
           "--overrides", str(overrides), "--select", str(sel), "--no-verify", "--out", str(out_dir)]
    with log.open("w", encoding="utf-8", errors="replace") as fh:
        rc = subprocess.run(cmd, stdout=fh, stderr=subprocess.STDOUT, check=False).returncode
    if rc != 0 or not (out_dir / MANIFEST_NAME).is_file():
        raise ProbeError(f"subset generation exited {rc} (log {log})")
    sub = json.loads((out_dir / MANIFEST_NAME).read_text(encoding="utf-8"))
    if sub.get("generator_params_sha") != manifest.get("generator_params_sha"):
        raise ProbeError("subset world has different generator_params_sha than its parent")


def sweep_plan(world: Path, views: List[dict], point_index: int, measure_s: float) -> dict:
    return {
        "schema": "godotgs-bv07-probe-plan/1",
        "mode": "sweep",
        "world": world.as_posix(),
        "resolution": list(RESOLUTION),
        "project_settings": SWEEP_SETTINGS,
        "node": {"max_splat_count": TUNED_VISIBLE_CAP,
                 "max_render_distance_m": max(v["far_m"] for v in views), "lod_enabled": False},
        "camera": dict(CAMERA, far_m=max(v["far_m"] for v in views)),
        "first_pose": views[0]["pose"],
        "sweep": {"views": views, "point_index": point_index, "settle_deadline_s": 60.0,
                  "warmup_s": 1.0, "measure_s": measure_s},
    }


def summarise_sweep_point(trace: ps.Trace, run: dict, point_id: str, subset_splats: int) -> Dict[str, object]:
    result = run["result"] or {}
    views = {}
    for entry in result.get("sweep", []):
        rows = list(range(entry["first_row"], entry["last_row"] + 1))
        drawn = [trace.get(trace.rows[i], "drawn_splats") for i in rows]
        settle = entry.get("settle", {})
        gpu = ps.gpu_breakdown(trace, rows)
        wall = ps.interval_stats(ps.frame_intervals(trace, rows))
        gpu_frame = [trace.get(trace.rows[i], "gpu_frame_ms") for i in rows if trace.get(trace.rows[i], "gpu_frame_valid")]
        views[entry["view"]] = {
            "point": point_id,
            "subset_splats": subset_splats,
            "settled": settle.get("settled"),
            "settle_s": ps.metric(settle["settle_usec"] / 1e6, unit="s") if settle.get("settled")
            else ps.missing(settle.get("reason", "not settled"), "s"),
            "frames": len(rows),
            "drawn_mean": ps.mean(drawn) if drawn else None,
            "drawn_min": min(drawn) if drawn else None,
            "drawn_max": max(drawn) if drawn else None,
            # The tuned 4M visible cap binds: the renderer drew exactly the cap, so N is the
            # cap, not the subset (the 4M point on the full 4.19M T0 in the overview view).
            "visible_cap_bound": bool(drawn) and max(drawn) >= TUNED_VISIBLE_CAP,
            "gpu": gpu,
            "gpu_pipeline_mean_ms": gpu["coherent_total_ms"]["mean"] if "mean" in gpu["coherent_total_ms"]
            else gpu["coherent_total_ms"],
            "gpu_frame_bracket_mean_ms": ps.metric(ps.mean(gpu_frame), unit="ms") if gpu_frame
            else ps.missing("gpu_frame_time not valid in any measured row", "ms"),
            "wall_interval": wall,
            "wall_interval_mean_ms": wall["mean"],
            "settings": ps.settings_drift(trace, rows),
        }
    return views


def run_sweep(binary: Path, gen_binary: Path, tier_dir: Path, out: Path, measure_s: float, timeout_s: float,
              keep: bool) -> Dict[str, object]:
    manifest = json.loads((tier_dir / MANIFEST_NAME).read_text(encoding="utf-8"))
    views = sweep_views(manifest)
    scratch = out / "sweep_worlds"
    points: List[Dict[str, object]] = []
    failures: List[str] = []
    try:
        for pi, (pid, target) in enumerate(SWEEP_POINTS):
            point_dir = out / "sweep" / pid
            if target is None:
                world_dir = tier_dir
                subset_splats = int(manifest["world"]["splat_count"])
                selection_info = {"chunks": len(manifest["chunks"]), "whole_world": True}
            else:
                sel = select_subset(manifest, target)
                world_dir = scratch / pid
                subset_splats = sum(int(c["count"]) for c in manifest["chunks"] if int(c["plan_index"]) in set(sel))
                selection_info = {"chunks": len(sel), "whole_world": False, "target_splats": target}
                point_dir.mkdir(parents=True, exist_ok=True)
                generate_subset(gen_binary, tier_dir, sel, world_dir, point_dir / "generate.log")
            run = run_godot(binary, sweep_plan(world_dir / WORLD_NAME, views, pi, measure_s), point_dir, timeout_s)
            fail = process_failure(run)
            entry: Dict[str, object] = {"id": pid, "subset_splats": subset_splats, "selection": selection_info,
                                        "process": {k: run[k] for k in ("exit_code", "timed_out", "wall_s")},
                                        "rss": run["rss"]}
            if fail:
                failures.append(f"{pid}: {fail}")
                entry["status"] = ps.INVALID
                entry["reason"] = fail
            else:
                trace, integrity = trace_integrity(run)
                entry["trace"] = integrity
                if trace is None:
                    failures.append(f"{pid}: {integrity['reason']}")
                else:
                    entry["views"] = summarise_sweep_point(trace, run, pid, subset_splats)
                    if integrity["status"] != ps.OK:
                        failures.append(f"{pid}: {integrity['reason']}")
            points.append(entry)
            if target is not None and not keep:
                shutil.rmtree(world_dir, ignore_errors=True)
    finally:
        if not keep:
            shutil.rmtree(scratch, ignore_errors=True)

    fits: Dict[str, object] = {}
    for view in views:
        vid = view["id"]
        vpoints = []
        for p in points:
            v = (p.get("views") or {}).get(vid)
            if v:
                vpoints.append(dict(v, id=p["id"]))
        fits[vid] = {
            "gpu_pipeline": ps.fit_frame_model(vpoints, "gpu_pipeline_mean_ms"),
            "wall_interval": ps.fit_frame_model(vpoints, "wall_interval_mean_ms"),
            "table": [
                {"point": v["id"], "drawn_mean": v["drawn_mean"],
                 "gpu_pipeline_mean_ms": v["gpu_pipeline_mean_ms"].get("value"),
                 "wall_interval_mean_ms": v["wall_interval_mean_ms"].get("value"),
                 "settled": v["settled"], "visible_cap_bound": v["visible_cap_bound"],
                 "settings_drifted": v["settings"].get("settings_drifted")}
                for v in vpoints
            ],
        }
        small = next((v for v in vpoints if v["id"] == "F_small"), None)
        fits[vid]["F_small"] = (
            {"drawn_mean": small["drawn_mean"], "gpu_pipeline_mean_ms": small["gpu_pipeline_mean_ms"],
             "wall_interval_mean_ms": small["wall_interval_mean_ms"],
             "gpu_frame_bracket_mean_ms": small["gpu_frame_bracket_mean_ms"]}
            if small else ps.missing("the F_small point did not produce a measurement")
        )
    drifted = any(
        v["settings"].get("settings_drifted")
        for p in points for v in (p.get("views") or {}).values()
    )
    return {
        "tier": SWEEP_TIER,
        "method": "T0 chunk subsets (error-diffusion over Morton order) at fixed views, prefix LOD off; "
                  "mean ms = F + k * N, N = mean drawn splats in millions",
        "settings_deviation_from_tuned": {k: v for k, v in SWEEP_SETTINGS.items() if TUNED_SETTINGS.get(k) != v},
        "views": views,
        "points": points,
        "fits": fits,
        "settings_drifted": drifted,
        "label": "settings_drifted" if drifted else "",
        "failures": failures,
    }


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def parse_worlds(values: Sequence[str]) -> Dict[str, Path]:
    worlds: Dict[str, Path] = {}
    for v in values:
        if "=" not in v:
            raise ProbeError(f"--world expects TIER=DIR, got {v!r}", EXIT_NOT_RUN)
        tier, d = v.split("=", 1)
        if tier in worlds:
            raise ProbeError(f"world {tier} given twice", EXIT_NOT_RUN)
        worlds[tier] = Path(d)
    return worlds


def _finite(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {k: _finite(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_finite(v) for v in value]
    return value


def write_summary(out: Path, summary: dict) -> None:
    summary = _finite(summary)
    out.mkdir(parents=True, exist_ok=True)
    tmp = out / (SUMMARY_NAME + ".tmp")
    tmp.write_text(json.dumps(summary, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    os.replace(tmp, out / SUMMARY_NAME)


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--godot-binary", required=True, type=Path)
    ap.add_argument("--world", action="append", default=[], help="TIER=DIR (repeatable)")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--passes", type=int, default=2, help="path passes per world (first, repeat)")
    ap.add_argument("--no-sweep", action="store_true", help="skip the F/k sweep")
    ap.add_argument("--no-path", action="store_true", help="skip the fly-through")
    ap.add_argument("--measure-s", type=float, default=3.0, help="sweep measurement window per view")
    ap.add_argument("--timeout-s", type=float, default=2400.0, help="per Godot process")
    ap.add_argument("--keep-sweep-worlds", action="store_true")
    ap.add_argument("--max-fps", type=int, default=0,
                    help="frame-rate cap for the fly-through (0 = uncapped, the measurement setting; "
                         "a cap is only for capturing a compact test fixture)")
    args = ap.parse_args(argv)

    out = args.out.resolve()
    summary: Dict[str, object] = {
        "schema": ps.SUMMARY_SCHEMA,
        "adr": "docs/architecture/adr-beyond-vram-worlds.md (BV-07)",
        "report_only": True,
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "settings": {"tuned": TUNED_SETTINGS, "visible_cap": TUNED_VISIBLE_CAP, "far_m": TUNED_FAR_M,
                     "camera": CAMERA, "resolution": list(RESOLUTION), "settle_deadlines_s": SETTLE},
        "worlds": {},
        "problems": [],
    }
    summary["settings"]["max_fps"] = args.max_fps
    code = EXIT_OK
    failed = False
    try:
        worlds = parse_worlds(args.world)
        if not worlds:
            raise ProbeError("NOT_RUN: no --world given; there is nothing to probe", EXIT_NOT_RUN)
        if not args.godot_binary.is_file():
            raise ProbeError(f"NOT_RUN: no Godot binary at {args.godot_binary}", EXIT_NOT_RUN)
        binary, note = resolve_binary(args.godot_binary.resolve())
        summary["binary"] = binary_identity(binary)
        if note:
            summary["binary"]["note"] = note
        manifests = {}
        for tier, d in worlds.items():
            if not (d / WORLD_NAME).is_file() or not (d / MANIFEST_NAME).is_file():
                raise ProbeError(f"NOT_RUN: world {tier} has no {WORLD_NAME} + {MANIFEST_NAME} under {d}", EXIT_NOT_RUN)
            manifests[tier] = json.loads((d / MANIFEST_NAME).read_text(encoding="utf-8"))

        for tier, d in worlds.items():
            wout = out / tier
            wout.mkdir(parents=True, exist_ok=True)
            manifest = manifests[tier]
            ws: Dict[str, object] = {
                "dir": str(d), "generator_params_sha": manifest.get("generator_params_sha"),
                "world_file_sha256": manifest.get("world", {}).get("file_sha256"),
                "splat_count": manifest.get("world", {}).get("splat_count"),
                "chunk_count": manifest.get("world", {}).get("chunk_count"),
                "file_bytes": manifest.get("world", {}).get("file_bytes"),
            }
            summary["worlds"][tier] = ws
            if args.no_path:
                ws["flythrough"] = {"status": "NOT_RUN", "reason": "--no-path"}
                continue
            path = probe_path.resolve_path(manifest)
            (wout / "path.json").write_text(json.dumps(path, indent=1), encoding="utf-8")
            gt = probe_groundtruth.build_groundtruth(manifest, path, probe_path.checkpoints(path), CAMERA, ASPECT)
            (wout / probe_groundtruth.GROUNDTRUTH_NAME).write_text(json.dumps(gt, indent=1), encoding="utf-8")
            ws["path_sha256"] = path["path_sha256"]
            ws["path_moving_length_m"] = round(probe_path.moving_length_m(path), 3)
            ws["groundtruth"] = {"file": f"{tier}/{probe_groundtruth.GROUNDTRUTH_NAME}",
                                 "sha256": gt["groundtruth_sha256"]}
            run = run_godot(binary, path_plan(d / WORLD_NAME, path, args.passes, args.max_fps), wout / "path_run", args.timeout_s)
            ws["process"] = {k: run[k] for k in ("exit_code", "timed_out", "wall_s", "log")}
            ws["rss"] = run["rss"]
            res = run["result"] or {}
            ws["driver"] = {k: res.get(k) for k in ("adapter", "api_version", "is_debug_build", "display_server",
                                                    "vsync_mode", "load_usec", "node_readback",
                                                    "project_settings_readback", "reason")}
            fail = process_failure(run)
            if fail:
                summary["problems"].append(f"{tier} fly-through: {fail}")
                ws["flythrough"] = {"status": ps.INVALID, "reason": fail}
                failed = True
                continue
            trace, integrity = trace_integrity(run)
            ws["trace"] = integrity
            if trace is None:
                summary["problems"].append(f"{tier}: {integrity['reason']}")
                continue
            if integrity["status"] != ps.OK:
                summary["problems"].append(f"{tier}: {integrity['reason']}")
            ws["flythrough"] = summarise_path(trace, run, path, gt, args.passes)
            summary["problems"] += [f"{tier}: {p}" for p in required_path_metrics(ws)]

        if not args.no_sweep:
            if SWEEP_TIER not in worlds:
                summary["sweep"] = ps.missing(f"the F/k sweep runs on {SWEEP_TIER}, which was not given")
                summary["problems"].append("F/k sweep not run: no T0 world")
            else:
                sw = run_sweep(binary, args.godot_binary.resolve(), worlds[SWEEP_TIER], out, args.measure_s,
                               args.timeout_s, args.keep_sweep_worlds)
                summary["sweep"] = sw
                summary["problems"] += [f"sweep: {f}" for f in sw["failures"]]
                for vid, fit in sw["fits"].items():
                    for key in ("gpu_pipeline", "wall_interval"):
                        if fit[key].get("status") != ps.OK:
                            summary["problems"].append(f"sweep {vid} {key} fit: {fit[key].get('reason')}")
                if any(p.get("status") == ps.INVALID for p in sw["points"]):
                    failed = True
        else:
            summary["sweep"] = {"status": "NOT_RUN", "reason": "--no-sweep"}

        # ADR V7 / D-6: drift in MEASURED frames (the moving rows of each pass; the sweep's
        # measurement windows). The load frames before them are reported, not labelled.
        drift = [
            tier for tier, w in summary["worlds"].items()
            if isinstance(w.get("flythrough"), dict)
            and any(p["settings"].get("settings_drifted") for p in w["flythrough"].get("passes", []))
        ]
        summary["settings_drifted"] = bool(drift) or bool((summary.get("sweep") or {}).get("settings_drifted"))
        summary["labels"] = ["settings_drifted"] if summary["settings_drifted"] else []
        if failed:
            code = EXIT_FAIL
        elif summary["problems"]:
            code = EXIT_INVALID
    except ProbeError as exc:
        summary["problems"].append(str(exc))
        code = exc.code
    except (OSError, ValueError, KeyError, probe_path.PathError) as exc:
        summary["problems"].append(f"probe error: {type(exc).__name__}: {exc}")
        code = EXIT_FAIL
    summary["exit_code"] = code
    summary["status"] = {EXIT_OK: "OK", EXIT_FAIL: "FAIL", EXIT_INVALID: "INVALID", EXIT_NOT_RUN: "NOT_RUN"}[code]
    summary["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_summary(out, summary)
    print(f"run_probe: {summary['status']} ({len(summary['problems'])} problem(s)); summary at {out / SUMMARY_NAME}")
    for p in summary["problems"]:
        print(f"run_probe:   {p}")
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
