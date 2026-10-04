#!/usr/bin/env python3
"""Execute a straight-line subset of GLSL on the host, from the real shader source.

Why this exists: several numerical contracts of the Gaussian splatting shaders
(the SH basis, the raster payload encodings) have no C++ mirror, and a mirror
written for a test only proves the mirror. This module translates the shader
functions themselves into Python and runs them, so a test written against it
reads the code the GPU compiles.

Scope (deliberately small; anything outside it FAILS, it is never skipped):
  * preprocessor: #ifdef / #ifndef / #if defined(X) / #else / #endif and
    object-like #define; anything else raises GlslUnsupported.
  * top level: `const` scalars, plain structs and function definitions are
    translated; other declarations (buffers, uniforms, const arrays) are
    skipped because the functions under test do not read them -- a function
    that does fails with NameError when called.
  * statements: declarations, assignment and compound assignment, ++/--,
    if/else, for (without `continue`), return, call statements.
  * `out` / `inout` parameters: arrays and structs are passed by reference;
    scalar and vector ones are returned -- the translated function returns
    (value, out_1, ...) and a call statement `f(a, b);` assigns them back. A
    call to such a function inside a larger expression raises.
  * a function whose body leaves the subset is replaced by a stub that raises
    GlslUnsupported when called, so it can never be evaluated by accident.
  * GLSL semantics kept where Python differs: 32-bit wrapping uint/int,
    truncating integer division, value (copy) semantics for vectors and
    structs, a C-precedence check (a bitwise and a comparison operator at the
    same parenthesis depth is rejected rather than evaluated with Python's
    precedence), and undefined behaviour (NaN into min/max/clamp, a negative
    float converted to uint, reading an uninitialised value) raises.
  * float arithmetic runs in float64; values crossing a storage boundary
    (floatBitsToUint, packHalf2x16) are rounded to float32 first. Tests must
    use tolerances that cover the float32-vs-float64 difference.
"""

from __future__ import annotations

import math
import re
import struct
from pathlib import Path
from typing import Any, Callable


class GlslUnsupported(Exception):
    """The source uses a construct this evaluator does not model."""


class GlslUndefinedBehaviour(Exception):
    """The evaluated code did something GLSL leaves undefined."""


# ---------------------------------------------------------------------------
# Runtime value model
# ---------------------------------------------------------------------------


class _Undef:
    """An uninitialised GLSL value. Any use raises."""

    def _fail(self, *_args: Any, **_kwargs: Any) -> Any:
        raise GlslUndefinedBehaviour("read of an uninitialised GLSL value")

    __add__ = __radd__ = __sub__ = __rsub__ = __mul__ = __rmul__ = _fail
    __truediv__ = __rtruediv__ = __and__ = __rand__ = __or__ = __ror__ = _fail
    __xor__ = __rxor__ = __lshift__ = __rshift__ = __neg__ = __invert__ = _fail
    __lt__ = __le__ = __gt__ = __ge__ = __bool__ = __float__ = __int__ = _fail
    __index__ = __getattr__ = _fail

    def __setattr__(self, _name: str, _value: Any) -> None:
        raise GlslUndefinedBehaviour("component write into an uninitialised scalar")

    def __eq__(self, _other: object) -> bool:  # noqa: D105
        raise GlslUndefinedBehaviour("read of an uninitialised GLSL value")

    __hash__ = None  # type: ignore[assignment]

    def __repr__(self) -> str:
        return "UNDEF"


UNDEF = _Undef()


def _is_float(v: Any) -> bool:
    return isinstance(v, float)


class U32(int):
    """GLSL uint: 32-bit wrapping."""

    def __new__(cls, value: int) -> "U32":
        return super().__new__(cls, int(value) & 0xFFFFFFFF)

    def _bin(self, other: Any, op: Callable[[int, int], int]) -> Any:
        if isinstance(other, Vec):
            return NotImplemented
        if _is_float(other):
            return op(float(self), other)  # type: ignore[arg-type]
        if isinstance(other, bool):
            raise GlslUnsupported("arithmetic on bool")
        return U32(op(int(self), int(other)))

    def _rbin(self, other: Any, op: Callable[[int, int], int]) -> Any:
        if isinstance(other, Vec):
            return NotImplemented
        if _is_float(other):
            return op(other, float(self))  # type: ignore[arg-type]
        return U32(op(int(other), int(self)))

    def __add__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a + b)
    def __radd__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a + b)
    def __sub__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a - b)
    def __rsub__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a - b)
    def __mul__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a * b)
    def __rmul__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a * b)

    def __truediv__(self, o: Any) -> Any:
        if _is_float(o):
            return float(self) / o
        if int(o) == 0:
            raise GlslUndefinedBehaviour("integer division by zero")
        return U32(int(self) // int(U32(o)))

    def __rtruediv__(self, o: Any) -> Any:
        if _is_float(o):
            return o / float(self)
        if int(self) == 0:
            raise GlslUndefinedBehaviour("integer division by zero")
        return U32(int(U32(o)) // int(self))

    def __mod__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a % b)
    def __and__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a & b)
    def __rand__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a & b)
    def __or__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a | b)
    def __ror__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a | b)
    def __xor__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a ^ b)
    def __rxor__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a ^ b)

    def __lshift__(self, o: Any) -> Any:
        if not 0 <= int(o) < 32:
            raise GlslUndefinedBehaviour(f"shift by {int(o)}")
        return U32(int(self) << int(o))

    def __rshift__(self, o: Any) -> Any:
        if not 0 <= int(o) < 32:
            raise GlslUndefinedBehaviour(f"shift by {int(o)}")
        return U32(int(self) >> int(o))

    def __neg__(self) -> "U32": return U32(-int(self))
    def __invert__(self) -> "U32": return U32(~int(self))
    def __repr__(self) -> str: return f"{int(self)}u"


class I32(int):
    """GLSL int: 32-bit two's complement, truncating division."""

    def __new__(cls, value: int) -> "I32":
        v = int(value) & 0xFFFFFFFF
        if v & 0x80000000:
            v -= 1 << 32
        return super().__new__(cls, v)

    def _bin(self, other: Any, op: Callable[[int, int], int]) -> Any:
        if isinstance(other, Vec):
            return NotImplemented
        if _is_float(other):
            return op(float(self), other)  # type: ignore[arg-type]
        if isinstance(other, U32):
            return U32(op(int(U32(self)), int(other)))
        return I32(op(int(self), int(other)))

    def _rbin(self, other: Any, op: Callable[[int, int], int]) -> Any:
        if isinstance(other, Vec):
            return NotImplemented
        if _is_float(other):
            return op(other, float(self))  # type: ignore[arg-type]
        return I32(op(int(other), int(self)))

    def __add__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a + b)
    def __radd__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a + b)
    def __sub__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a - b)
    def __rsub__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a - b)
    def __mul__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a * b)
    def __rmul__(self, o: Any) -> Any: return self._rbin(o, lambda a, b: a * b)

    def __truediv__(self, o: Any) -> Any:
        if _is_float(o):
            return float(self) / o
        if isinstance(o, U32):
            return U32(self) / o
        if int(o) == 0:
            raise GlslUndefinedBehaviour("integer division by zero")
        q = abs(int(self)) // abs(int(o))
        return I32(q if (int(self) >= 0) == (int(o) >= 0) else -q)

    def __and__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a & b)
    def __or__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a | b)
    def __xor__(self, o: Any) -> Any: return self._bin(o, lambda a, b: a ^ b)

    def __lshift__(self, o: Any) -> Any:
        if not 0 <= int(o) < 32:
            raise GlslUndefinedBehaviour(f"shift by {int(o)}")
        return I32(int(self) << int(o))

    def __rshift__(self, o: Any) -> Any:
        if not 0 <= int(o) < 32:
            raise GlslUndefinedBehaviour(f"shift by {int(o)}")
        return I32(int(self) >> int(o))

    def __neg__(self) -> "I32": return I32(-int(self))
    def __invert__(self) -> "I32": return I32(~int(self))


def _to_f32(x: float) -> float:
    """Round a Python float to the nearest float32 (inf on overflow)."""
    if math.isnan(x) or math.isinf(x):
        return x
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


_SWIZZLE_SETS = ("xyzw", "rgba", "stpq")


def _swizzle_indices(name: str) -> list[int] | None:
    if not name or len(name) > 4:
        return None
    for charset in _SWIZZLE_SETS:
        if all(ch in charset for ch in name):
            return [charset.index(ch) for ch in name]
    return None


class Vec:
    """GLSL vecN / uvecN / ivecN / bvecN with value semantics."""

    __slots__ = ("c",)

    def __init__(self, components: list[Any]) -> None:
        object.__setattr__(self, "c", list(components))

    def copy(self) -> "Vec":
        return Vec(list(self.c))

    def __len__(self) -> int:
        return len(self.c)

    def __getattr__(self, name: str) -> Any:
        idx = _swizzle_indices(name)
        if idx is None:
            raise AttributeError(name)
        for i in idx:
            if i >= len(self.c):
                raise GlslUnsupported(f"swizzle .{name} out of range for vec{len(self.c)}")
        if len(idx) == 1:
            return self.c[idx[0]]
        return Vec([self.c[i] for i in idx])

    def __setattr__(self, name: str, value: Any) -> None:
        idx = _swizzle_indices(name)
        if idx is None:
            raise AttributeError(name)
        if len(idx) == 1:
            self.c[idx[0]] = value
            return
        if not isinstance(value, Vec) or len(value) != len(idx):
            raise GlslUnsupported(f"swizzle assignment .{name} with mismatched value")
        for i, v in zip(idx, value.c):
            self.c[i] = v

    def __getitem__(self, i: int) -> Any:
        return self.c[int(i)]

    def __setitem__(self, i: int, v: Any) -> None:
        self.c[int(i)] = v

    def _zip(self, other: Any) -> list[tuple[Any, Any]]:
        if isinstance(other, Vec):
            if len(other) != len(self):
                raise GlslUnsupported("vector size mismatch")
            return list(zip(self.c, other.c))
        return [(a, other) for a in self.c]

    def __add__(self, o: Any) -> "Vec": return Vec([a + b for a, b in self._zip(o)])
    def __radd__(self, o: Any) -> "Vec": return Vec([b + a for a, b in self._zip(o)])
    def __sub__(self, o: Any) -> "Vec": return Vec([a - b for a, b in self._zip(o)])
    def __rsub__(self, o: Any) -> "Vec": return Vec([b - a for a, b in self._zip(o)])
    def __mul__(self, o: Any) -> "Vec": return Vec([a * b for a, b in self._zip(o)])
    def __rmul__(self, o: Any) -> "Vec": return Vec([b * a for a, b in self._zip(o)])
    def __truediv__(self, o: Any) -> "Vec": return Vec([a / b for a, b in self._zip(o)])
    def __rtruediv__(self, o: Any) -> "Vec": return Vec([b / a for a, b in self._zip(o)])
    def __and__(self, o: Any) -> "Vec": return Vec([a & b for a, b in self._zip(o)])
    def __or__(self, o: Any) -> "Vec": return Vec([a | b for a, b in self._zip(o)])
    def __lshift__(self, o: Any) -> "Vec": return Vec([a << b for a, b in self._zip(o)])
    def __rshift__(self, o: Any) -> "Vec": return Vec([a >> b for a, b in self._zip(o)])
    def __neg__(self) -> "Vec": return Vec([-a for a in self.c])

    def __eq__(self, o: object) -> bool:  # GLSL ==: all components equal
        return all(a == b for a, b in self._zip(o))

    def __ne__(self, o: object) -> bool:
        return not self.__eq__(o)

    __hash__ = None  # type: ignore[assignment]

    def __repr__(self) -> str:
        return f"vec{len(self.c)}({', '.join(repr(v) for v in self.c)})"


class GlslStruct:
    """Base class of translated GLSL structs."""

    _fields: tuple[tuple[str, str, int | None], ...] = ()

    def __init__(self) -> None:
        for name, _type, count in self._fields:
            setattr(self, name, [UNDEF] * count if count is not None else UNDEF)

    def copy(self) -> "GlslStruct":
        clone = type(self).__new__(type(self))
        for name, _type, _count in self._fields:
            setattr(clone, name, _copy(getattr(self, name)))
        return clone


def _copy(v: Any) -> Any:
    if isinstance(v, (Vec, GlslStruct)):
        return v.copy()
    if isinstance(v, list):
        return [_copy(x) for x in v]
    return v


# ---------------------------------------------------------------------------
# float32 <-> float16 conversion (packHalf2x16), with an explicit rounding mode
# ---------------------------------------------------------------------------


def f32_bits(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", _to_f32(x)))[0]


def f32_from_bits(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def f32_to_f16_bits(x: float, rounding: str = "rte") -> int:
    """IEEE binary32 -> binary16 bit pattern. rounding: 'rte' (nearest-even) or 'rtz'.

    Vulkan leaves the rounding of packHalf2x16 (OpFConvert to 16 bit) to the
    implementation, so tests that care run both modes.
    """
    if rounding not in ("rte", "rtz"):
        raise ValueError(rounding)
    bits = f32_bits(x)
    sign = (bits >> 16) & 0x8000
    exp = (bits >> 23) & 0xFF
    mant = bits & 0x7FFFFF
    if exp == 0xFF:
        return sign | 0x7C00 | (0x200 if mant else 0)
    # Unbiased exponent; build the value as an integer multiple of 2^-24 (smallest subnormal).
    if exp == 0:
        return sign  # float32 subnormals flush to (signed) zero in half
    e = exp - 127
    full = mant | 0x800000  # 24-bit significand, value = full * 2^(e-23)
    # Target: value / 2^-24 = full * 2^(e - 23 + 24) = full * 2^(e + 1)
    shift = e + 1
    if shift >= 0:
        q = full << shift
        rem_num, rem_den = 0, 1
    else:
        den = 1 << (-shift)
        q, rem = divmod(full, den)
        rem_num, rem_den = rem, den
    # q is the magnitude in units of 2^-24, truncated. Normalise to half precision.
    # Half: subnormal if q < 1024 (i.e. value < 2^-14), else normal with 11-bit significand.
    def _round(qv: int, extra_num: int, extra_den: int, drop: int) -> int:
        """Drop `drop` low bits from qv (+ fractional extra) with the rounding mode."""
        if drop > 0:
            dropped = qv & ((1 << drop) - 1)
            base = qv >> drop
            num = dropped * extra_den + extra_num
            den = (1 << drop) * extra_den
        else:
            base, num, den = qv, extra_num, extra_den
        if rounding == "rtz" or num == 0:
            return base
        if 2 * num > den or (2 * num == den and (base & 1)):
            return base + 1
        return base

    if q < 1024:
        m = _round(q, rem_num, rem_den, 0)
        return sign | m  # may round up into the smallest normal (m == 1024) -> still correct bits
    # Normal: find drop so that significand fits 11 bits.
    drop = q.bit_length() - 11
    sig = _round(q, rem_num, rem_den, drop)
    if sig == 2048:
        sig = 1024
        drop += 1
    half_exp = drop + 1  # value = sig * 2^(drop) * 2^-24, sig in [1024, 2048) -> exponent field
    if half_exp >= 31:
        return sign | (0x7C00 if rounding == "rte" else 0x7BFF)
    return sign | (half_exp << 10) | (sig - 1024)


def f16_bits_to_float(h: int) -> float:
    return struct.unpack("<e", struct.pack("<H", h & 0xFFFF))[0]


# ---------------------------------------------------------------------------
# Built-in functions
# ---------------------------------------------------------------------------


def _elementwise(fn: Callable[..., Any], *args: Any) -> Any:
    size = None
    for a in args:
        if isinstance(a, Vec):
            if size is not None and len(a) != size:
                raise GlslUnsupported("vector size mismatch in built-in")
            size = len(a)
    if size is None:
        return fn(*args)
    cols = [a.c if isinstance(a, Vec) else [a] * size for a in args]
    return Vec([fn(*vals) for vals in zip(*cols)])


def _nan_check(*vals: Any) -> None:
    for v in vals:
        if isinstance(v, float) and math.isnan(v):
            raise GlslUndefinedBehaviour("NaN operand to min/max/clamp is undefined in GLSL")


def _min(a: Any, b: Any) -> Any:
    def f(x: Any, y: Any) -> Any:
        _nan_check(x, y)
        return y if y < x else x
    return _elementwise(f, a, b)


def _max(a: Any, b: Any) -> Any:
    def f(x: Any, y: Any) -> Any:
        _nan_check(x, y)
        return y if y > x else x
    return _elementwise(f, a, b)


def _clamp(x: Any, lo: Any, hi: Any) -> Any:
    return _min(_max(x, lo), hi)


def _float(*args: Any) -> Any:
    if len(args) != 1:
        raise GlslUnsupported("float() takes one argument")
    v = args[0]
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    return float(v)


def _uint(v: Any) -> U32:
    if isinstance(v, bool):
        return U32(1 if v else 0)
    if isinstance(v, float):
        if math.isnan(v) or v < 0.0 or v >= 4294967296.0:
            raise GlslUndefinedBehaviour(f"uint({v!r}) is undefined")
        return U32(int(v))
    return U32(v)


def _int(v: Any) -> I32:
    if isinstance(v, bool):
        return I32(1 if v else 0)
    if isinstance(v, float):
        if math.isnan(v) or not -2147483648.0 <= v < 2147483648.0:
            raise GlslUndefinedBehaviour(f"int({v!r}) is undefined")
        return I32(int(v))
    return I32(v)


def _bool(v: Any) -> bool:
    return bool(v)


def _vec_ctor(size: int, conv: Callable[[Any], Any]) -> Callable[..., Vec]:
    def ctor(*args: Any) -> Vec:
        flat: list[Any] = []
        for a in args:
            flat.extend(a.c if isinstance(a, Vec) else [a])
        if len(flat) == 1:
            flat = flat * size
        if len(flat) < size:
            raise GlslUnsupported(f"vec{size} constructor with {len(flat)} components")
        return Vec([conv(v) for v in flat[:size]])
    return ctor


def _isnan(v: Any) -> Any:
    return _elementwise(lambda x: isinstance(x, float) and math.isnan(x), v)


def _isinf(v: Any) -> Any:
    return _elementwise(lambda x: isinstance(x, float) and math.isinf(x), v)


def _any(v: Vec) -> bool:
    return any(bool(x) for x in v.c)


def _all(v: Vec) -> bool:
    return all(bool(x) for x in v.c)


def _abs(v: Any) -> Any:
    return _elementwise(lambda x: type(x)(abs(int(x))) if isinstance(x, I32) else abs(x), v)


def _float_bits_to_uint(v: Any) -> Any:
    return _elementwise(lambda x: U32(f32_bits(float(x))), v)


def _uint_bits_to_float(v: Any) -> Any:
    return _elementwise(lambda x: f32_from_bits(int(x)), v)


def _bitfield_extract(value: Any, offset: Any, bits: Any) -> Any:
    off, n = int(offset), int(bits)
    if n == 0:
        return type(value)(0)
    raw = (int(value) >> off) & ((1 << n) - 1)
    if isinstance(value, I32) and raw & (1 << (n - 1)):
        raw -= 1 << n
    return type(value)(raw)


def _make_builtins(half_rounding: str) -> dict[str, Any]:
    def pack_half_2x16(v: Vec) -> U32:
        lo = f32_to_f16_bits(float(v.c[0]), half_rounding)
        hi = f32_to_f16_bits(float(v.c[1]), half_rounding)
        return U32(lo | (hi << 16))

    def unpack_half_2x16(u: Any) -> Vec:
        u = int(u)
        return Vec([f16_bits_to_float(u & 0xFFFF), f16_bits_to_float((u >> 16) & 0xFFFF)])

    def pack_unorm_2x16(v: Vec) -> U32:
        def q(x: float) -> int:
            _nan_check(x)
            return int(math.floor(min(max(x, 0.0), 1.0) * 65535.0 + 0.5))
        return U32(q(v.c[0]) | (q(v.c[1]) << 16))

    def unpack_unorm_2x16(u: Any) -> Vec:
        u = int(u)
        return Vec([(u & 0xFFFF) / 65535.0, ((u >> 16) & 0xFFFF) / 65535.0])

    return {
        "float": _float,
        "int": _int,
        "uint": _uint,
        "bool": _bool,
        "vec2": _vec_ctor(2, _float),
        "vec3": _vec_ctor(3, _float),
        "vec4": _vec_ctor(4, _float),
        "uvec2": _vec_ctor(2, _uint),
        "uvec3": _vec_ctor(3, _uint),
        "uvec4": _vec_ctor(4, _uint),
        "ivec2": _vec_ctor(2, _int),
        "ivec3": _vec_ctor(3, _int),
        "ivec4": _vec_ctor(4, _int),
        "bvec2": _vec_ctor(2, _bool),
        "bvec3": _vec_ctor(3, _bool),
        "min": _min,
        "max": _max,
        "clamp": _clamp,
        "abs": _abs,
        "floor": lambda v: _elementwise(lambda x: float(math.floor(x)), v),
        "exp": lambda v: _elementwise(lambda x: math.exp(x), v),
        "sqrt": lambda v: _elementwise(lambda x: math.sqrt(x), v),
        "isnan": _isnan,
        "isinf": _isinf,
        "any": _any,
        "all": _all,
        "floatBitsToUint": _float_bits_to_uint,
        "uintBitsToFloat": _uint_bits_to_float,
        "packHalf2x16": pack_half_2x16,
        "unpackHalf2x16": unpack_half_2x16,
        "packUnorm2x16": pack_unorm_2x16,
        "unpackUnorm2x16": unpack_unorm_2x16,
        "bitfieldExtract": _bitfield_extract,
        "_copy": _copy,
        "_U32": U32,
        "_I32": I32,
        "UNDEF": UNDEF,
        "Vec": Vec,
    }


# ---------------------------------------------------------------------------
# Source -> tokens
# ---------------------------------------------------------------------------

_TOKEN_RE = re.compile(
    r"""
    (?P<ws>\s+)
  | (?P<num>0[xX][0-9a-fA-F]+[uU]?
       | (?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?(?:lf|LF|[fFuU])?)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<=|>>=|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||\^\^|[-+*/%&|^]=
       |[{}()\[\];,.?:+\-*/%&|^~!<>=])
    """,
    re.VERBOSE,
)


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def preprocess(src: str, defines: dict[str, str] | None = None) -> tuple[str, dict[str, str]]:
    """Resolve conditionals; return (active source, object-like macro table)."""
    macros: dict[str, str] = dict(defines or {})
    out: list[str] = []
    stack: list[tuple[bool, bool]] = []  # (parent_active, this_branch_active)
    active = True
    for line in _strip_comments(src).splitlines():
        stripped = line.strip()
        if not stripped.startswith("#"):
            out.append(line if active else "")
            continue
        directive = stripped[1:].strip()
        m = re.match(r"(\w+)\s*(.*)$", directive)
        if not m:
            raise GlslUnsupported(f"preprocessor line: {stripped}")
        kind, rest = m.group(1), m.group(2).strip()
        if kind in ("ifdef", "ifndef"):
            cond = rest in macros
            if kind == "ifndef":
                cond = not cond
            stack.append((active, cond))
            active = active and cond
        elif kind == "if":
            md = re.fullmatch(r"(!?)\s*defined\s*\(?\s*(\w+)\s*\)?", rest)
            if md:
                cond = (md.group(2) in macros) != bool(md.group(1))
            elif re.fullmatch(r"\d+", rest):
                cond = int(rest) != 0
            else:
                raise GlslUnsupported(f"#if expression not modelled: {rest}")
            stack.append((active, cond))
            active = active and cond
        elif kind == "else":
            if not stack:
                raise GlslUnsupported("#else without #if")
            parent, cond = stack[-1]
            stack[-1] = (parent, not cond)
            active = parent and not cond
        elif kind == "endif":
            if not stack:
                raise GlslUnsupported("#endif without #if")
            parent, _ = stack.pop()
            active = parent
        elif kind == "define":
            if active:
                md = re.match(r"(\w+)(\(?)\s*(.*)$", rest)
                if not md:
                    raise GlslUnsupported(f"#define {rest}")
                if md.group(2):
                    raise GlslUnsupported(f"function-like macro {md.group(1)}")
                macros[md.group(1)] = md.group(3).strip()
        elif kind in ("include", "version", "extension", "pragma", "undef", "error", "elif"):
            if active:
                raise GlslUnsupported(f"#{kind} is not modelled")
        else:
            raise GlslUnsupported(f"#{kind} is not modelled")
        out.append("")
    if stack:
        raise GlslUnsupported("unterminated #if")
    return "\n".join(out), macros


def tokenize(src: str, macros: dict[str, str]) -> list[str]:
    tokens: list[str] = []
    pos = 0
    while pos < len(src):
        m = _TOKEN_RE.match(src, pos)
        if not m:
            raise GlslUnsupported(f"cannot tokenize near: {src[pos:pos + 30]!r}")
        pos = m.end()
        if m.lastgroup == "ws":
            continue
        tok = m.group(0)
        if m.lastgroup == "id" and tok in macros and macros[tok] != "":
            tokens.extend(tokenize(macros[tok], {k: v for k, v in macros.items() if k != tok}))
            continue
        tokens.append(tok)
    return tokens


# ---------------------------------------------------------------------------
# Tokens -> Python
# ---------------------------------------------------------------------------

_SCALAR_TYPES = {"void", "bool", "int", "uint", "float", "double"}
_VECTOR_TYPES = {
    f"{p}vec{n}" for p in ("", "u", "i", "b", "d") for n in (2, 3, 4)
} | {f"mat{n}" for n in (2, 3, 4)}
_QUALIFIERS = {"const", "in", "out", "inout", "highp", "mediump", "lowp", "precise"}
_PY_KEYWORDS = {
    "and", "as", "assert", "async", "await", "class", "def", "del", "elif", "except",
    "finally", "from", "global", "import", "in", "is", "lambda", "nonlocal", "not", "or",
    "pass", "raise", "try", "while", "with", "yield", "None", "True", "False",
}
_BITWISE = {"&", "|", "^", "<<", ">>"}
_COMPARE = {"==", "!=", "<", ">", "<=", ">="}


def _undef_value(type_name: str) -> str:
    """Python source for an uninitialised value of a GLSL type."""
    m = re.fullmatch(r"[uibd]?vec([234])", type_name)
    if m:
        return f"Vec([UNDEF] * {m.group(1)})"
    if type_name.startswith("mat"):
        raise GlslUnsupported("matrix values are not modelled")
    return "UNDEF"


def _py_name(name: str) -> str:
    return name + "_glsl" if name in _PY_KEYWORDS else name


class _Parser:
    def __init__(self, tokens: list[str], types: set[str], out_sigs: dict[str, list[int]]) -> None:
        self.t = tokens
        self.i = 0
        self.types = types
        self.out_sigs = out_sigs

    # -- token helpers ------------------------------------------------------
    def peek(self, k: int = 0) -> str | None:
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def take(self, expected: str | None = None) -> str:
        tok = self.peek()
        if tok is None:
            raise GlslUnsupported("unexpected end of source")
        if expected is not None and tok != expected:
            raise GlslUnsupported(f"expected {expected!r}, found {tok!r}")
        self.i += 1
        return tok

    def is_type(self, tok: str | None) -> bool:
        return tok is not None and (tok in _SCALAR_TYPES or tok in _VECTOR_TYPES or tok in self.types)

    def until(self, stops: set[str]) -> list[str]:
        """Collect tokens up to (not including) a stop token at depth 0."""
        depth = 0
        out: list[str] = []
        while True:
            tok = self.peek()
            if tok is None:
                raise GlslUnsupported("unterminated expression")
            if depth == 0 and tok in stops:
                return out
            if tok in "([{":
                depth += 1
            elif tok in ")]}":
                depth -= 1
                if depth < 0:
                    return out
            out.append(self.take())

    # -- expressions --------------------------------------------------------
    def expr(self, toks: list[str]) -> str:
        if not toks:
            raise GlslUnsupported("empty expression")
        self._check_precedence(toks)
        out: list[str] = []
        k = 0
        while k < len(toks):
            tok = toks[k]
            if tok == "?":
                raise GlslUnsupported("ternary operator is not modelled")
            if tok in ("++", "--"):
                raise GlslUnsupported("++/-- inside an expression is not modelled")
            if tok == "!":
                end = self._operand_end(toks, k + 1)
                out.append(f"(not ({self.expr(toks[k + 1:end])}))")
                k = end
                continue
            if tok == "&&":
                out.append(" and ")
            elif tok == "||":
                out.append(" or ")
            elif tok == "^^":
                raise GlslUnsupported("^^ is not modelled")
            elif tok == "true":
                out.append("True")
            elif tok == "false":
                out.append("False")
            elif re.fullmatch(r"0[xX][0-9a-fA-F]+[uU]?", tok):
                ctor = "_U32" if tok[-1] in "uU" else "_I32"
                out.append(f"{ctor}({tok.rstrip('uU')})")
            elif re.fullmatch(r"\d+[uU]", tok):
                out.append(f"_U32({tok[:-1]})")
            elif re.fullmatch(r"\d+", tok):
                out.append(f"_I32({tok})")
            elif re.fullmatch(r"(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?(?:lf|LF|[fF])?", tok):
                out.append(repr(float(re.sub(r"(lf|LF|[fF])$", "", tok))))
            elif re.fullmatch(r"[A-Za-z_]\w*", tok):
                prev = toks[k - 1] if k > 0 else None
                if prev != "." and tok in self.out_sigs and k + 1 < len(toks) and toks[k + 1] == "(":
                    raise GlslUnsupported(
                        f"{tok}() has out parameters; it is modelled only as a call statement")
                out.append(tok if prev == "." else _py_name(tok))
            elif tok == "=":
                raise GlslUnsupported("assignment inside an expression is not modelled")
            else:
                out.append(f" {tok} " if tok not in ".()[]," else tok)
            k += 1
        return "".join(out)

    @staticmethod
    def _operand_end(toks: list[str], start: int) -> int:
        k = start
        if k >= len(toks):
            raise GlslUnsupported("dangling '!'")
        if toks[k] == "!":
            return _Parser._operand_end(toks, k + 1)
        if toks[k] == "(":
            depth = 0
            while k < len(toks):
                if toks[k] == "(":
                    depth += 1
                elif toks[k] == ")":
                    depth -= 1
                    if depth == 0:
                        k += 1
                        break
                k += 1
        else:
            k += 1
        while k < len(toks) and toks[k] in (".", "[", "("):
            if toks[k] == ".":
                k += 2
                continue
            close = "]" if toks[k] == "[" else ")"
            depth = 0
            while k < len(toks):
                if toks[k] in "[(":
                    depth += 1
                elif toks[k] in "])":
                    depth -= 1
                    if depth == 0:
                        k += 1
                        break
                k += 1
            del close
        return k

    @staticmethod
    def _check_precedence(toks: list[str]) -> None:
        """Reject expressions whose meaning differs between C and Python precedence."""
        depth = 0
        seen: dict[int, set[str]] = {}
        for tok in toks:
            if tok in "([":
                depth += 1
                continue
            if tok in ")]":
                seen.pop(depth, None)
                depth -= 1
                continue
            if tok == ",":
                seen.pop(depth, None)
                continue
            if tok in _BITWISE:
                seen.setdefault(depth, set()).add("bit")
            elif tok in _COMPARE:
                seen.setdefault(depth, set()).add("cmp")
            if seen.get(depth) == {"bit", "cmp"}:
                raise GlslUnsupported(
                    "bitwise and comparison operators share a parenthesis level; C and Python "
                    "precedence differ there -- parenthesise the shader expression")

    # -- statements -----------------------------------------------------------
    def block(self, indent: int, out: list[str], ctx: dict[str, Any]) -> None:
        self.take("{")
        start = len(out)
        while self.peek() != "}":
            self.statement(indent, out, ctx)
        self.take("}")
        if len(out) == start:
            out.append("    " * indent + "pass")

    def body_statement(self, indent: int, out: list[str], ctx: dict[str, Any]) -> None:
        start = len(out)
        if self.peek() == "{":
            self.block(indent, out, ctx)
        else:
            self.statement(indent, out, ctx)
        if len(out) == start:
            out.append("    " * indent + "pass")

    def statement(self, indent: int, out: list[str], ctx: dict[str, Any]) -> None:
        pad = "    " * indent
        tok = self.peek()
        if tok == "{":
            self.block(indent, out, ctx)
            return
        if tok == ";":
            self.take()
            return
        if tok == "if":
            self.take()
            self.take("(")
            cond = self.expr(self.until({")"}))
            self.take(")")
            out.append(f"{pad}if {cond}:")
            self.body_statement(indent + 1, out, ctx)
            if self.peek() == "else":
                self.take()
                out.append(f"{pad}else:")
                self.body_statement(indent + 1, out, ctx)
            return
        if tok == "for":
            self.take()
            self.take("(")
            init: list[str] = []
            if self.peek() != ";":
                self.simple_statement(0, init, ctx, terminator=";")
            else:
                self.take(";")
            cond = self.expr(self.until({";"})) if self.peek() != ";" else "True"
            self.take(";")
            incr: list[str] = []
            if self.peek() != ")":
                self.simple_statement(0, incr, ctx, terminator=")")
            else:
                self.take(")")
            for line in init:
                out.append(pad + line)
            out.append(f"{pad}while {cond}:")
            body: list[str] = []
            self.body_statement(indent + 1, body, ctx)
            if any(re.search(r"\bcontinue\b", line) for line in body):
                raise GlslUnsupported("continue inside for is not modelled")
            out.extend(body)
            for line in incr:
                out.append("    " * (indent + 1) + line)
            return
        if tok in ("while", "do", "switch", "discard"):
            raise GlslUnsupported(f"'{tok}' is not modelled")
        if tok == "return":
            self.take()
            outs = ctx.get("outs") or []
            if self.peek() == ";":
                self.take()
                value = "None"
            else:
                value = f"_copy({self.expr(self.until({';'}))})"
                self.take(";")
            if outs:
                out.append(f"{pad}return ({value}, {', '.join(outs)},)")
            else:
                out.append(f"{pad}return {value}")
            return
        if tok == "break":
            self.take()
            self.take(";")
            out.append(f"{pad}break")
            return
        if tok == "continue":
            self.take()
            self.take(";")
            out.append(f"{pad}continue")
            return
        self.simple_statement(indent, out, ctx, terminator=";")

    def _out_call_statement(self, toks: list[str], pad: str, out: list[str]) -> bool:
        """`f(a, b, c);` where f has scalar/vector out params: call, then assign them back."""
        if len(toks) < 3 or toks[0] not in self.out_sigs or toks[1] != "(" or toks[-1] != ")":
            return False
        args: list[list[str]] = [[]]
        depth = 0
        for tok in toks[2:-1]:
            if tok in "([":
                depth += 1
            elif tok in ")]":
                depth -= 1
                if depth < 0:
                    return False  # `f(a)(b)` or `f(a) + g(b)` -- not a single call statement
            if tok == "," and depth == 0:
                args.append([])
                continue
            args[-1].append(tok)
        out_indices = self.out_sigs[toks[0]]
        arg_exprs = [self.expr(a) for a in args]
        out.append(f"{pad}__ret = {_py_name(toks[0])}({', '.join(arg_exprs)})")
        for slot, index in enumerate(out_indices):
            out.append(f"{pad}{arg_exprs[index]} = __ret[{slot + 1}]")
        return True

    def simple_statement(self, indent: int, out: list[str], ctx: dict[str, Any], terminator: str) -> None:
        pad = "    " * indent
        while self.peek() in _QUALIFIERS:
            self.take()
        if self.is_type(self.peek()) and re.fullmatch(r"[A-Za-z_]\w*", self.peek(1) or ""):
            type_name = self.take()
            while True:
                name = _py_name(self.take())
                if self.peek() == "[":
                    self.take("[")
                    size = self.expr(self.until({"]"}))
                    self.take("]")
                    if self.peek() == "=":
                        raise GlslUnsupported("array initialisers are not modelled")
                    out.append(f"{pad}{name} = [UNDEF] * int({size})")
                elif self.peek() == "=":
                    self.take("=")
                    value = self.expr(self.until({",", terminator}))
                    out.append(f"{pad}{name} = _copy({value})")
                elif type_name in self.types:
                    out.append(f"{pad}{name} = {type_name}()")
                else:
                    out.append(f"{pad}{name} = {_undef_value(type_name)}")
                if self.peek() == ",":
                    self.take(",")
                    continue
                break
            self.take(terminator)
            return
        toks = self.until({terminator})
        self.take(terminator)
        if self._out_call_statement(toks, pad, out):
            return
        if len(toks) >= 2 and toks[-1] in ("++", "--"):
            lhs = self.expr(toks[:-1])
            out.append(f"{pad}{lhs} = {lhs} {'+' if toks[-1] == '++' else '-'} _I32(1)")
            return
        if len(toks) >= 2 and toks[0] in ("++", "--"):
            lhs = self.expr(toks[1:])
            out.append(f"{pad}{lhs} = {lhs} {'+' if toks[0] == '++' else '-'} _I32(1)")
            return
        depth = 0
        for k, tok in enumerate(toks):
            if tok in "([":
                depth += 1
            elif tok in ")]":
                depth -= 1
            elif depth == 0 and (tok == "=" or (tok.endswith("=") and tok not in _COMPARE)):
                lhs = self.expr(toks[:k])
                rhs = self.expr(toks[k + 1:])
                if tok == "=":
                    out.append(f"{pad}{lhs} = _copy({rhs})")
                else:
                    out.append(f"{pad}{lhs} = {lhs} {tok[:-1]} ({rhs})")
                return
        out.append(f"{pad}{self.expr(toks)}")


class GlslProgram:
    """Translated functions, consts and structs of one shader source."""

    def __init__(self, source: str, defines: dict[str, str] | None = None,
                 half_rounding: str = "rte", origin: str = "<glsl>") -> None:
        self.origin = origin
        self.namespace: dict[str, Any] = _make_builtins(half_rounding)
        self.functions: dict[str, Callable[..., Any]] = {}
        self.unsupported: dict[str, str] = {}
        self.python_source: dict[str, str] = {}
        self.out_signatures: dict[str, list[int]] = {}
        active, macros = preprocess(source, defines)
        self.macros = macros
        tokens = tokenize(active, macros)
        types: set[str] = set()
        # Pre-scan struct names so declarations of them parse as types.
        for k, tok in enumerate(tokens[:-1]):
            if tok == "struct":
                types.add(tokens[k + 1])
        self._types = types
        self._translate(tokens)

    # Top level -----------------------------------------------------------------
    def _translate(self, tokens: list[str]) -> None:
        p = _Parser(tokens, self._types, self.out_signatures)
        while p.peek() is not None:
            start = p.i
            tok = p.peek()
            if tok == ";":
                p.take()
                continue
            if tok == "struct":
                self._struct(p)
                continue
            if tok == "const" and p.is_type(p.peek(1)) and p.peek(3) == "=":
                p.take()
                p.take()
                name = p.take()
                p.take("=")
                value = p.expr(p.until({";"}))
                p.take(";")
                exec(f"{_py_name(name)} = {value}", self.namespace)  # noqa: S102
                continue
            if p.is_type(tok) and re.fullmatch(r"[A-Za-z_]\w*", p.peek(1) or "") and p.peek(2) == "(":
                self._function(p)
                continue
            # Anything else (buffers, uniforms, const arrays, shared, layout): skip one declaration.
            p.i = start
            depth = 0
            while p.peek() is not None:
                t = p.take()
                if t in "([{":
                    depth += 1
                elif t in ")]}":
                    depth -= 1
                elif t == ";" and depth == 0:
                    break

    def _struct(self, p: _Parser) -> None:
        p.take("struct")
        name = p.take()
        p.take("{")
        fields: list[tuple[str, str, int | None]] = []
        while p.peek() != "}":
            while p.peek() in _QUALIFIERS:
                p.take()
            ftype = p.take()
            while True:
                fname = p.take()
                count = None
                if p.peek() == "[":
                    p.take("[")
                    count = int(eval(p.expr(p.until({"]"})), dict(self.namespace)))  # noqa: S307
                    p.take("]")
                fields.append((fname, ftype, count))
                if p.peek() == ",":
                    p.take(",")
                    continue
                break
            p.take(";")
        p.take("}")
        if p.peek() == ";":
            p.take(";")
        cls = type(name, (GlslStruct,), {"_fields": tuple(fields)})
        self.namespace[name] = cls

    def _function(self, p: _Parser) -> None:
        p.take()  # return type
        name = p.take()
        p.take("(")
        params: list[str] = []
        by_value: list[str] = []
        # Scalar/vector out/inout parameters cannot be passed by reference in Python: the
        # translated function returns (value, *outs) and call statements assign them back.
        returned_outs: list[tuple[int, str, bool, str]] = []  # (index, name, is_inout, type)
        while p.peek() != ")":
            quals = []
            while p.peek() in _QUALIFIERS:
                quals.append(p.take())
            ptype = p.take()
            if ptype == "void" and p.peek() == ")":
                break
            pname = _py_name(p.take())
            is_array = False
            if p.peek() == "[":
                p.until({",", ")"})
                is_array = True
            is_out = "out" in quals or "inout" in quals
            if is_out and not (is_array or ptype in self._types):
                returned_outs.append((len(params), pname, "inout" in quals, ptype))
            params.append(pname)
            if not is_out:
                by_value.append(pname)
            if p.peek() == ",":
                p.take(",")
        p.take(")")
        if p.peek() == ";":  # prototype
            p.take(";")
            return
        body_start = p.i
        lines = [f"def {_py_name(name)}({', '.join(params)}):"]
        for prm in by_value:  # GLSL `in` parameters are copies
            lines.append(f"    {prm} = _copy({prm})")
        for _index, prm, is_inout, ptype in returned_outs:
            lines.append(f"    {prm} = _copy({prm})" if is_inout else f"    {prm} = {_undef_value(ptype)}")
        ctx = {"outs": [prm for _index, prm, _inout, _type in returned_outs]}
        try:
            p.block(1, lines, ctx)
            if returned_outs:
                lines.append(f"    return (None, {', '.join(ctx['outs'])},)")
        except GlslUnsupported as exc:
            # Skip the rest of the body and leave a stub that fails loudly when called.
            p.i = body_start
            depth = 0
            while True:
                t = p.take()
                if t == "{":
                    depth += 1
                elif t == "}":
                    depth -= 1
                    if depth == 0:
                        break
            self.unsupported[name] = str(exc)
            message = f"{self.origin}: {name}() is outside the modelled GLSL subset: {exc}"

            def stub(*_args: Any, _message: str = message) -> Any:
                raise GlslUnsupported(_message)

            self.namespace[_py_name(name)] = stub
            self.functions[name] = stub
            return
        source = "\n".join(lines)
        self.python_source[name] = source
        exec(source, self.namespace)  # noqa: S102
        self.functions[name] = self.namespace[_py_name(name)]
        if returned_outs:
            self.out_signatures[name] = [index for index, _prm, _inout, _type in returned_outs]

    # Public API ----------------------------------------------------------------
    def fn(self, name: str) -> Callable[..., Any]:
        """The translated function. One with scalar/vector `out` parameters returns
        (return_value, out_1, out_2, ...) in parameter order; pass None for pure `out`s."""
        if name not in self.functions:
            raise KeyError(f"{self.origin}: no function {name}()")
        return self.functions[name]

    def const(self, name: str) -> Any:
        return self.namespace[_py_name(name)]

    def struct(self, name: str) -> type:
        return self.namespace[name]


def load(path: Path, defines: dict[str, str] | None = None, half_rounding: str = "rte") -> GlslProgram:
    return GlslProgram(path.read_text(encoding="utf-8"), defines, half_rounding, origin=str(path))
