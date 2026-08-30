#!/usr/bin/env python3
"""Generate deterministic synthetic fixtures used by benchmark/runtime flows.

Policy:
- Only canonical generated fixture paths are allowed.
- Legacy hand-managed fixture assets are forbidden.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

RUNTIME_DIR = Path(__file__).resolve().parent
if str(RUNTIME_DIR) not in sys.path:
    sys.path.insert(0, str(RUNTIME_DIR))

from open_world_chunked_asset_ladder import (
    MAIN_PROJECT_FIXTURE_ROOT,
    build_chunked_asset_reference,
    build_chunked_asset_ladder,
    validate_chunked_asset_ladder,
    write_stage_manifests,
)

SH_C0 = 0.28209479177387814


@dataclass(frozen=True)
class PLYSpec:
    relative_path: str
    count: int
    seed: int
    pattern: str
    scale: float

# The C++ [GeneratePLY] test case is the only producer of the rich fixtures
# (fBm noise, SH coefficients, anisotropy).  Which files it writes, and how many
# splats each carries, is DERIVED from the generator source rather than restated
# here: a hand-maintained copy of that list is the exact shape of invariant this
# repository has already watched drift (see tests/AGENTS.md, "Derive coverage
# lists").  Parsing fails closed - an unparseable or moved header raises at
# import instead of silently yielding an empty set that would make
# _generate_via_godot's completeness check vacuous.
CPP_GENERATOR_HEADER: Path = (
    RUNTIME_DIR.parents[1]
    / "modules"
    / "gaussian_splatting"
    / "tests"
    / "generate_synthetic_ply_fixtures.h"
)

# One alternation so the two tokens are read in source order: each generator
# block sets `cfg.splat_count = N;` and then names its output with
# `path_join("<name>.ply")`.  `CHECK(splats.size() == cfg.splat_count);` does not
# match (no `= <digits>;`), and the `path_join("..")` calls that build the output
# directory do not match either (no `.ply` suffix).
_CPP_GENERATOR_TOKEN_RE = re.compile(
    r'cfg\.splat_count\s*=\s*(\d+)\s*;|path_join\("([^"]+\.ply)"\)'
)


def parse_cpp_generator_counts(header_path: Path | None = None) -> dict[str, int]:
    """Return {fixture filename: splat count} as declared by the C++ generators."""
    path = header_path if header_path is not None else CPP_GENERATOR_HEADER
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(
            f"[prepare_synthetic_assets] cannot read the C++ fixture generator at {path}: {exc}. "
            "The rich-fixture splat counts are derived from it; refusing to continue with an "
            "unknown contract."
        ) from exc

    counts: dict[str, int] = {}
    pending: int | None = None
    for token in _CPP_GENERATOR_TOKEN_RE.finditer(text):
        raw_count, filename = token.group(1), token.group(2)
        if raw_count is not None:
            pending = int(raw_count)
            continue
        if pending is None:
            raise RuntimeError(
                f"[prepare_synthetic_assets] {path.name} writes '{filename}' before declaring a "
                "cfg.splat_count; the generator layout changed and the derived counts would be wrong."
            )
        previous = counts.get(filename)
        if previous is not None and previous != pending:
            raise RuntimeError(
                f"[prepare_synthetic_assets] {path.name} declares two different splat counts for "
                f"'{filename}' ({previous} and {pending})."
            )
        counts[filename] = pending
        pending = None

    if not counts:
        raise RuntimeError(
            f"[prepare_synthetic_assets] found no fixture generators in {path}. Either the file "
            "moved or its shape changed; the derived rich-fixture contract would be empty."
        )
    return counts


# {filename: splat count} written by the C++ [GeneratePLY] generators.
CPP_GENERATOR_SPLAT_COUNTS: dict[str, int] = parse_cpp_generator_counts()

# Files the C++ generators produce.  When --godot-binary is given these come from
# the engine binary; otherwise the Python fallback generators below create
# lightweight versions of them.
CPP_GENERATED_FILENAMES: frozenset[str] = frozenset(CPP_GENERATOR_SPLAT_COUNTS)

CANONICAL_SPECS: tuple[PLYSpec, ...] = (
    PLYSpec("tests/fixtures/test_splats.ply", 1024, 1101, "sphere", 3.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/test_splats.ply", 1024, 1101, "sphere", 3.0),
    PLYSpec("templates/gaussian_splat_template/assets/template_splats.ply", 768, 2202, "sphere", 2.4),
    PLYSpec("tests/fixtures/synthetic_sphere.ply", 2048, 3101, "sphere", 4.5),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_sphere.ply", 2048, 3101, "sphere", 4.5),
    PLYSpec("tests/fixtures/synthetic_cube.ply", 2048, 3201, "cube", 7.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_cube.ply", 2048, 3201, "cube", 7.0),
    PLYSpec("tests/fixtures/synthetic_plane.ply", 2048, 3301, "plane", 9.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_plane.ply", 2048, 3301, "plane", 9.0),
    PLYSpec("tests/fixtures/synthetic_torus.ply", 3072, 3401, "torus", 6.5),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_torus.ply", 3072, 3401, "torus", 6.5),
    PLYSpec("tests/fixtures/synthetic_spiral.ply", 25000, 3501, "spiral", 8.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_spiral.ply", 25000, 3501, "spiral", 8.0),
    PLYSpec("tests/fixtures/synthetic_mandelbulb.ply", 4096, 3601, "mandelbulb", 1.4),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_mandelbulb.ply", 4096, 3601, "mandelbulb", 1.4),
    PLYSpec("tests/fixtures/synthetic_cloud.ply", 4096, 3701, "cloud", 12.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_cloud.ply", 4096, 3701, "cloud", 12.0),
    PLYSpec("tests/fixtures/synthetic_flower_field.ply", 30000, 3801, "flower_field", 10.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/synthetic_flower_field.ply", 30000, 3801, "flower_field", 10.0),
)

FORBIDDEN_LEGACY_PLYS: tuple[str, ...] = (
    "tests/examples/godot/test_project/cabin.ply",
    "tests/examples/godot/test_project/ancient-corinth-clean.ply",
    "tests/examples/godot/test_project/splat_12000.ply",
    "tests/examples/godot/test_project/scenes/5x5%23-5_-10_0_-5%23-1_-2.ply",
    "tests/examples/godot/test_project/test_splats.ply",
)

FORBIDDEN_LEGACY_ASSET_DIRS: tuple[str, ...] = (
    "tests/examples/godot/test_project/benchmark_assets_generated",
)

FORBIDDEN_LEGACY_PATH_TOKENS: tuple[str, ...] = (
    "benchmark_assets_generated/",
)

CANONICAL_MANIFESTS: tuple[str, ...] = (
    "tests/fixtures/benchmark_asset_manifest.json",
    "tests/examples/godot/test_project/tests/fixtures/benchmark_asset_manifest.json",
)

DECK_MANIFEST_PATH = "tests/examples/godot/test_project_deck/tests/fixtures/benchmark_asset_manifest.json"

SCENE_DEFAULT_ASSETS: dict[str, str] = {
    "benchmark_suite_lane": "res://tests/fixtures/test_splats.ply",
    "benchmark_unified": "res://tests/fixtures/test_splats.ply",
    "benchmark_small_baseline": "res://tests/fixtures/test_splats.ply",
    "synthetic_sphere": "res://tests/fixtures/synthetic_sphere.ply",
    "synthetic_cube": "res://tests/fixtures/synthetic_cube.ply",
    "synthetic_plane": "res://tests/fixtures/synthetic_plane.ply",
    "synthetic_torus": "res://tests/fixtures/synthetic_torus.ply",
    "synthetic_spiral": "res://tests/fixtures/synthetic_spiral.ply",
    "synthetic_mandelbulb": "res://tests/fixtures/synthetic_mandelbulb.ply",
    "synthetic_cloud": "res://tests/fixtures/synthetic_cloud.ply",
    "synthetic_flower_field": "res://tests/fixtures/synthetic_flower_field.ply",
}

LANE_DEFAULT_ASSETS: dict[str, str] = {
    "static_baseline": "res://tests/fixtures/test_splats.ply",
    "streaming_corridor": "res://tests/fixtures/test_splats.ply",
    "open_world_corridor_proof": build_chunked_asset_reference("open_world_corridor_20m"),
    "city_flyover": "res://tests/fixtures/test_splats.ply",
    "instance_storm": "res://tests/fixtures/test_splats.ply",
    "lighting_stress": "res://tests/fixtures/test_splats.ply",
    "animation_arena": "res://tests/fixtures/test_splats.ply",
    "lod_torture": "res://tests/fixtures/test_splats.ply",
    "integrity_sentinel": "res://tests/fixtures/test_splats.ply",
    "parity_fidelity": "res://tests/fixtures/test_splats.ply",
    "long_soak": "res://tests/fixtures/test_splats.ply",
    "unified_composite": "res://tests/fixtures/test_splats.ply",
    "dense_resident_2m": "res://tests/fixtures/synthetic_spiral.ply",
    "sort32_coplanar_alpha": "res://tests/fixtures/test_splats.ply",
    "sort32_near_camera_large": "res://tests/fixtures/test_splats.ply",
    "sort32_depth_range": "res://tests/fixtures/test_splats.ply",
    "sort32_tile_bit_pressure": "res://tests/fixtures/test_splats.ply",
    "small_baseline": "res://tests/fixtures/test_splats.ply",
    "instance_pipeline_ab": "res://tests/fixtures/test_splats.ply",
    "instance_pipeline_ab_serial": "res://tests/fixtures/test_splats.ply",
    "instance_pipeline_ab_single_pass": "res://tests/fixtures/test_splats.ply",
    "synthetic_sphere": "res://tests/fixtures/synthetic_sphere.ply",
    "synthetic_cube": "res://tests/fixtures/synthetic_cube.ply",
    "synthetic_plane": "res://tests/fixtures/synthetic_plane.ply",
    "synthetic_torus": "res://tests/fixtures/synthetic_torus.ply",
    "synthetic_spiral": "res://tests/fixtures/synthetic_spiral.ply",
    "synthetic_mandelbulb": "res://tests/fixtures/synthetic_mandelbulb.ply",
    "synthetic_cloud": "res://tests/fixtures/synthetic_cloud.ply",
    "synthetic_flower_field": "res://tests/fixtures/synthetic_flower_field.ply",
}

LANE_METADATA: dict[str, dict[str, object]] = {
    "static_baseline": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "published_baseline",
        "notes": "Low-noise baseline lane backed by the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "streaming_corridor": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "proof_support_corridor_churn_smoke",
        "notes": "Streaming-shaped corridor churn support lane currently backed by test_splats.ply; useful for proof-shape smoke coverage, not representative chunked evidence.",
        "require_explicit_lane_default": True,
    },
    "open_world_corridor_proof": {
        "asset_classification": "chunked_open_world_candidate",
        "evidence_role": "proof_corridor_return_bootstrap",
        "notes": "Dedicated world-consuming corridor proof lane. Consumes the canonical open-world corridor stage contract to build an approximately 20M-total benchmark-local GaussianSplatWorld using the deterministic spiral fixture; honest large-world candidate evidence, but not yet promoted real_chunked proof.",
        "require_explicit_lane_default": True,
        "resource_kind": "gaussian_world_contract",
    },
    "city_flyover": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "proof_support_boundary_crossing_smoke",
        "notes": "High-altitude boundary-crossing support lane currently backed by test_splats.ply; useful for proof-shape smoke coverage, not representative chunked evidence.",
        "require_explicit_lane_default": True,
    },
    "instance_storm": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Submission-pressure lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "lighting_stress": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Lighting stress lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "animation_arena": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Animation lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "lod_torture": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "LOD churn lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "integrity_sentinel": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Deterministic artifact-detection lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "parity_fidelity": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Parity-fidelity lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "long_soak": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "proof_support_city_roam_soak_smoke",
        "notes": "Long-horizon city-roam soak support lane currently backed by test_splats.ply; useful for proof-shape smoke coverage, not representative chunked evidence.",
        "require_explicit_lane_default": True,
    },
    "unified_composite": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "proof_support_integrated_composite_smoke",
        "notes": "Integrated composite support lane currently backed by test_splats.ply; useful for combined-system smoke coverage, not representative chunked evidence.",
        "require_explicit_lane_default": True,
    },
    "dense_resident_2m": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Resident-pipeline 2M-visible-splat lane (81 x synthetic_spiral) for sort/raster timing under load.",
        "require_explicit_lane_default": True,
    },
    "sort32_coplanar_alpha": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "opt_in_sort32_visual_stress",
        "notes": "Opt-in resident/instance lane that forces a 32-bit tile/depth overlap sort key over coplanar, high-overlap alpha content.",
        "require_explicit_lane_default": True,
    },
    "sort32_near_camera_large": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "opt_in_sort32_visual_stress",
        "notes": "Opt-in resident/instance lane that keeps 32-bit tile/depth overlap keys while placing large splats close to the camera.",
        "require_explicit_lane_default": True,
    },
    "sort32_depth_range": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "opt_in_sort32_visual_stress",
        "notes": "Opt-in resident/instance lane that keeps 32-bit tile/depth overlap keys across a wide camera near/far range.",
        "require_explicit_lane_default": True,
    },
    "sort32_tile_bit_pressure": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "opt_in_sort32_visual_stress",
        "notes": "Opt-in resident/instance lane that deliberately undersizes 32-bit tile bits to record whether the active route can keep 32-bit keys or must fall back.",
        "require_explicit_lane_default": True,
    },
    "small_baseline": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Small baseline lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "instance_pipeline_ab": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Shared A/B benchmark scene using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "instance_pipeline_ab_serial": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Serial instance-pipeline A/B lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "instance_pipeline_ab_single_pass": {
        "asset_classification": "lightweight_smoke",
        "evidence_role": "suite_support",
        "notes": "Single-pass instance-pipeline A/B lane using the lightweight canonical smoke asset.",
        "require_explicit_lane_default": True,
    },
    "synthetic_sphere": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_cube": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_plane": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_torus": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_spiral": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_mandelbulb": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_cloud": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
    "synthetic_flower_field": {
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "synthetic_support",
        "notes": "Deterministic synthetic geometry support lane.",
        "require_explicit_lane_default": True,
    },
}


# Minimum splat counts a benchmark lane requires from each fixture (issue #669).
#
# These are CONTRACT floors, not a description of whatever the last generator run
# happened to write. A lane that loads a fixture below its floor fails instead of
# publishing a number produced by a different workload.
#
# The floors are NOT uniformly sourced, deliberately:
#
#   * test_splats.ply is gitignored and never committed, and the only producer that
#     yields the published benchmark workload is the C++ [GeneratePLY] generator in
#     modules/gaussian_splatting/tests/generate_synthetic_ply_fixtures.h (10000
#     splats). The lightweight Python fallback below writes 1024 — a 10x-smaller
#     workload — so the floor is set to the C++ count and the fallback fixture fails
#     loudly rather than silently standing in for it.
#
#   * The synthetic_*.ply fixtures ARE committed, at the Python-fallback sizes. Their
#     floors match those committed sizes, so a clean checkout passes. (The C++
#     generator writes far larger versions of these — 50000-100000 — so sourcing
#     these floors from C++ would fail every clean checkout.)
ASSET_MIN_SPLAT_COUNTS: dict[str, int] = {
    # Floor from the C++ generator; the committed tree has no copy of this file.
    "res://tests/fixtures/test_splats.ply": 10000,
    # Floors from the committed fixture sizes (see CANONICAL_SPECS below).
    "res://tests/fixtures/synthetic_sphere.ply": 2048,
    "res://tests/fixtures/synthetic_cube.ply": 2048,
    "res://tests/fixtures/synthetic_plane.ply": 2048,
    "res://tests/fixtures/synthetic_torus.ply": 3072,
    "res://tests/fixtures/synthetic_mandelbulb.ply": 4096,
    "res://tests/fixtures/synthetic_cloud.ply": 4096,
    "res://tests/fixtures/synthetic_spiral.ply": 25000,
    "res://tests/fixtures/synthetic_flower_field.ply": 30000,
}

# ---------------------------------------------------------------------------
# Fixture provenance (#790)
#
# A FLOOR cannot answer the question the benchmarks actually need answered.
# The floors above are set to the committed (Python-fallback) sizes so a clean
# checkout passes, which means a 2048-splat sphere and a 50000-splat sphere both
# satisfy `synthetic_sphere.ply` — a 24x workload difference with no signal.
# Raising the floors is not available: it would fail every clean checkout, and
# which workload a published lane is *entitled* to is a maintainer decision, not
# something a floor should decide by accident.
#
# So instead of one floor, each fixture declares the EXACT count each producer
# writes.  Both numbers are derived from their producers — CANONICAL_SPECS for
# the Python fallback, generate_synthetic_ply_fixtures.h for the C++ generators —
# so nothing here is invented and nothing can drift.  That turns "how big is it"
# into "which producer made it", which is the question a benchmark number needs
# stamped on it, and it makes a count matching NEITHER producer (a thinned,
# truncated or hand-edited fixture) detectable without moving any floor.
#
# Note synthetic_spiral.ply and synthetic_flower_field.ply have no C++ generator
# at all: 25000/30000 is their maximum available fidelity, not a reduced variant.
VARIANT_PYTHON_FALLBACK = "python_fallback"
VARIANT_CPP_RICH = "cpp_rich"


def _python_fallback_counts() -> dict[str, int]:
    """Return {fixture filename: splat count} the Python fallback generators write."""
    counts: dict[str, int] = {}
    for spec in CANONICAL_SPECS:
        filename = Path(spec.relative_path).name
        previous = counts.get(filename)
        if previous is not None and previous != spec.count:
            raise RuntimeError(
                "[prepare_synthetic_assets] CANONICAL_SPECS declares two different counts for "
                f"'{filename}' ({previous} and {spec.count}); the primary and project-local copies "
                "of a fixture must be identical or the same res:// path means two workloads."
            )
        counts[filename] = spec.count
    return counts


PYTHON_FALLBACK_SPLAT_COUNTS: dict[str, int] = _python_fallback_counts()


def _expected_splat_counts() -> dict[str, dict[str, int]]:
    """Return {asset res:// path: {variant: exact splat count}} for every declared fixture."""
    out: dict[str, dict[str, int]] = {}
    for asset_path in ASSET_MIN_SPLAT_COUNTS:
        filename = Path(asset_path).name
        variants: dict[str, int] = {}
        if filename in PYTHON_FALLBACK_SPLAT_COUNTS:
            variants[VARIANT_PYTHON_FALLBACK] = PYTHON_FALLBACK_SPLAT_COUNTS[filename]
        if filename in CPP_GENERATOR_SPLAT_COUNTS:
            variants[VARIANT_CPP_RICH] = CPP_GENERATOR_SPLAT_COUNTS[filename]
        if not variants:
            raise RuntimeError(
                f"[prepare_synthetic_assets] {asset_path} declares a splat-count floor but no "
                "generator produces it; a lane would enforce a contract nothing can satisfy."
            )
        out[asset_path] = variants
    return out


ASSET_EXPECTED_SPLAT_COUNTS: dict[str, dict[str, int]] = _expected_splat_counts()


def _validate_floor_provenance() -> None:
    """Every floor must be a count some generator actually writes.

    This is what stops a floor from becoming a number someone chose. Both
    directions are covered: a floor invented above any producer's output would
    fail every run, and a floor invented below the smallest producer's output
    would quietly stop discriminating.
    """
    for asset_path, floor in ASSET_MIN_SPLAT_COUNTS.items():
        variants = ASSET_EXPECTED_SPLAT_COUNTS[asset_path]
        if floor not in set(variants.values()):
            raise RuntimeError(
                f"[prepare_synthetic_assets] floor {floor} for {asset_path} matches no generator "
                f"output (declared: {variants}). Floors must name a producer, not a preference."
            )


_validate_floor_provenance()


def _benchmark_asset_manifest() -> dict[str, object]:
    return {
        "chunked_asset_ladder": build_chunked_asset_ladder(),
        "version": "2.5.0",
        "default_asset": "res://tests/fixtures/test_splats.ply",
        "scene_defaults": dict(SCENE_DEFAULT_ASSETS),
        "lane_defaults": dict(LANE_DEFAULT_ASSETS),
        "lane_metadata": dict(LANE_METADATA),
        "asset_min_splat_counts": dict(ASSET_MIN_SPLAT_COUNTS),
        "asset_expected_splat_counts": {
            asset_path: dict(variants)
            for asset_path, variants in ASSET_EXPECTED_SPLAT_COUNTS.items()
        },
    }


def _header(count: int) -> bytes:
    text = (
        "ply\n"
        "format binary_little_endian 1.0\n"
        f"element vertex {count}\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "property float f_dc_0\n"
        "property float f_dc_1\n"
        "property float f_dc_2\n"
        "property float opacity\n"
        "property float scale_0\n"
        "property float scale_1\n"
        "property float scale_2\n"
        "property float rot_0\n"
        "property float rot_1\n"
        "property float rot_2\n"
        "property float rot_3\n"
        "end_header\n"
    )
    return text.encode("ascii")


def _encode_splat(
    x: float,
    y: float,
    z: float,
    color_rgb: tuple[float, float, float],
    scale: float,
    opacity: float,
) -> tuple[float, ...]:
    opacity = max(0.01, min(0.99, opacity))
    scale = max(0.02, scale)

    opacity_logit = math.log(opacity / (1.0 - opacity))
    scale_log = math.log(scale)
    r, g, b = (channel / SH_C0 for channel in color_rgb)

    return (
        float(x),
        float(y),
        float(z),
        float(r),
        float(g),
        float(b),
        float(opacity_logit),
        float(scale_log),
        float(scale_log),
        float(scale_log),
        1.0,
        0.0,
        0.0,
        0.0,
    )


def _generate_rows(spec: PLYSpec) -> list[tuple[float, ...]]:
    rng = random.Random(spec.seed)
    rows: list[tuple[float, ...]] = []

    palette: tuple[tuple[float, float, float], ...] = (
        (1.0, 0.1, 0.1),
        (0.1, 1.0, 0.1),
        (0.1, 0.1, 1.0),
        (1.0, 0.8, 0.2),
        (0.9, 0.3, 0.9),
    )

    def _sample_sphere(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        theta = rng.random() * (2.0 * math.pi)
        phi = math.acos(2.0 * rng.random() - 1.0)
        radius = spec.scale * pow(rng.random(), 0.33)
        x = radius * math.sin(phi) * math.cos(theta)
        y = radius * math.sin(phi) * math.sin(theta)
        z = -4.0 + radius * math.cos(phi)
        color = palette[index % len(palette)]
        scale = 0.08 + 0.35 * rng.random()
        opacity = 0.55 + 0.4 * rng.random()
        return x, y, z, color, scale, opacity

    def _sample_cube(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        half = spec.scale * 0.5
        x = rng.uniform(-half, half)
        y = rng.uniform(-half, half)
        z = -4.0 + rng.uniform(-half, half)
        color = palette[index % len(palette)]
        scale = 0.09 + 0.24 * rng.random()
        opacity = 0.6 + 0.3 * rng.random()
        return x, y, z, color, scale, opacity

    def _sample_plane(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        x = rng.uniform(-spec.scale, spec.scale)
        y = rng.uniform(-0.2, 0.2)
        z = -4.0 + rng.uniform(-spec.scale, spec.scale)
        u = (x / (spec.scale * 2.0)) + 0.5
        v = (z + 4.0 + spec.scale) / (spec.scale * 2.0)
        color = (
            0.2 + 0.7 * max(0.0, min(1.0, u)),
            0.35 + 0.55 * max(0.0, min(1.0, v)),
            0.45 + 0.4 * rng.random(),
        )
        scale = 0.06 + 0.2 * rng.random()
        opacity = 0.55 + 0.35 * rng.random()
        return x, y, z, color, scale, opacity

    def _sample_torus(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        major = spec.scale
        minor = max(0.8, spec.scale * 0.32)
        u = rng.uniform(0.0, 2.0 * math.pi)
        v = rng.uniform(0.0, 2.0 * math.pi)
        ring = major + minor * math.cos(v)
        x = ring * math.cos(u)
        y = minor * math.sin(v)
        z = -4.0 + ring * math.sin(u)
        color = (
            0.5 + 0.45 * math.sin(u),
            0.45 + 0.4 * math.sin(v + 1.2),
            0.5 + 0.45 * math.cos(u + v),
        )
        scale = 0.05 + 0.18 * rng.random()
        opacity = 0.62 + 0.28 * rng.random()
        return x, y, z, color, scale, opacity

    def _sample_spiral(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        t = float(index) / max(1.0, float(spec.count - 1))
        # Double helix with varying pitch
        arm = index % 2
        angle = t * 20.0 * math.pi + arm * math.pi
        pitch_variation = 0.3 * math.sin(t * 6.0 * math.pi)
        radius = 0.8 + spec.scale * (0.2 + 0.8 * t + 0.1 * math.sin(t * 12.0 * math.pi))
        jitter = 0.15 + 0.25 * t
        x = radius * math.cos(angle) + rng.uniform(-jitter, jitter)
        y = (t - 0.5) * spec.scale * 2.0 + pitch_variation + rng.uniform(-0.08, 0.08)
        z = -4.0 + radius * math.sin(angle) + rng.uniform(-jitter, jitter)
        # Color varies by arm and position
        hue_shift = arm * 2.5
        color = (
            0.3 + 0.6 * max(0.0, min(1.0, math.sin(angle * 0.15 + hue_shift + 0.4))),
            0.3 + 0.6 * max(0.0, min(1.0, math.sin(angle * 0.17 + hue_shift + 2.0))),
            0.3 + 0.6 * max(0.0, min(1.0, math.sin(angle * 0.19 + hue_shift + 4.0))),
        )
        scale = 0.03 + 0.12 * rng.random() * (1.0 - 0.4 * t)
        opacity = 0.55 + 0.4 * rng.random()
        return x, y, z, color, scale, opacity

    def _sample_cloud(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        ex = spec.scale
        ey = spec.scale * 0.3
        ez = spec.scale * 0.7
        while True:
            x = rng.uniform(-ex, ex)
            y = rng.uniform(-ey, ey)
            z = rng.uniform(-ez, ez)
            d = (x / ex) ** 2 + (y / ey) ** 2 + (z / ez) ** 2
            if d <= 1.0:
                break
        z_world = -4.0 + z
        height = max(0.0, min(1.0, (y + ey) / (2.0 * ey)))
        shade = max(0.0, min(1.0, 1.0 - d))
        color = (
            0.65 + 0.3 * height,
            0.68 + 0.28 * height,
            0.75 + 0.2 * height + 0.05 * shade,
        )
        scale = 0.1 + 0.38 * shade
        opacity = 0.22 + 0.55 * shade
        return x, y + 4.0, z_world, color, scale, opacity

    def _sample_flower_field(index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        # More parts per flower: center + inner ring + outer ring + stem + leaves
        parts_per_flower = 12
        flower_idx = index // parts_per_flower
        part_idx = index % parts_per_flower
        rng_flower = random.Random(spec.seed + flower_idx * 17)
        cx = rng_flower.uniform(-spec.scale, spec.scale)
        cz = rng_flower.uniform(-spec.scale, spec.scale) - 4.0
        stem_h = rng_flower.uniform(0.3, 1.6)
        flower_hue = rng_flower.random() * 2.0 * math.pi

        if part_idx == 0:
            # Flower center (pistil)
            x = cx + rng.uniform(-0.03, 0.03)
            y = stem_h + rng.uniform(-0.02, 0.02)
            z = cz + rng.uniform(-0.03, 0.03)
            color = (0.95, 0.85, 0.12)
            scale = 0.06 + 0.06 * rng.random()
            opacity = 0.85 + 0.12 * rng.random()
        elif part_idx < 7:
            # Inner petals (6)
            angle = ((part_idx - 1) / 6.0) * 2.0 * math.pi + rng_flower.uniform(-0.15, 0.15)
            petal_r = rng_flower.uniform(0.18, 0.4)
            x = cx + math.cos(angle) * petal_r
            y = stem_h + rng.uniform(-0.03, 0.04)
            z = cz + math.sin(angle) * petal_r
            color = (
                0.5 + 0.45 * max(0.0, min(1.0, math.sin(angle + flower_hue + 0.2))),
                0.4 + 0.5 * max(0.0, min(1.0, math.sin(angle + flower_hue + 2.3))),
                0.4 + 0.5 * max(0.0, min(1.0, math.sin(angle + flower_hue + 4.4))),
            )
            scale = 0.06 + 0.12 * rng.random()
            opacity = 0.72 + 0.23 * rng.random()
        elif part_idx < 10:
            # Stem segments (3)
            seg = part_idx - 7
            stem_y = stem_h * (seg / 3.0) + rng.uniform(-0.02, 0.02)
            x = cx + rng.uniform(-0.04, 0.04)
            y = stem_y
            z = cz + rng.uniform(-0.04, 0.04)
            color = (0.18 + 0.1 * rng.random(), 0.45 + 0.2 * rng.random(), 0.12 + 0.08 * rng.random())
            scale = 0.03 + 0.04 * rng.random()
            opacity = 0.6 + 0.25 * rng.random()
        else:
            # Leaves (2)
            leaf_h = stem_h * rng.uniform(0.2, 0.5)
            leaf_angle = rng.uniform(0.0, 2.0 * math.pi)
            leaf_r = rng.uniform(0.15, 0.35)
            x = cx + math.cos(leaf_angle) * leaf_r
            y = leaf_h
            z = cz + math.sin(leaf_angle) * leaf_r
            color = (0.15 + 0.12 * rng.random(), 0.5 + 0.3 * rng.random(), 0.1 + 0.1 * rng.random())
            scale = 0.05 + 0.1 * rng.random()
            opacity = 0.65 + 0.25 * rng.random()

        return x, y, z, color, scale, opacity

    def _sample_mandelbulb(_index: int) -> tuple[float, float, float, tuple[float, float, float], float, float]:
        attempts = 0
        while attempts < 60:
            attempts += 1
            x = rng.uniform(-spec.scale, spec.scale)
            y = rng.uniform(-spec.scale, spec.scale)
            z = rng.uniform(-spec.scale, spec.scale)
            zx, zy, zz = x, y, z
            escaped = False
            for _ in range(8):
                r = math.sqrt(zx * zx + zy * zy + zz * zz)
                if r > 2.0:
                    escaped = True
                    break
                if r < 1e-6:
                    theta = 0.0
                    phi = 0.0
                else:
                    theta = math.acos(zz / r)
                    phi = math.atan2(zy, zx)
                power = 8.0
                rp = r ** power
                theta *= power
                phi *= power
                sin_t = math.sin(theta)
                zx = rp * sin_t * math.cos(phi) + x
                zy = rp * sin_t * math.sin(phi) + y
                zz = rp * math.cos(theta) + z
            if escaped:
                intensity = max(0.0, min(1.0, math.sqrt(x * x + y * y + z * z) / (spec.scale * 1.2)))
                color = (
                    0.45 + 0.5 * math.sin(8.0 * intensity + 0.2),
                    0.45 + 0.5 * math.sin(8.0 * intensity + 2.2),
                    0.45 + 0.5 * math.sin(8.0 * intensity + 4.2),
                )
                scale = 0.06 + 0.16 * (1.0 - intensity)
                opacity = 0.55 + 0.38 * (1.0 - intensity)
                return x * 5.0, y * 5.0, z * 5.0 - 4.0, color, scale, opacity
        return _sample_spiral(rng.randint(0, spec.count - 1))

    samplers = {
        "sphere": _sample_sphere,
        "cube": _sample_cube,
        "plane": _sample_plane,
        "torus": _sample_torus,
        "spiral": _sample_spiral,
        "cloud": _sample_cloud,
        "flower_field": _sample_flower_field,
        "mandelbulb": _sample_mandelbulb,
    }

    if spec.pattern not in samplers:
        raise ValueError(f"Unsupported pattern '{spec.pattern}' for {spec.relative_path}")

    sampler = samplers[spec.pattern]
    for index in range(spec.count):
        x, y, z, color, scale, opacity = sampler(index)
        rows.append(_encode_splat(x, y, z, color, scale, opacity))
    return rows


def _write_ply(path: Path, rows: list[tuple[float, ...]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(_header(len(rows)))
        for row in rows:
            handle.write(struct.pack("<14f", *row))


def _resolve_repo_root(arg_root: str | None) -> Path:
    if arg_root:
        return Path(arg_root).expanduser().resolve()
    return Path(__file__).resolve().parents[2]


def _write_manifest(repo_root: Path) -> None:
    manifest = _benchmark_asset_manifest()
    encoded = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    for rel_path in CANONICAL_MANIFESTS:
        path = repo_root / rel_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(encoded, encoding="utf-8")
    write_stage_manifests(repo_root / MAIN_PROJECT_FIXTURE_ROOT)


def _benchmark_scene_script_paths(repo_root: Path) -> list[str]:
    root = repo_root / "tests" / "examples" / "godot" / "test_project" / "scenes"
    if not root.is_dir():
        return []
    out: list[str] = []
    for file_path in root.rglob("*"):
        if not file_path.is_file():
            continue
        if file_path.suffix not in {".gd", ".tscn"}:
            continue
        out.append(str(file_path.relative_to(repo_root)).replace("\\", "/"))
    return sorted(out)


def _check_only(repo_root: Path) -> int:
    missing: list[str] = []
    forbidden_present: list[str] = []
    legacy_path_references: list[str] = []
    policy_failures: list[str] = []

    ladder_failures = validate_chunked_asset_ladder()
    for failure in ladder_failures:
        policy_failures.append(f"open-world ladder: {failure}")

    for spec in CANONICAL_SPECS:
        file_path = repo_root / spec.relative_path
        if not file_path.is_file() or file_path.stat().st_size <= 0:
            missing.append(spec.relative_path)

    for rel_path in CANONICAL_MANIFESTS:
        file_path = repo_root / rel_path
        if not file_path.is_file() or file_path.stat().st_size <= 0:
            missing.append(rel_path)
            continue
        try:
            data = json.loads(file_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            policy_failures.append(f"{rel_path}: invalid JSON ({exc})")
            continue
        expected_ladder = build_chunked_asset_ladder()
        if data.get("chunked_asset_ladder") != expected_ladder:
            policy_failures.append(f"{rel_path}: chunked_asset_ladder is stale or mismatched")
        raw_lane_defaults = data.get("lane_defaults", {})
        if not isinstance(raw_lane_defaults, dict):
            policy_failures.append(f"{rel_path}: lane_defaults must be a JSON object")
        else:
            chunked_lane_refs = sorted(
                lane_id for lane_id, asset_path in raw_lane_defaults.items()
                if isinstance(asset_path, str) and asset_path.startswith("chunked_ladder:")
            )
            illegal_chunked_refs = [lane_id for lane_id in chunked_lane_refs if lane_id != "open_world_corridor_proof"]
            if illegal_chunked_refs:
                policy_failures.append(
                    f"{rel_path}: only open_world_corridor_proof may use chunked_ladder during bootstrap staging "
                    f"({', '.join(illegal_chunked_refs)})"
                )
            if "open_world_corridor_proof" in chunked_lane_refs:
                lane_metadata = data.get("lane_metadata", {})
                proof_metadata = lane_metadata.get("open_world_corridor_proof", {}) if isinstance(lane_metadata, dict) else {}
                if not isinstance(proof_metadata, dict):
                    policy_failures.append(
                        f"{rel_path}: open_world_corridor_proof lane_metadata must be an object"
                    )
                elif str(proof_metadata.get("resource_kind", "")).strip() != "gaussian_world_contract":
                    policy_failures.append(
                        f"{rel_path}: open_world_corridor_proof must declare resource_kind=gaussian_world_contract"
                    )

    deck_manifest = repo_root / DECK_MANIFEST_PATH
    if not deck_manifest.is_file() or deck_manifest.stat().st_size <= 0:
        missing.append(DECK_MANIFEST_PATH)
    else:
        try:
            deck_data = json.loads(deck_manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            policy_failures.append(f"{DECK_MANIFEST_PATH}: invalid JSON ({exc})")
        else:
            deck_ladder = deck_data.get("chunked_asset_ladder")
            if deck_ladder != {}:
                policy_failures.append(f"{DECK_MANIFEST_PATH}: chunked_asset_ladder must stay empty for deck smoke lanes")

    for asset_id, entry in build_chunked_asset_ladder().items():
        staging = entry.get("staging", {})
        repo_stage_manifest_path = str(staging.get("repo_stage_manifest_path", ""))
        if not repo_stage_manifest_path:
            policy_failures.append(f"open-world ladder: {asset_id} missing repo_stage_manifest_path")
            continue
        stage_manifest_path = repo_root / repo_stage_manifest_path
        if not stage_manifest_path.is_file() or stage_manifest_path.stat().st_size <= 0:
            missing.append(repo_stage_manifest_path)

    for rel_path in FORBIDDEN_LEGACY_PLYS:
        file_path = repo_root / rel_path
        if file_path.is_file() and file_path.stat().st_size > 0:
            forbidden_present.append(rel_path)

    for rel_dir in FORBIDDEN_LEGACY_ASSET_DIRS:
        dir_path = repo_root / rel_dir
        if not dir_path.is_dir():
            continue
        for ply_file in sorted(dir_path.rglob("*.ply")):
            forbidden_present.append(str(ply_file.relative_to(repo_root)).replace("\\", "/"))

    for rel_path in _benchmark_scene_script_paths(repo_root):
        file_path = repo_root / rel_path
        try:
            text = file_path.read_text(encoding="utf-8")
        except OSError:
            continue
        for token in FORBIDDEN_LEGACY_PATH_TOKENS:
            if token in text:
                legacy_path_references.append(f"{rel_path}: contains legacy token '{token}'")
                break

    if missing or forbidden_present or legacy_path_references or policy_failures:
        print("[prepare_synthetic_assets] synthetic asset policy check failed")
        if missing:
            print("  missing canonical assets:")
            for rel in missing:
                print(f"    - {rel}")
        if forbidden_present:
            print("  forbidden legacy assets present:")
            for rel in forbidden_present:
                print(f"    - {rel}")
        if legacy_path_references:
            print("  forbidden legacy path references:")
            for rel in legacy_path_references:
                print(f"    - {rel}")
        if policy_failures:
            print("  manifest policy failures:")
            for rel in policy_failures:
                print(f"    - {rel}")
        return 1

    print("[prepare_synthetic_assets] synthetic asset policy check passed")
    return 0


# The C++ generators write ~390k splats across seven fixtures. On the CI runner
# the binary under test is a dev_build (-O0) editor, where that is minutes rather
# than seconds; the previous 120 s budget was never measured against that build.
# A timeout here is indistinguishable from a broken generator, and since #790
# that outcome is a hard failure rather than a silent downgrade - so the budget
# has to be generous enough that only a genuinely stuck generator hits it.
CPP_GENERATION_TIMEOUT_S = 900


def _generate_via_godot(godot_binary: Path, output_dir: Path, quiet: bool) -> bool:
    """Run the Godot [GeneratePLY] test case to produce high-quality fixtures.

    Returns True on success, False on failure.
    """
    env = os.environ.copy()
    env["SYNTHETIC_PLY_OUTPUT_DIR"] = str(output_dir)
    cmd = [
        str(godot_binary),
        "--headless",
        "--test",
        "--test-case=*GeneratePLY*",
    ]
    if not quiet:
        print(f"[prepare_synthetic_assets] running C++ generators via: {' '.join(cmd)}")
    # Captured BEFORE the run: the completeness check below has to distinguish
    # "this invocation wrote the fixtures" from "a previous Python-fallback run
    # left files with the right names lying around", which is the default state
    # of any workspace that has ever run this script without a binary.
    started_at = time.time()
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=CPP_GENERATION_TIMEOUT_S, env=env
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"[prepare_synthetic_assets] C++ generation failed: {exc}")
        return False

    if proc.returncode != 0:
        print(f"[prepare_synthetic_assets] C++ generation exited with code {proc.returncode}")
        if proc.stderr:
            for line in proc.stderr.strip().splitlines()[-5:]:
                print(f"  {line}")
        return False

    # Verify all expected files were written BY THIS RUN. Two seconds of slack
    # covers filesystem timestamp granularity, not a stale fixture.
    missing: list[str] = []
    stale: list[str] = []
    for name in sorted(CPP_GENERATED_FILENAMES):
        candidate = output_dir / name
        if not candidate.is_file():
            missing.append(name)
        elif candidate.stat().st_mtime < started_at - 2.0:
            stale.append(name)
    if missing:
        print(f"[prepare_synthetic_assets] C++ generation missing files: {missing}")
        return False
    if stale:
        print(
            "[prepare_synthetic_assets] C++ generation exited 0 but did not rewrite: "
            f"{stale}; these are leftovers from an earlier run, not generated fixtures"
        )
        return False

    if not quiet:
        for name in sorted(CPP_GENERATED_FILENAMES):
            size = (output_dir / name).stat().st_size
            print(
                f"[prepare_synthetic_assets] C++ generated {name} "
                f"({CPP_GENERATOR_SPLAT_COUNTS[name]} splats, {size:,} bytes)"
            )
    return True


CPP_PREP_COMMAND_HINT = (
    "python tests/runtime/prepare_synthetic_assets.py --godot-binary ./bin/<godot built with tests=yes>"
)


def _fallback_fidelity_report() -> list[str]:
    """Lines naming, per fixture, what the Python fallback costs against the C++ generator."""
    lines: list[str] = []
    for filename in sorted(CPP_GENERATOR_SPLAT_COUNTS):
        rich = CPP_GENERATOR_SPLAT_COUNTS[filename]
        fallback = PYTHON_FALLBACK_SPLAT_COUNTS.get(filename)
        if fallback is None:
            continue
        lines.append(f"    {filename}: {fallback} splats instead of {rich} ({rich / fallback:.0f}x smaller)")
    return lines


def _print_fallback_notice(reason: str) -> None:
    """Say plainly that the fixtures about to be written are not the benchmark workload.

    Deliberately unconditional on --quiet: --quiet is exactly what every CI
    invocation passes, and the whole defect in #790 was that this downgrade
    happened where nobody could see it.
    """
    print("[prepare_synthetic_assets] WARNING: LOW-FIDELITY FIXTURES")
    print(f"[prepare_synthetic_assets]   {reason}")
    print("[prepare_synthetic_assets]   the Python fallback generators will write:")
    for line in _fallback_fidelity_report():
        print(f"[prepare_synthetic_assets] {line}")
    print(
        "[prepare_synthetic_assets]   any benchmark number produced from these fixtures measures a "
        "different workload than the lane names."
    )
    print(f"[prepare_synthetic_assets]   to fix: {CPP_PREP_COMMAND_HINT}")


def _generate(
    repo_root: Path,
    quiet: bool,
    godot_binary: Path | None = None,
    allow_fallback: bool = False,
) -> int:
    removed: list[str] = []
    fixtures_dir = repo_root / "tests" / "fixtures"

    # Phase 1: Generate primary fixtures via C++ generators if a binary is available.
    cpp_generated = False
    if godot_binary is not None:
        cpp_generated = _generate_via_godot(godot_binary, fixtures_dir, quiet)
        if not cpp_generated:
            # #790: this used to fall back to Python and still exit 0. A caller
            # that passed --godot-binary asked for the benchmark workload; handing
            # it a 10x-smaller one and reporting success is the exact shape of
            # defect the issue documents. --allow-fallback opts back in explicitly.
            if not allow_fallback:
                print(
                    "[prepare_synthetic_assets] ERROR: --godot-binary was given but the C++ "
                    "generators did not produce the fixtures."
                )
                _print_fallback_notice(
                    "refusing to substitute them silently; re-run with --allow-fallback if a "
                    "low-fidelity tree is genuinely acceptable here"
                )
                return 1
            _print_fallback_notice(
                "C++ generation failed and --allow-fallback was given"
            )
    else:
        _print_fallback_notice("no --godot-binary was given")

    # Phase 2: Generate remaining files via Python.
    for spec in CANONICAL_SPECS:
        output = repo_root / spec.relative_path
        filename = Path(spec.relative_path).name
        is_primary = spec.relative_path.startswith("tests/fixtures/")

        if cpp_generated and is_primary and filename in CPP_GENERATED_FILENAMES:
            # Already generated by C++; skip Python generation for primary copy.
            continue

        if cpp_generated and not is_primary and filename in CPP_GENERATED_FILENAMES:
            # Copy the C++-generated primary to the secondary location.
            src = fixtures_dir / filename
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, output)
            if not quiet:
                print(f"[prepare_synthetic_assets] copied C++ {filename} -> {spec.relative_path}")
            continue

        # Python fallback generation.
        rows = _generate_rows(spec)
        _write_ply(output, rows)
        if not quiet:
            print(
                f"[prepare_synthetic_assets] wrote {spec.count:5d} splats ({spec.pattern}) -> {spec.relative_path}"
            )

    _write_manifest(repo_root)
    if not quiet:
        for rel_path in CANONICAL_MANIFESTS:
            print(f"[prepare_synthetic_assets] wrote manifest -> {rel_path}")

    for rel_path in FORBIDDEN_LEGACY_PLYS:
        file_path = repo_root / rel_path
        if file_path.exists():
            file_path.unlink()
            removed.append(rel_path)

    for rel_dir in FORBIDDEN_LEGACY_ASSET_DIRS:
        dir_path = repo_root / rel_dir
        if dir_path.is_dir():
            shutil.rmtree(dir_path)
            removed.append(rel_dir + "/")

    if not quiet and removed:
        print("[prepare_synthetic_assets] removed forbidden legacy assets:")
        for rel in removed:
            print(f"  - {rel}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate deterministic canonical benchmark/synthetic fixtures."
    )
    parser.add_argument(
        "--repo-root",
        default=None,
        help="Repository root (defaults to auto-detected root from script location).",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Only verify canonical synthetic assets and forbidden-asset policy.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress per-file output during generation.",
    )
    parser.add_argument(
        "--godot-binary",
        default=None,
        help="Path to a Godot editor binary built with tests=yes.  When given, "
             "the C++ [GeneratePLY] test case generates high-quality fixtures "
             "(50K-100K splats with SH, anisotropy, fBm noise) instead of the "
             "lightweight Python fallback generators.  Giving this flag is a "
             "REQUIREMENT, not a preference: if the C++ generators cannot run, "
             "the script fails instead of substituting the small fixtures.",
    )
    parser.add_argument(
        "--allow-fallback",
        action="store_true",
        help="Permit the lightweight Python fallback fixtures even though "
             "--godot-binary was given.  Only for trees where a low-fidelity "
             "corpus is genuinely acceptable; benchmark numbers produced from "
             "such a tree do not describe the workload their lane names.",
    )
    args = parser.parse_args()

    repo_root = _resolve_repo_root(args.repo_root)
    if not repo_root.is_dir():
        print(f"[prepare_synthetic_assets] invalid repo root: {repo_root}")
        return 1

    godot_binary: Path | None = None
    if args.godot_binary:
        godot_binary = Path(args.godot_binary).expanduser().resolve()
        if not godot_binary.is_file():
            print(f"[prepare_synthetic_assets] godot binary not found: {godot_binary}")
            return 1

    if args.check:
        return _check_only(repo_root)
    return _generate(repo_root, args.quiet, godot_binary, allow_fallback=args.allow_fallback)


if __name__ == "__main__":
    raise SystemExit(main())
