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
5. ``gdscript-unregistered-monitor`` -- a ``"gaussian_splatting/<name>"``
   string literal in a shipped script whose ``<name>`` no monitor definition
   in `modules/gaussian_splatting/core/performance_monitors.cpp` registers.
   **Both sides are derived**: the read set from the literals in the scripts,
   the registered set from the literals in
   ``_register_monitor_definitions``, so neither is a hand-written list that
   can drift. The starter template read 57 such ids and 18 had no producer
   anywhere; `Performance.get_custom_monitor()` errors on an unregistered id
   and returns null, and the null then aborted the whole refresh.
6. ``gdscript-monitor-id-built-at-runtime`` -- an id the script assembles
   instead of writing down, which makes every read in that file unverifiable
   by detector 5. Two *property* rules, not a list of spellings:

   * any literal starting with the prefix must be a COMPLETE id -- the bare
     ``"gaussian_splatting/"`` and a template like ``"gaussian_splatting/%s"``
     are the first half of an id being built. Exempt: the bare prefix as the
     argument of ``begins_with()`` or ``trim_prefix()``, which test or remove
     it and cannot produce one.
   * ``get_custom_monitor(`` / ``has_custom_monitor(`` take one complete string
     literal, or one bare identifier (a central helper forwarding its own
     parameter -- the correct shape, which both overlays here use). Anything
     composed in the call is flagged.

   This detector was previously written as one spelling -- a literal adjacent
   to ``+`` -- and an independent review walked a phantom id straight through
   it four ways: ``const MONITOR_PREFIX := "…"`` plus ``MONITOR_PREFIX + name``
   (the helper shape this repository itself shipped one PR earlier),
   ``"…/%s" % name``, ``"…/".path_join(name)`` and ``str("…/", name)``. All
   four are red under the rules above, and the ``%s`` form was live in three
   benchmark scripts in this tree at the time.

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

# The registry of custom monitors, and the function inside it that enumerates
# them. Both are asserted to exist; a rename must fail the guard, not empty it.
MONITOR_REGISTRY = ("modules", "gaussian_splatting", "core", "performance_monitors.cpp")
MONITOR_REGISTRY_FUNCTION = "_register_monitor_definitions"
MONITOR_PREFIX = "gaussian_splatting/"
# ANY literal that starts with the prefix, complete or not. Deliberately not
# restricted to id-shaped tails: `"gaussian_splatting/%s"` and the bare
# `"gaussian_splatting/"` are the two shapes a run-time-built id starts from,
# and a tail-restricted pattern cannot see either.
MONITOR_LITERAL_RE = re.compile(r'"(gaussian_splatting/[^"]*)"')
# What a COMPLETE id's tail may contain. Anything else -- `%s`, `{0}`, a space
# -- means the literal is a template the run time finishes.
MONITOR_ID_TAIL_RE = re.compile(r"^[A-Za-z0-9_./]+$")
# The bare prefix is legitimate only as the argument of a method that TESTS or
# REMOVES it -- neither can produce an id. `begins_with` filters an enumeration
# of the registered set; `trim_prefix` recovers the short name from a complete
# id. Anything that joins the prefix to something else is what this detector is
# for. Enumerated exemption, not a general escape hatch.
MONITOR_PREFIX_CONSUMERS = ("begins_with", "trim_prefix")
MONITOR_PREFIX_FILTER_RE = re.compile(
    r'\b(?:%s)\s*\(\s*"gaussian_splatting/"\s*\)' % "|".join(MONITOR_PREFIX_CONSUMERS))
# The two accessors an id can actually reach.
MONITOR_ACCESSOR_RE = re.compile(r"\b(get_custom_monitor|has_custom_monitor)\s*\(")
# What may be handed to them: one complete string literal, or one bare
# identifier (a central helper forwarding its own parameter, which both
# overlays in this tree use). Anything composed -- `+`, `%`, `path_join`,
# `str()` -- is a run-time-built id.
MONITOR_ARG_LITERAL_RE = re.compile(r'^\s*"[^"]*"\s*$')
MONITOR_ARG_IDENTIFIER_RE = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*\s*$")

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

# Everything that must be masked before GDScript is scanned, longest delimiter
# first so `"""` wins over `"`. A `#` inside a string is consumed by the string
# alternative, and a quote inside a comment by the comment alternative, because
# the scan is left-to-right from whichever opens first. The optional closing
# quote on the single-delimiter forms tolerates an unterminated literal without
# swallowing the rest of the file.
MASKABLE_SPAN_RE = re.compile(
    r'"""(?:[^\\]|\\.)*?"""'
    r"|'''(?:[^\\]|\\.)*?'''"
    r'|"(?:[^"\\\n]|\\.)*"?'
    r"|'(?:[^'\\\n]|\\.)*'?"
    r"|#[^\n]*",
    re.S,
)


def strip_gdscript_strings_and_comments(source: str,
                                        string_filler: str | None = STRING_FILLER
                                        ) -> list[str]:
    """Mask `#` comments, and optionally string literals, preserving structure.

    Returns one string per source line, same length and same column offsets as
    the input, so a column index in the result still maps to the original.

    Comment characters always become spaces. String characters -- delimiters
    included -- become `string_filler`, which is `_` and never whitespace: a
    call whose only argument is a string literal must still read as a call with
    an argument, since blanking `Engine.get_singleton("X")` to spaces makes it
    indistinguishable from the zero-argument form this guard flags.

    Pass ``string_filler=None`` to keep string literals verbatim while still
    removing comments. The monitor-id detector needs that: the ids it looks for
    *are* string literals, but an id quoted inside a doc comment is prose, not
    a read, and flagging it would be a false positive.

    Handles `"`, `'`, `\"\"\"`, `'''`, backslash escapes, and the `r` / `&` /
    `^` string prefixes (the prefix is an ordinary identifier character, so it
    needs no special handling -- the quote after it opens the literal as usual).
    """
    def mask(match: re.Match[str]) -> str:
        token = match.group(0)
        if token.startswith("#"):
            filler = COMMENT_FILLER
        elif string_filler is None:
            return token  # keep the literal verbatim; only comments are masked
        else:
            filler = string_filler
        # Newlines survive so the line count and every column offset are
        # unchanged; everything else in the span becomes filler.
        return "".join("\n" if ch == "\n" else filler for ch in token)

    return MASKABLE_SPAN_RE.sub(mask, source).split("\n")


def read_registered_monitor_ids(root: Path) -> tuple[set[str], str | None]:
    """Derive the registered monitor ids from the C++ registry.

    Returns (ids, error). `error` is non-None when the registry, the
    enumerating function, or a plausible number of ids could not be found --
    in which case the caller must fail, not silently accept every read.
    """
    path = root.joinpath(*MONITOR_REGISTRY)
    if not path.is_file():
        return set(), "monitor registry not found at %s" % path.relative_to(root).as_posix()
    text = path.read_text(encoding="utf-8")
    start = text.find(MONITOR_REGISTRY_FUNCTION)
    if start < 0:
        return set(), ("%s() not found in %s -- the derivation is stale"
                       % (MONITOR_REGISTRY_FUNCTION, path.name))
    # The definitions live in one table inside that function; stop at the next
    # top-level function so a later unrelated literal cannot widen the set.
    end = text.find("\nvoid GaussianSplattingPerformanceMonitors::", start + 1)
    body = text[start:end if end > 0 else len(text)]
    ids = {m for m in MONITOR_LITERAL_RE.findall(body)}
    if len(ids) < 50:
        return ids, ("only %d monitor ids parsed out of %s(); the table shape "
                     "changed and the derivation can no longer see it"
                     % (len(ids), MONITOR_REGISTRY_FUNCTION))
    return ids, None


def _accessor_argument(line: str, open_paren: int) -> str | None:
    """Text between an accessor's `(` and its matching `)`, or None if unclosed."""
    depth = 0
    for i in range(open_paren, len(line)):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
            if depth == 0:
                return line[open_paren + 1:i]
    return None


def scan_gdscript_monitor_ids(rel_path: str, source: str,
                              registered: set[str]) -> list[Finding]:
    """Two rules that together pin every monitor id to a complete literal.

    A: every string literal beginning with the prefix must be a complete,
       registered id. The bare `"gaussian_splatting/"` and a format template
       like `"gaussian_splatting/%s"` are not ids -- they are the first half of
       one being built at run time, which is what hides a read from this check.
       The single exemption is the bare prefix as the argument of
       `begins_with(`, which filters an enumeration of the registered set.

    B: `get_custom_monitor(` / `has_custom_monitor(` take either one complete
       string literal or one bare identifier. A bare identifier is the correct
       shape for a central helper forwarding its parameter (both overlays in
       this tree do that); anything *composed* -- `+`, `%`, `path_join()`,
       `str()` -- builds the id in the expression and is flagged.

    Neither rule is a spelling. The previous version matched only a literal
    adjacent to `+`, and an independent review carried a phantom id straight
    through it with `const MONITOR_PREFIX := "..."` plus `MONITOR_PREFIX + name`
    -- the exact helper shape this repository shipped one PR earlier -- as well
    as with `%`, `path_join` and `str()`. Rule A catches all four at the
    literal, rule B catches them again at the call.

    Limit, stated: an id assembled without any literal that starts with the
    prefix (`"gaussian_" + "splatting/"`) defeats rule A, and rule B still
    catches it only where it reaches an accessor directly.
    """
    findings: list[Finding] = []
    # Comments masked, string literals kept verbatim: the ids this detector
    # looks for ARE string literals, but an id quoted inside a doc comment is
    # prose describing a monitor, not a read of one.
    for lineno, line in enumerate(
            strip_gdscript_strings_and_comments(source, string_filler=None), start=1):
        # --- rule A ---------------------------------------------------------
        filtered = MONITOR_PREFIX_FILTER_RE.search(line) is not None
        for literal in MONITOR_LITERAL_RE.findall(line):
            if literal in registered:
                continue
            tail = literal[len(MONITOR_PREFIX):]
            if tail == "":
                if filtered:
                    continue
                findings.append(Finding(
                    "gdscript-monitor-id-built-at-runtime", rel_path, lineno,
                    'the bare prefix "%s" is not a monitor id; a script that '
                    "starts from it builds ids at run time, and none of its "
                    "reads can be checked against the registered set. Spell "
                    "the full id out at each call site. (Exempt: the prefix as "
                    "the argument of %s, which test or remove it rather than "
                    "building an id.)"
                    % (MONITOR_PREFIX,
                       " / ".join("`%s()`" % c for c in MONITOR_PREFIX_CONSUMERS)),
                ))
                continue
            if not MONITOR_ID_TAIL_RE.match(tail):
                findings.append(Finding(
                    "gdscript-monitor-id-built-at-runtime", rel_path, lineno,
                    '`%s` is a template, not a monitor id: the id is completed '
                    "at run time, so it cannot be checked against the "
                    "registered set." % literal,
                ))
                continue
            findings.append(Finding(
                "gdscript-unregistered-monitor", rel_path, lineno,
                "`%s` is not registered by %s(); "
                "Performance.get_custom_monitor() errors on it and returns "
                "null, and a null in a format string aborts the caller."
                % (literal, MONITOR_REGISTRY_FUNCTION),
            ))

        # --- rule B ---------------------------------------------------------
        for match in MONITOR_ACCESSOR_RE.finditer(line):
            argument = _accessor_argument(line, match.end() - 1)
            if argument is None:
                # The call spans lines; rule A still covers its literal.
                continue
            if MONITOR_ARG_LITERAL_RE.match(argument):
                continue
            if MONITOR_ARG_IDENTIFIER_RE.match(argument):
                continue
            findings.append(Finding(
                "gdscript-monitor-id-built-at-runtime", rel_path, lineno,
                "`%s(%s)` composes its monitor id in the call. Pass one "
                "complete string literal, or one variable a helper was handed, "
                "so the id can be checked against the registered set."
                % (match.group(1), argument.strip()),
            ))
    return findings


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

    registered, registry_error = read_registered_monitor_ids(root)
    if registry_error:
        messages.append("guard failed: " + registry_error)
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
            findings.extend(scan_gdscript_monitor_ids(rel, source, registered))
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
        "scanned, 6 detectors, %d registered monitor ids derived from %s()."
        % (gd_count, scene_count, len(registered), MONITOR_REGISTRY_FUNCTION))
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
