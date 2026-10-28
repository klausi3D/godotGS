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
        self.assertIn("(world_position + frame_anchor).xz", deformation)
        self.assertIn("effector_sphere.xyz -= frame_anchor", deformation)


if __name__ == "__main__":
    unittest.main()
