"""Small deterministic admission/publication controls; no engine/GPU process is launched."""

import copy
import contextlib
import importlib.util
import io
import json
import os
import struct
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("hlod_offline_job", ROOT / "scripts/bake_gsplatworld_offline.py")
JOB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(JOB)
RUNNER_SPEC = importlib.util.spec_from_file_location("hlod_module_test_runner", ROOT / "tests/ci/run_module_tests.py")
RUNNER = importlib.util.module_from_spec(RUNNER_SPEC)
sys.modules[RUNNER_SPEC.name] = RUNNER
RUNNER_SPEC.loader.exec_module(RUNNER)


def manifest():
    limits = {name: 1 for name in JOB.REQUIRED_LIMITS}
    limits.update(job_wall_seconds=300, batch_wall_seconds=3600, cancel_and_drain_seconds=20,
                  process_private_bytes=6 << 30, max_source_bytes=128 << 20,
                  max_input_splats=262144, max_prepared_splats=524288, max_nodes=32768,
                  max_output_bytes=512 << 20, max_scratch_bytes=1 << 30, pending_jobs=0,
                  **JOB.FIXED_METRIC)
    return {"schema_version": 1, "source": {"path": "source.ply", "sha256": "a" * 64,
            "bytes": 100, "count": 1, "sh_degree": 0}, "implementation": {
            "source_sha": "a" * 40, "build_sha256": "b" * 64}, "output": "world.gsplatworld",
            "quality": dict(JOB.QUALITY), "limits": limits}


class AdmissionTests(unittest.TestCase):
    def test_portable_contracts_are_mandatory_in_guard_lane(self):
        with mock.patch.object(RUNNER, "_run_message_guard", return_value=None) as invoke:
            self.assertIsNone(RUNNER._run_required_message_guards())
        callbacks = [call.args[0] for call in invoke.call_args_list]
        self.assertIn(RUNNER._run_hlod_offline_contract_tests, callbacks)

    def test_portable_guard_fails_closed_and_accepts_success(self):
        with mock.patch.object(RUNNER, "HLOD_OFFLINE_CONTRACT_TEST_SCRIPT", ROOT / "missing-offline-test.py"):
            self.assertFalse(RUNNER._run_hlod_offline_contract_tests()[0])
        for code in (1, -9, 0):
            with self.subTest(code=code), mock.patch.object(RUNNER, "_run_command", return_value=(code, "", "")) as run:
                self.assertFalse(RUNNER._run_hlod_offline_contract_tests()[0])
                self.assertEqual(run.call_args.args[0], [sys.executable, str(RUNNER.HLOD_OFFLINE_CONTRACT_TEST_SCRIPT)])

    def test_portable_guard_requires_complete_nonempty_unskipped_unittest_result(self):
        # Capture the actual unittest producer rather than inventing its footer.
        class Probe(unittest.TestCase):
            def test_pass(self):
                self.assertTrue(True)

            @unittest.skip("intentional contract control")
            def test_skip(self):
                pass

        def capture(names):
            stream = io.StringIO()
            unittest.TextTestRunner(stream=stream).run(unittest.TestSuite(Probe(name) for name in names))
            return stream.getvalue()

        complete = capture(["test_pass"])
        for output, expected in ((complete, True), (capture([]), False),
                                 (capture(["test_pass", "test_skip"]), False),
                                 (complete.rsplit("OK", 1)[0], False), ("", False)):
            with self.subTest(output=output), mock.patch.object(RUNNER, "_run_command", return_value=(0, "", output)):
                self.assertEqual(RUNNER._run_hlod_offline_contract_tests()[0], expected)
        with mock.patch.object(RUNNER, "_run_command", return_value=(0, complete, "")):
            self.assertTrue(RUNNER._run_hlod_offline_contract_tests()[0])

    def test_deleting_unittest_main_cannot_pass_the_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "probe.py"
            source = "import unittest\nclass Probe(unittest.TestCase):\n    def test_probe(self):\n        self.assertTrue(True)\n"
            with mock.patch.object(RUNNER, "HLOD_OFFLINE_CONTRACT_TEST_SCRIPT", script):
                script.write_text(source + "unittest.main()\n")
                self.assertTrue(RUNNER._run_hlod_offline_contract_tests()[0])
                script.write_text(source)
                self.assertFalse(RUNNER._run_hlod_offline_contract_tests()[0])

    def test_every_limit_is_mandatory_and_exact(self):
        control = manifest()
        JOB.validate_manifest(control)
        for key in JOB.REQUIRED_LIMITS:
            for invalid in (None, True, float("nan"), float("inf"), -1, 0.5, 1 << 54):
                changed = copy.deepcopy(control)
                changed["limits"][key] = invalid
                with self.subTest(key=key, invalid=invalid), self.assertRaises(ValueError):
                    JOB.validate_manifest(changed)
            changed = copy.deepcopy(control)
            del changed["limits"][key]
            with self.subTest(missing=key), self.assertRaises(ValueError):
                JOB.validate_manifest(changed)

    def test_identity_quality_and_worker_changes_refuse(self):
        for owner, key, value in (("quality", "views", 6), ("quality", "bake_rule_version", 1),
                                  ("implementation", "source_sha", "unknown"),
                                  ("implementation", "build_sha256", ""),
                                  ("limits", "bake_workers", 2), ("limits", "pending_jobs", 1),
                                  ("limits", "concurrent_jobs", 2), ("source", "count", 262145)):
            changed = manifest()
            changed[owner][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                JOB.validate_manifest(changed)
        for key, value in JOB.FIXED_METRIC.items():
            changed = manifest()
            changed["limits"][key] = value + 1
            with self.subTest(key=key), self.assertRaises(ValueError):
                JOB.validate_manifest(changed)

    def test_count_and_memory_consistency(self):
        for key, value in (("job_wall_seconds", 3601), ("max_prepared_splats", 1),
                           ("max_scratch_bytes", 512 << 20), ("process_private_bytes", 1),
                           ("max_nodes", 1 << 32)):
            changed = manifest()
            changed["limits"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                JOB.validate_manifest(changed)

    def test_scalar_ply_physical_and_sh_admission(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.ply"
            props = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"]
            header = ("ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
                      "".join("property float " + name + "\n" for name in props) + "end_header\n").encode()
            payload = header + struct.pack("<6f", 0, 0, 0, 0, 0, 0)
            path.write_bytes(payload)
            control = manifest()
            source = control["source"]
            source.update(path=str(path), bytes=len(payload), sha256=JOB.sha256(path))
            evidence = JOB.inspect_source(path, source, control["limits"])
            self.assertLess(evidence["load_reserved_bytes"], control["limits"]["max_scratch_bytes"])
            for override in ({"bytes": len(payload) + 1}, {"count": 2}, {"sh_degree": 1}):
                with self.subTest(override=override), self.assertRaises(ValueError):
                    JOB.inspect_source(path, dict(source, **override), control["limits"])
            # A physically truncated body cannot be admitted by a plausible count alone.
            path.write_bytes(payload[:-1])
            source["bytes"] = len(payload) - 1
            with self.assertRaises(ValueError):
                JOB.inspect_source(path, source, control["limits"])

    def test_world_admission_rejects_unbounded_metadata_and_import_remap(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "source.gsplatworld"
            header = bytearray(112 + 144)
            struct.pack_into("<7I", header, 0, 0x57505347, 3, 0, 1, 0, 0, 0)
            path.write_bytes(header)
            control = manifest()
            control["source"].update(path=str(path), bytes=len(header))
            JOB.inspect_source(path, control["source"], control["limits"])
            struct.pack_into("<Q", header, 96, 1000000000)
            path.write_bytes(header)
            with self.assertRaises(ValueError):
                JOB.inspect_source(path, control["source"], control["limits"])
            struct.pack_into("<Q", header, 96, 0)
            path.write_bytes(header)
            Path(str(path) + ".import").write_text("untrusted remap")
            with self.assertRaises(ValueError):
                JOB.inspect_source(path, control["source"], control["limits"])

    def test_unsupported_platform_refuses(self):
        platform = mock.Mock(wraps=os)
        platform.name = "unsupported-platform-control"
        # Replace only this module's facade; pathlib and the process os stay native.
        with mock.patch.object(JOB, "os", platform), self.assertRaisesRegex(ValueError, "requires native Windows Python"):
            JOB.NativeJob(manifest()["limits"])


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name) / "source.ply"
        self.output = Path(self.temp.name) / "last-good.gsplatworld"
        self.fresh_output = Path(self.temp.name) / "fresh.gsplatworld"
        self.candidate = Path(self.temp.name) / "new.gsplatworld"
        self.source.write_bytes(b"canonical source")
        self.output.write_bytes(b"previous valid package")
        self.candidate.write_bytes(b"strictly validated new package")

    def assert_retained(self):
        self.assertEqual(self.source.read_bytes(), b"canonical source")
        self.assertEqual(self.output.read_bytes(), b"previous valid package")

    def test_source_normalized_path_and_hardlink_refuse(self):
        with self.assertRaises(ValueError):
            JOB.refuse_alias(self.source, self.source.parent / "." / self.source.name)
        alias = self.source.parent / "hardlink.gsplatworld"
        os.link(self.source, alias)
        with self.assertRaises(ValueError):
            JOB.publish_candidate(self.candidate, alias, JOB.sha256(self.candidate), self.source)
        self.assertEqual(alias.read_bytes(), b"canonical source")
        self.assert_retained()

    def test_invalid_candidate_cancellation_and_disk_failure_preserve_destination(self):
        with self.assertRaises(ValueError):
            JOB.publish_candidate(self.candidate, self.fresh_output, "0" * 64, self.source)
        self.assert_retained()
        def cancelled():
            raise ValueError("cancelled while validating")
        with self.assertRaises(ValueError):
            JOB.publish_candidate(self.candidate, self.fresh_output, JOB.sha256(self.candidate), self.source, cancelled)
        self.assert_retained()
        with mock.patch.object(JOB.os, "link", side_effect=OSError("disk exhausted")), self.assertRaises(OSError):
            JOB.publish_candidate(self.candidate, self.fresh_output, JOB.sha256(self.candidate), self.source)
        self.assert_retained()

    def test_success_publishes_atomically_and_preserves_source(self):
        JOB.publish_candidate(self.candidate, self.fresh_output, JOB.sha256(self.candidate), self.source)
        self.assertEqual(self.fresh_output.read_bytes(), b"strictly validated new package")
        self.assertTrue(os.path.samefile(self.fresh_output, self.candidate))
        self.assert_retained()

    def test_existing_and_racing_destination_never_clobbered(self):
        with self.assertRaises(ValueError):
            JOB.require_fresh_output(self.source, self.output)
        with self.assertRaises(ValueError):
            JOB.publish_candidate(self.candidate, self.output, JOB.sha256(self.candidate), self.source)
        self.assert_retained()
        def create_racing_output():
            self.fresh_output.write_bytes(b"concurrent valid output")
        with self.assertRaises(FileExistsError):
            JOB.publish_verified_candidate(self.candidate, self.fresh_output, self.source,
                                           JOB.file_identity(self.candidate), create_racing_output)
        self.assertEqual(self.fresh_output.read_bytes(), b"concurrent valid output")
        self.assert_retained()


class PrivatePhaseTests(unittest.TestCase):
    def test_confirmed_publication_cleanup_releases_admission_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "published.gsplatworld"
            output.write_bytes(b"confirmed package")
            job = object.__new__(JOB.NativeJob)
            job.handles, job.process, job.pid = [], None, 1
            job.drain = {"status": "DRAINED"}
            job.publication_started, job.publication_uncertain = True, False
            job.marker_owned, job.poison_path = True, Path(directory) / "blocked.json"
            job.poison_path.write_text("active fixture")
            job.__exit__(None, None, None)
            self.assertFalse(job.marker_owned)
            self.assertFalse(job.poison_path.exists())
            self.assertEqual(job.drain["status"], "DRAINED")
            self.assertEqual(output.read_bytes(), b"confirmed package")

    def test_admission_cleanup_failure_retains_confirmed_output_but_fails_job_and_cli(self):
        # Exercise the supervisor and its real __exit__; native launch is replaced
        # by tiny deterministic phase receipts. This is not Job Object evidence.
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source, binary, output = (directory / name for name in ("source.ply", "engine.exe", "new.gsplatworld"))
            source.write_bytes(b"source identity fixture")
            binary.write_bytes(b"binary identity fixture")
            poison = directory / "admission-block.json"
            control = manifest()
            control["source"].update(path=str(source), bytes=source.stat().st_size, sha256=JOB.sha256(source))
            control["implementation"]["build_sha256"] = JOB.sha256(binary)
            control["output"] = str(output)

            class AdmittedFixture(JOB.NativeJob):
                def __init__(self, limits):
                    self.handles, self.process, self.pid = [], None, 1
                    self.drain = {"status": "DRAINED"}
                    self.publication_started = self.publication_uncertain = False
                    self.marker_owned, self.poison_path = True, poison
                    poison.write_text("active fixture")
                    self.lock_source = mock.Mock()
                    self.host_memory = mock.Mock(return_value={"available_physical": 1 << 40, "available_commit": 1 << 40})
                    self.peak = mock.Mock(return_value=1)

                def __enter__(self):
                    return self

            def phase(_job, name, _command, stage, _log, _record, _check):
                candidate = stage / "candidate.gsplatworld"
                if name == "admit":
                    JOB.write_json(stage / "admit.json", {"status": "CONTROL_COMPLETED", "control_sha256": JOB.control_identities(stage)})
                elif name == "engine":
                    candidate.write_bytes(b"confirmed data fixture")
                else:
                    JOB.publish_candidate(candidate, output, JOB.sha256(candidate), source)
                    JOB.write_json(stage / "verify.json", {"status": "PUBLISHED", "publication_committed": True,
                        "published_identity": JOB.file_identity(output), "candidate_identity": JOB.file_identity(candidate),
                        "candidate_sha256": JOB.sha256(candidate), "worker": {}})

            original_unlink = Path.unlink
            def fail_marker_unlink(path, *args, **kwargs):
                if path == poison:
                    raise PermissionError("injected admission marker unlink failure")
                return original_unlink(path, *args, **kwargs)

            platform = mock.Mock(wraps=os)
            platform.name = "nt"
            with mock.patch.object(JOB, "os", platform), mock.patch.object(JOB, "NativeJob", AdmittedFixture), \
                    mock.patch.object(JOB, "run_owned_phase", side_effect=phase), \
                    mock.patch.object(JOB.shutil, "disk_usage", return_value=mock.Mock(free=1 << 40)), \
                    mock.patch.object(Path, "unlink", fail_marker_unlink):
                record = JOB.run_job(control, binary, directory / "evidence")
            self.assertEqual(record["status"], "FAILED")
            self.assertEqual(record["publication"]["status"], "CONFIRMED")
            self.assertEqual(record["admission_cleanup"], "BLOCKED")
            self.assertIn("marker unlink failure", record["failure"])
            self.assertTrue(poison.exists())
            self.assertEqual(output.read_bytes(), b"confirmed data fixture")
            self.assertEqual(source.read_bytes(), b"source identity fixture")
            manifest_path = directory / "manifest.json"
            JOB.write_json(manifest_path, control)
            with mock.patch.object(JOB, "run_job", return_value=record), \
                    mock.patch.object(sys, "argv", ["offline", "--manifest", str(manifest_path), "--godot", str(binary), "--evidence", str(directory)]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(JOB.main(), 1)

    def test_source_and_candidate_locks_deny_write_and_delete_sharing(self):
        job = object.__new__(JOB.NativeJob)
        job.kernel = mock.Mock()
        job.own = mock.Mock(return_value=123)
        self.assertEqual(job.lock_source(__file__), 123)
        arguments = job.kernel.CreateFileW.call_args.args
        self.assertEqual(arguments[1:3], (0x80000000, 1))

    def test_executed_controls_must_match_admission_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for name in ("worker.gd", "project.godot", "request.json"):
                (stage / name).write_bytes(b"frozen control")
            admitted = {"control_sha256": JOB.control_identities(stage)}
            JOB.verify_control_identity(stage, admitted)
            for name in admitted["control_sha256"]:
                with self.subTest(name=name):
                    (stage / name).write_bytes(b"changed control")
                    with self.assertRaises(ValueError):
                        JOB.verify_control_identity(stage, admitted)
                    (stage / name).write_bytes(b"frozen control")
            with self.assertRaises(ValueError):
                JOB.verify_control_identity(stage, {})

    def test_private_admission_and_final_hash_control(self):
        # Tiny control files exercise the phase protocol, not an engine acceptance run.
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / "source.ply"
            properties = ["x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2"]
            source.write_bytes(("ply\nformat binary_little_endian 1.0\nelement vertex 1\n" +
                                "".join("property float " + name + "\n" for name in properties) +
                                "end_header\n").encode() + bytes(24))
            binary = stage / "fixture.exe"
            binary.write_bytes(b"identity control, never executed")
            (stage / "worker.gd").write_text("# fixture, never executed\n")
            (stage / "project.godot").write_text("# fixture, never executed\n")
            request = manifest()
            request["source"].update(path=str(source), bytes=source.stat().st_size, sha256=JOB.sha256(source))
            request["implementation"]["build_sha256"] = JOB.sha256(binary)
            request.update(original_source_path=str(source), godot_path=str(binary),
                           candidate_path=str(stage / "candidate.gsplatworld"), output=str(stage / "fresh.gsplatworld"),
                           deadline_monotonic=time.monotonic() + 60)
            request["limits"]["cancel_path"] = str(stage / "cancel")
            request_path = stage / "request.json"
            JOB.write_json(request_path, request)
            self.assertEqual(JOB.private_phase("admit", request_path), 0)
            admitted = json.loads((stage / "admit.json").read_text())
            self.assertEqual(admitted["status"], "CONTROL_COMPLETED")
            candidate = Path(request["candidate_path"])
            candidate.write_bytes(b"candidate identity fixture")
            JOB.write_json(stage / "result.json", {"status": "VALIDATED_CANDIDATE", "error": 0,
                "preparation": {"bake_rule_version": 2}, "candidate_sha256": JOB.sha256(candidate)})
            self.assertEqual(JOB.private_phase("verify", request_path), 0)
            verified = json.loads((stage / "verify.json").read_text())
            self.assertEqual(verified["candidate_identity"], JOB.file_identity(candidate))
            self.assertEqual(verified["status"], "PUBLISHED")
            # Remove only the test's publication link before the next independent negative control.
            Path(request["output"]).unlink()
            candidate.write_bytes(b"changed after engine receipt")
            self.assertEqual(JOB.private_phase("verify", request_path), 1)
            self.assertEqual(json.loads((stage / "verify.json").read_text())["status"], "FAILED")
            self.assertTrue(candidate.exists())

    def test_undrained_owned_process_leaves_persistent_admission_block(self):
        with tempfile.TemporaryDirectory() as directory:
            job = object.__new__(JOB.NativeJob)
            job.kernel = mock.Mock()
            job.process, job.job, job.attached, job.pid = 7, 8, True, 99
            job.limits = {"cancel_and_drain_seconds": 1}
            job.poison_path = Path(directory) / "blocked.json"
            job.poison_path.write_text('{"status":"ACTIVE_OR_UNDRAINED"}')
            job.poll = mock.Mock(return_value=None)
            with mock.patch.object(JOB.time, "monotonic", side_effect=[0, 2, 2]), self.assertRaises(RuntimeError):
                job.stop()
            self.assertEqual(json.loads(job.poison_path.read_text())["status"], "FAILED")
            self.assertEqual(job.drain["status"], "FAILED")
            job.kernel.TerminateJobObject.assert_called_once_with(8, 1)
            job.kernel.TerminateProcess.assert_not_called()

    def test_commit_without_acknowledgement_keeps_admission_block_after_drain(self):
        with tempfile.TemporaryDirectory() as directory:
            job = object.__new__(JOB.NativeJob)
            job.kernel = mock.Mock()
            job.handles, job.process, job.pid = [], None, 99
            job.drain = {"status": "DRAINED"}
            job.publication_uncertain, job.marker_owned = True, True
            job.poison_path = Path(directory) / "blocked.json"
            job.poison_path.write_text('{"status":"ACTIVE_OR_UNDRAINED"}')
            job.stop = mock.Mock()
            job.__exit__(None, None, None)
            self.assertTrue(job.poison_path.exists())
            self.assertEqual(json.loads(job.poison_path.read_text())["status"], "UNCONFIRMED_PUBLICATION")
            self.assertTrue(job.marker_owned)


if __name__ == "__main__":
    unittest.main()
