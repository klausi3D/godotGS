#!/usr/bin/env python3
"""Guard: GDScript examples and API references in the docs match the ClassDB bindings.

## The failure this guards against

The docs drifted from the script-visible API more than once: examples called
methods that are only declared in C++ (`GaussianSplatNode3D.get_aabb()`), used
dot access on grouped property names (`splat.rendering.opacity`, where
`rendering/opacity` is one property name), named classes that are not
registered, and API-page method tables listed members that were never bound.
Every one of them compiles in nobody's head and fails at runtime for the user
who copies it. PR #1104 fixed the known cases by hand; this guard keeps them
fixed.

## Ground truth (derived, never transcribed)

* `modules/gaussian_splatting/**/{*.h,*.cpp}` (tests and thirdparty excluded):
  every `_bind_*()` body -- `ClassDB::bind_method(D_METHOD(...), ..., DEFVAL...)`,
  `bind_static_method`, `bind_vararg_method`, `GDVIRTUAL_BIND`, `ADD_PROPERTY[I]`,
  `ADD_SIGNAL`, `BIND_*CONSTANT` -- plus `GDCLASS(Name, Parent)`.
* `modules/gaussian_splatting/register_types.cpp`: `GDREGISTER_*CLASS` (what is
  script-visible) and the `Engine::Singleton` names.
* `modules/gaussian_splatting/doc_classes/*.xml`: return types (for chaining)
  and enum names only. Existence is decided by the bindings, not by the XML.
* `doc/classes/*.xml`, `modules/*/doc_classes/*.xml`: the engine classes,
  `@GlobalScope` and `@GDScript` (global functions, singletons, builtin types).

Argument counts come from `D_METHOD` (argument names) and `DEFVAL` (defaults).
The parse fails closed: a `D_METHOD(` outside every parsed `_bind_*` body, a
registered class without a `GDCLASS`, or a missing XML corpus is an internal
error (exit 2), not a silently smaller API surface.

## What FAILS (exit 1)

In fenced GDScript blocks (```` ```gdscript ````/```` ```gd ````, or an untagged
block that reads as GDScript):

* `MEMBER_CPP_ONLY` / `MEMBER_NONEXISTENT` / `MEMBER_XML_ONLY` -- a member that
  is not bound on a receiver typed as a module class (declared type, `as` cast,
  `Class.new()`, an inferred return type, `Engine.get_singleton("...")`, or the
  script's own `extends` for bare calls). C++-only means a declaration exists
  in the class's header chain.
* `STATIC_CALL_OF_INSTANCE_METHOD` -- `Class.method()` for a non-static method
  of a class that is not an engine singleton.
* `UNTYPED_UNKNOWN_METHOD` -- `x.method()` on an untyped receiver where
  `method` is bound on no module class and exists on no engine class.
* `GROUPED_PROPERTY_DOT_ACCESS` -- `a.b.c` where `b/c` is a bound property name.
* `CLASS_NOT_REGISTERED` / `CLASS_UNKNOWN` -- a class name in a type position
  (`extends`, `: T`, `-> T`, `as T`, `is T`, `Array[T]`, `T.member`) that is a
  module `GDCLASS` without `GDREGISTER`, or no class at all.
* `ARG_COUNT` -- a call to a resolved method with the wrong number of arguments.
* `UNKNOWN_GLOBAL_FUNCTION` -- a bare call that is no GDScript/`@GlobalScope`
  function, no function defined on the page, and no member of the script's base.

In prose (backtick and `<code>` spans, outside fences):

* `PROSE_MEMBER_CPP_ONLY` / `PROSE_MEMBER_NONEXISTENT` / `PROSE_MEMBER_XML_ONLY`
  / `PROSE_CLASS_NOT_REGISTERED` -- `Class.member` with a module class. The dot
  is the script spelling; cite C++ symbols as `Class::member`.

On API pages (`docs/api/*.md` whose H1 names a registered class):

* `TABLE_METHOD_UNBOUND` -- a `name(...)` in the API cell (the first cell that
  starts with code) of a table row that is not bound on the page's class,
  unless the row says `C++`.

Markers (`MARKER_INVALID`, `MARKER_UNUSED`) are checked too, so an opt-out
cannot rot silently.

## Scope and severity

`docs/**/*.md`, except `docs/agent_memory/`, `docs/archive/`, `docs/reports/`
(historical records, not documentation of the current API). Pages under
`docs/architecture/` and any `adr-*.md` are historical design records: their
findings are reported as warnings and never fail the check.

## Opt-outs

A marker on its own line directly before a fenced block (blank lines allowed):

    <!-- snippet: pseudo reason="shows the shape of the call, not runnable code" -->
    <!-- snippet: prelude="var splat_node: GaussianSplatNode3D" -->

`pseudo` skips the block and needs a non-empty reason. `prelude` provides
declarations (separated by `;`) for a fragment, and is checked like code.

## Known limits (stated so a green run is not over-read)

This is a static approximation of the GDScript analyzer, not the analyzer.
Receivers it cannot type are only checked for "does this method exist
anywhere"; engine-typed receivers are treated the same way, because GDScript
allows calling a subclass member through a base-typed variable. Types flow
through `var`, parameters, `as`/`is`, `Class.new()` and XML return types, per
page and in document order. Methods bound only under a preprocessor condition
count as bound. A compile-only runtime tier (headless Godot `--check-only`) is
the follow-up that closes these gaps; it needs a module-enabled binary and is
not part of this guard.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import xml.parsers.expat  # in-repo XML only; the ElementTree API Bandit B314 flags is not used
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
MODULE_REL = "modules/gaussian_splatting"
EXCLUDED_PREFIXES = ("docs/agent_memory/", "docs/archive/", "docs/reports/")
WARN_ONLY_PREFIXES = ("docs/architecture/",)
UNLIMITED = 1 << 30

# ---------------------------------------------------------------------------
# C++ parsing helpers
# ---------------------------------------------------------------------------
_CPP_TOKEN = re.compile(r'("(?:\\.|[^"\\\n])*")|(//[^\n]*)|(/\*.*?\*/)', re.S)


def strip_cpp_comments(text: str) -> str:
    """Blank out comments (keeping line structure and string literals)."""

    def repl(match: re.Match[str]) -> str:
        if match.group(1):
            return str(match.group(1))
        return re.sub(r"[^\n]", " ", match.group(0))

    return _CPP_TOKEN.sub(repl, text)


def match_bracket(text: str, open_idx: int) -> int:
    """Index of the bracket closing text[open_idx]; quotes are skipped."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    opener = text[open_idx]
    closer = pairs[opener]
    depth = 0
    quote = None
    i = open_idx
    while i < len(text):
        ch = text[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(text) - 1


def split_top_level(args: str) -> list[str]:
    """Split on commas that are outside brackets and string literals."""
    out: list[str] = []
    cur: list[str] = []
    depth = 0
    quote = None
    i = 0
    while i < len(args):
        ch = args[i]
        if quote:
            cur.append(ch)
            if ch == "\\" and i + 1 < len(args):
                cur.append(args[i + 1])
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
            cur.append(ch)
        elif ch in "([{":
            depth += 1
            cur.append(ch)
        elif ch in ")]}":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    tail = "".join(cur).strip()
    if tail or out:
        out.append(tail)
    return [a for a in out if a]


# ---------------------------------------------------------------------------
# Surface model
# ---------------------------------------------------------------------------
@dataclass
class Method:
    min_args: int
    max_args: int
    static: bool = False


@dataclass
class ClassBind:
    name: str
    methods: dict[str, Method] = field(default_factory=dict)
    properties: dict[str, dict[str, Any]] = field(default_factory=dict)
    signals: set[str] = field(default_factory=set)
    constants: set[str] = field(default_factory=set)


@dataclass
class XmlClass:
    name: str
    inherits: str | None
    methods: dict[str, dict[str, Any]] = field(default_factory=dict)
    members: dict[str, str] = field(default_factory=dict)
    signals: set[str] = field(default_factory=set)
    constants: set[str] = field(default_factory=set)
    enums: set[str] = field(default_factory=set)


class SurfaceError(RuntimeError):
    """The ground truth could not be parsed completely; fail closed."""


_BIND_DEF = re.compile(r"\bvoid\s+((?:\w+::)*\w+)::(_bind_\w+)\s*\(\s*\)\s*\{")
_INLINE_BIND = re.compile(r"\bstatic\s+void\s+(_bind_\w+)\s*\(\s*\)\s*\{")
_GDCLASS = re.compile(r"\bGDCLASS\(\s*(\w+)\s*,\s*((?:\w+::)*\w+)\s*\)")
_BIND_METHOD = re.compile(r"\bClassDB::bind_method\s*\(")
_BIND_STATIC = re.compile(r"\bClassDB::bind_static_method\s*\(")
_BIND_VARARG = re.compile(r"\bbind_vararg_method\s*\([^,]*,\s*\"(\w+)\"")
_GDVIRTUAL = re.compile(r"\bGDVIRTUAL_BIND\(\s*(\w+)((?:\s*,\s*\"\w+\")*)\s*\)")
_SIGNAL = re.compile(r"\bADD_SIGNAL\(\s*MethodInfo\(\s*(?:Variant::\w+\s*,\s*)?\"(\w+)\"")
_CONST = re.compile(r"\bBIND_(?:ENUM_CONSTANT|CONSTANT|BITFIELD_FLAG)\(\s*((?:\w+::)*\w+)\s*\)")
_INT_CONST = re.compile(r"\bbind_integer_constant\([^,]+,[^,]+,\s*\"(\w+)\"")
_PROPERTY = re.compile(r"\bADD_PROPERTYI?\s*\(")
_D_METHOD = re.compile(r"\bD_METHOD\s*\(")
_REGISTER = re.compile(r"\bGDREGISTER(_ABSTRACT|_INTERNAL|_RUNTIME|_VIRTUAL)?_CLASS\(\s*(?:\w+::)*(\w+)\s*\)")
_SINGLETON_NAME = re.compile(r"\.name\s*=\s*\"(\w+)\"")
_ENUM_CAST = re.compile(r"\bVARIANT_(?:ENUM|BITFIELD)_CAST\(\s*(?:\w+::)*(\w+)::(\w+)\s*\)")

VARIANT_TYPES = {
    "NIL": None,
    "BOOL": "bool",
    "INT": "int",
    "FLOAT": "float",
    "STRING": "String",
    "VECTOR2": "Vector2",
    "VECTOR2I": "Vector2i",
    "RECT2": "Rect2",
    "RECT2I": "Rect2i",
    "VECTOR3": "Vector3",
    "VECTOR3I": "Vector3i",
    "TRANSFORM2D": "Transform2D",
    "VECTOR4": "Vector4",
    "VECTOR4I": "Vector4i",
    "PLANE": "Plane",
    "QUATERNION": "Quaternion",
    "AABB": "AABB",
    "BASIS": "Basis",
    "TRANSFORM3D": "Transform3D",
    "PROJECTION": "Projection",
    "COLOR": "Color",
    "STRING_NAME": "StringName",
    "NODE_PATH": "NodePath",
    "RID": "RID",
    "OBJECT": "Object",
    "CALLABLE": "Callable",
    "SIGNAL": "Signal",
    "DICTIONARY": "Dictionary",
    "ARRAY": "Array",
    "PACKED_BYTE_ARRAY": "PackedByteArray",
    "PACKED_INT32_ARRAY": "PackedInt32Array",
    "PACKED_INT64_ARRAY": "PackedInt64Array",
    "PACKED_FLOAT32_ARRAY": "PackedFloat32Array",
    "PACKED_FLOAT64_ARRAY": "PackedFloat64Array",
    "PACKED_STRING_ARRAY": "PackedStringArray",
    "PACKED_VECTOR2_ARRAY": "PackedVector2Array",
    "PACKED_VECTOR3_ARRAY": "PackedVector3Array",
    "PACKED_COLOR_ARRAY": "PackedColorArray",
    "PACKED_VECTOR4_ARRAY": "PackedVector4Array",
}


def _line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def _parse_bind_call(body: str, match_end: int) -> tuple[str, Method] | None:
    """Parse `bind_method(D_METHOD("name", "a", ...), &f, DEFVAL(x)...)` starting
    at the opening parenthesis that ends `match_end`."""
    open_idx = match_end - 1
    close_idx = match_bracket(body, open_idx)
    args = split_top_level(body[open_idx + 1 : close_idx])
    d_index = next((i for i, a in enumerate(args) if a.startswith("D_METHOD")), None)
    if d_index is None:
        return None
    d_text = args[d_index]
    d_open = d_text.index("(")
    d_args = split_top_level(d_text[d_open + 1 : match_bracket(d_text, d_open)])
    if not d_args or not re.fullmatch(r'"\w+"', d_args[0]):
        return None
    name = d_args[0].strip('"')
    max_args = len(d_args) - 1
    defaults = sum(1 for a in args[d_index + 1 :] if a.startswith("DEFVAL"))
    return name, Method(max(0, max_args - defaults), max_args)


class Surface:
    """Everything the checker knows about the script-visible API."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.module = root / MODULE_REL
        self.binds: dict[str, ClassBind] = {}
        self.parents: dict[str, str] = {}
        self.class_header: dict[str, str] = {}
        self.registered: dict[str, str] = {}
        self.module_singletons: set[str] = set()
        self.module_xml: dict[str, XmlClass] = {}
        self.engine_xml: dict[str, XmlClass] = {}
        self.module_enums: dict[str, set[str]] = {}
        self._header_text: dict[str, str] = {}
        self.cpp_callables: set[str] = set()
        self._parse_module()
        self._parse_xml()
        self._derive()

    # -- module C++ -------------------------------------------------------
    def _module_sources(self) -> list[Path]:
        if not self.module.is_dir():
            raise SurfaceError(f"module directory not found: {self.module}")
        out = []
        for path in sorted(self.module.rglob("*")):
            if path.suffix not in (".cpp", ".h") or not path.is_file():
                continue
            rel = path.relative_to(self.module).as_posix()
            if rel.startswith(("tests/", "thirdparty/")) or "/tests/" in rel:
                continue
            out.append(path)
        return out

    def _parse_module(self) -> None:
        stray: list[str] = []
        for path in self._module_sources():
            rel = path.relative_to(self.root).as_posix()
            text = strip_cpp_comments(path.read_text(encoding="utf-8", errors="replace"))
            if path.suffix == ".h":
                self._header_text[rel] = text
                self.cpp_callables.update(re.findall(r"\b([A-Za-z_]\w*)\s*\(", text))
            for m in _GDCLASS.finditer(text):
                self.parents[m.group(1)] = m.group(2).split("::")[-1]
                if path.suffix == ".h":
                    self.class_header.setdefault(m.group(1), rel)
            for m in _ENUM_CAST.finditer(text):
                self.module_enums.setdefault(m.group(1), set()).add(m.group(2))
            spans: list[tuple[str, int, int]] = []
            for m in _BIND_DEF.finditer(text):
                open_idx = m.end() - 1
                spans.append((m.group(1).split("::")[-1], open_idx, match_bracket(text, open_idx)))
            for m in _INLINE_BIND.finditer(text):
                owners = list(_GDCLASS.finditer(text, 0, m.start()))
                if not owners:
                    continue
                open_idx = m.end() - 1
                spans.append((owners[-1].group(1), open_idx, match_bracket(text, open_idx)))
            for cls, start, end in spans:
                self._parse_bind_body(cls, text[start : end + 1])
            for m in _D_METHOD.finditer(text):
                if not any(start <= m.start() <= end for _, start, end in spans):
                    stray.append(f"{rel}:{_line_of(text, m.start())}")
        if stray:
            raise SurfaceError(
                "D_METHOD outside every parsed _bind_* body (the binding parser would "
                "silently miss these methods): " + ", ".join(stray)
            )
        register = self.module / "register_types.cpp"
        if not register.is_file():
            raise SurfaceError(f"missing {register}")
        reg_text = strip_cpp_comments(register.read_text(encoding="utf-8"))
        for m in _REGISTER.finditer(reg_text):
            self.registered[m.group(2)] = (m.group(1) or "").strip("_") or "CLASS"
        self.module_singletons = set(_SINGLETON_NAME.findall(reg_text))
        if not self.registered:
            raise SurfaceError("no GDREGISTER_*CLASS found in register_types.cpp")
        missing = sorted(c for c in self.registered if c not in self.parents)
        if missing:
            raise SurfaceError("registered classes without a parsed GDCLASS: " + ", ".join(missing))

    def _parse_bind_body(self, cls: str, body: str) -> None:
        bind = self.binds.setdefault(cls, ClassBind(cls))
        for regex, static in ((_BIND_METHOD, False), (_BIND_STATIC, True)):
            for m in regex.finditer(body):
                parsed = _parse_bind_call(body, m.end())
                if parsed:
                    name, method = parsed
                    method.static = static
                    bind.methods[name] = method
        for m in _BIND_VARARG.finditer(body):
            bind.methods[m.group(1)] = Method(0, UNLIMITED)
        for m in _GDVIRTUAL.finditer(body):
            count = len(re.findall(r'"\w+"', m.group(2)))
            bind.methods[m.group(1)] = Method(count if count else 0, count if count else UNLIMITED)
        bind.signals.update(_SIGNAL.findall(body))
        bind.constants.update(c.split("::")[-1] for c in _CONST.findall(body))
        bind.constants.update(_INT_CONST.findall(body))
        for m in _PROPERTY.finditer(body):
            open_idx = m.end() - 1
            args = split_top_level(body[open_idx + 1 : match_bracket(body, open_idx)])
            if not args:
                continue
            info = re.match(r"PropertyInfo\s*\((.*)\)\s*$", args[0], re.S)
            if not info:
                continue
            pargs = split_top_level(info.group(1))
            if len(pargs) < 2 or not re.fullmatch(r'"[^"]+"', pargs[1]):
                continue
            vtype = pargs[0].replace("Variant::", "").strip()
            class_hint = None
            if vtype == "OBJECT" and len(pargs) > 3:
                hint = pargs[3].strip('"').split(",")[0].strip()
                class_hint = hint or None
            bind.properties[pargs[1].strip('"')] = {
                "type": class_hint or VARIANT_TYPES.get(vtype),
                "setter": args[1].strip('"') if len(args) > 1 else "",
                "getter": args[2].strip('"') if len(args) > 2 else "",
            }

    # -- XML ----------------------------------------------------------------
    @staticmethod
    def _parse_xml_file(path: Path) -> XmlClass | None:
        state: dict[str, Any] = {"cls": None, "method": None, "in_members": False, "in_constants": False}

        def start(tag: str, attrs: dict[str, str]) -> None:
            cls = state["cls"]
            if tag == "class" and cls is None:
                state["cls"] = XmlClass(attrs.get("name", ""), attrs.get("inherits"))
            elif cls is None:
                return
            elif tag == "method":
                state["method"] = {
                    "name": attrs.get("name"),
                    "return": "void",
                    "params": 0,
                    "required": 0,
                    "vararg": "vararg" in attrs.get("qualifiers", ""),
                    "static": "static" in attrs.get("qualifiers", ""),
                }
            elif tag == "return" and state["method"] is not None:
                state["method"]["return"] = attrs.get("type", "void")
            elif tag == "param" and state["method"] is not None:
                state["method"]["params"] += 1
                if attrs.get("default") is None:
                    state["method"]["required"] += 1
            elif tag == "members":
                state["in_members"] = True
            elif tag == "member" and state["in_members"]:
                cls.members[attrs.get("name", "")] = attrs.get("type", "")
            elif tag == "signal":
                cls.signals.add(attrs.get("name", ""))
            elif tag == "constant":
                cls.constants.add(attrs.get("name", ""))
                enum = attrs.get("enum")
                if enum:
                    cls.enums.add(enum.split(".")[-1])

        def end(tag: str) -> None:
            if tag == "method" and state["method"] is not None:
                m = state["method"]
                state["cls"].methods[m["name"]] = {
                    "return": m["return"],
                    "min": m["required"],
                    "max": UNLIMITED if m["vararg"] else m["params"],
                    "static": m["static"],
                }
                state["method"] = None
            elif tag == "members":
                state["in_members"] = False

        parser = xml.parsers.expat.ParserCreate()
        parser.StartElementHandler = start
        parser.EndElementHandler = end
        try:
            parser.Parse(path.read_bytes(), True)
        except xml.parsers.expat.ExpatError as exc:
            raise SurfaceError(f"malformed class XML {path}: {exc}") from exc
        parsed: XmlClass | None = state["cls"]
        return parsed

    def _load_xml_dir(self, directory: Path, into: dict[str, XmlClass]) -> None:
        for path in sorted(directory.glob("*.xml")):
            parsed = self._parse_xml_file(path)
            if parsed and parsed.name:
                into[parsed.name] = parsed

    def _parse_xml(self) -> None:
        engine_dir = self.root / "doc" / "classes"
        if not (engine_dir / "Object.xml").is_file() or not (engine_dir / "@GlobalScope.xml").is_file():
            raise SurfaceError(f"engine class XML not found under {engine_dir}")
        self._load_xml_dir(engine_dir, self.engine_xml)
        for directory in sorted((self.root / "modules").glob("*/doc_classes")):
            if directory.parent.name == "gaussian_splatting":
                self._load_xml_dir(directory, self.module_xml)
            else:
                self._load_xml_dir(directory, self.engine_xml)
        if "@GDScript" not in self.engine_xml:
            raise SurfaceError("modules/gdscript/doc_classes/@GDScript.xml not found")

    # -- derived views --------------------------------------------------------
    def _derive(self) -> None:
        self.global_functions = set(self.engine_xml["@GlobalScope"].methods) | set(self.engine_xml["@GDScript"].methods)
        self.engine_singletons = set(self.engine_xml["@GlobalScope"].members)
        self.singletons = self.engine_singletons | self.module_singletons
        self.all_method_names: set[str] = set()
        self.grouped_properties: dict[str, set[str]] = {}
        for cls, bind in self.binds.items():
            if cls not in self.registered:
                continue
            self.all_method_names |= set(bind.methods) | set(bind.signals)
            for prop, info in bind.properties.items():
                self.all_method_names |= {info["setter"], info["getter"]} - {""}
                if "/" in prop:
                    self.grouped_properties.setdefault(prop, set()).add(cls)
        for xml_cls in self.engine_xml.values():
            self.all_method_names |= set(xml_cls.methods) | set(xml_cls.signals)

    def is_module_class(self, name: str) -> bool:
        return name in self.parents and name in self.class_header

    def is_registered(self, name: str) -> bool:
        return name in self.registered

    def is_known_class(self, name: str) -> bool:
        return name in self.registered or name in self.engine_xml

    def chain(self, cls: str):
        seen: set[str] = set()
        cur: str | None = cls
        while cur and cur not in seen:
            seen.add(cur)
            yield cur
            if cur in self.parents:
                cur = self.parents[cur]
            elif cur in self.engine_xml:
                cur = self.engine_xml[cur].inherits
            else:
                cur = None

    def is_module_chain(self, cls: str | None) -> bool:
        if not cls:
            return False
        return any(c in self.registered for c in self.chain(cls))

    def lookup(self, cls: str, member: str) -> tuple[str, str | None, str, Method | None] | None:
        """(kind, result_type, owner, method) or None. kind 'xml-only' marks a
        member documented in module XML but not bound."""
        for c in self.chain(cls):
            if c in self.registered:
                bind = self.binds.get(c, ClassBind(c))
                xml_cls = self.module_xml.get(c)
                if member in bind.methods:
                    ret = xml_cls.methods[member]["return"] if xml_cls and member in xml_cls.methods else None
                    if ret is None:  # property getters are listed as members, not methods, in the XML
                        ret = next((p["type"] for p in bind.properties.values() if p["getter"] == member), None)
                    return "method", ret, c, bind.methods[member]
                if member in bind.properties:
                    return "property", bind.properties[member]["type"], c, None
                if member in bind.signals:
                    return "signal", "Signal", c, None
                if member in bind.constants:
                    return "constant", "int", c, None
                enums = self.module_enums.get(c, set()) | (xml_cls.enums if xml_cls else set())
                if member in enums:
                    return "enum", None, c, None
                if xml_cls and (member in xml_cls.methods or member in xml_cls.members or member in xml_cls.signals):
                    return "xml-only", None, c, None
                continue
            engine = self.engine_xml.get(c)
            if engine is None:
                continue
            if member in engine.methods:
                info = engine.methods[member]
                return "method", info["return"], c, Method(info["min"], info["max"], info["static"])
            if member in engine.members:
                return "property", engine.members[member], c, None
            if member in engine.signals:
                return "signal", "Signal", c, None
            if member in engine.constants or member in engine.enums:
                return "constant", "int", c, None
        return None

    def cpp_declaration(self, cls: str, member: str, call: bool) -> str | None:
        """Header in `cls`'s module chain that declares `member` (C++-only proof)."""
        pattern = re.compile(r"\b" + re.escape(member) + (r"\s*\(" if call else r"\b"))
        for c in self.chain(cls):
            header = self.class_header.get(c)
            if header and pattern.search(self._header_text.get(header, "")):
                return header
        return None

    def bound_elsewhere(self, member: str) -> list[str]:
        return sorted(
            c for c, b in self.binds.items() if c in self.registered and (member in b.methods or member in b.properties)
        )


# ---------------------------------------------------------------------------
# Findings
# ---------------------------------------------------------------------------
@dataclass
class Finding:
    path: str
    line: int
    code: str
    message: str
    excerpt: str = ""
    severity: str = "error"

    def render(self) -> str:
        text = f"{self.path}:{self.line}: {self.severity} [{self.code}] {self.message}"
        if self.excerpt:
            text += f"\n    | {self.excerpt.strip()[:160]}"
        return text


def is_warn_only(rel: str) -> bool:
    return rel.startswith(WARN_ONLY_PREFIXES) or Path(rel).name.startswith("adr-")


def is_excluded(rel: str) -> bool:
    return rel.startswith(EXCLUDED_PREFIXES)


# ---------------------------------------------------------------------------
# Markdown structure
# ---------------------------------------------------------------------------
_FENCE = re.compile(r"^(\s*)(`{3,}|~{3,})\s*([\w+-]*)")
_MARKER = re.compile(r"^\s*<!--\s*snippet:(.*?)-->\s*$")
_MARKER_LOOSE = re.compile(r"<!--\s*snippet\b")
_MARKER_TOKEN = re.compile(r'\s*(pseudo|reason|prelude)(?:\s*=\s*"([^"]*)")?')


@dataclass
class Block:
    lang: str
    first_line: int  # 1-based line of the first body line
    body: str
    marker: dict[str, Any] | None = None
    marker_line: int = 0


def parse_marker(text: str) -> tuple[dict[str, Any] | None, str | None]:
    """Returns (marker, error)."""
    marker: dict[str, Any] = {}
    pos = 0
    text = text.rstrip()
    while pos < len(text):
        m = _MARKER_TOKEN.match(text, pos)
        if not m or m.end() == pos:
            return None, f"unrecognised marker syntax at {text[pos:].strip()!r}"
        key, value = m.group(1), m.group(2)
        if key in marker:
            return None, f"duplicate marker key '{key}'"
        if key == "pseudo":
            if value is not None:
                return None, 'write pseudo as a bare flag plus reason="..."'
            marker["pseudo"] = True
        else:
            if value is None:
                return None, f"marker key '{key}' needs a quoted value"
            marker[key] = value
        pos = m.end()
    if marker.get("pseudo"):
        if not marker.get("reason", "").strip():
            return None, 'pseudo needs a non-empty reason="..."'
        if "prelude" in marker:
            return None, "a block is either pseudo or has a prelude, not both"
    elif "reason" in marker:
        return None, "reason is only meaningful with pseudo"
    elif not marker.get("prelude", "").strip():
        return None, 'marker needs pseudo reason="..." or a non-empty prelude="..."'
    return marker, None


def split_markdown(rel: str, text: str) -> tuple[list[Block], list[tuple[int, str]], list[Finding]]:
    lines = text.split("\n")
    blocks: list[Block] = []
    prose: list[tuple[int, str]] = []
    findings: list[Finding] = []
    pending: tuple[dict[str, Any], int] | None = None
    i = 0
    while i < len(lines):
        line = lines[i]
        fence = _FENCE.match(line)
        if fence:
            indent = len(fence.group(1))
            ticks = fence.group(2)
            lang = fence.group(3).lower()
            body: list[str] = []
            j = i + 1
            while j < len(lines):
                stripped = lines[j].strip()
                if stripped.startswith(ticks[0] * len(ticks)) and stripped.strip(ticks[0]) == "":
                    break
                raw = lines[j]
                body.append(raw[indent:] if raw[:indent].strip() == "" else raw.lstrip())
                j += 1
            block = Block(lang, i + 2, "\n".join(body))
            if pending:
                block.marker, block.marker_line = pending
                pending = None
            blocks.append(block)
            i = j + 1
            continue
        marker_match = _MARKER.match(line)
        if marker_match:
            if pending:
                findings.append(
                    Finding(rel, pending[1], "MARKER_UNUSED", "snippet marker is not followed by a fenced block")
                )
            marker, error = parse_marker(marker_match.group(1))
            if error or marker is None:
                findings.append(Finding(rel, i + 1, "MARKER_INVALID", error or "invalid marker", line))
                pending = None
            else:
                pending = (marker, i + 1)
            i += 1
            continue
        if _MARKER_LOOSE.search(line):
            findings.append(
                Finding(
                    rel,
                    i + 1,
                    "MARKER_INVALID",
                    "a snippet marker must be a single-line HTML comment on its own line",
                    line,
                )
            )
        if pending and line.strip():
            findings.append(
                Finding(rel, pending[1], "MARKER_UNUSED", "snippet marker is not directly followed by a fenced block")
            )
            pending = None
        prose.append((i + 1, line))
        i += 1
    if pending:
        findings.append(Finding(rel, pending[1], "MARKER_UNUSED", "snippet marker at end of file applies to no block"))
    return blocks, prose, findings


_GD_HINT = re.compile(r"^\s*(extends|func|var|@onready|@export|class_name|signal|await)\b|\.new\(\)|\$\w|:=", re.M)
_SHELL_HINT = re.compile(r"^\s*(\$ |python|scons|git |cd |pip |\./|bin/|godot|gh |mkdocs|pwsh|&)", re.M)


def block_is_gdscript(block: Block) -> bool:
    if block.lang in ("gdscript", "gd"):
        return True
    if block.lang:
        return False
    if _SHELL_HINT.search(block.body) and not _GD_HINT.search(block.body):
        return False
    if "#include" in block.body or re.search(r"->\s*\w+\s*\(|\bRef<", block.body):
        return False
    return bool(_GD_HINT.search(block.body))


# ---------------------------------------------------------------------------
# GDScript analysis
# ---------------------------------------------------------------------------
GD_KEYWORDS = set(
    """if elif else for while match when break continue pass return class class_name extends is in
    as self signal func static const enum var breakpoint preload await yield assert void true false
    null and or not PI TAU INF NAN super""".split()
)
GD_TYPE_KEYWORDS = {"void", "Variant", "int", "float", "bool", "String", "self"}
_IDENT = re.compile(r"[A-Za-z_]\w*")
_CHAIN_HEAD = re.compile(r"(?<![\w.$%@\"'/])[A-Za-z_]\w*")
_DECL = re.compile(
    r"^\s*(?:@\w+(?:\([^)]*\))?\s+)*(?:static\s+)?var\s+(\w+)\s*(?::\s*([\w.]+(?:\[[\w., ]+\])?))?\s*(:?=)?\s*(.*)$"
)
_ASSIGN = re.compile(r"^\s*(\w+)\s*=\s*(.+)$")
_FUNC_SIG = re.compile(r"^\s*(?:static\s+)?func\s+(\w+)\s*\((.*?)\)\s*(?:->\s*([\w.]+))?\s*:")
_PARAM = re.compile(r"(\w+)\s*:\s*([A-Za-z_]\w*)")
_NODE_PATH = re.compile(r"[$%](?:\"[^\"]*\"|'[^']*'|[A-Za-z0-9_/]+)")
_FILE_EXTENSIONS = {
    "xml",
    "h",
    "cpp",
    "hpp",
    "c",
    "md",
    "gd",
    "tscn",
    "tres",
    "res",
    "ply",
    "spz",
    "glsl",
    "json",
    "py",
    "yml",
    "yaml",
    "txt",
    "png",
    "exe",
    "gsplatworld",
    "gsplatcache",
    "godot",
    "import",
    "cfg",
    "ini",
    "log",
    "csv",
    "html",
    "svg",
    "jpg",
    "mp4",
}


def mask_gd_line(line: str) -> tuple[str, str]:
    """(masked, raw_without_comment). Masked blanks string contents and node paths."""
    out: list[str] = []
    raw: list[str] = []
    quote = None
    i = 0
    while i < len(line):
        ch = line[i]
        if quote:
            raw.append(ch)
            if ch == "\\" and i + 1 < len(line):
                raw.append(line[i + 1])
                out.append("  ")
                i += 2
                continue
            if ch == quote:
                quote = None
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            continue
        if ch in "\"'":
            quote = ch
            out.append(ch)
            raw.append(ch)
            i += 1
            continue
        if ch == "#":
            break
        out.append(ch)
        raw.append(ch)
        i += 1
    masked = "".join(out)
    # `$Path/To.method()` / `%Unique.method()`: an untyped node receiver.
    masked = _NODE_PATH.sub(lambda m: "__node__" + " " * max(0, len(m.group(0)) - 8), masked)
    return masked, "".join(raw)


@dataclass
class PageScope:
    """Names a page defines; blocks on one page are read as one script."""

    env: dict[str, str | None] = field(default_factory=dict)
    funcs: set[str] = field(default_factory=set)
    local_types: set[str] = field(default_factory=set)
    self_type: str | None = None


class GdAnalyzer:
    def __init__(self, surface: Surface, rel: str, findings: list[Finding]) -> None:
        self.s = surface
        self.rel = rel
        self.findings = findings

    def add(self, line: int, code: str, message: str, excerpt: str) -> None:
        self.findings.append(Finding(self.rel, line, code, message, excerpt))

    # -- names ---------------------------------------------------------------
    def known_type(self, name: str, scope: PageScope) -> bool:
        return name in GD_TYPE_KEYWORDS or self.s.is_known_class(name) or name in scope.local_types or name in scope.env

    def check_type_name(self, name: str, scope: PageScope, lineno: int, excerpt: str) -> None:
        if self.known_type(name, scope) or not re.match(r"[A-Z]", name):
            return
        if name in self.s.parents:  # a module GDCLASS (parents holds module sources only)
            self.add(
                lineno,
                "CLASS_NOT_REGISTERED",
                f"{name} is a module GDCLASS but not GDREGISTER'd, so scripts cannot see it",
                excerpt,
            )
        else:
            self.add(lineno, "CLASS_UNKNOWN", f"{name} is not a module or engine class", excerpt)

    # -- member resolution -----------------------------------------------------
    def report_missing(self, t: str, name: str, call: bool, lineno: int, excerpt: str, via: str = "") -> None:
        shown = f"{via or t}.{name}{'()' if call else ''}"
        header = self.s.cpp_declaration(t, name, call)
        if header:
            self.add(
                lineno,
                "MEMBER_CPP_ONLY",
                f"{shown} is declared in C++ ({header}) but not ClassDB-bound on {t}",
                excerpt,
            )
            return
        elsewhere = self.s.bound_elsewhere(name)
        hint = f" (bound on {', '.join(elsewhere)})" if elsewhere else ""
        self.add(lineno, "MEMBER_NONEXISTENT", f"{shown} does not exist on {t} or its bases{hint}", excerpt)

    def check_arity(self, t: str, name: str, method: Method | None, args: str, lineno: int, excerpt: str) -> None:
        if method is None:
            return
        count = len(split_top_level(args))
        if count < method.min_args or count > method.max_args:
            expected = (
                f"{method.min_args}+"
                if method.max_args >= UNLIMITED
                else str(method.min_args)
                if method.min_args == method.max_args
                else f"{method.min_args}..{method.max_args}"
            )
            self.add(lineno, "ARG_COUNT", f"{t}.{name}() takes {expected} argument(s), called with {count}", excerpt)

    def resolve_member(
        self, t: str, name: str, call: bool, args: str, lineno: int, excerpt: str, report: bool
    ) -> tuple[bool, str | None]:
        """(stop, next_type) for `<value of type t>.name`."""
        if name == "new" and call:
            return False, t
        found = self.s.lookup(t, name)
        module_receiver = self.s.is_module_chain(t)
        if found is None:
            if report:
                grouped = [p for p in self.s.grouped_properties if p.split("/")[0] == name]
                if module_receiver and not call and grouped:
                    self.add(
                        lineno,
                        "GROUPED_PROPERTY_DOT_ACCESS",
                        f"{t}.{name}.<x>: '{name}/...' are single property names ({', '.join(sorted(grouped)[:3])}...); "
                        f'use the setter/getter or set("{name}/<x>", value)',
                        excerpt,
                    )
                elif module_receiver:
                    self.report_missing(t, name, call, lineno, excerpt)
                elif call and name not in self.s.all_method_names:
                    self.add(
                        lineno,
                        "UNTYPED_UNKNOWN_METHOD",
                        f"{name}() exists on no module or engine class (receiver typed {t})",
                        excerpt,
                    )
            return True, None
        kind, next_type, owner, method = found
        if kind == "xml-only":
            if report:
                self.add(
                    lineno,
                    "MEMBER_XML_ONLY",
                    f"{t}.{name} is documented in doc_classes/{owner}.xml but not ClassDB-bound",
                    excerpt,
                )
            return True, None
        if report and call and kind == "method":
            self.check_arity(t, name, method, args, lineno, excerpt)
        return False, next_type

    def parse_chain(self, s: str, pos: int) -> tuple[list[tuple[str, bool, str]], int]:
        segs: list[tuple[str, bool, str]] = []
        i = pos
        while True:
            m = _IDENT.match(s, i)
            if not m:
                break
            name = m.group(0)
            i = m.end()
            call, args = False, ""
            j = i
            while j < len(s) and s[j] == " ":
                j += 1
            if j < len(s) and s[j] == "(":
                close = match_bracket(s, j)
                call, args, i = True, s[j + 1 : close], close + 1
            segs.append((name, call, args))
            if i < len(s) and s[i] == "[":
                segs.append(("[]", False, ""))
                break
            if i < len(s) and s[i] == "." and i + 1 < len(s) and (s[i + 1].isalpha() or s[i + 1] == "_"):
                i += 1
                continue
            break
        return segs, i

    def head_type(self, segs: list[tuple[str, bool, str]], scope: PageScope, raw: str) -> tuple[str | None, bool, int]:
        """(type, is_static_class_access, consumed_segments) for the chain head."""
        first, call, args = segs[0]
        if first == "self":
            return scope.self_type, False, 1
        if first in scope.env and not call:
            return scope.env[first], False, 1
        if first == "Engine" and len(segs) > 1 and segs[1][0] == "get_singleton":
            m = re.search(r"Engine\.get_singleton\(\s*[&]?[\"'](\w+)[\"']", raw)
            if m:
                return m.group(1), False, 2
        if not call and (self.s.is_known_class(first) or first in self.s.singletons):
            singleton = first in self.s.singletons
            return first, not singleton, 1
        if call and self.s.is_known_class(first):
            return first, False, 1  # constructor, e.g. Vector3(...)
        if call and first in self.s.global_functions:
            info = self.s.engine_xml["@GDScript"].methods.get(first) or self.s.engine_xml["@GlobalScope"].methods.get(
                first
            )
            ret = info["return"] if info else None
            return (ret if ret not in ("void", "Variant", "Object") else None), False, 1
        if call and first not in scope.funcs and first not in GD_KEYWORDS:
            base = scope.self_type or "Node"
            found = self.s.lookup(base, first)
            if found and found[0] == "method":
                return found[1], False, 1
        return None, False, 1

    def walk(self, segs, scope: PageScope, raw: str, lineno: int, excerpt: str, report: bool) -> str | None:
        t, static, consumed = self.head_type(segs, scope, raw)
        for index in range(consumed, len(segs)):
            name, call, args = segs[index]
            if t is None or name == "[]":
                return None
            if static:
                static = False
                if name == "new" and call:
                    continue
                if not self.s.is_registered(t):
                    return None  # engine statics / enums are not checked
                found = self.s.lookup(t, name)
                if found is None or found[0] == "xml-only":
                    if report:
                        if found is None:
                            self.report_missing(t, name, call, lineno, excerpt)
                        else:
                            self.add(
                                lineno,
                                "MEMBER_XML_ONLY",
                                f"{t}.{name} is documented in doc_classes XML but not ClassDB-bound",
                                excerpt,
                            )
                    return None
                kind, next_type, owner, method = found
                if report and call and kind == "method":
                    if method and not method.static and owner in self.s.registered:
                        self.add(
                            lineno,
                            "STATIC_CALL_OF_INSTANCE_METHOD",
                            f"{t}.{name}() is an instance method; call it on an instance",
                            excerpt,
                        )
                    else:
                        self.check_arity(t, name, method, args, lineno, excerpt)
                t = next_type if kind != "enum" else None
                continue
            stop, t = self.resolve_member(t, name, call, args, lineno, excerpt, report)
            if stop:
                return None
        return t

    def infer(self, expr: str, scope: PageScope, raw: str) -> str | None:
        expr = expr.strip()
        m = re.search(r"\bas\s+([A-Za-z_]\w*)\s*$", expr)
        if m:
            return m.group(1)
        m = re.match(r"\s*[A-Za-z_]\w*", expr)
        if not m or m.start() != 0:
            return None
        segs, end = self.parse_chain(expr, 0)
        if expr[end:].strip():
            return None  # an operator follows: not a plain chain
        return self.walk(segs, scope, raw, 0, "", report=False)

    # -- per page ------------------------------------------------------------
    def prescan(self, blocks: list[Block], scope: PageScope) -> None:
        for block in blocks:
            text = block.body + "\n" + "\n".join((block.marker or {}).get("prelude", "").split(";"))
            scope.funcs.update(re.findall(r"^\s*(?:static\s+)?func\s+(\w+)", text, re.M))
            scope.local_types.update(re.findall(r"^\s*class_name\s+(\w+)", text, re.M))
            scope.local_types.update(re.findall(r"^\s*class\s+(\w+)", text, re.M))
            scope.local_types.update(re.findall(r"^\s*enum\s+(\w+)", text, re.M))
            scope.local_types.update(re.findall(r"^\s*const\s+(\w+)", text, re.M))
            scope.funcs.update(re.findall(r"^\s*signal\s+(\w+)", text, re.M))

    def logical_lines(self, block: Block) -> list[tuple[int, str, str, str]]:
        """[(lineno, masked, raw_no_comment, excerpt)] with bracket continuation joined."""
        items: list[tuple[int, str, str]] = []
        if block.marker and block.marker.get("prelude"):
            for part in block.marker["prelude"].split(";"):
                if part.strip():
                    items.append((block.marker_line, part.strip(), part.strip()))
        for offset, raw in enumerate(block.body.split("\n")):
            items.append((block.first_line + offset, raw, raw))
        out: list[tuple[int, str, str, str]] = []
        buf_m, buf_r, first, depth, excerpt = "", "", 0, 0, ""
        for lineno, raw, original in items:
            masked, nocomment = mask_gd_line(raw)
            if not buf_m:
                first, excerpt = lineno, original
            joiner = " " if buf_m else ""
            buf_m += joiner + (masked.strip() if buf_m else masked)
            buf_r += joiner + (nocomment.strip() if buf_r else nocomment)
            depth += sum(masked.count(c) for c in "([{") - sum(masked.count(c) for c in ")]}")
            if depth <= 0 and not masked.rstrip().endswith("\\"):
                out.append((first, buf_m, buf_r, excerpt))
                buf_m, buf_r, depth = "", "", 0
        if buf_m:
            out.append((first, buf_m, buf_r, excerpt))
        return out

    def analyze_block(self, block: Block, scope: PageScope) -> None:
        body_with_prelude = block.body
        if block.marker and block.marker.get("prelude"):
            body_with_prelude = block.marker["prelude"].replace(";", "\n") + "\n" + block.body
        ext = re.search(r"^extends\s+([A-Za-z_]\w*)", body_with_prelude, re.M)
        if ext:
            scope.self_type = ext.group(1)
        for lineno, line, raw, excerpt in self.logical_lines(block):
            if not line.strip():
                continue
            self.analyze_line(line, raw, scope, lineno, excerpt)

    def analyze_line(self, line: str, raw: str, scope: PageScope, lineno: int, excerpt: str) -> None:
        s = self.s
        # 1. type positions
        type_names: list[str] = []
        m = re.match(r"^\s*extends\s+([A-Za-z_]\w*)", line)
        if m:
            type_names.append(m.group(1))
        type_names += re.findall(r"\b(?:var|const)\s+\w+\s*:\s*([A-Za-z_]\w*)", line)
        type_names += re.findall(r"->\s*([A-Za-z_]\w*)", line)
        type_names += re.findall(r"\bas\s+([A-Za-z_]\w*)", line)
        type_names += re.findall(r"\bis\s+(?:not\s+)?([A-Za-z_]\w*)", line)
        type_names += re.findall(r"\b(?:Array|Dictionary)\[\s*([A-Za-z_]\w*)", line)
        type_names += re.findall(r"\bDictionary\[\s*\w+\s*,\s*([A-Za-z_]\w*)", line)
        sig = _FUNC_SIG.match(line)
        if sig:
            for pname, ptype in _PARAM.findall(sig.group(2)):
                type_names.append(ptype)
                scope.env[pname] = ptype
        for name in type_names:
            self.check_type_name(name, scope, lineno, excerpt)

        # 2. declarations and assignments (types flow into scope.env)
        decl = _DECL.match(line)
        if decl:
            vname, vtype, _op, expr = decl.groups()
            if vtype:
                scope.env[vname] = vtype.split("[")[0]
            else:
                scope.env[vname] = self.infer(expr, scope, raw) if expr else None
        else:
            assign = _ASSIGN.match(line)
            if assign and assign.group(1) not in scope.env and assign.group(1) not in GD_KEYWORDS:
                inferred = self.infer(assign.group(2), scope, raw)
                if inferred:
                    scope.env[assign.group(1)] = inferred
        loop = re.match(r"^\s*for\s+(\w+)\s*(?::\s*(\w+))?\s+in\b", line)
        if loop:
            scope.env[loop.group(1)] = loop.group(2)
        narrowed = PageScope(dict(scope.env), scope.funcs, scope.local_types, scope.self_type)
        for var, typ in re.findall(r"\b(\w+)\s+is\s+([A-Z]\w*)", line):
            narrowed.env[var] = typ
        for var, typ in re.findall(r"\b(\w+)\s+as\s+([A-Z]\w*)", line):
            narrowed.env.setdefault(var, None)

        # 3. bare calls: global functions, page functions, or members of the base
        for mm in re.finditer(r"(?<![\w.$%@&^\"'])([A-Za-z_]\w*)\s*\(", line):
            fn = mm.group(1)
            if (
                fn in GD_KEYWORDS
                or fn in s.global_functions
                or fn in narrowed.funcs
                or self.known_type(fn, narrowed)
                or fn == "__node__"
            ):
                continue
            if re.search(r"\bfunc\s+$", line[: mm.start()]):
                continue
            base = narrowed.self_type or "Node"
            found = s.lookup(base, fn)
            if found and found[0] == "method":
                close = match_bracket(line, mm.end() - 1)
                self.check_arity(f"self ({base})", fn, found[3], line[mm.end() : close], lineno, excerpt)
                continue
            if s.is_module_chain(base) and narrowed.self_type:
                if found and found[0] == "xml-only":
                    self.add(
                        lineno, "MEMBER_XML_ONLY", f"{base}.{fn}() is documented in XML but not ClassDB-bound", excerpt
                    )
                else:
                    self.report_missing(base, fn, True, lineno, excerpt, via=f"self ({base})")
            else:
                self.add(
                    lineno,
                    "UNKNOWN_GLOBAL_FUNCTION",
                    f"{fn}() is not a GDScript/@GlobalScope function, not defined on this page, "
                    f"and not a member of {base}",
                    excerpt,
                )

        # 4. untyped receivers
        for mm in re.finditer(r"(?<![\w.$%@])([a-z_]\w*)\.([a-z_]\w*)\s*\(", line):
            var, meth = mm.groups()
            if var in ("self", "super") or narrowed.env.get(var) or var in s.singletons or s.is_known_class(var):
                continue
            if var in narrowed.local_types:
                continue
            if meth not in s.all_method_names and meth not in narrowed.funcs:
                declared = meth in s.cpp_callables
                shown = "$<node>" if var == "__node__" else var
                self.add(
                    lineno,
                    "UNTYPED_UNKNOWN_METHOD",
                    f"{shown}.{meth}() is bound on no module class and exists on no engine class"
                    + (" (a C++ declaration exists: C++-only)" if declared else " (declared nowhere in the module)"),
                    excerpt,
                )
        for mm in re.finditer(r"(?<![\w.$%@])([a-z_]\w*)\.([a-z_]\w*)\.([a-z_]\w*)", line):
            var, group, leaf = mm.groups()
            if var == "self" or s.is_module_chain(narrowed.env.get(var)):
                continue  # module-typed receivers are resolved (and reported) by the chain walk
            if f"{group}/{leaf}" in s.grouped_properties:
                owners = sorted(s.grouped_properties[f"{group}/{leaf}"])
                self.add(
                    lineno,
                    "GROUPED_PROPERTY_DOT_ACCESS",
                    f"{var}.{group}.{leaf}: '{group}/{leaf}' is a single property name on {', '.join(owners)}; "
                    f'use its setter or set("{group}/{leaf}", value)',
                    excerpt,
                )

        # 5. typed chains
        i = 0
        while True:
            head_m = _CHAIN_HEAD.search(line, i)
            if not head_m:
                break
            i = head_m.end()
            segs, _end = self.parse_chain(line, head_m.start())
            head = segs[0][0]
            if head in ("func", "var", "extends", "class_name", "signal", "const", "class", "enum", "__node__"):
                continue
            if len(segs) < 2:
                continue
            if re.match(r"[A-Z]", head) and head not in narrowed.env and not segs[0][1]:
                self.check_type_name(head, narrowed, lineno, excerpt)
                if not self.known_type(head, narrowed) and head not in s.singletons:
                    continue
            self.walk(segs, narrowed, raw, lineno, excerpt, report=True)


# ---------------------------------------------------------------------------
# Prose and API tables
# ---------------------------------------------------------------------------
_CODE_SPAN = re.compile(r"(`+)(.+?)\1|<code>(.*?)</code>")
_PROSE_REF = re.compile(r"(?<![\w/\\.:$@-])([A-Z][A-Za-z0-9_]*)\.([A-Za-z_]\w*(?:/\w+)*)(\s*\()?")


def code_spans(line: str) -> list[str]:
    return [m.group(2) if m.group(2) is not None else m.group(3) for m in _CODE_SPAN.finditer(line)]


def check_prose(surface: Surface, rel: str, prose: list[tuple[int, str]], findings: list[Finding]) -> None:
    for lineno, line in prose:
        for span in code_spans(line):
            for m in _PROSE_REF.finditer(span):
                cls, member, call = m.group(1), m.group(2), bool(m.group(3))
                if member in _FILE_EXTENSIONS and not call:
                    continue  # a file name such as GaussianSplatNode3D.xml
                if not surface.is_registered(cls):
                    if cls in surface.parents:
                        findings.append(
                            Finding(
                                rel,
                                lineno,
                                "PROSE_CLASS_NOT_REGISTERED",
                                f"`{cls}.{member}`: {cls} is not registered, so it has no script API; "
                                f"cite C++ as `{cls}::{member}`",
                                span,
                            )
                        )
                    continue
                found = surface.lookup(cls, member)
                if found is None:
                    header = surface.cpp_declaration(cls, member, call)
                    if header:
                        findings.append(
                            Finding(
                                rel,
                                lineno,
                                "PROSE_MEMBER_CPP_ONLY",
                                f"`{cls}.{member}` is C++-only (declared in {header}, not bound); "
                                f"cite it as `{cls}::{member}`",
                                span,
                            )
                        )
                    else:
                        findings.append(
                            Finding(
                                rel,
                                lineno,
                                "PROSE_MEMBER_NONEXISTENT",
                                f"`{cls}.{member}` does not exist on {cls} or its bases",
                                span,
                            )
                        )
                elif found[0] == "xml-only":
                    findings.append(
                        Finding(
                            rel,
                            lineno,
                            "PROSE_MEMBER_XML_ONLY",
                            f"`{cls}.{member}` is in doc_classes XML but not ClassDB-bound",
                            span,
                        )
                    )


_TABLE_ROW_HTML = re.compile(r"<tr>(.*?)</tr>", re.S)
_TABLE_CELL_HTML = re.compile(r"<t([dh])>(.*?)</t[dh]>", re.S)
_TABLE_METHOD = re.compile(r"^\s*([a-z_]\w*)\s*\(")


def page_class(surface: Surface, rel: str, text: str) -> str | None:
    if not rel.startswith("docs/api/"):
        return None
    m = re.search(r"^#\s+`?([A-Za-z_]\w*)`?", text, re.M)
    if m and surface.is_registered(m.group(1)):
        return m.group(1)
    return None


def table_rows(text: str, fenced: set[int]) -> list[tuple[int, list[tuple[bool, str]]]]:
    """[(line, [(is_header, cell_html)])] for HTML and pipe tables outside fences."""
    rows: list[tuple[int, list[tuple[bool, str]]]] = []
    for m in _TABLE_ROW_HTML.finditer(text):
        row_line = _line_of(text, m.start())
        if row_line in fenced:
            continue
        rows.append((row_line, [(kind == "h", cell) for kind, cell in _TABLE_CELL_HTML.findall(m.group(1))]))
    lines = text.split("\n")
    for index, line in enumerate(lines):
        if index + 1 in fenced or not line.lstrip().startswith("|"):
            continue
        if re.fullmatch(r"\s*\|?[\s:|-]+\|?\s*", line):
            continue
        is_header = (
            index + 1 < len(lines) and re.fullmatch(r"\s*\|?[\s:|-]+\|?\s*", lines[index + 1] or "x") is not None
        )
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        rows.append((index + 1, [(is_header, c) for c in cells]))
    return rows


def check_api_tables(surface: Surface, rel: str, text: str, fenced: set[int], findings: list[Finding]) -> None:
    cls = page_class(surface, rel, text)
    if not cls:
        return
    for line, cells in table_rows(text, fenced):
        if not cells or any(is_header for is_header, _ in cells):
            continue
        if any("C++" in cell for _, cell in cells):
            continue
        api_cell = next((cell for _, cell in cells if re.match(r"\s*(<code>|`)", cell)), None)
        if api_cell is None:
            continue
        for span in code_spans(api_cell):
            m = _TABLE_METHOD.match(span)
            if not m:
                continue
            name = m.group(1)
            found = surface.lookup(cls, name)
            if found is not None and found[0] != "xml-only":
                continue
            header = surface.cpp_declaration(cls, name, True)
            detail = (
                f"C++-only (declared in {header}, not bound)"
                if header
                else "documented in XML only"
                if found
                else "does not exist"
            )
            findings.append(
                Finding(
                    rel,
                    line,
                    "TABLE_METHOD_UNBOUND",
                    f"{cls} API table lists {name}(): {detail}; mark the row C++ or remove it",
                    span,
                )
            )


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------
def check_text(surface: Surface, rel: str, text: str) -> list[Finding]:
    findings: list[Finding] = []
    blocks, prose, marker_findings = split_markdown(rel, text)
    findings.extend(marker_findings)
    analyzer = GdAnalyzer(surface, rel, findings)
    scope = PageScope()
    gd_blocks = []
    fenced: set[int] = set()
    for block in blocks:
        fenced.update(range(block.first_line - 1, block.first_line + block.body.count("\n") + 2))
        is_gd = block_is_gdscript(block)
        if block.marker and not is_gd:
            findings.append(
                Finding(
                    rel,
                    block.marker_line,
                    "MARKER_UNUSED",
                    f"snippet marker precedes a '{block.lang or 'untagged'}' block that is not checked as GDScript",
                )
            )
            continue
        if is_gd and not (block.marker and block.marker.get("pseudo")):
            gd_blocks.append(block)
    analyzer.prescan(gd_blocks, scope)
    for block in gd_blocks:
        analyzer.analyze_block(block, scope)
    check_prose(surface, rel, prose, findings)
    check_api_tables(surface, rel, text, fenced, findings)
    severity = "warning" if is_warn_only(rel) else "error"
    unique: dict[tuple[int, str, str], Finding] = {}
    for f in findings:
        f.severity = severity
        unique.setdefault((f.line, f.code, f.message), f)
    return sorted(unique.values(), key=lambda f: (f.line, f.code, f.message))


def doc_files(docs_root: Path) -> list[tuple[Path, str]]:
    base = docs_root / "docs"
    out = []
    for path in sorted(base.rglob("*.md")):
        rel = path.relative_to(docs_root).as_posix()
        if not is_excluded(rel):
            out.append((path, rel))
    return out


def run(root: Path, docs_root: Path, files: list[str] | None = None) -> tuple[list[Finding], int]:
    surface = Surface(root)
    targets: list[tuple[Path, str]]
    if files:
        targets = []
        for f in files:
            path = (docs_root / f) if not Path(f).is_absolute() else Path(f)
            rel = path.resolve().relative_to(docs_root.resolve()).as_posix()
            if not is_excluded(rel):
                targets.append((path, rel))
    else:
        targets = doc_files(docs_root)
    findings: list[Finding] = []
    for path, rel in targets:
        findings.extend(check_text(surface, rel, path.read_text(encoding="utf-8", errors="replace")))
    return findings, len(targets)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("files", nargs="*", help="markdown files (default: docs/**/*.md)")
    parser.add_argument("--root", type=Path, default=ROOT, help="repository whose bindings are the ground truth")
    parser.add_argument("--docs-root", type=Path, default=None, help="directory containing docs/ (default: --root)")
    parser.add_argument("--json", action="store_true", help="print findings as JSON")
    args = parser.parse_args(argv)
    docs_root = args.docs_root or args.root
    try:
        findings, scanned = run(args.root, docs_root, args.files or None)
    except SurfaceError as exc:
        print(f"check_doc_snippets: internal error, refusing to pass: {exc}", file=sys.stderr)
        return 2
    if scanned == 0:
        print("check_doc_snippets: no markdown files scanned; refusing to pass vacuously", file=sys.stderr)
        return 2
    errors = [f for f in findings if f.severity == "error"]
    warnings = [f for f in findings if f.severity == "warning"]
    if args.json:
        print(json.dumps([f.__dict__ for f in findings], indent=1))
    else:
        for f in warnings + errors:
            print(f.render())
        print(
            f"check_doc_snippets: {scanned} pages, {len(errors)} error(s), "
            f"{len(warnings)} warning(s) (warnings: docs/architecture/ and ADRs are historical)"
        )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
