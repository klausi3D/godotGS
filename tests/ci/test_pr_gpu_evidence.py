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


class LegacyEvidenceVerdictTests(unittest.TestCase):
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


class UniversalPolicyTests(unittest.TestCase):
    def test_digest_uses_committed_dependency_bytes_modes_and_names(self):
        policy = watcher.policy
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            def git(*args):
                return subprocess.run(["git", *args], cwd=root, check=True,
                                      capture_output=True, encoding="utf-8").stdout.strip()
            git("init", "-q")
            git("config", "user.name", "Fixture")
            git("config", "user.email", "fixture@example.invalid")
            (root / "scripts/agentic").mkdir(parents=True)
            (root / "tests/runtime").mkdir(parents=True)
            manifest = {"version": policy.POLICY_VERSION, "files": [policy.MANIFEST],
                        "prefixes": ["tests/runtime/"]}
            (root / policy.MANIFEST).write_text(json.dumps(manifest))
            dependency = root / "tests/runtime/profile.json"
            dependency.write_text('{"threshold": 1}')
            git("add", "."); git("commit", "-qm", "trusted policy")
            base = git("rev-parse", "HEAD")
            with patch.object(policy, "ROOT", root):
                bundle = policy.policy_bundle(base)
                context = policy.context_for(bundle)
                dependency.write_text('{"threshold": 0}')
                (root / "tests/runtime/untracked.py").write_text("malicious local bytes")
                self.assertEqual(policy.context_for(policy.policy_bundle(base)), context)
                git("add", "."); git("commit", "-qm", "changed policy")
                self.assertNotEqual(policy.context_for(policy.policy_bundle()), context)
                proposed = policy.policy_bundle()
                with patch.object(policy, "_git", return_value=b"") as command:
                    with patch.object(policy, "policy_bundle", return_value=proposed), self.assertRaises(ValueError):
                        policy.require_producer_pin(git("rev-parse", "HEAD"), bundle)
                    self.assertIn(("fetch", "--no-tags", "origin", git("rev-parse", "HEAD")),
                                  [call.args for call in command.call_args_list])
        for mutation in ({"different-name": b"bytes"}, {"name": b"100755\0bytes"},
                         {"name": b"100644\0different"}):
            self.assertNotEqual(policy.context_for({"name": b"100644\0bytes"}), policy.context_for(mutation))

    def test_api_credentials_are_scoped_and_never_fall_back(self):
        env = {"GH_TOKEN": "read-only-fixture", "GS_PROTECTION_TOKEN": "settings-read-fixture",
               "GS_PUBLISH_TOKEN": "app-status-fixture"}
        requests = [("repos/o/r/git/commits/" + "a" * 40, {}, "GH_TOKEN"),
                    ("repos/o/r/branches/master/protection", {}, "GS_PROTECTION_TOKEN"),
                    ("repos/o/r/environments/trusted-gpu-verdict", {}, "GS_PROTECTION_TOKEN"),
                    ("repos/o/r/rulesets", {}, "GS_PROTECTION_TOKEN"),
                    ("repos/o/r/statuses/" + "a" * 40, {"payload": {"state": "failure"}}, "GS_PUBLISH_TOKEN")]
        for endpoint, kwargs, variable in requests:
            with self.subTest(endpoint=endpoint), patch.dict(os.environ, env, clear=True):
                with patch.object(watcher.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"{}")) as run:
                    watcher.api(endpoint, **kwargs)
                    self.assertEqual(run.call_args.kwargs["env"]["GH_TOKEN"], env[variable])
                del os.environ[variable]
                with patch.object(watcher.subprocess, "run") as run, self.assertRaises(ValueError):
                    watcher.api(endpoint, **kwargs)
                run.assert_not_called()


class UniversalReceiptProducerTests(unittest.TestCase):
    """Capture JSON from actual Python producers; synthetic observations are NOT GPU evidence."""
    @staticmethod
    def producer_reports(binary_path):
        universal = watcher.checker
        runner = universal.load_module("receipt_fixture_lane_producer", ROOT / "tests/ci/run_module_tests.py")
        runtime = universal.load_module("receipt_fixture_runtime_producer", ROOT / "tests/runtime/run_runtime_validation.py")
        runs = runner._build_module_test_runs(True)
        ledger = runner.LaneLedger((name, strict) for name, _, strict in runs)
        for index in range(len(runs)):
            # Deliberately synthetic count observations; no doctest output format
            # or native execution is fabricated. The real JSON producer is called.
            result = runner.LaneResult(outcome=runner.LANE_OUTCOME_PASS, exit_code=0,
                exit_code_reported=True, summary_reported=True, zero_coverage=False,
                passed_tests=1, failed_tests=0, passed_assertions=1, failed_assertions=0, skipped_markers=0)
            ledger.record(index, result, ended_run=False)
        reports = {"module-lanes.json": json.dumps(ledger.to_json(ledger.totals(), lane_loop_exit_code=0)).encode()}
        config = json.loads((ROOT / "tests/runtime/runtime_scenarios.json").read_text())
        for profile, filename in (("headless-ci", "headless-report.json"), ("streaming-gpu-ci", "streaming-report.json")):
            definition = config["profiles"][profile]
            observations = [runtime.TestResult(name, [binary_path], 0.0, 0, "", "", status="passed")
                            for name in definition["gd_tests"]]
            if definition.get("requires_renderer_proof"):
                observations[0].metrics = {"renderer_proof_kind": "schema-only", "renderer_proof_status": "passed"}
            report = runtime.summarise(observations)
            report["profile"] = profile
            report["gd_mode"] = definition["gd_mode"]
            report["renderer_proof"] = runtime._build_renderer_proof_summary(observations, required=bool(definition.get("requires_renderer_proof")))
            report["schema_errors"] = runtime._validate_summary_schema(report)
            report["schema_valid"] = not report["schema_errors"]
            reports[filename] = json.dumps(report).encode()
        return reports

    def setUp(self):
        self.universal = watcher.checker
        self.head = "a" * 40
        self.context = watcher.policy.context_for({"schema-fixture-policy": b"policy"})
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / "schema-fixture.bin"
        self.binary.write_bytes(b"schema fixture only, not a Godot build")
        self.reports = self.producer_reports(str(self.binary.resolve()))
        self.state = self.root / "measurement.json"
        self.destination = self.root / "evidence"
        self.env = {"GITHUB_RUN_ID": "9", "GITHUB_RUN_ATTEMPT": "1"}
        self.env.update({"GS_STEP_" + name.upper(): "success" for name in self.universal.REQUIRED_STEPS})
        self.identity = patch.object(self.universal, "current_identity", return_value=(self.head, self.context))
        self.identity.start(); self.addCleanup(self.identity.stop)
        # Temporary ROOT contains report bytes; production validation reads the
        # real fixed-policy source, never imports a downloaded artifact.
        with patch.object(self.universal, "ROOT", self.root):
            self.universal.start_measurement(self.state, self.binary)
        for name, relative in self.universal.REPORT_SOURCES.items():
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(self.reports[name])

    def write(self):
        original_load = self.universal.load_module
        def source_loader(name, path):
            relative = path.relative_to(self.root)
            return original_load(name, ROOT / relative)
        with patch.dict(os.environ, self.env), patch.object(self.universal, "ROOT", self.root), \
             patch.object(self.universal, "load_module", side_effect=source_loader):
            # Runtime config is actual producer configuration, not a format fixture.
            config = self.root / "tests/runtime/runtime_scenarios.json"
            config.parent.mkdir(parents=True, exist_ok=True)
            config.write_bytes((ROOT / "tests/runtime/runtime_scenarios.json").read_bytes())
            self.universal.write_receipt(self.destination, self.binary, self.state)
        return json.loads((self.destination / "pr-gpu-evidence.json").read_text())

    def validate(self, receipt, reports=None):
        self.universal.validate_universal_receipt(receipt, reports or self.reports,
            head=self.head, context=self.context, run_id="9", run_attempt="1")

    def test_actual_receipt_writer_schema_roundtrip_is_structural_only(self):
        receipt = self.write()
        self.validate(receipt)
        self.assertEqual(receipt["checkout_sha"], receipt["head_sha"])
        self.assertEqual(set(path.name for path in self.destination.iterdir()), set(self.universal.UNIVERSAL_ARTIFACT_FILES))

    def test_producer_rejects_changed_or_missing_binary_after_start(self):
        self.binary.write_bytes(b"changed binary")
        with self.assertRaises(ValueError): self.write()
        self.binary.unlink()
        with self.assertRaises(OSError): self.write()

    def test_windows_console_launcher_and_actual_editor_are_both_bound(self):
        self.binary = self.root / "godot.console.exe"
        self.binary.write_bytes(b"schema fixture launcher")
        editor = self.root / "godot.exe"
        with self.assertRaises(OSError): self.universal.binary_components(self.binary)
        editor.write_bytes(b"schema fixture editor")
        with patch.object(self.universal, "ROOT", self.root):
            self.universal.start_measurement(self.state, self.binary)
        self.reports = self.producer_reports(str(self.binary.resolve()))
        for name, relative in self.universal.REPORT_SOURCES.items():
            (self.root / relative).write_bytes(self.reports[name])
        receipt = self.write()
        self.validate(receipt)
        self.assertEqual(len(receipt["binary_components_sha256"]), 2)
        incomplete = copy.deepcopy(receipt)
        del incomplete["binary_components_sha256"][str(editor.resolve())]
        with self.assertRaises(ValueError): self.validate(incomplete)
        editor.write_bytes(b"changed actual editor")
        with self.assertRaises(ValueError): self.write()

    def test_producer_clears_stale_reports_before_measurement(self):
        with patch.object(self.universal, "ROOT", self.root):
            self.universal.start_measurement(self.state, self.binary)
        self.assertTrue(all(not (self.root / relative).exists() for relative in self.universal.REPORT_SOURCES.values()))
        with self.assertRaises(OSError): self.write()

    def test_producer_rejects_stale_report_even_if_bytes_look_valid(self):
        path = self.root / self.universal.REPORT_SOURCES["module-lanes.json"]
        os.utime(path, (1, 1))
        with self.assertRaises(ValueError): self.write()

    def test_every_binding_step_and_report_digest_can_fail(self):
        receipt = self.write()
        for field in ("head_sha", "checkout_sha", "policy_context", "run_id", "run_attempt", "schema_version", "binary_before_sha256", "binary_path"):
            changed = copy.deepcopy(receipt); changed[field] = "wrong"
            with self.subTest(field=field), self.assertRaises(ValueError): self.validate(changed)
        for step in self.universal.REQUIRED_STEPS:
            changed = copy.deepcopy(receipt); changed["steps"][step] = "skipped"
            with self.subTest(step=step), self.assertRaises(ValueError): self.validate(changed)
        for name in self.reports:
            changed = dict(self.reports); changed[name] = b"{}"
            with self.subTest(report=name), self.assertRaises(ValueError): self.validate(receipt, changed)

    def test_missing_gpu_lane_or_zero_coverage_cannot_validate(self):
        receipt = self.write()
        for modification in ("delete-lane", "zero-coverage", "missing-summary", "not-run"):
            report = json.loads(self.reports["module-lanes.json"])
            index = next(i for i, lane in enumerate(report["lanes"]) if lane["strict"])
            if modification == "delete-lane": del report["lanes"][index]
            if modification == "zero-coverage": report["lanes"][index]["passed_tests"] = 0
            if modification == "missing-summary": report["lanes"][index]["summary_reported"] = False
            if modification == "not-run": report["lanes"][index]["outcome"] = "NOT-RUN"
            modified = dict(self.reports); modified["module-lanes.json"] = json.dumps(report).encode()
            changed = copy.deepcopy(receipt)
            import hashlib
            changed["sha256"]["module-lanes.json"] = hashlib.sha256(modified["module-lanes.json"]).hexdigest()
            with self.subTest(modification=modification), self.assertRaises(ValueError): self.validate(changed, modified)

    def test_wrong_runtime_profile_missing_scenario_failed_or_skipped_never_validate(self):
        receipt = self.write()
        for field, value in (("profile", "other"), ("gd_mode", "headless"), ("failed", 1), ("skipped", 1), ("schema_valid", False), ("tests", [])):
            report = json.loads(self.reports["streaming-report.json"]); report[field] = value
            modified = dict(self.reports); modified["streaming-report.json"] = json.dumps(report).encode()
            changed = copy.deepcopy(receipt)
            import hashlib
            changed["sha256"]["streaming-report.json"] = hashlib.sha256(modified["streaming-report.json"]).hexdigest()
            with self.subTest(field=field), self.assertRaises(ValueError): self.validate(changed, modified)


class UniversalPublicationTests(unittest.TestCase):
    """Lifecycle/security metadata mocks; these tests do not measure GPU work."""
    def setUp(self):
        self.head, self.base = "a" * 40, "b" * 40
        self.bundle = {"policy-fixture": b"fixed fixture policy"}
        self.context = watcher.policy.context_for(self.bundle)
        self.pull = dict(number=1, state="open", head={"sha": self.head, "repo": {"full_name": "owner/repo"}},
                         base={"ref": "master", "repo": {"full_name": "owner/repo"}})
        self.event = {"pull_request": self.pull, "trusted_base_sha": self.base, "source_run_id": 9}
        self.run = dict(id=9, run_attempt=1, head_sha=self.head, event="pull_request", status="completed", conclusion="success")
        self.states = []
        self.parents = [{"sha": self.base}]
        self.strict = True
        self.app = 701
        self.required_context = self.context
        self.environment_entries = [{"name": "master", "type": "branch"}]
        self.merge_base = self.base
        self.bootstrap = None
        self.enforce_admins = True
        self.force_pushes = False
        self.rulesets = []
        self.result = watcher.EvidenceOutcome(0, "accepted", (9, 1), self.head, self.context)
        self.receipt_error = None
        self.producer_error = None

    def metadata(self, endpoint, **kwargs):
        if "payload" in kwargs:
            self.states.append(kwargs["payload"])
            return {}
        if "/git/commits/" in endpoint:
            return {"sha": self.head, "parents": self.parents}
        if endpoint.endswith("/pulls/1"):
            return self.pull
        if endpoint == "repos/owner/repo":
            return {"default_branch": "master"}
        if endpoint.endswith("/rulesets"):
            return self.rulesets
        if endpoint.endswith("/branches/master"):
            return {"protected": True, "commit": {"sha": self.base}}
        if "/compare/" in endpoint:
            return {"merge_base_commit": {"sha": self.merge_base}}
        if endpoint.endswith("/protection"):
            checks = [{"context": "agentic-pr-gate", "app_id": 15368}, {"context": "docs-build", "app_id": 15368}]
            if self.required_context:
                checks.append({"context": self.required_context, "app_id": self.app})
            return {"required_status_checks": {"strict": self.strict, "checks": checks},
                    "enforce_admins": {"enabled": self.enforce_admins},
                    "allow_force_pushes": {"enabled": self.force_pushes}, "allow_deletions": {"enabled": False}}
        if endpoint.endswith("/deployment-branch-policies"):
            return {"total_count": len(self.environment_entries), "branch_policies": self.environment_entries}
        if "/environments/" in endpoint:
            return {"deployment_branch_policy": {"protected_branches": False, "custom_branch_policies": True}}
        raise AssertionError(endpoint)

    def exercise(self, *, real_main=False):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "event.json"
            path.write_text(json.dumps(self.event))
            env = dict(GITHUB_EVENT_PATH=str(path), GITHUB_REPOSITORY="owner/repo", GITHUB_RUN_ID="123", GS_PUBLISH_APP_ID="701")
            settings = (self.bundle, self.context, self.bootstrap)
            patches = [patch.dict(os.environ, env), patch.object(watcher, "api", side_effect=self.metadata),
                       patch.object(watcher, "proof_settings", return_value=settings),
                       patch.object(watcher.policy, "require_producer_pin", side_effect=self.producer_error),
                       patch.object(watcher, "latest_source_run", side_effect=lambda *args: self.run)]
            if real_main:
                patches.extend([patch.object(sys, "argv", ["watcher"]),
                                patch.object(watcher, "run_verdict", side_effect=self.receipt_error)])
            else:
                patches.append(patch.object(watcher, "main", return_value=self.result))
            from contextlib import ExitStack
            with ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                return watcher.controller()

    def test_valid_single_parent_publisher_route(self):
        self.assertEqual(self.exercise(), 0)
        self.assertEqual([p["state"] for p in self.states], ["pending", "success"])
        self.assertTrue(all(p["context"] == self.context for p in self.states))

    def test_canonical_parent_count_rejects_every_merge_synthetic_or_missing_parent_route(self):
        for parents in ([], [{"sha": self.base}, {"sha": "c" * 40}], None, [{"sha": "wrong"}], [None]):
            with self.subTest(parents=parents):
                self.parents, self.states = parents, []
                self.assertEqual(self.exercise(), 1)
                self.assertNotIn("success", [p["state"] for p in self.states])

    def test_canonical_identity_cannot_be_replaced_by_receipt_or_grafted_local_view(self):
        with patch.object(watcher.policy, "eligible_commit", wraps=watcher.policy.eligible_commit):
            self.parents = [{"sha": self.base}, {"sha": "c" * 40}]
            with patch.object(watcher.checker, "git", return_value=self.head):
                self.assertEqual(self.exercise(), 1)
        self.assertEqual(self.states[-1]["state"], "failure")

    def test_steady_state_live_settings_are_mandatory(self):
        changes = [("strict", False), ("enforce_admins", False), ("force_pushes", True),
                   ("required_context", None), ("required_context", "gpu-evidence-gate"),
                   ("app", -1), ("app", 702), ("merge_base", "c" * 40), ("rulesets", [{}]),
                   ("environment_entries", [{"name": "master", "type": "tag"}]),
                   ("environment_entries", [{"name": "master", "type": "branch"}, {"name": "refs/pull/*/merge", "type": "branch"}])]
        for name, value in changes:
            previous = getattr(self, name)
            with self.subTest(setting=name, value=value):
                setattr(self, name, value)
                self.states = []
                self.assertEqual(self.exercise(), 1)
                self.assertNotIn("success", [p["state"] for p in self.states])
            setattr(self, name, previous)

    def test_named_bootstrap_never_bypasses_parent_strict_or_exact_run(self):
        self.required_context = None
        self.bootstrap = {"head_sha": self.head, "context": self.context, "run_id": "9", "run_attempt": "1"}
        self.assertEqual(self.exercise(), 0)
        for name, value in (("strict", False), ("parents", [{"sha": self.base}, {"sha": "c" * 40}])):
            previous = getattr(self, name)
            with self.subTest(setting=name):
                setattr(self, name, value)
                self.states = []
                self.assertEqual(self.exercise(), 1)
                self.assertNotIn("success", [p["state"] for p in self.states])
            setattr(self, name, previous)
        self.bootstrap["run_attempt"] = "2"
        self.assertEqual(self.exercise(), 1)

    def test_shared_success_and_bootstrap_cannot_bypass_changed_producer(self):
        self.producer_error = ValueError("changed producer definitions")
        for bootstrap in (None, {"head_sha": self.head, "context": self.context,
                                 "run_id": "9", "run_attempt": "1"}):
            self.bootstrap, self.states = bootstrap, []
            self.assertEqual(self.exercise(), 1)
            self.assertNotIn("success", [p["state"] for p in self.states])

    def test_unmeasured_head_or_new_attempt_cannot_receive_old_proof(self):
        for result in (watcher.EvidenceOutcome(0, "accepted"),
                       watcher.EvidenceOutcome(0, "accepted", (8, 1), self.head, self.context),
                       watcher.EvidenceOutcome(0, "accepted", (9, 1), "c" * 40, self.context),
                       watcher.EvidenceOutcome(0, "accepted", (9, 1), self.head, "old-context")):
            with self.subTest(proof=result):
                self.result, self.states = result, []
                self.assertEqual(self.exercise(), 1)
                self.assertNotIn("success", [p["state"] for p in self.states])

    def test_pr_number_risk_labels_and_prior_base_do_not_exempt_missing_evidence(self):
        for risk in ("R0", "R1", "R2", "R3"):
            with self.subTest(risk=risk):
                self.event["risk_class"] = risk
                self.pull["labels"] = [{"name": "ci/gpu-runner-unavailable"}]
                self.pull["changed_files"] = 1
                self.pull["base"]["sha"] = "e" * 40
                self.states = []
                self.receipt_error = ValueError("receipt absent")
                self.assertEqual(self.exercise(real_main=True), 1)
                self.assertNotIn("success", [p["state"] for p in self.states])

    def test_full_proof_can_be_reused_by_another_pr_when_immutable_code_and_policy_are_same(self):
        self.pull["number"] = 2
        original = self.metadata
        def second(endpoint, **kwargs):
            if endpoint.endswith("/pulls/2"):
                return self.pull
            return original(endpoint, **kwargs)
        self.metadata = second
        self.assertEqual(self.exercise(real_main=True), 0)

    def test_fork_and_unsupported_target_never_receive_success(self):
        for changed in ("fork", "other-target", "closed"):
            original = copy.deepcopy(self.pull)
            if changed == "fork": self.pull["head"]["repo"]["full_name"] = "fork/repo"
            if changed == "other-target": self.pull["base"]["ref"] = "other"
            if changed == "closed": self.pull["state"] = "closed"
            self.states = []
            self.assertEqual(self.exercise(), 1)
            self.assertNotIn("success", [p["state"] for p in self.states])
            self.pull.clear(); self.pull.update(original)

    def test_timeout_failure_description_cannot_claim_gpu_acceptance(self):
        self.result = watcher.EvidenceOutcome(1, "Full evidence missing")
        self.assertEqual(self.exercise(), 1)
        self.assertEqual(self.states[-1]["state"], "failure")
        self.assertNotIn("accepted", self.states[-1]["description"])


class UniversalCleanupTests(unittest.TestCase):
    def test_actual_cleanup_preserves_terminal_newer_runs_and_policy_moves(self):
        head, context = "a" * 40, watcher.policy.context_for({"fixture": b"policy"})
        own = "https://github.com/o/r/actions/runs/123"
        cases = [("pending", own, ["failure"]), ("pending", "newer", []), ("success", own, []), ("failure", own, [])]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "event.json"
            path.write_text(json.dumps({"pull_request": {"head": {"sha": head}}}))
            for state, url, expected in cases:
                calls = []
                with self.subTest(state=state, url=url), patch.dict(os.environ, dict(GITHUB_EVENT_PATH=str(path), GITHUB_REPOSITORY="o/r", GITHUB_RUN_ID="123")), \
                     patch.object(watcher, "pages", return_value=[{"id": 2, "context": context, "state": state, "target_url": url}]), \
                     patch.object(watcher, "api", side_effect=lambda endpoint, **kw: calls.append(kw["payload"]["state"])):
                    self.assertEqual(watcher.cleanup(), 0)
                    self.assertEqual(calls, expected)

    def test_cleanup_ignores_unrelated_context_and_absent_pending(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "event.json"
            path.write_text(json.dumps({"pull_request": {"head": {"sha": "a" * 40}}}))
            with patch.dict(os.environ, dict(GITHUB_EVENT_PATH=str(path), GITHUB_REPOSITORY="o/r", GITHUB_RUN_ID="123")), \
                 patch.object(watcher, "pages", return_value=[{"id": 1, "context": "gpu-evidence-gate", "state": "pending"}]), \
                 patch.object(watcher, "api") as api:
                self.assertEqual(watcher.cleanup(), 0)
                api.assert_not_called()


if __name__ == "__main__":
    unittest.main()
