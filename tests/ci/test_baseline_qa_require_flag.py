#!/usr/bin/env python3
"""Regression test for the QA-baseline enforcement switch (#596 follow-up).

BaselineQARunner._record_qa_baseline_skipped() - the path main() takes when
the "QA Scene Suite" test itself was skipped (e.g. no RenderingDevice in a
headless environment) - returned True unconditionally and never even accepted
a require_baseline argument. So --require-qa-baseline /
GS_CI_REQUIRE_QA_BASELINE had zero effect on that path: turning the switch on
could never fail a run, even with the baseline file completely absent.

CORRECTION (this claim was previously overstated here and in
run_baseline_qa.py, in a PR whose entire subject is CI honesty). An earlier
revision of this docstring said that skip path is "exactly the code path
production CI takes". It is the path NO current CI run takes. main() only
reaches it when the ``qa`` category is selected, and no workflow selects
``qa``: baseline_qa.yml runs ``--categories sorting,renderer`` and ``--categories
ply,pipeline,runtime,module``; gaussian_production_gates.yml runs
``--category pipeline``. WorkflowCategorySelectionTest below pins that
inventory to ground truth so the corrected statement cannot silently rot.

The underlying defect was real regardless of which lane hits it: the switch
was inert wherever that path *is* reached (local/manual ``--category qa``
runs, the command documented in docs/testing/setup-guide.md, and any future
QA lane).

This test locks in the fix's contract:
  - "legitimately not applicable" (QA Scene Suite could not run at all) is
    always allowed to skip, regardless of the switch - no environment
    variable makes a RenderingDevice appear.
  - "baseline missing" is what the switch actually polices, and it must now
    fail closed on this path too, not just inside compare_qa_baseline().
  - The switch's pre-existing off-by-default behavior is unchanged.
  - Requesting the switch on an invocation that never runs ``qa`` is a hard
    failure, not a silent no-op (RequireBaselineAppliesTest).
"""

from __future__ import annotations

import importlib.util
import json
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "ci" / "run_baseline_qa.py"
spec = importlib.util.spec_from_file_location("run_baseline_qa", SCRIPT)
assert spec and spec.loader
run_baseline_qa = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_baseline_qa)


class RecordQaBaselineSkippedTest(unittest.TestCase):
    """Exercises the exact call main() makes on the QA-scene-skip branch."""

    def _invoke(self, *, require_baseline: bool, baseline_present: bool):
        with tempfile.TemporaryDirectory() as raw_td:
            td = Path(raw_td)
            qa_results_path = td / "qa_results.json"  # never produced: suite didn't run
            baseline_path = td / "baselines" / "qa_results.json"
            if baseline_present:
                baseline_path.parent.mkdir(parents=True, exist_ok=True)
                baseline_path.write_text(json.dumps({"results": []}), encoding="utf-8")

            runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
            qa_ok = runner._record_qa_baseline_skipped(
                qa_results_path=qa_results_path,
                baseline_path=baseline_path,
                report_path=None,
                summary_path=None,
                reason="QA Scene Suite requires local RenderingDevice when run with current headless configuration.",
                require_baseline=require_baseline,
            )
            comparison = runner.test_results["summary"]["qa_baseline"]
            return qa_ok, comparison

    def test_switch_on_missing_baseline_fails_closed(self):
        """The defect: switch ON + baseline absent must FAIL, not silently pass."""
        qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=False)
        self.assertFalse(qa_ok, "require_baseline=True with no baseline must fail the run.")
        self.assertEqual(comparison["status"], "failed")
        self.assertTrue(comparison["coverage_gap"])
        self.assertTrue(comparison["require_baseline"])
        self.assertFalse(comparison["baseline_exists"])

    def test_switch_on_present_baseline_still_allows_legitimate_skip(self):
        """A GPU-unavailable skip is orthogonal to baseline enforcement: if a
        baseline is committed, the switch must not punish an environment that
        simply couldn't produce current results to compare."""
        qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=True)
        self.assertTrue(qa_ok, "A present baseline must not be penalized by an unrelated GPU skip.")
        self.assertEqual(comparison["status"], "skipped")
        self.assertTrue(comparison["coverage_gap"])
        self.assertTrue(comparison["require_baseline"])
        self.assertTrue(comparison["baseline_exists"])

    def test_switch_off_missing_baseline_keeps_prior_default_behavior(self):
        """Default (switch untouched) behavior must be unchanged: warn + skip,
        never a hard failure - the whole reason this switch exists opt-in."""
        qa_ok, comparison = self._invoke(require_baseline=False, baseline_present=False)
        self.assertTrue(qa_ok)
        self.assertEqual(comparison["status"], "skipped")
        self.assertTrue(comparison["coverage_gap"])
        self.assertFalse(comparison["require_baseline"])

    def test_failed_status_renders_as_fail_not_coverage_gap_in_markdown(self):
        """A hard failure must show as [FAIL] in the human-readable summary,
        not be swallowed under the softer "[NO BASELINE - COVERAGE GAP]"
        label just because coverage_gap is also true for that comparison.
        (This precedence bug becomes reachable only once _record_qa_baseline_
        skipped can itself set status="failed", which is exactly what this
        fix introduces.)"""
        _qa_ok, comparison = self._invoke(require_baseline=True, baseline_present=False)
        self.assertEqual(comparison["status"], "failed")
        self.assertTrue(comparison["coverage_gap"])

        runner = run_baseline_qa.BaselineQARunner(godot_binary="unused")
        markdown = runner._build_baseline_summary_markdown(comparison)
        self.assertIn("[FAIL]", markdown.splitlines()[2])
        self.assertNotIn("[NO BASELINE - COVERAGE GAP]", markdown)


class RequireBaselineAppliesTest(unittest.TestCase):
    """The second laundering path: enforcement requested where it cannot act.

    Every QA-baseline check in main() is gated on qa_ran. Before this fix,
    setting the switch on a non-``qa`` job meant the request evaporated with
    no warning and the run still exited 0.
    """

    def test_requested_on_non_qa_invocation_is_a_failure(self):
        self.assertFalse(
            run_baseline_qa.require_baseline_applies(True, qa_ran=False),
            "Requesting enforcement where it cannot apply must fail, not no-op.",
        )

    def test_requested_on_qa_invocation_is_fine(self):
        self.assertTrue(run_baseline_qa.require_baseline_applies(True, qa_ran=True))

    def test_not_requested_is_always_fine(self):
        """Off-by-default behavior is untouched on both kinds of invocation."""
        self.assertTrue(run_baseline_qa.require_baseline_applies(False, qa_ran=False))
        self.assertTrue(run_baseline_qa.require_baseline_applies(False, qa_ran=True))

    def test_inert_message_names_the_switch_and_the_remedy(self):
        message = run_baseline_qa.REQUIRE_QA_BASELINE_INERT_MESSAGE
        self.assertIn("--require-qa-baseline", message)
        self.assertIn(run_baseline_qa.REQUIRE_QA_BASELINE_ENV, message)
        self.assertIn("qa", message)


class ResolveQaRanTest(unittest.TestCase):
    """qa_ran is the single fact the whole enforcement surface hangs on."""

    def test_explicit_qa_category_runs_qa(self):
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category="qa", categories=None, quick=False))

    def test_other_single_category_does_not_run_qa(self):
        for category in ("ply", "pipeline", "sorting", "runtime", "module"):
            with self.subTest(category=category):
                self.assertFalse(
                    run_baseline_qa.resolve_qa_ran(category=category, categories=None, quick=False)
                )

    def test_categories_list_runs_qa_only_when_it_contains_qa(self):
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories={"qa", "ply"}, quick=False))
        self.assertFalse(
            run_baseline_qa.resolve_qa_ran(
                category=None, categories={"ply", "pipeline", "runtime", "module"}, quick=False
            )
        )

    def test_all_alias_normalizes_to_none_and_runs_qa(self):
        """'all' normalizes to None inside the set, which means every category."""
        self.assertIsNone(run_baseline_qa.normalize_test_category("all"))
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories={None}, quick=False))

    def test_quick_skips_qa_but_full_default_run_includes_it(self):
        self.assertFalse(run_baseline_qa.resolve_qa_ran(category=None, categories=None, quick=True))
        self.assertTrue(run_baseline_qa.resolve_qa_ran(category=None, categories=None, quick=False))


def _extract_baseline_qa_invocations():
    """Every run_baseline_qa.py invocation in .github/workflows, with the
    category selector each one passes.

    Fail closed: an invocation whose selector cannot be parsed is reported as
    an unparsed entry rather than being quietly dropped, because a dropped
    invocation is exactly how "no workflow selects qa" would rot into a false
    statement.
    """
    workflows_dir = ROOT / ".github" / "workflows"
    selector_re = re.compile(r"--categor(?:y|ies)\"?,?\s+\"?([A-Za-z][A-Za-z,]*)")
    invocations = []
    for workflow in sorted(workflows_dir.glob("*.yml")):
        lines = workflow.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if "run_baseline_qa.py" not in line:
                continue
            location = f"{workflow.name}:{index + 1}"
            match = selector_re.search(line)
            if match:
                invocations.append((location, frozenset(match.group(1).split(","))))
                continue
            if "@args" in line:
                # PowerShell splatting: the selector lives in the preceding
                # `$args = @( ... )` block.
                block = []
                for previous in reversed(lines[:index]):
                    block.append(previous)
                    if "$args" in previous and "@(" in previous:
                        break
                else:
                    invocations.append((location, None))
                    continue
                block_match = selector_re.search("\n".join(reversed(block)))
                invocations.append(
                    (location, frozenset(block_match.group(1).split(",")) if block_match else None)
                )
                continue
            invocations.append((location, None))
    return invocations


class WorkflowCategorySelectionTest(unittest.TestCase):
    """Pins the corrected premise to ground truth.

    The PR body and the docstrings above state that no CI workflow selects
    the ``qa`` category, and therefore that the QA-skip path is taken by no
    CI run today. That is a claim about repository state, so it is checked
    here rather than merely asserted in prose. If someone adds a QA lane,
    this test fails and the prose must be corrected with it.
    """

    # Keyed by workflow file + selector, not by line number: a line-number pin
    # would break on any unrelated edit above the invocation and train people
    # to update it mechanically, which is how a pin stops meaning anything.
    EXPECTED = sorted(
        [
            # #104: the GPU lane gained the render-thread dispatch characterization
            # (category "renderer"), a deliberate lane addition pinned here.
            ("baseline_qa.yml", frozenset({"sorting", "renderer"})),
            ("baseline_qa.yml", frozenset({"ply", "pipeline", "runtime", "module"})),
            ("gaussian_production_gates.yml", frozenset({"pipeline"})),
        ],
        key=lambda entry: (entry[0], sorted(entry[1])),
    )

    def test_every_invocation_has_a_parsed_selector(self):
        unparsed = [location for location, selector in _extract_baseline_qa_invocations() if selector is None]
        self.assertEqual(
            unparsed,
            [],
            "A run_baseline_qa.py invocation has no parseable category selector. "
            "With no selector the runner defaults to the full suite, which DOES "
            "include 'qa' - so this must be classified, not ignored.",
        )

    def test_invocation_inventory_is_pinned(self):
        """Count/shape pin: adding or changing a lane must be deliberate."""
        actual = sorted(
            [
                (location.split(":")[0], selector)
                for location, selector in _extract_baseline_qa_invocations()
            ],
            key=lambda entry: (entry[0], sorted(entry[1]) if entry[1] else []),
        )
        self.assertEqual(actual, self.EXPECTED)

    def test_no_workflow_selects_the_qa_category(self):
        offenders = [
            location
            for location, selector in _extract_baseline_qa_invocations()
            if selector is not None and "qa" in selector
        ]
        self.assertEqual(
            offenders,
            [],
            "A workflow now selects the 'qa' category. The claim in this file "
            "and in run_baseline_qa._record_qa_baseline_skipped that no CI run "
            "reaches the QA-skip path is now false - correct the prose.",
        )


if __name__ == "__main__":
    unittest.main()
