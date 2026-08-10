#!/usr/bin/env python3
"""Unit test for tests/ci/check_require_null_deref.py (#656).

A guard that has never been observed to fail is not evidence that it works, and a
guard nobody has tried to fool is not evidence that it is precise. These cases pin
BOTH directions: the shapes it must flag, and the shapes it must leave alone.
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

CI_DIR = Path(__file__).resolve().parent


def _load_guard():
    spec = importlib.util.spec_from_file_location(
        "_gs_check_require_null_deref", CI_DIR / "check_require_null_deref.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["_gs_check_require_null_deref"] = module
    spec.loader.exec_module(module)
    return module


GUARD = _load_guard()


class ScanTestCase(unittest.TestCase):
    def scan(self, body: str) -> list[tuple[int, str, str, str]]:
        """Scan a synthetic test-case body and return the violations found."""
        source = "TEST_CASE(\"[Synthetic] case\") {\n" + body + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            return GUARD._scan_file(path)

    def assertFlagged(self, body: str, symbol: str) -> None:
        found = self.scan(body)
        self.assertTrue(found, f"expected a violation for {symbol!r}, got none")
        self.assertIn(symbol, [v[1] for v in found])

    def assertClean(self, body: str) -> None:
        found = self.scan(body)
        self.assertEqual(found, [], f"expected no violation, got {found}")


class TruePositives(ScanTestCase):
    def test_nullptr_then_arrow(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  ptr->method();", "ptr")

    def test_null_macro_then_arrow(self):
        self.assertFlagged("  REQUIRE(ptr != NULL);\n  ptr->method();", "ptr")

    def test_is_valid_then_arrow(self):
        self.assertFlagged("  REQUIRE(ref.is_valid());\n  ref->method();", "ref")

    def test_require_message_then_arrow(self):
        self.assertFlagged(
            '  REQUIRE_MESSAGE(tree != nullptr, "needed");\n  Window *r = tree->get_root();',
            "tree",
        )

    def test_require_false_is_null(self):
        self.assertFlagged("  REQUIRE_FALSE(ref.is_null());\n  ref->method();", "ref")

    def test_require_ne_nullptr(self):
        self.assertFlagged("  REQUIRE_NE(ptr, nullptr);\n  ptr->method();", "ptr")

    def test_deref_via_star(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  int v = *ptr;", "ptr")

    def test_deref_via_index(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  int v = ptr[0].field;", "ptr")

    def test_scan_crosses_other_assertions(self):
        # The dominant real shape: several REQUIREs, then the dereference.
        self.assertFlagged(
            "  REQUIRE(a != nullptr);\n"
            "  REQUIRE(b != nullptr);\n"
            "  CHECK(b->size() == 2);\n"
            "  a->method();",
            "a",
        )

    def test_deref_inside_a_later_assertion(self):
        self.assertFlagged(
            "  REQUIRE(data.is_valid());\n  CHECK(data->get_count() == 1);", "data"
        )


class TrueNegatives(ScanTestCase):
    def test_explicit_guard_is_the_correct_pattern(self):
        self.assertClean(
            '  if (!ptr) {\n    FAIL("no ptr");\n    return;\n  }\n  ptr->method();'
        )

    def test_dereference_guarded_by_if(self):
        # REQUIRE does not abort, but the `if` does make the dereference safe.
        self.assertClean("  REQUIRE(ptr != nullptr);\n  if (ptr) {\n    ptr->method();\n  }")

    def test_member_call_on_ref_handle_is_not_a_dereference(self):
        self.assertClean("  REQUIRE(ref.is_valid());\n  ref.unref();")

    def test_non_null_predicate_is_ignored(self):
        self.assertClean("  REQUIRE(count == 3);\n  ptr->method();")

    def test_reassignment_stops_the_scan(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  ptr = other();\n  ptr->method();")

    def test_different_symbol_is_not_flagged(self):
        self.assertClean("  REQUIRE(a != nullptr);\n  b->method();")

    def test_similar_prefix_is_not_confused(self):
        # `asset_chunks` must not satisfy a dereference of `asset`.
        self.assertClean("  REQUIRE(asset != nullptr);\n  asset_chunks.resize(4);")

    def test_arrow_inside_a_string_is_not_a_dereference(self):
        self.assertClean('  REQUIRE(ptr != nullptr);\n  MESSAGE("ptr->method() skipped");')

    def test_dereference_in_a_comment_is_not_flagged(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  // ptr->method();\n  other();")

    def test_return_stops_the_scan(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  return;\n  ptr->method();")


class StripCommentsPreservesLineNumbers(unittest.TestCase):
    """Line drift is a silent miscount, which is the failure mode of this issue."""

    def test_line_count_is_preserved(self):
        samples = [
            'a();\n// comment\nb();\n',
            'a();\n/* block\nspanning\nlines */\nb();\n',
            'const char *s = "a\\"b";\nc();\n',
            "int x = 1'000;\nd();\n",  # digit separator, not a char literal
            "char c = '\\'';\ne();\n",
            'a(); /* trailing */ b();\nf();\n',
            "a();\n// spliced comment \\\nstill_comment();\ng();\n",
        ]
        for sample in samples:
            with self.subTest(sample=sample[:24]):
                self.assertEqual(
                    len(sample.split("\n")),
                    len(GUARD._strip_comments(sample).split("\n")),
                )

    def test_reported_line_matches_source_line(self):
        source = (
            "TEST_CASE(\"[Synthetic] case\") {\n"
            "\t// filler comment\n"
            "\t/* block\n"
            "\t   comment */\n"
            "\tint x = 1'000;\n"
            "\tREQUIRE(ptr != nullptr);\n"
            "\tptr->method();\n"
            "}\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            found = GUARD._scan_file(path)
        self.assertEqual(len(found), 1)
        line_no = found[0][0]
        self.assertIn("REQUIRE(ptr != nullptr);", source.split("\n")[line_no - 1])


class MemberChains(ScanTestCase):
    """A member chain is one symbol. The docstring once claimed this without doing it."""

    def test_dot_chain_then_arrow(self):
        self.assertFlagged(
            "  REQUIRE(state.hierarchical_structure != nullptr);\n"
            "  const AABB b = state.hierarchical_structure->get_bounds();",
            "state.hierarchical_structure",
        )

    def test_dot_chain_is_valid_then_arrow(self):
        self.assertFlagged(
            "  REQUIRE(resource_state.buffer_manager.is_valid());\n"
            "  const Error e = resource_state.buffer_manager->initialize(rd, 16);",
            "resource_state.buffer_manager",
        )

    def test_arrow_chain(self):
        self.assertFlagged(
            "  REQUIRE(node->renderer != nullptr);\n  node->renderer->flush();",
            "node->renderer",
        )

    def test_sibling_member_is_not_confused_with_a_prefix(self):
        # `state.hierarchical_structure_build_count` must not satisfy a
        # dereference of `state.hierarchical_structure`.
        self.assertClean(
            "  REQUIRE(state.hierarchical_structure != nullptr);\n"
            "  const uint64_t n = state.hierarchical_structure_build_count;"
        )

    def test_different_chain_root_is_not_flagged(self):
        self.assertClean("  REQUIRE(a.member != nullptr);\n  b.member->f();")


class ControlFlowHeaders(ScanTestCase):
    """A control-flow statement guards its BODY, never its own condition.

    Codex #659: `REQUIRE(ptr != nullptr); if (ptr->is_ready()) { ... }` used to be
    missed entirely - the scan broke on the `if` before testing it for a
    dereference, so the same crash pattern walked straight through the guard.
    """

    def test_deref_in_if_condition_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  if (ptr->is_ready()) {\n    CHECK(true);\n  }", "ptr"
        )

    def test_deref_in_while_condition_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  while (ptr->next()) {\n    step();\n  }", "ptr"
        )

    def test_deref_in_for_header_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n"
            "  for (int i = 0; i < ptr->size(); ++i) {\n    step();\n  }",
            "ptr",
        )

    def test_deref_in_return_is_flagged(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  return ptr->size();", "ptr")

    def test_deref_in_switch_condition_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  switch (ptr->kind()) {\n    default:\n      break;\n  }",
            "ptr",
        )

    def test_guarded_body_is_still_not_flagged(self):
        # The whole point of stopping at control flow: the `if` makes this safe.
        self.assertClean("  REQUIRE(ptr != nullptr);\n  if (ptr) {\n    ptr->f();\n  }")

    def test_is_valid_guarded_body_is_not_flagged(self):
        self.assertClean("  REQUIRE(ref.is_valid());\n  if (ref.is_valid()) {\n    ref->f();\n  }")

    def test_body_guarded_by_unrelated_condition_is_a_documented_blind_spot(self):
        # Real crash, deliberately NOT flagged: we cannot tell which conditions
        # protect the symbol. Pinned so the blind spot is a decision, not a drift.
        self.assertClean("  REQUIRE(ptr != nullptr);\n  if (other) {\n    ptr->f();\n  }")


class ShortCircuitGuards(ScanTestCase):
    """C++ short-circuiting makes some dereferences unreachable (Codex #659).

    The only FALSE-POSITIVE finding in this review series: `if (ptr && ptr->f())`
    is safe, and flagging it would block the guard lane on correct code. A guard
    that cries wolf gets weakened by whoever hits it next, so this matters as much
    as the under-reports.
    """

    def test_and_guard_is_safe(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  if (ptr && ptr->ready()) { s(); }")

    def test_or_negated_guard_is_safe(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  if (!ptr || ptr->ready()) { s(); }")

    def test_explicit_comparison_guard_is_safe(self):
        self.assertClean(
            "  REQUIRE(ptr != nullptr);\n  if (ptr != nullptr && ptr->f()) { s(); }"
        )

    def test_is_valid_guard_is_safe(self):
        self.assertClean("  REQUIRE(ref.is_valid());\n  CHECK(ref.is_valid() && ref->f());")

    def test_is_null_or_guard_is_safe(self):
        self.assertClean("  REQUIRE_FALSE(ref.is_null());\n  CHECK(ref.is_null() || ref->f());")

    def test_ternary_guard_is_safe(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  int v = ptr ? ptr->f() : 0;")

    def test_short_circuit_applies_outside_control_flow_too(self):
        # Not just headers: a plain assertion has the same semantics.
        self.assertClean("  REQUIRE(ptr != nullptr);\n  CHECK(ptr && ptr->size() == 3);")

    def test_unrelated_condition_does_not_guard(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  if (other && ptr->ready()) { s(); }", "ptr"
        )

    def test_guard_after_the_deref_does_not_help(self):
        # Evaluation order matters: only the prefix can guard.
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  if (ptr->ready() && ptr) { s(); }", "ptr"
        )

    def test_or_without_negation_does_not_guard(self):
        # `ptr || ptr->f()` evaluates the deref precisely when ptr is falsy.
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  CHECK(ptr || ptr->f());", "ptr")

    def test_unrelated_ternary_condition_does_not_guard(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  int v = other ? ptr->f() : 0;", "ptr")

    def test_a_later_bare_deref_is_still_reported(self):
        """Each dereference is judged on its own prefix, not the statement's."""
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  CHECK(ptr && ptr->a());\n  ptr->b();", "ptr"
        )


class MultipleRequiresPerLine(ScanTestCase):
    """Every REQUIRE on a compacted line is a guard, not just the first (Codex #659).

    Matching once from the start of the line meant
    `REQUIRE(a != nullptr); REQUIRE(b != nullptr); b->f();` established a guard
    for `a` only, and the `b` crash was never reported.
    """

    def symbols(self, body: str) -> list[str]:
        return sorted(symbol for _, symbol, _, _ in self.scan(body))

    def test_second_require_is_a_guard(self):
        self.assertEqual(
            self.symbols("  REQUIRE(a != nullptr); REQUIRE(b != nullptr); b->f();"), ["b"]
        )

    def test_first_require_still_works(self):
        self.assertEqual(
            self.symbols("  REQUIRE(a != nullptr); REQUIRE(b != nullptr); a->f();"), ["a"]
        )

    def test_both_symbols_reported_when_both_dereferenced(self):
        self.assertEqual(
            self.symbols("  REQUIRE(a != nullptr); REQUIRE(b != nullptr); a->f(); b->g();"),
            ["a", "b"],
        )

    def test_third_require_on_the_line(self):
        self.assertEqual(
            self.symbols(
                "  REQUIRE(a != nullptr); REQUIRE(b != nullptr); REQUIRE(c != nullptr); c->f();"
            ),
            ["c"],
        )

    def test_compacted_guarded_use_stays_clean(self):
        self.assertClean("  REQUIRE(a != nullptr); REQUIRE(b != nullptr); if (b) { b->f(); }")

    def test_fragments_split_at_depth_zero_only(self):
        # A ';' inside parentheses must not split the statement.
        self.assertEqual(
            GUARD._line_fragments("REQUIRE(a != nullptr); b->f();"),
            ["REQUIRE(a != nullptr);", "b->f();"],
        )
        self.assertEqual(
            GUARD._line_fragments("for (int i = 0; i < n; ++i) { s(); }"),
            ["for (int i = 0; i < n; ++i) { s(); }"],
        )

    def test_control_flow_tail_is_kept_whole(self):
        fragments = GUARD._line_fragments("REQUIRE(a != nullptr); for (int i = 0; i < n; ++i) {")
        self.assertEqual(fragments[0], "REQUIRE(a != nullptr);")
        self.assertEqual(fragments[1], "for (int i = 0; i < n; ++i) {")


class GuardMustDominate(ScanTestCase):
    """A short-circuit guard must DOMINATE the dereference (Codex #659).

    The first version of the short-circuit fix searched the textual prefix, which
    accepted `(ptr && ptr->f()) || ptr->g()` — a false NEGATIVE introduced while
    fixing a false positive. These cases pin the precedence decomposition.
    """

    def test_disjunct_after_a_guarded_group_is_not_guarded(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  CHECK((ptr && ptr->f()) || ptr->g());", "ptr"
        )

    def test_group_containing_a_guard_does_not_guard_a_later_disjunct(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  CHECK((ptr && a) || ptr->g());", "ptr"
        )

    def test_ternary_else_branch_is_not_guarded_by_a_positive_condition(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  int v = ptr ? x : ptr->f();", "ptr")

    def test_guard_inside_an_earlier_conjunct_group_does_not_escape_it(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  CHECK((a && ptr->f()) || b);", "ptr")

    def test_negated_unrelated_symbol_does_not_guard(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr);\n  CHECK(!other || ptr->f());", "ptr")

    def test_outer_conjunct_guards_a_nested_group(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  CHECK(ptr && (a || ptr->f()));")

    def test_outer_negated_disjunct_guards_a_nested_group(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  CHECK(!ptr || (a && ptr->f()));")

    def test_ternary_true_branch_is_guarded(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  int v = ptr ? ptr->f() : 0;")

    def test_ternary_else_branch_guarded_by_a_negative_condition(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  int v = !ptr ? 0 : ptr->f();")

    def test_guard_in_an_earlier_conjunct_still_works(self):
        self.assertClean("  REQUIRE(ptr != nullptr);\n  CHECK(ptr && a && ptr->f());")

    def test_decomposition_helpers(self):
        self.assertTrue(GUARD._positive_test("ptr", " ptr "))
        self.assertTrue(GUARD._positive_test("ptr", "ptr != nullptr"))
        self.assertTrue(GUARD._positive_test("ref", "ref.is_valid()"))
        self.assertFalse(GUARD._positive_test("ptr", "ptr->f()"))
        self.assertFalse(GUARD._positive_test("ptr", "other"))
        self.assertTrue(GUARD._negative_test("ptr", "!ptr"))
        self.assertTrue(GUARD._negative_test("ptr", "ptr == nullptr"))
        self.assertTrue(GUARD._negative_test("ref", "ref.is_null()"))
        self.assertFalse(GUARD._negative_test("ptr", "!other"))
        # An assignment prefix must not hide the ternary condition.
        self.assertEqual(GUARD._condition_tail("int v = ptr ").strip(), "ptr")
        self.assertEqual(GUARD._condition_tail("ptr != nullptr").strip(), "ptr != nullptr")

    def test_enclosing_group_peels_outermost_first(self):
        text = "CHECK(ptr && (a || ptr->f()))"
        at = text.index("ptr->f()")
        span = GUARD._enclosing_group(text, at)
        self.assertEqual(text[span[0] : span[1]], "ptr && (a || ptr->f())")


class MultiLineRequire(ScanTestCase):
    """A REQUIRE whose predicate spans physical lines (Codex #659).

    Latent at the time of the fix: the corpus has 34 multi-line `REQUIRE*` calls,
    but in the null-ish ones only the MESSAGE wraps — the predicate itself is on
    the first line, which the anchored pattern already matched. The baseline is
    unchanged at 325.
    """

    def test_predicate_split_across_lines(self):
        self.assertFlagged("  REQUIRE(\n      ptr != nullptr);\n  ptr->method();", "ptr")

    def test_message_form_split_across_lines(self):
        self.assertFlagged(
            '  REQUIRE_MESSAGE(\n      ptr != nullptr,\n      "needed");\n  ptr->method();',
            "ptr",
        )

    def test_is_valid_form_split_across_lines(self):
        self.assertFlagged("  REQUIRE(\n      ref.is_valid());\n  ref->method();", "ref")

    def test_require_ne_split_across_lines(self):
        self.assertFlagged("  REQUIRE_NE(\n      ptr,\n      nullptr);\n  ptr->method();", "ptr")

    def test_forward_scan_resumes_after_the_last_joined_line(self):
        # If the scan restarted at index+1 it would read the continuation line as
        # the "next statement" and miss the real one.
        self.assertFlagged(
            '  REQUIRE_MESSAGE(\n      ptr != nullptr,\n      "m");\n  ptr->method();', "ptr"
        )

    def test_split_predicate_then_guarded_use_is_clean(self):
        self.assertClean("  REQUIRE(\n      ptr != nullptr);\n  if (ptr) { ptr->f(); }")

    def test_split_predicate_without_deref_is_clean(self):
        self.assertClean("  REQUIRE(\n      ptr != nullptr);\n  other();")

    def test_continuation_line_does_not_produce_a_second_match(self):
        found = self.scan("  REQUIRE(\n      ptr != nullptr);\n  ptr->method();")
        self.assertEqual(len(found), 1, "the predicate line must not match again on its own")

    def test_logical_line_is_bounded(self):
        # A stray unbalanced paren must not swallow the file.
        lines = ["REQUIRE(("] + [f"line_{i};" for i in range(40)]
        _, last = GUARD._logical_line(lines, 0)
        self.assertLess(last, 13)


class DoctestMacroNames(ScanTestCase):
    """Every predicate row must name macros doctest actually exposes (Codex #659).

    `REQUIRE_FALSE(?:_MESSAGE|_UNARY_FALSE)?` put the alternation on the wrong
    side of the prefix: it accepted the nonexistent `REQUIRE_FALSE_UNARY_FALSE`
    and missed the real `REQUIRE_UNARY_FALSE`.
    """

    REAL_MACROS = (
        "REQUIRE",
        "REQUIRE_MESSAGE",
        "REQUIRE_FALSE",
        "REQUIRE_FALSE_MESSAGE",
        "REQUIRE_UNARY",
        "REQUIRE_UNARY_FALSE",
        "REQUIRE_NE",
    )

    def test_every_macro_this_guard_names_exists_in_doctest(self):
        """Guard against inventing a macro name again - check against the header."""
        doctest_header = (
            Path(__file__).resolve().parents[2] / "thirdparty" / "doctest" / "doctest.h"
        ).read_text(encoding="utf-8", errors="replace")
        for macro in self.REAL_MACROS:
            self.assertIn(
                f"define DOCTEST_{macro}",
                doctest_header,
                f"{macro} is not a real doctest macro",
            )
        self.assertNotIn(
            "define DOCTEST_REQUIRE_FALSE_UNARY_FALSE",
            doctest_header,
            "if this ever exists, the old regex was not as wrong as believed",
        )

    def test_require_unary_false_is_null(self):
        self.assertFlagged("  REQUIRE_UNARY_FALSE(ref.is_null());\n  ref->f();", "ref")

    def test_require_false_is_null(self):
        self.assertFlagged("  REQUIRE_FALSE(ref.is_null());\n  ref->f();", "ref")

    def test_require_false_message_is_null(self):
        self.assertFlagged(
            '  REQUIRE_FALSE_MESSAGE(ref.is_null(), "needed");\n  ref->f();', "ref"
        )

    def test_require_unary_is_valid(self):
        self.assertFlagged("  REQUIRE_UNARY(ref.is_valid());\n  ref->f();", "ref")

    def test_asserting_the_pointer_IS_null_is_not_a_null_guard(self):
        # REQUIRE_UNARY(x.is_null()) asserts the opposite; flagging it would be wrong.
        self.assertClean("  REQUIRE_UNARY(ref.is_null());\n  ref->f();")


class MultiLineControlFlowHeaders(ScanTestCase):
    """A ';' inside a for-header is not a statement terminator (Codex #659).

    Latent at the time of the fix: the scanned corpus contains multi-line `for`
    headers, but only range-based ones (no semicolons), and none follows a
    null-ish REQUIRE. So this closes a future hole rather than recovering sites -
    the baseline is unchanged at 325.
    """

    def test_multi_line_for_header_deref_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n"
            "  for (int i = 0;\n"
            "       i < ptr->size();\n"
            "       ++i) {\n"
            "    step();\n"
            "  }",
            "ptr",
        )

    def test_multi_line_for_header_without_deref_is_clean(self):
        self.assertClean(
            "  REQUIRE(ptr != nullptr);\n"
            "  for (int i = 0;\n"
            "       i < count;\n"
            "       ++i) {\n"
            "    step();\n"
            "  }"
        )

    def test_single_line_for_header_still_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  for (int i = 0; i < ptr->size(); ++i) {\n    s();\n  }",
            "ptr",
        )

    def test_multi_line_if_condition_deref_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  if (ptr->ready() &&\n      other) {\n    s();\n  }",
            "ptr",
        )

    def test_multi_line_call_argument_deref_is_flagged(self):
        self.assertFlagged(
            "  REQUIRE(ptr != nullptr);\n  helper(first_argument,\n         ptr->value());",
            "ptr",
        )

    def test_range_based_for_over_a_brace_list_is_unchanged(self):
        # The two real multi-line headers in the corpus are this shape; they must
        # keep grouping exactly as before (baseline fingerprints are unchanged).
        self.assertClean(
            "  REQUIRE(ptr != nullptr);\n"
            "  for (Kind kind : { Kind::A,\n"
            "         Kind::B }) {\n"
            "    step(kind);\n"
            "  }"
        )


class SameLineStatements(ScanTestCase):
    """The dangerous pattern written on ONE line (Codex #659).

    The forward scan used to start at the next line, so the rest of the REQUIRE's
    own line was never inspected - and that one-liner is exactly the shape
    tests/AGENTS.md uses to describe the bug.
    """

    def test_one_line_require_then_deref(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr); ptr->method();", "ptr")

    def test_one_line_is_valid_then_deref(self):
        self.assertFlagged("  REQUIRE(ref.is_valid()); ref->method();", "ref")

    def test_one_line_if_condition_deref(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr); if (ptr->ready()) { step(); }", "ptr")

    def test_one_line_guarded_body_is_clean(self):
        self.assertClean("  REQUIRE(ptr != nullptr); if (ptr) { ptr->f(); }")

    def test_one_line_without_deref_is_clean(self):
        self.assertClean("  REQUIRE(ptr != nullptr); other();")

    def test_same_line_statement_then_deref_on_the_next_line(self):
        self.assertFlagged("  REQUIRE(ptr != nullptr); other();\n  ptr->f();", "ptr")

    def test_agents_md_example_is_actually_caught(self):
        """The doc tells contributors this crashes; the guard must agree."""
        self.assertFlagged("  REQUIRE(ptr != nullptr); ptr->f();", "ptr")


class GetterExpressions(ScanTestCase):
    """A no-arg getter call is part of the symbol (Codex #659)."""

    def test_ref_returning_getter_then_arrow(self):
        self.assertFlagged(
            "  REQUIRE(loaded->get_gaussian_data().is_valid());\n"
            "  CHECK(loaded->get_gaussian_data()->get_count() == 1);",
            "loaded->get_gaussian_data()",
        )

    def test_dot_getter_then_arrow(self):
        self.assertFlagged(
            "  REQUIRE(holder.get_thing().is_valid());\n  holder.get_thing()->run();",
            "holder.get_thing()",
        )

    def test_getter_nullptr_form(self):
        self.assertFlagged(
            "  REQUIRE(tree->get_root() != nullptr);\n  tree->get_root()->add_child(node);",
            "tree->get_root()",
        )

    def test_getter_handle_call_is_not_a_dereference(self):
        self.assertClean(
            "  REQUIRE(loaded->get_gaussian_data().is_valid());\n"
            "  loaded->get_gaussian_data().unref();"
        )

    def test_different_getter_is_not_flagged(self):
        self.assertClean(
            "  REQUIRE(loaded->get_gaussian_data().is_valid());\n  loaded->get_other()->f();"
        )


class FingerprintUsesFullStatement(unittest.TestCase):
    """Hashing a truncated statement collapsed distinct sites into one identity."""

    def test_statements_differing_past_the_display_limit_differ(self):
        prefix = "const Result query = structure.query_visible_splats(camera->get_frustum(), "
        prefix += "x" * 150
        a = prefix + ", FIRST_VARIANT);"
        b = prefix + ", SECOND_VARIANT);"
        self.assertEqual(a[:120], b[:120], "the prefixes must be identical for this to prove anything")
        self.assertNotEqual(
            GUARD.fingerprint("camera", "!= nullptr", a),
            GUARD.fingerprint("camera", "!= nullptr", b),
        )

    def test_scan_returns_untruncated_statements(self):
        # Long tail in CODE, not in a string literal: literals are blanked by
        # _strip_comments, so a long string would not exercise truncation at all.
        long_tail = " + ".join(f"value_{i}" for i in range(40))
        body = "  REQUIRE(ptr != nullptr);\n" f"  CHECK(ptr->method() == {long_tail});"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text('TEST_CASE("[S] c") {\n' + body + "\n}\n", encoding="utf-8")
            found = GUARD._scan_file(path)
        self.assertEqual(len(found), 1)
        self.assertGreater(
            len(found[0][3]), 120, "the statement must reach fingerprint() untruncated"
        )

    def test_elide_shortens_only_for_display(self):
        self.assertEqual(GUARD._elide("short", 90), "short")
        long_text = "z" * 200
        self.assertEqual(len(GUARD._elide(long_text, 90)), 90)

    def test_no_truncation_collisions_in_the_shipped_baseline(self):
        """Duplicates are legal (identical statements), but not from truncation."""
        for name, violations in GUARD.scan_all().items():
            by_print: dict[str, set[str]] = {}
            for _, symbol, form, statement in violations:
                by_print.setdefault(GUARD.fingerprint(symbol, form, statement), set()).add(statement)
            for print_, statements in by_print.items():
                self.assertEqual(
                    len(statements),
                    1,
                    f"{name}: fingerprint {print_} covers {len(statements)} DIFFERENT statements",
                )


class SwapDetection(unittest.TestCase):
    """A count-only baseline licenses a swap; the fingerprint set must not.

    Reviewer-demonstrated hole: fix one site the prescribed way AND add a new one
    in the same file, and a per-file count is unchanged -> PASS, "0 new".
    """

    def _prints(self, body: str) -> list[str]:
        source = 'TEST_CASE("[Synthetic] case") {\n' + body + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            return sorted(
                GUARD.fingerprint(sym, form, stmt)
                for _, sym, form, stmt in GUARD._scan_file(path)
            )

    def test_swap_keeps_the_count_but_changes_the_fingerprints(self):
        before = self._prints("  REQUIRE(alpha != nullptr);\n  alpha->method();")
        after = self._prints("  REQUIRE(beta != nullptr);\n  beta->other();")
        self.assertEqual(len(before), 1)
        self.assertEqual(len(after), 1, "count is unchanged - this is the hole")
        self.assertNotEqual(before, after, "fingerprints MUST differ, or the swap is invisible")
        self.assertEqual(GUARD._multiset_difference(after, before), after)
        self.assertEqual(GUARD._multiset_difference(before, after), before)

    def test_fingerprint_is_independent_of_line_number(self):
        plain = self._prints("  REQUIRE(alpha != nullptr);\n  alpha->method();")
        shifted = self._prints(
            "  int filler = 0;\n  (void)filler;\n  REQUIRE(alpha != nullptr);\n  alpha->method();"
        )
        self.assertEqual(plain, shifted)

    def test_duplicate_sites_are_counted_as_a_multiset(self):
        both = self._prints(
            "  REQUIRE(alpha != nullptr);\n  alpha->method();\n"
            "  REQUIRE(alpha != nullptr);\n  alpha->method();"
        )
        self.assertEqual(len(both), 2)
        self.assertEqual(both[0], both[1])
        # Removing one of an identical pair must still register as a removal.
        self.assertEqual(len(GUARD._multiset_difference(both, both[:1])), 1)


class SizeIndexScanTestCase(unittest.TestCase):
    """Base for detector 2 (#844): size-assert-then-index."""

    def sites(self, body: str) -> list[tuple[int, str, str, str, int, str, str]]:
        source = 'TEST_CASE("[Synthetic] case") {\n' + body + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            return GUARD._scan_file_size_index(path)

    def null_deref_sites(self, body: str) -> list[tuple[int, str, str, str]]:
        source = 'TEST_CASE("[Synthetic] case") {\n' + body + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            return GUARD._scan_file(path)

    def assertSized(self, body: str, symbol: str) -> None:
        found = self.sites(body)
        self.assertTrue(found, f"expected a size-then-index site for {symbol!r}, got none")
        self.assertIn(symbol, [site[1] for site in found])

    def assertNotSized(self, body: str) -> None:
        found = self.sites(body)
        self.assertEqual(found, [], f"expected no size-then-index site, got {found}")


class SizeThenIndexIsTheNullDerefGuardsBlindSpot(SizeIndexScanTestCase):
    """The whole reason detector 2 exists: detector 1 cannot see this shape.

    Pinned as a test rather than asserted in prose, because "the other guard does
    not cover it" is exactly the claim that rots silently. If detector 1 ever does
    start reporting these, this test fails and the two detectors' scopes get
    re-decided deliberately instead of by accident.
    """

    SHAPES = (
        "  REQUIRE(payload.size() == 2);\n  CHECK(payload[0].target_opacity == 0.35f);",
        "  CHECK(selected_names.size() == 1);\n  CHECK(selected_names[0] == String());",
        "  REQUIRE_EQ(out_logits.size(), 4);\n  CHECK(out_logits[3] == 1.0f);",
        "  REQUIRE(!chunks.is_empty());\n  StreamingChunk &c = chunks[0];",
    )

    def test_detector_one_is_green_on_every_shape(self):
        for body in self.SHAPES:
            with self.subTest(body=body):
                self.assertEqual(
                    self.null_deref_sites(body),
                    [],
                    "detector 1 (null-deref) must stay GREEN here - this is its blind spot #7",
                )

    def test_detector_two_is_red_on_every_shape(self):
        for body in self.SHAPES:
            with self.subTest(body=body):
                self.assertTrue(
                    self.sites(body), "detector 2 must flag the size-then-index shape"
                )

    def test_check_is_covered_not_only_require(self):
        """#843's `:1323` site was a CHECK, which never aborts under ANY config."""
        require_form = self.sites("  REQUIRE(names.size() == 1);\n  CHECK(names[0] == 1);")
        check_form = self.sites("  CHECK(names.size() == 1);\n  CHECK(names[0] == 1);")
        self.assertEqual(len(require_form), 1)
        self.assertEqual(len(check_form), 1)
        self.assertEqual(require_form[0][2], "REQUIRE")
        self.assertEqual(check_form[0][2], "CHECK")


class SizeThenIndexTruePositives(SizeIndexScanTestCase):
    def test_equality_then_index(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);", "v")

    def test_greater_than_zero_then_index(self):
        self.assertSized("  REQUIRE(v.size() > 0);\n  auto &c = v[0];", "v")

    def test_not_equal_zero_then_index(self):
        self.assertSized("  REQUIRE(v.size() != 0);\n  CHECK(v[0] == 1);", "v")

    def test_not_is_empty_then_index(self):
        self.assertSized("  REQUIRE(!v.is_empty());\n  CHECK(v[0] == 1);", "v")

    def test_require_false_is_empty_then_index(self):
        self.assertSized("  REQUIRE_FALSE(v.is_empty());\n  CHECK(v[0] == 1);", "v")

    def test_comparison_macro_form(self):
        self.assertSized("  REQUIRE_EQ(v.size(), 4);\n  CHECK(v[3] == 1);", "v")

    def test_index_bound_assertion_then_index(self):
        """`CHECK(idx < splats.size()); splats[idx]` - test_lod_system.cpp:933."""
        self.assertSized("  CHECK(idx < (uint32_t)v.size());\n  const float d = v[idx].x;", "v")

    def test_member_chain(self):
        self.assertSized(
            "  REQUIRE(state.cached_counts.size() == 2);\n  CHECK(state.cached_counts[0] == 5);",
            "state.cached_counts",
        )

    def test_subscripted_chain_is_one_symbol(self):
        self.assertSized(
            "  CHECK_EQ(chunks[0].indices.size(), 2);\n  CHECK_EQ(chunks[0].indices[0], 0);",
            "chunks[0].indices",
        )

    def test_getter_call_chain(self):
        self.assertSized(
            "  CHECK_EQ(asset->get_ids().size(), n);\n  CHECK_EQ(asset->get_ids()[0], 7);",
            "asset->get_ids()",
        )

    def test_loop_bounded_by_a_LITERAL_is_still_dangerous(self):
        """`REQUIRE(v.size() == 4); for (i < 4) v[i]` crashes when the REQUIRE fails."""
        self.assertSized(
            "  REQUIRE(v.size() == 4);\n"
            "  for (uint32_t i = 0; i < 4; i++) {\n"
            "    CHECK(v[i] == i);\n"
            "  }",
            "v",
        )

    def test_index_after_an_own_size_bounded_loop_is_still_flagged(self):
        """The bound expires at the loop's closing brace - a stop-at-control-flow
        scanner would miss this, which is why a block STACK is used."""
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    CHECK(v[i] == i);\n"
            "  }\n"
            "  CHECK(v[0] == 1u);",
            "v",
        )

    def test_bound_from_a_different_container_does_not_make_it_safe(self):
        found = self.sites(
            "  REQUIRE(a.size() == 3);\n"
            "  REQUIRE(b.size() == 3);\n"
            "  for (uint32_t i = 0; i < a.size(); i++) {\n"
            "    CHECK(a[i] == b[i]);\n"
            "  }"
        )
        symbols = {site[1]: site[6] for site in found}
        self.assertIn("b", symbols, "b[i] crashes whenever b is the short one")
        self.assertEqual(symbols["b"], GUARD._CLASS_OTHER_BOUND)

    def test_index_inside_an_unrelated_if_is_flagged(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (other) {\n    CHECK(v[0] == 1);\n  }", "v"
        )

    def test_index_in_a_control_flow_header_is_flagged(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  if (v[0] == 1) {\n    f();\n  }", "v")


class SizeThenIndexTrueNegatives(SizeIndexScanTestCase):
    """A detector that flags the SAFE loop-bounded sites is worse than none.

    #844 counts 14 sites in the corpus whose index is bounded by the container's
    own size(). Every one of them must stay green, or the backlog becomes
    unreadable and the guard gets regenerated instead of read.
    """

    def test_loop_bounded_by_its_own_size_is_safe(self):
        self.assertNotSized(
            "  REQUIRE(opacities.size() == 4);\n"
            "  for (uint32_t i = 0; i < opacities.size(); i++) {\n"
            "    CHECK(Math::is_equal_approx(opacities[i], expected));\n"
            "  }"
        )

    def test_brace_less_loop_body_bounded_by_its_own_size_is_safe(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 4);\n"
            "  for (uint32_t i = 0; i < v.size(); i++)\n"
            "    CHECK(v[i] == i);"
        )

    def test_nested_block_inside_an_own_size_bounded_loop_is_safe(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 4);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    if (other) {\n"
            "      CHECK(v[i] == i);\n"
            "    }\n"
            "  }"
        )

    def test_if_guarded_by_its_own_size_is_safe(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n  if (v.size() > 0) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_asserting_empty_is_not_a_lower_bound(self):
        """`CHECK(v.is_empty())` failing makes v LONGER, not shorter."""
        self.assertNotSized("  CHECK(v.is_empty());\n  CHECK(v[0] == 5);")

    def test_asserting_size_zero_is_not_a_lower_bound(self):
        self.assertNotSized("  CHECK(v.size() == 0);\n  CHECK(v[0] == 5);")

    def test_upper_bound_only_is_not_a_lower_bound(self):
        self.assertNotSized("  CHECK(v.size() <= 256);\n  CHECK(v[0] == 5);")

    def test_vacuous_ge_zero_is_not_a_lower_bound(self):
        self.assertNotSized("  CHECK(v.size() >= 0);\n  CHECK(v[0] == 5);")

    def test_size_nested_in_another_call_is_not_a_predicate_on_it(self):
        """`REQUIRE(out.resize(ground_truth.size()) == OK)` says nothing about
        ground_truth's length."""
        self.assertNotSized(
            "  REQUIRE(out.resize(ground_truth.size()) == OK);\n"
            "  const Input &c = ground_truth[i];"
        )

    def test_a_different_container_is_not_the_asserted_one(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(other[0] == 1);")

    def test_a_longer_name_is_not_the_asserted_one(self):
        self.assertNotSized("  REQUIRE(keep.size() == 2);\n  CHECK(keep2[0] == 1);")

    def test_short_circuit_guard_dominates(self):
        self.assertNotSized(
            "  CHECK_EQ(chunks.size(), 2);\n  const bool ok = chunks.size() >= 2 && f(chunks[0]);"
        )

    def test_a_length_change_between_ends_the_scan(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  v.clear();\n  CHECK(v[0] == 1);")

    def test_a_reassignment_between_ends_the_scan(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  v = rebuild();\n  CHECK(v[0] == 1);")

    def test_no_index_at_all(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.get_total() == 4);")


class SizeThenIndexBoundDirection(SizeIndexScanTestCase):
    """A bound has a DIRECTION. Round-2 review of #849 (Codex) found three places
    that only checked whether a cardinality test was PRESENT.

    Each `assertSized` here was GREEN - reported clean - over a body that is
    reached exactly when the container is too short. A false negative in a guard
    whose whole purpose is finding these is the worst outcome available, so each
    one is pinned next to the safe shape it must not swallow.
    """

    def test_is_empty_header_does_not_bound_its_body(self):
        """`if (v.is_empty()) { v[0]; }` runs the index only when v is EMPTY."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (v.is_empty()) {\n    CHECK(v[0] == 1);\n  }", "v"
        )

    def test_not_is_empty_header_still_bounds_its_body(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n  if (!v.is_empty()) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_index_beyond_size_header_does_not_bound_its_body(self):
        """`if (i >= v.size()) { v[i]; }` selects exactly the out-of-range index."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (i >= v.size()) {\n    CHECK(v[i] == 1);\n  }", "v"
        )

    def test_index_below_size_header_alone_does_not_bound_its_body(self):
        """`if (i < v.size()) { v[i]; }` bounds nothing until `i` is known to be >= 0.

        Godot's `size()` is SIGNED (`CowData::Size` is int64_t), so a negative `i`
        satisfies this on an EMPTY container and the body indexes out of range
        (Codex, PR #849 round 4). Round 3 pinned the opposite - it assumed an
        unsigned count - which is why this case is spelled out on both sides.
        """
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (i < v.size()) {\n    CHECK(v[i] == 1);\n  }", "v"
        )

    def test_a_for_loop_starting_at_zero_still_bounds_its_body(self):
        """The counterpart: the initialiser is the proof `i` is not negative."""
        for header in (
            "for (uint32_t i = 0; i < v.size(); i++)",
            "for (int i = 0; i < v.size(); ++i)",
            "for (int i = 0; i < v.size(); i += 1)",
        ):
            with self.subTest(header=header):
                self.assertNotSized(
                    f"  REQUIRE(v.size() == 2);\n  {header} {{\n    CHECK(v[i] == 1);\n  }}"
                )

    def test_a_loop_counting_down_is_not_proven_nonnegative(self):
        """`i = v.size() - 1` is not a literal, and `i--` does not keep it >= 0."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = v.size() - 1; i >= 0; i--) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_a_loop_index_declared_elsewhere_is_not_proven_nonnegative(self):
        """Only the `for` initialiser is visible proof; a `while` header carries none."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  while (i < v.size()) {\n    CHECK(v[i] == 1);\n  }", "v"
        )

    def test_size_equal_zero_header_does_not_bound_its_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (v.size() == 0) {\n    CHECK(v[0] == 1);\n  }", "v"
        )

    def test_truthy_size_header_bounds_its_body(self):
        """`if (v.size())` IS a non-empty test; direction-checking must not lose it."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n  if (v.size()) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_a_size_handed_to_another_call_bounds_nothing(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (compute(v.size()) > 0) {\n    CHECK(v[0] == 1);\n  }",
            "v",
        )

    def test_switch_on_size_bounds_nothing(self):
        """A `switch` selector is not a boolean condition and bounds no index."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  switch (v.size()) {\n    CHECK(v[0] == 1);\n  }", "v"
        )

    def test_only_the_middle_clause_of_a_for_header_bounds(self):
        """The initializer and the increment are not the loop's condition."""
        self.assertSized(
            "  REQUIRE(v.size() == 4);\n"
            "  for (uint32_t i = v.size() - 1; i < 4; i--) {\n"
            "    CHECK(v[i] == i);\n"
            "  }",
            "v",
        )


class SizeThenIndexLoopUpdateMustBeProvenNondecreasing(SizeIndexScanTestCase):
    """The `for` update clause is judged by a WHITELIST, not by "is it a `--`".

    Round 4 accepted every update clause except a syntactic `--`/`-=` on the loop
    variable, so every shape it had not enumerated - `i += delta`, `i = -1`,
    `f(&i)` - kept the header's `i >= 0` proof and suppressed the subscript under
    it. With an actual length of 1, `REQUIRE(v.size() == 2)` fails, execution
    continues, `delta == -1` re-enters the body with `i == -1`, and `v[i]` aborts
    the whole batch while the detector reports clean (Codex, PR #849 round 7).

    The update clause is a path `_bound_after` never sees: round 6 closed the same
    hole for a rebinding in the loop BODY, and this one is in the header.
    """

    def test_an_unproven_step_does_not_bound_the_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i += delta) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_an_assignment_in_the_update_does_not_bound_the_body(self):
        """`i = -1` matches neither `--` nor `-=`, and was accepted for that reason."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i = -1) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_a_negative_literal_step_does_not_bound_the_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i += -1) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_an_update_that_takes_the_address_does_not_bound_the_body(self):
        """`advance(&i)` can write `i`; nothing here can see what it writes."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); advance(&i)) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_a_multiplying_update_is_not_on_the_whitelist(self):
        """Sound by accident is still unproven: only the listed forms may bound."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i *= 2) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_a_rebinding_from_another_name_does_not_bound_the_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i = j + 1) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_a_self_rebinding_that_SUBTRACTS_does_not_bound_the_body(self):
        """`i = i - 1` is `i--` spelled long; only the ADDING forms are on the list."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i = i - 1) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i = 1 - i) {\n    CHECK(v[i] == 1);\n  }",
            "v",
        )

    def test_the_proven_update_forms_still_bound_the_body(self):
        """The whitelist must not be vacuous: every real corpus spelling is on it."""
        for update in ("i++", "++i", "i += 1", "i += 4", "i = i + 1", "i = 1 + i", "i += 010"):
            with self.subTest(update=update):
                self.assertNotSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  for (int i = 0; i < v.size(); {update}) {{\n    CHECK(v[i] == 1);\n  }}"
                )

    def test_a_sibling_clause_is_judged_per_name(self):
        """`i++, j--` proves `i` and refutes `j`; one clause must not decide both."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0, j = 0; i < v.size(); i++, j--) {\n    CHECK(v[i] == 1);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0, j = 0; j < v.size(); i++, j--) {\n    CHECK(v[j] == 1);\n  }",
            "v",
        )

    def test_the_whitelist_is_asked_about_the_right_name(self):
        """Direct: only the listed forms answer True, and only for their own name."""
        self.assertTrue(GUARD._update_is_nondecreasing("i", "i++"))
        self.assertTrue(GUARD._update_is_nondecreasing("i", "++i"))
        self.assertTrue(GUARD._update_is_nondecreasing("i", "i += 2"))
        self.assertTrue(GUARD._update_is_nondecreasing("i", "j--"))  # does not touch `i`
        self.assertTrue(GUARD._update_is_nondecreasing("i", ""))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i--"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "--i"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i -= 2"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i += step"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i = -1"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i = f(i)"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "j += i"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i = i - 1"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i = 1 - i"))
        self.assertFalse(GUARD._update_is_nondecreasing("i", "i++, i -= 3"))

    def test_a_member_named_like_the_index_is_not_the_index(self):
        """`it.i++` writes a member, not the loop variable; the name must not match."""
        self.assertTrue(GUARD._update_is_nondecreasing("i", "it.i--"))
        self.assertTrue(GUARD._update_is_nondecreasing("i", "p->i--"))


class SizeThenIndexIntegerLiteralSpelling(SizeIndexScanTestCase):
    """C++ spelling rules, including the legacy octal one Python's `int(_, 0)` rejects.

    Every earlier finding on this file was the guard being too PERMISSIVE. This one
    is the opposite: `int("010", 0)` raises, so the safe
    `REQUIRE(v.size() == 9); if (v.size() >= 010) { CHECK(v[7]); }` was reported as
    a new violation although the branch proves eight elements (Codex, PR #849
    round 7). A ratchet that fails on valid input gets waived, and a waived guard
    proves nothing - but the fix is to read the literal CORRECTLY, never to let an
    unreadable one bound anything.
    """

    def test_a_legacy_octal_guard_bounds_its_body(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 010) {\n    CHECK(v[7] == 1);\n  }"
        )

    def test_a_legacy_octal_guard_bounds_only_as_far_as_its_value(self):
        """`010` is EIGHT, not ten: `v[8]` is past what the branch proves."""
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 010) {\n    CHECK(v[8] == 1);\n  }", "v"
        )

    def test_an_ill_formed_octal_literal_bounds_nothing(self):
        """`09` is not a literal in C++ either; unreadable must stay unproven."""
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 09) {\n    CHECK(v[7] == 1);\n  }", "v"
        )

    def test_an_octal_subscript_is_compared_by_value(self):
        """The index side is read by the same function: `v[07]` is `v[7]`."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 8) {\n    CHECK(v[07] == 1);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 8) {\n    CHECK(v[010] == 1);\n  }", "v"
        )

    def test_every_base_reads_as_c_plus_plus_reads_it(self):
        self.assertEqual(GUARD._literal_value("010"), 8)
        self.assertEqual(GUARD._literal_value("0"), 0)
        self.assertEqual(GUARD._literal_value("00"), 0)
        self.assertEqual(GUARD._literal_value("0755"), 493)
        self.assertEqual(GUARD._literal_value("010u"), 8)
        self.assertEqual(GUARD._literal_value("0'1'0"), 8)
        self.assertEqual(GUARD._literal_value("10"), 10)
        self.assertEqual(GUARD._literal_value("0x10"), 16)
        self.assertEqual(GUARD._literal_value("0X1F"), 31)
        self.assertEqual(GUARD._literal_value("0b101"), 5)
        self.assertEqual(GUARD._literal_value("0B11"), 3)
        self.assertIsNone(GUARD._literal_value("09"))
        self.assertIsNone(GUARD._literal_value("08"))
        self.assertIsNone(GUARD._literal_value("0xZ"))
        self.assertIsNone(GUARD._literal_value("expected"))
        self.assertIsNone(GUARD._literal_value("-1"))


class SizeThenIndexShortCircuitDirection(SizeIndexScanTestCase):
    """The same direction question, asked of a short-circuit operand.

    `_size_positive_test` matched on the OPERATOR and never on the VALUE, so an
    operand asserting the container is EMPTY counted as a guard for the index that
    follows it (Codex, PR #849 round 2).
    """

    def test_size_equals_zero_operand_is_not_a_guard(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() == 0 && v[0]);", "v")

    def test_size_not_equal_nonzero_operand_is_not_a_guard(self):
        """Zero satisfies `size() != 4`, so it cannot make `v[0]` reachable-only-if-safe."""
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() != 4 && v[0]);", "v")

    def test_size_at_most_operand_is_not_a_guard(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() <= 8 && v[0]);", "v")

    def test_size_not_equal_zero_operand_is_a_guard(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() != 0 && v[0]);")

    def test_size_at_least_operand_is_a_guard(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() >= 1 && v[0]);")

    def test_negative_operand_before_an_or_is_a_guard(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.is_empty() || v[0]);")


class SizeThenIndexCompoundConditions(SizeIndexScanTestCase):
    """A bound must be implied by the condition AS A WHOLE, not appear inside it.

    Round 3 (Codex, PR #849): `_expression_lower_bound` scanned for ANY cardinality
    test pointing the right way, so a bound sitting inside an `||` counted as one -
    although the other disjunct admits the index on an EMPTY container. Every
    `assertSized` below was reported clean before the expression was decomposed.
    """

    def test_or_with_an_unrelated_disjunct_does_not_bound_a_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (v.size() > 0 || fallback) {\n    CHECK(v[0] == 1);\n  }",
            "v",
        )

    def test_or_with_an_unrelated_disjunct_is_not_a_short_circuit_guard(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  CHECK((v.size() > 0 || fallback) && v[0]);", "v"
        )

    def test_every_disjunct_bounding_still_bounds(self):
        """`||` is not banned - it just has to bound on EVERY side."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (v.size() > 4 || v.size() == 2) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_one_bounding_conjunct_is_enough(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (fallback && v.size() > 0) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_a_ternary_condition_bounds_only_when_both_arms_do(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (fallback ? v.size() > 0 : other) {\n    CHECK(v[0] == 1);\n  }",
            "v",
        )
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (fallback ? v.size() > 0 : !v.is_empty()) {\n    CHECK(v[0] == 1);\n  }"
        )

    def test_a_negated_size_is_an_EMPTY_test(self):
        """`if (!v.size())` is entered exactly when the container is empty."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (!v.size()) {\n    CHECK(v[0] == 1);\n  }", "v"
        )

    def test_a_negated_emptiness_test_still_bounds(self):
        for condition in ("!v.is_empty()", "!(v.size() == 0)", "!(v.is_empty())"):
            with self.subTest(condition=condition):
                self.assertNotSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  if ({condition}) {{\n    CHECK(v[0] == 1);\n  }}"
                )

    def test_an_atom_built_from_a_bound_is_not_a_bound(self):
        """Round 3 decomposed `&&`, `||` and `?:` - and then scanned whatever was
        left for a qualifying `size()` test. An atom can still be BUILT from one
        without following its truth: `(v.size() > 0) == expected_nonempty` is true
        exactly when `v` is EMPTY once `expected_nonempty` is false (Codex, PR #849
        round 4). Each of these scanned clean before the atom was read as a whole.
        """
        for condition in (
            "(v.size() > 0) == expected_nonempty",
            "(v.size() > 0) != expected_empty",
            "expected_nonempty == (v.size() > 0)",
            "flag ^ (v.size() > 0)",
            "static_cast<int>(v.size() > 0) + offset",
            "v.size() - 1 > 0",
            "count(v.size()) > 0",
        ):
            with self.subTest(condition=condition):
                self.assertSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  if ({condition}) {{\n    CHECK(v[0] == 1);\n  }}",
                    "v",
                )

    def test_an_atom_built_from_a_bound_is_not_a_short_circuit_guard(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(((v.size() > 0) == expected_nonempty) && v[0]);",
            "v",
        )

    def test_a_declaration_in_front_of_a_bound_is_not_structure(self):
        """`ok = A` is true exactly when `A` is, so the assignment must not hide it."""
        self.assertNotSized(
            "  CHECK_EQ(chunks.size(), 2);\n"
            "  const bool ok = chunks.size() >= 2 && f(chunks[0]);"
        )


class SizeThenIndexEqualityOperandMustBeProven(SizeIndexScanTestCase):
    """`size() == n` bounds a BODY only when `n` is provably non-zero.

    Round 3 (Codex, PR #849): a non-literal operand was assumed non-zero, so
    `if (v.size() == expected) { CHECK(v[0]); }` scanned clean - yet with
    `expected == 0` that branch is entered exactly when `v` is empty.
    """

    def test_equality_against_an_unknown_does_not_bound_a_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (v.size() == expected) {\n    CHECK(v[0] == 1);\n  }",
            "v",
        )

    def test_at_least_an_unknown_does_not_bound_a_body(self):
        """`size() >= n` is vacuous at `n == 0`, the same hole as `==`."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (v.size() >= expected) {\n    CHECK(v[0] == 1);\n  }",
            "v",
        )

    def test_equality_against_an_unknown_is_not_a_short_circuit_guard(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  CHECK(v.size() == expected && v[0]);", "v"
        )

    def test_a_provable_operand_still_bounds(self):
        for condition in ("v.size() == 2", "v.size() == 2u", "v.size() == 0x2", "v.size() >= 1"):
            with self.subTest(condition=condition):
                self.assertNotSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  if ({condition}) {{\n    CHECK(v[0] == 1);\n  }}"
                )

    def test_a_strictly_greater_comparison_needs_a_nonnegative_operand(self):
        """`size() > n` is a lower bound only when `n` itself is at least zero.

        Round 3 accepted any `>`, reasoning that a length above a count is at least
        one. The count is not unsigned: `Vector::size()` returns `CowData`'s
        int64_t, so `if (v.size() > -1) { v[0]; }` is entered on an EMPTY container
        and scanned clean (Codex, PR #849 round 4).
        """
        for condition in ("v.size() > expected", "v.size() > -1", "v.size() > kOffset - 1"):
            with self.subTest(condition=condition):
                self.assertSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  if ({condition}) {{\n    CHECK(v[0] == 1);\n  }}",
                    "v",
                )

    def test_a_nonnegative_literal_still_bounds_strictly_greater(self):
        for condition in ("v.size() > 0", "v.size() > 1", "v.size() > 0x0", "0 < v.size()"):
            with self.subTest(condition=condition):
                self.assertNotSized(
                    "  REQUIRE(v.size() == 2);\n"
                    f"  if ({condition}) {{\n    CHECK(v[0] == 1);\n  }}"
                )

    def test_an_unproven_strict_comparison_still_asserts_as_an_assertion(self):
        """The same asymmetry as `==`: unproven REPORTS, and only SUPPRESSING needs
        the proof. `REQUIRE(v.size() > n)` is still a size assertion above `v[0]`."""
        self.assertSized("  REQUIRE(v.size() > n);\n  CHECK(v[0] == 1);", "v")

    def test_an_assertion_against_an_unknown_is_still_a_size_assertion(self):
        """The asymmetry is the point: unproven REPORTS as an assertion and must
        not SUPPRESS as a guard. Both directions are fail-closed."""
        self.assertSized("  CHECK_EQ(v.size(), n);\n  CHECK(v[0] == 1);", "v")

    def test_a_sibling_conjunct_can_prove_the_operand(self):
        """test_gaussian_importer.h:2930 - the equality is proven by its neighbour."""
        self.assertNotSized(
            "  CHECK_EQ(v.size(), a.size());\n"
            "  if (!a.is_empty() && v.size() == a.size()) {\n    CHECK(v[0] == a[0]);\n  }"
        )

    def test_without_the_sibling_the_same_equality_proves_nothing(self):
        self.assertSized(
            "  CHECK_EQ(v.size(), a.size());\n"
            "  if (v.size() == a.size()) {\n    CHECK(v[0] == a[0]);\n  }",
            "v",
        )

    def test_an_offset_equality_is_not_a_transfer(self):
        """`v.size() == a.size() - 1` says nothing about `v` even when `a` is bounded."""
        self.assertSized(
            "  CHECK_EQ(v.size(), a.size());\n"
            "  if (!a.is_empty() && v.size() == a.size() - 1) {\n    CHECK(v[0] == a[0]);\n  }",
            "v",
        )


class SizeThenIndexObjectResolution(SizeIndexScanTestCase):
    """Which CONTAINER a `.size()` belongs to.

    The forward regex grammar could not consume `chunks[order[0]].indices`, and
    Python's engine answered by backtracking to the longest tail it COULD consume -
    the bare member name `indices`. The detector then compared names instead of
    tracking one container and reported an unrelated `other.indices[0]`
    (Codex, PR #849 round 2).
    """

    def test_a_nested_subscript_is_not_tracked_by_its_member_name(self):
        self.assertNotSized(
            "  REQUIRE(chunks[order[0]].indices.size() == 2);\n  CHECK(other.indices[0] == 1);"
        )

    def test_a_nested_subscript_is_tracked_as_a_whole_symbol(self):
        self.assertSized(
            "  REQUIRE(chunks[order[0]].indices.size() == 2);\n"
            "  CHECK(chunks[order[0]].indices[0] == 1);",
            "chunks[order[0]].indices",
        )

    def test_a_call_with_arguments_is_resolved_as_the_object(self):
        found = self.sites(
            "  REQUIRE(!importer->get_preset_name(i).is_empty());\n"
            "  CHECK(importer->get_preset_name(i)[0] == 'x');"
        )
        self.assertEqual([site[1] for site in found], ["importer->get_preset_name(i)"])

    def test_a_call_with_arguments_on_an_unrelated_object_is_not_the_same_symbol(self):
        self.assertNotSized(
            "  REQUIRE(!importer->get_preset_name(i).is_empty());\n"
            "  CHECK(other->get_preset_name(i)[0] == 'x');"
        )

    def test_a_cast_before_the_container_is_not_part_of_it(self):
        """`CHECK(idx < (uint32_t)splats.size())` - test_lod_system.cpp, a real site."""
        found = self.sites("  CHECK(idx < (uint32_t)splats.size());\n  const float d = splats[idx].x;")
        self.assertEqual([site[1] for site in found], ["splats"])

    def test_a_static_call_object_keeps_its_qualification(self):
        found = self.sites(
            "  REQUIRE(!Path::get_source(asset).is_empty());\n"
            "  CHECK(Path::get_source(asset)[0] == 'r');"
        )
        self.assertEqual([site[1] for site in found], ["Path::get_source(asset)"])

    def test_an_object_that_is_not_an_expression_is_a_scan_error(self):
        """`(a + b).size()` has no container to track, so it cannot be called clean."""
        with self.assertRaises(GUARD.ScanError):
            self.sites("  REQUIRE((a + b).size() == 2);\n  CHECK(v[0] == 1);")


class SizeThenIndexSameLineBlockBodies(SizeIndexScanTestCase):
    """A body that shares its header's line is still a body (Codex, PR #849 round 5).

    `_statements()` emits `if (v.is_empty()) { CHECK(v[0]); }` as ONE statement, and
    the analyser truncated it at `{` to get the header - discarding the body and
    never revisiting it. So the branch that indexes PRECISELY when the container is
    empty, the exact shape this detector exists to catch, was reported clean. A
    one-line block must produce the same verdict as the same code with a line break.
    """

    def test_a_one_line_if_body_is_scanned(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                body = "  REQUIRE(v.size() == 2);\n  if (v.is_empty()) { CHECK(v[0]); }"
                self.assertSized(body.replace("\n", eol) if eol != "\n" else body, "v")

    def test_a_one_line_body_matches_the_multi_line_spelling(self):
        one = self.sites("  REQUIRE(v.size() == 2);\n  if (flag) { CHECK(v[0]); }")
        many = self.sites("  REQUIRE(v.size() == 2);\n  if (flag) {\n    CHECK(v[0]);\n  }")
        self.assertEqual(len(one), len(many), "the line break must not change the verdict")
        self.assertEqual(one[0][1:4], many[0][1:4])
        self.assertEqual(one[0][5], many[0][5])
        self.assertEqual(one[0][6], many[0][6])

    def test_a_one_line_loop_bounded_by_the_container_stays_clean(self):
        """The sound transfer still applies: the header's own bound guards the body."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n  for (int i = 0; i < v.size(); i++) { CHECK(v[i]); }"
        )

    def test_a_one_line_loop_bounded_by_another_container_is_classified(self):
        found = self.sites(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < other.size(); i++) { CHECK(v[i]); }"
        )
        self.assertEqual([site[6] for site in found], [GUARD._CLASS_OTHER_BOUND])

    def test_a_one_line_block_does_not_leak_its_bound_to_the_next_statement(self):
        """The other half of the same defect: a one-line block never opened a frame,
        so its bound was applied as `pending` to whatever came AFTER it."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < v.size(); i++) { use(i); }\n"
            "  CHECK(v[0]);",
            "v",
        )

    def test_nested_one_line_blocks(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (flag) { for (int i = 0; i < n; i++) { CHECK(v[i]); } }",
            "v",
        )

    def test_a_one_line_else_branch_is_scanned(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (flag) { use(); } else { CHECK(v[0]); }", "v"
        )

    def test_a_one_line_while_body_is_scanned(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  while (busy) { CHECK(v[0]); }", "v")

    def test_an_initializer_list_in_the_body_does_not_unbalance_the_stack(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (flag) { Vector<int> t = { 1, 2 }; CHECK(v[0]); }",
            "v",
        )

    def test_a_lambda_in_the_body_does_not_unbalance_the_stack(self):
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n  if (flag) { run([&]{ use(); }); CHECK(v[0]); }",
            "v",
        )

    def test_a_bare_initializer_statement_is_not_split(self):
        """Only control flow is expanded. Splitting every `{` would push a frame for
        an aggregate initializer, and the next `}` would then pop the wrong block."""
        self.assertEqual(GUARD._inline_pieces("Vector<int> t = { 1, 2 };"), ["Vector<int> t = { 1, 2 };"])

    def test_the_scan_window_still_counts_source_statements(self):
        """Expanding at the CALL site would spend the six-statement window on the
        pieces of one line: five one-line blocks would hide the sixth statement."""
        self.assertSized(
            "  REQUIRE(v.size() == 2);\n"
            "  if (a) { p(); }\n  if (b) { p(); }\n  if (c) { p(); }\n"
            "  if (d) { p(); }\n  if (e) { p(); }\n"
            "  CHECK(v[0]);",
            "v",
        )


class NullDerefLayoutInvariance(ScanTestCase):
    """Detector 1 reads the same atoms detector 2 does (round 8).

    `_scan_forward` stops at a block boundary on purpose - it cannot tell what a
    body's condition guarantees. But `_CONTROL_FLOW_RE` is a PREFIX test, and
    `_statements()` emits groups, so a block whose close shares its last
    statement's line (`use(); }`) did not read as a boundary and the scan walked
    out into the enclosing scope - reaching a verdict the fully expanded spelling
    of the same code never reaches. Layout must not change the verdict, in either
    detector.
    """

    ONE_LINE_CLOSE = (
        "  if (flag) {\n    REQUIRE(ptr != nullptr);\n    use(); }\n  ptr->f();"
    )
    EXPANDED = (
        "  if (flag) {\n    REQUIRE(ptr != nullptr);\n    use();\n  }\n  ptr->f();"
    )

    def test_a_block_close_sharing_a_statements_line_still_ends_the_scan(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                compact = [v[1:] for v in self.scan(self.ONE_LINE_CLOSE.replace("\n", eol))]
                expanded = [v[1:] for v in self.scan(self.EXPANDED.replace("\n", eol))]
                self.assertEqual(compact, expanded)

    def test_a_deref_still_inside_the_block_is_flagged_either_way(self):
        """The scan must not become inert: the shape it exists for still reports."""
        self.assertFlagged(
            "  if (flag) {\n    REQUIRE(ptr != nullptr);\n    ptr->f(); }", "ptr"
        )


class StatementAtomsAreTotal(unittest.TestCase):
    """`_statement_atoms` must leave nothing for a consumer to re-parse (round 8).

    Rounds 5 and 8 are the same defect twice: `_statements()` emits a line-oriented
    GROUP, a consumer re-parsed it with a prefix or suffix test, and the test did
    not cover one layout - a body sharing its header's line (round 5), then a
    closing brace sharing the body's last statement's line (round 8). Both went
    SILENT over a live crash.

    A third rule would have been a third guess. What is pinned instead is a
    PROPERTY, checked over every group the real corpus produces: decomposition is
    IDEMPOTENT. If `_statement_atoms(atom) != [atom]` for any atom, there is still
    something splittable in what a consumer is being handed, and that is the
    precondition for the next instance of this class. A list of layouts would have
    to be extended for a layout nobody thought of; this property does not.
    """

    def test_decomposition_is_idempotent_over_the_real_corpus(self):
        examined = 0
        for path in GUARD._test_sources():
            lines = GUARD._strip_comments(GUARD._read_source(path)).splitlines()
            for index in range(len(lines)):
                logical, last = GUARD._logical_line(lines, index)
                groups = list(GUARD._line_fragments(logical))
                groups += [text for _line, text in GUARD._statements(lines, last + 1, 6)]
                for group in groups:
                    for atom in GUARD._statement_atoms(group):
                        examined += 1
                        self.assertEqual(
                            GUARD._statement_atoms(atom),
                            [atom],
                            f"{path.name}: atom {atom!r} is still splittable",
                        )
        self.assertGreater(examined, 10000, "the corpus sweep examined too little to mean anything")

    def test_a_closing_brace_is_its_own_atom(self):
        self.assertEqual(GUARD._statement_atoms("CHECK(v[0]); }"), ["CHECK(v[0]);", "}"])

    def test_a_block_open_is_its_own_atom(self):
        self.assertEqual(GUARD._statement_atoms("if (a) {"), ["if (a) {"])

    def test_compacted_statements_split(self):
        self.assertEqual(GUARD._statement_atoms("a(); b();"), ["a();", "b();"])

    def test_a_balanced_initializer_is_not_split(self):
        self.assertEqual(
            GUARD._statement_atoms("Vector<int> t = { 1, 2 };"), ["Vector<int> t = { 1, 2 };"]
        )

    def test_a_for_header_semicolon_is_not_a_statement_boundary(self):
        self.assertEqual(
            GUARD._statement_atoms("for (int i = 0; i < n; i++) {"),
            ["for (int i = 0; i < n; i++) {"],
        )


class SizeThenIndexTrailingBlockClose(SizeIndexScanTestCase):
    """A `}` sharing the body's last statement's line still closes the block (round 8).

    `_statements()` emits `CHECK(v[0]); }` as one group, which does not START with
    `}`, so the block frame was never popped and the header's bound leaked onto
    every statement after the block:

        REQUIRE(v.size() == 3);
        if (v.size() >= 2) {
            CHECK(v[0]); }
        CHECK(v[1]);          // reported CLEAN on the `>= 2` bound that had ended

    An actual length of 1 fails the assertion, skips the branch, and aborts the
    process at `v[1]` (Codex, PR #849 round 8).
    """

    LEAKED = (
        "  REQUIRE(v.size() == 3);\n"
        "  if (v.size() >= 2) {\n"
        "    CHECK(v[0] == 1); }\n"
        "  CHECK(v[1] == 2);"
    )
    EXPANDED = (
        "  REQUIRE(v.size() == 3);\n"
        "  if (v.size() >= 2) {\n"
        "    CHECK(v[0] == 1);\n"
        "  }\n"
        "  CHECK(v[1] == 2);"
    )

    def test_the_bound_expires_at_a_trailing_brace(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                self.assertSized(self.LEAKED.replace("\n", eol), "v")

    def test_the_verdict_matches_the_fully_expanded_spelling(self):
        leaked = self.sites(self.LEAKED)
        expanded = self.sites(self.EXPANDED)
        self.assertEqual(len(leaked), len(expanded), "layout must not change the verdict")
        self.assertEqual(leaked[0][1:4], expanded[0][1:4])
        self.assertEqual(leaked[0][5], expanded[0][5])
        self.assertEqual(leaked[0][6], expanded[0][6])

    def test_the_bound_still_covers_the_body(self):
        """The sound transfer must survive: inside the block, `v[0]` IS proven."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (v.size() >= 2) {\n"
            "    CHECK(v[0] == 1); }"
        )

    def test_a_whole_block_closed_on_one_line_pops_once(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) { use(i); }\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_a_nested_block_closed_on_its_last_statement_pops_both(self):
        self.assertSized(
            "  REQUIRE(v.size() == 4);\n"
            "  if (a) {\n"
            "    if (v.size() >= 3) {\n"
            "      use(); } }\n"
            "  CHECK(v[2]);",
            "v",
        )

    def test_an_initializer_ending_the_line_does_not_pop(self):
        """`};` closes an aggregate initializer, not the enclosing block. Popping on
        it would end the loop's bound early and REPORT a proven-safe subscript."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    Vector<int> t = { 1, 2 };\n"
            "    CHECK(v[i]);\n"
            "  }"
        )


class SizeThenIndexGroupingParentheses(SizeIndexScanTestCase):
    """A grouping pair is not a nesting level (Codex, PR #849 round 8).

    `_size_assertions` required the cardinality call at parenthesis DEPTH 0 inside
    the macro, to reject `REQUIRE(out.resize(g.size()) == OK)` - which constrains
    the resize result, not `g`. But a depth test cannot tell an argument from a
    grouping pair, so the harmless `REQUIRE((v.size() == 2))` was read as an
    assertion with no size predicate and the `v[0]` after it was not a site.

    The distinction is pinned BOTH ways here: grouping must be peeled, a call must
    still be rejected.
    """

    def test_a_grouped_predicate_is_still_an_assertion(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                self.assertSized(
                    "  REQUIRE((v.size() == 2));\n  CHECK(v[0] == 1);".replace("\n", eol), "v"
                )

    def test_repeated_grouping_is_peeled(self):
        self.assertSized("  REQUIRE(((v.size() == 2)));\n  CHECK(v[0] == 1);", "v")

    def test_a_grouped_negated_emptiness_is_still_an_assertion(self):
        self.assertSized("  REQUIRE((!v.is_empty()));\n  CHECK(v[0] == 1);", "v")

    def test_a_size_handed_to_another_call_is_still_rejected(self):
        self.assertNotSized("  REQUIRE(f(v.size()) == 2);\n  CHECK(v[0] == 1);")

    def test_a_resize_argument_is_still_rejected(self):
        """The shape the depth test existed for: the assertion constrains the
        resize RESULT and says nothing about `v`."""
        self.assertNotSized("  REQUIRE(out.resize(v.size()) == OK);\n  CHECK(v[0] == 1);")

    def test_a_size_inside_a_grouped_call_argument_is_still_rejected(self):
        self.assertNotSized("  REQUIRE((f(v.size()) == 2));\n  CHECK(v[0] == 1);")

    def test_a_subscripted_size_is_still_rejected(self):
        self.assertNotSized("  REQUIRE(table[v.size()] == 2);\n  CHECK(v[0] == 1);")

    def test_a_grouped_disjunct_reports_rather_than_going_silent(self):
        """`REQUIRE(a || (v.size() == 2))` asserts no bound on `v` at all - which is
        the REPORTING direction for an assertion, not a reason to stay quiet."""
        self.assertSized("  REQUIRE(a || (v.size() == 2));\n  CHECK(v[0] == 1);", "v")


class DoctestMacroFamilyIsDerived(SizeIndexScanTestCase):
    """The negating / relational macro family is READ from the header (round 8).

    `macro.endswith("_FALSE")` is a hand-written list in disguise, and it was wrong
    the way such a list always is: doctest's real `REQUIRE_FALSE_MESSAGE` and
    `CHECK_FALSE_MESSAGE` do not end in `_FALSE`, so a negated `is_empty()` under
    them established no bound and the index after it was not a site. The corpus
    writes those two spellings 37 times.

    The companion suffix table had the mirror-image defect: it consulted
    `*_EQ_MESSAGE`, which doctest does not define at all - the same nonexistent
    spelling mistake round 1 made with `REQUIRE_FALSE_UNARY_FALSE`.
    """

    def _header(self) -> str:
        return GUARD.DOCTEST_HEADER.read_text(encoding="utf-8", errors="replace")

    def test_every_derived_macro_is_defined_by_the_header(self):
        header = self._header()
        macros = GUARD._doctest_assert_macros()
        self.assertGreater(len(macros), 20, "the derivation found implausibly few macros")
        for name in macros:
            self.assertIn(f"define {name}", header, f"{name} is not defined by doctest")

    def test_the_derived_negating_family_is_exactly_the_header_s(self):
        """Cross-check the semantic derivation (`return !(...)`) against the header's
        own naming, so neither can drift without this failing."""
        macros = GUARD._doctest_assert_macros()
        derived = {n for n, s in macros.items() if s.negated and not n.startswith("DOCTEST_")}
        by_name = {
            n for n in macros
            if not n.startswith("DOCTEST_") and ("_FALSE" in f"{n}_" or n.endswith("_FALSE"))
        }
        self.assertEqual(derived, by_name)
        self.assertIn("REQUIRE_FALSE_MESSAGE", derived)
        self.assertIn("CHECK_FALSE_MESSAGE", derived)
        self.assertIn("REQUIRE_UNARY_FALSE", derived)
        self.assertIn("FAST_REQUIRE_UNARY_FALSE", derived)

    def test_the_message_spellings_do_not_end_in_FALSE(self):
        """The exact reason the old spelling test failed."""
        self.assertFalse("REQUIRE_FALSE_MESSAGE".endswith("_FALSE"))
        self.assertTrue(GUARD._macro_semantics("REQUIRE_FALSE_MESSAGE").negated)

    def test_doctest_has_no_relational_MESSAGE_macros(self):
        """The retired suffix table consulted `*_EQ_MESSAGE`; no such macro exists."""
        header = self._header()
        for suffix in ("EQ", "NE", "GT", "GE", "LT", "LE"):
            self.assertNotIn(f"define DOCTEST_REQUIRE_{suffix}_MESSAGE", header)
        self.assertEqual(GUARD._macro_semantics("REQUIRE_EQ").relation, "==")
        self.assertEqual(GUARD._macro_semantics("CHECK_LT").relation, "<")

    def test_an_unknown_macro_reads_as_plain(self):
        """`test_macros.h` defines project-local `REQUIRE_*` wrappers; they carry
        neither a relation nor a negation."""
        self.assertEqual(GUARD._macro_semantics("REQUIRE_GPU_DEVICE"), GUARD._PLAIN_MACRO)

    def test_a_project_macro_merely_ENDING_in_EQ_does_not_borrow_the_relation(self):
        """This is where the suffix table was not just untidy but SILENT.

        A project-local `CHECK_SIZES_EQ(v.size(), 0)` is a macro of unknown meaning.
        The suffix table read `_EQ` off its name, concluded `size() == 0`, and
        suppressed the following `v[0]`. Derived, the macro is unrecognised, so the
        predicate is a bare cardinality test and the index after it is REPORTED -
        the fail-closed direction for something the guard cannot read.
        """
        self.assertSized("  CHECK_SIZES_EQ(v.size(), 0);\n  CHECK(v[0]);", "v")
        self.assertNotSized("  CHECK_EQ(v.size(), 0);\n  CHECK(v[0]);")

    def test_derivation_fails_closed_when_the_header_is_unreadable(self):
        original = GUARD.DOCTEST_HEADER
        GUARD._doctest_assert_macros.cache_clear()
        try:
            GUARD.DOCTEST_HEADER = Path(tempfile.gettempdir()) / "no_such_doctest_header.h"
            with self.assertRaises(GUARD.ScanError):
                GUARD._doctest_assert_macros()
        finally:
            GUARD.DOCTEST_HEADER = original
            GUARD._doctest_assert_macros.cache_clear()

    def test_derivation_fails_closed_when_the_block_is_gutted(self):
        """A header that parses to no negating macro must FAIL, not answer 'doctest
        has none' - which would quietly turn every REQUIRE_FALSE into a positive
        assertion and unreport its sites."""
        original = GUARD.DOCTEST_HEADER
        GUARD._doctest_assert_macros.cache_clear()
        with tempfile.TemporaryDirectory() as tmp:
            gutted = Path(tmp) / "doctest.h"
            gutted.write_text(
                "\n".join(
                    [
                        "    DOCTEST_RELATIONAL_OP(eq, ==)",
                        "    DOCTEST_RELATIONAL_OP(ne, !=)",
                        "    DOCTEST_RELATIONAL_OP(lt, <)",
                        "    DOCTEST_RELATIONAL_OP(gt, >)",
                        "    DOCTEST_RELATIONAL_OP(le, <=)",
                        "    DOCTEST_RELATIONAL_OP(ge, >=)",
                        "#define DOCTEST_REQUIRE(...) [&] { return __VA_ARGS__; }()",
                        "#define DOCTEST_REQUIRE_EQ(...) [&] { return doctest::detail::eq(__VA_ARGS__); }()",
                        "#define DOCTEST_REQUIRE_NE(...) [&] { return doctest::detail::ne(__VA_ARGS__); }()",
                        "#define DOCTEST_REQUIRE_LT(...) [&] { return doctest::detail::lt(__VA_ARGS__); }()",
                        "#define DOCTEST_REQUIRE_GT(...) [&] { return doctest::detail::gt(__VA_ARGS__); }()",
                        "#define DOCTEST_REQUIRE_LE(...) [&] { return doctest::detail::le(__VA_ARGS__); }()",
                        "#define DOCTEST_REQUIRE_GE(...) [&] { return doctest::detail::ge(__VA_ARGS__); }()",
                        "#define REQUIRE(...) DOCTEST_REQUIRE(__VA_ARGS__)",
                        "",
                    ]
                ),
                encoding="utf-8",
            )
            try:
                GUARD.DOCTEST_HEADER = gutted
                with self.assertRaises(GUARD.ScanError):
                    GUARD._doctest_assert_macros()
            finally:
                GUARD.DOCTEST_HEADER = original
                GUARD._doctest_assert_macros.cache_clear()


class SizeThenIndexNegatingMacros(SizeIndexScanTestCase):
    """A negating macro asserts the COMPLEMENT, and the direction follows (round 8).

    Both directions are pinned. `REQUIRE_FALSE(v.is_empty())` and
    `CHECK_FALSE_MESSAGE(v.is_empty(), "...")` assert NON-empty, so what follows is
    a site; `REQUIRE_FALSE(!v.is_empty())` and `REQUIRE_FALSE(v.size())` assert
    EMPTY, so they bound nothing and reporting them would name the wrong assertion.
    """

    def test_require_false_message_on_is_empty_is_a_bound(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                self.assertSized(
                    '  REQUIRE_FALSE_MESSAGE(v.is_empty(), "must have data");\n'
                    "  CHECK(v[0] == 1);".replace("\n", eol),
                    "v",
                )

    def test_check_false_message_on_is_empty_is_a_bound(self):
        self.assertSized(
            '  CHECK_FALSE_MESSAGE(v.is_empty(), "must have data");\n  CHECK(v[0] == 1);', "v"
        )

    def test_the_message_spelling_matches_the_plain_one(self):
        plain = self.sites("  REQUIRE_FALSE(v.is_empty());\n  CHECK(v[0] == 1);")
        message = self.sites(
            '  REQUIRE_FALSE_MESSAGE(v.is_empty(), "must have data");\n  CHECK(v[0] == 1);'
        )
        self.assertEqual(len(plain), len(message))
        self.assertEqual(plain[0][1], message[0][1])
        self.assertEqual(plain[0][5], message[0][5])

    def test_a_doubly_negated_emptiness_bounds_nothing(self):
        """`REQUIRE_FALSE(!v.is_empty())` asserts the container IS empty."""
        self.assertNotSized("  REQUIRE_FALSE(!v.is_empty());\n  CHECK(v[0] == 1);")

    def test_a_negated_truthiness_bounds_nothing(self):
        """`REQUIRE_FALSE(v.size())` asserts `size() == 0`."""
        self.assertNotSized("  REQUIRE_FALSE(v.size());\n  CHECK(v[0] == 1);")

    def test_a_negated_equality_to_zero_is_a_bound(self):
        """`CHECK_FALSE(v.size() == 0)` asserts `size() != 0` - non-empty."""
        self.assertSized("  CHECK_FALSE(v.size() == 0);\n  CHECK(v[0] == 1);", "v")

    def test_a_negated_upper_bound_is_a_bound(self):
        """`REQUIRE_FALSE(v.size() <= 2)` asserts `size() > 2`."""
        self.assertSized("  REQUIRE_FALSE(v.size() <= 2);\n  CHECK(v[0] == 1);", "v")

    def test_a_negated_lower_bound_bounds_nothing(self):
        """`REQUIRE_FALSE(v.size() >= 3)` asserts `size() < 3` - an UPPER bound."""
        self.assertNotSized("  REQUIRE_FALSE(v.size() >= 3);\n  CHECK(v[0] == 1);")

    def test_a_negated_nonempty_test_bounds_nothing(self):
        """`CHECK_FALSE(v.size() != 0)` asserts the container empty."""
        self.assertNotSized("  CHECK_FALSE(v.size() != 0);\n  CHECK(v[0] == 1);")

    def test_a_plain_macro_is_unaffected(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);", "v")
        self.assertNotSized("  REQUIRE(v.size() == 0);\n  CHECK(v[0] == 1);")


class SizeThenIndexBoundMustReachTheIndex(SizeIndexScanTestCase):
    """A bound on the CONTAINER is not a bound on every subscript of it (round 6).

    Until this, a guard's answer was a boolean - "the container is non-empty" - and
    any lower bound suppressed every index under it. So
    `REQUIRE(v.size() == 2); if (!v.is_empty()) { CHECK(v[1]); }` reported clean
    although a container of length 1 fails the assertion, enters the branch and
    aborts the whole batch on `v[1]`. Every case here pins the RELATION between the
    proven bound and the specific index expression, in both directions: the shapes
    the magnitude must now catch, and the ones it must still leave alone.
    """

    # --- a constant subscript against a proven minimum length -------------------

    def test_a_constant_subscript_above_a_nonemptiness_guard_is_flagged(self):
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                body = (
                    "  REQUIRE(v.size() == 2);\n"
                    "  if (!v.is_empty()) {\n    CHECK(v[1] == 3);\n  }"
                )
                self.assertSized(body.replace("\n", eol), "v")

    def test_index_zero_under_a_nonemptiness_guard_stays_clean(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 2);\n  if (!v.is_empty()) {\n    CHECK(v[0] == 3);\n  }"
        )

    def test_a_minimum_of_four_covers_index_three_and_not_index_four(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 4) {\n    CHECK(v[3] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() >= 4) {\n    CHECK(v[4] == 3);\n  }",
            "v",
        )

    def test_a_strict_greater_than_proves_one_more(self):
        """`size() > 3` means at least four elements, so index 3 is the last safe one."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() > 3) {\n    CHECK(v[3] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() > 3) {\n    CHECK(v[4] == 3);\n  }",
            "v",
        )

    def test_an_equality_guard_proves_exactly_its_operand(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() == 2) {\n    CHECK(v[1] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n  if (v.size() == 2) {\n    CHECK(v[2] == 3);\n  }",
            "v",
        )

    def test_the_literal_is_read_in_its_own_base(self):
        """`_literal_value` parses C++ spelling, so a hex bound is a real bound."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 99);\n  if (v.size() >= 0x10) {\n    CHECK(v[15] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 99);\n  if (v.size() >= 0x10) {\n    CHECK(v[16] == 3);\n  }",
            "v",
        )

    # --- a loop index against the loop's own bound -------------------------------

    def test_a_loop_bounds_its_index_and_not_an_offset_of_it(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[i] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[i + 1] == 3);\n  }",
            "v",
        )
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[i - 1] == 3);\n  }",
            "v",
        )

    def test_a_loop_does_not_bound_an_unrelated_index(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[j] == 3);\n  }",
            "v",
        )

    def test_a_loop_over_the_container_still_proves_it_is_non_empty(self):
        """`i >= 0` and `i < v.size()` together mean the length is at least 1, so a
        literal `v[0]` inside the body is covered even though it is not the index."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[0] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[1] == 3);\n  }",
            "v",
        )

    def test_casts_and_parentheses_do_not_change_a_subscript(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n    CHECK(v[(uint32_t)i] == 3);\n  }"
        )
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n  if (!v.is_empty()) {\n    CHECK(v[( 0 )] == 3);\n  }"
        )

    # --- the last-element idiom ---------------------------------------------------

    def test_the_last_element_idiom_is_covered_by_non_emptiness(self):
        """`v[v.size() - 1]` is in range exactly when `v` is non-empty, which is what
        `if (!v.is_empty())` proves. The corpus writes this at
        test_gaussian_importer.h:2933 and it must NOT be reported."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (!v.is_empty()) {\n    CHECK(v[v.size() - 1] == 3);\n  }"
        )

    def test_a_deeper_offset_needs_a_bigger_minimum(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (!v.is_empty()) {\n    CHECK(v[v.size() - 2] == 3);\n  }",
            "v",
        )
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (v.size() >= 2) {\n    CHECK(v[v.size() - 2] == 3);\n  }"
        )

    def test_a_non_literal_offset_is_not_covered(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (!v.is_empty()) {\n    CHECK(v[v.size() - k] == 3);\n  }",
            "v",
        )

    def test_the_last_element_of_a_DIFFERENT_container_is_not_covered(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (!v.is_empty()) {\n    CHECK(v[other.size() - 1] == 3);\n  }",
            "v",
        )

    def test_a_length_peer_carries_the_last_element_idiom_across(self):
        """The real corpus shape (test_gaussian_importer.h:2929-2933): the guard
        proves `b` non-empty AND `a.size() == b.size()`, so `a[b.size() - 1]` is in
        range - the bound and the subscript are on different containers of proven
        equal length."""
        self.assertNotSized(
            "  CHECK_EQ(a.size(), b.size());\n"
            "  if (!b.is_empty() && a.size() == b.size()) {\n"
            "    CHECK_EQ(a[b.size() - 1], b[b.size() - 1]);\n  }"
        )

    def test_a_length_peer_does_not_grant_more_than_the_peer_proves(self):
        self.assertSized(
            "  CHECK_EQ(a.size(), b.size());\n"
            "  if (!b.is_empty() && a.size() == b.size()) {\n"
            "    CHECK_EQ(a[b.size() - 2], 0);\n  }",
            "a",
        )

    # --- how bounds combine --------------------------------------------------------

    def test_a_disjunction_keeps_only_what_every_arm_proves(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (v.size() >= 3 || v.size() >= 1) {\n    CHECK(v[0] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (v.size() >= 3 || v.size() >= 1) {\n    CHECK(v[2] == 3);\n  }",
            "v",
        )

    def test_a_ternary_keeps_only_what_both_arms_prove(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (flag ? v.size() >= 3 : v.size() >= 1) {\n    CHECK(v[0] == 3);\n  }"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (flag ? v.size() >= 3 : v.size() >= 1) {\n    CHECK(v[2] == 3);\n  }",
            "v",
        )

    def test_a_conjunction_combines_what_its_parts_prove(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (v.size() >= 2 && ready) {\n    CHECK(v[1] == 3);\n  }"
        )

    def test_nested_blocks_combine_their_bounds(self):
        """Every enclosing condition holds at once, so the frames are unioned: the
        outer `>= 2` covers `v[1]` and the inner loop covers `v[i]`."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (v.size() >= 2) {\n"
            "    for (uint32_t i = 0; i < v.size(); i++) {\n"
            "      CHECK(v[1] == v[i]);\n    }\n  }"
        )

    def test_a_nested_block_still_does_not_reach_past_the_union(self):
        self.assertSized(
            "  REQUIRE(v.size() == 9);\n"
            "  if (v.size() >= 2) {\n"
            "    for (uint32_t i = 0; i < v.size(); i++) {\n"
            "      CHECK(v[2] == v[i]);\n    }\n  }",
            "v",
        )

    # --- short-circuit operands are judged the same way ----------------------------

    def test_a_short_circuit_operand_must_reach_the_index_too(self):
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() >= 1 && v[1] == 3);", "v")
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.size() >= 2 && v[1] == 3);")

    def test_a_negative_short_circuit_operand_covers_index_zero_only(self):
        self.assertNotSized("  REQUIRE(v.size() == 2);\n  CHECK(v.is_empty() || v[0] == 3);")
        self.assertSized("  REQUIRE(v.size() == 2);\n  CHECK(v.is_empty() || v[1] == 3);", "v")

    # --- the relation must still hold where the index is written ------------------

    def test_a_body_that_rebinds_the_loop_index_loses_the_bound(self):
        """The relation is about VALUES and the model compares TEXT, so the moment a
        statement rebinds the name the two stop agreeing. This also closes round 4's
        recorded limit that a loop body driving the variable out of range was not
        modelled."""
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    i += 5;\n    CHECK(v[i] == 3);\n  }",
            "v",
        )

    def test_a_body_that_leaves_the_loop_index_alone_keeps_the_bound(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    use(i);\n    CHECK(v[i] == 3);\n  }"
        )

    def test_comparisons_are_not_rebindings(self):
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    CHECK(i != 9);\n    CHECK(i <= 9);\n    CHECK(i >= 0);\n"
            "    CHECK(v[i] == 3);\n  }"
        )

    def test_an_inner_loop_reusing_the_index_name_loses_the_outer_bound(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    for (uint32_t i = 0; i < n; i++) {\n      CHECK(v[i] == 3);\n    }\n  }",
            "v",
        )

    def test_growing_the_length_PEER_invalidates_the_last_element_idiom(self):
        """The indexed container's own mutators end the scan; the peer's did not, and
        `b.push_back(x)` makes `a[b.size() - 1]` one past the end of `a`."""
        self.assertSized(
            "  CHECK_EQ(a.size(), b.size());\n"
            "  if (!b.is_empty() && a.size() == b.size()) {\n"
            "    b.push_back(x);\n    CHECK_EQ(a[b.size() - 1], 0);\n  }",
            "a",
        )

    def test_leaving_the_length_peer_alone_keeps_the_idiom(self):
        self.assertNotSized(
            "  CHECK_EQ(a.size(), b.size());\n"
            "  if (!b.is_empty() && a.size() == b.size()) {\n"
            "    use(b);\n    CHECK_EQ(a[b.size() - 1], 0);\n  }"
        )

    # --- classification and fail-closed --------------------------------------------

    def test_an_under_bounded_site_is_labelled_as_such(self):
        """Not `loop-bounded-by-another-container`: the bound is on the RIGHT
        container, at the wrong magnitude, and naming the wrong container would send
        a maintainer to read the wrong line."""
        found = self.sites(
            "  REQUIRE(v.size() == 2);\n  if (!v.is_empty()) {\n    CHECK(v[1] == 3);\n  }"
        )
        self.assertEqual([site[6] for site in found], [GUARD._CLASS_UNDER_BOUND])

    def test_a_cross_container_site_keeps_its_own_label(self):
        found = self.sites(
            "  REQUIRE(v.size() == 2);\n"
            "  for (int i = 0; i < other.size(); i++) {\n    CHECK(v[i] == 3);\n  }"
        )
        self.assertEqual([site[6] for site in found], [GUARD._CLASS_OTHER_BOUND])

    def test_an_unreadable_subscript_is_not_covered(self):
        """An index whose brackets do not balance is unprovable, so it is reported."""
        self.assertEqual(GUARD._index_expressions("v", "CHECK(v[i"), [(6, None)])
        self.assertFalse(GUARD._bound_covers(GUARD._Bound(9, frozenset()), None, "v"))

    def test_an_unmodelled_index_expression_is_not_covered(self):
        for index in ("i + 1", "n - 1", "v.size()", "f(i)", "i * 2", "-1"):
            with self.subTest(index=index):
                self.assertFalse(
                    GUARD._bound_covers(GUARD._Bound(9, frozenset({"i"})), index, "v"),
                    f"{index!r} must not be treated as proven",
                )


class SizeIndexFailsClosed(unittest.TestCase):
    """Unreadable or unlexable input must FAIL, never read as 'no violations'."""

    def _scan(self, text: str, name: str = "test_synthetic.h"):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / name
            path.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)
            return GUARD._scan_file_size_index(path)

    def test_missing_file_is_a_scan_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(GUARD.ScanError):
                GUARD._scan_file_size_index(Path(tmp) / "absent.h")

    def test_invalid_utf8_is_a_scan_error_not_a_replacement_character(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_bytes(b'TEST_CASE("x") { REQUIRE(v.size() == 2); \xff\xfe }')
            with self.assertRaises(GUARD.ScanError):
                GUARD._scan_file_size_index(path)
            with self.assertRaises(GUARD.ScanError):
                GUARD._scan_file(path)

    def test_unterminated_raw_string_is_a_scan_error(self):
        with self.assertRaises(GUARD.ScanError):
            self._scan('const char *p = R"delim(never closed\nREQUIRE(v.size() == 2);\n')

    def test_terminated_raw_string_is_blanked_and_keeps_line_numbers(self):
        sites = self._scan(
            'const char *p = R"(ply\nformat ascii\nend_header\n)";\n'
            "TEST_CASE(\"x\") {\n  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);\n}\n"
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 6, "the assertion is on line 6 of the original file")

    def test_a_raw_string_opener_inside_a_line_comment_is_not_a_raw_string(self):
        """The false negative AND its mirror false positive, in one shape.

        Searching for raw-string openers before recognising comments blanked
        everything between a `// ... R"(` comment and a LATER `// ... )"` comment -
        real code included - and reported the file clean; with no later `)"` the same
        valid file was rejected as unterminated (Codex, PR #849 round 2). C++ has one
        lexer, so the guard needs one pass.
        """
        with_closer = self._scan(
            'TEST_CASE("x") {\n'
            '  // explain R"(\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            '  // closes with )"\n'
            "}\n"
        )
        self.assertEqual(len(with_closer), 1, "the violation between the comments is real code")
        self.assertEqual(with_closer[0][4], 4)
        without_closer = self._scan(
            'TEST_CASE("x") {\n'
            '  // explain R"(\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(without_closer), 1, "a comment cannot make a valid file unlexable")

    def test_a_raw_string_opener_inside_a_block_comment_is_not_a_raw_string(self):
        sites = self._scan(
            'TEST_CASE("x") {\n'
            '  /* explain R"( */\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)

    def test_a_raw_string_opener_inside_a_string_literal_is_not_a_raw_string(self):
        sites = self._scan(
            'TEST_CASE("x") {\n'
            '  const char *p = "R\\"(";\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)

    def test_a_comment_marker_inside_a_raw_string_is_not_a_comment(self):
        """The raw string comes FIRST here, so it wins - and it ends at the first `)\"`."""
        sites = self._scan(
            'const char *p = R"(// )" is not the end\n)";\n'
            'TEST_CASE("x") {\n  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);\n}\n'
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 4, "line numbers must survive the blanking")

    def test_an_apostrophe_in_a_comment_does_not_eat_a_later_raw_string(self):
        with self.assertRaises(GUARD.ScanError):
            self._scan("// don't\nconst char *p = R\"delim(never closed\n")

    def test_a_spliced_line_comment_does_not_end_at_the_physical_newline(self):
        """C++ removes `\\`-newline BEFORE recognising comments (Codex, PR #849 r3).

        So the `R"(` on the continuation line is COMMENT, not a raw-string opener.
        Reading it as one blanked everything up to the later `// )"` - the real
        assertion and index between them included - and reported the file clean.
        """
        sites = self._scan(
            'TEST_CASE("x") {\n'
            "  // continued comment \\\n"
            '  const char *p = R"(\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            '  // )"\n'
            "}\n"
        )
        self.assertEqual(len(sites), 1, "the assertion and index between the comments are code")
        self.assertEqual(sites[0][0], 4)
        self.assertEqual(sites[0][4], 5)

    def test_a_spliced_line_comment_comments_out_the_next_line(self):
        """`_strip_comments` must apply the same rule, or it reads what the raw-string
        pass skipped. The continuation line is emitted EMPTY, keeping line numbers."""
        stripped = GUARD._strip_comments("a();\n// note \\\nREQUIRE(x);\nb();\n").split("\n")
        self.assertEqual(stripped[2], "")
        self.assertEqual(stripped[3], "b();")
        self.assertEqual(len(stripped), 5)

    def test_a_chain_of_splices_continues_the_comment(self):
        stripped = GUARD._strip_comments("// a \\\nb(); \\\nc();\nd();\n").split("\n")
        self.assertEqual(stripped[:3], ["", "", ""])
        self.assertEqual(stripped[3], "d();")

    def test_a_backslash_before_a_space_does_not_splice(self):
        """Non-conforming spelling: compilers warn and this pass keeps the next line
        as CODE, which is the fail-closed answer (it can only ADD reports)."""
        sites = self._scan(
            'TEST_CASE("x") {\n'
            "  // note \\ \n"
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)

    def test_crlf_line_endings_splice_the_same_way(self):
        """A CRLF file must lex like an LF one: `\\r` belongs to the line terminator."""
        sites = self._scan(
            'TEST_CASE("x") {\r\n'
            "  // continued comment \\\r\n"
            '  const char *p = R"(\r\n'
            "  REQUIRE(v.size() == 2);\r\n"
            "  CHECK(v[0] == 1);\r\n"
            '  // )"\r\n'
            "}\r\n"
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][4], 5)

    def test_a_spliced_ordinary_literal_is_still_one_literal(self):
        """An ordinary string continued with backslash-newline is spliced BEFORE
        tokenising, so a `/*` on the continuation is string content and not a
        comment opener. Reading it as one opened a block comment that blanked the
        assertion, the index and everything to EOF, and the file scanned clean
        (Codex, PR #849 round 4). The same holds for a `//` or an `R"(` there."""
        for continuation in ("/*", "//", 'R"(', "plain"):
            for eol in ("\n", "\r\n"):
                with self.subTest(continuation=continuation, eol=eol):
                    sites = self._scan(
                        eol.join(
                            [
                                'TEST_CASE("x") {',
                                '  const char *p = "abc \\',
                                f'{continuation} still inside the string";',
                                "  REQUIRE(v.size() == 2);",
                                "  CHECK(v[0] == 1);",
                                "}",
                                "",
                            ]
                        )
                    )
                    self.assertEqual(len(sites), 1, "the assertion and index are code")
                    self.assertEqual(sites[0][0], 4, "line numbers must survive blanking")
                    self.assertEqual(sites[0][4], 5)

    def test_nothing_multi_line_survives_the_raw_string_pass(self):
        """The invariant `_strip_comments` relies on to stay line-oriented: after
        `_read_source`, no ordinary literal spans a newline either."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_bytes(b'const char *p = "abc \\\n/* still string";\nCHECK(v[0]);\n')
            lexed = GUARD._read_source(path)
        self.assertEqual(len(lexed.split("\n")), 4, "line count is preserved")
        self.assertNotIn("/*", lexed, "the continuation was string content, not a comment")

    def test_a_digit_separator_before_a_splice_is_not_a_literal(self):
        """`1'000` is not an opener, and a line continuation after it must not turn
        it into one - that would blank the code that follows."""
        sites = self._scan(
            "TEST_CASE(\"x\") {\n"
            "  const int big = 1'000; \\\n"
            "  const int other = 2;\n"
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 4)

    def test_splice_is_applied_once_for_every_recogniser(self):
        """The map `_splice` returns is what lets a spliced token be recognised.

        Rounds 3, 4 and 5 each found splicing missing from ONE lexical context. The
        rule is not a property of any token - phase 2 runs before tokens exist - so
        it is applied once here and every recogniser reads the result.
        """
        for label, text, expected in (
            ("lf", "a\\\nb", "ab"),
            ("crlf", "a\\\r\nb", "ab"),
            ("backslash then space does not splice", "a\\ \nb", "a\\ \nb"),
            # Phase 2 is a single pass: the backslash left behind by deleting
            # `\\`-newline does not splice the newline after it, and neither does
            # the compiler.
            ("no re-splicing of its own output", "a\\\\\n\nb", "a\\\nb"),
        ):
            with self.subTest(label=label):
                logical, offsets = GUARD._splice(text)
                self.assertEqual(logical, expected)
                self.assertEqual(len(offsets), len(logical) + 1)
                self.assertEqual(offsets[-1], len(text), "sentinel maps the end offset")
                self.assertEqual(offsets, sorted(set(offsets)), "strictly increasing")
                for i, ch in enumerate(logical):
                    self.assertEqual(text[offsets[i]], ch)

    def test_a_comment_opener_formed_by_splicing_is_a_comment(self):
        """`/` + backslash-newline + `/ explain R"(` IS a `//` comment in C++.

        This pass scanned the UNSPLICED text, so it read the marker as a raw-string
        opener and blanked everything to a later `)"` - the assertion and the index
        between them included - and reported the file clean (Codex, PR #849 round 5).
        """
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                sites = self._scan(
                    eol.join(
                        [
                            'TEST_CASE("x") {',
                            "  /\\",
                            '/ explain R"(',
                            "  REQUIRE(v.size() == 2);",
                            "  CHECK(v[0] == 1);",
                            '  const char *tail = ")";',
                            "}",
                            "",
                        ]
                    )
                )
                self.assertEqual(len(sites), 1, "the assertion and index are code")
                self.assertEqual(sites[0][0], 4, "line numbers survive the blanking")
                self.assertEqual(sites[0][4], 5)

    def test_a_spliced_comment_is_erased_so_the_line_pass_cannot_reopen_it(self):
        """`_strip_comments` is line-oriented and cannot see a spliced opener.

        Left in place, its `/*` reads as a block comment that is not in the source,
        blanking every assertion to the next `*/` or to EOF. So the spliced comment
        is removed by the pass that CAN see it, and the two passes agree by
        construction rather than by both implementing the same rule.
        """
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                sites = self._scan(
                    eol.join(
                        [
                            'TEST_CASE("x") {',
                            "  /\\",
                            "/ explain /*",
                            "  REQUIRE(v.size() == 2);",
                            "  CHECK(v[0] == 1);",
                            "}",
                            "",
                        ]
                    )
                )
                self.assertEqual(len(sites), 1)
                self.assertEqual(sites[0][0], 4)

    def test_a_block_comment_opener_formed_by_splicing_is_a_comment(self):
        sites = self._scan(
            'TEST_CASE("x") {\n'
            "  /\\\n"
            '* explain R"(\n'
            "  still comment */\n"
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 5)

    def test_a_block_comment_closer_formed_by_splicing_ends_the_comment(self):
        """`*` + splice + `/` closes the comment, so what follows is CODE.

        Missing it kept the comment open and blanked the assertion and the index.
        """
        sites = self._scan(
            'TEST_CASE("x") {\n'
            "  /* explain *\\\n"
            "/\n"
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 4)

    def test_a_raw_string_opener_formed_by_splicing_is_a_raw_string(self):
        """`R` + splice + `"(` opens a raw string; its BODY must not read as code."""
        source = (
            'TEST_CASE("x") {\n'
            "  const char *p = R\\\n"
            '"(\n'
            "  REQUIRE(inside.size() == 9);\n"
            '  )";\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        lexed = GUARD._blank_raw_strings("test_synthetic.h", source)
        self.assertNotIn("inside.size()", lexed, "the raw body is not code")
        self.assertEqual(len(lexed.split("\n")), len(source.split("\n")))
        sites = self._scan(source)
        self.assertEqual(len(sites), 1)
        self.assertEqual(sites[0][0], 6)

    def test_a_spliced_comment_does_not_invent_an_unterminated_raw_string(self):
        """The other half of round 5: with no later `)"` the same misreading
        REJECTED the file instead of blanking it. Both directions are wrong."""
        sites = self._scan(
            'TEST_CASE("x") {\n'
            "  /\\\n"
            '/ explain R"(\n'
            "  REQUIRE(v.size() == 2);\n"
            "  CHECK(v[0] == 1);\n"
            "}\n"
        )
        self.assertEqual(len(sites), 1)

    def test_unbalanced_assertion_parens_are_a_scan_error(self):
        with self.assertRaises(GUARD.ScanError):
            GUARD._size_assertions("REQUIRE(v.size() == 2;", "test_synthetic.h")

    def test_scan_errors_fail_the_run_rather_than_reporting_clean(self):
        found, errors = GUARD.scan_all_size_index()
        self.assertEqual(errors, [], "the shipped corpus must scan cleanly")
        self.assertTrue(found)


class SizeIndexRatchet(unittest.TestCase):
    """The baseline is shrink-only: an addition fails, and so does a stale entry."""

    def _prints(self, body: str) -> list[str]:
        source = 'TEST_CASE("[Synthetic] case") {\n' + body + "\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test_synthetic.h"
            path.write_text(source, encoding="utf-8")
            return sorted(
                GUARD.size_index_fingerprint(site[1], site[2], site[3], site[5])
                for site in GUARD._scan_file_size_index(path)
            )

    def test_swap_keeps_the_count_but_changes_the_fingerprints(self):
        before = self._prints("  REQUIRE(alpha.size() == 2);\n  CHECK(alpha[0] == 1);")
        after = self._prints("  REQUIRE(beta.size() == 2);\n  CHECK(beta[0] == 1);")
        self.assertEqual(len(before), 1)
        self.assertEqual(len(after), 1, "count is unchanged - this is the hole")
        self.assertNotEqual(before, after)

    def test_fingerprint_is_independent_of_line_number(self):
        plain = self._prints("  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);")
        shifted = self._prints(
            "  int filler = 0;\n  (void)filler;\n  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);"
        )
        self.assertEqual(plain, shifted)

    def test_a_second_index_under_the_same_assertion_is_a_new_fingerprint(self):
        one = self._prints("  REQUIRE(v.size() == 2);\n  CHECK(v[0] == 1);")
        two = self._prints("  REQUIRE(v.size() == 2);\n  CHECK(v[1] == 1);")
        self.assertNotEqual(one, two, "hashing only the assertion would collapse these")

    def test_a_new_site_is_reported_as_added_against_the_shipped_baseline(self):
        baseline, _ = GUARD.load_size_index_baseline()
        added = self._prints("  REQUIRE(brand_new_vec.size() == 9);\n  CHECK(brand_new_vec[0]);")
        self.assertEqual(len(added), 1)
        self.assertEqual(
            GUARD._multiset_difference(added, baseline.get("test_synthetic.h", [])), added
        )


class SizeIndexBaselineIntegrity(unittest.TestCase):
    def test_baseline_loads(self):
        baseline, problems = GUARD.load_size_index_baseline()
        self.assertEqual(problems, [])
        self.assertTrue(baseline)

    def test_baseline_entries_are_non_empty(self):
        baseline, _ = GUARD.load_size_index_baseline()
        for name, prints in baseline.items():
            self.assertTrue(prints, f"{name}: an empty baseline entry should be removed")

    def test_baseline_matches_the_corpus(self):
        prints, errors = GUARD.scan_size_index_fingerprints()
        self.assertEqual(errors, [])
        baseline, _ = GUARD.load_size_index_baseline()
        self.assertEqual(
            prints, baseline, "baseline has drifted from the corpus; run the guard for the diff."
        )

    def test_missing_baseline_file_is_a_failure_not_a_pass(self):
        original = GUARD.SIZE_INDEX_BASELINE_PATH
        try:
            GUARD.SIZE_INDEX_BASELINE_PATH = original.with_name("does_not_exist.json")
            _, problems = GUARD.load_size_index_baseline()
            self.assertTrue(problems)
        finally:
            GUARD.SIZE_INDEX_BASELINE_PATH = original

    def test_the_count_reconciles_with_issue_844(self):
        """#844's sweep: 46 dangerous, 4 fixed by #843 -> 42 remaining.

        This detector reports 50 = 43 straight-line + 7 bounded only by ANOTHER
        container's size(). See the guard's docstring for the +1/+7 reconciliation;
        pinned here so the split cannot drift without someone re-deciding it.
        """
        found, errors = GUARD.scan_all_size_index()
        self.assertEqual(errors, [])
        sites = [site for file_sites in found.values() for site in file_sites]
        straight = [s for s in sites if s[6] == GUARD._CLASS_STRAIGHT_LINE]
        other = [s for s in sites if s[6] == GUARD._CLASS_OTHER_BOUND]
        under = [s for s in sites if s[6] == GUARD._CLASS_UNDER_BOUND]
        self.assertEqual(len(straight), 43)
        self.assertEqual(len(other), 7)
        # Round 6's population is EMPTY on this corpus, and pinned at zero so that
        # a site whose guard proves too small a bound cannot appear unremarked.
        self.assertEqual(len(under), 0)
        self.assertEqual(len(sites), 50)

    def test_the_named_concentrations_in_issue_844_reconcile_exactly(self):
        """#844 names three files by count. All three match the straight-line
        population, which is the evidence that this is the same set of sites and
        not a different set of similar size."""
        found, _ = GUARD.scan_all_size_index()
        expected = {
            "test_renderer_pipeline.h": 7,
            "test_resident_atlas_budget.h": 7,
            "test_gaussian_importance.h": 5,
        }
        for name, count in expected.items():
            straight = [
                s for s in found.get(name, []) if s[6] == GUARD._CLASS_STRAIGHT_LINE
            ]
            self.assertEqual(len(straight), count, name)


class BaselineIntegrity(unittest.TestCase):
    def test_baseline_loads(self):
        baseline, problems = GUARD.load_baseline()
        self.assertEqual(problems, [])
        self.assertTrue(baseline)

    def test_baseline_entries_are_non_empty(self):
        baseline, _ = GUARD.load_baseline()
        for name, prints in baseline.items():
            self.assertTrue(prints, f"{name}: an empty baseline entry should be removed")

    def test_baseline_matches_the_corpus(self):
        """The shipped baseline must equal reality, in both directions."""
        baseline, _ = GUARD.load_baseline()
        self.assertEqual(
            GUARD.scan_fingerprints(),
            baseline,
            "baseline has drifted from the corpus; run the guard for the diff.",
        )

    def test_missing_baseline_file_is_a_failure_not_a_pass(self):
        original = GUARD.BASELINE_PATH
        try:
            GUARD.BASELINE_PATH = original.with_name("does_not_exist.json")
            _, problems = GUARD.load_baseline()
            self.assertTrue(problems)
        finally:
            GUARD.BASELINE_PATH = original

    def test_guard_passes_on_the_current_tree(self):
        self.assertEqual(GUARD.main(), 0)


if __name__ == "__main__":
    unittest.main()
