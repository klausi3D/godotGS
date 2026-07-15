#!/usr/bin/env python3
"""
Gaussian Splatting shader validation matrix.

Implements milestone pre-alpha shader gates:
- #1267 / #1318: explicit runtime stage compile matrix coverage.
- #1320: shader-host ABI contract verification.
- #1322: explicit per-dispatch counter initialization contract verification.
- #1324: diagnostics instrumentation gate verification.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_DIR = REPO_ROOT / "modules" / "gaussian_splatting"
SHADERS_DIR = MODULE_DIR / "shaders"
COMPUTE_DIR = MODULE_DIR / "compute"
DEFAULT_OUTPUT_DIR = SHADERS_DIR / ".compiled_spv"

ISSUE_RUNTIME_MATRIX = "#1267/#1318"
ISSUE_ABI = "#1320"
ISSUE_COUNTER_INIT = "#1322"
ISSUE_DIAGNOSTICS = "#1324"
ISSUE_RASTER_BOUNDS = "#51"

SECTION_TAG_RE = re.compile(r"^\s*#\[(compute|vertex|fragment)\]\s*$")
VERSION_DEFINES_RE = re.compile(r"^\s*#VERSION_DEFINES\s*$")
INCLUDE_GEN_RE = re.compile(r"#include\s+\"(?:\.\./)?(?P<dir>shaders|compute)/(?P<name>[A-Za-z0-9_./-]+)\.glsl\.gen\.h\"")
INCLUDE_EXTENSION_RE = re.compile(r"^\s*#extension\s+GL_(GOOGLE_include_directive|ARB_shading_language_include)\b", re.MULTILINE)
VERSION_LINE_RE = re.compile(r"^\s*#version[^\n]*(?:\n|$)", re.MULTILINE)


@dataclass(frozen=True)
class Variant:
    name: str
    defines: tuple[str, ...] = ()
    issue_ids: tuple[str, ...] = ()


@dataclass(frozen=True)
class ShaderMatrixEntry:
    key: str
    source: Path
    stages: tuple[str, ...]
    variants: tuple[Variant, ...]
    issue_ids: tuple[str, ...]


@dataclass(frozen=True)
class FilePatternSet:
    path: Path
    patterns: tuple[str, ...]


@dataclass(frozen=True)
class ValidationContract:
    key: str
    issue_id: str
    description: str
    files: tuple[FilePatternSet, ...]


@dataclass(frozen=True)
class CompilerTool:
    kind: str
    path: str


def _merge_defines(*define_groups: Iterable[str]) -> tuple[str, ...]:
    merged: list[str] = []
    seen: set[str] = set()
    for group in define_groups:
        for define in group:
            if define in seen:
                continue
            seen.add(define)
            merged.append(define)
    return tuple(merged)


LIGHTING_DEFINES = (
    "MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS=4",
    "GS_MAX_OMNI_LIGHTS=32",
    "GS_MAX_SPOT_LIGHTS=32",
)

TILE_COMMON_DEFINES = _merge_defines(
    LIGHTING_DEFINES,
    (
        "GS_TILE_SIZE=16",
        "GS_TILE_SPLAT_CAPACITY=262144",
    ),
)

TILE_GLOBAL_SORT_DEFINES = (
    "GS_TILE_GLOBAL_SORT=1",
    "GS_SORT_KEY_BITS=64",
    "GS_SORT_TILE_BITS=32",
    "GS_SORT_DEPTH_BITS=32",
)

# Default 64-bit production layout WITH the deterministic tie-break suffix that
# TileRenderer::_build_binning_shader_defines() emits in normal frames: tie_bits=16
# whenever the tile id fits in <=16 bits (i.e. essentially every real scene). This is
# the DEFAULT shipped 64-bit path and exercises tile_binning.glsl's tie-break branch
# (the GS_SORT_TIE_BITS > 0 arm of gs_pack_sort_key, tile_binning.glsl:342). The base
# TILE_GLOBAL_SORT_DEFINES above leaves GS_SORT_TIE_BITS at its #ifndef default of 0,
# so without this group the production tie path was never compiled.
TILE_SORT_TIE_DEFINES = TILE_GLOBAL_SORT_DEFINES + ("GS_SORT_TIE_BITS=16",)

# 32-bit sort-key layout. Runtime-reachable only via an explicit opt-in
# (gpu_sorting/gpu_preset="custom" + key_bits=32 with a valid tile/depth split).
# SortKeyConfig::from_settings() (renderer/gpu_sorter.cpp) clamps the registered
# 32/32 tile/depth defaults to tile=32/depth=0 under key_bits=32, which
# TileRenderer::_get_effective_sort_key_config() then rejects back to 64-bit; the
# smallest valid split that survives to the shader is tile=16/depth=16 (the historical
# 32/16/16 layout the shader itself references at tile_binning.glsl:305-311). tie_bits
# is always 0 for 32-bit keys (tie_bits>0 is emitted only when key_bits>32). This
# exercises the GS_SORT_KEY_BITS == 32 arm of gs_pack_sort_key (tile_binning.glsl:293).
TILE_SORT_KEY32_DEFINES = (
    "GS_TILE_GLOBAL_SORT=1",
    "GS_SORT_KEY_BITS=32",
    "GS_SORT_TILE_BITS=16",
    "GS_SORT_DEPTH_BITS=16",
    "GS_SORT_TIE_BITS=0",
)

TILE_DIAGNOSTICS_OFF = ("GS_DEBUG_COUNTERS_DISABLED=1",)


RUNTIME_SHADER_MATRIX: tuple[ShaderMatrixEntry, ...] = (
    ShaderMatrixEntry(
        key="tile_binning",
        source=SHADERS_DIR / "tile_binning.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318", "#1324"),
        variants=(
            Variant(
                "emit_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                    ),
                ),
                ("#1267", "#1318", "#1324"),
            ),
            Variant(
                "emit_diag",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                    ),
                ),
                ("#1324",),
            ),
            Variant(
                "count_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_COUNT_PASS=1",
                    ),
                ),
                ("#1318",),
            ),
            Variant(
                "emit_metal_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_TARGET_METAL=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_mobile_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_TARGET_MOBILE=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_metal_mobile_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_TARGET_METAL=1",
                        "GS_TARGET_MOBILE=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            # --- G4 per-branch coverage: runtime-selectable emit-pass permutations ---
            # The base emit_prod above compiles only the 64-bit, no-tie, unpacked,
            # unquantized path. The following singleton toggles + one combined build
            # exercise every remaining runtime-selectable #ifdef branch in the emit
            # pass. No full Cartesian product (keeps the CI lane fast).
            Variant(
                "emit_tie",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_SORT_TIE_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_key32",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_SORT_KEY32_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_quantized",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "USE_QUANTIZED_GAUSSIANS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_packed",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_PACKED_STAGE_DATA=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_sh_amortized",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_SH_AMORTIZATION=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_subgroups",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_ENABLE_SUBGROUPS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "emit_prod_full",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_SORT_TIE_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_EMIT_PASS=1",
                        "GS_PACKED_STAGE_DATA=1",
                        "GS_SH_AMORTIZATION=1",
                        "USE_QUANTIZED_GAUSSIANS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            # Count-pass mirrors: _build_binning_count_defines() derives from
            # _build_common_shader_defines(true), so the quantized / packed / subgroup
            # toggles reach the count pass too (buffer-layout + platform-compat
            # branches). Tie/key32 do NOT apply to the count pass (gs_pack_sort_key is
            # guarded by GS_TILE_GLOBAL_SORT_EMIT_PASS), so they are not mirrored here.
            Variant(
                "count_quantized",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_COUNT_PASS=1",
                        "USE_QUANTIZED_GAUSSIANS=1",
                    ),
                ),
                ("#1318",),
            ),
            Variant(
                "count_packed",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_COUNT_PASS=1",
                        "GS_PACKED_STAGE_DATA=1",
                    ),
                ),
                ("#1318",),
            ),
            Variant(
                "count_subgroups",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_GLOBAL_SORT_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_DISPATCH_LOCAL_SIZE_X=256",
                        "GS_TILE_GLOBAL_SORT_COUNT_PASS=1",
                        "GS_ENABLE_SUBGROUPS=1",
                    ),
                ),
                ("#1318",),
            ),
        ),
    ),
    ShaderMatrixEntry(
        key="tile_prefix_scan",
        source=SHADERS_DIR / "tile_prefix_scan.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318"),
        variants=(
            Variant(
                "pass1_small",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_1=1", "GS_PREFIX_LOCAL_SIZE=128")),
                ("#1318",),
            ),
            Variant(
                "pass2_large",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_2=1", "GS_PREFIX_LOCAL_SIZE=256")),
                ("#1318",),
            ),
            Variant(
                "pass3_small",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_3=1", "GS_PREFIX_LOCAL_SIZE=128")),
                ("#1318",),
            ),
            # GS_ENABLE_SUBGROUPS reaches every prefix-scan pass at runtime:
            # ShaderCompilationManager::_build_prefix_defines() derives from
            # TileRenderer::_build_common_shader_defines(true), and tile_prefix_scan.glsl
            # includes platform_compat.glsl where the define switches the subgroup
            # extensions to `require`. Compile the subgroup permutation of all three
            # passes so the runtime subgroup path never ships uncompiled. The
            # per-variant _variant_target_env() lifts these to target-env vulkan1.1.
            Variant(
                "pass1_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_1=1", "GS_PREFIX_LOCAL_SIZE=128", "GS_ENABLE_SUBGROUPS=1")),
                ("#1318",),
            ),
            Variant(
                "pass2_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_2=1", "GS_PREFIX_LOCAL_SIZE=256", "GS_ENABLE_SUBGROUPS=1")),
                ("#1318",),
            ),
            Variant(
                "pass3_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_3=1", "GS_PREFIX_LOCAL_SIZE=128", "GS_ENABLE_SUBGROUPS=1")),
                ("#1318",),
            ),
        ),
    ),
    ShaderMatrixEntry(
        key="tile_rasterizer",
        source=SHADERS_DIR / "tile_rasterizer.glsl",
        stages=("vertex", "fragment"),
        issue_ids=("#1267", "#1318", "#1324"),
        variants=(
            Variant(
                "prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    ("GS_TILE_GLOBAL_SORT=1", "GS_MAX_RASTER_SPLATS_PER_TILE=4096", "GS_SORT_KEY_BITS=64"),
                ),
                ("#1324",),
            ),
            Variant(
                "diag",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    ("GS_TILE_GLOBAL_SORT=1", "GS_MAX_RASTER_SPLATS_PER_TILE=4096", "GS_COLLECT_RASTER_STATS=1", "GS_SORT_KEY_BITS=32"),
                ),
                ("#1324",),
            ),
            # GS_PACKED_STAGE_DATA and GS_ENABLE_SUBGROUPS come from
            # _build_common_shader_defines() and reach the rasterizer at runtime, so
            # the packed payload unpack path (tile_projection_common.glsl) and the
            # platform_compat subgroup extensions must both be compiled.
            Variant(
                "packed",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    ("GS_TILE_GLOBAL_SORT=1", "GS_MAX_RASTER_SPLATS_PER_TILE=4096", "GS_SORT_KEY_BITS=64", "GS_PACKED_STAGE_DATA=1"),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "subgroups",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    ("GS_TILE_GLOBAL_SORT=1", "GS_MAX_RASTER_SPLATS_PER_TILE=4096", "GS_SORT_KEY_BITS=64", "GS_ENABLE_SUBGROUPS=1"),
                ),
                ("#1267", "#1318"),
            ),
        ),
    ),
    ShaderMatrixEntry(
        key="tile_rasterizer_compute",
        source=SHADERS_DIR / "tile_rasterizer_compute.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318", "#1324"),
        variants=(
            Variant(
                "prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_SORT_KEY_BITS=64",
                    ),
                ),
                ("#1318", "#1324"),
            ),
            Variant(
                "diag",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_COLLECT_RASTER_STATS=1",
                        "GS_SORT_KEY_BITS=32",
                    ),
                ),
                ("#1324",),
            ),
            Variant(
                "metal_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_TARGET_METAL=1",
                    ),
                ),
                ("#1318",),
            ),
            Variant(
                "mobile_prod",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_TARGET_MOBILE=1",
                    ),
                ),
                ("#1318",),
            ),
            # Packed payload + subgroup paths reach the compute rasterizer via
            # _build_common_shader_defines() the same way they reach the fragment
            # rasterizer above.
            Variant(
                "packed",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_SORT_KEY_BITS=64",
                        "GS_PACKED_STAGE_DATA=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
            Variant(
                "subgroups",
                _merge_defines(
                    TILE_COMMON_DEFINES,
                    TILE_DIAGNOSTICS_OFF,
                    (
                        "GS_TILE_GLOBAL_SORT=1",
                        "GS_MAX_RASTER_SPLATS_PER_TILE=4096",
                        "GS_TILE_RASTER_COMPUTE=1",
                        "GS_SORT_KEY_BITS=64",
                        "GS_ENABLE_SUBGROUPS=1",
                    ),
                ),
                ("#1267", "#1318"),
            ),
        ),
    ),
    ShaderMatrixEntry(
        key="tile_resolve",
        source=SHADERS_DIR / "tile_resolve.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318"),
        variants=(
            Variant("rgba8", _merge_defines(LIGHTING_DEFINES, ("TILE_RESOLVE_FORMAT=0",)), ("#1318",)),
            Variant("rgba16f", _merge_defines(LIGHTING_DEFINES, ("TILE_RESOLVE_FORMAT=1",)), ("#1318",)),
            Variant("rgba32f", _merge_defines(LIGHTING_DEFINES, ("TILE_RESOLVE_FORMAT=2",)), ("#1318",)),
        ),
    ),
    ShaderMatrixEntry(
        key="gaussian_splat",
        source=SHADERS_DIR / "gaussian_splat.glsl",
        stages=("vertex", "fragment"),
        issue_ids=("#1267",),
        variants=(
            Variant("baseline", (), ("#1267",)),
            Variant("palette", ("PAINTERLY_ENABLE_PALETTE=1",), ("#1267",)),
            Variant("brush", ("PAINTERLY_ENABLE_BRUSH=1",), ("#1267",)),
            Variant("lighting", ("PAINTERLY_ENABLE_LIGHTING=1",), ("#1267",)),
            Variant(
                "full",
                (
                    "PAINTERLY_ENABLE_PALETTE=1",
                    "PAINTERLY_ENABLE_BRUSH=1",
                    "PAINTERLY_ENABLE_LIGHTING=1",
                ),
                ("#1267",),
            ),
        ),
    ),
    ShaderMatrixEntry(
        key="gs_shadow_blit",
        source=SHADERS_DIR / "gs_shadow_blit.glsl",
        stages=("vertex", "fragment"),
        issue_ids=("#1267",),
        variants=(Variant("default", (), ("#1267",)),),
    ),
    ShaderMatrixEntry(
        key="sobel_outline",
        source=SHADERS_DIR / "sobel_outline.glsl",
        stages=("compute",),
        issue_ids=("#1267",),
        variants=(Variant("default", (), ("#1267",)),),
    ),
    ShaderMatrixEntry(
        key="brush_accumulate",
        source=SHADERS_DIR / "brush_accumulate.glsl",
        stages=("compute",),
        issue_ids=("#1267",),
        variants=(Variant("default", (), ("#1267",)),),
    ),
    ShaderMatrixEntry(
        key="painterly_composite",
        source=SHADERS_DIR / "painterly_composite.glsl",
        stages=("vertex", "fragment"),
        issue_ids=("#1267",),
        variants=(Variant("default", (), ("#1267",)),),
    ),
    ShaderMatrixEntry(
        key="viewport_blit",
        source=SHADERS_DIR / "viewport_blit.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318"),
        variants=(
            Variant("format_unorm", ("VIEWPORT_BLIT_FORMAT=0",), ("#1318",)),
            Variant("format_half", ("VIEWPORT_BLIT_FORMAT=1",), ("#1318",)),
            Variant("format_float", ("VIEWPORT_BLIT_FORMAT=2",), ("#1318",)),
        ),
    ),
    ShaderMatrixEntry(
        key="frustum_cull",
        source=COMPUTE_DIR / "frustum_cull.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318", "#1324"),
        variants=(
            Variant("standard", (), ("#1318",)),
            Variant("subgroup", ("GS_ENABLE_SUBGROUPS=1",), ("#1318",)),
        ),
    ),
    ShaderMatrixEntry(
        key="depth_compute",
        source=COMPUTE_DIR / "depth_compute.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318"),
        variants=(
            Variant("default", (), ("#1318",)),
            Variant("quantized", ("USE_QUANTIZED_GAUSSIANS=1",), ("#1318",)),
        ),
    ),
    ShaderMatrixEntry(
        key="instance_chunk_dispatch",
        source=COMPUTE_DIR / "instance_chunk_dispatch.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318", "#1322"),
        variants=(Variant("default", (), ("#1322",)),),
    ),
    ShaderMatrixEntry(
        key="instance_count_clamp",
        source=COMPUTE_DIR / "instance_count_clamp.glsl",
        stages=("compute",),
        issue_ids=("#1267", "#1318", "#1320"),
        variants=(Variant("default", ("GS_DISPATCH_LOCAL_SIZE_X=256",), ("#1318", "#1320")),),
    ),
)


# G4 exit criterion ("every runtime-selectable shader permutation branch is compiled
# in CI"), made self-enforcing. Each key is a shader source (repo-relative, POSIX
# separators); each value is the set of exact define TOKENS (NAME=VALUE) that at least
# one of that source's matrix variants MUST carry. These tokens map 1:1 to
# runtime-selectable #ifdef branches whose runtime define is emitted by
# TileRenderer::_build_common_shader_defines() / _build_binning_shader_defines() /
# _build_raster_shader_defines() (see renderer/tile_renderer.cpp and the define audit
# in renderer/spirv_disk_cache.cpp). Adding a runtime-selectable branch to one of
# these shaders without a matrix variant that compiles it fails this check with a
# non-zero exit, the same way the entrypoint-coverage check does. Messages are
# ASCII-only. Do not weaken this map to make a build pass.
REQUIRED_VARIANT_DEFINES: dict[str, tuple[str, ...]] = {
    "modules/gaussian_splatting/shaders/tile_binning.glsl": (
        "GS_SORT_KEY_BITS=32",        # 32-bit sort-key pack path (tile_binning.glsl:293)
        "GS_SORT_TIE_BITS=16",        # default 64-bit tie-break suffix (tile_binning.glsl:342)
        "USE_QUANTIZED_GAUSSIANS=1",  # quantized atlas buffer layout (tile_binning.glsl:89)
        "GS_PACKED_STAGE_DATA=1",     # packed projection payload (tile_projection_common.glsl:13)
        "GS_SH_AMORTIZATION=1",       # SH color cache binding (tile_binning.glsl:283)
        "GS_ENABLE_SUBGROUPS=1",      # subgroup extensions (platform_compat.glsl:18)
    ),
    "modules/gaussian_splatting/shaders/tile_prefix_scan.glsl": (
        "GS_ENABLE_SUBGROUPS=1",      # subgroup extensions via _build_prefix_defines (platform_compat.glsl:18)
    ),
    "modules/gaussian_splatting/shaders/tile_rasterizer.glsl": (
        "GS_PACKED_STAGE_DATA=1",
        "GS_ENABLE_SUBGROUPS=1",
    ),
    "modules/gaussian_splatting/shaders/tile_rasterizer_compute.glsl": (
        "GS_PACKED_STAGE_DATA=1",
        "GS_ENABLE_SUBGROUPS=1",
    ),
}


ABI_CONTRACTS: tuple[ValidationContract, ...] = (
    ValidationContract(
        key="tile_raster_indirect_count_clamp",
        issue_id=ISSUE_RASTER_BOUNDS,
        description="Tile rasterizers clamp GPU indirect overlap counts to the bound sorted-value buffer capacity.",
        files=(
            FilePatternSet(
                path=SHADERS_DIR / "includes" / "tile_raster_common.glsl",
                patterns=(
                    r"uint gs_get_sorted_value_capacity\(\)",
                    r"sorted_values\.values\.length\(\)",
                    r"uint gs_get_clamped_overlap_record_count\(\)",
                    r"min\(indirect_dispatch\.element_count, gs_get_sorted_value_capacity\(\)\)",
                ),
            ),
            FilePatternSet(
                path=SHADERS_DIR / "tile_rasterizer_compute.glsl",
                patterns=(
                    r"uint record_count = gs_get_clamped_overlap_record_count\(\);",
                ),
            ),
            FilePatternSet(
                path=SHADERS_DIR / "tile_rasterizer.glsl",
                patterns=(
                    r"uint record_count = gs_get_clamped_overlap_record_count\(\);",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="indirect_dispatch_layout",
        issue_id=ISSUE_ABI,
        description="Indirect dispatch ABI is aligned across host and shaders.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "renderer" / "pipeline_io_contracts.h",
                patterns=(
                    r"struct IndirectDispatchLayout",
                    r"static_assert\(offsetof\(IndirectDispatchLayout, element_count\) == 12",
                    r"static_assert\(sizeof\(IndirectDispatchLayout\) == sizeof\(uint32_t\) \* 6",
                ),
            ),
            FilePatternSet(
                path=SHADERS_DIR / "tile_prefix_scan.glsl",
                patterns=(
                    r"layout\(set = 0, binding = 5, std430\) buffer IndirectDispatch",
                    r"uint dispatch_xyz\[3\];",
                    r"uint element_count;",
                    r"uint overflow_flag;",
                    r"uint unclamped_total;",
                ),
            ),
            FilePatternSet(
                path=COMPUTE_DIR / "instance_count_clamp.glsl",
                patterns=(
                    r"layout\(set = 0, binding = 1, std430\) buffer IndirectDispatch",
                    r"uint dispatch_xyz\[3\];",
                    r"uint element_count;",
                    r"uint overflow_flag;",
                    r"uint unclamped_total;",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="resolve_push_constants",
        issue_id=ISSUE_ABI,
        description="Resolve push-constant ABI is pinned between host and shader.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "renderer" / "tile_render_stages.h",
                patterns=(
                    r"struct ResolvePushConstants",
                    r"static_assert\(sizeof\(ResolvePushConstants\) == 48",
                ),
            ),
            FilePatternSet(
                path=SHADERS_DIR / "tile_resolve.glsl",
                patterns=(
                    r"layout\(push_constant, std430\) uniform ResolveParams",
                    r"int viewport_width;",
                    r"int output_is_premultiplied;",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="tile_projection_payload",
        issue_id=ISSUE_ABI,
        description="Tile projection payload ABI remains explicitly asserted.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "renderer" / "tile_render_types.h",
                patterns=(
                    r"struct TileProjectionLayout",
                    r"static_assert\(sizeof\(Payload\) == 36",
                    r"static_assert\(sizeof\(PackedPayload\) == 32",
                ),
            ),
            FilePatternSet(
                path=SHADERS_DIR / "includes" / "tile_projection_common.glsl",
                patterns=(
                    r"struct ProjectedGaussian",
                    r"uint data\[9\];",
                    r"uint data\[8\];",
                ),
            ),
        ),
    ),
)

COUNTER_INIT_CONTRACTS: tuple[ValidationContract, ...] = (
    ValidationContract(
        key="gpu_culler_primary_counter_reset",
        issue_id=ISSUE_COUNTER_INIT,
        description="Primary frustum cull path performs explicit counter reset before dispatch.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "interfaces" / "gpu_culler.cpp",
                patterns=(
                    r"Reset counters with an explicit host write so zero-visibility frames are",
                    r"buffer_update\(counter_buffer, 0, sizeof\(zero_counters\), &zero_counters\);",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="gpu_culler_instance_counter_reset",
        issue_id=ISSUE_COUNTER_INIT,
        description="Instance cull path resets counters explicitly at the dispatch boundary.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "interfaces" / "gpu_culler.cpp",
                patterns=(
                    r"static const uint32_t zero_instance_counters\[3\] = \{ 0u, 0u, 0u \};",
                    r"buffer_update\(p_inputs\.counter_buffer, 0, sizeof\(zero_instance_counters\), zero_instance_counters\);",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="instance_chunk_dispatch_stage_reset",
        issue_id=ISSUE_COUNTER_INIT,
        description="Stage transition shader explicitly clears counters for subsequent dispatch.",
        files=(
            FilePatternSet(
                path=COMPUTE_DIR / "instance_chunk_dispatch.glsl",
                patterns=(
                    r"Clear counters for Stage B splat counting",
                    r"counters\.visible_chunk_count = 0u;",
                    r"counters\.overflowed_chunks = 0u;",
                ),
            ),
        ),
    ),
)

DIAGNOSTICS_CONTRACTS: tuple[ValidationContract, ...] = (
    ValidationContract(
        key="tile_binning_debug_macros",
        issue_id=ISSUE_DIAGNOSTICS,
        description="Tile binning debug instrumentation is behind explicit disable macros.",
        files=(
            FilePatternSet(
                path=SHADERS_DIR / "tile_binning.glsl",
                patterns=(
                    r"#ifdef GS_DEBUG_COUNTERS_DISABLED",
                    r"#define GS_DEBUG_INCREMENT\(counter\)",
                    r"#define GS_DEBUG_INCREMENT\(counter\) atomicAdd\(debug_counters\.counter, 1u\)",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="tile_raster_common_debug_guard",
        issue_id=ISSUE_DIAGNOSTICS,
        description="Per-splat raster diagnostics are guarded out for production variants.",
        files=(
            FilePatternSet(
                path=SHADERS_DIR / "includes" / "tile_raster_common.glsl",
                patterns=(
                    r"#ifndef GS_DEBUG_COUNTERS_DISABLED",
                    r"if \(any\(isnan\(screen_px\)\) \|\| any\(isinf\(screen_px\)\)",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="tile_renderer_production_define",
        issue_id=ISSUE_DIAGNOSTICS,
        description="Tile renderer only enables diagnostics atomics when explicitly requested.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "renderer" / "tile_renderer.cpp",
                patterns=(
                    r"if \(!diagnostics\.debug_binning_counters_enabled\) \{",
                    r"#define GS_DEBUG_COUNTERS_DISABLED 1",
                ),
            ),
        ),
    ),
    ValidationContract(
        key="gpu_culler_production_define",
        issue_id=ISSUE_DIAGNOSTICS,
        description="GPU culler variant wiring keeps diagnostics defines explicit.",
        files=(
            FilePatternSet(
                path=MODULE_DIR / "interfaces" / "gpu_culler.cpp",
                patterns=(
                    r"String debug_counter_define = debug_counters_enabled \? \"\" : \"#define GS_DEBUG_COUNTERS_DISABLED 1\\n\";",
                ),
            ),
        ),
    ),
)


def _extract_stage_sources(source_text: str) -> dict[str, str]:
    stage_sources: dict[str, str] = {}
    current_stage: str | None = None
    current_lines: list[str] = []
    prefix_lines: list[str] = []
    saw_stage_tag = False

    for line in source_text.splitlines(keepends=True):
        if VERSION_DEFINES_RE.match(line):
            continue

        tag_match = SECTION_TAG_RE.match(line)
        if tag_match:
            saw_stage_tag = True
            if current_stage is not None:
                stage_sources[current_stage] = "".join(current_lines)
            current_stage = tag_match.group(1)
            current_lines = []
            continue

        if current_stage is None:
            prefix_lines.append(line)
        else:
            current_lines.append(line)

    if current_stage is not None:
        stage_sources[current_stage] = "".join(current_lines)

    if not saw_stage_tag:
        cleaned = VERSION_DEFINES_RE.sub("", source_text)
        return {"__single__": cleaned}

    prefix_text = "".join(prefix_lines)
    if prefix_text.strip():
        for stage in list(stage_sources.keys()):
            stage_sources[stage] = prefix_text + stage_sources[stage]

    return stage_sources


def _inject_include_directive_extension(source_text: str) -> str:
    if "#include" not in source_text:
        return source_text
    if INCLUDE_EXTENSION_RE.search(source_text):
        return source_text

    version_match = VERSION_LINE_RE.search(source_text)
    if version_match is None:
        return source_text

    insert_pos = version_match.end()
    extension_line = "#extension GL_GOOGLE_include_directive : require\n"
    return source_text[:insert_pos] + extension_line + source_text[insert_pos:]


def _find_shader_compiler(preference: str) -> CompilerTool | None:
    if preference in ("auto", "glslc"):
        glslc = shutil.which("glslc")
        if glslc:
            return CompilerTool(kind="glslc", path=glslc)
    if preference in ("auto", "glslangValidator"):
        validator = shutil.which("glslangValidator")
        if validator:
            return CompilerTool(kind="glslangValidator", path=validator)
    return None


def _variant_target_env(defines: tuple[str, ...]) -> str | None:
    """Minimum compiler target env required by a variant's runtime-selectable defines.

    Subgroup ops (subgroupAdd / subgroupBallot / ... in tile_raster_common.glsl)
    require SPIR-V 1.3, which maps to the Vulkan 1.1 target environment. At runtime
    these permutations are compiled only when the device advertises subgroup support
    (a Vulkan 1.1+ capability, gated by TileRenderer::_detect_subgroup_support), so the
    offline matrix compiles the GS_ENABLE_SUBGROUPS variants against the same minimum
    environment. Non-subgroup variants return None and keep the default target env, so
    their compile command is unchanged.
    """
    for define in defines:
        if define.split("=", 1)[0] == "GS_ENABLE_SUBGROUPS":
            return "vulkan1.1"
    return None


def _compiler_command(
    tool: CompilerTool,
    stage: str,
    input_path: Path,
    output_path: Path,
    defines: tuple[str, ...],
    include_dirs: tuple[Path, ...],
    target_env: str | None = None,
) -> list[str]:
    if tool.kind == "glslc":
        cmd = [tool.path, "-O", f"-fshader-stage={stage}"]
        if target_env:
            cmd.append(f"--target-env={target_env}")
        for include_dir in include_dirs:
            cmd.extend(["-I", str(include_dir)])
        for define in defines:
            cmd.append(f"-D{define}")
        cmd.extend([str(input_path), "-o", str(output_path)])
        return cmd

    if tool.kind == "glslangValidator":
        stage_map = {
            "compute": "comp",
            "vertex": "vert",
            "fragment": "frag",
        }
        glslang_stage = stage_map.get(stage)
        if glslang_stage is None:
            raise ValueError(f"Unsupported shader stage '{stage}' for glslangValidator")
        cmd = [tool.path, "-V", "-S", glslang_stage, "-o", str(output_path)]
        if target_env:
            cmd.extend(["--target-env", target_env])
        for include_dir in include_dirs:
            cmd.append(f"-I{include_dir}")
        for define in defines:
            cmd.append(f"-D{define}")
        cmd.append(str(input_path))
        return cmd

    raise ValueError(f"Unsupported compiler tool '{tool.kind}'")


def _run_contract_set(name: str, contracts: tuple[ValidationContract, ...]) -> tuple[bool, list[dict[str, object]]]:
    print(f"[contracts] {name} ({len(contracts)} checks)")
    all_ok = True
    results: list[dict[str, object]] = []

    for contract in contracts:
        contract_ok = True
        failures: list[str] = []

        for file_set in contract.files:
            if not file_set.path.exists():
                contract_ok = False
                failures.append(f"Missing file: {file_set.path}")
                continue

            text = file_set.path.read_text(encoding="utf-8")
            for pattern in file_set.patterns:
                if re.search(pattern, text, re.MULTILINE) is None:
                    contract_ok = False
                    failures.append(f"{file_set.path}: missing pattern `{pattern}`")

        status = "PASS" if contract_ok else "FAIL"
        print(f"  [{status}] {contract.key} ({contract.issue_id}) - {contract.description}")
        if failures:
            for failure in failures:
                print(f"    - {failure}")

        all_ok = all_ok and contract_ok
        results.append(
            {
                "key": contract.key,
                "issue_id": contract.issue_id,
                "description": contract.description,
                "ok": contract_ok,
                "failures": failures,
            }
        )

    return all_ok, results


def _discover_runtime_entrypoints() -> set[Path]:
    entrypoints: set[Path] = set()
    valid_suffixes = {".h", ".hpp", ".cpp", ".cc", ".cxx", ".inc"}

    for source_file in MODULE_DIR.rglob("*"):
        if not source_file.is_file() or source_file.suffix.lower() not in valid_suffixes:
            continue
        try:
            text = source_file.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue

        for match in INCLUDE_GEN_RE.finditer(text):
            shader_dir = match.group("dir")
            shader_name = match.group("name")
            entrypoints.add(Path("modules") / "gaussian_splatting" / shader_dir / f"{shader_name}.glsl")

    return entrypoints


def _validate_runtime_matrix_coverage() -> tuple[bool, dict[str, object]]:
    runtime_entrypoints = _discover_runtime_entrypoints()
    matrix_sources = {entry.source.relative_to(REPO_ROOT) for entry in RUNTIME_SHADER_MATRIX}

    missing = sorted(str(path) for path in runtime_entrypoints - matrix_sources)
    extra = sorted(str(path) for path in matrix_sources - runtime_entrypoints)
    ok = len(missing) == 0

    print(f"[matrix] Runtime entrypoints discovered: {len(runtime_entrypoints)}")
    print(f"[matrix] Matrix sources configured: {len(matrix_sources)}")

    if missing:
        print(f"[matrix][FAIL] Missing runtime entrypoints in matrix ({ISSUE_RUNTIME_MATRIX}):")
        for path in missing:
            print(f"  - {path}")
    else:
        print(f"[matrix][PASS] Matrix covers all discovered runtime entrypoints ({ISSUE_RUNTIME_MATRIX}).")

    if extra:
        print("[matrix][WARN] Matrix includes non-runtime sources (kept intentionally if needed):")
        for path in extra:
            print(f"  - {path}")

    return ok, {
        "ok": ok,
        "runtime_entrypoints": sorted(str(path) for path in runtime_entrypoints),
        "matrix_sources": sorted(str(path) for path in matrix_sources),
        "missing": missing,
        "extra": extra,
    }


def _validate_required_variant_defines() -> tuple[bool, dict[str, object]]:
    """G4: every runtime-selectable branch must be exercised by >=1 matrix variant.

    For each source in REQUIRED_VARIANT_DEFINES, union the define tokens across all of
    that source's variants and confirm every required token is present. Fails (returned
    ok=False -> non-zero process exit via checks_ok) if any required branch has no
    compiling variant. This makes the exit criterion self-enforcing rather than relying
    on a reviewer to notice a newly added #ifdef. ASCII-only output.
    """
    entry_by_source = {
        entry.source.relative_to(REPO_ROOT).as_posix(): entry for entry in RUNTIME_SHADER_MATRIX
    }

    missing: list[str] = []
    checked = 0
    for source, required_defines in sorted(REQUIRED_VARIANT_DEFINES.items()):
        entry = entry_by_source.get(source)
        if entry is None:
            missing.append(f"{source}: source not present in RUNTIME_SHADER_MATRIX")
            continue

        exercised: set[str] = set()
        for variant in entry.variants:
            exercised.update(variant.defines)

        for required in required_defines:
            checked += 1
            if required not in exercised:
                missing.append(f"{source}: no variant exercises `{required}`")

    ok = len(missing) == 0

    print(f"[matrix] Required runtime-branch define tokens checked: {checked}")
    if missing:
        print(f"[matrix][FAIL] Runtime-selectable branches without a compile variant ({ISSUE_RUNTIME_MATRIX}):")
        for item in missing:
            print(f"  - {item}")
    else:
        print(f"[matrix][PASS] Every required runtime-selectable branch has a compile variant ({ISSUE_RUNTIME_MATRIX}).")

    return ok, {
        "ok": ok,
        "checked": checked,
        "missing": missing,
        "required": {source: list(defines) for source, defines in REQUIRED_VARIANT_DEFINES.items()},
    }


def _print_matrix() -> None:
    print(f"[matrix] Explicit runtime shader matrix ({len(RUNTIME_SHADER_MATRIX)} entries)")
    for entry in RUNTIME_SHADER_MATRIX:
        source_rel = entry.source.relative_to(REPO_ROOT)
        issue_str = ",".join(entry.issue_ids)
        print(
            f"  - {entry.key}: {source_rel} stages={','.join(entry.stages)} "
            f"variants={len(entry.variants)} issues={issue_str}"
        )


def _compile_entry(
    entry: ShaderMatrixEntry,
    tool: CompilerTool,
    output_dir: Path,
    include_dirs: tuple[Path, ...],
) -> tuple[bool, list[dict[str, object]]]:
    source_text = entry.source.read_text(encoding="utf-8")
    stage_sources = _extract_stage_sources(source_text)

    compile_results: list[dict[str, object]] = []
    entry_ok = True

    for stage in entry.stages:
        if stage in stage_sources:
            stage_source = stage_sources[stage]
        elif "__single__" in stage_sources and len(entry.stages) == 1:
            stage_source = stage_sources["__single__"]
        else:
            entry_ok = False
            compile_results.append(
                {
                    "entry": entry.key,
                    "source": str(entry.source.relative_to(REPO_ROOT)),
                    "stage": stage,
                    "variant": "<missing_stage>",
                    "ok": False,
                    "error": f"Stage '{stage}' not found in source.",
                }
            )
            continue

        stage_source = _inject_include_directive_extension(stage_source)

        temp_file: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                suffix=f".{stage}.stage.glsl",
                dir=str(entry.source.parent),
                delete=False,
            ) as temp_handle:
                temp_handle.write(stage_source)
                temp_file = Path(temp_handle.name)

            for variant in entry.variants:
                output_file = output_dir / f"{entry.key}.{variant.name}.{stage}.spv"
                target_env = _variant_target_env(variant.defines)
                cmd = _compiler_command(tool, stage, temp_file, output_file, variant.defines, include_dirs, target_env)
                proc = subprocess.run(cmd, capture_output=True, text=True)
                ok = proc.returncode == 0
                entry_ok = entry_ok and ok

                result = {
                    "entry": entry.key,
                    "source": str(entry.source.relative_to(REPO_ROOT)),
                    "stage": stage,
                    "variant": variant.name,
                    "issues": list(variant.issue_ids or entry.issue_ids),
                    "ok": ok,
                    "command": cmd,
                    "output_file": str(output_file),
                }

                if not ok:
                    result["stderr"] = proc.stderr.strip()
                    result["stdout"] = proc.stdout.strip()
                    print(
                        f"[compile][FAIL] {entry.key}:{stage}:{variant.name} "
                        f"issues={','.join(variant.issue_ids or entry.issue_ids)}"
                    )
                    if proc.stderr.strip():
                        print(proc.stderr.strip())
                    elif proc.stdout.strip():
                        print(proc.stdout.strip())
                else:
                    print(
                        f"[compile][PASS] {entry.key}:{stage}:{variant.name} "
                        f"issues={','.join(variant.issue_ids or entry.issue_ids)}"
                    )

                compile_results.append(result)
        finally:
            if temp_file is not None and temp_file.exists():
                temp_file.unlink()

    return entry_ok, compile_results


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile and validate Gaussian Splatting runtime shader matrix.")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR,
                        help="Output directory for generated SPIR-V binaries.")
    parser.add_argument("--summary-json", type=Path, default=None,
                        help="Optional path to write a JSON summary report.")
    parser.add_argument("--compiler", choices=("auto", "glslc", "glslangValidator"), default="auto",
                        help="Shader compiler preference.")
    parser.add_argument("--clean-output", action="store_true",
                        help="Delete output directory before compiling.")
    parser.add_argument("--list-matrix", action="store_true",
                        help="Print runtime matrix entries before validation.")
    parser.add_argument("--contracts-only", action="store_true",
                        help="Run coverage + contract checks only; skip shader compilation.")
    parser.add_argument("--skip-compile", action="store_true",
                        help="Skip shader compilation after contract checks.")
    args = parser.parse_args()

    summary: dict[str, object] = {
        "issues": sorted({
            ISSUE_RUNTIME_MATRIX,
            ISSUE_ABI,
            ISSUE_COUNTER_INIT,
            ISSUE_DIAGNOSTICS,
        }),
        "matrix": [
            {
                "key": entry.key,
                "source": str(entry.source.relative_to(REPO_ROOT)),
                "stages": list(entry.stages),
                "variants": [
                    {
                        "name": variant.name,
                        "defines": list(variant.defines),
                        "issues": list(variant.issue_ids or entry.issue_ids),
                    }
                    for variant in entry.variants
                ],
                "issues": list(entry.issue_ids),
            }
            for entry in RUNTIME_SHADER_MATRIX
        ],
    }

    if args.list_matrix:
        _print_matrix()

    matrix_ok, matrix_summary = _validate_runtime_matrix_coverage()
    summary["matrix_coverage"] = matrix_summary

    required_defines_ok, required_defines_summary = _validate_required_variant_defines()
    summary["required_variant_defines"] = required_defines_summary

    abi_ok, abi_results = _run_contract_set("ABI", ABI_CONTRACTS)
    summary["abi_contracts"] = abi_results

    counter_ok, counter_results = _run_contract_set("CounterInit", COUNTER_INIT_CONTRACTS)
    summary["counter_init_contracts"] = counter_results

    diagnostics_ok, diagnostics_results = _run_contract_set("Diagnostics", DIAGNOSTICS_CONTRACTS)
    summary["diagnostics_contracts"] = diagnostics_results

    checks_ok = matrix_ok and required_defines_ok and abi_ok and counter_ok and diagnostics_ok

    compile_enabled = not args.contracts_only and not args.skip_compile
    compile_results: list[dict[str, object]] = []
    compile_ok = True
    compiler_info: dict[str, object] = {"enabled": compile_enabled}

    if compile_enabled:
        tool = _find_shader_compiler(args.compiler)
        if tool is None:
            compile_ok = False
            compiler_info.update(
                {
                    "ok": False,
                    "error": "No shader compiler found. Install glslc or glslangValidator.",
                    "preference": args.compiler,
                }
            )
            print("[compile][FAIL] No shader compiler found (glslc/glslangValidator).")
        else:
            compiler_info.update({"ok": True, "kind": tool.kind, "path": tool.path})
            print(f"[compile] Using {tool.kind}: {tool.path}")

            if args.clean_output and args.output_dir.exists():
                shutil.rmtree(args.output_dir)
            args.output_dir.mkdir(parents=True, exist_ok=True)

            include_dirs = (SHADERS_DIR, SHADERS_DIR / "includes", COMPUTE_DIR)
            for entry in RUNTIME_SHADER_MATRIX:
                entry_ok, entry_results = _compile_entry(entry, tool, args.output_dir, include_dirs)
                compile_ok = compile_ok and entry_ok
                compile_results.extend(entry_results)

    summary["compiler"] = compiler_info
    summary["compile_results"] = compile_results
    summary["compile_success"] = compile_ok
    summary["checks_success"] = checks_ok

    if args.summary_json is not None:
        args.summary_json.parent.mkdir(parents=True, exist_ok=True)
        args.summary_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"[summary] Wrote {args.summary_json}")

    compile_attempts = sum(1 for result in compile_results if result.get("variant") != "<missing_stage>")
    compile_failures = sum(1 for result in compile_results if not result.get("ok"))
    print(
        "[result] "
        f"contracts={'PASS' if checks_ok else 'FAIL'} "
        f"compile={'PASS' if compile_ok else 'FAIL'} "
        f"attempts={compile_attempts} failures={compile_failures}"
    )

    return 0 if (checks_ok and compile_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
