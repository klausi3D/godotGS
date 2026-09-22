#!/usr/bin/env python3
"""Guard (#54): the overlap-record drop proof cannot vanish from the GPU harness.

## The defect this guards against

`test_overflow_drop_telemetry`
(`modules/gaussian_splatting/tests/tile_renderer_regression_test.cpp`) is the module's ONLY
on-GPU proof that a real binning overlap-record drop raises the always-on C4b drop telemetry
-- and that it does NOT fire without a drop. It reaches the harness through exactly one path:
the case name

    [GaussianSplatting][TileRenderer][RequiresGPU] Overflow-record drop raises the C4b
    telemetry counter

is matched by the `TileRenderer` batch filter `*TileRenderer*][RequiresGPU]*` in
`tests/ci/run_gpu_harness.py`.

Nothing pinned that path. `TileRenderer` is an ADVISORY batch (it is not in
`REQUIRED_BATCHES`, because of the unrelated #643 exclusion), and for advisory batches the
harness deliberately treats `0 tests matched` as SUCCESS. So a retag of the case, a narrowing
of the batch filter, a new `excludes` entry, or the batch's removal would leave the gate green
with the overflow proof silently gone -- the exact "a check invoked by no lane" shape catalogued
in `docs/governance/evidence-integrity.md`.

## What is pinned

  1. The batch that is this case's home EXISTS in `run_gpu_harness.py`.
  2. The case exists exactly ONCE, found by its human-readable name suffix (so the guard
     survives a tag change it is meant to catch, rather than failing to find the case at all).
  3. The case still carries `[RequiresGPU]`, without which the harness never runs it.
  4. The case is SELECTED by that batch: matched by one of its filters AND not subtracted by
     any of its `excludes`. Selection is evaluated with doctest's own `wildcmp` semantics
     (case-insensitive, `[`/`]` literal), not `fnmatch`, which would read `[...]` as a
     character class.
  5. The case's body actually CALLS the drop-telemetry method -- a case that kept the name but
     stopped invoking the real test would be hollow.
  6. The method body still contains the three things that make it discriminate:
     the forced-low overlap budget that provokes the drop, the CONTROL assertion (a
     non-overflowing scene must NOT move the counter), and the OVERFLOW assertion (a dense
     overflowing scene MUST move it). A body that lost the control assertion would pass on a
     counter that ticks unconditionally; one that lost the overflow assertion proves nothing.

## Residual this guard does NOT close

`TileRenderer` is advisory. This guard makes the proof impossible to lose SILENTLY -- a
de-selected or hollowed case now reds the guard lane, which is required. It does not make a
FAILING case red the GPU gate; that needs the batch promoted to `REQUIRED_BATCHES`, which ADR
`docs/architecture/adr-phase1-guard-hardening.md` §5.5 gates on promotion evidence recorded on
the runner, and which the #643 exclusion currently blocks for this batch as a whole. Stated
here rather than left implicit.

## Fail-closed posture

A missing source file, a suffix matching zero or more than one case, a missing `[RequiresGPU]`
tag, a case selected by no batch filter, a case swallowed by an exclude, a case that does not
call the method, a missing method, or any missing discriminating assertion all FAIL. The guard
also runs its own `--self-test` discrimination cases, so a parser regression that made it
vacuous is caught even when the real tree is clean.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARNESS_PATH = ROOT / "tests" / "ci" / "run_gpu_harness.py"
PROOF_SOURCE = (
    ROOT / "modules" / "gaussian_splatting" / "tests" / "tile_renderer_regression_test.cpp"
)

# The batch that is this case's home. Pinned by name: if the case migrates to another batch
# that is a deliberate decision to record here, not a drift to tolerate silently.
BATCH_NAME = "TileRenderer"

# The case is located by its human-readable suffix (everything after the tag block), because
# the TAG is exactly what may drift and what this guard must catch. Keying on the full name
# would make a retag look like a deleted case rather than a de-selected one.
CASE_SUFFIX = "Overflow-record drop raises the C4b telemetry counter"

# The C++ method the case must call, and the discriminating content that method must keep.
PROOF_METHOD = "test_overflow_drop_telemetry"
REQUIRED_METHOD_CONTENT: dict[str, str] = {
    # The forced-low global overlap budget is what provokes a real drop. Without it the dense
    # scene fits and the case proves nothing (it would pass on a renderer that never drops).
    "forced-low overlap-record budget": r"MAX_OVERLAP_RECORDS_PATH",
    # CONTROL phase: a non-overflowing scene must leave the counter alone. This is what makes
    # the proof discriminate rather than pass on a counter that ticks every frame.
    "control-phase assertion (counter must NOT move without a drop)":
        r"control_after\s*!=\s*control_baseline",
    # OVERFLOW phase: the dense scene must move the counter above its baseline.
    "overflow-phase assertion (counter MUST move on a drop)":
        r"drop_events\s*<=\s*baseline_drop_events",
}

_CASE_RE = re.compile(r'TEST_CASE\(\s*"((?:[^"\\]|\\.)*)"\s*\)\s*\{')
_METHOD_RE_TEMPLATE = r"TileRendererRegressionTest::TestResult\s+TileRendererRegressionTest::{name}\s*\("


def _load_harness():
    spec = importlib.util.spec_from_file_location("gs_gpu_harness_overflow_cov", HARNESS_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {HARNESS_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def doctest_wildcmp(text: str, pattern: str, case_sensitive: bool = False) -> bool:
    """Port of doctest's `wildcmp` (thirdparty/doctest/doctest.h).

    Only `*` and `?` are special; `[` and `]` are literal. doctest's `case_sensitive` option
    defaults to false and run_gpu_harness.py passes `--test-case=` without `--case-sensitive`,
    so lane matching is case-INSENSITIVE. Kept byte-identical in behaviour to the port in
    check_gpu_sorting_order_coverage.py rather than using fnmatch, which would read `[...]`
    as a character class and silently mis-model every tag.
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
        cases.append((match.group(1), text[match.end() : i]))
    return cases


def _extract_method_body(text: str, method_name: str) -> str | None:
    """Return the brace-matched body of a TileRendererRegressionTest method DEFINITION, or
    None when the definition is absent (the declaration alone is not enough)."""
    match = re.search(_METHOD_RE_TEMPLATE.format(name=re.escape(method_name)), text)
    if match is None:
        return None
    open_brace = text.find("{", match.end())
    if open_brace == -1:
        return None
    depth = 1
    i = open_brace + 1
    n = len(text)
    while i < n and depth > 0:
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return text[open_brace + 1 : i]


def _selection(harness, case_name: str) -> tuple[bool, list[str], list[str]]:
    """Return (batch_exists, matching_filters, swallowing_excludes) for BATCH_NAME."""
    spec = next((b for b in harness.BATCHES if b.name == BATCH_NAME), None)
    if spec is None:
        return False, [], []
    matched = [f for f in spec.filters if doctest_wildcmp(case_name, f)]
    swallowed = [e for e in spec.excludes if doctest_wildcmp(case_name, e)]
    return True, matched, swallowed


def analyze() -> list[str]:
    problems: list[str] = []

    if not PROOF_SOURCE.is_file():
        return [f"missing overflow-drop proof source: {PROOF_SOURCE.relative_to(ROOT)}"]

    harness = _load_harness()
    text = PROOF_SOURCE.read_text(encoding="utf-8", errors="replace")
    cases = _iter_cases(text)

    matches = [(name, body) for (name, body) in cases if name.endswith(CASE_SUFFIX)]
    if len(matches) != 1:
        return problems + [
            f"case suffix {CASE_SUFFIX!r} matched {len(matches)} TEST_CASE(s) in "
            f"{PROOF_SOURCE.name} (expected exactly 1). A renamed, deleted or duplicated "
            "overflow-drop proof must be reconciled, not silently tolerated."
        ]
    case_name, case_body = matches[0]

    if "[RequiresGPU]" not in case_name:
        problems.append(
            f"case {case_name!r} lost its [RequiresGPU] tag; the GPU harness would never run it."
        )

    batch_exists, matched_filters, swallowing_excludes = _selection(harness, case_name)
    if not batch_exists:
        problems.append(
            f"run_gpu_harness.py has no batch named {BATCH_NAME!r}. The overflow-drop proof has "
            "no home batch, and an advisory batch that matches nothing is reported as SUCCESS, "
            "so the proof would be gone with the gate still green."
        )
    else:
        if not matched_filters:
            problems.append(
                f"case {case_name!r} is NOT selected by any {BATCH_NAME} batch filter. It "
                "therefore executes in NO harness batch, and because that batch is advisory the "
                "harness counts `0 tests matched` as success -- the overflow-drop proof would "
                "vanish with nothing going red. Restore a tag the filter matches, or move the "
                "case to a batch and update BATCH_NAME here."
            )
        if swallowing_excludes:
            problems.append(
                f"case {case_name!r} is subtracted from the {BATCH_NAME} batch by "
                f"exclude(s) {swallowing_excludes}. An exclude may only carve out a case with a "
                "declared waiver in docs/reference/renderer_release_gate_manifest.json; it must "
                "not be used to drop the module's only on-GPU overlap-record drop proof."
            )

    if PROOF_METHOD not in case_body:
        problems.append(
            f"case {case_name!r} no longer calls {PROOF_METHOD}(). The case would run, pass, and "
            "prove nothing about overlap-record drops."
        )

    method_body = _extract_method_body(text, PROOF_METHOD)
    if method_body is None:
        problems.append(
            f"no definition of TileRendererRegressionTest::{PROOF_METHOD} found in "
            f"{PROOF_SOURCE.name}; the proof the case names does not exist."
        )
    else:
        for label, pattern in REQUIRED_METHOD_CONTENT.items():
            if re.search(pattern, method_body) is None:
                problems.append(
                    f"{PROOF_METHOD} no longer contains its {label} (/{pattern}/). Without it the "
                    "case cannot discriminate a real overlap-record drop from no drop at all, and "
                    "passes vacuously."
                )

    return problems


def _self_test() -> list[str]:
    """Pin the discrimination the guard depends on, independent of the real tree."""
    problems: list[str] = []
    tile_filter = "*TileRenderer*][RequiresGPU]*"
    real_name = (
        "[GaussianSplatting][TileRenderer][RequiresGPU] "
        "Overflow-record drop raises the C4b telemetry counter"
    )

    # (a) The shipped name must be selected by the TileRenderer filter.
    if not doctest_wildcmp(real_name, tile_filter):
        problems.append(
            "self-test: the shipped overflow-drop case name does not match the TileRenderer "
            "filter; the guard's selection model is wrong."
        )

    # (b) A retag away from [TileRenderer] must NOT be selected -- this is the wiring defect the
    #     guard exists to catch, and if the model could not see it the guard is decorative.
    retagged = (
        "[GaussianSplatting][TileRasterizer][RequiresGPU] "
        "Overflow-record drop raises the C4b telemetry counter"
    )
    if doctest_wildcmp(retagged, tile_filter):
        problems.append(
            "self-test: a case retagged away from [TileRenderer] still matched the TileRenderer "
            "filter; the guard could not discriminate a de-selecting retag."
        )

    # (c) Dropping [RequiresGPU] must also de-select it.
    no_gpu = "[GaussianSplatting][TileRenderer] Overflow-record drop raises the C4b telemetry counter"
    if doctest_wildcmp(no_gpu, tile_filter):
        problems.append(
            "self-test: a case without [RequiresGPU] still matched the TileRenderer filter."
        )

    # (d) The #643 exclude must NOT swallow this case (a false positive here would make the
    #     guard red on a clean tree), while an exclude aimed at it MUST be seen.
    waiver = "*Output format coercion keeps deterministic defaults*"
    if doctest_wildcmp(real_name, waiver):
        problems.append("self-test: the #643 waiver exclude wrongly matched the overflow-drop case.")
    if not doctest_wildcmp(real_name, "*Overflow-record drop*"):
        problems.append(
            "self-test: an exclude aimed directly at the overflow-drop case was not detected; "
            "check (4)'s exclude arm is blind."
        )

    # (e) Every discriminating-content detector must REJECT a body missing it and ACCEPT one
    #     containing it, or check (6) is vacuous.
    have = (
        "ps->set_setting(GPUSortingConfig::MAX_OVERLAP_RECORDS_PATH, 100000);\n"
        "if (control_after != control_baseline) { return r; }\n"
        "if (drop_events <= baseline_drop_events) { return r; }\n"
    )
    missing = "Error err = tile_renderer->initialize(p_rd, size, tile);\nreturn r;\n"
    for label, pattern in REQUIRED_METHOD_CONTENT.items():
        if re.search(pattern, have) is None:
            problems.append(f"self-test: detector for {label!r} rejected a body that HAS it.")
        if re.search(pattern, missing) is not None:
            problems.append(f"self-test: detector for {label!r} accepted a body that LACKS it.")

    # (f) The brace-matched extractors must isolate bodies so one case's or method's content is
    #     not credited to another.
    sample = (
        'TEST_CASE("[X] a") { regression_test->test_overflow_drop_telemetry(rd); }\n'
        'TEST_CASE("[X] b") { CHECK(true); }\n'
    )
    extracted = dict(_iter_cases(sample))
    if (
        "test_overflow_drop_telemetry" in extracted.get("[X] b", "")
        or "test_overflow_drop_telemetry" not in extracted.get("[X] a", "")
    ):
        problems.append("self-test: case-body extractor leaked content across TEST_CASE boundaries.")

    method_sample = (
        "TileRendererRegressionTest::TestResult TileRendererRegressionTest::test_overflow_drop_telemetry"
        "(RenderingDevice *p_rd) {\n    if (a) { b(); }\n    MARKER_INSIDE;\n}\n"
        "TileRendererRegressionTest::TestResult TileRendererRegressionTest::other(RenderingDevice *p) {\n"
        "    MARKER_OUTSIDE;\n}\n"
    )
    body = _extract_method_body(method_sample, "test_overflow_drop_telemetry")
    if body is None or "MARKER_INSIDE" not in body or "MARKER_OUTSIDE" in body:
        problems.append(
            "self-test: method-body extractor did not isolate the method (nested braces or the "
            "following definition leaked in)."
        )
    if _extract_method_body("int unrelated() { return 0; }", "test_overflow_drop_telemetry") is not None:
        problems.append("self-test: method-body extractor invented a body for an absent method.")

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
        print(f"[overflow-drop-coverage] FAIL ({label}) {len(problems)} problem(s):")
        for problem in problems:
            print(f"  - {problem}")
        return 1

    if args.self_test:
        print("[overflow-drop-coverage] PASSED self-test discrimination cases.")
    else:
        print(
            f"[overflow-drop-coverage] PASSED - the {BATCH_NAME} batch selects the overflow-drop "
            f"proof ({CASE_SUFFIX!r}), it carries [RequiresGPU], calls {PROOF_METHOD}(), and that "
            f"method still holds all {len(REQUIRED_METHOD_CONTENT)} discriminating elements "
            "(forced-low budget, control assertion, overflow assertion)."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
