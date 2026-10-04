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

# A function writing a 16-entry SH basis: any `out float NAME[16]` parameter.
_SH_BASIS_SIGNATURE = re.compile(r"\b(?:void)\s+(\w+)\s*\(([^)]*\bout\s+float\s+\w+\s*\[\s*16\s*\][^)]*)\)\s*\{")


def discover_sh_basis_functions() -> list[tuple[Path, str, list[str]]]:
    """Every shader function that fills a 16-entry SH basis -- derived, not listed."""
    found = []
    for path in sorted(SHADERS_DIR.rglob("*.glsl")):
        text = path.read_text(encoding="utf-8")
        for m in _SH_BASIS_SIGNATURE.finditer(text):
            param_types = [p.split()[-2] if p.split()[0] in ("in", "out", "inout") else p.split()[0]
                           for p in (s.strip() for s in m.group(2).split(",")) if p]
            found.append((path, m.group(1), param_types))
    return found


class ShBasisMatchesInriaReference(unittest.TestCase):
    """#1157: band-3 basis[11] and [13] were sign-flipped and [14] doubled."""

    def test_discovery_finds_the_production_basis(self) -> None:
        found = {(p.name, name) for p, name, _types in discover_sh_basis_functions()}
        self.assertIn(("gs_sh_binning.glsl", "compute_sh_basis"), found,
                      "the tile-binning SH basis is no longer discovered; the pattern drifted")

    def _evaluate(self, path: Path, name: str, param_types: list[str], direction: tuple[float, float, float],
                  band: int) -> list[float]:
        program = glsl.load(path)
        vec3 = program.namespace["vec3"]
        args = []
        basis = [None] * 16
        for ptype in param_types:
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
            has_band_param = "uint" in param_types
            for direction in REFERENCE_DIRECTIONS:
                reference = inria_sh_basis(*direction)
                for band in ((0, 1, 2, 3) if has_band_param else (3,)):
                    basis = self._evaluate(path, name, param_types, direction, band)
                    used = (1, 4, 9, 16)[band]
                    for index in range(16):
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


if __name__ == "__main__":
    unittest.main()
