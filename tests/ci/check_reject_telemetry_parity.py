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

An assignment is not an invalidation (round-5)
----------------------------------------------
The first version of this guard recorded only the *target* of each `_reject_frame()`
assignment, so any assignment at all counted as coverage. Review caught that this is the
very defect class the guard exists to catch, one link down:

    renderer.timing_state.last_submission_cpu_ms = renderer.timing_state.last_submission_cpu_ms;

is a no-op that publishes the last SUCCESSFUL frame's CPU cost on a rejected frame, and
it satisfied a target-only check. So did `= 42.0f`, and so did `= true` on a validity
flag -- which is worse than a stale duration, because the flag is the only thing telling
a consumer "this stage really ran". A completeness check that a no-op can satisfy is not
a completeness check.

The guard therefore checks three things about each covering assignment, all derived:

  * the assigned VALUE is the invalid state for the field's declared type -- `0`/`0.0f`
    for a duration or counter, `false` for a validity flag, `String()`/`""` for a
    reason string. A type this guard has no invalid-value rule for is FAILED, not
    assumed fine (see `_INVALID_VALUE_RULES`).
  * the assignment is UNCONDITIONAL: at the top statement level of `_reject_frame()`, not
    governed by any control statement. "Invalidated on some rejects" is exactly the bug
    #586 round-3 and round-4 kept re-finding. See the next section for why brace depth
    alone did not establish this.
  * `_reject_frame()`'s body is free of preprocessor directives, for the same reason the
    struct body is: the guard cannot know which branch compiles, so it must not credit an
    assignment that may be compiled out.

Braces are not control flow (round-8)
-------------------------------------
"Unconditional" was established by brace depth alone, and brace depth is not a statement
structure. An unbraced conditional has depth 0:

    if (p_stage != GaussianSplatting::FrameRejectStage::DEVICE)
        renderer.timing_state.last_submission_cpu_ms = 0.0f;

`_line_brace_depths()` reported that assignment at depth 0 and the guard credited it for
EVERY reject, while in reality every other rejection path kept publishing the last
successful frame's CPU cost -- the exact defect this blocking guard exists to make
impossible, passing green. The same held for an unbraced `else` arm and for a body
governed by an unbraced `for`/`while`. That was the third consecutive round in this
series in which a guard added to close a hole was found to have the same hole one level
down: a line/brace heuristic standing in for structure.

So the scan is now structural. `_statement_depths()` walks the function body tracking
braces AND control statements, skipping string/character literals, and the depth it
reports for a line is `braces + pending control bodies`. On top of that it REFUSES the
shape outright: an unbraced control statement anywhere in `_reject_frame()` is a hard
failure, because the guard will not decide for itself where an unbraced sub-statement
ends. Brace it. Anything the scanner cannot carry to a clean end state (unbalanced
braces/parens, a control statement with no condition, a literal that does not terminate
on its line) is likewise a failure rather than a best guess.

For the same reason the getter scan is scoped to `class TileRenderer`: a same-named
getter on some other class in that header resolves a name, not the backing member.

A field that is neither reject-covered nor exempt fails the guard: clear its backing
member in `_reject_frame()` (preferred), or -- if it must survive a reject -- add it to
`SURVIVES_REJECT_FIELDS` with a justification and bump the pin. Do not silence this
guard by deleting a field from the check, and do not weaken it.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "modules" / "gaussian_splatting"
PERF_STRUCT_HEADER = MODULE / "interfaces" / "rasterizer_interfaces.h"
POPULATOR_SOURCE = MODULE / "interfaces" / "tile_rasterizer.cpp"
RENDERER_HEADER = MODULE / "renderer" / "tile_renderer.h"
RENDERER_SOURCE = MODULE / "renderer" / "tile_renderer.cpp"

STRUCT_NAME = "RasterPerformance"
POPULATOR_ANCHOR = f"{STRUCT_NAME} TileRasterizer::get_performance() const"
REJECT_ANCHOR = "RID _reject_frame(GaussianSplatting::FrameRejectStage p_stage)"
# Getters are resolved only inside this class: a getter of the same name on another class
# in the same header would resolve a NAME, not the member that `_reject_frame()` clears.
RENDERER_CLASS_ANCHOR = "class TileRenderer"

# A plain-old-data member: "<type...> <name> = <default>;" or "<type...> <name>;".
# The TYPE is captured too -- it is what says which value counts as "invalidated".
_FIELD_RE = re.compile(r"^\s*([\w:]+(?:\s*[*&])?)\s+(\w+)\s*(?:=|;)")
# `perf.<field> = tile_renderer-><getter>();` inside get_performance().
_POPULATE_RE = re.compile(r"^\s*perf\.(\w+)\s*=\s*tile_renderer->(\w+)\s*\(\s*\)\s*;")
# An inline getter on TileRenderer: `<type> <name>() const { return <group>.<member>; }`.
# Restricted to the single-expression `return a.b;` shape on purpose -- a getter that
# computes or branches is NOT resolvable to one backing member, and is reported rather
# than guessed at (see _resolve_backing below).
_GETTER_RE = re.compile(
    r"^\s*[\w:]+(?:\s*[*&])?\s+(\w+)\s*\(\s*\)\s*const\s*\{\s*return\s+(\w+)\.(\w+)\s*;\s*\}"
)
# A member assignment inside _reject_frame(): `renderer.<group>.<member> = <value>;`.
# The VALUE is captured, not just the target -- an assignment that merely exists is not
# an invalidation (see _INVALID_VALUE_RULES and _invalidation_problems). A statement that
# does not close on its own line simply does not match, and the field is then reported as
# uncovered, which is the fail-closed direction.
_REJECT_ASSIGN_RE = re.compile(r"^\s*renderer\.(\w+)\.(\w+)\s*=\s*([^=].*);\s*$")
_PP_DIRECTIVE_RE = re.compile(r"^\s*#")
_IDENT_RE = re.compile(r"[A-Za-z_]\w*")
# Statement structure, not braces: these introduce a sub-statement whose body governs the
# assignments inside it whether or not that body happens to be braced.
_CONTROL_WITH_CONDITION = frozenset({"if", "while", "for", "switch", "catch"})
_CONTROL_WITHOUT_CONDITION = frozenset({"else", "do", "try"})
_ACCESS_SPECIFIER_RE = re.compile(r"^(?:public|private|protected)\s*:\s*$")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")

# What "invalidated" means, per declared field type. Keyed by the base type name, so the
# check is derived from the struct rather than from a per-field hand-written expectation.
# Anything not listed here is FAILED rather than accepted: if the guard cannot say what
# the invalid value of a type is, it cannot tell an invalidation from a no-op, and
# guessing is how a target-only check got shipped in the first place.
_INVALID_VALUE_RULES = (
    (frozenset({"bool"}), re.compile(r"^false$"), "false"),
    (frozenset({"float", "double", "real_t"}), re.compile(r"^0(?:\.0*)?[fF]?$"), "0.0f"),
    (
        frozenset(
            {
                "int",
                "unsigned",
                "long",
                "short",
                "char",
                "int8_t",
                "int16_t",
                "int32_t",
                "int64_t",
                "uint8_t",
                "uint16_t",
                "uint32_t",
                "uint64_t",
                "size_t",
                "ptrdiff_t",
            }
        ),
        re.compile(r"^0[uUlL]*$"),
        "0",
    ),
    (
        frozenset({"String", "StringName"}),
        re.compile(r'^(?:String\(\)|StringName\(\)|"")$'),
        'String() or ""',
    ),
)


class RejectAssignment(NamedTuple):
    """One `renderer.<group>.<member> = <value>;` statement in `_reject_frame()`.

    `depth` is the STATEMENT nesting relative to the function body -- open braces plus any
    unbraced control body in force. Only depth 0 is an unconditional invalidation, which is
    what a rejected frame owes every field. Brace depth alone was not enough: it reported an
    unbraced conditional's body at 0 (#586 round-8)."""

    value: str
    depth: int


class _StatementScan(NamedTuple):
    """Per-line statement depths, plus the shapes `_statement_depths()` refuses to parse."""

    depths: list[tuple[int, str]]
    problems: list[str]

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


def _skip_literal(text: str, index: int) -> int:
    """Index just past the string/char literal opening at `index`.

    A literal that does not terminate on its own line is not advanced past -- the caller
    keeps scanning from the next character, which is the fail-closed direction (the
    structure it derives will not balance, and it refuses rather than guesses)."""
    quote = text[index]
    cursor = index + 1
    length = len(text)
    while cursor < length:
        char = text[cursor]
        if char == "\\":
            if cursor + 1 < length and text[cursor + 1] == "\n":
                return cursor
            cursor += 2
            continue
        if char == quote:
            return cursor + 1
        if char == "\n":
            return cursor
        cursor += 1
    return cursor


def _brace_body(text: str, anchor: str) -> str | None:
    """Return the `{...}` body (contents only) following `anchor`, comments stripped
    first so braces inside comments cannot mis-bound it. String and character literals
    are skipped for the same reason -- a `}` inside a log message is not a scope."""
    stripped = _strip_cpp_comments(text)
    start = stripped.find(anchor)
    if start == -1:
        return None
    brace = stripped.find("{", start)
    if brace == -1:
        return None
    depth = 0
    index = brace
    length = len(stripped)
    while index < length:
        char = stripped[index]
        if char in ("'", '"'):
            index = _skip_literal(stripped, index)
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return stripped[brace + 1 : index]
        index += 1
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


def _extract_struct_fields(body: str) -> tuple[dict[str, str], list[str]]:
    """Return ({field_name: declared_type}, unrecognized_lines). Fail closed: a
    struct-body line that is not a parsed data member or an access specifier is reported
    so it must be classified, rather than silently dropped from the check."""
    fields: dict[str, str] = {}
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
            fields[match.group(2)] = match.group(1).strip()
            continue
        unrecognized.append(stripped)
    return fields, unrecognized


def _invalid_value_rule(field_type: str) -> tuple[re.Pattern[str], str] | None:
    """The (matcher, canonical-form) saying which literal invalidates this type, or None
    if the guard has no rule for it -- in which case the caller must FAIL, not assume."""
    base = field_type.replace("*", "").replace("&", "").strip().split("::")[-1]
    for type_names, pattern, canonical in _INVALID_VALUE_RULES:
        if base in type_names:
            return pattern, canonical
    return None


def _statement_depths(body: str) -> _StatementScan:
    """(statement_depth_at_line_start, line) for each line of a comment-stripped body, plus
    the structural shapes this guard refuses to reason about.

    Statement depth is `open braces + pending unbraced control bodies`, NOT brace depth:
    brace depth alone reports an unbraced conditional's body at depth 0 and would credit it
    as an unconditional invalidation (#586 round-8). String and character literals are
    skipped so a brace, paren or semicolon inside one cannot shift the structure.

    Every unbraced control statement is additionally reported as a problem. The guard does
    not need to locate the end of an unbraced sub-statement if it refuses to accept one."""
    lines = body.split("\n")
    depths: list[tuple[int, str]] = []
    problems: list[str] = []

    brace_depth = 0
    paren_depth = 0
    unbraced: list[int] = []  # brace depths carrying a pending unbraced control body
    do_open: list[int] = []  # brace depths at which a `do { ... }` block is open
    awaiting_condition = False  # saw `if`/`while`/... , the next token must be `(`
    condition_active = False  # inside a control statement's condition
    condition_level = 0  # paren depth the condition returns to when it closes
    condition_is_do_tail = False  # this condition belongs to a `do ... while (...)` tail
    awaiting_body = False  # condition consumed (or `else`/`do`/`try`): next token is the body
    do_pending = False  # the pending body belongs to a `do`
    expect_do_while = False  # a `do` block just closed; a following `while` is its tail

    def snippet(at: int) -> str:
        return body[at : at + 60].split("\n", 1)[0].strip()

    index = 0
    line_index = 0
    length = len(body)
    depths.append((0, lines[0] if lines else ""))

    while index < length:
        char = body[index]

        if char == "\n":
            line_index += 1
            if line_index < len(lines):
                # A body pending from the previous line governs this line's statement.
                pending = 1 if (awaiting_body or awaiting_condition or condition_active) else 0
                depths.append((brace_depth + len(unbraced) + pending, lines[line_index]))
            index += 1
            continue
        if char.isspace():
            index += 1
            continue

        # --- a control statement's condition must open here ---
        if awaiting_condition:
            awaiting_condition = False
            if char == "(":
                condition_active = True
                condition_level = paren_depth
                paren_depth += 1
                index += 1
                continue
            problems.append(
                f"_reject_frame() has a control statement with no parenthesised condition at "
                f"`{snippet(index)}`; this guard refuses shapes it cannot parse rather than "
                f"assuming the statements after it are unconditional"
            )

        # --- a control statement's body must open here ---
        if awaiting_body:
            awaiting_body = False
            if char == "{":
                brace_depth += 1
                if do_pending:
                    do_open.append(brace_depth - 1)
                    do_pending = False
                index += 1
                continue
            keyword_match = _IDENT_RE.match(body, index)
            keyword = keyword_match.group(0) if keyword_match else ""
            if keyword in _CONTROL_WITH_CONDITION or keyword in _CONTROL_WITHOUT_CONDITION:
                # `else if (...)`: the body IS another control statement, whose own
                # braced-ness is checked when it is processed just below.
                if do_pending:
                    problems.append(
                        f"_reject_frame() has an unbraced `do` body at `{snippet(index)}`; "
                        f"brace it"
                    )
                    do_pending = False
                    unbraced.append(brace_depth)
            else:
                problems.append(
                    f"_reject_frame() has an UNBRACED control statement body at "
                    f"`{snippet(index)}`. Brace depth alone cannot tell that statement apart "
                    f"from an unconditional one, so an unbraced conditional invalidation would "
                    f"be credited for every reject while leaving telemetry stale on every other "
                    f"rejection path (#586 round-8). Add braces."
                )
                unbraced.append(brace_depth)
                do_pending = False

        do_while_tail = expect_do_while
        expect_do_while = False

        if char in ("'", '"'):
            quote = char
            index += 1
            terminated = False
            while index < length:
                current = body[index]
                if current == "\\":
                    if index + 1 < length and body[index + 1] == "\n":
                        break  # line-continued literal: refuse below
                    index += 2
                    continue
                if current == quote:
                    index += 1
                    terminated = True
                    break
                if current == "\n":
                    break
                index += 1
            if not terminated:
                problems.append(
                    "_reject_frame() has a string or character literal that does not terminate "
                    "on its own line; this guard cannot establish statement structure around "
                    "it, and will not guess"
                )
            continue
        if char == "{":
            brace_depth += 1
            index += 1
            continue
        if char == "}":
            brace_depth -= 1
            if brace_depth < 0:
                problems.append("_reject_frame() has an unbalanced `}`; refusing to guess")
                brace_depth = 0
            while unbraced and unbraced[-1] >= brace_depth:
                unbraced.pop()
            if do_open and do_open[-1] == brace_depth:
                do_open.pop()
                expect_do_while = True
            index += 1
            continue
        if char == "(":
            paren_depth += 1
            index += 1
            continue
        if char == ")":
            paren_depth -= 1
            if paren_depth < 0:
                problems.append("_reject_frame() has an unbalanced `)`; refusing to guess")
                paren_depth = 0
            if condition_active and paren_depth == condition_level:
                condition_active = False
                if condition_is_do_tail:
                    condition_is_do_tail = False
                else:
                    awaiting_body = True
            index += 1
            continue
        if char == ";":
            if paren_depth == 0:
                while unbraced and unbraced[-1] == brace_depth:
                    unbraced.pop()
            index += 1
            continue

        word_match = _IDENT_RE.match(body, index)
        if word_match:
            word = word_match.group(0)
            index = word_match.end()
            if word in _CONTROL_WITH_CONDITION:
                if word == "while" and do_while_tail:
                    condition_is_do_tail = True
                awaiting_condition = True
            elif word in _CONTROL_WITHOUT_CONDITION:
                if word == "do":
                    do_pending = True
                awaiting_body = True
            continue
        index += 1

    if unbraced or awaiting_body or awaiting_condition or condition_active or do_pending:
        problems.append(
            "_reject_frame() ends with an unresolved control statement; this guard could not "
            "establish where every statement sits, so it refuses rather than crediting an "
            "assignment it cannot place"
        )
    if brace_depth != 0 or paren_depth != 0 or do_open:
        problems.append(
            f"_reject_frame() does not parse to a balanced end state (braces {brace_depth}, "
            f"parens {paren_depth}); refusing rather than guessing at statement structure"
        )
    return _StatementScan(depths, problems)


def _extract_populators(body: str) -> dict[str, str]:
    """Map RasterPerformance field -> the TileRenderer getter that fills it."""
    mapping: dict[str, str] = {}
    for line in body.splitlines():
        match = _POPULATE_RE.match(line)
        if match:
            mapping[match.group(1)] = match.group(2)
    return mapping


def _extract_getters(class_body: str) -> dict[str, tuple[str, str]]:
    """Map TileRenderer getter name -> (state_group, member) it returns.

    `class_body` must already be scoped to `class TileRenderer`; a getter of the same name
    on another class in the header backs a different member and must not resolve here."""
    getters: dict[str, tuple[str, str]] = {}
    for line in class_body.splitlines():
        match = _GETTER_RE.match(line)
        if match:
            getters[match.group(1)] = (match.group(2), match.group(3))
    return getters


def _extract_reject_assignments(
    scan: _StatementScan,
) -> dict[tuple[str, str], list[RejectAssignment]]:
    """Map (state_group, member) -> every assignment `_reject_frame()` makes to it.

    Every assignment is kept, not just the last: the guard requires ALL of them to be
    unconditional invalidations, so a later `= <stale>` cannot hide behind an earlier
    `= 0.0f` (or the reverse) on a path the parser does not model."""
    assigned: dict[tuple[str, str], list[RejectAssignment]] = {}
    for depth, line in scan.depths:
        match = _REJECT_ASSIGN_RE.match(line)
        if match:
            key = (match.group(1), match.group(2))
            assigned.setdefault(key, []).append(RejectAssignment(match.group(3).strip(), depth))
    return assigned


def _invalidation_problems(
    field: str,
    field_type: str,
    backing: tuple[str, str],
    assignments: list[RejectAssignment],
) -> list[str]:
    """Why `_reject_frame()`'s assignments to `backing` do NOT invalidate `field` (empty
    list = they do). This is the check that separates an invalidation from a no-op."""
    where = f"{backing[0]}.{backing[1]}"
    rule = _invalid_value_rule(field_type)
    if rule is None:
        return [
            f"{STRUCT_NAME}.{field} is backed by {where}, which _reject_frame() assigns, but this "
            f"guard has no invalid-value rule for the declared type `{field_type}`, so it cannot "
            f"tell an invalidation from a no-op. Add the type to _INVALID_VALUE_RULES in "
            f"tests/ci/check_reject_telemetry_parity.py; do not drop the field from the check."
        ]
    pattern, canonical = rule
    problems: list[str] = []
    for assignment in assignments:
        if assignment.depth != 0:
            problems.append(
                f"{STRUCT_NAME}.{field} is backed by {where}, which _reject_frame() assigns only "
                f"inside a nested or CONDITIONALLY REACHED statement (`{where} = "
                f"{assignment.value};`, statement depth {assignment.depth}). Every rejected frame "
                f"must invalidate it, so the assignment has to be unconditional at the top "
                f"statement level of _reject_frame() -- 'invalidated on some rejects' is the bug "
                f"this guard exists to catch, and an unbraced `if` governing it counts (#586 "
                f"round-8)."
            )
        elif not pattern.match(assignment.value):
            problems.append(
                f"{STRUCT_NAME}.{field} is backed by {where}, which _reject_frame() assigns "
                f"`{assignment.value}` -- not the invalid state for a `{field_type}` (expected "
                f"{canonical}). An assignment that merely EXISTS invalidates nothing: "
                f"`{where} = {where};` would satisfy a target-only check while the last SUCCESSFUL "
                f"frame's value keeps being published, and `= true` on a validity flag destroys "
                f"the one signal that separates 'did not run' from 'ran and measured 0'. Assign "
                f"{canonical}, or -- if the field must survive a reject -- add it to "
                f"SURVIVES_REJECT_FIELDS with a reason and bump the pin."
            )
    return problems


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

    renderer_class_body = _brace_body(
        RENDERER_HEADER.read_text(encoding="utf-8"), RENDERER_CLASS_ANCHOR
    )
    if renderer_class_body is None:
        print(
            f"{prefix} FAIL could not find `{RENDERER_CLASS_ANCHOR}` in {RENDERER_HEADER.name}; "
            f"getters are resolved inside that class only, so a same-named getter elsewhere in "
            f"the header cannot be mistaken for the backing member"
        )
        return 1
    getters = _extract_getters(renderer_class_body)
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

    reject_directives = [ln.strip() for ln in reject_body.splitlines() if _PP_DIRECTIVE_RE.match(ln)]
    if reject_directives:
        for directive in reject_directives:
            print(
                f"{prefix} FAIL _reject_frame() contains a preprocessor directive `{directive}`; "
                f"the guard cannot know which branch compiles, so an invalidation inside one "
                f"cannot be credited. Keep the reject boundary free of conditional compilation"
            )
        return 1

    # Statement structure first: if the body contains a shape this guard cannot place a
    # statement in (an unbraced control body above all), nothing below it can be trusted, so
    # refuse rather than credit assignments whose reachability is a guess.
    reject_scan = _statement_depths(reject_body)
    if reject_scan.problems:
        for problem in reject_scan.problems:
            print(f"{prefix} FAIL {problem}")
        return 1

    reject_assignments = _extract_reject_assignments(reject_scan)
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

        assignments = reject_assignments.get(backing)
        is_covered = assignments is not None
        if is_covered and exempt_reason is not None:
            failures.append(
                f"{STRUCT_NAME}.{field} is both cleared by _reject_frame() (via "
                f"{backing[0]}.{backing[1]}) and listed in SURVIVES_REJECT_FIELDS; remove the "
                f"stale exemption and lower the pin"
            )
        elif is_covered:
            # An assignment exists -- but only an assignment of the field's INVALID value,
            # made unconditionally, actually invalidates anything.
            problems = _invalidation_problems(field, fields[field], backing, assignments)
            if problems:
                failures.extend(problems)
            else:
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
