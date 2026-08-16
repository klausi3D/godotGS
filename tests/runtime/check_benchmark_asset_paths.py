#!/usr/bin/env python3
"""Guard benchmark indirection and runtime PLY-reference floor coverage."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROJECT_ROOT = ROOT / "tests" / "examples" / "godot" / "test_project"
RUNTIME_ROOT = ROOT / "tests" / "runtime"

if str(RUNTIME_ROOT) not in sys.path:
    sys.path.insert(0, str(RUNTIME_ROOT))

from prepare_synthetic_assets import ASSET_MIN_SPLAT_COUNTS

SCAN_ROOTS = (
    PROJECT_ROOT / "scenes",
    PROJECT_ROOT / "scripts",
)

SCAN_SUFFIXES = {".gd", ".tscn"}
TARGET_NAME_TOKENS = ("benchmark", "synthetic")
HARDCODED_PLY_RE = re.compile(r"res://tests/fixtures/[A-Za-z0-9_\-]+\.ply")


def _iter_candidate_files() -> list[Path]:
    out: list[Path] = []
    for root in SCAN_ROOTS:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix not in SCAN_SUFFIXES:
                continue
            lowered = str(path.relative_to(PROJECT_ROOT)).replace("\\", "/").lower()
            if not any(token in lowered for token in TARGET_NAME_TOKENS):
                continue
            out.append(path)
    return sorted(out)


def _iter_runtime_candidate_files() -> list[Path]:
    """Derive every runtime GDScript consumer; do not hand-maintain a list."""
    if not RUNTIME_ROOT.is_dir():
        return []
    return sorted(path for path in RUNTIME_ROOT.rglob("*.gd") if path.is_file())


def _runtime_reference_violations(
    path: Path,
    declared_floors: dict[str, int],
) -> list[str]:
    """Reject runtime fixture references that have no positive consumer floor."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        return [f"{path}: could not read file ({exc})"]

    violations: list[str] = []
    for idx, line in enumerate(lines, start=1):
        for match in HARDCODED_PLY_RE.finditer(line):
            asset_path = match.group(0)
            if int(declared_floors.get(asset_path, 0)) <= 0:
                violations.append(
                    f"{path}:{idx}: runtime fixture '{asset_path}' has no "
                    "ASSET_MIN_SPLAT_COUNTS floor"
                )
    return violations


def main() -> int:
    violations: list[str] = []
    for path in _iter_candidate_files():
        rel_path = str(path.relative_to(ROOT)).replace("\\", "/")
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except OSError as exc:
            violations.append(f"{rel_path}: could not read file ({exc})")
            continue
        for idx, line in enumerate(lines, start=1):
            match = HARDCODED_PLY_RE.search(line)
            if match:
                violations.append(f"{rel_path}:{idx}: hardcoded asset path '{match.group(0)}'")

    runtime_references = 0
    for path in _iter_runtime_candidate_files():
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as exc:
            violations.append(f"{path}: could not read file ({exc})")
            continue
        runtime_references += len(HARDCODED_PLY_RE.findall(text))
        violations.extend(_runtime_reference_violations(path, ASSET_MIN_SPLAT_COUNTS))

    if runtime_references <= 0:
        violations.append(
            "tests/runtime: runtime PLY-reference scan found no consumers; "
            "the floor coverage guard is inert"
        )

    if violations:
        print("[benchmark-asset-guard] benchmark/runtime asset reference policy failed")
        for violation in violations:
            print(f"  - {violation}")
        print("[benchmark-asset-guard] use benchmark_asset_manifest.json + benchmark_scene_contract.gd resolution")
        return 1

    print(
        "[benchmark-asset-guard] passed "
        f"(runtime_fixture_references={runtime_references})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
