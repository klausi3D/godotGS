#!/usr/bin/env python3
"""Per-checkpoint ground truth for the BV-07 probe lane (ADR section 6.4).

The BV-05 generator manifest deliberately carries no checkpoint data (the
checkpoints are a property of the path, not of the world). The probe lane writes
it next to its own output as ``world.groundtruth.json``:

* per checkpoint: the frustum chunk set, the expected drawn count (the splats of
  those chunks; an upper bound, since per-splat culling and the chunk route's
  prefix LOD can only remove splats), the expected beacons (beacon panels whose
  centre is inside the frustum), and the T-ref spec (the chunk selection a
  reference world would hold, and whether it is within the ADR section 6.1 12M
  T-ref cap);
* the oracle cut at the frozen drawn budget: NOT_APPLICABLE on the flat chunk
  route (it needs a V4 HLOD bake and a frozen ``drawn_budget_splats``, BV-26a);
* the oracle's expected unique uploaded bytes over the path for the flat arm: the
  stored bytes of every chunk that enters the frustum at any pose-table row of a
  moving segment or at a checkpoint (ADR V9).

The frustum is the probe camera's: Godot Euler YXZ (yaw, then pitch), -Z forward,
the horizontal field of view fixed (KEEP_WIDTH), near/far planes. A chunk is in the
frustum when its AABB is not entirely outside any of the six planes (the standard
conservative AABB test, the same one the streaming route's chunk culler uses
before its padding).

Standard library only.
"""

from __future__ import annotations

import hashlib
import json
import math
from typing import Dict, Iterable, List, Sequence, Tuple

GROUNDTRUTH_SCHEMA = "godotgs-bv07-groundtruth/1"
GROUNDTRUTH_NAME = "world.groundtruth.json"
TREF_MAX_SPLATS = 12_000_000

Plane = Tuple[float, float, float, float]


def camera_axes(yaw_deg: float, pitch_deg: float) -> Tuple[Tuple[float, float, float], ...]:
    """(forward, right, up) of a Godot camera with rotation_degrees = (pitch, yaw, 0)."""
    y = math.radians(yaw_deg)
    p = math.radians(pitch_deg)
    forward = (-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))
    right = (math.cos(y), 0.0, -math.sin(y))
    up = (
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0],
    )
    return forward, right, up


def frustum_planes(pose: Sequence[float], camera: dict, aspect: float) -> List[Plane]:
    """Inward-facing planes (n, d) with n.x + d >= 0 inside."""
    px, py, pz, yaw, pitch = (float(v) for v in pose[:5])
    f, r, u = camera_axes(yaw, pitch)
    fov = math.radians(float(camera["fov_deg"]))
    if camera.get("keep", "width") == "width":
        tan_h = math.tan(fov / 2.0)
        tan_v = tan_h / aspect
    else:
        tan_v = math.tan(fov / 2.0)
        tan_h = tan_v * aspect
    near = float(camera["near_m"])
    far = float(camera["far_m"])
    o = (px, py, pz)

    def plane(n: Tuple[float, float, float], point: Tuple[float, float, float]) -> Plane:
        ln = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        n = (n[0] / ln, n[1] / ln, n[2] / ln)
        return (n[0], n[1], n[2], -(n[0] * point[0] + n[1] * point[1] + n[2] * point[2]))

    def add(a, b, s=1.0):
        return (a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2])

    planes = [
        plane(f, add(o, f, near)),
        plane((-f[0], -f[1], -f[2]), add(o, f, far)),
    ]
    # Side planes through the eye, spanned by two adjacent corner rays; each normal is
    # oriented so that the forward axis is inside.
    corners = [
        add(add(f, r, sx * tan_h), u, sy * tan_v) for sx, sy in ((-1.0, 1.0), (1.0, 1.0), (1.0, -1.0), (-1.0, -1.0))
    ]
    for i in range(4):
        n = _cross(corners[i], corners[(i + 1) % 4])
        if n[0] * f[0] + n[1] * f[1] + n[2] * f[2] < 0.0:
            n = (-n[0], -n[1], -n[2])
        planes.append(plane(n, o))
    return planes


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def point_inside(planes: Sequence[Plane], p: Sequence[float]) -> bool:
    return all(n0 * p[0] + n1 * p[1] + n2 * p[2] + d >= 0.0 for n0, n1, n2, d in planes)


def aabb_intersects(planes: Sequence[Plane], aabb: Sequence[float]) -> bool:
    x0, y0, z0, x1, y1, z1 = (float(v) for v in aabb)
    for n0, n1, n2, d in planes:
        # The AABB corner farthest along the plane normal.
        px = x1 if n0 >= 0.0 else x0
        py = y1 if n1 >= 0.0 else y0
        pz = z1 if n2 >= 0.0 else z0
        if n0 * px + n1 * py + n2 * pz + d < 0.0:
            return False
    return True


def frustum_chunks(manifest: dict, pose: Sequence[float], camera: dict, aspect: float) -> List[dict]:
    planes = frustum_planes(pose, camera, aspect)
    far = float(camera["far_m"])
    px, py, pz = (float(v) for v in pose[:3])
    out = []
    for c in manifest["chunks"]:
        cx, cy, cz = c["center"]
        # Cheap reject: farther than far + radius cannot intersect.
        if math.sqrt((cx - px) ** 2 + (cy - py) ** 2 + (cz - pz) ** 2) > far + float(c["radius"]) + 1e-3:
            continue
        if aabb_intersects(planes, c["aabb"]):
            out.append(c)
    return out


def chunk_stored_bytes(chunk: dict) -> int:
    return int(chunk.get("gaussian_bytes", 0)) + int(chunk.get("sh_bytes", 0))


def checkpoint_truth(manifest: dict, pose: Sequence[float], camera: dict, aspect: float) -> Dict[str, object]:
    chunks = frustum_chunks(manifest, pose, camera, aspect)
    planes = frustum_planes(pose, camera, aspect)
    far = float(camera["far_m"])
    beacons = []
    for f in manifest.get("features", []):
        if f.get("class") != "beacon":
            continue
        c = f["center"]
        dist = math.sqrt(sum((float(c[i]) - float(pose[i])) ** 2 for i in range(3)))
        if dist <= far and point_inside(planes, c):
            beacons.append({"id": f["id"], "code_hex": f.get("code_hex"), "center": c})
    selection = sorted(int(c["plan_index"]) for c in chunks)
    splats = sum(int(c["count"]) for c in chunks)
    return {
        "pose": [round(float(v), 4) for v in pose[:5]],
        "frustum_chunks": sorted(int(c["index"]) for c in chunks),
        "frustum_chunk_count": len(chunks),
        "expected_drawn_upper_bound": splats,
        "expected_beacons": beacons,
        "tref_spec": {
            "select_plan_indices": selection,
            "splats": splats,
            "within_tref_cap": splats <= TREF_MAX_SPLATS,
            "tref_cap_splats": TREF_MAX_SPLATS,
        },
    }


def path_poses(path: dict) -> Iterable[Sequence[float]]:
    for seg in path["segments"]:
        if seg["kind"] == "move":
            for row in seg["table"]:
                yield row[1:6]
        else:
            yield seg["pose"]


def expected_unique_upload(manifest: dict, path: dict, camera: dict, aspect: float) -> Dict[str, object]:
    seen: Dict[int, dict] = {}
    poses = 0
    for pose in path_poses(path):
        poses += 1
        for c in frustum_chunks(manifest, pose, camera, aspect):
            seen[int(c["index"])] = c
    return {
        "method": "union of the frustum chunk sets at every pose-table row and checkpoint, without the "
        "streaming route's frustum padding or prefetch (a lower bound on what the route uploads)",
        "poses": poses,
        "chunks": len(seen),
        "stored_bytes": sum(chunk_stored_bytes(c) for c in seen.values()),
        "splats": sum(int(c["count"]) for c in seen.values()),
    }


def build_groundtruth(manifest: dict, path: dict, checkpoints: Sequence[Tuple[str, Sequence[float]]],
                      camera: dict, aspect: float) -> dict:
    body = {
        "schema": GROUNDTRUTH_SCHEMA,
        "generator_params_sha": manifest.get("generator_params_sha"),
        "world_file_sha256": manifest.get("world", {}).get("file_sha256"),
        "path_sha256": path.get("path_sha256"),
        "camera": camera,
        "aspect": aspect,
        "checkpoints": {name: checkpoint_truth(manifest, pose, camera, aspect) for name, pose in checkpoints},
        "oracle_cut": {
            "status": "NOT_APPLICABLE",
            "reason": "flat chunk route: the oracle cut needs a V4 HLOD bake and the frozen "
            "drawn_budget_splats (BV-26a); ADR section 6.4",
        },
        "expected_unique_upload": {"flat": expected_unique_upload(manifest, path, camera, aspect)},
    }
    body["groundtruth_sha256"] = hashlib.sha256(
        json.dumps(body, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return body
