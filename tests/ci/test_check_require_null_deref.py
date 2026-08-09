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
        self.assertEqual(len(straight), 43)
        self.assertEqual(len(other), 7)
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
