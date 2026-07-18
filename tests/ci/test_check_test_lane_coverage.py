#!/usr/bin/env python3
"""Unit test for tests/ci/check_test_lane_coverage.py (#520).

Pins the two properties the guard's value depends on:

* the matcher reproduces **doctest's** wildcard semantics, not `fnmatch`'s, and
* a wildcard declaration in `unlaned_tests` cannot silently amnesty cases written
  after it.

The second was a demonstrated hole: family patterns like `[TileRenderer]*` are
open-ended, so a brand-new stranded case joining an already-declared family used
to pass. The per-entry `count` closes it, and these cases keep it closed.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

CI_DIR = Path(__file__).resolve().parent


def _load_guard():
    spec = importlib.util.spec_from_file_location(
        "_gs_check_test_lane_coverage", CI_DIR / "check_test_lane_coverage.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["_gs_check_test_lane_coverage"] = module
    spec.loader.exec_module(module)
    return module


GUARD = _load_guard()


class DoctestWildcardSemantics(unittest.TestCase):
    """`[` and `]` are literal in doctest; `fnmatch` would call them a char class."""

    def test_brackets_are_literal(self):
        name = "[GaussianSplatting][World][SceneTree] World node forwards overrides"
        self.assertTrue(GUARD._doctest_wildcmp(name, "*GaussianSplatting*][SceneTree]*"))
        self.assertTrue(GUARD._doctest_wildcmp(name, "*][World][SceneTree]*"))

    def test_disagrees_with_fnmatch_on_the_real_patterns(self):
        import fnmatch

        name = "[GaussianSplatting][World][SceneTree] World node forwards overrides"
        for pattern in ("*GaussianSplatting*][SceneTree]*", "*][World][SceneTree]*"):
            self.assertTrue(GUARD._doctest_wildcmp(name, pattern))
            self.assertFalse(
                fnmatch.fnmatch(name, pattern),
                "fnmatch agreeing here would mean this test proves nothing",
            )

    def test_matching_is_case_insensitive(self):
        # doctest's case_sensitive option defaults to false.
        self.assertTrue(GUARD._doctest_wildcmp("[Tag] Name", "*[tag]*"))
        self.assertTrue(GUARD._doctest_wildcmp("[tag] name", "*[TAG]*"))

    def test_question_mark_matches_exactly_one_character(self):
        self.assertTrue(GUARD._doctest_wildcmp("abc", "a?c"))
        self.assertFalse(GUARD._doctest_wildcmp("ac", "a?c"))

    def test_star_matches_empty(self):
        self.assertTrue(GUARD._doctest_wildcmp("ab", "a*b"))
        self.assertTrue(GUARD._doctest_wildcmp("axxb", "a*b"))
        self.assertFalse(GUARD._doctest_wildcmp("axxc", "a*b"))

    def test_anchored_without_stars(self):
        self.assertTrue(GUARD._doctest_wildcmp("exact", "exact"))
        self.assertFalse(GUARD._doctest_wildcmp("exact tail", "exact"))


class LaneMatching(unittest.TestCase):
    def test_exclude_beats_include(self):
        name = "[GaussianSplatting][World][SceneTree] case"
        self.assertFalse(
            GUARD._lane_matches(name, ("*GaussianSplatting*][SceneTree]*",), ("*][World][SceneTree]*",))
        )

    def test_include_without_matching_exclude(self):
        name = "[GaussianSplatting][Node][SceneTree] case"
        self.assertTrue(
            GUARD._lane_matches(name, ("*GaussianSplatting*][SceneTree]*",), ("*][World][SceneTree]*",))
        )


class DeclarationCounts(unittest.TestCase):
    """A family wildcard must not amnesty cases written after the declaration."""

    def test_required_fields_include_count(self):
        self.assertIn("count", GUARD.UNLANED_REQUIRED_FIELDS)

    def test_shipped_declarations_are_well_formed(self):
        declarations, problems = GUARD._load_unlaned_declarations()
        self.assertEqual(problems, [])
        self.assertTrue(declarations)
        for entry in declarations:
            self.assertIsInstance(entry["count"], int)
            self.assertGreaterEqual(entry["count"], 1)

    def test_declared_counts_equal_the_real_stranded_counts(self):
        """The shipped counts must equal reality, so a new case cannot hide."""
        declarations, problems = GUARD._load_unlaned_declarations()
        self.assertEqual(problems, [])

        runner = GUARD._load_module("_rmt_for_test", GUARD.CI_DIR / "run_module_tests.py")
        harness = GUARD._load_module("_rgh_for_test", GUARD.CI_DIR / "run_gpu_harness.py")
        linkage = GUARD._load_module("_ctl_for_test", GUARD.CI_DIR / "check_test_linkage.py")
        cases, _ = GUARD._collect_corpus(linkage._strip_comments)

        module_lanes = list(runner.MODULE_TEST_FILTERS) + list(runner.REQUIRES_RD_TEST_FILTERS)
        batches = [(b.name, tuple(b.filters)) for b in harness.BATCHES]

        stranded = [
            name
            for name, _ in cases
            if not any(GUARD._lane_matches(name, inc, exc) for _, inc, exc, _ in module_lanes)
            and not any(
                GUARD._doctest_wildcmp(name, pattern) for _, filters in batches for pattern in filters
            )
        ]

        for entry in declarations:
            actual = sum(
                1 for name in stranded if GUARD._doctest_wildcmp(name, str(entry["test_case"]))
            )
            self.assertEqual(
                actual,
                entry["count"],
                f"{entry['test_case']!r}: declares {entry['count']} but matches {actual} "
                f"stranded case(s)",
            )

    def test_a_new_case_in_a_declared_family_is_not_covered_by_the_wildcard_alone(self):
        """The pattern still matches it - only the count makes it visible."""
        declarations, _ = GUARD._load_unlaned_declarations()
        tile = next(e for e in declarations if e["test_case"] == "[TileRenderer]*")
        newcomer = "[TileRenderer] brand new stranded case added long after the declaration"
        self.assertTrue(
            GUARD._doctest_wildcmp(newcomer, tile["test_case"]),
            "the wildcard does match it - which is exactly why a count is required",
        )


class CorpusScope(unittest.TestCase):
    def test_engine_tree_is_scanned_recursively(self):
        linkage = GUARD._load_module("_ctl_for_scope", GUARD.CI_DIR / "check_test_linkage.py")
        cases, _ = GUARD._collect_corpus(linkage._strip_comments)
        names = [name for name, _ in cases]
        self.assertIn(
            "[RendererSceneCull] Hidden indexing policy gates Gaussian exemption",
            names,
            "a nested engine-tree case that mentions Gaussian must be in the corpus",
        )

    def test_untagged_gaussian_cases_are_included(self):
        # The RendererSceneCull case carries no [GaussianSplatting] tag; a
        # tag-filtered scan would drop it.
        self.assertNotIn(
            "[gaussiansplatting]",
            "[RendererSceneCull] Hidden indexing policy gates Gaussian exemption".lower(),
        )


class GuardPasses(unittest.TestCase):
    def test_guard_passes_on_the_current_tree(self):
        self.assertEqual(GUARD.main(), 0)


if __name__ == "__main__":
    unittest.main()
