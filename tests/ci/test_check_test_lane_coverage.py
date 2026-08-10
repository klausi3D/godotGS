#!/usr/bin/env python3
"""Unit test for tests/ci/check_test_lane_coverage.py (#520).

Pins the three properties the guard's value depends on:

* the matcher reproduces **doctest's** wildcard semantics, not `fnmatch`'s,
* a wildcard declaration in `unlaned_tests` cannot silently amnesty cases written
  after it, and
* a corpus promoted into a strict lane cannot lose strict coverage without the
  guard going red (`StrictCoverageContracts` below, #846 / PR #850 round 3).

The second was a demonstrated hole: family patterns like `[TileRenderer]*` are
open-ended, so a brand-new stranded case joining an already-declared family used
to pass. The per-entry `count` closes it, and these cases keep it closed.

The third was the same shape one level up: the earlier proof for #846 mutated the
lane tuple *only*, which goes red merely because the tag stays in
`HEADLESS_GAUSSIAN_SCOPED_TAGS` and the cases become stranded. That proves the
mechanism, not the property. `StrictCoverageContracts` drives the two mutations
that actually undo a promotion - deleting both halves, and retagging a subset -
and requires each to be red.
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


class StrictCoverageContracts(unittest.TestCase):
    """A promoted corpus must not be able to drift back into an advisory lane.

    Every case here drives `evaluate_strict_coverage_contract()` with the REAL
    corpus and the REAL lane table, mutating one of them in memory. Mutating the
    inputs rather than the tree is what makes these mutations reproducible in CI
    instead of a transcript nobody can re-run.

    Every mutation runs against **every** shipped contract, not against a chosen
    one (#852). A mutation proof that names one contract is the same artifact as
    a hand-written coverage list: it stops being true the moment a second
    contract is added, and nothing says so. The tag and the covering lane are
    derived per contract - from `contract.test_case` and from the evaluator's own
    `lanes` result - so a contract added for `[AtomicWrite]`, `[SPZ]` or
    `[MalformedCorpus]` (#853) is mutation-proven the day it lands, with no edit
    here.
    """

    def setUp(self):
        self.runner = GUARD._load_module("_rmt_strict", GUARD.CI_DIR / "run_module_tests.py")
        linkage = GUARD._load_module("_ctl_strict", GUARD.CI_DIR / "check_test_linkage.py")
        self.cases, _ = GUARD._collect_corpus(linkage._strip_comments)
        self.lanes = list(self.runner.MODULE_TEST_FILTERS)
        self.assertTrue(GUARD.STRICT_COVERAGE_CONTRACTS, "no contracts to exercise")
        self.contracts = list(GUARD.STRICT_COVERAGE_CONTRACTS)
        self.contract = next(
            c for c in GUARD.STRICT_COVERAGE_CONTRACTS if c.name == "[DataAuthority]"
        )

    # -- helpers ---------------------------------------------------------

    def _evaluate(self, cases=None, lanes=None, contract=None):
        return GUARD.evaluate_strict_coverage_contract(
            contract or self.contract,
            self.cases if cases is None else cases,
            self.lanes if lanes is None else lanes,
        )

    def _tag_fragment(self, contract):
        """`*][SortFallback]*` -> `][SortFallback]`.

        Derived from the contract rather than written down, so these mutations
        cover a contract nobody edited this file for. A contract whose pattern is
        not a bare `*][Tag]*` fails loudly here instead of being skipped.
        """
        fragment = contract.test_case.strip("*")
        self.assertTrue(
            fragment.startswith("][") and fragment.endswith("]") and len(fragment) > 3,
            f"{contract.name}: test_case {contract.test_case!r} is not a bare '*][Tag]*' "
            "pattern, so the mutations below cannot derive its tag. Give the contract a "
            "bare pattern or teach this helper - do not let it silently opt out.",
        )
        return fragment

    def _covering_strict_lanes(self, contract):
        """The strict lanes that actually execute this corpus today, derived."""
        lanes = set(self._evaluate(contract=contract).lanes)
        self.assertTrue(lanes, f"{contract.name}: no strict lane covers it, nothing to unwind")
        return lanes

    def _promoted_cases(self, contract):
        fragment = self._tag_fragment(contract)
        return [name for name, _ in self.cases if fragment in name]

    def _retag_count(self, contract):
        """Retag a strict SUBSET, so the strict lane survives and stays non-empty."""
        count = min(4, len(self._promoted_cases(contract)) - 1)
        self.assertGreaterEqual(count, 1, f"{contract.name}: too few cases to retag a subset")
        return count

    def _lanes_without_the_promotion(self, contract=None):
        """Route (a): delete the strict lane AND the HEADLESS_GAUSSIAN_SCOPED_TAGS entry.

        Deleting the tag entry is what removes `*][<Tag>]*` from the `[untagged]`
        safety net's exclude list, so the cases land back in an advisory lane
        instead of being stranded. Both halves are modelled here because it is the
        combination that is invisible to every other check.
        """
        contract = contract or self.contract
        fragment = self._tag_fragment(contract)
        covering = self._covering_strict_lanes(contract)
        mutated = []
        for name, includes, excludes, strict in self.lanes:
            if name in covering:
                continue
            excludes = tuple(e for e in excludes if fragment not in e)
            mutated.append((name, includes, excludes, strict))
        return mutated

    def _lanes_with_the_lane_demoted(self, contract=None):
        """The third shape: keep everything, flip `strict` to False."""
        contract = contract or self.contract
        covering = self._covering_strict_lanes(contract)
        return [
            (name, includes, excludes, False if name in covering else strict)
            for name, includes, excludes, strict in self.lanes
        ]

    def _corpus_with_a_retagged_subset(self, contract=None, count=4, replacement="]"):
        """Route (b): retag `count` of the cases out of the promoted tag."""
        contract = contract or self.contract
        fragment = self._tag_fragment(contract)
        mutated = []
        retagged = 0
        for name, file_name in self.cases:
            if fragment in name and retagged < count:
                retagged += 1
                name = name.replace(fragment, replacement)
            mutated.append((name, file_name))
        self.assertEqual(retagged, count, "the corpus no longer has enough cases to retag")
        return mutated

    # -- control: the mechanism is live and non-empty ---------------------

    def test_the_committed_tree_satisfies_every_contract(self):
        for contract in GUARD.STRICT_COVERAGE_CONTRACTS:
            with self.subTest(contract=contract.name):
                result = self._evaluate(contract=contract)
                self.assertEqual(result.failures, [], "\n".join(result.failures))
                self.assertEqual(result.uncovered, [])
                self.assertTrue(result.lanes, "no strict lane is credited with the coverage")

    def test_the_contract_actually_enumerates_the_promoted_corpus(self):
        """Non-vacuity control: a guard over an empty set proves nothing.

        Asserted as a floor, not an equality - pinning 11 would turn writing a
        twelfth [DataAuthority] test into a CI failure, which is the drift this
        file's sibling cases already had to unlearn twice.
        """
        result = self._evaluate()
        self.assertGreaterEqual(len(result.protected), 11)
        self.assertTrue(
            all("][DataAuthority]" in name for name, _ in result.protected),
            "the protected set drifted away from the promoted corpus",
        )

    def test_every_contract_enumerates_more_than_a_token_corpus(self):
        """The same non-vacuity control, for contracts nobody wrote a case for.

        The floor is derived (a subset must be retaggable while the strict lane
        stays non-empty), not pinned per contract, because a pinned per-contract
        number is the hand-written artifact this file keeps replacing.
        """
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                result = self._evaluate(contract=contract)
                self.assertGreaterEqual(len(result.protected), 2)
                self.assertTrue(
                    all(fragment in name for name, _ in result.protected),
                    "the protected set drifted away from the promoted corpus",
                )

    def test_every_shipped_contract_is_completely_declared(self):
        for contract in GUARD.STRICT_COVERAGE_CONTRACTS:
            with self.subTest(contract=contract.name):
                self.assertTrue(contract.name.strip())
                self.assertTrue(contract.sources)
                self.assertTrue(contract.test_case.strip())
                self.assertIn("github.com", contract.issue_url)
                self.assertTrue(contract.rationale.strip())

    # -- the two escape routes Codex named --------------------------------

    def test_deleting_both_halves_of_the_promotion_is_red(self):
        """Route (a). The cases are NOT stranded here - that is the whole point."""
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                lanes = self._lanes_without_the_promotion(contract)
                promoted = self._promoted_cases(contract)
                self.assertTrue(promoted)
                for name in promoted:
                    self.assertTrue(
                        any(GUARD._lane_matches(name, inc, exc) for _, inc, exc, _ in lanes),
                        "this mutation must leave the cases running in an advisory lane; if "
                        "they were stranded, the stranded check would catch it and this test "
                        "would be proving the wrong thing",
                    )
                result = self._evaluate(lanes=lanes, contract=contract)
                self.assertEqual(len(result.uncovered), len(promoted))
                self.assertTrue(result.failures)

    def test_retagging_a_subset_is_red(self):
        """Route (b). The strict lane survives, non-empty, and still passes."""
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                count = self._retag_count(contract)
                cases = self._corpus_with_a_retagged_subset(contract, count=count)
                still_tagged = [name for name, _ in cases if fragment in name]
                self.assertTrue(
                    still_tagged,
                    "the strict lane must remain non-empty, or the runner's own zero-coverage "
                    "check would already fail and this test would prove the wrong thing",
                )
                retagged = [
                    (name, file_name)
                    for name, file_name in cases
                    if fragment not in name and file_name in contract.sources
                ]
                self.assertEqual(len(retagged), count)
                for name, _ in retagged:
                    self.assertTrue(
                        any(
                            GUARD._lane_matches(name, inc, exc)
                            for _, inc, exc, _ in self.lanes
                        ),
                        "a retagged case must still reach the advisory [untagged] net - "
                        "otherwise it is stranded and a different check catches it",
                    )
                result = self._evaluate(cases=cases, contract=contract)
                self.assertEqual(len(result.uncovered), count)
                self.assertTrue(result.failures)

    def test_retagging_to_another_new_tag_is_red_too(self):
        """Same route, different shape: a new tag rather than a dropped one."""
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                count = self._retag_count(contract)
                cases = self._corpus_with_a_retagged_subset(
                    contract, count=count, replacement=f"{fragment[:-1]}Legacy]"
                )
                result = self._evaluate(cases=cases, contract=contract)
                self.assertEqual(len(result.uncovered), count)
                self.assertTrue(result.failures)

    def test_demoting_the_lane_to_advisory_is_red(self):
        """The shape neither Codex round named: leave the tuple, flip `strict`."""
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                result = self._evaluate(
                    lanes=self._lanes_with_the_lane_demoted(contract), contract=contract
                )
                self.assertTrue(result.protected)
                self.assertEqual(len(result.uncovered), len(result.protected))
                self.assertTrue(result.failures)

    # -- why both keys exist ---------------------------------------------

    def test_a_case_moved_to_another_file_is_still_protected_by_the_tag_key(self):
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                moved = [
                    (name, "test_somewhere_else.h" if fragment in name else file_name)
                    for name, file_name in self.cases
                ]
                result = self._evaluate(cases=moved, contract=contract)
                self.assertTrue(result.protected)
                self.assertTrue(
                    all(
                        file_name == "test_somewhere_else.h"
                        for _, file_name in result.protected
                    ),
                    "the file key contributes nothing here; the tag key must carry it",
                )

    def test_a_case_retagged_in_place_is_still_protected_by_the_file_key(self):
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                count = self._retag_count(contract)
                cases = self._corpus_with_a_retagged_subset(contract, count=count)
                result = self._evaluate(cases=cases, contract=contract)
                protected_names = {name for name, _ in result.protected}
                untagged_but_protected = [n for n in protected_names if fragment not in n]
                self.assertEqual(
                    len(untagged_but_protected),
                    count,
                    "the tag key cannot see a retagged case; the file key must carry it",
                )

    # -- vacuity: the contract must not pass by enumerating nothing -------

    def test_a_misspelled_tag_fails_even_when_the_file_key_still_matches(self):
        """The half-vacuous case, which is the dangerous one.

        With only a union check, a typo'd pattern would contribute nothing, the
        file key would still cover every case, and the contract would report a
        clean pass while half of it silently checked nothing.
        """
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                fragment = self._tag_fragment(contract)
                broken = GUARD.StrictCoverageContract(
                    name=f"{contract.name} (typo)",
                    sources=contract.sources,
                    test_case=f"*{fragment[:-1]}y]*",
                    issue_url=contract.issue_url,
                    rationale="typo control",
                )
                result = self._evaluate(contract=broken)
                self.assertEqual(result.uncovered, [], "the file key still covers everything")
                self.assertTrue(
                    result.problems, "a pattern matching nothing must fail, not pass"
                )
                self.assertTrue(result.failures)

    def test_a_renamed_source_file_fails(self):
        for contract in self.contracts:
            with self.subTest(contract=contract.name):
                broken = GUARD.StrictCoverageContract(
                    name=f"{contract.name} (renamed file)",
                    sources=tuple(f"{source}_OLD" for source in contract.sources),
                    test_case=contract.test_case,
                    issue_url=contract.issue_url,
                    rationale="rename control",
                )
                result = self._evaluate(contract=broken)
                self.assertTrue(result.problems)
                self.assertTrue(result.failures)

    def test_a_contract_matching_nothing_at_all_fails(self):
        broken = GUARD.StrictCoverageContract(
            name="(empty)",
            sources=("no_such_file.h",),
            test_case="*][NoSuchTag]*",
            issue_url=self.contract.issue_url,
            rationale="empty control",
        )
        result = self._evaluate(contract=broken)
        self.assertEqual(result.protected, [])
        self.assertEqual(result.uncovered, [])
        self.assertGreaterEqual(len(result.problems), 3)
        self.assertTrue(result.failures)

    # -- wiring: a red contract must actually fail the guard process ------

    def test_main_exits_nonzero_when_a_contract_fails(self):
        """A pure function returning failures nobody reads would be no guard at all.

        The green half of this pair is `GuardPasses.test_guard_passes_on_the_current_tree`
        (`main() == 0` on the committed tree), so it is not repeated here.
        """
        real_analyze = GUARD.analyze
        analysis = real_analyze()
        broken = GUARD.evaluate_strict_coverage_contract(
            self.contract, self.cases, self._lanes_without_the_promotion()
        )
        self.assertTrue(broken.failures)
        analysis.strict_contracts = [broken]
        try:
            GUARD.analyze = lambda: analysis
            self.assertEqual(GUARD.main(), 1)
        finally:
            GUARD.analyze = real_analyze

    def test_removing_every_contract_is_itself_a_failure(self):
        real_analyze = GUARD.analyze
        analysis = real_analyze()
        analysis.strict_contracts = []
        try:
            GUARD.analyze = lambda: analysis
            self.assertEqual(GUARD.main(), 1)
        finally:
            GUARD.analyze = real_analyze


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
