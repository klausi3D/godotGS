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

RESIZE_RE = re.compile(
    r"^\s*([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)\s*"
    r"\.resize(?:_initialized|_uninitialized)?\s*\(\s*(.+?)\s*\)\s*;\s*$"
)
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
MAX_FUNCTION_SPAN = 200


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


def _function_end(lines: list[str], start: int) -> int:
    limit = min(start + MAX_FUNCTION_SPAN, len(lines))
    for j in range(start + 1, limit):
        if lines[j].startswith("}"):
            return j
    return limit


def find_sites() -> list[str]:
    sites: list[str] = []
    for path in _module_sources():
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        for i, line in enumerate(lines):
            match = RESIZE_RE.match(line)
            if not match or CONSUMED_RE.search(line):
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
            end = _function_end(lines, i)
            if any(consumer.search(lines[j]) for j in range(i + 1, end)):
                # Key on file + symbol + count expression, NOT the line number. A
                # line-keyed baseline reports a false "new site" for every edit that
                # shifts lines, which would make this guard fire on unrelated changes
                # and get it switched off. Found by mutation-testing this script:
                # injecting one site made an untouched site 5 lines below look new.
                sites.append(f"{rel}::{var}.resize({_normalise(count)})")
    return sorted(set(sites))


def load_baseline() -> list[str]:
    if not BASELINE_PATH.exists():
        return []
    data = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    return sorted(data.get("sites", []))


def write_baseline(sites: list[str]) -> None:
    BASELINE_PATH.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "comment": (
                    "GENERATED by tests/ci/check_unchecked_resize.py --regenerate. "
                    "Known unchecked Vector::resize() sites feeding a raw write (#794, #798). "
                    "This list is a ratchet, NOT an assertion that these sites are safe. "
                    "It must only ever shrink; a new entry means a new unchecked site."
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
                        help="rewrite the baseline from the current tree (expected to shrink it)")
    args = parser.parse_args()

    sites = find_sites()

    if args.regenerate:
        previous = load_baseline()
        write_baseline(sites)
        delta = len(sites) - len(previous)
        print(f"[unchecked-resize] baseline regenerated: {len(sites)} site(s) "
              f"({delta:+d} vs previous {len(previous)}).")
        if delta > 0:
            print("[unchecked-resize] WARNING: the baseline GREW. Regenerating is for "
                  "recording fixes; a growth should be justified or fixed, not baselined.")
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
