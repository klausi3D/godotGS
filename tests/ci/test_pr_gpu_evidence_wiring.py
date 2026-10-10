#!/usr/bin/env python3
"""Hosted-only YAML wiring tests; uses the pinned automation dependency."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import yaml
from test_pr_gpu_evidence import checker, watcher
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

    def test_every_producer_preflight_and_receipt_script_is_pinned(self):
        # Derived from the real job, so a new preflight script cannot slip past
        # the trusted consumer's self-certification check.
        steps = self.workflow["jobs"]["module-validation"]["steps"]
        producer = [step for step in steps if step.get("name", "").startswith(("Preflight", "Postflight"))
                    or "--write-receipt" in step.get("run", "")]
        scripts = {script for step in producer for script in re.findall(r"python\s+(\S+\.py)", step.get("run", ""))}
        self.assertTrue({"tests/ci/preflight_runner_gpu_environment.py", "tests/ci/runner_gpu_contention.py",
                         "tests/ci/check_pr_gpu_evidence.py"} <= scripts)
        self.assertLessEqual(scripts, set(watcher.PRODUCER_DEFINITIONS))

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

    def resolve_merge_queue_event(self, head_branch, merge_base):
        # Executes the real resolver script under node with a stubbed API whose
        # branch tip has moved past the group's base.
        workflow = yaml.safe_load((ROOT / ".github/workflows/pr_gpu_evidence_verdict.yml").read_text(encoding="utf-8"))
        resolver = next(step for step in workflow["jobs"]["trusted-gpu-controller"]["steps"] if step.get("id") == "event")
        node = shutil.which("node")
        self.assertIsNotNone(node, "node is required to execute the resolver script")
        run = dict(event="merge_group", head_sha="b" * 40, head_branch=head_branch, id=7)
        harness = """
const outputs = {}, statuses = [];
const context = {payload: {workflow_run: RUN}, eventName: 'workflow_run', repo: {owner: 'o', repo: 'r'}, runId: 1};
const core = {setOutput: (key, value) => { outputs[key] = value; }};
const github = {rest: {
  repos: {createCommitStatus: async args => { statuses.push(args.state); },
          listPullRequestsAssociatedWithCommit: async () => ({data: []}),
          getBranch: async () => ({data: {commit: {sha: 'f'.repeat(40)}}}),
          compareCommits: async () => ({data: {merge_base_commit: {sha: MERGE_BASE}}})},
  pulls: {get: async () => { throw new Error('unexpected'); }}}};
(async () => {
SCRIPT
})().then(() => console.log(JSON.stringify({outputs, statuses})),
          error => console.log(JSON.stringify({outputs, statuses, error: String(error)})));
""".replace("RUN", json.dumps(run)).replace("MERGE_BASE", json.dumps(merge_base)).replace("SCRIPT", resolver["with"]["script"])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "resolver.js"
            path.write_text(harness, encoding="utf-8")
            result = subprocess.run([node, str(path)], capture_output=True, encoding="utf-8", timeout=60,
                                    env={**os.environ, "RUNNER_TEMP": temp}, check=True)
            state = json.loads(result.stdout.strip().splitlines()[-1])
            if "path" in state["outputs"]:
                state["event"] = json.loads(Path(state["outputs"]["path"]).read_text(encoding="utf-8"))
            return state

    def test_merge_queue_lifecycle_keeps_the_immutable_group_base(self):
        base = "c" * 40
        for branch in ("master", "release/v1.0"):
            with self.subTest(branch=branch):
                state = self.resolve_merge_queue_event(f"gh-readonly-queue/{branch}/pr-12-{base}", base)
                self.assertNotIn("error", state)
                self.assertEqual(state["outputs"]["base"], base)
                self.assertEqual(state["event"]["merge_group"], {"base_sha": base, "head_sha": "b" * 40})
        # A ref without the base suffix, or a suffix that is not the group's
        # ancestor, fails closed instead of reading the moving branch tip.
        for branch, merge_base in (("gh-readonly-queue/master/pr-12", base),
                                   (f"gh-readonly-queue/master/pr-12-{base}", "d" * 40)):
            with self.subTest(branch=branch, merge_base=merge_base):
                state = self.resolve_merge_queue_event(branch, merge_base)
                self.assertIn("error", state)
                self.assertEqual(state["statuses"], ["pending", "failure"])
                self.assertNotIn("base", state["outputs"])



if __name__ == "__main__":
    unittest.main()
