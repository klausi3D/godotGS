#!/usr/bin/env python3
"""Negative controls for the PR evidence consumer and its real workflow wiring."""
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import yaml

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("pr_evidence", ROOT / "tests/ci/check_pr_gpu_evidence.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
watch_spec = importlib.util.spec_from_file_location("evidence_watcher", ROOT / "scripts/agentic/watch_pr_gpu_evidence.py")
watcher = importlib.util.module_from_spec(watch_spec)
watch_spec.loader.exec_module(watcher)


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
        self.assertEqual(workflow["permissions"], {"contents": "read", "actions": "read", "statuses": "write"})
        job = workflow["jobs"]["trusted-gpu-controller"]
        self.assertEqual(job["runs-on"], "ubuntu-latest")
        checkout = next(step for step in job["steps"] if step.get("uses") == "actions/checkout@v4")
        self.assertEqual(checkout["with"]["ref"], "${{ github.event.pull_request.base.sha || github.event.merge_group.base_sha }}")
        self.assertFalse(checkout["with"]["persist-credentials"])
        commands = [step["run"] for step in job["steps"] if "run" in step]
        self.assertEqual(commands, ["python scripts/agentic/watch_pr_gpu_evidence.py"])


class RealProducerTests(unittest.TestCase):
    def test_receipt_producer_and_consumer_legal_route(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tests/runtime").mkdir(parents=True)
            for profile in ("headless", "streaming_gpu"):
                (root / f"tests/runtime/runtime_validation_report_{profile}_ci.json").write_text("{}")
            binary = root / "godot.exe"
            binary.write_bytes(b"test binary")
            expected = dict(checkout_sha="a" * 40, head_sha="b" * 40,
                            base_sha="c" * 40, run_id="123", run_attempt="1")
            env = {"GS_EVIDENCE_" + key.upper(): value for key, value in expected.items()}
            env.update({"GS_STEP_" + key.upper(): "success" for key in checker.REQUIRED_STEPS})
            path = root / "receipt.json"
            with patch.object(checker, "ROOT", root), patch.object(checker, "git", return_value=expected["checkout_sha"]), patch.dict(os.environ, env):
                checker.write_receipt(path, binary)
                receipt = json.loads(path.read_text())
                checker.validate_receipt(receipt, expected)
                # A checkout mismatch rejects production, not merely consumption.
                with patch.object(checker, "git", return_value="d" * 40), self.assertRaises(ValueError):
                    checker.write_receipt(path, binary)
                binary.unlink()
                with self.assertRaises(OSError):
                    checker.write_receipt(path, binary)

    def test_classifier_reads_different_immutable_policy_and_classifier(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            def run(*args):
                return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True,
                                      encoding="utf-8").stdout.strip()
            run("init", "-q")
            run("config", "user.name", "Fixture")
            run("config", "user.email", "fixture@example.invalid")
            (root / "scripts/agentic").mkdir(parents=True)
            (root / ".agentic").mkdir()
            (root / "scripts/agentic/classify_change.py").write_text((ROOT / "scripts/agentic/classify_change.py").read_text(encoding="utf-8"), encoding="utf-8")
            policy = json.loads((ROOT / ".agentic/policy.json").read_text())
            (root / ".agentic/policy.json").write_text(json.dumps(policy))
            run("add", ".")
            run("commit", "-qm", "base")
            base = run("rev-parse", "HEAD")
            (root / "modules/gaussian_splatting/renderer").mkdir(parents=True)
            (root / "modules/gaussian_splatting/renderer/probe.cpp").write_text("changed")
            run("add", ".")
            run("commit", "-qm", "renderer change")
            with patch.object(checker, "ROOT", root):
                self.assertEqual(checker.classify(base), "R2")
                # HEAD's wrapper and policy may lie; the base copies must still win.
                (root / "scripts/agentic/classify_change.py").write_text('print(\'{"risk_class":"R0"}\')')
                policy["classification"]["rules"] = []
                policy["classification"]["default_unclassified"] = "R0"
                (root / ".agentic/policy.json").write_text(json.dumps(policy))
                self.assertEqual(checker.classify(base), "R2")
                with self.assertRaises(ValueError):
                    checker.classify("")


class TrustedConsumerTests(unittest.TestCase):
    def test_controller_publishes_on_proposed_head_even_on_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            event_path = Path(tmp) / "event.json"
            for event in ({"pull_request": {"head": {"sha": "a" * 40}}},
                          {"merge_group": {"head_sha": "a" * 40}}):
                event_path.write_text(json.dumps(event))
                env = dict(GITHUB_EVENT_PATH=str(event_path), GITHUB_REPOSITORY="owner/repo", GITHUB_RUN_ID="123")
                for result in (0, 1):
                    with patch.dict(os.environ, env), patch.object(watcher, "api") as publish, patch.object(watcher, "main", return_value=result):
                        self.assertEqual(watcher.controller(), result)
                        calls = publish.call_args_list
                        self.assertEqual(len(calls), 2)
                        for call in calls:
                            self.assertEqual(call.args[0], "repos/owner/repo/statuses/" + "a" * 40)
                        self.assertEqual(calls[0].kwargs["payload"]["state"], "pending")
                        self.assertEqual(calls[1].kwargs["payload"]["state"], "success" if result == 0 else "failure")
                with patch.dict(os.environ, env), patch.object(watcher, "api") as publish, patch.object(watcher, "main", side_effect=ValueError("absent evidence")):
                    self.assertEqual(watcher.controller(), 1)
                    self.assertEqual(publish.call_args_list[-1].kwargs["payload"]["state"], "failure")

    def test_actual_job_step_failures_cannot_be_hidden_in_receipt(self):
        steps = [dict(name=name, conclusion="success", started_at="start", completed_at="end")
                 for name in watcher.JOB_STEPS.values()]
        jobs = [dict(name=watcher.GUARD_JOB, conclusion="success"),
                dict(name=watcher.MODULE_JOB, conclusion="success", steps=steps)]
        self.assertEqual(watcher.verify_jobs(jobs)["module-validation"], "success")
        for index in range(len(steps)):
            for state in ("failure", "skipped", "cancelled"):
                changed = copy.deepcopy(jobs)
                changed[1]["steps"][index]["conclusion"] = state
                with self.subTest(index=index, state=state), self.assertRaises(ValueError):
                    watcher.verify_jobs(changed)
        with self.assertRaises(ValueError):
            watcher.verify_jobs([])

    def test_archive_is_bounded_and_unambiguous(self):
        def archive(files):
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w") as handle:
                for name, content in files:
                    handle.writestr(name, content)
            return output.getvalue()
        self.assertEqual(watcher.read_receipt(archive([("pr-gpu-evidence.json", '{"test":true}')])), {"test": True})
        for files in ([], [("wrong.json", "{}")], [("pr-gpu-evidence.json", " " * 65537)],
                      [("pr-gpu-evidence.json", "{}"), ("extra.json", "{}")]):
            with self.subTest(files=len(files)), self.assertRaises(ValueError):
                watcher.read_receipt(archive(files))


if __name__ == "__main__":
    unittest.main()
