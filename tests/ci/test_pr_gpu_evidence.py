#!/usr/bin/env python3
"""Negative controls for the PR evidence consumer and its real workflow wiring."""
import copy
import importlib.util
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pr_evidence", ROOT / "tests/ci/check_pr_gpu_evidence.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class EvidenceVerdictTests(unittest.TestCase):
    def setUp(self):
        self.expected = dict(checkout_sha="a" * 40, head_sha="b" * 40,
                             base_sha="c" * 40, run_id="123", run_attempt="2")
        self.receipt = dict(schema_version=1, **self.expected,
                            steps={name: "success" for name in checker.REQUIRED_STEPS},
                            sha256={name: "d" * 64 for name in checker.REQUIRED_HASHES})
        self.jobs = {"pr-evidence-policy": "success", "guards": "success", "module-validation": "success"}

    def test_exact_run_is_accepted(self):
        for risk in ("R2", "R3"):
            self.assertIn(self.expected["head_sha"], checker.verdict(risk, self.jobs, self.receipt, self.expected))

    def test_low_risk_is_explicitly_not_a_gpu_pass(self):
        for risk in ("R0", "R1"):
            self.assertIn("not a GPU pass", checker.verdict(risk, {"pr-evidence-policy": "success"}, None, {}))

    def test_missing_or_unknown_risk_fails(self):
        for risk in ("", "R4", None):
            with self.subTest(risk=risk), self.assertRaises(ValueError):
                checker.verdict(risk, self.jobs, self.receipt, self.expected)

    def test_missing_receipt_fails(self):
        with self.assertRaises(ValueError):
            checker.verdict("R2", self.jobs, None, self.expected)

    def test_every_non_success_job_fails(self):
        for job in self.jobs:
            for state in ("skipped", "failure", "cancelled", "neutral", "", None):
                jobs = {**self.jobs, job: state}
                with self.subTest(job=job, state=state), self.assertRaises(ValueError):
                    checker.verdict("R3", jobs, self.receipt, self.expected)

    def test_every_required_step_must_execute(self):
        for step in checker.REQUIRED_STEPS:
            for state in ("skipped", "failure", "cancelled", "", None):
                receipt = copy.deepcopy(self.receipt)
                receipt["steps"][step] = state
                with self.subTest(step=step, state=state), self.assertRaises(ValueError):
                    checker.verdict("R2", self.jobs, receipt, self.expected)

    def test_old_run_wrong_attempt_or_wrong_commit_fails(self):
        for binding in checker.BINDINGS:
            for changed in ("wrong", "", None):
                receipt = {**self.receipt, binding: changed}
                with self.subTest(binding=binding, changed=changed), self.assertRaises(ValueError):
                    checker.verdict("R2", self.jobs, receipt, self.expected)

    def test_missing_hashes_steps_or_schema_fail(self):
        for key in ("sha256", "steps", "schema_version"):
            receipt = dict(self.receipt)
            del receipt[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                checker.verdict("R2", self.jobs, receipt, self.expected)
        for name in checker.REQUIRED_HASHES:
            receipt = copy.deepcopy(self.receipt)
            receipt["sha256"][name] = "invalid"
            with self.subTest(hash=name), self.assertRaises(ValueError):
                checker.verdict("R2", self.jobs, receipt, self.expected)


class WorkflowWiringTests(unittest.TestCase):
    def setUp(self):
        self.text = (ROOT / ".github/workflows/gaussian_production_gates.yml").read_text(encoding="utf-8")
        self.workflow = yaml.safe_load(self.text)

    def test_all_prs_and_merge_queue_can_report(self):
        events = self.workflow.get("on", self.workflow.get(True))
        for event in ("pull_request", "merge_group"):
            self.assertIn(event, events)
            for exclusion in ("branches", "branches-ignore", "paths", "paths-ignore"):
                self.assertNotIn(exclusion, events[event])
        gate = self.workflow["jobs"]["gpu-evidence-gate"]
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
        steps = self.workflow["jobs"]["gpu-evidence-gate"]["steps"]
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


if __name__ == "__main__":
    unittest.main()
