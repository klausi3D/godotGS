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
ISSUE_SORTER_MATRIX = "#525"

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
            # The runtime compiles every prefix pass at GS_PREFIX_LOCAL_SIZE=256
            # (GaussianSplatting::kTilePrefixPassLocalSize, renderer/tile_prefix_scan_utils.h:14;
            # ShaderCompilationManager::compile_prefix_shaders() passes that constant for
            # passes 1, 2 and 3). There is no runtime path that uses a 128-thread prefix
            # workgroup, so every variant below compiles at the true 256-thread shape.
            Variant(
                "pass1",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_1=1", "GS_PREFIX_LOCAL_SIZE=256")),
                ("#1318",),
            ),
            Variant(
                "pass2",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_2=1", "GS_PREFIX_LOCAL_SIZE=256")),
                ("#1318",),
            ),
            Variant(
                "pass3",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_3=1", "GS_PREFIX_LOCAL_SIZE=256")),
                ("#1318",),
            ),
            # GS_ENABLE_SUBGROUPS reaches every prefix-scan pass at runtime:
            # ShaderCompilationManager::_build_prefix_defines() derives from
            # TileRenderer::_build_common_shader_defines(true), and tile_prefix_scan.glsl
            # includes platform_compat.glsl where the define switches the subgroup
            # extensions to `require`. Compile the subgroup permutation of all three
            # passes (at the same runtime 256-thread size) so the runtime subgroup path
            # never ships uncompiled. _variant_target_env() lifts these to vulkan1.1.
            Variant(
                "pass1_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_1=1", "GS_PREFIX_LOCAL_SIZE=256", "GS_ENABLE_SUBGROUPS=1")),
                ("#1318",),
            ),
            Variant(
                "pass2_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_2=1", "GS_PREFIX_LOCAL_SIZE=256", "GS_ENABLE_SUBGROUPS=1")),
                ("#1318",),
            ),
            Variant(
                "pass3_subgroups",
                _merge_defines(TILE_GLOBAL_SORT_DEFINES, ("GS_TILE_PREFIX_PASS_3=1", "GS_PREFIX_LOCAL_SIZE=256", "GS_ENABLE_SUBGROUPS=1")),
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


# ---------------------------------------------------------------------------
# Embedded (runtime-string) sorter shader coverage -- issue #525
# ---------------------------------------------------------------------------
# The GPU sort path builds its compute shaders as runtime `vformat()` strings, not
# file-based `.glsl` with `.glsl.gen.h` includes, so `_discover_runtime_entrypoints`
# never sees them and the C5 permutation matrix above never compiles them. A GLSL
# syntax break in any sorter permutation (32/64-bit key, workgroup {64,128,256,512},
# 4/8-bit radix, subgroups on/off) is therefore caught by NOTHING until the shader
# first compiles on an end-user GPU across the driver matrix.
#
# This section closes that gap WITHOUT duplicating the GLSL: it extracts the actual
# raw-string shader templates straight out of the C++ translation units (the single
# source of truth) and reproduces the exact `vformat()` substitution the runtime
# performs, then feeds the assembled permutations through the SAME offline compiler
# (`_compiler_command`) the file-based matrix uses. Because the GLSL bodies are read
# from the live C++ sources, a syntax error introduced into a sorter shader string in
# gpu_sorter.cpp / gpu_sorting_pipeline.cpp fails this check -- exactly the runtime
# family the file matrix could not reach. Do not hand-copy the GLSL here; that would
# re-introduce the drift this check exists to prevent.
#
# Permutation axes mirror the runtime clamps:
#   * key_bits       -> GPUSortingConfig::validate() accepts {32, 64}
#                       (use_64bit_keys = key_bits > 32 -> uvec2 vs uint key type).
#   * workgroup_size -> validate() accepts {64, 128, 256, 512}.
#   * radix_bits     -> GPUSortingConstants::is_supported_radix_bits() accepts {4, 8}.
#   * subgroups      -> RadixSort selects the subgroup ballot path when the device
#                       advertises support; both arms must compile.
# Bitonic, indirect-dispatch, remap, gather and the OneSweep passes are compiled at
# their fixed runtime shape. Every scalar these fixed shaders substitute is PARSED
# from the C++ header that defines it (below) rather than pinned here, so a constant
# change re-shapes the compiled permutation instead of silently drifting.

GPU_SORTER_CPP = MODULE_DIR / "renderer" / "gpu_sorter.cpp"
GPU_SORTING_PIPELINE_CPP = MODULE_DIR / "interfaces" / "gpu_sorting_pipeline.cpp"
# Headers whose constants are substituted into the embedded sorter shaders. Parsed at
# runtime so the offline matrix always compiles the true shape; each is mirrored in
# the gaussian_shader_validation.yml `paths:` trigger so a constant change re-runs the
# gate.
GPU_SORTER_H = MODULE_DIR / "renderer" / "gpu_sorter.h"
GPU_SORTING_CONSTANTS_H = MODULE_DIR / "renderer" / "gpu_sorting_constants.h"
SORTING_CONTRACT_H = MODULE_DIR / "renderer" / "sorting_contract.h"
# GPUSortingConfig::validate() is the single source of truth for which radix / workgroup
# / key-width values the runtime ACCEPTS. The coverage self-check parses those
# acceptance sets from here (rather than a hand-copied tuple), so if the runtime later
# accepts a new value, a compiled permutation for it is REQUIRED before the gate passes.
GPU_SORTING_CONFIG_CPP = MODULE_DIR / "renderer" / "gpu_sorting_config.cpp"

# Both subgroup arms are reachable at runtime (RadixSort compiles the ballot path when
# the device advertises support, the scalar path otherwise); both must compile. This is
# an inherent boolean axis, not a config-tunable set, so it is declared here.
SORTER_SUBGROUP_STATES = (False, True)

_RAW_STRING_RE = re.compile(r'R"\((.*?)\)"', re.DOTALL)
_CSTRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_VFORMAT_TOKEN_RE = re.compile(r"%[ds]")

# Shader families the sorter coverage MUST compile. Missing any of these (e.g. an
# anchor stopped matching because a shader was renamed or extracted to a file) fails
# the coverage self-check with a non-zero exit rather than silently dropping the
# family from CI. Fixed-shape (single-permutation) families are marked False.
SORTER_REQUIRED_FAMILIES: dict[str, bool] = {
    "radix_histogram": True,
    "radix_wg_prefix": True,
    "radix_bin_prefix": True,
    "radix_scatter": True,
    "bitonic": False,
    "radix_indirect_dispatch": False,
    "onesweep_global_histogram": False,
    "onesweep_digit_binning": False,
    "onesweep_chained_scan": False,
    "onesweep_scatter": False,
    "remap": False,
    "gather": False,
}


@dataclass(frozen=True)
class SorterPermutation:
    key: str
    variant: str
    source: str
    target_env: str | None
    issue_ids: tuple[str, ...]
    axes: dict[str, object]


class _SorterExtractionError(Exception):
    """Raised when a sorter shader template can no longer be located/assembled."""


def _unescape_c_literal(text: str) -> str:
    # Minimal C string-literal unescape for the concatenated remap source. The
    # sorter shaders only use \n (plus the structural \\ / \" cases handled here).
    return (
        text.replace("\\\\", "\x00")
        .replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace('\\"', '"')
        .replace("\x00", "\\")
    )


def _apply_vformat(template: str, *args: object) -> str:
    """Reproduce Godot vformat() for the %d/%s placeholders the sorter sources use.

    Substitutes positionally and asserts the placeholder count matches the argument
    count, so a template whose placeholder shape changed (a real ABI/source drift)
    fails loudly here instead of emitting a malformed shader.
    """
    parts = _VFORMAT_TOKEN_RE.split(template)
    tokens = _VFORMAT_TOKEN_RE.findall(template)
    if len(tokens) != len(args):
        raise _SorterExtractionError(
            f"vformat placeholder/argument mismatch: template has {len(tokens)} "
            f"placeholders, received {len(args)} arguments"
        )
    out: list[str] = []
    for index, chunk in enumerate(parts):
        out.append(chunk)
        if index < len(args):
            out.append(str(args[index]))
    return "".join(out)


def _extract_raw_string_containing(text: str, anchor: str) -> str:
    for match in _RAW_STRING_RE.finditer(text):
        body = match.group(1)
        if anchor in body:
            return body
    raise _SorterExtractionError(f"no R\"(...)\" raw string contains anchor: {anchor!r}")


def _extract_cstring_template(text: str, func_name: str, arg_marker: str) -> str:
    func_pos = text.find(func_name)
    if func_pos == -1:
        raise _SorterExtractionError(f"function not found: {func_name}")
    vformat_pos = text.find("vformat(", func_pos)
    if vformat_pos == -1:
        raise _SorterExtractionError(f"no vformat() call in {func_name}")
    arg_pos = text.find(arg_marker, vformat_pos)
    if arg_pos == -1:
        raise _SorterExtractionError(f"argument marker {arg_marker!r} not found in {func_name}")
    segment = text[vformat_pos:arg_pos]
    literals = _CSTRING_LITERAL_RE.findall(segment)
    if not literals:
        raise _SorterExtractionError(f"no string literals in {func_name}")
    return "".join(_unescape_c_literal(literal) for literal in literals)


def _extract_adjacent_cstrings(text: str, anchor: str) -> str:
    """Extract a C++ adjacent-string-literal concatenation containing `anchor`.

    Finds the double-quoted literal whose content holds `anchor`, then greedily
    absorbs the run of neighbouring literals separated from it by whitespace only --
    exactly the C/C++ adjacent-literal concatenation rule. Used to source the small
    interpolated GLSL fragments (subgroup preamble, per-key read snippets) from
    RadixSort::create_variant() in gpu_sorter.cpp, so a future edit to those fragments
    fails the compile check instead of matching a stale Python copy.
    """
    literals = list(_CSTRING_LITERAL_RE.finditer(text))
    hit = next((i for i, m in enumerate(literals) if anchor in m.group(1)), None)
    if hit is None:
        raise _SorterExtractionError(f"no string literal contains anchor: {anchor!r}")

    start = hit
    while start > 0 and text[literals[start - 1].end():literals[start].start()].strip() == "":
        start -= 1
    end = hit
    while end + 1 < len(literals) and text[literals[end].end():literals[end + 1].start()].strip() == "":
        end += 1

    return "".join(_unescape_c_literal(literals[k].group(1)) for k in range(start, end + 1))


def _extract_ternary_string_arms(text: str, decl_anchor: str) -> tuple[str, str]:
    """Return (true_arm, false_arm) string literals of a `cond ? "a" : "b"` decl.

    Locates the statement beginning at `decl_anchor` (e.g. the `String key_type = ...`
    declaration) up to its terminating `;`, and returns its two string literals in
    source order. Keeps the small type-keyword fragments (uvec2 / uint) sourced from
    gpu_sorter.cpp rather than pinned in Python.
    """
    pos = text.find(decl_anchor)
    if pos == -1:
        raise _SorterExtractionError(f"declaration not found: {decl_anchor!r}")
    end = text.find(";", pos)
    if end == -1:
        raise _SorterExtractionError(f"unterminated declaration: {decl_anchor!r}")
    literals = _CSTRING_LITERAL_RE.findall(text[pos:end])
    if len(literals) != 2:
        raise _SorterExtractionError(
            f"expected 2 string arms in {decl_anchor!r}, found {len(literals)}"
        )
    return _unescape_c_literal(literals[0]), _unescape_c_literal(literals[1])


def _parse_uint_constant(path: Path, name: str) -> int:
    """Parse `... name = <int>;` from a C++ header (single source of truth).

    Word-boundary anchored so `RADIX_BITS` never matches `DEFAULT_RADIX_BITS`, and so
    a usage (`align_up(count, kSortWorkgroupSize)`) never matches the definition. Fails
    closed (raises) when the constant cannot be located.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise _SorterExtractionError(f"cannot read {path}: {exc}") from exc
    match = re.search(rf"\b{re.escape(name)}\b\s*=\s*(\d+)", text)
    if match is None:
        raise _SorterExtractionError(f"constant {name} not found in {path.name}")
    return int(match.group(1))


def _parse_accepted_values(path: Path, var_name: str) -> tuple[int, ...]:
    """Parse an `(var == A || var == B || ...)` acceptance OR-chain from C++ source.

    GPUSortingConfig::validate() is the single source of truth for the radix /
    workgroup / key-width values the runtime accepts; parsing the acceptance list
    (rather than hand-copying it) means a newly accepted value is picked up here and,
    if no permutation exercises it, fails the coverage check closed. Word-boundary
    anchored so the variable name never matches a longer identifier. Fails closed
    (raises) when the acceptance chain cannot be located.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise _SorterExtractionError(f"cannot read {path}: {exc}") from exc
    var = re.escape(var_name)
    chain = re.search(rf"\(\s*\b{var}\b\s*==\s*\d+(?:\s*\|\|\s*\b{var}\b\s*==\s*\d+)*\s*\)", text)
    if chain is None:
        raise _SorterExtractionError(f"no `({var_name} == ...)` acceptance chain in {path.name}")
    values = tuple(int(v) for v in re.findall(rf"\b{var}\b\s*==\s*(\d+)", chain.group(0)))
    if not values:
        raise _SorterExtractionError(f"empty acceptance set for {var_name} in {path.name}")
    return tuple(sorted(set(values)))


def _radix_variant_label(key_bits: int, radix_bits: int, workgroup: int, subgroups: bool) -> str:
    return f"key{key_bits}_r{radix_bits}_wg{workgroup}_{'sg' if subgroups else 'plain'}"


# Curated (non-Cartesian) radix permutation set. Six variants exercise every axis
# endpoint -- key_bits {32,64}, radix_bits {4,8}, workgroup {64,128,256,512},
# subgroups {off,on} -- without compiling the full 2*2*4*2 product on the CI lane.
SORTER_RADIX_PERMUTATIONS: tuple[tuple[int, int, int, bool], ...] = (
    (64, 4, 256, False),  # shipped default: 64-bit key, 4-bit radix, 256 threads
    (64, 4, 256, True),   # subgroup ballot path of the default
    (32, 4, 128, False),  # 32-bit key opt-in + interior workgroup size
    (64, 8, 64, False),   # 8-bit radix (256 bins) strided over the MIN workgroup
    (64, 8, 512, True),   # 8-bit radix + MAX workgroup + subgroups
    (32, 8, 64, True),    # 32-bit key + 8-bit radix + min workgroup + subgroups
)


def _target_env_for_source(source: str) -> str | None:
    # Subgroup ops (subgroupBallot/...) need SPIR-V 1.3 == Vulkan 1.1, matching the
    # file-matrix's _variant_target_env() and the runtime's subgroup capability gate.
    return "vulkan1.1" if "#define GS_ENABLE_SUBGROUPS 1" in source else None


def _build_sorter_permutations() -> tuple[list[SorterPermutation], list[str]]:
    """Assemble every embedded sorter permutation from the live C++ sources.

    Returns (permutations, errors). `errors` is non-empty when a template anchor no
    longer resolves or a vformat shape drifted; callers surface it as a fail-closed
    coverage error (checks) and a compile failure.
    """
    permutations: list[SorterPermutation] = []
    errors: list[str] = []

    try:
        sorter_text = GPU_SORTER_CPP.read_text(encoding="utf-8")
    except OSError as exc:
        return [], [f"cannot read {GPU_SORTER_CPP}: {exc}"]
    try:
        pipeline_text = GPU_SORTING_PIPELINE_CPP.read_text(encoding="utf-8")
    except OSError as exc:
        return [], [f"cannot read {GPU_SORTING_PIPELINE_CPP}: {exc}"]

    issues = (ISSUE_SORTER_MATRIX,)

    def add(key: str, variant: str, source: str, axes: dict[str, object]) -> None:
        permutations.append(
            SorterPermutation(
                key=key,
                variant=variant,
                source=source,
                target_env=_target_env_for_source(source),
                issue_ids=issues,
                axes=axes,
            )
        )

    # --- RadixSort family (renderer/gpu_sorter.cpp, RadixSort::create_variant) -----
    try:
        histogram_tpl = _extract_raw_string_containing(
            sorter_text,
            "uint base = params.histogram_offset + gl_WorkGroupID.x * params.workgroup_stride;",
        )
        wg_prefix_tpl = _extract_raw_string_containing(
            sorter_text, "bin_counts_buf.bin_counts[params.bin_offset + bin] = prefix;"
        )
        bin_prefix_tpl = _extract_raw_string_containing(
            sorter_text, "bin_prefix_buf.bin_prefix[idx] = prefix;"
        )
        scatter_tpl = _extract_raw_string_containing(sorter_text, "keys_out.keys[pos] = key;")
        key_helper_64 = _extract_raw_string_containing(
            sorter_text, "uint get_radix(uvec2 key, uint shift)"
        )
        key_helper_32 = _extract_raw_string_containing(
            sorter_text, "uint get_radix(uint key, uint shift)"
        )
        histogram_update = _extract_raw_string_containing(
            sorter_text, "atomicAdd(local_histogram[r], count);"
        )
        scatter_bin_update = _extract_raw_string_containing(
            sorter_text, "uint subgroup_words = (gl_SubgroupSize + 31u) / 32u;"
        )

        # Interpolated helper fragments -- sourced from the SAME C++ (create_variant)
        # rather than reproduced as Python literals, so an edit to any of them fails
        # this check instead of matching a stale copy (#525 coverage-drift fix).
        key_type_64, key_type_32 = _extract_ternary_string_arms(
            sorter_text, "String key_type = use_64bit_keys"
        )
        subgroup_on = _extract_adjacent_cstrings(sorter_text, "GL_KHR_shader_subgroup_basic : enable")
        subgroup_off = _extract_adjacent_cstrings(sorter_text, "#define GS_ENABLE_SUBGROUPS 0")
        hist_read_64 = _extract_adjacent_cstrings(sorter_text, "uvec2 key = keys_in.keys[idx];")
        hist_read_32 = _extract_adjacent_cstrings(sorter_text, "uint key = keys_in.keys[idx];")
        scatter_read_64 = _extract_adjacent_cstrings(sorter_text, "uvec2 key = uvec2(0u);")
        scatter_read_32 = _extract_adjacent_cstrings(sorter_text, "uint key = 0u;")

        for key_bits, radix_bits, workgroup, subgroups in SORTER_RADIX_PERMUTATIONS:
            use_64bit = key_bits > 32
            radix_size = 1 << radix_bits
            mask_words = (workgroup + 31) // 32
            key_type = key_type_64 if use_64bit else key_type_32
            key_helper = key_helper_64 if use_64bit else key_helper_32
            preamble = subgroup_on if subgroups else subgroup_off
            hist_read = hist_read_64 if use_64bit else hist_read_32
            scatter_read = scatter_read_64 if use_64bit else scatter_read_32
            label = _radix_variant_label(key_bits, radix_bits, workgroup, subgroups)
            axes = {
                "key_bits": key_bits,
                "radix_bits": radix_bits,
                "workgroup": workgroup,
                "subgroups": subgroups,
            }

            histogram_source = _apply_vformat(
                histogram_tpl,
                radix_bits,
                radix_size,
                workgroup,
                preamble,
                key_type,
                key_helper,
                hist_read,
                histogram_update,
            )
            add("radix_histogram", label, histogram_source, axes)

            wg_prefix_source = _apply_vformat(wg_prefix_tpl, radix_size, workgroup)
            add("radix_wg_prefix", label, wg_prefix_source, axes)

            bin_prefix_source = _apply_vformat(bin_prefix_tpl, radix_size)
            add("radix_bin_prefix", label, bin_prefix_source, axes)

            scatter_source = _apply_vformat(
                scatter_tpl,
                radix_bits,
                radix_size,
                workgroup,
                mask_words,
                preamble,
                key_type,
                key_type,
                key_helper,
                scatter_read,
                scatter_bin_update,
            )
            add("radix_scatter", label, scatter_source, axes)
    except _SorterExtractionError as exc:
        errors.append(f"radix family: {exc}")

    # --- BitonicSort (renderer/gpu_sorter.cpp, BitonicSort::initialize) ------------
    # WORKGROUP_SIZE = GPUSortingConstants::DEFAULT_WORKGROUP_SIZE, parsed from the
    # header; the sorter compiles only this shape (float keys, no key parametrization).
    try:
        bitonic_wg = _parse_uint_constant(GPU_SORTING_CONSTANTS_H, "DEFAULT_WORKGROUP_SIZE")
        bitonic_tpl = _extract_raw_string_containing(sorter_text, "Batcher bitonic sorting network")
        add("bitonic", f"wg{bitonic_wg}", _apply_vformat(bitonic_tpl, bitonic_wg), {"workgroup": bitonic_wg})
    except _SorterExtractionError as exc:
        errors.append(f"bitonic: {exc}")

    # --- RadixSort indirect-dispatch args shader (fully static, no placeholders) ---
    try:
        dispatch_tpl = _extract_raw_string_containing(
            sorter_text, "indirect_out.dispatch_xyz[0] = groups;"
        )
        add("radix_indirect_dispatch", "default", _apply_vformat(dispatch_tpl), {})
    except _SorterExtractionError as exc:
        errors.append(f"indirect_dispatch: {exc}")

    # --- OneSweepSort passes (renderer/gpu_sorter.cpp) -----------------------------
    # RADIX_BITS / WORKGROUP_SIZE (gpu_sorting_constants.h) and CHAINING_FACTOR
    # (gpu_sorter.h) are parsed from their headers; RADIX_SIZE = 1 << RADIX_BITS
    # mirrors the constexpr. A change to any of them re-shapes these compiles.
    try:
        os_radix_bits = _parse_uint_constant(GPU_SORTING_CONSTANTS_H, "RADIX_BITS")
        os_radix_size = 1 << os_radix_bits
        os_wg = _parse_uint_constant(GPU_SORTING_CONSTANTS_H, "DEFAULT_WORKGROUP_SIZE")
        os_chaining = _parse_uint_constant(GPU_SORTER_H, "CHAINING_FACTOR")
    except _SorterExtractionError as exc:
        errors.append(f"onesweep constants: {exc}")
        os_radix_bits = os_radix_size = os_wg = os_chaining = None

    if os_wg is not None:
        onesweep_specs = (
            (
                "onesweep_global_histogram",
                "atomicAdd(global_histogram.global_hist[tid], local_histogram[tid]);",
                (os_radix_bits, os_radix_size, os_wg),
            ),
            (
                "onesweep_digit_binning",
                "digit_histogram.digit_hist[wid * RADIX_SIZE + tid]",
                (os_radix_bits, os_radix_size, os_wg, os_chaining),
            ),
            ("onesweep_chained_scan", "shared uint scan_scratch[RADIX_SIZE];", (os_radix_size, os_wg)),
            (
                "onesweep_scatter",
                "uint output_pos = atomicAdd(local_offsets[digit], 1);",
                (os_radix_bits, os_radix_size, os_wg),
            ),
        )
        for key, anchor, fmt_args in onesweep_specs:
            try:
                template = _extract_raw_string_containing(sorter_text, anchor)
                add(key, "default", _apply_vformat(template, *fmt_args), {})
            except _SorterExtractionError as exc:
                errors.append(f"{key}: {exc}")

    # --- Remap + gather (interfaces/gpu_sorting_pipeline.cpp) -----------------------
    # kSortWorkgroupSize is parsed from renderer/sorting_contract.h (single source of
    # truth) instead of pinned, so a change to the constant re-shapes these compiles.
    try:
        sort_wg = _parse_uint_constant(SORTING_CONTRACT_H, "kSortWorkgroupSize")
    except _SorterExtractionError as exc:
        errors.append(f"kSortWorkgroupSize: {exc}")
        sort_wg = None

    if sort_wg is not None:
        try:
            remap_tpl = _extract_cstring_template(
                pipeline_text, "_get_remap_compute_source", "GaussianSplatting::kSortWorkgroupSize"
            )
            add("remap", "default", _apply_vformat(remap_tpl, sort_wg), {})
        except _SorterExtractionError as exc:
            errors.append(f"remap: {exc}")

        try:
            gather_tpl = _extract_raw_string_containing(
                pipeline_text, "position_buffer.positions[idx] = vec4(g.position, radius);"
            )
            add("gather", "default", _apply_vformat(gather_tpl, sort_wg), {})
        except _SorterExtractionError as exc:
            errors.append(f"gather: {exc}")

    return permutations, errors


def _validate_sorter_coverage() -> tuple[bool, dict[str, object]]:
    """#525: prove the embedded sorter shader family is fully assembled for CI.

    Fails closed when a template anchor no longer resolves (a family would silently
    leave CI), when a required family produced no permutation, or when ANY accepted
    value of a runtime permutation axis is unexercised. The accepted key-width / radix
    / workgroup sets are parsed from GPUSortingConfig::validate() (single source of
    truth), and the subgroup axis (both arms) is checked too -- so dropping a whole
    axis, or the runtime accepting a new value with no permutation, fails closed.
    Mirrors the fail-closed philosophy of the runtime-matrix coverage checks above.
    ASCII-only output. Do not weaken this to make a build pass.
    """
    permutations, errors = _build_sorter_permutations()

    families_present = {perm.key for perm in permutations}
    for family, is_parametrized in sorted(SORTER_REQUIRED_FAMILIES.items()):
        if family not in families_present:
            errors.append(f"required sorter family has no compiled permutation: {family}")
        elif is_parametrized:
            count = sum(1 for perm in permutations if perm.key == family)
            if count < 2:
                errors.append(
                    f"parametrized sorter family {family} has only {count} permutation(s); "
                    "expected multiple across the key/workgroup/radix axes"
                )

    # Every accepted value of every declared axis must be exercised by >=1 permutation.
    # radix_scatter carries one perm per radix permutation, so its axes are the matrix.
    radix_axes = [perm.axes for perm in permutations if perm.key == "radix_scatter"]
    accepted_sets: dict[str, list[int]] = {}
    for axis_key, config_var in (
        ("key_bits", "key_bits"),
        ("radix_bits", "radix_bits"),
        ("workgroup", "workgroup_size"),
    ):
        try:
            accepted = _parse_accepted_values(GPU_SORTING_CONFIG_CPP, config_var)
        except _SorterExtractionError as exc:
            errors.append(f"cannot parse accepted {config_var} set: {exc}")
            continue
        accepted_sets[config_var] = list(accepted)
        exercised = {axis.get(axis_key) for axis in radix_axes}
        for value in accepted:
            if value not in exercised:
                errors.append(
                    f"no radix permutation exercises {config_var}={value} (accepted by validate())"
                )
        for value in sorted(v for v in exercised if v is not None):
            if value not in accepted:
                errors.append(
                    f"radix permutation exercises {config_var}={value} not accepted by validate()"
                )

    exercised_subgroups = {axis.get("subgroups") for axis in radix_axes}
    for state in SORTER_SUBGROUP_STATES:
        if state not in exercised_subgroups:
            errors.append(f"no radix permutation exercises subgroups={state}")

    ok = len(errors) == 0
    print(f"[sorter] Embedded sorter permutations assembled: {len(permutations)}")
    if ok:
        print(f"[sorter][PASS] Sorter shader family covered for compilation ({ISSUE_SORTER_MATRIX}).")
    else:
        print(f"[sorter][FAIL] Sorter shader coverage incomplete ({ISSUE_SORTER_MATRIX}):")
        for item in errors:
            print(f"  - {item}")

    return ok, {
        "ok": ok,
        "issue_id": ISSUE_SORTER_MATRIX,
        "permutation_count": len(permutations),
        "families": sorted(families_present),
        "accepted_axis_sets": accepted_sets,
        "subgroup_states": [bool(state) for state in SORTER_SUBGROUP_STATES],
        "permutations": [
            {"key": perm.key, "variant": perm.variant, "axes": perm.axes}
            for perm in permutations
        ],
        "errors": errors,
    }


def _compile_sorter_permutations(
    tool: CompilerTool, output_dir: Path
) -> tuple[bool, list[dict[str, object]]]:
    """Compile every embedded sorter permutation through the shared compiler path."""
    permutations, errors = _build_sorter_permutations()
    results: list[dict[str, object]] = []
    all_ok = not errors

    for error in errors:
        print(f"[compile][FAIL] sorter:<assembly> {error}")
        results.append(
            {
                "entry": "sorter",
                "source": "modules/gaussian_splatting/renderer/gpu_sorter.cpp",
                "stage": "compute",
                "variant": "<assembly_error>",
                "issues": [ISSUE_SORTER_MATRIX],
                "ok": False,
                "error": error,
            }
        )

    for perm in permutations:
        output_file = output_dir / f"sorter.{perm.key}.{perm.variant}.compute.spv"
        temp_file: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                suffix=".sorter.compute.glsl",
                dir=str(output_dir),
                delete=False,
            ) as temp_handle:
                temp_handle.write(perm.source)
                temp_file = Path(temp_handle.name)

            cmd = _compiler_command(
                tool, "compute", temp_file, output_file, (), (), perm.target_env
            )
            proc = subprocess.run(cmd, capture_output=True, text=True)
            ok = proc.returncode == 0
            all_ok = all_ok and ok

            pipeline_family = perm.key in ("remap", "gather")
            source_rel = (
                "modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp"
                if pipeline_family
                else "modules/gaussian_splatting/renderer/gpu_sorter.cpp"
            )
            result: dict[str, object] = {
                "entry": f"sorter_{perm.key}",
                "source": source_rel,
                "stage": "compute",
                "variant": perm.variant,
                "issues": list(perm.issue_ids),
                "ok": ok,
                "command": cmd,
                "output_file": str(output_file),
            }
            if not ok:
                result["stderr"] = proc.stderr.strip()
                result["stdout"] = proc.stdout.strip()
                print(f"[compile][FAIL] sorter:{perm.key}:{perm.variant} issues={ISSUE_SORTER_MATRIX}")
                if proc.stderr.strip():
                    print(proc.stderr.strip())
                elif proc.stdout.strip():
                    print(proc.stdout.strip())
            else:
                print(f"[compile][PASS] sorter:{perm.key}:{perm.variant} issues={ISSUE_SORTER_MATRIX}")

            results.append(result)
        finally:
            if temp_file is not None and temp_file.exists():
                temp_file.unlink()

    return all_ok, results


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
            ISSUE_SORTER_MATRIX,
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

    sorter_coverage_ok, sorter_coverage_summary = _validate_sorter_coverage()
    summary["sorter_coverage"] = sorter_coverage_summary

    abi_ok, abi_results = _run_contract_set("ABI", ABI_CONTRACTS)
    summary["abi_contracts"] = abi_results

    counter_ok, counter_results = _run_contract_set("CounterInit", COUNTER_INIT_CONTRACTS)
    summary["counter_init_contracts"] = counter_results

    diagnostics_ok, diagnostics_results = _run_contract_set("Diagnostics", DIAGNOSTICS_CONTRACTS)
    summary["diagnostics_contracts"] = diagnostics_results

    checks_ok = (
        matrix_ok
        and required_defines_ok
        and sorter_coverage_ok
        and abi_ok
        and counter_ok
        and diagnostics_ok
    )

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

            # #525: embedded (runtime-string) sorter permutations, assembled from the
            # live C++ sources and compiled through the same compiler path above.
            sorter_ok, sorter_results = _compile_sorter_permutations(tool, args.output_dir)
            compile_ok = compile_ok and sorter_ok
            compile_results.extend(sorter_results)

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
