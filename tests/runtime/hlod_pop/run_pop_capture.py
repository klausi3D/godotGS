#!/usr/bin/env python3
"""Run hlod_pop_capture.gd for one plan in a throwaway project (local tool, not a lane).

Usage: run_pop_capture.py --godot-binary B --plan PLAN.json [--timeout S]
The plan's out_dir receives meta.json, dolly.json, static.json, frames/, static/ and godot.log.
Exit 0 only when the engine exited 0 and wrote dolly.json; a timeout or crash is a failure.
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--godot-binary", required=True)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--timeout", type=float, default=5400)
    a = ap.parse_args()
    plan = json.loads(Path(a.plan).read_text(encoding="utf-8"))
    out = Path(plan["out_dir"])
    out.mkdir(parents=True, exist_ok=True)
    w, h = plan.get("resolution", [1920, 1080])
    with tempfile.TemporaryDirectory(prefix="gs_hlod_pop_") as proj:
        Path(proj, "project.godot").write_text('[application]\nconfig/name="hlod_pop"\n', encoding="utf-8")
        cmd = [a.godot_binary, "--path", proj, "--rendering-driver", "vulkan", "--resolution", f"{w}x{h}",
               "--windowed", "--script", (HERE / "hlod_pop_capture.gd").as_posix(), "--", "--plan",
               str(Path(a.plan).resolve())]
        print("run_pop_capture:", " ".join(cmd), flush=True)
        t0 = time.monotonic()
        with (out / "godot.log").open("w", encoding="utf-8", errors="replace") as log:
            try:
                rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=a.timeout).returncode
            except subprocess.TimeoutExpired:
                rc = "timeout"
    ok = rc == 0 and (out / "dolly.json").is_file()
    print(f"run_pop_capture: rc={rc} dolly_json={(out / 'dolly.json').is_file()} wall={time.monotonic() - t0:.1f}s", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
