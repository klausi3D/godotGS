#!/usr/bin/env python3
"""Hosted real-YAML/JavaScript wiring controls for universal commit evidence."""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
CONTROLLER = ROOT / ".github/workflows/pr_gpu_evidence_verdict.yml"
PRODUCER = ROOT / ".github/workflows/gaussian_production_gates.yml"


def load(path):
    return yaml.safe_load(path.read_text(encoding="utf-8"))


class UniversalWiringTests(unittest.TestCase):
    @staticmethod
    def evaluate(expression, values):
        expression = expression.removeprefix("${{").removesuffix("}}").strip()
        expression = expression.replace("always()", "true")
        for name in sorted(values, key=len, reverse=True):
            expression = expression.replace(name, json.dumps(values[name]))
        expression = re.sub(r"(?:github|needs|inputs)\.[A-Za-z0-9_.-]+", "''", expression)
        node = shutil.which("node")
        if not node:
            raise AssertionError("Node is required for hosted expression wiring")
        result = subprocess.run([node, "-e", "process.stdout.write(JSON.stringify(Boolean(" + expression + ")));"],
                                capture_output=True, encoding="utf-8", timeout=20, check=True)
        return json.loads(result.stdout)

    def test_controller_is_hosted_pinned_trusted_default_without_github_token_write(self):
        workflow = load(CONTROLLER)
        events = workflow.get("on", workflow.get(True))
        self.assertEqual(events["workflow_run"]["workflows"], ["Gaussian Production Gates"])
        self.assertEqual(events["workflow_run"]["types"], ["in_progress", "completed"])
        self.assertEqual(workflow["permissions"], {"contents": "read", "actions": "read", "pull-requests": "read"})
        for job in workflow["jobs"].values():
            self.assertEqual(job["runs-on"], "ubuntu-latest")
            self.assertEqual(job["environment"], "trusted-gpu-verdict")
            for step in job["steps"]:
                if "uses" in step:
                    self.assertRegex(step["uses"], r"^[A-Za-z0-9_-]+/[A-Za-z0-9_-]+@[0-9a-f]{40}$")
                if step.get("uses", "").startswith("actions/checkout@"):
                    self.assertEqual(step["with"]["ref"], "${{ steps.event.outputs.base }}")
                    self.assertFalse(step["with"]["persist-credentials"])
                if "script" in step.get("with", {}):
                    self.assertNotIn("createCommitStatus", step["with"]["script"])
        run = next(step for step in workflow["jobs"]["trusted-gpu-controller"]["steps"] if "run" in step)
        self.assertEqual(run["run"], "python scripts/agentic/watch_pr_gpu_evidence.py")
        self.assertEqual(run["env"]["GS_PUBLISH_TOKEN"], "${{ steps.publisher.outputs.token }}")
        self.assertEqual(run["env"]["GS_PROTECTION_TOKEN"], "${{ secrets.GPU_CODE_EVIDENCE_PROTECTION_READ_TOKEN }}")
        self.assertNotIn("GS_CODE_EVIDENCE_BOOTSTRAP", run["env"])
        publisher = next(step for step in workflow["jobs"]["trusted-gpu-controller"]["steps"] if step.get("id") == "publisher")
        self.assertEqual(publisher["with"]["permission-statuses"], "write")
        self.assertEqual(publisher["with"]["private-key"], "${{ secrets.GPU_CODE_EVIDENCE_APP_PRIVATE_KEY }}")
        self.assertEqual(publisher["with"]["repositories"], "${{ github.event.repository.name }}")

    def test_timeout_cleanup_is_independent_and_has_no_success_command(self):
        workflow = load(CONTROLLER)
        controller, cleanup = (workflow["jobs"][name] for name in ("trusted-gpu-controller", "trusted-gpu-cleanup"))
        self.assertEqual(cleanup["needs"], "trusted-gpu-controller")
        self.assertIn("always()", cleanup["if"])
        self.assertEqual(cleanup["timeout-minutes"], 5)
        verify = next(step for step in controller["steps"] if "run" in step)
        self.assertLess(verify["timeout-minutes"], controller["timeout-minutes"])
        final = next(step for step in cleanup["steps"] if "run" in step)
        self.assertEqual(final["run"], "python scripts/agentic/watch_pr_gpu_evidence.py --cleanup")
        resolver = next(step for step in cleanup["steps"] if step.get("id") == "event")
        self.assertNotIn("needs.trusted-gpu-controller.outputs", resolver["with"]["script"])
        self.assertIn("workflow_run?.head_sha", resolver["with"]["script"])
        for result in ("success", "failure", "cancelled", "skipped"):
            self.assertTrue(self.evaluate(cleanup["if"], {"github.event_name": "pull_request_target", "needs.trusted-gpu-controller.result": result}))
        for event in ("pull_request", "merge_group", "push", "schedule", "workflow_dispatch", ""):
            self.assertEqual(self.evaluate(cleanup["if"], {"github.event_name": "workflow_run", "github.event.workflow_run.name": "Gaussian Production Gates", "github.event.workflow_run.event": event}), event == "pull_request")
        self.assertFalse(self.evaluate(cleanup["if"], {"github.event_name": "merge_group"}))
        self.assertFalse(self.evaluate(cleanup["if"], {"github.event_name": "push"}))

    def test_every_risk_and_metadata_variant_reaches_mandatory_same_repo_lane(self):
        guards = load(PRODUCER)["jobs"]["guards"]
        for risk in ("R0", "R1", "R2", "R3", ""):
            self.assertTrue(self.evaluate(guards["if"], {
                "github.event_name": "pull_request", "github.event.pull_request.head.repo.full_name": "o/r",
                "github.repository": "o/r", "needs.pr-evidence-policy.result": "success",
                "needs.pr-evidence-policy.outputs.risk_class": risk}))
        self.assertFalse(self.evaluate(guards["if"], {"github.event_name": "pull_request",
            "github.event.pull_request.head.repo.full_name": "fork/r", "github.repository": "o/r", "needs.pr-evidence-policy.result": "success"}))

    def test_evidence_checkout_and_commands_bind_exact_head_and_full_gpu_profile(self):
        workflow = load(PRODUCER)
        expected = "${{ github.event.pull_request.head.sha || github.event.merge_group.head_sha || github.sha }}"
        for job_id in ("pr-evidence-policy", "guards", "module-validation", "canonical-gpu-receipt"):
            job = workflow["jobs"][job_id]
            checkout = next(step for step in job["steps"] if step.get("uses", "").startswith("actions/checkout@"))
            self.assertEqual(checkout["with"]["ref"], expected)
        steps = workflow["jobs"]["module-validation"]["steps"]
        module = next(step for step in steps if step.get("id") == "module")
        self.assertIn('--gpu --lane-report artifacts/module-lanes.json', module["run"])
        self.assertIn('"pull_request", "merge_group"', module["run"])
        gpu = next(step for step in steps if step.get("id") == "gpu_harness")
        self.assertEqual(gpu["run"], 'python tests/ci/run_gpu_harness.py --godot "$env:GODOT_BINARY" --report artifacts/gpu-harness-report.json --baseline-mode compare')
        self.assertNotIn("--batch", gpu["run"])
        for event in ("pull_request", "merge_group"):
            self.assertTrue(self.evaluate(gpu["if"], {"github.event_name": event}))
        self.assertNotIn("needs.", gpu["if"])
        capture = next(step for step in steps if step.get("id") == "capture")
        self.assertIn("--start-measurement", capture["run"])
        for step in (capture, next(s for s in steps if s["name"] == "Bind canonical GPU evidence to reviewed commit")):
            self.assertEqual(step["env"]["GS_EVIDENCE_HEAD_SHA"], "${{ github.event.pull_request.head.sha || github.event.merge_group.head_sha }}")
        order = {step["name"]: index for index, step in enumerate(steps)}
        self.assertLess(order["Capture measured commit and binary before runtime"], order["Run Gaussian pipeline smoke tests"])
        self.assertLess(order["Postflight - GPU contention verdict (#875)"], order["Bind canonical GPU evidence to reviewed commit"])
        self.assertLess(order["Run streaming GPU runtime gate (canonical blocking profile, windows-vulkan)"], order[gpu["name"]])
        self.assertLess(order[gpu["name"]], order["Postflight - GPU contention verdict (#875)"])
        receipt = next(step for step in steps if step["name"] == "Bind canonical GPU evidence to reviewed commit")
        self.assertIn("check_universal_gpu_evidence.py --write-receipt", receipt["run"])
        self.assertIn("--measurement artifacts/gpu-measurement.json", receipt["run"])
        self.assertEqual(receipt["env"]["GS_STEP_GPU_HARNESS"], "${{ steps.gpu_harness.outcome }}")
        artifact = next(step for step in steps if step["name"] == "Upload canonical PR GPU receipt")
        self.assertEqual(artifact["with"]["path"], "repo/artifacts/universal-gpu/")
        self.assertEqual(artifact["with"]["if-no-files-found"], "error")
        download = next(step for step in workflow["jobs"]["canonical-gpu-receipt"]["steps"] if step.get("uses", "").startswith("actions/download-artifact@"))
        self.assertNotIn("risk_class", download["if"])

    def test_all_producer_enforcement_scripts_belong_to_digest_manifest(self):
        manifest = json.loads((ROOT / "scripts/agentic/gpu_commit_policy.json").read_text())
        spec = importlib.util.spec_from_file_location("wiring_policy", ROOT / "scripts/agentic/gpu_commit_policy.py")
        policy = importlib.util.module_from_spec(spec); spec.loader.exec_module(policy)
        covered = lambda path: policy.selected_dependency(path, manifest)
        commands = [step["run"] for job in load(PRODUCER)["jobs"].values() for step in job.get("steps", []) if "run" in step]
        invoked = {path for command in commands for path in re.findall(r"python\s+([A-Za-z0-9_./-]+\.py)", command)}
        self.assertTrue(invoked)
        for path in invoked:
            with self.subTest(path=path): self.assertTrue(covered(path), "Missing producer execution dependency: " + path)
        for path in ("scripts/agentic/watch_pr_gpu_evidence.py", "scripts/agentic/gpu_commit_policy.py", "scripts/agentic/gpu_commit_policy.json", "tests/runtime/runtime_scenarios.json"):
            self.assertTrue(covered(path))
        for path in ("tests/SCsub", "modules/SCsub", "modules/modules_builders.py", "thirdparty/doctest/doctest.h",
                     "SConstruct", "methods.py", "platform/windows/detect.py", "modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp",
                     "tests/test_projection_math.cpp", "tests/servers/rendering/test_renderer_scene_cull.h", "docs/reference/renderer_release_gate_manifest.json"):
            with self.subTest(critical_input=path): self.assertTrue(covered(path))
        for job in load(PRODUCER)["jobs"].values():
            for step in job.get("steps", []):
                if "uses" in step:
                    self.assertRegex(step["uses"], r"@[0-9a-f]{40}$")

    def resolver(self, *, cleanup=False, source=None, head=None, target="master", associated=1):
        job = load(CONTROLLER)["jobs"]["trusted-gpu-cleanup" if cleanup else "trusted-gpu-controller"]
        script = next(step for step in job["steps"] if step.get("id") == "event")["with"]["script"]
        pull = {"number": 1, "state": "open", "head": {"sha": head or "a" * 40}, "base": {"ref": target}}
        payload = {"pull_request": pull} if source is None else {"workflow_run": source}
        harness = """
const outputs = {};
const context = {payload: PAYLOAD, repo: {owner: 'o', repo: 'r'}};
const core = {setOutput: (key, value) => { outputs[key] = value; }};
const github = {paginate: async () => Array.from({length: ASSOCIATED}, (_, i) => ({...PULL, number: i + 1})), rest: {
repos: {get: async () => ({data: {default_branch: 'master'}}), getBranch: async () => ({data: {commit: {sha: 'b'.repeat(40)}}})},
pulls: {get: async () => ({data: PULL})}}};
(async () => { SCRIPT })().then(() => console.log(JSON.stringify({outputs})), error => console.log(JSON.stringify({outputs, error: String(error)})));
""".replace("PAYLOAD", json.dumps(payload)).replace("PULL", json.dumps(pull)).replace("ASSOCIATED", str(associated)).replace("SCRIPT", script)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "resolver.js"; path.write_text(harness)
            result = subprocess.run([shutil.which("node"), str(path)], check=True, capture_output=True, encoding="utf-8", timeout=20, env={**os.environ, "RUNNER_TEMP": temp})
            state = json.loads(result.stdout.strip().splitlines()[-1])
            if "path" in state["outputs"]:
                state["event"] = json.loads(Path(state["outputs"]["path"]).read_text())
            return state

    def test_actual_resolver_binds_live_default_not_stale_pr_base(self):
        state = self.resolver()
        self.assertNotIn("error", state)
        self.assertEqual(state["event"]["trusted_base_sha"], "b" * 40)
        self.assertEqual(state["event"]["pull_request"]["head"]["sha"], "a" * 40)

    def test_actual_resolver_allows_code_reuse_without_private_pr_ambiguity(self):
        source = {"name": "Gaussian Production Gates", "event": "pull_request", "head_sha": "a" * 40, "id": 9}
        state = self.resolver(source=source, associated=2)
        self.assertNotIn("error", state)
        self.assertEqual(state["event"]["source_run_id"], 9)
        for event in ("merge_group", "push", "schedule", "workflow_dispatch"):
            self.assertIn("error", self.resolver(source={**source, "event": event}))
        self.assertIn("error", self.resolver(source=source, associated=0))

    def test_actual_cleanup_resolver_uses_event_when_controller_outputs_are_absent(self):
        state = self.resolver(cleanup=True)
        self.assertNotIn("error", state)
        self.assertEqual(state["event"]["pull_request"]["head"]["sha"], "a" * 40)
        self.assertIn("error", self.resolver(cleanup=True, head="malformed"))

    def test_actual_resolver_rejects_unsupported_target_or_malformed_sha(self):
        self.assertIn("error", self.resolver(target="other"))
        self.assertIn("error", self.resolver(head="malformed"))

    def test_required_hosted_lane_registers_this_actual_wiring_suite(self):
        gate = (ROOT / ".github/workflows/agentic_pr_gate.yml").read_text()
        self.assertIn("python tests/ci/test_pr_gpu_evidence_wiring.py", gate)


if __name__ == "__main__":
    unittest.main()
