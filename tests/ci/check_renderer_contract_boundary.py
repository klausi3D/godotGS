#!/usr/bin/env python3
"""#611: keep every blocking render-thread dispatch behind the instrumented boundary.

`GaussianSplatSceneDirector::world_mutex` sits on both sides of a lock-order
inversion: the main thread holds it and issues a *blocking* render-thread
dispatch, while the render thread needs the same mutex inside the director's
`*_for_renderer` builders.

PR A (#665) instrumented two functions -- `_apply_world_submission_to_renderer`
and `_restore_world_submission_renderer` -- so a live run counts the violation
itself. That made the counter a LOWER BOUND rather than a total, because
`register_instance` called `GaussianSplatRenderer::initialize()` directly and
bypassed both. PR B1 routes that call through `_initialize_world_renderer`.

The counter is only complete for as long as no new direct call appears. No lane
can reproduce the stall behaviourally (every doctest process runs
`--headless --test`, tests/ci/run_module_tests.py:350, and the dispatcher
short-circuits under headless), so this static guard is what keeps the claim
true: every call in the scene director to a renderer method that can reach
`_dispatch_call_on_render_thread_blocking` must sit in one of the allowlisted
functions.

PR B2 closes the last site, `submit_world_submission`, and adds the CALLER-SIDE
rule this file was missing. That gap mattered: the function-level check above
passes on the pre-fix tree, because the two functions `submit_world_submission`
called through the lock are themselves legitimately allowlisted. Proving the
callee is instrumented says nothing about whether the caller held the mutex. So
`check_no_boundary_call_under_lock` now asserts the property that actually
matters -- it reports 3 violations on the pre-fix tree (director.cpp:2318, :2319,
:2324) and 0 after.

Fail-closed: a dispatching call whose enclosing function cannot be determined is
an ERROR, not a skip.

## #723: two holes this file transcribed by hand

Two independent audits found the `DISPATCHING_METHODS` list stale. It was
maintained by hand and had drifted from the renderer, missing two real
dispatchers:

* `GaussianSplatRenderer::force_sort_for_view`
  (renderer/render_sorting_orchestrator.cpp) -- public and GDScript-exposed, so
  the director could grow a call to it and this guard would not notice.
* `GaussianSplatRenderer::~GaussianSplatRenderer`
  (renderer/gaussian_splat_renderer.cpp) -- its blocking teardown dispatches with
  `p_allow_timeout = false`, the indefinite-hang variant (#628). A renderer `Ref`
  dropped while `world_mutex` is held is exactly that hang, and a `->method(`
  matcher structurally cannot see a destructor running on scope exit.

A hand-maintained list is the same class of artifact as the bug it guards
against: only as complete as its last edit. So this file no longer transcribes
the list. Following the precedent set by `check_test_lane_coverage.py` ("Issue
#520 catalogued three stranded families by hand and missed a fourth"), it now:

1. **Derives** `DISPATCHING_METHODS` from the renderer sources -- the set of
   `GaussianSplatRenderer` methods that transitively call
   `_dispatch_call_on_render_thread_blocking`. Adding, removing or renaming a
   dispatcher re-derives automatically; nothing is kept in sync by hand.
2. **Adds a declaration-order rule** (`check_renderer_ref_released_under_lock`)
   for the destructor hazard the method-call matcher cannot see: a
   `Ref<GaussianSplatRenderer>` (or its release vector) whose scope-exit
   destruction happens while `world_mutex` is held. The invariant is documented
   at `core/gaussian_splat_scene_director.h` -- the release vector MUST be
   declared BEFORE the `ThreadOwnedMutexLock lock(world_mutex)`, so by reverse
   destruction order the renderer `Ref` drops only after the lock is released.
   Declaring it after the lock is the #628 indefinite hang; this rule fails on
   it.

Usage:
    python tests/ci/check_renderer_contract_boundary.py
    python tests/ci/check_renderer_contract_boundary.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIRECTOR_SOURCE = ROOT / "modules" / "gaussian_splatting" / "core" / "gaussian_splat_scene_director.cpp"

# The renderer sources GaussianSplatRenderer's methods are split across; the
# class is defined across every orchestrator translation unit, so all of them
# must be parsed to build the call graph.
RENDERER_DIR = ROOT / "modules" / "gaussian_splatting" / "renderer"
RENDERER_CLASS = "GaussianSplatRenderer"
# The blocking render-thread dispatch primitive. Every method that reaches this,
# directly or transitively, is a dispatcher the director must not call under
# world_mutex. If this is ever renamed, the derivation below finds zero
# dispatchers and `check_derivation_integrity` fails closed rather than passing a
# guard that now matches nothing (the R11-shader-guard vacuity failure mode).
DISPATCH_SINK = "_dispatch_call_on_render_thread_blocking"


def strip_comments(text: str, strip_chars: bool = False) -> str:
    """Blank out // and /* */ comments and string literals, preserving lines.

    Comments and strings in these sources quote the very tokens the guard looks
    for, so scanning raw text would produce false positives. Line and column
    structure is preserved so reported line numbers stay accurate. `strip_chars`
    additionally blanks `'x'` char literals -- needed by the renderer parser,
    whose brace matching would be thrown off by a `'{'` or `'}'` literal.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end == -1:
                break
            out.append(" " * (end - i))
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:end]))
            i = end
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(" " * (min(j + 1, n) - i))
            i = min(j + 1, n)
        elif strip_chars and text[i] == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(" " * (min(j + 1, n) - i))
            i = min(j + 1, n)
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


# --- #723: derive DISPATCHING_METHODS from the renderer sources ---------------

_RENDERER_DEF_RE = re.compile(r"\b" + RENDERER_CLASS + r"::(~?\w+)\s*\(")


def parse_class_methods(source_text: str, class_name: str) -> dict[str, str]:
    """Return {method_name: body_text} for every `class_name::method` definition.

    A definition starts at column 0 (Godot never indents a function definition),
    names the class, and is not a forward declaration (`;`-terminated). The body
    is taken by brace-matching from the first `{`, so multi-line signatures and
    constructor initializer lists are handled. Overloads merge under one name --
    reachability only needs the union of their bodies.
    """
    def_re = re.compile(r"\b" + re.escape(class_name) + r"::(~?\w+)\s*\(")
    text = strip_comments(source_text, strip_chars=True)
    lines = text.split("\n")
    methods: dict[str, str] = {}
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        first = line[:1]
        if first not in (" ", "\t", "", "#") and (class_name + "::") in line and not line.rstrip().endswith(";"):
            match = def_re.search(line)
            if match:
                name = match.group(1)
                depth = 0
                started = False
                body_lines: list[str] = []
                j = i
                while j < n:
                    current = lines[j]
                    for ch in current:
                        if ch == "{":
                            depth += 1
                            started = True
                        elif ch == "}":
                            depth -= 1
                    if started and j > i:
                        body_lines.append(current)
                    if started and depth == 0:
                        break
                    j += 1
                if started:
                    methods[name] = methods.get(name, "") + "\n".join(body_lines) + "\n"
                    i = j + 1
                    continue
        i += 1
    return methods


def collect_renderer_methods() -> dict[str, str]:
    """Parse every `GaussianSplatRenderer::method` across the renderer sources."""
    methods: dict[str, str] = {}
    for cpp in sorted(RENDERER_DIR.glob("*.cpp")):
        for name, body in parse_class_methods(cpp.read_text(encoding="utf-8"), RENDERER_CLASS).items():
            methods[name] = methods.get(name, "") + body
    return methods


def derive_dispatching_methods(methods: dict[str, str]) -> tuple[str, ...]:
    """Transitive closure of methods that reach `DISPATCH_SINK`.

    Depth is bounded only to terminate (fixpoint over a finite method set); the
    real closure here is shallow. A "call" is a self-call: a bare `name(` or an
    explicit `this->name(`. Calls through another object (`obj->name(` /
    `obj.name(`) are deliberately excluded, so `sorting_orchestrator->
    force_sort_for_view(` is not mistaken for `GaussianSplatRenderer::
    force_sort_for_view` -- the two share a name.
    """
    sink_call = re.compile(r"(?<![\w.])" + re.escape(DISPATCH_SINK) + r"\s*\(")
    reachable: set[str] = {
        name for name, body in methods.items() if name != DISPATCH_SINK and sink_call.search(body)
    }
    # Fixpoint: a method that self-calls anything already reachable is reachable.
    for _ in range(len(methods) + 1):
        newly: set[str] = set()
        for name, body in methods.items():
            if name in reachable or name == DISPATCH_SINK:
                continue
            for target in reachable:
                escaped = re.escape(target)
                edge = re.compile(r"(?<![\w>.])" + escaped + r"\s*\(|this->" + escaped + r"\s*\(")
                if edge.search(body):
                    newly.add(name)
                    break
        if not newly:
            break
        reachable |= newly
    return tuple(sorted(reachable))


# Derived at import so `_CALL_RE` below can be built from it, exactly as the
# hand-maintained tuple used to be. `_RENDERER_METHODS` is kept for the
# integrity check, which fails closed if the derivation collapsed.
_RENDERER_METHODS = collect_renderer_methods()
DISPATCHING_METHODS = derive_dispatching_methods(_RENDERER_METHODS)


def check_derivation_integrity() -> list[str]:
    """Fail closed if the derivation stopped seeing the renderer.

    A guard that derives its inputs is worthless if the derivation silently
    yields nothing -- it then matches nothing and passes vacuously. So: the
    renderer sources must exist, the sink method must still be defined there, and
    the derived set must be non-empty.
    """
    errors: list[str] = []
    if not RENDERER_DIR.is_dir():
        errors.append(f"renderer source dir not found: {RENDERER_DIR}; cannot derive dispatchers.")
        return errors
    if not _RENDERER_METHODS:
        errors.append(
            f"parsed zero {RENDERER_CLASS} methods from {RENDERER_DIR}; the parser or the "
            f"source layout changed. Fail-closed rather than derive an empty guard."
        )
    if DISPATCH_SINK not in _RENDERER_METHODS:
        errors.append(
            f"the blocking-dispatch sink `{RENDERER_CLASS}::{DISPATCH_SINK}` is no longer "
            f"defined in {RENDERER_DIR}. If it was renamed, update DISPATCH_SINK in this "
            f"guard -- do not leave it pointing at a name that matches nothing."
        )
    if not DISPATCHING_METHODS:
        errors.append(
            f"derived zero dispatching methods. Every method that reaches `{DISPATCH_SINK}` "
            f"went missing at once, which means the derivation broke -- fail-closed."
        )
    return errors


# Functions permitted to make such a call, each with the reason it is safe.
ALLOWED_FUNCTIONS = {
    "GaussianSplatSceneDirector::DeferredRendererWork::flush": (
        "runs with world_mutex released by construction (callers declare the queue "
        "before the lock, so it is destroyed after it)"
    ),
    "GaussianSplatSceneDirector::_apply_world_submission_to_renderer": (
        "instrumented boundary: reports via _report_renderer_contract_lock_violation"
    ),
    "GaussianSplatSceneDirector::_restore_world_submission_renderer": (
        "instrumented boundary: reports via _report_renderer_contract_lock_violation"
    ),
    "GaussianSplatSceneDirector::_initialize_world_renderer": (
        "instrumented boundary: reports via _report_renderer_contract_lock_violation"
    ),
    "GaussianSplatSceneDirector::_apply_world_submission_contract_unlocked": (
        "#611 PR B2: submit_world_submission's apply, called between two world_mutex "
        "scopes rather than inside one; instrumented, so calling it under the lock "
        "still reports"
    ),
    "GaussianSplatSceneDirector::teardown_world_for_scenario": (
        "#628: explicitly moves the renderer Ref out under the lock and calls this "
        "after the MutexLock scope has ended"
    ),
}

# The boundary functions must keep their instrumentation. Without this the
# guard could pass on a tree where the counter had been quietly gutted.
INSTRUMENTED_FUNCTIONS = (
    "GaussianSplatSceneDirector::_apply_world_submission_to_renderer",
    "GaussianSplatSceneDirector::_restore_world_submission_renderer",
    "GaussianSplatSceneDirector::_initialize_world_renderer",
    "GaussianSplatSceneDirector::_apply_world_submission_contract_unlocked",
)

# #611 PR B2 -- CALLER-SIDE RULE.
#
# The checks above prove that every dispatching call sits in a *recognised*
# function. They cannot prove the caller was unlocked, because that is a property
# of the call site, not of the callee. Until PR B2 the remaining inversion lived
# exactly there: `submit_world_submission` held world_mutex and called
# `_apply_world_submission_to_renderer` / `_restore_world_submission_renderer`
# straight through it. Both are allowlisted callees, so the function-level check
# passed while the defect was fully live.
#
# So: a function that acquires world_mutex must not call these two at all. It has
# two correct alternatives -- queue the work on DeferredRendererWork (when the
# result is discarded), or restructure so the dispatch happens between lock
# scopes (when the result gates a decision, as in submit_world_submission).
LOCK_ACQUISITION_RE = re.compile(r"\bThreadOwnedMutexLock\s+\w+\s*\(\s*world_mutex\s*\)")
FORBIDDEN_UNDER_LOCK = (
    "_apply_world_submission_to_renderer",
    "_restore_world_submission_renderer",
)
_FORBIDDEN_CALL_RE = re.compile(r"\b(" + "|".join(FORBIDDEN_UNDER_LOCK) + r")\s*\(")

# #723 -- a renderer Ref (or its release vector) declared as a local. Matches
# `Ref<GaussianSplatRenderer> name` and `LocalVector<Ref<GaussianSplatRenderer>>
# name`, but NOT a reference parameter (`> &name`) or a constructor temporary
# (`Ref<GaussianSplatRenderer>(...)`), whose scope-exit destruction is not this
# function's concern.
#
# The optional cv-qualifier groups matter: `const Ref<GaussianSplatRenderer>
# renderer = world->renderer;` (or `const LocalVector<Ref<...>>`) owns the last
# reference exactly like the non-const form and runs `~GaussianSplatRenderer` at
# the same scope exit -- a `const`-blind pattern would let it drop the renderer
# under world_mutex and pass the guard anyway (Codex #726 review).
_RENDERER_REF_DECL_RE = re.compile(
    r"(?:(?:const|volatile)\s+)*"
    r"(?:LocalVector\s*<\s*(?:(?:const|volatile)\s+)*)?"
    r"Ref\s*<\s*" + RENDERER_CLASS + r"\s*>\s*(?:>\s*)?([A-Za-z_]\w*)\s*[;={(]"
)

# #730 -- an `auto`-deduced OWNING local that resolves to Ref<GaussianSplatRenderer>
# hides the type from _RENDERER_REF_DECL_RE but has the identical scope-exit
# destructor hazard. The right-hand sources that yield a renderer Ref are DERIVED,
# not guessed: the only Ref<GaussianSplatRenderer> value sources reachable in the
# director are the `renderer` member of the SharedWorld-family structs
# (`Ref<GaussianSplatRenderer> renderer;`, 3 sites) accessed via `.`/`->`, and
# `get_shared_renderer(...)`, which returns one. `auto&` / `const auto&` are
# NON-owning references (their destruction drops no reference) and are excluded by
# requiring a name -- not `&` -- immediately after `auto`. Conservative by design
# (#730): a dismissible false positive on this guard is far cheaper than a missed
# indefinite-hang (#628); a bare `renderer\b` word boundary still excludes
# `renderer_config` / `renderer_count` etc.
_RENDERER_AUTO_DECL_RE = re.compile(
    r"(?:const\s+)?auto\s+([A-Za-z_]\w*)\s*=\s*"
    r"(?:"
    # Terminal `.renderer` / `->renderer` -- the Ref member ITSELF. The negative
    # lookahead rejects any CHAINED use (#733 review): `world->renderer->foo()`
    # yields foo()'s result and `world->renderer.ptr()` yields a raw pointer --
    # neither local owns a Ref, so neither is a scope-exit hazard.
    r"[^;]*(?:->|\.)\s*renderer\b(?!\s*(?:->|\.|\(|\[|<))"
    r"|"
    # `get_shared_renderer(...)` as the terminal value (ends the statement). A
    # chained `get_shared_renderer(w)->initialize()` is excluded by requiring the
    # close paren be followed by `;`/end. Args are matched without nested parens
    # (get_shared_renderer takes a single World3D*); a nested-paren arg would miss
    # conservatively (no false positive), not over-match.
    r"[^;]*\bget_shared_renderer\s*\([^;)]*\)\s*(?:;|$)"
    r")"
)

_DEFINITION_RE = re.compile(
    r"^[A-Za-z_][\w:<>*&,\s]*?\b(GaussianSplatSceneDirector(?:::\w+)+)\s*\("
)
_CALL_RE = re.compile(r"(?:->|\.)\s*(" + "|".join(re.escape(m) for m in DISPATCHING_METHODS) + r")\s*\(")


def find_violations(source_text: str) -> tuple[list[str], dict[str, list[int]]]:
    """Return (errors, {function: [line numbers of dispatching calls]})."""
    errors: list[str] = []
    found: dict[str, list[int]] = {}
    current: str | None = None

    for lineno, line in enumerate(strip_comments(source_text).splitlines(), start=1):
        definition = _DEFINITION_RE.match(line)
        if definition:
            current = definition.group(1)

        for call in _CALL_RE.finditer(line):
            method = call.group(1)
            if current is None:
                # Fail closed rather than assume it is harmless.
                errors.append(
                    f"{DIRECTOR_SOURCE.name}:{lineno}: call to `{method}` outside any "
                    f"recognised GaussianSplatSceneDirector function definition. The "
                    f"guard cannot prove it is off the locked path; fail-closed."
                )
                continue
            found.setdefault(current, []).append(lineno)
            if current not in ALLOWED_FUNCTIONS:
                errors.append(
                    f"{DIRECTOR_SOURCE.name}:{lineno}: `{current}` calls `{method}`, "
                    f"which reaches a blocking render-thread dispatch, but is not an "
                    f"instrumented boundary.\n"
                    f"    #611: doing this while world_mutex is held is a lock-order "
                    f"inversion -- the render thread needs that mutex inside the "
                    f"*_for_renderer builders, so the dispatch stalls for its full "
                    f"timeout and the operation is then dropped or rejected.\n"
                    f"    Fix: queue the work on DeferredRendererWork (declared before "
                    f"the lock) so it dispatches after world_mutex is released, or route "
                    f"it through one of: {', '.join(sorted(ALLOWED_FUNCTIONS))}."
                )

    return errors, found


def function_bounds(lines: list[str]) -> dict[str, tuple[int, int]]:
    """Map each recognised function to its [start, end) line span (0-based)."""
    bounds: dict[str, tuple[int, int]] = {}
    current: str | None = None
    start = 0
    for index, line in enumerate(lines):
        definition = _DEFINITION_RE.match(line)
        if definition:
            if current is not None:
                bounds[current] = (start, index)
            current = definition.group(1)
            start = index
    if current is not None:
        bounds[current] = (start, len(lines))
    return bounds


def check_no_boundary_call_under_lock(source_text: str) -> list[str]:
    """A function that takes world_mutex must not call the locked-path boundaries.

    This is the caller-side half of the guard. The function-level allowlist above
    cannot see it: both callees are legitimately allowlisted, so a caller holding
    world_mutex across them passes that check while being the exact defect #611
    is about.
    """
    errors: list[str] = []
    lines = strip_comments(source_text).splitlines()

    for name, (begin, end) in function_bounds(lines).items():
        lock_line: int | None = None
        for offset in range(begin, end):
            if lock_line is None and LOCK_ACQUISITION_RE.search(lines[offset]):
                lock_line = offset
                continue
            if lock_line is None or offset == begin:
                # Nothing acquired yet, or this is the definition line itself
                # (whose own name would otherwise read as a call).
                continue
            match = _FORBIDDEN_CALL_RE.search(lines[offset])
            if not match:
                continue
            errors.append(
                f"{DIRECTOR_SOURCE.name}:{offset + 1}: `{name}` acquires world_mutex "
                f"(line {lock_line + 1}) and then calls `{match.group(1)}`, which "
                f"reaches a blocking render-thread dispatch.\n"
                f"    #611: this is the lock-order inversion itself -- the render "
                f"thread needs world_mutex inside the *_for_renderer builders, so the "
                f"dispatch stalls for its full timeout and the operation is then "
                f"dropped (set_max_splats) or rejected (set_gaussian_data).\n"
                f"    Fix: if the result is discarded, queue it on "
                f"DeferredRendererWork (declared before the lock). If the result "
                f"gates a decision, restructure into arbitrate-under-lock -> "
                f"apply-unlocked -> commit-under-lock and dispatch via "
                f"_apply_world_submission_contract_unlocked, as "
                f"submit_world_submission does."
            )
    return errors


def check_renderer_ref_released_under_lock(source_text: str) -> list[str]:
    """#723: a renderer Ref must not be destroyed while world_mutex is held.

    The method-call matcher in `find_violations` structurally cannot see a
    `~GaussianSplatRenderer` running on scope exit -- there is no `->method(` to
    match. Yet that destructor issues the indefinite-hang blocking dispatch
    (`p_allow_timeout = false`, #628). The invariant that keeps it safe is
    declaration order: a `Ref<GaussianSplatRenderer>` (or the
    `LocalVector<Ref<GaussianSplatRenderer>>` release vector) is declared BEFORE
    `ThreadOwnedMutexLock lock(world_mutex)`, so by reverse destruction order the
    Ref drops only after the lock scope ends.

    Declaring it AFTER the lock inverts that: the Ref (or vector) is destroyed
    first, dropping a possibly-last renderer reference while world_mutex is still
    held -- the #628 hang. So: within a function that acquires world_mutex, a
    renderer-Ref local declared after the acquisition is a violation.
    """
    errors: list[str] = []
    lines = strip_comments(source_text).splitlines()

    for name, (begin, end) in function_bounds(lines).items():
        lock_line: int | None = None
        for offset in range(begin, end):
            if lock_line is None:
                if LOCK_ACQUISITION_RE.search(lines[offset]):
                    lock_line = offset
                continue
            if offset == begin:
                continue
            decl = _RENDERER_REF_DECL_RE.search(lines[offset]) or _RENDERER_AUTO_DECL_RE.search(lines[offset])
            if not decl:
                continue
            errors.append(
                f"{DIRECTOR_SOURCE.name}:{offset + 1}: `{name}` declares renderer Ref "
                f"`{decl.group(1)}` AFTER acquiring world_mutex (line {lock_line + 1}). "
                f"By reverse destruction order it drops while the lock is still held.\n"
                f"    #628: dropping a possibly-last Ref<GaussianSplatRenderer> under "
                f"world_mutex runs ~GaussianSplatRenderer, whose teardown blocks on a "
                f"render-thread dispatch with p_allow_timeout=false -- the render thread "
                f"can itself be blocked acquiring world_mutex inside a *_for_renderer "
                f"builder, so this is an INDEFINITE hang, not a bounded stall.\n"
                f"    Fix: declare the release vector (or the Ref) BEFORE the "
                f"`ThreadOwnedMutexLock lock(world_mutex)`, as the header requires and "
                f"as try_prune_world_if_unused / teardown_world_for_scenario do, so the "
                f"Ref is destroyed only after the lock is released."
            )
    return errors


def check_instrumentation(source_text: str) -> list[str]:
    """Each boundary function must still report violations."""
    errors: list[str] = []
    text = strip_comments(source_text)
    lines = text.splitlines()
    bounds = function_bounds(lines)

    for name in INSTRUMENTED_FUNCTIONS:
        if name not in bounds:
            errors.append(
                f"{DIRECTOR_SOURCE.name}: expected boundary function `{name}` not found. "
                f"If it was renamed, update INSTRUMENTED_FUNCTIONS and ALLOWED_FUNCTIONS "
                f"in this guard -- do not delete the check."
            )
            continue
        begin, end = bounds[name]
        body = "\n".join(lines[begin:end])
        if "_report_renderer_contract_lock_violation" not in body:
            errors.append(
                f"{DIRECTOR_SOURCE.name}:{begin + 1}: `{name}` no longer calls "
                f"_report_renderer_contract_lock_violation. #611's counter would "
                f"silently stop seeing this route."
            )
    return errors


def run_self_test() -> int:
    """Prove the detector discriminates, rather than passing vacuously."""
    failures: list[str] = []

    def expect(label: str, condition: bool) -> None:
        if not condition:
            failures.append(label)

    # --- #723: the derivation must find both direct and transitive dispatchers,
    # and must NOT sweep in unrelated methods. Run it on synthetic sources so the
    # discrimination is proven independently of the real tree. ---
    synthetic = (
        "void GaussianSplatRenderer::_dispatch_call_on_render_thread_blocking(const Callable &c) {\n"
        "\treturn;\n"
        "}\n"
        "void GaussianSplatRenderer::initialize() {\n"
        "\t_dispatch_call_on_render_thread_blocking(cb);\n"
        "}\n"
        "GaussianSplatRenderer::~GaussianSplatRenderer() {\n"
        "\t_dispatch_call_on_render_thread_blocking(cb, nullptr, false);\n"
        "}\n"
        "Error GaussianSplatRenderer::apply_world_submission_contract(const Contract &p) {\n"
        "\tinitialize();\n"
        "}\n"
        "void GaussianSplatRenderer::unrelated_helper() {\n"
        "\tother_orchestrator->initialize();\n"  # call through another object: not a self-call
        "}\n"
        "void GaussianSplatRenderer::render_frame() {\n"
        "\tget_frame_state();\n"
        "}\n"
    )
    methods = parse_class_methods(synthetic, RENDERER_CLASS)
    derived = set(derive_dispatching_methods(methods))
    expect("derivation finds a direct dispatcher (initialize)", "initialize" in derived)
    expect("derivation finds the destructor dispatcher", "~GaussianSplatRenderer" in derived)
    expect(
        "derivation finds a transitive dispatcher (apply_world_submission_contract)",
        "apply_world_submission_contract" in derived,
    )
    expect(
        "derivation does NOT sweep in a method that only calls through another object",
        "unrelated_helper" not in derived,
    )
    expect("derivation does NOT sweep in an unrelated method", "render_frame" not in derived)
    expect("the sink itself is not reported as a dispatcher", DISPATCH_SINK not in derived)

    # The real derivation must reproduce the historical hand-list AND add the two
    # dispatchers #723 is about. This is the non-vacuity proof for change 1: the
    # derived set is a strict superset of what the hand list transcribed.
    historical_hand_list = {
        "initialize", "set_max_splats", "set_gaussian_data", "set_file_backed_payload_source",
        "apply_world_submission_contract", "restore_world_submission_runtime_state",
        "clear_world_submission_contract",
    }
    expect(
        "the derived set reproduces every method the hand list transcribed",
        historical_hand_list.issubset(set(DISPATCHING_METHODS)),
    )
    expect(
        "the derived set adds force_sort_for_view, which the hand list missed",
        "force_sort_for_view" in DISPATCHING_METHODS,
    )
    expect(
        "the derived set adds ~GaussianSplatRenderer, which the hand list missed",
        "~GaussianSplatRenderer" in DISPATCHING_METHODS,
    )
    expect("the real derivation is non-empty and the integrity check passes", not check_derivation_integrity())

    # A call in a non-allowlisted function must be caught.
    bad = (
        "void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id) {\n"
        "\tworld->renderer->initialize();\n"
        "}\n"
    )
    errors, _ = find_violations(bad)
    expect("a dispatching call in a non-allowlisted function must be reported", len(errors) == 1)

    # A newly-derived dispatcher (force_sort_for_view) in a non-allowlisted
    # function must ALSO be caught -- proving the derivation feeds the matcher.
    bad_force_sort = (
        "void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id) {\n"
        "\tworld->renderer->force_sort_for_view(xform);\n"
        "}\n"
    )
    errors, _ = find_violations(bad_force_sort)
    expect("a call to a newly-derived dispatcher must be reported", len(errors) == 1)

    # The same call inside an allowlisted function must not be.
    good = (
        "void GaussianSplatSceneDirector::_initialize_world_renderer(SharedWorld &p_world) {\n"
        "\tp_world.renderer->initialize();\n"
        "}\n"
    )
    errors, found = find_violations(good)
    expect("an allowlisted boundary must not be reported", not errors)
    expect(
        "an allowlisted boundary's call must still be recorded",
        found.get("GaussianSplatSceneDirector::_initialize_world_renderer") == [2],
    )

    # Comments quoting a call must not trip it.
    commented = (
        "void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id) {\n"
        "\t// used to call world->renderer->initialize() here\n"
        "\t/* and set_gaussian_data(x) in a block comment */\n"
        "}\n"
    )
    errors, _ = find_violations(commented)
    expect("commented-out calls must not be reported", not errors)

    # A call before any recognised definition must fail closed.
    orphan = "\tsome_renderer->set_gaussian_data(data);\n"
    errors, _ = find_violations(orphan)
    expect("a call outside any known function must fail closed", len(errors) == 1)

    # Losing the instrumentation must be caught.
    gutted = (
        "void GaussianSplatSceneDirector::_apply_world_submission_to_renderer() {\n"
        "\treturn;\n"
        "}\n"
        "void GaussianSplatSceneDirector::_restore_world_submission_renderer() {\n"
        "\t_report_renderer_contract_lock_violation(\"x\");\n"
        "}\n"
        "void GaussianSplatSceneDirector::_initialize_world_renderer() {\n"
        "\t_report_renderer_contract_lock_violation(\"y\");\n"
        "}\n"
        "bool GaussianSplatSceneDirector::_apply_world_submission_contract_unlocked() {\n"
        "\t_report_renderer_contract_lock_violation(\"z\");\n"
        "}\n"
    )
    expect("a boundary that lost its violation report must be caught", len(check_instrumentation(gutted)) == 1)

    # #611 PR B2 -- the caller-side rule. This is the exact pre-fix shape of
    # submit_world_submission: it passes find_violations (both callees are
    # allowlisted) and must be caught here instead.
    locked_caller = (
        "bool GaussianSplatSceneDirector::submit_world_submission(const WorldSubmission &p_submission) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tif (!_apply_world_submission_to_renderer(*world, record, state)) {\n"
        "\t\t_restore_world_submission_renderer(*world, previous);\n"
        "\t\treturn false;\n"
        "\t}\n"
        "\treturn true;\n"
        "}\n"
    )
    pre_fix_errors, _ = find_violations(locked_caller)
    expect(
        "the pre-fix locked caller must slip past the function-level check "
        "(this is why the caller-side rule exists)",
        not pre_fix_errors,
    )
    expect(
        "a boundary call under an acquired world_mutex must be caught",
        len(check_no_boundary_call_under_lock(locked_caller)) == 2,
    )

    # The post-fix shape -- dispatch between two lock scopes, via the unlocked
    # helper -- must NOT be flagged, or the rule would forbid the fix itself.
    three_phase = (
        "bool GaussianSplatSceneDirector::submit_world_submission(const WorldSubmission &p_submission) {\n"
        "\tMutexLock submission_lock(world_submission_apply_mutex);\n"
        "\t{\n"
        "\t\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\t}\n"
        "\tconst bool applied = _apply_world_submission_contract_unlocked(renderer, contract);\n"
        "\t{\n"
        "\t\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\t}\n"
        "\treturn applied;\n"
        "}\n"
    )
    expect(
        "the three-phase restructure must not be flagged by the caller-side rule",
        not check_no_boundary_call_under_lock(three_phase),
    )

    # A function that never takes the lock may still call the boundaries -- that
    # is _get_or_create_world_for_scenario's documented fallback.
    unlocked_caller = (
        "GaussianSplatSceneDirector::SharedWorld *GaussianSplatSceneDirector::_get_or_create_world_for_scenario(const RID &p_scenario) {\n"
        "\t_apply_world_submission_to_renderer(*entry, entry->world_submission, state);\n"
        "}\n"
    )
    expect(
        "a caller that does not acquire world_mutex must not be flagged",
        not check_no_boundary_call_under_lock(unlocked_caller),
    )

    # --- #723: the declaration-order / destructor rule. ---

    # A renderer release vector declared AFTER the lock is the #628 hang.
    ref_after_lock = (
        "void GaussianSplatSceneDirector::try_prune_world_if_unused(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tLocalVector<Ref<GaussianSplatRenderer>> deferred_renderer_release;\n"
        "\t_prune_world_if_unused(p_scenario, deferred_renderer_release);\n"
        "}\n"
    )
    expect(
        "a renderer release vector declared after the lock must be caught",
        len(check_renderer_ref_released_under_lock(ref_after_lock)) == 1,
    )

    # A bare Ref<GaussianSplatRenderer> declared after the lock is equally bad.
    bare_ref_after_lock = (
        "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tRef<GaussianSplatRenderer> deferred_renderer_release;\n"
        "}\n"
    )
    expect(
        "a bare renderer Ref declared after the lock must be caught",
        len(check_renderer_ref_released_under_lock(bare_ref_after_lock)) == 1,
    )

    # A cv-qualified owning local (`const Ref<...>`) has the identical scope-exit
    # destructor hazard and must not slip past a const-blind matcher (#726 review).
    const_ref_after_lock = (
        "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tconst Ref<GaussianSplatRenderer> renderer = _shared_renderer_for(p_scenario);\n"
        "}\n"
    )
    expect(
        "a const renderer Ref declared after the lock must be caught",
        len(check_renderer_ref_released_under_lock(const_ref_after_lock)) == 1,
    )

    # #730: an `auto`-deduced owning local (from a renderer-Ref source) hides the
    # type but has the identical scope-exit destructor hazard and must be caught.
    auto_ref_after_lock_member = (
        "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tauto renderer = world->renderer;\n"
        "}\n"
    )
    expect(
        "an auto local deduced from a renderer-Ref member must be caught",
        len(check_renderer_ref_released_under_lock(auto_ref_after_lock_member)) == 1,
    )
    auto_ref_after_lock_call = (
        "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tconst auto shared = get_shared_renderer(p_world);\n"
        "}\n"
    )
    expect(
        "a const auto local from get_shared_renderer() must be caught",
        len(check_renderer_ref_released_under_lock(auto_ref_after_lock_call)) == 1,
    )
    # An `auto&` reference is NON-owning -- it drops no reference -- and must NOT be flagged.
    auto_ref_reference_ok = (
        "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tconst auto &renderer = world->renderer;\n"
        "}\n"
    )
    expect(
        "an auto& reference to a renderer Ref must NOT be flagged",
        not check_renderer_ref_released_under_lock(auto_ref_reference_ok),
    )
    # #733: chained access through the renderer member yields a non-owning local
    # (a count / a raw pointer / a call result) -- must NOT be flagged.
    for chained in (
        "\tauto count = world->renderer->get_reference_count();\n",
        "\tauto ptr = world->renderer.ptr();\n",
        "\tauto handle = get_shared_renderer(p_world)->initialize();\n",
    ):
        chained_ok = (
            "void GaussianSplatSceneDirector::clear_world_for_scenario(const RID &p_scenario) {\n"
            "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
            + chained +
            "}\n"
        )
        expect(
            f"chained non-owning renderer access must NOT be flagged: {chained.strip()}",
            not check_renderer_ref_released_under_lock(chained_ok),
        )

    # The compliant shape -- declared BEFORE the lock -- must NOT be flagged.
    ref_before_lock = (
        "void GaussianSplatSceneDirector::try_prune_world_if_unused(const RID &p_scenario) {\n"
        "\tLocalVector<Ref<GaussianSplatRenderer>> deferred_renderer_release;\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\t_prune_world_if_unused(p_scenario, deferred_renderer_release);\n"
        "}\n"
    )
    expect(
        "a renderer release vector declared before the lock must not be flagged",
        not check_renderer_ref_released_under_lock(ref_before_lock),
    )

    # A constructor temporary / return is not a local declaration -- not flagged.
    temporary_after_lock = (
        "Ref<GaussianSplatRenderer> GaussianSplatSceneDirector::get_shared_renderer(World3D *p_world) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\treturn Ref<GaussianSplatRenderer>();\n"
        "}\n"
    )
    expect(
        "a renderer Ref temporary (not a declaration) must not be flagged",
        not check_renderer_ref_released_under_lock(temporary_after_lock),
    )

    # A reference parameter is not a local that destructs here -- not flagged.
    ref_param = (
        "void GaussianSplatSceneDirector::_prune_world_if_unused(const RID &p_scenario, "
        "LocalVector<Ref<GaussianSplatRenderer>> &r_deferred_release) {\n"
        "\tGaussianSplatting::ThreadOwnedMutexLock lock(world_mutex);\n"
        "\tr_deferred_release.push_back(x);\n"
        "}\n"
    )
    expect(
        "a renderer Ref reference-parameter must not be flagged",
        not check_renderer_ref_released_under_lock(ref_param),
    )

    # A renderer Ref in a function that never takes world_mutex is fine.
    ref_no_lock = (
        "void GaussianSplatSceneDirector::make_renderer() {\n"
        "\tRef<GaussianSplatRenderer> r;\n"
        "}\n"
    )
    expect(
        "a renderer Ref in a lock-free function must not be flagged",
        not check_renderer_ref_released_under_lock(ref_no_lock),
    )

    if failures:
        print("Renderer-contract boundary guard SELF-TEST FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("Renderer-contract boundary guard self-test passed.")
    print(
        f"  derived {len(DISPATCHING_METHODS)} dispatching method(s) from the renderer sources: "
        f"{', '.join(DISPATCHING_METHODS)}"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run the guard's own discrimination cases instead of scanning the tree.",
    )
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if not DIRECTOR_SOURCE.is_file():
        print(f"ERROR: scene director source not found: {DIRECTOR_SOURCE}", file=sys.stderr)
        return 1

    integrity_errors = check_derivation_integrity()
    if integrity_errors:
        print("Renderer-contract boundary guard FAILED (#723 derivation integrity):", file=sys.stderr)
        for error in integrity_errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    source_text = DIRECTOR_SOURCE.read_text(encoding="utf-8")
    errors, found = find_violations(source_text)
    errors.extend(check_instrumentation(source_text))
    errors.extend(check_no_boundary_call_under_lock(source_text))
    errors.extend(check_renderer_ref_released_under_lock(source_text))

    if errors:
        print("Renderer-contract boundary guard FAILED (#611):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    total = sum(len(lines) for lines in found.values())
    print(
        f"Renderer-contract boundary guard passed: {total} blocking-dispatch call(s) "
        f"in {len(found)} function(s), all instrumented or provably unlocked."
    )
    print(
        f"  DISPATCHING_METHODS derived from renderer sources ({len(DISPATCHING_METHODS)}): "
        f"{', '.join(DISPATCHING_METHODS)}"
    )
    for name in sorted(found):
        print(f"  - {name}: {len(found[name])} call(s) -- {ALLOWED_FUNCTIONS[name]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
