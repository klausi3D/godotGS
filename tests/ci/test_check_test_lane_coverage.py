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
        """The shipped counts must equal reality, so a new case cannot hide.

        Uses the guard's own analyze()/attribute() rather than re-deriving the
        lane rules: an earlier version of this test duplicated them and silently
        disagreed once the guard stopped crediting the requires-RD catalogue.
        """
        declarations, problems = GUARD._load_unlaned_declarations()
        self.assertEqual(problems, [])
        analysis = GUARD.analyze()
        matched_by, undeclared = GUARD.attribute(analysis.stranded, declarations)
        self.assertEqual(undeclared, [], "every stranded case must be declared")
        for index, entry in enumerate(declarations):
            self.assertEqual(
                matched_by[index],
                entry["count"],
                f"{entry['test_case']!r}: declares {entry['count']} but is attributed "
                f"{matched_by[index]} stranded case(s)",
            )

    def test_a_new_case_in_a_declared_family_is_not_covered_by_the_wildcard_alone(self):
        """The pattern still matches it - only the count makes it visible.

        #637: this used to hardcode the `[TileRenderer]*` declaration, so it
        broke with StopIteration the moment that family was un-quarantined -
        the same stale-fixture shape #663 removed from the sibling lane tests.
        The property under test is about trailing-wildcard families in general,
        so derive the subject from whatever the manifest currently declares.
        """
        declarations, _ = GUARD._load_unlaned_declarations()
        family = next(
            (e for e in declarations if e["test_case"].endswith("*")),
            None,
        )
        self.assertIsNotNone(
            family,
            "expected at least one trailing-wildcard family declaration to exercise",
        )
        prefix = family["test_case"][:-1]
        newcomer = f"{prefix} brand new stranded case added long after the declaration"
        self.assertTrue(
            GUARD._doctest_wildcmp(newcomer, family["test_case"]),
            "the wildcard does match it - which is exactly why a count is required",
        )


class RequiresRdIsNotCoverage(unittest.TestCase):
    """The opt-in [requires-RD] catalogue must not count as CI coverage.

    Codex #658: `_build_module_test_runs()` only appends that lane under
    `--gpu` / `GS_RUN_GPU_TESTS=1`, and the blocking workflow invokes the runner
    without it. Crediting it made cases look covered when no CI lane could fail
    on them - the same under-report class as the family-wildcard hole.
    """

    def setUp(self):
        self.runner = GUARD._load_module("_rmt_rd", GUARD.CI_DIR / "run_module_tests.py")
        self.harness = GUARD._load_module("_rgh_rd", GUARD.CI_DIR / "run_gpu_harness.py")
        linkage = GUARD._load_module("_ctl_rd", GUARD.CI_DIR / "check_test_linkage.py")
        self.cases, _ = GUARD._collect_corpus(linkage._strip_comments)

    def _hits(self, name):
        module = any(
            GUARD._lane_matches(name, i, e) for _, i, e, _ in self.runner.MODULE_TEST_FILTERS
        )
        rd = any(
            GUARD._lane_matches(name, i, e) for _, i, e, _ in self.runner.REQUIRES_RD_TEST_FILTERS
        )
        gpu = any(
            GUARD._doctest_wildcmp(name, p) for b in self.harness.BATCHES for p in b.filters
        )
        return module, rd, gpu

    def test_the_requires_rd_lane_is_opt_in_in_the_runner(self):
        """If this ever becomes unconditional, the exclusion here must be revisited."""
        source = (GUARD.CI_DIR / "run_module_tests.py").read_text(encoding="utf-8")
        index = source.index("REQUIRES_RD_TEST_FILTERS:")
        appended = source.index("for name, test_filters, exclude_filters, strict in REQUIRES_RD_TEST_FILTERS", index)
        preceding = source[:appended].rstrip().splitlines()[-1]
        self.assertIn(
            "if run_gpu:",
            preceding,
            "the requires-RD lane is no longer gated on run_gpu; re-evaluate whether it "
            "should count as coverage",
        )

    def test_codex_cited_case_is_treated_as_stranded(self):
        name = "[GaussianSplatting][RequiresGPU] Debug projection output matches golden gradient"
        self.assertIn(name, [n for n, _ in self.cases], "the cited case must be in the corpus")
        module, rd, gpu = self._hits(name)
        self.assertFalse(module)
        self.assertFalse(gpu)
        self.assertTrue(rd, "it does match requires-RD - which is exactly why crediting it hid it")

    def test_requires_rd_only_cases_are_all_declared(self):
        declarations, problems = GUARD._load_unlaned_declarations()
        self.assertEqual(problems, [])
        rd_only = [
            name
            for name, _ in self.cases
            if not self._hits(name)[0] and not self._hits(name)[2] and self._hits(name)[1]
        ]
        self.assertTrue(rd_only, "expected requires-RD-only cases to exist")
        for name in rd_only:
            self.assertTrue(
                any(GUARD._doctest_wildcmp(name, str(e["test_case"])) for e in declarations),
                f"requires-RD-only case is undeclared: {name}",
            )

    def test_gpu_world_cases_are_not_credited_to_the_requires_rd_catalogue(self):
        """[RequiresGPU] [World] cases must be attributed, not treated as covered.

        DERIVED, never hardcoded. An earlier version asserted count == 7 ("3
        non-GPU + 4 [RequiresGPU]"), which broke the moment #660 moved those 4
        into real GPU batches - a genuine coverage IMPROVEMENT turned the suite
        red. Pinning a number here re-creates the drift this guard exists to
        catch, so assert the property instead: every stranded [World] case is
        attributed to the [World] declaration, and the declared count equals
        what is actually attributed.
        """
        analysis = GUARD.analyze()
        declarations, _ = GUARD._load_unlaned_declarations()
        index = next(
            i
            for i, e in enumerate(declarations)
            if e["test_case"] == "[GaussianSplatting][World]*"
        )
        matched_by, undeclared = GUARD.attribute(analysis.stranded, declarations)

        pattern = str(declarations[index]["test_case"])
        world_stranded = [
            name
            for name, _ in analysis.stranded
            if GUARD._doctest_wildcmp(name, pattern)
        ]
        self.assertEqual(
            matched_by[index],
            len(world_stranded),
            "every stranded case matching the [World] pattern must land on that "
            "declaration, not on a later catch-all",
        )
        # NOTE: cases whose tag ORDER puts [World] later (e.g.
        # [GaussianSplatting][SceneDirector][World]...) do not match this
        # pattern and are legitimately absorbed by the catch-all. They are
        # still declared - `undeclared` below is the assertion that matters.
        self.assertEqual(
            declarations[index]["count"],
            matched_by[index],
            "declared [World] count must equal the attributed stranded count",
        )
        self.assertFalse(
            [name for name, _ in undeclared if "][World]" in name],
            "no [World] case may be stranded-but-undeclared",
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
