#!/usr/bin/env python3
"""Static guard over the GDScript and scene files this repository ships.

Why this exists
---------------
`templates/gaussian_splat_template/` -- the starter project a user is told to
open first -- shipped for months in a state where it could not start:

* `scripts/ui/performance_overlay.gd` used the C ternary ``cond ? a : b``,
  which GDScript 2 does not have, so the script never parsed; the `class_name`
  it declares then took `scripts/main_scene.gd` down with it (#833).
* `scripts/ab_pipeline_features.gd` had the same defect, so the A/B measurement
  harness had never run either (#1019).
* `scenes/main.tscn` wrote ``script=ExtResource("1")`` *inside* the ``[node ...]``
  header instead of as a property line. In Godot 4 an unrecognised header
  attribute is discarded **with no error at all**, so no script attached and the
  camera rig was inert -- a defect an error-free console does not reveal.
* `scenes/ui/performance_overlay.tscn` used the Godot 3 theme-override spelling
  ``custom_styles/panel``, which Godot 4 likewise discards in silence.

Nothing in `.github/`, `tests/` or `docs/reference/build-test-ci.md` loaded any
project under `templates/`, so all four shipped and stayed shipped. This guard
is the cheapest check that would have caught every one of them: it is static,
needs no engine binary and no GPU, and runs in the `--guard-only` lane.

What it checks, and what it does NOT
------------------------------------
Four detectors, each a *ratchet over one enumerated defect shape*. This is
deliberately **not** a GDScript parser or a Godot scene loader:

1. ``gdscript-c-ternary`` -- a ``?`` token in GDScript outside a string literal
   or a comment. GDScript has no ``?`` operator in any position, so any such
   token is a parse error. (Detector, not parser: it says nothing about the
   rest of the file's syntax.)
2. ``gdscript-zero-arg-get-singleton`` -- ``X.get_singleton()`` with no
   argument. Engine singletons do not bind a static ``get_singleton()`` to
   ClassDB; ``Performance.get_singleton()`` is an analyzer error, and the
   correct spellings are ``Performance.get_custom_monitor(...)`` directly or
   ``Engine.get_singleton("Performance")``. Limit: a *user-defined* class with
   its own zero-argument ``get_singleton()`` static would be flagged. None
   exists in this tree; if one is added, the guard fails loudly and the choice
   gets made on purpose rather than by accident.
3. ``scene-unknown-node-header-attribute`` -- any key inside a ``[node ...]``
   header that is not one Godot 4 recognises there. ``script=`` is the instance
   this repository shipped; the detector is written over the whole header
   grammar rather than that one key, because every unknown key fails the same
   silent way. Limit: the legal-key set is enumerated policy (below). A future
   Godot release that adds a header key makes this guard fail until the key is
   added here -- fail-closed by design.
4. ``scene-godot3-theme-override`` -- the Godot 3 ``custom_styles/`` /
   ``custom_fonts/`` / ``custom_colors/`` / ``custom_constants/`` /
   ``custom_icons/`` property prefixes, which Godot 4 renamed to
   ``theme_override_*`` and now discards silently. Enumerated policy: only
   these five prefixes.

It does not check semantics, it does not resolve `ext_resource` paths, and it
does not prove a project starts. A project that passes this guard can still
fail to run; the runtime proof is a launched scene, not this script.

Corpus derivation
-----------------
The file set is derived by walking the repository, not listed here, so a new
`.gd` or `.tscn` is covered the day it lands. Two things are subtracted, both
enumerated and both asserted to exist so a rename fails the guard instead of
silently emptying the corpus (evidence-integrity practice: a check that can
find nothing to check must not report success):

* `EXCLUDED_PREFIXES` -- `modules/gdscript/tests/`, the upstream Godot GDScript
  parser/analyzer corpus. It contains scripts that are *deliberately* invalid
  (`modules/gdscript/tests/scripts/parser/errors/invalid_ternary_operator.gd`
  is literally detector 1's fixture), and it carries its own `project.godot`,
  which is why "every file under a directory containing a project.godot" is not
  a usable derivation.
* `SKIPPED_DIR_NAMES` -- build, VCS and agent scratch directories that hold no
  shipped source.

Exit codes: 0 clean, 1 one or more findings, 2 the guard could not run (empty
corpus, missing exclusion path) -- never a silent pass.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

GDSCRIPT_SUFFIXES = (".gd",)
SCENE_SUFFIXES = (".tscn", ".escn")

# Upstream corpora that are not this repository's shipped source and that
# contain intentionally-invalid files. Each is asserted to exist.
EXCLUDED_PREFIXES: tuple[str, ...] = ("modules/gdscript/tests",)

# Directory names that never hold shipped source (build output, VCS, caches,
# per-agent scratch). Matched on the directory name at any depth.
SKIPPED_DIR_NAMES = frozenset({
    ".git", ".godot", ".claude", ".venv", "venv", "bin", "__pycache__",
    "node_modules", ".mypy_cache", ".pytest_cache",
})

# Keys Godot 4 accepts inside a `[node ...]` scene header. Everything else in a
# header is discarded without a diagnostic.
#
# Derived from the vendored engine's own text-scene writer and reader, not from
# memory: `scene/resources/resource_format_text.cpp` emits `name`, `type`,
# `parent`, `index`, `instance` / `instance_placeholder`, `owner`, `groups` and
# `node_paths` into the header, and its loader consumes the same set.
# `node_paths` in particular is written by Godot itself the moment a shipped
# script gains an exported `Node` reference wired in the editor -- flagging it
# would be a false accusation against correct engine output, so it is listed
# here even though nothing in this repository emits it today.
LEGAL_NODE_HEADER_KEYS = frozenset({
    "name", "type", "parent", "index", "groups", "instance",
    "instance_placeholder", "owner", "node_paths",
})

# Godot 3 theme-override property prefixes -> the Godot 4 spelling.
GODOT3_THEME_PREFIXES = {
    "custom_styles/": "theme_override_styles/",
    "custom_fonts/": "theme_override_fonts/",
    "custom_colors/": "theme_override_colors/",
    "custom_constants/": "theme_override_constants/",
    "custom_icons/": "theme_override_icons/",
}

NODE_HEADER_RE = re.compile(r"^\[node\s+(?P<body>.*)\]\s*$")
HEADER_KEY_RE = re.compile(r'(?P<key>[A-Za-z_][A-Za-z0-9_]*)\s*=')
# A double-quoted span inside a scene header, with `\"` escapes. Masked before
# the key scan: `=` is legal inside a node or group NAME -- the invalid set is
# only `. : @ / " %` (`core/string/ustring.cpp:5338`) -- so a header like
# `[node name="Speed=Fast"]` or `groups=["kind=enemy"]` is correct output that
# an unmasked key scan reads as an unknown `Speed=` / `kind=` attribute.
HEADER_QUOTED_SPAN_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
GET_SINGLETON_RE = re.compile(r"\bget_singleton\s*\(\s*\)")
PROPERTY_LINE_RE = re.compile(r"^(?P<key>[A-Za-z_][A-Za-z0-9_/]*)\s*=")


@dataclass(frozen=True)
class Finding:
    detector: str
    path: str
    line: int
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.detector}] {self.message}"


STRING_FILLER = "_"
COMMENT_FILLER = " "


def strip_gdscript_strings_and_comments(source: str) -> list[str]:
    """Mask string literals and `#` comments, preserving line and column structure.

    Returns one string per source line. Comment characters become spaces;
    string characters -- delimiters included -- become `_`, never whitespace,
    so a call whose only argument is a string literal still reads as a call
    with an argument (blanking `Engine.get_singleton("X")` to spaces would make
    it look like the zero-argument form this guard flags). Column indices in
    the result still map to the same column in the original. Handles `"`, `'`,
    `\"\"\"`, `'''`, backslash escapes, and the `r` / `&` / `^` string prefixes
    (the prefix is an ordinary identifier character, so it needs no special
    handling -- the quote after it opens the literal as usual).
    """
    out: list[list[str]] = [[]]
    i = 0
    n = len(source)
    in_string: str | None = None  # the closing delimiter we are looking for
    in_comment = False

    def emit(ch: str, filler: str | None) -> None:
        if ch == "\n":
            out.append([])
        else:
            out[-1].append(ch if filler is None else filler)

    while i < n:
        ch = source[i]
        if in_comment:
            if ch == "\n":
                in_comment = False
                emit(ch, None)
            else:
                emit(ch, COMMENT_FILLER)
            i += 1
            continue
        if in_string is not None:
            if ch == "\n":
                # An unterminated single-quoted literal cannot span lines; a
                # triple-quoted one can. Keep the line structure either way.
                emit(ch, None)
                if in_string in ('"', "'"):
                    in_string = None
                i += 1
                continue
            if ch == "\\" and i + 1 < n:
                emit(ch, STRING_FILLER)
                emit(source[i + 1], STRING_FILLER)
                i += 2
                continue
            if source.startswith(in_string, i):
                for c in in_string:
                    emit(c, STRING_FILLER)
                i += len(in_string)
                in_string = None
                continue
            emit(ch, STRING_FILLER)
            i += 1
            continue
        if ch == "#":
            in_comment = True
            emit(ch, COMMENT_FILLER)
            i += 1
            continue
        for delim in ('"""', "'''", '"', "'"):
            if source.startswith(delim, i):
                in_string = delim
                for c in delim:
                    emit(c, STRING_FILLER)
                i += len(delim)
                break
        else:
            emit(ch, None)
            i += 1
    return ["".join(line) for line in out]


def scan_gdscript(rel_path: str, source: str) -> list[Finding]:
    findings: list[Finding] = []
    stripped = strip_gdscript_strings_and_comments(source)
    for lineno, line in enumerate(stripped, start=1):
        column = line.find("?")
        if column >= 0:
            findings.append(Finding(
                "gdscript-c-ternary", rel_path, lineno,
                "`?` is not a GDScript token (column %d). GDScript has no C "
                "ternary; write `a if cond else b`. This file cannot parse."
                % (column + 1),
            ))
        match = GET_SINGLETON_RE.search(line)
        if match:
            findings.append(Finding(
                "gdscript-zero-arg-get-singleton", rel_path, lineno,
                "zero-argument `get_singleton()` is not bound to ClassDB for "
                "engine singletons. Use the singleton object directly (e.g. "
                '`Performance.get_custom_monitor(...)`) or '
                '`Engine.get_singleton("Name")`.',
            ))
    return findings


def mask_header_quoted_spans(body: str) -> str:
    """Blank the contents of double-quoted spans in a `[node ...]` header body.

    Length-preserving, so a column offset still maps back. Only the *contents*
    of a scan matter here, not its columns, but keeping the length makes the
    masking obviously non-destructive.

    Without this the key scan reads `=` inside a quoted value as an attribute
    boundary and rejects correct engine output: `=` is legal in a node name and
    in a group name (the invalid set is `. : @ / " %`,
    `core/string/ustring.cpp:5338`), so `[node name="Speed=Fast"]` and
    `groups=["kind=enemy"]` are both things Godot writes and reads back. That
    is the same false-accusation class as the missing `node_paths` key: the
    finding's own advice would have made the scene wrong.
    """
    return HEADER_QUOTED_SPAN_RE.sub(lambda m: '"' + "_" * (len(m.group(0)) - 2) + '"', body)


def scan_scene(rel_path: str, source: str) -> list[Finding]:
    findings: list[Finding] = []
    for lineno, raw in enumerate(source.splitlines(), start=1):
        line = raw.strip()
        header = NODE_HEADER_RE.match(line)
        if header:
            body = mask_header_quoted_spans(header.group("body"))
            for key in HEADER_KEY_RE.findall(body):
                if key in LEGAL_NODE_HEADER_KEYS:
                    continue
                extra = ""
                if key == "script":
                    extra = (" In Godot 4 the script is a property line "
                             "(`script = ExtResource(\"1\")` on its own line "
                             "below the header), not a header attribute.")
                findings.append(Finding(
                    "scene-unknown-node-header-attribute", rel_path, lineno,
                    "`%s=` is not a recognised `[node ...]` header attribute; "
                    "Godot discards unknown header attributes silently.%s"
                    % (key, extra),
                ))
            continue
        prop = PROPERTY_LINE_RE.match(line)
        if prop:
            key = prop.group("key")
            for old, new in GODOT3_THEME_PREFIXES.items():
                if key.startswith(old):
                    findings.append(Finding(
                        "scene-godot3-theme-override", rel_path, lineno,
                        "`%s` is the Godot 3 spelling; Godot 4 discards it "
                        "silently. Use `%s`." % (key, key.replace(old, new, 1)),
                    ))
    return findings


def iter_candidate_files(root: Path) -> list[Path]:
    wanted = GDSCRIPT_SUFFIXES + SCENE_SUFFIXES
    excluded = tuple((root / prefix).resolve() for prefix in EXCLUDED_PREFIXES)
    results: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIPPED_DIR_NAMES]
        here = Path(dirpath).resolve()
        if any(here == ex or ex in here.parents for ex in excluded):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if name.endswith(wanted):
                results.append(Path(dirpath) / name)
    return sorted(results)


def run(root: Path) -> tuple[int, list[str]]:
    messages: list[str] = []

    missing = [p for p in EXCLUDED_PREFIXES if not (root / p).exists()]
    if missing:
        messages.append(
            "guard failed: exclusion path(s) no longer exist: %s. The corpus "
            "derivation is stale -- update EXCLUDED_PREFIXES deliberately "
            "rather than letting the guard scan or skip the wrong tree."
            % ", ".join(sorted(missing)))
        return 2, messages

    files = iter_candidate_files(root)
    gd_count = sum(1 for f in files if f.suffix in GDSCRIPT_SUFFIXES)
    scene_count = sum(1 for f in files if f.suffix in SCENE_SUFFIXES)
    if gd_count == 0 or scene_count == 0:
        messages.append(
            "guard failed: corpus is empty (%d GDScript, %d scene files). A "
            "check that found nothing to check has not passed."
            % (gd_count, scene_count))
        return 2, messages

    findings: list[Finding] = []
    for path in files:
        rel = path.relative_to(root).as_posix()
        try:
            source = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            findings.append(Finding(
                "unreadable", rel, 0, "file is not valid UTF-8"))
            continue
        if path.suffix in GDSCRIPT_SUFFIXES:
            findings.extend(scan_gdscript(rel, source))
        else:
            findings.extend(scan_scene(rel, source))

    if findings:
        messages.append(
            "guard failed: %d finding(s) in shipped GDScript/scene files:"
            % len(findings))
        messages.extend("  " + f.render() for f in findings)
        return 1, messages

    messages.append(
        "[shipped-project-scripts] clean: %d GDScript + %d scene file(s) "
        "scanned, 4 detectors." % (gd_count, scene_count))
    return 0, messages


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=str(ROOT),
                        help="repository root to scan (default: this repo)")
    args = parser.parse_args(argv)
    code, messages = run(Path(args.root).resolve())
    for line in messages:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main())
