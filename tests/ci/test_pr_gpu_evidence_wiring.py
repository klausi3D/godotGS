#!/usr/bin/env python3
"""Hosted-only YAML wiring tests; uses the pinned automation dependency."""
from pathlib import Path
import unittest
import yaml
from test_pr_gpu_evidence import checker
ROOT = Path(__file__).resolve().parents[2]

class WorkflowWiringTests(unittest.TestCase):
    def setUp(self):
        self.text = (ROOT / ".github/workflows/gaussian_production_gates.yml").read_text(encoding="utf-8")
        self.workflow = yaml.safe_load(self.text)

    def test_all_prs_and_merge_queue_can_report(self):
        events = self.workflow.get("on", self.workflow.get(True))
        for event in ("pull_request", "merge_group"):
            self.assertIn(event, events)
            if event == "pull_request":
                self.assertIn("edited", events[event]["types"])
            for exclusion in ("branches", "branches-ignore", "paths", "paths-ignore"):
                self.assertNotIn(exclusion, events[event])
        gate = self.workflow["jobs"]["canonical-gpu-receipt"]
        self.assertEqual(gate["runs-on"], "ubuntu-latest")
        self.assertIn("always()", gate["if"])
        self.assertEqual(set(gate["needs"]), {"pr-evidence-policy", "guards", "module-validation"})

    def test_producer_follows_postflight_and_binds_step_outcomes(self):
        steps = self.workflow["jobs"]["module-validation"]["steps"]
        by_id = {step["id"]: step for step in steps if "id" in step}
        self.assertTrue(set(checker.REQUIRED_STEPS) <= by_id.keys())
        producer = next(step for step in steps if "--write-receipt" in step.get("run", ""))
        self.assertGreater(steps.index(producer), steps.index(by_id["postflight"]))
        self.assertIn("success()", producer["if"])
        for name in checker.REQUIRED_STEPS:
            self.assertEqual(producer["env"]["GS_STEP_" + name.upper()], "${{ steps." + name + ".outcome }}")
        upload = next(step for step in steps if step.get("uses") == "actions/upload-artifact@v4"
                      and step["with"]["name"].startswith("pr-gpu-evidence-"))
        self.assertEqual(upload["with"]["if-no-files-found"], "error")

    def test_consumer_uses_current_run_artifact_and_dependency_outcomes(self):
        steps = self.workflow["jobs"]["canonical-gpu-receipt"]["steps"]
        download = next(step for step in steps if step.get("uses") == "actions/download-artifact@v4")
        self.assertEqual(download["with"]["name"], "pr-gpu-evidence-${{ github.run_attempt }}")
        self.assertNotIn("run-id", download["with"])
        consumer = next(step for step in steps if "--verify" in step.get("run", ""))
        self.assertEqual(consumer["env"]["GS_JOB_MODULE"], "${{ needs.module-validation.result }}")
        self.assertEqual(consumer["env"]["GS_JOB_GUARDS"], "${{ needs.guards.result }}")
        self.assertNotIn("if", consumer)

    def test_test_is_wired_into_required_agentic_gate(self):
        workflow = (ROOT / ".github/workflows/agentic_pr_gate.yml").read_text(encoding="utf-8")
        self.assertIn("python tests/ci/test_pr_gpu_evidence.py", workflow)
        self.assertIn("python tests/ci/test_pr_gpu_evidence_wiring.py", workflow)

    def test_exempt_prs_do_not_queue_self_hosted_work(self):
        guards = self.workflow["jobs"]["guards"]
        self.assertEqual(guards["needs"], "pr-evidence-policy")
        condition = guards["if"]
        for risk in ("R2", "R3"):
            self.assertIn("needs.pr-evidence-policy.outputs.risk_class == '" + risk + "'", condition)
        self.assertIn("needs.pr-evidence-policy.result == 'success'", condition)

    def test_required_verdict_checks_out_only_the_trusted_base(self):
        workflow = yaml.safe_load((ROOT / ".github/workflows/pr_gpu_evidence_verdict.yml").read_text(encoding="utf-8"))
        events = workflow.get("on", workflow.get(True))
        self.assertIn("pull_request_target", events)
        self.assertIn("merge_group", events)
        self.assertIn("edited", events["pull_request_target"]["types"])
        self.assertEqual(events["workflow_run"]["types"], ["in_progress", "completed"])
        self.assertEqual(events["workflow_run"]["workflows"], ["Gaussian Production Gates"])
        self.assertEqual(workflow["permissions"], {"contents": "read", "actions": "read", "statuses": "write", "pull-requests": "read"})
        job = workflow["jobs"]["trusted-gpu-controller"]
        self.assertEqual(job["runs-on"], "ubuntu-latest")
        checkout = next(step for step in job["steps"] if step.get("uses") == "actions/checkout@v4")
        self.assertEqual(checkout["with"]["ref"], "${{ steps.event.outputs.base }}")
        self.assertFalse(checkout["with"]["persist-credentials"])
        resolver = next(step for step in job["steps"] if step.get("id") == "event")
        self.assertIn("await status('pending')", resolver["with"]["script"])
        self.assertIn("await status('failure')", resolver["with"]["script"])
        self.assertIn("source_run_id: run.id", resolver["with"]["script"])
        commands = [step["run"] for step in job["steps"] if "run" in step]
        self.assertEqual(commands, ["python scripts/agentic/watch_pr_gpu_evidence.py"])



if __name__ == "__main__":
    unittest.main()
