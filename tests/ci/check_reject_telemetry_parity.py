#!/usr/bin/env python3
"""Guard: every `RasterPerformance` field has an explicit disposition for a REJECTED
frame -- either its backing TileRenderer state is cleared by
`TileRenderer::RenderFrameExecutor::_reject_frame()`, or it is listed below as
deliberately surviving a reject (with a reason).

Why this exists (#586 round-4)
------------------------------
`_reject_frame()` is the one place a frame stops being published. Round 3 centralized
every rejection there and argued the per-frame telemetry invalidation was therefore
"complete by construction". Centralizing the EXITS did make the *accounting* complete
-- a new failure exit inherits the counter automatically -- but the set of *fields*
that get invalidated was still a hand-written list of assignments inside that function,
and a hand-written list is not complete by construction. Round 4 review found two more
CPU-measured pairs that were missing from it:

    last_overlap_sort_cpu_dispatch_ms / overlap_sort_cpu_dispatch_valid
    last_prefix_cpu_sync_fallback_ms  / prefix_cpu_sync_fallback_valid

Both are reset by their own producing stage rather than at the top of `run()`, so a
frame rejected BEFORE that stage (the pre-binning global-sort resource check is exactly
such an exit) kept publishing the previous SUCCESSFUL frame's CPU cost. Round 3 had
already been the *second* round to find a missing field. A third would have been the
next review round; this guard is what makes it a CI failure instead.

The rule being enforced
-----------------------
A `RasterPerformance` field is *per-frame CPU-measured telemetry* unless it is one of:

  * a GPU-timestamp value, which legitimately resolves several frames late (see the
    comment on `_reset_timestamp_tracking()`: clearing every frame makes telemetry flap
    to zero and hides otherwise-valid samples). Those are sticky ON PURPOSE, bounded by
    `GPU_PASS_STALE_FRAMES`, and self-describing -- `timing_frame_serial` and
    `timing_frames_behind` are published alongside them so a consumer knows how old the
    number is.
  * a cumulative/lifetime counter, which must survive per-frame resets by definition.

Everything else is measured inline on the CPU during the frame, has no "it is from an
earlier frame" alibi, and must therefore read invalid/zero after a reject.

How completeness is established (this is the point)
---------------------------------------------------
The COVERED side is *derived*, not listed. The guard walks the real chain:

    struct RasterPerformance          (interfaces/rasterizer_interfaces.h)
      -> TileRasterizer::get_performance()  -- which getter fills each field
      -> TileRenderer's inline getter       -- which member backs that getter
      -> _reject_frame() body               -- which members it assigns

so "field X is invalidated on reject" is read out of the source every run and cannot
drift. Only the EXEMPTIONS are hand-written, and they are pinned by a count and
reason-validated, exactly like `check_metric_reset_parity.py`. That asymmetry is
deliberate: a newly added field defaults to "must be invalidated", so forgetting to
classify it FAILS rather than silently passing.

A field that is neither reject-covered nor exempt fails the guard: clear its backing
member in `_reject_frame()` (preferred), or -- if it must survive a reject -- add it to
`SURVIVES_REJECT_FIELDS` with a justification and bump the pin. Do not silence this
guard by deleting a field from the check, and do not weaken it.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
PERF_STRUCT_HEADER = MODULE / "interfaces" / "rasterizer_interfaces.h"
POPULATOR_SOURCE = MODULE / "interfaces" / "tile_rasterizer.cpp"
RENDERER_HEADER = MODULE / "renderer" / "tile_renderer.h"
RENDERER_SOURCE = MODULE / "renderer" / "tile_renderer.cpp"

STRUCT_NAME = "RasterPerformance"
POPULATOR_ANCHOR = f"{STRUCT_NAME} TileRasterizer::get_performance() const"
REJECT_ANCHOR = "RID _reject_frame(GaussianSplatting::FrameRejectStage p_stage)"

# A plain-old-data member: "<type...> <name> = <default>;" or "<type...> <name>;".
_FIELD_RE = re.compile(r"^\s*[\w:]+(?:\s*[*&])?\s+(\w+)\s*(?:=|;)")
# `perf.<field> = tile_renderer-><getter>();` inside get_performance().
_POPULATE_RE = re.compile(r"^\s*perf\.(\w+)\s*=\s*tile_renderer->(\w+)\s*\(\s*\)\s*;")
# An inline getter on TileRenderer: `<type> <name>() const { return <group>.<member>; }`.
# Restricted to the single-expression `return a.b;` shape on purpose -- a getter that
# computes or branches is NOT resolvable to one backing member, and is reported rather
# than guessed at (see _resolve_backing below).
_GETTER_RE = re.compile(
    r"^\s*[\w:]+(?:\s*[*&])?\s+(\w+)\s*\(\s*\)\s*const\s*\{\s*return\s+(\w+)\.(\w+)\s*;\s*\}"
)
# A member assignment inside _reject_frame(): `renderer.<group>.<member> = ...;`.
_REJECT_ASSIGN_RE = re.compile(r"^\s*renderer\.(\w+)\.(\w+)\s*=[^=]")
_PP_DIRECTIVE_RE = re.compile(r"^\s*#")
_ACCESS_SPECIFIER_RE = re.compile(r"^(?:public|private|protected)\s*:\s*$")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")

# Growing this allow-list is how the guard would be quietly hollowed out: one appended
# line moves a field from "must be invalidated on reject" to "exempt", and nothing else
# in the diff has to change. The pin makes that impossible to do invisibly -- an
# addition must also bump this number in the same diff, so the exemption shows up as a
# deliberate edit to a reviewed constant. Bump it only together with a real
# justification; never bump it to make the guard pass.
EXPECTED_SURVIVES_REJECT_COUNT = 19
MINIMUM_REASON_LENGTH = 12
_PLACEHOLDER_REASONS = {"todo", "tbd", "n/a", "na", "none", "fixme", "xxx", "?"}

# Fields that deliberately keep their value through a rejected frame, with the reason.
# Two categories only -- see the module docstring for why these two and nothing else.
_GPU_STICKY = (
    "async GPU timestamp; resolves frames late by design, kept by "
    "_reset_timestamp_tracking, aged out after GPU_PASS_STALE_FRAMES, and its age is "
    "published via timing_frame_serial/timing_frames_behind"
)
SURVIVES_REJECT_FIELDS: dict[str, str] = {
    # --- GPU timestamp values: sticky on purpose, and self-describing ---
    "overlap_count_gpu_ms": _GPU_STICKY,
    "overlap_count_gpu_valid": _GPU_STICKY,
    "binning_gpu_ms": _GPU_STICKY,
    "overlap_emit_gpu_ms": _GPU_STICKY,
    "overlap_emit_gpu_valid": _GPU_STICKY,
    "overlap_sort_gpu_ms": _GPU_STICKY,
    "overlap_sort_gpu_valid": _GPU_STICKY,
    "raster_gpu_ms": _GPU_STICKY,
    "raster_gpu_valid": _GPU_STICKY,
    "prefix_gpu_ms": _GPU_STICKY,
    "prefix_gpu_valid": _GPU_STICKY,
    "resolve_gpu_ms": _GPU_STICKY,
    "resolve_gpu_valid": _GPU_STICKY,
    "frame_gpu_ms": _GPU_STICKY,
    "frame_gpu_valid": _GPU_STICKY,
    # --- Metadata describing how stale the GPU values above are ---
    "timing_frame_serial": (
        "identifies WHICH frame the sticky GPU timings belong to; zeroing it on a reject "
        "would destroy the only signal that says they are not this frame's"
    ),
    "timing_frames_behind": (
        "how many frames behind the sticky GPU timings are; same rationale as "
        "timing_frame_serial, and a reject does not make them fresher"
    ),
    # --- Cumulative / lifetime counters ---
    "sort_sync_fallback_count": (
        "cumulative lifetime counter of sync sort fallbacks; a reject must not erase "
        "fallbacks that really happened on earlier frames"
    ),
    "raster_pipeline_reformats": (
        "monotonic lifetime counter; clearing it on a reject would mask earlier reformats"
    ),
}


def _strip_cpp_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return _LINE_COMMENT_RE.sub("", text)


def _brace_body(text: str, anchor: str) -> str | None:
    """Return the `{...}` body (contents only) following `anchor`, comments stripped
    first so braces inside comments cannot mis-bound it."""
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


def _has_top_level_comma(line: str) -> bool:
    depth = 0
    for char in line:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth = max(0, depth - 1)
        elif char == "," and depth == 0:
            return True
    return False


def _is_balanced(line: str) -> bool:
    pairs = {")": "(", "]": "[", "}": "{"}
    stack: list[str] = []
    for char in line:
        if char in "([{":
            stack.append(char)
        elif char in ")]}":
            if not stack or stack.pop() != pairs[char]:
                return False
    return not stack


def _extract_struct_fields(body: str) -> tuple[list[str], list[str]]:
    """Return (field_names, unrecognized_lines). Fail closed: a struct-body line that is
    not a parsed data member or an access specifier is reported so it must be
    classified, rather than silently dropped from the check."""
    fields: list[str] = []
    unrecognized: list[str] = []
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped or stripped in ("{", "}"):
            continue
        if _ACCESS_SPECIFIER_RE.match(stripped):
            continue
        match = _FIELD_RE.match(line)
        if (
            match
            and stripped.endswith(";")
            and _is_balanced(stripped)
            and not _has_top_level_comma(stripped)
        ):
            fields.append(match.group(1))
            continue
        unrecognized.append(stripped)
    return fields, unrecognized


def _extract_populators(body: str) -> dict[str, str]:
    """Map RasterPerformance field -> the TileRenderer getter that fills it."""
    mapping: dict[str, str] = {}
    for line in body.splitlines():
        match = _POPULATE_RE.match(line)
        if match:
            mapping[match.group(1)] = match.group(2)
    return mapping


def _extract_getters(text: str) -> dict[str, tuple[str, str]]:
    """Map TileRenderer getter name -> (state_group, member) it returns."""
    stripped = _strip_cpp_comments(text)
    getters: dict[str, tuple[str, str]] = {}
    for line in stripped.splitlines():
        match = _GETTER_RE.match(line)
        if match:
            getters[match.group(1)] = (match.group(2), match.group(3))
    return getters


def _extract_reject_assignments(body: str) -> set[tuple[str, str]]:
    """The set of (state_group, member) pairs `_reject_frame()` assigns."""
    assigned: set[tuple[str, str]] = set()
    for line in body.splitlines():
        match = _REJECT_ASSIGN_RE.match(line)
        if match:
            assigned.add((match.group(1), match.group(2)))
    return assigned


def main() -> int:  # noqa: PLR0911, PLR0912, PLR0915 -- linear fail-closed checks
    prefix = "[reject-telemetry-parity]"

    for path in (PERF_STRUCT_HEADER, POPULATOR_SOURCE, RENDERER_HEADER, RENDERER_SOURCE):
        if not path.is_file():
            print(f"{prefix} FAIL missing {path.relative_to(ROOT)}")
            return 1

    if len(SURVIVES_REJECT_FIELDS) != EXPECTED_SURVIVES_REJECT_COUNT:
        print(
            f"{prefix} FAIL SURVIVES_REJECT_FIELDS has {len(SURVIVES_REJECT_FIELDS)} entries "
            f"but EXPECTED_SURVIVES_REJECT_COUNT is {EXPECTED_SURVIVES_REJECT_COUNT}. Exempting "
            f"a field from reject-time invalidation must be a deliberate, visible change: "
            f"update the pin in the same diff as the entry, with a justification in review."
        )
        return 1

    thin_reasons = [
        field
        for field, reason in SURVIVES_REJECT_FIELDS.items()
        if len(reason.strip()) < MINIMUM_REASON_LENGTH
        or reason.strip().lower().rstrip(".") in _PLACEHOLDER_REASONS
    ]
    if thin_reasons:
        for field in thin_reasons:
            print(
                f"{prefix} FAIL SURVIVES_REJECT_FIELDS['{field}'] has an empty or placeholder "
                f"reason; state why the field must survive a rejected frame"
            )
        return 1

    struct_text = PERF_STRUCT_HEADER.read_text(encoding="utf-8")
    struct_body = _brace_body(struct_text, f"struct {STRUCT_NAME}")
    if struct_body is None:
        print(f"{prefix} FAIL could not parse struct {STRUCT_NAME} in {PERF_STRUCT_HEADER.name}")
        return 1

    directives = [ln.strip() for ln in struct_body.splitlines() if _PP_DIRECTIVE_RE.match(ln)]
    if directives:
        for directive in directives:
            print(
                f"{prefix} FAIL struct {STRUCT_NAME} contains a preprocessor directive "
                f"`{directive}`; the guard cannot know which branch compiles, so keep the "
                f"field region free of conditional compilation"
            )
        return 1

    fields, unrecognized = _extract_struct_fields(struct_body)
    if not fields:
        print(f"{prefix} FAIL parsed no fields from struct {STRUCT_NAME}")
        return 1
    if unrecognized:
        for line in unrecognized:
            print(
                f"{prefix} FAIL unrecognized declaration in {STRUCT_NAME}: `{line}` -- extend "
                f"the parser (tests/ci/check_reject_telemetry_parity.py) to classify it"
            )
        return 1

    populator_body = _brace_body(POPULATOR_SOURCE.read_text(encoding="utf-8"), POPULATOR_ANCHOR)
    if populator_body is None:
        print(
            f"{prefix} FAIL could not parse `{POPULATOR_ANCHOR}` in {POPULATOR_SOURCE.name}; "
            f"the field -> getter chain is what makes this guard derived rather than a list"
        )
        return 1
    populators = _extract_populators(populator_body)
    if not populators:
        print(f"{prefix} FAIL parsed no `perf.<field> = tile_renderer-><getter>();` assignments")
        return 1

    getters = _extract_getters(RENDERER_HEADER.read_text(encoding="utf-8"))
    if not getters:
        print(f"{prefix} FAIL parsed no inline getters from {RENDERER_HEADER.name}")
        return 1

    reject_body = _brace_body(RENDERER_SOURCE.read_text(encoding="utf-8"), REJECT_ANCHOR)
    if reject_body is None:
        print(
            f"{prefix} FAIL could not find `{REJECT_ANCHOR}` in {RENDERER_SOURCE.name}. If the "
            f"reject boundary was renamed or removed, update REJECT_ANCHOR -- do not delete "
            f"this check; the boundary is what the whole #586 rejection contract rests on."
        )
        return 1
    reject_assignments = _extract_reject_assignments(reject_body)
    if not reject_assignments:
        print(
            f"{prefix} FAIL `_reject_frame()` assigns no `renderer.<group>.<member>` fields; "
            f"a reject boundary that invalidates nothing cannot be correct"
        )
        return 1

    failures: list[str] = []
    covered: list[str] = []

    for field in fields:
        exempt_reason = SURVIVES_REJECT_FIELDS.get(field)
        getter = populators.get(field)

        if getter is None:
            # Not populated by get_performance() at all: the field is either dead or
            # filled by a route this guard does not model. Either way it must be
            # classified by a human rather than silently skipped.
            if exempt_reason is None:
                failures.append(
                    f"{STRUCT_NAME}.{field} is never assigned in get_performance(), so this "
                    f"guard cannot resolve what backs it. Populate it, remove it, or add it "
                    f"to SURVIVES_REJECT_FIELDS with a reason."
                )
            continue

        backing = getters.get(getter)
        if backing is None:
            failures.append(
                f"{STRUCT_NAME}.{field} is filled by TileRenderer::{getter}(), which is not a "
                f"simple `return <group>.<member>;` inline getter in {RENDERER_HEADER.name}. "
                f"This guard resolves backing state through that shape only -- keep the getter "
                f"trivial, or extend the parser; do not drop the field from the check."
            )
            continue

        is_covered = backing in reject_assignments
        if is_covered and exempt_reason is not None:
            failures.append(
                f"{STRUCT_NAME}.{field} is both cleared by _reject_frame() (via "
                f"{backing[0]}.{backing[1]}) and listed in SURVIVES_REJECT_FIELDS; remove the "
                f"stale exemption and lower the pin"
            )
        elif is_covered:
            covered.append(field)
        elif exempt_reason is None:
            failures.append(
                f"{STRUCT_NAME}.{field} is per-frame telemetry that a REJECTED frame does not "
                f"invalidate: it is backed by {backing[0]}.{backing[1]} (via {getter}()), which "
                f"_reject_frame() never assigns, so it keeps reporting the last SUCCESSFUL "
                f"frame's value. Clear it in _reject_frame() (tile_renderer.cpp) -- clearing the "
                f"validity flag too, if it has one -- or, if it must survive a reject, add it to "
                f"SURVIVES_REJECT_FIELDS with a reason and bump the pin."
            )

    # No exemption may name a field that no longer exists.
    field_set = set(fields)
    for field in SURVIVES_REJECT_FIELDS:
        if field not in field_set:
            failures.append(
                f"SURVIVES_REJECT_FIELDS lists `{field}`, which is not a {STRUCT_NAME} field; "
                f"remove the stale entry and lower the pin"
            )

    if failures:
        for failure in failures:
            print(f"{prefix} FAIL {failure}")
        print(
            f"{prefix} {len(failures)} problem(s); {len(covered)} reject-invalidated, "
            f"{len(SURVIVES_REJECT_FIELDS)} survives-reject, {len(fields)} fields total."
        )
        return 1

    print(
        f"{prefix} PASSED - all {len(fields)} {STRUCT_NAME} fields are invalidated by "
        f"_reject_frame() ({len(covered)}) or explicitly allowed to survive a reject "
        f"({len(SURVIVES_REJECT_FIELDS)}); backing state resolved through "
        f"{len(populators)} getter(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
