#!/usr/bin/env python3
"""Guard: in the RadixSort initialization path, the error a failure site returns must match
what that failure IS -- a GPU program that would not build returns `ERR_COMPILATION_FAILED`,
a resource acquisition that failed returns `ERR_CANT_CREATE`.

Why this exists (#586 round-7)
------------------------------
`TileGlobalSortResources::ensure_resources()` decides whether a failed sorter build is worth
retrying. Before round 7 it could not decide anything: `GPUSorterFactory::create_sorter()`
discarded `RadixSort::initialize()`'s `Error` and returned a bare invalid `Ref`, so an
out-of-VRAM histogram buffer and a scatter shader the driver refuses to compile were the same
observation, and both were classified `CREATION_FAILED` -- transient. The renderer therefore
retried a permanently failing shader compile forever at the saturated backoff, while rejecting
every translucent frame anyway.

Round 7 preserved the error and split the classification
(`GaussianSplatting::classify_sorter_creation_error`, sort_fallback_policy.h). That fix rests
entirely on a CORRESPONDENCE:

    the deterministic sites return ERR_COMPILATION_FAILED, and nothing else does.

which was, until this guard, enforced by a comment. One `return ERR_CANT_CREATE;` typed at a
shader-creation site silently reverts the round-7 behaviour with no test failing -- the
classifier is still correct, the policy predicates are still correct, and a deterministic
failure is transient again. That is the same shape as every earlier round in this series: an
invariant held in place by a hand-maintained list rather than derived from the code.

What is derived
---------------
Everything except the two expected codes. The guard reads the real function bodies, finds
which local each GPU object came FROM, and requires the error returned on that object's
validity check to match its PRODUCER:

    create_compute_shader_from_spirv(...)        -> a GPU program  -> ERR_COMPILATION_FAILED
    <device>->compute_pipeline_create(...)       -> a GPU program  -> ERR_COMPILATION_FAILED
    <device>->storage_buffer_create(...)         -> an allocation  -> ERR_CANT_CREATE

So a NEW shader or pipeline added to the sort path is covered the moment it is written; it
does not have to be added to a list here.

Scope
-----
`RadixSort::create_variant()` and `RadixSort::initialize()` only, and that scope is itself a
derived fact rather than a preference: the classified call site passes
`GPUSorterFactory::ALGORITHM_RADIX` explicitly, so the AUTO branch never runs and the only
`initialize()` that can produce the error the classifier sees is `RadixSort`'s. BitonicSort
and OneSweepSort live in the same file and are deliberately NOT in scope -- retagging their
errors would change behaviour on the instance-sorting path, which this round does not touch.

Fail-closed behaviour
---------------------
  * A tracked object with no `is_valid()` failure branch is a FAILURE, not a pass: an
    unchecked shader/pipeline/buffer is a worse defect than a mislabelled one.
  * A failure branch whose `return` this parser cannot read is a FAILURE.
  * The site counts are PINNED. Deleting sites until the guard has nothing left to check is
    the way a derived guard gets hollowed out while still printing PASSED, so a change in the
    number of covered sites must be a visible, reviewed edit to a constant here.

Do not silence this guard by narrowing the scope or lowering the pins to make a change pass.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
SORTER_SOURCE = ROOT / "modules" / "gaussian_splatting" / "renderer" / "gpu_sorter.cpp"
POLICY_HEADER = ROOT / "modules" / "gaussian_splatting" / "renderer" / "sort_fallback_policy.h"

# The two functions whose Error reaches classify_sorter_creation_error(). See "Scope" above.
SCOPED_FUNCTIONS = (
    "Error RadixSort::create_variant(",
    "Error RadixSort::initialize(",
)

# Producer call -> what kind of failure a bad result from it IS.
PROGRAM = "gpu-program"
ALLOCATION = "allocation"
_PRODUCERS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"([\w.]+)\s*=\s*create_compute_shader_from_spirv\s*\("), PROGRAM),
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->compute_pipeline_create\s*\("), PROGRAM),
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->storage_buffer_create\s*\("), ALLOCATION),
)

EXPECTED_ERROR = {
    PROGRAM: "ERR_COMPILATION_FAILED",
    ALLOCATION: "ERR_CANT_CREATE",
}

WHY = {
    PROGRAM: (
        "compiling generated GLSL and building a pipeline from the resulting SPIR-V are pure "
        "functions of (source, device, driver): they do not depend on momentary VRAM pressure, "
        "so they fail identically on every attempt. Returning the allocation code makes "
        "classify_sorter_creation_error() call the failure TRANSIENT, and the renderer "
        "recompiles the same failing shader forever at the saturated backoff (#586 round-7)"
    ),
    ALLOCATION: (
        "a failed buffer acquisition can succeed on a later attempt once pressure lifts, so it "
        "must stay classified TRANSIENT. Returning the deterministic code latches the sorter "
        "off until renderer teardown over one momentary VRAM blip, which is the permanent "
        "black screen #586 round-1 removed"
    ),
}

# Pins. Bump these only together with a real change to the number of covered sites.
EXPECTED_PROGRAM_SITES = 10
EXPECTED_ALLOCATION_SITES = 3

_VALIDITY_CHECK_RE = re.compile(r"!\s*([\w.]+)\s*\.is_valid\s*\(\s*\)")
_RETURN_ERROR_RE = re.compile(r"\breturn\s+(ERR_\w+)\s*;")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


class Site(NamedTuple):
    """One producer -> validity-check -> return triple inside a scoped function."""

    function: str
    variable: str
    kind: str
    returned: str


def _strip_cpp_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return _LINE_COMMENT_RE.sub("", text)


def _function_body(text: str, anchor: str) -> str | None:
    """The `{...}` body following `anchor`, comments stripped so braces inside one cannot
    mis-bound it. Returns None if the anchor or its body cannot be found -- which the caller
    treats as a failure, never as "nothing to check"."""
    stripped = _strip_cpp_comments(text)
    start = stripped.find(anchor)
    if start == -1:
        return None
    brace = stripped.find("{", start)
    if brace == -1:
        return None
    depth = 0
    for i in range(brace, len(stripped)):
        char = stripped[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return stripped[brace + 1 : i]
    return None


def _collect_producers(body: str) -> dict[str, str]:
    """Map object name -> producer kind, for every GPU object built in this function."""
    producers: dict[str, str] = {}
    for pattern, kind in _PRODUCERS:
        for match in pattern.finditer(body):
            producers[match.group(1)] = kind
    return producers


def _collect_sites(function: str, body: str, producers: dict[str, str]) -> tuple[list[Site], list[str]]:
    """For each `!<tracked>.is_valid()` check, the FIRST `return ERR_...;` after it.

    Taking the first return after the check is what ties the code to that specific failure
    branch: the checks in this path all have the shape
    `if (!x.is_valid()) { <cleanup...>; return ERR_...; }`."""
    sites: list[Site] = []
    problems: list[str] = []
    seen: set[str] = set()
    for check in _VALIDITY_CHECK_RE.finditer(body):
        variable = check.group(1)
        kind = producers.get(variable)
        if kind is None:
            # Not something this guard classifies (e.g. a caller-supplied RID). Ignored on
            # purpose: guessing at an object whose origin is unknown is how a check starts
            # asserting the wrong thing.
            continue
        seen.add(variable)
        returned = _RETURN_ERROR_RE.search(body, check.end())
        if returned is None:
            problems.append(
                f"{function}: `{variable}` is checked with is_valid() but this guard cannot find "
                f"the `return ERR_...;` that failure takes, so it cannot verify the error class. "
                f"Keep the failure branch in the `if (!x.is_valid()) {{ ...; return ERR_...; }}` "
                f"shape, or extend tests/ci/check_sorter_error_class_parity.py."
            )
            continue
        sites.append(Site(function, variable, kind, returned.group(1)))

    for variable, kind in producers.items():
        if variable not in seen:
            problems.append(
                f"{function}: `{variable}` is built by a {kind} call but its result is never "
                f"checked with is_valid(). An unchecked GPU object is a worse defect than a "
                f"mislabelled error -- check it and return {EXPECTED_ERROR[kind]} on failure."
            )
    return sites, problems


def main() -> int:  # noqa: PLR0911, PLR0912 -- linear fail-closed checks
    prefix = "[sorter-error-class-parity]"

    for path in (SORTER_SOURCE, POLICY_HEADER):
        if not path.is_file():
            print(f"{prefix} FAIL missing {path.relative_to(ROOT)}")
            return 1

    # The guard is only meaningful while the consumer of these codes still exists. If the
    # classifier is gone or renamed, the contract this enforces has changed and a human must
    # look, rather than the guard quietly passing over a dead invariant.
    policy_text = POLICY_HEADER.read_text(encoding="utf-8")
    for expected_error in sorted(set(EXPECTED_ERROR.values())):
        if expected_error not in policy_text:
            print(
                f"{prefix} FAIL `{expected_error}` no longer appears in "
                f"{POLICY_HEADER.name}, so classify_sorter_creation_error() cannot be mapping it. "
                f"The error-class contract this guard enforces has changed; update both sides "
                f"together -- do not delete the guard."
            )
            return 1

    source_text = SORTER_SOURCE.read_text(encoding="utf-8")
    sites: list[Site] = []
    problems: list[str] = []

    for anchor in SCOPED_FUNCTIONS:
        body = _function_body(source_text, anchor)
        if body is None:
            print(
                f"{prefix} FAIL could not parse `{anchor}` in {SORTER_SOURCE.name}. If it was "
                f"renamed or removed, update SCOPED_FUNCTIONS -- do not drop it; the retry "
                f"policy for a failed sorter build is decided by the errors it returns."
            )
            return 1
        producers = _collect_producers(body)
        if not producers:
            print(
                f"{prefix} FAIL parsed no shader/pipeline/buffer creation in `{anchor}`. A "
                f"sorter init path that builds nothing cannot be right, so this is a parser "
                f"failure, not a clean result."
            )
            return 1
        function_sites, function_problems = _collect_sites(anchor, body, producers)
        sites.extend(function_sites)
        problems.extend(function_problems)

    for site in sites:
        expected = EXPECTED_ERROR[site.kind]
        if site.returned != expected:
            problems.append(
                f"{site.function}: `{site.variable}` comes from a {site.kind} call, so its "
                f"failure must return {expected}, but it returns {site.returned}. Why it "
                f"matters: {WHY[site.kind]}."
            )

    program_sites = sum(1 for site in sites if site.kind == PROGRAM)
    allocation_sites = sum(1 for site in sites if site.kind == ALLOCATION)
    if program_sites != EXPECTED_PROGRAM_SITES or allocation_sites != EXPECTED_ALLOCATION_SITES:
        problems.append(
            f"covered site count changed: {program_sites} GPU-program site(s) and "
            f"{allocation_sites} allocation site(s), pinned at {EXPECTED_PROGRAM_SITES} and "
            f"{EXPECTED_ALLOCATION_SITES}. A derived guard with nothing left to check still "
            f"prints PASSED, so this has to be a deliberate edit: update the pins in the same "
            f"diff that adds or removes a site."
        )

    if problems:
        for problem in problems:
            print(f"{prefix} FAIL {problem}")
        print(f"{prefix} {len(problems)} problem(s); {len(sites)} site(s) inspected.")
        return 1

    print(
        f"{prefix} PASSED - {program_sites} GPU-program failure site(s) return "
        f"{EXPECTED_ERROR[PROGRAM]} and {allocation_sites} allocation failure site(s) return "
        f"{EXPECTED_ERROR[ALLOCATION]}, across {len(SCOPED_FUNCTIONS)} scoped function(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
