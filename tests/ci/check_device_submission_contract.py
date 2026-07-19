#!/usr/bin/env python3
"""#685: keep every local-device submit and blocking readback behind gs_device_utils.

A Godot local `RenderingDevice` (`is_main_rendering_device() == false`) is the only
device class on which `submit()` / `sync()` do anything. Between the two it sets
`local_device_processing` and has already run `_end_frame()` -- its command buffer
and draw graph are ended. Anything that re-enters the frame lifecycle in that
window operates on ended state:

  * `submit()` again        -> ERR_FAIL "device already submitted"
                               (servers/rendering/rendering_device.cpp:6323), and
                               the flush is silently dropped
  * `buffer_get_data()`     -> `_flush_and_stall_for_all_frames()` -> `_end_frame()`
    `texture_get_data()`       -> `RenderingDeviceGraph::end()` replays into an
                               ended command buffer -> driver fault
  * any further RECORDING   -> compute lists, buffer_updates and uniform sets are
    on that frame              appended to an ended draw graph, which the next
                               _end_frame() replays into a command buffer that was
                               never begun -> VK_ERROR_DEVICE_LOST

Both faults were live in the `[World][SceneTree][RequiresGPU]` corpus:
`_sort_instance_pipeline` issued `safe_submit(compute_rd)` with no matching sync
(interfaces/gpu_sorting_pipeline.cpp), the sorter submitted twice more, and
`_capture_instance_count_sync` then read the count buffer back synchronously --
while the instance-cull submit (interfaces/gpu_culler.cpp) left the device
submitted across the whole sort pass and lost the device outright.

Nothing in this module ever paired a `safe_submit` with a later `sync()`, so the
in-flight window had no owner and no consumer. That is why the contract is "a
local device is never left submitted" rather than "remember to sync": there was no
caller in a position to remember.

Shipping configurations do not hit any of it:
`GaussianSplatManager::get_primary_rendering_device()`
(core/gaussian_splat_manager.cpp) deliberately returns the MAIN device, for which
`safe_submit` is a no-op, so the imbalance never forms. That protection is a
property of one function's return value, not of this code path -- which is exactly
why it needs a guard rather than a comment. Re-point the renderer at a local
device (the manager still creates them, and
`rendering/gaussian_splatting/shared_submission_device_enabled` is a live setting)
and the crashes come straight back.

So the rule is enforced at the boundary instead of documented at the call sites:
every submit/sync/blocking-readback in module production code goes through
`gs_device_utils`, whose helpers leave the device in a recording state.

Fail-closed: an unrecognised raw call is an ERROR, not a skip. A missing
`sync_policy.h`, a missing engine accessor, or a helper that has lost its sync or
settle call all fail the guard -- otherwise it could pass on a tree where the
contract had been quietly gutted.

Usage:
    python tests/ci/check_device_submission_contract.py
    python tests/ci/check_device_submission_contract.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_ROOT = ROOT / "modules" / "gaussian_splatting"
SYNC_POLICY = MODULE_ROOT / "interfaces" / "sync_policy.h"
ENGINE_DEVICE_HEADER = ROOT / "servers" / "rendering" / "rendering_device.h"

# The only file allowed to touch the raw RenderingDevice submission API. Every
# other module source must go through the helpers it defines.
EXEMPT_RELATIVE_PATHS = frozenset({"interfaces/sync_policy.h"})

# Directories excluded from the scan. Tests legitimately drive raw devices to
# construct the very states this guard protects production code from, and
# `test_integration.cpp` asserts safe_submit's own behaviour on both device
# classes.
EXCLUDED_DIR_NAMES = frozenset({"tests"})

# Raw calls that are forbidden outside gs_device_utils, with the helper that
# replaces each.
FORBIDDEN_CALLS = {
    "submit": "gs_device_utils::safe_submit / safe_submit_and_sync",
    "sync": "gs_device_utils::safe_sync / safe_submit_and_sync",
    "buffer_get_data": "gs_device_utils::safe_buffer_get_data",
    "texture_get_data": "gs_device_utils::safe_texture_get_data",
}

# `buffer_get_data_async` is the non-blocking path: it queues a copy and returns,
# so it never re-enters _end_frame() and is not covered by this rule. The
# word-boundary-terminated alternation below must not match it.
_FORBIDDEN_CALL_RE = re.compile(
    r"(?:->|\.)\s*(" + "|".join(FORBIDDEN_CALLS) + r")\s*\("
)

# gs_device_utils helpers that must keep leaving the device in a RECORDING state.
# Each maps to the token that has to appear inside its body.
#
#   safe_submit / safe_submit_and_sync -- must sync(). An in-flight local-device
#       submission has no owner in this module: every caller keeps recording
#       afterwards, into the command buffer and draw graph that submit() ended.
#   safe_buffer_get_data / safe_texture_get_data -- must settle first. They stall
#       through _flush_and_stall_for_all_frames() -> _end_frame(), which re-ends
#       already-ended state if a submission is outstanding.
HELPER_SETTLE_REQUIREMENTS = {
    "safe_submit": "p_device->sync()",
    "safe_sync": "settle_outstanding_submit",
    "safe_submit_and_sync": "p_device->sync()",
    "safe_buffer_get_data": "settle_outstanding_submit",
    "safe_texture_get_data": "settle_outstanding_submit",
}

# The accessor the whole contract rests on. Without it `has_outstanding_submit`
# cannot answer, and every helper degrades to its pre-fix behaviour.
ENGINE_ACCESSOR = "is_local_device_submission_pending"

_SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".cc", ".inc")


def strip_comments_and_strings(text: str) -> str:
    """Blank out //, /* */ and string literals, preserving line structure.

    This file's own prose quotes the call shapes it forbids, and so do the
    comments in sync_policy.h and gpu_sorting_pipeline.cpp, so scanning raw text
    would report the documentation as violations. Line count is preserved so
    reported line numbers stay accurate.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end == -1:
                out.append(" " * (n - i))
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
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def iter_module_sources() -> list[Path]:
    sources: list[Path] = []
    for path in sorted(MODULE_ROOT.rglob("*")):
        if not path.is_file() or path.suffix not in _SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(MODULE_ROOT)
        if EXCLUDED_DIR_NAMES.intersection(relative.parts[:-1]):
            continue
        if relative.as_posix() in EXEMPT_RELATIVE_PATHS:
            continue
        sources.append(path)
    return sources


def find_raw_calls(source_text: str) -> list[tuple[int, str]]:
    """Return [(line number, method name)] for each forbidden raw call."""
    hits: list[tuple[int, str]] = []
    for lineno, line in enumerate(strip_comments_and_strings(source_text).splitlines(), start=1):
        for match in _FORBIDDEN_CALL_RE.finditer(line):
            hits.append((lineno, match.group(1)))
    return hits


def check_helper_instrumentation(sync_policy_text: str) -> list[str]:
    """Every helper must still settle. A gutted helper would silently pass everything."""
    errors: list[str] = []
    stripped = strip_comments_and_strings(sync_policy_text)
    for helper, required_token in HELPER_SETTLE_REQUIREMENTS.items():
        pattern = re.compile(
            r"\b" + re.escape(helper) + r"\s*\([^)]*\)\s*\{(.*?)\n\}", re.DOTALL
        )
        match = pattern.search(stripped)
        if match is None:
            errors.append(
                f"interfaces/sync_policy.h: helper '{helper}' not found -- the "
                f"submission contract cannot be enforced without it"
            )
            continue
        if required_token not in match.group(1):
            errors.append(
                f"interfaces/sync_policy.h: helper '{helper}' no longer calls "
                f"'{required_token}'; it would leave a local device submitted "
                f"and reintroduce #685"
            )
    return errors


def check_engine_accessor(header_text: str) -> list[str]:
    if ENGINE_ACCESSOR not in header_text:
        return [
            f"servers/rendering/rendering_device.h: missing '{ENGINE_ACCESSOR}()'. "
            f"gs_device_utils::has_outstanding_submit needs it to observe "
            f"local_device_processing; without it the guard's helpers cannot tell "
            f"a submitted device from a recording one (#685)."
        ]
    return []


_SELF_TEST_CASES: tuple[tuple[str, str, bool], ...] = (
    (
        "raw submit is caught",
        "void f() { device->submit(); }",
        True,
    ),
    (
        "raw sync is caught",
        "void f() { device->sync(); }",
        True,
    ),
    (
        "raw buffer_get_data is caught",
        "void f() { auto d = rd->buffer_get_data(b, 0, 4); }",
        True,
    ),
    (
        "raw texture_get_data is caught",
        "void f() { auto d = rd->texture_get_data(t, 0); }",
        True,
    ),
    (
        "dot-call form is caught",
        "void f() { device.submit(); }",
        True,
    ),
    (
        "whitespace before paren is caught",
        "void f() { device->submit  (); }",
        True,
    ),
    (
        "async readback is NOT caught (non-blocking, never re-enters _end_frame)",
        "void f() { rd->buffer_get_data_async(b, cb, 0, 4); }",
        False,
    ),
    (
        "helper call is NOT caught",
        "void f() { gs_device_utils::safe_submit(device); }",
        False,
    ),
    (
        "the same call inside a // comment is NOT caught",
        "void f() { /* nothing */ } // device->submit(); is described here",
        False,
    ),
    (
        "the same call inside a block comment is NOT caught",
        "/* device->buffer_get_data(b, 0, 4); */\nvoid f() {}",
        False,
    ),
    (
        "the same call inside a string literal is NOT caught",
        'void f() { log("device->submit() failed"); }',
        False,
    ),
)


def run_self_test() -> int:
    failures: list[str] = []

    for label, snippet, should_flag in _SELF_TEST_CASES:
        flagged = bool(find_raw_calls(snippet))
        if flagged != should_flag:
            failures.append(
                f"{label}: expected flagged={should_flag}, got flagged={flagged}"
            )

    # A helper that has lost its settle call must be reported.
    gutted = (
        "namespace gs_device_utils {\n"
        "inline void safe_submit(RenderingDevice *p_device) {\n"
        "    p_device->submit();\n"
        "}\n"
        "inline void safe_sync(RenderingDevice *p_device) {\n"
        "    settle_outstanding_submit(p_device);\n"
        "}\n"
        "inline void safe_submit_and_sync(RenderingDevice *p_device) {\n"
        "    p_device->submit();\n"
        "    p_device->sync();\n"
        "}\n"
        "inline Vector<uint8_t> safe_buffer_get_data(RenderingDevice *p_device) {\n"
        "    settle_outstanding_submit(p_device);\n"
        "}\n"
        "inline Vector<uint8_t> safe_texture_get_data(RenderingDevice *p_device) {\n"
        "    settle_outstanding_submit(p_device);\n"
        "}\n"
        "}\n"
    )
    gutted_errors = check_helper_instrumentation(gutted)
    if len(gutted_errors) != 1 or "safe_submit'" not in gutted_errors[0]:
        failures.append(
            "instrumentation check did not report exactly the gutted safe_submit: "
            f"{gutted_errors}"
        )

    # A missing helper must be reported, not silently skipped.
    if not check_helper_instrumentation("namespace gs_device_utils {}\n"):
        failures.append("instrumentation check did not report missing helpers")

    # A missing engine accessor must be reported.
    if not check_engine_accessor("class RenderingDevice { bool is_main_rendering_device(); };"):
        failures.append("engine-accessor check did not report the missing accessor")
    if check_engine_accessor(f"bool {ENGINE_ACCESSOR}() const {{ return x; }}"):
        failures.append("engine-accessor check false-positived on a present accessor")

    if failures:
        print("Device-submission contract guard SELF-TEST FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        f"Device-submission contract guard self-test passed "
        f"({len(_SELF_TEST_CASES) + 4} discrimination cases)."
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

    errors: list[str] = []

    if not SYNC_POLICY.is_file():
        print(f"ERROR: sync policy header not found: {SYNC_POLICY}", file=sys.stderr)
        return 1
    if not ENGINE_DEVICE_HEADER.is_file():
        print(f"ERROR: engine device header not found: {ENGINE_DEVICE_HEADER}", file=sys.stderr)
        return 1

    errors.extend(check_helper_instrumentation(SYNC_POLICY.read_text(encoding="utf-8")))
    errors.extend(check_engine_accessor(ENGINE_DEVICE_HEADER.read_text(encoding="utf-8")))

    sources = iter_module_sources()
    if not sources:
        print(
            f"ERROR: no module sources found under {MODULE_ROOT}; the guard would "
            f"pass vacuously.",
            file=sys.stderr,
        )
        return 1

    scanned = 0
    for path in sources:
        scanned += 1
        relative = path.relative_to(ROOT).as_posix()
        for lineno, method in find_raw_calls(path.read_text(encoding="utf-8", errors="replace")):
            errors.append(
                f"{relative}:{lineno}: raw '{method}()' on a RenderingDevice; "
                f"use {FORBIDDEN_CALLS[method]} so an outstanding local-device "
                f"submission is settled first (#685)"
            )

    if errors:
        print("Device-submission contract guard FAILED (#685):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        f"Device-submission contract guard passed: {scanned} module source(s) scanned, "
        f"0 raw submit/sync/blocking-readback calls outside gs_device_utils."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
