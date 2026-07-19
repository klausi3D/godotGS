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

Fail-closed: a dispatching call whose enclosing function cannot be determined is
an ERROR, not a skip.

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

# Renderer methods that reach `_dispatch_call_on_render_thread_blocking`, either
# directly or through one call:
#   initialize                          -> renderer/gaussian_splat_renderer.cpp:1616
#   set_max_splats                      -> renderer/render_quality_orchestrator.cpp
#   set_gaussian_data                   -> renderer/render_data_orchestrator.cpp
#   set_file_backed_payload_source      -> renderer/render_data_orchestrator.cpp
#   apply_world_submission_contract     -> reaches all of the above
#   restore_world_submission_runtime_state / clear_world_submission_contract
#                                       -> reach set_gaussian_data
DISPATCHING_METHODS = (
    "initialize",
    "set_max_splats",
    "set_gaussian_data",
    "set_file_backed_payload_source",
    "apply_world_submission_contract",
    "restore_world_submission_runtime_state",
    "clear_world_submission_contract",
)

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
    "GaussianSplatSceneDirector::teardown_world_for_scenario": (
        "#628: explicitly moves the renderer Ref out under the lock and calls this "
        "after the MutexLock scope has ended"
    ),
}

# The three boundary functions must keep their instrumentation. Without this the
# guard could pass on a tree where the counter had been quietly gutted.
INSTRUMENTED_FUNCTIONS = (
    "GaussianSplatSceneDirector::_apply_world_submission_to_renderer",
    "GaussianSplatSceneDirector::_restore_world_submission_renderer",
    "GaussianSplatSceneDirector::_initialize_world_renderer",
)

_DEFINITION_RE = re.compile(
    r"^[A-Za-z_][\w:<>*&,\s]*?\b(GaussianSplatSceneDirector(?:::\w+)+)\s*\("
)
_CALL_RE = re.compile(r"(?:->|\.)\s*(" + "|".join(DISPATCHING_METHODS) + r")\s*\(")


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, preserving line structure.

    Comments in this file quote the very call sites the guard looks for, so
    scanning raw text would produce false positives. Line count is preserved so
    reported line numbers stay accurate.
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
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


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


def check_instrumentation(source_text: str) -> list[str]:
    """Each boundary function must still report violations."""
    errors: list[str] = []
    text = strip_comments(source_text)
    lines = text.splitlines()

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

    # A call in a non-allowlisted function must be caught.
    bad = (
        "void GaussianSplatSceneDirector::register_instance(ObjectID p_node_id) {\n"
        "\tworld->renderer->initialize();\n"
        "}\n"
    )
    errors, _ = find_violations(bad)
    expect("a dispatching call in a non-allowlisted function must be reported", len(errors) == 1)

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
    )
    expect("a boundary that lost its violation report must be caught", len(check_instrumentation(gutted)) == 1)

    if failures:
        print("Renderer-contract boundary guard SELF-TEST FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("Renderer-contract boundary guard self-test passed (5 discrimination cases).")
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

    source_text = DIRECTOR_SOURCE.read_text(encoding="utf-8")
    errors, found = find_violations(source_text)
    errors.extend(check_instrumentation(source_text))

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
    for name in sorted(found):
        print(f"  - {name}: {len(found[name])} call(s) -- {ALLOWED_FUNCTIONS[name]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
