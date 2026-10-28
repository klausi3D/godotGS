#!/usr/bin/env python3
"""S1b matched-arm render + quality-metrics harness (HLOD stack PR 11, Refs #1131).

Consumes the export written by the HLODQualityExport driver
(modules/gaussian_splatting/tests/test_hlod_matched_arms.h): per row a
``reference.gsplatworld`` plus ``sat``, ``a_area`` and ``s1b`` arm worlds and a
``manifest.json``. For each row it renders all four worlds at the frozen view camera
(one fresh Godot process per world, identical settings, ``s1b_render_arm.gd``), computes
the common region-of-change ROI, PSNR and SSIM per arm against the reference
(``BenchmarkVisualMetrics`` via ``s1b_metrics.gd``) and applies the UNCHANGED S1b rule
from ADR docs/architecture/adr-hlod-streaming.md section 8 test 12 and
C:/godotgs-evidence/hlod-s1b-quality-20261020/bounded-acceptance.md:

    pass iff PSNR(S1b) >= max(PSNR(SAT), PSNR(A-area)) - 0.5 dB

Constants below are the frozen spec; they must not be tuned. The pure functions
(ROI, decision rule, fail-closed validation) are unit-tested in test_s1b_quality.py
without a GPU. Absence of a signal is never a pass: a missing arm, mismatched count,
zero-splat render, black frame, loss counter, stale fixture or empty ROI is INVALID.

Usage:
    python tests/runtime/hlod_s1b_quality/s1b_quality.py \
        --godot <godot.windows.editor.x86_64.console.exe> \
        --export-dir <exporter output dir> --views <reconstructed-views-frozen.json> \
        --out <fresh evidence dir>
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent

# ---- Frozen spec (do not tune) ------------------------------------------------------
ROI_DIFF_THRESHOLD_8BIT = 8  # pixels differing by > 8/255 in any channel
ROI_PAD_PX = 8
DECISION_MARGIN_DB = 0.5
ARMS: Tuple[str, ...] = ("sat", "a_area", "s1b")
REFERENCE = "reference"
EXPECTED_VIEWPORT = (960, 540)
EXPECTED_FOV = 60.0
SSIM_MIN_DIM = 11  # BenchmarkVisualMetrics.SSIM_WINDOW_SIZE
PSNR_IDENTICAL_CAP = 100.0  # BenchmarkVisualMetrics returns 100.0 when mse <= 1e-7
MIN_NONBLACK_PIXELS = 64

# The frozen view declares far=1e6; the checkpoint found that renders no frame. The
# clamp is applied to EVERY arm, and s1b_render_arm.gd proves per arm that
# near < nearest depth and farthest corner distance < far for the AABB grown by 5 m.
DEFAULT_FAR_CLAMP = 1000.0

# Quality settings pinned for every arm (ADR 5.1: no LOD, no SH reduction, no cache,
# no jitter). Recorded in each arm's telemetry together with every other rendering
# setting; the harness requires all arms' settings to be identical.
PINNED_SETTINGS: Dict[str, Any] = {
    "lod/enabled": False,
    "lod/sh_reduction_enabled": False,
    "lod/splat_skip_enabled": False,
    "lod/opacity_fade_enabled": False,
    "lod/blend_enabled": False,
    "import/use_gsplatworld_cache": False,
}


class HarnessError(RuntimeError):
    """A condition that makes the run unusable (distinct from a row verdict)."""


# ---- Pure functions (unit-tested) ----------------------------------------------------
def changed_mask(reference: np.ndarray, arm: np.ndarray, threshold: int = ROI_DIFF_THRESHOLD_8BIT) -> np.ndarray:
    """Pixels where any channel differs from the reference by MORE than ``threshold``/255."""
    if reference.shape != arm.shape or reference.ndim != 3:
        raise HarnessError(f"image shape mismatch {reference.shape} vs {arm.shape}")
    diff = np.abs(reference.astype(np.int16) - arm.astype(np.int16))
    return (diff > threshold).any(axis=2)


def common_roi(
    reference: np.ndarray, arms: Dict[str, np.ndarray], pad: int = ROI_PAD_PX
) -> Optional[Tuple[int, int, int, int]]:
    """Bounding box (x0, y0, x1, y1; exclusive max) of the union of changed pixels over
    every arm, padded and clamped to the image. ``None`` when nothing changed."""
    if not arms:
        raise HarnessError("common ROI needs at least one arm")
    union = np.zeros(reference.shape[:2], dtype=bool)
    for arm in arms.values():
        union |= changed_mask(reference, arm)
    ys, xs = np.nonzero(union)
    if xs.size == 0:
        return None
    h, w = union.shape
    return (
        max(int(xs.min()) - pad, 0),
        max(int(ys.min()) - pad, 0),
        min(int(xs.max()) + 1 + pad, w),
        min(int(ys.max()) + 1 + pad, h),
    )


def crop(image: np.ndarray, roi: Tuple[int, int, int, int]) -> np.ndarray:
    x0, y0, x1, y1 = roi
    return image[y0:y1, x0:x1]


def decide(psnr: Dict[str, Any], margin_db: float = DECISION_MARGIN_DB) -> Dict[str, Any]:
    """The unchanged S1b rule. Any missing / non-finite PSNR makes the row INVALID,
    never a pass."""
    values: Dict[str, float] = {}
    for arm in ARMS:
        value = psnr.get(arm)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            return {"verdict": "INVALID", "reason": f"PSNR for {arm} missing or not finite: {value!r}"}
        values[arm] = float(value)
    best_baseline_arm = "sat" if values["sat"] >= values["a_area"] else "a_area"
    best_baseline = values[best_baseline_arm]
    threshold = best_baseline - margin_db
    margin = values["s1b"] - threshold
    return {
        "verdict": "PASS" if values["s1b"] >= threshold else "FAIL",
        "best_baseline_arm": best_baseline_arm,
        "best_baseline_psnr": best_baseline,
        "required_min_psnr": threshold,
        "s1b_psnr": values["s1b"],
        "margin_db": margin,
        "rule": f"PSNR(S1b) >= max(PSNR(SAT), PSNR(A-area)) - {margin_db} dB",
    }


LIVE_ZERO_COUNTERS = ("sampled_dropped_records", "sampled_raster_truncated_tiles", "overflow_drop_events", "raster_tile_cap_drop_events", "clamped_records", "overflow_tile_count")
LIVE_SERIAL_KEY = "sampled_drop_frame_serial"


def live_loss_failures(name: str, telemetry: Dict[str, Any]) -> List[str]:
    """Zero-loss evidence from the renderer's LIVE overflow readback (get_overflow_stats).

    get_render_stats()'s raster_overflow_tiles / raster_overlap_records read 0 regardless of
    load, so they prove nothing. Evidence here needs: a positive sampled_drop_frame_serial that
    strictly advances across the stable window, and every loss counter a present int equal to 0
    in every frame. A counter that is absent, non-int or not advancing is a failure."""
    frames = telemetry.get("frames")
    if not isinstance(frames, list) or len(frames) < 2:
        return [f"{name}: no frame samples for the live loss counters"]
    failures: List[str] = []
    serials = [f.get("live_" + LIVE_SERIAL_KEY) for f in frames]
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in serials) or serials[0] <= 0 or serials[-1] <= serials[0]:
        failures.append(f"{name}: live overflow readback did not advance (serials {serials}); zero loss is unproven")
    inserts = frames[-1].get("live_aggregated_records")
    if isinstance(inserts, bool) or not isinstance(inserts, int) or inserts <= 0:
        failures.append(f"{name}: live tile-record total (aggregated_records) is {inserts!r}: counters unpopulated, zero loss is unproven")
    for key in LIVE_ZERO_COUNTERS:
        for f in frames:
            v = f.get("live_" + key)
            if isinstance(v, bool) or not isinstance(v, int):
                failures.append(f"{name}: live loss counter {key} missing or not an int ({v!r})")
                break
            if v != 0:
                failures.append(f"{name}: loss observed, {key}={v}")
                break
    return failures


def srgb_to_linear(image: np.ndarray) -> np.ndarray:
    c = image.astype(np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def psnr_array(a: np.ndarray, b: np.ndarray, linear: bool) -> float:
    """Full-resolution PSNR over all channels (peak 1.0) in sRGB-encoded or linear light.
    Informational: the gate uses BenchmarkVisualMetrics (sRGB, downsampled to <=256 px)."""
    if a.shape != b.shape:
        raise HarnessError(f"shape mismatch {a.shape} vs {b.shape}")
    x, y = (srgb_to_linear(a), srgb_to_linear(b)) if linear else (a.astype(np.float64) / 255.0, b.astype(np.float64) / 255.0)
    mse = float(np.mean((x - y) ** 2))
    return math.inf if mse <= 0.0 else 10.0 * math.log10(1.0 / mse)


def far_clamp_clips_nothing(min_depth: float, max_depth: float, max_corner_distance: float, near: float, far: float) -> bool:
    """The far clamp clips nothing: every corner of the (margin-grown) asset bounds is closer than
    ``far`` along the view axis and in distance. Valid for interior cameras too: ``min_depth`` may be
    <= ``near`` (camera inside the bounds); near-plane clipping is the frozen intrinsic and is
    identical for the reference and every arm, so it is not a failure. Non-finite input fails."""
    return (
        all(math.isfinite(v) for v in (min_depth, max_depth, max_corner_distance, near, far))
        and near > 0.0
        and max_depth < far
        and max_corner_distance < far
    )


def find_view(views: Dict[str, Any], row_id: str) -> Dict[str, Any]:
    """Match ``holzbank_d2_tau4.0`` to exactly one frozen view row."""
    try:
        asset, label, tau_text = row_id.split("_", 2)
        tau = float(tau_text.removeprefix("tau"))
    except ValueError as exc:
        raise HarnessError(f"unparseable row id {row_id!r}") from exc
    matches = [
        r
        for r in views.get("rows", [])
        if r.get("asset") == asset and r.get("distance_label") == label and float(r.get("tau_pixels", -1)) == tau
    ]
    if len(matches) != 1:
        raise HarnessError(f"row {row_id}: {len(matches)} matching frozen views (need exactly 1)")
    return matches[0]


def validate_row(
    manifest: Dict[str, Any],
    view: Dict[str, Any],
    telemetry: Dict[str, Optional[Dict[str, Any]]],
    far_clamp: float,
) -> List[str]:
    """Fail-closed validation of one row's four renders. Returns every failure found;
    an empty list means the row's renders are admissible for scoring."""
    failures: List[str] = []
    files = manifest.get("files", {})
    names = (REFERENCE,) + ARMS
    for name in names:
        t = telemetry.get(name)
        if not isinstance(t, dict):
            failures.append(f"{name}: no telemetry (missing arm or render did not run)")
            continue
        if t.get("status") != "ok":
            failures.append(f"{name}: render status {t.get('status')!r}: {t.get('reason')!r}")
            continue
        entry = files.get(name, {})
        if not entry:
            failures.append(f"{name}: not listed in manifest files")
            continue
        if t.get("world_sha256") != entry.get("sha256"):
            failures.append(f"{name}: world sha256 differs from manifest (stale/substituted fixture)")
        expected = int(entry.get("splats", -1))
        if expected <= 0:
            failures.append(f"{name}: manifest splat count {expected} (zero-splat fixture)")
        for key in ("world_splat_count", "data_count"):
            if t.get(key) != expected:
                failures.append(f"{name}: {key}={t.get(key)} != manifest splats {expected}")
        drawn = t.get("drawn") or {}
        if drawn.get("total_splats") != expected:
            failures.append(f"{name}: renderer total_splats={drawn.get('total_splats')} != manifest {expected}")
        if not isinstance(drawn.get("visible_splats"), int) or drawn["visible_splats"] <= 0:
            failures.append(f"{name}: no visible splats reported (zero-splat render)")
        if t.get("sh_first_order_count") != manifest.get("sh_first_order_count") or t.get("sh_high_order_count") != manifest.get(
            "sh_high_order_count"
        ):
            failures.append(f"{name}: SH term counts differ from manifest")
        if not isinstance(t.get("nonblack_pixels"), int) or t["nonblack_pixels"] < MIN_NONBLACK_PIXELS:
            failures.append(f"{name}: black frame (nonblack_pixels={t.get('nonblack_pixels')})")
        if t.get("distinct_frame_hashes") != 1:
            failures.append(f"{name}: unstable static frames (distinct_frame_hashes={t.get('distinct_frame_hashes')})")
        if t.get("world_has_hlod_tree") is not False:
            failures.append(f"{name}: world carries an HLOD tree (arm must be a materialized cut)")
        cam = t.get("camera") or {}
        if tuple(int(v) for v in cam.get("viewport", (0, 0))) != EXPECTED_VIEWPORT or cam.get("fov") != EXPECTED_FOV:
            failures.append(f"{name}: camera viewport/fov differ from the frozen 960x540 / 60 deg")
        if cam.get("far") != far_clamp:
            failures.append(f"{name}: far plane {cam.get('far')} != declared clamp {far_clamp}")
        clip = t.get("clip_proof") or {}
        if not far_clamp_clips_nothing(
            clip.get("min_depth", math.nan),
            clip.get("max_depth", math.nan),
            clip.get("max_corner_distance", math.nan),
            clip.get("near", math.nan),
            clip.get("far", math.nan),
        ):
            failures.append(f"{name}: near/far clip proof missing or failed")
        failures.extend(live_loss_failures(name, t))
    if failures and any(f.endswith("no telemetry (missing arm or render did not run)") for f in failures):
        return failures
    good = {n: telemetry[n] for n in names if isinstance(telemetry.get(n), dict) and telemetry[n].get("status") == "ok"}
    if len(good) != len(names):
        return failures

    # Matched-arm contract: the three arms are materialized cuts of one common budget.
    cut_splats = manifest.get("cut_splats")
    counts = {a: (good[a].get("drawn") or {}).get("total_splats") for a in ARMS}
    if len(set(counts.values())) != 1 or any(c != cut_splats for c in counts.values()):
        failures.append(f"arms are not equal-count: {counts} vs manifest cut_splats {cut_splats}")
    if (good[REFERENCE].get("drawn") or {}).get("total_splats") != manifest.get("domain_splats"):
        failures.append("reference total_splats != manifest domain_splats")
    if good[REFERENCE].get("sh_degree") != good["s1b"].get("sh_degree"):
        failures.append("reference and arms differ in SH degree")
    for a in ARMS:
        if good[a].get("sh_degree") != good[REFERENCE].get("sh_degree"):
            failures.append(f"{a}: SH degree differs from reference")
    # Identical camera and settings in every arm.
    for key in ("project_settings", "camera_job", "engine_version", "adapter"):
        ref_value = good[REFERENCE].get(key)
        for n in names:
            if good[n].get(key) != ref_value:
                failures.append(f"{n}: {key} differs from the reference render")
    # Camera is the frozen view's.
    cam_job = good[REFERENCE].get("camera_job") or {}
    for key in ("position", "target"):
        if [float(v) for v in cam_job.get(key, [])] != [float(v) for v in view.get(key, [])]:
            failures.append(f"camera {key} is not the frozen view's {key}")
    if cam_job.get("basis_rows") != view.get("basis_rows"):
        failures.append("camera basis is not the frozen view's")
    return failures


def row_verdict(
    validation_failures: Sequence[str],
    reference: np.ndarray,
    arms: Dict[str, np.ndarray],
    scores: Optional[Dict[str, Dict[str, Any]]],
) -> Dict[str, Any]:
    """Combine validation, ROI disposition and the decision rule. ``scores`` maps arm to
    {psnr, ssim, ssim_valid}; it may be None when the ROI is empty or validation failed."""
    if validation_failures:
        return {"verdict": "INVALID", "reason": "render validation failed", "failures": list(validation_failures)}
    roi = common_roi(reference, arms)
    if roi is None:
        identical = all(np.array_equal(reference, a) for a in arms.values())
        if identical:
            return {
                "verdict": "IDENTICAL_NOT_SCORED",
                "reason": "every arm is byte-identical to the reference; there is no region of change to score",
                "informative": False,
            }
        return {
            "verdict": "INVALID",
            "reason": "no pixel differs by more than 8/255 yet the arms are not identical: empty ROI is not a score",
        }
    if scores is None:
        return {"verdict": "INVALID", "reason": "metrics were not computed", "roi": list(roi)}
    psnr = {a: scores.get(a, {}).get("psnr") for a in ARMS}
    result = decide(psnr)
    result["roi"] = list(roi)
    result["roi_size"] = [roi[2] - roi[0], roi[3] - roi[1]]
    return result


# ---- Orchestration (needs Godot + a GPU) --------------------------------------------
def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def busy_processes() -> List[str]:
    """Names of processes that make a GPU run untrustworthy on this shared machine."""
    if sys.platform != "win32":
        return []
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, check=False).stdout
    busy = []
    for line in out.splitlines():
        name = line.split('","')[0].strip('"').lower()
        if name.startswith("runner.worker") or name.startswith("godot"):
            busy.append(name)
    return busy


def gpu_utilization() -> Optional[int]:
    exe = shutil.which("nvidia-smi")
    if not exe:
        return None
    out = subprocess.run(
        [exe, "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"], capture_output=True, text=True, check=False
    ).stdout.strip()
    try:
        return int(out.splitlines()[0])
    except (ValueError, IndexError):
        return None


def wait_for_idle_gpu(max_wait_s: float, util_limit: int = 30) -> None:
    deadline = time.monotonic() + max_wait_s
    while True:
        busy = busy_processes()
        util = gpu_utilization()
        if not busy and (util is None or util <= util_limit):
            return
        if time.monotonic() >= deadline:
            raise HarnessError(f"GPU not idle after {max_wait_s}s: processes={busy} utilization={util}")
        time.sleep(15)


def run_godot(godot: Path, extra: Sequence[str], timeout_s: float, project: Optional[Path] = None) -> subprocess.CompletedProcess:
    cmd = [str(godot), "--path", str(project or HERE), *extra]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s, check=False)


def render_world(godot: Path, world: Path, entry: Dict[str, Any], camera: Dict[str, Any], out_dir: Path, name: str, timeout_s: float, settings: Optional[Dict[str, Any]] = None, project: Optional[Path] = None) -> Dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    job = {
        "world": str(world).replace("\\", "/"),
        "expected_sha256": entry["sha256"],
        "expected_splats": entry["splats"],
        "camera": camera,
        "project_settings": settings if settings is not None else PINNED_SETTINGS,
        "out_png": str(out_dir / f"{name}.png").replace("\\", "/"),
        "out_json": str(out_dir / f"{name}.telemetry.json").replace("\\", "/"),
    }
    job_path = out_dir / f"{name}.job.json"
    job_path.write_text(json.dumps(job, indent=2))
    tel_path = Path(job["out_json"])
    if tel_path.exists():
        tel_path.unlink()
    proc = run_godot(godot, ["--script", "s1b_render_arm.gd", "--", f"--job={str(job_path).replace(chr(92), '/')}"], timeout_s, project)
    (out_dir / f"{name}.stdout.log").write_text(proc.stdout + "\n--- stderr ---\n" + proc.stderr)
    if tel_path.exists():
        telemetry = json.loads(tel_path.read_text())
    else:
        telemetry = {"status": "failed", "reason": f"no telemetry written (exit {proc.returncode})"}
    telemetry["exit_code"] = proc.returncode
    return telemetry


def compute_metrics(godot: Path, pairs: List[Dict[str, str]], work: Path, timeout_s: float = 600) -> Dict[str, Dict[str, Any]]:
    out_json = work / "metrics.out.json"
    if out_json.exists():
        out_json.unlink()
    job_path = work / "metrics.job.json"
    job_path.write_text(json.dumps({"pairs": pairs, "out_json": str(out_json).replace("\\", "/")}, indent=2))
    proc = run_godot(godot, ["--headless", "--script", "s1b_metrics.gd", "--", f"--job={str(job_path).replace(chr(92), '/')}"], timeout_s)
    (work / "metrics.stdout.log").write_text(proc.stdout + "\n--- stderr ---\n" + proc.stderr)
    if not out_json.exists():
        raise HarnessError(f"metrics script wrote no output (exit {proc.returncode})")
    data = json.loads(out_json.read_text())
    results = {}
    for entry in data["results"]:
        psnr, ssim = entry.get("psnr"), entry.get("ssim")
        entry["ssim_valid"] = (
            ssim is not None and entry["width"] >= 1 and min(entry["width"], entry["height"]) >= SSIM_MIN_DIM and ssim > 0.0
        )
        if entry.get("error") or psnr is None:
            entry["psnr"] = None  # a failed metric is never a number
        results[entry["id"]] = entry
    return results


def save_png(array: np.ndarray, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(array).save(path)


def load_png(path: Path) -> np.ndarray:
    with Image.open(path) as img:
        return np.asarray(img.convert("RGB")).copy()


def score_set(godot: Path, work: Path, tag: str, reference: np.ndarray, arms: Dict[str, np.ndarray], roi: Tuple[int, int, int, int]) -> Dict[str, Dict[str, Any]]:
    crops = work / f"crops_{tag}"
    save_png(crop(reference, roi), crops / "reference.png")
    pairs = []
    for name, arr in arms.items():
        save_png(crop(arr, roi), crops / f"{name}.png")
        pairs.append({"id": name, "a": str(crops / "reference.png").replace("\\", "/"), "b": str(crops / f"{name}.png").replace("\\", "/")})
    return compute_metrics(godot, pairs, work)


def perturb(image: np.ndarray, shift_px: int = 3) -> np.ndarray:
    """Deterministic deliberate damage for the negative control: a horizontal shift."""
    return np.roll(image, shift_px, axis=1)


def cut_decisions(manifest: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Per-interior-node decision and the baker's recorded errors, from the exporter manifest."""
    out = []
    for node in manifest.get("cut", []):
        if node.get("kind") == "leaf":
            continue
        out.append(
            {
                "node": node.get("node"),
                "decision": node.get("kind"),
                "candidate_count": node.get("candidate_count"),
                "error_chosen": node.get("error_chosen"),
                "error_rejected": node.get("error_rejected"),
            }
        )
    return out


def live_loss_summary(telemetry: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    if not telemetry or not telemetry.get("frames"):
        return None
    frames = telemetry["frames"]
    keys = [LIVE_SERIAL_KEY, *LIVE_ZERO_COUNTERS, "raster_tile_cap", "aggregated_records"]
    return {
        "serial_first": frames[0].get("live_" + LIVE_SERIAL_KEY),
        "serial_last": frames[-1].get("live_" + LIVE_SERIAL_KEY),
        **{k: frames[-1].get("live_" + k) for k in keys[1:]},
        "visible_splats": frames[-1].get("visible_splats"),
        "stale_render_stats_context": {
            k: frames[-1].get(k) for k in ("raster_overflow_tiles", "raster_overlap_records", "raster_max_splats_per_tile")
        },
    }


def run_loss_control(godot: Path, export_dir: Path, camera: Dict[str, Any], out_root: Path, timeout_s: float) -> Dict[str, Any]:
    """Prove the live loss counters can move: render the full reference with the per-tile raster
    cap forced to the 256-record minimum. It must be reported as loss (render fails closed with nonzero
    counters). If it renders clean, the counters are unpopulated and zero-loss claims are void."""
    world = export_dir / "reference.gsplatworld"
    out_dir = out_root / "control_forced_cap"
    entry = {"sha256": sha256_file(world), "splats": 0}
    # expected splat count comes from the normal reference telemetry of the same run
    ref_tel = json.loads((out_root / "_reference_count.json").read_text())
    entry["splats"] = ref_tel["splats"]
    # The sorting config is read when the project loads, not from a later set_setting, so the
    # cap is forced through a throwaway copy of the harness project (settings_changed does not
    # re-read it: first attempt left raster_tile_cap at 12288).
    project = out_dir / "project"
    project.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / "s1b_render_arm.gd", project / "s1b_render_arm.gd")
    text = (HERE / "project.godot").read_text()
    text += "\n[rendering]\n\ngaussian_splatting/gpu_sorting/max_raster_splats_per_tile=256\n"
    (project / "project.godot").write_text(text)
    return render_world(godot, world, entry, camera, out_dir, "reference_cap256", timeout_s, dict(PINNED_SETTINGS), project)


def run_row(godot: Path, export_dir: Path, views: Dict[str, Any], row_dir_name: str, out_root: Path, far_clamp: float, wait_s: float, timeout_s: float, loss_control: bool = False) -> Dict[str, Any]:
    row_src = export_dir / row_dir_name
    manifest = json.loads((row_src / "manifest.json").read_text())
    row_id = manifest["id"]
    view = find_view(views, row_id)
    row_out = out_root / row_id
    row_out.mkdir(parents=True, exist_ok=True)

    camera = {
        "vertical_fov_degrees": views["camera_rule"]["vertical_fov_degrees"],
        "viewport": views["camera_rule"]["viewport"],
        "near": views["camera_rule"]["near"],
        "far": far_clamp,
        "frozen_far": views["camera_rule"]["far"],
        "position": view["position"],
        "target": view["target"],
        "basis_rows": view["basis_rows"],
    }
    worlds = {REFERENCE: export_dir / "reference.gsplatworld"}
    for arm in ARMS:
        worlds[arm] = row_src / f"{arm}.gsplatworld"

    telemetry: Dict[str, Optional[Dict[str, Any]]] = {}
    for name in (REFERENCE,) + ARMS:
        entry = manifest["files"].get(name)
        if not entry or not worlds[name].is_file():
            telemetry[name] = None  # missing arm: fail closed in validate_row
            continue
        wait_for_idle_gpu(wait_s)
        telemetry[name] = render_world(godot, worlds[name], entry, camera, row_out, name, timeout_s)

    failures = validate_row(manifest, view, telemetry, far_clamp)
    result: Dict[str, Any] = {
        "id": row_id,
        "asset": view["asset"],
        "distance_m": view["distance_metres"],
        "tau_px": view["tau_pixels"],
        "cut_nodes": manifest.get("cut_nodes"),
        "cut_splats": manifest.get("cut_splats"),
        "domain_splats": manifest.get("domain_splats"),
        "cut_kinds_leaf_merged_selected": manifest.get("cut_kinds_leaf_merged_selected"),
        "sh_degree": (telemetry.get(REFERENCE) or {}).get("sh_degree"),
        "far_clamp": far_clamp,
        "frozen_far": views["camera_rule"]["far"],
        "_camera": camera,
        "files": {n: (manifest["files"].get(n) or {}) for n in (REFERENCE,) + ARMS},
        "drawn": {n: (telemetry[n] or {}).get("drawn") for n in (REFERENCE,) + ARMS},
        "validation_failures": failures,
        "decisions": cut_decisions(manifest),
        "live_loss_counters": {n: live_loss_summary(telemetry.get(n)) for n in (REFERENCE,) + ARMS},
    }
    # The three arms are expected to share sha when the choice made the same selection
    # everywhere (e.g. S1b picked SAT everywhere); recorded, not a defect.
    result["arm_world_sha256_equal"] = {
        "s1b_vs_sat": manifest["files"].get("s1b", {}).get("sha256") == manifest["files"].get("sat", {}).get("sha256"),
        "s1b_vs_a_area": manifest["files"].get("s1b", {}).get("sha256") == manifest["files"].get("a_area", {}).get("sha256"),
    }

    if failures:
        result["verdict"] = row_verdict(failures, np.zeros((1, 1, 3), np.uint8), {"x": np.zeros((1, 1, 3), np.uint8)}, None)
        (row_out / "result.json").write_text(json.dumps(result, indent=2))
        return result

    images = {n: load_png(Path(telemetry[n]["png"])) for n in (REFERENCE,) + ARMS}
    for n, arr in images.items():
        if arr.shape[:2][::-1] != EXPECTED_VIEWPORT:
            raise HarnessError(f"{n}: PNG size {arr.shape} != {EXPECTED_VIEWPORT}")
    reference = images[REFERENCE]
    arm_images = {a: images[a] for a in ARMS}
    roi = common_roi(reference, arm_images)
    scores: Optional[Dict[str, Dict[str, Any]]] = None
    if roi is not None:
        scores = score_set(godot, row_out, "real", reference, arm_images, roi)
        result["scores"] = {a: {"psnr": scores[a]["psnr"], "ssim": scores[a]["ssim"], "ssim_valid": scores[a]["ssim_valid"]} for a in ARMS}
        crops = {n: crop(images[n], roi) for n in (REFERENCE,) + ARMS}
        np_srgb = {a: psnr_array(crops[REFERENCE], crops[a], False) for a in ARMS}
        np_lin = {a: psnr_array(crops[REFERENCE], crops[a], True) for a in ARMS}
        result["psnr_by_color_space"] = {
            "gate": "srgb_benchmark_visual_metrics",
            "srgb_gate": {a: scores[a]["psnr"] for a in ARMS},
            "srgb_numpy_fullres": np_srgb,
            "linear_numpy_fullres": np_lin,
            "linear_rule_informational": decide(np_lin),
            "srgb_numpy_rule_informational": decide(np_srgb),
        }
        result["exact_pixel_identity_vs_reference"] = {a: bool(np.array_equal(reference, arm_images[a])) for a in ARMS}
        result["changed_pixels_vs_reference"] = {a: int(changed_mask(reference, arm_images[a]).sum()) for a in ARMS}
    result["verdict"] = row_verdict([], reference, arm_images, scores)

    # ---- Controls (exercised through the same pipeline) ----
    controls: Dict[str, Any] = {}
    # (1) reference vs itself: identical, PSNR at the metric cap (infinite), no ROI.
    full = (0, 0, reference.shape[1], reference.shape[0])
    ident = score_set(godot, row_out, "control_identity", reference, {"self": reference.copy()}, full)
    controls["reference_vs_itself"] = {
        "psnr_reported": ident["self"]["psnr"],
        "psnr": "inf" if np.array_equal(reference, reference.copy()) else ident["self"]["psnr"],
        "metric_cap": PSNR_IDENTICAL_CAP,
        "ssim": ident["self"]["ssim"],
        "roi": common_roi(reference, {"self": reference.copy()}),
        "row_disposition": row_verdict([], reference, {a: reference.copy() for a in ARMS}, None)["verdict"],
        "expected": "PSNR infinite (cap 100), empty ROI -> IDENTICAL_NOT_SCORED (never PASS)",
    }
    # (2) deliberately perturbed S1b arm must FAIL the unchanged rule.
    bad = dict(arm_images)
    bad["s1b"] = perturb(arm_images["s1b"])
    bad_roi = common_roi(reference, bad)
    if bad_roi is None:
        controls["perturbed_s1b"] = {"verdict": "INVALID", "reason": "perturbation produced no change"}
    else:
        bad_scores = score_set(godot, row_out, "control_perturbed", reference, bad, bad_roi)
        bad_decision = row_verdict([], reference, bad, bad_scores)
        bad_decision["perturbation"] = "S1b image shifted 3 px horizontally (np.roll); image-level, other arms untouched"
        bad_decision["scores"] = {a: {"psnr": bad_scores[a]["psnr"], "ssim": bad_scores[a]["ssim"]} for a in ARMS}
        bad_decision["expected"] = "FAIL"
        controls["perturbed_s1b"] = bad_decision
    # (3) discrimination of the guards themselves: a missing arm must be INVALID.
    missing = dict(telemetry)
    missing["a_area"] = None
    controls["missing_arm_is_invalid"] = {
        "verdict": row_verdict(validate_row(manifest, view, missing, far_clamp), reference, arm_images, scores)["verdict"],
        "expected": "INVALID",
    }
    result["controls"] = controls
    (row_out / "result.json").write_text(json.dumps(result, indent=2))
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--godot", required=True, type=Path)
    ap.add_argument("--export-dir", required=True, type=Path)
    ap.add_argument("--views", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--far", type=float, default=DEFAULT_FAR_CLAMP, help="far-plane clamp (frozen far=1e6 renders no frame)")
    ap.add_argument("--rows", nargs="*", help="row directory names (default: every row directory in the export)")
    ap.add_argument("--wait-idle-s", type=float, default=1200.0)
    ap.add_argument("--render-timeout-s", type=float, default=400.0)
    args = ap.parse_args()

    if args.out.exists() and any(args.out.iterdir()):
        raise HarnessError(f"--out must be fresh/empty: {args.out}")
    args.out.mkdir(parents=True, exist_ok=True)
    views = json.loads(args.views.read_text())
    rows = args.rows or sorted(p.name for p in args.export_dir.iterdir() if p.is_dir())
    if not rows:
        raise HarnessError("no rows in export")

    results = [run_row(args.godot, args.export_dir, views, r, args.out, args.far, args.wait_idle_s, args.render_timeout_s) for r in rows]

    # Forced-cap loss control, on the row whose reference view carries the most tile records (an
    # inside-root view can have only a few hundred, which a 256 cap cannot truncate). The control
    # proves the live loss counters can move; the run aborts if it does not.
    def inserts(r: Dict[str, Any]) -> int:
        live = (r.get("live_loss_counters") or {}).get(REFERENCE) or {}
        v = live.get("aggregated_records")
        return v if isinstance(v, int) else -1

    host = max(results, key=inserts)
    ref = host["files"][REFERENCE]
    (args.out / "_reference_count.json").write_text(json.dumps({"splats": ref.get("splats")}))
    wait_for_idle_gpu(args.wait_idle_s)
    tel = run_loss_control(args.godot, args.export_dir, host["_camera"], args.out, args.render_timeout_s)
    frames = tel.get("frames") or []
    last = frames[-1] if frames else {}
    detected = tel.get("status") == "failed" and any(int(last.get("live_" + k) or 0) > 0 for k in LIVE_ZERO_COUNTERS)
    control = {
        "row": host["id"],
        "setting": "gpu_sorting/max_raster_splats_per_tile=256",
        "render_status": tel.get("status"),
        "reason": tel.get("reason"),
        "live_counters": live_loss_summary(tel),
        "loss_detected": bool(detected),
        "expected": "loss_detected true: the live counters can move",
    }
    (args.out / "loss_control.json").write_text(json.dumps(control, indent=2))
    if not detected:
        raise HarnessError(f"forced-cap control on {host['id']} did not register loss: live loss counters are not trustworthy")

    verdicts = [r["verdict"]["verdict"] for r in results]
    summary = {
        "schema_version": 1,
        "views": str(args.views),
        "views_sha256": sha256_file(args.views),
        "export_dir": str(args.export_dir),
        "far_clamp": args.far,
        "rule": f"PSNR(S1b) >= max(PSNR(SAT), PSNR(A-area)) - {DECISION_MARGIN_DB} dB; common ROI >{ROI_DIFF_THRESHOLD_8BIT}/255 union padded {ROI_PAD_PX}px",
        "loss_control_forced_cap": control,
        "rows": results,
        "not_run": ["baum3 (6 rows): no baked world yet", "baum2 (2 rows): no baked world yet"],
        "all_rows_pass": all(v == "PASS" for v in verdicts),
        "verdicts": dict(zip([r["id"] for r in results], verdicts)),
    }
    (args.out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary["verdicts"], indent=2))
    return 0 if summary["all_rows_pass"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as exc:
        print(f"S1B_HARNESS_ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
