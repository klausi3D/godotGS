#!/usr/bin/env python3
"""Per-transition HLOD pop metric for a dolly captured by hlod_pop_capture.gd (local tool, not a lane).

A whole-image difference per dolly step cannot see a pop: one node switching inside a 1920x1080 frame
changes a few percent of the pixels, and the camera motion of the step changes all of them. This metric
looks only where the switch happens and compares it with what the camera motion alone does there.
Use a fine dolly (about 0.1% distance per step, a slow walk at 60 fps): at 1% per step the motion of
high-frequency real-scan texture is as large as the switch itself.

Per step i (frames i-1 -> i):
  1. Event. The weight of a node is its draw fade (1 for a plain record, 0 when not drawn). Every node
     whose weight changed is grouped under its highest changed ancestor (the transition's parent). Only
     LOD transitions count: the group must hold a node whose weight moved opposite to the parent's (a
     parent -> children switch, a children -> parent merge, or one step of a cross-fade). Groups that
     only enter or leave the frustum are frustum events, not pops, and are skipped.
     The event's size is the largest weight change in the group (1.0 for a hard switch).
  2. Footprint. The event parent's world AABB (nodes.json) projected with the step's camera, as a
     clipped screen rectangle at the shot's resolution (skipped when it covers over half the frame).
  3. Change. The shift of the footprint's mean colour between frames i-1 and i: the mean over RGB of
     |mean_F(I[i]) - mean_F(I[i-1])| in 8-bit levels. Texture sliding under the camera keeps the local
     mean; a switch of representation (coverage, colour, density) moves it.
  4. Baseline. The median of the same rectangle's change over the BASELINE_RADIUS frame pairs on
     each side of step i (i excluded): what the camera motion does there. A median, so a single
     other switch in the window does not move it; no requirement that the neighbours be free of
     events, because a cross-fade changes every frame of its window by design and would otherwise
     leave its events unscored. An event without a full window (the ends of the run) is unscored
     (reported, never counted as a pass).
  5. Score. excess = change - baseline, ratio = change / max(baseline, RATIO_FLOOR). An event is a
     pop when excess >= POP_EXCESS and ratio >= POP_RATIO and the footprint covers at least
     MIN_FOOTPRINT_PX pixels. The thresholds are fixed constants, not tuned per run.
  6. Control. The same rule applied to the frame pair BASELINE_RADIUS + 1 steps before the event against
     its own window, in the same rectangle: the rate at which ordinary frames are flagged. A run whose
     events are not flagged far more often than its controls says nothing about pops.

Usage: hlod_pop_metric.py --run DIR --nodes nodes.json [--out report.json] [--top N]
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

POP_EXCESS = 0.5  # footprint mean-colour shift (8-bit levels) above the camera-motion baseline
POP_RATIO = 3.0  # change / baseline
RATIO_FLOOR = 0.05  # a near-static baseline does not make every tiny change a pop
MIN_FOOTPRINT_PX = 256  # at the shot's resolution
BASELINE_RADIUS = 4  # frame pairs on each side of the event
WEIGHT_EPS = 1e-6


def load_nodes(path):
    """id -> (parent, height, aabb_min, aabb_size) from the realscan node export."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    cols = {name: i for i, name in enumerate(data["columns"])}
    nodes = {}
    for r in data["nodes"]:
        nodes[int(r[cols["id"]])] = (
            int(r[cols["parent"]]), int(r[cols["height"]]),
            (float(r[cols["min_x"]]), float(r[cols["min_y"]]), float(r[cols["min_z"]])),
            (float(r[cols["size_x"]]), float(r[cols["size_y"]]), float(r[cols["size_z"]])))
    return nodes


def weights(row):
    ids = row.get("drawn_node_ids")
    if ids is None:
        return None
    fades = row.get("drawn_node_fades")
    if fades is None or len(fades) != len(ids):
        return {int(n): 1.0 for n in ids}
    return {int(n): float(f) for n, f in zip(ids, fades)}


def ancestors(nodes, n):
    out = []
    p = nodes[n][0] if n in nodes else -1
    while p >= 0:
        out.append(p)
        p = nodes[p][0] if p in nodes else -1
    return out


def events_between(nodes, w_prev, w_cur):
    """Group the nodes whose weight changed under their highest changed ancestor; keep LOD transitions."""
    changed = {}
    for n in set(w_prev) | set(w_cur):
        dw = w_cur.get(n, 0.0) - w_prev.get(n, 0.0)
        if abs(dw) > WEIGHT_EPS:
            changed[n] = dw
    groups = {}
    for n, dw in changed.items():
        root = n
        for a in ancestors(nodes, n):
            if a in changed:
                root = a
        g = groups.setdefault(root, {"parent": root, "members": 0, "max_dw": 0.0, "opposite": False})
        g["members"] += 1
        g["max_dw"] = max(g["max_dw"], abs(dw))
        if n != root and dw * changed[root] < 0:
            g["opposite"] = True
    return [g for g in groups.values() if g["opposite"]], set(changed)


def project_rect(aabb_min, aabb_size, cam_pos, look_at, fov_deg, width, height, near):
    """Clipped screen rectangle [x0, y0, x1, y1) of a world AABB; None when off screen. A box that reaches
    behind the near plane covers the whole screen (its projection is unbounded). Camera: Godot
    looking_at with +Y up and a vertical field of view."""
    fx, fy, fz = (look_at[k] - cam_pos[k] for k in range(3))
    fl = math.sqrt(fx * fx + fy * fy + fz * fz)
    f = (fx / fl, fy / fl, fz / fl)
    up = (0.0, 1.0, 0.0)
    r = (f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0])
    rl = math.sqrt(sum(c * c for c in r))
    r = tuple(c / rl for c in r)
    u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
    ty = math.tan(math.radians(fov_deg) * 0.5)
    tx = ty * width / height
    xs, ys = [], []
    for cx in (0, 1):
        for cy in (0, 1):
            for cz in (0, 1):
                p = (aabb_min[0] + cx * aabb_size[0], aabb_min[1] + cy * aabb_size[1], aabb_min[2] + cz * aabb_size[2])
                d = tuple(p[k] - cam_pos[k] for k in range(3))
                z = sum(d[k] * f[k] for k in range(3))
                if z <= near:
                    return (0, 0, width, height)
                xs.append((sum(d[k] * r[k] for k in range(3)) / z / tx + 1.0) * 0.5 * width)
                ys.append((1.0 - sum(d[k] * u[k] for k in range(3)) / z / ty) * 0.5 * height)
    x0, x1 = max(0, int(math.floor(min(xs)))), min(width, int(math.ceil(max(xs))))
    y0, y1 = max(0, int(math.floor(min(ys)))), min(height, int(math.ceil(max(ys))))
    if x1 <= x0 or y1 <= y0:
        return None
    return (x0, y0, x1, y1)


def region_change(img_a, img_b, rect):
    """Mean over RGB of the absolute shift of the rectangle's mean colour (8-bit levels)."""
    x0, y0, x1, y1 = rect
    a = img_a[y0:y1, x0:x1, :3].reshape(-1, 3).mean(axis=0)
    b = img_b[y0:y1, x0:x1, :3].reshape(-1, 3).mean(axis=0)
    return float(abs(a - b).mean())


def is_pop(footprint_px, change, baseline):
    excess = change - baseline
    ratio = change / max(baseline, RATIO_FLOOR)
    return footprint_px >= MIN_FOOTPRINT_PX and excess >= POP_EXCESS and ratio >= POP_RATIO, excess, ratio


def median(values):
    v = sorted(values)
    return v[len(v) // 2] if len(v) % 2 else 0.5 * (v[len(v) // 2 - 1] + v[len(v) // 2])


def score(run_dir, nodes, image_loader=None):
    run_dir = Path(run_dir)
    dolly = json.loads((run_dir / "dolly.json").read_text(encoding="utf-8"))
    meta = json.loads((run_dir / "meta.json").read_text(encoding="utf-8"))
    rows = dolly["rows"]
    fov = float(meta.get("camera", {}).get("fov_deg", 75.0))
    near = float(meta.get("camera", {}).get("near_m", 0.05))
    if image_loader is None:
        import numpy as np
        from PIL import Image

        def image_loader(path):
            return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)
    cache = {}

    def img(i):
        if i not in cache:
            if len(cache) > 8:
                cache.pop(min(cache))
            cache[i] = image_loader(run_dir / rows[i]["shot"])
        return cache[i]

    ws = [weights(r) for r in rows]
    events_per_step = [[]] * len(rows)
    for i in range(1, len(rows)):
        if ws[i] is not None and ws[i - 1] is not None:
            events_per_step[i] = events_between(nodes, ws[i - 1], ws[i])[0]
    out = []
    for i in range(1, len(rows)):
        for ev in events_per_step[i]:
            root = ev["parent"]
            if root not in nodes:
                continue
            h, w_px = img(i).shape[:2]
            rect = project_rect(nodes[root][2], nodes[root][3], rows[i]["cam_pos"], rows[i]["look_at"], fov, w_px, h, near)
            rec = {"step": i, "parent": root, "level": nodes[root][1], "members": ev["members"],
                   "max_dw": round(ev["max_dw"], 4), "rect": rect, "footprint_px": 0,
                   "change": None, "baseline": None, "excess": None, "ratio": None, "pop": False, "scored": False,
                   "control_pop": None}
            if rect is not None and (rect[2] - rect[0]) * (rect[3] - rect[1]) * 2 > w_px * h:
                rect = None  # Whole-frame footprint: the camera-inside case, where this metric degenerates.
                rec["rect"] = None
            if rect is None:
                out.append(rec)
                continue
            rec["footprint_px"] = (rect[2] - rect[0]) * (rect[3] - rect[1])
            rec["change"] = round(region_change(img(i - 1), img(i), rect), 3)

            def window(k):
                if k - BASELINE_RADIUS < 1 or k + BASELINE_RADIUS >= len(rows):
                    return None
                return [region_change(img(j - 1), img(j), rect)
                        for j in range(k - BASELINE_RADIUS, k + BASELINE_RADIUS + 1) if j != k]
            base = window(i)
            if base is not None:
                b = median(base)
                pop, excess, ratio = is_pop(rec["footprint_px"], rec["change"], b)
                rec.update(baseline=round(b, 3), excess=round(excess, 3), ratio=round(ratio, 3), scored=True, pop=pop)
                k = i - BASELINE_RADIUS - 1
                control = window(k)
                if control is not None:
                    rec["control_pop"] = is_pop(rec["footprint_px"], region_change(img(k - 1), img(k), rect), median(control))[0]
            out.append(rec)
    return out, rows


def summarize(events, rows):
    scored = [e for e in events if e["scored"] and e["footprint_px"] >= MIN_FOOTPRINT_PX]
    excess = sorted(e["excess"] for e in scored)
    controls = [e for e in events if e["control_pop"] is not None]

    def pct(v, q):
        return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))] if v else None
    return {
        "steps": len(rows),
        "lod_events": len(events),
        "hard_switch_events": sum(1 for e in events if e["max_dw"] >= 0.999),
        "scored_events": len(scored),
        "unscored_events": sum(1 for e in events if not e["scored"]),
        "pops": sum(1 for e in events if e["pop"]),
        "pop_steps": len({e["step"] for e in events if e["pop"]}),
        "pop_rate": round(sum(1 for e in scored if e["pop"]) / len(scored), 4) if scored else None,
        "controls": len(controls),
        "control_pops": sum(1 for e in controls if e["control_pop"]),
        "control_pop_rate": round(sum(1 for e in controls if e["control_pop"]) / len(controls), 4) if controls else None,
        "excess_p50": pct(excess, 0.5), "excess_p95": pct(excess, 0.95), "excess_max": excess[-1] if excess else None,
        "rule": {"POP_EXCESS": POP_EXCESS, "POP_RATIO": POP_RATIO, "RATIO_FLOOR": RATIO_FLOOR,
                 "MIN_FOOTPRINT_PX": MIN_FOOTPRINT_PX},
        "fade_pairs_max": max((r.get("hlod_fade_pairs") or 0) for r in rows) if rows else 0,
        "budget_bound_steps": sum(1 for r in rows if r.get("hlod_cut_budget_bound")),
        "unsettled_steps": sum(1 for r in rows if r.get("settle") and not r["settle"].get("settled")),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", required=True)
    ap.add_argument("--nodes", required=True)
    ap.add_argument("--out")
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args()
    nodes = load_nodes(a.nodes)
    events, rows = score(a.run, nodes)
    summary = summarize(events, rows)
    report = {"summary": summary, "events": events}
    Path(a.out or Path(a.run) / "pop_report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(summary, indent=1))
    top = sorted((e for e in events if e["scored"]), key=lambda e: -e["excess"])[:a.top]
    for e in top:
        print(f"step {e['step']:4d} parent {e['parent']:5d} L{e['level']} dw {e['max_dw']:.3f} "
              f"px {e['footprint_px']:7d} change {e['change']:.2f} base {e['baseline']:.2f} "
              f"excess {e['excess']:.2f} ratio {e['ratio']:.2f}{'  POP' if e['pop'] else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
