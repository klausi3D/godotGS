#!/usr/bin/env python3
"""Guard: no `REQUIRE(<null-ish>)` is followed by a dereference of the same symbol (#656).

## The failure this guards against

`REQUIRE` does **not** abort a test case in this build. It reports and execution
continues into the next statement.

The mechanism is explicit in the build, not incidental. Both `tests/SCsub` and
`modules/gaussian_splatting/SCsub` define
`DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS` when `disable_exceptions` is
set (it defaults to `True` in `SConstruct`). That macro makes doctest define
`DOCTEST_CONFIG_NO_EXCEPTIONS`, under which:

    // thirdparty/doctest/doctest.h
    #else // DOCTEST_CONFIG_NO_EXCEPTIONS
        void throwException() {}

`REQUIRE`'s abort path is that `throwException()`. Compiled to nothing, `REQUIRE`
degrades into a louder `CHECK`. (The `_BUT_WITH_ALL_ASSERTS` half is what keeps
`REQUIRE` compiling at all: without it doctest `#undef`s `REQUIRE` and replaces it
with a `static_assert(false)`, so the alternative to "silently does not abort" is
"does not build".)

So this:

    REQUIRE(ptr != nullptr);
    ptr->method();

does not fail one test case. It segfaults the whole test binary, and every case
after it never runs. doctest reports no result for them, so the run ends up
*shorter* rather than *red* - and "fewer cases ran" is not a signal anyone alarms
on, especially alongside lanes that already silently skip (#520, #329).

The correct pattern is an explicit guard:

    if (!ptr) {
        FAIL("<what was missing and why the case cannot continue>");
        return;
    }

## What this guard flags (deliberately narrow)

Precision over recall. A null-ish `REQUIRE*` on a symbol, followed - within a
short forward window - by a statement that **dereferences that same symbol**,
where nothing in between could have made the dereference safe.

* Null-ish predicates: `x != nullptr`, `x != NULL`, `x.is_valid()`,
  `x->is_valid()`, `REQUIRE_FALSE(x.is_null())`, `REQUIRE_UNARY_FALSE(x.is_null())`,
  `REQUIRE_NE(x, nullptr)`. The predicate may span physical lines
  (`REQUIRE(
    ptr != nullptr);`) - continuation lines are joined until the
  parentheses balance.
* A "symbol" may be a member chain or a no-arg getter call, so
  `state.hierarchical_structure` and `loaded->get_gaussian_data()` are each one
  symbol. Calls WITH arguments are not: matching those textually would be
  comparing expressions, not tracking a symbol.
* Dereference: `x->`, `*x`, `x[`. Note `x.foo()` is NOT treated as a
  dereference - on a `Ref<T>` it is a safe call on the handle, and on a value
  type it is not a dereference at all.
* A dereference C++ short-circuiting cannot reach is not flagged:
  `ptr && ptr->f()`, `!ptr || ptr->f()`, `ptr ? ptr->f() : x`, and the explicit
  `ptr != nullptr && ptr->f()` / `ref.is_valid() && ref->f()` forms.
  The guard must **dominate** the dereference, not merely precede it textually:
  the expression is decomposed by precedence (`?:`, then `||`, then `&&`,
  descending one parenthesis layer at a time), so
    - `ptr && (a || ptr->f())`          -> safe, the outer `&&` dominates;
    - `(ptr && ptr->f()) || ptr->g()`   -> FLAGGED, `ptr->g()` runs precisely when
      the left disjunct is false, i.e. when ptr is null;
    - `ptr ? x : ptr->f()`              -> FLAGGED, the else-branch runs when null;
    - `ptr->f() && ptr`                 -> FLAGGED, the dereference is evaluated first.
  Anything the decomposition cannot parse unambiguously is treated as UNGUARDED,
  because a guard that under-reports is worse here than one that over-reports.
* The forward scan stops at anything that changes reachability or the symbol:
  `if` / `for` / `while` / `switch` / `return` / `else`, a block boundary, or a
  reassignment of the symbol. But it checks that statement's **header** before
  stopping, because a control-flow statement guards its body, never its own
  condition:
    - `REQUIRE(x); if (x) { x->f(); }`        -> NOT flagged, the `if` makes it safe;
    - `REQUIRE(x); if (x->is_ready()) { … }`  -> FLAGGED, the condition dereferences
      before any guarding can happen, and a non-aborting `REQUIRE` did not stop
      us reaching it.
* The scan crosses other assertion macros (the real corpus writes
  `REQUIRE(a); REQUIRE(b); a->f();`), and flags them if they themselves
  dereference the symbol.

## What this guard deliberately does NOT catch

Stated plainly, because a guard whose blind spots are undocumented invites
exactly the false confidence #656 is about:

1. **Bare `REQUIRE(x);`** with no comparison. It is indistinguishable from a
   boolean assertion without type information, and the corpus uses it for both.
2. **Dereferences through an alias.** `REQUIRE(a != nullptr); T *b = a; b->f();`
   is a real crash this guard does not see - it tracks one symbol, not
   assignment flow.
3. **Dereferences further away than the scan window.**
4. **Dereferences inside a control-flow BODY guarded by an unrelated condition**:
   `REQUIRE(ptr != nullptr); if (other) { ptr->f(); }` is a real crash, but the
   guard stops at the `if` because it cannot tell which conditions protect the
   symbol. Only the control-flow HEADER is checked - `if (ptr->is_ready())`
   evaluates the dereference before any guarding, so that IS flagged.
5. **Symbols reached through `::`.** `MessageQueue::get_main_singleton() != nullptr`
   is not matched, because the symbol grammar covers `.` and `->` chains only. Two
   such REQUIREs exist in the corpus today; neither is followed by a dereference,
   so this is latent. Widening the grammar is deliberately left as follow-up
   rather than bundled into an already long review.
6. **Dereferences inside a macro body** that expands to one, and
   dereferences of a container's *element* (`v[0]->f()` after
   `REQUIRE(!v.is_empty())`).
7. **Any REQUIRE whose failure is harmful for a non-dereference reason** - e.g.
   `REQUIRE(count == 3);` followed by code that indexes past the end.
   *One shape of (7) - a cardinality assertion followed by an index of the SAME
   container - is now covered by the second detector below (#844). The rest of
   (7) remains uncovered.*

Reliable detection of (2) and (4) needs real type and dataflow information, i.e.
a compiler plugin or a clang-tidy check, not a source scan. This guard is scoped
to the highest-confidence shape on purpose. It is a ratchet against the pattern
spreading, not a proof that the corpus is free of it: of the ~800 `REQUIRE*`
usages in the module tests, the null-ish subset alone is ~460, and most of those
are followed by something this guard cannot and should not judge.

## Second detector: size-assert-then-index (#844)

Same mechanism, different payload. `LocalVector::operator[]`
(`CRASH_BAD_UNSIGNED_INDEX`) and `CowData::get` (`CRASH_BAD_INDEX`) abort
unconditionally - not DEV-only - so:

    REQUIRE(payload.size() == 2);
    CHECK(payload[0].target_opacity == doctest::Approx(0.35f));   // runs anyway

is not a failing test, it is a process kill. Measured on PR #843 by perturbing a
fixture so one payload came back short, same machine, same `NodeSceneTree` batch:

| | cases reported | assertions | result |
| --- | ---: | ---: | --- |
| unguarded | **0 / 0** | **0 / 0** | `0xC0000409`, `Index p_index = 2 is out of bounds (size() = 2)` |
| guarded | 21 / 22 | 265 / 266 | one readable `FATAL ERROR`, all 22 cases ran |

**Zero cases reported** is what makes this P1: one short container silently
deletes an entire batch's results, and reads as an infrastructure hiccup rather
than a failure.

`CHECK` is covered as well as `REQUIRE`. `CHECK` never aborts under *any* doctest
configuration, so it is strictly worse, and one of the four sites #843 fixed
(`test_gaussian_splat_node.h:1323`) was a `CHECK`.

### What detector 2 flags

A `REQUIRE*`/`CHECK*` assertion whose predicate establishes a **lower bound** on
some container's length, followed - within the same short forward window - by an
index `container[...]` that nothing between them bounds.

* **Lower bound, not any mention of `size()`.** `size() == N` (N != 0), `size() >
  N`, `size() >= N` (N != 0), `size() != 0`, `!is_empty()`, `idx < size()` and the
  `_EQ/_NE/_GT/_GE/_LT/_LE` macro forms all qualify, and a C-style cast between
  the operator and the call (`idx < (uint32_t)splats.size()`) is peeled. `size() == 0`,
  `size() <= N`, `size() < N` and a positively asserted `is_empty()` do **not**:
  when those fail the container is LONGER than claimed, so a following index is
  not made unsafe by the failure. (`CHECK(state.cached_counts.is_empty())` in
  `test_tile_async_readback_freshness.cpp` is precisely that case, five statements
  above a real violation - counting it would have named the wrong assertion.)
* **The `size()` call must be a direct argument of the assertion macro** (depth 1
  inside its parentheses). `REQUIRE(cpu_results.resize(ground_truth.size()) == OK)`
  constrains the *resize result*, not `ground_truth`, and is not a site.
* **Loop-bounded indexes are safe and are not flagged.** An index inside a loop
  or `if` whose header bounds by the indexed container's OWN `size()` /
  `is_empty()` cannot go out of bounds no matter how the assertion failed:

      REQUIRE(opacities.size() == 4);
      for (uint32_t i = 0; i < opacities.size(); i++) {
          CHECK(Math::is_equal_approx(opacities[i], expected));   // NOT flagged
      }

  This is tracked with a block stack, not by stopping at the first control-flow
  statement, so the bound applies to the loop BODY and expires at its closing
  brace - `REQUIRE(a.size() == 3); for (i < a.size()) { a[i]; } CHECK(a[0]);` is
  still flagged on the post-loop `a[0]`.
* **A bound from a DIFFERENT container does not count.** In
  `for (i < a.size()) { CHECK(a[i] == b[i]); }` after `REQUIRE(b.size() == 3)`,
  `b[i]` crashes whenever `b` is the short one. Seven such sites exist; they are
  flagged, and reported separately from the straight-line ones so the two
  populations stay auditable (see "Reconciliation" below).
* **Short-circuiting is honoured**, reusing the same dominance decomposition as
  the null-deref detector with size-aware predicates, so
  `chunks.size() >= 2 && f(chunks[0])` is not flagged. No site in the corpus
  needs this today; it is here so widening the window later cannot introduce a
  false positive silently.
* The scan stops at a statement that can change the container's length
  (assignment, `resize`, `clear`, `push_back`, ...), and at a depth-0 `return`.

### What detector 2 deliberately does NOT catch

1. **An index further than `_SIZE_SCAN_STATEMENTS` statements away.** This is not
   hypothetical: the fourth site #843 fixed
   (`test_gaussian_splat_node.h:1415`, `REQUIRE(payload.size() == 4)` indexed
   ~20 statements later) is NOT found at the shipped window. Raising the window
   to 30 finds it plus two more real sites in
   `test_gaussian_splat_asset_prune.h` - and one false positive that only the
   short-circuit handling above suppresses. Widening is deliberately left as
   follow-up under #844 rather than bundled here; it changes the baseline and
   needs its own delta review. The other three #843 sites (`:1288`, `:1323`,
   `:1424`) ARE found by this detector at the shipped window - verified by
   running it against `test_gaussian_splat_node.h` at #843's base SHA
   `d9d2dfd2842`.
2. **Indexes through an alias** (`const T &e = v[0]` then `e`), through
   `.ptr()[i]` or `.get(i)`. Neither of the latter two occurs in the corpus.
3. **An index whose value is itself asserted elsewhere.** The detector never
   reasons about the index expression, only about the bound.
4. **Non-container `size()`** - anything named `size()` is treated as a length.

### Reconciliation of the count

#844's sweep of the corpus reported 60 size-shape sites, 14 loop-bounded, 46
dangerous, 4 fixed by #843 -> **42 remaining**. This detector reports **50**:
**43 straight-line** and **7 bounded only by another container's `size()`**. Both
figures are printed on every run so the split cannot quietly drift, and both are
pinned by a unit test.

The delta against 42 is +1 straight-line and +7 cross-container, and neither is
the baseline being tuned to fit:

* The **7** cross-container sites (e.g. `CHECK(a[i] == b[i])` inside
  `for (i < a.size())`, after `REQUIRE(b.size() == 3)`) sit inside a loop, so
  #844's sweep counted them with its 14 loop-bounded ones. The bound is on the
  WRONG container: `b[i]` crashes whenever `b` is the short one. They are real,
  so this detector is deliberately the stricter of the two.
* The **1** extra straight-line site is `test_lod_system.cpp:933`
  (`CHECK(idx < (uint32_t)splats.size());` then `splats[idx]`). It needs C-style
  cast handling to be recognised at all - an earlier revision of this detector
  missed it for exactly that reason - and it also sits inside a `for` bounded by
  a *different* container's `size()`, so a sweep would naturally have filed it
  under loop-bounded.

The three per-file concentrations #844 names reconcile **exactly** against the
straight-line population: `test_renderer_pipeline.h` 7, `test_resident_atlas_budget.h`
7, `test_gaussian_importance.h` 5 (its 6th site is one of the cross-container
seven). That agreement across three independent files is the evidence that the
43 is the same population as #844's 42 plus the one site above, not a different
set of the same size.

## Scope boundary

Scanned: `modules/gaussian_splatting/tests/*.{h,cpp}` and the top level of
`tests/test_*.cpp`. **Not** scanned: the rest of the engine test tree
(`tests/core/`, `tests/servers/`, ...). Those are upstream Godot's tests; they run
under the same no-exceptions configuration and the same crash is possible there,
but policing upstream is out of this module's scope and would bury the module
signal under an unownable baseline. If a module-owned test is ever added under a
nested engine test directory, widen `_test_sources()` rather than assuming it is
covered.

## Baseline

The pattern predates the guard: 325 sites across 32 files match it today. #656 is
explicit that they must not be mass-rewritten, so `require_null_deref_baseline.json`
records a **fingerprint per site** and the guard fails on any change to that set.

A count-only baseline is not enough. It licenses a swap: fix one site the
prescribed way and add a brand-new one in the same file, and the count is
unchanged, so the guard reports "0 new" and the new crash ships. The fingerprint
set reports both the removed and the added site.

The fingerprint is (symbol, predicate form, hash of the dereferencing statement) -
deliberately NOT the line number, which would go stale on every unrelated edit
above it and train people to regenerate without reading, which is how a guard
becomes a formality. The FULL statement is hashed: hashing a truncation made
sites differing only past the cut collapse into one identity, silently weakening
the ratchet. Truncation is a display concern only (see `_elide`).

The ratchet only turns one way. A **removed** fingerprint also fails, with an
instruction to delete it from the baseline - so fixing sites tightens the guard
permanently instead of leaving slack for new ones to occupy.

Detector 2 has its **own, separate** baseline (`size_then_index_baseline.json`),
with the same per-site fingerprint scheme and the same one-way ratchet. It is
**shrink-only**: the only legitimate edit to that file is a deletion. An added
fingerprint fails as a new violation; a fingerprint that no longer matches the
corpus also fails, and its only fix is to delete the entry, which shrinks the
baseline. `--regenerate-size-index-baseline` rewrites the file and REFUSES to
write it if that would add an entry, so the shrink-only property is mechanical
rather than a convention. The 42 conversions are deliberately NOT part of this
guard's landing: #844 records two hand-checked counter-examples
(`test_memory_leak_detection.h:165`, where an early `return` would skip
`track_resource_free` and poison every later `SUBCASE`; and
`test_resident_atlas_budget.h:109`, where three further independent assertions
follow, so the correct shape is an `else` branch) proving the conversion is not
mechanical. Converting blind trades a loud failure for quiet wrong results.

## Failing closed

A guard that cannot read or cannot parse must FAIL, never report "clean":

* a source file that cannot be read or decoded is a scan error, not an empty file;
* an assertion macro whose parentheses never balance within the continuation
  bound is a scan error, not an assertion with no size predicate;
* an unterminated raw string literal is a scan error;
* a missing or malformed baseline is a failure, for both baselines;
* an empty source list is a failure.

Scan errors fail the run before any baseline comparison, because a partial scan
cannot tell "no new site" from "did not look".
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from collections.abc import Callable
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"

# Pre-existing violations, so the guard can land without the 325-site rewrite
# #656 explicitly rules out. Tracking issue:
# https://github.com/klausi3D/godotGS/issues/656
#
# The baseline records a FINGERPRINT PER SITE, not a count per file. A count-only
# baseline licenses a swap: fix one site the prescribed way and add a brand-new
# one in the same file, and the count is unchanged, so the guard reports "0 new"
# and the new crash ships. A fingerprint set catches that -- the removed
# fingerprint and the added one are both reported.
#
# The fingerprint is (symbol, predicate form, hash of the dereferencing
# statement). Deliberately NOT the line number: a line-keyed baseline goes stale
# on every unrelated edit above it and trains people to regenerate without
# reading, which is how a guard becomes a formality. Renaming a variable does
# re-fingerprint its site; that surfaces as one removed + one added, which is
# accurate.
#
# This is a RATCHET: an added fingerprint fails (new violation), and a removed
# one also fails, telling you to drop it from the baseline. Never add a
# fingerprint to make a check pass - that is the one edit this file exists to
# prevent.
BASELINE_PATH = Path(__file__).resolve().parent / "require_null_deref_baseline.json"
BASELINE_ISSUE = "https://github.com/klausi3D/godotGS/issues/656"

# Detector 2 (#844) keeps its OWN baseline. Separate file, separate ratchet: the
# two detectors find different shapes with different conversion recipes, and
# folding them together would make either detector's delta unreadable in review.
SIZE_INDEX_BASELINE_PATH = Path(__file__).resolve().parent / "size_then_index_baseline.json"
SIZE_INDEX_ISSUE = "https://github.com/klausi3D/godotGS/issues/844"
SIZE_INDEX_REGENERATE_FLAG = "--regenerate-size-index-baseline"
_SIZE_INDEX_BASELINE_NOTE = (
    "Per-site fingerprints of pre-existing size-assert-then-index sites, generated by "
    "tests/ci/check_require_null_deref.py --regenerate-size-index-baseline (#844). This "
    "list is a RATCHET, not an assertion that these sites are safe: each one can still "
    "kill a whole test batch. It is SHRINK-ONLY -- the only legitimate edit is a "
    "deletion, made when the site is converted to `if (...) { FAIL(...); return; }` or "
    "to an `else` branch. Regeneration REFUSES to add an entry. #844 keeps the 42 "
    "conversions open deliberately: they are not mechanical (see "
    "test_memory_leak_detection.h:165 and test_resident_atlas_budget.h:109), and "
    "converting blind trades a loud failure for quiet wrong results."
)

# How many statements to look ahead after the REQUIRE before giving up.
_SCAN_STATEMENTS = 6

# A doctest assertion macro that we scan THROUGH (it does not change reachability).
_ASSERT_MACRO_RE = re.compile(
    r"^\s*(?:REQUIRE|CHECK|WARN|INFO|MESSAGE|CAPTURE)\w*\s*\(", re.IGNORECASE
)
# Statements that change reachability or scope: stop the scan (fail-safe: we
# would rather miss a violation than report a guarded dereference).
_CONTROL_FLOW_RE = re.compile(
    r"^\s*(?:\}|\{|if\b|else\b|for\b|while\b|switch\b|case\b|default\s*:|return\b|"
    r"break\b|continue\b|do\b|try\b|catch\b|SUBCASE\b|TEST_CASE\b)"
)

# A C++ identifier, or a member chain reached through '.' / '->' that we treat as
# a single symbol (e.g. `state.hierarchical_structure`,
# `resource_state.buffer_manager`). Segments may end in a NO-ARG call, so a
# getter form like `loaded->get_gaussian_data()` is one symbol too - that shape
# occurs in the corpus (test_gaussian_splat_world_io.h:711) and was previously
# skipped entirely (Codex, PR #659). Arguments are deliberately not supported:
# matching `f(a, b)` textually would start comparing expressions, not symbols.
# The chain is matched greedily; regex backtracking peels the trailing
# `.is_valid()` / `.is_null()` back off in the predicate patterns below.
_SYMBOL = r"[A-Za-z_]\w*(?:\s*\(\s*\))?(?:\s*(?:\.|->)\s*[A-Za-z_]\w*(?:\s*\(\s*\))?)*"

# Null-ish REQUIRE forms. Each yields the symbol asserted to be non-null.
_NULLISH_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("!= nullptr", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*nullptr\b")),
    ("!= NULL", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*({_SYMBOL})\s*!=\s*NULL\b")),
    ("nullptr !=", re.compile(rf"^\s*REQUIRE(?:_MESSAGE)?\s*\(\s*nullptr\s*!=\s*({_SYMBOL})\b")),
    ("is_valid()", re.compile(rf"^\s*REQUIRE(?:_MESSAGE|_UNARY)?\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_valid\s*\(\s*\)")),
    # doctest exposes REQUIRE_FALSE, REQUIRE_FALSE_MESSAGE and REQUIRE_UNARY_FALSE.
    # An earlier `REQUIRE_FALSE(?:_MESSAGE|_UNARY_FALSE)?` put the alternation on
    # the wrong side of the prefix: it accepted the NONEXISTENT
    # REQUIRE_FALSE_UNARY_FALSE and missed the real REQUIRE_UNARY_FALSE
    # (Codex, PR #659).
    ("!is_null()", re.compile(rf"^\s*(?:REQUIRE_FALSE(?:_MESSAGE)?|REQUIRE_UNARY_FALSE)\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_null\s*\(\s*\)")),
    ("REQUIRE_NE nullptr", re.compile(rf"^\s*REQUIRE_NE\s*\(\s*({_SYMBOL})\s*,\s*nullptr\s*\)")),
    ("REQUIRE_NE nullptr", re.compile(rf"^\s*REQUIRE_NE\s*\(\s*nullptr\s*,\s*({_SYMBOL})\s*\)")),
)


class ScanError(Exception):
    """A source could not be read or lexed.

    Raised, never swallowed: a file the scanner cannot process is not a file with
    no violations. Callers collect these and FAIL the run before comparing
    anything to a baseline, because a partial scan cannot tell "no new site" from
    "did not look". Three separate guards in this repo have shipped that same
    fail-open hole; this one does not.
    """


# A C++ raw string literal, including its optional encoding prefix. The delimiter
# is bounded by the standard's 16 characters and excludes the characters the
# standard already forbids in it.
_RAW_STRING_OPEN_RE = re.compile(r"(?<![A-Za-z0-9_])(?:u8|u|U|L)?R\"([^ ()\\\t\v\f\n]{0,16})\(")


def _blank_raw_strings(name: str, text: str) -> str:
    """Replace every raw string literal's BODY with nothing, preserving line count.

    `_strip_comments` is deliberately line-oriented, so a MULTI-LINE raw string
    (the PLY fixtures in `test_ply_importer.h` and friends are written that way)
    would otherwise be handed to the scanners as if it were code. Blanking it here
    - before comments are stripped, which is the order the C++ lexer uses - means
    nothing downstream ever reads fixture text as source.

    The literal is replaced by `""` followed by exactly as many newlines as it
    spanned, so every later line keeps its number. An UNTERMINATED raw string is a
    ScanError: it means the rest of the file cannot be lexed, and guessing is how
    a guard starts reporting on text it does not understand.
    """
    out: list[str] = []
    position = 0
    while True:
        match = _RAW_STRING_OPEN_RE.search(text, position)
        if match is None:
            out.append(text[position:])
            return "".join(out)
        terminator = f"){match.group(1)}\""
        end = text.find(terminator, match.end())
        if end == -1:
            line_no = text.count("\n", 0, match.start()) + 1
            raise ScanError(
                f"{name}:{line_no}: unterminated raw string literal "
                f"(no closing `{terminator}`). Refusing to scan a file this cannot lex."
            )
        end += len(terminator)
        out.append(text[position : match.start()])
        out.append('""' + "\n" * text.count("\n", match.start(), end))
        position = end


def _read_source(path: Path) -> str:
    """Read one test source, failing closed on anything unreadable or unlexable."""
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise ScanError(f"{path.name}: cannot be read ({exc}).") from exc
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        # Deliberately NOT errors="replace": a replacement character silently
        # rewrites the source the scanner then reasons about.
        raise ScanError(f"{path.name}: is not valid UTF-8 ({exc}).") from exc
    return _blank_raw_strings(path.name, text)


def _strip_comments(text: str) -> str:
    """Remove comments and blank out string/char literals, LINE BY LINE.

    Deliberately line-oriented: every input line maps to exactly one output line,
    so a reported line number cannot drift no matter what the file contains. (An
    earlier character-stream version of this function silently lost a newline on
    an unterminated char literal and shifted every subsequent report by one -
    precisely the kind of quiet miscount this guard exists to prevent.)

    Literals are replaced by empty ones rather than deleted so a `->` inside a
    message string cannot read as a dereference.
    """
    lines_out: list[str] = []
    in_block = False
    for line in text.split("\n"):
        out: list[str] = []
        i = 0
        n = len(line)
        while i < n:
            if in_block:
                end = line.find("*/", i)
                if end == -1:
                    i = n
                else:
                    in_block = False
                    i = end + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            ch = line[i]
            if ch in ('"', "'"):
                # Find the closing quote on THIS line, honouring backslash escapes.
                j = i + 1
                closed = False
                while j < n:
                    if line[j] == "\\":
                        j += 2
                        continue
                    if line[j] == ch:
                        closed = True
                        break
                    j += 1
                if closed:
                    out.append(ch * 2)
                    i = j + 1
                else:
                    # Not a literal (digit separator like 1'000, stray apostrophe,
                    # or a raw/multi-line string). Keep the character verbatim
                    # rather than swallowing the rest of the line.
                    out.append(ch)
                    i += 1
                continue
            out.append(ch)
            i += 1
        lines_out.append("".join(out))
    return "\n".join(lines_out)


def _symbol_regex(symbol: str) -> str:
    """Build a regex matching `symbol`, tolerating whitespace and `.`/`->` in a chain.

    The two accessors are treated as interchangeable: a chain asserted as `a.b`
    and dereferenced as `a->b` is the same object either way, and refusing to
    match across them would be an under-report.
    """
    parts = [part for part in re.split(r"\s*(?:\.|->)\s*", symbol) if part]
    rendered = []
    for part in parts:
        if part.endswith(")"):
            name = part.split("(", 1)[0].strip()
            rendered.append(re.escape(name) + r"\s*\(\s*\)")
        else:
            rendered.append(re.escape(part))
    return r"\s*(?:\.|->)\s*".join(rendered)


def _deref_positions(symbol: str, text: str) -> list[int]:
    """Offsets in `text` where `symbol` is dereferenced."""
    sym = _symbol_regex(symbol)
    positions: list[int] = []
    for pattern in (
        rf"(?<![\w.>]){sym}\s*->",
        rf"(?<![\w)\]]){sym}\s*\[",
        rf"(?<![\w)\]])\*\s*{sym}\b",
    ):
        positions.extend(match.start() for match in re.finditer(pattern, text))
    return sorted(positions)


def _positive_test(symbol: str, expr: str) -> bool:
    """`expr` being TRUE implies `symbol` is non-null (`ptr`, `ptr != nullptr`, ...)."""
    sym = _symbol_regex(symbol)
    body = _strip_outer_parens(expr.strip())[0].strip()
    return any(
        re.fullmatch(pattern, body)
        for pattern in (
            sym,
            rf"{sym}\s*!=\s*(?:nullptr|NULL)",
            rf"(?:nullptr|NULL)\s*!=\s*{sym}",
            rf"{sym}\s*(?:\.|->)\s*is_valid\s*\(\s*\)",
        )
    )


def _negative_test(symbol: str, expr: str) -> bool:
    """`expr` being FALSE implies `symbol` is non-null (`!ptr`, `ptr == nullptr`, ...)."""
    sym = _symbol_regex(symbol)
    body = _strip_outer_parens(expr.strip())[0].strip()
    return any(
        re.fullmatch(pattern, body)
        for pattern in (
            rf"!\s*{sym}",
            rf"{sym}\s*==\s*(?:nullptr|NULL)",
            rf"(?:nullptr|NULL)\s*==\s*{sym}",
            rf"{sym}\s*(?:\.|->)\s*is_null\s*\(\s*\)",
        )
    )


def _strip_outer_parens(text: str) -> tuple[str, int]:
    """Remove one wrapping paren pair if it encloses the WHOLE text.

    Returns (inner_text, offset_of_inner_within_text).
    """
    stripped = text.strip()
    offset = len(text) - len(text.lstrip())
    if not stripped.startswith("(") or not stripped.endswith(")"):
        return text, 0
    depth = 0
    for i, ch in enumerate(stripped):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0 and i != len(stripped) - 1:
                return text, 0  # the pair closes early; not a full wrap
    return stripped[1:-1], offset + 1


def _split_top_level(text: str, op: str) -> list[tuple[int, int]]:
    """Spans of `text` separated by `op` at parenthesis depth 0."""
    spans: list[tuple[int, int]] = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and text.startswith(op, i):
            spans.append((start, i))
            i += len(op)
            start = i
            continue
        i += 1
    spans.append((start, len(text)))
    return spans


def _ternary_spans(text: str) -> tuple[tuple[int, int], tuple[int, int], tuple[int, int]] | None:
    """Split `cond ? a : b` at depth 0, or None when it is not an unambiguous ternary.

    `::` is skipped so scope resolution is never mistaken for the ternary colon.
    Anything ambiguous returns None, which makes the caller treat the dereference
    as UNGUARDED - failing toward reporting, since a guard that under-reports is
    worse than one that over-reports.
    """
    depth = 0
    q = -1
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and ch == "?":
            q = i
            break
    if q == -1:
        return None
    depth = 0
    i = q + 1
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and ch == ":":
            if text.startswith("::", i) or (i > 0 and text[i - 1] == ":"):
                i += 2
                continue
            return (0, q), (q + 1, i), (i + 1, len(text))
        i += 1
    return None


def _enclosing_group(text: str, at: int) -> tuple[int, int] | None:
    """Span inside the OUTERMOST parenthesis pair containing `at`, or None.

    Outermost, not innermost: descending must peel ONE layer at a time so the
    operators at each level are examined on the way down. Jumping straight to the
    innermost group skips them - `CHECK(ptr && (a || ptr->f()))` would land on
    `a || ptr->f()` and never see the `ptr &&` that guards it.
    """
    depth = 0
    start: int | None = None
    for i, ch in enumerate(text):
        if ch == "(":
            if depth == 0:
                start = i
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0 and start is not None:
                if start < at < i:
                    return (start + 1, i)
                start = None
    return None


def _condition_tail(expr: str) -> str:
    """Drop a leading assignment so only the condition remains.

    `int v = ptr ? ptr->f() : 0` hands us `int v = ptr` as the ternary condition;
    without this the positive test would fail and the safe branch would be
    reported.
    """
    depth = 0
    last = -1
    for i, ch in enumerate(expr):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and ch == "=":
            if i > 0 and expr[i - 1] in "=!<>":
                continue
            if i + 1 < len(expr) and expr[i + 1] == "=":
                continue
            last = i
    return expr[last + 1 :] if last >= 0 else expr


def _short_circuit_guarded(
    symbol: str,
    text: str,
    deref_at: int,
    positive: Callable[[str, str], bool] | None = None,
    negative: Callable[[str, str], bool] | None = None,
) -> bool:
    """True when C++ short-circuiting prevents reaching the dereference.

    A guard must DOMINATE the dereference, not merely precede it textually. An
    earlier prefix-search version accepted `(ptr && ptr->f()) || ptr->g()` because
    `ptr &&` appeared somewhere earlier - but `ptr->g()` runs precisely when the
    left disjunct is false, i.e. when ptr is null. That was a false NEGATIVE
    introduced while fixing a false positive (Codex, PR #659).

    So the expression is decomposed by precedence instead:

    * `cond ? a : b` - `a` is guarded by a positive test, `b` by a negative one;
    * `A || B`       - `B` is guarded only if an EARLIER disjunct is a NEGATIVE test
                       (`!ptr || ptr->f()`), since `B` runs when they are false;
    * `A && B`       - `B` is guarded only if an EARLIER conjunct is a POSITIVE test
                       (`ptr && ptr->f()`), since `B` runs when they are true.

    Recursion descends into whichever part contains the dereference, so an outer
    guard still counts (`ptr && (a || ptr->f())`). Anything it cannot parse
    unambiguously is reported as unguarded.

    `positive` / `negative` are the two predicates that decide what "guarded"
    MEANS, and default to the null-ish pair. Detector 2 (#844) passes the
    size-aware pair instead, so `chunks.size() >= 2 && f(chunks[0])` is not
    reported. The dominance logic itself is the same either way, which is the
    point of injecting them rather than writing a second copy of it: the
    `(a && b) || c` false-negative that took a review round to find (PR #659) is
    fixed once, for both detectors.
    """
    positive = _positive_test if positive is None else positive
    negative = _negative_test if negative is None else negative
    if deref_at < 0 or deref_at > len(text):
        return False

    inner, offset = _strip_outer_parens(text)
    if offset:
        return _short_circuit_guarded(symbol, inner, deref_at - offset, positive, negative)

    def contains(span: tuple[int, int]) -> bool:
        return span[0] <= deref_at < span[1]

    ternary = _ternary_spans(text)
    if ternary:
        cond, when_true, when_false = ternary
        condition = _condition_tail(text[cond[0] : cond[1]])
        if contains(when_true) and positive(symbol, condition):
            return True
        if contains(when_false) and negative(symbol, condition):
            return True
        for span in (cond, when_true, when_false):
            if contains(span):
                return _short_circuit_guarded(
                    symbol, text[span[0] : span[1]], deref_at - span[0], positive, negative
                )
        return False

    for op, test in (("||", negative), ("&&", positive)):
        spans = _split_top_level(text, op)
        if len(spans) == 1:
            continue
        for position, span in enumerate(spans):
            if not contains(span):
                continue
            if any(test(symbol, text[s[0] : s[1]]) for s in spans[:position]):
                return True
            return _short_circuit_guarded(
                symbol, text[span[0] : span[1]], deref_at - span[0], positive, negative
            )
        return False

    # No top-level operator applies, so the dereference sits inside a call's
    # argument list (`CHECK(ptr && ptr->f())`). Peel ONE parenthesis layer and
    # re-examine. This runs AFTER the operator splits, so an outer guard still
    # wins: `ptr && (a || ptr->f())` is decided by the outer `&&`.
    group = _enclosing_group(text, deref_at)
    if group:
        return _short_circuit_guarded(
            symbol, text[group[0] : group[1]], deref_at - group[0], positive, negative
        )
    return False


def _derefs(symbol: str, statement: str) -> bool:
    """True when `statement` dereferences `symbol`.

    `symbol->`, `*symbol` and `symbol[` count. A trailing `symbol.` does NOT: on a
    Ref<T> that is a call on the handle itself, which is exactly what is safe.
    A dereference that C++ short-circuiting cannot reach does not count either -
    see _short_circuit_guarded().
    """
    return any(
        not _short_circuit_guarded(symbol, statement, at)
        for at in _deref_positions(symbol, statement)
    )


def _reassigns(symbol: str, statement: str) -> bool:
    """True when the statement looks like it rebinds the symbol."""
    sym = _symbol_regex(symbol)
    return re.search(rf"(?<![\w.>]){sym}\s*=(?!=)", statement) is not None


def _line_fragments(line: str) -> list[str]:
    """Split one logical line into its statements at depth-0 ';'.

    A doctest assertion macro cannot contain a bare ';' outside parentheses, so
    depth-0 splitting reliably separates compacted statements. Once a
    control-flow statement starts, the remainder is kept WHOLE rather than split,
    since `for (a; b; c)` would otherwise be shredded.

    Returning every fragment (not just the tail after the first ';') is what lets
    EACH `REQUIRE` on a compacted line act as a guard. Matching only from the
    start of the line meant `REQUIRE(a != nullptr); REQUIRE(b != nullptr); b->f();`
    established a guard for `a` alone and never reported `b` (Codex, PR #659).
    """
    fragments: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(line):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == ";" and depth == 0:
            piece = line[start : i + 1].strip()
            if piece:
                if _CONTROL_FLOW_RE.match(piece):
                    fragments.append(line[start:].strip())
                    return fragments
                fragments.append(piece)
            start = i + 1
    tail = line[start:].strip()
    if tail:
        fragments.append(tail)
    return fragments


def _statements(
    lines: list[str], start_index: int, limit: int = _SCAN_STATEMENTS
) -> list[tuple[int, str]]:
    """Yield (line_number, statement_text) for statements after start_index.

    A statement is accumulated until a ';' at depth 0, or until a line that opens
    or closes a block, which is emitted on its own so the caller can stop there.

    `limit` is the caller's scan window. It is a PARAMETER because the two
    detectors own their windows independently: slicing the result afterwards
    cannot widen it, and a caller that assumed it could would silently get six
    statements while believing it had asked for thirty.
    """
    statements: list[tuple[int, str]] = []
    buffer = ""
    buffer_line = 0
    depth = 0
    for offset in range(start_index, min(start_index + 60, len(lines))):
        raw = lines[offset]
        stripped = raw.strip()
        if not stripped:
            continue
        if not buffer:
            buffer_line = offset + 1
        buffer = f"{buffer} {stripped}".strip()
        # Depth tracking so the two ';' inside a MULTI-LINE `for (a; b; c)` header
        # do not each look like a statement terminator. Without it only the
        # initializer was emitted, the `for` matched as control flow, and the scan
        # broke before ever reading the condition - missing a dereference that is
        # evaluated before the loop body can guard anything (Codex, PR #659).
        # Literals are already blanked by _strip_comments, so no parenthesis here
        # can come from inside a string.
        depth = max(0, depth + stripped.count("(") - stripped.count(")"))
        if depth == 0 and (
            ";" in stripped or stripped.endswith("{") or stripped.endswith("}")
        ):
            statements.append((buffer_line, buffer))
            buffer = ""
            if len(statements) >= limit:
                break
    return statements


def _logical_line(lines: list[str], index: int) -> tuple[str, int]:
    """Join continuation lines from `index` until parentheses balance.

    Returns (joined_text, index_of_last_line_consumed). Bounded so a stray
    unbalanced '(' cannot swallow the rest of the file.
    """
    text = lines[index]
    depth = text.count("(") - text.count(")")
    last = index
    while depth > 0 and last + 1 < len(lines) and last - index < 12:
        last += 1
        text = f"{text.rstrip()} {lines[last].strip()}"
        depth += lines[last].count("(") - lines[last].count(")")
    return text, last


def _scan_file(path: Path) -> list[tuple[int, str, str, str]]:
    """Return (line, symbol, form, statement) for each violation in the file."""
    text = _strip_comments(_read_source(path))
    lines = text.splitlines()
    violations: list[tuple[int, str, str, str]] = []

    for index, line in enumerate(lines):
        # A REQUIRE may be split across physical lines:
        #     REQUIRE(
        #             ptr != nullptr);
        # Matching only the current line made those invisible (Codex, PR #659), so
        # continuation lines are joined until the parentheses balance. Predicates
        # are anchored at ^\s*REQUIRE, so a continuation line can never itself
        # start a second, duplicate match.
        line, last_index = _logical_line(lines, index)
        fragments = _line_fragments(line)
        # EVERY null-ish REQUIRE on this logical line becomes a guard, not just
        # the first: statements are routinely compacted onto one line, and
        # matching once from the start left later REQUIREs unguarded.
        for position, fragment in enumerate(fragments):
            symbol = None
            form = ""
            for form_name, pattern in _NULLISH_PATTERNS:
                match = pattern.match(fragment)
                if match:
                    symbol = match.group(1)
                    form = form_name
                    break
            if symbol is None:
                continue
            # What follows may still be on the SAME line
            # (`REQUIRE(ptr != nullptr); ptr->method();`) - that one-liner is
            # exactly the shape tests/AGENTS.md uses to describe the bug.
            following = [(index + 1, f) for f in fragments[position + 1 :]]
            _scan_forward(symbol, form, index, following + _statements(lines, last_index + 1), violations)
    return violations


def _scan_forward(
    symbol: str,
    form: str,
    index: int,
    following: list[tuple[int, str]],
    violations: list[tuple[int, str, str, str]],
) -> None:
    """Walk the statements after a null-ish REQUIRE looking for a dereference.

    Appends at most one violation: the first unguarded dereference of `symbol`.
    `index` is the zero-based line of the REQUIRE, reported as `index + 1`.
    """
    for _stmt_line, statement in following[:_SCAN_STATEMENTS]:
        if _CONTROL_FLOW_RE.match(statement):
            # A control-flow statement guards its BODY, never its own header.
            # `if (ptr) { ptr->f(); }` is safe, but `if (ptr->is_ready())`
            # evaluates the dereference before any guarding can happen - and a
            # non-aborting REQUIRE did not stop us getting here. So test the
            # header, then stop either way (the body is out of scope: we cannot
            # tell what guards it).
            header = statement.split("{", 1)[0]
            if _derefs(symbol, header):
                violations.append((index + 1, symbol, form, header.strip()))
            return
        if _derefs(symbol, statement):
            violations.append((index + 1, symbol, form, statement.strip()))
            return
        if _ASSERT_MACRO_RE.match(statement):
            continue
        if _reassigns(symbol, statement):
            return


# ---------------------------------------------------------------------------
# Detector 2: a cardinality assertion followed by an index of the same container
# (#844). See the module docstring for the mechanism, the shape and the count.
# ---------------------------------------------------------------------------

# How many statements to look ahead after the size assertion. Same window as the
# null-deref detector. Raising it finds more REAL sites (see docstring blind spot
# 1) and changes the baseline, so it is a separate, reviewable change.
_SIZE_SCAN_STATEMENTS = 6

# Detector 2's symbol grammar extends _SYMBOL with ONE subscript per segment, so
# `chunks[0].indices` is a single symbol. Without it the scanner would extract the
# bare tail `indices` from `chunks[0].indices.size()` and then match `.indices[`
# on any other object in the window - comparing names instead of tracking one
# container.
_SIZE_SEGMENT = r"[A-Za-z_]\w*(?:\s*\(\s*\))?(?:\s*\[[^\[\]]*\])?"
_SIZE_SYMBOL = rf"{_SIZE_SEGMENT}(?:\s*(?:\.|->)\s*{_SIZE_SEGMENT})*"

# REQUIRE* and CHECK* both. CHECK is not the weaker case here: it never aborts
# under ANY doctest configuration, so it is strictly worse than a REQUIRE that
# merely does not abort in THIS build. One of the four sites #843 fixed was a
# CHECK.
_SIZE_ASSERT_HEAD_RE = re.compile(r"^\s*((?:REQUIRE|CHECK)\w*)\s*\(")
_CARDINALITY_RE = re.compile(
    rf"({_SIZE_SYMBOL})\s*(?:\.|->)\s*(size|is_empty|empty)\s*\(\s*\)"
)
_ANY_CARDINALITY_RE = re.compile(
    rf"{_SIZE_SYMBOL}\s*(?:\.|->)\s*(?:size|is_empty|empty)\s*\(\s*\)"
)
# A relational comparison macro carries the operator in its NAME.
_COMPARISON_MACRO_SUFFIXES: tuple[tuple[str, str], ...] = (
    ("_EQ", "=="), ("_NE", "!="), ("_GE", ">="), ("_LE", "<="), ("_GT", ">"), ("_LT", "<"),
)
_LITERAL_ZERO_RE = re.compile(r"^\(*\s*0[uUlL]*\s*\)*$")
# A trailing C-style cast, e.g. the `(uint32_t)` in `idx < (uint32_t)v.size()`.
_CAST_SUFFIX_RE = re.compile(r"\(\s*(?:const\s+)?[A-Za-z_][\w:]*(?:\s*[*&]+)?\s*\)\s*$")

# Control flow whose HEADER may bound the loop/branch. `case`/`default` are not
# here: they do not carry a condition that could bound anything.
_SIZE_CONTROL_FLOW_RE = re.compile(r"^\s*(?:\}\s*)?(?:if\b|else\b|for\b|while\b|do\b|switch\b)")
# Leaving the enclosing test case entirely: nothing after it is the same scope.
_SIZE_SCAN_STOP_RE = re.compile(r"^\s*(?:TEST_CASE\b|TEST_SUITE\b)")
_RETURN_RE = re.compile(r"^\s*return\b")
# Calls that can change a container's length, invalidating the asserted bound.
_LENGTH_MUTATORS = (
    "resize", "clear", "push_back", "append", "append_array", "insert", "remove_at",
    "remove", "erase", "pop_back", "ordered_insert", "reserve", "set_size", "fill_with",
)

# Reported classes. Both are baselined; they are distinguished only so the count
# stays reconcilable against #844's sweep (42 + 7 = 49).
_CLASS_STRAIGHT_LINE = "straight-line"
_CLASS_OTHER_BOUND = "loop-bounded-by-another-container"


def _size_symbol_regex(symbol: str) -> str:
    """Like _symbol_regex, but each segment may carry one `[...]` subscript."""
    parts = [part for part in re.split(r"\s*(?:\.|->)\s*", symbol) if part]
    rendered = []
    for part in parts:
        subscript = ""
        match = re.search(r"\[([^\[\]]*)\]\s*$", part)
        if match:
            subscript = r"\s*\[\s*" + re.escape(match.group(1).strip()) + r"\s*\]"
            part = part[: match.start()].strip()
        if part.endswith(")"):
            rendered.append(re.escape(part.split("(", 1)[0].strip()) + r"\s*\(\s*\)" + subscript)
        else:
            rendered.append(re.escape(part) + subscript)
    return r"\s*(?:\.|->)\s*".join(rendered)


def _macro_argument_span(fragment: str, name: str) -> tuple[int, int]:
    """(start, end) of the assertion macro's argument list, exclusive of its parens.

    Raises ScanError when the parentheses never balance. That is NOT "an assertion
    with no size predicate": it means the scanner does not know where the
    assertion ends, and reporting it clean would be a guess.
    """
    open_at = fragment.find("(")
    if open_at < 0:
        raise ScanError(
            f"{name}: assertion `{_elide(fragment.strip(), 90)}` has no argument list."
        )
    depth = 0
    for i in range(open_at, len(fragment)):
        if fragment[i] == "(":
            depth += 1
        elif fragment[i] == ")":
            depth -= 1
            if depth == 0:
                return open_at + 1, i
    raise ScanError(
        f"{name}: unbalanced parentheses in assertion `{_elide(fragment.strip(), 90)}` - "
        f"cannot tell where the assertion ends, refusing to call it clean."
    )


def _paren_depth(text: str, start: int, at: int) -> int:
    depth = 0
    for i in range(start, at):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
    return depth


def _split_macro_arguments(text: str) -> list[str]:
    """Split a macro argument list at depth-0 ',' (parens AND brackets count)."""
    parts: list[str] = []
    depth = 0
    start = 0
    for i, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return parts


def _establishes_lower_bound(
    macro: str, fragment: str, span: tuple[int, int], match: re.Match[str]
) -> bool:
    """True when the assertion HOLDING implies the container is long enough to index.

    The point is the contrapositive: only then does the assertion FAILING make a
    following index unsafe. `CHECK(v.size() == 0)` and `CHECK(v.is_empty())` fail
    by the container being LONGER than claimed, so an index after them is not made
    unsafe by the failure and must not be reported.
    """
    lo, hi = span
    kind = match.group(2)
    before = fragment[lo : match.start()]
    after = fragment[match.end() : hi]
    if kind in ("is_empty", "empty"):
        # `!v.is_empty()`, `REQUIRE_FALSE(v.is_empty())`, `REQUIRE_UNARY_FALSE(...)`.
        return bool(re.search(r"!\s*$", before)) or macro.endswith("_FALSE")

    relation = re.match(r"\s*(==|!=|>=|<=|>|<)\s*(.*)$", after, re.S)
    if relation:
        operator, other, flipped = relation.group(1), relation.group(2), False
    else:
        # A C-style cast sits between the operator and the `size()` call in
        # `CHECK(idx < (uint32_t)splats.size())` (test_lod_system.cpp:933), which
        # is a real site. Peel casts off the tail before looking for the operator.
        left = _CAST_SUFFIX_RE.sub("", before)
        while left != before:
            before, left = left, _CAST_SUFFIX_RE.sub("", left)
        reversed_relation = re.search(r"(==|!=|>=|<=|>|<)\s*$", left)
        if reversed_relation:
            operator = reversed_relation.group(1)
            other = left[: reversed_relation.start()]
            flipped = True
        else:
            # No adjacent operator: the relation may be carried by the macro NAME
            # (`REQUIRE_EQ(v.size(), 4)`).
            operator = ""
            for suffix, symbol in _COMPARISON_MACRO_SUFFIXES:
                if macro.endswith(suffix) or macro.endswith(f"{suffix}_MESSAGE"):
                    operator = symbol
                    break
            if not operator:
                return False
            arguments = _split_macro_arguments(fragment[lo:hi])
            if len(arguments) < 2:
                return False
            first_argument_end = lo + len(arguments[0])
            flipped = match.start() >= first_argument_end
            other = arguments[0] if flipped else arguments[1]
    other = other.split(",")[0].strip()
    if flipped:
        operator = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(operator, operator)
    against_zero = bool(_LITERAL_ZERO_RE.match(other))
    if operator == "==":
        return not against_zero          # `size() == 0` asserts EMPTY
    if operator == "!=":
        return against_zero              # `size() != 0` asserts NON-EMPTY
    if operator == ">":
        return True                      # `size() > n` and `idx < size()`
    if operator == ">=":
        return not against_zero          # `size() >= 0` is vacuous
    return False                         # `<` / `<=`: an UPPER bound only


def _size_assertions(fragment: str, name: str) -> list[tuple[str, str]]:
    """(container symbol, macro name) for each lower bound this assertion asserts."""
    head = _SIZE_ASSERT_HEAD_RE.match(fragment)
    if head is None:
        return []
    macro = head.group(1)
    span = _macro_argument_span(fragment, name)
    lo, hi = span
    found: list[tuple[str, str]] = []
    seen: set[str] = set()
    for match in _CARDINALITY_RE.finditer(fragment, lo, hi):
        # Depth 1 relative to the macro: a DIRECT argument. Nested deeper it is an
        # argument to some other call, e.g.
        # `REQUIRE(cpu_results.resize(ground_truth.size()) == OK)`, which
        # constrains the resize result and says nothing about `ground_truth`.
        if _paren_depth(fragment, lo, match.start()) != 0:
            continue
        symbol = match.group(1).strip()
        if symbol in seen:
            continue
        if not _establishes_lower_bound(macro, fragment, span, match):
            continue
        seen.add(symbol)
        found.append((symbol, macro))
    return found


def _index_positions(symbol: str, text: str) -> list[int]:
    """Offsets in `text` where `symbol` is subscripted."""
    pattern = rf"(?<![\w)\]]){_size_symbol_regex(symbol)}\s*\["
    return [match.start() for match in re.finditer(pattern, text)]


def _size_positive_test(symbol: str, expr: str) -> bool:
    """`expr` being TRUE constrains `symbol`'s length from below."""
    sym = _size_symbol_regex(symbol)
    body = _strip_outer_parens(expr.strip())[0].strip()
    return any(
        re.search(pattern, body)
        for pattern in (
            rf"(?<![\w)\]]){sym}\s*(?:\.|->)\s*size\s*\(\s*\)\s*(?:==|!=|>=|>)",
            rf"(?:<|<=|==|!=)\s*{sym}\s*(?:\.|->)\s*size\s*\(\s*\)",
            rf"!\s*{sym}\s*(?:\.|->)\s*(?:is_empty|empty)\s*\(\s*\)",
        )
    )


def _size_negative_test(symbol: str, expr: str) -> bool:
    """`expr` being FALSE implies `symbol` is non-empty."""
    sym = _size_symbol_regex(symbol)
    body = _strip_outer_parens(expr.strip())[0].strip()
    return any(
        re.fullmatch(pattern, body)
        for pattern in (
            rf"{sym}\s*(?:\.|->)\s*(?:is_empty|empty)\s*\(\s*\)",
            rf"{sym}\s*(?:\.|->)\s*size\s*\(\s*\)\s*==\s*0[uUlL]*",
        )
    )


def _unguarded_index(symbol: str, text: str) -> bool:
    """True when `text` subscripts `symbol` somewhere short-circuiting can reach."""
    return any(
        not _short_circuit_guarded(
            symbol, text, at, positive=_size_positive_test, negative=_size_negative_test
        )
        for at in _index_positions(symbol, text)
    )


def _bounds_iteration(symbol: str, header: str) -> bool:
    """True when a control-flow header bounds by the indexed container's OWN length."""
    sym = _size_symbol_regex(symbol)
    pattern = rf"(?<![\w)\]]){sym}\s*(?:\.|->)\s*(?:size|is_empty|empty)\s*\(\s*\)"
    return re.search(pattern, header) is not None


def _changes_length(symbol: str, statement: str) -> bool:
    """True when the statement can rebind `symbol` or change its length."""
    sym = _size_symbol_regex(symbol)
    if re.search(rf"(?<![\w.>)\]]){sym}\s*=(?!=)", statement):
        return True
    mutators = "|".join(_LENGTH_MUTATORS)
    return re.search(rf"(?<![\w)\]]){sym}\s*(?:\.|->)\s*(?:{mutators})\s*\(", statement) is not None


def _first_unbounded_index(
    symbol: str, following: list[tuple[int, str]]
) -> tuple[int, str, str] | None:
    """(line, statement, class) of the first index of `symbol` nothing bounds.

    A block STACK, not a stop-at-the-first-control-flow rule: a loop bounded by
    the container's own `size()` makes its BODY safe and nothing after it, so
    `for (i < a.size()) { a[i]; } CHECK(a[0]);` is still reported on `a[0]`.
    Each frame records two facts - bounded by THIS container's length, and bounded
    by ANY container's length - because only the first makes the index safe, while
    the second is what #844's sweep counted as loop-bounded and is reported
    separately so the two counts stay reconcilable.
    """
    stack: list[tuple[bool, bool]] = []
    pending: tuple[bool, bool] | None = None
    for line_no, statement in following:
        if _SIZE_SCAN_STOP_RE.match(statement):
            return None
        if _RETURN_RE.match(statement) and not stack:
            return None
        if statement.lstrip().startswith("}") and stack:
            stack.pop()
        own_bound = any(frame[0] for frame in stack) or bool(pending and pending[0])
        other_bound = any(frame[1] for frame in stack) or bool(pending and pending[1])
        header = statement.split("{", 1)[0]
        opens_block = statement.rstrip().endswith("{")
        if _SIZE_CONTROL_FLOW_RE.match(statement):
            # A header's own bound guards its BODY, never itself: in
            # `while (v[i] && i < v.size())` the subscript is evaluated first. So
            # the header is judged against the ENCLOSING bound only.
            if not own_bound and _unguarded_index(symbol, header):
                return line_no, header.strip(), (
                    _CLASS_OTHER_BOUND if other_bound else _CLASS_STRAIGHT_LINE
                )
            frame = (
                own_bound or _bounds_iteration(symbol, header),
                other_bound or bool(_ANY_CARDINALITY_RE.search(header)),
            )
            if opens_block:
                stack.append(frame)
                pending = None
            else:
                pending = frame  # brace-less body: applies to the next statement
            continue
        if not own_bound and _unguarded_index(symbol, statement):
            return line_no, statement.strip(), (
                _CLASS_OTHER_BOUND if other_bound else _CLASS_STRAIGHT_LINE
            )
        if _changes_length(symbol, statement):
            return None
        if opens_block:
            stack.append((own_bound, other_bound))
        pending = None
    return None


def _scan_file_size_index(path: Path) -> list[tuple[int, str, str, str, int, str, str]]:
    """(line, symbol, macro, assertion, index_line, index_statement, class) per site.

    At most one entry per (container, index site): when several assertions
    constrain the same container above the same index, the NEAREST one is
    reported, because that is the assertion whose failure reaches the index and
    the one a conversion has to rewrite.
    """
    text = _strip_comments(_read_source(path))
    lines = text.splitlines()
    nearest: dict[tuple[str, int, str], tuple[int, str, str, str, int, str, str]] = {}

    for index, _ in enumerate(lines):
        line, last_index = _logical_line(lines, index)
        fragments = _line_fragments(line)
        for position, fragment in enumerate(fragments):
            for symbol, macro in _size_assertions(fragment, path.name):
                following = [(index + 1, f) for f in fragments[position + 1 :]]
                following += _statements(lines, last_index + 1, _SIZE_SCAN_STATEMENTS)
                hit = _first_unbounded_index(symbol, following[:_SIZE_SCAN_STATEMENTS])
                if hit is None:
                    continue
                index_line, index_statement, klass = hit
                nearest[(symbol, index_line, index_statement)] = (
                    index + 1, symbol, macro, fragment.strip(),
                    index_line, index_statement, klass,
                )
    return sorted(nearest.values())


def _test_sources() -> list[Path]:
    return sorted(
        list(MODULE_TESTS_DIR.glob("*.h"))
        + list(MODULE_TESTS_DIR.glob("*.cpp"))
        + list(ENGINE_TESTS_DIR.glob("test_*.cpp"))
    )


def scan_all_size_index() -> tuple[dict[str, list[tuple[int, str, str, str, int, str, str]]], list[str]]:
    """(basename -> size-then-index sites, scan errors). Errors are never violations."""
    results: dict[str, list[tuple[int, str, str, str, int, str, str]]] = {}
    errors: list[str] = []
    for path in _test_sources():
        try:
            sites = _scan_file_size_index(path)
        except ScanError as exc:
            errors.append(str(exc))
            continue
        if sites:
            results[path.name] = sites
    return results, errors


def size_index_fingerprint(
    symbol: str, macro: str, assertion: str, index_statement: str
) -> str:
    """Stable identity for one size-then-index site, independent of line numbers.

    BOTH statements are hashed. Hashing only the index would collapse two sites
    that index the same container from different assertions onto one identity, and
    hashing only the assertion would miss a second index added under an existing
    assertion - either way the ratchet would stop distinguishing sites it must.
    """
    return fingerprint(symbol, macro, f"{assertion} >>> {index_statement}")


def scan_size_index_fingerprints() -> tuple[dict[str, list[str]], list[str]]:
    found, errors = scan_all_size_index()
    return (
        {
            name: sorted(
                size_index_fingerprint(symbol, macro, assertion, statement)
                for _, symbol, macro, assertion, _, statement, _ in sites
            )
            for name, sites in found.items()
        },
        errors,
    )


def scan_all() -> dict[str, list[tuple[int, str, str, str]]]:
    """Basename -> violations, for every test source that has any."""
    results: dict[str, list[tuple[int, str, str, str]]] = {}
    for path in _test_sources():
        violations = _scan_file(path)
        if violations:
            results[path.name] = violations
    return results


def _multiset_difference(left: list[str], right: list[str]) -> list[str]:
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
    return text if len(text) <= limit else text[: limit - 1] + "…"


def fingerprint(symbol: str, form: str, statement: str) -> str:
    """Stable identity for one violation site, independent of its line number.

    Hashes the FULL statement. An earlier version hashed a 120-character
    truncation, so two sites differing only past column 120 collapsed to one
    fingerprint and the ratchet silently stopped distinguishing them (Codex,
    PR #659) - two test_lod_system.cpp query sites did exactly that. Truncation
    is a display concern; see _elide().
    """
    normalized = re.sub(r"\s+", " ", statement).strip()
    digest = hashlib.sha1(normalized.encode("utf-8")).hexdigest()[:10]
    return f"{symbol}|{form}|{digest}"


def scan_fingerprints() -> dict[str, list[str]]:
    """Basename -> sorted fingerprints of every violation in that file."""
    return {
        name: sorted(fingerprint(sym, form, stmt) for _, sym, form, stmt in violations)
        for name, violations in scan_all().items()
    }


def _load_fingerprint_baseline(path: Path) -> tuple[dict[str, list[str]], list[str]]:
    """Read a per-site fingerprint baseline. Missing or malformed is a FAILURE, never a pass."""
    if not path.is_file():
        return {}, [
            f"Baseline file missing: {path.name}. Refusing to treat an absent "
            f"baseline as 'nothing to report'."
        ]
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return {}, [f"Baseline file {path.name} is not valid JSON: {exc}"]
    if not isinstance(data, dict) or not isinstance(data.get("files"), dict):
        return {}, [f"Baseline file {path.name} must be an object with a 'files' object."]
    out: dict[str, list[str]] = {}
    for name, prints in data["files"].items():
        if not isinstance(prints, list) or not all(isinstance(p, str) for p in prints):
            return {}, [
                f"Baseline entry '{name}' in {path.name} must be a list of fingerprint strings."
            ]
        out[name] = sorted(prints)
    return out, []


def load_baseline() -> tuple[dict[str, list[str]], list[str]]:
    """Read the null-deref fingerprint baseline."""
    return _load_fingerprint_baseline(BASELINE_PATH)


def load_size_index_baseline() -> tuple[dict[str, list[str]], list[str]]:
    """Read the size-then-index fingerprint baseline (#844)."""
    return _load_fingerprint_baseline(SIZE_INDEX_BASELINE_PATH)


def _preflight_sources(files: list[Path]) -> list[str]:
    """Read every source once, so an unreadable or unlexable file fails the RUN.

    Deliberately before any scanning: a scan that silently skipped a file would
    report "0 new" for it, which is the fail-open hole this repo has now found in
    three separate guards.
    """
    errors: list[str] = []
    for path in files:
        try:
            _read_source(path)
        except ScanError as exc:
            errors.append(str(exc))
    return errors


def _check_size_index() -> tuple[int, list[str], str]:
    """Run detector 2. Returns (exit code, report lines, one-line summary)."""
    found, scan_errors = scan_all_size_index()
    found_prints, _ = scan_size_index_fingerprints()
    baseline, failures = load_size_index_baseline()
    total = sum(len(sites) for sites in found.values())
    straight = sum(
        1 for sites in found.values() for site in sites if site[6] == _CLASS_STRAIGHT_LINE
    )
    summary = (
        f"{total} baselined site(s) across {len(found)} file(s) "
        f"({straight} {_CLASS_STRAIGHT_LINE}, {total - straight} {_CLASS_OTHER_BOUND})"
    )
    if scan_errors:
        report = ["the scan is INCOMPLETE, so its result cannot be trusted:"]
        report += [f"    {error}" for error in scan_errors]
        return 1, report, summary

    # Line lookup so a report can point at the source even though the baseline
    # itself is line-independent.
    where: dict[str, dict[str, tuple[int, str, str, str, int, str, str]]] = {}
    for name, sites in found.items():
        where[name] = {
            size_index_fingerprint(site[1], site[2], site[3], site[5]): site for site in sites
        }

    any_added = False
    for name in sorted(set(found_prints) | set(baseline)):
        actual = found_prints.get(name, [])
        allowed = baseline.get(name, [])
        added = _multiset_difference(actual, allowed)
        removed = _multiset_difference(allowed, actual)
        if added:
            any_added = True
            failures.append(f"{name}: {len(added)} NEW size-assert-then-index site(s):")
            for print_ in added:
                line_no, _symbol, _macro, assertion, index_line, statement, _klass = (
                    where[name][print_]
                )
                failures.append(f"    line {line_no}: {_elide(assertion, 90)}")
                failures.append(f"    line {index_line}: {_elide(statement, 90)}")
        if removed:
            failures.append(
                f"{name}: {len(removed)} baselined site(s) no longer found. This baseline is "
                f"SHRINK-ONLY: delete these entries from {SIZE_INDEX_BASELINE_PATH.name} so the "
                f"slack cannot be reoccupied by a new violation."
            )
            for print_ in removed:
                failures.append(f"    {print_}")
    if any_added:
        failures.append(
            "Neither REQUIRE (DOCTEST_CONFIG_NO_EXCEPTIONS in this build) nor CHECK (in any "
            "build) aborts: on failure they report and CONTINUE, so the index runs out of "
            "bounds. LocalVector::operator[] and CowData::get abort UNCONDITIONALLY, killing "
            "the process before doctest prints its summary - the batch then reports "
            "cases=0/0, not a red test. Write instead: "
            "if (c.size() != N) { FAIL(\"... got \", c.size()); return; } - or an `else` "
            f"branch where independent assertions follow it. ({SIZE_INDEX_ISSUE})"
        )
    return (1 if failures else 0), failures, summary


def _regenerate_size_index_baseline() -> int:
    """Rewrite detector 2's baseline, REFUSING to add an entry.

    Shrink-only is enforced here mechanically rather than left to review: the
    whole point of the baseline is that a new site cannot be absorbed into it.
    """
    found_prints, scan_errors = scan_size_index_fingerprints()
    if scan_errors:
        print("[size-then-index] REFUSED: the scan is incomplete.")
        for error in scan_errors:
            print(f"    {error}")
        return 1
    baseline, problems = load_size_index_baseline()
    if problems:
        print("[size-then-index] REFUSED: the existing baseline cannot be read.")
        for problem in problems:
            print(f"    {problem}")
        return 1
    additions = {
        name: _multiset_difference(prints, baseline.get(name, []))
        for name, prints in found_prints.items()
    }
    additions = {name: added for name, added in additions.items() if added}
    if additions:
        print(
            f"[size-then-index] REFUSED: regeneration would ADD "
            f"{sum(len(v) for v in additions.values())} entr(ies). This baseline may only "
            f"shrink - a new site is a new crash, not a new baseline line. Fix the site."
        )
        for name in sorted(additions):
            for print_ in additions[name]:
                print(f"    {name}: {print_}")
        return 1
    document = {
        "schema_version": 1,
        "issue_url": SIZE_INDEX_ISSUE,
        "note": _SIZE_INDEX_BASELINE_NOTE,
        "files": {name: found_prints[name] for name in sorted(found_prints)},
    }
    SIZE_INDEX_BASELINE_PATH.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    total = sum(len(v) for v in found_prints.values())
    print(f"[size-then-index] baseline rewritten: {total} site(s) across {len(found_prints)} file(s).")
    return 0


def main(argv: list[str] | None = None) -> int:
    # Deliberately not argparse: main() is called with no arguments by the unit
    # test and by run_module_tests.py, and argparse would then parse THEIR argv.
    arguments = list(argv or [])
    regenerate = SIZE_INDEX_REGENERATE_FLAG in arguments
    unknown = [a for a in arguments if a != SIZE_INDEX_REGENERATE_FLAG]
    if unknown:
        print(f"[require-null-deref] FAIL unknown argument(s): {' '.join(unknown)}")
        return 1

    files = _test_sources()
    if not files:
        print("[require-null-deref] FAIL no test sources found - the scan is broken.")
        return 1

    read_errors = _preflight_sources(files)
    if read_errors:
        print(f"[require-null-deref] FAIL {len(read_errors)} test source(s) could not be scanned.")
        for error in read_errors:
            print(f"  - {error}")
        return 1

    if regenerate:
        return _regenerate_size_index_baseline()

    found = scan_all()
    found_prints = scan_fingerprints()
    baseline, failures = load_baseline()
    total = sum(len(v) for v in found.values())
    # line lookup so a report can point at the source even though the baseline
    # itself is line-independent.
    where = {
        name: {
            fingerprint(sym, form, stmt): (line_no, sym, form, stmt)
            for line_no, sym, form, stmt in violations
        }
        for name, violations in found.items()
    }

    for name in sorted(set(found_prints) | set(baseline)):
        actual = found_prints.get(name, [])
        allowed = baseline.get(name, [])
        added = _multiset_difference(actual, allowed)
        removed = _multiset_difference(allowed, actual)
        if added:
            failures.append(f"{name}: {len(added)} NEW REQUIRE-then-dereference site(s):")
            for print_ in added:
                line_no, symbol, form, statement = where[name][print_]
                failures.append(
                    f"    line {line_no}: REQUIRE({symbol} {form}) then `{_elide(statement, 90)}`"
                )
            failures.append(
                f"    REQUIRE does not abort in this build (DOCTEST_CONFIG_NO_EXCEPTIONS): on "
                f"failure it reports and CONTINUES, so the dereference runs on null and crashes "
                f"the whole test binary, taking every later case with it. Write instead: "
                f"if (!<symbol>) {{ FAIL(\"...\"); return; }}  ({BASELINE_ISSUE})"
            )
        if removed:
            failures.append(
                f"{name}: {len(removed)} baselined site(s) no longer found - the ratchet must "
                f"tighten. Remove these from {BASELINE_PATH.name} so the slack cannot be "
                f"reoccupied by a new violation:"
            )
            for print_ in removed:
                failures.append(f"    {print_}")

    if failures:
        print(f"[require-null-deref] FAIL {total} site(s) found across {len(found)} file(s).")
        for failure in failures:
            print(f"  - {failure}" if not failure.startswith("    ") else failure)
        status = 1
    else:
        print(
            f"[require-null-deref] PASS {len(files)} test source(s) scanned; "
            f"{total} baselined site(s) across {len(found)} file(s), 0 new, 0 stale."
        )
        status = 0

    # Detector 2 always runs, even when detector 1 already failed: one guard
    # masking the other's report is how a second defect ships behind a first.
    size_status, size_report, size_summary = _check_size_index()
    if size_status:
        print(f"[size-then-index] FAIL {size_summary}.")
        for line in size_report:
            print(f"  - {line}" if not line.startswith("    ") else line)
    else:
        print(
            f"[size-then-index] PASS {len(files)} test source(s) scanned; "
            f"{size_summary}, 0 new, 0 stale."
        )
    return status or size_status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
