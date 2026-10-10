#!/usr/bin/env python3
"""Negative controls for the PR evidence consumer and its real workflow wiring."""
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


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

    def exercise_controller(self, runs, *, source_id=None, partial=False, stale=False,
                            files=("modules/gaussian_splatting/renderer/probe.cpp",), shared=(),
                            risk="R2"):
        base, head, checkout = "c" * 40, "b" * 40, "a" * 40
        pull = dict(number=1, base={"sha": base}, head={"sha": head, "repo": {"full_name": "owner/repo"}},
                    changed_files=len(files), merge_commit_sha=checkout, updated_at="2026-10-06T12:00:00Z")
        associated = [dict(number=1, state="open", head={"sha": head})] + list(shared)
        event = {"pull_request": pull}
        if source_id is not None:
            event["source_run_id"] = source_id
        states, selected = [], []
        current = copy.deepcopy(pull)
        if stale:
            current["base"]["sha"] = "e" * 40
        steps = [dict(name=name, conclusion="success", started_at="start", completed_at="end")
                 for name in watcher.JOB_STEPS.values()]
        if partial:
            steps.pop()
        def metadata(endpoint, **kwargs):
            if "payload" in kwargs:
                states.append(kwargs["payload"]["state"])
                return {}
            if endpoint.endswith("/pulls/1"):
                return current
            receipt = dict(schema_version=1, base_sha=base, head_sha=head, checkout_sha=checkout,
                           run_id=str(selected[-1]["id"]), run_attempt=str(selected[-1]["run_attempt"]),
                           steps={name: "success" for name in checker.REQUIRED_STEPS},
                           sha256={name: "d" * 64 for name in checker.REQUIRED_HASHES})
            output = io.BytesIO()
            with zipfile.ZipFile(output, "w") as archive:
                archive.writestr("pr-gpu-evidence.json", json.dumps(receipt))
            return output.getvalue()
        sequence = iter(runs)
        last_batch = []
        def listing(endpoint, key=None):
            nonlocal last_batch
            if endpoint.endswith("/files"):
                return [{"filename": name} for name in files]
            if endpoint.endswith(f"/commits/{head}/pulls"):
                return associated
            if "workflows/" in endpoint:
                last_batch = next(sequence, last_batch)
                selected.extend(sorted(last_batch, key=lambda run: run["id"]))
                return last_batch
            if endpoint.endswith("/jobs"):
                return [dict(name=watcher.GUARD_JOB, conclusion="success"),
                        dict(name=watcher.MODULE_JOB, conclusion="success", steps=steps)]
            return [dict(name="pr-gpu-evidence-" + str(selected[-1]["run_attempt"]),
                         expired=False, id=456, size_in_bytes=1000)]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "event.json"
            path.write_text(json.dumps(event))
            env = dict(GITHUB_EVENT_PATH=str(path), GITHUB_REPOSITORY="owner/repo", GITHUB_RUN_ID="123")
            with patch.dict(os.environ, env), patch.object(sys, "argv", ["watcher"]), \
                 patch.object(watcher.checker, "git", return_value=base), \
                 patch.object(watcher, "classify_paths", return_value=risk), \
                 patch.object(watcher, "api", side_effect=metadata), \
                 patch.object(watcher, "pages", side_effect=listing), patch.object(watcher.time, "sleep"):
                result = watcher.controller()
        return result, states

    @staticmethod
    def source_run(run_id=9, attempt=1, conclusion="success", created="2026-10-06T12:00:01Z"):
        return dict(id=run_id, run_attempt=attempt, status="completed", conclusion=conclusion,
                    html_url="https://example.invalid/run", created_at=created)

    def test_old_failed_run_waits_for_newly_registered_execution(self):
        old = self.source_run(8, conclusion="failure", created="2026-10-06T11:00:00Z")
        new = self.source_run()
        self.assertEqual(self.exercise_controller([[old], [old, new]]), (0, ["pending", "success"]))

    def test_success_is_reset_by_failed_or_partial_rerun_and_recovers(self):
        self.assertEqual(self.exercise_controller([[self.source_run()]], source_id=9), (0, ["pending", "success"]))
        self.assertEqual(self.exercise_controller([[self.source_run(attempt=2, conclusion="failure")]], source_id=9),
                         (1, ["pending", "failure"]))
        self.assertEqual(self.exercise_controller([[self.source_run(attempt=2)]], source_id=9, partial=True),
                         (1, ["pending", "failure"]))
        self.assertEqual(self.exercise_controller([[self.source_run(attempt=3)]], source_id=9), (0, ["pending", "success"]))

    def test_delayed_old_success_cannot_override_newer_failed_run(self):
        old, new = self.source_run(9), self.source_run(10, conclusion="failure")
        self.assertEqual(self.exercise_controller([[old, new]], source_id=9), (1, ["pending", "failure"]))

    def test_new_rerun_during_receipt_read_invalidates_old_success(self):
        old, new = self.source_run(), self.source_run(attempt=2, conclusion="failure")
        self.assertEqual(self.exercise_controller([[old], [new]], source_id=9), (1, ["pending", "failure"]))

    def test_changed_base_rejects_stale_event_before_exemption(self):
        self.assertEqual(self.exercise_controller([], stale=True), (1, ["pending", "failure"]))

    def test_pr_changing_the_evidence_producer_cannot_certify_itself(self):
        # Same labels and a valid receipt, but the proposed tree defines what ran.
        for path in watcher.PRODUCER_DEFINITIONS:
            with self.subTest(path=path):
                files = ("modules/gaussian_splatting/renderer/probe.cpp", path)
                self.assertEqual(self.exercise_controller([[self.source_run()]], source_id=9, files=files),
                                 (1, ["pending", "failure"]))

    def test_head_shared_with_another_open_pr_fails_closed(self):
        head = "b" * 40
        other = dict(number=2, state="open", head={"sha": head})
        for risk in ("R0", "R2"):
            with self.subTest(risk=risk):
                self.assertEqual(self.exercise_controller([[self.source_run()]], source_id=9, shared=[other], risk=risk),
                                 (1, ["pending", "failure"]))
        # A closed PR, or an open PR whose head has moved on, is not a sharer.
        unrelated = [dict(other, state="closed"), dict(other, head={"sha": "f" * 40})]
        self.assertEqual(self.exercise_controller([[self.source_run()]], source_id=9, shared=unrelated),
                         (0, ["pending", "success"]))

    def test_head_shared_while_waiting_rejects_success(self):
        # A second PR can adopt the head during the (up to 3 h) wait.
        calls = []
        original = watcher.require_unshared_head
        def late_sharer(repo, number, sha):
            calls.append(sha)
            if len(calls) > 1:
                raise ValueError("Head SHA is shared with other open PRs #2")
            return original(repo, number, sha)
        with patch.object(watcher, "require_unshared_head", side_effect=late_sharer):
            self.assertEqual(self.exercise_controller([[self.source_run()]], source_id=9), (1, ["pending", "failure"]))
        self.assertEqual(len(calls), 2)

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
