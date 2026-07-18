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
