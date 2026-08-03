#!/usr/bin/env python3
"""Guard: the module's environment skips are a counted, shrink-only inventory (#595).

## The failure this guards against

An "environment skip" is a test case that returns early because the machine
could not supply something it needs — no `RenderingDevice`, no
`WorkerThreadPool`, no editor runtime. In this tree such a skip is unavoidably

    MESSAGE("Skipping test - ..."); return;

because the vendored doctest (2.4.12) has **no runtime skip API**:
`doctest::skip(bool)` is a registration-time decorator evaluated during static
registration, before `Main::test_setup()` has bootstrapped any device, and
`test_case_skipped` fires only for filter/decorator skips. And because the build
defines `DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS`, no assertion
unwinds either — so there is no shape available other than "emit something, then
return".

doctest scores a `TEST_CASE` that returns as **PASSED**. So an environment skip
is, in the report, indistinguishable from a case that ran and verified
something. The emitted text is the only distinguishing signal there is, which is
why #595 introduces the fixed token `GS_ENV_SKIP:` (see
`modules/gaussian_splatting/tests/test_macros.h`) and repairs the runtime
detector in `tests/ci/run_module_tests.py`.

This guard is the **static** half of that. The runtime detector can only see the
lanes that actually run; this one sees every skip site in the source tree,
including the ones in cases no lane currently executes.

## Unit of counting

A **whole-file** scan of `modules/gaussian_splatting/tests/*.{h,cpp}` (excluding
`test_macros.h`, which defines the macros rather than calling them), with
comments stripped first. Sites are counted wherever they appear — inside a
`TEST_CASE` body, in a helper function, or at file scope. Nothing is attributed
to an enclosing case.

That is a deliberate choice, and it is why this number can differ from a
per-`TEST_CASE`-body count of the same corpus: a skip in a shared helper is one
site here and zero-or-many there. If you reconcile against a case-scoped count,
reconcile on the unit before arguing about the number. (Case attribution by
brace depth in particular is unsound in this corpus: a body's braces do not end
where the case does, and over-running attributes the NEXT case's macro to the
previous one. Bound at the next `TEST_CASE`, not by depth.)

## SHAPE CONTRACT — what is counted, and what is knowingly not

This is a written contract, not whatever the regexes happen to do. Both this
guard and `run_module_tests.py`'s runtime detector implement exactly this list,
and they must be changed together.

**Counted:**

1. `macro`    — a call site of one of the canonical environment-skip macros:
   `REQUIRE_GPU_DEVICE()`, `REQUIRE_LOCAL_GPU_DEVICE()`,
   `REQUIRE_STREAMING_CAPABLE()`, `REQUIRE_WORKER_THREAD_POOL()`, or a direct
   `GS_ENV_SKIP()`.
2. `message`  — a `MESSAGE("…")` / `MESSAGE(vformat("…"))` whose first string
   literal **begins** with skip prose (`Skipping…`, `Skipped…`), case-insensitive.
   The **PREFIX FORM**. These are the not-yet-converted legacy sites; #595
   freezes them rather than rewriting them (that is slice GS-595-B).
3. `warn`     — a `WARN_PRINT(…)` / `ERR_PRINT(…)` whose literal mentions
   skipping **anywhere**. Only 2 exist; they are matched loosely because the
   population is small enough to have been read individually.

**NOT counted, knowingly:**

4. The **EMBEDDED FORM** — a `MESSAGE(…)` whose literal mentions skipping
   mid-sentence rather than at the start:

       MESSAGE("Cache file not created (caching may be disabled); skipping version guard test");
       MESSAGE("[TileRenderer] RenderingServer not available, skipping regression test");
       MESSAGE("Renderer unavailable (headless mode) - skipping renderer state checks");

   These are genuine environment skips — the case returns without asserting —
   and this detector does not see them. **Measured on this tree: 9 sites across
   4 files** (`test_ply_importer.h` 4, `test_shadow_instance_subset.h` 2,
   `tile_renderer_regression_test.cpp` 2, `test_node_bootstrap.h` 1).

   This matters more than the count suggests, because the shape is not evenly
   distributed. `test_shadow_instance_subset.h` and `test_node_bootstrap.h`
   contain **only** embedded-form skips, so both files are entirely absent from
   the baseline below — and both hold `[SceneTree]` cases, i.e. the strict
   `GaussianSplatting [SceneTree]` lane. Under a prefix-only detector that lane
   can report zero skip markers while skipping at runtime, which is exactly the
   "gated but not executing" failure this task exists to eliminate. Do not read
   a zero here as proof a lane executes.

   The gap is left OPEN on purpose. Widening the detector inside this slice
   would move the baseline number and the enforcement blast radius in the same
   change, and a ratchet whose definition shifts mid-slice is not a ratchet. It
   is a named follow-on (GS-595-E), to be done as its own measured step. The
   equally wrong fix is rewriting the messages to fit the detector: that is the
   tail wagging the dog and it would invalidate the inventory being frozen here.

5. `print_line(…)` skip prose. The corpus uses `print_line` for ordinary
   logging; matching it would trade a precise inventory for a noisy one.
6. `REQUIRE_RENDERING_DEVICE_SINGLETON()`. It `FAIL`s rather than skipping, so
   it is not part of the silent-pass surface at all.

## DECLARED LIMITATIONS — the recognised surface is a text scan, not a compiler

This guard reads source text. It is a ratchet against the pattern spreading, NOT
a proof that the corpus is free of environment skips. Every shape below is a
real skip that it does not see; each was reproduced as undetected rather than
assumed:

* a message built by concatenation — `MESSAGE(String("Skipping…") + reason)`;
* adjacent string literals — `MESSAGE("Skip" "ping - no device")`;
* a raw string literal — `MESSAGE(R"(Skipping - no device)")`;
* `INFO(…)`, `CAPTURE(…)` and any other doctest logging macro;
* `print_line(…)` (excluded on purpose, above);
* a **bare `return;`** with no message at all — the shape with no textual
  evidence whatsoever, and therefore the one nothing here can ever catch;
* the EMBEDDED prose form (item 4 above), which is measured and tracked.

### Named limitation: `WARN_PRINT` skips are static-only

`WARN_PRINT`-based skips are counted **statically** by this guard but **not** at
runtime by `run_module_tests.py`. `WARN_PRINT` does not go through doctest; it
reaches the stream as Godot's own `WARNING: <text>` framing, and counting that
shape at runtime would also count ordinary logging (38 `WARN_PRINT`/`ERR_PRINT`
literals engine-wide mention skipping, including this module's own
`gaussian_streaming.cpp` "[Streaming] Skipping Morton sort…"; a captured sample
contains a renderer line reading "…collected but skipped because no renderer can
be attached"). At an allowance of 0, one false positive fails a lane, so the
runtime detector deliberately does not look.

Exactly TWO cases rely on `WARN_PRINT` exclusively, both in
`test_painterly_pipeline.h`:

* `[GaussianSplatting][Painterly] Shader permutations compile and headless render succeeds`
  — `WARN_PRINT("RenderingDevice unavailable - skipping painterly shader compilation test")`
* `[GaussianSplatting][Painterly] Animated camera path produces varying frames`
  — `WARN_PRINT("RenderingDevice unavailable - skipping painterly animation validation")`

A lane containing only those cases will report **0 runtime skip markers while
genuinely skipping**. Both currently sit only in the advisory
`GaussianSplatting [untagged]` lane, so no strict lane is affected today — but
promoting `[untagged]`, or introducing a strict `[Painterly]` lane, on the
strength of a 0 marker count would gate a lane that does not execute.

This is the same trap as the embedded-prose gap above, from a different
direction: **the detector's definition decides which lanes look safe to promote
to strict.** Read a 0 as "no marker of a recognised shape", never as "this lane
executes".

### Named limitation: 232 of 384 sites are in `[RequiresGPU]` cases

**60.4%** of the inventory (232 sites: 21 `macro`, 211 `message`) lives in cases
tagged `[RequiresGPU]`. Those run only under `tests/ci/run_gpu_harness.py`, and
**that harness performs no skip detection at all** — it does not consume
`DOCTEST_SKIP_MARKER_RE` and has no allowance. So for the majority of the
inventory there is a static count and *no runtime enforcement whatsoever*.

This is by far the largest of the three declared gaps — 20× the embedded-prose
gap (9) and 100× the `WARN_PRINT` gap (2) — and it is the reason the static
inventory exists at all: it is the only mechanism that sees those sites. Adding
detection to the GPU harness is follow-on GS-595-C. Until then, do not read a
green GPU batch as evidence that its cases executed.

### The macro surface, and why it is derived

It used to be a hand-written five-name list, which meant a new wrapper macro
defined in `test_macros.h` — the one file the site scan excludes — could be
called from N files and yield ZERO sites. An invariant guarded by a hand-written
list is already broken, so that list is now DERIVED from the header itself
(`skip_macro_names()`), for BOTH function-like `#define NAME(args) …` and
object-like `#define NAME do { … } while (0)` forms, in both the compliant and
the regressed shape.

That claim is narrower than it sounds, and the narrowness is the point: it holds
for a macro DEFINED IN `test_macros.h` whose body textually contains
`GS_ENV_SKIP(` or skip prose. A wrapper defined in another header, or one whose
body reaches a skip only through a further indirection this scan cannot follow,
is still invisible.

Closing the remaining shapes needs a real C++ parser, not a text scan. Do not
read a passing run as "there are no undeclared skips"; read it as "no skip of a
RECOGNISED shape was added".

Comments are stripped before matching, so a comment that merely *mentions*
`REQUIRE_GPU_DEVICE()` does not inflate the inventory — which is exactly what a
raw `git grep -c` did during the #595 investigation.

## The macro contract check

The macros are only worth having if they still emit the token. So the guard also
parses `test_macros.h` and fails if any of the four macro bodies stops routing
its skip through `GS_ENV_SKIP`, or if `GS_ENV_SKIP` stops emitting the literal
`GS_ENV_SKIP: ` token. Without this, reverting the macro bodies to free-form
`MESSAGE(...)` would leave the runtime detector blind again while every static
count stayed identical.

## Baseline

`environment_skip_baseline.json` records, per file, a **fingerprint per site**
plus that file's count, an owner, and the tracking issue for the conversion
slice that will retire it.

A count-only baseline is not enough: it licenses a swap — delete one skip, add a
different one in the same file, and the count is unchanged. The fingerprint
multiset reports both the removal and the addition.

The ratchet only turns one way:

* an unbaselined site FAILS as **new**;
* a baselined site that no longer exists FAILS as **stale**, with an instruction
  to lower the number, so the slack cannot be reoccupied.

`--write-baseline` regenerates the file **from this guard's own scan**, never by
hand. It refuses to write any entry that would ADD a fingerprint (set inclusion,
not net counts), so it cannot be used to launder a new skip into the baseline;
it can only record removals.

That check is UNCONDITIONAL, and there is **no create-from-nothing path at all**
— after two attempts at one. Version 1 nested the check inside
`if path.is_file()`, so deleting the baseline bypassed it. Version 2 moved
creation behind an explicit `--bootstrap-baseline` flag, which merely RELOCATED
the bypass: that flag wrote unconditionally, and the resulting diff (`count
1→2` plus one `sites` line) is indistinguishable from a legitimate shrink, so
"loud in review" was never a real mitigation. The flag is gone. A missing
baseline is a FAILURE with one instruction: restore the committed file.

`--rename OLD=NEW` re-keys an entry when a source file moves, so a rename is not
a deadlock between "the guard reports stale+new" and "the writer refuses to
add". It is constrained so it cannot become a laundering primitive: OLD must be
baselined AND **gone from disk**, and NEW must exist and be scanned. Without the
gone-from-disk rule it transfers credit between two live files — delete three
sites from A, add three to unrelated B, `--rename A=B`, green.

The `runtime_lane_allowance` block is validated on EVERY run, not only when a
lane trips, and is ratcheted shrink-only against the committed document: a new
lane or a raised number fails here. It previously passed through untouched, so a
hand-edited `allowed: 9999` on a real lane would have sat in the tree unnoticed.

Keys are repo-relative POSIX paths, not basenames: `tests/test_utils.h` and
`modules/gaussian_splatting/tests/test_utils.h` both exist, so a basename key
collides and one file's sites would silently mask the other's.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"
BASELINE_PATH = Path(__file__).resolve().parent / "environment_skip_baseline.json"

BASELINE_ISSUE = "https://github.com/klausi3D/godotGS/issues/595"
CONVERSION_SLICE = "GS-595-B"
BASELINE_OWNER = "gaussian-splatting-module"
BASELINE_SCHEMA_VERSION = 1

# The header that DEFINES the macros. Excluded from the site scan (a definition
# is not a skip site) and checked structurally instead, below.
MACRO_HEADER_NAME = "test_macros.h"

# Sites outside any TEST_CASE (helpers, file scope). A stable literal rather than
# a hash of nothing, so the key reads plainly in the baseline.
FILE_SCOPE = "<file-scope>"

# The token the canonical helper must emit into doctest's output. Kept in sync
# with tests/ci/run_module_tests.py:DOCTEST_SKIP_MARKER_RE and with
# modules/gaussian_splatting/tests/test_macros.h:GS_ENV_SKIP.
ENV_SKIP_TOKEN = "GS_ENV_SKIP:"

# The four macros that MUST route their skip through GS_ENV_SKIP. This list is
# an assertion about macros that exist TODAY -- if one of them disappears or
# stops routing, that is a regression and the contract check says so. It is NOT
# the list of macros the scanner recognises; see skip_macro_names() below.
GUARDED_MACRO_BODIES: tuple[str, ...] = (
    "REQUIRE_GPU_DEVICE",
    "REQUIRE_LOCAL_GPU_DEVICE",
    "REQUIRE_STREAMING_CAPABLE",
    "REQUIRE_WORKER_THREAD_POOL",
)

# Macros in test_macros.h that are NOT skips even though they mention the
# subject. REQUIRE_RENDERING_DEVICE_SINGLETON FAILs rather than skipping, so it
# is not part of the silent-pass surface at all.
NON_SKIP_MACROS: frozenset[str] = frozenset({"REQUIRE_RENDERING_DEVICE_SINGLETON"})

# Captures the macro name and whether a '(' follows IMMEDIATELY (no space),
# which is what distinguishes a function-like macro from an object-like one.
_DEFINE_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(?P<name>[A-Za-z_]\w*)(?P<paren>\()?", re.MULTILINE
)

# A C/C++ string literal, honouring backslash escapes.
_STRING_LITERAL = r'"(?P<text>(?:[^"\\]|\\.)*)"'

# `MESSAGE("Skip…")` and `MESSAGE(vformat("Skip…", …))`. `\s` spans newlines, so
# a message whose literal sits on a continuation line is still found, and each
# site is found exactly once (the scan runs over the whole file, not per line).
_MESSAGE_RE = re.compile(rf"\bMESSAGE\s*\(\s*(?:vformat\s*\(\s*)?{_STRING_LITERAL}")
_PRINT_RE = re.compile(
    rf"\b(?:WARN_PRINT|ERR_PRINT)\s*\(\s*(?:vformat\s*\(\s*)?{_STRING_LITERAL}"
)

# Prose that marks a MESSAGE as a skip: the literal must BEGIN with it. Anchoring
# at the start is what keeps ordinary diagnostics ("… skipped 3 chunks …") out of
# the inventory.
_SKIP_PROSE_PREFIX_RE = re.compile(r"^\s*skipp?(?:ing|ed)\b", re.IGNORECASE)
# For WARN_PRINT/ERR_PRINT the prose is conventionally trailing
# ("RenderingDevice unavailable - skipping …"), so a contains-match is used.
_SKIP_PROSE_ANYWHERE_RE = re.compile(r"\bskipp(?:ing|ed)\b", re.IGNORECASE)


def skip_macro_names(macro_header_text: str | None = None) -> tuple[str, ...]:
    """The macros whose expansion is an environment skip, DERIVED from the header.

    A hand-written list of macro names is an invariant guarded by a hand-written
    list, i.e. already broken: add a new wrapper macro to `test_macros.h` -- the
    one file the site scan excludes -- call it from N files, and a hard-coded
    scanner reports ZERO sites for N real skips.

    So the set is derived: every macro defined in `test_macros.h` whose body
    either routes through `GS_ENV_SKIP(` or emits skip prose of its own. A new
    wrapper is picked up automatically, in both the compliant and the regressed
    form.

    BOTH macro forms are derived, and the distinction matters at the call site:

    * function-like — `#define NAME(args) …`, called as `NAME(...)`;
    * object-like   — `#define NAME do { … } while (0)`, called as `NAME;`.

    An earlier version only matched `#define NAME(`, and only emitted call
    patterns ending in `\\(`. So
    `#define GS_SKIP_NO_GPU do { GS_ENV_SKIP("no gpu"); return; } while (0)`
    was invisible in both places at once: absent from the derived set, and
    unmatchable even if present. The two are returned separately because a bare
    `NAME` needs a word-boundary match, not a paren match.

    `GS_ENV_SKIP` itself is always included (it is the canonical helper, and a
    direct call in a test is a site). Macros in NON_SKIP_MACROS are always
    excluded -- they FAIL rather than skip.
    """
    if macro_header_text is None:
        header = MODULE_TESTS_DIR / MACRO_HEADER_NAME
        macro_header_text = (
            header.read_text(encoding="utf-8", errors="replace") if header.is_file() else ""
        )
    text = strip_comments(macro_header_text)

    function_like = {"GS_ENV_SKIP"}
    object_like: set[str] = set()
    for match in _DEFINE_RE.finditer(text):
        name = match.group("name")
        if name in NON_SKIP_MACROS or name == "GS_ENV_SKIP":
            continue
        body = _macro_body(text, name)
        if body is None:
            continue
        emits_skip = "GS_ENV_SKIP(" in body or any(
            _SKIP_PROSE_ANYWHERE_RE.search(m.group("text"))
            for pattern in (_MESSAGE_RE, _PRINT_RE)
            for m in pattern.finditer(body)
        )
        if not emits_skip:
            continue
        if match.group("paren"):
            function_like.add(name)
        else:
            object_like.add(name)
    return tuple(sorted(function_like)), tuple(sorted(object_like))


def _macro_call_patterns(
    names: tuple[tuple[str, ...], tuple[str, ...]]
) -> list[tuple[str, re.Pattern[str]]]:
    """(macro_name, call-site pattern) for every derived skip macro.

    A function-like macro is only a call when followed by `(`; an object-like one
    is a call as a bare identifier. Matching the latter with `\\s*\\(` finds
    nothing, which is how an object-like wrapper produced `0 new, 0 stale` for
    real skips.

    One pattern per name rather than a single alternation, so the matched name is
    unambiguous without relying on alternation order (a shorter name that is a
    prefix of a longer one would otherwise decide the label).
    """
    function_like, object_like = names
    patterns: list[tuple[str, re.Pattern[str]]] = []
    for name in function_like:
        patterns.append((name, re.compile(rf"\b{re.escape(name)}\s*\(")))
    for name in object_like:
        # `(?!\s*\()` so an object-like name that is ALSO used call-style is not
        # double-counted against a function-like sibling.
        patterns.append((name, re.compile(rf"\b{re.escape(name)}\b(?!\s*\()")))
    return patterns


def strip_comments(text: str) -> str:
    """Blank out `//` and `/* */` comments, preserving line structure and literals.

    Line-for-line and character-for-character: every removed character becomes a
    space and every newline is kept, so a byte offset in the output maps to the
    same line as in the input. String and char literals are preserved verbatim —
    unlike the sibling guard in check_require_null_deref.py, this guard needs to
    READ the literals.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            for j in range(i, end):
                out.append("\n" if text[j] == "\n" else " ")
            i = end
            continue
        if ch in ('"', "'"):
            j = i + 1
            closed = False
            while j < n and text[j] != "\n":
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == ch:
                    closed = True
                    break
                j += 1
            if closed:
                out.append(text[i : j + 1])
                i = j + 1
            else:
                # Not a literal (a digit separator like 1'000, a stray
                # apostrophe in a comment we already passed, ...). Keep the
                # character rather than swallowing the rest of the file.
                out.append(ch)
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _normalize(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def fingerprint(kind: str, detail: str, case: str = FILE_SCOPE) -> str:
    """Stable identity for one skip SITE, independent of its line number.

    Line numbers are deliberately not part of it: a line-keyed baseline goes
    stale on every unrelated edit above it, which trains people to regenerate
    without reading — which is how a guard becomes a formality.

    For a `macro` site the detail IS the macro name and is used verbatim: the
    call sites are homogeneous (`REQUIRE_GPU_DEVICE();`) and carry no other
    distinguishing text, so hashing would only obscure the report.

    ## Why the enclosing case is part of the key

    Without it this identifies a SHAPE, not a site. Every `REQUIRE_GPU_DEVICE()`
    in a file collapsed to the single key `macro|REQUIRE_GPU_DEVICE`, so deleting
    one and adding an identical one ELSEWHERE IN THE SAME FILE left every
    multiset comparison unchanged: the shrink-only inventory gained skipped
    coverage with no baseline edit and no report. The same swap was already
    closed at the FILE level (that is what per-file entries are for) and was
    still wide open at the site level.

    The enclosing `TEST_CASE` name is the right granularity: stable across
    unrelated edits (unlike a line number) and specific enough that moving a skip
    to a different case is correctly reported as one removal plus one addition.

    RESIDUAL, stated rather than implied: two identical skips inside the SAME
    case are still interchangeable, because they share a key. Distinguishing them
    would need positional information, which reintroduces exactly the staleness
    the line-number decision rejected. Swapping one duplicate for another within
    a single case is close to a no-op semantically, so this is where the
    precision/stability trade is deliberately settled.

    ## Why SHA1, deliberately, and why it must not be "upgraded"

    This digest is a CONTENT FINGERPRINT used as a ratchet key. It is not a
    security primitive: nothing here relies on collision resistance against an
    adversary, and the value is truncated to 10 hex characters anyway, which
    discards most of the digest whatever algorithm produced it. Its only job is
    to give the same skip text the same stable identity across runs.

    `usedforsecurity=False` states that intent to the runtime (it selects a
    non-FIPS backend) and to linters (bandit B324). It does NOT change the bytes
    of the digest, which is the property the baseline depends on.

    Do NOT switch this to sha256. Every one of the 384 keys in
    environment_skip_baseline.json is derived from this function, so changing
    the algorithm silently re-keys the entire ratchet: every existing entry
    reports as stale and every real site reports as new, and the obvious way to
    "fix" that is to regenerate -- which is exactly how a ratchet quietly resets
    to zero. If it ever must change, that is its own reviewed commit whose diff
    shows all 384 keys moving and says why.
    """
    case_digest = hashlib.sha1(
        _normalize(case).encode("utf-8"), usedforsecurity=False
    ).hexdigest()[:10]
    if kind == "macro":
        return f"macro|{detail}|{case_digest}"
    digest = hashlib.sha1(
        _normalize(detail).encode("utf-8"), usedforsecurity=False
    ).hexdigest()[:10]
    return f"{kind}|{digest}|{case_digest}"


# A doctest case header. The name is the first string literal. Bounding a case
# at the NEXT header (never by brace depth) is deliberate: a body's braces do not
# end where the case does, and over-running attributes the following case's macro
# to the previous one.
_CASE_RE = re.compile(r"\bTEST_CASE(?:_TEMPLATE)?\s*\(\s*\"((?:[^\"\\]|\\.)*)\"")



def _case_bounds(text: str) -> list[tuple[int, str]]:
    """(offset, case_name) for each case header, in source order."""
    return [(m.start(), m.group(1)) for m in _CASE_RE.finditer(text)]


def _enclosing_case(bounds: list[tuple[int, str]], offset: int) -> str:
    """The case a site belongs to: the last header at or before it."""
    owner = FILE_SCOPE
    for start, name in bounds:
        if start <= offset:
            owner = name
        else:
            break
    return owner


def scan_source(
    text: str,
    macro_names: tuple[tuple[str, ...], tuple[str, ...]] | None = None,
) -> list[tuple[int, str, str, str]]:
    """Return (line, kind, detail, case) for every environment-skip site.

    `case` is the enclosing TEST_CASE name, and it is part of a site's identity.
    Without it a fingerprint identifies a SHAPE, not a site: every
    `REQUIRE_GPU_DEVICE()` maps to `macro|REQUIRE_GPU_DEVICE`, so deleting one
    and adding an identical one elsewhere in the same file leaves both multiset
    comparisons unchanged and the inventory silently gains skipped coverage.
    Several committed files already hold duplicate fingerprints, so that was
    reachable today, not theoretical.

    `macro_names` is the (function_like, object_like) pair from
    skip_macro_names(), and defaults to deriving it at CALL time -- never a
    module-level default, which would bind once and make the unit tests silently
    scan against the real header.
    """
    if macro_names is None:
        macro_names = skip_macro_names()
    stripped = strip_comments(text)
    bounds = _case_bounds(stripped)
    sites: list[tuple[int, str, str, str]] = []

    def add(offset: int, kind: str, detail: str) -> None:
        sites.append(
            (_line_of(stripped, offset), kind, detail, _enclosing_case(bounds, offset))
        )

    for name, pattern in _macro_call_patterns(macro_names):
        for match in pattern.finditer(stripped):
            add(match.start(), "macro", name)

    for match in _MESSAGE_RE.finditer(stripped):
        literal = match.group("text")
        if _SKIP_PROSE_PREFIX_RE.search(literal):
            add(match.start(), "message", literal)

    for match in _PRINT_RE.finditer(stripped):
        literal = match.group("text")
        if _SKIP_PROSE_ANYWHERE_RE.search(literal):
            add(match.start(), "warn", literal)

    return sorted(sites)


SOURCE_SUFFIXES: frozenset[str] = frozenset({".h", ".hpp", ".cpp", ".cc", ".inc"})


def test_sources() -> list[Path]:
    """Every scanned source. See SCAN SCOPE in the module docstring.

    RECURSIVE under the module tests directory and covering .cc/.hpp/.inc as
    well as .h/.cpp, so a test moved into a subdirectory or written with another
    extension does not silently leave the inventory. Also covers the top level of
    the engine test tree, mirroring check_require_null_deref.py's scope, because
    tests/test_projection_math.cpp holds gaussian-relevant cases that
    check_test_lane_coverage.py already counts as module coverage.

    Both widenings currently add ZERO sites (measured: 0 files under
    subdirectories, 0 skip sites in tests/test_*.{cpp,h}); they are future-proofing,
    not a change to the frozen number.

    `test_macros.h` is EXCLUDED, deliberately: it DEFINES the macros, and a
    definition is not a call site. Counting it would make the inventory move
    whenever the macro is edited, and would double-count every skip. The
    exclusion used to be dangerous because the recognised macro list was
    hand-written, so a new wrapper defined in the excluded file was invisible;
    skip_macro_names() removes that hazard by DERIVING the list from that same
    file. test_macros.h is still read on every run -- by skip_macro_names() and
    by check_macro_contract() -- it is only excluded from site counting.
    """
    module_sources = [
        path
        for path in MODULE_TESTS_DIR.rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES and path.name != MACRO_HEADER_NAME
    ]
    engine_sources = [
        path
        for path in ENGINE_TESTS_DIR.glob("test_*")
        # tests/test_macros.h is upstream Godot's own macro header, not a test
        # body; excluded for the same reason as the module's, and excluding it
        # also keeps the basename keying collision-free.
        if path.is_file() and path.suffix in SOURCE_SUFFIXES and path.name != MACRO_HEADER_NAME
    ]
    return sorted(module_sources + engine_sources)


def source_key(path: Path) -> str:
    """The baseline key for a source: its repo-relative POSIX path.

    NOT the basename. `modules/gaussian_splatting/tests/test_utils.h` and
    `tests/test_utils.h` both exist in this tree, so a basename key collides and
    one file's sites would mask the other's -- silently, and in the direction of
    under-reporting. The sibling guard check_require_null_deref.py keys by
    basename and is exposed to the same hazard; it is not fixed here only
    because that is a different baseline and a different task.

    Falls back to the path relative to whichever scan root contains it (and
    finally to the bare name) so the unit tests, which point the scan at a
    tempdir outside ROOT, still get stable keys instead of a ValueError.
    """
    resolved = path.resolve()
    for base in (ROOT, MODULE_TESTS_DIR, ENGINE_TESTS_DIR):
        try:
            return resolved.relative_to(Path(base).resolve()).as_posix()
        except ValueError:
            continue
    return path.name


def scan_all() -> dict[str, list[tuple[int, str, str, str]]]:
    """Repo-relative path -> sites, for every test source that has any."""
    macro_names = skip_macro_names()
    results: dict[str, list[tuple[int, str, str, str]]] = {}
    for path in test_sources():
        sites = scan_source(path.read_text(encoding="utf-8", errors="replace"), macro_names)
        if sites:
            results[source_key(path)] = sites
    return results


def scan_fingerprints() -> dict[str, list[str]]:
    return {
        name: sorted(fingerprint(kind, detail, case) for _, kind, detail, case in sites)
        for name, sites in scan_all().items()
    }


def _macro_body(text: str, name: str) -> str | None:
    """The body of `#define <name>`, function-like or object-like, continuations joined.

    Matches both `#define NAME(args) ...` and `#define NAME ...`. Restricting
    this to the function-like form was how an object-like skip wrapper escaped
    the derived-macro set entirely.

    Returns None when the macro is not defined at all — which is itself a
    failure for the callers below.
    """
    pattern = re.compile(
        rf"^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b", re.MULTILINE
    )
    match = pattern.search(text)
    if not match:
        return None
    body: list[str] = []
    index = match.start()
    lines = text.split("\n")
    start_line = text.count("\n", 0, index)
    for line in lines[start_line:]:
        body.append(line)
        if not line.rstrip().endswith("\\"):
            break
    return "\n".join(body)


def check_macro_contract() -> list[str]:
    """The four macros must still route their skip through the canonical token.

    Without this check, reverting the macro bodies to free-form `MESSAGE(...)`
    would blind the runtime detector while every static count stayed identical —
    i.e. the guard would report a clean tree for a tree that had lost the very
    mechanism this task installs.
    """
    header = MODULE_TESTS_DIR / MACRO_HEADER_NAME
    if not header.is_file():
        return [f"Macro header missing: {header.relative_to(ROOT)}"]
    text = strip_comments(header.read_text(encoding="utf-8", errors="replace"))

    failures: list[str] = []

    helper = _macro_body(text, "GS_ENV_SKIP")
    if helper is None:
        failures.append(
            f"{MACRO_HEADER_NAME}: the canonical helper GS_ENV_SKIP(reason) is not defined. "
            f"Environment skips have no machine-readable marker without it ({BASELINE_ISSUE})."
        )
    elif f'"{ENV_SKIP_TOKEN} "' not in helper and f'"{ENV_SKIP_TOKEN}' not in helper:
        failures.append(
            f"{MACRO_HEADER_NAME}: GS_ENV_SKIP no longer emits the literal token "
            f"'{ENV_SKIP_TOKEN}'. tests/ci/run_module_tests.py counts that token in raw "
            f"doctest output; changing it silently un-counts every environment skip."
        )

    for name in GUARDED_MACRO_BODIES:
        body = _macro_body(text, name)
        if body is None:
            failures.append(f"{MACRO_HEADER_NAME}: {name} is not defined.")
            continue
        if "GS_ENV_SKIP(" not in body:
            failures.append(
                f"{MACRO_HEADER_NAME}: {name} does not route its skip through GS_ENV_SKIP(). "
                f"A free-form MESSAGE() there is scored as a PASS and is invisible to the "
                f"runtime detector ({BASELINE_ISSUE})."
            )
        elif re.search(r"(?<![A-Z_])MESSAGE\s*\(", body):
            failures.append(
                f"{MACRO_HEADER_NAME}: {name} still contains a raw MESSAGE(...) alongside "
                f"GS_ENV_SKIP(). Exactly one marker per skip, or the counts double."
            )
    return failures


def multiset_difference(left: list[str], right: list[str]) -> list[str]:
    """Elements of `left` not covered by `right`, honouring duplicates."""
    remaining = list(right)
    out: list[str] = []
    for item in left:
        if item in remaining:
            remaining.remove(item)
        else:
            out.append(item)
    return out


def _elide(text: str, limit: int) -> str:
    """Shorten for DISPLAY only. Never feed this to fingerprint()."""
    flat = _normalize(text)
    return flat if len(flat) <= limit else flat[: limit - 1] + "…"


def load_baseline(path: Path | None = None) -> tuple[dict[str, list[str]], list[str]]:
    """Read the baseline. A missing or malformed file is a FAILURE, never a pass.

    `path` resolves to the module global at CALL time, not at definition time, so
    the unit tests can redirect it at a tempdir. A default argument would bind the
    original value once and make every test silently exercise the real baseline.
    """
    path = BASELINE_PATH if path is None else path
    if not path.is_file():
        return {}, [
            f"Baseline file missing: {path.name}. Refusing to treat an absent baseline as "
            f"'nothing to report'. Generate it with --write-baseline."
        ]
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError, UnicodeDecodeError) as exc:
        return {}, [f"Baseline file is not readable JSON: {exc}"]
    if not isinstance(data, dict) or not isinstance(data.get("files"), dict):
        return {}, ["Baseline file must be an object with a 'files' object."]
    out: dict[str, list[str]] = {}
    for name, entry in data["files"].items():
        if not isinstance(entry, dict):
            return {}, [f"Baseline entry '{name}' must be an object."]
        sites = entry.get("sites")
        if not isinstance(sites, list) or not all(isinstance(s, str) for s in sites):
            return {}, [f"Baseline entry '{name}' must carry a 'sites' list of strings."]
        count = entry.get("count")
        if not isinstance(count, int) or isinstance(count, bool) or count != len(sites):
            return {}, [
                f"Baseline entry '{name}': 'count' ({count!r}) does not equal len(sites) "
                f"({len(sites)}). A de-synced count is a corrupt baseline, not a warning."
            ]
        for field in ("owner", "issue_url", "conversion_slice"):
            if not isinstance(entry.get(field), str) or not entry[field].strip():
                return {}, [f"Baseline entry '{name}' is missing a non-empty '{field}'."]
        out[name] = sorted(sites)
    return out, []


def build_baseline_document(
    found: dict[str, list[str]],
    previous: dict | None,
    new_renames: dict[str, str] | None = None,
) -> dict:
    """Render the on-disk baseline document from a scan result."""
    document = {
        "schema_version": BASELINE_SCHEMA_VERSION,
        "issue_url": BASELINE_ISSUE,
        "conversion_slice": CONVERSION_SLICE,
        "note": (
            "Per-site fingerprints of environment-skip sites in the Gaussian Splatting "
            "module tests. Generated by tests/ci/check_environment_skip_marker.py "
            "(--write-baseline). Entries may only be REMOVED as sites are converted or "
            "deleted, never added by hand."
        ),
        "counting_unit": (
            "Whole-file scan, recursive, of modules/gaussian_splatting/tests/ plus the "
            "top level of tests/ (.h/.hpp/.cpp/.cc/.inc), excluding both test_macros.h "
            "headers, comments stripped. Keys are repo-relative POSIX paths, not "
            "basenames. Sites are counted wherever they appear, including helper "
            "functions and file scope; they are NOT attributed to an enclosing "
            "TEST_CASE. Reconcile the unit before reconciling the number against any "
            "case-scoped count."
        ),
        "known_gap": {
            "shape": "embedded prose",
            "description": (
                "MESSAGE(...) whose literal mentions skipping mid-sentence rather than "
                "at the start is a genuine environment skip and is NOT counted here. "
                "Two of the affected files contain ONLY this shape and are therefore "
                "absent from 'files' entirely, despite skipping at runtime."
            ),
            "measured_sites": 9,
            "measured_files": [
                "test_node_bootstrap.h",
                "test_ply_importer.h",
                "test_shadow_instance_subset.h",
                "tile_renderer_regression_test.cpp",
            ],
            "invisible_files": [
                "test_node_bootstrap.h",
                "test_shadow_instance_subset.h",
            ],
            "affected_strict_lane": "GaussianSplatting [SceneTree]",
            "follow_on_slice": "GS-595-E",
            "warning": (
                "A zero in this inventory is NOT evidence that a lane executes. Do not "
                "promote a lane to strict on the strength of this file alone."
            ),
        },
        "runtime_lane_allowance": (previous or {}).get("runtime_lane_allowance", {}),
        # Renames recorded by --write-baseline --rename, so the base comparison
        # can re-key the REFERENCE instead of reading a legitimate move as pure
        # growth. Append-only and purely re-keying: it can never introduce a
        # fingerprint that was not already baselined at the base.
        "rename_ledger": (
            list((previous or {}).get("rename_ledger", []))
            + [{"from": old_key, "to": new_key} for old_key, new_key in sorted((new_renames or {}).items())]
        ),
        "files": {
            name: {
                "owner": BASELINE_OWNER,
                "issue_url": BASELINE_ISSUE,
                "conversion_slice": CONVERSION_SLICE,
                "count": len(prints),
                "sites": sorted(prints),
            }
            for name, prints in sorted(found.items())
        },
    }
    return document


def _serialize(document: dict) -> str:
    return json.dumps(document, indent=2, ensure_ascii=False, sort_keys=False) + "\n"


def _parse_renames(pairs: list[str]) -> tuple[dict[str, str], list[str]]:
    """`OLD=NEW` arguments -> {old_key: new_key}."""
    renames: dict[str, str] = {}
    errors: list[str] = []
    for pair in pairs:
        old, sep, new = pair.partition("=")
        if not sep or not old.strip() or not new.strip():
            errors.append(f"--rename expects OLD=NEW, got {pair!r}.")
            continue
        renames[old.strip()] = new.strip()
    return renames, errors


ALLOWANCE_REQUIRED_FIELDS: tuple[str, ...] = (
    "allowed",
    "owner",
    "reason",
    "issue_url",
    "expires_utc",
)


def _load_allowance(document: dict) -> tuple[dict[str, int], list[str]]:
    """Parse `runtime_lane_allowance`, validating every entry. Never fail-open."""
    raw = document.get("runtime_lane_allowance", {})
    if not isinstance(raw, dict):
        return {}, ["'runtime_lane_allowance' must be an object."]
    out: dict[str, int] = {}
    failures: list[str] = []
    for lane, entry in sorted(raw.items()):
        where = f"runtime_lane_allowance['{lane}']"
        if not isinstance(entry, dict):
            failures.append(
                f"{where} must be an object carrying {{{', '.join(ALLOWANCE_REQUIRED_FIELDS)}}}, "
                f"got {entry!r}. A bare number is a silencer with no owner and no expiry."
            )
            continue
        missing = [f for f in ALLOWANCE_REQUIRED_FIELDS if f not in entry]
        if missing:
            failures.append(f"{where} is missing required field(s): {', '.join(missing)}.")
            continue
        allowed = entry["allowed"]
        if not isinstance(allowed, int) or isinstance(allowed, bool) or allowed < 0:
            failures.append(f"{where}.allowed must be a non-negative integer, got {allowed!r}.")
            continue
        for field in ("owner", "reason", "issue_url"):
            if not isinstance(entry[field], str) or not entry[field].strip():
                failures.append(f"{where}.{field} must be a non-empty string.")
        out[lane] = allowed
    return out, failures


def check_allowance(document: dict, previous: dict | str | None) -> list[str]:
    """Validate the allowance ALWAYS, and ratchet it shrink-only.

    Two holes this closes. The allowance used to be read only by
    run_module_tests.py, and only when a lane actually tripped -- so a
    hand-edited `allowed: 9999` on a real lane sat in the tree passing every
    check until the day it silently swallowed nine thousand skips. And the
    static guard copied the key through verbatim on every regeneration, so
    nothing ever looked at it.

    Now: the schema is validated on every run, and the number may only ever
    SHRINK relative to the document AT THE REVIEW BASE. Raising one, or adding a
    lane, is a deliberate act that has to happen somewhere a reviewer looks --
    not a side effect of regenerating the site inventory.

    `previous` MUST come from the review base, never from HEAD. See
    resolve_base_sha(): in CI, HEAD is the proposed commit, so a HEAD-relative
    reference compares the change with itself and permits any increase.
    ABSENT_AT_BASE means the file is being introduced by this change; there is
    genuinely nothing to ratchet against, and the caller reports that explicitly.
    """
    current, failures = _load_allowance(document)
    if failures or previous is None or previous == ABSENT_AT_BASE:
        return failures
    assert isinstance(previous, dict)
    baseline_allowance, previous_failures = _load_allowance(previous)
    if previous_failures:
        return [f"previously committed allowance is malformed: {f}" for f in previous_failures]
    for lane, allowed in sorted(current.items()):
        if lane not in baseline_allowance:
            failures.append(
                f"runtime_lane_allowance['{lane}'] is NEW. An allowance may only shrink; "
                f"adding a lane needs its own reviewed change, with the measurement that "
                f"justifies it."
            )
        elif allowed > baseline_allowance[lane]:
            failures.append(
                f"runtime_lane_allowance['{lane}'] rose {baseline_allowance[lane]} -> {allowed}. "
                f"This ratchet only turns one way: lower the skips, do not raise the tolerance."
            )
    return failures


# Where the review base comes from, in priority order. CI supplies the PR base
# sha; .github/workflows/agentic_pr_gate.yml already passes
# `github.event.pull_request.base.sha` around, so this reads the same value.
BASE_REF_ENV_VARS: tuple[str, ...] = (
    "GS_CI_ENV_SKIP_BASE_REF",
    "GS_CI_BASE_REF",
    "GITHUB_BASE_SHA",
    # GitHub Actions sets this automatically on every `pull_request` event, and
    # it is a BRANCH NAME (`master`, `gs/650-quarantine-ratchet`), never a sha.
    # Omitting it meant the one variable CI supplies for free was ignored, and
    # resolution fell through to origin/master on every stacked PR.
    "GITHUB_BASE_REF",
)
# Local fallbacks, tried in order, only when nothing explicit was supplied.
DEFAULT_BASE_REFS: tuple[str, ...] = ("origin/master", "master")

# Returned when the base resolved fine but the baseline did not exist there --
# i.e. this change INTRODUCES the file. Distinct from "could not resolve", which
# is a hard failure.
ABSENT_AT_BASE = "absent-at-base"


def _git(args: list[str]) -> tuple[int, str]:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except (OSError, ValueError) as exc:
        return 1, str(exc)
    return result.returncode, result.stdout


def resolve_base_sha(base_ref: str | None = None) -> tuple[str | None, list[str]]:
    """The merge-base of HEAD and the review base, or a failure.

    A ratchet must compare against an immutable reference OUTSIDE the change
    under review. `HEAD` is not that: locally, pre-commit, HEAD is the previous
    commit and reading the reference from it happens to work -- but in CI HEAD
    IS the proposed commit, so the guard reads back the very document the change
    just edited and compares it with itself. Raising an allowance from 1 to 9999
    then passes the shrink-only check, because both sides moved together.

    This repo has already written that lesson down, in
    tests/ci/test_gpu_harness_deferred_contract.py: "The first version of this
    guard read the allowed backlog out of the manifest and compared the manifest
    against itself. That is not a ratchet." It is the same defect.

    So the base is resolved explicitly and, when it cannot be, this FAILS. It
    must never fall back to HEAD: that reinstates the bug in precisely the
    situation where it matters, and it would encode "cannot determine the base"
    and "nothing changed" identically -- the exact conflation this whole branch
    exists to remove.
    """
    # An EXPLICIT base (argument or environment) is authoritative. If it does not
    # resolve, that is a failure -- never a silent fall-through to the local
    # defaults. Falling through would grade a stacked PR against master while
    # reporting green, which is the precise defect the base plumbing exists to
    # prevent; "the base you named is unreachable" and "you named no base" are
    # different conditions and must not share an outcome.
    candidates: list[str] = []
    explicit = False
    if base_ref:
        candidates.append(base_ref)
        explicit = True
    else:
        for name in BASE_REF_ENV_VARS:
            value = os.environ.get(name, "").strip()
            if value:
                candidates.append(value)
                explicit = True
        if not explicit:
            candidates.extend(DEFAULT_BASE_REFS)

    tried: list[str] = []
    for candidate in candidates:
        # A ref may reach us as a BRANCH NAME rather than a sha:
        # GITHUB_BASE_REF is `master` or `gs/650-quarantine-ratchet`, never a
        # sha. Under actions/checkout the base branch usually exists ONLY as a
        # remote-tracking ref, so a bare `<ref>` lookup fails and the guard fails
        # closed on a legitimate PR -- correct-but-unusable, the same shape as
        # the --rename path. So both forms are tried.
        forms = [candidate]
        if not candidate.startswith("origin/"):
            forms.append(f"origin/{candidate}")

        resolved: list[str] = []
        for form in forms:
            code, _ = _git(["rev-parse", "--verify", f"{form}^{{commit}}"])
            if code != 0:
                tried.append(form)
                continue
            code, merge_base = _git(["merge-base", "HEAD", form])
            if code != 0 or not merge_base.strip():
                tried.append(f"{form} (no merge-base with HEAD)")
                continue
            resolved.append(merge_base.strip())

        if not resolved:
            continue
        # When both a local branch and its remote-tracking ref resolve, take the
        # DESCENDANT-most merge-base. A stale local branch yields an OLDER base,
        # and an older base is not merely imprecise: if the baseline did not
        # exist there, the comparison degrades to ABSENT_AT_BASE and the ratchet
        # silently stops constraining anything. Measured on this very worktree:
        # local `master` sat at b6b2d7258bf while origin/master was a3bb6925132.
        # Tightest available reference wins.
        best = resolved[0]
        for other in resolved[1:]:
            code, _ = _git(["merge-base", "--is-ancestor", best, other])
            if code == 0:
                best = other
        return best, []

    named = "an explicitly named base did not resolve" if explicit else "no base was named"
    return None, [
        f"cannot resolve the review base ({named}), so the shrink-only ratchet has nothing "
        "immutable to compare against. Refusing to fall back to HEAD or to master: in CI, "
        "HEAD IS the change, and grading a stacked PR against master reports green either "
        f"way (tried: {', '.join(tried) or 'nothing'}). Pass --base-ref, or set one of "
        f"{', '.join(BASE_REF_ENV_VARS)}. Under actions/checkout the base branch may exist "
        "only as origin/<name>; both forms are tried, so a failure here means neither is "
        "fetched -- check fetch-depth."
    ]


def baseline_document_at_base(
    path: Path, base_ref: str | None = None
) -> tuple[dict | str | None, list[str]]:
    """The baseline as committed at the REVIEW BASE.

    Returns (document, failures), where document may be ABSENT_AT_BASE when the
    base resolved but the file did not exist there -- that is this change
    introducing the file, which is a fact about history rather than a bypass,
    and is reported rather than passed over in silence.
    """
    base_sha, failures = resolve_base_sha(base_ref)
    if failures:
        return None, failures
    try:
        rel = path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None, [
            f"baseline {path} is outside the repository root {ROOT}; cannot locate it at the "
            f"review base."
        ]
    code, out = _git(["show", f"{base_sha}:{rel}"])
    if code != 0:
        return ABSENT_AT_BASE, []
    try:
        document = json.loads(out)
    except json.JSONDecodeError as exc:
        return None, [f"baseline at review base {base_sha[:12]} is not valid JSON: {exc}"]
    if not isinstance(document, dict):
        return None, [f"baseline at review base {base_sha[:12]} is not a JSON object."]
    return document, []


def check_sites_against_base(
    current: dict[str, list[str]],
    base_document: dict | str,
    rename_ledger: list[dict] | None = None,
    scanned_keys: set[str] | None = None,
) -> list[str]:
    """The baseline's SITE LIST may only shrink relative to the review base.

    Closes the hand-edit path. The scan-vs-baseline check catches "added a skip
    and forgot the baseline"; it cannot catch "added a skip AND added its
    fingerprint to the baseline in the same commit", because then `actual` and
    `allowed` match and nothing is reported. Removing the --bootstrap-baseline
    flag closed the TOOL route to that; it did nothing about a text editor.

    Comparing the baseline file itself against the base closes it: the file grew,
    and growth is a violation regardless of how the bytes got there.

    ## The rename ledger, and why it is not a second bypass

    A file that legitimately MOVED has its fingerprints under a new key, which
    against the base reads as pure growth -- so the base comparison, added in
    round 3, left `--write-baseline --rename` unable to produce a baseline that
    passes. A guard with no legal route for a legitimate operation gets bypassed
    rather than obeyed, so the route has to exist.

    The ledger records `{"from": old_key, "to": new_key}` and is applied to the
    BASE document before comparing, i.e. it only ever RE-KEYS the reference. It
    cannot introduce a fingerprint: whatever it moves was already baselined at
    the base. And an entry whose `from` is still a live scanned source is
    rejected outright -- that is the "transfer credit between two live files"
    shape `_validate_renames` blocks at write time, re-checked here at read time
    so a hand-written ledger entry cannot do what the writer refuses to.
    """
    if base_document == ABSENT_AT_BASE:
        return []
    assert isinstance(base_document, dict)
    base_files = base_document.get("files")
    if not isinstance(base_files, dict):
        return ["baseline at the review base has no 'files' object; refusing to compare."]
    base_sites: dict[str, list[str]] = {}
    for name, entry in base_files.items():
        if isinstance(entry, dict) and isinstance(entry.get("sites"), list):
            base_sites[name] = sorted(str(s) for s in entry["sites"])
        else:
            return [f"baseline at the review base has a malformed entry for '{name}'."]

    failures: list[str] = []
    for entry in rename_ledger or []:
        if not isinstance(entry, dict):
            failures.append(f"rename_ledger entry is not an object: {entry!r}")
            continue
        old, new = entry.get("from"), entry.get("to")
        if not isinstance(old, str) or not isinstance(new, str) or not old or not new:
            failures.append(f"rename_ledger entry needs string 'from' and 'to': {entry!r}")
            continue
        if scanned_keys is not None and old in scanned_keys:
            failures.append(
                f"rename_ledger claims '{old}' moved to '{new}', but '{old}' is still a live "
                f"scanned source. A rename means the source is GONE; this shape transfers "
                f"credit between two live files."
            )
            continue
        if old in base_sites:
            base_sites[new] = sorted(base_sites.get(new, []) + base_sites.pop(old))
    if failures:
        return failures

    for name in sorted(set(current) | set(base_sites)):
        added = multiset_difference(current.get(name, []), base_sites.get(name, []))
        if added:
            failures.append(
                f"{name}: the BASELINE FILE gained {len(added)} site(s) relative to the review "
                f"base. The inventory may only shrink; adding a fingerprint by hand is the "
                f"same weakening as adding one with a tool:"
            )
            failures.extend(f"    {print_}" for print_ in added)
    return failures


def _validate_renames(
    renames: dict[str, str],
    baseline: dict[str, list[str]],
    found: dict[str, list[str]],
) -> list[str]:
    """Reject anything that is not an actual file rename.

    Without these checks `--rename` is a laundering primitive rather than a
    convenience: delete three sites from A, add three to an unrelated live file
    B, then `--rename A=B` transfers A's baselined credit onto B and the run
    goes green. The total cannot grow, but the ATTRIBUTION is a lie, and if the
    added fingerprints happen to match the deleted ones the swap is invisible.

    A real rename has three properties, all required here:

    * OLD is baselined (there is credit to move);
    * OLD is GONE FROM DISK -- not merely emptied of sites. This is what stops
      the transfer-between-live-files case, because A still exists;
    * NEW exists on disk AND appears in the current scan, so credit cannot be
      parked on an invented path (`totally/made/up.h` was accepted before).
    """
    failures: list[str] = []
    scanned = {source_key(path): path for path in test_sources()}
    for old, new in sorted(renames.items()):
        if old not in baseline:
            failures.append(f"'{old}' is not in the baseline; there is no credit to move.")
        if old in scanned:
            failures.append(
                f"'{old}' still exists on disk. --rename is for a file that MOVED; using it "
                f"while the source is still present transfers credit between two live files."
            )
        if new not in scanned:
            failures.append(
                f"'{new}' is not a scanned source file. Credit cannot be parked on a path "
                f"that does not exist."
            )
        elif new not in found:
            failures.append(
                f"'{new}' exists but has no environment-skip sites, so it cannot be the "
                f"destination of a rename that carries any."
            )
    return failures


def write_baseline(
    path: Path | None = None,
    *,
    renames: dict[str, str] | None = None,
) -> int:
    """Regenerate the baseline from this guard's own scan. Refuses to ADD sites.

    There is NO create-from-nothing path, by design and after two attempts at
    one. The first version nested the no-additions check inside
    `if path.is_file():`, so deleting the baseline bypassed it. The second put
    creation behind an explicit `--bootstrap-baseline` flag, which merely MOVED
    the bypass: the flag itself wrote unconditionally, and the resulting diff
    (`count 1->2` plus one `sites` entry) is indistinguishable from a legitimate
    shrink, so "loud in review" was not a real mitigation.

    The baseline is a committed file. If it is missing, the fix is
    `git checkout -- tests/ci/environment_skip_baseline.json`, not regeneration.
    Callers that genuinely need to compose a fresh document (only the unit
    tests, which scan synthetic trees) use build_baseline_document() and
    _serialize() directly; those are pure composition helpers with no write and
    no inclusion semantics to subvert.

    `renames` re-keys entries verbatim (`--rename old=new`) before the inclusion
    check, so moving or renaming a file is not a deadlock between "guard says
    stale/new" and "writer refuses". See _validate_renames() for what stops it
    being used to transfer credit between unrelated files.
    """
    path = BASELINE_PATH if path is None else path
    found = scan_fingerprints()
    renames = renames or {}

    if not path.is_file():
        print(
            f"[env-skip] FAIL baseline file missing: {path.name}. There is NO path that "
            f"creates it from the current tree -- that is exactly how a new skip would be "
            f"laundered in (delete the baseline, regenerate, ship a diff that looks like a "
            f"shrink). The baseline is a committed file: restore it with "
            f"`git checkout -- tests/ci/{path.name}`."
        )
        return 1

    try:
        previous_document = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError, UnicodeDecodeError) as exc:
        print(f"[env-skip] FAIL existing baseline is not readable JSON: {exc}")
        return 1
    baseline, failures = load_baseline(path)
    if failures:
        print("[env-skip] FAIL existing baseline is malformed; refusing to overwrite it.")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    rename_failures = _validate_renames(renames, baseline, found)
    if rename_failures:
        print("[env-skip] FAIL --rename rejected:")
        for failure in rename_failures:
            print(f"  - {failure}")
        return 1
    for old, new in renames.items():
        baseline[new] = sorted(baseline.pop(old) + baseline.get(new, []))

    additions: list[str] = []
    for name in sorted(set(found) | set(baseline)):
        added = multiset_difference(found.get(name, []), baseline.get(name, []))
        additions.extend(f"{name}: {print_}" for print_ in added)
    if additions:
        print(
            "[env-skip] FAIL --write-baseline refuses to ADD environment-skip sites. "
            "This ratchet only shrinks; a new skip must be removed from the source, not "
            "laundered into the baseline."
        )
        for addition in additions:
            print(f"  - {addition}")
        return 1

    document = build_baseline_document(found, previous_document, renames)
    path.write_text(_serialize(document), encoding="utf-8")
    total = sum(len(v) for v in found.values())
    print(
        f"[env-skip] WROTE {path.name}: {total} site(s) across {len(found)} file(s)."
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--write-baseline",
        action="store_true",
        help=(
            "Regenerate environment_skip_baseline.json from this guard's own scan. "
            "Refuses to add sites; it can only record removals."
        ),
    )
    parser.add_argument(
        "--rename",
        action="append",
        default=[],
        metavar="OLD=NEW",
        help=(
            "Re-key a baselined entry when its source file moves or is renamed. "
            "Moves fingerprints verbatim; can never introduce one. Repeatable."
        ),
    )
    parser.add_argument(
        "--base-ref",
        default=None,
        help=(
            "The review base to ratchet against (a commit-ish; the merge-base with HEAD is "
            "used). CI should pass the PR base sha. Defaults to "
            f"{', '.join(BASE_REF_ENV_VARS)} then {', '.join(DEFAULT_BASE_REFS)}. NEVER "
            "falls back to HEAD -- in CI, HEAD is the change, and a ratchet that compares "
            "the change with itself is not a ratchet."
        ),
    )
    args = parser.parse_args(argv)
    base_ref = args.base_ref

    renames, rename_errors = _parse_renames(args.rename)
    if rename_errors:
        for error in rename_errors:
            print(f"[env-skip] FAIL {error}")
        return 1
    if renames and not args.write_baseline:
        print("[env-skip] FAIL --rename only applies together with --write-baseline.")
        return 1

    if args.write_baseline:
        return write_baseline(renames=renames)

    files = test_sources()
    if not files:
        print("[env-skip] FAIL no module test sources found - the scan is broken.")
        return 1

    found = scan_all()
    found_prints = scan_fingerprints()
    baseline, failures = load_baseline()
    failures = list(failures) + check_macro_contract()

    # Both ratchets compare against the REVIEW BASE, never HEAD. In CI, HEAD is
    # the proposed commit, so a HEAD-relative reference compares the change with
    # itself and permits exactly the weakening it exists to reject. The base is
    # resolved once and shared by the allowance check and the site-inventory
    # check -- they are one rule applied to two parts of the same file.
    if BASELINE_PATH.is_file():
        try:
            document = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError, UnicodeDecodeError) as exc:
            failures.append(f"baseline is not readable JSON: {exc}")
        else:
            base_document, base_failures = baseline_document_at_base(BASELINE_PATH, base_ref)
            failures.extend(base_failures)
            if not base_failures:
                if base_document == ABSENT_AT_BASE:
                    # Reported, never silent: this run has no ratchet reference
                    # because the change INTRODUCES the file. The scan-vs-baseline
                    # check below is still fully enforced.
                    print(
                        "[env-skip] NOTE the baseline does not exist at the review base, so this "
                        "change introduces it; the shrink-only comparison has no reference this "
                        "run. Review the whole file, not a diff."
                    )
                failures.extend(check_allowance(document, base_document))
                failures.extend(
                    check_sites_against_base(
                        baseline,
                        base_document,
                        document.get("rename_ledger"),
                        {source_key(p) for p in files},
                    )
                )

    total = sum(len(v) for v in found.values())

    # Fingerprint -> the concrete sites carrying it, in source order. A list (not
    # a single entry) because a file legitimately holds many identical sites
    # (`REQUIRE_GPU_DEVICE();` twelve times); reporting the same line twelve
    # times would hide eleven of them.
    where: dict[str, dict[str, list[tuple[int, str, str, str]]]] = {}
    for name, sites in found.items():
        table: dict[str, list[tuple[int, str, str, str]]] = {}
        for line_no, kind, detail, case in sites:
            table.setdefault(fingerprint(kind, detail, case), []).append(
                (line_no, kind, detail, case)
            )
        where[name] = table

    for name in sorted(set(found_prints) | set(baseline)):
        actual = found_prints.get(name, [])
        allowed = baseline.get(name, [])
        added = multiset_difference(actual, allowed)
        removed = multiset_difference(allowed, actual)
        if added:
            failures.append(f"{name}: {len(added)} NEW environment-skip site(s):")
            # The LAST occurrences are the reported ones: the baselined copies
            # account for the earlier ones, so pointing at the tail is what makes
            # "you added one more of these" readable.
            pending = dict(where[name])
            for print_ in added:
                occurrences = pending.get(print_) or []
                line_no, kind, detail, case = (
                    occurrences.pop() if occurrences else (0, "?", print_, FILE_SCOPE)
                )
                failures.append(
                    f"    line {line_no}: [{kind}] {_elide(detail, 70)}  (case: {_elide(case, 60)})"
                )
            failures.append(
                f"    An environment skip is scored by doctest as a PASS (the vendored 2.4.12 "
                f"has no runtime skip API), so a new one silently removes coverage. Either give "
                f"the case a lane that can satisfy its precondition, or make the precondition a "
                f"FAIL. If it genuinely must skip, it belongs in the conversion slice "
                f"{CONVERSION_SLICE} with an owner ({BASELINE_ISSUE})."
            )
        if removed:
            failures.append(
                f"{name}: {len(removed)} baselined site(s) no longer found - the ratchet must "
                f"tighten. Re-run `python tests/ci/check_environment_skip_marker.py "
                f"--write-baseline` so the slack cannot be reoccupied:"
            )
            for print_ in removed:
                failures.append(f"    {print_}")

    if failures:
        print(f"[env-skip] FAIL {total} site(s) found across {len(found)} file(s).")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ") else failure)
        return 1

    print(
        f"[env-skip] PASS {len(files)} test source(s) scanned; "
        f"{total} baselined site(s) across {len(found)} file(s), 0 new, 0 stale."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
