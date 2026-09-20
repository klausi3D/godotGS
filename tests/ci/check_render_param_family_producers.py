#!/usr/bin/env python3
"""Every live TileRenderParams producer must populate every registered field family.

WHY THIS GUARD EXISTS (#1018, #851)
-----------------------------------
``TileRenderParams`` is filled member-by-member by more than one producer. The
baseline raster producer (``renderer/render_pipeline_stages.cpp``) assigns
almost all of it; the painterly producer
(``interfaces/painterly_renderer.cpp``) assigned a subset, and nothing told
anyone which subset. The wind group was the sharpest case: painterly assigned
**none** of its six fields, so ``wind_time_seconds`` stayed at its struct
default ``0.0f`` on every painterly frame. That is not "wind off" -- the shader
substitutes ``strength = 1.0`` when the pass-global strength is zero
(``shaders/includes/gs_deformation.glsl:210-217``), so a node with
``rendering/wind_override_enabled`` rendered a *static* displacement with a
phase that never advanced. A wrong image, silently, for as long as the gap
existed.

Fixing the six assignments once does not stop it happening again. What stops it
is that the family can only be written through one function, and that every
live producer is checked for calling it.

THE CONTRACT
------------
For each family in ``FAMILIES``:

* the family's fields are derived from ``renderer/tile_render_types.h`` by
  prefix -- **ground truth, not a hand-written list**, so a seventh wind field
  is covered the day it is added rather than the day someone remembers;
* the shared applier function must exist and must assign every one of them;
* every producer in ``LIVE_PRODUCERS`` must call that applier;
* nothing outside the applier may assign the fields directly, so a producer
  cannot half-populate the family while still "calling the applier".

FAIL-CLOSED RULES
-----------------
1. A family whose prefix matches no field in the struct. The prefix is stale or
   the struct moved; "no fields to check" must never read as "checked".
2. The applier function is missing from the header.
3. The applier does not assign every field of its family.
4. A live producer does not call the applier.
5. A field of a family is assigned outside the applier.
6. A registered file (producer, header) does not exist.
7. A ``DEAD_PRODUCERS`` entry that has acquired a caller -- it is live now, and
   an exemption written when it was unreachable no longer applies.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[2]
MODULE = Path("modules/gaussian_splatting")
TYPES_HEADER = MODULE / "renderer" / "tile_render_types.h"

#: family name -> (field-name prefix, shared applier function name)
FAMILIES: Dict[str, Tuple[str, str]] = {
    "wind": ("wind_", "apply_wind_to_render_params"),
}

#: family name -> (ProjectSettings path prefix, the one file allowed to read it)
#:
#: Rule 8. The applier rules above only look at TileRenderParams producers, so
#: they were blind to a THIRD hand-kept copy of the eight `animation/wind_*`
#: settings in the depth/sort pass (`interfaces/gpu_sorting_pipeline.cpp`),
#: which fills InstanceDepthParamsGPU instead. Independent review on #1033 found
#: it after the PR body had already claimed the shared reader replaced every
#: copy. Guarding the WRITE side and not the READ side is how that claim could
#: be false and green at the same time.
SETTING_READERS: Dict[str, Tuple[str, Path]] = {
    "wind": (
        "rendering/gaussian_splatting/animation/wind_",
        MODULE / "core" / "gs_project_settings.h",
    ),
}

#: Where rule 8 looks for stray setting reads.
SETTING_SEARCH_ROOTS: Tuple[Path, ...] = (MODULE,)

#: Files allowed to name a family's setting paths without reading them: the
#: settings manifest / registration sites, which must name every setting.
SETTING_READER_EXEMPT: Tuple[Path, ...] = (
    MODULE / "core" / "gaussian_splat_manager.cpp",
)

#: Producers that build a TileRenderParams and are REACHABLE. Each must call
#: every family's applier.
LIVE_PRODUCERS: Tuple[Path, ...] = (
    MODULE / "renderer" / "render_pipeline_stages.cpp",
    MODULE / "interfaces" / "painterly_renderer.cpp",
)

#: Producers that build a TileRenderParams but have no caller. They are exempt
#: only for as long as that stays true -- rule 7 re-checks it every run rather
#: than trusting this comment.
#: path -> (gate type, reason)
#:
#: The GATE TYPE is what makes rule 7 sound rather than a name-match guess. The
#: dead chain can only be entered by constructing a value of this type, so the
#: guard looks for a CONSTRUCTION of it, not for calls to a method whose bare
#: name ("render") half the module also uses. Declared limit: this proves the
#: chain's entry point is never constructed in-tree; it is not a proof that no
#: exotic route reaches the code.
DEAD_PRODUCERS: Dict[Path, Tuple[str, str]] = {
    MODULE / "interfaces" / "tile_rasterizer.cpp": (
        "PainterlyRenderInput",
        "TileRasterizer::render(const RasterParams &) builds a second painterly G-buffer "
        "parameter set. Its only caller is PainterlyRenderer::render(const PainterlyRenderInput &), "
        "and no code anywhere in modules/ or tests/ constructs a PainterlyRenderInput, so that "
        "entry point is unreachable. Exempt while that holds; rule 7 fails this guard the moment "
        "anything constructs one.",
    ),
}

#: Files allowed to mention the gate type without that counting as a
#: construction: the struct's own definition, the interface that declares the
#: entry point, and the dead implementation itself.
GATE_TYPE_DECLARATION_FILES: Tuple[Path, ...] = (
    MODULE / "interfaces" / "painterly_renderer_interfaces.h",
    MODULE / "interfaces" / "painterly_renderer.h",
    MODULE / "interfaces" / "painterly_renderer.cpp",
)

#: Where the dead-producer reachability check looks for call sites.
CALL_SITE_SEARCH_ROOTS: Tuple[Path, ...] = (MODULE, Path("tests"))

#: An assignment to `.field`, including compound assignment, but NOT `==`.
#: The plain `=` form matched `==` (a false positive, at least fail-closed) and
#: missed `+=` / `*=` entirely (a false negative, which is not). #1033 review.
ASSIGN_RE_TEMPLATE = r"\.\s*{field}\s*(?:[-+*/|&^]|<<|>>)?=(?!=)"


def _read(path: Path) -> str:
    resolved = ROOT / path
    if not resolved.is_file():
        raise FileNotFoundError(f"registered file '{path.as_posix()}' does not exist")
    return resolved.read_text(encoding="utf-8", errors="replace")


def _struct_fields(header_text: str, prefix: str) -> List[str]:
    """Field names of TileRenderParams starting with `prefix`, from the struct itself."""
    start = header_text.find("struct TileRenderParams {")
    if start < 0:
        raise ValueError("struct TileRenderParams not found in the types header")
    # The struct ends at the first line that is exactly "};" at column 0.
    end = header_text.find("\n};", start)
    if end < 0:
        raise ValueError("could not find the end of struct TileRenderParams")
    body = header_text[start:end]
    pattern = re.compile(r"^\s*(?:[\w:<>,\s\*&]+?)\s(" + re.escape(prefix) + r"\w+)\s*(?:=|;|\{)", re.M)
    seen: Dict[str, None] = {}
    for match in pattern.finditer(body):
        seen.setdefault(match.group(1), None)
    return list(seen)


def _applier_body(header_text: str, name: str) -> str | None:
    start = header_text.find(f"void {name}(")
    if start < 0:
        return None
    brace = header_text.find("{", start)
    if brace < 0:
        return None
    depth = 0
    for index in range(brace, len(header_text)):
        if header_text[index] == "{":
            depth += 1
        elif header_text[index] == "}":
            depth -= 1
            if depth == 0:
                return header_text[brace:index + 1]
    return None


def check(root: Path = ROOT) -> Tuple[List[str], List[str]]:
    failures: List[str] = []
    notes: List[str] = []

    try:
        header_text = _read(TYPES_HEADER)
    except FileNotFoundError as exc:
        return [str(exc)], notes

    for family, (prefix, applier) in sorted(FAMILIES.items()):
        try:
            fields = _struct_fields(header_text, prefix)
        except ValueError as exc:
            failures.append(f"family '{family}': {exc}")
            continue

        # Rule 1.
        if not fields:
            failures.append(
                f"family '{family}': no TileRenderParams field starts with '{prefix}'. "
                f"The prefix is stale or the struct moved; this guard would silently check nothing."
            )
            continue
        notes.append(f"family '{family}': {len(fields)} field(s) -> {', '.join(sorted(fields))}")

        # Rules 2 and 3.
        body = _applier_body(header_text, applier)
        if body is None:
            failures.append(
                f"family '{family}': shared applier '{applier}' not found in "
                f"{TYPES_HEADER.as_posix()}. Without it there is no single place the family is written."
            )
            continue
        missing = [f for f in fields
                   if not re.search(ASSIGN_RE_TEMPLATE.format(field=re.escape(f)), body)]
        if missing:
            failures.append(
                f"family '{family}': '{applier}' does not assign {', '.join(sorted(missing))}. "
                f"A producer calling it would still leave those at their struct defaults."
            )

        # Rule 4.
        for producer in LIVE_PRODUCERS:
            try:
                text = _read(producer)
            except FileNotFoundError as exc:
                failures.append(str(exc))
                continue
            # A mention in a comment or a string is not a call -- including a
            # trailing `// ...` on an otherwise real line of code, which a
            # start-of-line comment test does not catch (Codex review).
            calls_applier = any(
                f"{applier}(" in line for line in _code_lines(text, keep_strings=False)
            )
            if not calls_applier:
                failures.append(
                    f"family '{family}': live producer {producer.as_posix()} never calls "
                    f"'{applier}'. It builds a TileRenderParams and would ship the family at its "
                    f"struct defaults -- which for wind means a frozen clock, not wind off (#1018)."
                )

        # Rule 5.
        for producer in LIVE_PRODUCERS:
            try:
                text = _read(producer)
            except FileNotFoundError:
                continue
            # Comments and string literals removed first: documenting
            # `p.wind_enabled = ...` in a comment used to be reported as a real
            # assignment (Codex review).
            code = "\n".join(_code_lines(text, keep_strings=False))
            for field in fields:
                if re.search(ASSIGN_RE_TEMPLATE.format(field=re.escape(field)), code):
                    failures.append(
                        f"family '{family}': {producer.as_posix()} assigns '{field}' directly. "
                        f"The family must be written only through '{applier}', or a producer can "
                        f"populate half of it and still satisfy rule 4."
                    )

    # Rule 8: one reader per family.
    for family, (setting_prefix, reader) in sorted(SETTING_READERS.items()):
        try:
            reader_text = _read(reader)
        except FileNotFoundError as exc:
            failures.append(f"family '{family}': {exc}")
            continue
        if setting_prefix not in reader_text:
            failures.append(
                f"family '{family}': the designated reader {reader.as_posix()} does not mention "
                f"'{setting_prefix}'. Either the prefix is stale or the reader moved; this rule "
                f"would then permit every copy it exists to forbid."
            )
            continue
        strays = _find_setting_readers(root, setting_prefix, allowed=reader)
        for stray in strays:
            failures.append(
                f"family '{family}': {stray} reads '{setting_prefix}*' directly instead of going "
                f"through {reader.as_posix()}. A second copy of the setting list can drift from the "
                f"first without anything noticing -- which is exactly how the depth/sort pass kept "
                f"its own wind reader (#1033 review)."
            )

    # Rules 6 and 7.
    for dead, (gate_type, reason) in sorted(DEAD_PRODUCERS.items()):
        if not reason.strip():
            failures.append(f"DEAD_PRODUCERS['{dead.as_posix()}'] has an empty reason.")
        try:
            _read(dead)
        except FileNotFoundError:
            failures.append(
                f"DEAD_PRODUCERS['{dead.as_posix()}'] names a file that does not exist; "
                f"remove the stale entry."
            )
            continue
        constructions = _find_gate_type_constructions(root, gate_type)
        if constructions:
            failures.append(
                f"DEAD_PRODUCERS['{dead.as_posix()}'] is no longer unreachable -- a "
                f"'{gate_type}' is constructed in {', '.join(sorted(constructions))}. A producer "
                f"that has acquired a caller must move to LIVE_PRODUCERS and populate every family."
            )
        else:
            notes.append(
                f"dead producer {dead.as_posix()}: no '{gate_type}' is constructed in-tree, "
                f"so its entry point is still unreachable"
            )

    return failures, notes


def _find_setting_readers(root: Path, setting_prefix: str, allowed: Path) -> List[str]:
    """Files other than `allowed` that name `setting_prefix`, comments excluded.

    Also catches the obvious way to defeat a full-literal search: splitting the
    path, as in `"rendering/.../animation/" "wind_enabled"` or a `+` concat
    (Codex review). A bare `"wind_<leaf>"` string literal in module C++ is a
    settings leaf in practice, so it is treated as a read.

    Declared limit: still lexical. A path assembled from a variable, or from
    pieces that do not include a recognisable leaf, is not detected. This rule
    raises the cost of a second copy; it does not make one impossible.
    """
    allowed_resolved = (root / allowed).resolve()
    exempt = {(root / p).resolve() for p in SETTING_READER_EXEMPT}
    # The leaf half of the prefix, e.g. "wind_" from ".../animation/wind_".
    leaf_prefix = setting_prefix.rsplit("/", 1)[-1]
    split_literal_re = (
        re.compile(r'"' + re.escape(leaf_prefix) + r'[A-Za-z0-9_]+"')
        if leaf_prefix else None
    )
    hits: List[str] = []
    for search_root in SETTING_SEARCH_ROOTS:
        base = root / search_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp"):
                continue
            resolved = path.resolve()
            if resolved == allowed_resolved or resolved in exempt:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            # Comments stripped; string literals KEPT, because the setting path
            # being looked for lives inside one.
            for line in _code_lines(text, keep_strings=True):
                if setting_prefix in line or (
                        split_literal_re is not None and split_literal_re.search(line)):
                    hits.append(resolved.relative_to(root).as_posix())
                    break
    return hits


_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT_RE = re.compile(r"//.*$")
_STRING_LITERAL_RE = re.compile(r'"(?:[^"\\]|\\.)*"')


def _is_comment_line(line: str) -> bool:
    stripped = line.strip()
    return stripped.startswith(("//", "*", "/*", "#"))


def _strip_block_comments(text: str) -> str:
    """Blank out /* ... */ spans, preserving newlines so line numbers survive."""
    return _BLOCK_COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def _code_lines(text: str, keep_strings: bool) -> List[str]:
    """Lines with comments removed, and optionally string literals blanked too.

    A trailing `// ...` on a line of real code used to defeat rule 4 and trip
    rule 5, because the only filter was "does the line START with a comment"
    (Codex review). Rules that look for CODE (calls, assignments) blank string
    literals as well; rule 8 looks for a setting path, which lives INSIDE a
    string literal, so it keeps them.

    Declared limit: this is lexical, not a parser. A `//` inside a string
    literal on a line where strings are kept will over-truncate that line.
    """
    text = _strip_block_comments(text)
    out: List[str] = []
    for line in text.splitlines():
        if not keep_strings:
            line = _STRING_LITERAL_RE.sub('""', line)
        line = _LINE_COMMENT_RE.sub("", line)
        out.append(line)
    return out


def _find_gate_type_constructions(root: Path, gate_type: str) -> List[str]:
    """Files that CONSTRUCT `gate_type`, excluding the files that declare it.

    A declaration (`struct X {`, `const X &p`, `virtual f(const X &) = 0`) is not
    a construction. A construction is `X name`, `X{`, or `X(` in a value context.
    """
    decl_context = re.compile(
        r"(struct|class)\s+" + re.escape(gate_type) + r"\b"
        r"|(const\s+)?" + re.escape(gate_type) + r"\s*&"
    )
    construct = re.compile(
        r"\b" + re.escape(gate_type) + r"\s*(?:\{|\(|[A-Za-z_]\w*\s*[;={])"
    )
    declaration_files = {(root / p).resolve() for p in GATE_TYPE_DECLARATION_FILES}
    hits: List[str] = []
    for search_root in CALL_SITE_SEARCH_ROOTS:
        base = root / search_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp"):
                continue
            if path.resolve() in declaration_files:
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for match in construct.finditer(text):
                line_start = text.rfind("\n", 0, match.start()) + 1
                line_end = text.find("\n", match.start())
                line = text[line_start:line_end if line_end > 0 else len(text)]
                stripped = line.strip()
                if stripped.startswith(("//", "*", "/*", "#")):
                    continue
                if decl_context.search(line):
                    continue
                hits.append(path.resolve().relative_to(root).as_posix())
                break
    return hits


def _self_test() -> int:
    cases: List[Tuple[str, bool]] = []

    header_ok = (
        "struct TileRenderParams {\n"
        "\tbool wind_enabled = false;\n"
        "\tVector3 wind_direction = Vector3();\n"
        "\tfloat wind_time_seconds = 0.0f;\n"
        "};\n"
        "inline void apply_wind_to_render_params(TileRenderParams &r, int s) {\n"
        "\tr.wind_enabled = s;\n\tr.wind_direction = s;\n\tr.wind_time_seconds = s;\n"
        "}\n"
    )
    header_partial = header_ok.replace("\tr.wind_time_seconds = s;\n", "")
    header_no_applier = header_ok[:header_ok.find("inline void")]
    producer_ok = "void f() {\n\tTileRenderParams p;\n\tapply_wind_to_render_params(p, 1);\n}\n"
    producer_missing = "void f() {\n\tTileRenderParams p;\n}\n"
    producer_direct = producer_ok.replace("}\n", "\tp.wind_enabled = true;\n}\n")

    with tempfile.TemporaryDirectory() as tmp:
        fake = Path(tmp)

        def run(header: str, producers: Dict[str, str], prefix: str = "wind_",
                dead: Dict[Path, Tuple[str, str]] | None = None,
                readers: Dict[str, Tuple[str, Path]] | None = None,
                extra_files: Dict[str, str] | None = None) -> List[str]:
            (fake / TYPES_HEADER).parent.mkdir(parents=True, exist_ok=True)
            (fake / TYPES_HEADER).write_text(header, encoding="utf-8")
            live: List[Path] = []
            for rel, body in producers.items():
                target = fake / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(body, encoding="utf-8")
                live.append(Path(rel))
            # Written but NOT registered as producers: files that exist only to
            # be seen (or not seen) by the reader rule.
            for rel, body in (extra_files or {}).items():
                target = fake / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(body, encoding="utf-8")
            global ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS, SETTING_READERS
            saved = (ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS, SETTING_READERS)
            ROOT = fake
            LIVE_PRODUCERS = tuple(live)
            FAMILIES = {"wind": (prefix, "apply_wind_to_render_params")}
            DEAD_PRODUCERS = dead or {}
            SETTING_READERS = readers or {}
            try:
                return check(fake)[0]
            finally:
                ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS, SETTING_READERS = saved

        cases.append(("baseline: compliant producer accepted",
                      not run(header_ok, {"a.cpp": producer_ok})))
        cases.append(("rule 1: prefix matching no field",
                      any("no TileRenderParams field starts with" in f
                          for f in run(header_ok, {"a.cpp": producer_ok}, prefix="gust_"))))
        cases.append(("rule 2: applier missing from the header",
                      any("not found in" in f
                          for f in run(header_no_applier, {"a.cpp": producer_ok}))))
        cases.append(("rule 3: applier does not assign every field",
                      any("does not assign" in f
                          for f in run(header_partial, {"a.cpp": producer_ok}))))
        cases.append(("rule 4: live producer never calls the applier",
                      any("never calls" in f
                          for f in run(header_ok, {"a.cpp": producer_missing}))))
        cases.append(("rule 5: live producer assigns a family field directly",
                      any("directly" in f
                          for f in run(header_ok, {"a.cpp": producer_direct}))))
        cases.append(("rule 4 is a CALL, not a substring: a comment mentioning the applier",
                      any("never calls" in f for f in run(
                          header_ok,
                          {"a.cpp": "void f() {\n\tTileRenderParams p;\n"
                                    "\t// call apply_wind_to_render_params(p, 1) here one day\n}\n"}))))
        cases.append(("rule 5 catches a compound assignment (+=)",
                      any("directly" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok.replace("}\n", "\tp.wind_enabled += 1;\n}\n")}))))
        cases.append(("rule 5 does not fire on an equality comparison (==)",
                      not any("directly" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok.replace("}\n", "\tif (p.wind_enabled == 1) {}\n}\n")}))))

        reader_path = MODULE / "core" / "gs_project_settings.h"
        stray_path = MODULE / "interfaces" / "some_other_pass.cpp"
        reader_src = 'get_bool(ps, "rendering/gaussian_splatting/animation/wind_enabled", false);\n'
        wind_readers = {"wind": ("rendering/gaussian_splatting/animation/wind_", reader_path)}
        cases.append(("rule 8: a second file reading the family's settings directly",
                      any("reads" in f and "directly" in f for f in run(
                          header_ok, {"a.cpp": producer_ok}, readers=wind_readers,
                          extra_files={reader_path.as_posix(): reader_src,
                                       stray_path.as_posix(): reader_src}))))
        cases.append(("rule 8 negative: only the designated reader reads them",
                      not run(header_ok, {"a.cpp": producer_ok}, readers=wind_readers,
                              extra_files={reader_path.as_posix(): reader_src,
                                           stray_path.as_posix(): "// nothing to see\n"})))
        cases.append(("rule 8 negative: a COMMENT naming the settings is not a read",
                      not run(header_ok, {"a.cpp": producer_ok}, readers=wind_readers,
                              extra_files={reader_path.as_posix(): reader_src,
                                           stray_path.as_posix():
                                               "// see rendering/gaussian_splatting/animation/wind_enabled\n"})))
        cases.append(("rule 4: a TRAILING comment naming the applier is not a call",
                      any("never calls" in f for f in run(
                          header_ok,
                          {"a.cpp": "void f() {\n\tTileRenderParams p;"
                                    "\t// apply_wind_to_render_params(p, 1)\n}\n"}))))
        cases.append(("rule 4: the applier named in a STRING is not a call",
                      any("never calls" in f for f in run(
                          header_ok,
                          {"a.cpp": 'void f() {\n\tTileRenderParams p;\n'
                                    '\tlog("apply_wind_to_render_params(p)");\n}\n'}))))
        cases.append(("rule 5 negative: a COMMENT documenting an assignment is not one",
                      not any("directly" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok.replace(
                              "}\n", "\t// sets p.wind_enabled = true internally\n}\n")}))))
        cases.append(("rule 5 negative: a BLOCK comment documenting an assignment is not one",
                      not any("directly" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok.replace(
                              "}\n", "\t/* p.wind_strength = 1.0f; */\n}\n")}))))
        cases.append(("rule 8: a SPLIT string literal still counts as a read",
                      any("reads" in f and "directly" in f for f in run(
                          header_ok, {"a.cpp": producer_ok}, readers=wind_readers,
                          extra_files={reader_path.as_posix(): reader_src,
                                       stray_path.as_posix():
                                           'get_bool(ps, base_path + "wind_enabled", false);\n'}))))
        cases.append(("rule 8: designated reader that no longer mentions the prefix",
                      any("does not mention" in f for f in run(
                          header_ok, {"a.cpp": producer_ok}, readers=wind_readers,
                          extra_files={reader_path.as_posix(): "// moved away\n"}))))

        cases.append(("rule 6: registered producer file missing",
                      _missing_file_fires(fake, header_ok)))
        dead_path = Path("modules/gaussian_splatting/interfaces/tile_rasterizer.cpp")
        # The construction has to live under a CALL_SITE_SEARCH_ROOT, or the
        # scan would not look at it and the case would pass for the wrong reason.
        caller_path = MODULE / "renderer" / "some_caller.cpp"
        cases.append(("rule 7: dead producer whose gate type is now constructed",
                      any("no longer unreachable" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok,
                           caller_path.as_posix(): "void g(){ PainterlyRenderInput in; use(in); }\n",
                           dead_path.as_posix(): "void dead(){}\n"},
                          dead={dead_path: ("PainterlyRenderInput", "dead")}))))
        cases.append(("rule 7 negative: a reference parameter is not a construction",
                      not any("no longer unreachable" in f for f in run(
                          header_ok,
                          {"a.cpp": producer_ok,
                           caller_path.as_posix(): "void g(const PainterlyRenderInput &in);\n",
                           dead_path.as_posix(): "void dead(){}\n"},
                          dead={dead_path: ("PainterlyRenderInput", "dead")}))))

    failed = [name for name, ok in cases if not ok]
    for name, ok in cases:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
    print(f"self-test: {len(cases) - len(failed)}/{len(cases)} mutations flagged")
    if failed:
        print("check_render_param_family_producers: guard failed self-test", file=sys.stderr)
        return 1
    return 0


def _missing_file_fires(fake: Path, header: str) -> bool:
    global ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS, SETTING_READERS
    (fake / TYPES_HEADER).parent.mkdir(parents=True, exist_ok=True)
    (fake / TYPES_HEADER).write_text(header, encoding="utf-8")
    SETTING_READERS = {}
    saved = (ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS)
    ROOT = fake
    LIVE_PRODUCERS = (Path("no_such_producer.cpp"),)
    FAMILIES = {"wind": ("wind_", "apply_wind_to_render_params")}
    DEAD_PRODUCERS = {}
    try:
        return any("does not exist" in f for f in check(fake)[0])
    finally:
        ROOT, LIVE_PRODUCERS, FAMILIES, DEAD_PRODUCERS = saved


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="mutation-check the guard itself and exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures, notes = check()
    print(f"check_render_param_family_producers: {len(FAMILIES)} family/families, "
          f"{len(LIVE_PRODUCERS)} live producer(s), {len(DEAD_PRODUCERS)} exempt dead producer(s)")
    for note in notes:
        print(f"  {note}")
    if failures:
        print("check_render_param_family_producers: guard failed", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("check_render_param_family_producers: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
