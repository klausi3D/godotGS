#!/usr/bin/env python3
"""Generate a beyond-VRAM test world (BV-05; docs/architecture/adr-beyond-vram-worlds.md §6).

Entry point of the dispatch lane's ``generate`` step (BV-06a,
tests/runtime/flythrough/dispatch_lane.py)::

    python3 tests/runtime/flythrough/generate_world.py --godot-binary B \\
        --tier {T0,A,A-mini,T-smoke} --encoding plain_v3_uncompressed --out DIR

writes ``DIR/world.gsplatworld`` and its sidecar ``DIR/world.manifest.json``. The
generation itself is the C++ generator (GaussianSplatWorldGenerator, driven by
generate_world.gd in this directory) running headless in the given binary; this
wrapper only supplies a throwaway project, checks the result and, unless
``--no-verify``, re-checks every digest with the independent stdlib verifier
(verify_world_manifest.py). Standard library only.

Exit status: 0 the world and a verified manifest exist; 1 generation or
verification failed (nothing is left at DIR/world.manifest.json); 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GD_SCRIPT = HERE / "generate_world.gd"
WORLD_NAME = "world.gsplatworld"
MANIFEST_NAME = "world.manifest.json"
ENCODINGS = ("plain_v3_uncompressed",)
TIERS = ("T0", "A", "A-mini", "T-smoke")

sys.path.insert(0, str(HERE))
import verify_world_manifest  # noqa: E402


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--godot-binary", required=True, type=Path)
    ap.add_argument("--tier", required=True, choices=TIERS)
    ap.add_argument("--encoding", default=ENCODINGS[0], choices=ENCODINGS)
    ap.add_argument("--out", required=True, type=Path, help="output directory")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--overrides", type=Path, help="JSON object of parameter overrides")
    ap.add_argument("--select", type=Path, help="JSON array of chunk ids (T-ref)")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args(argv)

    if not args.godot_binary.is_file():
        print(f"generate_world: no Godot binary at {args.godot_binary}", file=sys.stderr)
        return 2
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    world = out / WORLD_NAME
    manifest = out / MANIFEST_NAME
    report = out / "generate_report.json"
    if manifest.exists():
        manifest.unlink()

    with tempfile.TemporaryDirectory(prefix="gs_worldgen_project_") as proj:
        Path(proj, "project.godot").write_text('[application]\nconfig/name="worldgen"\n', encoding="utf-8")
        cmd = [
            str(args.godot_binary), "--headless", "--path", proj, "--script", str(GD_SCRIPT), "--",
            "--tier", args.tier, "--out", world.as_posix(), "--report", report.as_posix(),
        ]
        if args.seed is not None:
            cmd += ["--seed", str(args.seed)]
        if args.overrides:
            cmd += ["--overrides", str(args.overrides.resolve())]
        if args.select:
            cmd += ["--select", str(args.select.resolve())]
        print("generate_world: " + " ".join(cmd), flush=True)
        rc = subprocess.run(cmd, check=False).returncode

    if rc != 0:
        print(f"generate_world: the generator exited {rc}", file=sys.stderr)
        return 1
    try:
        rep = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"generate_world: exit 0 without a readable report: {exc}", file=sys.stderr)
        return 1
    if not rep.get("ok") or not world.is_file() or not manifest.is_file():
        print(f"generate_world: generation did not complete: {rep.get('message')}", file=sys.stderr)
        if manifest.exists():
            manifest.unlink()
        return 1
    if not args.no_verify:
        result = verify_world_manifest.verify(world)
        print(json.dumps(result, indent=2))
        if not result["ok"]:
            manifest.unlink()
            return 1
    print(f"generate_world: {world} ({rep['splat_count']} splats, {rep['chunk_count']} chunks, "
          f"{rep['file_bytes']} bytes, generator_params_sha {rep['generator_params_sha']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
