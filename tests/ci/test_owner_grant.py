#!/usr/bin/env python3
"""Actual Linux OpenSSH owner-grant controls, with temporary synthetic keys only.

The required hosted lane runs this full suite. --portable exercises the shipped
inactive profile without claiming Linux crypto on an unsupported platform.
"""
import copy
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("owner_grant_test_subject", ROOT / "scripts/agentic/verify_owner_grant.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class PortableOwnerGrantTests(unittest.TestCase):
    def test_explicit_unconfigured_profile_cannot_authorize_any_success(self):
        profile = json.loads((ROOT / verifier.CONFIG_PATH).read_text())
        profile.update(public_key=None, executable_sha256=None)
        bundle = {verifier.CONFIG_PATH: b"100644\0" + json.dumps(profile).encode()}
        with self.assertRaisesRegex(ValueError, "NOT ACTIVE"):
            verifier.configured_profile(bundle)

    def test_duplicate_nested_or_top_level_transport_is_rejected(self):
        for raw in ('{"binding":{},"binding":{}}', '{"binding":{"kind":"bootstrap","kind":"migration"}}',
                    '"object required"', ' ' * (verifier.MAX_ENVELOPE + 1)):
            with self.subTest(raw=raw[:50]), self.assertRaises(ValueError):
                verifier.parse_transport(raw)


class ActualOpenSSHOwnerGrantTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Unsupported/missing tooling is an actual required-lane failure, never a skip.
        if sys.platform != "linux" or not Path(verifier.EXECUTABLE).is_file():
            raise RuntimeError("The actual signature suite requires supported Linux OpenSSH")
        cls.temp = tempfile.TemporaryDirectory(prefix="gs-synthetic-owner-")
        cls.root = Path(cls.temp.name)
        cls.key = cls.root / "synthetic-key"
        cls.wrong_key = cls.root / "synthetic-wrong-key"
        for path in (cls.key, cls.wrong_key):
            subprocess.run([verifier.EXECUTABLE, "-q", "-t", "ed25519", "-N", "", "-f", str(path)],
                           check=True, capture_output=True, timeout=20)
        cls.profile = json.loads((ROOT / verifier.CONFIG_PATH).read_text())
        cls.profile.update(public_key=" ".join(Path(str(cls.key) + ".pub").read_text().split()[:2]),
                           executable_sha256=hashlib.sha256(Path(verifier.EXECUTABLE).read_bytes()).hexdigest())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.bundle = {verifier.CONFIG_PATH: b"100644\0" + json.dumps(self.profile).encode()}
        self.binding = dict(version="gs-owner-grant-v1", kind="bootstrap", repository=verifier.REPOSITORY,
                            owner_id=verifier.OWNER_ID, target_branch="master", base_sha="a" * 40,
                            policy_sha="a" * 40, head_sha="b" * 40, context="gpu-code-evidence-v1-" + "c" * 64,
                            app_id=701, run_id="9", run_attempt="1",
                            expires_utc=(datetime.now(timezone.utc) + timedelta(hours=1)).isoformat())
        self.envelope = self.sign(self.binding)

    def sign(self, binding, *, key=None, namespace=None):
        # Actual OpenSSH producer output is used unchanged by the production verifier.
        result = subprocess.run([verifier.EXECUTABLE, "-Y", "sign", "-f", str(key or self.key),
                                 "-n", namespace or verifier.NAMESPACE],
                                input=verifier.canonical_binding(binding), check=True,
                                capture_output=True, timeout=20)
        return {"binding": copy.deepcopy(binding), "signature": result.stdout.decode("ascii")}

    def verify(self, envelope=None, *, kind="bootstrap", context=None):
        return verifier.verify_grant(envelope or self.envelope, self.bundle, kind=kind,
                                     repo=verifier.REPOSITORY,
                                     owner={"type": "User", "id": verifier.OWNER_ID, "login": "klausi3D"},
                                     base="a" * 40, head="b" * 40, app_id=701,
                                     context=context or self.binding["context"])

    def test_real_bootstrap_and_migration_signature_positive(self):
        self.assertEqual(self.verify(), self.binding)
        binding = {**self.binding, "kind": "migration", "policy_sha": "d" * 40}
        self.assertEqual(self.verify(self.sign(binding), kind="migration"), binding)

    def test_changed_bytes_wrong_key_namespace_and_signature_are_rejected(self):
        changed = copy.deepcopy(self.envelope)
        changed["binding"]["run_attempt"] = "2"
        bad_signature = copy.deepcopy(self.envelope)
        lines = bad_signature["signature"].splitlines()
        lines[1] = ("B" if lines[1][0] != "B" else "C") + lines[1][1:]
        bad_signature["signature"] = "\n".join(lines) + "\n"
        for envelope in (changed, self.sign(self.binding, key=self.wrong_key),
                         self.sign(self.binding, namespace="unrelated-purpose"), bad_signature):
            with self.subTest(envelope=envelope["binding"]["run_attempt"]), self.assertRaises(ValueError):
                self.verify(envelope)

    def test_genuinely_signed_cross_binding_expiry_and_old_grants_fail(self):
        mutations = {"repository": "writer/other", "owner_id": True, "target_branch": "other",
                     "version": "old", "kind": "migration", "base_sha": "e" * 40,
                     "head_sha": "f" * 40, "policy_sha": "d" * 40, "app_id": 702,
                     "context": "gpu-code-evidence-v1-" + "e" * 64, "run_id": "0", "run_attempt": 1,
                     "expires_utc": "2000-01-01T00:00:00+00:00"}
        for field, value in mutations.items():
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.verify(self.sign({**self.binding, field: value}))
        for expiry in ("2999-01-01T00:00:00", "2999-01-01T00:00:00+01:00"):
            with self.assertRaises(ValueError): self.verify(self.sign({**self.binding, "expires_utc": expiry}))

    def test_unknown_missing_fields_unsigned_records_and_key_options_fail(self):
        for envelope in (self.binding, {**self.envelope, "approval_comment_id": 71},
                         {"binding": {**self.binding, "unknown": True}, "signature": self.envelope["signature"]}):
            with self.assertRaises(ValueError): self.verify(envelope)
        for key in (None, self.profile["public_key"] + " comment", "cert-authority " + self.profile["public_key"]):
            profile = {**self.profile, "public_key": key}
            self.bundle[verifier.CONFIG_PATH] = b"100644\0" + json.dumps(profile).encode()
            with self.assertRaises(ValueError): self.verify()

    def test_null_drifted_tool_and_other_executable_cannot_authorize(self):
        for field, value in (("executable_sha256", None), ("executable_sha256", "0" * 64),
                             ("executable", "/bin/true"), ("owner_id", 17)):
            self.bundle[verifier.CONFIG_PATH] = b"100644\0" + json.dumps({**self.profile, field: value}).encode()
            with self.subTest(field=field), self.assertRaises(ValueError): self.verify()

    def test_actual_shared_bootstrap_migration_and_fresh_transport_recheck(self):
        subject = importlib.util.spec_from_file_location("owner_grant_policy_subject", ROOT / "scripts/agentic/gpu_commit_policy.py")
        policy = importlib.util.module_from_spec(subject)
        subject.loader.exec_module(policy)
        record = self.envelope
        variable = policy.BOOTSTRAP_VARIABLE
        def api(endpoint):
            if endpoint.endswith("/variables"):
                return {"total_count": 1, "variables": [{"name": variable}]}
            if endpoint.endswith("/variables/" + variable):
                return {"name": variable, "value": json.dumps(record)}
            if endpoint == "repos/" + verifier.REPOSITORY:
                return {"owner": {"type": "User", "id": verifier.OWNER_ID, "login": "klausi3D"}}
            raise AssertionError(endpoint)
        for kind in ("bootstrap", "migration"):
            variable = policy.BOOTSTRAP_VARIABLE if kind == "bootstrap" else policy.MIGRATION_VARIABLE
            binding = {**self.binding, "kind": kind, "policy_sha": "a" * 40 if kind == "bootstrap" else "d" * 40}
            record = self.sign(binding)
            def evaluate():
                return policy.owner_binding(api, verifier.REPOSITORY, "a" * 40, "b" * 40, 701,
                                            self.bundle, kind=kind, context=self.binding["context"])
            self.assertEqual(evaluate(), binding)
            # A writer can edit transport and an old owner comment; neither is a signature.
            record["binding"]["run_attempt"] = "2"
            with self.subTest(kind=kind), self.assertRaises(ValueError): evaluate()

    def test_actual_proof_settings_rejects_null_profile_in_ordinary_mode(self):
        subject = importlib.util.spec_from_file_location("owner_watcher_subject", ROOT / "scripts/agentic/watch_pr_gpu_evidence.py")
        watcher = importlib.util.module_from_spec(subject)
        subject.loader.exec_module(watcher)
        event = {"trusted_base_sha": "a" * 40, "pull_request": {"head": {"sha": "b" * 40}}}
        published = []
        def api(endpoint, **kwargs):
            if "payload" in kwargs:
                published.append(kwargs["payload"])
                return {}
            if endpoint.endswith("/branches/master"): return {"commit": {"sha": "a" * 40}}
            if endpoint.endswith("/variables"): return {"total_count": 0, "variables": []}
            raise AssertionError(endpoint)
        null_profile = json.loads((ROOT / verifier.CONFIG_PATH).read_text())
        null_profile.update(public_key=None, executable_sha256=None)
        null_bundle = {verifier.CONFIG_PATH: b"100644\0" + json.dumps(null_profile).encode()}
        with patch.dict(watcher.os.environ, {"GITHUB_REPOSITORY": verifier.REPOSITORY, "GS_PUBLISH_APP_ID": "701"}), \
             patch.object(watcher.checker, "git", return_value="a" * 40), \
             patch.object(watcher, "api", side_effect=api), \
             patch.object(watcher.policy, "policy_bundle", return_value=null_bundle):
            with self.assertRaisesRegex(ValueError, "NOT ACTIVE"):
                watcher.proof_settings(event)

    def test_actual_watcher_bootstrap_reverifies_current_signed_transport(self):
        subject = importlib.util.spec_from_file_location("owner_watcher_live_subject", ROOT / "scripts/agentic/watch_pr_gpu_evidence.py")
        watcher = importlib.util.module_from_spec(subject)
        subject.loader.exec_module(watcher)
        context = watcher.policy.context_for(self.bundle)
        binding = {**self.binding, "context": context}
        record = self.sign(binding)
        event = {"trusted_base_sha": "a" * 40, "pull_request": {"head": {"sha": "b" * 40}}}
        published = []
        def api(endpoint, **kwargs):
            if "payload" in kwargs:
                published.append(kwargs["payload"])
                return {}
            if endpoint.endswith("/branches/master"): return {"commit": {"sha": "a" * 40}}
            if endpoint.endswith("/variables"):
                return {"total_count": 1, "variables": [{"name": watcher.policy.BOOTSTRAP_VARIABLE}]}
            if endpoint.endswith("/variables/" + watcher.policy.BOOTSTRAP_VARIABLE):
                return {"name": watcher.policy.BOOTSTRAP_VARIABLE, "value": json.dumps(record)}
            if endpoint == "repos/" + verifier.REPOSITORY:
                return {"owner": {"type": "User", "id": verifier.OWNER_ID, "login": "klausi3D"}}
            raise AssertionError(endpoint)
        with patch.dict(watcher.os.environ, {"GITHUB_REPOSITORY": verifier.REPOSITORY, "GS_PUBLISH_APP_ID": "701"}), \
             patch.object(watcher.checker, "git", return_value="a" * 40), \
             patch.object(watcher, "api", side_effect=api), \
             patch.object(watcher.policy, "policy_bundle", return_value=self.bundle):
            self.assertEqual(watcher.proof_settings(event), (self.bundle, context, binding))
            self.assertEqual(watcher.proof_settings(event), (self.bundle, context, binding))
            record["binding"]["run_attempt"] = "2"
            with self.assertRaises(ValueError): watcher.proof_settings(event)
            run = {"id": 9, "run_attempt": 1, "status": "completed", "conclusion": "success"}
            outcome = watcher.EvidenceOutcome(0, "fixture only", (9, 1), "b" * 40, context)
            with patch.object(watcher.policy, "require_producer_pin"), patch.object(watcher, "current_pull"), \
                 patch.object(watcher, "latest_source_run", return_value=run), patch.object(watcher, "live_proof"), \
                 patch.dict(watcher.os.environ, {"GITHUB_RUN_ID": "123"}), self.assertRaises(ValueError):
                watcher.publish_status(verifier.REPOSITORY, "b" * 40, context, "success", "fixture only",
                                       event=event, proof=outcome)
            self.assertEqual(published, [])
            record = {key: binding[key] for key in ("head_sha", "context", "run_id", "run_attempt")}
            with self.assertRaises(ValueError): watcher.proof_settings(event)


if __name__ == "__main__":
    if "--portable" in sys.argv:
        sys.argv.remove("--portable")
        unittest.main(defaultTest="PortableOwnerGrantTests")
    else:
        unittest.main()
