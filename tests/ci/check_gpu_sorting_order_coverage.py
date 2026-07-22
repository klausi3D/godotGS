#!/usr/bin/env python3
"""Guard: the required GpuSorting batch actually gates sort-ORDER correctness (#622).

## The defect this guards against

The GPU harness's `GpuSorting` batch is REQUIRED (`run_gpu_harness.py`
REQUIRED_BATCHES, promoted #744) and matches on the filter
`*Sort*][RequiresGPU]*`. Before #622 that filter matched EXACTLY ONE linked
case -- the #508 `[GPUSortPipeline][RequiresGPU]` instance-count OVERFLOW-
stickiness test -- and NO sort-ORDER oracle. The real Bitonic/Radix order
tests in `test_gpu_sorting.h` were linked and registered, but tagged
`[GaussianSplatting][RequiresGPU]` with the word "sort" only in the
*description* (AFTER `][RequiresGPU]`), so the batch glob never selected them.
They executed in no harness batch: the batch NAME said "GpuSorting" while the
batch CONTENT proved nothing about whether the GPU sort produces the right
order.

#622 retags the three order oracles with `[GpuSort]` so the batch selects them.
This guard pins that outcome so it cannot silently regress:

  1. `GpuSorting` is REQUIRED, so a failing order oracle actually reds CI.
  2. Every required sort-order ORACLE below is SELECTED by the GpuSorting
     batch filter (using doctest wildcard semantics, not fnmatch), i.e. it
     EXECUTES in that batch.
  3. Each oracle still carries `[RequiresGPU]` (so the harness runs it at all).
  4. Each oracle still contains its discriminating ORDER assertion -- the CHECK
     that compares the GPU output against an ascending / CPU-reference order.
     That assertion is what makes a deliberately-wrong sort turn the required
     batch RED; an oracle that lost it would pass vacuously (#690/#692 shape),
     so its ABSENCE fails this guard.

## Why this is red on unmodified origin/master

On base the oracle case names are `[GaussianSplatting][RequiresGPU] <suffix>`,
which the GpuSorting filter `*Sort*][RequiresGPU]*` does NOT match (the only
"sort" is in the suffix, after `][RequiresGPU]`). Check (2) therefore FAILS on
base and PASSES once the `[GpuSort]` tag is present -- the discrimination the
ticket requires. `--self-test` pins that discrimination directly.

## Fail-closed posture

A suffix that matches zero or more than one registered case FAILS (an ambiguous
or renamed oracle is not silently tolerated). A missing test file, a removed
`[RequiresGPU]` tag, a missing order assertion, or a GpuSorting batch that is
not REQUIRED all FAIL. Like its sibling guards it runs its own `--self-test`
discrimination cases so a parser regression that made the guard vacuous is
caught even when the real tree happens to be clean.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARNESS_PATH = ROOT / "tests" / "ci" / "run_gpu_harness.py"
ORACLE_SOURCE = ROOT / "modules" / "gaussian_splatting" / "tests" / "test_gpu_sorting.h"

# The batch that must gate sort-order correctness. It must exist AND be required.
BATCH_NAME = "GpuSorting"

# The sort-ORDER oracles that must run in the GpuSorting batch, keyed by the
# case-name SUFFIX (the human phrase after the tag block, unchanged by the
# [GpuSort] retag). The value is a regex that MUST appear inside that case's
# body: its discriminating order/reference assertion. If a wrong sort could not
# fail this assertion, the oracle is not an oracle -- so the guard requires it.
REQUIRED_ORDER_ORACLES: dict[str, str] = {
    # Ascending order of the sorted depth buffer (BitonicSort).
    "GPU Bitonic Sorting": r"depth_ptr\[\s*i\s*\]\s*>=\s*depth_ptr\[\s*i\s*-\s*1\s*\]",
    # Exact key order against a hand-computed reference (32-bit radix).
    "Radix sort factory honors 32-bit key layout":
        r"sorted_keys\[\s*i\s*\]\s*==\s*expected_keys\[\s*i\s*\]",
    # Exact key order against a CPU-sorted reference at every workgroup size (8-bit radix).
    "Radix sort 8-bit is correct at all workgroup sizes":
        r"sorted_keys\[\s*i\s*\]\s*==\s*expected_keys\[\s*i\s*\]",
}

_CASE_RE = re.compile(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"\s*\)\s*\{')


def _load_harness():
    spec = importlib.util.spec_from_file_location("gs_gpu_harness_order_cov", HARNESS_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {HARNESS_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def doctest_wildcmp(text: str, pattern: str, case_sensitive: bool = False) -> bool:
    """Port of doctest's `wildcmp` (thirdparty/doctest/doctest.h).

    Only `*` and `?` are special; `[` and `]` are literal. doctest's
    `case_sensitive` option defaults to false, and run_gpu_harness.py passes
    `--test-case=` without `--case-sensitive`, so lane matching is
    case-INSENSITIVE -- modelled here the same way check_test_lane_coverage.py
    does, rather than with fnmatch (which would read `[...]` as a char class).
    """
    if not case_sensitive:
        text = text.lower()
        pattern = pattern.lower()

    n_text, n_pat = len(text), len(pattern)
    ti = pi = 0
    star_pat = star_text = 0

    while ti < n_text and (pi >= n_pat or pattern[pi] != "*"):
        if pi >= n_pat:
            return False
        if pattern[pi] != text[ti] and pattern[pi] != "?":
            return False
        pi += 1
        ti += 1

    while ti < n_text:
        if pi < n_pat and pattern[pi] == "*":
            pi += 1
            if pi >= n_pat:
                return True
            star_pat = pi
            star_text = ti + 1
        elif pi < n_pat and (pattern[pi] == text[ti] or pattern[pi] == "?"):
            pi += 1
            ti += 1
        else:
            pi = star_pat
            ti = star_text
            star_text += 1

    while pi < n_pat and pattern[pi] == "*":
        pi += 1
    return pi >= n_pat


def _iter_cases(text: str) -> list[tuple[str, str]]:
    """Return (case_name, case_body) for each TEST_CASE, body via brace matching."""
    cases: list[tuple[str, str]] = []
    for match in _CASE_RE.finditer(text):
        name = match.group(1)
        # match.end() sits just past the opening `{`.
        depth = 1
        i = match.end()
        n = len(text)
        while i < n and depth > 0:
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        cases.append((name, text[match.end():i]))
    return cases


def _batch_filters_and_required(harness) -> tuple[tuple[str, ...], bool, list[str]]:
    problems: list[str] = []
    batches = {b.name: b for b in harness.BATCHES}
    spec = batches.get(BATCH_NAME)
    if spec is None:
        problems.append(
            f"run_gpu_harness.py has no batch named {BATCH_NAME!r}; the sort-order "
            "oracles have no required home."
        )
        return (), False, problems
    required = BATCH_NAME in set(harness.REQUIRED_BATCHES)
    if not required:
        problems.append(
            f"batch {BATCH_NAME!r} is not in REQUIRED_BATCHES. Sort-order oracles that "
            "only run in an advisory batch cannot fail CI, so a wrong sort would not red "
            "the gate. Keep GpuSorting required."
        )
    return tuple(spec.filters), required, problems


def analyze() -> list[str]:
    problems: list[str] = []

    if not ORACLE_SOURCE.is_file():
        return [f"missing sort-order oracle source: {ORACLE_SOURCE.relative_to(ROOT)}"]

    harness = _load_harness()
    filters, _required, batch_problems = _batch_filters_and_required(harness)
    problems.extend(batch_problems)
    if not filters:
        return problems

    text = ORACLE_SOURCE.read_text(encoding="utf-8", errors="replace")
    cases = _iter_cases(text)

    for suffix, assertion_re in REQUIRED_ORDER_ORACLES.items():
        matches = [(name, body) for (name, body) in cases if name.endswith(suffix)]
        if len(matches) != 1:
            problems.append(
                f"sort-order oracle suffix {suffix!r} matched {len(matches)} TEST_CASE(s) "
                f"in {ORACLE_SOURCE.name} (expected exactly 1). A renamed or duplicated "
                "oracle must be reconciled, not silently tolerated."
            )
            continue
        name, body = matches[0]

        if "[RequiresGPU]" not in name:
            problems.append(
                f"oracle {name!r} lost its [RequiresGPU] tag; the GPU harness would no "
                "longer run it."
            )

        if not any(doctest_wildcmp(name, pat) for pat in filters):
            problems.append(
                f"oracle {name!r} is NOT selected by the {BATCH_NAME} batch filter(s) "
                f"{list(filters)}. It therefore executes in no required batch, so sort-order "
                "correctness is not gated. Give it a Sort-bearing tag before [RequiresGPU] "
                "(e.g. [GpuSort]) so the filter matches it. This is the #622 defect."
            )

        if re.search(assertion_re, body) is None:
            problems.append(
                f"oracle {name!r} no longer contains its discriminating order assertion "
                f"(/{assertion_re}/). Without it a deliberately-wrong sort would not turn "
                "the batch RED and the oracle is vacuous (#690/#692 shape)."
            )

    return problems


def _self_test() -> list[str]:
    """Pin the discrimination the guard depends on, independent of the real tree."""
    problems: list[str] = []
    order_filter = "*Sort*][RequiresGPU]*"

    # (a) A base-style name (no Sort-bearing tag) must NOT be selected -- this is
    #     exactly the pre-#622 state the guard must fail on.
    base_name = "[GaussianSplatting][RequiresGPU] GPU Bitonic Sorting"
    if doctest_wildcmp(base_name, order_filter):
        problems.append(
            "self-test: base-tagged oracle unexpectedly matched the GpuSorting filter; "
            "the guard could not discriminate the #622 defect."
        )

    # (b) The retagged name MUST be selected.
    fixed_name = "[GaussianSplatting][GpuSort][RequiresGPU] GPU Bitonic Sorting"
    if not doctest_wildcmp(fixed_name, order_filter):
        problems.append(
            "self-test: [GpuSort]-retagged oracle failed to match the GpuSorting filter; "
            "the fix would not actually select it."
        )

    # (c) The pipeline sticky-overflow case must also still match (regression fence).
    sticky = ("[GaussianSplatting][GPUSortPipeline][RequiresGPU] Instance-count "
              "overflow_flag is sticky across a skipped async readback (C4b/G4 miss-fix)")
    if not doctest_wildcmp(sticky, order_filter):
        problems.append("self-test: the #508 sticky-overflow case no longer matches the filter.")

    # (d) The order-assertion detector must REJECT a body missing the assertion and
    #     ACCEPT one containing it, or check (4) would be vacuous.
    have = "for (uint32_t i = 1; i < count; i++) { CHECK(depth_ptr[i] >= depth_ptr[i - 1]); }"
    missing = "for (uint32_t i = 1; i < count; i++) { CHECK(err == OK); }"
    rx = REQUIRED_ORDER_ORACLES["GPU Bitonic Sorting"]
    if re.search(rx, have) is None:
        problems.append("self-test: order-assertion detector rejected a body that DOES assert order.")
    if re.search(rx, missing) is not None:
        problems.append("self-test: order-assertion detector accepted a body with NO order assertion.")

    # (e) The brace-matched case extractor must isolate bodies so one case's
    #     assertion is not credited to another.
    sample = (
        'TEST_CASE("[X] a") { CHECK(sorted_keys[i] == expected_keys[i]); }\n'
        'TEST_CASE("[X] b") { CHECK(err == OK); }\n'
    )
    extracted = dict(_iter_cases(sample))
    if "expected_keys" in extracted.get("[X] b", "") or "expected_keys" not in extracted.get("[X] a", ""):
        problems.append("self-test: case-body extractor leaked assertions across TEST_CASE boundaries.")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run the guard's own discrimination cases instead of scanning the tree.",
    )
    args = parser.parse_args()

    if args.self_test:
        problems = _self_test()
        label = "self-test"
    else:
        problems = analyze()
        label = "guard"

    if problems:
        print(f"[gpu-sorting-order-coverage] FAIL ({label}) {len(problems)} problem(s):")
        for p in problems:
            print(f"  - {p}")
        return 1

    if args.self_test:
        print("[gpu-sorting-order-coverage] PASSED self-test discrimination cases.")
    else:
        oracle_names = ", ".join(sorted(REQUIRED_ORDER_ORACLES))
        print(
            f"[gpu-sorting-order-coverage] PASSED - the required {BATCH_NAME} batch selects "
            f"{len(REQUIRED_ORDER_ORACLES)} sort-order oracle(s), each carrying [RequiresGPU] "
            f"and its discriminating order assertion: {oracle_names}."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
