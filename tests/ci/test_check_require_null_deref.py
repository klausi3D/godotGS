#!/usr/bin/env python3
"""Unit test for tests/ci/check_require_null_deref.py (#656).

A guard that has never been observed to fail is not evidence that it works, and a
guard nobody has tried to fool is not evidence that it is precise. These cases pin
BOTH directions: the shapes it must flag, and the shapes it must leave alone.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_no_atom_that_creates_a_pending_frame_carries_its_own_body(self):
        """The property that makes `pending` sound (round 10).

        `pending` is a one-slot lookahead meaning "the body of this header is the
        NEXT atom". Rounds 8, 9 and 10 each found a shape where that guess was
        wrong, and round 10's was the shape where the body was inside the atom all
        along: `if (v.size() >= 2) consume(v);` stored the condition's frame in
        `pending`, which then bounded the statement AFTER the branch.

        Idempotence (the test above) does not catch that - the atom really is one
        statement, and splitting it further is what round 5's `{`-split does only
        for BRACED bodies. So the property pinned here is the one `pending`
        actually needs: an atom that reaches `pending = frame` holds no body of its
        own, hence "the body is the next atom" is a fact about the decomposition
        and not a guess about layout. If this ever fails there is a fourth shape,
        and it fails here rather than in a review round.
        """
        examined = 0
        pending_creators = 0
        for path in GUARD._test_sources():
            lines = GUARD._strip_comments(GUARD._read_source(path)).splitlines()
            for index in range(len(lines)):
                logical, last = GUARD._logical_line(lines, index)
                groups = list(GUARD._line_fragments(logical))
                groups += [text for _line, text in GUARD._statements(lines, last + 1, 6)]
                for group in groups:
                    for atom in GUARD._statement_atoms(group):
                        examined += 1
                        if not GUARD._SIZE_CONTROL_FLOW_RE.match(atom):
                            continue
                        if atom.rstrip().endswith("{") or GUARD._guards_no_body(atom):
                            continue
                        pending_creators += 1
                        self.assertEqual(
                            GUARD._split_header(atom),
                            None,
                            f"{path.name}: atom {atom!r} would put a frame in `pending` "
                            f"while carrying its own body",
                        )
        self.assertGreater(examined, 10000, "the corpus sweep examined too little to mean anything")
        self.assertGreater(
            pending_creators,
            0,
            "no atom in the corpus reaches `pending` - this property passed vacuously",
        )

    def test_every_control_keyword_has_a_header_end_rule(self):
        """DERIVED from `_SIZE_CONTROL_FLOW_RE`, not listed again beside it.

        `_header_end` is a second reading of the same vocabulary the walker uses to
        decide a statement is control flow. A keyword accepted there but unknown
        here would keep its body glued to its header - exactly round 10's defect,
        reintroduced by an edit nobody thought was risky. So the keyword set comes
        out of the walker's own pattern.
        """
        keywords = re.findall(r"(\w+)\\b", GUARD._SIZE_CONTROL_FLOW_RE.pattern)
        self.assertGreaterEqual(len(keywords), 6, keywords)
        for keyword in keywords:
            with self.subTest(keyword=keyword):
                statement = f"{keyword} (cond) body();"
                self.assertIsNotNone(GUARD._header_end(statement))
                self.assertIsNotNone(GUARD._split_header(statement))
                atoms = GUARD._statement_atoms(statement)
                self.assertGreater(len(atoms), 1, atoms)
                self.assertIsNone(GUARD._split_header(atoms[0]), atoms)

    def test_a_braceless_body_splits_off_its_header(self):
        self.assertEqual(
            GUARD._statement_atoms("if (v.size() >= 2) consume(v);"),
            ["if (v.size() >= 2)", "consume(v);"],
        )

    def test_a_header_with_no_body_of_its_own_is_left_whole(self):
        self.assertEqual(GUARD._statement_atoms("if (v.size() >= 2)"), ["if (v.size() >= 2)"])

    def test_an_empty_body_is_not_split_into_a_stray_semicolon(self):
        self.assertEqual(GUARD._statement_atoms("while (poll());"), ["while (poll());"])
        self.assertEqual(
            GUARD._statement_atoms("} while (v.size() >= 2);"), ["}", "while (v.size() >= 2);"]
        )

    def test_a_braceless_do_body_splits_off_its_keyword(self):
        """`do` carries no condition, so its header is the keyword alone. Without
        that rule the first `(` in the BODY is mistaken for a condition and the body
        stays glued on - the round-10 shape, in the one keyword whose header has no
        parentheses."""
        self.assertEqual(GUARD._statement_atoms("do consume(v);"), ["do", "consume(v);"])
        self.assertEqual(GUARD._statement_atoms("do {"), ["do {"])

    def test_a_nested_braceless_body_splits_all_the_way_down(self):
        self.assertEqual(
            GUARD._statement_atoms("if (a) if (b) consume(v);"),
            ["if (a)", "if (b)", "consume(v);"],
        )

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
        # Keyed by repo-relative path, not basename, since GS-AUDIT-TEST-003.
        expected = {
            "modules/gaussian_splatting/tests/test_renderer_pipeline.h": 7,
            "modules/gaussian_splatting/tests/test_resident_atlas_budget.h": 7,
            "modules/gaussian_splatting/tests/test_gaussian_importance.h": 5,
        }
        for name, count in expected.items():
            straight = [
                s for s in found.get(name, []) if s[6] == GUARD._CLASS_STRAIGHT_LINE
            ]
            self.assertEqual(len(straight), count, name)


class DoWhileTerminatorIsNotALoopHead(SizeIndexScanTestCase):
    """`} while (cond);` ends a loop; it does not start one (#849 round 9).

    Atomisation splits the terminator into `"}"` and `while (...);`, and the
    `while` was read as a brace-less loop HEAD, so its condition was stored as the
    `pending` frame and bounded the NEXT statement. With an actual size of 1 the
    assertion fails, the loop exits, and the subscript aborts the process - while
    the nonexistent `while` body's bound suppressed the report.
    """

    def test_do_while_bound_does_not_reach_the_following_statement(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  do {\n"
            "    consume(v);\n"
            "  } while (v.size() >= 2);\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_the_do_may_be_above_the_assertion_and_therefore_invisible(self):
        """The scan starts at the assertion, so the matching `do` is often not in
        the window at all. The terminator is recognised by SHAPE for that reason."""
        self.assertSized(
            "  do {\n"
            "    REQUIRE(v.size() == 3);\n"
            "    consume(v);\n"
            "  } while (v.size() >= 2);\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_a_deliberately_empty_body_bounds_nothing_after_it(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n  while (v.size() >= 2);\n  CHECK(v[1]);", "v"
        )

    def test_a_real_block_body_is_still_bounded_by_its_header(self):
        """The fix must not turn every loop into a reported site."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) {\n"
            "    CHECK(v[i]);\n"
            "  }"
        )

    def test_a_do_body_is_still_scanned(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n  do {\n    CHECK(v[1]);\n  } while (again());",
            "v",
        )

    def test_the_terminator_condition_is_itself_judged(self):
        """`} while (v[5] > 0);` evaluates the subscript; it is not a header whose
        body might be guarded, so it must be reported like any other statement."""
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n  do {\n    step();\n  } while (v[5] > 0);", "v"
        )

    def test_layout_does_not_change_the_verdict(self):
        """Same code, terminator compacted onto the body's line."""
        compact = self.sites(
            "  REQUIRE(v.size() == 3);\n  do { consume(v); } while (v.size() >= 2);\n  CHECK(v[1]);"
        )
        self.assertEqual(len(compact), 1, compact)


class LayoutInvarianceOverTheRealCorpus(unittest.TestCase):
    """Un-brace the real corpus and both detectors must not notice (#849 round 10).

    The synthetic cases below pin the SHAPES. This pins the PROPERTY they are all
    instances of, over real code rather than over examples: removing the braces
    from a single-statement body is a reformat that changes no semantics, so it
    must change no verdict. Rounds 5, 8, 9 and 10 were each a way of failing that.

    Measured against HEAD when this was written: on the shipped corpus the rewrite
    collapses 1136 real bodies, and the round-9 guard's answer moves on 18 detector-2
    entries and 6 detector-1 entries - including reporting a bounded
    `for (i < a.size()) CHECK(a[i] == b[i]);` as unbounded and, because the scan
    reports the FIRST unbounded index, thereby MISSING the real `CHECK(a[0] == 1u)`
    that follows it (`test_gaussian_importance.h:47-54`). This test is zero for the
    guard in this file. It is the corpus-wide form of "a frame is applied only to
    statements in its scope", which is otherwise an argument.
    """

    HEAD_RE = re.compile(r"^(\s*)((?:\}\s*)?(?:else\s+if|if|for|while)\s*\(.*\))\s*\{\s*$")

    @classmethod
    def _unbrace(cls, text: str) -> tuple[str, int]:
        """`H (c) {\\n s;\\n }` -> `H (c) s;`. Deliberately conservative: one
        statement, no braces of its own, `}` alone on its line, no `else` after."""
        lines = text.splitlines()
        out: list[str] = []
        index = 0
        collapsed = 0
        while index < len(lines):
            head = cls.HEAD_RE.match(lines[index])
            body = lines[index + 1] if index + 1 < len(lines) else ""
            closer = lines[index + 2].strip() if index + 2 < len(lines) else ""
            following = lines[index + 3].strip() if index + 3 < len(lines) else ""
            if (
                head
                and body.strip().endswith(";")
                and "{" not in body
                and "}" not in body
                and closer == "}"
                and not following.startswith("else")
            ):
                out.append(f"{head.group(1)}{head.group(2)} {body.strip()}")
                index += 3
                collapsed += 1
                continue
            out.append(lines[index])
            index += 1
        return "\n".join(out) + "\n", collapsed

    def test_unbracing_the_corpus_changes_no_verdict(self):
        collapsed_total = 0
        braced2: list = []
        braced1: list = []
        unbraced2: list = []
        unbraced1: list = []
        with tempfile.TemporaryDirectory() as tmp:
            for path in GUARD._test_sources():
                text = GUARD._read_source(path)
                rewritten, collapsed = self._unbrace(text)
                if not collapsed:
                    continue
                collapsed_total += collapsed
                for label, source, sink2, sink1 in (
                    ("braced", text, braced2, braced1),
                    ("unbraced", rewritten, unbraced2, unbraced1),
                ):
                    target = Path(tmp) / f"{label}_{path.name}"
                    target.write_text(source, encoding="utf-8", newline="")
                    sink2 += [
                        (path.name, s[1], s[2], s[3], s[5], s[6])
                        for s in GUARD._scan_file_size_index(target)
                    ]
                    sink1 += [
                        (path.name, s[1], s[2], s[3]) for s in GUARD._scan_file(target)
                    ]
        self.assertGreater(collapsed_total, 100, "the rewrite did not fire enough to mean anything")
        self.assertGreater(len(braced2), 0, "no detector-2 site in the rewritten file set")
        self.assertGreater(len(braced1), 0, "no detector-1 site in the rewritten file set")
        self.assertEqual(sorted(unbraced2), sorted(braced2), "detector 2 is layout-dependent")
        self.assertEqual(sorted(unbraced1), sorted(braced1), "detector 1 is layout-dependent")


class BracelessBodyEndsWithItsBody(SizeIndexScanTestCase):
    """A brace-less body is ONE statement, and the frame ends there (#849 round 10).

    Third instance of one mechanism. Rounds 8, 9 and 10 all found a frame applied
    to a statement outside its scope, and all three arrived through `pending` - the
    one-slot lookahead that means "the body of this header is the next atom":

    * round 8 - a trailing `}` shared the last body statement's line, so the block
      frame was never popped and leaked past the block;
    * round 9 - a `do … while (c);` TERMINATOR read as a brace-less loop head, so a
      body that does not exist bounded the next statement;
    * round 10 - a brace-less body INSIDE the atom, so the condition's frame went
      to `pending` and bounded the statement after the branch, while the body it
      was supposed to guard was judged against the enclosing bound instead.

    All three are one defect: a frame whose scope was inferred from surface syntax
    rather than from structure. The repair is structural rather than a fourth
    special case - `_statement_atoms` now splits a brace-less body off its header,
    so the atom no longer contains its body and `pending` cannot mis-attribute it.
    `StatementAtomsAreTotal.test_no_atom_that_creates_a_pending_frame_carries_its_own_body`
    is what holds that down: it is checked over the corpus rather than argued.

    The corpus contains 36 inline brace-less bodies (24 distinct, 10 files) and
    NONE of them is inside a cardinality-assertion window today, so this fix moves
    no baseline. The syntax is ordinary C++ though - `if (c) return;` is 11 of the
    24 - so the silence is one ordinary edit away, which is why the shapes below
    are pinned rather than left to a follow-up.
    """

    def test_the_reported_shape_is_a_site(self):
        """Codex, PR #849 round 10. At a real length of 1 the assertion fails, the
        branch is SKIPPED, and `v[1]` aborts the batch."""
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                self.assertSized(
                    (
                        "  REQUIRE(v.size() == 3);\n"
                        "  if (v.size() >= 2) consume(v);\n"
                        "  CHECK(v[1]);"
                    ).replace("\n", eol),
                    "v",
                )

    def test_the_body_may_be_on_its_own_line(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (v.size() >= 2)\n"
            "    consume(v);\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_a_braceless_loop_bound_also_ends_at_its_body(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n  while (v.size() >= 2) step(v);\n  CHECK(v[1]);", "v"
        )
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  for (uint32_t i = 0; i < v.size(); i++) consume(v);\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_a_nested_braceless_body_is_still_scoped(self):
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (ok()) if (v.size() >= 2) consume(v);\n"
            "  CHECK(v[1]);",
            "v",
        )

    def test_the_body_itself_is_judged_UNDER_the_frame(self):
        """The other half of the same defect: the atom carried its body into the
        header test, where the header's own bound deliberately does not apply, so a
        branch that IS guarded was reported."""
        self.assertNotSized("  REQUIRE(v.size() == 3);\n  if (v.size() >= 2) CHECK(v[1]);")

    def test_an_index_the_frame_does_not_reach_is_still_reported(self):
        """The fix must not become a blanket suppressor: `v.size() >= 2` proves
        nothing about `v[5]`."""
        self.assertSized("  REQUIRE(v.size() == 3);\n  if (v.size() >= 2) CHECK(v[5]);", "v")

    BODIES = (
        ("consume(v);", "CHECK(v[1]);"),
        ("CHECK(v[1]);", "consume(v);"),
        ("CHECK(v[5]);", "consume(v);"),
        ("step(v);", "CHECK(v[0]);"),
    )

    def test_braced_and_braceless_spellings_agree(self):
        """The property the three rounds were each a violation of: LAYOUT does not
        change the verdict. Compared on everything but the line number."""
        for body, after in self.BODIES:
            for head in ("if (v.size() >= 2)", "while (v.size() >= 2)", "for (uint32_t i = 0; i < v.size(); i++)"):
                with self.subTest(head=head, body=body):
                    prefix = f"  REQUIRE(v.size() == 3);\n  {head} "
                    braced = self.sites(f"{prefix}{{ {body} }}\n  {after}")
                    braceless = self.sites(f"{prefix}{body}\n  {after}")
                    self.assertEqual(
                        [(s[1], s[5], s[6]) for s in braced],
                        [(s[1], s[5], s[6]) for s in braceless],
                    )

    def test_a_braceless_do_body_is_scanned_like_a_braced_one(self):
        """A `do` body runs whatever the condition says, so the round-9 rule that a
        `do` TERMINATOR bounds nothing must still leave the site reported - and the
        body's own statements must be reached, which they are not while the body is
        glued to the keyword."""
        braceless = self.sites(
            "  REQUIRE(v.size() == 3);\n  do consume(v); while (v.size() >= 2);\n  CHECK(v[1]);"
        )
        braced = self.sites(
            "  REQUIRE(v.size() == 3);\n  do { consume(v); } while (v.size() >= 2);\n  CHECK(v[1]);"
        )
        self.assertEqual(len(braceless), 1, braceless)
        self.assertEqual(
            [(s[1], s[5], s[6]) for s in braceless], [(s[1], s[5], s[6]) for s in braced]
        )

    def test_a_conditional_return_does_not_end_the_scan(self):
        """`if (skip()) return;` is 11 of the corpus's 24 inline bodies. Splitting
        the body off must not turn it into an unconditional `return`, which would
        make the scan stop and the site vanish - the fail-OPEN direction."""
        self.assertSized(
            "  REQUIRE(v.size() == 3);\n  if (skip()) return;\n  CHECK(v[1]);", "v"
        )

    def test_an_unconditional_return_still_ends_the_scan(self):
        self.assertNotSized("  REQUIRE(v.size() == 3);\n  return;\n  CHECK(v[1]);")

    def test_a_body_that_really_is_the_next_atom_still_gets_the_frame(self):
        """`pending`'s genuine case, which the split must leave working: a header
        whose atom ends without a body because the body is a later statement."""
        self.assertNotSized(
            "  REQUIRE(v.size() == 3);\n"
            "  if (v.size() >= 2)\n"
            "    for (uint32_t i = 0; i < 3; i++)\n"
            "      CHECK(v[1]);"
        )

    def test_detector_one_reads_the_same_atoms(self):
        """One decomposition, both detectors. Detector 1's documented rule is that
        a control-flow statement guards its BODY and it therefore stops at one - and
        for a brace-less body it was breaking that rule in the reporting direction,
        flagging `if (ptr) ptr->method();`, the shape where the deref is guarded by
        the very null check the guard is looking for."""
        for eol in ("\n", "\r\n"):
            with self.subTest(eol=eol):
                braceless = self.null_deref_sites(
                    "  REQUIRE(ptr != nullptr);\n  if (ptr) ptr->method();".replace("\n", eol)
                )
                braced = self.null_deref_sites(
                    "  REQUIRE(ptr != nullptr);\n  if (ptr) { ptr->method(); }".replace("\n", eol)
                )
                self.assertEqual(braceless, [])
                self.assertEqual([v[1:] for v in braceless], [v[1:] for v in braced])

    def test_a_deref_in_the_header_is_still_flagged(self):
        """Non-inertness for detector 1: the header is evaluated before anything can
        guard it, so a deref THERE must still report."""
        self.assertEqual(
            len(self.null_deref_sites("  REQUIRE(ptr != nullptr);\n  if (ptr->ready()) use();")),
            1,
        )

    def test_the_walker_fails_closed_if_the_decomposition_regresses(self):
        """The backstop, exercised rather than asserted in prose.

        `_first_unbounded_index` refuses to put a frame in `pending` when the atom
        it came from carries its own body. That branch is unreachable while
        `_statement_atoms` splits, so it is checked by REMOVING the split: with the
        decomposition regressed to its round-9 behaviour, the walker must still
        report the shape rather than suppress it. Delete the branch and this test
        goes silent exactly the way the guard did.
        """
        original = GUARD._inline_pieces

        def without_the_braceless_split(statement: str) -> list[str]:
            pieces = original(statement)
            if len(pieces) > 1 and not pieces[0].rstrip().endswith("{"):
                return [statement]
            return pieces

        with mock.patch.object(GUARD, "_inline_pieces", without_the_braceless_split):
            self.assertEqual(
                GUARD._statement_atoms("if (v.size() >= 2) consume(v);"),
                ["if (v.size() >= 2) consume(v);"],
                "the regression harness did not actually regress the split",
            )
            self.assertSized(
                "  REQUIRE(v.size() == 3);\n"
                "  if (v.size() >= 2) consume(v);\n"
                "  CHECK(v[1]);",
                "v",
            )


class AssertionVocabularyIsDerived(SizeIndexScanTestCase):
    """One derivation decides what a NAME means AND whether it is accepted (round 9).

    Round 8 derived doctest's macro family for its NEGATION and RELATION
    semantics, and then left a separate hard-coded head regex deciding, before the
    family was ever consulted, which names got to reach it. That is a second source
    of truth, and it was wrong in two directions at once: it rejected doctest's own
    `DOCTEST_*` spellings, and - in detector 1 - it rejected every `CHECK`/`WARN`
    spelling of a null-ish assertion, hiding 18 real corpus sites.
    """

    def test_the_prefixed_spelling_is_the_same_macro(self):
        self.assertSized("  DOCTEST_REQUIRE(v.size() == 2);\n  CHECK(v[0]);", "v")
        self.assertSized("  DOCTEST_CHECK(v.size() == 2);\n  CHECK(v[0]);", "v")

    def test_the_prefixed_spelling_keeps_its_negation(self):
        """Derived, not spelled: `DOCTEST_REQUIRE_FALSE(v.size() == 0)` is the lower
        bound `size() != 0`, and `DOCTEST_REQUIRE_FALSE(v.size())` asserts EMPTY."""
        self.assertSized("  DOCTEST_REQUIRE_FALSE(v.size() == 0);\n  CHECK(v[0]);", "v")
        self.assertNotSized("  DOCTEST_REQUIRE_FALSE(v.size());\n  CHECK(v[0]);")

    def test_the_WARN_family_is_scanned(self):
        """Retires blind spot 7. `WARN` reports and continues exactly like `CHECK`,
        so a short container runs into the same aborting index."""
        self.assertSized("  WARN(v.size() == 2);\n  CHECK(v[0]);", "v")
        self.assertSized("  WARN_FALSE(v.is_empty());\n  CHECK(v[0]);", "v")

    def test_godots_WARN_PRINT_is_not_an_assertion(self):
        """An EXACT derived name set is what a `WARN\\w*` prefix could not express.
        `WARN_PRINT` is Godot's, not doctest's, and reading it as an assertion would
        invent a bound nobody wrote."""
        self.assertIsNone(
            GUARD._assertion_vocabulary().size_head.match('WARN_PRINT("v.size() == 2");')
        )
        self.assertNotSized('  WARN_PRINT("short");\n  CHECK(v[0]);')

    def test_project_local_wrappers_still_reach_the_scan(self):
        """The derived half is a union member, not a replacement: `test_macros.h`
        wrappers must keep being scanned and read as plain."""
        self.assertSized("  CHECK_SIZES_EQ(v.size(), 0);\n  CHECK(v[0]);", "v")
        self.assertIsNotNone(
            GUARD._assertion_vocabulary().size_head.match("REQUIRE_GPU_DEVICE();")
        )

    def test_the_accepted_set_covers_the_whole_derived_family(self):
        """The property, not a list of names: every macro doctest defines is
        accepted as an assertion head. A future hand-written restriction fails
        here rather than waiting for a review round."""
        head = GUARD._assertion_vocabulary().size_head
        for name in GUARD._doctest_assert_macros():
            self.assertIsNotNone(head.match(f"{name}(v.size() == 2)"), name)
            self.assertIsNotNone(
                GUARD._assertion_vocabulary().scan_through.match(f"{name}(x);"), name
            )

    def test_the_macro_name_reported_is_the_one_written(self):
        site = self.sites("  DOCTEST_CHECK(v.size() == 2);\n  CHECK(v[0]);")
        self.assertEqual([s[2] for s in site], ["DOCTEST_CHECK"])


class NullDerefVocabularyIsDerived(ScanTestCase):
    """Detector 1's accepted heads are derived too - the THIRD spelling site.

    `CHECK` is not the weaker case: it never aborts under ANY doctest
    configuration, where `REQUIRE` merely does not abort in this build. Detector 1
    spelled `REQUIRE` and its suffixes by hand and so could not see any of it. The
    18 sites this exposed in the corpus are enumerated in the baseline.
    """

    def test_check_null_then_deref_is_a_site(self):
        self.assertFlagged("  CHECK(ptr != nullptr);\n  ptr->method();", "ptr")

    def test_check_message_is_valid_then_deref_is_a_site(self):
        self.assertFlagged(
            '  CHECK_MESSAGE(ref.is_valid(), "needed");\n  ref->f();', "ref"
        )

    def test_check_false_is_null_then_deref_is_a_site(self):
        self.assertFlagged("  CHECK_FALSE(ref.is_null());\n  ref->f();", "ref")

    def test_check_ne_nullptr_then_deref_is_a_site(self):
        self.assertFlagged("  CHECK_NE(ptr, nullptr);\n  ptr->method();", "ptr")

    def test_warn_null_then_deref_is_a_site(self):
        self.assertFlagged("  WARN(ptr != nullptr);\n  ptr->method();", "ptr")

    def test_prefixed_and_fast_spellings_are_the_same_macros(self):
        self.assertFlagged("  DOCTEST_REQUIRE(ptr != nullptr);\n  ptr->method();", "ptr")
        self.assertFlagged("  FAST_CHECK_UNARY(ref.is_valid());\n  ref->f();", "ref")

    def test_a_relational_macro_that_is_not_NE_is_not_a_null_guard(self):
        """`CHECK_EQ(ptr, nullptr)` asserts the pointer IS null. Reading it as a
        guard would name the wrong assertion on a real crash."""
        self.assertClean("  CHECK_EQ(ptr, nullptr);\n  ptr->method();")
        self.assertClean("  CHECK_LT(ptr, nullptr);\n  ptr->method();")

    def test_godots_WARN_PRINT_is_not_a_null_guard(self):
        self.assertClean('  WARN_PRINT("ptr != nullptr");\n  ptr->method();')


class OneSourceOfTruthForAssertionNames(unittest.TestCase):
    """No fifth hand-written spelling may be added (round 9).

    Rounds 1, 8 and 9 all produced a finding of the same shape - an external
    vocabulary spelled by hand - and round 8's structural claim failed precisely
    because deriving the family did not stop a second spelling from gating it.
    So the invariant is machine-checked over this file's own source rather than
    asserted in prose: every regex that names a doctest macro lives inside
    `_assertion_vocabulary`.
    """

    def _module_source(self) -> str:
        return (CI_DIR / "check_require_null_deref.py").read_text(encoding="utf-8")

    def test_every_macro_naming_regex_lives_in_the_vocabulary(self):
        import ast

        source = self._module_source()
        tree = ast.parse(source)
        vocabulary = next(
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.FunctionDef) and node.name == "_assertion_vocabulary"
        )
        offenders = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            target = node.func
            if not (isinstance(target, ast.Attribute) and target.attr == "compile"):
                continue
            text = ast.get_source_segment(source, node) or ""
            if "REQUIRE" not in text and "CHECK" not in text:
                continue
            if vocabulary.lineno <= node.lineno <= (vocabulary.end_lineno or 0):
                continue
            offenders.append((node.lineno, text.splitlines()[0]))
        self.assertEqual(
            offenders,
            [],
            "a doctest macro name is spelled outside _assertion_vocabulary - that is "
            "the second source of truth #849 round 9 was about",
        )

    def test_an_empty_semantic_bucket_fails_closed(self):
        """Deriving no negating macro must be a ScanError, not 'there are none'."""
        with self.assertRaises(GUARD.ScanError):
            GUARD._macro_alternation(lambda semantics: False, "impossible")


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
        """GS-AUDIT-TEST-003: `main()` now also grades both baselines against the
        REVIEW BASE, which needs something for `resolve_review_base(None)` to fall
        back to (`origin/master` then `master`) when nothing explicit is set. That
        ref topology is a property of the checkout, not of this guard, and Codex
        review found it missing in the checkout this PR was graded in (a topic-branch
        clone with neither ref) -- exactly the class of environment an isolated
        agent worktree can have. `--base-ref HEAD` sidesteps it: `merge-base(HEAD,
        HEAD)` is HEAD itself, so it resolves in ANY git repository regardless of
        what other branches exist, while still exercising the real review-base code
        path (resolution, base-content fetch, comparison) end to end. The FALLBACK
        CHAIN itself (unset ref -> origin/master -> master) is covered separately,
        in complete isolation from this checkout's topology, by
        `ResolveReviewBaseAgainstRealGit` below.
        """
        self.assertEqual(GUARD.main(["--base-ref", "HEAD"]), 0)


class SiteKeyingIsPathBased(unittest.TestCase):
    """GS-AUDIT-TEST-003: sites are keyed by repo-relative path, not basename.

    `modules/gaussian_splatting/tests/test_utils.h` and `tests/test_utils.h` both
    exist in this tree and motivate the fix, though `_test_sources()`'s
    `ENGINE_TESTS_DIR` glob (`test_*.cpp` only) means that exact pair is not an
    ACTIVE collision for this guard today -- see `_site_key`'s docstring. A
    basename key collides between ANY same-named pair the scan DOES admit (a `.cpp`
    pair across the two directories, today), and `results[key] = sites` -- plain
    dict assignment -- silently OVERWRITES rather than merges, so one file's sites
    vanish. These cases construct that exact shape directly (bypassing the real
    glob's suffix restriction, since the collision mechanism being tested is the
    KEYING, not the glob) and prove both halves: the keys come out distinct, and
    neither file's site is lost under the other's.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        # Nested the same way the real tree is (ROOT/modules/gaussian_splatting/tests
        # and ROOT/tests), so `_site_key`'s PRIMARY branch (relative_to ROOT) produces
        # genuinely distinct repo-relative paths -- not its tempdir fallback, which
        # (like the sibling's source_key() it mirrors) can only recover the bare name
        # when the scan roots are not nested under a common ROOT.
        self.root = Path(self._tmp.name)
        self.module_dir = self.root / "modules" / "gaussian_splatting" / "tests"
        self.engine_dir = self.root / "tests"
        self.module_dir.mkdir(parents=True)
        self.engine_dir.mkdir(parents=True)

        self._saved = {
            name: getattr(GUARD, name) for name in ("ROOT", "MODULE_TESTS_DIR", "ENGINE_TESTS_DIR")
        }
        self.addCleanup(lambda: [setattr(GUARD, k, v) for k, v in self._saved.items()])
        GUARD.ROOT = self.root
        GUARD.MODULE_TESTS_DIR = self.module_dir
        GUARD.ENGINE_TESTS_DIR = self.engine_dir

    def test_distinct_directories_get_distinct_keys(self):
        module_file = self.module_dir / "test_utils.h"
        engine_file = self.engine_dir / "test_utils.h"
        module_file.write_text(
            'TEST_CASE("[Synthetic] module") {\n'
            "  REQUIRE(module_only != nullptr);\n"
            "  module_only->method();\n"
            "}\n",
            encoding="utf-8",
        )
        engine_file.write_text(
            'TEST_CASE("[Synthetic] engine") {\n'
            "  REQUIRE(engine_only != nullptr);\n"
            "  engine_only->method();\n"
            "}\n",
            encoding="utf-8",
        )
        # _test_sources() globs MODULE_TESTS_DIR/*.h and ENGINE_TESTS_DIR/test_*.cpp,
        # neither of which matches a bare `test_utils.h` sitting directly under
        # ENGINE_TESTS_DIR -- irrelevant to what this proves (the KEY collision), so
        # the source list is supplied directly rather than widening what is scanned.
        original_test_sources = GUARD._test_sources
        GUARD._test_sources = lambda: sorted([module_file, engine_file])
        self.addCleanup(lambda: setattr(GUARD, "_test_sources", original_test_sources))

        found = GUARD.scan_all()
        self.assertEqual(
            set(found), {"modules/gaussian_splatting/tests/test_utils.h", "tests/test_utils.h"},
            f"expected two distinct repo-relative keys, got {sorted(found)}",
        )
        module_symbols = {v[1] for v in found["modules/gaussian_splatting/tests/test_utils.h"]}
        engine_symbols = {v[1] for v in found["tests/test_utils.h"]}
        self.assertEqual(module_symbols, {"module_only"})
        self.assertEqual(engine_symbols, {"engine_only"})


class ReviewBaseGrowthCheck(unittest.TestCase):
    """GS-AUDIT-TEST-003: the baseline is graded against the REVIEW BASE, not only
    against its own working-tree copy -- closing the hole where a change adds a
    violation AND appends its fingerprint to the baseline in the SAME commit, which
    a working-tree-only comparison cannot see because both sides moved together.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.module_dir = root / "modules_tests"
        self.engine_dir = root / "engine_tests"
        self.module_dir.mkdir()
        self.engine_dir.mkdir()

        self._saved = {
            name: getattr(GUARD, name)
            for name in (
                "MODULE_TESTS_DIR",
                "ENGINE_TESTS_DIR",
                "BASELINE_PATH",
                "SIZE_INDEX_BASELINE_PATH",
                "resolve_review_base",
                "_blob_at_base",
                "detector_differs_from_base",
                "_rescan_base_content",
            )
        }
        self.addCleanup(lambda: [setattr(GUARD, k, v) for k, v in self._saved.items()])

        GUARD.MODULE_TESTS_DIR = self.module_dir
        GUARD.ENGINE_TESTS_DIR = self.engine_dir
        GUARD.BASELINE_PATH = root / "require_null_deref_baseline.json"
        GUARD.SIZE_INDEX_BASELINE_PATH = root / "size_then_index_baseline.json"

    def _stub_base(
        self,
        base_null_deref_files,
        base_size_index_files=None,
        base_sha="feedfacecafe0011",
        detector_differs=False,
        rescan_results=None,
    ):
        """`rescan_results`, when given, maps a scan key to what
        `_rescan_base_content` should report for it: (fingerprints-or-None, failures).
        A key not present raises -- the lenient (detector-differs) route must never
        query a key the test did not anticipate, since an unanticipated fallback
        answer is exactly how the pre-fix flattened pool went unnoticed.
        """
        GUARD.resolve_review_base = lambda base_ref=None: (base_sha, [])
        GUARD.detector_differs_from_base = lambda sha: (detector_differs, [])

        def _blob(sha, path):
            name = Path(path).name
            if name == GUARD.BASELINE_PATH.name:
                if base_null_deref_files is None:
                    return GUARD.ABSENT_AT_BASE, []
                return json.dumps({"schema_version": 1, "files": base_null_deref_files}), []
            if name == GUARD.SIZE_INDEX_BASELINE_PATH.name:
                files = {} if base_size_index_files is None else base_size_index_files
                return json.dumps({"schema_version": 1, "files": files}), []
            raise AssertionError(f"unexpected _blob_at_base call for {path}")

        GUARD._blob_at_base = _blob

        results = {} if rescan_results is None else rescan_results

        def _rescan(name, sha, scan_kind):
            if name not in results:
                raise AssertionError(
                    f"unexpected _rescan_base_content call for {name!r} "
                    f"(scan_kind={scan_kind!r}); the lenient route must only query "
                    f"keys the test explicitly stubbed a rescan answer for"
                )
            return results[name]

        GUARD._rescan_base_content = _rescan

    def _write_violation(self, name: str = "test_mutation_proof.h") -> tuple[str, list[str]]:
        (self.module_dir / name).write_text(
            'TEST_CASE("[Synthetic] joint mutation") {\n'
            "  REQUIRE(ptr != nullptr);\n"
            "  ptr->method();\n"
            "}\n",
            encoding="utf-8",
        )
        found_prints = GUARD.scan_fingerprints()
        self.assertEqual(len(found_prints), 1, found_prints)
        [(key, prints)] = found_prints.items()
        self.assertEqual(len(prints), 1, prints)
        return key, prints

    def test_joint_mutation_is_caught(self):
        """The exact shape GS-AUDIT-TEST-003 names: a REQUIRE-then-deref site and its
        baseline fingerprint land in the SAME change. The working-tree comparison
        alone reports 0 new (both sides moved together, by construction below); the
        review-base comparison must still fail the run.
        """
        key, prints = self._write_violation()

        # The joint mutation: the working tree's OWN baseline already carries the new
        # fingerprint, so it agrees with the scan (0 new / 0 stale on that check alone).
        GUARD.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {key: prints}}), encoding="utf-8"
        )
        GUARD.SIZE_INDEX_BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8"
        )

        # At the review base this fingerprint never existed, under this key or any
        # other. detector_differs=False: an ordinary PR that only edits a test source
        # and a baseline never touches this detector script itself.
        self._stub_base(base_null_deref_files={}, base_size_index_files={})

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = GUARD.main()
        output = buffer.getvalue()
        self.assertEqual(
            code, 1,
            "a violation and its baseline entry added in the same change must fail "
            "the review-base comparison even though the working-tree copy agrees:\n" + output,
        )
        self.assertIn("NEW relative to review base", output, output)
        self.assertIn(prints[0], output, output)

    def test_joint_mutation_is_caught_for_size_then_index(self):
        """The size-then-index baseline needs the SAME review-base protection as the
        null-deref one, but `main()` grades the two through a SEPARATE call each
        (once per baseline in its grading loop) -- without a test that exercises the
        size-then-index call specifically, THAT entry could be dropped from the
        loop and nothing above would notice, since every other test here only
        supplies a null-deref violation.
        """
        (self.module_dir / "test_size_mutation.h").write_text(
            'TEST_CASE("[Synthetic] size joint mutation") {\n'
            "  REQUIRE(container.size() == 4);\n"
            "  CHECK(container[0] == 1);\n"
            "}\n",
            encoding="utf-8",
        )
        size_prints, size_errors = GUARD.scan_size_index_fingerprints()
        self.assertEqual(size_errors, [])
        self.assertEqual(len(size_prints), 1, size_prints)
        [(key, prints)] = size_prints.items()
        self.assertEqual(len(prints), 1, prints)

        GUARD.BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8"
        )
        # The joint mutation, on the SIZE-INDEX baseline this time: the working
        # tree's own copy already carries the new fingerprint.
        GUARD.SIZE_INDEX_BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {key: prints}}), encoding="utf-8"
        )
        self._stub_base(base_null_deref_files={}, base_size_index_files={})

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = GUARD.main()
        output = buffer.getvalue()
        self.assertEqual(
            code, 1,
            "a size-then-index site and its baseline entry added in the same "
            "change must fail the review-base comparison too:\n" + output,
        )
        self.assertIn("[size-then-index] FAIL", output, output)
        self.assertIn("NEW relative to review base", output, output)
        self.assertIn(prints[0], output, output)

    def test_unresolvable_review_base_fails_closed(self):
        """No base means no reference. It must never degrade to grading nothing."""
        (self.module_dir / "test_clean.h").write_text(
            'TEST_CASE("[Synthetic] clean") { int x = 1; (void)x; }\n', encoding="utf-8"
        )
        GUARD.resolve_review_base = lambda base_ref=None: (None, ["no base reachable"])
        GUARD._blob_at_base = lambda sha, path: (_ for _ in ()).throw(
            AssertionError("must not be called when the base did not resolve")
        )
        GUARD.detector_differs_from_base = lambda sha: (_ for _ in ()).throw(
            AssertionError("must not be called when the base did not resolve")
        )
        GUARD._rescan_base_content = lambda name, sha, scan_kind: (_ for _ in ()).throw(
            AssertionError("must not be called when the base did not resolve")
        )

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = GUARD.main()
        self.assertEqual(code, 1, "an unresolvable review base must fail, not pass")
        self.assertIn("cannot resolve the review base", buffer.getvalue())

    def test_a_pure_rekey_is_not_rejected(self):
        """This PR's own transition: the SAME fingerprint, at a DIFFERENT (basename)
        key in the committed base baseline, must not be rejected when the detector --
        which derives the key -- genuinely changed in this diff, PROVIDED the file's
        own content at the base, rescanned, still contains it. Measured for real
        against the actual repo corpus too (see the PR body): regenerating both
        baselines after the `_site_key` change reports 0 refused additions and the
        guard's own review-base check reports "0 new" against origin/master. This is
        the precise, synthetic version of that same claim, isolating
        `_baseline_growth_vs_base` from the rest of `main()`'s wiring (covered
        separately by `test_joint_mutation_is_caught`).
        """
        fingerprint = "ptr|!= nullptr|deadbeef00"
        self._stub_base(
            base_null_deref_files={"old_basename.h": [fingerprint]},
            base_size_index_files={},
            detector_differs=True,
            # The rekeyed file's OWN content at the base, rescanned with the current
            # detector, reproduces the exact fingerprint the committed baseline
            # recorded under its old key -- a pure rename touches no C++ at all.
            rescan_results={"new/nested/path.h": ([fingerprint], [])},
        )
        base_sha, base_failures = GUARD.resolve_review_base()
        self.assertEqual(base_failures, [])
        new_relative, growth_failures, introduced = GUARD._baseline_growth_vs_base(
            {"new/nested/path.h": [fingerprint]}, GUARD.BASELINE_PATH, base_sha, True, "null_deref"
        )
        self.assertEqual(growth_failures, [])
        self.assertFalse(introduced)
        self.assertEqual(
            new_relative, {},
            "a fingerprint the file's OWN base-commit content still contains, "
            "rescanned, must not be treated as new when the detector genuinely "
            "changed (a pure rekey)",
        )

    def test_a_same_key_duplicate_reveals_exactly_the_new_copy(self):
        """A same-key rescan must not let the base's supply cover the same
        occurrences twice (regression: an earlier version of this fix subtracted the
        rescan from the ALREADY-reduced `added` instead of from `current[name]`
        directly, so a rescan that merely reproduced the committed baseline
        1-for-1 -- the ordinary, honest case -- silently absolved every excess copy).

        The base holds two copies of a duplicated fingerprint under one key, matched
        exactly by a rescan of that key's own base content (an honest, unremarkable
        rescan, not a laundering one). The current scan holds three copies under that
        SAME key: exactly one is genuinely new and must be reported; the other two
        must not be re-flagged just because a rescan happened to run at all.
        """
        fingerprint = "dup|!= nullptr|deadbeef00"
        self._stub_base(
            base_null_deref_files={"same_key.h": [fingerprint, fingerprint]},
            base_size_index_files={},
            detector_differs=True,
            rescan_results={"same_key.h": ([fingerprint, fingerprint], [])},
        )
        base_sha, base_failures = GUARD.resolve_review_base()
        self.assertEqual(base_failures, [])
        new_relative, growth_failures, introduced = GUARD._baseline_growth_vs_base(
            {"same_key.h": [fingerprint, fingerprint, fingerprint]},
            GUARD.BASELINE_PATH, base_sha, True, "null_deref",
        )
        self.assertEqual(growth_failures, [])
        self.assertFalse(introduced)
        self.assertEqual(
            new_relative, {"same_key.h": [fingerprint]},
            "exactly the one genuinely new copy must be reported -- neither zero "
            "(the base's 2 recorded copies covering all 3 current ones) nor three "
            "(the rescan re-flagging what the committed baseline already covered)",
        )

    def test_a_different_key_does_not_license_growth_without_a_detector_change(self):
        """The lenient (rekey) route must not be reachable by an ordinary PR that
        never touches the detector -- otherwise it is a second way to write anything
        into the baseline, exactly what GS-AUDIT-TEST-003 exists to close.

        `rescan_results={}` (no stubbed answer for any key): if the code under test
        reached the rescan at all, `_stub_base`'s `_rescan` raises `AssertionError`
        for the unanticipated key, which -- since `_baseline_growth_vs_base` does not
        catch it -- would fail this test with an error rather than a clean assertion
        mismatch. That the test instead reaches its normal assertion is itself part
        of the proof that `detector_differs=False` never calls the rescan.
        """
        fingerprint = "ptr|!= nullptr|deadbeef00"
        self._stub_base(
            base_null_deref_files={"old_basename.h": [fingerprint]},
            base_size_index_files={},
            detector_differs=False,
        )
        base_sha, base_failures = GUARD.resolve_review_base()
        self.assertEqual(base_failures, [])
        new_relative, growth_failures, introduced = GUARD._baseline_growth_vs_base(
            {"new/nested/path.h": [fingerprint]}, GUARD.BASELINE_PATH, base_sha, False, "null_deref"
        )
        self.assertEqual(growth_failures, [])
        self.assertFalse(introduced)
        self.assertEqual(
            new_relative, {"new/nested/path.h": [fingerprint]},
            "a fingerprint recorded at a DIFFERENT key must be treated as new when "
            "the detector did not change -- otherwise renaming a file would launder "
            "an unrelated pre-existing fingerprint onto it",
        )

    def test_cross_file_laundering_is_rejected(self):
        """The verifier's reproduction (GS-AUDIT-TEST-003 round 2): remove a
        baselined site from file A, add BYTE-IDENTICAL code (same fingerprint) to a
        DIFFERENT file B, sync the committed baseline. An earlier version of this fix
        drew from a repo-wide flattened pool of every fingerprint anywhere in the
        base baseline, so A's now-orphaned fingerprint -- never tied to A specifically
        once dropped from the pool's bookkeeping -- was free to license B's "new"
        occurrence. The fix restricts the proof to B's OWN base-commit content: B
        never contained this code at the base, so B's rescan (stubbed here to prove
        exactly that) is empty, and the addition is rejected regardless of what
        happened in A or anywhere else in the base baseline.
        """
        fingerprint = "ptr|!= nullptr|deadbeef00"
        self._stub_base(
            # A's occurrence was fixed: the COMMITTED baseline no longer lists it
            # under A at all (this diff's own baseline edit already removed it), so
            # it plays no role in `base_files` here -- the whole point is that base
            # bookkeeping for A must not leak into B's claim.
            base_null_deref_files={},
            base_size_index_files={},
            detector_differs=True,
            # B's OWN base-commit content never had this fingerprint.
            rescan_results={"modules/gaussian_splatting/tests/test_utils.h": ([], [])},
        )
        base_sha, base_failures = GUARD.resolve_review_base()
        self.assertEqual(base_failures, [])
        new_relative, growth_failures, introduced = GUARD._baseline_growth_vs_base(
            {"modules/gaussian_splatting/tests/test_utils.h": [fingerprint]},
            GUARD.BASELINE_PATH, base_sha, True, "null_deref",
        )
        self.assertEqual(growth_failures, [])
        self.assertFalse(introduced)
        self.assertEqual(
            new_relative,
            {"modules/gaussian_splatting/tests/test_utils.h": [fingerprint]},
            "byte-identical code copied into a file that never contained it at the "
            "base must be reported as new, regardless of what was fixed elsewhere",
        )

    def test_a_legitimate_detector_improvement_over_unchanged_content_passes(self):
        """Requirement (c): a real detector improvement -- one that reveals a site
        which was always there, in a file whose C++ content did not change -- must
        still PASS, or this fix over-tightens into blocking legitimate refactors
        (the same failure mode #849 round 9 is this guard's own precedent for:
        widening the accepted assertion-macro set surfaced 18 pre-existing sites,
        319 -> 337, none of them new code).

        The committed baseline already covers one of the two fingerprints the widened
        detector now finds in this file; the second was always there too, and a
        rescan of the file's OWN (unchanged) base content -- run through the CURRENT,
        widened detector -- finds both, because the content never changed.
        """
        already_recorded = "old|CHECK|aaaaaaaaaa"
        newly_surfaced = "old|WARN|bbbbbbbbbb"
        self._stub_base(
            base_null_deref_files={"c.h": [already_recorded]},
            base_size_index_files={},
            detector_differs=True,
            rescan_results={"c.h": ([already_recorded, newly_surfaced], [])},
        )
        base_sha, base_failures = GUARD.resolve_review_base()
        self.assertEqual(base_failures, [])
        new_relative, growth_failures, introduced = GUARD._baseline_growth_vs_base(
            {"c.h": [already_recorded, newly_surfaced]},
            GUARD.BASELINE_PATH, base_sha, True, "null_deref",
        )
        self.assertEqual(growth_failures, [])
        self.assertFalse(introduced)
        self.assertEqual(
            new_relative, {},
            "a genuinely pre-existing site the widened detector newly recognizes in "
            "UNCHANGED content must pass, not be rejected as new growth",
        )

    def test_baseline_absent_at_base_is_reported_not_failed(self):
        """A change that INTRODUCES a baseline has no shrink-only reference; say so,
        and do not fail the run over it."""
        (self.module_dir / "test_clean.h").write_text(
            'TEST_CASE("[Synthetic] clean") { int x = 1; (void)x; }\n', encoding="utf-8"
        )
        GUARD.BASELINE_PATH.write_text(json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8")
        GUARD.SIZE_INDEX_BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8"
        )
        self._stub_base(base_null_deref_files=None, base_size_index_files=None)
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = GUARD.main()
        self.assertEqual(code, 0, buffer.getvalue())
        self.assertIn("NOTE", buffer.getvalue())

    def test_regenerate_refuses_a_genuinely_new_fingerprint(self):
        """`_refused_flattened_additions` (GS-AUDIT-TEST-003) is the one deliberate
        loosening in this change: both regenerate tools compare the freshly scanned
        baseline against the EXISTING one's FLATTENED fingerprint set, not per-key,
        because the basename -> path rekey moves every entry to a new key in one
        commit and a per-key comparison would refuse to regenerate ANYTHING. That
        loosening must still refuse a fingerprint that is not present anywhere in
        the existing baseline; if `_refused_flattened_additions` were gutted to
        `return []`, this is the test that would notice -- neither this test nor
        `--regenerate-null-deref-baseline` touches the review-base machinery at
        all (`main()` dispatches to `_regenerate_null_deref_baseline()` before ever
        resolving a base), so `_stub_base` is not needed here.
        """
        (self.module_dir / "test_regen.h").write_text(
            'TEST_CASE("[Synthetic] regen") {\n'
            "  REQUIRE(ptr != nullptr);\n"
            "  ptr->method();\n"
            "}\n",
            encoding="utf-8",
        )
        # An EXISTING baseline that does not contain the fingerprint the scan above
        # will find, under this key or any other.
        GUARD.BASELINE_PATH.write_text(
            json.dumps(
                {"schema_version": 1, "files": {"unrelated.h": ["other|is_valid()|deadbeef00"]}}
            ),
            encoding="utf-8",
        )
        GUARD.SIZE_INDEX_BASELINE_PATH.write_text(
            json.dumps({"schema_version": 1, "files": {}}), encoding="utf-8"
        )
        before = GUARD.BASELINE_PATH.read_text(encoding="utf-8")

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = GUARD.main(["--regenerate-null-deref-baseline"])
        self.assertEqual(
            code, 1, "regeneration that adds a genuinely new fingerprint must be refused"
        )
        self.assertIn("REFUSED", buffer.getvalue())
        self.assertEqual(
            GUARD.BASELINE_PATH.read_text(encoding="utf-8"), before,
            "a refused regeneration must not have written the baseline",
        )


_BASE_ENV_VARS = (
    "GS_CI_ENV_SKIP_BASE_REF",
    "GS_CI_BASE_REF",
    "GITHUB_BASE_SHA",
    "GITHUB_BASE_REF",
)


class ResolveReviewBaseAgainstRealGit(unittest.TestCase):
    """A thin, non-mocked check that the git plumbing itself fails closed -- the
    mocked cases above stub `resolve_review_base` and cannot catch a regression in
    the function itself.

    GS-AUDIT-TEST-003 (Codex + independent review, round 2): the first version of
    this class ran `resolve_review_base()` against the AMBIENT checkout, so its
    fallback-chain assertion depended on that checkout having `origin/master` or
    `master` -- absent in a checkout containing only a topic branch, which both
    reviews reproduced. `_git`'s subprocess calls run with `cwd=ROOT`, and `ROOT` is
    computed fresh, from `__file__`, by the DYNAMICALLY IMPORTED copy of
    check_environment_skip_marker.py that `resolve_review_base()` loads each call --
    so isolating this test means giving that import a real file to load from INSIDE
    a disposable fixture repo, not merely monkeypatching a global on this module (a
    monkeypatched `GUARD.ROOT` would not reach it: the loaded copy's `ROOT` is its
    own global, unrelated to this module's). The fixture below copies the resolver
    to the same relative path it lives at for real (`tests/ci/...`) inside a fresh
    git repo, points `BASE_RESOLVER_PATH` at that copy, and builds branches with
    real git commands -- mirroring test_check_environment_skip_marker.py's own
    `git branch -f master HEAD` fixture pattern for the same function one layer
    down.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        (self.repo / "tests" / "ci").mkdir(parents=True)

        real_resolver = GUARD.BASE_RESOLVER_PATH
        fixture_resolver = self.repo / "tests" / "ci" / "check_environment_skip_marker.py"
        fixture_resolver.write_text(real_resolver.read_text(encoding="utf-8"), encoding="utf-8")

        self._saved_resolver_path = GUARD.BASE_RESOLVER_PATH
        GUARD.BASE_RESOLVER_PATH = fixture_resolver
        self.addCleanup(lambda: setattr(GUARD, "BASE_RESOLVER_PATH", self._saved_resolver_path))

        self._git("init", "-q", "-b", "topic")
        self._git("config", "user.email", "t@example.com")
        self._git("config", "user.name", "t")
        (self.repo / "seed.txt").write_text("seed\n", encoding="utf-8")
        self._git("add", "-A")
        self._git("commit", "-q", "-m", "seed")

    def _git(self, *args: str) -> None:
        subprocess.run(
            ["git", *args], cwd=self.repo, check=True, capture_output=True, text=True
        )

    def _resolve_with_clean_env(self, base_ref=None):
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in _BASE_ENV_VARS:
                os.environ.pop(name, None)
            return GUARD.resolve_review_base(base_ref)

    def test_an_unresolvable_named_ref_fails_closed(self):
        base_sha, failures = self._resolve_with_clean_env(
            "this-ref-does-not-exist-anywhere-gs-audit-test-003"
        )
        self.assertIsNone(base_sha)
        self.assertTrue(failures)

    def test_no_master_ref_at_all_fails_closed(self):
        """A checkout with only a topic branch -- no `master`, no `origin/master` --
        must fail closed, never raise and never silently pass. This is the exact
        checkout shape both reviews reproduced against the pre-fix version of this
        test."""
        base_sha, failures = self._resolve_with_clean_env(None)
        self.assertIsNone(base_sha)
        self.assertTrue(failures)
        self.assertIn("cannot resolve the review base", failures[0])

    def test_head_resolves_against_master_when_present(self):
        """No explicit ref, `master` present locally: falls back to it -- exactly
        like check_environment_skip_marker.py's `resolve_base_sha`, which this
        delegates to."""
        self._git("branch", "-f", "master", "HEAD")
        base_sha, failures = self._resolve_with_clean_env(None)
        self.assertEqual(failures, [], failures)
        self.assertIsNotNone(base_sha)
        self.assertRegex(base_sha, r"^[0-9a-f]{40}$")

    def test_head_resolves_against_origin_master_when_present(self):
        """No explicit ref, only `origin/master` (a remote-tracking ref, no local
        `master`) present: falls back to it. Under `actions/checkout` the base
        branch typically exists ONLY this way."""
        self._git("update-ref", "refs/remotes/origin/master", "HEAD")
        base_sha, failures = self._resolve_with_clean_env(None)
        self.assertEqual(failures, [], failures)
        self.assertIsNotNone(base_sha)
        self.assertRegex(base_sha, r"^[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
