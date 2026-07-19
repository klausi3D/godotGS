#!/usr/bin/env python3
"""Guard: no raw Node pointer survives a re-entrant editor call (#698).

## The failure this guards against

`EditorFileSystem::reimport_file_with_custom_parameters()` emits
`resources_reimporting` / `resources_reimported` **synchronously**
(`editor/file_system/editor_file_system.cpp`). Editor handlers on those signals
can close a scene, which frees every node in it. Any raw `Node *` a caller
resolved *before* that call is therefore potentially dangling *after* it — and a
dangling pointer is not detectable by testing it for null:

    GaussianSplatNode3D *current_node = _get_current_node();
    ...
    fs->reimport_file_with_custom_parameters(path, importer, options);  // may free it
    ...
    if (current_node) {                 // reads freed memory; "true" proves nothing
        current_node->set_splat_asset(asset);   // use-after-free
    }

The correct pattern is to keep an `ObjectID` across the re-entrant call and
re-resolve through `ObjectDB` immediately before **each** later dereference:

    const ObjectID node_id = current_node ? current_node->get_instance_id() : ObjectID();
    fs->reimport_file_with_custom_parameters(path, importer, options);
    if (GaussianSplatNode3D *node = _resolve_node(node_id)) {
        node->set_splat_asset(asset);
    }

`ObjectDB::get_instance()` returns null for a freed instance, so the re-resolve
is the only construct that actually observes the free.

## What this guard flags (deliberately narrow)

Precision over recall, and scoped to this module's editor sources.

For each function body in `modules/gaussian_splatting/editor/*.cpp`:

* Collect locals declared as a raw pointer to a node-ish type — `T *name = ...`
  where `T` ends in `Node3D`, or is `Node`, `Control` or `Window`. `Ref<T>` and
  value locals are not tracked: they are not raw non-owning pointers.
* Find the first line containing a **re-entrant call** (`REENTRANT_CALLS`).
* Any appearance of a tracked local's identifier at or after that line is a
  violation — including a bare `if (x)` null test, which is itself a read of a
  possibly-freed pointer and, worse, reads as a safety check while providing
  none.

Function parameters are NOT tracked: the callee cannot know what the caller's
lifetime contract is, and flagging them here would be guessing.

## Fail-closed

The guard fails, rather than passing vacuously, when:

* the editor source directory is missing or contains no `.cpp`;
* no `REENTRANT_CALLS` name occurs anywhere in those sources — the anchor this
  whole rule is defined against would have been renamed away, and a guard that
  silently matches nothing is worse than no guard.

Run standalone:

    python tests/ci/check_editor_node_pointer_lifetime.py
    python tests/ci/check_editor_node_pointer_lifetime.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
EDITOR_DIR = REPO_ROOT / "modules" / "gaussian_splatting" / "editor"

# Calls that synchronously re-enter editor code that may free nodes.
REENTRANT_CALLS = (
    "reimport_file_with_custom_parameters",
    "reimport_files",
)

# `GaussianSplatNode3D *current_node = _get_current_node();`
POINTER_DECL_RE = re.compile(
    r"^\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=",
)

FUNC_START_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*\b([A-Za-z_][A-Za-z0-9_]*)::([A-Za-z_][A-Za-z0-9_]*)\s*\(",
)


def _is_node_type(type_name: str) -> bool:
    return type_name.endswith("Node3D") or type_name in {"Node", "Control", "Window"}


def _strip_comment(line: str) -> str:
    # Enough for this corpus: line comments only. A `//` inside a string literal
    # would truncate early, which can only ever LOSE a match on the trailing
    # part of that one line, never invent one.
    idx = line.find("//")
    return line if idx < 0 else line[:idx]


def _split_functions(lines: list[str]) -> list[tuple[str, int, int]]:
    """Return (name, start_index, end_index_exclusive) for each top-level body."""
    functions: list[tuple[str, int, int]] = []
    i = 0
    while i < len(lines):
        match = FUNC_START_RE.match(lines[i])
        if not match:
            i += 1
            continue
        # Walk forward to the opening brace of the body, then to its match.
        depth = 0
        started = False
        j = i
        while j < len(lines):
            stripped = _strip_comment(lines[j])
            for char in stripped:
                if char == "{":
                    depth += 1
                    started = True
                elif char == "}":
                    depth -= 1
            if started and depth <= 0:
                break
            j += 1
        if started:
            functions.append((f"{match.group(1)}::{match.group(2)}", i, min(j + 1, len(lines))))
            i = j + 1
        else:
            i += 1
    return functions


def scan_source(path_label: str, text: str) -> list[str]:
    violations: list[str] = []
    lines = text.splitlines()
    for func_name, start, end in _split_functions(lines):
        body = lines[start:end]
        reentrant_line = None
        for offset, line in enumerate(body):
            stripped = _strip_comment(line)
            if any(call in stripped for call in REENTRANT_CALLS):
                reentrant_line = offset
                break
        if reentrant_line is None:
            continue

        tracked: dict[str, int] = {}
        for offset, line in enumerate(body[:reentrant_line]):
            decl = POINTER_DECL_RE.match(_strip_comment(line))
            if decl and _is_node_type(decl.group(1)):
                tracked[decl.group(2)] = offset

        for name in tracked:
            ident_re = re.compile(rf"\b{re.escape(name)}\b")
            for offset in range(reentrant_line, len(body)):
                stripped = _strip_comment(body[offset])
                if ident_re.search(stripped):
                    violations.append(
                        f"{path_label}:{start + offset + 1}: raw node pointer "
                        f"`{name}` (declared at line {start + tracked[name] + 1}) is used after a "
                        f"re-entrant editor call in {func_name}(); re-resolve it through "
                        f"ObjectDB::get_instance(ObjectID) instead"
                    )
    return violations


def run_repo_scan() -> tuple[list[str], list[str]]:
    """Return (violations, fatal_errors)."""
    if not EDITOR_DIR.is_dir():
        return [], [f"editor source directory not found: {EDITOR_DIR}"]
    sources = sorted(EDITOR_DIR.glob("*.cpp"))
    if not sources:
        return [], [f"no .cpp sources under {EDITOR_DIR}"]

    violations: list[str] = []
    anchor_seen = False
    for source in sources:
        text = source.read_text(encoding="utf-8", errors="replace")
        if any(call in text for call in REENTRANT_CALLS):
            anchor_seen = True
        rel = source.relative_to(REPO_ROOT).as_posix()
        violations.extend(scan_source(rel, text))

    fatal: list[str] = []
    if not anchor_seen:
        fatal.append(
            "none of the re-entrant anchor calls "
            f"{REENTRANT_CALLS} occur in {EDITOR_DIR}; the guard would match nothing. "
            "If the engine API was renamed, update REENTRANT_CALLS."
        )
    return violations, fatal


# --------------------------------------------------------------------------
# Self-tests: each case pins one discrimination the rule has to make.
# --------------------------------------------------------------------------

_PRE_FIX = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path) {
    GaussianSplatNode3D *current_node = _get_current_node();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    ObjectID node_id = current_node ? current_node->get_instance_id() : ObjectID();
    if (current_node) {
        current_node->set_splat_asset(asset);
    }
    return OK;
}
"""

_POST_FIX = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path) {
    GaussianSplatNode3D *current_node = _get_current_node();
    const ObjectID node_id = current_node ? current_node->get_instance_id() : ObjectID();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    if (GaussianSplatNode3D *node = _resolve_node(node_id)) {
        node->set_splat_asset(asset);
    }
    return OK;
}
"""

_NO_REENTRANT_CALL = """
Error GaussianEditorPlugin::_other(const String &p_path) {
    GaussianSplatNode3D *current_node = _get_current_node();
    current_node->set_splat_asset(asset);
    return OK;
}
"""

_REF_LOCAL_NOT_TRACKED = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path) {
    Ref<GaussianSplatAsset> asset = _load();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    asset->set_source_path(p_path);
    return OK;
}
"""

_NULL_TEST_ONLY = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path) {
    GaussianSplatNode3D *current_node = _get_current_node();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    if (current_node) {
        return OK;
    }
    return ERR_BUG;
}
"""

_DECLARED_AFTER_CALL = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path) {
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    GaussianSplatNode3D *current_node = _get_current_node();
    current_node->set_splat_asset(asset);
    return OK;
}
"""

_SECOND_FUNCTION_UNAFFECTED = _POST_FIX + _NO_REENTRANT_CALL

SELF_TESTS = (
    ("pre-fix pattern is flagged", _PRE_FIX, lambda n: n > 0),
    ("post-fix re-resolve pattern is clean", _POST_FIX, lambda n: n == 0),
    ("function without a re-entrant call is not scanned", _NO_REENTRANT_CALL, lambda n: n == 0),
    ("Ref<T> locals are not tracked", _REF_LOCAL_NOT_TRACKED, lambda n: n == 0),
    ("a bare null test on a stale pointer is still flagged", _NULL_TEST_ONLY, lambda n: n > 0),
    ("a pointer declared after the call is not flagged", _DECLARED_AFTER_CALL, lambda n: n == 0),
    ("a clean neighbour function does not mask the rule", _SECOND_FUNCTION_UNAFFECTED, lambda n: n == 0),
)


def run_self_tests() -> int:
    failures = 0
    for label, snippet, predicate in SELF_TESTS:
        count = len(scan_source("<self-test>", snippet))
        ok = predicate(count)
        print(f"  [{'PASS' if ok else 'FAIL'}] {label} (violations={count})")
        if not ok:
            failures += 1
    print(f"self-test: {len(SELF_TESTS) - failures}/{len(SELF_TESTS)} passed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run the guard's discrimination cases")
    args = parser.parse_args()

    if args.self_test:
        return run_self_tests()

    violations, fatal = run_repo_scan()
    for message in fatal:
        print(f"FATAL: {message}")
    for message in violations:
        print(f"VIOLATION: {message}")
    if fatal or violations:
        print(
            f"check_editor_node_pointer_lifetime: FAIL "
            f"({len(violations)} violation(s), {len(fatal)} fatal)"
        )
        return 1
    print("check_editor_node_pointer_lifetime: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
