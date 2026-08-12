#!/usr/bin/env python3
"""Guard: in the RadixSort initialization path, the error a failure site returns must match
what that failure CAN ACTUALLY BE -- a program source that would not translate returns
`ERR_COMPILATION_FAILED`, and every acquisition of a DEVICE OBJECT returns `ERR_CANT_CREATE`.

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
entirely on a CORRESPONDENCE between what a site returns and what its failure is, which was,
until this guard, enforced by a comment. One `return ERR_CANT_CREATE;` typed at a
source-compilation site silently reverts the round-7 behaviour with no test failing -- the
classifier is still correct, the policy predicates are still correct, and a deterministic
failure is transient again.

What round 9 changed, and why the expectations moved with it
------------------------------------------------------------
Round 7 drew the line at "a GPU program would not build", and put BOTH
`create_compute_shader_from_spirv()` and `RenderingDevice::compute_pipeline_create()` on the
deterministic side of it. Codex's round-9 P1 finding is that this line is in the wrong place,
and the evidence is in the driver rather than in this module:

* `RenderingDeviceDriverVulkan::shader_create_from_container()` formats the `VkResult` of
  `vkCreateShaderModule()` into a message and returns `ShaderID(0)` for every failure
  (drivers/vulkan/rendering_device_driver_vulkan.cpp). `RenderingDevice::shader_create_from_bytecode()`
  turns that into a bare `RID()`.
* `RenderingDeviceDriverVulkan::compute_pipeline_create()` does the same with
  `vkCreateComputePipelines()`: `ERR_FAIL_COND_V_MSG(err, PipelineID(), ...)`.

`VK_ERROR_OUT_OF_HOST_MEMORY`, `VK_ERROR_OUT_OF_DEVICE_MEMORY` and a genuine driver rejection
therefore arrive at these call sites as the SAME invalid RID, with no code, no flag and no way
to re-ask. So "this failure is deterministic" was not a fact about those sites; it was an
assumption, and holding it re-created the defect rounds 1-2 removed: one momentary allocation
failure at shader-object or pipeline creation latched the sorter off for the rest of the
renderer's life, rejecting every translucent frame.

The two costs are not symmetric, which is what decides the direction:

* latching a transient failure => the renderer publishes nothing, permanently, with no
  recovery short of teardown;
* retrying a deterministic one => one `create_sorter()` per `SORTER_RETRY_MAX_DELAY_CALLS`
  (~1.7-5 s at 60 fps), in a state where nothing is being published anyway.

So the expectation for the driver-object sites is now `ERR_CANT_CREATE`. This is a CHANGED
EXPECTATION with a stated reason, not a relaxed one: the guard still pins exactly one code per
site, still derives the kind from the producer, and now additionally pins the LEG MAPPING
inside `create_compute_shader_from_spirv()` itself (see below), which it did not check at all
before. The deterministic class did not disappear -- it is now exactly the two legs that
translate SOURCE and touch no device object, plus the `ERR_UNAVAILABLE` capability answers,
which are precisely the causes a configuration change can fix (and #586 round-9 makes such a
change lift the latch).

What is derived
---------------
Everything except the expected codes. The guard reads the real function bodies, finds which
local each GPU object came FROM, and requires the error reported on that object's failure
check to match its PRODUCER:

    <device>->shader_compile_spirv_from_source(...)  -> program source -> ERR_COMPILATION_FAILED
    <device>->shader_compile_binary_from_spirv(...)  -> program source -> ERR_COMPILATION_FAILED
    <device>->shader_create_from_bytecode(...)       -> device object  -> ERR_CANT_CREATE
    <device>->compute_pipeline_create(...)           -> device object  -> ERR_CANT_CREATE
    <device>->storage_buffer_create(...)             -> allocation     -> ERR_CANT_CREATE
    create_compute_shader_from_spirv(..., &err)      -> PROPAGATES err

So a NEW shader or pipeline added to the sort path is covered the moment it is written; it
does not have to be added to a list here.

The last row is the round-9 addition. The helper is three operations with two different
answers, so a site cannot type a constant for it without re-deciding what the helper already
decided: the site must return the `Error` out-parameter it passed, and the guard then checks
the helper's own legs. That closes the hole a constant would leave -- collapsing the helper
back onto `RenderingDevice::shader_create_from_spirv()` (which is exactly legs 2+3 fused)
would merge the deterministic and driver legs again, and this guard fails on the missing leg.

Scope
-----
`RadixSort::create_variant()` and `RadixSort::initialize()` for the site check, plus
`create_compute_shader_from_spirv()` for the leg check. That scope is itself a derived fact
rather than a preference: the classified call site passes `GPUSorterFactory::ALGORITHM_RADIX`
explicitly, so the AUTO branch never runs and the only `initialize()` that can produce the
error the classifier sees is `RadixSort`'s. BitonicSort and OneSweepSort live in the same file
and are deliberately NOT in scope -- retagging their errors would change behaviour on the
instance-sorting path, which this round does not touch. They do, however, call the same
helper, so the leg check protects them too.

The return must be BOUND to its own branch (round-8)
----------------------------------------------------
The first version of this guard took "the first `return ERR_...;` anywhere after the
`!x.is_valid()` check" as that check's error. That search was not bounded to the `if` body,
so it could attribute a LATER object's return to this failure, and the guard could not detect
the very mutation it was written to catch:

    if (!histogram_shader_file.is_valid()) {
        GS_LOG_ERROR_DEFAULT("...");          // reports and FALLS THROUGH -- no return
    }
    ...
    if (!wg_prefix_shader_file.is_valid()) {
        return ERR_COMPILATION_FAILED;        // <- stolen, and credited to histogram
    }

Both sites were then recorded as correctly classified, with no problems, while the real code
carried on using an invalid shader. `return FAILED;` or `return _map_error(err);` hid the same
way: unreadable to the `ERR_\w+` pattern, so the scan simply walked on to the next site's
return. That is the third consecutive round in this series in which a guard added to close a
hole was found to have the same hole one level down -- a line/offset heuristic standing in for
structure. So the parse is now structural: locate the `if (...)` whose CONDITION contains the
failure check, take that branch's `{...}` body, and require exactly the expected error in it.

Fail-closed behaviour
---------------------
  * A tracked object with no failure branch is a FAILURE, not a pass: an unchecked
    shader/pipeline/buffer is a worse defect than a mislabelled one.
  * A failure check this guard cannot bind to an `if (...) { ... }` branch is a FAILURE.
    An unbraced branch is REFUSED rather than guessed at: the guard will not decide for
    itself where an unbraced statement ends. Brace the branch.
  * A branch that neither returns nor reports an error is a FAILURE -- that is the
    fall-through above.
  * A branch whose error this parser cannot read, or that reports more than one distinct
    error, is a FAILURE.
  * A propagating site whose helper call does not pass an `&<identifier>` error out-parameter,
    or that returns anything other than that identifier, is a FAILURE.
  * A missing leg inside `create_compute_shader_from_spirv()` is a FAILURE: fusing the legs
    back together is how the round-9 distinction would be lost silently.
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

# The helper whose legs decide the error the propagating sites hand on.
SHADER_HELPER_NAME = "create_compute_shader_from_spirv"
SHADER_HELPER_ANCHOR = f"static RID {SHADER_HELPER_NAME}("

# Producer call -> what kind of failure a bad result from it IS.
PROGRAM_SOURCE = "program-source"
DEVICE_OBJECT = "device-object"
ALLOCATION = "allocation"
PROPAGATED = "propagated"

_PRODUCERS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->compute_pipeline_create\s*\("), DEVICE_OBJECT),
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->storage_buffer_create\s*\("), ALLOCATION),
    (re.compile(rf"([\w.]+)\s*=\s*{SHADER_HELPER_NAME}\s*\("), PROPAGATED),
)

# Producer call -> kind, inside the helper itself. These are the three legs
# RenderingDevice::shader_create_from_spirv() fuses; the helper splits them precisely because
# they do not fail for the same reasons.
_HELPER_PRODUCERS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->shader_compile_spirv_from_source\s*\("), PROGRAM_SOURCE),
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->shader_compile_binary_from_spirv\s*\("), PROGRAM_SOURCE),
    (re.compile(r"([\w.]+)\s*=\s*[\w.>-]+->shader_create_from_bytecode\s*\("), DEVICE_OBJECT),
)
EXPECTED_HELPER_LEGS = 3

EXPECTED_ERROR = {
    PROGRAM_SOURCE: "ERR_COMPILATION_FAILED",
    DEVICE_OBJECT: "ERR_CANT_CREATE",
    ALLOCATION: "ERR_CANT_CREATE",
}

WHY = {
    PROGRAM_SOURCE: (
        "translating the generated GLSL to SPIR-V and reflecting that SPIR-V into a shader "
        "container are pure CPU functions of (source, driver target version) that create no "
        "device object, so they fail identically on every attempt. Returning the retryable "
        "code makes classify_sorter_creation_error() call the failure TRANSIENT, and the "
        "renderer recompiles the same failing source forever at the saturated backoff "
        "(#586 round-7)"
    ),
    DEVICE_OBJECT: (
        "the driver collapses an allocation failure and a genuine rejection into the same "
        "invalid RID (vkCreateShaderModule / vkCreateComputePipelines; the VkResult is "
        "formatted into a message and discarded), so this site CANNOT know which happened. "
        "Latching it turns one momentary host/device allocation failure into a renderer that "
        "publishes nothing for the rest of the session, while retrying a truly deterministic "
        "one costs a bounded build per saturated backoff (#586 round-9)"
    ),
    ALLOCATION: (
        "a failed buffer acquisition can succeed on a later attempt once pressure lifts, so it "
        "must stay classified TRANSIENT. Returning the deterministic code latches the sorter "
        "off until renderer teardown over one momentary VRAM blip, which is the permanent "
        "black screen #586 round-1 removed"
    ),
}

# Pins. Bump these only together with a real change to the number of covered sites.
EXPECTED_PROPAGATED_SITES = 5
EXPECTED_DEVICE_OBJECT_SITES = 5
EXPECTED_ALLOCATION_SITES = 3

# `!x.is_valid()` for RID results, `x.is_empty()` for the Vector<uint8_t> legs. Both are
# "this producer failed"; nothing else is accepted, so a check written some third way is
# reported as unbindable rather than silently skipped.
_FAILURE_CHECK_RE = re.compile(r"(?:!\s*([\w.]+)\s*\.is_valid\s*\(\s*\))|(?<![!\w])([\w.]+)\s*\.is_empty\s*\(\s*\)")
# Every `return <something>;` in a branch, whatever it returns. Anything that is not a bare
# `ERR_...` constant is reported as unreadable rather than skipped over -- skipping it is how
# the round-8 defect let a later site's return stand in for this one.
_ANY_RETURN_RE = re.compile(r"\breturn\b([^;]*);")
_ERROR_CODE_RE = re.compile(r"ERR_\w+")
_IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")
_ERROR_ASSIGN_RE = re.compile(r"\*\s*r_error\s*=\s*([^;]*);")
_LEAVES_FUNCTION_RE = re.compile(r"\breturn\b|\bERR_FAIL_\w*V\w*\s*\(")
_IF_HEAD_RE = re.compile(r"\bif\s*\(")
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


class Site(NamedTuple):
    """One producer -> failure-check -> reported-error triple inside a scoped function."""

    function: str
    variable: str
    kind: str
    returned: str
    expected: str


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


def _collect_producers(body: str, producers_spec) -> dict[str, str]:
    """Map object name -> producer kind, for every GPU object built in this function."""
    producers: dict[str, str] = {}
    for pattern, kind in producers_spec:
        for match in pattern.finditer(body):
            producers[match.group(1)] = kind
    return producers


def _match_delimiter(text: str, open_index: int, opener: str, closer: str) -> int | None:
    """Index of the `closer` matching the `opener` at `open_index`, or None."""
    depth = 0
    for i in range(open_index, len(text)):
        char = text[i]
        if char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return i
    return None


def _split_top_level_args(arguments: str) -> list[str]:
    """Split a call's argument text on top-level commas only."""
    args: list[str] = []
    depth = 0
    current = ""
    for char in arguments:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if char == "," and depth == 0:
            args.append(current.strip())
            current = ""
            continue
        current += char
    if current.strip():
        args.append(current.strip())
    return args


def _propagated_error_name(body: str, variable: str) -> tuple[str | None, str]:
    """The identifier a propagating site passes as `create_compute_shader_from_spirv`'s
    `Error *r_error`. Derived from the call, not assumed, so a site that stops passing one is
    a failure rather than an unchecked pass."""
    pattern = re.compile(rf"{re.escape(variable)}\s*=\s*{SHADER_HELPER_NAME}\s*\(")
    match = pattern.search(body)
    if match is None:
        return None, "its producer call could not be re-located"
    open_paren = match.end() - 1
    close_paren = _match_delimiter(body, open_paren, "(", ")")
    if close_paren is None:
        return None, "its producer call's argument list could not be closed"
    args = _split_top_level_args(body[open_paren + 1 : close_paren])
    if len(args) != 3:
        return None, (
            f"its producer call passes {len(args)} argument(s); it must pass an `Error *` "
            f"out-parameter as the third, because the helper's legs -- not the call site -- "
            f"decide whether the failure is deterministic"
        )
    error_arg = args[2]
    if not error_arg.startswith("&"):
        return None, (
            f"its producer call passes `{error_arg}` as the error out-parameter; the guard "
            f"needs a plain `&<local>` so it can require the branch to return exactly that "
            f"value"
        )
    name = error_arg[1:].strip()
    if not _IDENTIFIER_RE.fullmatch(name):
        return None, f"its error out-parameter `{error_arg}` is not a plain `&<local>`"
    return name, ""


def _checked_branch(body: str, check: re.Match[str]) -> tuple[str | None, str]:
    """The `{...}` body of the `if (...)` whose CONDITION contains this failure check.

    Returns (branch_body, "") on success, or (None, why) when the shape cannot be established.
    Nothing here is inferred from proximity: the branch is delimited by matching the
    condition's parentheses and then the branch's braces, so an error found inside it belongs
    to THIS failure and to no other."""
    head: re.Match[str] | None = None
    for candidate in _IF_HEAD_RE.finditer(body, 0, check.start()):
        head = candidate
    if head is None:
        return None, "is not inside any `if (...)` this guard can locate"
    open_paren = head.end() - 1
    close_paren = _match_delimiter(body, open_paren, "(", ")")
    if close_paren is None:
        return None, "is inside an `if (...)` whose condition this guard cannot close"
    if close_paren < check.end():
        # The nearest preceding `if` closes before the check, so the check is not a condition
        # at all (an assignment, a `while`, a lambda...). Refuse rather than guess.
        return None, (
            "is not part of an `if (...)` condition; this guard only classifies the "
            "`if (<producer failed>) { ...; return ERR_...; }` shape"
        )
    cursor = close_paren + 1
    while cursor < len(body) and body[cursor].isspace():
        cursor += 1
    if cursor >= len(body) or body[cursor] != "{":
        return None, (
            "has an UNBRACED failure branch. This guard will not guess where an unbraced "
            "statement ends -- an unbounded search is exactly how a later site's return got "
            "credited to this one (#586 round-8). Brace the branch"
        )
    close_brace = _match_delimiter(body, cursor, "{", "}")
    if close_brace is None:
        return None, "has a failure branch whose `{` this guard cannot close"
    return body[cursor + 1 : close_brace], ""


def _checked_variable(check: re.Match[str]) -> str:
    return check.group(1) or check.group(2)


def _collect_sites(function: str, body: str, producers: dict[str, str]) -> tuple[list[Site], list[str]]:
    """For each tracked producer's failure check, the error reported BY THAT CHECK'S OWN branch.

    The branch is parsed, not approximated: `_checked_branch()` delimits the `if (...) {...}`
    the check controls, and the error must be found inside it. A branch that reports and falls
    through therefore fails here instead of silently inheriting the next site's return."""
    sites: list[Site] = []
    problems: list[str] = []
    seen: set[str] = set()
    for check in _FAILURE_CHECK_RE.finditer(body):
        variable = _checked_variable(check)
        kind = producers.get(variable)
        if kind is None:
            # Not something this guard classifies (e.g. a caller-supplied RID). Ignored on
            # purpose: guessing at an object whose origin is unknown is how a check starts
            # asserting the wrong thing.
            continue
        seen.add(variable)
        if kind == PROPAGATED:
            expected, why = _propagated_error_name(body, variable)
            if expected is None:
                problems.append(
                    f"{function}: `{variable}` comes from {SHADER_HELPER_NAME}(), but {why}. "
                    f"The helper's three legs do not share one error class, so the site must "
                    f"pass `&<local>` and return it."
                )
                continue
        else:
            expected = EXPECTED_ERROR[kind]
        branch, why = _checked_branch(body, check)
        if branch is None:
            problems.append(
                f"{function}: `{variable}` has a failure check, but that check {why}, so "
                f"this guard cannot bind an error class to the failure. Keep the failure branch "
                f"in the `if (<{variable} failed>) {{ ...; return {expected}; }}` shape, or extend "
                f"tests/ci/check_sorter_error_class_parity.py."
            )
            continue
        returned = [match.group(1).strip() for match in _ANY_RETURN_RE.finditer(branch)]
        if not returned:
            problems.append(
                f"{function}: `{variable}` has a failure check, but its failure branch "
                f"does not RETURN -- it reports and falls through into code that then uses an "
                f"invalid {kind}. Return {expected} from the branch. (This is the "
                f"shape #586 round-8 found the previous, unbounded version of this guard could "
                f"not see: the search walked on and credited the next site's return to this one.)"
            )
            continue
        if kind == PROPAGATED:
            unreadable = sorted({value for value in returned if value != expected})
            if unreadable:
                problems.append(
                    f"{function}: `{variable}` comes from {SHADER_HELPER_NAME}(), whose legs "
                    f"already decided the error class, but its failure branch returns "
                    f"{', '.join('`return ' + value + ';`' for value in unreadable)} instead of "
                    f"the `{expected}` it passed in. A constant typed here re-decides -- and can "
                    f"silently contradict -- what the helper reported."
                )
                continue
        else:
            unreadable = sorted({value for value in returned if not _ERROR_CODE_RE.fullmatch(value)})
            if unreadable:
                problems.append(
                    f"{function}: `{variable}`'s failure branch returns "
                    f"{', '.join('`return ' + value + ';`' for value in unreadable)}, which this "
                    f"guard cannot read as an error class. It must return a bare "
                    f"{expected} so the class the classifier sees is visible in the "
                    f"source; do not route it through a helper or a non-ERR_ constant."
                )
                continue
        codes = sorted(set(returned))
        if len(codes) != 1:
            problems.append(
                f"{function}: `{variable}`'s failure branch returns more than one error class "
                f"({', '.join(codes)}). classify_sorter_creation_error() sees exactly one, so "
                f"the branch must return exactly one; split the check instead."
            )
            continue
        sites.append(Site(function, variable, kind, codes[0], expected))

    for variable, kind in producers.items():
        if variable not in seen:
            problems.append(
                f"{function}: `{variable}` is built by a {kind} call but its result is never "
                f"checked for failure. An unchecked GPU object is a worse defect than a "
                f"mislabelled error -- check it and report the failure on it."
            )
    return sites, problems


def _check_helper_legs(source_text: str) -> tuple[int, list[str]]:
    """The round-9 half: inside the helper, each leg must report the class ITS OWN failure can
    be. This is what makes the propagating sites above meaningful -- without it, the helper
    could report one constant for everything and the sites would faithfully hand it on."""
    problems: list[str] = []
    body = _function_body(source_text, SHADER_HELPER_ANCHOR)
    if body is None:
        return 0, [
            f"could not parse `{SHADER_HELPER_ANCHOR}` in {SORTER_SOURCE.name}. The sites that "
            f"propagate its error are only as good as its leg mapping, so a rename must update "
            f"SHADER_HELPER_ANCHOR here -- do not drop the check."
        ]
    producers = _collect_producers(body, _HELPER_PRODUCERS)
    if len(producers) != EXPECTED_HELPER_LEGS:
        found = ", ".join(sorted(producers)) or "none"
        problems.append(
            f"{SHADER_HELPER_NAME}(): expected {EXPECTED_HELPER_LEGS} separately-checked legs "
            f"(SPIR-V compilation, container reflection, driver shader object) but found "
            f"{len(producers)}: {found}. Fusing them back into "
            f"RenderingDevice::shader_create_from_spirv() merges a deterministic failure with "
            f"an indistinguishable driver one, which is exactly the #586 round-9 defect."
        )
        return len(producers), problems

    seen: set[str] = set()
    for check in _FAILURE_CHECK_RE.finditer(body):
        variable = _checked_variable(check)
        kind = producers.get(variable)
        if kind is None:
            continue
        seen.add(variable)
        branch, why = _checked_branch(body, check)
        if branch is None:
            problems.append(f"{SHADER_HELPER_NAME}(): `{variable}`'s failure check {why}.")
            continue
        reported = [match.group(1).strip() for match in _ERROR_ASSIGN_RE.finditer(branch)]
        if not reported:
            problems.append(
                f"{SHADER_HELPER_NAME}(): `{variable}`'s failure branch does not write "
                f"`*r_error`, so the propagating call sites hand on whatever an earlier leg "
                f"left there. Report {EXPECTED_ERROR[kind]} in this branch."
            )
            continue
        if not _LEAVES_FUNCTION_RE.search(branch):
            problems.append(
                f"{SHADER_HELPER_NAME}(): `{variable}`'s failure branch reports an error and "
                f"FALLS THROUGH into code that uses an invalid result. Leave the function."
            )
            continue
        codes = sorted(set(reported))
        if len(codes) != 1 or not _ERROR_CODE_RE.fullmatch(codes[0]):
            problems.append(
                f"{SHADER_HELPER_NAME}(): `{variable}`'s failure branch reports "
                f"{', '.join(codes)}, which this guard cannot read as exactly one error class."
            )
            continue
        if codes[0] != EXPECTED_ERROR[kind]:
            problems.append(
                f"{SHADER_HELPER_NAME}(): `{variable}` comes from a {kind} call, so its failure "
                f"must report {EXPECTED_ERROR[kind]}, but it reports {codes[0]}. Why it "
                f"matters: {WHY[kind]}."
            )
    for variable, kind in producers.items():
        if variable not in seen:
            problems.append(
                f"{SHADER_HELPER_NAME}(): `{variable}` is produced by a {kind} call whose "
                f"result is never checked, so its failure is reported as another leg's class."
            )
    return len(producers), problems


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
        producers = _collect_producers(body, _PRODUCERS)
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

    helper_legs, helper_problems = _check_helper_legs(source_text)
    problems.extend(helper_problems)

    for site in sites:
        if site.returned != site.expected:
            problems.append(
                f"{site.function}: `{site.variable}` comes from a {site.kind} call, so its "
                f"failure must return {site.expected}, but it returns {site.returned}. Why it "
                f"matters: {WHY.get(site.kind, WHY[DEVICE_OBJECT])}."
            )

    propagated_sites = sum(1 for site in sites if site.kind == PROPAGATED)
    device_object_sites = sum(1 for site in sites if site.kind == DEVICE_OBJECT)
    allocation_sites = sum(1 for site in sites if site.kind == ALLOCATION)
    if (
        propagated_sites != EXPECTED_PROPAGATED_SITES
        or device_object_sites != EXPECTED_DEVICE_OBJECT_SITES
        or allocation_sites != EXPECTED_ALLOCATION_SITES
    ):
        problems.append(
            f"covered site count changed: {propagated_sites} propagating site(s), "
            f"{device_object_sites} device-object site(s) and {allocation_sites} allocation "
            f"site(s), pinned at {EXPECTED_PROPAGATED_SITES}, {EXPECTED_DEVICE_OBJECT_SITES} "
            f"and {EXPECTED_ALLOCATION_SITES}. A derived guard with nothing left to check still "
            f"prints PASSED, so this has to be a deliberate edit: update the pins in the same "
            f"diff that adds or removes a site."
        )

    if problems:
        for problem in problems:
            print(f"{prefix} FAIL {problem}")
        print(f"{prefix} {len(problems)} problem(s); {len(sites)} site(s) inspected.")
        return 1

    print(
        f"{prefix} PASSED - {propagated_sites} site(s) propagate {SHADER_HELPER_NAME}()'s "
        f"error across its {helper_legs} separately-classified legs, "
        f"{device_object_sites} device-object site(s) and {allocation_sites} allocation "
        f"site(s) return {EXPECTED_ERROR[ALLOCATION]}, across "
        f"{len(SCOPED_FUNCTIONS)} scoped function(s)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
