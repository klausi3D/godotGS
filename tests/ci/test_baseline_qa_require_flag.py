#!/usr/bin/env python3
"""Regression test for the QA-baseline enforcement switch (#596 follow-up).

An external reviewer found that BaselineQARunner._record_qa_baseline_skipped()
- the path main() takes whenever the "QA Scene Suite" test itself was skipped
(e.g. no RenderingDevice in a headless environment, which is what
tests/ci/run_baseline_qa.py's hardcoded ``--headless`` invocation of that
suite produces on every runner today) - returned True unconditionally and
never even accepted a require_baseline argument. That meant
--require-qa-baseline / GS_CI_REQUIRE_QA_BASELINE had zero effect on exactly
the code path production CI takes: turning the switch on could never fail a
run, even with the baseline file completely absent.

This test locks in the fix's contract:
  - "legitimately not applicable" (QA Scene Suite could not run at all) is
    always allowed to skip, regardless of the switch - no environment
    variable makes a RenderingDevice appear.
  - "baseline missing" is what the switch actually polices, and it must now
    fail closed on this path too, not just inside compare_qa_baseline().
  - The switch's pre-existing off-by-default behavior is unchanged.
"""

from __future__ import annotations

import importlib.util
import json
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


if __name__ == "__main__":
    unittest.main()
