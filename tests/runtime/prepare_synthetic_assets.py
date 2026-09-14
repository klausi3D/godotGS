#!/usr/bin/env python3
"""Generate deterministic synthetic fixtures used by benchmark/runtime flows.

Policy:
- Only canonical generated fixture paths are allowed.
- Legacy hand-managed fixture assets are forbidden.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

RUNTIME_DIR = Path(__file__).resolve().parent
if str(RUNTIME_DIR) not in sys.path:
    sys.path.insert(0, str(RUNTIME_DIR))

import fixture_provenance
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
SYNTHETIC_PLY_WRITER = (
    RUNTIME_DIR.parents[1] / "modules" / "gaussian_splatting" / "tests" / "synthetic_ply_writer.cpp"
)

#: One pattern over both ways the writer appends a property, so a single ordered
#: scan can reproduce the ORDER it emits them in. Two separate passes cannot: the
#: writer emits `f_rest_0..44` between `f_dc_2` and `opacity`, and collecting the
#: literals first put that block at the end -- a property list that matches no
#: file the producer writes, since PLY property order IS the binary layout.
_PLY_PROPERTY_RE = re.compile(
    r'header \+= "property float ([A-Za-z0-9_]+)'
    r'|for \(int i = 0; i < (\d+); i\+\+\) \{\s*header \+= vformat\("property float ([a-z_]+)%d'
)


def parse_cpp_writer_properties() -> tuple[str, ...]:
    """The property names the C++ writer emits, READ FROM THE WRITER.

    The rich-fixture tests previously hand-authored the producer's header shape.
    A locally invented shape can only ever confirm what the author already
    believed: if `synthetic_ply_writer.cpp` changed its header, the positive test
    would stay green while describing a file the producer no longer writes.

    Names come back in EMISSION order, including the `f_rest_*` loop at the point
    the writer runs it. The first version collected the literals in one pass and
    appended the loop afterwards, which put `f_rest_0..44` after `rot_3` -- an
    order no file the producer writes has, and in PLY the property order IS the
    binary layout. A captured fixture is what showed it (see
    `ProducerCapturedPositiveTests`), which is the case for capture in one line:
    derivation from source is only as good as the reading of the source.

    Both optional blocks are included. `p_write_normals` and `p_write_sh1` are set
    together by the surface generators (sphere, cube, plane, torus in
    `generate_synthetic_ply_fixtures.h`), so this is the shape those fixtures
    have; the uniform and volumetric generators pass `p_write_normals=false` and
    their headers are the same list without `nx/ny/nz`.

    This is derivation from source, and NOT the same thing as a fixture captured
    from a real producer run. It establishes the coupling: the writer changing its
    header changes this list, so the shape a test asserts against cannot silently
    drift away from the shape the producer emits.
    """
    try:
        source = SYNTHETIC_PLY_WRITER.read_text(encoding="utf-8")
    except OSError:
        return ()
    names: list[str] = []
    for literal, loop_count, loop_prefix in _PLY_PROPERTY_RE.findall(source):
        if literal:
            names.append(literal)
        else:
            names.extend(f"{loop_prefix}{index}" for index in range(int(loop_count)))
    return tuple(names)


CPP_GENERATOR_SPLAT_COUNTS: dict[str, int] = parse_cpp_generator_counts()

# Files the C++ generators produce.  When --godot-binary is given these come from
# the engine binary; otherwise the Python fallback generators below create
# lightweight versions of them.
CPP_GENERATED_FILENAMES: frozenset[str] = frozenset(CPP_GENERATOR_SPLAT_COUNTS)

CANONICAL_SPECS: tuple[PLYSpec, ...] = (
    PLYSpec("tests/fixtures/test_splats.ply", 1024, 1101, "sphere", 3.0),
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/test_splats.ply", 1024, 1101, "sphere", 3.0),
    # The QA scene suite's own corpus. A separate file from test_splats.ply on
    # purpose: test_splats.ply is a benchmark fixture whose floor needs the C++
    # producer, while the QA route pairs compare THIS file against the committed
    # test_splats.gsplatworld bake, and tests/ci/baselines/qa_results.json was
    # measured at 1024 splats. Python-only by construction -- the name is not in
    # CPP_GENERATED_FILENAMES -- so no producer choice changes what QA loads.
    # Same seed and shape as the test_splats fallback, so it is the same bytes
    # the QA baseline was measured on. QaCorpusIsPinnedTest holds the coupling.
    PLYSpec("tests/examples/godot/test_project/tests/fixtures/qa_splats_1024.ply", 1024, 1101, "sphere", 3.0),
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
        "evidence_role": "low_noise_smoke_reference",
        "notes": "Low-noise regression reference lane backed by the lightweight canonical smoke asset (10k splats, single instance). Its frame rate is dominated by fixed per-frame overhead rather than splat workload, so it is a sensitive detector of overhead regressions and NOT a representative workload; it is deliberately no longer the published baseline (#790).",
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
        "asset_classification": "deterministic_synthetic",
        "evidence_role": "published_baseline",
        "notes": "Published baseline lane (#790). Resident-pipeline dense workload: a 14x14 grid of 196 synthetic_spiral instances at 25,000 splats each, so ~4.9M splats resident and ~4.9M reported visible per frame. Distance/LOD/screen culling are disabled and the per-instance cap (max_splats=32000) is above the asset's 25,000, so nothing is thinned; 64-bit sort keys are forced. The lane id says '2m' because the preset was sized against the per-instance LOD floor (min_splats_per_frame=10000, 196 x 10k = 1.96M); that floor is a lower bound, not the cap, and the measured figure is ~4.9M visible. This is the lane whose number is published, because it is the only lane that exercises the resident sort/raster path under load; it measures ~12 FPS, and that is the honest figure for that workload.",
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

#: A `res://tests/fixtures/...` PLY reference, as written in scenario, scene and
#: benchmark source. ONE definition, imported by every consumer: it decides which
#: references the floor contract can see, and two copies of that decision drift.
#:
#: Deliberately permissive after the prefix. `[A-Za-z0-9_-]+\.ply` matched only a
#: flat, dot-free basename, so a legal path such as
#: `res://tests/fixtures/cases/sample.v2.ply` matched NOTHING -- and a reference
#: this reader cannot see is a reference the guard cannot govern: the scenario
#: showed no direct reference, an empty fixture contract was accepted, and the run
#: skipped floor preparation for a fixture it actually loads (#934 review).
#: An UNQUOTED `res://tests/fixtures/...` PLY reference. Bare references cannot
#: contain a space -- nothing would tell the path from the next word -- so this
#: half stops at whitespace, and `fixture_references_in()` reads the quoted half
#: from the quotes instead.
FIXTURE_REFERENCE_RE = re.compile(r"res://tests/fixtures/[^\s\"'\\]+\.ply")

#: A quoted string literal, as GDScript and `.tscn` write one. The value inside is
#: taken whole, spaces included: `res://tests/fixtures/cases/sample data.ply` is a
#: legal resource path, and a matcher that stops at the space sees no reference at
#: all -- so the scenario shows none, an empty fixture contract is accepted, and
#: the run skips floor preparation for a fixture it loads (#934 review).
_QUOTED_STRING_RE = re.compile(r"""(["'])((?:(?!\1)[^\r\n])*)\1""")

_FIXTURE_PREFIX = "res://tests/fixtures/"


def fixture_references_in(text: str) -> list[str]:
    """Every floor-governed fixture path `text` references, in order of appearance.

    Quoted values are read from the quotes, so a path containing spaces is seen
    whole. Bare occurrences -- comments, `.tscn` values that are not quoted -- fall
    back to the whitespace-terminated form, which is the most that can be read
    without a delimiter. Duplicates are collapsed; order is kept so a caller can
    report the first one.
    """
    seen: dict[str, None] = {}
    for _quote, value in _QUOTED_STRING_RE.findall(text):
        if value.startswith(_FIXTURE_PREFIX) and value.endswith(".ply"):
            seen.setdefault(value, None)
    for match in FIXTURE_REFERENCE_RE.finditer(text):
        seen.setdefault(match.group(0), None)
    return list(seen)


#: Prefix of the directory the producer writes into before anything is
#: published. It lives beside the corpus so the publish is a same-filesystem
#: rename, which means a killed run leaves it inside `tests/fixtures/` with
#: `.ply` files in it -- and `.gitignore`'s `tests/fixtures/*.ply` does not match
#: a nested path, so those leftovers show up as untracked and can be committed,
#: which the contribution rules forbid for generated fixtures. The ignore rule
#: keyed on this prefix is asserted by the tests.
STAGING_DIR_PREFIX = ".gs_ply_staging_"

#: The same floors, keyed by the filename a producer writes. Derived rather than
#: transcribed: a floor added above must not be able to go unchecked here.
FIXTURE_FLOORS_BY_FILENAME: dict[str, int] = {
    Path(resource_path).name: required
    for resource_path, required in ASSET_MIN_SPLAT_COUNTS.items()
}

# The same res:// path is consumed in two project roots: the repository-root
# doctest/runtime context and the canonical Godot test project.  A consumer
# gate must validate both copies or a direct res:// reference can select the
# unchecked one (issue #895).
ASSET_CONSUMER_PROJECT_ROOTS: tuple[Path, ...] = (
    Path("."),
    Path("tests/examples/godot/test_project"),
)


def read_ply_vertex_count(path: Path) -> int | None:
    """Read the declared PLY vertex count, failing closed on malformed input."""
    try:
        with path.open("rb") as stream:
            if stream.readline().strip() != b"ply":
                return None
            for _ in range(64):
                line = stream.readline()
                if not line:
                    return None
                fields = line.strip().split()
                if len(fields) == 3 and fields[:2] == [b"element", b"vertex"]:
                    try:
                        return int(fields[2])
                    except ValueError:
                        return None
                if line.strip() == b"end_header":
                    return None
    except OSError:
        return None
    return None


def ply_payload_failure(path: Path) -> str | None:
    """Why `path` is not a COMPLETE PLY, or None when its body is all there.

    `read_ply_vertex_count()` returns as soon as it reads the `element vertex`
    line: it never sees `end_header` and never looks at a single vertex. A
    declared count is therefore a CLAIM, and a producer that exits 0 after a short
    write -- `synthetic_ply_writer.cpp` ignores the result of every
    `store_buffer`/`store_float` call -- leaves a file whose claim satisfies every
    floor while its body is missing (#934 review).

    The body's size is derived from the header: vertex count times the number of
    declared properties times four. Anything the reader cannot size -- a non-float
    property, a second element block, a missing `end_header` -- is a reason, not a
    pass: sizing the body wrongly and calling it complete is the failure this
    exists to prevent.
    """
    try:
        with path.open("rb") as stream:
            if stream.readline().strip() != b"ply":
                return "does not begin with a PLY magic line"
            vertex_count: int | None = None
            properties = 0
            elements = 0
            formats: list[bytes] = []
            saw_end_header = False
            for _ in range(512):
                line = stream.readline()
                if not line:
                    break
                stripped = line.strip()
                if stripped == b"end_header":
                    saw_end_header = True
                    break
                fields = stripped.split()
                if fields[:1] == [b"format"]:
                    formats.append(stripped)
                    continue
                if fields[:1] == [b"element"]:
                    elements += 1
                    if elements > 1:
                        return "declares more than one element block; the body cannot be sized"
                    if len(fields) != 3 or fields[1] != b"vertex":
                        return f"unexpected element line {stripped!r}"
                    try:
                        vertex_count = int(fields[2])
                    except ValueError:
                        return f"unreadable vertex count in {stripped!r}"
                elif fields[:1] == [b"property"]:
                    if vertex_count is None:
                        # PLYLoader::parse_header() attaches a property to the vertex
                        # schema only while the current element is `vertex`, so a
                        # property declared before it is ignored: the loader reads a
                        # narrower record at the wrong stride while this check sized
                        # the body WITH it (#934 review).
                        return (
                            f"declares {stripped.decode('ascii', 'replace')!r} before the "
                            "vertex element; the loader does not count it"
                        )
                    if len(fields) != 3 or fields[1] != b"float":
                        return f"property {stripped!r} is not a float; the body cannot be sized"
                    properties += 1
            if not saw_end_header:
                return "the header never ends (no end_header line)"
            # The loader decides binary vs ASCII from this one line and defaults to
            # ASCII without it (`PLYLoader::PLYHeader::is_binary = false`), so a
            # binary body under a missing or wrong declaration is handed to the
            # ASCII parser and rejected as corrupt -- while every byte count here
            # still matched. And the size below only means anything for the
            # encoding both producers write (#934 review).
            if len(formats) != 1:
                return (
                    "declares no format line" if not formats
                    else f"declares {len(formats)} format lines"
                )
            if formats[0] != REQUIRED_PLY_FORMAT_LINE:
                return (
                    f"declares {formats[0].decode('ascii', 'replace')!r}, not "
                    f"{REQUIRED_PLY_FORMAT_LINE.decode('ascii')!r}; the body cannot be "
                    "sized, and the loader would not read it as binary"
                )
            if vertex_count is None:
                return "the header declares no vertex element"
            if properties == 0:
                return "the header declares no properties"
            expected = stream.tell() + vertex_count * properties * 4
        actual = path.stat().st_size
    except OSError as exc:
        return f"could not be read ({exc})"
    if actual != expected:
        return (
            f"{actual:,} bytes on disk, {expected:,} expected for {vertex_count:,} "
            f"vertices x {properties} float properties"
        )
    return None


def ply_property_names(path: Path) -> "tuple[str, ...] | None":
    """The properties of the VERTEX element, in order; None if unreadable.

    Scoped the way the loader scopes them. `PLYLoader::parse_header()` adds a
    property to the vertex schema only while the current element is `vertex`, so a
    name declared before that element, or under another one, is not a vertex
    property no matter what it is called -- and counting it let a file satisfy
    the schema with a property the loader never reads (#934 review).
    """
    names: list[str] = []
    current_element: bytes | None = None
    try:
        with path.open("rb") as stream:
            if stream.readline().strip() != b"ply":
                return None
            for _ in range(512):
                line = stream.readline()
                if not line:
                    return None
                stripped = line.strip()
                if stripped == b"end_header":
                    return tuple(names)
                fields = stripped.split()
                if fields[:1] == [b"element"] and len(fields) >= 2:
                    current_element = fields[1]
                elif (
                    fields[:1] == [b"property"]
                    and len(fields) >= 3
                    and current_element == b"vertex"
                ):
                    names.append(fields[-1].decode("ascii", "replace"))
    except OSError:
        return None
    return None


def ply_schema_failure(path: Path) -> str | None:
    """Why `path` is not a Gaussian splat PLY this repo's producers write, or None.

    A complete payload is not a usable one. `ply_payload_failure()` sizes the body
    from whatever properties the header declares, so a 10,000-vertex file holding
    only `property float x` and 40,000 bytes is complete by that rule and clears
    every floor (#934 review). The loader does not refuse it either: a missing
    property is filled with a default (`ply_loader.cpp`, `default_gaussian` and
    `parse_vertex`), so that file loads as 10,000 splats at the origin with unit
    scale and white colour -- a fixture that measures nothing, silently.

    The required set is REQUIRED_PLY_PROPERTIES: the properties BOTH producers
    always write. It is derived from the Python fallback's own header rather than
    listed here, and the tests couple it to the C++ writer and to the loader, so
    none of the three can drift without a failure naming it.
    """
    names = ply_property_names(path)
    if names is None:
        return "the header cannot be read"
    duplicated = sorted({name for name in names if names.count(name) > 1})
    if duplicated:
        return f"declares {', '.join(duplicated)} more than once"
    missing = [name for name in REQUIRED_PLY_PROPERTIES if name not in names]
    if missing:
        return f"is missing required propert{'y' if len(missing) == 1 else 'ies'} {', '.join(missing)}"
    return None


#: `PLYLoader::parse_vertex()` stores `exp(scale_n)` in a float field
#: (ply_loader.cpp:654-656); a log-scale above ln(FLT_MAX) overflows it to inf.
FLOAT32_MAX = 3.4028234663852886e38
MAX_LOG_SCALE = math.log(FLOAT32_MAX)
SCALE_PROPERTIES: tuple[str, ...] = ("scale_0", "scale_1", "scale_2")
ROTATION_PROPERTIES: tuple[str, ...] = ("rot_0", "rot_1", "rot_2", "rot_3")
#: A float32 square can only round to zero below this magnitude: the smallest
#: subnormal float32 is ~1.4e-45, and (1e-22)**2 = 1e-44 is well above half of it.
#: Components at or above it take no float32 emulation at all.
FLOAT32_SQUARE_UNDERFLOW_BOUND = 1e-22


def _float32(value: float) -> float:
    """`value` rounded to the nearest float32, the way a single-precision `real_t` holds it."""
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def ply_value_failure(path: Path) -> str | None:
    """Why the loader would REFUSE the splats in `path`, or None when it would load them.

    A complete body with the producers' schema can still hold values the runtime
    refuses: `GaussianSplatAsset::load_from_file()` rejects the whole asset when
    `GaussianData::all_render_fields_finite()` finds a single non-finite position,
    scale, rotation, opacity or SH value (gaussian_data.cpp:586-624). So a
    producer regression that wrote a NaN passed every structural check here and
    replaced a usable corpus with one no scenario can load (#934 review). Two
    decoded values turn non-finite from finite input, so they are checked the way
    the loader computes them:

    * scale -- the loader stores `exp(scale_n)` (ply_loader.cpp:654-656), which
      overflows a float to inf above ln(FLT_MAX);
    * rotation -- it normalizes the quaternion (ply_loader.cpp:660-664), and
      `Quaternion::normalize()` divides by the length (core/math/quaternion.cpp:65-67).
      The length comes from `length_squared()`, `x*x + y*y + z*z + w*w` in `real_t`
      (core/math/quaternion.h:173-179), which is float in the default
      `precision=single` build (SConstruct:192). So not only an all-zero rotation
      becomes NaN, but any rotation whose every component squares to a float32
      zero -- `rot_0 = 1e-30` does (#934 review). A double-precision build would
      load those; this follows the default build.

    Any non-finite float in the body is refused. That is slightly stricter than the
    loader, which does not check normals and saturates an infinite opacity logit;
    a producer that writes one is broken either way. Call it on a file that
    `ply_payload_failure()` and `ply_schema_failure()` have already accepted.
    """
    names = ply_property_names(path)
    vertex_count = read_ply_vertex_count(path)
    if not names or vertex_count is None:
        return "the header cannot be read"
    stride = len(names)
    try:
        with path.open("rb") as stream:
            while True:
                line = stream.readline()
                if not line:
                    return "the header never ends (no end_header line)"
                if line.strip() == b"end_header":
                    break
            body = stream.read()
    except OSError as exc:
        return f"could not be read ({exc})"
    if len(body) != vertex_count * stride * 4:
        return f"the body is {len(body):,} bytes, not {vertex_count * stride * 4:,}"
    values = array.array("f")
    values.frombytes(body)
    if sys.byteorder != "little":
        values.byteswap()

    # One pass decides; the slow scan runs only to name the offender.
    try:
        finite = math.isfinite(math.fsum(values))
    except (ValueError, OverflowError):  # fsum raises on inf + -inf
        finite = False
    if not finite:
        index = next(i for i, value in enumerate(values) if not math.isfinite(value))
        return (
            f"splat {index // stride} has a non-finite {names[index % stride]} "
            f"({values[index]}); the loader refuses the whole asset"
        )

    for name in SCALE_PROPERTIES:
        if name not in names:
            continue
        column = values[names.index(name)::stride]
        if max(column, default=0.0) > MAX_LOG_SCALE:
            splat = next(i for i, value in enumerate(column) if value > MAX_LOG_SCALE)
            return (
                f"splat {splat} has {name}={column[splat]}; the loader's exp() of it "
                "overflows to inf and the asset is refused"
            )

    if all(name in names for name in ROTATION_PROPERTIES):
        columns = [values[names.index(name)::stride] for name in ROTATION_PROPERTIES]
        for splat, quaternion in enumerate(zip(*columns)):
            # The product of two float32 values is exact in a double, so rounding it
            # once reproduces the float32 square; a sum of non-negative float32
            # values is zero only when every term is.
            if all(abs(c) < FLOAT32_SQUARE_UNDERFLOW_BOUND for c in quaternion) and all(
                _float32(c * c) == 0.0 for c in quaternion
            ):
                return (
                    f"splat {splat} has a rotation whose float32 length is zero "
                    f"({', '.join(repr(c) for c in quaternion)}); the loader's normalize() "
                    "turns it into NaN and the asset is refused"
                )
    return None


def fixture_floor_failure(path: Path, required_splats: int) -> str | None:
    """Why `path` does not satisfy `required_splats`, or None when it does.

    COMPLETE first, then counted. A declared vertex count is a claim the file
    makes about itself; `ply_payload_failure()` is what checks the file kept it.
    Round 7 taught the staging path that difference, but every decision about the
    PUBLISHED corpus -- the files the runtime lanes actually load -- still rested
    on the claim alone: the `--require-asset-floors` gate and both preservation
    branches read `read_ply_vertex_count()` and nothing else. A fixture truncated
    by a short write, an interrupted copy or a killed job keeps a header claiming
    50,000 splats, so it cleared the floor, was preserved in place by the next
    run, and was measured by the lanes as if it were whole (#934 review).

    One predicate, three callers, so "satisfies the floor" cannot mean two
    different things in the same script.
    """
    if not path.is_file():
        return "MISSING"
    payload_problem = ply_payload_failure(path)
    if payload_problem is not None:
        return f"INCOMPLETE ({payload_problem})"
    schema_problem = ply_schema_failure(path)
    if schema_problem is not None:
        return f"NOT A SPLAT FIXTURE ({schema_problem})"
    # ...and a well-formed splat file can still hold values the loader refuses.
    value_problem = ply_value_failure(path)
    if value_problem is not None:
        return f"UNLOADABLE ({value_problem})"
    actual = read_ply_vertex_count(path)
    if actual is None:
        return "UNVERIFIABLE"
    if required_splats > 0 and actual < required_splats:
        return f"UNDERSIZED (has {actual} splats)"
    return None


def _resource_path_for_spec(spec: PLYSpec) -> str | None:
    spec_path = Path(spec.relative_path)
    for project_root in ASSET_CONSUMER_PROJECT_ROOTS:
        try:
            project_relative = spec_path.relative_to(project_root)
        except ValueError:
            continue
        candidate = "res://" + project_relative.as_posix()
        if candidate in ASSET_MIN_SPLAT_COUNTS:
            return candidate
    return None


def asset_floor_failures(repo_root: Path) -> list[str]:
    """Return every absent, unreadable, or undersized consumer fixture."""
    failures: list[str] = []
    for project_root in ASSET_CONSUMER_PROJECT_ROOTS:
        for resource_path, required in sorted(ASSET_MIN_SPLAT_COUNTS.items()):
            relative_resource = Path(resource_path.removeprefix("res://"))
            asset_file = repo_root / project_root / relative_resource
            problem = fixture_floor_failure(asset_file, required)
            if problem is None:
                continue
            relative = asset_file.relative_to(repo_root).as_posix()
            if problem.startswith("UNDERSIZED"):
                failures.append(
                    f"{relative}: {problem} but {resource_path} requires >= {required}"
                )
            else:
                failures.append(
                    f"{relative}: {problem}; {resource_path} requires >= {required} splats"
                )
    return failures

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
# The two producer labels live in fixture_provenance: the record this script
# writes and the classifier that reads it must mean the same strings, and a
# second spelling here is how they would drift apart.
VARIANT_PYTHON_FALLBACK = fixture_provenance.VARIANT_PYTHON_FALLBACK
VARIANT_CPP_RICH = fixture_provenance.VARIANT_CPP_RICH


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


#: The properties every Gaussian splat fixture in this repo carries: the ones BOTH
#: producers always write. Read back from the fallback's own header rather than
#: listed a second time, and pinned by the tests against
#: `synthetic_ply_writer.cpp` (each must be an unconditional emission there) and
#: against `ply_loader.cpp` (each must be a property the loader reads).
#: The format declaration both producers write, read back from the fallback's
#: header rather than spelled again; the tests pin it against the C++ writer.
REQUIRED_PLY_FORMAT_LINE: bytes = next(
    line.strip() for line in _header(1).splitlines() if line.startswith(b"format ")
)

REQUIRED_PLY_PROPERTIES: tuple[str, ...] = tuple(
    line.split()[-1].decode("ascii")
    for line in _header(1).splitlines()
    if line.startswith(b"property ")
)


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

#: Where the pre-staging prep (#969) moved existing fixtures aside while the C++
#: producer wrote straight into tests/fixtures. Staging replaced that: the producer
#: now writes into an empty staging directory and the canonical files are not
#: touched until the staged corpus is validated. A directory left by an older
#: run can still hold originals a failed restore stranded, and nothing reads it
#: any more -- so its presence is reported, never silently ignored.
LEGACY_QUARANTINE_DIRNAME = ".pre_cpp_generation"


def _generate_via_godot(godot_binary: Path, output_dir: Path, quiet: bool) -> bool:
    """Run the Godot [GeneratePLY] test case to produce high-quality fixtures.

    Returns True on success, False on failure (caller should fall back to Python).

    The producer writes into an empty staging directory, never straight into
    ``output_dir``, and a file is only accepted once it has been moved out of
    that staging directory.  Exit status alone does not prove the generators
    ran: doctest returns ``EXIT_SUCCESS`` whenever no test case *failed*,
    including when zero cases matched the filter
    (``thirdparty/doctest/doctest.h``), so a binary that predates the
    ``[GeneratePLY]`` case exits 0 and writes nothing.  Deciding success from
    the mere existence of the expected names in ``output_dir`` would then hand
    back whatever an unrelated earlier run left there and report it as this
    producer's output.  Staging makes existence proof of writing.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=STAGING_DIR_PREFIX, dir=output_dir) as staging_name:
        staging_dir = Path(staging_name)
        env = os.environ.copy()
        env["SYNTHETIC_PLY_OUTPUT_DIR"] = str(staging_dir)
        cmd = [
            str(godot_binary),
            "--headless",
            "--test",
            "--test-case=*GeneratePLY*",
        ]
        if not quiet:
            print(f"[prepare_synthetic_assets] running C++ generators via: {' '.join(cmd)}")
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=CPP_GENERATION_TIMEOUT_S, env=env)
        except (OSError, subprocess.TimeoutExpired) as exc:
            print(f"[prepare_synthetic_assets] C++ generation failed: {exc}")
            return False

        if proc.returncode != 0:
            print(f"[prepare_synthetic_assets] C++ generation exited with code {proc.returncode}")
            if proc.stderr:
                for line in proc.stderr.strip().splitlines()[-5:]:
                    print(f"  {line}")
            return False

        # Verify all expected files were written *by this run*.  They can only be
        # here if the producer wrote them: the staging directory was created empty.
        missing = [
            name for name in sorted(CPP_GENERATED_FILENAMES) if not (staging_dir / name).is_file()
        ]
        if missing:
            print(
                f"[prepare_synthetic_assets] C++ generation exited 0 but did not write: {missing}"
            )
            print(
                "[prepare_synthetic_assets] the selected binary ran no [GeneratePLY] case "
                "(doctest exits 0 when zero cases match); any file already in "
                f"{output_dir} came from a different run and is not this producer's output"
            )
            return False

        # Validate the staged corpus BEFORE it replaces the canonical one.
        #
        # A producer regression that writes every expected file but writes it
        # SMALL is exactly what the floors exist to catch, and publishing first
        # meant `asset_floor_failures()` correctly failed the command in a
        # workspace whose usable fixtures had already been overwritten with the
        # bad ones -- broken until the next successful generation. Rejecting here
        # needs no rollback to get right: the staging directory is discarded and
        # the canonical files were never touched.
        rejected: list[str] = []
        for name in sorted(CPP_GENERATED_FILENAMES):
            staged = staging_dir / name
            # Structure first: a declared count is a claim, and the floor check
            # below believes it. A truncated file whose header claims 50,000
            # splats clears every floor there is.
            payload_problem = ply_payload_failure(staged)
            if payload_problem is not None:
                rejected.append(f"{name}: {payload_problem}")
                continue
            # ...and a complete file is not a splat file. A body sized from ONE
            # declared float passes the structural check above and every floor.
            schema_problem = ply_schema_failure(staged)
            if schema_problem is not None:
                rejected.append(f"{name}: {schema_problem}")
                continue
            # ...and a splat file is not a loadable one. A NaN position or an
            # all-zero rotation passes both checks above and every floor, and the
            # runtime then refuses the asset in every scenario (#934 review).
            value_problem = ply_value_failure(staged)
            if value_problem is not None:
                rejected.append(f"{name}: {value_problem}")
                continue
            floor = FIXTURE_FLOORS_BY_FILENAME.get(name, 0)
            if floor <= 0:
                continue
            staged_splats = read_ply_vertex_count(staged)
            if staged_splats is None:
                rejected.append(f"{name}: no vertex count could be read from the header")
            elif staged_splats < floor:
                rejected.append(f"{name}: {staged_splats} splats, floor is {floor}")
        if rejected:
            print(
                "[prepare_synthetic_assets] the producer ran but its output is not usable "
                "as the fixture corpus:"
            )
            for line in rejected:
                print(f"  - {line}")
            print(
                "  nothing was published; the fixtures already in the workspace are "
                "untouched and still usable"
            )
            return False

        # Publish by REPLACING, not by deleting and then moving.
        #
        # `unlink()` followed by `shutil.move()` leaves a window in which the
        # canonical fixture is already gone and the new one is not yet there. A
        # move that fails in that window -- a sharing lock on the persistent
        # Windows runner, a full disk -- destroyed the existing fixture, raised
        # out of prep as an unhandled OSError, and took the staging directory
        # (with the replacement in it) down with the `with` block. `os.replace()`
        # is atomic within a filesystem, and the staging directory is created
        # inside `output_dir`, so the destination is never transiently absent.
        #
        # ...and publish the corpus ALL OR NOTHING. Each replace is atomic, but the
        # corpus is several files: a replace that failed after earlier ones had
        # succeeded left this run's fixtures beside the previous run's. Every one
        # of them is structurally whole and above its floor, so no later check can
        # see the mixture (#934 review). Copy each file about to be replaced first
        # -- copying leaves the destination in place -- and put the previous corpus
        # back if any replacement fails.
        previous: dict[str, Path | None] = {}
        backup_dir = staging_dir / ".previous_corpus"
        try:
            backup_dir.mkdir()
            for name in sorted(CPP_GENERATED_FILENAMES):
                destination = output_dir / name
                if destination.exists():
                    shutil.copy2(destination, backup_dir / name)
                    previous[name] = backup_dir / name
                else:
                    previous[name] = None
        except OSError as exc:
            print(
                "[prepare_synthetic_assets] could not keep a copy of the current fixtures "
                f"before publishing ({exc}); nothing was published"
            )
            return False

        published: list[str] = []
        for name in sorted(CPP_GENERATED_FILENAMES):
            destination = output_dir / name
            try:
                os.replace(staging_dir / name, destination)
            except OSError as exc:
                print(
                    "[prepare_synthetic_assets] could not publish the generated "
                    f"{name}: {exc}"
                )
                not_restored = _roll_back_publish(output_dir, published, previous)
                if not_restored:
                    print(
                        "  ROLLBACK INCOMPLETE: "
                        + ", ".join(not_restored)
                        + " still hold this run's output while the rest of the corpus is "
                        "the previous run's. Every file is individually valid, so nothing "
                        f"downstream can detect the mixture: delete the fixtures in {output_dir} "
                        "and regenerate before using them"
                    )
                elif published:
                    print(
                        "  rolled back " + ", ".join(published) + "; the fixtures in the "
                        "workspace are the previous corpus, unchanged"
                    )
                else:
                    print("  nothing was published; the fixtures in the workspace are unchanged")
                return False
            published.append(name)

    if not quiet:
        for name in sorted(CPP_GENERATED_FILENAMES):
            size = (output_dir / name).stat().st_size
            print(
                f"[prepare_synthetic_assets] C++ generated {name} "
                f"({CPP_GENERATOR_SPLAT_COUNTS[name]} splats, {size:,} bytes)"
            )
    return True


def _roll_back_publish(
    output_dir: Path, published: list[str], previous: dict[str, Path | None]
) -> list[str]:
    """Restore the corpus a failed publish had partly replaced; return what it could not.

    A file that existed before is put back from its copy by an atomic replace. A
    file that did not exist before is removed again, because "absent" is what the
    workspace held. Newest first, so a failure part-way leaves the longest possible
    prefix of the corpus unchanged.
    """
    not_restored: list[str] = []
    for name in reversed(published):
        destination = output_dir / name
        backup = previous.get(name)
        try:
            if backup is None:
                destination.unlink()
            else:
                os.replace(backup, destination)
        except OSError:
            not_restored.append(name)
    return sorted(not_restored)


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
    print(
        "[prepare_synthetic_assets]   where no whole, floor-valid fixture is already in place, "
        "the Python fallback generators write:"
    )
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
    *,
    preserve_floor_valid: bool = False,
    allow_fallback: bool = False,
) -> int:
    removed: list[str] = []
    fixtures_dir = repo_root / "tests" / "fixtures"

    legacy_quarantine = fixtures_dir / LEGACY_QUARANTINE_DIRNAME
    if legacy_quarantine.exists():
        # Unconditional on --quiet: it is a statement about files a person may need.
        print(
            f"[prepare_synthetic_assets] WARNING: {legacy_quarantine} was left by an older "
            "prep run. It may hold original fixtures that run could not restore; this "
            "script no longer reads it. Inspect it, then delete it."
        )

    # Phase 1: Generate primary fixtures via C++ generators if a binary is available.
    cpp_generated = False
    if godot_binary is not None:
        cpp_generated = _generate_via_godot(godot_binary, fixtures_dir, quiet)
        if not cpp_generated:
            # Passing --godot-binary SELECTS a producer; it does not merely offer
            # one. Falling back reported success while the selected producer had
            # failed: the Python fallback declares fewer splats than the floor,
            # the preservation branch below then keeps an existing fixture from an
            # unrelated earlier run, and module and runtime validation proceed
            # against it. The floor check passes because the old file satisfies
            # it -- so the failure of the thing under test is laundered into a
            # pass by a leftover file.
            #
            # Round 2 made that fatal only under --require-asset-floors, which
            # left the documented benchmark-prep commands -- neither of which
            # passes that flag -- taking the silent fallback (#934 review). The
            # rule is now the mode-independent one: a selected producer that fails
            # fails the command. --allow-fallback is the explicit opt-in for a
            # low-fidelity tree, and it cannot override the floor rule, because
            # under floors a leftover fixture is exactly what would absorb the
            # failure.
            if preserve_floor_valid or not allow_fallback:
                print(
                    "[prepare_synthetic_assets] ERROR: the selected --godot-binary did "
                    "not generate the fixtures."
                )
                if preserve_floor_valid:
                    print(
                        "  --require-asset-floors is set, so --allow-fallback does not "
                        "apply: a fixture already in the workspace came from a different "
                        "run and cannot stand in for this producer's output."
                    )
                else:
                    _print_fallback_notice(
                        "refusing to substitute them silently; re-run with --allow-fallback "
                        "if a low-fidelity tree is genuinely acceptable here"
                    )
                return 1
            _print_fallback_notice("C++ generation failed and --allow-fallback was given")
    else:
        _print_fallback_notice("no --godot-binary was given")

    # Phase 2: Generate remaining files via Python.
    #
    # `produced` records who wrote each file as it is written, rather than being
    # reconstructed afterwards from the same conditions -- a second copy of this
    # branching is a second thing to keep in step with it.
    produced: dict[Path, str] = {}
    if cpp_generated:
        for name in sorted(CPP_GENERATED_FILENAMES):
            produced[fixtures_dir / name] = VARIANT_CPP_RICH
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
            resource_path = _resource_path_for_spec(spec)
            required_splats = ASSET_MIN_SPLAT_COUNTS.get(resource_path or "", 0)
            # Preserve only what is WHOLE and over the floor. Deciding this on
            # the declared count alone kept a truncated consumer copy in place
            # and skipped the copy that would have repaired it, run after run.
            existing_splats = read_ply_vertex_count(output)
            if (
                preserve_floor_valid
                and required_splats > 0
                and fixture_floor_failure(output, required_splats) is None
            ):
                if not quiet:
                    print(
                        f"[prepare_synthetic_assets] preserved {existing_splats:5d}-splat "
                        f"floor-valid consumer fixture -> {spec.relative_path}"
                    )
                continue
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, output)
            produced[output] = VARIANT_CPP_RICH
            if not quiet:
                print(f"[prepare_synthetic_assets] copied C++ {filename} -> {spec.relative_path}")
            continue

        # The Python fallback is intentionally lightweight.  In particular it
        # declares only 1024 vertices for test_splats.ply while the canonical
        # consumer contract requires 10000.  Never replace an existing fixture
        # that already satisfies the contract with that weaker fallback.
        resource_path = _resource_path_for_spec(spec)
        required_splats = ASSET_MIN_SPLAT_COUNTS.get(resource_path or "", 0)
        existing_splats = read_ply_vertex_count(output)
        fallback_is_below_floor = required_splats > 0 and spec.count < required_splats
        # Same rule as above, and it matters more here: this branch runs without
        # --require-asset-floors, so a truncated leftover claiming enough splats
        # was preserved by EVERY subsequent run and the fallback never repaired
        # it. A file that is not whole is not a fixture worth keeping; falling
        # through writes the small-but-honest fallback instead.
        if (
            fallback_is_below_floor
            and fixture_floor_failure(output, required_splats) is None
        ):
            if not quiet:
                print(
                    f"[prepare_synthetic_assets] preserved {existing_splats:5d}-splat "
                    f"canonical fixture -> {spec.relative_path}"
                )
            continue

        # Python fallback generation.
        rows = _generate_rows(spec)
        _write_ply(output, rows)
        produced[output] = VARIANT_PYTHON_FALLBACK
        if not quiet:
            print(
                f"[prepare_synthetic_assets] wrote {spec.count:5d} splats ({spec.pattern}) -> {spec.relative_path}"
            )

    # Record which producer wrote each file. `--require-asset-variant`
    # authenticated fixtures from their header and their vertex count, and both
    # describe a file shape that can be assembled outside the generator or copied
    # from another fixture; the record is what makes a label mean "this producer
    # wrote these bytes under this name" (#790 review). BOTH producers are
    # recorded -- the fallback label is advertised as provenance too, and a
    # fixture used to satisfy it on its count alone.
    #
    # `retain` carries forward entries for copies still on disk, so a run that
    # leaves a fixture in place does not orphan its provenance, and prunes the
    # rest rather than leaving them to vouch for bytes no longer in the workspace.
    if not fixture_provenance.record_producer_output(
        fixtures_dir,
        produced,
        retain=[repo_root / spec.relative_path for spec in CANONICAL_SPECS],
    ):
        # A corpus whose provenance was not recorded is a corpus every consumer
        # will refuse: `--require-asset-variant` reads that record, so reporting
        # success here would hand the next step fixtures it must reject, with the
        # cause several minutes and one job behind it.
        print(
            "[prepare_synthetic_assets] ERROR: the fixtures were generated but their "
            "producer record could not be written, so nothing downstream can "
            "authenticate them."
        )
        return 1

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
        "--require-asset-floors",
        action="store_true",
        help="Fail after generation unless every runtime consumer fixture meets ASSET_MIN_SPLAT_COUNTS.",
    )
    parser.add_argument(
        "--allow-fallback",
        action="store_true",
        help="Permit the lightweight Python corpus when a --godot-binary was given "
             "but its generators failed. Without this, a selected producer that "
             "fails fails the command; with --require-asset-floors it fails "
             "regardless. Benchmark numbers produced from such a tree do not "
             "describe the workload their lane names.",
    )
    args = parser.parse_args()

    repo_root = _resolve_repo_root(args.repo_root)
    if not repo_root.is_dir():
        print(f"[prepare_synthetic_assets] invalid repo root: {repo_root}")
        return 1

    godot_binary: Path | None = None
    if args.godot_binary:
        binary_candidate = Path(args.godot_binary).expanduser()
        resolved_command = shutil.which(args.godot_binary)
        if binary_candidate.is_file():
            godot_binary = binary_candidate.resolve()
        elif resolved_command:
            godot_binary = Path(resolved_command).resolve()
        else:
            print(f"[prepare_synthetic_assets] godot binary not found: {args.godot_binary}")
            return 1

    if args.check:
        return _check_only(repo_root)
    result = _generate(
        repo_root,
        args.quiet,
        godot_binary,
        preserve_floor_valid=args.require_asset_floors,
        allow_fallback=args.allow_fallback,
    )
    if result != 0 or not args.require_asset_floors:
        return result

    failures = asset_floor_failures(repo_root)
    if failures:
        print("[prepare_synthetic_assets] runtime fixture floor check failed")
        for failure in failures:
            print(f"  - {failure}")
        print(
            "  regenerate with a tests-enabled binary: "
            "python tests/runtime/prepare_synthetic_assets.py --godot-binary <binary> "
            "--require-asset-floors"
        )
        return 1
    if not args.quiet:
        print("[prepare_synthetic_assets] runtime fixture floor check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
