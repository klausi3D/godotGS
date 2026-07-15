#!/usr/bin/env python3
"""Validate PackedGaussian layout parity between host C++ and shader mirrors."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HOST_LAYOUT = ROOT / "modules" / "gaussian_splatting" / "renderer" / "gaussian_gpu_layout.h"
STREAMING_QUANTIZATION_H = ROOT / "modules" / "gaussian_splatting" / "core" / "streaming_quantization.h"
RENDER_PARAMS_GLSL = ROOT / "modules" / "gaussian_splatting" / "shaders" / "includes" / "gs_render_params.glsl"
SHADER_ROOTS = (
    ROOT / "modules" / "gaussian_splatting" / "shaders",
    ROOT / "modules" / "gaussian_splatting" / "compute",
)
EMBEDDED_SHADER_MIRROR_FILES = (
    ROOT / "modules" / "gaussian_splatting" / "interfaces" / "gpu_sorting_pipeline.cpp",
)
TARGET_STRUCT_NAMES = ("PackedGaussian", "Gaussian", "GaussianQuantized")

# Additional CPU<->GPU mirror structs that also appear as GLSL `struct`s (the
# instancing / asset / chunk tables and the per-chunk quantization bounds) and carry
# literal sizeof/offsetof static_assert contracts. Each spec is
# (host_header_path, host_struct_name, shader_struct_name). Each mirror is (a) checked
# for host self-consistency (computed layout == its own static_asserts) and (b)
# compared field-by-field, by std430 offset, against every GLSL mirror. This extends
# the guard beyond the PackedGaussian family so these push-through GPU structs cannot
# silently drift from their shader mirrors.
#
# The host struct usually lives in gaussian_gpu_layout.h with the same name as its GLSL
# mirror, but the spec supports (a) a different host header and (b) a host name that
# differs from the shader name -- as with ChunkQuantizationGPU (host, in
# core/streaming_quantization.h) vs `struct ChunkQuantization` (GLSL). Field names still
# match one-to-one between the two mirrors, so the comparison is by field name.
#
# (AssetMetaGPU is intentionally excluded for now: it nests an
# AssetLodRangeGPU[GS_MAX_ASSET_LODS] array whose bound is an external constant, which
# needs nested-struct-array + constant-resolution support the layout engine does not
# yet have -- tracked as a follow-on.)
EXTRA_MIRROR_STRUCTS = (
    (HOST_LAYOUT, "InstanceDataGPU", "InstanceDataGPU"),
    (HOST_LAYOUT, "InstanceGradingGPU", "InstanceGradingGPU"),
    (HOST_LAYOUT, "AssetLodRangeGPU", "AssetLodRangeGPU"),
    (HOST_LAYOUT, "ChunkMetaGPU", "ChunkMetaGPU"),
    (HOST_LAYOUT, "AssetChunkIndexGPU", "AssetChunkIndexGPU"),
    (HOST_LAYOUT, "VisibleChunkRefGPU", "VisibleChunkRefGPU"),
    (HOST_LAYOUT, "SplatRefGPU", "SplatRefGPU"),
    (STREAMING_QUANTIZATION_H, "ChunkQuantizationGPU", "ChunkQuantization"),
)


@dataclass(frozen=True)
class RawField:
    type_name: str
    name: str
    count: int | None = None


@dataclass(frozen=True)
class StructDef:
    name: str
    fields: tuple[RawField, ...]
    alignas: int | None = None


@dataclass(frozen=True)
class FlatField:
    name: str
    base_type: str
    components: int


@dataclass(frozen=True)
class LayoutSpec:
    fields: tuple[FlatField, ...]
    offsets: dict[str, int]
    size: int
    alignment: int


_HOST_OFFSET_RE = re.compile(r"static_assert\(offsetof\(PackedGaussian,\s*(\w+)\) == (\d+)")
_FIELD_RE = re.compile(r"^\s*(?P<type>\w+)\s+(?P<name>\w+)(?:\[(?P<count>[A-Za-z_]\w*|\d+)\])?\s*;\s*$")
_CONST_RE = re.compile(r"^static constexpr \w+\s+(?P<name>\w+)\s*=\s*(?P<value>\d+)[uU]?\s*;\s*$")
_SCALAR_BASE_TYPES: dict[str, tuple[str, int, int]] = {
    "float": ("float", 4, 4),
    "uint": ("uint", 4, 4),
    "uint32_t": ("uint", 4, 4),
    "uint16_t": ("uint", 2, 2),
    "int": ("int", 4, 4),
    "int32_t": ("int", 4, 4),
}
_STD430_VECTOR_TYPES: dict[str, tuple[str, int, int]] = {
    "vec2": ("float", 8, 8),
    "vec3": ("float", 16, 12),
    "vec4": ("float", 16, 16),
    "uvec2": ("uint", 8, 8),
    "uvec3": ("uint", 16, 12),
    "uvec4": ("uint", 16, 16),
}
_VECTOR_COMPONENT_COUNTS = {
    "vec2": 2,
    "vec3": 3,
    "vec4": 4,
    "uvec2": 2,
    "uvec3": 3,
    "uvec4": 4,
}
_STRUCT_RE_TEMPLATE = r"struct(?:\s+alignas\((?P<align>\d+)\))?\s+{name}\s*\{{(?P<body>.*?)\}};"

# std140 member layout: type -> (base_alignment, base_size) in bytes. Used only by the
# RenderParams uniform-block checker (blocks are std140, not std430 like the SSBO
# structs above). A mat4 is 4 column vec4s (16-aligned, 64 bytes).
_STD140_TYPES: dict[str, tuple[int, int]] = {
    "float": (4, 4),
    "int": (4, 4),
    "uint": (4, 4),
    "vec2": (8, 8),
    "vec3": (16, 12),
    "vec4": (16, 16),
    "ivec2": (8, 8),
    "ivec3": (16, 12),
    "ivec4": (16, 16),
    "uvec2": (8, 8),
    "uvec3": (16, 12),
    "uvec4": (16, 16),
    "mat4": (16, 64),
}
_UBO_BLOCK_RE = re.compile(r"uniform\s+RenderParams\s*\{(?P<body>.*?)\}\s*params\s*;", re.DOTALL)
_UBO_FIELD_RE = re.compile(r"^(?P<type>\w+)\s+(?P<name>\w+)(?:\[(?P<count>[A-Za-z_]\w*|\d+)\])?\s*;$")
_GLSL_DEFINE_RE = re.compile(r"^\s*#define\s+(?P<name>\w+)\s+(?P<value>\d+)\b", re.MULTILINE)
_RENDER_PARAMS_VERSION_HOST_RE = re.compile(r"GS_RENDER_PARAMS_LAYOUT_VERSION\s*=\s*(\d+)")
# Host C++ field: leading scalar type token + name, tolerating 1D and 2D array suffixes
# (e.g. `float effector_spheres[GS_MAX_SPHERE_EFFECTORS][4];`). Only the base type token
# is captured -- layout is NOT computed for the UBO host struct, so this deliberately
# avoids the mat/2D-array parsing the std430 engine cannot do.
_HOST_FIELD_TYPE_RE = re.compile(r"^(?P<type>\w+)\s+(?P<name>\w+)\s*(?:\[[A-Za-z0-9_]+\]\s*)*;$")

# Scalar "kind" of a type: float / uint / int. mat* and vec* are float-kind; uvec*/uint
# are uint-kind; ivec*/int are int-kind. A same-size type drift (e.g. GLSL `uint`->`float`
# or `uvec4`->`vec4`) keeps the byte offset but changes how the shader reinterprets the
# bytes, so the UBO check compares kinds per member in addition to offsets.
_GLSL_SCALAR_KIND: dict[str, str] = {
    "float": "float", "vec2": "float", "vec3": "float", "vec4": "float",
    "mat2": "float", "mat3": "float", "mat4": "float",
    "uint": "uint", "uvec2": "uint", "uvec3": "uint", "uvec4": "uint",
    "int": "int", "ivec2": "int", "ivec3": "int", "ivec4": "int",
}
_HOST_SCALAR_KIND: dict[str, str] = {
    "float": "float", "double": "float",
    "Vector2": "float", "Vector3": "float", "Vector4": "float", "Color": "float",
    "uint": "uint", "uint32_t": "uint", "uint16_t": "uint", "uint8_t": "uint",
    "int": "int", "int32_t": "int", "int16_t": "int", "int8_t": "int",
}


def _struct_pattern(name: str) -> re.Pattern[str]:
    return re.compile(_STRUCT_RE_TEMPLATE.format(name=re.escape(name)), re.DOTALL)


def _round_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def _parse_host_contracts(path: Path) -> tuple[dict[str, int], int]:
    text = path.read_text(encoding="utf-8")
    offsets = {match.group(1): int(match.group(2)) for match in _HOST_OFFSET_RE.finditer(text)}
    size_match = re.search(r"static_assert\(sizeof\(PackedGaussian\) == (\d+)", text)
    if not size_match:
        raise RuntimeError(f"Missing PackedGaussian sizeof contract in {path}")
    return offsets, int(size_match.group(1))


def _parse_host_size_contract(path: Path, struct_name: str) -> int:
    text = path.read_text(encoding="utf-8")
    size_match = re.search(rf"static_assert\(sizeof\({re.escape(struct_name)}\) == (\d+)", text)
    if not size_match:
        raise RuntimeError(f"Missing {struct_name} sizeof contract in {path}")
    return int(size_match.group(1))


def _parse_struct_definition(path: Path, struct_name: str) -> StructDef:
    text = path.read_text(encoding="utf-8")
    match = _struct_pattern(struct_name).search(text)
    if not match:
        raise RuntimeError(f"Could not find `struct {struct_name}` in {path}")

    fields: list[RawField] = []
    constants: dict[str, int] = {}
    for raw_line in match.group("body").splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line:
            continue
        const_match = _CONST_RE.match(line)
        if const_match:
            constants[const_match.group("name")] = int(const_match.group("value"))
            continue
        if line.startswith(("static_assert", "using ", "typedef ", "friend ", "void ", "#")):
            continue
        if "(" in line:
            continue
        field_match = _FIELD_RE.match(line)
        if not field_match:
            raise RuntimeError(f"Unsupported field syntax in {path}: {raw_line.strip()}")
        count = field_match.group("count")
        if count is None:
            count_value = None
        elif count.isdigit():
            count_value = int(count)
        elif count in constants:
            count_value = constants[count]
        else:
            raise RuntimeError(f"Unknown array bound `{count}` in {path}: {raw_line.strip()}")
        fields.append(RawField(field_match.group("type"), field_match.group("name"), count_value))
    return StructDef(struct_name, tuple(fields), int(match.group("align")) if match.group("align") else None)


def _discover_shader_struct_sources() -> tuple[tuple[Path, str], ...]:
    discovered: list[tuple[Path, str]] = []
    seen: set[tuple[Path, str]] = set()

    for root in SHADER_ROOTS:
        for path in sorted(root.rglob("*.glsl")):
            text = path.read_text(encoding="utf-8")
            for struct_name in TARGET_STRUCT_NAMES:
                if _struct_pattern(struct_name).search(text):
                    key = (path, struct_name)
                    if key not in seen:
                        seen.add(key)
                        discovered.append(key)

    for path in EMBEDDED_SHADER_MIRROR_FILES:
        text = path.read_text(encoding="utf-8")
        for struct_name in TARGET_STRUCT_NAMES:
            if _struct_pattern(struct_name).search(text):
                key = (path, struct_name)
                if key not in seen:
                    seen.add(key)
                    discovered.append(key)

    if not discovered:
        raise RuntimeError("Could not find any shader mirrors declaring `struct Gaussian` or `struct PackedGaussian`")
    return tuple(discovered)


def _normalized_type_signature(field: RawField) -> tuple[str, int]:
    if field.count is not None:
        if field.type_name not in _SCALAR_BASE_TYPES:
            raise RuntimeError(f"Unsupported array type `{field.type_name}[{field.count}]`")
        base_type, _, _ = _SCALAR_BASE_TYPES[field.type_name]
        return base_type, field.count

    if field.type_name in _SCALAR_BASE_TYPES:
        base_type, _, _ = _SCALAR_BASE_TYPES[field.type_name]
        return base_type, 1
    if field.type_name in _STD430_VECTOR_TYPES:
        base_type, _, _ = _STD430_VECTOR_TYPES[field.type_name]
        components = _VECTOR_COMPONENT_COUNTS[field.type_name]
        return base_type, components
    raise RuntimeError(f"Unsupported field type `{field.type_name}`")


def _field_layout(field: RawField, mode: str) -> tuple[int, int]:
    if field.count is not None:
        if field.type_name not in _SCALAR_BASE_TYPES:
            raise RuntimeError(f"Unsupported array type `{field.type_name}[{field.count}]`")
        _, alignment, element_size = _SCALAR_BASE_TYPES[field.type_name]
        return alignment, element_size * field.count

    if field.type_name in _SCALAR_BASE_TYPES:
        _, alignment, size = _SCALAR_BASE_TYPES[field.type_name]
        return alignment, size
    if mode == "shader" and field.type_name in _STD430_VECTOR_TYPES:
        _, alignment, size = _STD430_VECTOR_TYPES[field.type_name]
        return alignment, size
    raise RuntimeError(f"Unsupported {'std430' if mode == 'shader' else 'C++'} field type `{field.type_name}`")


def _layout_struct(struct_definitions: dict[str, StructDef], struct_name: str, mode: str, prefix: str = "") -> LayoutSpec:
    struct_def = struct_definitions[struct_name]
    offset = 0
    max_alignment = struct_def.alignas or 1
    fields: list[FlatField] = []
    offsets: dict[str, int] = {}

    for field in struct_def.fields:
        if field.count is None and field.type_name in struct_definitions:
            child = _layout_struct(struct_definitions, field.type_name, mode, prefix + field.name + "_")
            offset = _round_up(offset, child.alignment)
            for child_field in child.fields:
                fields.append(child_field)
            for child_name, child_offset in child.offsets.items():
                offsets[child_name] = offset + child_offset
            offset += child.size
            max_alignment = max(max_alignment, child.alignment)
            continue

        alignment, size = _field_layout(field, mode)
        offset = _round_up(offset, alignment)
        base_type, components = _normalized_type_signature(field)
        flat_name = prefix + field.name
        fields.append(FlatField(flat_name, base_type, components))
        offsets[flat_name] = offset
        offset += size
        max_alignment = max(max_alignment, alignment)

    size = _round_up(offset, max_alignment)
    return LayoutSpec(tuple(fields), offsets, size, max_alignment)


def _format_signature(base_type: str, components: int) -> str:
    return base_type if components == 1 else f"{base_type}[{components}]"


def _build_quantized_expected_layout(host_structs: dict[str, StructDef]) -> LayoutSpec:
    host_layout = _layout_struct(host_structs, "PackedGaussianQuantized", "host")
    fields = (
        FlatField("position_chunk", "uint", 2),
        FlatField("opacity", "float", 1),
        FlatField("scale_area_lo", "uint", 1),
        FlatField("scale_area_hi", "uint", 1),
        FlatField("rotation_lo", "uint", 1),
        FlatField("rotation_hi", "uint", 1),
        FlatField("_padding", "uint", 1),
        FlatField("sh_dc", "float", 4),
        FlatField("sh_encoded_01", "uint", 2),
        FlatField("sh_encoded_23", "uint", 2),
        FlatField("sh_encoded_45", "uint", 2),
        FlatField("normal_xy", "uint", 1),
        FlatField("normal_z_stroke", "uint", 1),
    )
    offsets = {
        "position_chunk": host_layout.offsets["quantized_position"],
        "opacity": host_layout.offsets["opacity"],
        "scale_area_lo": host_layout.offsets["quantized_scale"],
        "scale_area_hi": host_layout.offsets["quantized_scale"] + 4,
        "rotation_lo": host_layout.offsets["rotation"],
        "rotation_hi": host_layout.offsets["rotation"] + 4,
        "_padding": host_layout.offsets["_pre_sh_padding"],
        "sh_dc": host_layout.offsets["sh_dc"],
        "sh_encoded_01": host_layout.offsets["sh_encoded"],
        "sh_encoded_23": host_layout.offsets["sh_encoded"] + 8,
        "sh_encoded_45": host_layout.offsets["sh_encoded"] + 16,
        "normal_xy": host_layout.offsets["normal_xy"],
        "normal_z_stroke": host_layout.offsets["normal_z_stroke"],
    }
    return LayoutSpec(fields, offsets, host_layout.size, host_layout.alignment)


def _compare_layouts(
    expected: LayoutSpec,
    actual: LayoutSpec,
    source_path: Path,
    source_struct_name: str,
    expected_label: str,
    failures: list[str],
) -> None:
    if len(actual.fields) != len(expected.fields):
        failures.append(
            f"{source_path.relative_to(ROOT)}: `struct {source_struct_name}` field count {len(actual.fields)} != expected {len(expected.fields)}"
        )

    for index, expected_field in enumerate(expected.fields):
        if index >= len(actual.fields):
            failures.append(
                f"{source_path.relative_to(ROOT)}: `struct {source_struct_name}` is missing field `{expected_field.name}` at index {index}"
            )
            continue

        actual_field = actual.fields[index]
        expected_signature = _format_signature(expected_field.base_type, expected_field.components)
        actual_signature = _format_signature(actual_field.base_type, actual_field.components)
        if actual_field.name != expected_field.name:
            failures.append(
                f"{source_path.relative_to(ROOT)}: field {index} name `{actual_field.name}` != expected `{expected_field.name}`"
            )
        if actual_signature != expected_signature:
            failures.append(
                f"{source_path.relative_to(ROOT)}: field `{actual_field.name}` type `{actual_signature}` != expected `{expected_signature}`"
            )

        expected_offset = expected.offsets[expected_field.name]
        actual_offset = actual.offsets.get(actual_field.name)
        if actual_offset != expected_offset:
            failures.append(
                f"{source_path.relative_to(ROOT)}: field `{actual_field.name}` offset {actual_offset} != expected {expected_offset}"
            )

    if actual.size != expected.size:
        failures.append(
            f"{source_path.relative_to(ROOT)}: struct size {actual.size} != expected {expected_label} size {expected.size}"
        )


def _parse_struct_size_contract(text: str, struct_name: str) -> int | None:
    match = re.search(rf"static_assert\(sizeof\({re.escape(struct_name)}\)\s*==\s*(\d+)\b", text)
    return int(match.group(1)) if match else None


def _parse_struct_offset_contracts(text: str, struct_name: str) -> dict[str, int]:
    pattern = re.compile(rf"static_assert\(offsetof\({re.escape(struct_name)},\s*(\w+)\)\s*==\s*(\d+)\b")
    return {match.group(1): int(match.group(2)) for match in pattern.finditer(text)}


def _discover_struct_mirrors(struct_name: str) -> tuple[Path, ...]:
    pattern = _struct_pattern(struct_name)
    mirrors: list[Path] = []
    seen: set[Path] = set()
    for root in SHADER_ROOTS:
        for path in sorted(root.rglob("*.glsl")):
            if path in seen:
                continue
            if pattern.search(path.read_text(encoding="utf-8")):
                seen.add(path)
                mirrors.append(path)
    return tuple(mirrors)


def _check_extra_mirror_struct(host_header: Path, host_name: str, shader_name: str, failures: list[str]) -> None:
    host_text = host_header.read_text(encoding="utf-8")
    host_def = _parse_struct_definition(host_header, host_name)
    host_layout = _layout_struct({host_name: host_def}, host_name, "host")

    # (a) Host self-consistency: the layout the engine computes must match this
    # struct's own literal sizeof/offsetof contracts, so the contracts stay the
    # single source of truth even before comparing to shaders.
    contract_size = _parse_struct_size_contract(host_text, host_name)
    if contract_size is None:
        failures.append(
            f"{host_header.relative_to(ROOT)}: `{host_name}` is in EXTRA_MIRROR_STRUCTS but has no literal `sizeof` static_assert to anchor"
        )
    elif host_layout.size != contract_size:
        failures.append(
            f"{host_header.relative_to(ROOT)}: computed {host_name} size {host_layout.size} != host contract {contract_size}"
        )
    for field_name, expected_offset in _parse_struct_offset_contracts(host_text, host_name).items():
        actual_offset = host_layout.offsets.get(field_name)
        if actual_offset != expected_offset:
            failures.append(
                f"{host_header.relative_to(ROOT)}: {host_name}.{field_name} computed offset {actual_offset} != host contract {expected_offset}"
            )

    # (b) Every GLSL mirror must match the host layout (field names, std430
    # signatures, offsets, size). The GLSL struct may carry a different name than
    # the host struct, but its field names match one-to-one.
    mirrors = _discover_struct_mirrors(shader_name)
    if not mirrors:
        failures.append(
            f"{shader_name}: no GLSL mirror `struct {shader_name}` found under shaders/ or compute/"
        )
    for shader_path in mirrors:
        shader_def = _parse_struct_definition(shader_path, shader_name)
        shader_layout = _layout_struct({shader_name: shader_def}, shader_name, "shader")
        _compare_layouts(host_layout, shader_layout, shader_path, shader_name, host_name, failures)


def _std140_member_layout(type_name: str, count: int | None) -> tuple[int, int]:
    """Return (alignment, total_size) in bytes for one std140 uniform-block member."""
    if type_name not in _STD140_TYPES:
        raise RuntimeError(f"Unsupported std140 uniform member type `{type_name}`")
    base_align, base_size = _STD140_TYPES[type_name]
    if count is None:
        return base_align, base_size
    # std140 arrays: element alignment and stride round up to 16 bytes (vec4 rule),
    # so vec4[N] has stride 16 and mat4[N] has stride 64.
    stride = _round_up(base_size, 16)
    align = _round_up(base_align, 16)
    return align, stride * count


@dataclass(frozen=True)
class _UboMember:
    name: str
    type_name: str
    offset: int


def _parse_host_struct_field_types(host_text: str, struct_name: str) -> dict[str, str]:
    """Return {field_name: base C++ type token} for a struct, tolerating 1D/2D array
    suffixes. Layout is deliberately NOT computed (that needs the mat/2D-array parsing we
    avoid); only the leading scalar type token is captured so its scalar KIND can be
    compared against the GLSL mirror."""
    match = _struct_pattern(struct_name).search(host_text)
    if not match:
        raise RuntimeError(f"Could not find `struct {struct_name}` to read field types")
    types: dict[str, str] = {}
    for raw_line in match.group("body").splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line or line.startswith(("static_assert", "static ", "using ", "typedef ", "friend ", "#")):
            continue
        if "(" in line:  # skip method declarations
            continue
        field_match = _HOST_FIELD_TYPE_RE.match(line)
        if field_match:
            types[field_match.group("name")] = field_match.group("type")
    return types


def _walk_std140_members(
    body: str, defines: dict[str, int], glsl_rel: Path, failures: list[str]
) -> tuple[tuple[_UboMember, ...], int] | None:
    """Walk a std140 uniform-block body, returning (members, end_offset). Each member
    records its resolved std140 offset. Returns None (after appending a failure) if a
    line, array bound, or type cannot be parsed."""
    members: list[_UboMember] = []
    offset = 0
    for raw_line in body.splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line or line.startswith("#"):
            continue
        field_match = _UBO_FIELD_RE.match(line)
        if not field_match:
            failures.append(f"{glsl_rel}: unsupported RenderParams member syntax: {raw_line.strip()}")
            return None
        type_name = field_match.group("type")
        name = field_match.group("name")
        count_token = field_match.group("count")
        if count_token is None:
            count = None
        elif count_token.isdigit():
            count = int(count_token)
        elif count_token in defines:
            count = defines[count_token]
        else:
            failures.append(f"{glsl_rel}: unresolved RenderParams array bound `{count_token}` for member `{name}`")
            return None
        try:
            alignment, size = _std140_member_layout(type_name, count)
        except RuntimeError as exc:
            failures.append(f"{glsl_rel}: {exc}")
            return None
        offset = _round_up(offset, alignment)
        members.append(_UboMember(name, type_name, offset))
        offset += size
    return tuple(members), offset


def _check_ubo_member(
    member: _UboMember,
    host_offsets: dict[str, int],
    host_types: dict[str, str],
    glsl_rel: Path,
    host_rel: Path,
    failures: list[str],
) -> None:
    """Assert one non-pad UBO member matches the C++ contract in both std140 OFFSET and
    scalar KIND (float/uint/int). The kind check catches same-size type drift the offset
    check cannot see (e.g. GLSL `uint`->`float`, `uvec4`->`vec4`)."""
    expected_offset = host_offsets.get(member.name)
    if expected_offset is None:
        failures.append(
            f"{glsl_rel}: RenderParams member `{member.name}` has no matching offsetof(TileRenderParamsGPU, {member.name}) contract in {host_rel}"
        )
    elif member.offset != expected_offset:
        failures.append(
            f"{glsl_rel}: RenderParams.{member.name} std140 offset {member.offset} != TileRenderParamsGPU contract {expected_offset}"
        )

    glsl_kind = _GLSL_SCALAR_KIND.get(member.type_name)
    if glsl_kind is None:
        failures.append(
            f"{glsl_rel}: RenderParams.{member.name} GLSL type `{member.type_name}` has no known scalar kind; cannot verify against TileRenderParamsGPU"
        )
        return
    host_type = host_types.get(member.name)
    if host_type is None:
        # Fail closed: a member checked for offset must have a parseable C++ field type.
        failures.append(
            f"{glsl_rel}: RenderParams member `{member.name}` has no parseable C++ field in TileRenderParamsGPU ({host_rel}); cannot verify scalar kind"
        )
        return
    host_kind = _HOST_SCALAR_KIND.get(host_type)
    if host_kind is None:
        failures.append(
            f"{host_rel}: TileRenderParamsGPU.{member.name} C++ type `{host_type}` has no known scalar kind; cannot verify RenderParams.{member.name}"
        )
    elif glsl_kind != host_kind:
        failures.append(
            f"{glsl_rel}: RenderParams.{member.name} scalar kind `{glsl_kind}` (`{member.type_name}`) != TileRenderParamsGPU.{member.name} `{host_kind}` (`{host_type}`)"
        )


def _check_render_params_version(
    host_text: str, defines: dict[str, int], glsl_rel: Path, host_rel: Path, failures: list[str]
) -> None:
    host_version_match = _RENDER_PARAMS_VERSION_HOST_RE.search(host_text)
    host_version = int(host_version_match.group(1)) if host_version_match else None
    glsl_version = defines.get("GS_RENDER_PARAMS_LAYOUT_VERSION")
    if host_version is None:
        failures.append(f"{host_rel}: missing `GS_RENDER_PARAMS_LAYOUT_VERSION` constant to anchor RenderParams")
    elif glsl_version is None:
        failures.append(f"{glsl_rel}: missing `#define GS_RENDER_PARAMS_LAYOUT_VERSION` to anchor RenderParams")
    elif host_version != glsl_version:
        failures.append(
            f"GS_RENDER_PARAMS_LAYOUT_VERSION mismatch: host {host_version} ({host_rel}) != shader {glsl_version} ({glsl_rel})"
        )


def _check_render_params_ubo(host_text: str, failures: list[str]) -> None:
    """Validate the RenderParams std140 uniform block against TileRenderParamsGPU.

    RenderParams is a `uniform` block (not a `struct`) with mat4 and vec4[const]
    members, so the std430 struct machinery above cannot handle it. For every non-`_pad*`
    member this asserts (1) the std140 offset lands on the C++
    `offsetof(TileRenderParamsGPU, <same name>)` contract and (2) the member's scalar KIND
    (float/uint/int) matches the C++ field's scalar kind -- so a same-size type drift the
    offset check cannot see (GLSL `uint`->`float`, `uvec4`->`vec4`) is still caught. It
    also asserts the block size matches the C++ `sizeof` contract and the two
    GS_RENDER_PARAMS_LAYOUT_VERSION numbers agree. Padding members are skipped: they carry
    different names/counts across the two files (host `_pad_before_camera[2]` vs GLSL
    `_pad0`/`_pad1`) but occupy equal space.
    """
    glsl_text = RENDER_PARAMS_GLSL.read_text(encoding="utf-8")
    glsl_rel = RENDER_PARAMS_GLSL.relative_to(ROOT)
    host_rel = HOST_LAYOUT.relative_to(ROOT)

    defines = {m.group("name"): int(m.group("value")) for m in _GLSL_DEFINE_RE.finditer(glsl_text)}

    block_match = _UBO_BLOCK_RE.search(glsl_text)
    if not block_match:
        failures.append(f"{glsl_rel}: could not find `uniform RenderParams {{ ... }} params;` block")
        return

    host_offsets = _parse_struct_offset_contracts(host_text, "TileRenderParamsGPU")
    host_size = _parse_struct_size_contract(host_text, "TileRenderParamsGPU")
    host_types = _parse_host_struct_field_types(host_text, "TileRenderParamsGPU")
    if host_size is None:
        failures.append(f"{host_rel}: missing `sizeof(TileRenderParamsGPU)` static_assert to anchor RenderParams")
        return
    if not host_offsets:
        failures.append(f"{host_rel}: no `offsetof(TileRenderParamsGPU, ...)` contracts found to anchor RenderParams")
        return
    if not host_types:
        failures.append(f"{host_rel}: no parseable TileRenderParamsGPU field types to anchor RenderParams scalar kinds")
        return

    walked = _walk_std140_members(block_match.group("body"), defines, glsl_rel, failures)
    if walked is None:
        return
    members, end_offset = walked

    for member in members:
        if member.name.startswith("_pad"):
            continue
        _check_ubo_member(member, host_offsets, host_types, glsl_rel, host_rel, failures)

    block_size = _round_up(end_offset, 16)
    if block_size != host_size:
        failures.append(
            f"{glsl_rel}: RenderParams std140 block size {block_size} != TileRenderParamsGPU contract {host_size}"
        )

    _check_render_params_version(host_text, defines, glsl_rel, host_rel, failures)


def main() -> int:
    host_offsets, host_size = _parse_host_contracts(HOST_LAYOUT)
    packed_host_structs = {
        "PackedGaussian": _parse_struct_definition(HOST_LAYOUT, "PackedGaussian"),
        "PackedSphericalHarmonics": _parse_struct_definition(HOST_LAYOUT, "PackedSphericalHarmonics"),
    }
    packed_host_layout = _layout_struct(packed_host_structs, "PackedGaussian", "host")
    quantized_host_structs = {
        "PackedGaussianQuantized": _parse_struct_definition(HOST_LAYOUT, "PackedGaussianQuantized"),
    }
    quantized_host_layout = _layout_struct(quantized_host_structs, "PackedGaussianQuantized", "host")
    quantized_host_size = _parse_host_size_contract(HOST_LAYOUT, "PackedGaussianQuantized")

    failures: list[str] = []
    if packed_host_layout.size != host_size:
        failures.append(
            f"{HOST_LAYOUT.relative_to(ROOT)}: computed PackedGaussian size {packed_host_layout.size} != host contract {host_size}"
        )
    for contract_name, expected_offset in host_offsets.items():
        actual_name = "sh_dc" if contract_name == "sh" else contract_name
        actual_offset = packed_host_layout.offsets.get(actual_name)
        if actual_offset != expected_offset:
            failures.append(
                f"{HOST_LAYOUT.relative_to(ROOT)}: host field `{contract_name}` offset {actual_offset} != contract {expected_offset}"
            )
    if quantized_host_layout.size != quantized_host_size:
        failures.append(
            f"{HOST_LAYOUT.relative_to(ROOT)}: computed PackedGaussianQuantized size {quantized_host_layout.size} != host contract {quantized_host_size}"
        )

    quantized_expected_layout = _build_quantized_expected_layout(quantized_host_structs)

    for shader_path, struct_name in _discover_shader_struct_sources():
        shader_structs = {struct_name: _parse_struct_definition(shader_path, struct_name)}
        shader_layout = _layout_struct(shader_structs, struct_name, "shader")
        if struct_name in ("PackedGaussian", "Gaussian"):
            expected_layout = packed_host_layout
            expected_label = "PackedGaussian"
        elif struct_name == "GaussianQuantized":
            expected_layout = quantized_expected_layout
            expected_label = "PackedGaussianQuantized"
        else:
            raise RuntimeError(f"Unexpected discovered struct `{struct_name}`")
        _compare_layouts(expected_layout, shader_layout, shader_path, struct_name, expected_label, failures)

    host_text = HOST_LAYOUT.read_text(encoding="utf-8")
    for host_header, host_name, shader_name in EXTRA_MIRROR_STRUCTS:
        _check_extra_mirror_struct(host_header, host_name, shader_name, failures)

    _check_render_params_ubo(host_text, failures)

    if failures:
        for failure in failures:
            print(f"[gaussian-layout-check] FAIL {failure}")
        return 1

    print("[gaussian-layout-check] PASSED")
    print("[gaussian-layout-check] PackedGaussian and PackedGaussianQuantized host/mirror field signatures, offsets, and size are aligned.")
    print(
        "[gaussian-layout-check] Instancing/asset/chunk/quantization mirror structs are aligned: "
        + ", ".join(host_name for _, host_name, _ in EXTRA_MIRROR_STRUCTS)
        + "."
    )
    print("[gaussian-layout-check] RenderParams std140 uniform block matches TileRenderParamsGPU offsets, scalar kinds, size, and layout version.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
