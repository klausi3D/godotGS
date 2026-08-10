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
* **The same direction test everywhere.** A control-flow header and a
  short-circuit operand are judged by the same `_bound_direction` as the
  assertion. Until #849's round-2 review they were judged by weaker rules of their
  own - any mention of the container's cardinality bounded a body, and any
  relational operator made an operand a guard - so `if (v.is_empty()) { v[0]; }`,
  `if (i >= v.size()) { v[i]; }` and `CHECK(v.size() == 0 && v[0]);` were all
  reported clean. Those are false NEGATIVES over live crash sites, which is the
  one failure this detector cannot afford.
* **The container is resolved by walking BACKWARD over a balanced expression**,
  so `chunks[order[0]].indices`, `importer->get_preset_name(i)` and
  `Path::get_source(asset)` are each ONE symbol at any nesting depth. A forward
  regex has to pick a nesting limit, and past it Python backtracks to the longest
  tail it can consume - the bare member name `indices` - which then matches an
  unrelated `other.indices[0]`. An object that is not an expression at all
  (`(a + b).size()`) is a ScanError, never an assertion with no size predicate.
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
  still flagged on the post-loop `a[0]`. A body that shares its header's LINE is
  expanded into the same pieces first, so `if (v.is_empty()) { CHECK(v[0]); }` -
  which indexes precisely when the container is empty - is flagged exactly as the
  multi-line spelling is.
* **A bound from a DIFFERENT container does not count.** In
  `for (i < a.size()) { CHECK(a[i] == b[i]); }` after `REQUIRE(b.size() == 3)`,
  `b[i]` crashes whenever `b` is the short one. Seven such sites exist; they are
  flagged, and reported separately from the straight-line ones so the two
  populations stay auditable (see "Reconciliation" below).
* **A bound must reach the SPECIFIC index, not merely the container.** This is a
  relation, not a boolean, and until #849's round-6 review it was a boolean: any
  lower bound on the container suppressed every subscript under it, so

      REQUIRE(v.size() == 2);
      if (!v.is_empty()) { CHECK(v[1]); }                      // reported CLEAN
      for (uint32_t i = 0; i < v.size(); i++) { CHECK(v[i + 1]); }   // reported CLEAN

  were both silent over a real batch-killing index - length 1 fails the assertion,
  enters the branch, and `v[1]` aborts the process. A guard now yields a `_Bound`
  (see the class for the model), and a subscript is suppressed by exactly three
  rules and nothing else:

  1. an integer LITERAL below the proven minimum length (`v.size() >= 4` covers
     `v[0..3]`, `!v.is_empty()` covers `v[0]` alone);
  2. an index EXPRESSION proven `0 <= e < size()`, matched textually modulo casts,
     parentheses and whitespace - `for (uint32_t i = 0; i < v.size(); ++i)` covers
     `v[i]` and `v[(uint32_t)i]`, never `v[i + 1]` or `v[j]`;
  3. the last-element idiom `v[w.size() - k]` for a literal `k` no greater than the
     minimum, over a `w` whose length is proven equal to `v`'s. The corpus writes
     exactly this at `test_gaussian_importer.h:2933`, where the guard is
     `!brush.is_empty() && reloaded.size() == brush.size()`.

  Every other index expression is unproven and therefore REPORTED. On the current
  corpus rule 1 decides 28 subscripts, rule 2 decides 21 and rule 3 decides 2.
* **The relation must still hold where the index is written.** A proven relation is
  about values and this file compares text, so a statement that rebinds a name drops
  every bound mentioning it (`i += 5; CHECK(v[i]);`), and a statement that can
  resize a length PEER drops that peer (`b.push_back(x); CHECK(a[b.size() - 1]);`).
* **Short-circuiting is honoured**, reusing the same dominance decomposition as
  the null-deref detector with size-aware predicates, so
  `chunks.size() >= 2 && f(chunks[0])` is not flagged - while
  `chunks.size() >= 1 && f(chunks[1])` is, by the same three rules. No site in the
  corpus needs this today; it is here so widening the window later cannot introduce
  a false positive silently.
* The scan stops at a statement that can change the container's length
  (assignment, `resize`, `clear`, `push_back`, ...), and at a depth-0 `return`.

### What detector 2 deliberately does NOT catch

1. **An index further than `_SIZE_SCAN_STATEMENTS` statements away.** This is not
   hypothetical: the fourth site #843 fixed
   (`test_gaussian_splat_node.h:1415`, `REQUIRE(payload.size() == 4)` indexed
   ~20 statements later) is NOT found at the shipped window - re-verified by
   scanning that file at #843's base SHA `d9d2dfd2842`, where a window of 30 does
   find it. The other three #843 sites (`:1288`, `:1323`, `:1424`) ARE found at
   the shipped window, verified the same way.

   Measured on the CURRENT corpus, raising the window to 30 adds exactly **three**
   sites, all real, and no false positive:

       test_gaussian_splat_asset_prune.h:77  out_scales   -> :92   (literal-bounded loop)
       test_gaussian_splat_asset_prune.h:78  out_colors   -> :93   (literal-bounded loop)
       test_projection_math.cpp:69           gpu_results  -> :88   (cross-container)

   A fourth would appear without the short-circuit handling above -
   `test_gaussian_splat_world_io.h:364`, `chunks.size() >= 2 && ...chunks[i]` -
   and is correctly suppressed.

   #849's round-2 and round-6 fixes did NOT change this delta: the window-30 scan
   is site-for-site the same three additions before and after them, so widening is
   neither made safe nor unsafe by them and remains follow-up under #844. It
   changes the baseline (+3) and needs its own delta review. **This blind spot is
   open, not covered.**
2. **Indexes through an alias** (`const T &e = v[0]` then `e`), through
   `.ptr()[i]` or `.get(i)`. Neither of the latter two occurs in the corpus.
3. **An index whose value is bounded somewhere the model does not look.** Since
   round 6 the detector DOES relate the index expression to the proven bound, but
   only through the three rules listed above and only from a guard that dominates
   the subscript. A bound established by an earlier statement
   (`const uint32_t n = v.size() - 1;` then `v[n]`), by an assertion on the index
   rather than the container (`REQUIRE(idx < v.size())` - that IS a bound, but on a
   later `v[idx]` it is the assertion, not a guard), or by any arithmetic other than
   `<peer>.size() - <literal>` is unproven and therefore REPORTED, never suppressed.
   This one is a precision limit, not a soundness limit: it costs false positives,
   not silence.
4. **Non-container `size()`** - anything named `size()` is treated as a length.
5. **A rebinding this file's syntax cannot see.** Round 6's bound expires when a
   statement assigns, increments or decrements a name it depends on, or resizes a
   length peer, and since round 7 a `for` header's own update clause must be
   PROVABLY nondecreasing rather than merely not spelled `--`/`-=`
   (`_update_is_nondecreasing`, a whitelist: `i += delta`, `i = -1` and `f(&i)` all
   refute the header now). What remains is a rebinding the syntax cannot reach:

   * through a reference or pointer alias, or inside a callee handed `&i` - the
     same dataflow blind spot detector 1 has for aliases (its item 2 above);
   * carried over the loop's BACK EDGE. The scan is one forward pass, so a
     rebinding placed AFTER the subscript in the body is seen too late:
     `for (int i = 0; i < v.size(); i++) { CHECK(v[i]); i = -5; }` re-enters with
     `i == -4` and is reported clean today (measured, not inferred). This one is
     structural rather than an oversight - the scan window is
     `_SIZE_SCAN_STATEMENTS` statements, so for most real loops the end of the body
     is not visible at the point the frame is pushed, and "assume a rebinding
     whenever the body is truncated" would report every long loop in the corpus.
     Closing it needs the whole body, i.e. a different scan shape, not another
     rule here.

   That is the honest edge of the model: what a guard proves is three named rules,
   so the surface a future review can attack is enumerable rather than open-ended,
   and it is exactly rule 2's textual identity, rule 3's peer equality, the
   nonnegativity proof `_nonnegative_loop_indices` supplies to rule 2, and the two
   dataflow paths above. Round 4's separate limit - "the increment clause must not
   decrease the same name" - is retired by round 7's whitelist rather than still
   standing beside it.
6. **An integer literal this file cannot read.** `_literal_value` implements C++
   spelling (decimal, `0x`, `0b`, legacy `010` octal, digit separators, `u`/`l`
   suffixes) and returns None for everything else - a named constant, a `sizeof`,
   an ill-formed `09`. None never bounds and never suppresses, so this costs false
   positives, not silence. Legacy octal was in that None bucket until round 7 and
   made the guard OVER-report on valid code, which is its own failure: a ratchet
   that fails on correct input gets waived.

### Reconciliation of the count

#844's sweep of the corpus reported 60 size-shape sites, 14 loop-bounded, 46
dangerous, 4 fixed by #843 -> **42 remaining**. This detector reports **50**:
**43 straight-line**, **7 bounded only by another container's `size()`** and
**0 bounded below the index** (round 6's population - the right container, too
small a bound). All three figures are printed on every run, including the zero, so
that a population cannot arrive unremarked, and all three are pinned by a unit
test.

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

import bisect
import hashlib
import json
import re
import sys
from collections.abc import Callable
from pathlib import Path
from typing import NamedTuple

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
_RAW_STRING_OPEN = r"(?<![A-Za-z0-9_])(?:u8|u|U|L)?R\"([^ ()\\\t\v\f\n]{0,16})\("
_RAW_STRING_OPEN_RE = re.compile(_RAW_STRING_OPEN)

# ONE token regex for the whole lexical pass below. `re.search` returns the
# LEFTMOST match, which is exactly C++'s rule: whichever of `//`, `/*`, a raw
# string opener or an ordinary quote comes FIRST wins, and the others inside it
# are just characters. Alternation order only breaks ties at the same offset,
# where the raw opener must precede the bare quote because it starts at the `R`.
_LEX_TOKEN_RE = re.compile(rf"//|/\*|{_RAW_STRING_OPEN}|\"|'")


def _splices_at(text: str, newline_at: int) -> bool:
    """True when the newline at `newline_at` is DELETED in translation phase 2.

    C++ splices a line whose last character is a backslash into the next one before
    anything else is recognised - comments included. `\\r` counts as part of the
    line terminator (a CRLF file must behave like an LF one), but any other trailing
    character does not: a space between the backslash and the newline is the
    non-conforming spelling that compilers merely warn about, and guessing that it
    splices would let this pass skip real code.
    """
    i = newline_at
    while i > 0 and text[i - 1] == "\r":
        i -= 1
    return i > 0 and text[i - 1] == "\\"


def _splice(text: str) -> tuple[str, list[int]]:
    """Translation phase 2, applied ONCE: `(spliced text, logical -> physical)`.

    Every backslash-newline is deleted, exactly as the compiler does before a single
    token is recognised. The returned map has one entry per logical character plus a
    trailing sentinel, so a logical [start, end) span converts to a physical one and
    the caller can still count the PHYSICAL newlines it covers - which is how the
    line-count contract survives a pass that lexes text the file does not literally
    contain.

    This exists because splicing was being handled per token. Rounds 3 and 4 each
    taught ONE lexical context about it - the `//` body, then the ordinary literal -
    and round 5 found a third (a `//` opener spelled `/` + splice + `/`, which this
    pass read as code and whose `R"(` then blanked real assertions). Splicing is not
    a property of any one token; it happens before tokens exist. So it is done here,
    once, and every recogniser below reads the spliced view instead of the file.
    Deletion is a single left-to-right pass and is deliberately NOT re-applied to
    its own output: `\\\\` + newline + newline leaves a backslash-newline behind in
    the result, and the compiler does not re-splice it either.
    """
    out: list[str] = []
    offsets: list[int] = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] == "\\":
            j = i + 1
            while j < n and text[j] == "\r":
                j += 1
            if j < n and text[j] == "\n":
                i = j + 1
                continue
        out.append(text[i])
        offsets.append(i)
        i += 1
    offsets.append(n)
    return "".join(out), offsets


def _blank_raw_strings(name: str, text: str) -> str:
    """Replace every raw string literal's BODY with nothing, preserving line count.

    `_strip_comments` is deliberately line-oriented, so a MULTI-LINE raw string
    (the PLY fixtures in `test_ply_importer.h` and friends are written that way)
    would otherwise be handed to the scanners as if it were code. Blanking it here
    - before comments are stripped, which is the order the C++ lexer uses - means
    nothing downstream ever reads fixture text as source.

    Comments and ordinary literals are recognised in the SAME pass, not by a later
    line-oriented function, because C++ has one lexer and not two. Searching for
    raw-string openers first read the `R"(` inside a `// explain R"(` comment as a
    real literal and blanked everything up to the next `)"` - which could be another
    comment many lines later, swallowing real code and reporting the file clean;
    without that later `)"` the same file was rejected as unterminated. Both are
    gone once the pass skips a comment as a comment (Codex, PR #849 round 2).

    Every token below is recognised in the SPLICED view (`_splice`), never in the
    physical text, because translation phase 2 runs before phase 3 and a token can
    therefore be spelled across a backslash-newline. Recognising them physically let
    `/` + splice + `/ explain R"(` - a real `//` comment - read as code, so its
    `R"(` opened a raw string that blanked every assertion and index up to a later
    `)"` and the file scanned clean (Codex, PR #849 round 5). That was the THIRD
    splice defect in three rounds, each in a different lexical context, so the rule
    now lives in one place rather than at each recogniser.

    The one deliberate exception is a raw string's own body and closing delimiter:
    [lex.pptoken]/3 REVERTS phases 1-2 between the initial and final quote, so the
    terminator is searched in the physical text.

    The literal is replaced by `""` followed by exactly as many newlines as it
    spanned, so every later line keeps its number. An UNTERMINATED raw string is a
    ScanError: it means the rest of the file cannot be lexed, and guessing is how
    a guard starts reporting on text it does not understand.

    Comments and ordinary literals are left VERBATIM unless they SPAN lines or exist
    only BECAUSE of a splice; `_strip_comments` still removes them, and it can do so
    line by line safely because nothing multi-line and nothing spliced is left. That
    second condition is what keeps the two passes from disagreeing: `_strip_comments`
    reads physical lines and cannot see a spliced comment, so it would read the
    comment's text as code - and a `/*` in there opens a block comment that blanks
    every later assertion to the next `*/` or to EOF.
    """
    logical, to_physical = _splice(text)
    out: list[str] = []
    position = 0
    cursor = 0

    def blank_if_spliced(start_l: int, end_l: int) -> None:
        """Erase a comment the line-oriented pass could not have recognised."""
        nonlocal position
        start_p, end_p = to_physical[start_l], to_physical[end_l]
        if end_p - start_p == end_l - start_l:
            return  # no splice inside it; `_strip_comments` sees the same comment
        out.append(text[position:start_p])
        out.append("\n" * text.count("\n", start_p, end_p))
        position = end_p

    while True:
        token = _LEX_TOKEN_RE.search(logical, cursor)
        if token is None:
            out.append(text[position:])
            return "".join(out)
        lexeme = token.group(0)
        if lexeme == "//":
            end = logical.find("\n", token.end())
            end = len(logical) if end == -1 else end
            blank_if_spliced(token.start(), end)
            cursor = end
            continue
        if lexeme == "/*":
            end = logical.find("*/", token.end())
            # An unterminated block comment swallows the rest of the file for the
            # real compiler too, and `_strip_comments` agrees, so this is not a
            # guess about unlexable text.
            end = len(logical) if end == -1 else end + 2
            blank_if_spliced(token.start(), end)
            cursor = end
            continue
        if lexeme in ('"', "'"):
            end = _skip_plain_literal(logical, token.start())
            start_p, end_p = to_physical[token.start()], to_physical[end]
            spanned = text.count("\n", start_p, end_p)
            if spanned:
                # An ordinary literal continued with backslash-newline. C++ splices
                # it into ONE literal before tokenising; `_strip_comments` cannot,
                # being line-oriented, so it read the continuation as code - and a
                # continuation opening with `/*` started a block comment there that
                # blanked every later assertion, to the next `*/` or to EOF, and the
                # file scanned clean (Codex, PR #849 round 4). Collapsing it here,
                # exactly like a raw string, keeps the invariant this pass exists
                # for: nothing multi-line is left for the line-oriented pass.
                out.append(text[position:start_p])
                out.append('""' + "\n" * spanned)
                position = end_p
            cursor = end
            continue
        terminator = f"){token.group(1)}\""
        start_p = to_physical[token.start()]
        end_p = text.find(terminator, to_physical[token.end()])
        if end_p == -1:
            line_no = text.count("\n", 0, start_p) + 1
            raise ScanError(
                f"{name}:{line_no}: unterminated raw string literal "
                f"(no closing `{terminator}`). Refusing to scan a file this cannot lex."
            )
        end_p += len(terminator)
        out.append(text[position:start_p])
        out.append('""' + "\n" * text.count("\n", start_p, end_p))
        position = end_p
        # Resume in the spliced view at the first character that survives at or
        # after the physical end: `to_physical` is strictly increasing, so this is
        # the only lookup the reverse direction needs.
        cursor = bisect.bisect_left(to_physical, end_p)


def _skip_plain_literal(text: str, at: int) -> int:
    """Offset just past the ordinary `"..."` / `'...'` literal opening at `at`.

    `text` is the SPLICED view, so `"abc \\` continued on the next physical line has
    already become one line here and is one literal, not an unterminated one (Codex,
    PR #849 round 4). Splicing is no longer this function's business; it is applied
    once, for every recogniser, in `_splice`.

    When the literal does not close on that line the quote was not a literal opener
    at all (a digit separator like `1'000`, a stray apostrophe), so only that one
    character is consumed. `_blank_raw_strings` collapses whatever this spans across
    physical newlines, so the line-oriented `_strip_comments` never sees a literal
    that is not contained in one physical line.
    """
    quote = text[at]
    i = at + 1
    while i < len(text):
        ch = text[i]
        if ch == "\n":
            # Reached without a closing quote: the literal does not close on this
            # line, and nothing continues it (every splice is already gone).
            return at + 1
        if ch == "\\":
            # An escape. A backslash immediately before the line terminator cannot
            # appear here - that was the splice - so a trailing one is a malformed
            # literal and ends the search rather than swallowing the newline.
            if i + 1 >= len(text) or text[i + 1] in ("\n", "\r"):
                return at + 1
            i += 2
            continue
        if ch == quote:
            return i + 1
        i += 1
    return at + 1


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

    A `//` comment ending in a backslash CONTINUES onto the next physical line -
    splicing happens before comments are recognised - so that line is emitted empty
    rather than scanned as code. `_blank_raw_strings` applies the same rule on the
    same test (`_splices_at`); if the two disagreed, one of them would blank text
    the other reads.

    That agreement is only reachable for a comment whose OPENER is physically
    contiguous, which is the whole reason this function can stay line-oriented:
    `_blank_raw_strings` erases any comment that exists only because of a splice
    before this ever sees it. Without that, `/` + splice + `/ note /*` reached here
    as two lines of apparent code, the `/*` opened a block comment that is not in
    the source at all, and every assertion to the next `*/` or to EOF was blanked.
    """
    lines_out: list[str] = []
    in_block = False
    in_line_comment = False
    for line in text.split("\n"):
        if in_line_comment:
            # Still inside a spliced `//` comment: the whole line is comment, and it
            # continues again if IT ends in a backslash.
            in_line_comment = _splices_at(line + "\n", len(line))
            lines_out.append("")
            continue
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
                in_line_comment = _splices_at(line + "\n", len(line))
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

# A symbol may not START in the middle of a member chain. Used by every FORWARD
# search for an already-resolved symbol; the resolver below never needs it because
# a backward walk always lands on a real expression start.
_SYMBOL_START = r"(?<![\w.])(?<!->)"

# REQUIRE* and CHECK* both. CHECK is not the weaker case here: it never aborts
# under ANY doctest configuration, so it is strictly worse than a REQUIRE that
# merely does not abort in THIS build. One of the four sites #843 fixed was a
# CHECK.
_SIZE_ASSERT_HEAD_RE = re.compile(r"^\s*((?:REQUIRE|CHECK)\w*)\s*\(")
# The cardinality CALL. Its OBJECT is resolved by walking backward over a balanced
# expression (`_object_start`), not by a forward regex.
#
# The forward regex it replaced could not describe C++: with a bounded grammar of
# one subscript per segment it failed on `chunks[order[0]].indices.size()`, and
# Python's regex engine responded by BACKTRACKING to the longest tail it could
# consume - the bare member name `indices` - which then matched an unrelated
# `other.indices[0]` and reported it as an index of the asserted container. Raising
# the nesting limit only moves the cliff; a balanced walk removes it, and it also
# resolves the call-with-arguments objects (`importer->get_preset_name(i)`,
# `(uint32_t)splats.size()`) the regex grammar had to give up on (Codex, PR #849
# round 2).
_CARDINALITY_CALL_RE = re.compile(r"(?:\.|->)\s*(size|is_empty|empty)\s*\(\s*\)")
_IDENTIFIER_TAIL_RE = re.compile(r"[A-Za-z_]\w*$")
# A relational comparison macro carries the operator in its NAME.
_COMPARISON_MACRO_SUFFIXES: tuple[tuple[str, str], ...] = (
    ("_EQ", "=="), ("_NE", "!="), ("_GE", ">="), ("_LE", "<="), ("_GT", ">"), ("_LT", "<"),
)
_LITERAL_ZERO_RE = re.compile(r"^\(*\s*0[uUlL]*\s*\)*$")
# An INTEGER literal in any C++ base, digit separators included. Only a literal can
# be evaluated here; a named constant, a `sizeof`, or any other runtime expression
# is deliberately not matched, because its value is not knowable from this file.
_INTEGER_LITERAL_RE = re.compile(r"^\(*\s*([0-9][0-9a-fA-FxXbB']*)[uUlL]*\s*\)*$")
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

# Reported classes. All are baselined identically - the class is a LABEL and does
# not enter the fingerprint - and they are distinguished only so the counts stay
# reconcilable against #844's sweep (42 + 7 = 49).
_CLASS_STRAIGHT_LINE = "straight-line"
_CLASS_OTHER_BOUND = "loop-bounded-by-another-container"
# Round 6's population: the container IS bounded where it is indexed, just not far
# enough for THIS subscript (`if (!v.is_empty()) { v[1]; }`). Kept apart from the
# cross-container class because calling it that would be a false statement about
# the code - the bound is on the right container, at the wrong magnitude - and a
# guard that mislabels what it found is the same defect as one that overclaims.
_CLASS_UNDER_BOUND = "bounded-below-the-index"


def _matching_open(text: str, at: int, lo: int) -> int:
    """Offset of the `(`/`[` matching the closer at `at`, or -1 within [lo, at]."""
    closer = text[at]
    opener = "(" if closer == ")" else "["
    depth = 0
    for i in range(at, lo - 1, -1):
        if text[i] == closer:
            depth += 1
        elif text[i] == opener:
            depth -= 1
            if depth == 0:
                return i
    return -1


def _object_start(text: str, at: int, lo: int = 0) -> int | None:
    """Start of the object expression that ends at `at`, or None if there is none.

    Walks BACKWARD over a balanced expression: trailing `(...)`/`[...]` groups, then
    an identifier (with any `::` qualification), then the same again across each
    `.`/`->`. A backward walk is what makes the grammar closed - it handles calls
    with arguments and any nesting depth, where a forward regex has to pick a
    nesting limit and then silently backtracks past it.

    None means the expression is genuinely not an object (`(a + b).size()`), which
    callers must treat as a FAILURE to parse, never as "no container here".
    """
    i = at
    while True:
        j = i
        while j > lo and text[j - 1].isspace():
            j -= 1
        while j > lo and text[j - 1] in ")]":
            open_at = _matching_open(text, j - 1, lo)
            if open_at < 0:
                return None
            j = open_at
            while j > lo and text[j - 1].isspace():
                j -= 1
        name = _IDENTIFIER_TAIL_RE.search(text[lo:j])
        if name is None:
            return None
        j = lo + name.start()
        while j - 2 >= lo and text[j - 2 : j] == "::":
            qualifier = _IDENTIFIER_TAIL_RE.search(text[lo : j - 2])
            if qualifier is None:
                return None
            j = lo + qualifier.start()
        i = j
        k = i
        while k > lo and text[k - 1].isspace():
            k -= 1
        if k - 2 >= lo and text[k - 2 : k] == "->":
            i = k - 2
            continue
        if k - 1 >= lo and text[k - 1] == ".":
            i = k - 1
            continue
        return i


def _cardinality_calls(
    text: str, lo: int, hi: int, name: str, strict: bool
) -> list[tuple[str, str, int, int]]:
    """(symbol, kind, symbol_start, call_end) for each cardinality call in [lo, hi).

    `strict` decides what an unresolvable object means. Where a missed symbol makes
    the scanner report clean over an assertion it did not understand, it is a
    ScanError; where the result only labels an already-reported site, it is skipped.
    """
    found: list[tuple[str, str, int, int]] = []
    for call in _CARDINALITY_CALL_RE.finditer(text, lo, hi):
        start = _object_start(text, call.start(), lo)
        if start is None:
            if not strict:
                continue
            raise ScanError(
                f"{name}: cannot parse the container in `{_elide(text[lo:hi].strip(), 90)}` - "
                f"the object of `{call.group(0).strip()}` is not an object expression. "
                f"Refusing to call this assertion clean."
            )
        found.append((text[start : call.start()].strip(), call.group(1), start, call.end()))
    return found


def _split_symbol_segments(symbol: str) -> list[str]:
    """Split a symbol at `.`/`->` that are OUTSIDE any bracket or parenthesis.

    `re.split` on the accessors cannot be used: it shreds `chunks[a.b].indices` and
    `f(a.b).items` into nonsense parts and builds a regex matching nothing.
    """
    parts: list[str] = []
    depth = 0
    start = 0
    i = 0
    while i < len(symbol):
        ch = symbol[i]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif depth == 0 and (ch == "." or symbol.startswith("->", i)):
            parts.append(symbol[start:i])
            i += 1 if ch == "." else 2
            start = i
            continue
        i += 1
    parts.append(symbol[start:])
    return [part.strip() for part in parts if part.strip()]


def _size_symbol_regex(symbol: str) -> str:
    """A regex matching `symbol` again elsewhere in the window.

    Each segment is rendered token by token with `\\s*` between, so the same
    expression written `[i + 1]` and `[i+1]` is one symbol; segments are joined so
    `.` and `->` stay interchangeable, since a chain written either way is the same
    object. Whitespace is the ONLY difference tolerated - anything else would start
    comparing expressions instead of tracking one container.
    """
    return r"\s*(?:\.|->)\s*".join(
        r"\s*".join(re.escape(token) for token in re.findall(r"\w+|\S", part))
        for part in _split_symbol_segments(symbol)
    )


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


def _literal_is_nonzero(text: str) -> bool:
    """True when `text` is an integer literal whose value is PROVABLY not zero.

    Anything else - a named constant, a `sizeof`, a parameter, any runtime
    expression - is False, because this file cannot know its value. That matters
    for `==` and `>=`, whose direction depends entirely on the operand: as a GUARD,
    `if (v.size() == expected) { v[0]; }` selects the empty case exactly when
    `expected == 0`, so an unknown operand must not count as a lower bound
    (Codex, PR #849 round 3).
    """
    value = _literal_value(text)
    return value is not None and value != 0


def _literal_value(text: str) -> int | None:
    """`text` as an integer literal's value, or None when it is not one.

    Python's `int(_, 0)` implements C++'s spelling rules for every base EXCEPT the
    legacy octal one: it reads `0x10` as 16 and `0b1` as 1, but *raises* on `010`,
    which C++ reads as 8. Left unhandled that made the guard OVER-report - the only
    direction on this file that had not bitten yet - so
    `REQUIRE(v.size() == 9); if (v.size() >= 010) { CHECK(v[7]); }` was called a new
    violation although the branch proves eight elements (Codex, PR #849 round 7).
    The legacy base is therefore selected explicitly.

    Ill-formed spellings stay None, which is the fail-closed answer everywhere this
    is consulted: `09` is not an octal literal in C++ either, and an operand whose
    value cannot be determined must not be allowed to bound anything.
    """
    literal = _INTEGER_LITERAL_RE.match(text.strip())
    if literal is None:
        return None
    digits = literal.group(1).replace("'", "")
    # `0x`/`0X` hex and `0b`/`0B` binary keep base 0; a bare leading zero is octal.
    base = 8 if len(digits) > 1 and digits[0] == "0" and digits[1] not in "xXbB" else 0
    try:
        return int(digits, base)
    except ValueError:
        return None  # not a well-formed literal after all: unproven, so None


def _operand_is_nonnegative(text: str, nonnegative: frozenset[str]) -> bool:
    """True when the operand `text` is PROVABLY at least zero.

    `size() > n` bounds a length below only when `n` is not negative, and Godot's
    containers do not make that free: `Vector::size()` returns `CowData`'s
    `int64_t`, so `v.size() > -1` is true for an EMPTY container and would let
    `v[0]` through as guarded (Codex, PR #849 round 4).

    Two things prove it. An integer literal - the grammar `_INTEGER_LITERAL_RE`
    accepts has no sign at all, so matching it IS the proof, `-1` simply not being
    a literal here. Or a name in `nonnegative`, which a caller has established
    from a declaration it can see (`for (uint32_t i = 0; i < v.size(); ...)`).
    Everything else is unproven and therefore not a bound.
    """
    body = _strip_all_outer_parens(text)
    return _literal_value(body) is not None or body in nonnegative


def _bound_direction(
    text: str,
    span: tuple[int, int],
    kind: str,
    start: int,
    end: int,
    macro: str = "",
    *,
    guard: bool = False,
    nonnegative: frozenset[str] = frozenset(),
) -> bool:
    """True when this `size()`/`is_empty()` occurrence, HELD TRUE, bounds the length below.

    The single place the DIRECTION of a cardinality test is decided. It answers one
    question for three callers that used to answer it three different ways:

    * the assertion (`_establishes_lower_bound`) - decided it correctly;
    * a control-flow header (`_bounds_iteration`) - accepted ANY mention of the
      container's size, so `if (v.is_empty()) { v[0]; }` and
      `if (i >= v.size()) { v[i]; }` marked their bodies safe although both select
      exactly the out-of-bounds case;
    * a short-circuit operand (`_size_bound_tests`) - matched on the OPERATOR
      alone, so `v.size() == 0 && v[0]` and `v.size() != 4 && v[0]` counted as
      guarded although `v[0]` is evaluated precisely when `v` is empty.

    Both were false NEGATIVES: the guard reported clean over a real crash site
    (Codex, PR #849 round 2). Having one implementation is the point - the operand
    and the assertion cannot drift apart again.

    `macro` is empty for plain expressions; only an assertion can carry its relation
    in its name (`REQUIRE_EQ(v.size(), 4)`).

    `guard` says which way to fail when the compared-against operand is NOT a
    literal, because the safe answer is opposite for the two callers:

    * an ASSERTION (`guard=False`): `REQUIRE(v.size() == expected)` asserts a
      cardinality the following statements then index into, and whether `expected`
      is 2 or 0 the index still runs after the assertion fails. Answering True
      REPORTS the site, which is the fail-closed direction here.
    * a GUARD (`guard=True`): `if (v.size() == expected) { v[0]; }` SUPPRESSES the
      report, and with `expected == 0` the branch is entered exactly when `v` is
      empty. So only a provably non-zero operand may bound (Codex, PR #849 round 3).

    `nonnegative` names the identifiers a guard's caller has PROVED to be at least
    zero (see `_operand_is_nonnegative`); it is meaningless for an assertion, whose
    unproven operands report anyway.
    """
    lo, hi = span
    if _is_call_argument(text, start, lo):
        # `resize(other.size())`, `a[v.size()]`: the call is being handed to
        # something else, so the enclosing expression's truth says nothing about
        # this container's length.
        return False
    before = text[lo:start]
    # Any `)` immediately after the call closes a group opened BEFORE the symbol,
    # so the relation of `(v.size()) == 0` sits past it. Not peeling them read the
    # expression as a bare truthiness test with the wrong answer.
    after = re.sub(r"^[\s)]+", " ", text[end:hi])
    if kind in ("is_empty", "empty"):
        # `!v.is_empty()`, `REQUIRE_FALSE(v.is_empty())`, `REQUIRE_UNARY_FALSE(...)`.
        return bool(re.search(r"!\s*$", before)) or macro.endswith("_FALSE")

    relation = re.match(r"\s*(==|!=|>=|<=|>|<)\s*(.*)$", after, re.S)
    if relation:
        operator, other, flipped = relation.group(1), _operand_before(relation.group(2)), False
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
            other = _operand_after(left[: reversed_relation.start()])
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
                # No relation anywhere: the call stands alone as a truthiness test.
                # `if (v.size()) { v[0]; }` and `REQUIRE(v.size())` both bound the
                # length below. (`is_empty()` already returned above - untested, it
                # is the WRONG direction.)
                return True
            arguments = _split_macro_arguments(text[lo:hi])
            if len(arguments) < 2:
                return False
            first_argument_end = lo + len(arguments[0])
            flipped = start >= first_argument_end
            other = arguments[0] if flipped else arguments[1]
    other = other.strip()
    if flipped:
        operator = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(operator, operator)
    against_zero = bool(_LITERAL_ZERO_RE.match(other))
    if operator in ("==", ">="):
        # `size() == 0` / `size() >= 0`: EMPTY and vacuous respectively. Above zero
        # the bound holds only if the operand is KNOWN to be above zero, which as a
        # guard has to be proven and as an assertion is assumed - see `guard`.
        return _literal_is_nonzero(other) if guard else not against_zero
    if operator == "!=":
        return against_zero              # `size() != 0` asserts NON-EMPTY
    if operator == ">":
        # `size() > n` and `idx < size()` put the length at 1 or more only when `n`
        # is itself 0 or more, and Godot's `size()` is SIGNED (`CowData::Size` is
        # int64_t): `if (v.size() > -1) { v[0]; }` is entered on an EMPTY container
        # and suppressed the report (Codex, PR #849 round 4). As an ASSERTION the
        # unproven operand still reports, which is the fail-closed direction there.
        return _operand_is_nonnegative(other, nonnegative) if guard else True
    return False                         # `<` / `<=`: an UPPER bound only


# Keywords that take a parenthesised operand without being a call.
_CONDITION_KEYWORDS = frozenset({"if", "while", "for", "switch", "do", "return", "else"})


def _is_call_argument(text: str, at: int, lo: int = 0) -> bool:
    """True when the expression at `at` is an ARGUMENT rather than an operand.

    Walks out to the innermost group that is still open at `at`. A `[` means a
    subscript (`a[v.size()]`); a `(` preceded by an identifier that is not a
    control-flow keyword means a call (`out.resize(other.size())`). Either way the
    enclosing expression's truth constrains the call's RESULT, not the container -
    `REQUIRE(out.resize(g.size()) == OK)` says nothing about `g`.
    """
    depth = 0
    for i in range(at - 1, lo - 1, -1):
        ch = text[i]
        if ch in ")]":
            depth += 1
        elif ch in "([":
            if depth:
                depth -= 1
                continue
            if ch == "[":
                return True
            head = text[lo:i].rstrip()
            name = re.search(r"(\w+)$", head)
            if name is not None:
                return name.group(1) not in _CONDITION_KEYWORDS
            return bool(re.search(r"[\]\)]$", head))  # `f(a)(...)`, `fns[i](...)`
    return False


# `,` `;` `?` `:` and the two short-circuit operators all end an operand.
_OPERAND_BREAK = ("&&", "||")


def _operand_before(text: str) -> str:
    """`text` up to the first top-level separator or the first UNMATCHED `)`.

    Isolates the value a relation is compared against, so `v.size() == 0 && flag`
    compares against `0` and not against `0 && flag` - which is not the literal zero
    and so read as a NON-empty assertion, the exact inversion this guard must not make.
    """
    depth = 0
    for i, ch in enumerate(text):
        if ch in "([":
            depth += 1
        elif ch in ")]":
            if depth == 0:
                return text[:i]
            depth -= 1
        elif depth == 0 and (ch in ",;?:" or any(text.startswith(op, i) for op in _OPERAND_BREAK)):
            return text[:i]
    return text


def _operand_after(text: str) -> str:
    """`text` after the last top-level separator or the last UNMATCHED `(`.

    The mirror of `_operand_before` for a REVERSED relation (`flag && 0 != v.size()`).
    """
    depth = 0
    for i in range(len(text) - 1, -1, -1):
        ch = text[i]
        if ch in ")]":
            depth += 1
        elif ch in "([":
            if depth == 0:
                return text[i + 1 :]
            depth -= 1
        elif depth == 0:
            for op in _OPERAND_BREAK:
                if text.startswith(op, i):
                    return text[i + len(op) :]
            if ch in ",;?:":
                return text[i + 1 :]
    return text


def _strip_all_outer_parens(expr: str) -> str:
    """`expr` with every wrapping paren pair removed - `((a && b))` is `a && b`."""
    body = expr.strip()
    while True:
        inner, offset = _strip_outer_parens(body)
        if not offset:
            return body
        body = inner.strip()


def _equal_cardinality_partner(symbol: str, expr: str) -> str | None:
    """The container `expr` equates `symbol`'s LENGTH to, when that is all it says.

    `reloaded.size() == original.size()` carries a lower bound from `original` to
    `reloaded`, but only half of one: the caller still has to bound `original`, and
    only a conjunction can do that (see `_expression_bound`). Both sides must
    be exactly a `size()` call and nothing else, so `a.size() == b.size() - 1` and
    `a.size() == b.size() + n` do not qualify.
    """
    body = _strip_all_outer_parens(expr)
    sides = _split_top_level(body, "==")
    if len(sides) != 2:
        return None
    left, right = (body[span[0] : span[1]].strip() for span in sides)
    own = rf"{_size_symbol_regex(symbol)}\s*(?:\.|->)\s*size\s*\(\s*\)"
    for mine, theirs in ((left, right), (right, left)):
        if re.fullmatch(own, mine) is None:
            continue
        calls = _cardinality_calls(theirs, 0, len(theirs), "", strict=False)
        if len(calls) != 1:
            continue
        other, kind, start, end = calls[0]
        if other and kind == "size" and start == 0 and end == len(theirs):
            return other
    return None


# The relations an atom may consist of. `<=>` is NOT here: three-way comparison
# yields an ordering, not a truth, and reading it as one would be a guess.
_ATOM_RELATIONS = ("==", "!=", ">=", "<=", ">", "<")
_CAST_PREFIX_RE = re.compile(r"^\(\s*(?:const\s+)?[A-Za-z_][\w:]*(?:\s*[*&]+)?\s*\)\s*")


def _split_atom_relation(text: str) -> tuple[str, str, str] | None:
    """`(left, operator, right)` when `text` is ONE top-level comparison, else None.

    None means "this atom is not a plain comparison", and the caller must then
    treat it as no bound at all. Everything unmodelled lands there deliberately:
    a second comparison (`a < b < c`), a three-way `<=>`, a comma operator whose
    value is its LAST operand, the angle brackets of `static_cast<int>(...)`.
    `->` and the shift operators are stepped over rather than read as `>`/`<`.
    """
    depth = 0
    found: tuple[int, int] | None = None
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch in "([{":
            depth += 1
            i += 1
            continue
        if ch in ")]}":
            depth -= 1
            i += 1
            continue
        if depth == 0:
            if text.startswith("<=>", i):
                return None
            if text.startswith("->", i) or text.startswith("<<", i) or text.startswith(">>", i):
                i += 2
                continue
            if ch == ",":
                return None
            for operator in _ATOM_RELATIONS:
                if text.startswith(operator, i):
                    if found is not None:
                        return None
                    found = (i, i + len(operator))
                    i += len(operator)
                    break
            else:
                i += 1
            continue
        i += 1
    if found is None:
        return None
    lo, hi = found
    return text[:lo], text[lo:hi], text[hi:]


def _strip_assignment_prefix(text: str) -> str:
    """`text` with any leading `lhs =` chain removed.

    `const bool ok = v.size() >= 2` is true exactly when `v.size() >= 2` is, so the
    declaration in front of an operand is noise rather than structure. Only a PLAIN
    `=` is dropped: a compound assignment (`n += v.size()`) yields the result of the
    operation, whose truth is not the right-hand side's.
    """
    depth = 0
    cut: int | None = None
    i = 0
    while i < len(text):
        ch = text[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and ch == "=":
            if text.startswith("==", i):
                i += 2
                continue
            if i and text[i - 1] in "=!<>+-*/%&|^":
                i += 1
                continue
            cut = i + 1
        i += 1
    return text if cut is None else text[cut:]


def _bare_cardinality(symbol: str, text: str) -> tuple[str, str] | None:
    """`(kind, call text)` when `text` is EXACTLY `symbol`'s cardinality call.

    Wrapping parens and C-style casts are peeled - `(uint32_t)v.size()` is still
    just the length - but nothing else is: `v.size() - 1` and `f(v.size())` are
    values built FROM the length, and a relation over them says nothing about it.
    """
    body = _strip_all_outer_parens(text)
    while True:
        peeled = _strip_all_outer_parens(_CAST_PREFIX_RE.sub("", body, count=1))
        if peeled == body:
            break
        body = peeled
    match = re.fullmatch(
        rf"\s*{_size_symbol_regex(symbol)}\s*(?:\.|->)\s*(size|is_empty|empty)\s*\(\s*\)\s*",
        body,
    )
    return (match.group(1), body.strip()) if match else None


def _atom_cardinality(symbol: str, body: str) -> tuple[str, str, str, str] | None:
    """`(kind, call, operator, operand)` when the atom's WHOLE truth is that test.

    An atom has no `&&`, `||` or `?:` left in it, but it can still be built out of
    a cardinality test rather than BE one, and then its truth does not follow the
    test's. `(v.size() > 0) == expected_nonempty` is the case Codex found in round
    3's fix (PR #849 round 4): the inner `> 0` points the right way, so scanning
    the atom for a qualifying subexpression accepted it - while with
    `expected_nonempty == false` the atom is true exactly when `v` is EMPTY.

    So one side of the comparison must be the cardinality call and NOTHING else,
    and with no comparison at all the atom must be the bare call (`if (v.size())`).
    Anything else returns None, which the caller reads as "not a bound" - the
    fail-closed answer, since being wrong here suppresses a report.
    """
    body = _strip_all_outer_parens(_strip_assignment_prefix(_strip_all_outer_parens(body)))
    relation = _split_atom_relation(body)
    if relation is None:
        bare = _bare_cardinality(symbol, body)
        return (bare[0], bare[1], "", "") if bare else None
    left, operator, right = relation
    bare = _bare_cardinality(symbol, left)
    if bare is not None:
        return bare[0], bare[1], operator, right.strip()
    bare = _bare_cardinality(symbol, right)
    if bare is None:
        return None
    return bare[0], bare[1], _FLIPPED_RELATION[operator], left.strip()


_FLIPPED_RELATION = {"<": ">", ">": "<", "<=": ">=", ">=": "<=", "==": "==", "!=": "!="}


class _Bound(NamedTuple):
    """What a guard being TRUE proves about which subscripts of a container are safe.

    A boolean "the container is non-empty" is NOT what a subscript needs. `v[1]`
    under `if (!v.is_empty())` still runs when the length is 1, so the guard has to
    be related to the specific index expression rather than to the container
    (Codex, PR #849 round 6). Three facts do that, and they are the three fields:

    * `minimum` - a proven lower bound on the LENGTH. It makes exactly the constant
      subscripts `0 .. minimum - 1` safe. `!v.is_empty()` and `v.size() != 0` prove
      1; `v.size() == 4` and `v.size() >= 4` prove 4; `v.size() > 4` proves 5.
    * `below` - index EXPRESSIONS, normalised, proven to satisfy `0 <= e < size()`.
      `for (uint32_t i = 0; i < v.size(); ++i)` puts `i` here, which is what makes
      `v[i]` safe and leaves `v[i + 1]` and `v[j]` unproven.
    * `lengths` - containers, normalised, whose length is proven EQUAL to this
      one's. That is what makes the last-element idiom decidable: `v[w.size() - 1]`
      is in range when `w.size() == v.size()` and `minimum >= 1`, and #844's corpus
      writes exactly that (`test_gaussian_importer.h:2933`). The container's own
      spelling counts implicitly, so `v[v.size() - 1]` needs no entry.

    Anything the model cannot relate to one of those three facts is NOT covered, so
    an unmodelled index expression reports rather than being suppressed.
    """

    minimum: int
    below: frozenset[str]
    lengths: frozenset[str] = frozenset()


_NO_BOUND = _Bound(0, frozenset())
# A guard that proves only non-emptiness: index 0 and nothing else.
_NONEMPTY_BOUND = _Bound(1, frozenset())


def _bound_union(left: _Bound, right: _Bound) -> _Bound:
    """Both bounds hold (a conjunction), so take the STRONGER of each fact."""
    return _Bound(
        max(left.minimum, right.minimum),
        left.below | right.below,
        left.lengths | right.lengths,
    )


def _bound_intersection(left: _Bound, right: _Bound) -> _Bound:
    """Either bound may be the only one that holds (a disjunction or a ternary)."""
    return _Bound(
        min(left.minimum, right.minimum),
        left.below & right.below,
        left.lengths & right.lengths,
    )


def _peel_casts(text: str) -> str:
    """`text` with wrapping parens and leading C-style casts removed."""
    body = _strip_all_outer_parens(text)
    while True:
        peeled = _strip_all_outer_parens(_CAST_PREFIX_RE.sub("", body, count=1))
        if peeled == body:
            return body
        body = peeled


def _normalize_index(expr: str) -> str:
    """The spelling an index expression is compared by: no casts, parens or spaces.

    Deliberately TEXTUAL. `i` and `(uint32_t)i` and `( i )` are the same subscript;
    `i` and `i + 0` are not, and the second is simply unproven. Guessing at
    arithmetic equivalences here would suppress reports, which is the direction
    this detector must never be wrong in.
    """
    return re.sub(r"\s+", "", _peel_casts(expr))


_LENGTH_EXPRESSION_RE = re.compile(r"(.+?)(?:\.|->)size\(\)")


def _length_minus_literal(index: str) -> tuple[str, int] | None:
    """`(container, k)` when `index` is `<container>.size() - k` for a literal k >= 1.

    The last-element idiom, and the only arithmetic on an index this model reads.
    `v[v.size() - 1]` is in range exactly when `v.size() >= 1`, so it is decidable
    from `minimum` - unlike `v[i + 1]`, `v[n - 1]` or `v[v.size() - k]` for a
    non-literal k, which are all returned as None and therefore not covered.
    """
    depth = 0
    cut = -1
    for i, ch in enumerate(index):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif depth == 0 and ch == "-" and i and index[i + 1 : i + 2] != ">":
            if index[i - 1] in "+-*/%<>=!&|^,(":
                continue                      # unary minus, or part of `->` / `--`
            cut = i
    if cut < 0:
        return None
    value = _literal_value(index[cut + 1 :])
    if value is None or value < 1:
        return None
    base = _LENGTH_EXPRESSION_RE.fullmatch(index[:cut])
    return (base.group(1), value) if base else None


def _bound_covers(bound: _Bound, index: str | None, symbol: str = "") -> bool:
    """True when `bound` proves the subscript `index` is in range.

    Three ways, one per field of `_Bound`, and nothing else: the expression is
    listed in `below`, it is a constant literal under `minimum`, or it is the
    last-element idiom `<peer>.size() - k` with `k <= minimum` over a container
    whose length is proven equal to this one's.

    `index` is None when the subscript's brackets do not balance, which is not
    provable and therefore not covered. Every other unmodelled shape lands in the
    same place: `False`, which REPORTS the site.
    """
    if index is None:
        return False
    normalized = _normalize_index(index)
    if not normalized:
        return False
    if normalized in bound.below:
        return True
    value = _literal_value(normalized)
    if value is not None:
        return value < bound.minimum
    offset = _length_minus_literal(normalized)
    if offset is None:
        return False
    container, subtracted = offset
    peers = bound.lengths | ({_normalize_index(symbol)} if symbol else frozenset())
    return container in peers and subtracted <= bound.minimum


def _atom_bound(
    kind: str, call: str, operator: str, other: str, nonnegative: frozenset[str]
) -> _Bound:
    """How much a single cardinality atom proves, once its DIRECTION is settled.

    Called only after `_bound_direction` has said the atom bounds the length below,
    so this adds magnitude on top of that answer and can only ever narrow it. The
    direction is still decided in exactly one place; splitting it in two is how the
    assertion and the guard drifted apart in round 2.
    """
    rendered = call if not operator else f"{call} {operator} {other}"
    if not _bound_direction(
        rendered, (0, len(rendered)), kind, 0, len(call), guard=True, nonnegative=nonnegative
    ):
        return _NO_BOUND
    operand = _peel_casts(other)
    value = _literal_value(operand)
    if operator in ("==", ">="):
        # `_bound_direction` already required a non-zero LITERAL for these as a
        # guard, so `value` is known here: `size() == 4` and `size() >= 4` each
        # make 0..3 safe.
        return _Bound(value, frozenset()) if value is not None else _NO_BOUND
    if operator == ">":
        if value is not None:
            return _Bound(value + 1, frozenset())   # `size() > 4` makes 0..4 safe
        # A proven-nonnegative NAME: `i < v.size()` says `v[i]` is in range, and
        # since `i >= 0` it also says the length is at least 1. It says nothing at
        # all about `v[i + 1]` or `v[j]`.
        return _Bound(1, frozenset({_normalize_index(operand)}))
    # `size() != 0` and the bare truthiness test `if (v.size())` prove non-empty.
    return _NONEMPTY_BOUND


def _expression_bound(
    symbol: str, expr: str, nonnegative: frozenset[str] = frozenset()
) -> _Bound:
    """What `expr` being TRUE **as a whole** proves about `symbol`'s subscripts.

    The expression is DECOMPOSED by precedence rather than scanned for a qualifying
    subexpression. Scanning accepted any single `size()` test pointing the right
    way, so `if (v.size() > 0 || fallback) { CHECK(v[0]); }` and
    `CHECK((v.size() > 0 || fallback) && v[0]);` both read as bounded although
    `fallback == true` admits the index on an EMPTY container (Codex, PR #849
    round 3). What matters is not that a bound appears somewhere but that the
    expression cannot be true without it:

    * `c ? a : b` - both arms must bound, since either may be the one taken, so
                    only what BOTH prove survives (`_bound_intersection`);
    * `A || B`    - EVERY disjunct must bound, since any one of them may be the
                    only true one - again the intersection;
    * `A && B`    - all conjuncts hold together, so their proofs COMBINE
                    (`_bound_union`), and one of them may also supply what ANOTHER
                    needs (`_equal_cardinality_partner`).

    Whatever is left is an atom, and an atom is a bound only when its WHOLE truth
    is the cardinality test - `_atom_cardinality` decides that, because scanning
    the atom for a nested test accepted `(v.size() > 0) == expected_nonempty`,
    which with `expected_nonempty == false` is true exactly when `v` is EMPTY
    (Codex, PR #849 round 4). Negation is answered by `_size_negative_test`, the
    only thing that knows which tests imply non-emptiness when FALSE, so
    `!v.is_empty()` still bounds while `!v.size()` (true exactly when empty) does
    not - and, being a non-emptiness test, it proves index 0 and no more.

    `nonnegative` carries the identifiers the CALLER has proved to be at least
    zero, which is what makes `for (uint32_t i = 0; i < v.size(); ++i)` bound its
    body while a bare `if (i < v.size())` does not (see `_operand_is_nonnegative`).
    """
    body = _strip_all_outer_parens(expr)
    if not body:
        return _NO_BOUND

    ternary = _ternary_spans(body)
    if ternary:
        arms = [
            _expression_bound(symbol, body[span[0] : span[1]], nonnegative)
            for span in ternary[1:]
        ]
        return _bound_intersection(arms[0], arms[1])
    disjuncts = _split_top_level(body, "||")
    if len(disjuncts) > 1:
        bound = _expression_bound(symbol, body[disjuncts[0][0] : disjuncts[0][1]], nonnegative)
        for span in disjuncts[1:]:
            bound = _bound_intersection(
                bound, _expression_bound(symbol, body[span[0] : span[1]], nonnegative)
            )
        return bound
    conjuncts = _split_top_level(body, "&&")
    if len(conjuncts) > 1:
        parts = [body[span[0] : span[1]] for span in conjuncts]
        bound = _NO_BOUND
        for part in parts:
            bound = _bound_union(bound, _expression_bound(symbol, part, nonnegative))
        # All conjuncts hold together, so one of them may prove what another needs:
        # `!a.is_empty() && b.size() == a.size()` bounds `b` even though neither
        # half does alone (test_gaussian_importer.h:2930). The lengths are EQUAL,
        # so `b` inherits the partner's bound whole - the minimum length, the index
        # expressions proven below it, AND the partner itself as a length peer,
        # which is what decides `b[a.size() - 1]` two lines further down.
        for position, part in enumerate(parts):
            partner = _equal_cardinality_partner(symbol, part)
            if partner is None:
                continue
            siblings = parts[:position] + parts[position + 1 :]
            for sibling in siblings:
                bound = _bound_union(bound, _expression_bound(partner, sibling, nonnegative))
            bound = _bound_union(
                bound, _Bound(0, frozenset(), frozenset({_normalize_index(partner)}))
            )
        return bound

    negated = False
    while body.startswith("!") and not body.startswith("!="):
        negated = not negated
        body = _strip_all_outer_parens(body[1:])
    if negated:
        return _NONEMPTY_BOUND if _size_negative_test(symbol, body) else _NO_BOUND

    atom = _atom_cardinality(symbol, body)
    if atom is None:
        return _NO_BOUND
    kind, call, operator, other = atom
    # `_atom_bound` re-renders the atom in the one canonical spelling
    # `size() <op> <operand>` so that `_bound_direction` - still the single place a
    # DIRECTION is decided - reads the same relation whichever side of the atom the
    # call sat on, and then adds the MAGNITUDE that direction alone cannot give.
    return _atom_bound(kind, call, operator, other, nonnegative)


def _condition_lower_bounds(expr: str) -> bool:
    """True when `expr` being TRUE bounds SOME container's length from below.

    Used only to classify a reported site as loop-bounded-by-another-container, so
    the two counts stay reconcilable against #844's sweep. It is direction-aware for
    the same reason `_expression_bound` is: a header that merely mentions a
    `size()` has not bounded anything. It is deliberately NOT decomposed, not
    `guard=True` strict and not related to the index expression the way
    `_expression_bound` is, because its answer can only change the LABEL on a site
    that is already reported, never hide one.
    """
    return any(
        _bound_direction(expr, (0, len(expr)), kind, start, end)
        for _, kind, start, end in _cardinality_calls(expr, 0, len(expr), "", strict=False)
    )


def _size_assertions(fragment: str, name: str) -> list[tuple[str, str]]:
    """(container symbol, macro name) for each lower bound this assertion asserts.

    STRICT: a cardinality call whose object cannot be resolved is a ScanError, not
    an assertion with no size predicate. The scanner would otherwise go quiet over
    an assertion it did not understand and the statements that follow it.
    """
    head = _SIZE_ASSERT_HEAD_RE.match(fragment)
    if head is None:
        return []
    macro = head.group(1)
    span = _macro_argument_span(fragment, name)
    lo, hi = span
    found: list[tuple[str, str]] = []
    seen: set[str] = set()
    for symbol, kind, start, end in _cardinality_calls(fragment, lo, hi, name, strict=True):
        # Depth 0 relative to the macro: a DIRECT argument. Nested deeper it is an
        # argument to some other call, e.g.
        # `REQUIRE(cpu_results.resize(ground_truth.size()) == OK)`, which
        # constrains the resize result and says nothing about `ground_truth`.
        if _paren_depth(fragment, lo, start) != 0 or symbol in seen:
            continue
        if not _bound_direction(fragment, span, kind, start, end, macro):
            continue
        seen.add(symbol)
        found.append((symbol, macro))
    return found


def _index_expressions(symbol: str, text: str) -> list[tuple[int, str | None]]:
    """(offset, subscript text) for each place `text` subscripts `symbol`.

    The subscript itself is what a bound has to be related to: `v[0]` and `v[1]`
    are not made safe by the same guard. A subscript whose brackets do not balance
    within `text` yields None, which no bound covers - the fail-closed answer for
    something the scanner could not read.
    """
    pattern = rf"(?<![\w)\]]){_size_symbol_regex(symbol)}\s*\["
    found: list[tuple[int, str | None]] = []
    for match in re.finditer(pattern, text):
        open_at = match.end() - 1
        depth = 0
        inner: str | None = None
        for i in range(open_at, len(text)):
            if text[i] in "([{":
                depth += 1
            elif text[i] in ")]}":
                depth -= 1
                if depth == 0:
                    inner = text[open_at + 1 : i]
                    break
        found.append((match.start(), inner))
    return found


def _size_bound_tests(index: str | None) -> tuple[Callable[[str, str], bool], Callable[[str, str], bool]]:
    """The (positive, negative) pair `_short_circuit_guarded` needs for ONE subscript.

    A short-circuit operand is judged exactly like a control-flow header: not "does
    it bound the container" but "does it bound THIS index". `v.size() >= 1 && v[1]`
    short-circuits on a test that proves only index 0, so it does not guard `v[1]`
    (Codex, PR #849 round 6). Before that the operand's bound was a boolean, and
    any lower bound on the container suppressed every subscript of it.

    The negative side is the `A || B` case (`v.is_empty() || v[0]`): the operand
    being FALSE proves non-emptiness and nothing more, so it covers index 0 only.
    """

    def positive(symbol: str, expr: str) -> bool:
        bound = _expression_bound(symbol, _strip_outer_parens(expr.strip())[0].strip())
        return _bound_covers(bound, index, symbol)

    def negative(symbol: str, expr: str) -> bool:
        return _size_negative_test(symbol, expr) and _bound_covers(
            _NONEMPTY_BOUND, index, symbol
        )

    return positive, negative


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


def _unguarded_index(symbol: str, text: str, bound: _Bound = _NO_BOUND) -> bool:
    """True when `text` subscripts `symbol` at an index nothing here proves in range.

    `bound` is what the ENCLOSING blocks have proved (see `_first_unbounded_index`).
    Each subscript is judged against it individually, because a bound on the
    container is not a bound on every index of it: under `if (!v.is_empty())`,
    `v[0]` is covered and `v[1]` is not (Codex, PR #849 round 6).
    """
    for at, index in _index_expressions(symbol, text):
        if _bound_covers(bound, index, symbol):
            continue
        positive, negative = _size_bound_tests(index)
        if not _short_circuit_guarded(symbol, text, at, positive=positive, negative=negative):
            return True
    return False


_CONTROL_HEAD_RE = re.compile(r"\s*(?:\}\s*)?(?:else\s+)?(if|while|switch|for|do)\b")


def _control_condition(header: str) -> str:
    """The CONDITION of a control-flow header - what its true branch actually tests.

    `for` yields its middle clause, since the initializer and the increment bound
    nothing. `switch` and `do` yield nothing: neither carries a boolean condition,
    so neither can bound an index, and pretending otherwise is how
    `switch (v.size()) { case 0: v[0]; }` would be called safe.
    """
    head = _CONTROL_HEAD_RE.match(header)
    if head is None:
        return header
    keyword = head.group(1)
    if keyword in ("switch", "do"):
        return ""
    inner = _control_operand(header, head.end())
    if inner is None:
        return ""  # `else {` - no condition at all
    if keyword == "for":
        clauses = _split_top_level(inner, ";")
        return inner[clauses[1][0] : clauses[1][1]] if len(clauses) >= 2 else ""
    return inner


def _control_operand(header: str, at: int) -> str | None:
    """The text inside the parentheses a control-flow keyword opens after `at`."""
    open_at = header.find("(", at)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(header)):
        if header[i] == "(":
            depth += 1
        elif header[i] == ")":
            depth -= 1
            if depth == 0:
                return header[open_at + 1 : i]
    return header[open_at + 1 :]  # never closes; use what there is


_FOR_INITIALIZER_RE = re.compile(r"^\s*(?:[A-Za-z_][\w:]*\s+)*([A-Za-z_]\w*)\s*=\s*(.+)$", re.S)


def _update_is_nondecreasing(name: str, update: str) -> bool:
    """True when a `for` header's update clause PROVABLY cannot decrease `name`.

    A WHITELIST, and that is the whole point. Until #849's round 7 this asked the
    opposite question - "is this a `--` or a `-=` on `name`?" - so every update it
    had not enumerated was accepted, and

        REQUIRE(v.size() == 2);
        for (int i = 0; i < v.size(); i += delta) { CHECK(v[i]); }

    was reported clean: at an actual length of 1 the assertion fails, execution
    continues, `delta == -1` re-enters with `i == -1`, and `v[i]` aborts the batch.
    `i = -1` was accepted for the same reason. An invariant that rests on a list of
    the bad cases is already broken; this one lists the provable cases instead, and
    everything outside the list - `i += delta`, `i = f()`, `i = -1`, `g(&i)`, a
    clause that merely mentions `name` - is unproven and does NOT bound (Codex,
    PR #849 round 7).

    Provable, given that the initialiser already established `name >= 0`:

    * `++name` / `name++`;
    * `name += <integer literal>` - the literal grammar carries no sign, so
      matching it IS the proof that the step is not negative;
    * `name = name + <integer literal>` and `name = <integer literal> + name`;
    * any clause that does not mention `name` at all, which therefore cannot
      write it (`name` is declared by the initialiser, so it is a local: only a
      clause naming it, or an alias made in the body, can reach it).
    """
    escaped = re.escape(name)
    mentions = re.compile(rf"(?<![\w.>]){escaped}(?![\w])")
    steps = re.compile(rf"^\s*(?:\+\+\s*{escaped}|{escaped}\s*\+\+)\s*$")
    adds = re.compile(rf"^\s*{escaped}\s*\+=\s*(.+)$", re.S)
    rebinds = re.compile(
        rf"^\s*{escaped}\s*=\s*(?:{escaped}\s*\+\s*(.+)|(.+?)\s*\+\s*{escaped})\s*$", re.S
    )
    for clause in _split_macro_arguments(update):
        if mentions.search(clause) is None:
            continue
        if steps.match(clause):
            continue
        added = adds.match(clause) or rebinds.match(clause)
        if added is None:
            return False
        operand = next((group for group in added.groups() if group is not None), None)
        if operand is None or _literal_value(operand) is None:
            return False
    return True


def _nonnegative_loop_indices(header: str) -> frozenset[str]:
    """Names a `for` header proves are at least zero when its condition is tested.

    `for (uint32_t i = 0; i < v.size(); i++)` DOES bound its body by `v`'s length:
    the condition is first evaluated with `i` at 0, so entering the body means the
    length is at least 1. A bare `if (i < v.size())` proves nothing of the sort -
    `size()` is signed here, so a negative `i` satisfies it on an EMPTY container -
    and that is the difference this set carries (Codex, PR #849 round 4).

    Limits, stated rather than hidden: the initialiser must be a nonnegative
    integer literal, and the update clause must be PROVABLY nondecreasing
    (`_update_is_nondecreasing` - a whitelist since round 7, not "is it a `--`"), so
    `for (int i = v.size() - 1; i >= 0; i--)` qualifies on neither count and
    `for (int i = 0; i < v.size(); i += delta)` qualifies on the second. A loop BODY
    that rebinds the variable no longer rides on the header's relation -
    `_bound_after` drops it at the rebinding statement (round 6) - so what is left
    unmodelled here is a rebinding this file's syntax cannot see: through a
    reference or a pointer, or inside a callee handed `&i`, and a rebinding carried
    over the loop's BACK EDGE (`for (int i = 0; i < v.size();) { CHECK(v[i]); i = -1; }`
    - the forward scan sees the subscript before the rebinding, and there is no
    second pass over the body).
    """
    head = _CONTROL_HEAD_RE.match(header)
    if head is None or head.group(1) != "for":
        return frozenset()
    inner = _control_operand(header, head.end())
    if inner is None:
        return frozenset()
    clauses = _split_top_level(inner, ";")
    if len(clauses) < 2:
        return frozenset()
    initializer = inner[clauses[0][0] : clauses[0][1]]
    increment = inner[clauses[2][0] : clauses[2][1]] if len(clauses) >= 3 else ""
    names: set[str] = set()
    for part in _split_macro_arguments(initializer):
        declaration = _FOR_INITIALIZER_RE.match(part)
        if declaration is None or _literal_value(declaration.group(2)) is None:
            continue
        name = declaration.group(1)
        if not _update_is_nondecreasing(name, increment):
            continue
        names.add(name)
    return frozenset(names)


def _bounds_iteration(symbol: str, header: str) -> _Bound:
    """What a control-flow header proves about subscripts of `symbol` in its BODY.

    DIRECTION-aware. Accepting any mention of the container's cardinality made the
    unsafe body of `if (v.is_empty()) { CHECK(v[0]); }` and
    `if (i >= v.size()) { CHECK(v[i]); }` invisible: both conditions select exactly
    the out-of-bounds case, and both were reported clean (Codex, PR #849 round 2).

    MAGNITUDE-aware since round 6: the answer is a `_Bound`, not a boolean, so
    `for (uint32_t i = 0; i < v.size(); ++i)` protects `v[i]` without also
    protecting `v[i + 1]`, and `if (!v.is_empty())` protects `v[0]` without also
    protecting `v[1]`.
    """
    return _expression_bound(
        symbol, _control_condition(header), _nonnegative_loop_indices(header)
    )


def _changes_length(symbol: str, statement: str) -> bool:
    """True when the statement can rebind `symbol` or change its length."""
    sym = _size_symbol_regex(symbol)
    if re.search(rf"(?<![\w.>)\]]){sym}\s*=(?!=)", statement):
        return True
    mutators = "|".join(_LENGTH_MUTATORS)
    return re.search(rf"(?<![\w)\]]){sym}\s*(?:\.|->)\s*(?:{mutators})\s*\(", statement) is not None


def _top_level_brace(statement: str) -> int:
    """Offset of the first `{` outside parentheses, or -1.

    Outside parentheses only, so the braces of `for (auto x : {1, 2})` and of a
    lambda passed as an argument are not mistaken for the body's.
    """
    depth = 0
    for i, ch in enumerate(statement):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "{" and depth <= 0:
            return i
    return -1


def _block_body_pieces(text: str) -> list[str]:
    """Split the text after a block's `{` into statements plus its closing `}`.

    Splits at `;` and at the `}` that closes the block, both at paren AND brace
    depth zero. Depth matters: an initializer list (`Vector<int> v = {1, 2};`), a
    lambda body and a nested block all carry braces that do not close this one, and
    treating any of them as the closer would unbalance the caller's stack.
    """
    pieces: list[str] = []
    parens = 0
    braces = 0
    start = 0
    for i, ch in enumerate(text):
        if ch == "(":
            parens += 1
        elif ch == ")":
            parens -= 1
        elif parens > 0:
            continue
        elif ch == "{":
            braces += 1
        elif ch == "}":
            if braces:
                braces -= 1
                continue
            piece = text[start:i].strip()
            if piece:
                pieces.append(piece)
            pieces.append("}")
            start = i + 1
        elif ch == ";" and not braces:
            piece = text[start : i + 1].strip()
            if piece:
                pieces.append(piece)
            start = i + 1
    tail = text[start:].strip()
    if tail:
        pieces.append(tail)
    return pieces


def _inline_pieces(statement: str) -> list[str]:
    """A control-flow statement whose BODY shares its line, split into pieces.

    `if (v.is_empty()) { CHECK(v[0]); }` is one statement to `_statements()`, and
    truncating it at `{` to get the header DISCARDED the body - so the branch that
    indexes precisely when the container is empty, the exact shape this detector
    exists to catch, was reported clean (Codex, PR #849 round 5). Emitting the
    header, the body's statements and the closing `}` as separate pieces lets the
    block stack handle a one-line body exactly as it handles a multi-line one: the
    header's bound guards the body and is popped at the `}` rather than leaking on
    to whatever follows.

    Only control flow is split. A bare `{` at statement level would otherwise catch
    every aggregate initializer, and pushing a frame for one would make the next `}`
    pop the wrong block.
    """
    if not _SIZE_CONTROL_FLOW_RE.match(statement):
        return [statement]
    brace = _top_level_brace(statement)
    if brace == -1 or not statement[brace + 1 :].strip():
        return [statement]
    pieces = [statement[: brace + 1].strip()]
    for piece in _block_body_pieces(statement[brace + 1 :]):
        pieces.extend(_inline_pieces(piece))
    return pieces


# A statement-level rebinding of a name: `i = 0`, `i += 2`, `i++`, `--i`. `==`,
# `!=`, `<=` and `>=` are excluded by the lookahead and by the operator class.
_REBIND_RE = re.compile(
    r"(?<![\w.>])([A-Za-z_]\w*)\s*(?:\+\+|--|(?:[-+*/%&|^]|<<|>>)?=(?!=))"
    r"|(?:\+\+|--)\s*([A-Za-z_]\w*)"
)
_IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")


def _bound_after(bound: _Bound, statement: str) -> _Bound:
    """`bound` with everything this statement can invalidate removed.

    A proven relation is a statement about VALUES, and the model compares index
    expressions by TEXT. The two agree only while the text still denotes the same
    value, so a statement that rebinds a name drops every `below` entry mentioning
    it - otherwise `for (uint32_t i = 0; i < v.size(); i++) { i += 5; CHECK(v[i]); }`
    would ride on a relation that stopped holding one statement earlier. A `lengths`
    peer goes the same way when the statement can rebind or resize THAT container:
    the indexed container's own mutators already stop the scan, but the peer's did
    not, and `b.push_back(x); CHECK(a[b.size() - 1]);` is out of bounds on `a`.

    `minimum` survives: it is a fact about the container, whose own mutators end the
    scan in `_first_unbounded_index`.
    """
    rebound = {match.group(1) or match.group(2) for match in _REBIND_RE.finditer(statement)}
    if not rebound and not bound.lengths:
        return bound
    return _Bound(
        bound.minimum,
        frozenset(
            entry for entry in bound.below
            if rebound.isdisjoint(_IDENTIFIER_RE.findall(entry))
        ),
        frozenset(
            peer for peer in bound.lengths
            if rebound.isdisjoint(_IDENTIFIER_RE.findall(peer))
            and not _changes_length(peer, statement)
        ),
    )


def _classify(own_bound: _Bound, other_bound: bool) -> str:
    """Which population a reported site belongs to.

    Checked in order of specificity. A site whose OWN container is bounded where it
    is indexed is round 6's population - the bound is real but too small for this
    subscript - and reporting it as bounded by another container would name the
    wrong container in the message a maintainer then goes to read.
    """
    if own_bound != _NO_BOUND:
        return _CLASS_UNDER_BOUND
    return _CLASS_OTHER_BOUND if other_bound else _CLASS_STRAIGHT_LINE


def _first_unbounded_index(
    symbol: str, following: list[tuple[int, str]]
) -> tuple[int, str, str] | None:
    """(line, statement, class) of the first index of `symbol` nothing bounds.

    A block STACK, not a stop-at-the-first-control-flow rule: a loop bounded by
    the container's own `size()` makes its BODY safe and nothing after it, so
    `for (i < a.size()) { a[i]; } CHECK(a[0]);` is still reported on `a[0]`.
    Each frame records two facts - what THIS container's length is proved to be
    (a `_Bound`, not a boolean), and whether ANY container's length was bounded -
    because only the first can make an index safe, while the second is what #844's
    sweep counted as loop-bounded and is reported separately so the two counts stay
    reconcilable.

    The enclosing frames' bounds are UNIONED: every enclosing condition holds at
    once, so `if (v.size() >= 2) { for (i < v.size()) { v[1]; v[i]; } }` proves
    both subscripts. What one frame proves it proves for the whole subtree, and a
    subscript is safe only if the union covers that specific index expression.

    A body that shares its header's line is expanded first (`_inline_pieces`), so
    the stack sees the same pieces whether or not the author used a line break. The
    expansion happens HERE and not at the call site so the caller's scan window
    still counts SOURCE statements: a one-line block must not consume three of the
    six statements this detector is allowed to look ahead.
    """
    stack: list[tuple[_Bound, bool]] = []
    pending: tuple[_Bound, bool] | None = None
    expanded = [
        (line_no, piece)
        for line_no, statement in following
        for piece in _inline_pieces(statement)
    ]
    for line_no, statement in expanded:
        if _SIZE_SCAN_STOP_RE.match(statement):
            return None
        if _RETURN_RE.match(statement) and not stack:
            return None
        if statement.lstrip().startswith("}") and stack:
            stack.pop()
        # Applied BEFORE the statement is judged, not after: `i += 5; v[i];` is two
        # statements, but `i = j, v[i]` is one, and the relation is already gone by
        # the time the subscript in it is evaluated.
        stack = [(_bound_after(bound, statement), other) for bound, other in stack]
        if pending:
            pending = (_bound_after(pending[0], statement), pending[1])
        own_bound = _NO_BOUND
        for frame in stack:
            own_bound = _bound_union(own_bound, frame[0])
        if pending:
            own_bound = _bound_union(own_bound, pending[0])
        other_bound = any(frame[1] for frame in stack) or bool(pending and pending[1])
        header = statement.split("{", 1)[0]
        opens_block = statement.rstrip().endswith("{")
        if _SIZE_CONTROL_FLOW_RE.match(statement):
            # A header's own bound guards its BODY, never itself: in
            # `while (v[i] && i < v.size())` the subscript is evaluated first. So
            # the header is judged against the ENCLOSING bound only.
            if _unguarded_index(symbol, header, own_bound):
                return line_no, header.strip(), _classify(own_bound, other_bound)
            frame = (
                _bound_union(own_bound, _bounds_iteration(symbol, header)),
                other_bound or _condition_lower_bounds(_control_condition(header)),
            )
            if opens_block:
                stack.append(frame)
                pending = None
            else:
                pending = frame  # brace-less body: applies to the next statement
            continue
        if _unguarded_index(symbol, statement, own_bound):
            return line_no, statement.strip(), _classify(own_bound, other_bound)
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
    # Every class is printed, including the ones currently at zero: a population
    # that only appears in the summary once it is non-empty is a population nobody
    # notices arriving.
    by_class = {
        klass: sum(1 for sites in found.values() for site in sites if site[6] == klass)
        for klass in (_CLASS_STRAIGHT_LINE, _CLASS_OTHER_BOUND, _CLASS_UNDER_BOUND)
    }
    split = ", ".join(f"{count} {klass}" for klass, count in by_class.items())
    summary = f"{total} baselined site(s) across {len(found)} file(s) ({split})"
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
