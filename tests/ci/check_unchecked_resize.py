#!/usr/bin/env python3
"""Guard: no NEW unchecked `Vector::resize()` feeding a raw write (#794, #798).

## The failure this guards against

`Vector<T>::resize()` reports allocation failure through its RETURN VALUE. Ignoring it
is not benign, and the consequence depends on the vector's prior state -- there are
three distinct shapes, all visible in `core/templates/cowdata.h`:

    Error CowData<T>::_fork_allocate(USize p_size) {
        if (p_size == 0) {
            _unref();                 // <- only HERE does _ptr become null
            return OK;
        }
        ...
            const Error error = _realloc(alloc_size);
            if (error) {
                // Out of memory; the current array is still valid though.
                return error;         // <- size() KEEPS its old value
            }

1. **Fresh/empty vector, allocation fails** -> `_ptr` stays null. A later
   `vec.write[i]` traps loudly in `CRASH_BAD_INDEX` (`core/templates/vector.h:54`),
   which is NOT `DEBUG_ENABLED`-gated, so it hard-fails in every build including
   release. A later `vec.ptrw()[i]` instead writes through **nullptr** with no bounds
   check and no diagnostic at all.
2. **Populated vector, failed GROW** -> the engine keeps the old, TOO-SHORT buffer and
   `size()` is unchanged. `is_empty()` is false and `ptrw()` returns a VALID pointer to
   a short live block, so writing the requested count is a **silent heap overflow into
   a real allocation**. This is the worst shape and the least obvious.
3. **refcount > 1 (a CoW copy), fork fails** -> the old data is dropped and `_ptr` is
   null: a wild write *plus* silent content loss.

Shape 1 killed the nightly GPU Streaming Stress lane (#787: `0xC0000409`, fast-fail
subcode 7). Shapes 2 and 3 were found later, during #798, and are why `is_empty()` is
NOT a valid resize-failure test.

The fix is `gs_resize_or_fail()` in `modules/gaussian_splatting/core/gs_vector_alloc.h`,
which checks the return, clears the vector, reports count/element-size/total-bytes, and
is `[[nodiscard]]`.

## Why this is a RATCHET and not a zero-tolerance gate

Measured on the tree at the time of writing, the predicate below flags ~29 sites once
the in-flight #798 PRs land. Most of the remainder are believed safe -- typically the
write index is bounded by the vector's OWN `size()`, so a failed resize shrinks the
bound along with the buffer -- but proving that per site needs real dataflow analysis,
not a source scan. Rather than pretend otherwise, this guard records the known set in a
GENERATED baseline and fails only when a NEW site appears.

That keeps it honest in both directions: it cannot silently bless a new defect, and it
does not claim the existing set is proven safe.

## The predicate, and why the window is function-scoped

A site is flagged when ALL of:

* the statement is `X.resize(N);` (or `resize_initialized` / `resize_uninitialized`)
  and the statement itself does not consume the result -- no assignment, no `if (`, no
  `ERR_FAIL*`, no `gs_resize_or_fail`;
* `N` is NOT a compile-time constant. A literal, an ALL_CAPS constant or a bare
  `sizeof(...)` cannot scale with scene/asset/file data, and a failure to allocate a
  handful of small structs means the process is already gone. This is a RULE, not a
  curated exception list;
* the declared type is a CoW container (`Vector<T>` or a `Packed*Array`). `LocalVector`
  is out of class entirely: its `resize()` returns **void** and `reserve()` ends in
  `CRASH_COND_MSG(!data, "Out of memory")`, so it aborts loudly rather than silently
  under-allocating;
* a raw consumer (`.write[`, `.ptrw()`, `.ptr()`) of that same object appears later in
  the SAME function.

The last point is deliberate and was measured, not guessed. The #794/#798 sweeps used an
11-line window between the resize and the write. That window misses **22** of the
function-scope sites on this tree -- including `io/streaming_chunk_bake.cpp:72`, whose
matching `records.write[]` sits ~38 lines below and which turned out to be a live
`CRASH_BAD_INDEX`. An 11-line window is therefore known-insufficient by counterexample.

## What this guard deliberately does NOT catch

Stated plainly, because an undocumented blind spot invites false confidence:

1. **Vectors reached through a reference parameter or a member of another object**,
   where the declaration is not in the enclosing function. Type resolution here is a
   backward text scan, so `Vector<uint8_t> &r_out` sized by its callee is invisible.
   This is not hypothetical: the single most dangerous site found during #798
   (`spz_loader.cpp` decompress output, sized from a FILE-DECLARED length) was missed by
   exactly this limitation.
2. **The consumer being in a different function** -- e.g. resize here, `ptrw()` in a
   helper that takes the vector by reference.
3. **Whether the flagged site is actually a defect.** The guard cannot tell a write
   bounded by `size()` from one bounded by the requested count; that is the whole reason
   for the baseline.
4. **Non-`Vector` containers with the same hazard**, and raw `memalloc`/`memnew_arr`.
5. **Integer overflow in `N` itself** -- e.g. `uint32_t * sizeof(...)` wrapping and
   under-allocating *past* a correct check. Two such cases were found by hand during
   #798; a source scan cannot evaluate the arithmetic.
6. **Code inside `#if 0` / disabled preprocessor regions**, which is still counted as
   live source. Comments and string literals no longer are -- they are masked out
   before any analysis (`_mask_source`) -- but the preprocessor is not evaluated, so
   braces in a disabled region can still miscount function depth.
7. **Whether a recorded baseline entry is the statement it was recorded for**, once a
   key repeats in one function. Duplicates are identified by their surrounding
   statements plus, for a run of identical blocks, their statement offset in the
   function. Both are textual: an edit inside a duplicate's context re-identifies it and
   the guard fires. That is the safe direction -- it reports for review rather than
   missing -- but it is a false positive, not a defect found.

Reliable detection of 1-3 needs type and dataflow information, i.e. a clang-tidy check
or a compiler plugin. This is a ratchet against the pattern spreading, not a proof that
the module is free of it.

## Usage

    python tests/ci/check_unchecked_resize.py              # verify against the baseline
    python tests/ci/check_unchecked_resize.py --regenerate # after fixing sites

Regenerating is expected to SHRINK the baseline. If it would grow, `--regenerate`
FAILS and prints the new sites; that is the case that must be justified or fixed, never
silently re-baselined.

There is exactly one other way the recorded set may grow, and it is not a defect being
blessed: when THE DETECTOR changes -- a new key format, or a widened predicate that
reveals sites which were always in the tree and merely invisible. That path is
`--accept-detector-change --reason "..."`, it is never run by CI, and it prints the full
delta so each addition is reviewed against the diff. Widening detection and then
declining to record what it finds would leave the guard permanently red; recording it
silently would be indistinguishable from blessing a new defect. Printing it and naming
the reason is the only honest option, and the reason is a claim a reviewer must check,
not a fact this script can prove.

## Why the baseline is graded against the REVIEW BASE

Everything above reads the baseline out of the proposed commit, so on its own it cannot
see the one shape that defeats every ratchet: a change that adds an unsafe site AND
writes that site's key into `unchecked_resize_baseline.json`. `new_sites` is then empty
and the run exits 0, because both sides moved together.

So the committed baseline is compared against the baseline as committed at the review
base (`--base-ref`, else `GS_CI_BASE_REF` / `GITHUB_BASE_REF` / ..., else
`origin/master` -- the same resolver `tests/ci/check_environment_skip_marker.py` uses,
imported rather than copied), and it may only SHRINK. If the base cannot be resolved the
guard FAILS: with no immutable reference there is nothing to ratchet against, and
"cannot determine the base" must never share an outcome with "nothing changed".

Growth relative to the base needs BOTH of:

* a `detector_change_ledger` entry naming the added key with a reason, and
* this script actually differing from its form at the review base.

The second is the load-bearing one. A reason is self-attested and only a reviewer can
grade it; "the detector changed" is a fact git settles, and it denies the growth route
entirely to any change that does not touch the detector.
"""
from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.util
import json
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_ROOT = REPO_ROOT / "modules" / "gaussian_splatting"
BASELINE_PATH = REPO_ROOT / "tests" / "ci" / "unchecked_resize_baseline.json"

# Resolved from __file__, never from REPO_ROOT: the self-tests repoint REPO_ROOT at a
# temporary fixture tree, and git must still be asked about the real repository.
SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
SCRIPT_ROOT = SCRIPT_DIR.parents[1]
# The review base resolver is SHARED with tests/ci/check_environment_skip_marker.py
# rather than copied. That guard already resolves the PR base (--base-ref, then
# GS_CI_BASE_REF / GITHUB_BASE_REF / ..., then origin/master) and every workflow step
# that runs this lane already exports GS_CI_BASE_REF for it, so the plumbing exists;
# a second copy of a security-relevant resolver would only drift from it.
BASE_RESOLVER_PATH = SCRIPT_DIR / "check_environment_skip_marker.py"
# The baseline as committed at the review base did not exist there -- this change
# introduces it. A fact about history, distinct from "the base could not be resolved",
# which is a hard failure.
ABSENT_AT_BASE = "absent-at-base"
LEDGER_KEY = "detector_change_ledger"

# Applied to an ASSEMBLED, COMMENT-MASKED statement, never to a raw line.
#
# Two ordinary formatting choices used to defeat this, and both are the SAME defect:
# the guard parsed C++ with a line-anchored regex, so anything the compiler treats as
# one statement but the reader writes across two lines -- or anything with a trailing
# comment -- simply was not seen. Neither is an exotic evasion; Godot style produces
# both routinely.
#
#   values.resize(              <- wrapped call: never matched at all
#           runtime_count);
#   values.resize(n); // note   <- `;\s*$` rejects it, so the ptrw() below is never read
#
# The second form is not hypothetical either: it hid a live site at
# renderer/render_resource_orchestrator.cpp:267, whose `vertex_data.ptrw()` sits two
# lines below. Rather than special-case a trailing comment in the pattern, comments and
# string literals are MASKED OUT of the source before any analysis (see _mask_source),
# which removes the whole class -- including the `}` -inside-a-comment case that used to
# close a function span early in _function_spans().
RESIZE_RE = re.compile(
    r"^\s*([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)\s*"
    r"\.resize(?:_initialized|_uninitialized)?\s*\(\s*(.+?)\s*\)\s*;\s*$"
)
# Cheap prefilter so statement assembly only runs on candidate lines.
RESIZE_HEAD_RE = re.compile(
    r"^\s*([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)\s*"
    r"\.resize(?:_initialized|_uninitialized)?\s*\("
)
# A function definition opening a top-level block. Deliberately rejects lines ending
# in ';' (declarations) and control-flow keywords.
# Captures the QUALIFIED name: `Error Alpha::build(` -> "Alpha::build", not "build".
# Capturing only the trailing identifier let two classes' same-named methods in one
# file collide back onto a single key, which is the P1 defect this whole change is
# about -- just one level further in.
FUNC_DEF_RE = re.compile(
    r"^[A-Za-z_~][\w\s:<>,\*&\[\]]*?\b((?:[A-Za-z_~][\w]*::)*[A-Za-z_~][\w]*)\s*\("
)
NOT_A_FUNCTION = frozenset({"if", "for", "while", "switch", "catch", "return", "else", "do"})
# Any form on the SAME line that consumes or checks the return value.
CONSUMED_RE = re.compile(
    r"(=\s*[\w:]*resize|if\s*\(|ERR_FAIL|ERR_CONTINUE|ERR_BREAK|return\s|CHECK|REQUIRE"
    r"|\|\||&&|gs_resize_or_fail)"
)
COW_RE = re.compile(
    r"\b(Vector\s*<|PackedByteArray|PackedInt32Array|PackedInt64Array|PackedFloat32Array"
    r"|PackedFloat64Array|PackedVector2Array|PackedVector3Array|PackedVector4Array"
    r"|PackedColorArray|PackedStringArray)"
)
LOCAL_VECTOR_RE = re.compile(r"\bLocalVector\s*<")
# A count that cannot scale with scene/asset/file data.
CONST_COUNT_RE = re.compile(r"^(\d+|[A-Z][A-Z0-9_]{2,}|sizeof\s*\(.*\)|\d+\s*[*+]\s*sizeof\s*\(.*\))$")

# How many neighbouring statements on each side form a duplicate's context identity.
# One neighbour per side was too little: fixing one duplicate while adding another whose
# single neighbours matched left the identity set unchanged. See _context_digest().
CONTEXT_STATEMENTS = 3


def _is_digit_separator(line: str, index: int) -> bool:
    """True when the `'` at `index` is a C++14 digit separator, not a char literal.

    What decides this is the TOKEN the quote belongs to, not merely the character in
    front of it. `1'000'000` separates digits, but `U'}'`, `L'}'`, `u'}'` and `u8'}'`
    are ordinary character literals whose PREFIX is alphanumeric as well. Testing only
    `line[i - 1].isalnum()` classified every prefixed literal as a separator, so its
    contents were left unmasked -- and a `}` inside one still closed a function span
    early in _function_spans(), which drops the resize's consumer silently.

    A numeric literal always begins with a DIGIT and every char-literal prefix begins
    with a letter, so walking back to the start of the token settles it.
    """
    k = index - 1
    while k >= 0 and (line[k].isalnum() or line[k] in "_'"):
        k -= 1
    return k + 1 < index and line[k + 1].isdigit()


def _mask_source(lines: list[str]) -> list[str]:
    """Blank out comments and string/char literal CONTENT, preserving geometry.

    Every character that belongs to a comment or a literal is replaced by a space, so
    the returned list has the same number of lines and each line the same length. Line
    numbers, columns and brace/paren positions therefore stay usable, while text that
    the compiler ignores can no longer steer the analysis.

    This one step closes three separately-reported holes that were all the same
    underlying problem -- the guard parsing C++ with regex and raw brace counting:

    * a trailing `// comment` after `resize(n);` made RESIZE_RE reject the statement;
    * a `}` inside a comment or a string between the resize and its consumer closed the
      enclosing function span early, so _consumer_scan_end() returned a premature end
      and the consumer was never reached. The advertised unbounded fallback did NOT
      save this case: the resize still fell inside the truncated span, so the fallback
      never ran;
    * `.resize(` or `.ptrw()` mentioned inside a comment or a string was read as code.

    Handled: `//`, `/* */` (including multi-line), `"..."` and `'...'` with escapes, and
    raw strings `R"delim(...)delim"`. Digit separators (`1'000'000`) are NOT treated as
    char literals, while PREFIXED character literals (`U'}'`, `L'}'`, `u'}'`, `u8'}'`)
    are -- see _is_digit_separator(), which decides by the token rather than by the one
    character in front of the quote. Not handled, and stated rather than hidden: `#if 0`
    blocks are still counted as live code, so braces inside a disabled region can still
    miscount depth.
    """
    out: list[str] = []
    in_block = False
    raw_terminator: str | None = None
    for line in lines:
        buf = list(line)
        i = 0
        n = len(line)
        while i < n:
            if raw_terminator is not None:
                idx = line.find(raw_terminator, i)
                stop = n if idx < 0 else idx + len(raw_terminator)
                for k in range(i, stop):
                    buf[k] = " "
                i = stop
                if idx >= 0:
                    raw_terminator = None
                continue
            if in_block:
                idx = line.find("*/", i)
                stop = n if idx < 0 else idx + 2
                for k in range(i, stop):
                    buf[k] = " "
                i = stop
                if idx >= 0:
                    in_block = False
                continue
            ch = line[i]
            nxt = line[i + 1] if i + 1 < n else ""
            if ch == "/" and nxt == "/":
                for k in range(i, n):
                    buf[k] = " "
                i = n
                continue
            if ch == "/" and nxt == "*":
                in_block = True
                buf[i] = buf[i + 1] = " "
                i += 2
                continue
            if ch == "R" and nxt == '"':
                j = i + 2
                delim = ""
                while j < n and line[j] != "(":
                    delim += line[j]
                    j += 1
                if j < n:
                    raw_terminator = f"){delim}\""
                    for k in range(i, j + 1):
                        buf[k] = " "
                    i = j + 1
                    continue
            if ch == "'" and _is_digit_separator(line, i):
                i += 1  # digit separator such as 1'000'000, not a char literal
                continue
            if ch in ('"', "'"):
                buf[i] = " "
                j = i + 1
                while j < n:
                    if line[j] == "\\" and j + 1 < n:
                        buf[j] = buf[j + 1] = " "
                        j += 2
                        continue
                    closing = line[j] == ch
                    buf[j] = " "
                    j += 1
                    if closing:
                        break
                i = j
                continue
            i += 1
        out.append("".join(buf))
    return out


def _module_sources() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for pattern in ("*.cpp", "*.h"):
        for path in sorted(MODULE_ROOT.rglob(pattern)):
            if "tests" in path.parts:
                continue
            out.append(path)
    return out


def _declared_container(lines: list[str], index: int, var: str, floor: int) -> str:
    """Resolve the container kind by scanning back for a mention of the symbol.

    `floor` is the first line of the ENCLOSING FUNCTION, not a fixed line count. The
    fixed 70-line lookback it replaces was a silent drop: a `Vector` declared 70+ lines
    above its `resize()` resolved as "unknown", and find_sites() discards unknowns, so a
    long function could carry exactly the unchecked allocation this guard ratchets and
    produce no finding at all. Any cap on a backward scan is the same window-length
    evasion that _function_spans() removed on the forward side; the enclosing function
    is the real boundary in both directions.

    Scanning to the function's first line also brings the SIGNATURE into range, so a
    `Vector<T> &r_out` parameter now resolves instead of reading as unknown. That
    narrows -- it does not close -- blind spot 1 in the module docstring.
    """
    base = re.escape(var.split(".")[-1])
    for k in range(index, max(-1, floor - 1), -1):
        if re.search(rf"\b{base}\b", lines[k]):
            if LOCAL_VECTOR_RE.search(lines[k]):
                return "local_vector"
            if COW_RE.search(lines[k]):
                return "cow"
    return "unknown"


def _normalise(expr: str) -> str:
    """Collapse whitespace so trivial reformatting does not read as a new site."""
    return re.sub(r"\s+", "", expr)


def _assemble_statement(lines: list[str], start: int) -> tuple[str, int]:
    """Join physical lines from `start` into one logical statement.

    Returns (text, last_line_index), or ("", start) when the call never terminates
    before end of file -- which the caller treats as a PARSE FAILURE, not as "nothing
    here". A resize call split across lines is a single statement; matching per physical
    line missed it entirely, which let a new unsafe allocation through.

    Assembly stops when the parentheses opened by the call have balanced AND a ';' has
    been seen after the closing paren. It is deliberately UNBOUNDED. The previous
    12-physical-line cap reinstated the very hole it was added to fix, one size larger:
    a `resize(` whose closing paren sat on line 13 returned nothing, the consumer below
    it was never examined, and the guard passed -- with no signal that anything had been
    skipped. A cap on statement assembly is a silent miss; running to the end of the
    file and failing closed on the residue is not.
    """
    text = ""
    for j in range(start, len(lines)):
        text = lines[j].strip() if not text else f"{text} {lines[j].strip()}"
        depth = 0
        opened = False
        close_at = -1
        for pos, ch in enumerate(text):
            if ch == "(":
                depth += 1
                opened = True
            elif ch == ")":
                depth -= 1
                if opened and depth == 0:
                    close_at = pos
        if opened and depth == 0 and close_at >= 0 and ";" in text[close_at:]:
            return text, j
    return "", start


def _function_spans(lines: list[str]) -> list[tuple[int, int, str]]:
    """Derive real top-level function spans by brace depth.

    Replaces a fixed MAX_FUNCTION_SPAN cap. That cap meant any function whose raw
    consumer sat further than the cap below its resize() had the site silently
    dropped -- recreating exactly the window-length evasion that motivated
    function-scoped scanning in the first place.

    MUST be called on masked lines (see _mask_source). Raw brace counting used to
    include braces inside comments and string literals, and the consequence was not
    the harmless over-scan the previous revision of this docstring claimed: a stray
    `}` in a comment between a resize and its consumer closed the span EARLY, the
    resize still fell inside that truncated span, so _consumer_scan_end() returned the
    premature end and the unbounded fallback below it never ran. The site was dropped
    silently. Masking removes the input, rather than relying on a fallback that the
    failing case cannot reach.

    The fallback in _consumer_scan_end() stays unbounded regardless, for the cases
    masking cannot fix (`#if 0`, unbalanced macros): a guard that over-scans reports a
    site for review; one that under-scans misses a defect.
    """
    spans: list[tuple[int, int, str]] = []
    depth = 0
    sig: tuple[str, int] | None = None
    fn_start: int | None = None
    fn_name = ""
    fn_depth = 0
    for i, line in enumerate(lines):
        opens = line.count("{")
        closes = line.count("}")
        if fn_start is None:
            match = FUNC_DEF_RE.match(line)
            if match and match.group(1) not in NOT_A_FUNCTION and not line.rstrip().endswith(";"):
                # A candidate must survive until a block actually opens: Godot style
                # routinely puts the '{' on the line AFTER a wrapped parameter list.
                sig = (match.group(1), i)
            # `sig is not None` is the load-bearing part, and is what keeps
            # `namespace GaussianSplatting {` transparent: a container line has no '('
            # so FUNC_DEF_RE never matches it, and a brace with no pending signature
            # opens no span. An earlier revision opened a span for ANY brace and named
            # it "<anonymous>", which swallowed a whole file into one scope and
            # collapsed every function identity in it back together.
            #
            # An explicit namespace/class/struct pattern was tried here as well and
            # REMOVED: mutation testing showed reverting it changed no behaviour and no
            # test, i.e. it was dead code carrying a comment that claimed otherwise.
            if opens > 0 and sig is not None:
                fn_start, fn_name, fn_depth = sig[1], sig[0], depth
                sig = None
        depth += opens - closes
        if fn_start is not None and depth <= fn_depth:
            spans.append((fn_start, i, fn_name))
            fn_start = None
            fn_name = ""
    return spans


def _consumer_scan_end(lines: list[str], index: int, spans: list[tuple[int, int, str]]) -> int:
    """End of the enclosing function, never a fixed-length window."""
    for start, end, _ in spans:
        if start <= index <= end:
            return end
    # No span resolved (brace miscount, or file-scope). Scan to the next column-0
    # '}' with NO cap, then to end of file. Over-scanning is the safe direction.
    for j in range(index + 1, len(lines)):
        if lines[j].startswith("}"):
            return j
    return len(lines)


def _function_name_at(index: int, spans: list[tuple[int, int, str]]) -> str:
    for start, end, name in spans:
        if start <= index <= end:
            return name
    return "<file-scope>"


def _span_start_at(index: int, spans: list[tuple[int, int, str]]) -> int:
    """First line of the enclosing function, or the top of the file.

    The backward floor for declaration lookup, mirroring _consumer_scan_end() on the
    forward side. Falling back to line 0 rather than to a fixed window keeps the
    unresolved-span case OVER-scanning: a guard that over-scans reports a site for
    review, one that under-scans misses a defect.
    """
    for start, end, _ in spans:
        if start <= index <= end:
            return start
    return 0


def _context_digest(lines: list[str], start: int, last: int) -> str:
    """A stable identity for one of several IDENTICAL statements in one function.

    An occurrence ORDINAL (`key`, `key#2`, ...) does not identify a statement, it only
    counts them: fixing the first duplicate while adding a new unchecked one elsewhere
    in the same function leaves both the number and the exact set of keys unchanged, so
    the ratchet passes over a real new site. That is the same "cardinality is not
    identity" hole the function name fixed one level up.

    The identity used instead is the CONTEXT_STATEMENTS nearest preceding and following
    statements (whitespace-collapsed, braces skipped), which distinguishes two copies
    that sit in different places while staying stable under line shifts elsewhere in the
    file.

    One neighbour per side was NOT enough, and the difference is the reported evasion
    rather than a theoretical one: for two repeated blocks the immediately adjacent
    statements are part of the repeated block itself, so they are identical by
    construction. Fixing one block and appending another then reproduced the same digest
    and the ratchet passed. Widening the window to three reaches PAST the repeated block
    into what surrounds it, where the fix and the addition are both visible.

    Deliberately narrow: this suffix is only attached to keys that actually REPEAT
    within a file (see find_sites). A unique key keeps its plain, context-free form, so
    an edit next to a normal site can never make it look new.
    """
    def _neighbours(rng) -> list[str]:
        found: list[str] = []
        for k in rng:
            text = _normalise(lines[k])
            if text and text not in ("{", "}", "{}"):
                found.append(text)
                if len(found) == CONTEXT_STATEMENTS:
                    break
        return found

    before = _neighbours(range(start - 1, -1, -1))
    after = _neighbours(range(last + 1, len(lines)))
    payload = "|".join(before) + "@@" + "|".join(after)
    return hashlib.sha1(payload.encode("utf-8")).hexdigest()[:8]


def _statement_offset(lines: list[str], index: int, span_start: int) -> int:
    """How many non-trivial statements precede `index` inside its function.

    Used ONLY to separate duplicates whose full context digests still collide -- a run
    of identical adjacent blocks long enough that even the widened window sees the same
    text on both sides. An occurrence ordinal cannot separate those in a useful way: it
    renumbers with the group, so fixing one member and adding another elsewhere yields
    the same numbers and the ratchet passes. A statement offset is a position in the
    function, so the added member necessarily gets an identity the baseline does not
    contain.

    It moves when statements are inserted ABOVE it in the same function, which reads as
    a new site and fails the guard. That is the safe direction and it can only ever
    affect true duplicates -- there are none in the tree today.
    """
    count = 0
    for k in range(span_start, index):
        text = _normalise(lines[k])
        if text and text not in ("{", "}", "{}"):
            count += 1
    return count


def find_sites() -> tuple[list[str], list[str]]:
    """Return (sites, scan_errors).

    scan_errors is NOT cosmetic: an unreadable source used to be skipped with
    `continue`, which dropped every site in that file and still let the run exit 0,
    reporting the file's baseline entries as "fixed". Incomplete scanning must never
    be accepted as evidence that no new site exists, so the caller fails on it.

    The same rule now covers a `resize(` the scanner cannot ASSEMBLE into a statement.
    Both conditions mean the scan did not look; neither may read as "found nothing".
    """
    sites: list[str] = []
    errors: list[str] = []
    for path in _module_sources():
        try:
            raw_lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            errors.append(f"{path.relative_to(REPO_ROOT).as_posix()}: {exc}")
            continue
        # Analyse the MASKED source throughout -- statement matching, type lookback,
        # brace depth and consumer search alike. Comments and string literals are not
        # code, and letting them steer any of those four is what produced three of the
        # reported evasions.
        lines = _mask_source(raw_lines)
        rel = path.relative_to(REPO_ROOT).as_posix()
        spans = _function_spans(lines)
        candidates: list[tuple[str, str, int]] = []
        for i, line in enumerate(lines):
            if not RESIZE_HEAD_RE.match(line):
                continue
            statement, last = _assemble_statement(lines, i)
            if not statement:
                # Assembly is unbounded, so the only way to get here is a `resize(`
                # whose call never closes before end of file. That is malformed input
                # to this scanner, and the one thing it must not do is treat it as an
                # absence of findings -- the consumer below it was never examined.
                errors.append(
                    f"{rel}:{i + 1}: could not assemble the resize() statement "
                    f"(unterminated call); the scan of this site is incomplete."
                )
                continue
            match = RESIZE_RE.match(statement)
            if not match or CONSUMED_RE.search(statement):
                continue
            var, count = match.group(1), match.group(2)
            if CONST_COUNT_RE.match(count):
                continue  # exclusion rule: compile-time-constant count
            span_start = _span_start_at(i, spans)
            kind = _declared_container(lines, i, var, span_start)
            if kind == "local_vector":
                continue  # out of class: LocalVector::resize() returns void
            if kind != "cow":
                continue  # unresolved type -> not flagged; see blind spot 1
            base = re.escape(var)
            consumer = re.compile(rf"{base}\s*(\.write\s*\[|\.ptrw\s*\(|\.ptr\s*\()")
            end = _consumer_scan_end(lines, i, spans)
            if any(consumer.search(lines[j]) for j in range(last + 1, end + 1)):
                # Key on file + ENCLOSING FUNCTION + symbol + count expression, and
                # never on the line number. Line numbers make every unrelated edit
                # look like a new site; but file+symbol+count ALONE collapses distinct
                # statements together -- measured: 80 statements folded into 69 keys,
                # with `result.resize(splat_count)` in gaussian_splat_asset.cpp
                # covering eight separate getters. Fixing one of those eight, or adding
                # a ninth, left the key set unchanged and the ratchet passed silently.
                # The function name restores independent tracking while staying stable
                # under line shifts. True repeats within one function are disambiguated
                # by _context_digest() below -- NOT by an occurrence ordinal, which
                # counts statements without identifying them.
                key = f"{rel}::{_function_name_at(i, spans)}::{var}.resize({_normalise(count)})"
                candidates.append((key, _context_digest(lines, i, last),
                                   _statement_offset(lines, i, span_start)))

        # Second pass: only keys that actually REPEAT in this file carry a contextual
        # suffix. Unique keys -- every entry in the baseline as recorded today -- keep
        # their plain form, so context sensitivity cannot churn them.
        repeated = {key for key in {k for k, _, _ in candidates}
                    if sum(1 for k, _, _ in candidates if k == key) > 1}
        identities = [(f"{key}@{digest}" if key in repeated else key, offset)
                      for key, digest, offset in candidates]
        # Residual: duplicates whose WIDENED context still matches, i.e. a run of
        # identical adjacent blocks. Every member of such a group takes a statement
        # offset -- a position in the function, not an occurrence ordinal. The ordinal
        # this replaces renumbered with the group, so fixing one member and adding
        # another produced the identical key set and the ratchet passed; an offset
        # cannot, because the added member sits somewhere the baseline never recorded.
        colliding = {identity for identity in {i for i, _ in identities}
                     if sum(1 for i, _ in identities if i == identity) > 1}
        for identity, offset in identities:
            sites.append(f"{identity}~{offset}" if identity in colliding else identity)
    return sorted(sites), errors


def _base_key(site: str) -> str:
    """A site identity stripped of its duplicate-disambiguation suffix.

    `@digest` and `@digest~offset` are attached only while a key REPEATS in its file
    (see find_sites), so the same statement is recorded one way as a duplicate and
    another way once its sibling is gone. `@` cannot occur in a path, a C++ qualified
    name or an expression, so splitting on it recovers the underlying key exactly.
    """
    return site.split("@", 1)[0]


def partition_sites(
    sites: list[str],
    baseline: list[str],
    claims: list[tuple[str, str]] | None = None,
) -> tuple[list[str], list[str]]:
    """Classify the scan against the baseline: (new_sites, no_longer_present).

    Exact matches are consumed first; only then may a currently-UNIQUE key claim a
    baseline entry recorded under the same base key with a duplicate suffix. Pass
    `claims` to collect every such (site, claimed entry) pair -- see the residual at
    the bottom of this docstring, which is why the caller must be able to say so.

    That second step is not a relaxation, it is what makes a legitimate FIX possible.
    Two duplicates are baselined as `k@d1` and `k@d2`; fixing one leaves a single
    statement, which is no longer a duplicate and so is emitted as plain `k`. Compared
    by exact string, the untouched survivor read as a NEW site -- the guard failed on a
    repair -- and `--regenerate` then refused to record the replacement key, so the
    ratchet could not shrink at all. Measured, not argued: that pair is
    test_fixing_one_duplicate_keeps_the_survivor.

    Two conditions bound the claim, because "the base key matches" on its own is not
    evidence that the plain site IS one of the recorded statements:

    * **it must be the sole scan entry at that base key.** The plain form means the key
      occurs exactly once in its file, so a second scan entry sharing the base key --
      plain or suffixed -- contradicts the premise the claim rests on. Unenforced, two
      plain `k`s consumed one recorded entry EACH and both vanished silently
      (`partition_sites(["k", "k"], ["k@d1", "k@d2"]) -> ([], [])`);
    * **the baseline must record a duplicate GROUP -- two or more entries -- at that
      base key.** A shrink is the only story the claim models, and a lone `k@d1` is not
      a group that could shrink: a suffix is emitted only while a key REPEATS, so a
      single suffixed entry cannot have come from a scan of its own file. Unenforced,
      `partition_sites(["k"], ["k@d1"]) -> ([], [])` -- a new site consumed the entry
      and `no_longer_present` was empty too, so the run printed "no new ones" and the
      site left no trace anywhere in the output.

    Residual, stated rather than papered over. Within a group that really did shrink,
    this cannot tell the SURVIVOR from a statement newly written at the same base key
    after every recorded member was repaired: both produce a single plain `k` against
    `[k@d1, k@d2]`, byte for byte. No comparison of the two string lists can separate
    them, and the context digest cannot either -- it is unstable across exactly the
    edit that creates the situation (repairing a sibling rewrites the survivor's
    context window; see test_fixing_one_duplicate_adds_another). A check strong enough
    to reject the addition therefore also rejects the repair, which is the regression
    the claim exists to prevent. So the claim is REPORTED instead: `claims` carries it
    out and main() prints it, because an undecidable case that leaves no trace in the
    output is the one shape this guard must never produce.

    Note this residual needs the whole recorded group to disappear first. Repairing one
    duplicate and adding another leaves two live sites at the key, so both are suffixed,
    both are unmatched, and both are reported -- test_fixing_one_duplicate_adds_another.
    """
    remaining = list(baseline)
    unmatched: list[str] = []
    for site in sites:
        if site in remaining:
            remaining.remove(site)
        else:
            unmatched.append(site)

    new_sites: list[str] = []
    for site in unmatched:
        if "@" in site:
            new_sites.append(site)
            continue
        if sum(1 for other in sites if _base_key(other) == site) != 1:
            # Not the sole scan entry at this base key, so "the group shrank to this
            # one statement" is not what the scan says. Report it.
            new_sites.append(site)
            continue
        group = [entry for entry in remaining if _base_key(entry) == site]
        if len(group) < 2:
            new_sites.append(site)
            continue
        remaining.remove(group[0])
        if claims is not None:
            claims.append((site, group[0]))
    return new_sites, remaining


def _report_base_key_claims(claims: list[tuple[str, str]]) -> None:
    """Print every duplicate entry that was matched by BASE KEY rather than exactly.

    The one case partition_sites() cannot decide (see its docstring): inside a duplicate
    group that shrank to a single statement, the survivor and a statement newly written
    at the same base key after every recorded member was repaired produce identical
    input. The guard has to accept the claim, or every legitimate repair fails; what it
    must not do is accept it SILENTLY, because then the only trace of an undecided
    question is a line saying the ratchet got better.
    """
    if not claims:
        return
    print(f"[unchecked-resize] NOTE: {len(claims)} site(s) matched a recorded DUPLICATE entry "
          "by base key, not exactly:")
    for site, recorded in claims:
        print(f"    assumed survivor: {site}")
        print(f"      claimed entry:  {recorded}")
    print("  Each is assumed to be the surviving statement of a duplicate group whose other\n"
          "  member(s) were repaired. This script cannot verify that -- a statement newly\n"
          "  written at the same file/function/variable/count after the whole recorded group\n"
          "  was repaired is indistinguishable from it. Check the diff before trusting the\n"
          "  reduction.")


def load_baseline_document() -> tuple[dict, list[str]]:
    """The baseline file as a document. Unreadable or malformed is a FAILURE.

    A missing file yields an empty document, which makes every scanned site read as
    new -- loud, and in the safe direction. Malformed JSON used to raise out of the
    previous `load_baseline()` as an unhandled traceback; it is reported as a guard
    failure now so the reason is legible in the CI log.
    """
    if not BASELINE_PATH.exists():
        return {}, []
    try:
        data = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        return {}, [f"baseline {BASELINE_PATH} is not readable JSON: {exc}"]
    if not isinstance(data, dict):
        return {}, [f"baseline {BASELINE_PATH} is not a JSON object."]
    if not isinstance(data.get("sites", []), list):
        return {}, [f"baseline {BASELINE_PATH} has a malformed 'sites' list."]
    return data, []


def load_ledger(document: dict) -> tuple[dict[str, str], list[str]]:
    """The detector-change ledger: site -> the reason recorded for adding it.

    Malformed entries FAIL rather than being ignored: an ignored entry is a claim the
    guard silently declined to evaluate, and the only thing this ledger does is license
    an addition.
    """
    raw = document.get(LEDGER_KEY, [])
    if not isinstance(raw, list):
        return {}, [f"'{LEDGER_KEY}' must be a list of objects."]
    ledger: dict[str, str] = {}
    failures: list[str] = []
    for entry in raw:
        if not isinstance(entry, dict):
            failures.append(f"'{LEDGER_KEY}' entry is not an object: {entry!r}")
            continue
        site, reason = entry.get("site"), entry.get("reason")
        if not isinstance(site, str) or not site.strip():
            failures.append(f"'{LEDGER_KEY}' entry needs a non-empty 'site': {entry!r}")
            continue
        if not isinstance(reason, str) or not reason.strip():
            failures.append(f"'{LEDGER_KEY}' entry for '{site}' needs a non-empty 'reason'.")
            continue
        ledger[site] = reason.strip()
    return ledger, failures


def write_baseline(sites: list[str], ledger: dict[str, str] | None = None) -> None:
    BASELINE_PATH.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "comment": (
                    "GENERATED by tests/ci/check_unchecked_resize.py --regenerate. "
                    "Known unchecked Vector::resize() sites feeding a raw write (#794, #798). "
                    "This list is a ratchet, NOT an assertion that these sites are safe. "
                    "It must only ever shrink; a new entry means a new unchecked site. "
                    "schema 2: keys carry the ENCLOSING FUNCTION name "
                    "(file::function::symbol.resize(count)). Under schema 1 the key omitted the "
                    "function, so distinct statements sharing a file/symbol/count collapsed onto "
                    "one key -- 80 statements folded into 69 keys, with result.resize(splat_count) "
                    "in gaussian_splat_asset.cpp standing in for eight separate getters. Fixing "
                    "one of those, or adding a ninth, left the key set unchanged and the ratchet "
                    "passed silently. --regenerate now REFUSES to add any key; a change to the "
                    "DETECTOR itself (key format, or a widened predicate that reveals "
                    "pre-existing sites) uses --accept-detector-change and must have its full "
                    "delta reviewed in the PR. The render_resource_orchestrator.cpp "
                    "vertex_data entry arrived that way: comment masking made a site that had "
                    "always been there visible for the first time. It is NOT newly introduced "
                    "and is NOT blessed as safe -- it is tracked for a fix under #798. Every "
                    "such addition is recorded in detector_change_ledger with a reason, and "
                    "the guard rejects an addition relative to the REVIEW BASE unless the "
                    "ledger covers it AND this script itself differs from the base."
                ),
                "sites": sites,
                LEDGER_KEY: [
                    {"site": site, "reason": reason}
                    for site, reason in sorted((ledger or {}).items())
                    if site in set(sites)
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def _git(args: list[str]) -> tuple[int, str, str]:
    try:
        result = subprocess.run(
            ["git", *args],
            cwd=SCRIPT_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except (OSError, ValueError) as exc:
        return 1, "", str(exc)
    return result.returncode, result.stdout, result.stderr


def resolve_review_base(base_ref: str | None = None) -> tuple[str | None, list[str]]:
    """The immutable review base, delegated to the shared resolver.

    Importing the environment-skip guard for this is deliberate. It is the same
    repository, the same lane and the same question, it has already been through three
    review rounds on exactly this resolution order, and every workflow step that runs
    `run_module_tests.py --guard-only` exports GS_CI_BASE_REF for it. A private copy
    would answer the question differently the first time either was edited.

    An import failure is a FAILURE, not a fallback: without a base there is nothing
    immutable to compare the baseline against, and "could not look" must never be
    reported as "found nothing".

    One consequence of sharing it, stated rather than discovered later: the resolver's
    first environment variable is GS_CI_ENV_SKIP_BASE_REF, so overriding the base for
    the environment-skip guard moves this guard's base too. They are two ratchets on one
    diff and there is only one review base, so answering differently would be the bug.
    """
    try:
        spec = importlib.util.spec_from_file_location(
            "_gs_review_base_resolver", BASE_RESOLVER_PATH
        )
        if spec is None or spec.loader is None:
            raise ImportError(f"no loader for {BASE_RESOLVER_PATH}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        resolve = module.resolve_base_sha
    except Exception as exc:  # noqa: BLE001 -- any failure here must fail closed
        return None, [
            f"cannot load the shared review-base resolver from {BASE_RESOLVER_PATH}: {exc}. "
            f"The baseline can only be graded against the review base, so this run has no "
            f"reference and fails rather than passing on an unanchored comparison."
        ]
    return resolve(base_ref)


def _blob_at_base(base_sha: str, path: pathlib.Path) -> tuple[str | None, list[str]]:
    """File content at the review base, or ABSENT_AT_BASE, or a failure.

    Absence is established with `ls-tree` BEFORE `show` is attempted, deliberately.
    `git show <sha>:<path>` exits non-zero both when the path is not in that tree and
    when git could not answer at all -- a corrupt object store, an unfetched base, no
    git on PATH. Mapping both onto "the file did not exist at the base" would turn every
    such breakage into the ABSENT_AT_BASE branch, which passes the comparison; the
    ratchet would then be disabled by anything that broke git, silently and in exactly
    the conditions where nobody is looking. `ls-tree` separates them: it exits 0 with no
    output for a path that is genuinely absent, and non-zero when git failed.
    """
    try:
        rel = path.resolve().relative_to(SCRIPT_ROOT).as_posix()
    except ValueError:
        return None, [f"{path} is outside {SCRIPT_ROOT}; cannot locate it at the review base."]
    code, listing, err = _git(["ls-tree", "-r", "--name-only", base_sha, "--", rel])
    if code != 0:
        return None, [
            f"git could not read the tree at review base {base_sha[:12]} for '{rel}' "
            f"(exit {code}): {err.strip() or 'no stderr'}. Refusing to read that as the "
            f"file being absent -- the two are different answers."
        ]
    if not listing.strip():
        return ABSENT_AT_BASE, []
    code, out, err = _git(["show", f"{base_sha}:{rel}"])
    if code != 0:
        return None, [
            f"'{rel}' is present in the tree at review base {base_sha[:12]} but could not "
            f"be read (exit {code}): {err.strip() or 'no stderr'}."
        ]
    return out, []


def detector_differs_from_base(base_sha: str) -> tuple[bool, list[str]]:
    """Whether THIS SCRIPT differs from its committed form at the review base.

    This is what makes the detector-change ledger something other than a second way to
    write whatever you like into the baseline. A ledger entry is a CLAIM that the
    addition comes from the detector changing rather than from new code; if the detector
    is byte-identical to the base, the claim is false on its face and no ledger entry can
    rescue it. A change that only touches C++ therefore has no route to add a key at all.
    """
    content, failures = _blob_at_base(base_sha, pathlib.Path(__file__).resolve())
    if failures:
        return False, failures
    if content is ABSENT_AT_BASE:
        return True, []  # this change introduces the detector
    try:
        current = pathlib.Path(__file__).resolve().read_text(encoding="utf-8")
    except OSError as exc:
        return False, [f"cannot read this script to compare it against the review base: {exc}"]
    return current != content, []


def _additions_against_base(sites: list[str], base_ref: str | None = None) -> tuple[list[str], list[str]]:
    """Sites the write path is about to record that the review base does not have.

    Best effort by design, and only ever used to SEED the ledger. If the base cannot be
    read here the write still happens and the omission is reported, because the verify
    path re-derives the same set and fails on anything the ledger does not cover -- the
    guard's answer must not depend on a local tool having had network or a fetched base.
    """
    base_sha, failures = resolve_review_base(base_ref)
    if failures or base_sha is None:
        return [], failures or ["the review base did not resolve."]
    raw, failures = _blob_at_base(base_sha, BASELINE_PATH)
    if failures:
        return [], failures
    if raw is ABSENT_AT_BASE:
        # The change introduces the baseline. check_baseline_against_base() has no
        # shrink-only reference in that case and demands no ledger, so seeding one with
        # all 81 sites under a single reason would be noise, not evidence.
        return [], []
    try:
        base_document = json.loads(raw)
        base_sites = [str(s) for s in base_document["sites"]]
    except (json.JSONDecodeError, KeyError, TypeError) as exc:
        return [], [f"baseline at review base {base_sha[:12]} is unusable: {exc}"]
    added = collections.Counter(sites) - collections.Counter(base_sites)
    return sorted(added.elements()), []


def check_baseline_against_base(document: dict, base_ref: str | None = None) -> list[str]:
    """The COMMITTED baseline may only shrink relative to the review base.

    Without this the ratchet grades the change against itself. `load_baseline_document()` reads
    the baseline out of the proposed commit, so a change that adds an unsafe site AND
    writes its key into `unchecked_resize_baseline.json` -- by hand, or through
    `--accept-detector-change` -- produces an empty `new_sites` and exits 0. Both sides
    moved together, which is not a ratchet; this repo has already recorded that exact
    lesson twice (tests/ci/test_gpu_harness_deferred_contract.py, and #595 round 3 in
    check_environment_skip_marker.py, whose resolver this reuses).

    Growth is permitted on exactly one route, and it has two independent locks:

    * every added key is named in `detector_change_ledger` with a reason, and
    * this script DIFFERS from its form at the review base.

    The second lock is the load-bearing one. A reason is self-attested and a reviewer
    must judge it; "the detector actually changed" is a fact git can settle, and it
    denies the whole route to any change that does not touch the detector.
    """
    base_sha, failures = resolve_review_base(base_ref)
    if failures or base_sha is None:
        return failures or ["the review base did not resolve."]

    raw, failures = _blob_at_base(base_sha, BASELINE_PATH)
    if failures:
        return failures
    if raw is ABSENT_AT_BASE:
        print("[unchecked-resize] NOTE the baseline does not exist at the review base "
              f"{base_sha[:12]}, so this change introduces it; there is no shrink-only "
              "reference this run. Review the whole file, not a diff.")
        return []
    try:
        base_document = json.loads(raw)
    except json.JSONDecodeError as exc:
        return [f"baseline at review base {base_sha[:12]} is not valid JSON: {exc}"]
    if not isinstance(base_document, dict) or not isinstance(base_document.get("sites"), list):
        return [f"baseline at review base {base_sha[:12]} has no 'sites' list; refusing to compare."]

    current = collections.Counter(str(s) for s in document.get("sites", []))
    previous = collections.Counter(str(s) for s in base_document["sites"])
    added = sorted((current - previous).elements())
    if not added:
        return []

    ledger, ledger_failures = load_ledger(document)
    if ledger_failures:
        return ledger_failures

    differs, failures = detector_differs_from_base(base_sha)
    if failures:
        return failures
    if not differs:
        return [
            f"the BASELINE FILE gained {len(added)} site(s) relative to the review base "
            f"{base_sha[:12]} while {pathlib.Path(__file__).name} is byte-identical to the "
            f"base. The only sanctioned growth is a DETECTOR change, and the detector did "
            f"not change, so these keys record new unchecked sites:",
        ] + [f"    + {site}" for site in added]

    uncovered = [site for site in added if site not in ledger]
    if uncovered:
        return [
            f"the BASELINE FILE gained {len(added)} site(s) relative to the review base "
            f"{base_sha[:12]}, and {len(uncovered)} of them are not recorded in "
            f"'{LEDGER_KEY}'. An addition has to say why it is not a new defect:",
        ] + [f"    + {site}" for site in uncovered]

    print(f"[unchecked-resize] NOTE {len(added)} baseline addition(s) relative to the review "
          f"base {base_sha[:12]}, each attributed to a DETECTOR change and each a claim to "
          f"check against the diff:")
    for site in added:
        print(f"    + {site}\n      reason: {ledger[site]}")
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regenerate", action="store_true",
                        help="rewrite the baseline from the current tree (may only shrink it)")
    parser.add_argument("--accept-detector-change", "--migrate-key-schema",
                        dest="accept_detector_change", action="store_true",
                        help="rewrite the baseline when THE DETECTOR changed rather than the "
                             "tree: a new key format, or a widened predicate that reveals "
                             "sites which were always present but previously invisible. Prints "
                             "the full added/removed sets, which must be reviewed in the PR. "
                             "Never used by CI, and never a way to record a NEW defect.")
    parser.add_argument("--reason", default="",
                        help="required with --accept-detector-change: why each added key was "
                             "not already visible. Recorded per site in the baseline's "
                             f"'{LEDGER_KEY}' and printed by the guard for review.")
    parser.add_argument("--base-ref", dest="base_ref", default=None,
                        help="the review base to grade the committed baseline against. "
                             "Defaults to the same resolution order as the environment-skip "
                             "guard (GS_CI_BASE_REF / GITHUB_BASE_REF / ..., then origin/master).")
    args = parser.parse_args()

    sites, scan_errors = find_sites()

    # Fail closed on an incomplete scan -- an unreadable source, or a resize() call the
    # scanner could not assemble. A partial scan cannot distinguish "no new site" from
    # "did not look", and the old code returned success either way.
    if scan_errors:
        print("[unchecked-resize] FAILED: the module scan is incomplete, so its result "
              "cannot be trusted.\n")
        for item in scan_errors:
            print(f"  {item}")
        return 1

    document, document_failures = load_baseline_document()
    if document_failures:
        print("[unchecked-resize] FAILED: the baseline cannot be read.\n")
        for item in document_failures:
            print(f"  {item}")
        return 1

    if args.regenerate or args.accept_detector_change:
        if args.accept_detector_change and not args.reason.strip():
            print("[unchecked-resize] REFUSED: --accept-detector-change requires --reason, "
                  "which is recorded per added site and is what a reviewer grades. An "
                  "addition with no stated reason is indistinguishable from a blessed defect.")
            return 1

        previous = sorted(str(s) for s in document.get("sites", []))
        claims: list[tuple[str, str]] = []
        added, removed = partition_sites(sites, previous, claims)

        ledger, ledger_failures = load_ledger(document)
        if ledger_failures:
            print("[unchecked-resize] FAILED: the baseline's detector-change ledger is "
                  "malformed.\n")
            for item in ledger_failures:
                print(f"  {item}")
            return 1

        if added and not args.accept_detector_change:
            # Set inclusion, NOT a net count. Fixing one old site while introducing a
            # new one nets zero, and the old code wrote the new site into the baseline
            # and returned 0 -- after which normal CI passed on the committed file.
            print("[unchecked-resize] REFUSED: regeneration would ADD new site(s) to the "
                  "baseline. Regenerating records fixes; it must never bless a new defect.\n")
            for site in added:
                print(f"  + {site}")
            print("\nFix the site(s). Only if THE DETECTOR changed -- a new key format, or a "
                  "widened predicate exposing sites that were always there -- re-run with "
                  "--accept-detector-change and have the full delta reviewed in the PR.")
            return 1

        # The ledger records what this CHANGE adds, so it is computed against the review
        # base rather than against the working-tree baseline. Recording additions
        # relative to the file being overwritten would leave a key added by an earlier
        # --accept-detector-change in the same branch uncovered, and the verify path
        # would then reject the branch's own baseline.
        base_added, base_failures = _additions_against_base(sites, args.base_ref)
        if base_failures:
            print("[unchecked-resize] NOTE could not read the baseline at the review base, so "
                  "the ledger below covers only what changed against the working-tree "
                  "baseline. The verify run will re-check this and fail if it is short:")
            for item in base_failures:
                print(f"  {item}")
        if args.accept_detector_change:
            for site in sorted(set(added) | set(base_added)):
                ledger[site] = args.reason.strip()

        write_baseline(sites, ledger)
        print(f"[unchecked-resize] baseline written: {len(sites)} site(s) "
              f"(previous {len(previous)}; -{len(removed)} removed, +{len(added)} added).")
        for site in removed:
            print(f"  - {site}")
        for site in added:
            print(f"  + {site}")
        _report_base_key_claims(claims)
        if added or base_added:
            print("\n[unchecked-resize] DETECTOR CHANGE: the additions above are attributed to a "
                  "changed key format or a widened predicate, NOT to newly written code. That "
                  "attribution is a claim, not a fact this script can verify -- review each "
                  "addition against the diff before committing. The guard re-checks it against "
                  "the review base and rejects it outright unless this script itself changed.")
        return 0

    baseline = sorted(str(s) for s in document.get("sites", []))
    claims = []
    new_sites, fixed_sites = partition_sites(sites, baseline, claims)

    # The committed baseline is graded against the REVIEW BASE, not against itself.
    # Everything above reads the baseline out of the proposed commit, so on its own it
    # cannot see a change that adds an unsafe site and its key in the same diff.
    base_failures = check_baseline_against_base(document, args.base_ref)
    if base_failures:
        print("[unchecked-resize] FAILED: the committed baseline did not survive comparison "
              "with the review base.\n")
        for item in base_failures:
            print(f"  {item}")
        return 1

    _report_base_key_claims(claims)

    if new_sites:
        print("[unchecked-resize] FAILED: new unchecked resize()->raw-write site(s) found.\n")
        for site in new_sites:
            print(f"  {site}")
        print(
            "\nEach is an unchecked `Vector::resize()` with a runtime-sized count whose result\n"
            "is later written through `write[]`, `ptrw()` or `ptr()` in the same function.\n"
            "A failed resize leaves the vector at its PREVIOUS size, so the write can run past\n"
            "the end of a live allocation (see this script's docstring for the three shapes).\n\n"
            "Fix with gs_resize_or_fail() from modules/gaussian_splatting/core/gs_vector_alloc.h,\n"
            "applying the enclosing function's OWN existing failure contract. If the site is\n"
            "genuinely safe (index bounded by the vector's own size(), or a compile-time-constant\n"
            "count), say so in a comment at the site -- do NOT re-baseline to silence this.\n"
        )
        return 1

    if fixed_sites:
        print(f"[unchecked-resize] PASSED: {len(sites)} known site(s); "
              f"{len(fixed_sites)} baseline entr(y/ies) no longer present.")
        print("  Run with --regenerate to record the fixes and tighten the ratchet:")
        for site in fixed_sites[:10]:
            print(f"    fixed: {site}")
        if len(fixed_sites) > 10:
            print(f"    ... and {len(fixed_sites) - 10} more")
        return 0

    print(f"[unchecked-resize] PASSED: {len(sites)} known site(s), no new ones.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
