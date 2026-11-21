#!/usr/bin/env python3
"""Numerical contracts of the Gaussian splatting shaders, evaluated from the shader source.

The functions under test are translated from the real `.glsl` files by
`tests/ci/glsl_host_eval.py` and executed on the host, so a test here reads the
code the GPU compiles -- not a C++ or Python mirror that can drift from it.
A construct the evaluator does not model raises; nothing here is skipped.

Run by `run_module_tests.py --guard-only` (lane: shader host-numerics tests).
"""

from __future__ import annotations

import importlib.util
import math
import re
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
SHADERS_DIR = ROOT / "modules" / "gaussian_splatting" / "shaders"
INCLUDES_DIR = SHADERS_DIR / "includes"

_spec = importlib.util.spec_from_file_location("glsl_host_eval", Path(__file__).with_name("glsl_host_eval.py"))
assert _spec and _spec.loader
glsl = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = glsl
_spec.loader.exec_module(glsl)


# ---------------------------------------------------------------------------
# Evaluator self-tests: the harness must be able to fail before its greens mean anything.
# ---------------------------------------------------------------------------


class GlslHostEvalSelfTests(unittest.TestCase):
    def test_half_conversion_matches_ieee_round_to_nearest_even(self) -> None:
        values = [0.0, -0.0, 1.0, 0.1, 1.0 / 3.0, 65504.0, 65519.0, 6.1e-5, 5.96e-8, 2.98e-8,
                  1919.4, 1500.3, 3001.2, 1024.5, 2049.0, 0.999, -7.25]
        values += [i * 0.37 + 0.011 for i in range(400)]
        for v in values:
            expected = struct.unpack("<H", struct.pack("<e", v))[0]
            self.assertEqual(glsl.f32_to_f16_bits(v, "rte"), expected, f"{v!r}")
        self.assertEqual(glsl.f32_to_f16_bits(70000.0, "rte"), 0x7C00)
        self.assertEqual(glsl.f32_to_f16_bits(70000.0, "rtz"), 0x7BFF)
        self.assertEqual(glsl.f32_to_f16_bits(1919.9, "rtz"), struct.unpack("<H", struct.pack("<e", 1919.0))[0])

    def test_c_precedence_hazard_is_rejected_not_evaluated(self) -> None:
        program = glsl.GlslProgram("bool f(uint x) { return x & 1u == 0u; }")
        self.assertIn("f", program.unsupported)
        with self.assertRaises(glsl.GlslUnsupported):
            program.fn("f")(glsl.U32(2))

    def test_undefined_behaviour_raises(self) -> None:
        program = glsl.GlslProgram(
            "float f(float x) { return clamp(x, 0.0, 1.0); }\n"
            "uint g(float x) { return uint(x); }\n"
            "float h() { float y; return y + 1.0; }\n")
        with self.assertRaises(glsl.GlslUndefinedBehaviour):
            program.fn("f")(math.nan)
        with self.assertRaises(glsl.GlslUndefinedBehaviour):
            program.fn("g")(-1.0)
        with self.assertRaises(glsl.GlslUndefinedBehaviour):
            program.fn("h")()

    def test_uint_wraps_and_int_division_truncates(self) -> None:
        program = glsl.GlslProgram(
            "uint f(uint a) { return (a << 4u) + 0xFFFFFFFFu; }\n"
            "int g(int a) { return a / 2; }\n")
        self.assertEqual(int(program.fn("f")(glsl.U32(0x10000000))), 0xFFFFFFFF)
        self.assertEqual(int(program.fn("g")(glsl.I32(-7))), -3)

    def test_out_parameters_and_value_semantics(self) -> None:
        program = glsl.GlslProgram(
            "void split(vec2 v, out float a, out float b) { a = v.x; b = v.y; v.x = 9.0; }\n"
            "float sum(vec2 v) { float a; float b; split(v, a, b); return a + b + v.x; }\n")
        vec2 = program.namespace["vec2"]
        v = vec2(1.0, 2.0)
        self.assertEqual(program.fn("sum")(v), 4.0)
        self.assertEqual(v.x, 1.0, "an `in` parameter must be a copy")


# ---------------------------------------------------------------------------
# #1157: the SH basis must equal the Inria 3DGS reference (computeColorFromSH).
# ---------------------------------------------------------------------------

# Inria diff-gaussian-rasterization forward.cu constants, signed as published.
INRIA_SH_C0 = 0.28209479177387814
INRIA_SH_C1 = 0.4886025119029199
INRIA_SH_C2 = (1.0925484305920792, -1.0925484305920792, 0.31539156525252005,
               -1.0925484305920792, 0.5462742152960396)
INRIA_SH_C3 = (-0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154,
               -0.4570457994644658, 1.445305721320277, -0.5900435899266435)


def inria_sh_basis(x: float, y: float, z: float) -> list[float]:
    """The 16 basis values computeColorFromSH multiplies sh[0..15] by, in its own form."""
    xx, yy, zz = x * x, y * y, z * z
    xy, yz, xz = x * y, y * z, x * z
    return [
        INRIA_SH_C0,
        -INRIA_SH_C1 * y,
        INRIA_SH_C1 * z,
        -INRIA_SH_C1 * x,
        INRIA_SH_C2[0] * xy,
        INRIA_SH_C2[1] * yz,
        INRIA_SH_C2[2] * (2.0 * zz - xx - yy),
        INRIA_SH_C2[3] * xz,
        INRIA_SH_C2[4] * (xx - yy),
        INRIA_SH_C3[0] * y * (3.0 * xx - yy),
        INRIA_SH_C3[1] * xy * z,
        INRIA_SH_C3[2] * y * (4.0 * zz - xx - yy),
        INRIA_SH_C3[3] * z * (2.0 * zz - 3.0 * xx - 3.0 * yy),
        INRIA_SH_C3[4] * x * (4.0 * zz - xx - yy),
        INRIA_SH_C3[5] * z * (xx - yy),
        INRIA_SH_C3[6] * x * (xx - 3.0 * yy),
    ]


def _normalized(v: tuple[float, float, float]) -> tuple[float, float, float]:
    n = math.sqrt(sum(c * c for c in v))
    return (v[0] / n, v[1] / n, v[2] / n)


# 22 unit directions: the 6 axes, the 8 cube diagonals, and 8 generic directions with
# every component non-zero and distinct, so every band-3 term is exercised with both signs.
REFERENCE_DIRECTIONS = (
    [(1.0, 0.0, 0.0), (-1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, -1.0, 0.0), (0.0, 0.0, 1.0), (0.0, 0.0, -1.0)]
    + [_normalized((sx, sy, sz)) for sx in (1.0, -1.0) for sy in (1.0, -1.0) for sz in (1.0, -1.0)]
    + [_normalized(v) for v in (
        (0.3, -0.5, 0.81), (-0.7, 0.2, 0.4), (0.15, 0.9, -0.35), (-0.25, -0.6, -0.75),
        (0.8, 0.45, 0.1), (-0.05, 0.3, -0.95), (0.62, -0.71, 0.33), (-0.41, -0.12, 0.9))]
)

# A function writing a 16- or 25-entry SH basis: any `out float NAME[16|25]` parameter
# (gs_sh_basis.glsl spells the qualifier GS_SH_OUT so the native tests can include it).
_SH_BASIS_SIGNATURE = re.compile(r"\b(?:void)\s+(\w+)\s*\(([^)]*\b(?:out|GS_SH_OUT)\s+float\s+\w+\s*\[\s*(16|25)\s*\][^)]*)\)\s*\{")


def discover_sh_basis_functions() -> list[tuple[Path, str, list[str]]]:
    """Every shader function that fills a 16-entry SH basis -- derived, not listed."""
    found = []
    for path in sorted(SHADERS_DIR.rglob("*.glsl")):
        text = path.read_text(encoding="utf-8")
        for m in _SH_BASIS_SIGNATURE.finditer(text):
            param_types = [p.split()[-2] if p.split()[0] in ("in", "out", "inout", "GS_SH_OUT") else p.split()[0]
                           for p in (s.strip() for s in m.group(2).split(",")) if p]
            found.append((path, m.group(1), param_types + [int(m.group(3))]))
    return found


class ShBasisMatchesInriaReference(unittest.TestCase):
    """#1157: band-3 basis[11] and [13] were sign-flipped and [14] doubled."""

    def test_discovery_finds_the_production_basis(self) -> None:
        found = {(p.name, name) for p, name, _types in discover_sh_basis_functions()}
        # Tile binning evaluates the shared SH0-4 basis include since #1200/#1205.
        self.assertIn(("gs_sh_basis.glsl", "gs_compute_real_sh_basis"), found,
                      "the tile-binning SH basis is no longer discovered; the pattern drifted")

    def _evaluate(self, path: Path, name: str, param_types: list[str], direction: tuple[float, float, float],
                  band: int) -> list[float]:
        program = glsl.load(path)
        vec3 = program.namespace["vec3"]
        args = []
        basis = [None] * param_types[-1]
        for ptype in param_types[:-1]:
            if ptype == "vec3":
                args.append(vec3(*direction))
            elif ptype == "uint":
                args.append(glsl.U32(band))
            elif ptype == "float":
                args.append(basis)
            else:
                self.fail(f"{path.name}:{name}: unexpected parameter type {ptype}")
        program.fn(name)(*args)
        return basis

    def test_every_sh_basis_function_equals_the_inria_reference(self) -> None:
        functions = discover_sh_basis_functions()
        self.assertGreaterEqual(len(functions), 1)
        for path, name, param_types in functions:
            has_band_param = "uint" in param_types[:-1]
            for direction in REFERENCE_DIRECTIONS:
                reference = inria_sh_basis(*direction)
                for band in ((0, 1, 2, 3) if has_band_param else (3,)):
                    basis = self._evaluate(path, name, param_types, direction, band)
                    used = (1, 4, 9, 16)[band]
                    for index in range(len(basis)):  # entries past band 3 must stay zero
                        expected = reference[index] if index < used else 0.0
                        with self.subTest(file=path.name, function=name, direction=direction,
                                          band=band, index=index):
                            self.assertAlmostEqual(
                                float(basis[index]), expected, places=6,
                                msg=f"{path.relative_to(ROOT)}:{name} basis[{index}] at {direction} "
                                    f"(band {band}) is {basis[index]!r}, Inria reference {expected!r}")

    def test_reference_table_pins_the_band3_terms_that_were_wrong(self) -> None:
        # Hand-checked values on direction (0, 0.6, 0.8): xx = 0, yy = 0.36, zz = 0.64.
        # basis[11] = SH_C3[2] * y * (4zz - xx - yy) = -0.45705 * 0.6 * 2.2 = -0.603300...
        # The reference itself is pinned so a sign slip in the table cannot cancel one in the shader.
        reference = inria_sh_basis(0.0, 0.6, 0.8)
        self.assertAlmostEqual(reference[11], -0.4570457994644658 * 0.6 * 2.2, places=12)
        self.assertAlmostEqual(reference[14], 1.445305721320277 * 0.8 * (0.0 - 0.36), places=12)
        reference = inria_sh_basis(*_normalized((1.0, 0.0, 1.0)))
        self.assertAlmostEqual(reference[13], -0.4570457994644658 * math.sqrt(0.5) * (4 * 0.5 - 0.5), places=12)


# ---------------------------------------------------------------------------
# #1153: the raster payload must carry the splat screen centre exactly.
# ---------------------------------------------------------------------------

TILE_PROJECTION_COMMON = INCLUDES_DIR / "tile_projection_common.glsl"
TILE_RENDER_TYPES_H = ROOT / "modules" / "gaussian_splatting" / "renderer" / "tile_render_types.h"
PAYLOAD_LAYOUTS = (("full", {}, "Payload"), ("packed", {"GS_PACKED_STAGE_DATA": ""}, "PackedPayload"))


def _f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _host_payload_words(struct_name: str) -> int:
    text = TILE_RENDER_TYPES_H.read_text(encoding="utf-8")
    m = re.search(r"struct\s+alignas\(4\)\s+" + struct_name + r"\s*\{\s*uint32_t\s+data\[(\d+)\];", text)
    if not m:
        raise AssertionError(f"TileProjectionLayout::{struct_name} not found in {TILE_RENDER_TYPES_H}")
    return int(m.group(1))


# Centres across a 4K frame and well off-screen (large splats overlapping the edge keep an
# off-screen centre). fp16 spacing is 1 px in [1024, 2048), 2 px in [2048, 4096) and it
# overflows above 65504, so most of these were snapped or lost by the old encoding.
SCREEN_CENTRES = [
    (0.5, 0.5), (12.125, 7.875), (511.3, 1023.6), (1024.5, 1079.5), (1500.3, 1079.6),
    (1919.4, 1079.6), (2047.75, 2049.25), (3001.2, 2159.4), (3839.9, 0.25), (-37.3, 5120.6),
    (-6000.25, 70000.5), (1800.25 + 0.25, 900.0 - 0.25),
] + [(7.0 + i * 3.7131, 2160.0 - i * 1.9173) for i in range(0, 2000, 7)]


class PayloadScreenCentreIsExact(unittest.TestCase):
    """#1153: packHalf2x16 in pixels snapped centres to 1 px above x = 1024."""

    def _round_trip(self, program, centre):
        vec2, vec3 = program.namespace["vec2"], program.namespace["vec3"]
        payload = program.struct("ProjectedGaussian")()
        program.fn("gs_pack_projected_gaussian")(
            payload, vec2(*centre), 12.5, 0.75, vec3(0.25, 0.5, 1.0), vec3(0.0, 0.6, 0.8),
            vec3(0.125, -0.0625, 0.5), glsl.U32(4321))
        for index, word in enumerate(payload.data):
            self.assertIsInstance(word, glsl.U32, f"payload word {index} was never written")
        result = program.fn("gs_unpack_projected_gaussian")(payload, None, None, None, None, None, None, None)
        _ret, screen_pos, depth, opacity, color, normal, conic, global_idx = result
        return payload, screen_pos, depth, opacity, color, normal, conic, global_idx

    def test_glsl_payload_word_count_matches_the_host_mirror(self) -> None:
        for label, defines, host_struct in PAYLOAD_LAYOUTS:
            program = glsl.load(TILE_PROJECTION_COMMON, defines)
            payload = program.struct("ProjectedGaussian")()
            with self.subTest(layout=label):
                self.assertEqual(len(payload.data), _host_payload_words(host_struct),
                                 f"ProjectedGaussian ({label}) and TileProjectionLayout::{host_struct} "
                                 "disagree on the payload size")

    def test_screen_centre_round_trips_bit_exactly(self) -> None:
        for label, defines, _host in PAYLOAD_LAYOUTS:
            program = glsl.load(TILE_PROJECTION_COMMON, defines)
            worst = 0.0
            for centre in SCREEN_CENTRES:
                _payload, screen_pos, *_rest = self._round_trip(program, centre)
                expected = (_f32(centre[0]), _f32(centre[1]))
                error = max(abs(screen_pos.x - expected[0]), abs(screen_pos.y - expected[1]))
                worst = max(worst, error)
            with self.subTest(layout=label):
                self.assertEqual(worst, 0.0, f"{label} payload moves splat centres by up to {worst} px")

    def test_other_payload_fields_survive_next_to_the_centre(self) -> None:
        # The centre took a new word; nothing may alias it.
        for label, defines, _host in PAYLOAD_LAYOUTS:
            program = glsl.load(TILE_PROJECTION_COMMON, defines)
            _payload, screen_pos, depth, opacity, color, normal, conic, global_idx = self._round_trip(
                program, (1919.4, 1079.6))
            with self.subTest(layout=label):
                self.assertEqual(depth, 12.5)
                self.assertAlmostEqual(opacity, 0.75, delta=0.5 / 255.0)
                self.assertEqual(int(global_idx), 4321)
                self.assertEqual((conic.x, conic.z), (0.125, 0.5))
                self.assertAlmostEqual(conic.y, -0.0625, places=4)
                self.assertAlmostEqual(normal.y, 0.6, places=3)
                self.assertAlmostEqual(normal.z, 0.8, places=3)
                for got, want in zip((color.x, color.y, color.z), (0.25, 0.5, 1.0)):
                    self.assertAlmostEqual(got, want, delta=want / 32.0)


COMPUTE_DIR = ROOT / "modules" / "gaussian_splatting" / "compute"
CAMERA_RELATIVE = INCLUDES_DIR / "gs_camera_relative.glsl"
INSTANCE_LAYOUT = INCLUDES_DIR / "gs_instance_layout.glsl"


def _camera_relative_program(hlod: bool = True) -> "glsl.GlslProgram":
    """gs_camera_relative.glsl as the GPU compiles it. The evaluator models no #include, so the instance flag
    constants are taken verbatim from gs_instance_layout.glsl and the include line is dropped."""
    layout = INSTANCE_LAYOUT.read_text(encoding="utf-8")
    consts = "".join(m.group(0) + "\n" for m in re.finditer(r"^const uint GS_INSTANCE_FLAG_\w+ = [^;]+;", layout, re.M))
    body = "".join(line + "\n" for line in CAMERA_RELATIVE.read_text(encoding="utf-8").splitlines()
                   if not line.strip().startswith("#include"))
    return glsl.GlslProgram(consts + body, {"GS_HLOD_RELATIVE_PAYLOAD": "1"} if hlod else None, origin=str(CAMERA_RELATIVE))


def _f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _quat_axis_angle(axis: tuple[float, float, float], angle: float) -> tuple[float, float, float, float]:
    n = math.sqrt(sum(a * a for a in axis))
    s = math.sin(angle / 2.0) / n
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2.0))


def _rotate(q: tuple[float, float, float, float], v: tuple[float, float, float]) -> tuple[float, float, float]:
    x, y, z, w = q
    t = (2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]))
    c = (y * t[2] - z * t[1], z * t[0] - x * t[2], x * t[1] - y * t[0])
    return (v[0] + w * t[0] + c[0], v[1] + w * t[1] + c[1], v[2] + w * t[2] + c[2])


class HlodCameraRelativeFrame(unittest.TestCase):
    """Package C2: the camera-relative frame of depth_compute.glsl / tile_binning.glsl, evaluated from the shader
    source (gs_camera_relative.glsl) and compared against a float64 reference at 0, 1 km and 100 km.

    Scope: glsl_host_eval does not emulate float32 arithmetic (only storage boundaries round), so these are algebra
    checks of the frame (what the shader computes, with float32 inputs) and regex source checks of the wiring. They
    do not measure the precision of the GPU's float operations."""

    BASES = (0.0, 1000.0, 100000.0)

    def _case(self, base: float):
        q = _quat_axis_angle((0.3, 1.0, -0.2), 0.7)
        s = 2.0
        c = (base + 3.25, 7.5, -0.5 * base + 1.125)          # node centre in asset space (double)
        cam = (base + 0.3, 1.0, -0.5 * base + 9.0)           # precise camera (double)
        # Near detail: the instance is placed so the node sits a few metres from the camera.
        rs_c = _rotate(q, tuple(s * v for v in c))
        t = tuple(cam[i] + (2.0, -1.0, -6.0)[i] - rs_c[i] for i in range(3))  # instance translation T (double)
        return q, s, t, c, cam

    def test_matches_double_reference_at_every_offset(self) -> None:
        program = _camera_relative_program()
        vec3, vec4, uint = program.namespace["vec3"], program.namespace["vec4"], program.namespace["uint"]
        fn = program.fn("gs_hlod_camera_relative_position")
        local_points = [(0.0, 0.0, 0.0), (1.5, -2.25, 0.125), (-3.0, 4.0, 2.5), (0.001, 0.002, -0.003)]
        for base in self.BASES:
            q, s, t, c, cam = self._case(base)
            rs_c = _rotate(q, tuple(s * v for v in c))
            offset = tuple(_f32(rs_c[i] + t[i] - cam[i]) for i in range(3))  # double on the CPU, float last
            worst_precise = worst_naive = 0.0
            for p in local_points:
                p32 = tuple(_f32(v) for v in p)
                got = fn(vec3(*p32), uint(0), vec4(*q), s, vec3(*offset)).c
                # Truth: R s (c + p) + T - camera, all in double.
                exact = _rotate(q, tuple(s * (c[i] + p32[i]) for i in range(3)))
                truth = tuple(exact[i] + t[i] - cam[i] for i in range(3))
                worst_precise = max(worst_precise, max(abs(got[i] - truth[i]) for i in range(3)))
                # Base path: absolute position rounded to float32 first, camera subtracted last.
                absolute = tuple(_f32(exact[i] + t[i]) for i in range(3))
                naive = tuple(absolute[i] - _f32(cam[i]) for i in range(3))
                worst_naive = max(worst_naive, max(abs(naive[i] - truth[i]) for i in range(3)))
            self.assertLessEqual(worst_precise, 4.0e-6, f"base {base}")
            if base == 0.0:
                self.assertLess(worst_naive, 1.0e-4)
            if base >= 100000.0:
                self.assertGreater(worst_naive, 1.0e-3, "the absolute float path must be visibly wrong here")

    def test_identity_placement_is_a_plain_float_add(self) -> None:
        program = _camera_relative_program()
        vec3, vec4, uint = program.namespace["vec3"], program.namespace["vec4"], program.namespace["uint"]
        flags = uint(0x2 | 0x4 | 0x8)  # rotation, scale, translation identity
        local = (1.25, -2.5, 0.75)
        offset = (-3.5, 0.25, -10.0)
        got = program.fn("gs_hlod_camera_relative_position")(vec3(*local), flags, vec4(0.0, 0.0, 0.0, 1.0), 7.0, vec3(*offset)).c
        # Identity flags skip rotation and scale entirely (a non-unit scale value must not leak in).
        self.assertEqual(got, [_f32(local[i] + offset[i]) for i in range(3)])

    def test_instance_translation_is_not_added_twice(self) -> None:
        # T lives inside camera_offset. The function has no translation parameter at all.
        signature = re.search(r"gs_hlod_camera_relative_position\(([^)]*)\)", CAMERA_RELATIVE.read_text(encoding="utf-8"))
        self.assertIsNotNone(signature)
        self.assertNotIn("translation", signature.group(1))

    def test_origin_zero_matches_the_base_formulation_within_reassociation(self) -> None:
        # Base: view = V * (R s (p + c_f) + T, 1) with V = R_v (x - cam) (translation -R_v cam).
        # C2:   view = R_v * (R s p + (R s c + T - cam)). Same value, different association. NOT a float32
        # emulation of the shader: glsl_host_eval runs every operation in Python double; only the storage values
        # (positions, offset) and the old formulation's intermediates are rounded to float32 by this test. This is an
        # algebra check (the two formulations agree up to the rounding the test injects), not a measurement of GPU
        # float behaviour; that belongs to the end-of-stack GPU gate.
        program = _camera_relative_program()
        vec3, vec4, uint = program.namespace["vec3"], program.namespace["vec4"], program.namespace["uint"]
        fn = program.fn("gs_hlod_camera_relative_position")
        q = (0.0, 0.0, 0.0, 1.0)
        rv = _quat_axis_angle((0.2, 1.0, 0.1), 0.4)
        cam = (2.0, 1.5, 8.0)
        c = (3.25, 0.5, -4.0)
        offset = tuple(_f32(c[i] - cam[i]) for i in range(3))
        worst_rel = 0.0
        points = [(-3.0 + 0.37 * i, 1.1 * math.sin(i), 2.0 - 0.21 * i) for i in range(40)]
        for p in points:
            p32 = tuple(_f32(v) for v in p)
            new_pos = fn(vec3(*p32), uint(0x2 | 0x4 | 0x8), vec4(*q), 1.0, vec3(*offset)).c
            new_view = _rotate(rv, tuple(_f32(v) for v in new_pos))
            old_world = tuple(_f32(p32[i] + _f32(c[i])) for i in range(3))
            old_view = _rotate(rv, tuple(_f32(old_world[i] - cam[i]) for i in range(3)))
            scale = max(abs(v) for v in (*old_world, *cam, *p32, 1.0))
            worst_rel = max(worst_rel, max(abs(new_view[i] - old_view[i]) for i in range(3)) / scale)
        self.assertLessEqual(worst_rel, 4.0e-7)

    def test_view_transform_drops_the_translation_only_for_hlod(self) -> None:
        hlod = _camera_relative_program(True)
        plain = _camera_relative_program(False)
        self.assertEqual(hlod.macros["GS_POSITION_FRAME_W"], "0.0")
        self.assertEqual(plain.macros["GS_POSITION_FRAME_W"], "1.0")

    def test_consumers_are_wired_to_the_frame(self) -> None:
        depth = (COMPUTE_DIR / "depth_compute.glsl").read_text(encoding="utf-8")
        binning = (SHADERS_DIR / "tile_binning.glsl").read_text(encoding="utf-8")
        for name, text in (("depth_compute", depth), ("tile_binning", binning)):
            # The node centre in ChunkMetaGPU is no longer a shader input.
            self.assertNotRegex(text, r"node_center_[xyz]", name)
            # Every use of a view matrix on a position goes through the frame's w.
            self.assertNotRegex(text, r"view_matrix\s*\*\s*vec4\([^)]*,\s*1\.0\)", name)
            self.assertIn("gs_hlod_camera_relative_position", text, name)
            self.assertIn("frame_anchor", text, name)
            self.assertIn("frame_camera", text, name)
        # The distance cull and the SH view direction measure from the frame's camera, not from the uniform.
        self.assertNotRegex(binning, r"camera_position\.xyz\s*-\s*g\.position")
        self.assertNotRegex(binning, r"g\.position\s*-\s*params\.camera_position")
        self.assertNotRegex(depth, r"world_position\s*-\s*params\.camera_position_ortho")
        # Wind phase and effector spheres take the anchor (the deformation functions are outside the evaluator's
        # subset: ternaries, continue, pow/sin, so this part is a source check).
        deformation = (INCLUDES_DIR / "gs_deformation.glsl").read_text(encoding="utf-8")
        self.assertIn("gs_wind_displacement(world_position + frame_anchor", deformation)
        self.assertIn("effector_sphere.xyz -= frame_anchor", deformation)


class ShadowCasterCurrentRowsTests(unittest.TestCase):
    """P3: run the offset expression wired into the real caster main, including its old-order control."""

    def _offset_program(self):
        source = (SHADERS_DIR / "gs_shadow_caster.glsl").read_text(encoding="utf-8")
        main = source[source.index("void main() {"):]
        match = re.search(r"vec3 frame_position\s*=\s*gs_hlod_camera_relative_position\(\s*local_position,\s*"
                          r"instance_flags,\s*instance\.rotation,\s*uniform_scale,\s*(.*?)\);", main, re.S)
        self.assertIsNotNone(match, "caster position consumer disappeared")
        expression = match.group(1)
        for original, parameter in (("drawn.instance_id", "row"), ("drawn.camera_offset.xyz", "drawn_offset"),
                                    ("instance.translation_scale.xyz", "translation"),
                                    ("deformation_params.frame_anchor.xyz", "frame_camera")):
            expression = expression.replace(original, parameter)
        helper = re.search(r"vec3 gs_caster_camera_offset\([^{}]+\)\s*\{.*?\n\}", source, re.S)
        # Before P3 the actual expression is simply drawn.camera_offset.xyz. Evaluate it too:
        # the moving-row assertion below must go RED numerically, not error on a missing new helper.
        return glsl.GlslProgram((helper.group(0) if helper else "") +
                               "\nvec3 wired_offset(uint row, vec3 drawn_offset, vec3 translation, vec3 frame_camera) {"
                               "return " + expression + "; }")

    def _positions(self, previous_camera, current_camera, current_row, previous_row, *, rows_before_shadow=True):
        offset_program = self._offset_program()
        vec3 = offset_program.namespace["vec3"]
        current_q, current_scale, current_translation = current_row
        q, scale, translation = current_row if rows_before_shadow else previous_row
        previous_translation = previous_row[2]
        drawn_offset = tuple(previous_translation[i] - previous_camera[i] for i in range(3))
        offset = offset_program.fn("wired_offset")(glsl.U32(1), vec3(*drawn_offset),
                                                   vec3(*translation), vec3(*previous_camera))
        camera_program = _camera_relative_program()
        v3, v4 = camera_program.namespace["vec3"], camera_program.namespace["vec4"]
        local = (0.25, -0.5, 0.125)
        position = camera_program.fn("gs_hlod_camera_relative_position")(
            v3(*local), glsl.U32(0), v4(*q), scale, v3(*offset.c)).c
        caster_world = tuple(position[i] + previous_camera[i] for i in range(3))
        # The colour view forms this frame's offset against its own camera.
        colour_offset = tuple(current_translation[i] - current_camera[i] for i in range(3))
        colour = camera_program.fn("gs_hlod_camera_relative_position")(
            v3(*local), glsl.U32(0), v4(*current_q), current_scale, v3(*colour_offset)).c
        colour_world = tuple(colour[i] + current_camera[i] for i in range(3))
        return caster_world, colour_world

    def _assert_same_frame(self, caster, colour):
        for actual, expected in zip(caster, colour):
            self.assertAlmostEqual(actual, expected, places=6, msg="caster must use colour frame N, not frame N-1")

    def test_moving_rigid_row_matches_same_colour_frame_with_previous_list_camera(self):
        previous_row = (_quat_axis_angle((0.3, 1.0, -0.2), -0.2), 0.75, (-1.0, 2.0, -7.0))
        current_row = (_quat_axis_angle((0.3, 1.0, -0.2), 0.7), 1.5, (1.0, 3.0, -6.0))
        for cameras in (((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)), ((17.0, 2.0, -4.0), (18.0, 3.0, -2.0))):
            with self.subTest(cameras=cameras):
                self._assert_same_frame(*self._positions(*cameras, current_row, previous_row))

    def test_forcing_rows_after_shadow_fails_the_same_frame_assertion(self):
        previous_row = (_quat_axis_angle((0.3, 1.0, -0.2), -0.2), 0.75, (-1.0, 2.0, -7.0))
        current_row = (_quat_axis_angle((0.3, 1.0, -0.2), 0.7), 1.5, (1.0, 3.0, -6.0))
        with self.assertRaises(AssertionError):
            self._assert_same_frame(*self._positions((17.0, 2.0, -4.0), (18.0, 3.0, -2.0),
                                                    current_row, previous_row, rows_before_shadow=False))

    def test_world_keeps_previous_draw_offset_instead_of_instance_translation(self):
        program = self._offset_program()
        vec3 = program.namespace["vec3"]
        drawn = (-2.25, 1.5, -8.0)
        got = program.fn("wired_offset")(glsl.U32(0), vec3(*drawn), vec3(123.0), vec3(19.0))
        self.assertEqual(got.c, list(drawn), "world centres are encoded only in the previous draw list")

    def test_current_rows_refresh_is_wired_before_light_cache_decisions_and_shadow_plan(self):
        host = (ROOT / "modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        collect = host[host.index("void GaussianSplatRenderer::collect_hlod_shadow_refresh_owners("):
                       host.index("bool GaussianSplatRenderer::_render_hlod_shadow_casters(")]
        cast = host[host.index("bool GaussianSplatRenderer::_render_hlod_shadow_casters("):
                    host.index("bool GaussianSplatRenderer::render_shadow_depth_map(")]
        self.assertIn("refresh_hlod_shadow_rows()", collect, "positional cache must observe current rows before deciding reuse")
        self.assertIn("refresh_hlod_shadow_rows()", cast, "directional/first shadow pass must upload current rows")
        self.assertLess(collect.index("refresh_hlod_shadow_rows()"), collect.index("index.rebuild("))
        self.assertLess(cast.index("refresh_hlod_shadow_rows()"), cast.index("plan_pass("))
        self.assertIn("current_rows_refusal(", host, "row-key/asset mismatches must gate the real shadow submission")
        self.assertIn("Refusal::ROWS_MISMATCH", host)

    def test_shadow_and_colour_reuse_one_row_snapshot_per_server_frame(self):
        renderer = ROOT / "modules/gaussian_splatting/renderer"
        host = (renderer / "gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        publisher = (renderer / "resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        shadow = host[host.index("gs_shadow_caster::Refusal GaussianSplatRenderer::refresh_hlod_shadow_rows()"):
                      host.index("void GaussianSplatRenderer::collect_hlod_shadow_refresh_owners(")]
        colour = publisher[publisher.index("bool GaussianSplatRenderer::refresh_hlod_instance_rows("):
                           publisher.index("uint32_t GaussianSplatRenderer::get_hlod_view_splat_cap()")]
        for name, body in (("shadow", shadow), ("colour", colour)):
            self.assertRegex(body, r"if\s*\(\s*hlod_shadow_rows_latched\(\)\s*\)\s*\{[^{}]*\breturn\b",
                             name + ": a late director edit must not change this frame's already shadowed rows")
            self.assertLess(body.index("hlod_shadow_rows_latched()"), body.index("GaussianSplatSceneDirector::get_singleton()"))
        self.assertLess(colour.index("hlod_shadow_rows_latched()"), colour.index("build_hlod_instance_rows_for_renderer("))
        self.assertLess(colour.index("hlod_shadow_rows_latched()"), colour.index("rd->buffer_update("))
        self.assertIn("bool GaussianSplatRenderer::hlod_shadow_rows_latched() const", host)
        self.assertIn("rows_frame_latched(", host, "real latch must use the tested frame/bundle predicate")

    def test_changed_latched_contract_clears_bindings_and_cannot_fall_through_to_fresh_canonical_rows(self):
        publisher = (ROOT / "modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")

        def block_after(pattern, source):
            match = re.search(pattern + r"\s*\{", source)
            self.assertIsNotNone(match, "missing terminal refusal block: " + pattern)
            start = match.end() - 1
            depth = 1
            cursor = start + 1
            while cursor < len(source) and depth:
                depth += (source[cursor] == "{") - (source[cursor] == "}")
                cursor += 1
            self.assertEqual(depth, 0, "unterminated refusal block")
            return source[start + 1:cursor - 1], start

        attempt = publisher.index("try_publish_hlod_resident_contract(hlod_reason);")
        outer = publisher[attempt:]
        refusal, position = block_after(r"if\s*\(\s*hlod_result\s*<\s*0\s*\|\|\s*"
                                        r"p_renderer->hlod_shadow_colour_refused\(\)\s*\|\|\s*"
                                        r"\(hlod_result\s*==\s*0\s*&&\s*p_renderer->hlod_shadow_rows_latched\(\)\)\s*\)", outer)
        self.assertIn("clear_instance_pipeline_buffers();", refusal, "terminal refusal must discard a previously bound contract")
        self.assertRegex(refusal, r"return\s+false\s*;")
        self.assertLess(refusal.index("clear_instance_pipeline_buffers();"), refusal.index("return false;"))
        self.assertLess(position, outer.index("hlod_edit_committed_here()"), "edited-world fallback must not bypass terminal refusal")
        self.assertLess(position, outer.index("resident_atlas_override_mb"), "canonical fallback must not pack fresh rows after refusal")

        for method, result in (("idle_hlod_streaming_bundle", "false"), ("publish_hlod_bundle", "false"),
                               ("try_publish_hlod_resident_contract", "-1")):
            body, _ = block_after(r"(?:bool|int)\s+GaussianSplatRenderer::" + method + r"\([^{}]*\)", publisher)
            guard, _ = block_after(r"if\s*\(\s*hlod_shadow_colour_refused\(\)\s*\|\|\s*"
                                   r"\(hlod_shadow_rows_latched\(\)\s*&&\s*change\.action\s*!=\s*"
                                   r"gs_hlod_bundle::ContractChangeAction::ROWS_ONLY\)\s*\)", body)
            self.assertIn('"hlod_shadow_rows_contract_changed_within_frame"', guard)
            marker = "state.hlod_shadow_rows_contract_refused_frame = state.hlod_shadow_rows_server_frame;"
            self.assertIn(marker, guard, method + ": nested return-zero paths need a terminal signal lasting this RS frame")
            stamp, _ = block_after(r"if\s*\(\s*!hlod_shadow_colour_refused\(\)\s*\)", guard)
            self.assertIn(marker, stamp, method + ": repeated refusal must preserve a frame retained across resource release")
            self.assertIn("clear_instance_pipeline_buffers();", guard,
                          method + ": ignored prepare failure must not leave a stale contract bound")
            self.assertRegex(guard, r"return\s+" + re.escape(result) + r"\s*;")
            self.assertLess(guard.index(marker), guard.index("clear_instance_pipeline_buffers();"))
            self.assertLess(guard.index("clear_instance_pipeline_buffers();"), guard.index("return " + result + ";"))

        host = (ROOT / "modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        getter, _ = block_after(r"bool\s+GaussianSplatRenderer::hlod_shadow_colour_refused\(\)\s+const", host)
        # The server frame is read through gs_live_rasterizer(), which returns null when no RenderingServer is live.
        self.assertRegex(getter, r"(?:gs_live_rasterizer\(\)->get_frame_number\(\)\s*==\s*state\.hlod_shadow_rows_contract_refused_frame|"
                                r"state\.hlod_shadow_rows_contract_refused_frame\s*==\s*gs_live_rasterizer\(\)->get_frame_number\(\))")
        self.assertIn("return RenderingServer::get_singleton() ? RSG::rasterizer : nullptr;", host)

    def test_release_retains_same_frame_refusal_before_destroying_the_latched_bundle(self):
        publisher = (ROOT / "modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        match = re.search(r"void GaussianSplatRenderer::release_hlod_resources\(bool p_renderer_teardown\)\s*\{(.*?)\n\}", publisher, re.S)
        self.assertIsNotNone(match)
        release = match.group(1)
        # Renderer teardown may outlive RSG::rasterizer (freed without nulling): it neither retains a refusal nor
        # publishes a snapshot that reads the server frame. Every other release keeps the retention below.
        self.assertIn("p_renderer_teardown ? UINT64_MAX :", release)
        self.assertIn("if (!p_renderer_teardown) {\n        publish_hlod_shadow_rows_snapshot();", release)
        retained = re.search(r"retained_refusal_frame\(\s*hlod_shadow_rows_latched\(\),\s*hlod_shadow_colour_refused\(\),\s*"
                             r"state\.hlod_shadow_rows_server_frame,\s*state\.hlod_shadow_rows_contract_refused_frame\)", release)
        self.assertIsNotNone(retained, "resource teardown must capture this frame's terminal refusal before clearing its latch")
        self.assertLess(retained.start(), release.index("release_hlod_bundle("))
        self.assertLess(retained.start(), release.index("state.hlod_shadow_rows_server_frame = UINT64_MAX;"))
        self.assertIn("state.hlod_shadow_rows_contract_refused_frame = refused_frame;", release)

    def test_edited_world_release_cannot_escape_through_final_canonical_fallback(self):
        publisher = (ROOT / "modules/gaussian_splatting/renderer/resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        outer = publisher[publisher.index("try_publish_hlod_resident_contract(hlod_reason);"):]
        match = re.search(r"if\s*\(\s*p_renderer->hlod_shadow_rows_latched\(\)\s*\|\|\s*"
                          r"p_renderer->hlod_shadow_colour_refused\(\)\s*\)\s*\{", outer)
        self.assertIsNotNone(match, "an edited-world release after the first refusal check must still block canonical fallback")
        self.assertGreater(match.start(), outer.index("if (hlod_result != 0 && !omit_edited_world)"))
        self.assertLess(match.start(), outer.index("resident_atlas_override_mb"))
        self.assertLess(match.start(), outer.index("LocalVector<ResidentAssetDescriptor> assets;"))
        start = match.end() - 1
        depth, cursor = 1, start + 1
        while cursor < len(outer) and depth:
            depth += (outer[cursor] == "{") - (outer[cursor] == "}")
            cursor += 1
        self.assertEqual(depth, 0)
        guard = outer[start + 1:cursor - 1]
        self.assertIn("clear_instance_pipeline_buffers();", guard)
        self.assertRegex(guard, r"return\s+false\s*;")
        self.assertLess(guard.index("clear_instance_pipeline_buffers();"), guard.index("return false;"))

    def test_rows_oracles_remain_linked_and_selected_by_required_lanes(self):
        renderer_module = ROOT / "modules/gaussian_splatting"
        compiled = (renderer_module / "SCsub").read_text(encoding="utf-8")
        anchors = (renderer_module / "tests/test_gaussian_splatting.h").read_text(encoding="utf-8")
        self.assertIn('add_source_files(module_sources, "tests/*.cpp")', compiled)
        for stem in ("test_hlod_shadow_casters", "test_hlod_runtime_gpu_identity"):
            self.assertIn(stem + "_cpp_force_link_anchor = " + stem + "_cpp_force_link();", anchors)
        gpu_source = (renderer_module / "tests/test_hlod_runtime_gpu_identity.cpp").read_text(encoding="utf-8")
        names = re.findall(r'TEST_CASE\("([^"\n]+)"\)', gpu_source)
        moving = [name for name in names if "noncontiguous page placement preserves drawn pixels and corrupted mapping changes them (#1514)" in name]
        self.assertEqual(len(moving), 1, "required moving-caster oracle disappeared")
        spec = importlib.util.spec_from_file_location("_p3_gpu_harness", ROOT / "tests/ci/run_gpu_harness.py")
        self.assertIsNotNone(spec)
        harness = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = harness
        spec.loader.exec_module(harness)
        batches = [batch for batch in harness.BATCHES if batch.name == "GpuSorting"]
        self.assertEqual(len(batches), 1)
        self.assertIn("GpuSorting", harness.REQUIRED_BATCHES, "moving-caster GPU oracle must fail a REQUIRED lane")
        def matches(pattern):
            # doctest wildcards treat brackets literally (fnmatch does not).
            return re.fullmatch(re.escape(pattern).replace(r"\*", ".*").replace(r"\?", "."), moving[0], re.I) is not None
        self.assertTrue(any(matches(pattern) for pattern in batches[0].filters))
        self.assertFalse(any(matches(pattern) for pattern in batches[0].excludes))
        runner = (ROOT / "tests/ci/run_module_tests.py").read_text(encoding="utf-8")
        self.assertRegex(runner, r"\(\s*True,\s*_run_shader_host_numerics_guard,")
        self.assertRegex(runner, r'"GaussianSplatting \[HLOD\]",\s*\("\*GaussianSplatting\*\]\[HLOD\]\*",\),\s*\([^()]*\),\s*True,')


class ShadowCasterPerspectiveTests(unittest.TestCase):
    """Evaluate the caster's real quotient rule; also check that its three consumers are wired in."""

    def test_perspective_jacobian_matches_projected_finite_differences(self) -> None:
        text = (SHADERS_DIR / "gs_shadow_caster.glsl").read_text(encoding="utf-8")
        match = re.search(r"vec3 gs_caster_perspective_gradient\([^{}]+\)\s*\{[^{}]+\}", text)
        self.assertIsNotNone(match, "the caster still uses an orthographic Jacobian for positional lights")
        program = glsl.GlslProgram(match.group(0))
        gradient = program.fn("gs_caster_perspective_gradient")
        vec3 = program.namespace["vec3"]
        # Rows of a rotated, translated perspective clip transform. Include both UV and reverse-Z depth rows,
        # at unequal distances and off-axis points so dropping either /w or the quotient's w derivative fails.
        rows = ((1.1, 0.2, -0.3), (-0.1, -0.8, 0.25), (0.002, -0.003, 0.006))
        offsets = (0.7, -0.2, 0.08)
        w_row, w_offset = (0.2, -0.3, -0.9), 2.0
        failures_without_divide = failures_without_w_derivative = 0
        for point in ((0.4, 0.7, -2.0), (-1.2, 0.3, -7.0), (2.1, -1.7, -13.0)):
            clip_w = sum(a * b for a, b in zip(w_row, point)) + w_offset
            for row, offset in zip(rows, offsets):
                ndc = (sum(a * b for a, b in zip(row, point)) + offset) / clip_w
                actual = gradient(vec3(*row), vec3(*w_row), ndc, clip_w).c
                for axis in range(3):
                    step = 1.0e-4
                    def sample(delta: float) -> float:
                        shifted = tuple(value + (delta if i == axis else 0.0) for i, value in enumerate(point))
                        return ((sum(a * b for a, b in zip(row, shifted)) + offset)
                                / (sum(a * b for a, b in zip(w_row, shifted)) + w_offset))
                    expected = (sample(step) - sample(-step)) / (2.0 * step)
                    self.assertAlmostEqual(actual[axis], expected, delta=1.0e-7)
                    failures_without_divide += abs(row[axis] - expected) > 1.0e-5
                    failures_without_w_derivative += abs(row[axis] / clip_w - expected) > 1.0e-5
        self.assertGreater(failures_without_divide, 0)
        self.assertGreater(failures_without_w_derivative, 0)

    def test_perspective_shader_and_host_mode_are_wired(self) -> None:
        shader = (SHADERS_DIR / "gs_shadow_caster.glsl").read_text(encoding="utf-8")
        renderer = ROOT / "modules" / "gaussian_splatting" / "renderer"
        host = (renderer / "gs_shadow_caster.cpp").read_text(encoding="utf-8")
        hook = (renderer / "gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        for axis in ("x", "y", "z"):
            self.assertIsNotNone(re.search(rf"j{axis}\s*=\s*gs_caster_perspective_gradient\([^;]+ndc\.{axis}, clip\.w\)", shader),
                                 f"perspective {axis} gradient consumer is disconnected")
        self.assertTrue("bool perspective = params.counts.z == 2u;" in shader, "shader perspective mode missing")
        self.assertTrue("perspective && clip.w <= 1e-6" in shader, "perspective behind-light rejection missing")
        self.assertTrue("p_inputs.perspective ? 2u : (p_inputs.pancake ? 1u : 0u)" in host, "host mode encoding changed")
        self.assertTrue("inputs.perspective = !r_result.light_pass.orthographic;" in hook, "hook perspective selection missing")
        self.assertTrue("inputs.pancake = !inputs.perspective;" in hook, "positional pancake disabling missing")
        self.assertTrue("sizeof(ShadowCasterPushConstant) == 96" in host, "host packet size changed")
        # Both stages must agree on the unchanged 96-byte layout; no new bindings or include inputs are needed.
        blocks = re.findall(r"layout\(push_constant, std430\) uniform Params\s*\{([^}]+)\}", shader)
        self.assertEqual(len(blocks), 2)
        layouts = [re.findall(r"\b(mat4|vec4|uvec4)\s+(\w+)\s*;", block) for block in blocks]
        self.assertEqual(layouts, [[("mat4", "light_from_frame"), ("vec4", "viewport"), ("uvec4", "counts")]] * 2)

    def test_hlod_cube_shader_consumer_runs_before_shared_cube_copy(self) -> None:
        forward = (ROOT / "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp").read_text(encoding="utf-8")
        start = forward.index("void RenderForwardClustered::_render_shadow_pass(")
        body = forward[start:forward.index("void RenderForwardClustered::_render_shadow_begin()", start)]
        self.assertTrue("GaussianShadowRoute::HLOD" in body, "cube-only lights still cannot reach the HLOD shader")
        mesh_end = body.index("_render_shadow_end();")
        submit = body.index("_gaussian_shadow_dispatch(")
        copy = body.index("copy_effects->copy_cubemap_to_dp(")
        camera_reset = body.index("light_instance_set_shadow_transform(p_light, Projection()")
        self.assertLess(mesh_end, submit)
        self.assertLess(submit, copy)
        self.assertLess(copy, camera_reset)
        self.assertTrue("p_render_data->render_shadows[index].light == p_light" in body, "cube faces mix light IDs")
        self.assertTrue("}, GaussianShadowRoute::CANONICAL);" in forward[:start], "late replay still includes HLOD")
        cull = (ROOT / "servers/rendering/renderer_scene_cull.cpp").read_text(encoding="utf-8")
        self.assertNotIn("has_hlod_shadow_owner", cull, "a visible owner must not dirty every positional light")
        self.assertRegex(cull, r"if \(gs_shadow_update.refresh\)\s*\{[^{}]*light->last_version\+\+;\s*\}\s*#endif\s*bool redraw = .*shadow_atlas_update_light")


class HlodPositionalRefreshTests(unittest.TestCase):
    """P1c: execute host geometry/decision and pin spatial, anchor-free key wiring; strict native cases exercise state."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.renderer = ROOT / "modules/gaussian_splatting/renderer"
        cls.source = (cls.renderer / "hlod_shadow_refresh.h").read_text(encoding="utf-8")
        decision = re.search(r"inline bool needs_refresh\([^{}]+\)\s*\{[^{}]+\}", cls.source)
        if decision is None:
            raise AssertionError("missing production refresh predicate")
        cls.decision = staticmethod(glsl.GlslProgram(decision.group(0).replace("inline ", "")).fn("needs_refresh"))
        geometry = re.search(r"inline bool sphere_intersects_bounds\([^{}]+\)\s*\{([^{}]+)\}", cls.source)
        if geometry is None:
            raise AssertionError("missing production sphere/AABB test")
        # Flatten AABB's two vectors and substitute only host type/builtin names. Every arithmetic operation is real.
        body = geometry.group(1).replace("p_bounds.position", "p_min").replace("p_bounds.size", "p_size")
        body = body.replace("Vector3", "vec3").replace("real_t", "float").replace("MAX", "max")
        cls.geometry = glsl.GlslProgram("bool intersects(vec3 p_center, float p_radius, vec3 p_min, vec3 p_size) {" + body + "}")
        contained = re.search(r"inline bool sphere_contains_bounds\([^{}]+\)\s*\{([^{}]+)\}", cls.source)
        if contained is None:
            raise AssertionError("missing production cell-enclosure fast path")
        body = contained.group(1).replace("p_bounds.position", "p_min").replace("p_bounds.size", "p_size")
        body = body.replace("Vector3", "vec3").replace("real_t", "float").replace("MAX", "max").replace("Math::abs", "abs")
        cls.containment = glsl.GlslProgram("bool contains(vec3 p_center, float p_radius, vec3 p_min, vec3 p_size) {" + body + "}")
        prepare = cls.source[cls.source.index("inline LightUpdate prepare_light("):]
        removal = re.search(r"if \(([^\n]+)\) \{ update.refresh = true; \}", prepare)
        if removal is None:
            raise AssertionError("missing production observation-set change check")
        # Execute the production set-size condition, including its old empty-set exclusion in the RED control.
        condition = removal.group(1).replace("update.owners.is_empty()", "(current_count == 0u)")
        condition = condition.replace("update.owners.size()", "current_count").replace("p_state.last_pass.size()", "previous_count")
        cls.removal = staticmethod(glsl.GlslProgram(
            "bool removal_refresh(uint previous_count, uint current_count) { return " + condition + "; }").fn("removal_refresh"))

    def test_static_then_changed_or_animated_content(self) -> None:
        decide = self.decision
        self.assertTrue(decide(True, False, False, False), "first static pass")
        self.assertFalse(decide(True, True, False, False), "static next frame must reuse")
        self.assertTrue(decide(True, True, True, False), "published cut or transform changed")
        self.assertTrue(decide(True, True, False, True), "active casting-row deformation")
        for seen in (False, True):
            for changed in (False, True):
                for animated in (False, True):
                    self.assertFalse(decide(False, seen, changed, animated), "no owner intersection/non-HLOD")

    def test_omni_sphere_rejects_outside_range_and_includes_tangency(self) -> None:
        vec3 = self.geometry.namespace["vec3"]
        intersects = self.geometry.fn("intersects")
        bounds = (vec3(-1, -1, -1), vec3(2, 2, 2))
        for center, radius, expected in (((0, 0, 0), 0, True), ((3, 0, 0), 2, True),
                                          ((3.01, 0, 0), 2, False), ((2, 2, 2), 1, False),
                                          ((20, 0, 0), 2, False)):
            self.assertEqual(intersects(vec3(*center), radius, *bounds), expected)

    def test_hidden_owner_clears_once_after_a_committed_pass(self) -> None:
        self.assertTrue(self.removal(glsl.U32(1), glsl.U32(0)), "hidden/freed/noncasting/non-HLOD owner must clear its old map")
        self.assertFalse(self.removal(glsl.U32(0), glsl.U32(0)), "committing the empty observation set ends the refresh")

    def test_owner_moved_out_of_range_clears_once(self) -> None:
        vec3 = self.geometry.namespace["vec3"]
        intersects = self.geometry.fn("intersects")
        self.assertTrue(intersects(vec3(0, 0, 0), 2, vec3(-1, -1, -1), vec3(2, 2, 2)))
        self.assertFalse(intersects(vec3(0, 0, 0), 2, vec3(20, -1, -1), vec3(2, 2, 2)))
        self.assertTrue(self.removal(glsl.U32(1), glsl.U32(0)))
        self.assertFalse(self.removal(glsl.U32(0), glsl.U32(0)))

    def test_light_without_previous_observations_never_clears(self) -> None:
        self.assertFalse(self.removal(glsl.U32(0), glsl.U32(0)))
        self.assertFalse(self.decision(False, False, False, False))

    def test_refresh_pipeline_wiring_and_lifetime_are_connected(self) -> None:
        cull = (ROOT / "servers/rendering/renderer_scene_cull.cpp").read_text(encoding="utf-8")
        host = (self.renderer / "gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        publisher = (self.renderer / "resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        self.assertNotIn("has_hlod_shadow_owner", cull)
        self.assertIn("collect_hlod_shadow_refresh_owners(scene_cull_result.gaussian_splats, gs_shadow_owners)", cull)
        self.assertIn("prepare_hlod_shadow_light(light->instance, gs_shadow_owners,", cull)
        self.assertIn("ins->transform.origin, RSG::light_storage->light_get_param(ins->base, RS::LIGHT_PARAM_RANGE)", cull)
        self.assertIn("RS::LIGHT_PARAM_SPOT_ANGLE) : -1.0f", cull)
        self.assertRegex(cull, r"if \(gs_shadow_update.refresh\)\s*\{[^{}]*light->last_version\+\+;")
        self.assertRegex(cull, r"if \(max_shadows_used > gs_shadow_passes_before\)\s*\{\s*GaussianSplatRenderer::commit_hlod_shadow_light")
        self.assertLess(cull.index("forget_hlod_shadow_light(light->instance)"), cull.index("light_instance_free(light->instance)"))
        collect = host[host.index("void GaussianSplatRenderer::collect_hlod_shadow_refresh_owners("):host.index("bool GaussianSplatRenderer::_render_hlod_shadow_casters(")]
        for required in ("gaussian_get_casts_shadow(owner_rid)", "!renderer->uses_hlod_shadow_casters()",
                         "record.bundle_builds != state.hlod_bundle_builds", "record.draw_list_upload != state.hlod_draw_list_uploads",
                         "record.row_casts.size() != state.hlod_instance_row_data.instances.size()",
                         "owner.bounds = storage->gaussian_get_aabb(owner_rid)",
                         "gs_wind::wind_active_on_casting_row(wind, rows[row], casts)",
                         "effectors_active_on_casting_row(rows[row], casts, effects.effector_meta,",
                         "pack_deformation_effectors", "owner.bounds.grow_by",
                         "frame_displacement_bound(wind, &rows[row], 1, effector_push",
                         "index.rebuild(state.hlod_shadow_content, record, rows, row_margins, row_animated)",
                         "owner.content = &index", "indexed_renderers.has(renderer_key)", "indexed_renderers.insert(renderer_key)",
                         "owner.owner = renderer_key", "gs_shadow_refresh::enclosing_merge(owner.bounds, index.cells[0].bounds)"):
            self.assertIn(required, collect)
        self.assertNotRegex(collect, r"owner\.revision\s*[+^|*/-]?=", "collector must not supply a global list key")
        self.assertIn("(p_meta[3] > 0.5f && (mask & (1u << slot)) == 0)", self.source)
        self.assertIn("if (!p_casts) { return false; }", self.source)
        self.assertIn("hlod_shadow_lights.erase(p_light.get_id())", host)
        self.assertIn("if (p_update.owners.is_empty()) { hlod_shadow_lights.erase(p_light.get_id()); return; }", host)
        self.assertIn("state.hlod_shadow_content.publish(caster, draws, geometry)", publisher)
        self.assertIn("outcome.records[i].version.content_revision", publisher)
        self.assertIn("outcome.records[i].version.bake_rule_revision", publisher)
        self.assertIn("g.payload = gs_shadow_refresh::hash_value(world_shadow_versions[i], source->source_epoch)", publisher)
        self.assertIn("geometry.chunk_key = k - region.assets[meta.asset_id - 1u].first_chunk", publisher)
        self.assertIn("g.bounds = gs_shadow_refresh::enclosing_bounds(lo, hi)", publisher)
        self.assertIn("state.hlod_relative.shadow_instance_geometry[chunk - uint32_t(source->nodes.size())]", publisher)
        self.assertIn("p_world_records ? state.hlod_relative.draw_entries.ptr() : state.hlod_instance_draws.ptr()", publisher)
        self.assertNotIn("previous.camera ==", self.source)
        self.assertNotIn("equal_bytes(draws, casting_draws.ptr(), casting_draws.size())", self.source)
        self.assertNotIn("previous.draw_list_upload ==", self.source)
        runner = (ROOT / "tests/ci/run_module_tests.py").read_text(encoding="utf-8")
        self.assertRegex(runner, r"\(\s*True,\s*_run_shader_host_numerics_guard,")
        self.assertTrue(re.search(r'"GaussianSplatting \[HLOD\]",\s*\("\*GaussianSplatting\*\]\[HLOD\]\*",\),\s*\([^()]*\),\s*True,', runner),
                        "native HLOD regressions must remain in the strict host lane")

    def test_camera_and_whole_list_cannot_enter_per_light_key(self) -> None:
        self.assertIn("inline AABB record_bounds(", self.source, "P1c requires world-space per-record content")
        publish = self.source[self.source.index("struct PublishedContent {"):self.source.index("inline AABB record_bounds(")]
        self.assertIn("for (float &offset : draws[i].camera_offset) { offset = 0; }", publish)
        digest = self.source[self.source.index("inline uint64_t record_digest("):self.source.index("inline uint64_t row_digest(")]
        for forbidden in ("camera", "bundle_builds", "source_revision", "draw_list_upload", "instance_id", "content_revision", "p_draw.chunk_id"):
            self.assertNotIn(forbidden, digest)
        for required in ("p_geometry.chunk_key", "p_geometry.payload", "p_row_digest", "p_geometry.bounds"):
            self.assertIn(required, digest)
        prepare = self.source[self.source.index("inline LightUpdate prepare_light("):]
        self.assertIn("owner.revision = 0;", prepare)
        self.assertEqual(re.findall(r"owner\.revision\s*[+^|*/-]?=", prepare), ["owner.revision ="])
        self.assertNotIn("source.revision", prepare, "light-local digest must not mix in a whole-owner/list revision")
        self.assertIn("owner.animated = false;", prepare)
        self.assertIn("source.content->query(0, p_center, p_radius, owner.revision, owner.record_count, owner.animated)", prepare)
        self.assertIn("if (owner.record_count == 0) { continue; }", prepare)
        query = self.source[self.source.index("inline void ContentIndex::query("):self.source.index("inline uint64_t record_digest(")]
        self.assertIn("if (!sphere_intersects_bounds(p_center, p_radius, cell.bounds)) { return; }", query)
        self.assertIn("if (!sphere_intersects_bounds(p_center, p_radius, record.bounds)) { continue; }", query)
        self.assertIn("r_digest += record.digest;", query)
        self.assertIn("r_animated |= record.animated;", query)
        self.assertIn("if (sphere_contains_bounds(p_center, p_radius, cell.bounds))", query)
        for field in ("digest", "count", "animated"):
            self.assertIn("cell." + field, query)
        self.assertIn("root.add(record)", self.source)
        self.assertIn("cells[child].add(records[i])", self.source)
        self.assertIn("bounds = enclosing_merge(bounds, p_record.bounds)", self.source)
        self.assertIn("row.ids[0] = 0;", self.source)
        self.assertIn("row.lod[0] = row.lod[1] = 0;", self.source)
        self.assertNotIn("p_content", query) # No whole-list fallback in the per-light query.
        rebuild = self.source[self.source.index("inline void ContentIndex::rebuild("):self.source.index("inline bool needs_refresh(")]
        self.assertIn("if (row >= p_rows.size() || !p_record.row_casts[row]) { continue; }", rebuild)
        self.assertIn("p_margins[row]", rebuild)
        self.assertIn("spatial.animated = p_animated[row] != 0;", rebuild)
        self.assertIn("spatial.digest = record_digest(draw, p_content.geometry[i], row_digests[row]);", rebuild)
        # Structural RED on P1b and on removal of native assertion wiring, not a helper that nobody calls.
        native = (ROOT / "modules/gaussian_splatting/tests/test_hlod_shadow_casters.cpp").read_text(encoding="utf-8")
        for case in ("static omni ignores moving anchor and far cut but refreshes near cut",
                     "wind and effectors refresh only casting records near the light",
                     "inflated records enter light and final departure clears once",
                     "static depth at two anchors has the same light clip coordinates",
                     "dense chunk row and legacy camera LOD remaps preserve light content",
                     "large-coordinate record and aggregate bounds retain their positive edge"):
            self.assertIn('TEST_CASE("[GaussianSplatting][HLOD][ShadowCasters] ' + case + '")', native)

    def test_aggregate_cell_requires_complete_sphere_enclosure(self) -> None:
        vec3 = self.containment.namespace["vec3"]
        contains = self.containment.fn("contains")
        self.assertTrue(contains(vec3(0), 2, vec3(-1), vec3(2)))
        self.assertFalse(contains(vec3(0), 1, vec3(-1), vec3(2)), "intersecting does not mean fully contained")
        self.assertFalse(contains(vec3(0), 2, vec3(99, -1, -1), vec3(2)))
        self.assertTrue(contains(vec3(0), 3, vec3(0), vec3(3, 0, 0)))
        self.assertFalse(contains(vec3(0), -1, vec3(0), vec3(0)))

    def test_light_volume_filter_excludes_far_cut_and_animation_with_inflation(self) -> None:
        # Execute production sphere/AABB arithmetic and production refresh predicate, varying light-local inputs.
        vec3 = self.geometry.namespace["vec3"]
        intersects = self.geometry.fn("intersects")
        def hits(x: float, margin: float = 0.0) -> bool:
            return intersects(vec3(0, 0, 0), 2, vec3(x - margin, -1 - margin, -1 - margin),
                              vec3(2 + 2 * margin))
        self.assertTrue(hits(-1))
        self.assertFalse(hits(99))
        for far_changed, far_animated in ((True, False), (False, True), (True, True)):
            self.assertFalse(self.decision(True, True, far_changed and hits(99), far_animated and hits(99)))
        self.assertTrue(self.decision(True, True, hits(-1), False))
        self.assertTrue(self.decision(True, True, False, hits(-1)))
        self.assertFalse(hits(4))
        self.assertTrue(hits(4, 2), "wind/effector bound can reach the light even when rest bounds cannot")


class HlodWindFieldTests(unittest.TestCase):
    """W-S: execute the production position field; reject identity/address inputs."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.source = (INCLUDES_DIR / "gs_deformation.glsl").read_text(encoding="utf-8")
        semantic = (INCLUDES_DIR / "gs_semantic.glsl").read_text(encoding="utf-8")
        cls.program = glsl.GlslProgram(semantic + "\n" + cls.source, defines={
            "GS_INSTANCE_WIND_MODE_FORCE_ENABLED": "2u",
            "GS_INSTANCE_WIND_MODE_FORCE_DISABLED": "1u",
            "GS_INSTANCE_WIND_MODE_SEMANTIC_INHERIT": "3u",
        })
        # Standard GLSL scalar sin, evaluated in double as the rest of this harness.
        cls.program.namespace["sin"] = math.sin

    def test_phase_is_smooth_scene_rest_field_without_identity_inputs(self) -> None:
        noise = self.program.fn("gs_wind_phase_noise")
        vec3 = self.program.namespace["vec3"]
        for position in ((1.2, 7.1, -3.4), (31.999, 0.0, -64.001), (-0.001, 1.0, 32.0)):
            # Relocating this rest point to any atlas slot or row cannot enter the field's API.
            reference = noise(vec3(*position))
            for atlas_index, instance_id in ((0, 0), (413, 21), (900001, 63)):
                self.assertEqual(noise(vec3(*position)), reference, (atlas_index, instance_id))
            nearby = noise(vec3(position[0] + 0.001, position[1], position[2]))
            self.assertLess(abs(reference - nearby), 0.001)
        for boundary in (-64.0, -32.0, 0.0, 32.0, 64.0):
            left = noise(vec3(boundary - 1.0e-4, 0.0, 11.0))
            right = noise(vec3(boundary + 1.0e-4, 0.0, 11.0))
            self.assertAlmostEqual(left, right, delta=1.0e-5)
        self.assertNotIn("stable_seed", self.source)
        self.assertNotIn("phase_jitter", self.source)
        for path in (ROOT / "modules/gaussian_splatting/compute/depth_compute.glsl", SHADERS_DIR / "tile_binning.glsl"):
            self.assertNotIn("uint stable_seed", path.read_text(encoding="utf-8"))

    def test_fade_reaches_zero_at_distance_and_coarse_level(self) -> None:
        fade = self.program.fn("gs_wind_distance_level_fade")
        vec3 = self.program.namespace["vec3"]
        self.assertEqual(fade(vec3(0.0), glsl.U32(0)), 1.0)
        self.assertEqual(fade(vec3(64.0, 0.0, 0.0), glsl.U32(0)), 1.0)
        self.assertEqual(fade(vec3(128.0, 0.0, 0.0), glsl.U32(0)), 0.0)
        self.assertEqual(fade(vec3(0.0), glsl.U32(4)), 0.0)
        self.assertGreater(fade(vec3(96.0, 0.0, 0.0), glsl.U32(2)), 0.0)
        self.assertLess(fade(vec3(96.0, 0.0, 0.0), glsl.U32(2)), 1.0)

    def test_displacement_bound_dominates_signed_wind_and_effector_push(self) -> None:
        bound = self.program.fn("gs_wind_displacement_bound")
        self.assertEqual(bound(2.0, 3.0, 4.0), 30.0)
        self.assertEqual(bound(0.0, 3.0, 4.0), 0.0)
        self.assertEqual(bound(2.0, 0.0, 4.0), 0.0)
        self.assertEqual(bound(-1.0, 3.0, 4.0), 0.0)
        for strength in (1.0, 2.0, 10.0):
            for intensity in (0.0, 0.3, 2.0):
                for push in (0.0, 0.2, 4.0):
                    for phase in (-1.0, 0.0, 1.0):
                        displacement = phase * strength * intensity + phase * push * intensity
                        self.assertLessEqual(abs(displacement), bound(strength, intensity, push))

    def test_caster_and_colour_math_follow_current_frame_time(self) -> None:
        displacement = self.program.fn("gs_wind_displacement")
        vec3 = self.program.namespace["vec3"]
        rest = vec3(1.25, 0.5, -4.0)
        weight = self.program.fn("gs_splat_wind_weight")(glsl.U32(0), 0.75, 0.0)
        previous = displacement(rest, 0.1, 1.0, 0.1, 2.0, weight, 1.0, 1.0)
        colour = displacement(rest, 1.1, 1.0, 0.1, 2.0, weight, 1.0, 1.0)
        caster = displacement(rest, 1.1, 1.0, 0.1, 2.0, weight, 1.0, 1.0)
        self.assertEqual(caster, colour)
        self.assertGreater(abs(caster - previous), 0.01)
        self.assertEqual(displacement(rest, 1.1, 1.0, 0.1, 2.0, weight, 1.0, 0.0), 0.0)

    def test_depth_inflation_retains_wind_reachable_edge_splat(self) -> None:
        # Execute the actual GPU frustum helper and actual displacement bound,
        # with one right plane at x=1. A resting splat centred at x=1.4 can
        # sway into the view; culling its uninflated sphere would pop it out.
        source = (ROOT / "modules/gaussian_splatting/compute/depth_compute.glsl").read_text(encoding="utf-8")
        helper = re.search(r"bool gs_sphere_frustum_visible\([^\n]+\).*?\n\}", source, re.S)
        self.assertIsNotNone(helper)
        program = glsl.GlslProgram(helper.group(0))
        vec3, vec4 = program.namespace["vec3"], program.namespace["vec4"]
        program.namespace["dot"] = lambda a, b: sum(x * y for x, y in zip(a.c, b.c))
        program.namespace["params"] = SimpleNamespace(
            cull_frustum_radius=vec4(1.0, 1.0, 1.0, 0.0),
            frustum_planes=[vec4(1.0, 0.0, 0.0, 1.0)] + [vec4(0.0, 0.0, 0.0, 10.0)] * 5)
        visible = program.fn("gs_sphere_frustum_visible")
        margin = self.program.fn("gs_wind_displacement_bound")(0.5, 1.0, 0.0)
        rest, radius = vec3(1.4, 0.0, 0.0), 0.1
        self.assertFalse(visible(rest, radius), "forcing control must actually cull the rest sphere")
        self.assertTrue(visible(rest, radius + margin), "inflation must preserve the wind-reachable splat")

    def test_colour_depth_and_caster_use_same_rest_field_and_caster_current_time(self) -> None:
        paths = (ROOT / "modules/gaussian_splatting/compute/depth_compute.glsl",
                 SHADERS_DIR / "tile_binning.glsl", SHADERS_DIR / "gs_shadow_caster.glsl")
        for path in paths:
            source = path.read_text(encoding="utf-8")
            self.assertIn('gs_deformation.glsl"', source, path)
            self.assertIn("gs_apply_wind_deformation(", source, path)
            self.assertIn("gs_wind_distance_level_fade(", source, path)
            for field in ("instance.params.z", "instance.params.w", "instance.wind_params", "instance.effect_params"):
                self.assertIn(field, source, path)
        caster = paths[2].read_text(encoding="utf-8")
        self.assertLess(caster.index("GSDeformationResult deformation"), caster.index("params.light_from_frame * vec4(frame_position"))
        self.assertIn("deformation.opacity * instance.params.x", caster)
        self.assertIn("frame_position = deformation.position", caster)
        host = (ROOT / "modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp").read_text(encoding="utf-8")
        start = host.index("bool GaussianSplatRenderer::_render_hlod_shadow_casters(")
        body = host[start:host.index("bool GaussianSplatRenderer::render_shadow_depth_map(", start)]
        self.assertIn("compute_wind_time_seconds(", body)
        self.assertIn("inputs.deformation.wind_time_config[0]", body)
        # The previous view supplies rows/list, never a cached animation timestamp.
        self.assertNotRegex(body, r"record\.(?:wind_time|animation_time)")
        depth = paths[0].read_text(encoding="utf-8")
        self.assertIn("gs_row_deformation_bound(", depth)
        self.assertIn("splat_radius + deformation_margin", depth)

    def test_caster_deformation_buffer_matches_host_without_growing_push_constants(self) -> None:
        shader = (SHADERS_DIR / "gs_shadow_caster.glsl").read_text(encoding="utf-8")
        renderer = ROOT / "modules/gaussian_splatting/renderer"
        header = (renderer / "gs_shadow_caster.h").read_text(encoding="utf-8")
        host = (renderer / "gs_shadow_caster.cpp").read_text(encoding="utf-8")
        shader_block = re.search(r"binding = 6, std430\) readonly buffer DeformationBuffer\s*\{([^}]+)\}", shader)
        host_block = re.search(r"struct DeformationUniform\s*\{(.*?)\n\s*\};", header, re.S)
        self.assertIsNotNone(shader_block)
        self.assertIsNotNone(host_block)
        # Derive field order and array extents from both definitions, including
        # the actual shared effector capacity, rather than checking a spare size.
        layout = (INCLUDES_DIR / "gs_instance_layout.glsl").read_text(encoding="utf-8")
        count = int(re.search(r"#define GS_MAX_SPHERE_EFFECTORS\s+(\d+)", layout).group(1))
        shader_fields = [(name, count if array else 1) for name, array in
                         re.findall(r"vec4\s+(\w+)\s*(\[GS_MAX_SPHERE_EFFECTORS\])?\s*;", shader_block.group(1))]
        # Each host field ends in four floats; nested arrays describe N vec4s.
        host_fields = []
        for name, rows, columns in re.findall(r"float\s+(\w+)\[(\d+)\](?:\[(\d+)\])?", host_block.group(1)):
            self.assertEqual(int(columns or rows), 4)
            host_fields.append((name, int(rows) if columns else 1))
        self.assertEqual(shader_fields, host_fields)
        self.assertEqual(sum(extent * 16 for _, extent in shader_fields), 256)
        self.assertIn("storage_uniform(6, deformation_buffer)", host)
        self.assertIn("_free_deformation_buffer();", host)


class B3WindWeightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (INCLUDES_DIR / "gs_deformation.glsl").read_text(encoding="utf-8")
        cls.semantic = (INCLUDES_DIR / "gs_semantic.glsl").read_text(encoding="utf-8")
        cls.defines = {"GS_INSTANCE_WIND_MODE_FORCE_ENABLED": "2u",
                       "GS_INSTANCE_WIND_MODE_FORCE_DISABLED": "1u",
                       "GS_INSTANCE_WIND_MODE_SEMANTIC_INHERIT": "3u"}
        cls.program = cls.make_program()

    @classmethod
    def make_program(cls, extra=None):
        program = glsl.GlslProgram(cls.semantic + "\n" + cls.source, defines=cls.defines | (extra or {}))
        program.namespace["sin"] = math.sin
        return program

    def weight(self, word, opacity=0.75, mode=3, program=None):
        return (program or self.program).fn("gs_splat_wind_weight")(glsl.U32(word), opacity, float(mode))

    def test_w4_rigid_zero_and_tests_only_legacy_forcing_control(self):
        rigid = 0x80000007
        self.assertEqual(self.weight(rigid), 0.0)
        forced = self.make_program({"GS_TESTS_ENABLED": "1", "GS_TEST_LEGACY_WIND_WEIGHT": "1"})
        self.assertAlmostEqual(self.weight(rigid, program=forced), 0.4)
        shipping = self.make_program({"GS_TEST_LEGACY_WIND_WEIGHT": "1"})
        self.assertEqual(self.weight(rigid, program=shipping), 0.0, "forcing define alone cannot alter production")

    def test_full_half_weights_replace_opacity_and_keep_row_intensity(self):
        p = self.program
        rest = p.namespace["vec3"](1.25, 0.5, -4.0)
        phase = (1.25 - 4.0) * 0.1 + 1.1 + p.fn("gs_wind_phase_noise")(rest)
        oracle = math.sin(phase) * 2.0 * 0.7
        self.assertGreater(abs(oracle), 0.01)
        for opacity in (0.0, 0.1, 0.75, 1.0):
            for weight in (0, 128, 255):
                actual_weight = self.weight(0x80000009 | (weight << 16), opacity)
                self.assertEqual(actual_weight, weight / 255.0)
                actual = p.fn("gs_wind_displacement")(rest, 1.1, 1.0, 0.1, 2.0, actual_weight, 0.7, 1.0)
                self.assertAlmostEqual(actual, oracle * weight / 255.0)

    def test_o4_auto_legacy_on_legacy_and_instance_legacy(self):
        self.assertEqual(self.weight(0), 0.0)
        self.assertEqual(self.weight(0x00ff0000), 0.0, "valid bit, not nonzero wind byte, enables semantics")
        for opacity in (-0.1, 0.0, 0.75, 1.0, 1.1):
            expected = 0.2 + 0.8 * min(1.0, max(0.0, 1.0 - opacity))
            self.assertAlmostEqual(self.weight(0, opacity, 0), expected)  # ON world and default instance.
        self.assertEqual(self.weight(0x80000001), 0.0, "semantic world with no wind input")
        enabled = self.program.fn("gs_is_wind_enabled_for_instance")
        vec4 = self.program.namespace["vec4"]
        self.assertFalse(enabled(1.0, vec4(1, 1, 1, 1)), "OFF must disable semantic wind too")
        self.assertTrue(enabled(3.0, vec4(1, 1, 1, 1)))
        self.assertFalse(enabled(3.0, vec4(1, 1, 1, 0)))

    def test_w2_parent_tracks_weighted_children_below_fade_and_coarse_control_moves(self):
        p = self.program
        vec3 = p.namespace["vec3"]
        displacement = p.fn("gs_wind_displacement")
        self.assertEqual(self.weight(0x80800000), 128.0 / 255.0)
        def move(rest, byte, fade=1.0):
            return displacement(rest, 1.1, 1.0, 0.1, 0.5, self.weight(0x80000000 | (byte << 16)), 1.0, fade)
        parent = vec3(0, 0, -8)
        children = (vec3(-0.005, 0, -8), vec3(0.005, 0, -8))
        pixels_per_unit = 1080.0 / (2.0 * math.tan(math.radians(35.0)) * 8.0)
        child_mean = (move(children[0], 0) + move(children[1], 255)) / 2.0
        self.assertLessEqual(abs(move(parent, 128) - child_mean) * pixels_per_unit, 0.5)
        fade = p.fn("gs_wind_distance_level_fade")(parent, glsl.U32(4))
        self.assertEqual(move(parent, 128, fade), 0.0)
        self.assertGreater(abs(move(parent, 128, 1.0)) * pixels_per_unit, 0.5, "coarse fade forcing must be observable")

    def test_every_payload_route_passes_semantic_to_the_shared_helper(self):
        paths = ("compute/depth_compute.glsl", "shaders/tile_binning.glsl", "shaders/gs_shadow_caster.glsl")
        for name in paths:
            source = (ROOT / "modules/gaussian_splatting" / name).read_text()
            call = source[source.index("GSDeformationResult deformation ="):]
            call = call[:call.index(");")]
            self.assertRegex(call, r"(?:gaussian|g|gaussians\[atlas_index\])\.semantic", name)
            self.assertIn("gs_apply_wind_deformation(", call)
        binning = (SHADERS_DIR / "tile_binning.glsl").read_text()
        self.assertIn("g.semantic = src.semantic;", binning, "quantized colour must copy the semantic word")
        self.assertIn("gs_splat_wind_weight(semantic, opacity, instance_wind_mode)", self.source)
        self.assertIn("* splat_weight", self.source, "position effectors must share W(splat)")
        self.assertNotIn("opacity_scale = max(instance_effect_config.y, 0.0) * splat_weight", self.source)

    def test_effector_position_uses_same_weight_and_opacity_remains_independent(self):
        # Execute the production scalar expressions without modelling buffer loops or result structs.
        for field, expected in (("position_scale", 0.0), ("opacity_scale", 0.7)):
            expression = re.search(r"float " + field + r" = ([^;]+);", self.source).group(1)
            program = glsl.GlslProgram("float sample(vec4 instance_effect_config, float deformation_fade, float splat_weight) { return " + expression + "; }")
            fn = program.fn("sample")
            config = program.namespace["vec4"](2.0, 0.7, 0.0, 0.0)
            self.assertEqual(fn(config, 1.0, 0.0), expected)
            if field == "position_scale":
                self.assertEqual(fn(config, 1.0, 128.0 / 255.0), 2.0 * 128.0 / 255.0)
                self.assertEqual(fn(config, 1.0, 1.0), 2.0)

    def test_node_cut_cull_and_record_estimates_use_bit5_scaled_margin(self):
        source = (ROOT / "modules/gaussian_splatting/lod/hlod_view_set.cpp").read_text()
        body = source.split("bool ViewSet::prepare_displacement_bounds(", 1)[1].split("ViewSet::AdmitResult", 1)[0]
        self.assertIn("gs_wind::node_displacement_bound(local_margin, original.topology.max_wind_weight, original.topology.is_semantic_valid())", body)
        for expression in ("std::sqrt(3.0) * node_margin", "aabb_min[axis]) - node_margin", "aabb_max[axis]) + node_margin",
                           "node.footprint = original.footprint * ratio * ratio"):
            self.assertIn(expression, body)
        header = (ROOT / "modules/gaussian_splatting/renderer/gs_wind_bounds.h").read_text()
        self.assertIn("p_semantic_valid ? double(p_weight) / 255.0 : 1.0", header)

    def test_auto_on_off_api_and_required_gpu_wiring_are_present(self):
        module = ROOT / "modules/gaussian_splatting"
        node = (module / "nodes/gaussian_splat_world_3d.cpp").read_text()
        header = (module / "nodes/gaussian_splat_world_3d.h").read_text()
        self.assertNotIn("set_wind_enabled", node)
        self.assertNotIn("is_wind_enabled", header)
        for mode in ("AUTO", "ON", "OFF"):
            self.assertIn("BIND_ENUM_CONSTANT(WIND_MODE_" + mode + ")", node)
        self.assertIn("WORLD_WIND_MODE_PROPERTY", node)
        self.assertIn('PROPERTY_HINT_ENUM, "Auto,On,Off"', node)
        self.assertIn("WindMode wind_mode = WIND_MODE_AUTO", header)
        for name in ("test_b3_wind.h", "test_b3_wind_gpu.h"):
            self.assertIn('#include "' + name + '"', (module / "tests/test_gaussian_splatting.h").read_text())
        gpu = (module / "tests/test_b3_wind_gpu.h").read_text()
        self.assertIn("[Sort][RequiresGPU][B3]", gpu)
        self.assertIn("rd->texture_get_data(target, 0)", gpu)
        self.assertIn("GS_TESTS_ENABLED 1", gpu)
        self.assertIn("GS_TEST_LEGACY_WIND_WEIGHT 1", gpu)


class StableSplatIdentityTests(unittest.TestCase):
    """WP1: execute the real sort/history expressions, including the address control."""

    def evaluate(self, node=7, local=1029, placement=0x123456789, atlas=3077,
                 row=0, forced=False, tests_enabled=True):
        depth = (ROOT / "modules/gaussian_splatting/compute/depth_compute.glsl").read_text(encoding="utf-8")
        binning = (SHADERS_DIR / "tile_binning.glsl").read_text(encoding="utf-8")
        identity = INCLUDES_DIR / "gs_splat_identity.glsl"
        # Parent code has no identity helper yet; execute its real address expressions
        # so the test-only commit fails assertions, rather than import/translation.
        shared = identity.read_text(encoding="utf-8") if identity.exists() else ""
        culling = (INCLUDES_DIR / "gs_culling_utils.glsl").read_text(encoding="utf-8")
        tie = re.search(r"uint tie_break = ([^;]+);", depth)
        self.assertIsNotNone(tie)
        start = binning.index("    uint history_idx =")
        conditional = binning.rfind("\n#if", 0, start)
        if binning[conditional:start].strip() == "#if defined(GS_HLOD_RELATIVE_PAYLOAD)":
            start = conditional
        end = binning.index("    uint history_packed =", start)
        signature = "uint node, uint local, uvec2 placement, uint atlas_index"
        helper = ("uint identity_hash = gs_splat_identity_hash(node, local, placement);"
                  if shared else "uint identity_hash = 0u;")
        source = shared + culling + f"\nuint tie({signature}) {{ {helper} return {tie.group(1)}; }}\n"
        source += f"uvec2 history({signature}) {{ {helper} uint gaussian_idx = atlas_index; uint history_size = 65521u;\n"
        source += binning[start:end] + "return uvec2(history_idx, history_key); }\n"
        defines = {"GS_HLOD_RELATIVE_PAYLOAD": "1"}
        if tests_enabled:
            defines["GS_TESTS_ENABLED"] = "1"
        if forced:
            defines["GS_TEST_ADDRESS_IDENTITY"] = "1"
        program = glsl.GlslProgram(source, defines=defines)
        program.namespace["splat_ref"] = SimpleNamespace(instance_id=glsl.U32(row))
        args = (glsl.U32(node), glsl.U32(local), program.namespace["uvec2"](placement & 0xffffffff, placement >> 32), glsl.U32(atlas))
        history = program.fn("history")(*args)
        return int(program.fn("tie")(*args)), int(history.x), int(history.y)

    def test_same_identity_survives_noncontiguous_page_relocation_and_row_reorder(self):
        for placement in (0, 0x123456789):
            first = self.evaluate(placement=placement, row=0)
            relocated = self.evaluate(placement=placement, atlas=19461, row=19)
            self.assertEqual(first, relocated)

    def test_distinct_nodes_local_indices_and_placement_keys_discriminate(self):
        first = self.evaluate()
        self.assertEqual(first, (2280985405, 2832, 133501757))
        for changed in ({"node": 8}, {"local": 1030}, {"placement": 0x223456789}):
            other = self.evaluate(**changed)
            self.assertNotEqual(first[0], other[0])
            self.assertNotEqual(first[1], other[1])
            self.assertNotEqual(first[2], other[2])

    def test_address_forcing_control_changes_both_values_and_restores_stability_when_off(self):
        legacy_hash = glsl.load(INCLUDES_DIR / "gs_culling_utils.glsl").fn("gs_hash_u32")
        for row in (0, 19):
            first = self.evaluate(forced=True, row=row)
            relocated = self.evaluate(forced=True, atlas=19461, row=row)
            slot = 3077 if row == 0 else int(legacy_hash(glsl.U32(3077 ^ (row * 0x9e3779b9)))) % 65521
            key = int(legacy_hash(glsl.U32(3077 ^ (row * 0x85ebca6b)))) & 0x7fffffff
            self.assertEqual(first, (3077, slot, key))
            for a, b in zip(first, relocated):
                self.assertNotEqual(a, b)
            self.assertNotEqual(first, self.evaluate(row=row))

    def test_address_forcing_define_is_inert_without_test_build_define(self):
        self.assertEqual(self.evaluate(tests_enabled=False),
                         self.evaluate(forced=True, tests_enabled=False, atlas=19461))

    def test_identity_is_wired_to_real_node_local_index_and_stable_placement(self):
        depth = (ROOT / "modules/gaussian_splatting/compute/depth_compute.glsl").read_text(encoding="utf-8")
        binning = (SHADERS_DIR / "tile_binning.glsl").read_text(encoding="utf-8")
        self.assertIn("gs_splat_identity_hash(visible_chunk.chunk_id, splat_index_in_chunk, instance.placement_key)", depth)
        self.assertIn("gs_splat_identity_hash(selected.chunk_id, local_splat_index, instance.placement_key)", binning)
        self.assertIn("gs_hlod_local_splat_index(", binning)
        self.assertIn("atomicExchange(subpixel_history.entries[history_idx]", binning)
        layout = (INCLUDES_DIR / "gs_instance_layout.glsl").read_text(encoding="utf-8")
        self.assertRegex(layout, r"GS_INSTANCE_DATA_GPU_SIZE\s*=\s*128u;")

    def test_reverse_slot_recovers_local_page_and_rejects_foreign_pages_and_padding(self):
        path = INCLUDES_DIR / "gs_splat_identity.glsl"
        self.assertTrue(path.is_file(), "the identity decoder must exist")
        if not path.is_file():
            return
        source = (INCLUDES_DIR / "gs_hlod_page_constants.glsl").read_text(encoding="utf-8") + path.read_text(encoding="utf-8")
        program = glsl.GlslProgram(source)
        decode = program.fn("gs_hlod_local_splat_index")
        def run(atlas, reverse_slot, table_ref=18, base=0, count=1030):
            valid, local = decode(*(glsl.U32(v) for v in (atlas, reverse_slot, table_ref, base, count, 0)))
            return int(local) if valid else 0xffffffff
        # The second logical page moved from physical page 3 to physical page 19.
        self.assertEqual(run(3077, 18), 1029)
        self.assertEqual(run(19461, 18), 1029)
        for reverse_slot in (0xffffffff, 16, 19, 0xfffffffe):
            self.assertEqual(run(3077, reverse_slot), 0xffffffff)
        self.assertEqual(run(3078, 18), 0xffffffff, "last-page padding is not a splat")
        self.assertEqual(run(3077, 18, count=0), 0xffffffff)
        self.assertEqual(run(1034, 0xffffffff, table_ref=0, base=1029, count=6), 5)
        self.assertEqual(run(1028, 0xffffffff, table_ref=0, base=1029, count=6), 0xffffffff)
        self.assertEqual(run(1035, 0xffffffff, table_ref=0, base=1029, count=6), 0xffffffff)
        self.assertEqual(run(0xfffffffe, 4194303, table_ref=1, count=0xffffffff), 0xfffffffe)
        self.assertEqual(run(0xffffffff, 4194303, table_ref=1, count=0xffffffff), 0xffffffff)

    def test_shader_control_has_host_wiring_and_cache_invalidation(self):
        pipeline = (ROOT / "modules/gaussian_splatting/interfaces/gpu_sorting_pipeline.cpp").read_text(encoding="utf-8")
        tile = (ROOT / "modules/gaussian_splatting/renderer/tile_renderer.cpp").read_text(encoding="utf-8")
        control = (ROOT / "modules/gaussian_splatting/renderer/splat_identity_test_control.h").read_text(encoding="utf-8")
        for source in (pipeline, tile):
            self.assertIn("GSSplatIdentityTestControl::shader_defines()", source)
            self.assertIn("GSSplatIdentityTestControl::get_force_address_keying()", source)
        self.assertIn("identity_variant_changed", pipeline)
        self.assertIn("test_address_identity_enabled != force_address", tile)
        self.assertIn("#ifdef TESTS_ENABLED", control)
        self.assertIn("force_address_keying{ false }", control)


if __name__ == "__main__":
    unittest.main()
