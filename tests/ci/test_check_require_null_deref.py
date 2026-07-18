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
