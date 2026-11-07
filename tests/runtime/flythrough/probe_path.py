#!/usr/bin/env python3
"""Arc-length fly-through path for the BV-07 probe lane.

ADR: docs/architecture/adr-beyond-vram-worlds.md section 7.2 ("Pose is a function
of path distance on an arc-length Catmull-Rom spline. `path_sha256` is frozen in
the thresholds file") and the segment table S0..S12.

This module is the only implementation of the path. It resolves the segment
table against one world (its sidecar manifest: extent, corridor walls, town) to a
list of segments, each with a pose table sampled at a fixed arc-length step. The
GDScript driver (probe_world.gd) only interpolates that table linearly at the
distance ``s = speed * wall_clock_elapsed``; it never evaluates a spline itself,
so there is no second implementation that could drift.

Determinism: every number comes from the manifest and the constants below, the
spline arithmetic is plain IEEE double, and the table is rounded to 1e-4 m /
1e-4 degree before hashing. ``path_sha256`` is sha256 of the canonical JSON of the
resolved path (sorted keys, no whitespace), so the same manifest always gives the
same hash and any change to the path, the world layout or the sampling changes it.

Scale. The ADR table is written for Tier A (2,048 m). Lengths are multiplied by
``scale = min(1, world_width / 2048)`` so the same segment kinds fit T0 (512 m,
scale 0.25); speeds, yaw rates, the climb height and the hold rules are not
scaled. The scale is part of the resolved path and of its hash.

Standard library only.
"""

from __future__ import annotations

import hashlib
import json
import math
from typing import Dict, List, Optional, Sequence, Tuple

PATH_SCHEMA = "godotgs-bv07-probe-path/1"

#: Arc-length step of the pose table, metres (before rounding).
TABLE_STEP_M = 1.0
#: Samples per Catmull-Rom span for the arc-length integral.
ARC_SAMPLES_PER_SPAN = 256

TIER_A_WIDTH_M = 2048.0
EYE_HEIGHT_M = 1.7
CRUISE_HEIGHT_M = 12.0
CLIMB_HEIGHT_M = 150.0
CLIMB_SECONDS = 20.0
CLIMB_PITCH_DEG = -45.0
WALK_YAW_AMPLITUDE_DEG = 15.0
WALK_YAW_PERIOD_M = 40.0
TOWN_YAW_RATE_DEG_S = 24.0
TOWN_SPEED_M_S = 10.0
SEAM_HEIGHT_M = 20.0
#: Heading = XZ direction to the point this far ahead on the curve; below
#: MIN_HEADING_XZ_M of horizontal travel, or where the curve is steeper than
#: about 60 degrees, the previous heading is kept.
HEADING_LOOKAHEAD_M = 4.0
MIN_HEADING_XZ_M = 0.5

#: Segment kinds (ADR section 7.2). "hold" and "stop" settle in place;
#: "teleport" moves the camera instantly, then settles; "move" follows a spline.
KIND_HOLD = "hold"
KIND_MOVE = "move"
KIND_STOP = "stop"
KIND_TELEPORT = "teleport"

Vec3 = Tuple[float, float, float]


class PathError(ValueError):
    """The manifest does not carry what the path needs."""


# --------------------------------------------------------------------------
# Catmull-Rom with arc-length parameterisation
# --------------------------------------------------------------------------


def _lerp(a: Vec3, b: Vec3, ta: float, tb: float, t: float) -> Vec3:
    if tb == ta:
        return a
    f = (t - ta) / (tb - ta)
    return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f)


def _cr_point(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3, u: float) -> Vec3:
    """Centripetal Catmull-Rom (alpha 0.5, Barry-Goldman) between p1 and p2 at u in [0, 1].

    The centripetal knots never form cusps or loops, even where control points are
    unevenly spaced (a uniform spline overshot backwards on the T0 S4 lead-in).
    """
    t0 = 0.0
    t1 = t0 + max(_dist(p0, p1), 1e-9) ** 0.5
    t2 = t1 + max(_dist(p1, p2), 1e-9) ** 0.5
    t3 = t2 + max(_dist(p2, p3), 1e-9) ** 0.5
    t = t1 + (t2 - t1) * u
    a1 = _lerp(p0, p1, t0, t1, t)
    a2 = _lerp(p1, p2, t1, t2, t)
    a3 = _lerp(p2, p3, t2, t3, t)
    b1 = _lerp(a1, a2, t0, t2, t)
    b2 = _lerp(a2, a3, t1, t3, t)
    return _lerp(b1, b2, t1, t2, t)


def _dist(a: Vec3, b: Vec3) -> float:
    return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)


class ArcLengthSpline:
    """Centripetal Catmull-Rom through ``points``, indexed by arc length.

    ``point_at(s)`` for s in [0, length] returns the position at arc length s,
    measured along a dense polyline of the spline (ARC_SAMPLES_PER_SPAN samples
    per span), with linear interpolation inside a sample. ``tangent_at(s)`` is the
    unit direction of travel there.
    """

    def __init__(self, points: Sequence[Vec3]) -> None:
        if len(points) < 2:
            raise PathError("a spline needs at least two control points")
        for a, b in zip(points, points[1:]):
            if _dist(a, b) <= 0.0:
                raise PathError(f"repeated control point {a}")
        # Phantom end points by reflection, so the end spans have a non-degenerate knot.
        first = tuple(2.0 * a - b for a, b in zip(points[0], points[1]))
        last = tuple(2.0 * a - b for a, b in zip(points[-1], points[-2]))
        pts = [first, *points, last]
        self._samples: List[Vec3] = []
        self._cumulative: List[float] = []
        total = 0.0
        prev: Optional[Vec3] = None
        for span in range(len(points) - 1):
            p0, p1, p2, p3 = pts[span], pts[span + 1], pts[span + 2], pts[span + 3]
            first = 0 if span == 0 else 1
            for i in range(first, ARC_SAMPLES_PER_SPAN + 1):
                q = _cr_point(p0, p1, p2, p3, i / ARC_SAMPLES_PER_SPAN)
                if prev is not None:
                    total += _dist(prev, q)
                self._samples.append(q)
                self._cumulative.append(total)
                prev = q
        self.length = total

    def _locate(self, s: float) -> Tuple[int, float]:
        s = min(max(s, 0.0), self.length)
        lo, hi = 0, len(self._cumulative) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self._cumulative[mid] <= s:
                lo = mid
            else:
                hi = mid
        span = self._cumulative[hi] - self._cumulative[lo]
        f = 0.0 if span <= 0.0 else (s - self._cumulative[lo]) / span
        return lo, f

    def point_at(self, s: float) -> Vec3:
        i, f = self._locate(s)
        a = self._samples[i]
        b = self._samples[min(i + 1, len(self._samples) - 1)]
        return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f)

    def tangent_at(self, s: float) -> Vec3:
        i, _ = self._locate(s)
        j = min(i + 1, len(self._samples) - 1)
        if j == i:
            i = max(i - 1, 0)
        a, b = self._samples[i], self._samples[j]
        d = _dist(a, b)
        if d <= 0.0:
            return (1.0, 0.0, 0.0)
        return ((b[0] - a[0]) / d, (b[1] - a[1]) / d, (b[2] - a[2]) / d)


def heading_deg(tangent: Vec3) -> float:
    """Godot yaw (degrees about +Y) that makes -Z point along the tangent's XZ part."""
    return math.degrees(math.atan2(-tangent[0], -tangent[2]))


# --------------------------------------------------------------------------
# Resolving the ADR segment table against a world
# --------------------------------------------------------------------------


def _features(manifest: dict, cls: str) -> List[dict]:
    return [f for f in manifest.get("features", []) if f.get("class") == cls]


def world_layout(manifest: dict) -> Dict[str, object]:
    """What the path needs from the manifest, with a typed error when absent."""
    try:
        x0, z0, x1, z1 = (float(v) for v in manifest["world"]["extent_m"])
    except (KeyError, TypeError, ValueError) as exc:
        raise PathError(f"manifest has no world.extent_m: {exc}") from exc
    if not (x1 > x0 and z1 > z0):
        raise PathError(f"degenerate world extent {[x0, z0, x1, z1]}")
    walls = _features(manifest, "wall")
    if len(walls) < 2:
        raise PathError(f"the corridor needs two walls, the manifest has {len(walls)}")
    walls = sorted(walls, key=lambda f: float(f["plane_z_m"]))
    wx0 = max(float(walls[0]["footprint"][0]), float(walls[1]["footprint"][0]))
    wx1 = min(float(walls[0]["footprint"][2]), float(walls[1]["footprint"][2]))
    if wx1 <= wx0:
        raise PathError("the corridor walls do not overlap in x")
    corridor_z = 0.5 * (float(walls[0]["plane_z_m"]) + float(walls[1]["plane_z_m"]))
    towns = _features(manifest, "town")
    if not towns:
        raise PathError("the manifest has no town")
    town = towns[0]
    tx0, tz0, tx1, tz1 = (float(v) for v in town["footprint"])
    tile = float(manifest.get("generator_params", {}).get("tile_size_m", 256))
    return {
        "extent": (x0, z0, x1, z1),
        "corridor": (wx0, wx1, corridor_z),
        "town_center": (0.5 * (tx0 + tx1), 0.5 * (tz0 + tz1)),
        "town_half": 0.5 * max(tx1 - tx0, tz1 - tz0),
        "tile_m": tile,
    }


def _clamp_xz(p: Vec3, extent: Tuple[float, float, float, float], margin: float) -> Vec3:
    x0, z0, x1, z1 = extent
    return (min(max(p[0], x0 + margin), x1 - margin), p[1], min(max(p[2], z0 + margin), z1 - margin))


def _move(
    seg_id: str,
    kind_label: str,
    speed: float,
    points: Sequence[Vec3],
    yaw_fn=None,
    pitch_fn=None,
    absolute_yaw_fn=None,
) -> dict:
    """A moving segment. The camera yaw is the travel heading plus ``yaw_fn(s, L)``, or,
    with ``absolute_yaw_fn(start_heading, s, L)``, exactly that function of the heading at
    s = 0 (the travel heading is then ignored)."""
    spline = ArcLengthSpline(points)
    start_heading: Optional[float] = None
    n = max(1, int(math.ceil(spline.length / TABLE_STEP_M)))
    table = []
    prev_yaw: Optional[float] = None
    for i in range(n + 1):
        s = spline.length * i / n
        p = spline.point_at(s)
        # Heading from the XZ displacement towards a point a little ahead (from a point a
        # little behind, at the end): a climbing or sharply turning spline has an unstable
        # XZ tangent.
        if s + HEADING_LOOKAHEAD_M <= spline.length:
            ref_a, ref_b = p, spline.point_at(s + HEADING_LOOKAHEAD_M)
        else:
            ref_a, ref_b = spline.point_at(max(s - HEADING_LOOKAHEAD_M, 0.0)), p
        dxz = (ref_b[0] - ref_a[0], 0.0, ref_b[2] - ref_a[2])
        xz = math.hypot(dxz[0], dxz[2])
        if xz >= MIN_HEADING_XZ_M and xz >= 0.5 * _dist(ref_a, ref_b):
            yaw = heading_deg(dxz)
        elif prev_yaw is not None:
            yaw = prev_yaw
        else:
            yaw = heading_deg(spline.tangent_at(s))
        if prev_yaw is not None:
            # Unwrap so that linear interpolation between rows never spins round.
            yaw += 360.0 * round((prev_yaw - yaw) / 360.0)
        prev_yaw = yaw
        if start_heading is None:
            start_heading = yaw
        if absolute_yaw_fn is not None:
            yaw = absolute_yaw_fn(start_heading, s, spline.length)
        else:
            yaw += yaw_fn(s, spline.length) if yaw_fn else 0.0
        pitch = pitch_fn(s, spline.length) if pitch_fn else 0.0
        table.append([round(s, 4), round(p[0], 4), round(p[1], 4), round(p[2], 4), round(yaw, 4), round(pitch, 4)])
    return {
        "id": seg_id,
        "kind": KIND_MOVE,
        "label": kind_label,
        "speed_m_s": speed,
        "length_m": round(spline.length, 4),
        "table": table,
    }


def _hold(seg_id: str, kind: str, label: str, pose: Sequence[float], checkpoint: Optional[str], extra_s: float = 0.0) -> dict:
    return {
        "id": seg_id,
        "kind": kind,
        "label": label,
        "pose": [round(float(v), 4) for v in pose],
        "checkpoint": checkpoint,
        "hold_after_ready_s": extra_s,
    }


def _last_pose(seg: dict) -> List[float]:
    if seg["kind"] == KIND_MOVE:
        return list(seg["table"][-1][1:])
    return list(seg["pose"])


def resolve_path(manifest: dict) -> dict:
    """The ADR section 7.2 path for this world. Pure function of the manifest."""
    lay = world_layout(manifest)
    extent = lay["extent"]
    x0, z0, x1, z1 = extent
    width = min(x1 - x0, z1 - z0)
    scale = min(1.0, width / TIER_A_WIDTH_M)
    margin = 0.03 * width
    wx0, wx1, cz = lay["corridor"]

    def L(m: float) -> float:
        return m * scale

    segs: List[dict] = []
    p0 = (wx0 + L(10.0) + 2.0, EYE_HEIGHT_M, cz)
    p0_pose = [p0[0], p0[1], p0[2], heading_deg((1.0, 0.0, 0.0)), 0.0]
    segs.append(_hold("S0", KIND_HOLD, "cold hold P0", p0_pose, "C0", extra_s=5.0))

    # S1 corridor walk, +-15 deg yaw, 6 m/s, 240 m (scaled).
    walk_end = min(p0[0] + L(240.0), wx1 - 2.0)
    s1 = _move(
        "S1", "corridor walk", 6.0,
        [p0, ((p0[0] + walk_end) / 2.0, EYE_HEIGHT_M, cz), (walk_end, EYE_HEIGHT_M, cz)],
        yaw_fn=lambda s, _l: WALK_YAW_AMPLITUDE_DEG * math.sin(2.0 * math.pi * s / WALK_YAW_PERIOD_M),
    )
    segs.append(s1)
    # S2 corridor drive, 25 m/s, 750 m (scaled), never past the corridor end.
    drive_end = min(walk_end + L(750.0), wx1 - 2.0)
    if drive_end - walk_end < 4.0:
        raise PathError("the corridor is too short for the drive segment")
    segs.append(_move(
        "S2", "corridor drive", 25.0,
        [(walk_end, EYE_HEIGHT_M, cz), ((walk_end + drive_end) / 2.0, EYE_HEIGHT_M, cz), (drive_end, EYE_HEIGHT_M, cz)],
    ))
    segs.append(_hold("S3", KIND_STOP, "stop", _last_pose(segs[-1]), "C1"))

    # S4 diagonal across the plain, 60 m/s, 1,500 m (scaled), at cruise height,
    # heading for the corner farthest from the corridor end.
    start = (drive_end, EYE_HEIGHT_M, cz)
    corners = [(x0, z0), (x0, z1), (x1, z0), (x1, z1)]
    far = max(corners, key=lambda c: (c[0] - start[0]) ** 2 + (c[1] - start[2]) ** 2)
    dx, dz = far[0] - start[0], far[1] - start[2]
    dn = math.hypot(dx, dz)
    ux, uz = dx / dn, dz / dn
    d4 = min(L(1500.0), dn - margin)
    # The climb to cruise height runs over at least twice its rise, so the spline does not
    # overshoot backwards (found on T-smoke, scale 0.125: a 180 degree heading flip).
    lead = max(L(40.0), 2.0 * (CRUISE_HEIGHT_M - EYE_HEIGHT_M))
    p4a = (start[0] + ux * lead, CRUISE_HEIGHT_M, start[2] + uz * lead)
    p4b = _clamp_xz((start[0] + ux * d4, CRUISE_HEIGHT_M, start[2] + uz * d4), extent, margin)
    segs.append(_move("S4", "diagonal across the plain", 60.0, [start, p4a, p4b]))
    # S5 burst, 120 m/s, 600 m (scaled): turn 90 deg and run along the edge.
    perp = (-uz, ux)
    p5 = _clamp_xz((p4b[0] + perp[0] * L(600.0), CRUISE_HEIGHT_M, p4b[2] + perp[1] * L(600.0)), extent, margin)
    if _dist(p5, p4b) < 4.0:
        perp = (uz, -ux)
        p5 = _clamp_xz((p4b[0] + perp[0] * L(600.0), CRUISE_HEIGHT_M, p4b[2] + perp[1] * L(600.0)), extent, margin)
    p5mid = ((p4b[0] + p5[0]) / 2.0, CRUISE_HEIGHT_M, (p4b[2] + p5[2]) / 2.0)
    segs.append(_move("S5", "burst", 120.0, [p4b, p5mid, p5]))
    segs.append(_hold("S6", KIND_STOP, "stop", _last_pose(segs[-1]), "C2"))

    # S7 reversal, 60 m/s, 900 m (scaled): back towards the town.
    tcx, tcz = lay["town_center"]
    town_edge = lay["town_half"] + 6.0
    tx, tz = tcx - p5[0], tcz - p5[2]
    tn = math.hypot(tx, tz)
    approach = (tcx - tx / tn * town_edge, EYE_HEIGHT_M + 4.0, tcz - tz / tn * town_edge)
    d7 = min(L(900.0), max(tn - town_edge, 4.0))
    p7 = (p5[0] + tx / tn * d7, CRUISE_HEIGHT_M, p5[2] + tz / tn * d7)
    if math.hypot(p7[0] - approach[0], p7[2] - approach[2]) > 2.0 * abs(p7[1] - approach[1]) + 4.0:
        p7_points = [p5, p7, approach]
    else:
        p7_points = [p5, approach]
    segs.append(_move("S7", "reversal", 60.0, p7_points))

    # S8 town 360 deg yaw at 10 m/s, 24 deg/s: 15 s, 150 m, along the town edge.
    yaw_seconds = 360.0 / TOWN_YAW_RATE_DEG_S
    town_len = TOWN_SPEED_M_S * yaw_seconds
    a = approach
    r = town_edge
    ang0 = math.atan2(a[2] - tcz, a[0] - tcx)
    arc = town_len / r
    ring = [
        (tcx + r * math.cos(ang0 + arc * k / 6.0), a[1], tcz + r * math.sin(ang0 + arc * k / 6.0))
        for k in range(7)
    ]
    ring = [_clamp_xz(p, extent, margin) for p in ring]
    # The camera turns a full 360 degrees at 24 deg/s from the heading it enters with,
    # independent of the ring's own turning (adding the ramp to the ring tangent gave only
    # ~78 degrees net on T-smoke).
    s8 = _move(
        "S8", "town 360 yaw", TOWN_SPEED_M_S, ring,
        absolute_yaw_fn=lambda h0, s, length: h0 + 360.0 * s / length,
    )
    segs.append(s8)
    segs.append(_hold("S8c", KIND_STOP, "town stop", _last_pose(s8), "C3"))

    # S9 teleport > 1.5 km (scaled): to the corner farthest from the town.
    tp_corner = max(corners, key=lambda c: (c[0] - tcx) ** 2 + (c[1] - tcz) ** 2)
    tp = _clamp_xz((tp_corner[0], CRUISE_HEIGHT_M, tp_corner[1]), extent, margin + L(32.0))
    tp_heading = heading_deg((tcx - tp[0], 0.0, tcz - tp[2]))
    segs.append(_hold("S9", KIND_TELEPORT, "teleport", [tp[0], tp[1], tp[2], tp_heading, 0.0], "C4"))

    # S10 climb to 150 m with pitch to -45 deg over 20 s.
    climb_speed = (CLIMB_HEIGHT_M - tp[1]) / CLIMB_SECONDS
    top = (tp[0], CLIMB_HEIGHT_M, tp[2])
    mid = (tp[0], 0.5 * (tp[1] + CLIMB_HEIGHT_M), tp[2])
    s10 = _move(
        "S10", "climb", climb_speed, [tp, mid, top],
        yaw_fn=None,
        pitch_fn=lambda s, length: CLIMB_PITCH_DEG * s / length,
    )
    # A vertical spline has no XZ heading: keep the teleport heading.
    for row in s10["table"]:
        row[4] = round(tp_heading, 4)
    segs.append(s10)
    segs.append(_hold("S10c", KIND_STOP, "climb top", _last_pose(s10), "C5"))

    # S11 along tile seams, 15 m/s, 450 m (scaled): along the seam x nearest the centre.
    tile = lay["tile_m"]
    seam_x = x0 + tile * round(((x0 + x1) / 2.0 - x0) / tile)
    if not (x0 < seam_x < x1):
        seam_x = (x0 + x1) / 2.0
    seam_len = min(L(450.0), (z1 - z0) - 2.0 * margin)
    sz0 = (z0 + z1) / 2.0 - seam_len / 2.0
    seam = [(seam_x, SEAM_HEIGHT_M, sz0), (seam_x, SEAM_HEIGHT_M, sz0 + seam_len / 2.0), (seam_x, SEAM_HEIGHT_M, sz0 + seam_len)]
    segs.append(_hold("S11t", KIND_TELEPORT, "to the seam", [seam[0][0], seam[0][1], seam[0][2], heading_deg((0.0, 0.0, 1.0)), 0.0], None))
    segs.append(_move("S11", "along tile seams", 15.0, seam))
    segs.append(_hold("S11c", KIND_STOP, "seam stop", _last_pose(segs[-1]), "C6"))

    # S12 teleport to P0, C7.
    segs.append(_hold("S12", KIND_TELEPORT, "teleport to P0", p0_pose, "C7"))

    path = {
        "schema": PATH_SCHEMA,
        "world_generator_params_sha": manifest.get("generator_params_sha"),
        "scale": round(scale, 6),
        "table_step_m": TABLE_STEP_M,
        "segments": segs,
    }
    path["path_sha256"] = path_sha256(path)
    return path


def canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def path_sha256(path: dict) -> str:
    body = {k: v for k, v in path.items() if k != "path_sha256"}
    return hashlib.sha256(canonical_json(body).encode("utf-8")).hexdigest()


def checkpoints(path: dict) -> List[Tuple[str, List[float]]]:
    out = []
    for seg in path["segments"]:
        if seg["kind"] != KIND_MOVE and seg.get("checkpoint"):
            out.append((seg["checkpoint"], list(seg["pose"])))
    return out


def moving_length_m(path: dict) -> float:
    return sum(seg["length_m"] for seg in path["segments"] if seg["kind"] == KIND_MOVE)


def interpolate(table: Sequence[Sequence[float]], s: float) -> List[float]:
    """Linear interpolation of a pose table at arc length s (what the driver does)."""
    if s <= table[0][0]:
        return list(table[0][1:])
    if s >= table[-1][0]:
        return list(table[-1][1:])
    lo, hi = 0, len(table) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if table[mid][0] <= s:
            lo = mid
        else:
            hi = mid
    a, b = table[lo], table[hi]
    f = (s - a[0]) / (b[0] - a[0])
    return [a[i] + (b[i] - a[i]) * f for i in range(1, 6)]
