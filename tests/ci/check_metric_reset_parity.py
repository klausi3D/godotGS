#!/usr/bin/env python3
"""Guard: every PerformanceMetrics field has an explicit per-frame reset disposition
-- it is either cleared by one of the struct's `reset_*` helper methods, or it is
listed below as intentionally NOT per-frame-reset (with a reason).

Why this exists (#528): the renderer clears stale telemetry field-by-field at
several render-path sites (`reset_render_state_for_frame` and
`render_sorted_splats_with_context` in renderer/render_pipeline_stages.cpp, and the
no-rasterizer branch of `update_gpu_pass_metrics_from_tile_renderer` in
renderer/render_resource_orchestrator.cpp). Those hand-lists were duplicated inline:
a new `PerformanceMetrics` field added to the struct but missed at a site silently
reports the previous route's value -- a stale-telemetry bug.

The duplication is now folded into named `reset_*` helpers on the struct (each owns
one logical group of fields; the sites compose the groups they need), and this guard
fails closed so a *newly added* metric can never silently escape a reset decision:

  * covered   -- assigned inside a `reset_*` helper body, or
  * NOT_RESET -- listed below with a reason (cumulative/lifetime counter, rolling
                 aggregate, or a value written fresh every frame by its producing
                 stage, so it is not a stale-carry hazard at the reset points).

A field that is neither covered nor listed fails the guard: add it to the
appropriate `reset_*` helper (preferred), or -- if it must persist across frames --
add it to NOT_RESET here with a justification. Do not silence this guard by deleting
a field from the check, and do not weaken it.

These are deliberately *partial* resets. A single blanket `*this = PerformanceMetrics{}`
was rejected: the sites intentionally preserve cumulative counters (e.g.
total_frames_rendered, the monotonic raster_pipeline_reformats) and per-stage
outputs, which a whole-struct reset would corrupt.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
PERF_HEADER = MODULE / "renderer" / "render_types" / "render_performance_types.h"

STRUCT_NAME = "PerformanceMetrics"

# A plain-old-data member declaration inside the struct body:
# "<type...> <name> = <default>;" or "<type...> <name>;". Method declarations
# ("void reset_x();") are excluded separately because a "(" follows the name.
_FIELD_RE = re.compile(r"^\s*[\w:]+(?:\s*[*&])?\s+(\w+)\s*(?:=|;)")
# An out-of-line reset helper definition:
# `inline void PerformanceMetrics::reset_name() {`. The `reset_` prefix on the
# captured name is REQUIRED, not decorative: without it this regex would match
# any zero-arg inline mutator on the struct (e.g. a hypothetical
# `mark_frame_started()` that increments a counter), and any field it happened
# to assign would be counted as reset-covered even though the method never
# resets anything -- a false pass (audit #528 follow-up: a non-reset no-arg
# mutator was being counted as reset coverage). Requiring the prefix shrinks
# the accepted surface to exactly the helpers this guard is meant to recognize.
#
# Out of scope (cannot be done by a regex, and is not attempted): confirming a
# `reset_*`-named method's body is *semantically* a reset (e.g. it could not
# catch a `reset_foo()` that, by bug, increments instead of zeroing a field).
# That is a code-review concern, not this guard's job -- the guard's contract
# is "every field has an explicit, named reset disposition", not "every
# disposition is bug-free".
_RESET_DEF_RE = re.compile(
    r"\binline\s+void\s+" + re.escape(STRUCT_NAME) + r"::(reset_\w+)\s*\(\s*\)\s*\{"
)
# A statement inside a reset body that assigns a member: `field = value;`. Bare
# assignment (no leading type token) distinguishes it from a field *declaration*.
_ASSIGN_RE = re.compile(r"^\s*(\w+)\s*=[^=]")
_PP_DIRECTIVE_RE = re.compile(r"^\s*#")
_ACCESS_SPECIFIER_RE = re.compile(r"^(?:public|private|protected)\s*:\s*$")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def _strip_cpp_comments(text: str) -> str:
    text = _BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return _LINE_COMMENT_RE.sub("", text)


# Fields deliberately NOT cleared by a per-frame reset helper. Each maps to a reason.
# Categories: cumulative/lifetime counters; rolling aggregates maintained across
# frames; and per-stage outputs that the producing stage overwrites every frame (so
# they are never a stale-carry hazard at the reset points). A field here must not
# also be covered by a reset helper (the guard flags that as a stale entry).
NOT_RESET_FIELDS: dict[str, str] = {
    # --- Cumulative / lifetime counters (must survive per-frame resets) ---
    "total_frames_rendered": "cumulative lifetime frame counter",
    "instance_sort_sync_fallback_count": "cumulative lifetime fallback counter",
    "sort_cached_fallback_count": "cumulative lifetime fallback counter",
    "sort_identity_fallback_count": "cumulative lifetime fallback counter",
    "sort_cull_order_fallback_count": "cumulative lifetime fallback counter",
    "sort_cache_hits": "cumulative lifetime cache counter",
    "sort_cache_misses": "cumulative lifetime cache counter",
    "cull_projection_contract_mismatch_count": "cumulative lifetime mismatch counter",
    "raster_pipeline_reformats": "monotonic lifetime counter; reset would mask earlier reformats",
    # --- Rolling aggregates maintained across frames (finalize_frame_metrics) ---
    "avg_frame_time_ms": "rolling average maintained across frames",
    "peak_frame_time_ms": "rolling peak maintained across frames",
    "avg_frame_to_frame_ms": "rolling average maintained across frames",
    "frame_to_frame_time_ms": "derived from last_frame_start_usec each frame",
    "last_frame_start_usec": "frame-pacing anchor carried between frames",
    # --- Written fresh each frame by the producing stage (no stale-carry) ---
    "buffer_upload_time_ms": "set by the buffer-upload stage each frame",
    "culling_time_ms": "set by the cull stage each frame",
    "gpu_memory_usage_mb": "set by resource accounting each frame",
    "uploaded_splat_count": "set by the buffer-upload stage each frame",
    "rendered_splat_count": "set by the raster stage each frame",
    "using_real_data": "set by apply_data_source_plan each frame",
    "data_source": "set by apply_data_source_plan each frame",
    "data_source_error": "set by apply_data_source_plan each frame",
    "raster_path": "route label stamped per-route (main path sets 'unknown' inline before selection)",
    "raster_scene_clip_active": "set by the raster scene-clip pre-check each frame",
    "sort_submission_time_ms": "set by the sort stage each frame",
    "sort_wait_time_ms": "set by the sort stage each frame",
    "sort_input_build_time_ms": "set by the sort stage each frame",
    "async_sort_used": "set by the sort stage each frame",
    "async_sort_waited": "set by the sort stage each frame",
    "async_overlap_efficiency": "set by the sort stage each frame",
    "culled_frustum_count": "set by the cull stage each frame",
    "culled_distance_count": "set by the cull stage each frame",
    "culled_screen_count": "set by the cull stage each frame",
    "culled_importance_count": "set by the cull stage each frame",
    "culling_candidate_count": "set by the cull stage each frame",
    "visible_after_culling": "set by the cull stage each frame",
    "cull_route_uid": "set by the cull stage each frame",
    "cull_route_reason": "set by the cull stage each frame",
    "used_hierarchical_culling": "set by the cull stage each frame",
    "streaming_state": "set by the streaming stage each frame",
}


def _brace_body(text: str, anchor: str) -> str | None:
    """Return the `{...}` body (contents only) that follows `anchor`, with comments
    stripped from the whole text first so braces inside comments cannot mis-bound it."""
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


def _extract_struct_fields(body: str) -> tuple[list[str], list[str]]:
    """Return (field_names, unrecognized_lines). Fail closed: any struct-body line
    that is neither a parsed data member, a `reset_*` method declaration, nor an
    access specifier is reported as unrecognized so it must be classified."""
    fields: list[str] = []
    unrecognized: list[str] = []
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped or stripped in ("{", "}"):
            continue
        # Method declarations (`void reset_x();`) carry a "(" and are not data members.
        if "(" in stripped:
            continue
        match = _FIELD_RE.match(line)
        if match:
            if _has_top_level_comma(stripped):
                unrecognized.append(stripped)
            else:
                fields.append(match.group(1))
            continue
        if _ACCESS_SPECIFIER_RE.match(stripped):
            continue
        unrecognized.append(stripped)
    return fields, unrecognized


def _extract_reset_methods(text: str) -> dict[str, list[str]]:
    """Map each `reset_*` helper name to the list of member names it assigns."""
    stripped = _strip_cpp_comments(text)
    methods: dict[str, list[str]] = {}
    for match in _RESET_DEF_RE.finditer(stripped):
        name = match.group(1)
        brace = match.end() - 1  # the '{' the regex ends on
        depth = 0
        body_start = brace
        body_end = brace
        for i in range(brace, len(stripped)):
            char = stripped[i]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    body_end = i
                    break
        assigned: list[str] = []
        for line in stripped[body_start + 1 : body_end].splitlines():
            assign = _ASSIGN_RE.match(line)
            if assign:
                assigned.append(assign.group(1))
        methods[name] = assigned
    return methods


def main() -> int:
    prefix = "[metric-reset-parity]"
    if not PERF_HEADER.is_file():
        print(f"{prefix} FAIL missing {PERF_HEADER.relative_to(ROOT)}")
        return 1

    text = PERF_HEADER.read_text(encoding="utf-8")
    struct_body = _brace_body(text, f"struct {STRUCT_NAME}")
    if struct_body is None:
        print(f"{prefix} FAIL could not parse struct {STRUCT_NAME} in {PERF_HEADER.name}")
        return 1

    # Fail closed on conditional compilation in the struct body: the guard cannot
    # know which branch is compiled, so a directive risks a dropped-but-active member.
    directives = [ln.strip() for ln in struct_body.splitlines() if _PP_DIRECTIVE_RE.match(ln)]
    if directives:
        for directive in directives:
            print(
                f"{prefix} FAIL struct {STRUCT_NAME} contains a preprocessor directive "
                f"`{directive}`; keep the field region free of conditional compilation"
            )
        return 1

    fields, unrecognized = _extract_struct_fields(struct_body)
    if not fields:
        print(f"{prefix} FAIL parsed no fields from struct {STRUCT_NAME}")
        return 1
    if unrecognized:
        for line in unrecognized:
            print(
                f"{prefix} FAIL unrecognized declaration in {STRUCT_NAME}: `{line}` "
                f"-- extend the parser (tests/ci/check_metric_reset_parity.py) to classify it"
            )
        return 1
    field_set = set(fields)

    methods = _extract_reset_methods(text)
    if not methods:
        print(f"{prefix} FAIL found no `inline void {STRUCT_NAME}::reset_*()` helpers to parse")
        return 1

    failures: list[str] = []

    # Every field a reset helper assigns must be a real struct field.
    covered: set[str] = set()
    for method, assigned in methods.items():
        for name in assigned:
            if name not in field_set:
                failures.append(
                    f"{STRUCT_NAME}::{method}() assigns `{name}`, which is not a "
                    f"{STRUCT_NAME} field (typo or renamed field?)"
                )
            else:
                covered.add(name)

    # 1. Every field is covered by a reset helper or listed as NOT_RESET.
    for field in fields:
        if field not in covered and field not in NOT_RESET_FIELDS:
            failures.append(
                f"{STRUCT_NAME}.{field} is neither cleared by a reset_*() helper nor "
                f"listed in NOT_RESET_FIELDS; add it to the right reset helper "
                f"(render_performance_types.h) or, if it must persist across frames, "
                f"add it to NOT_RESET_FIELDS with a reason"
            )

    # 2. No field is both covered and listed as NOT_RESET (stale entry).
    for field in NOT_RESET_FIELDS:
        if field in covered:
            failures.append(
                f"{STRUCT_NAME}.{field} is both cleared by a reset helper and listed in "
                f"NOT_RESET_FIELDS; remove it from NOT_RESET_FIELDS"
            )

    # 3. No NOT_RESET entry names a field that no longer exists.
    for field in NOT_RESET_FIELDS:
        if field not in field_set:
            failures.append(
                f"NOT_RESET_FIELDS lists `{field}`, which is not a {STRUCT_NAME} field; "
                f"remove the stale entry"
            )

    if failures:
        for failure in failures:
            print(f"{prefix} FAIL {failure}")
        print(
            f"{prefix} {len(failures)} problem(s); {len(covered)} reset-covered, "
            f"{len(NOT_RESET_FIELDS)} not-reset, {len(fields)} fields total."
        )
        return 1

    print(
        f"{prefix} PASSED - all {len(fields)} {STRUCT_NAME} fields are reset-covered "
        f"({len(covered)}) or explicitly not-per-frame-reset ({len(NOT_RESET_FIELDS)}); "
        f"{len(methods)} reset helper(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
