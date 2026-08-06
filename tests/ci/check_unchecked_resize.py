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
`--accept-detector-change`, it is never run by CI, and it prints the full delta so each
addition is reviewed against the diff. Widening detection and then declining to record
what it finds would leave the guard permanently red; recording it silently would be
indistinguishable from blessing a new defect. Printing it and naming the reason is the
only honest option, and the reason is a claim a reviewer must check, not a fact this
script can prove.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_ROOT = REPO_ROOT / "modules" / "gaussian_splatting"
BASELINE_PATH = REPO_ROOT / "tests" / "ci" / "unchecked_resize_baseline.json"

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

DECL_LOOKBACK = 70
# How many physical lines a single statement may span before assembly gives up.
# This bounds only STATEMENT assembly, never function scope -- see _function_spans().
MAX_STATEMENT_LINES = 12


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
    char literals. Not handled, and stated rather than hidden: `#if 0` blocks are still
    counted as live code, so braces inside a disabled region can still miscount depth.
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
            if ch == "'" and i > 0 and (line[i - 1].isalnum() or line[i - 1] == "_"):
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


def _declared_container(lines: list[str], index: int, var: str) -> str:
    """Resolve the container kind by scanning back for a mention of the symbol."""
    base = re.escape(var.split(".")[-1])
    for k in range(index, max(-1, index - DECL_LOOKBACK), -1):
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

    Returns (text, last_line_index). A resize call split across lines is a single
    statement; matching per physical line missed it entirely, which let a new
    unsafe allocation through. Assembly stops when the parentheses opened by the
    call have balanced AND a ';' has been seen after the closing paren.
    """
    text = ""
    for j in range(start, min(start + MAX_STATEMENT_LINES, len(lines))):
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


def _context_digest(lines: list[str], start: int, last: int) -> str:
    """A stable identity for one of several IDENTICAL statements in one function.

    An occurrence ORDINAL (`key`, `key#2`, ...) does not identify a statement, it only
    counts them: fixing the first duplicate while adding a new unchecked one elsewhere
    in the same function leaves both the number and the exact set of keys unchanged, so
    the ratchet passes over a real new site. That is the same "cardinality is not
    identity" hole the function name fixed one level up.

    The identity used instead is the nearest preceding and nearest following statement
    (whitespace-collapsed, braces skipped), which distinguishes two copies that sit in
    different places while staying stable under line shifts elsewhere in the file.

    Deliberately narrow: this suffix is only attached to keys that actually REPEAT
    within a file (see find_sites). A unique key keeps its plain, context-free form, so
    an edit next to a normal site can never make it look new. Two duplicates whose
    surrounding statements are also identical remain indistinguishable by any textual
    identity; they fall back to an ordinal, which at least preserves their COUNT.
    """
    def _neighbour(rng) -> str:
        for k in rng:
            text = _normalise(lines[k])
            if text and text not in ("{", "}", "{}"):
                return text
        return ""

    before = _neighbour(range(start - 1, -1, -1))
    after = _neighbour(range(last + 1, len(lines)))
    return hashlib.sha1(f"{before}|{after}".encode("utf-8")).hexdigest()[:8]


def find_sites() -> tuple[list[str], list[str]]:
    """Return (sites, read_errors).

    read_errors is NOT cosmetic: an unreadable source used to be skipped with
    `continue`, which dropped every site in that file and still let the run exit 0,
    reporting the file's baseline entries as "fixed". Incomplete scanning must never
    be accepted as evidence that no new site exists, so the caller fails on it.
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
        candidates: list[tuple[str, str]] = []
        for i, line in enumerate(lines):
            if not RESIZE_HEAD_RE.match(line):
                continue
            statement, last = _assemble_statement(lines, i)
            if not statement:
                continue
            match = RESIZE_RE.match(statement)
            if not match or CONSUMED_RE.search(statement):
                continue
            var, count = match.group(1), match.group(2)
            if CONST_COUNT_RE.match(count):
                continue  # exclusion rule: compile-time-constant count
            kind = _declared_container(lines, i, var)
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
                candidates.append((key, _context_digest(lines, i, last)))

        # Second pass: only keys that actually REPEAT in this file carry a contextual
        # suffix. Unique keys -- every entry in the baseline as recorded today -- keep
        # their plain form, so context sensitivity cannot churn them.
        repeated = {key for key in {k for k, _ in candidates}
                    if sum(1 for k, _ in candidates if k == key) > 1}
        used: dict[str, int] = {}
        for key, digest in candidates:
            if key not in repeated:
                sites.append(key)
                continue
            identity = f"{key}@{digest}"
            nth = used.get(identity, 0) + 1
            used[identity] = nth
            # Ordinal only for the residual case of two duplicates with IDENTICAL
            # surrounding statements, which no textual identity can tell apart. It
            # preserves the count, so adding a third still fails the ratchet.
            sites.append(identity if nth == 1 else f"{identity}#{nth}")
    return sorted(sites), errors


def load_baseline() -> list[str]:
    if not BASELINE_PATH.exists():
        return []
    data = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    return sorted(data.get("sites", []))


def write_baseline(sites: list[str]) -> None:
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
                    "and is NOT blessed as safe -- it is tracked for a fix under #798."
                ),
                "sites": sites,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


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
    args = parser.parse_args()

    sites, read_errors = find_sites()

    # Fail closed on unreadable sources. A partial scan cannot distinguish "no new
    # site" from "did not look", and the old code returned success either way.
    if read_errors:
        print("[unchecked-resize] FAILED: could not read module source(s); the scan is "
              "incomplete and its result cannot be trusted.\n")
        for item in read_errors:
            print(f"  {item}")
        return 1

    if args.regenerate or args.accept_detector_change:
        previous = load_baseline()
        added = sorted(set(sites) - set(previous))
        removed = sorted(set(previous) - set(sites))

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

        write_baseline(sites)
        print(f"[unchecked-resize] baseline written: {len(sites)} site(s) "
              f"(previous {len(previous)}; -{len(removed)} removed, +{len(added)} added).")
        for site in removed:
            print(f"  - {site}")
        for site in added:
            print(f"  + {site}")
        if added:
            print("\n[unchecked-resize] DETECTOR CHANGE: the additions above are attributed to a "
                  "changed key format or a widened predicate, NOT to newly written code. That "
                  "attribution is a claim, not a fact this script can verify -- review each "
                  "addition against the diff before committing, and record why it was not "
                  "already visible.")
        return 0

    baseline = load_baseline()
    new_sites = [s for s in sites if s not in baseline]
    fixed_sites = [s for s in baseline if s not in sites]

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
