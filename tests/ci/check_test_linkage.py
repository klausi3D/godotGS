#!/usr/bin/env python3
"""Guard: no Gaussian Splatting test `.cpp` is silently linker-dropped (#178).

## The failure this guards against

Godot builds the module tests in two different ways:

* Every `modules/gaussian_splatting/tests/*.h` is collected by `modules/SCsub`
  (`glob.glob(.../tests/*.h)`) into the generated `modules_tests.gen.h`, which is
  `#include`d by `tests/test_main.cpp`. A `.h` test's `TEST_CASE`s are therefore
  compiled *into the test-main translation unit* and always register.
* Every `modules/gaussian_splatting/tests/*.cpp` is compiled as its OWN object
  file into the module static library (`modules/gaussian_splatting/SCsub`:
  `env_gaussian_splatting.add_source_files(module_sources, "tests/*.cpp")`).

doctest registers a `TEST_CASE` through a file-scope static initializer. MSVC (and
`--gc-sections` on GCC/Clang) drop an object file pulled from a static library when
*nothing outside it references any of its symbols* - and a `TEST_CASE`-only object
exports no symbol anything references. The static initializer is dropped with the
object, so the cases compile clean but NEVER register and NEVER run. The module's
test count is silently inflated by every such file.

The established defence (see `test_gpu_streaming.cpp` /
`test_gaussian_streaming_lifecycle.cpp`) is a `force_link` anchor: the `.cpp`
defines `extern "C" int <name>_force_link() { return 0; }` and the auto-included
aggregator header `tests/test_gaussian_splatting.h` *calls* it from a file-scope
initializer, forcing the linker to keep the object (and thus its `TEST_CASE`
registrations).

## The method this guard uses (static, no binary required)

For every `tests/*.cpp` that declares at least one `TEST_CASE(` /
`TEST_CASE_TEMPLATE(`, the guard requires EITHER:

  * a `force_link` anchor: the file defines an `extern "C" int <sym>()` whose
    `<sym>` is *called* from the aggregator header
    `tests/test_gaussian_splatting.h` (the `= <sym>();` initializer), OR
  * an explicit `KNOWN_UNLINKED` allow-list entry carrying a reason + tracking
    issue for a file whose cases are intentionally NOT linked (e.g. structurally
    broken and tracked elsewhere).

Any test `.cpp` that is neither anchored nor allow-listed FAILS the guard: its
cases are (or will be) silently linker-dropped.

The static signal was cross-checked against the built binary's registered
`TEST_CASE` list when this guard landed; it is the reliable deterministic signal
for the binary-free `--guard-only` lane.

## Fail-closed rules

A guard that can be silently switched off is worse than no guard, so every
ambiguity below FAILS rather than passing:

1. **Comments never disable a check.** Both the `.cpp` and the aggregator header
   are comment-stripped before analysis, so commenting out an anchor call does
   NOT read as "still anchored" - the file reports as unanchored and FAILS.
2. **No conditional compilation in the aggregator header.** The header is a pure
   `#pragma once` + `#include` + anchor list. Any other preprocessor directive in
   it FAILS: the guard refuses to emulate a preprocessor, so an `#if 0` around
   the anchor block can never read as green. (Same posture as
   `check_cull_signature_parity.py`'s preprocessor-free-region rule.)
3. **No conditional compilation around a `.cpp`'s anchor definition** other than
   the whole-file build-configuration wrappers in `_BUILD_CONFIG_MACROS`. An
   anchor under `#if 0` (or in the `#else` branch of a build-config wrapper) is
   not compiled, so its cases are dropped while the file *looks* anchored: FAIL.
4. **Cases reachable through `#include` are accounted for, never ignored.** Every
   `tests/*.h` is ALREADY compiled once into the test-main TU by the `modules/SCsub`
   glob, and `#pragma once` only dedupes within a single translation unit. So a
   test `.cpp` that includes a case-carrying `tests/*.h` compiles a SECOND copy of
   those cases into its own object. While the object is linker-dropped that is
   harmless; the moment it is anchored, every one of those cases double-registers
   and double-runs. The guard therefore FAILS on an anchored `.cpp` that reaches
   cases through includes, and requires an explicit `HEADER_SOURCED_CASES` entry
   (which forbids anchoring) for an unanchored one. It is never silently ignored.
5. **Stale bookkeeping FAILS.** A missing module tests directory, a
   missing/renamed aggregator header, or a stale `KNOWN_UNLINKED` /
   `HEADER_SOURCED_CASES` entry (naming a file that no longer exists, that has no
   tests, or whose status has changed) all FAIL, so neither registry can silently
   mask a recovered or deleted file.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
TESTS_DIR = MODULE / "tests"
# Auto-included via modules/SCsub's tests/*.h glob -> modules_tests.gen.h; this is
# where force_link anchors must be *called* for the linker to keep the object.
AGGREGATOR_HEADER = TESTS_DIR / "test_gaussian_splatting.h"

# Files intentionally NOT linked. Every entry must name a real test .cpp whose
# cases are genuinely linker-dropped, with a human reason and a tracking issue.
# The guard fails closed if an entry becomes stale (file gone / no tests / now
# anchored) so this list can never silently mask a recovered or deleted file.
#
# Schema: filename (basename, no path) -> {"reason": str, "issue": str}
KNOWN_UNLINKED: dict[str, dict[str, str]] = {
    "test_gpu_sorting.cpp": {
        "reason": (
            "OPEN QUESTION - awaiting maintainer disposition, see #622 and the "
            "review thread on PR #642. Verified facts only: its 8 "
            "[GaussianSplatting][RequiresGPU] cases are confirmed absent from the "
            "built binary's --list-test-cases output, i.e. they are dropped today "
            "exactly like the 11 files this guard's PR anchored. Anchoring it was "
            "NOT attempted, so there is no evidence here that it cannot link or "
            "cannot pass; this entry records an unproven exclusion, not a "
            "demonstrated defect. It must not be read as a justification to keep "
            "the cases dark: either anchor it (and file whatever it surfaces, as "
            "#636-#641 did for the other files) or replace this text with a "
            "reason backed by a run. #622 itself documents a DIFFERENT problem "
            "(the GpuSorting harness batch glob matches 0 tests) and does not "
            "assert these cases are broken."
        ),
        "issue": "https://github.com/klausi3D/godotGS/issues/622",
    },
    "test_painterly_viewport_copy.cpp": {
        "reason": (
            "Structurally broken: its case calls "
            "GaussianSplatRenderer::test_override_rendering_device(), which is "
            "declared in gaussian_splat_renderer.h but never defined anywhere in "
            "the codebase. Force-linking this file turns the missing definition "
            "into a hard LNK2019 unresolved-external build break. Left unlinked "
            "until the method is implemented (or the test rewritten/removed)."
        ),
        "issue": "https://github.com/klausi3D/godotGS/issues/631",
    },
}

# Test .cpp files whose TEST_CASEs come from an included tests/*.h rather than
# from their own text. Those cases DO run - the header is glob-included into the
# test-main TU independently - but the .cpp must never be force-link anchored, or
# each of them registers and runs twice. Entries are verified in both directions
# (must really include case-carrying headers; must really not be anchored).
#
# Schema: filename (basename, no path) -> {"reason": str}
HEADER_SOURCED_CASES: dict[str, dict[str, str]] = {
    "test_config_validation.cpp": {
        "reason": (
            "Declares no TEST_CASE of its own; includes test_config_validation.h, "
            "whose cases are already compiled into the test-main TU by the "
            "modules/SCsub tests/*.h glob. Correct as-is precisely BECAUSE this "
            "object is linker-dropped. Do NOT add a force_link anchor: that would "
            "compile a second copy of all of that header's cases into this object "
            "and double-register every one of them."
        ),
    },
}

# doctest case-declaring macros. TEST_SUITE is a grouping wrapper (not itself a
# case) and is tracked only for reporting.
_CASE_RE = re.compile(r"\bTEST_CASE(?:_TEMPLATE)?\s*\(")
_SUITE_RE = re.compile(r"\bTEST_SUITE\s*\(")
# A force_link anchor definition: `extern "C" int <sym>(...) {` in the .cpp.
_FORCE_LINK_DEF_RE = re.compile(r'extern\s+"C"\s+int\s+(\w+)\s*\([^)]*\)\s*\{')
# Local (quoted) includes; angle-bracket includes can never name a tests/*.h.
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)

_PP_DIRECTIVE_RE = re.compile(r"^\s*#\s*(\w+)")
_COND_OPEN_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)$")
_COND_ELSE_RE = re.compile(r"^\s*#\s*(else|elif)\b")
_COND_CLOSE_RE = re.compile(r"^\s*#\s*endif\b")

# The only directives the aggregator header may contain. It is a pure include
# aggregator plus the anchor list; anything else means conditional compilation the
# guard would have to emulate to stay honest, so it fails closed instead.
_ALLOWED_HEADER_DIRECTIVES = {"pragma", "include"}

# Whole-file build-configuration wrappers a force_link anchor may legitimately sit
# inside. Anything else enclosing an anchor (notably `#if 0`) fails closed.
_BUILD_CONFIG_MACROS = {"TESTS_ENABLED", "TOOLS_ENABLED", "DEBUG_ENABLED", "DEV_ENABLED"}


def _strip_comments(text: str) -> str:
    """Remove // line comments and /* */ block comments so a TEST_CASE or an anchor
    call mentioned in a comment is not counted as real code. Line structure is
    preserved (a stripped block comment keeps its newlines) so line numbers stay
    usable for the conditional-nesting analysis. String literals are left intact; a
    stray macro-looking token inside a string is vanishingly unlikely in these files
    and would only ever cause a (safe) over-count that a real anchor satisfies."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _count_cases(text: str) -> tuple[int, int]:
    return len(_CASE_RE.findall(text)), len(_SUITE_RE.findall(text))


def _force_link_symbols(text: str) -> list[str]:
    return _FORCE_LINK_DEF_RE.findall(text)


def _is_referenced(symbol: str, header_text: str) -> bool:
    # The load-bearing line is the initializer call `= <sym>();`. A bare extern
    # declaration without a call would NOT force the link, so a call is required.
    # `header_text` MUST already be comment-stripped by the caller, otherwise a
    # commented-out anchor would still read as present (fail-open).
    return re.search(r"=\s*" + re.escape(symbol) + r"\s*\(\s*\)", header_text) is not None


def _header_directive_violations(header_text: str) -> list[str]:
    """Every preprocessor directive in the aggregator header other than `#pragma`
    and `#include`. The guard does not emulate a preprocessor: it requires the
    anchor list to be unconditionally compiled, so `#if 0` (or any other
    conditional) around it is a hard FAIL rather than a silently-honoured switch."""
    violations = []
    for lineno, line in enumerate(header_text.splitlines(), start=1):
        match = _PP_DIRECTIVE_RE.match(line)
        if match and match.group(1) not in _ALLOWED_HEADER_DIRECTIVES:
            violations.append(f"line {lineno}: `{line.strip()}`")
    return violations


def _conditional_stack_at(text: str, needle_line: int) -> list[tuple[str, str, bool, int]]:
    """The stack of open preprocessor conditionals enclosing 1-based `needle_line`.

    Each frame is (kind, condition, in_else_branch, lineno). Purely structural line
    bookkeeping - no macro expansion, no branch evaluation - so it cannot silently
    mis-evaluate; it only reports what a line is nested inside."""
    stack: list[list] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if lineno >= needle_line:
            break
        open_match = _COND_OPEN_RE.match(line)
        if open_match:
            stack.append([open_match.group(1), open_match.group(2).strip(), False, lineno])
            continue
        if _COND_ELSE_RE.match(line):
            if stack:
                stack[-1][2] = True
            continue
        if _COND_CLOSE_RE.match(line):
            if stack:
                stack.pop()
    return [tuple(frame) for frame in stack]


def _condition_is_build_config(kind: str, condition: str) -> bool:
    """True only for the whole-file build-configuration wrappers the test .cpp
    files legitimately use (`#ifdef TESTS_ENABLED` and friends). `#if 0`, and any
    condition this does not positively recognise, returns False -> fail closed."""
    if kind in ("ifdef", "ifndef"):
        return condition.split()[0] in _BUILD_CONFIG_MACROS if condition.split() else False
    # `#if defined(X)` / `#if defined X`, single macro only.
    defined_match = re.fullmatch(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)", condition.strip())
    if defined_match:
        macro = defined_match.group(1) or defined_match.group(2)
        return macro in _BUILD_CONFIG_MACROS
    return False


def _anchor_guard_violations(text: str, symbols: list[str]) -> list[str]:
    """Conditional-compilation problems around a .cpp's force_link anchor definition.

    An anchor that is not unconditionally compiled does not force anything: the
    object is still dropped, but the file reads as anchored. Fail closed."""
    violations = []
    lines = text.splitlines()
    for symbol in symbols:
        def_line = next(
            (i for i, line in enumerate(lines, start=1)
             if re.search(r'extern\s+"C"\s+int\s+' + re.escape(symbol) + r"\s*\(", line)),
            None,
        )
        if def_line is None:
            continue
        for frame in _conditional_stack_at(text, def_line):
            kind, condition, in_else, open_line = frame
            directive = f"#{kind} {condition}".strip()
            if in_else:
                violations.append(
                    f"force_link anchor `{symbol}` (line {def_line}) sits in the #else/#elif "
                    f"branch of `{directive}` (line {open_line}); the anchor must be compiled "
                    "unconditionally in test builds, so this reads as anchored while the "
                    "object is still linker-dropped."
                )
            elif not _condition_is_build_config(kind, condition):
                violations.append(
                    f"force_link anchor `{symbol}` (line {def_line}) is nested inside "
                    f"`{directive}` (line {open_line}), which is not a recognised build-config "
                    f"wrapper ({', '.join(sorted(_BUILD_CONFIG_MACROS))}). A conditionally-"
                    "compiled anchor forces nothing: the cases are still dropped while the "
                    "file looks anchored. Keep the anchor unconditional (or extend "
                    "_BUILD_CONFIG_MACROS if this really is a build-config wrapper)."
                )
    return violations


def _reachable_case_headers(cpp_path: Path, cpp_text: str) -> dict[str, int]:
    """tests/*.h files reachable through `#include` from this .cpp that declare
    TEST_CASEs, mapped to their case count.

    Only headers inside TESTS_DIR matter: those are exactly the files the
    `modules/SCsub` `tests/*.h` glob already compiles into the test-main TU, so
    they are the ones a second copy would double-register. Upstream/thirdparty
    headers (`tests/test_macros.h`, `doctest.h`) mention TEST_CASE in macro
    definitions and are deliberately not followed."""
    found: dict[str, int] = {}
    seen: set[Path] = {cpp_path.resolve()}
    queue: list[tuple[Path, str]] = [(cpp_path, cpp_text)]
    while queue:
        current_path, current_text = queue.pop()
        for include in _INCLUDE_RE.findall(current_text):
            candidate = (current_path.parent / include).resolve()
            if candidate.parent != TESTS_DIR.resolve() or candidate.suffix != ".h":
                continue
            if candidate in seen or not candidate.is_file():
                continue
            seen.add(candidate)
            header_text = _strip_comments(candidate.read_text(encoding="utf-8", errors="replace"))
            n_cases, _ = _count_cases(header_text)
            if n_cases:
                found[candidate.name] = n_cases
            queue.append((candidate, header_text))
    return found


def main() -> int:
    if not TESTS_DIR.is_dir():
        print(f"[test-linkage-check] FAIL missing tests dir {TESTS_DIR.relative_to(ROOT)}")
        return 1
    if not AGGREGATOR_HEADER.is_file():
        print(
            f"[test-linkage-check] FAIL missing aggregator header "
            f"{AGGREGATOR_HEADER.relative_to(ROOT)} (force_link anchors live here)"
        )
        return 1

    raw_header_text = AGGREGATOR_HEADER.read_text(encoding="utf-8")

    # Fail closed on conditional compilation in the aggregator header before any
    # anchor matching: the guard cannot know which branch compiles, and an `#if 0`
    # around the anchor list would otherwise leave every anchored file reading as
    # green while its cases silently stopped registering.
    directive_violations = _header_directive_violations(raw_header_text)
    if directive_violations:
        for violation in directive_violations:
            print(
                f"[test-linkage-check] FAIL {AGGREGATOR_HEADER.name} contains a non-include "
                f"preprocessor directive at {violation}; this header must stay a plain "
                "`#pragma once` + `#include` + force_link-anchor list so the anchor set can be "
                "enumerated deterministically. Conditional compilation here can silently "
                "un-register every anchored test."
            )
        return 1

    # Comment-stripped: a commented-out anchor call must NOT read as anchored.
    header_text = _strip_comments(raw_header_text)

    cpp_files = sorted(TESTS_DIR.glob("*.cpp"))
    if not cpp_files:
        print(f"[test-linkage-check] FAIL found no tests/*.cpp under {TESTS_DIR.relative_to(ROOT)}")
        return 1

    failures: list[str] = []
    anchored: list[str] = []
    allowlisted: list[str] = []
    header_sourced: list[str] = []
    no_tests = 0
    seen_names: set[str] = set()

    for cpp in cpp_files:
        name = cpp.name
        seen_names.add(name)
        stripped = _strip_comments(cpp.read_text(encoding="utf-8"))
        n_cases, _n_suites = _count_cases(stripped)
        symbols = _force_link_symbols(stripped)
        is_anchored = any(_is_referenced(sym, header_text) for sym in symbols)
        in_allowlist = name in KNOWN_UNLINKED
        in_header_sourced = name in HEADER_SOURCED_CASES
        via_includes = _reachable_case_headers(cpp, stripped)
        n_via_includes = sum(via_includes.values())

        # An anchor that is not unconditionally compiled forces nothing.
        if is_anchored:
            for violation in _anchor_guard_violations(stripped, symbols):
                failures.append(f"{name}: {violation}")

        # Cases pulled in through a tests/*.h are ALREADY compiled into the
        # test-main TU. Anchoring such a .cpp double-registers every one of them.
        if via_includes:
            summary = ", ".join(f"{h} ({c} cases)" for h, c in sorted(via_includes.items()))
            if is_anchored:
                failures.append(
                    f"{name}: is force-link anchored AND reaches {n_via_includes} TEST_CASE(s) "
                    f"through #include [{summary}]. Every tests/*.h is already glob-included "
                    "into the test-main translation unit by modules/SCsub, and #pragma once "
                    "only dedupes within one TU - so anchoring this object compiles and "
                    "registers a SECOND copy of those cases, which then run twice. Remove the "
                    "TEST_CASE-carrying include(s) (see the note in test_gaussian_splatting.cpp) "
                    "or drop the anchor."
                )
                continue
            if not in_header_sourced:
                failures.append(
                    f"{name}: reaches {n_via_includes} TEST_CASE(s) through #include [{summary}] "
                    f"but has no HEADER_SOURCED_CASES entry. Those cases run via the test-main "
                    "TU, not via this object, and force-link anchoring this file would "
                    "double-register them. Add a HEADER_SOURCED_CASES entry recording that (and "
                    "that the file must stay unanchored), or remove the include."
                )
                continue
            if not HEADER_SOURCED_CASES[name].get("reason", "").strip():
                failures.append(f"{name}: HEADER_SOURCED_CASES entry must carry a non-empty 'reason'.")
                continue
            header_sourced.append(name)
            if n_cases == 0:
                continue

        if n_cases == 0:
            no_tests += 1
            # A file with no test cases must not squat in the allow-list.
            if in_allowlist:
                failures.append(
                    f"{name}: listed in KNOWN_UNLINKED but declares 0 TEST_CASEs; "
                    "remove the stale allow-list entry."
                )
            continue

        if in_allowlist:
            # An allow-listed file that is actually anchored is a contradiction:
            # its cases now run, so the allow-list entry is stale and misleading.
            if is_anchored:
                failures.append(
                    f"{name}: listed in KNOWN_UNLINKED but IS force-link anchored "
                    f"({n_cases} cases now link); remove the stale allow-list entry."
                )
                continue
            entry = KNOWN_UNLINKED[name]
            if not entry.get("reason", "").strip() or not entry.get("issue", "").strip():
                failures.append(
                    f"{name}: KNOWN_UNLINKED entry must carry a non-empty 'reason' and 'issue'."
                )
                continue
            allowlisted.append(name)
            continue

        if not is_anchored:
            failures.append(
                f"{name}: declares {n_cases} TEST_CASE(s) but has no force_link anchor "
                f"referenced from {AGGREGATOR_HEADER.name} -> its object is silently "
                "linker-dropped and its cases NEVER run. Add a force_link anchor "
                "(mirror test_gpu_streaming.cpp) after verifying the cases pass, or add "
                "a KNOWN_UNLINKED entry with a reason + tracking issue."
            )
            continue

        anchored.append(name)

    # Fail closed on stale allow-list entries that name a nonexistent file.
    for allow_name in KNOWN_UNLINKED:
        if allow_name not in seen_names:
            failures.append(
                f"{allow_name}: KNOWN_UNLINKED names a file not present under "
                f"{TESTS_DIR.relative_to(ROOT)}; remove the stale allow-list entry."
            )
    # Same for the header-sourced registry: an entry is only valid while the file
    # exists AND still reaches cases through an include.
    for hs_name in HEADER_SOURCED_CASES:
        if hs_name not in seen_names:
            failures.append(
                f"{hs_name}: HEADER_SOURCED_CASES names a file not present under "
                f"{TESTS_DIR.relative_to(ROOT)}; remove the stale entry."
            )
        elif hs_name not in header_sourced and not any(hs_name in f for f in failures):
            failures.append(
                f"{hs_name}: HEADER_SOURCED_CASES entry is stale - the file no longer reaches "
                "any TEST_CASE through #include; remove the entry."
            )

    total_with_tests = len(anchored) + len(allowlisted) + sum(
        1 for f in failures if ": declares " in f
    )

    if failures:
        for failure in failures:
            print(f"[test-linkage-check] FAIL {failure}")
        print(
            f"[test-linkage-check] {len(failures)} linkage problem(s) across "
            f"{len(cpp_files)} tests/*.cpp "
            f"({len(anchored)} anchored, {len(allowlisted)} allow-listed, {no_tests} no-tests)."
        )
        return 1

    print(
        f"[test-linkage-check] PASSED - {len(cpp_files)} tests/*.cpp scanned: "
        f"{total_with_tests} declare TEST_CASEs "
        f"({len(anchored)} force-link anchored, {len(allowlisted)} allow-listed as "
        f"KNOWN_UNLINKED), {no_tests} declare none, "
        f"{len(header_sourced)} carry header-sourced cases (must stay unanchored)."
    )
    if allowlisted:
        for name in allowlisted:
            print(f"[test-linkage-check]   allow-listed (unlinked): {name} -> {KNOWN_UNLINKED[name]['issue']}")
    if header_sourced:
        for name in header_sourced:
            print(f"[test-linkage-check]   header-sourced cases (must stay unanchored): {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
