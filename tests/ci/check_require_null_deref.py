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

Precision over recall. A null-ish assertion on a symbol, followed - within a
short forward window - by a statement that **dereferences that same symbol**,
where nothing in between could have made the dereference safe.

* **Which macro counts as an assertion is DERIVED, not spelled here.** The
  accepted names come from `_doctest_assert_macros()`, so `CHECK`, `WARN`, the
  `_MESSAGE` / `_UNARY` / `_FALSE` / `_NE` forms, the `FAST_*` aliases and the
  `DOCTEST_*` spellings are all the same assertions as `REQUIRE`. Until #849's
  round 9 this detector spelled `REQUIRE` and its suffixes by hand, and `CHECK`
  is not the weaker case - it never aborts under ANY doctest configuration,
  where `REQUIRE` merely does not abort in THIS build. That hand-written
  spelling hid **18 real sites** across 7 corpus files; they are pre-existing,
  so they enter the baseline (319 -> 337) rather than being rewritten, which
  #656 rules out.

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
  reassignment of the symbol. Where those boundaries are does not depend on how
  the author laid the source out: the window is sliced to six SOURCE statements
  and each is then decomposed into atoms (`_statement_atoms`), so a closing brace
  that shares the last statement's line still ends the scan. It did not until
  #849's round 8, and the compacted spelling of a block reached a verdict its
  expanded spelling never reached. But it checks that statement's **header** before
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
* **What a macro NAME means is read out of doctest, not guessed from its
  spelling.** `_doctest_assert_macros()` parses `thirdparty/doctest/doctest.h`,
  where the `DOCTEST_CONFIG_EVALUATE_ASSERTS_EVEN_WHEN_DISABLED` block states each
  assertion's meaning as an expression (`[&] { return !(cond); }()`), and derives
  both the relation a name carries (`REQUIRE_EQ` -> `==`) and whether the macro
  NEGATES its predicate. A negating macro asserts the complement, so
  `REQUIRE_FALSE(v.size() == 0)` is the lower bound `size() != 0`, while
  `REQUIRE_FALSE(v.size())` and `REQUIRE_FALSE(!v.is_empty())` assert the
  container EMPTY and bound nothing. Both questions used to be answered by
  hand-written spelling rules and both were wrong: `macro.endswith("_FALSE")` is
  false for doctest's real `REQUIRE_FALSE_MESSAGE` / `CHECK_FALSE_MESSAGE`, which
  this corpus writes 37 times, and the relational suffix table consulted
  `*_EQ_MESSAGE`, which doctest does not define at all (#849 round 8). Deriving
  also fails CLOSED on a macro doctest does not define - a project-local
  `CHECK_SIZES_EQ(v.size(), 0)` no longer borrows `==` off its name and suppresses
  the index under it.
* **The `size()` call must not be an ARGUMENT to another call.**
  `REQUIRE(cpu_results.resize(ground_truth.size()) == OK)` constrains the *resize
  result*, not `ground_truth`, and is not a site; neither is `REQUIRE(a[v.size()])`.
  That is a question about what ENCLOSES the call, and it is asked once, in
  `_bound_direction`. It used to be asked a second time in `_size_assertions` as a
  parenthesis-DEPTH test, which cannot tell an argument from a grouping pair, so
  the harmless `REQUIRE((v.size() == 2))` was read as an assertion with no size
  predicate and the `v[0]` after it was silently not a site (#849 round 8).
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
  still flagged on the post-loop `a[0]`.
* **Layout does not change the verdict.** `_statements()` emits a line-oriented
  GROUP, which may hold a header and its whole body, a statement and a block
  delimiter, or several statements at once. Every group is decomposed into ATOMS
  (`_statement_atoms`) before either detector reads it, so a body sharing its
  header's line (`if (v.is_empty()) { CHECK(v[0]); }`, #849 round 5) and a closing
  brace sharing the body's last statement's line (`CHECK(v[0]); }`, #849 round 8)
  give the same answer as the fully expanded spelling. Both of those were silent
  over a live crash before they were fixed, and both were the same defect: a
  consumer re-parsing a group with its own prefix or suffix test. There is now one
  decomposition, shared by both detectors, and its totality is pinned as a
  PROPERTY rather than a list of layouts - decomposition is idempotent over every
  group the real corpus produces (420,814 atoms), so anything still splittable in
  what a consumer is handed fails a test rather than waiting for a review round.

  Round 10 extends the decomposition rather than the property: a BRACE-LESS body
  (`if (v.size() >= 2) consume(v);`) is split off its header too, so an atom never
  contains the body it guards. Before that split the atom was read as a header
  whose body is the NEXT atom, and its frame bounded a statement outside the
  branch - `REQUIRE(v.size() == 3); if (v.size() >= 2) consume(v); CHECK(v[1]);`
  reported nothing, though at a real length of 1 the assertion fails, the branch is
  skipped and `v[1]` aborts the batch. The same atom also carried its body into the
  header test, where a header's own bound deliberately does not apply, so the
  branch that IS guarded (`if (v.size() >= 2) CHECK(v[1]);`) was reported instead.
  One split fixes both directions and both detectors.

  Totality is about the PIECES, though, not about what each piece MEANS, and
  round 9 found the difference. `} while (v.size() >= 2);` atomises correctly,
  into `"}"` and `while (...);` - and the `while` was then read as a brace-less
  loop head whose condition bounded the NEXT statement, so
  `REQUIRE(v.size() == 3); do { … } while (v.size() >= 2); CHECK(v[1]);` reported
  nothing: at a real length of 1 the assertion fails, the loop exits, and `v[1]`
  kills the batch, suppressed by the bound of a body that does not exist. A
  control-flow atom with no body (`_guards_no_body`: a `do` terminator, or a
  deliberately empty `while (poll());`) now creates no frame at all. It is
  recognised by SHAPE rather than by finding the matching `do`, because the scan
  starts at the assertion and a `do` opened above it is not in the window.

  Rounds 8, 9 and 10 are three instances of ONE mechanism: a frame applied to a
  statement outside its scope, each time through `pending` - a one-slot lookahead
  meaning "the body of this header is the next atom". So what round 10 pins is the
  property `pending` needs rather than a fourth shape: **an atom that creates a
  `pending` frame holds no body of its own**, checked over every atom the corpus
  produces (`test_no_atom_that_creates_a_pending_frame_carries_its_own_body`).
  Under it "the body is the next atom" is a fact about the decomposition instead of
  a guess about layout, and the walker refuses to create the frame at all if that
  ever stops holding - a frame whose extent is unknown bounds nothing. What the
  property does NOT close is item 8 below.
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
7. ~~**An assertion that is not spelled `REQUIRE*` or `CHECK*`.**~~ **RETIRED in
   round 9.** The accepted head is now the DERIVED macro family (see
   `_assertion_vocabulary`), so doctest's `WARN*` family, the `FAST_*` aliases and
   the `DOCTEST_*` spellings are all scanned. Round 8 left this open on the
   evidence that the corpus contains zero doctest `WARN` assertions and that a
   `WARN\\w*` prefix could only mis-read Godot's `WARN_PRINT` - both still true,
   and both beside the point: the fix is not a wider prefix but an EXACT derived
   name set, under which `WARN` is an assertion and `WARN_PRINT` is not. It still
   adds no corpus site. What round 9 showed is that leaving it as a documented
   limit meant keeping a second, hand-written source of truth for a vocabulary
   this file already derives, and that second source was wrong in a direction
   nobody had measured: it also rejected `DOCTEST_REQUIRE`.

8. **A brace-less body that is not in the same ATOM as its header.** Round 10's
   split makes an atom carry no body of its own, so `pending` now means what it
   says - and the one way a frame can still reach a statement that is not its body
   is a header whose atom ends before the body starts, leaving the body to arrive
   from the NEXT group:

       if (v.size() >= 2)
           for (uint32_t i = 0; i < 3; i++)     // `_statements()` ends the group here
               CHECK(v[1]);                     // ... and the body is the next one

   `_statements()` ends a group at a depth-0 `;`, a trailing `{` or a trailing `}`,
   so a group ending on a bare header can only be a header CHAIN like this one, and
   the following group does begin with its body - but that is a property of the
   grouper, not something checked here, and it is the honest residual of this
   mechanism. **Measured: zero corpus groups end on a bare header** (117 files;
   every brace-less body in the corpus is inside its header's atom, where the split
   handles it). It is deliberately not asserted: the shape is valid C++ that the
   detector answers CORRECTLY today, and a ratchet that fails on correct input gets
   waived (see item 6). Follow-up under #844/#865 if it ever appears.

   Two smaller residuals of the same decomposition, both measured at zero and both
   pre-dating round 10: a control-flow header followed by an aggregate initializer
   (`if (c) v = {1, 2};`) splits at the initializer's `{` rather than at the header,
   which yields a stray `;` atom - the frame it opens is closed by the initializer's
   own `}`, so its extent is still right; and a line-spliced macro DEFINITION
   inside a test header (`test_macros.h`) is the one corpus group with text between
   a header and a top-level `{`.

Round 8 retired no item on this list, and that is worth saying plainly: all three
of its findings were shapes this section did not know it was missing - a macro
spelling, a grouping pair, a brace placement - not limits anyone had chosen. The
list is what the model KNOWS it cannot do; it has never been the boundary of what
the model gets wrong. Round 9 retires item 7 and refutes the reason it was left
standing. See "Convergence" below.

### Reconciliation of the count

#844's sweep of the corpus reported 60 size-shape sites, 14 loop-bounded, 46
dangerous, 4 fixed by #843 -> **42 remaining**. This detector reports **50**:
**43 straight-line**, **7 bounded only by another container's `size()`** and
**0 bounded below the index** (round 6's population - the right container, too
small a bound). All three figures are printed on every run, including the zero, so
that a population cannot arrive unremarked, and all three are pinned by a unit
test. Round 9 left all three unchanged: neither the `do` terminator nor the wider
macro vocabulary adds a site to THIS detector, which is the measured statement
that both of its shapes occur zero times in the corpus. Round 10 leaves all three
unchanged too, and detector 1's 337 as well - see the count under "Convergence".

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

## Convergence

Ten review rounds have landed on this one file, and **three** claims that the
remaining gap was bounded have now been refuted by the next round. So the state is
recorded here rather than asserted again. Sorting all ten rounds' findings by
WHERE they came from:

* **the inputs to the rules** - the text handed to a recogniser was not the
  statement it assumed: a `REQUIRE` split over lines and several compacted onto
  one (round 1), a body sharing its header's line and unspliced line
  continuations (round 5), a closing brace sharing the body's last statement's
  line and a grouping pair read as a nesting level (round 8), a brace-less body
  still inside its header's atom (round 10);
* **an external vocabulary spelled by hand** - a doctest macro name that does not
  exist and a real one that was missed (rounds 1 and 8), a relational suffix table
  naming a macro family doctest never defined (round 8), C++ integer-literal
  spellings the reader did not implement (round 7), a head regex that rejected
  doctest's own `DOCTEST_*` spellings before the derived family was consulted
  (round 9);
* **the semantic model itself** - the bound's DIRECTION (round 2), the asymmetry
  between what a guard may assume and what an assertion may (round 3), `size()`
  being signed (round 4), a bound on the container not being a bound on a specific
  subscript (round 6), a `for` update clause that must be proven nondecreasing
  (round 7), a `do` terminator read as a loop head (round 9).

**Round 8 claimed the first two categories had a structural answer, and predicted
round 9 would be semantic. Both of round 9's findings refute that.** The second
category's answer was not structural: the family WAS derived, for negation and
relation semantics - and a separate hard-coded regex still decided, before the
family was ever reached, which names got to be assertions at all. A derivation
that is not consulted at the decision point is not one source of truth, it is two.
There were in fact FOUR such spellings in the file, and looking for the third is
what found the largest defect of the round: detector 1's hand-spelled `REQUIRE`
heads had been hiding 18 real corpus sites, because `CHECK` - which never aborts
in any build - was not in them.

What is structural now, and what is not:

* statement SHAPE - one total decomposition shared by both detectors, its
  totality machine-checked over the corpus. Round 9 did not dent this. `} while
  (…);` decomposed correctly; the atoms were then MISREAD. Totality of the pieces
  is not totality of their meaning, and the property as stated never claimed to be.
  Round 10 DID dent it: a brace-less body was a piece the decomposition had never
  been asked to split, so the atom contained the body it guarded. The answer was to
  split it and to pin the property `pending` actually depends on - no atom that
  creates a `pending` frame carries its own body - beside the idempotence one;
* frame SCOPE - this is what rounds 8, 9 and 10 were each an instance of, and it is
  now two claims of different strength, deliberately not merged. Within an atom it
  is structural and machine-checked (above). ACROSS atoms it rests on `_statements()`
  ending a group at a `;`, a `{` or a `}`, which is an argument, not an assertion -
  see blind spot 8, measured at zero corpus occurrences. So the narrow question
  "can a frame still be attributed to a statement that is not in its scope?" has a
  narrow answer: not from an atom that contains its body, by a corpus-wide check;
  and from a group that ends on a bare header, only if one is ever written;
* macro VOCABULARY - now genuinely single-sourced: `_assertion_vocabulary()` is
  the only accepted-name test in the file, and a unit test parses this module's
  own AST to fail if a fifth hand-written spelling is ever added. That test is the
  actual structural answer; the derivation alone was not;
* the semantic MODEL - still no structural answer, still a textual approximation
  of a dataflow question with an open-ended rule set.

The prediction record is what it is: three convergence claims, three refutations.
So this section states no fourth prediction, and round 10 adds none - round 9
declined to predict and was not thereby made right, it was simply not made wrong.
Rounds 5, 8, 9 and 10 each found a defect in a mechanism the previous round had
just called closed, and the only pattern that has held across all ten is that the
file gets a finding whenever someone looks at it. What replaces a prediction here
is the pair of claims above: one checked over the corpus, one explicitly not.

What that is worth is bounded by what this guard IS. It is a **ratchet over a
frozen baseline**, not a proof that the corpus is safe - the docstring says so
above, and the 42 conversions are deliberately still open under #844. The
corpus-occurrence test is what separates the two kinds of round-9 finding, and it
separated them cleanly:

* the shapes fixed in rounds 5-8, and the `do` terminator of round 9, occur
  **zero** times in the corpus (measured: 9 `do`/`while` terminators, none with a
  cardinality condition; 0 `DOCTEST_*` invocations). Every one of those fixes has
  changed the reported set by exactly zero sites. They buy recall against code
  nobody has written yet;
* the hand-spelled `REQUIRE` heads in detector 1 occurred **18** times, and were
  worth the round on their own;
* round 10's shape is the first that does not sort cleanly into either bucket, and
  the numbers are worth stating rather than rounding to "zero". The SYNTAX is
  ordinary C++ and it is already here: **36 inline brace-less bodies, 24 distinct,
  across 10 of the 117 files** - `if (c) return;` alone is 11 of the 24. What is
  zero is the CO-OCCURRENCE: of 217 cardinality-assertion windows, **0** contain
  one, and of 780 null-ish windows, 6 do (all `if (c) return;`, all already
  answered the same way). So both baselines move by exactly zero sites, as in
  rounds 5-9 - but unlike those, this shape is one ordinary edit away from
  mattering rather than a spelling nobody uses, and un-bracing any single-statement
  body in the corpus is that edit. That is why it is fixed here rather than filed.

The disposition this file is landed under: further findings of this class are
triaged by whether the shape OCCURS in the corpus. If it does, it is a bug and it
gets fixed - round 9's 18 sites are what that rule looks like when it fires. If it
does not, it is a follow-up issue against #844 (tracked in #865) - together with
the limits already enumerated above, of which the window (blind spot 1, +3 real
sites at a window of 30) is now the one concrete remaining example, blind spot 7
having been retired here - and not a merge blocker.

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

The pattern predates the guard: **337 sites across 33 files** match it today. #656
is explicit that they must not be mass-rewritten, so
`require_null_deref_baseline.json` records a **fingerprint per site** and the
guard fails on any change to that set.

That number has moved once for a reason other than the corpus changing. Round 9
derived the accepted assertion-macro names instead of spelling them, and the
`CHECK`/`WARN` spellings of a null-ish assertion became visible: 319 -> 337, all
18 pre-existing, none new code, enumerated in the baseline diff of that commit
(`test_integration.cpp` 6, `test_gaussian_data.h` 4, `test_gaussian_splat_node.h`
2, `test_gaussian_splat_world_io.h` 2, `test_renderer_pipeline.h` 2,
`test_node_bootstrap.h` 1, `test_persistence_roundtrip.h` 1). A baseline that
grows because the GUARD got sharper is the only legitimate growth there is, and
it is called out here so it cannot be confused with the growth this file exists
to prevent.

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

Detector 1's baseline (`require_null_deref_baseline.json`) has the matching tool,
`--regenerate-null-deref-baseline`, added by GS-AUDIT-TEST-003; before that it was
hand-maintained (round 9's 319 -> 337 was a hand edit, reviewed as a diff).

Both baselines key each entry's fingerprint list by the source file's
**repo-relative POSIX path**, not its basename (GS-AUDIT-TEST-003). A basename key
collides when two files share a name in different directories, silently merging
one file's sites onto the other's, in the under-reporting direction. This tree
already has a same-named pair -- `modules/gaussian_splatting/tests/test_utils.h`
and `tests/test_utils.h` -- but it is NOT currently an active collision for this
guard specifically: `_test_sources()`'s `ENGINE_TESTS_DIR` glob is `test_*.cpp`
only, never `.h`, so `tests/test_utils.h` is never a member of the scanned set
here (it would collide with the module's copy if that glob were ever widened to
match `.h`, or for any future `tests/test_X.cpp` vs
`modules/gaussian_splatting/tests/test_X.cpp` pair, both of which the current
`.cpp`-only glob does admit). `check_environment_skip_marker.py`'s own baseline,
whose `_module_and_engine_sources()` DOES glob `.h` there, is where this exact
pair collides for real today, and its `source_key()` already carries this fix and
names this guard as the sibling still exposed to the (latent) hazard; `_site_key()`
ports that fix's mechanism, independent of whether today's scanned set happens to
exercise it.

## Review-base comparison

Both checks above compare the scan against the WORKING TREE's own copy of each
baseline file -- and a change that adds a violation AND appends its fingerprint to
that file, in the same commit, passes both: the scan and the baseline moved
together, so there is nothing to diff. `check_unchecked_resize.py` (#794/#798)
already carries the fix for this exact shape; this ports it rather than inventing a
new one.

`resolve_review_base()` resolves the review base the same way every other
base-anchored guard in this repo does: `--base-ref`, else `GS_CI_BASE_REF` /
`GITHUB_BASE_SHA` / `GITHUB_BASE_REF`, else (locally, when nothing is explicit)
`origin/master` then `master`. When it cannot resolve, the run **fails closed** --
it never falls back to grading the working tree against itself, which is the exact
defect this exists to remove.

Unlike the environment-skip and unchecked-resize guards, this one does NOT get
`--base-ref` threaded through `run_module_tests.py`'s own CLI override -- that
would mean editing `tests/ci/run_module_tests.py`, which this repo's risk policy
(`.agentic/policy.json`) classifies as R3 "CI deterministic-check / release-gate
machinery" by path alone, regardless of what the edit says. A `GS_CI_BASE_REF`
(etc.) **environment variable** still reaches this guard correctly, through plain
subprocess environment inheritance -- which is what CI and
`run_module_tests.py --guard-only` both use. The gap is narrow and deliberate:
`run_module_tests.py --guard-only --base-ref X` (the CLI override, as opposed to
the env var) will not reach this guard, unlike the other two. See the PR that
introduced this file's review-base comparison (GS-AUDIT-TEST-003) for why that
tradeoff was made instead of folding an R3 path into an R1 change.

For each baseline, the CURRENT SCAN (never the working-tree file) is compared
against that file's content **as committed at the review base**. A fingerprint
that already existed under the same key at the base is not new. Growth is
otherwise rejected outright, with one narrow, mechanically-provable exception: when
this script itself differs from its form at the review base (`detector_differs_from_base`
-- true for a real detector change, e.g. round 9, or for this PR's own basename ->
path rekey), a fingerprint that exists **anywhere** in the base baseline (any key,
consumed one-for-one) is recognized as pre-existing content that only looks new
under a changed key, not as a self-attested claim. A fingerprint that does not
exist anywhere at the base is new regardless, whatever else changed in the diff.

## Failing closed

A guard that cannot read or cannot parse must FAIL, never report "clean":

* a source file that cannot be read or decoded is a scan error, not an empty file;
* an assertion macro whose parentheses never balance within the continuation
  bound is a scan error, not an assertion with no size predicate;
* an unterminated raw string literal is a scan error;
* a vendored `doctest.h` that cannot be read, or that parses to a degenerate
  assertion-macro family - no negating macro, a missing relation, a `REQUIRE` that
  is not a plain assertion - is a scan error. "doctest has no `REQUIRE_FALSE`" is
  the answer that would quietly read every negated assertion as a positive one and
  unreport the sites under it, so it is refused rather than believed;
* a missing or malformed baseline is a failure, for both baselines;
* an empty source list is a failure.

Scan errors fail the run before any baseline comparison, because a partial scan
cannot tell "no new site" from "did not look".
"""

from __future__ import annotations

import bisect
import collections
import functools
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
MODULE_TESTS_DIR = ROOT / "modules" / "gaussian_splatting" / "tests"
ENGINE_TESTS_DIR = ROOT / "tests"
# The doctest this fork actually compiles against. Every question this file asks
# about an assertion macro - does it negate its predicate, does its NAME carry a
# relational operator - is answered from THIS file, never from a spelling rule.
DOCTEST_HEADER = ROOT / "thirdparty" / "doctest" / "doctest.h"

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

# Detector 1's regeneration tool (GS-AUDIT-TEST-003). Previously this baseline had NO
# regenerate flag at all and was maintained by hand; #656 explicitly tolerates that
# because a legitimate edit is rare and reviewable either way (see the note field: 18
# sites moved into the baseline in round 9, by hand, and the diff was the review). This
# PR needs a MECHANICAL, hand-edit-free way to re-key the file from basename to
# repo-relative path (`_site_key`, below) without asserting any fingerprint is safe or
# new, so a tool now exists, mirroring detector 2's shape and its "refuses to add" rule.
NULL_DEREF_REGENERATE_FLAG = "--regenerate-null-deref-baseline"
_NULL_DEREF_BASELINE_NOTE = (
    "Per-site fingerprints of pre-existing assert-then-dereference sites. Generated by "
    "tests/ci/check_require_null_deref.py --regenerate-null-deref-baseline (tool added by "
    "GS-AUDIT-TEST-003; this file was previously hand-maintained -- see git history for "
    "the pre-tool provenance). Entries may only be REMOVED (as sites are fixed), never "
    "added -- the ONE exception is the guard itself gaining recall over code that was "
    "already there, which is why this list grew from 319 to 337 in PR #849 round 9: the "
    "accepted assertion-macro names are now DERIVED from doctest's header, so the "
    "CHECK/WARN spellings of a null-ish assertion are seen at last. Those 18 sites are "
    "pre-existing and #656 rules out mass-rewriting them; a site added by NEW code still "
    "fails. Keyed by repo-relative POSIX path, not basename (GS-AUDIT-TEST-003): a "
    "basename key collides when two test files this guard's _test_sources() BOTH "
    "scan share a name in different directories (e.g. a hypothetical "
    "tests/test_x.cpp vs modules/gaussian_splatting/tests/test_x.cpp -- the "
    "ENGINE_TESTS_DIR glob here is test_*.cpp only, so this is a real hazard for a "
    ".cpp pair, not for the modules/.../test_utils.h vs tests/test_utils.h pair "
    "that motivated the fix, which this guard's glob happens not to scan both "
    "halves of), silently masking one file's sites under the other's."
)

# ---------------------------------------------------------------------------------
# Review-base comparison (GS-AUDIT-TEST-003).
#
# Everything above this point answers "does the committed baseline match the working
# tree scan?" -- and that question alone cannot see a change that adds a violation AND
# appends its fingerprint to the baseline in the SAME commit, because both sides moved
# together and the comparison is against itself. `check_unchecked_resize.py` (#794/#798)
# already carries the fix for exactly this shape: resolve the review base via
# GS_CI_BASE_REF (falling back to origin/master locally, exactly as
# check_environment_skip_marker.py's resolve_base_sha documents), and grade the
# WORKING-TREE SCAN against the baseline as it was COMMITTED AT THAT BASE, never
# against the working tree's own copy of the file. What follows ports that base
# resolution -- not a new one -- to this guard's two baselines.
#
# resolve_review_base() below is a near-verbatim mirror of check_unchecked_resize.py's
# function of the same name, not an import of it: the one genuinely shared piece is
# resolve_base_sha() in check_environment_skip_marker.py (three review rounds of
# GS_CI_BASE_REF/GITHUB_BASE_REF/origin-master precedence live there once), and both
# check_unchecked_resize.py and this file load it the same way -- dynamically, because
# tests/ci is not a package and every guard here runs standalone. Importing
# check_unchecked_resize.py's copy of the ~15-line wrapper instead would make this
# guard depend on ANOTHER guard's module surface (and its unrelated resize-specific
# argv/globals) for a function that is only a thin, mechanical call-through; mirroring
# the wrapper keeps the one load-bearing piece (resolve_base_sha) singular while
# keeping each guard's own file self-contained, which is how every guard in this
# directory is already structured (each is runnable standalone as `python check_*.py`).
BASE_RESOLVER_PATH = Path(__file__).resolve().parent / "check_environment_skip_marker.py"
# The baseline as committed at the review base did not exist there -- this change
# introduces it. A fact about history, distinct from "the base could not be resolved",
# which is a hard failure.
ABSENT_AT_BASE = "absent-at-base"


def _git(args: list[str]) -> tuple[int, str, str]:
    """Run git in ROOT. Mirrors check_unchecked_resize.py's `_git`, 3-tuple included:

    the stderr is what makes "git failed" and "git said no" distinguishable in the
    failure messages below, which is exactly the distinction _blob_at_base() depends
    on to avoid reading a broken git as an absent file.
    """
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
        return 1, "", str(exc)
    return result.returncode, result.stdout, result.stderr


def resolve_review_base(base_ref: str | None = None) -> tuple[str | None, list[str]]:
    """The immutable review base, delegated to the shared resolver.

    See the module-level comment above ABSENT_AT_BASE for why this is a mirror of
    check_unchecked_resize.py's function of the same name rather than an import of it.

    An import failure is a FAILURE, not a fallback: without a base there is nothing
    immutable to compare either baseline against, and "could not look" must never be
    reported as "found nothing".
    """
    try:
        spec = importlib.util.spec_from_file_location(
            "_gs_review_base_resolver_require_null_deref", BASE_RESOLVER_PATH
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


def _blob_at_base(base_sha: str, path: Path) -> tuple[str | None, list[str]]:
    """File content at the review base, or ABSENT_AT_BASE, or a failure.

    Absence is established with `ls-tree` BEFORE `show` is attempted, deliberately --
    mirrors check_unchecked_resize.py's function of the same name. `git show <sha>:<path>`
    exits non-zero both when the path is not in that tree and when git could not answer
    at all; conflating the two would let anything that breaks git (a corrupt object
    store, an unfetched base) read as "the file did not exist at the base", which passes
    the comparison and disables the ratchet silently, in exactly the conditions where
    nobody is looking. `ls-tree` separates them: it exits 0 with no output for a path
    that is genuinely absent, and non-zero when git failed.
    """
    try:
        rel = path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None, [f"{path} is outside {ROOT}; cannot locate it at the review base."]
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

    A baseline may grow relative to the base ONLY when it is not new content but
    content that was already there and only now surfaces under a changed key or a
    sharper detector (round 9's 319 -> 337 is exactly this; this PR's basename ->
    repo-relative-path rekey is another instance). Licensing that requires proof the
    detector genuinely changed here, not a self-report -- otherwise any PR could claim
    its new violation is "just a rename". `_baseline_growth_vs_base` below additionally
    requires the surfaced fingerprint to already be present in the base's baseline
    (content-hash identity, not a claim), which is the load-bearing check; this is the
    gate on top of it, mirroring check_unchecked_resize.py's function of the same name.
    """
    content, failures = _blob_at_base(base_sha, Path(__file__).resolve())
    if failures:
        return False, failures
    if content is ABSENT_AT_BASE:
        return True, []  # this change introduces the detector
    try:
        current = Path(__file__).resolve().read_text(encoding="utf-8")
    except OSError as exc:
        return False, [f"cannot read this script to compare it against the review base: {exc}"]
    return current != content, []


def _repo_path_for_key(name: str) -> Path:
    """The Path a scan key names -- the inverse of `_site_key`'s PRIMARY branch.

    `_site_key` computes `name` as `path.relative_to(ROOT).as_posix()` whenever the
    scanned path sits under ROOT, which is every real (non-fixture) invocation; this
    reconstructs that same Path directly. A monkeypatched ROOT (as some self-tests
    use) inverts correctly too, since it is the SAME ROOT `_site_key` used to derive
    the key. This does not need to invert `_site_key`'s tempdir FALLBACK branches
    (used only when the scan roots are not nested under ROOT): tests that exercise
    those route `name` through a stubbed `_rescan_base_content` instead of a real
    path and git, exactly as `resolve_review_base` / `_blob_at_base` are stubbed.
    """
    return ROOT / name


def _rescan_base_content(
    name: str, base_sha: str, scan_kind: str
) -> tuple[list[str] | None, list[str]]:
    """Fingerprints THIS (current) detector finds re-run over `name`'s content as it
    existed at the review base, or None if that file did not exist there.

    `scan_kind` selects which of the two detectors does the re-run: "null_deref"
    (`_scan_file` + `fingerprint`) or "size_index" (`_scan_file_size_index` +
    `size_index_fingerprint`) -- the two baselines are graded by the SAME function
    below, so the caller says which one this rescan is for.

    The base content is written to a REAL temporary file, named identically to the
    source (so any suffix- or name-dependent behaviour in the scanners -- error
    messages via `path.name`, `_size_assertions`'s `path.name` argument -- behaves
    exactly as an ordinary scan of that file would), rather than refactoring the
    scanners to accept text directly: reusing `_scan_file` / `_scan_file_size_index`
    unmodified means this rescan is provably the SAME code path a normal scan takes,
    not a second, drift-prone implementation of "what counts as a violation".
    """
    path = _repo_path_for_key(name)
    raw, failures = _blob_at_base(base_sha, path)
    if failures:
        return None, failures
    if raw is ABSENT_AT_BASE:
        return None, []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp) / path.name
        try:
            tmp_path.write_text(raw, encoding="utf-8")
        except OSError as exc:
            return None, [f"cannot stage '{name}' at the review base for rescanning: {exc}"]
        try:
            if scan_kind == "null_deref":
                violations = _scan_file(tmp_path)
                return sorted(
                    fingerprint(sym, form, stmt) for _, sym, form, stmt in violations
                ), []
            if scan_kind == "size_index":
                sites = _scan_file_size_index(tmp_path)
                return sorted(
                    size_index_fingerprint(sym, macro, assertion, stmt)
                    for _, sym, macro, assertion, _, stmt, _ in sites
                ), []
        except ScanError as exc:
            return None, [f"cannot rescan '{name}' at the review base {base_sha[:12]}: {exc}"]
    raise AssertionError(f"unknown scan_kind {scan_kind!r}")  # pragma: no cover


def _baseline_growth_vs_base(
    current: dict[str, list[str]],
    baseline_path: Path,
    base_sha: str,
    detector_differs: bool,
    scan_kind: str,
) -> tuple[dict[str, list[str]], list[str], bool]:
    """(new fingerprints per file relative to the base, failures, introduced-here).

    `current` is THIS RUN's scan (never the working-tree copy of `baseline_path`): the
    working-tree baseline is exactly what a joint mutation edits to agree with the
    scan, so comparing against it proves nothing. This compares the scan against the
    file's content as committed at the review base instead, which the mutation cannot
    reach.

    A fingerprint present under the SAME key in the base baseline is never new. One
    added under the current key is otherwise new UNLESS `detector_differs` is true
    (this script itself changed relative to the base -- see `detector_differs_from_base`)
    AND re-running THIS (current) detector over THAT SAME FILE's content AS IT EXISTED
    AT THE BASE finds that exact fingerprint there too (`_rescan_base_content`).

    That per-FILE rescan, not a cross-file/global pool, is deliberate -- an earlier
    version of this function drew from a flattened, repo-wide multiset of every
    fingerprint anywhere in the base baseline, and a review found it exploitable: fix
    a site in file A (removing its fingerprint's only base occurrence from nowhere in
    particular, since the pool was never tied to A), copy byte-identical code into
    file B, and B's "new" fingerprint matched something -- ANYTHING -- still sitting
    unclaimed in the global pool, even though B never contained that code at the base.
    Restricting the proof to "does B's OWN base-commit content contain this
    fingerprint" closes that: a genuinely new site, wherever it is copied from, was
    never in the file the guard is now asked to excuse it in. It still licenses a pure
    rekey (this PR's own basename -> path migration touches no C++ at all, so every
    file's base content and current content are byte-identical) and a genuine
    detector improvement over unchanged content (e.g. #849 round 9: the file's base
    content, rescanned with the WIDENED current detector, reveals the same
    previously-invisible site the live scan finds) -- both are exactly "this file, at
    the base, already contained what the live scan now reports", provable from git
    history rather than asserted.
    """
    raw, failures = _blob_at_base(base_sha, baseline_path)
    if failures:
        return {}, failures, False
    if raw is ABSENT_AT_BASE:
        return {}, [], True
    try:
        base_document = json.loads(raw)
        base_files_raw = base_document["files"]
        base_files = {
            str(name): [str(p) for p in prints] for name, prints in base_files_raw.items()
        }
    except (json.JSONDecodeError, KeyError, TypeError, AttributeError) as exc:
        return {}, [f"baseline at review base {base_sha[:12]} is unusable: {exc}"], False

    new_relative: dict[str, list[str]] = {}
    for name in sorted(current):
        added = _multiset_difference(current[name], base_files.get(name, []))
        if not added:
            continue
        if detector_differs:
            base_scan_prints, rescan_failures = _rescan_base_content(name, base_sha, scan_kind)
            if rescan_failures:
                return {}, rescan_failures, False
            if base_scan_prints is not None:
                # Recomputed against the FRESH RESCAN, not a further reduction of
                # `added`: `added` was already reduced by `base_files.get(name, [])`
                # (the COMMITTED baseline's recorded entries), so subtracting the
                # rescan from it too would let the base's supply cover the same
                # occurrences twice. current[3.h] = [FP,FP,FP] against a committed
                # baseline of [FP,FP] and a genuine base-content rescan of [FP,FP]
                # must find exactly ONE new copy, not zero -- reducing already-reduced
                # `added` against the rescan again would report zero.
                added = _multiset_difference(current[name], base_scan_prints)
        if added:
            new_relative[name] = added
    return new_relative, [], False


# How many statements to look ahead after the REQUIRE before giving up.
_SCAN_STATEMENTS = 6

# Every question of the form "is this NAME an assertion macro?" is answered by
# `_assertion_vocabulary()` below, from the family derived out of doctest's own
# header. There is deliberately no regex here spelling those names by hand: this
# file used to carry FOUR independent spellings of that one decision, and #849's
# round 9 found two of them wrong in opposite directions.
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

# The null-ish PREDICATE forms. The macro NAME in front of each of them is not
# spelled here; it comes from the derived family, in `_assertion_vocabulary()`.


class ScanError(Exception):
    """A source could not be read or lexed.

    Raised, never swallowed: a file the scanner cannot process is not a file with
    no violations. Callers collect these and FAIL the run before comparing
    anything to a baseline, because a partial scan cannot tell "no new site" from
    "did not look". Three separate guards in this repo have shipped that same
    fail-open hole; this one does not.
    """


# ---------------------------------------------------------------------------
# doctest's assertion-macro family, DERIVED from the vendored header
# ---------------------------------------------------------------------------
#
# Two questions this file has to answer about an assertion macro:
#
#   * does it assert its predicate FALSE (`REQUIRE_FALSE(v.is_empty())`)?
#   * does its NAME carry the relational operator (`REQUIRE_EQ(v.size(), 4)`)?
#
# Both were answered by hand-written spelling rules - `macro.endswith("_FALSE")`
# and a tuple of `("_EQ", "==")` suffixes - and both were wrong, in the way a
# hand-written list is always eventually wrong:
#
#   * `endswith("_FALSE")` is FALSE for the real `REQUIRE_FALSE_MESSAGE` and
#     `CHECK_FALSE_MESSAGE`, which the corpus writes 37 times. A negated
#     `is_empty()` under them established no bound, so the index after it was
#     silently not a site (Codex, PR #849 round 8);
#   * the suffix tuple was consulted as `_EQ` OR `_EQ_MESSAGE`, and doctest has
#     no `*_EQ_MESSAGE` macro at all - the same nonexistent-spelling mistake
#     round 1 made with `REQUIRE_FALSE_UNARY_FALSE`.
#
# So the family is not spelled out here. It is READ OUT of the header the fork
# compiles against, from the one block where doctest states each macro's meaning
# as an expression instead of a name: under
# `DOCTEST_CONFIG_EVALUATE_ASSERTS_EVEN_WHEN_DISABLED` every assertion is defined
# as a lambda returning its predicate's truth value, so
#
#     #define DOCTEST_REQUIRE_FALSE_MESSAGE(cond, ...) [&] { return !(cond); }()
#     #define DOCTEST_REQUIRE_EQ(...) [&] { return doctest::detail::eq(...); }()
#
# say "negating" and "==" outright. `DOCTEST_RELATIONAL_OP(eq, ==)` supplies the
# helper-name-to-operator mapping, the `#define DOCTEST_FAST_X DOCTEST_X` aliases
# are resolved, and the public short spellings come from the
# `#define X(...) DOCTEST_X(__VA_ARGS__)` block. Upgrading doctest re-derives.


class _MacroSemantics(NamedTuple):
    """What an assertion macro asserts about the expression handed to it.

    `relation` is the comparison the macro NAME carries (`""` when the predicate
    carries its own operator); `negated` is True when the macro asserts the
    predicate is FALSE.
    """

    relation: str
    negated: bool


_PLAIN_MACRO = _MacroSemantics("", False)

# `#define DOCTEST_<NAME>(args) [&] { return <body>; }()`
_DOCTEST_ASSERT_DEFINE_RE = re.compile(
    r"^#define\s+DOCTEST_(\w+)\s*\([^)]*\)\s*\[&\]\s*\{\s*return\s+(.*?);\s*\}\(\)\s*$", re.M
)
# `DOCTEST_RELATIONAL_OP(eq, ==)`
_DOCTEST_RELATIONAL_OP_RE = re.compile(r"^\s*DOCTEST_RELATIONAL_OP\(\s*(\w+)\s*,\s*(\S+?)\s*\)\s*$", re.M)
# `#define DOCTEST_FAST_WARN_EQ  DOCTEST_WARN_EQ`
_DOCTEST_ALIAS_RE = re.compile(r"^#define\s+DOCTEST_(\w+)\s+DOCTEST_(\w+)\s*$", re.M)
# `#define REQUIRE_EQ(...) DOCTEST_REQUIRE_EQ(__VA_ARGS__)`
_DOCTEST_SHORT_NAME_RE = re.compile(r"^#define\s+(\w+)\s*\([^)]*\)\s+DOCTEST_(\w+)\s*\(", re.M)
# The body of a relational assertion: `doctest::detail::eq(__VA_ARGS__)`.
_DOCTEST_RELATIONAL_BODY_RE = re.compile(r"^!?\s*\(?\s*doctest::detail::(\w+)\s*\(")


@functools.lru_cache(maxsize=1)
def _doctest_assert_macros() -> dict[str, _MacroSemantics]:
    """Public assertion-macro name -> semantics, read out of the vendored header.

    FAILS CLOSED. A header that cannot be read, or that parses to something
    degenerate, is a ScanError - not "doctest has no negating macros", which is
    the answer that would quietly turn every `REQUIRE_FALSE` into a positive
    assertion and unreport its sites. The degeneracy floor deliberately checks
    STRUCTURE (all six relations present, at least one negating macro, `REQUIRE`
    present and plain) rather than a count or an expected member list, so it
    survives a doctest upgrade that adds macros and still fails one that guts the
    block this derivation reads.
    """
    try:
        text = DOCTEST_HEADER.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ScanError(f"{DOCTEST_HEADER}: doctest header unreadable ({exc})") from exc

    operators = dict(_DOCTEST_RELATIONAL_OP_RE.findall(text))
    internal: dict[str, _MacroSemantics] = {}
    for name, body in _DOCTEST_ASSERT_DEFINE_RE.findall(text):
        body = body.strip()
        relation = ""
        call = _DOCTEST_RELATIONAL_BODY_RE.match(body)
        if call is not None:
            relation = operators.get(call.group(1), "")
        internal[name] = _MacroSemantics(relation, body.startswith("!"))
    # Object-like aliases (`DOCTEST_FAST_REQUIRE_UNARY_FALSE`). Iterated to a
    # fixed point so an alias of an alias resolves too.
    for _ in range(len(internal) + 1):
        added = False
        for alias, target in _DOCTEST_ALIAS_RE.findall(text):
            if target in internal and alias not in internal:
                internal[alias] = internal[target]
                added = True
        if not added:
            break

    public = {
        short: internal[long]
        for short, long in _DOCTEST_SHORT_NAME_RE.findall(text)
        if long in internal
    }
    # Both spellings answer to the same semantics: `DOCTEST_CONFIG_NO_SHORT_MACRO_NAMES`
    # would leave only the prefixed one, and a source may write either.
    macros = {f"DOCTEST_{name}": semantics for name, semantics in internal.items()}
    macros.update(public)

    missing = {"==", "!=", "<", ">", "<=", ">="} - {s.relation for s in macros.values()}
    if missing:
        raise ScanError(
            f"{DOCTEST_HEADER}: derived no macro for relation(s) {sorted(missing)} - "
            "the assertion-macro block this guard reads has moved or changed shape"
        )
    if not any(s.negated for s in macros.values()):
        raise ScanError(
            f"{DOCTEST_HEADER}: derived no negating assertion macro (REQUIRE_FALSE and "
            "friends) - the assertion-macro block this guard reads has moved or changed shape"
        )
    if macros.get("REQUIRE") != _PLAIN_MACRO:
        raise ScanError(
            f"{DOCTEST_HEADER}: derived REQUIRE as {macros.get('REQUIRE')!r}, expected a "
            "plain non-negating assertion - the derivation is reading the wrong block"
        )
    return macros


def _macro_semantics(macro: str) -> _MacroSemantics:
    """Semantics of `macro`, or the plain reading for a name doctest does not define.

    The corpus also writes project-local wrappers (`REQUIRE_GPU_DEVICE()`,
    `CHECK_PERFORMANCE(...)` in `test_macros.h`), which the size-assertion head
    admits because they start with `REQUIRE`/`CHECK`. They carry neither a
    relation nor a negation, so reading them as plain is both correct for them and
    the REPORTING direction for anything else unrecognised.
    """
    return _doctest_assert_macros().get(macro, _PLAIN_MACRO)


class _Vocabulary(NamedTuple):
    """Every place this file decides whether a NAME is an assertion macro.

    One object, one derivation. Before #849's round 9 this decision was made in
    FOUR places from four hand-written spellings, and the round-8 claim that the
    vocabulary class had a "structural answer" was false in exactly the way a
    second source of truth always is: the family WAS derived, and then a separate
    hard-coded head regex decided, before the family was ever consulted, which
    names got to reach it. Two of the four were measurably wrong:

    * `_SIZE_ASSERT_HEAD_RE` accepted only short names starting `REQUIRE`/`CHECK`,
      so `DOCTEST_REQUIRE(v.size() == 2); CHECK(v[0]);` produced no site although
      the prefixed spelling is the same macro (Codex, PR #849 round 9);
    * the null-ish predicate patterns spelled `REQUIRE` and its `_MESSAGE` /
      `_UNARY` / `_FALSE` / `_NE` suffixes by hand, so every `CHECK`, `WARN`,
      `FAST_*` and `DOCTEST_*` spelling of the same assertion was invisible to
      detector 1. That is not a hypothetical: it hid **18 real sites** in the
      corpus, and `CHECK` is the WORSE case - it never aborts under ANY doctest
      configuration, where `REQUIRE` merely does not abort in this build.

    The three members below are the only accepted-name tests in the file.
    """

    nullish: tuple[tuple[str, re.Pattern[str]], ...]
    scan_through: re.Pattern[str]
    size_head: re.Pattern[str]


def _macro_alternation(select: Callable[[_MacroSemantics], bool], what: str) -> str:
    """Regex alternation over the DERIVED macro names whose semantics `select` accepts.

    Longest name first. Python's alternation is leftmost-first, so `REQUIRE` would
    otherwise be tried before `REQUIRE_MESSAGE` on every pattern; the trailing
    `\\s*\\(` makes that recoverable by backtracking, but only by accident, and an
    ordering that does not depend on the tail is one less thing to be wrong about.

    An EMPTY selection is a ScanError. Deriving no negating macro, or no `!=`
    macro, is the answer that would silently stop recognising a whole family and
    unreport every site under it - the same fail-open direction the degeneracy
    floor in `_doctest_assert_macros()` refuses.
    """
    names = sorted(
        (name for name, semantics in _doctest_assert_macros().items() if select(semantics)),
        key=lambda name: (-len(name), name),
    )
    if not names:
        raise ScanError(
            f"{DOCTEST_HEADER}: derived no {what} assertion macro - the assertion-macro "
            "block this guard reads has moved or changed shape"
        )
    return "|".join(re.escape(name) for name in names)


@functools.lru_cache(maxsize=1)
def _assertion_vocabulary() -> _Vocabulary:
    """The accepted assertion-macro names, built from the derived family.

    The three semantic buckets map one-to-one onto the three null-ish predicate
    shapes, so the macro half of each pattern is DERIVED and only the predicate
    half is written here:

    * PLAIN - asserts its argument true (`REQUIRE`, `CHECK_MESSAGE`,
      `REQUIRE_UNARY`, `WARN`, `FAST_CHECK_UNARY`, ...): `ptr != nullptr` and
      `ref.is_valid()` are non-null claims under it;
    * NEGATING - asserts its argument false (`REQUIRE_FALSE`,
      `CHECK_FALSE_MESSAGE`, `REQUIRE_UNARY_FALSE`, ...): `ref.is_null()` under it
      is the non-null claim;
    * `!=` - the relation is in the NAME (`REQUIRE_NE`, `CHECK_NE`, ...), so the
      arguments are compared against `nullptr` positionally.

    A relational macro that is NOT `!=` is in none of them, and must not be:
    `CHECK_EQ(ptr, nullptr)` asserts the pointer IS null.
    """
    plain = _macro_alternation(lambda s: not s.negated and not s.relation, "plain")
    negating = _macro_alternation(lambda s: s.negated, "negating")
    not_equal = _macro_alternation(lambda s: s.relation == "!=" and not s.negated, "`!=`")
    any_assertion = _macro_alternation(lambda s: True, "any")
    return _Vocabulary(
        nullish=(
            ("!= nullptr", re.compile(rf"^\s*(?:{plain})\s*\(\s*({_SYMBOL})\s*!=\s*nullptr\b")),
            ("!= NULL", re.compile(rf"^\s*(?:{plain})\s*\(\s*({_SYMBOL})\s*!=\s*NULL\b")),
            ("nullptr !=", re.compile(rf"^\s*(?:{plain})\s*\(\s*nullptr\s*!=\s*({_SYMBOL})\b")),
            ("is_valid()", re.compile(rf"^\s*(?:{plain})\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_valid\s*\(\s*\)")),
            ("!is_null()", re.compile(rf"^\s*(?:{negating})\s*\(\s*({_SYMBOL})\s*(?:\.|->)\s*is_null\s*\(\s*\)")),
            ("_NE nullptr", re.compile(rf"^\s*(?:{not_equal})\s*\(\s*({_SYMBOL})\s*,\s*nullptr\s*\)")),
            ("_NE nullptr", re.compile(rf"^\s*(?:{not_equal})\s*\(\s*nullptr\s*,\s*({_SYMBOL})\s*\)")),
        ),
        # Scanned THROUGH: an assertion does not change reachability. The legacy
        # alternation is kept as a UNION member rather than replaced, because
        # `INFO`, `MESSAGE` and `CAPTURE` are doctest context macros that the
        # assertion derivation does not (and should not) produce, and because
        # project-local `REQUIRE_*`/`CHECK_*` wrappers must keep scanning through.
        # Widening this only ever lets the scan continue, which is the REPORTING
        # direction.
        scan_through=re.compile(
            rf"^\s*(?:(?:{any_assertion})|(?:REQUIRE|CHECK|WARN|INFO|MESSAGE|CAPTURE)\w*)\s*\(",
            re.IGNORECASE,
        ),
        # Detector 2's head. Union again: the derived family adds the `DOCTEST_*`,
        # `WARN*` and `FAST_*` spellings, and the prefix branch keeps the
        # project-local wrappers (`CHECK_PERFORMANCE`, `REQUIRE_GPU_DEVICE`) that
        # `_macro_semantics()` then reads as plain. Because the derived half is an
        # EXACT name set, `WARN_PRINT` - Godot's, not doctest's - is still not an
        # assertion, which a `WARN\w*` prefix could not have expressed.
        size_head=re.compile(
            rf"^\s*((?:{any_assertion})|(?:DOCTEST_)?(?:REQUIRE|CHECK)\w*)\s*\("
        ),
    )


# `!` applied to a relation. Used to fold a negating macro into the operator, so
# `REQUIRE_FALSE(v.size() == 0)` is judged as the `size() != 0` it asserts.
_NEGATED_RELATION = {"==": "!=", "!=": "==", "<": ">=", ">=": "<", ">": "<=", "<=": ">"}


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
            for form_name, pattern in _assertion_vocabulary().nullish:
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

    The window is sliced to `_SCAN_STATEMENTS` SOURCE statements first and each of
    those is then decomposed into atoms, so this loop never has to ask whether a
    piece of text is really one statement. That matters for the same reason it
    matters in `_first_unbounded_index`: `_statements()` emits a line-oriented
    group, and `_CONTROL_FLOW_RE` is a PREFIX test, so a group whose block close
    trails its last statement (`foo(); }`) did not read as a block boundary and the
    scan walked out of the block into an enclosing scope. Both consumers now share
    one decomposition (`_statement_atoms`) rather than each carrying its own
    re-parse - the shape that produced findings in rounds 5 and 8.
    """
    scanned = [
        (line_no, atom)
        for line_no, statement in following[:_SCAN_STATEMENTS]
        for atom in _statement_atoms(statement)
    ]
    for _stmt_line, statement in scanned:
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
        if _assertion_vocabulary().scan_through.match(statement):
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

# The accepted assertion-macro NAME is `_assertion_vocabulary().size_head`, derived
# from doctest's own header. CHECK is not the weaker case here: it never aborts
# under ANY doctest configuration, so it is strictly worse than a REQUIRE that
# merely does not abort in THIS build. One of the four sites #843 fixed was a
# CHECK.
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
# A relational comparison macro carries the operator in its NAME - and which
# operator that is comes from `_doctest_assert_macros()`, not from a suffix table
# here. The table this replaced also matched `*_EQ_MESSAGE`, a macro doctest does
# not define.
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

    `macro` is empty for plain expressions; only an assertion carries meaning in its
    NAME, and what that name means is READ OUT of the vendored doctest header
    (`_doctest_assert_macros`), never guessed from its spelling. Two things it can
    carry, and both are folded into the operator here so the rest of the function
    judges one relation:

    * the relation itself - `REQUIRE_EQ(v.size(), 4)`;
    * a NEGATION - `REQUIRE_FALSE`, `REQUIRE_FALSE_MESSAGE`, `REQUIRE_UNARY_FALSE`
      and the `CHECK`/`WARN`/`FAST_` spellings of each. The predicate is asserted
      false, so the relation is complemented (`REQUIRE_FALSE(v.size() == 0)`
      asserts `size() != 0`, a lower bound; `REQUIRE_FALSE(v.size() >= 3)` asserts
      `size() < 3`, which is not). Before round 8 negation was a
      `macro.endswith("_FALSE")` test consulted for `is_empty()` alone: it missed
      the `_MESSAGE` spellings the corpus writes 37 times, and it read
      `REQUIRE_FALSE(v.size())` - which asserts the container EMPTY - as a lower
      bound (Codex, PR #849 round 8).

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
    semantics = _macro_semantics(macro)
    before = text[lo:start]
    # Any `)` immediately after the call closes a group opened BEFORE the symbol,
    # so the relation of `(v.size()) == 0` sits past it. Not peeling them read the
    # expression as a bare truthiness test with the wrong answer.
    after = re.sub(r"^[\s)]+", " ", text[end:hi])
    if kind in ("is_empty", "empty"):
        # `!v.is_empty()`, `REQUIRE_FALSE(v.is_empty())`, `REQUIRE_UNARY_FALSE(...)`,
        # `CHECK_FALSE_MESSAGE(v.is_empty(), "...")`. Two independent negations, so
        # they XOR: `REQUIRE_FALSE(!v.is_empty())` asserts the container EMPTY and
        # bounds nothing below.
        return bool(re.search(r"!\s*$", before)) != semantics.negated

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
            # (`REQUIRE_EQ(v.size(), 4)`), which the header states outright.
            operator = semantics.relation
            if not operator:
                # No relation anywhere: the call stands alone as a truthiness test.
                # `if (v.size()) { v[0]; }` and `REQUIRE(v.size())` both bound the
                # length below - but `REQUIRE_FALSE(v.size())` asserts the container
                # EMPTY and bounds nothing. (`is_empty()` already returned above -
                # untested, it is the WRONG direction.)
                return not semantics.negated
            arguments = _split_macro_arguments(text[lo:hi])
            if len(arguments) < 2:
                return False
            first_argument_end = lo + len(arguments[0])
            flipped = start >= first_argument_end
            other = arguments[0] if flipped else arguments[1]
    other = other.strip()
    if flipped:
        operator = {"<": ">", ">": "<", "<=": ">=", ">=": "<="}.get(operator, operator)
    if semantics.negated:
        # A negating macro asserts the COMPLEMENT of what is written. Complement and
        # operand-order flip commute, so applying them in either order is the same
        # relation.
        operator = _NEGATED_RELATION.get(operator, operator)
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

    A cardinality call nested as an ARGUMENT to another call constrains that call's
    result, not the container - `REQUIRE(cpu_results.resize(ground_truth.size()) == OK)`
    says nothing about `ground_truth`. That rule lives in `_bound_direction`
    (`_is_call_argument`), which is where every caller's direction question is
    already answered, and is deliberately NOT duplicated here.

    Until round 8 it WAS duplicated here, as a parenthesis-DEPTH test, and the two
    did not mean the same thing: a depth test cannot tell an argument from a
    grouping pair, so the harmless `REQUIRE((v.size() == 2))` was read as an
    assertion with no size predicate and the `v[0]` after it was not a site
    (Codex, PR #849 round 8).
    """
    head = _assertion_vocabulary().size_head.match(fragment)
    if head is None:
        return []
    macro = head.group(1)
    span = _macro_argument_span(fragment, name)
    lo, hi = span
    found: list[tuple[str, str]] = []
    seen: set[str] = set()
    for symbol, kind, start, end in _cardinality_calls(fragment, lo, hi, name, strict=True):
        if symbol in seen:
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


def _guards_no_body(statement: str) -> bool:
    """True when this control-flow atom has NO body for its condition to guard.

    Two shapes reach here, and pushing a frame for either is fail-OPEN:

    * the TERMINATOR of a `do` loop - `} while (v.size() >= 2);`. Atomisation
      splits that into `"}"` and `while (...);`, and the `while` was then read as
      a brace-less loop HEAD whose bound was carried into the next statement. So

          REQUIRE(v.size() == 3);
          do { ... } while (v.size() >= 2);
          CHECK(v[1]);

      reported NO site: at an actual size of 1 the assertion fails, the loop
      exits, and `v[1]` kills the batch - while the nonexistent `while` body's
      bound suppressed exactly that subscript (Codex, PR #849 round 9);
    * a deliberately EMPTY body - `while (poll());`, `for (init; cond; step);`.
      There is nothing after the header for the condition to hold over there
      either.

    Recognised by SHAPE, not by finding the matching `do`. The scan window starts
    at the assertion, so a `do` opened ABOVE it is not visible, and a rule that
    needed to see it would still be fail-open for the case that matters most - an
    assertion inside the loop body. A genuine brace-less loop is `while (c) stmt;`
    (text after the condition) or `while (c)` with its body in a later atom
    (nothing after the condition). Neither is a lone `;`.
    """
    head = _CONTROL_HEAD_RE.match(statement)
    if head is None or head.group(1) == "do":
        return False
    open_at = statement.find("(", head.end())
    if open_at < 0:
        return False
    depth = 0
    for i in range(open_at, len(statement)):
        if statement[i] == "(":
            depth += 1
        elif statement[i] == ")":
            depth -= 1
            if depth == 0:
                return statement[i + 1 :].strip() == ";"
    return False


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


# ---------------------------------------------------------------------------
# Statement decomposition - SHARED by both detectors
# ---------------------------------------------------------------------------
#
# `_top_level_brace` / `_block_body_pieces` / `_inline_pieces` / `_statement_atoms`
# are not detector-2 machinery even though they live in its section: `_scan_forward`
# (detector 1) runs its scan window through `_statement_atoms` too. They are the
# one place a group emitted by `_statements()` is turned into genuine statements,
# so neither detector re-parses one with a prefix or suffix test of its own.


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
    """Split `text` at each depth-0 `;` and at each `}` that closes an OUTER block.

    Splits at paren AND brace depth zero. Depth matters: an initializer list
    (`Vector<int> v = {1, 2};`), a lambda body and a nested block all carry braces
    that do not close an enclosing one, and treating any of them as a closer would
    unbalance the caller's stack. An UNMATCHED `}` - one with no `{` before it in
    this text - is exactly a close of the block the caller is inside, and is
    emitted as its own piece.
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


_BARE_ELSE_RE = re.compile(r"\s*(?:\}\s*)?else\b")


def _header_end(statement: str) -> int | None:
    """Offset just past a control-flow header, i.e. where its BODY would start.

    None only when `statement` is not a control-flow head at all. The two regexes
    consulted here cover, between them, every keyword `_SIZE_CONTROL_FLOW_RE`
    accepts - `_CONTROL_HEAD_RE` takes `if`/`for`/`while`/`switch`/`do` (and
    `else if`), `_BARE_ELSE_RE` takes a lone `else` - and a test derives that
    keyword set from `_SIZE_CONTROL_FLOW_RE` itself, so a keyword added there
    without a rule here fails rather than silently keeping its body glued on.

    `do` and a lone `else` carry no parenthesised condition, so their header is
    the keyword. A condition that never closes inside this text returns the END of
    the text: there is then no body HERE to split off, and claiming one would be
    inventing a scope out of a truncated statement.
    """
    head = _CONTROL_HEAD_RE.match(statement)
    if head is None:
        bare = _BARE_ELSE_RE.match(statement)
        return bare.end() if bare else None
    if head.group(1) == "do":
        return head.end()
    open_at = statement.find("(", head.end())
    if open_at < 0:
        return head.end()
    depth = 0
    for i in range(open_at, len(statement)):
        if statement[i] == "(":
            depth += 1
        elif statement[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(statement)


def _split_header(statement: str) -> tuple[str, str] | None:
    """(header, brace-less body) when a control-flow atom carries its own body.

    None when it does not: a header that owns no body text (`if (c)`, whose body is
    a later statement), a non-header, and a lone `;` (`_guards_no_body`: there is
    nothing to run). ONE function answers both "does this atom contain its body"
    for the walker and "where do I cut" for the decomposition, so the two cannot
    drift into disagreeing about the same atom.
    """
    end = _header_end(statement)
    if end is None:
        return None
    body = statement[end:].strip()
    if not body or body == ";":
        return None
    return statement[:end].strip(), body


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

    A BRACE-LESS body is split off the same way (round 10). `if (v.size() >= 2)
    consume(v);` reached `_first_unbounded_index` as ONE atom, which read it as a
    header whose body is the NEXT atom - so the condition's frame was stored in
    `pending` and bounded a statement outside the branch, and
    `REQUIRE(v.size() == 3); if (v.size() >= 2) consume(v); CHECK(v[1]);` reported
    nothing: at a real length of 1 the assertion fails, the branch is SKIPPED, and
    `v[1]` aborts the batch (Codex, PR #849 round 10). The same atom also carried
    its body into the header test, so the branch that IS guarded
    (`if (v.size() >= 2) CHECK(v[1]);`) was reported instead. Both directions are
    the one defect - an atom that contains its own body - and splitting here fixes
    both, for both detectors, because there is one decomposition.

    No synthetic `}` is emitted for the brace-less split, and none is needed: a
    brace-less body is exactly ONE statement, so the frame `pending` holds expires
    on it by the rule that already ends `pending` after every non-header atom. The
    invariant this establishes is what makes `pending` sound - after this split, no
    atom that creates a `pending` frame contains its own body, so "the body is the
    next atom" is a fact about the decomposition rather than a guess about layout.

    Only control flow is split HERE. A bare `{` at statement level would otherwise
    catch every aggregate initializer, and pushing a frame for one would make the
    next `}` pop the wrong block. Delimiters that are unambiguous - a depth-0 `;`,
    an unmatched `}` - are split off by `_statement_atoms` before this runs.
    """
    if not _SIZE_CONTROL_FLOW_RE.match(statement):
        return [statement]
    brace = _top_level_brace(statement)
    if brace != -1:
        if not statement[brace + 1 :].strip():
            return [statement]
        return [statement[: brace + 1].strip(), *_statement_atoms(statement[brace + 1 :])]
    split = _split_header(statement)
    if split is None:
        return [statement]
    return [split[0], *_statement_atoms(split[1])]


def _statement_atoms(text: str) -> list[str]:
    """`text` decomposed into GENUINE statements and block delimiters.

    TOTAL, by construction: whatever `_statements()` or `_line_fragments()` hands
    over, every atom out of here is exactly one of

    * `"}"` - the close of a block the scan is inside;
    * a control-flow HEADER, ending in `{` when it opens a block;
    * one simple statement.

    That totality is the point, and it is why this exists rather than a fourth
    special case in `_first_unbounded_index`. `_statements()` emits a line-oriented
    GROUP, not a statement: it ends a group at a `;`, a trailing `{` or a trailing
    `}`, so a group can carry a statement AND a delimiter (`CHECK(v[0]); }`), a
    header AND its whole body (`if (c) { CHECK(v[0]); }`), or several statements
    compacted onto one line. Each consumer that re-parsed a group with its own
    prefix/suffix heuristic got one shape wrong and stayed silent over a live
    crash:

    * round 5 - a header whose body shares its line: the body was truncated away
      at `{`, so `if (v.is_empty()) { CHECK(v[0]); }` was reported clean;
    * round 8 - a body's last statement sharing a line with the closing brace:
      `CHECK(v[0]); }` does not START with `}`, so the block frame was never
      popped and its bound leaked onto every statement after the block
      (Codex, PR #849).

    The group is NOT split inside `_statements()`, which would be the other place
    to fix this, because `limit` there counts GROUPS and the caller slices the
    merged same-line-plus-following list to the same number: emitting atoms would
    silently shrink the lookahead window, which is the fail-OPEN direction (sites
    vanish rather than appear). Splitting here keeps the window counting source
    statements exactly as documented.
    """
    atoms: list[str] = []
    for piece in _block_body_pieces(text):
        atoms.extend(_inline_pieces(piece))
    return atoms


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

    Each scanned statement is decomposed into ATOMS first (`_statement_atoms`), so
    the stack sees the same pieces however the author laid the source out: a body
    on its header's line, a closing brace on the body's last statement's line, and
    several statements compacted onto one line all reduce to the same atoms as the
    fully expanded spelling. The prefix and suffix tests below are then reading an
    atom, where they are exact - a block close IS the atom `"}"` and a block open
    IS an atom ending in `{` - instead of reading a group, where each of them has
    now been wrong about one layout. The decomposition happens HERE and not at the
    call site so the caller's scan window still counts SOURCE statements: a
    one-line block must not consume three of the six statements this detector is
    allowed to look ahead.
    """
    stack: list[tuple[_Bound, bool]] = []
    pending: tuple[_Bound, bool] | None = None
    expanded = [
        (line_no, atom)
        for line_no, statement in following
        for atom in _statement_atoms(statement)
    ]
    for line_no, statement in expanded:
        if _SIZE_SCAN_STOP_RE.match(statement):
            return None
        # `and not pending`: a `return` reached while a brace-less frame is pending
        # IS that frame's conditional body (`if (skip()) return;`), exactly as a
        # `return` inside braces is the block's - and a CONDITIONAL return does not
        # end the scan any more than `if (skip()) { return; }` does. Without this,
        # round 10's split would have turned eleven corpus `if (c) return;` lines
        # into unconditional scan stops, which is the fail-OPEN direction.
        if _RETURN_RE.match(statement) and not stack and not pending:
            return None
        # Atomisation guarantees a block close arrives as the atom `"}"` on its own.
        # The test stays a PREFIX one anyway, because the two readings differ only
        # if an unsplit group ever reaches here, and then `} foo();` must still pop:
        # not popping leaks a bound that has ended, which is the fail-OPEN direction.
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
            if _guards_no_body(statement):
                # A `do` terminator or an empty body: the condition guards nothing
                # that FOLLOWS it, so no frame is created and any pending one has
                # just expired on this statement.
                pending = None
                continue
            frame = (
                _bound_union(own_bound, _bounds_iteration(symbol, header)),
                other_bound or _condition_lower_bounds(_control_condition(header)),
            )
            if opens_block:
                stack.append(frame)
                pending = None
            elif _split_header(statement) is not None:
                # This atom carries its own body, so "the body is the next atom" is
                # false for it. `_statement_atoms` splits every such atom (round 10),
                # so nothing it produces reaches here - but if that ever regresses,
                # the frame's extent is unknown and the fail-CLOSED answer is to bound
                # nothing rather than to bound the following statement. Pinned by
                # `test_the_walker_fails_closed_if_the_decomposition_regresses`.
                pending = None
            else:
                # The body really is the NEXT atom, because this one holds no body.
                pending = frame
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


def _site_key(path: Path) -> str:
    """The baseline key for a source: its repo-relative POSIX path, not its basename.

    GS-AUDIT-TEST-003: a basename key collides whenever two SCANNED files share a
    name in different directories, silently masking one file's sites under the
    other's -- the wrong direction for a guard against a test-binary-killing defect
    (#656). `modules/gaussian_splatting/tests/test_utils.h` and `tests/test_utils.h`
    both exist in this tree and motivated the fix, but this guard's own
    `_test_sources()` globs `ENGINE_TESTS_DIR` for `test_*.cpp` only (never `.h`),
    so that specific pair is not an ACTIVE collision here today; the hazard is real
    for any `.cpp` pair sharing a name across the two directories, present or
    future, and for the `.h` pair too the day this glob (or a future guard reusing
    this key function) widens to match it.
    check_environment_skip_marker.py's `source_key()` -- whose OWN
    `_module_and_engine_sources()` globs `.h` on both sides, so that exact pair DOES
    collide for it today -- already carries this fix and names this guard as the
    sibling still exposed to the hazard; this mirrors it, including its fallback
    chain, so the self-tests below -- which point `_test_sources()` at directories
    outside ROOT via monkeypatched MODULE_TESTS_DIR/ENGINE_TESTS_DIR -- get stable
    keys instead of a ValueError.
    """
    resolved = path.resolve()
    for base in (ROOT, MODULE_TESTS_DIR, ENGINE_TESTS_DIR):
        try:
            return resolved.relative_to(Path(base).resolve()).as_posix()
        except ValueError:
            continue
    return path.name


def scan_all_size_index() -> tuple[dict[str, list[tuple[int, str, str, str, int, str, str]]], list[str]]:
    """(repo-relative path -> size-then-index sites, scan errors). Errors are never violations."""
    results: dict[str, list[tuple[int, str, str, str, int, str, str]]] = {}
    errors: list[str] = []
    for path in _test_sources():
        try:
            sites = _scan_file_size_index(path)
        except ScanError as exc:
            errors.append(str(exc))
            continue
        if sites:
            results[_site_key(path)] = sites
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
    """Repo-relative path -> violations, for every test source that has any."""
    results: dict[str, list[tuple[int, str, str, str]]] = {}
    for path in _test_sources():
        violations = _scan_file(path)
        if violations:
            results[_site_key(path)] = violations
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


def _refused_flattened_additions(
    found_prints: dict[str, list[str]], baseline: dict[str, list[str]]
) -> list[str]:
    """Fingerprints regeneration would write that are not present ANYWHERE in the
    existing baseline, regardless of which key they sit under.

    GS-AUDIT-TEST-003 re-keys both baselines from basename to repo-relative path
    (`_site_key`), so EVERY entry's dict key changes in that one commit. A per-key
    "is this fingerprint already recorded under this exact key" comparison would then
    see the whole (unchanged) baseline as new, because nothing survives under its old
    key by construction of the rename. A fingerprint is a sha1 of the full statement
    text (see `fingerprint`), so a flattened, cross-key comparison is still a precise
    identity check -- what a rename preserves -- rather than a looser one. It is only
    looser than a per-key check for one corner case, identical code duplicated
    verbatim into a different file in the same regeneration run, which a human
    reviewing the regenerated JSON diff still sees; this tool is never run by CI (see
    `main()`), only by a person preparing a change.
    """
    existing_flat = collections.Counter(fp for prints in baseline.values() for fp in prints)
    new_flat = collections.Counter(fp for prints in found_prints.values() for fp in prints)
    return sorted((new_flat - existing_flat).elements())


def _regenerate_size_index_baseline() -> int:
    """Rewrite detector 2's baseline, REFUSING to add a fingerprint.

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
    additions = _refused_flattened_additions(found_prints, baseline)
    if additions:
        print(
            f"[size-then-index] REFUSED: regeneration would ADD {len(additions)} "
            f"fingerprint(s) not present anywhere in the existing baseline. This baseline "
            f"may only shrink or be re-keyed - a new site is a new crash, not a new "
            f"baseline line. Fix the site."
        )
        for print_ in additions:
            print(f"    {print_}")
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


def _regenerate_null_deref_baseline() -> int:
    """Rewrite detector 1's baseline, REFUSING to add a fingerprint.

    Detector 1 had no regeneration tool before GS-AUDIT-TEST-003; the file was
    maintained by hand on the theory that a legitimate change to it is rare and
    reviewable as a diff either way (round 9's 319 -> 337 landed that way). This PR
    needs a mechanical, hand-edit-free way to re-key the file from basename to
    repo-relative path, so a tool now exists, mirroring detector 2's shape and its
    refuses-to-add rule (see `_refused_flattened_additions`).
    """
    found_prints = scan_fingerprints()
    baseline, problems = load_baseline()
    if problems:
        print("[require-null-deref] REFUSED: the existing baseline cannot be read.")
        for problem in problems:
            print(f"    {problem}")
        return 1
    additions = _refused_flattened_additions(found_prints, baseline)
    if additions:
        print(
            f"[require-null-deref] REFUSED: regeneration would ADD {len(additions)} "
            f"fingerprint(s) not present anywhere in the existing baseline. #656 rules out "
            f"blessing a new site this way - fix it instead."
        )
        for print_ in additions:
            print(f"    {print_}")
        return 1
    document = {
        "schema_version": 1,
        "issue_url": BASELINE_ISSUE,
        "note": _NULL_DEREF_BASELINE_NOTE,
        "files": {name: found_prints[name] for name in sorted(found_prints)},
    }
    BASELINE_PATH.write_text(
        json.dumps(document, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    total = sum(len(v) for v in found_prints.values())
    print(f"[require-null-deref] baseline rewritten: {total} site(s) across {len(found_prints)} file(s).")
    return 0


def main(argv: list[str] | None = None) -> int:
    # Deliberately not argparse: main() is called with no arguments by the unit
    # test and by run_module_tests.py, and argparse would then parse THEIR argv.
    arguments = list(argv or [])

    # --base-ref VALUE, parsed by hand for the same reason the flags above are:
    # mirrors check_unchecked_resize.py's `--base-ref`, the review base to grade the
    # baseline's growth against (GS_CI_BASE_REF / GITHUB_BASE_REF / ..., then
    # origin/master, resolved by resolve_review_base() below).
    base_ref: str | None = None
    if "--base-ref" in arguments:
        idx = arguments.index("--base-ref")
        if idx + 1 >= len(arguments):
            print("[require-null-deref] FAIL --base-ref requires a value.")
            return 1
        base_ref = arguments[idx + 1]
        arguments = arguments[:idx] + arguments[idx + 2 :]

    regenerate_size_index = SIZE_INDEX_REGENERATE_FLAG in arguments
    regenerate_null_deref = NULL_DEREF_REGENERATE_FLAG in arguments
    unknown = [
        a for a in arguments
        if a not in (SIZE_INDEX_REGENERATE_FLAG, NULL_DEREF_REGENERATE_FLAG)
    ]
    if unknown:
        print(f"[require-null-deref] FAIL unknown argument(s): {' '.join(unknown)}")
        return 1
    if regenerate_size_index and regenerate_null_deref:
        print(
            f"[require-null-deref] FAIL pass only one of {SIZE_INDEX_REGENERATE_FLAG} / "
            f"{NULL_DEREF_REGENERATE_FLAG} at a time."
        )
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

    if regenerate_null_deref:
        return _regenerate_null_deref_baseline()
    if regenerate_size_index:
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
            failures.append(f"{name}: {len(added)} NEW assert-then-dereference site(s):")
            for print_ in added:
                line_no, symbol, form, statement = where[name][print_]
                # Deliberately not "REQUIRE(...)": since #849 round 9 the accepted
                # macro names are derived from doctest, so the assertion may be a
                # CHECK or a WARN, and naming the wrong macro in a failure report
                # sends the reader to the wrong line.
                failures.append(
                    f"    line {line_no}: asserted `{symbol} {form}`, then `{_elide(statement, 90)}`"
                )
            failures.append(
                f"    Neither REQUIRE (DOCTEST_CONFIG_NO_EXCEPTIONS in this build) nor CHECK/WARN "
                f"(in any build) aborts: on "
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

    # GS-AUDIT-TEST-003: the two checks above compare the scan against the WORKING
    # TREE's copy of each baseline, which a change edits together with the source it
    # is adding a violation to -- so they cannot see a joint mutation, only a baseline
    # that has drifted from an honest scan. This closes that hole by grading the scan
    # against each baseline as committed at the REVIEW BASE instead, which the mutation
    # cannot reach. Runs even when the checks above already failed, for the same reason
    # detector 2 always runs: one guard masking another's report is how a second
    # defect ships behind a first.
    base_sha, base_sha_failures = resolve_review_base(base_ref)
    if base_sha_failures or base_sha is None:
        print("[require-null-deref] FAIL cannot resolve the review base needed to grade "
              "either baseline's growth (GS-AUDIT-TEST-003):")
        for line in (base_sha_failures or ["the review base did not resolve."]):
            print(f"  {line}")
        base_growth_status = 1
    else:
        differs, differ_failures = detector_differs_from_base(base_sha)
        if differ_failures:
            print(f"[require-null-deref] FAIL cannot compare this script against the "
                  f"review base {base_sha[:12]}:")
            for line in differ_failures:
                print(f"  {line}")
            base_growth_status = 1
        else:
            base_growth_status = 0
            size_index_prints, _size_index_scan_errors = scan_size_index_fingerprints()
            for label, current_prints, baseline_path, issue, scan_kind in (
                ("require-null-deref", found_prints, BASELINE_PATH, BASELINE_ISSUE, "null_deref"),
                ("size-then-index", size_index_prints, SIZE_INDEX_BASELINE_PATH, SIZE_INDEX_ISSUE, "size_index"),
            ):
                new_relative, growth_failures, introduced = _baseline_growth_vs_base(
                    current_prints, baseline_path, base_sha, differs, scan_kind
                )
                if growth_failures:
                    print(f"[{label}] FAIL cannot grade {baseline_path.name} against the "
                          f"review base {base_sha[:12]}:")
                    for line in growth_failures:
                        print(f"  {line}")
                    base_growth_status = 1
                elif introduced:
                    print(f"[{label}] NOTE {baseline_path.name} is absent at review base "
                          f"{base_sha[:12]}: this change introduces it, so there is no "
                          f"shrink-only reference for the review-base comparison this run.")
                elif new_relative:
                    total_new = sum(len(v) for v in new_relative.values())
                    print(f"[{label}] FAIL {total_new} fingerprint(s) NEW relative to review "
                          f"base {base_sha[:12]}. Comparing against the WORKING TREE baseline "
                          f"alone cannot see this: a change that adds a violation and its "
                          f"baseline entry in the same commit would grade itself. ({issue})")
                    for name in sorted(new_relative):
                        for fp in new_relative[name]:
                            print(f"    {name}: {fp}")
                    base_growth_status = 1
                else:
                    print(f"[{label}] PASS 0 new relative to review base {base_sha[:12]}.")

    return status or size_status or base_growth_status


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
