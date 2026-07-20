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
* Collect locals declared as a **container of** such pointers —
  `Vector<T *> name`, `LocalVector<T *> name`, and the other
  `CONTAINER_TEMPLATES`. A container holds `T *` by value, so the elements are
  raw non-owning pointers with exactly the lifetime of the scalar case; putting
  them in a `Vector` does not make them survive the free, it only hides the
  same use-after-free behind an index. `Vector<Ref<T>>` is deliberately NOT
  tracked — that owns a reference.
* Find the first line containing a **re-entrant call** (`REENTRANT_CALLS`),
  ignoring the function's own signature so that naming a barrier function does
  not blind the guard inside that function's body.
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
#
# `_import_from_path` is a MODULE-LOCAL barrier, not an engine API: it calls
# `reimport_file_with_custom_parameters()` itself
# (gaussian_editor_plugin.cpp), so every one of its callers inherits the same
# re-entrancy. It is listed because the hot-reload path reaches the reimport
# only through it, and a guard anchored solely on the engine call is blind to
# every caller one frame up the stack.
REENTRANT_CALLS = (
    "reimport_file_with_custom_parameters",
    "reimport_files",
    "_import_from_path",
)

# `GaussianSplatNode3D *current_node = _get_current_node();`
POINTER_DECL_RE = re.compile(
    r"^\s*(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*=",
)

# Containers that hold their elements by value — so a `T *` element is a raw
# non-owning pointer with exactly the lifetime of the scalar case, and putting
# it in a container does not extend it. `Ref<T>`/`Vector<Ref<T>>` are absent by
# design: those own a reference and are not the failure mode.
CONTAINER_TEMPLATES = (
    "Vector",
    "LocalVector",
    "TightLocalVector",
    "PagedArray",
    "List",
    "HashSet",
    "RBSet",
)

# `Vector<GaussianSplatNode3D *> watched_nodes = _collect_live_hot_reload_nodes(...);`
# Also matches a bare `Vector<GaussianSplatNode3D *> live_nodes;` declaration,
# which can be populated later and is the same hazard.
CONTAINER_DECL_RE = re.compile(
    r"^\s*(?:const\s+)?(?:" + "|".join(CONTAINER_TEMPLATES) + r")\s*<\s*"
    r"(?:const\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*\*\s*>\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*[=;]",
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


def _signature_offset(body: list[str]) -> int:
    """Offset of the line holding the body's opening brace."""
    for offset, line in enumerate(body):
        if "{" in _strip_comment(line):
            return offset
    return 0


def _body_code(body: list[str], offset: int, sig_offset: int) -> str:
    """Code on `offset`, excluding the function's own signature text.

    A barrier name occurring in the signature is the function's own NAME, not a
    call it makes. Counting it would set the barrier at offset 0, leaving no
    lines before it to declare anything — so the function would silently scan
    clean. That is how `_import_from_path` would blind the guard to the very
    bug it was written for; `_barrier_does_not_shadow_own_body` pins it.
    """
    stripped = _strip_comment(body[offset])
    if offset > sig_offset:
        return stripped
    if offset < sig_offset:
        return ""
    brace = stripped.find("{")
    return stripped[brace + 1:] if brace >= 0 else ""


def scan_source(path_label: str, text: str) -> list[str]:
    violations: list[str] = []
    lines = text.splitlines()
    for func_name, start, end in _split_functions(lines):
        body = lines[start:end]
        sig_offset = _signature_offset(body)

        reentrant_line = None
        for offset in range(len(body)):
            code = _body_code(body, offset, sig_offset)
            if any(call in code for call in REENTRANT_CALLS):
                reentrant_line = offset
                break
        if reentrant_line is None:
            continue

        # name -> (declaration offset, human-readable kind)
        tracked: dict[str, tuple[int, str]] = {}
        for offset in range(sig_offset + 1, reentrant_line):
            stripped = _strip_comment(body[offset])
            decl = POINTER_DECL_RE.match(stripped)
            if decl and _is_node_type(decl.group(1)):
                tracked[decl.group(2)] = (offset, "raw node pointer")
                continue
            # A container of raw node pointers is the same hazard: the elements
            # are non-owning `T *` and the container does not keep them alive.
            container = CONTAINER_DECL_RE.match(stripped)
            if container and _is_node_type(container.group(1)):
                tracked[container.group(2)] = (offset, "container of raw node pointers")

        for name, (decl_offset, kind) in tracked.items():
            ident_re = re.compile(rf"\b{re.escape(name)}\b")
            for offset in range(reentrant_line, len(body)):
                stripped = _strip_comment(body[offset])
                if ident_re.search(stripped):
                    violations.append(
                        f"{path_label}:{start + offset + 1}: {kind} "
                        f"`{name}` (declared at line {start + decl_offset + 1}) is used after a "
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

# --- container-shaped captures (the half the scalar-only matcher missed) ---

_CONTAINER_PRE_FIX = """
void GaussianEditorPlugin::_process_hot_reload_for_watch(const String &p_path, HotReloadWatch &p_watch) {
    Vector<GaussianSplatNode3D *> watched_nodes = _collect_live_hot_reload_nodes(p_watch.node_ids);
    const Error import_err = _import_from_path(p_path, options);
    _apply_hot_reload_asset_to_nodes(p_path, watched_nodes, refreshed_asset);
    if (watched_nodes.is_empty()) {
        return;
    }
}
"""

_CONTAINER_POST_FIX = """
void GaussianEditorPlugin::_process_hot_reload_for_watch(const String &p_path, HotReloadWatch &p_watch) {
    Vector<GaussianSplatNode3D *> watched_nodes = _collect_live_hot_reload_nodes(p_watch.node_ids);
    const Error import_err = _import_from_path(p_path, options);
    Vector<GaussianSplatNode3D *> live_nodes = _collect_live_hot_reload_nodes(p_watch.node_ids);
    _apply_hot_reload_asset_to_nodes(p_path, live_nodes, refreshed_asset);
    if (live_nodes.is_empty()) {
        return;
    }
}
"""

_CONTAINER_LOCALVECTOR = """
void GaussianEditorPlugin::_other(const String &p_path) {
    LocalVector<Node3D *> nodes;
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    nodes[0]->force_update();
}
"""

# A container whose elements are NOT node pointers must not be tracked, or the
# rule degenerates into "no local may be named after a reimport".
_CONTAINER_NON_NODE = """
void GaussianEditorPlugin::_other(const String &p_path) {
    Vector<GaussianSplatAsset *> assets = _collect();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    assets.clear();
}
"""

# `Vector<Ref<T>>` owns its elements; it is not the failure mode.
_CONTAINER_OF_REF_NOT_TRACKED = """
void GaussianEditorPlugin::_other(const String &p_path) {
    Vector<Ref<GaussianSplatNode3D>> nodes = _collect();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    nodes.clear();
}
"""

# `_import_from_path` is itself a barrier, so its CALLERS are scanned...
_IMPORT_FROM_PATH_IS_A_BARRIER = """
void GaussianEditorPlugin::_caller(const String &p_path) {
    GaussianSplatNode3D *node = _get_current_node();
    const Error err = _import_from_path(p_path, options);
    node->force_update();
}
"""

# ...but naming it must NOT blind the guard inside its own definition, where the
# barrier is the engine call on a later line. Without the signature exclusion
# this returns 0 and the original #698 bug stops being caught.
_BARRIER_DOES_NOT_SHADOW_OWN_BODY = """
Error GaussianEditorPlugin::_import_from_path(const String &p_path, const Dictionary &p_options) {
    GaussianSplatNode3D *current_node = _get_current_node();
    fs->reimport_file_with_custom_parameters(p_path, importer_name, options);
    current_node->set_splat_asset(asset);
    return OK;
}
"""

SELF_TESTS = (
    ("pre-fix pattern is flagged", _PRE_FIX, lambda n: n > 0),
    ("post-fix re-resolve pattern is clean", _POST_FIX, lambda n: n == 0),
    ("function without a re-entrant call is not scanned", _NO_REENTRANT_CALL, lambda n: n == 0),
    ("Ref<T> locals are not tracked", _REF_LOCAL_NOT_TRACKED, lambda n: n == 0),
    ("a bare null test on a stale pointer is still flagged", _NULL_TEST_ONLY, lambda n: n > 0),
    ("a pointer declared after the call is not flagged", _DECLARED_AFTER_CALL, lambda n: n == 0),
    ("a clean neighbour function does not mask the rule", _SECOND_FUNCTION_UNAFFECTED, lambda n: n == 0),
    ("a Vector<T *> captured across the call is flagged", _CONTAINER_PRE_FIX, lambda n: n > 0),
    ("re-collecting the container after the call is clean", _CONTAINER_POST_FIX, lambda n: n == 0),
    ("LocalVector<T *> is tracked too", _CONTAINER_LOCALVECTOR, lambda n: n > 0),
    ("a container of non-node pointers is not tracked", _CONTAINER_NON_NODE, lambda n: n == 0),
    ("Vector<Ref<T>> is not tracked", _CONTAINER_OF_REF_NOT_TRACKED, lambda n: n == 0),
    ("_import_from_path is a barrier for its callers", _IMPORT_FROM_PATH_IS_A_BARRIER, lambda n: n > 0),
    ("naming a barrier does not blind its own body", _BARRIER_DOES_NOT_SHADOW_OWN_BODY, lambda n: n > 0),
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
