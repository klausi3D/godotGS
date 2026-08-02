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

Reliable detection of 1-3 needs type and dataflow information, i.e. a clang-tidy check
or a compiler plugin. This is a ratchet against the pattern spreading, not a proof that
the module is free of it.

## Usage

    python tests/ci/check_unchecked_resize.py              # verify against the baseline
    python tests/ci/check_unchecked_resize.py --regenerate # after fixing sites

Regenerating is expected to SHRINK the baseline. If it would grow, the guard fails and
prints the new sites; that is the case that must be justified or fixed, never silently
re-baselined.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_ROOT = REPO_ROOT / "modules" / "gaussian_splatting"
BASELINE_PATH = REPO_ROOT / "tests" / "ci" / "unchecked_resize_baseline.json"

# Applied to an ASSEMBLED statement, not to a raw line: a normally formatted call
# such as `values.resize(` / `        runtime_count);` is a single statement split
# across two lines, and a line-anchored match silently skipped it -- so a later
# `values.ptrw()` was never examined and a new unsafe allocation passed the guard.
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

    Known limitation, stated rather than hidden: braces inside string literals or
    comments are not excluded, so depth can be miscounted in pathological files.
    The fallback in _consumer_scan_end() is therefore deliberately unbounded, so a
    miscount widens the scan rather than truncating it -- a guard that over-scans
    reports a site for review; one that under-scans misses a defect.
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
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            errors.append(f"{path.relative_to(REPO_ROOT).as_posix()}: {exc}")
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        spans = _function_spans(lines)
        seen_in_file: dict[str, int] = {}
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
                # under line shifts; an ordinal disambiguates true repeats within one
                # function.
                key = f"{rel}::{_function_name_at(i, spans)}::{var}.resize({_normalise(count)})"
                occurrence = seen_in_file.get(key, 0) + 1
                seen_in_file[key] = occurrence
                sites.append(key if occurrence == 1 else f"{key}#{occurrence}")
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
                    "passed silently. --regenerate now REFUSES to add any key; the one-time "
                    "schema migration used --migrate-key-schema."
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
    parser.add_argument("--migrate-key-schema", action="store_true",
                        help="one-time: rewrite the baseline when the KEY FORMAT itself changed. "
                             "Prints the full added/removed sets for review. Never used by CI.")
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

    if args.regenerate or args.migrate_key_schema:
        previous = load_baseline()
        added = sorted(set(sites) - set(previous))
        removed = sorted(set(previous) - set(sites))

        if added and not args.migrate_key_schema:
            # Set inclusion, NOT a net count. Fixing one old site while introducing a
            # new one nets zero, and the old code wrote the new site into the baseline
            # and returned 0 -- after which normal CI passed on the committed file.
            print("[unchecked-resize] REFUSED: regeneration would ADD new site(s) to the "
                  "baseline. Regenerating records fixes; it must never bless a new defect.\n")
            for site in added:
                print(f"  + {site}")
            print("\nFix the site(s), or if the key format itself changed, re-run with "
                  "--migrate-key-schema and have the full delta reviewed.")
            return 1

        write_baseline(sites)
        print(f"[unchecked-resize] baseline written: {len(sites)} site(s) "
              f"(previous {len(previous)}; -{len(removed)} removed, +{len(added)} added).")
        for site in removed:
            print(f"  - {site}")
        for site in added:
            print(f"  + {site}")
        if added:
            print("\n[unchecked-resize] KEY SCHEMA MIGRATION: the additions above are key-format "
                  "changes, not necessarily new defects. Review the mapping before committing.")
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
