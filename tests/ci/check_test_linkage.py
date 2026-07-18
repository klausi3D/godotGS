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

## Fail-closed

A missing module tests directory, a missing/renamed aggregator header, or a stale
`KNOWN_UNLINKED` entry (naming a file that no longer exists, that has no tests, or
that is now anchored) all FAIL. The allow-list is kept honest: an entry is only
valid while the file it names is genuinely test-carrying AND genuinely unlinked.
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
            "Structurally broken: the 8 [RequiresGPU] sort cases submit on a "
            "device that does not match the sorter's submission device, so "
            "linking them would turn the GPU harness red. Left unlinked until "
            "the submission-device mismatch is fixed."
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

# doctest case-declaring macros. TEST_SUITE is a grouping wrapper (not itself a
# case) and is tracked only for reporting.
_CASE_RE = re.compile(r"\bTEST_CASE(?:_TEMPLATE)?\s*\(")
_SUITE_RE = re.compile(r"\bTEST_SUITE\s*\(")
# A force_link anchor definition: `extern "C" int <sym>(...) {` in the .cpp.
_FORCE_LINK_DEF_RE = re.compile(r'extern\s+"C"\s+int\s+(\w+)\s*\([^)]*\)\s*\{')


def _strip_comments(text: str) -> str:
    """Remove // line comments and /* */ block comments so a TEST_CASE mentioned
    in a comment is not miscounted. String literals are left intact; a stray
    macro-looking token inside a string is vanishingly unlikely in these files
    and would only ever cause a (safe) over-count that a real anchor satisfies."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def _count_cases(text: str) -> tuple[int, int]:
    return len(_CASE_RE.findall(text)), len(_SUITE_RE.findall(text))


def _force_link_symbols(text: str) -> list[str]:
    return _FORCE_LINK_DEF_RE.findall(text)


def _is_referenced(symbol: str, header_text: str) -> bool:
    # The load-bearing line is the initializer call `= <sym>();`. A bare extern
    # declaration without a call would NOT force the link, so a call is required.
    return re.search(r"=\s*" + re.escape(symbol) + r"\s*\(\s*\)", header_text) is not None


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

    header_text = AGGREGATOR_HEADER.read_text(encoding="utf-8")

    cpp_files = sorted(TESTS_DIR.glob("*.cpp"))
    if not cpp_files:
        print(f"[test-linkage-check] FAIL found no tests/*.cpp under {TESTS_DIR.relative_to(ROOT)}")
        return 1

    failures: list[str] = []
    anchored: list[str] = []
    allowlisted: list[str] = []
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
        f"KNOWN_UNLINKED), {no_tests} declare none."
    )
    if allowlisted:
        for name in allowlisted:
            print(f"[test-linkage-check]   allow-listed (unlinked): {name} -> {KNOWN_UNLINKED[name]['issue']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
