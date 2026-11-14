#!/usr/bin/env python3
"""D3-6 runner plumbing, Python only.

Native observation samples are read unchanged from the documented D3-5/BV-07
captures. New envelope/settings records below are constructed policy inputs, not
claimed engine output. The binary corruption corpus is deliberately constructed
to test offset isolation; it is NOT a fixture asserting a real V4 writer output.
"""

from __future__ import annotations

import copy
import contextlib
import gzip
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = importlib.util.spec_from_file_location("shared_layers_t0", HERE / "shared_layers_t0.py")
shared = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = shared
spec.loader.exec_module(shared)


class RunnerPlumbing(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)

    def captured(self, name):
        return json.loads((HERE / "fixtures" / name).read_text(encoding="utf-8"))

    def test_binary_identity_checks_immutable_commit_and_actual_bytes(self):
        binary = self.tmp / "godot.linuxbsd.editor.x86_64"
        binary.write_bytes(b"identity test bytes, not an engine executable")
        identity = shared.rp.binary_identity(binary)
        info = {"sha256": identity["sha256"], "version": "4.5.rc.custom_build." + shared.PRE_D3[:9], "provenance": {
            "commit": shared.PRE_D3, "binary": binary.name, "sha256": identity["sha256"]}}
        path = self.tmp / "build_info.json"
        shared.write_json(path, info)
        self.assertEqual(shared.verified_binary(binary, path, shared.PRE_D3)["sha256"], identity["sha256"])
        for where, key in ((info, "sha256"), (info["provenance"], "sha256"), (info["provenance"], "commit")):
            with self.subTest(key=key):
                original = where[key]
                where[key] = "wrong"
                shared.write_json(path, info)
                with self.assertRaisesRegex(ValueError, "identity mismatch"):
                    shared.verified_binary(binary, path, shared.PRE_D3)
                where[key] = original

    def test_process_settings_never_borrows_the_requested_settings(self):
        self.assertEqual(shared.process_settings({}), {})
        policy = {"shared_layers_settings": {"camera_poses": [{"kind": "move", "pose": [1, 2, 3]}]}}
        result = shared.process_settings(policy)
        self.assertEqual(result["camera_path_sha256"], shared.object_sha(policy["shared_layers_settings"]["camera_poses"]))
        result["camera_poses"].clear()
        self.assertTrue(policy["shared_layers_settings"]["camera_poses"])

    def test_requested_and_process_settings_use_identical_float32_hash_canonicalization(self):
        # A constructed settings policy record, not a claimed engine capture.
        path = {"segments": [{"kind": "stop", "pose": [0.1, 2.2, 3.3, 45.1, -0.2]}]}
        prepared = {"codec": {"sh_bands": 3, "pos_step_m": 0.001}}
        plan = shared.rp.path_plan(self.tmp / "world.gsplatworld", path, 2)
        expected = shared.requested_settings(plan, prepared)
        process_readback = copy.deepcopy(expected)
        process_readback.pop("camera_path_sha256")
        process_readback["camera_poses"] = path["segments"]
        actual = shared.process_settings({"shared_layers_settings": process_readback})
        self.assertEqual(actual, expected)

    def test_real_trace_camera_poses_verify_at_engine_float32_precision(self):
        manifest = self.captured("bv07/tsmoke.manifest.json")
        path = shared.rp.probe_path.resolve_path(manifest)
        trace_path = self.tmp / "trace.bin"
        trace_path.write_bytes(gzip.decompress((HERE / "fixtures/bv07/tsmoke_path.trace.bin.gz").read_bytes()))
        trace = shared.ps.read_trace(trace_path)
        observed = shared.camera_application(trace, path)
        self.assertIs(observed["matches"], True, observed)
        self.assertEqual(observed["rows_checked"], len(trace.rows))
        mutated = copy.deepcopy(path)
        mutated["segments"][0]["pose"][0] += 1
        self.assertIs(shared.camera_application(trace, mutated)["matches"], False)

    def test_arm_failure_is_recorded_without_raising_into_later_arms(self):
        with mock.patch.object(shared, "run_arm", side_effect=OSError("test missing process")):
            for name in ("chunk", "hlod", "base", "candidate"):
                result = shared.capture_arm(self.tmp / "binary", self.tmp / "world", "hlod", {}, {}, {}, {},
                                            self.tmp / name, shared.PRE_D3)
                self.assertEqual(result["execution"], "NOT_RUN")
                self.assertIs(result["observed"]["final_marker"], False)
                self.assertTrue((self.tmp / name / "arm.json").is_file())

    def test_native_hlod_fields_are_retained_and_unexported_safety_fields_stay_missing(self):
        capture = self.captured("d3_5/d3_5_hlod_test_results.json")
        native = capture["completed"]
        result = {"final_render_stats": native, "payload_telemetry": native["payload_telemetry"],
                  "route": "hlod", "source_generation": capture["source_generation"]}
        observed = shared.observations(result, 0)
        self.assertEqual(observed["runtime"]["hlod_stream_paged"], native["hlod_stream_paged"])
        self.assertEqual(observed["queues"]["ready"]["bytes"], native["payload_telemetry"]["ready_bytes"])
        for metric in (observed["queues"]["ready"]["byte_bound"], observed["queues"]["retained"]["jobs"],
                       observed["failures"]["stale_generation_publications"], observed["saturation"]["admission_waits"]):
            self.assertIs(metric["measured"], False)
            self.assertTrue(metric["reason"])

    def test_no_result_cannot_manufacture_success_or_zero_failures(self):
        observed = shared.observations({}, 1)
        self.assertIs(observed["final_marker"], False)
        self.assertEqual(observed["exit_code"], 1)
        self.assertEqual(observed["settings"], {})
        for metric in observed["failures"].values():
            self.assertIs(metric["measured"], False)

    def test_cold_preparation_drops_each_arm_separately_then_measures_residency(self):
        world = self.tmp / "world.gsplatworld"
        with mock.patch.object(shared.wc, "drop_from_page_cache", return_value=55) as drop, mock.patch.object(
                shared.wc, "residency", return_value={"cold": True}) as resident:
            for _arm in ("chunk", "hlod", "base", "candidate"):
                result = shared.cold_prepare(world)
                self.assertIs(result["independently_prepared"], True)
                self.assertEqual(result["repeat_label"], "repeat")
            self.assertEqual(drop.call_count, 4)
            self.assertEqual(resident.call_count, 4)
            self.assertEqual(drop.call_args.args[0], [world, world.with_suffix(".manifest.json")])

    def test_missing_cold_measurement_is_not_cold(self):
        with mock.patch.object(shared.wc, "drop_from_page_cache", side_effect=shared.wc.CacheError("unavailable")):
            result = shared.cold_prepare(self.tmp / "world.gsplatworld")
        self.assertIs(result["independently_prepared"], False)
        self.assertEqual(result["label"], "NOT_RUN")

    def test_mutation_changes_payload_only_and_never_the_clean_input(self):
        # Constructed mutation corpus, not a valid V4 writer fixture. The
        # function must honor indexed extents rather than flip a header byte.
        corpus = bytearray(4096 + 65536 + 4096)
        struct.pack_into("<II", corpus, 0, 0x57505347, 4)
        struct.pack_into("<I", corpus, 40, 1)
        payload = 4096 + 65536
        struct.pack_into("<3Q", corpus, 4096 + 64 + 128, payload, 256, 4096)
        corpus[payload:payload + 256] = b"x" * 256
        source = self.tmp / "clean" / "world.gsplatworld"
        source.parent.mkdir()
        source.write_bytes(corpus)
        source.with_suffix(".manifest.json").write_text("{}", encoding="utf-8")
        target = self.tmp / "control" / "world.gsplatworld"
        receipt = shared.corrupt_copy(source, target)
        self.assertEqual(source.read_bytes(), corpus)
        mutated = target.read_bytes()
        changed = [i for i, (before, after) in enumerate(zip(corpus, mutated)) if before != after]
        self.assertEqual(changed, [payload + 255])
        self.assertEqual(receipt["mutations"], [{"node": 0, "offset": payload + 255}])
        self.assertNotEqual(receipt["source_sha256"], receipt["corrupt_sha256"])

    def test_corruption_control_refuses_non_v4_instead_of_reporting_forced(self):
        source = self.tmp / "world.gsplatworld"
        source.write_bytes(b"bad header")
        source.with_suffix(".manifest.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "requires a V4"):
            shared.corrupt_copy(source, self.tmp / "control" / "world.gsplatworld")

    def test_pre_d3_common_behavior_does_not_fabricate_decoded_bytes(self):
        native = self.captured("bv07/tsmoke_path.result.json")
        behavior = shared.payload_behavior(native, None)
        self.assertIs(behavior["decoded_bytes"]["measured"], False)
        self.assertEqual(behavior["drawn_counts"][0]["drawn_splats"], native["checkpoints"][0]["drawn_splats"])
        self.assertIs(behavior["trace_drawn_splats_max"]["measured"], False)

    def test_prepare_reuses_d2_path_and_has_no_scan_parameter(self):
        import dispatch_lane as lane
        calls = []
        def logged(command, *_args):
            calls.append(command)
            return 0
        with mock.patch.object(lane, "_run_logged", side_effect=logged), mock.patch.object(lane, "_entry_point", return_value=HERE / "generate_world.py"), mock.patch.object(
                lane, "_auto_build", return_value={"w13": "producer report"}) as build:
            result = lane.prepare_auto_tree_t0(HERE, self.tmp / "binary", self.tmp / "scratch", self.tmp / "out")
        self.assertEqual(calls[0][calls[0].index("--tier") + 1], "T0")
        self.assertEqual(calls[0][calls[0].index("--encoding") + 1], "plain_v3_gsq")
        self.assertEqual(build.call_args.args[2], result["plain"])
        self.assertEqual(build.call_args.args[3], result["tree"])
        self.assertEqual(build.call_args.args[6], lane.auto_tree_job("T0"))

    def test_dispatch_command_calls_the_runner_with_both_binaries_and_control(self):
        import dispatch_lane as lane
        with mock.patch.object(shared, "run", return_value=0) as run:
            rc = lane.main(["shared-layers-t0", "--repo", str(self.tmp), "--godot-binary", str(self.tmp / "candidate"),
                            "--base-binary", str(self.tmp / "base"), "--base-build-info", str(self.tmp / "base-info.json"),
                            "--cache-root", str(self.tmp / "cache"), "--out", str(self.tmp / "out"),
                            "--run-id", "unit-orchestration", "--control", "skip_compare"])
        self.assertEqual(rc, 0)
        self.assertEqual(run.call_args.args[-2:], ("unit-orchestration", "skip_compare"))
        self.assertEqual(run.call_args.args[1:3], (self.tmp / "candidate", self.tmp / "base"))

    def test_four_clean_arms_finish_before_corrupt_control_and_clean_v4_is_reused(self):
        # The arm records are constructed controller-policy preconditions, not
        # fabricated native probe output. Native-format parsers are tested with
        # real captures elsewhere in this file and in phase-1 comparison tests.
        import dispatch_lane as lane
        manifest = self.captured("bv07/tsmoke.manifest.json")
        plain, tree = self.tmp / "plain" / "world.gsplatworld", self.tmp / "tree" / "world.gsplatworld"
        calls = []
        def capture(binary, world, route, prepared, path, truth, source_manifest, out, expected_commit):
            calls.append((out.name, binary, world))
            return {"execution": "RAN", "source": {"loaded_file_sha256": "controller policy digest"},
                    "observed": {"exit_code": 0, "final_marker": True, "settings": {}}, "payload_behavior": {},
                    "trace": {"file": str(out / "trace.bin")}, "cache": {}, "native": {"process_failure": None},
                    "probe": {"payload_telemetry": None}}
        def corrupt(source, target):
            self.assertEqual([call[0] for call in calls], ["chunk", "hlod", "base", "candidate"])
            self.assertEqual(source, tree)
            return {"status": "FORCED"}
        with mock.patch.object(shared.subprocess, "run", return_value=mock.Mock(stdout="a" * 40 + "\n")), mock.patch.object(
                shared, "verified_binary", side_effect=lambda binary, info, expected: {"commit": expected, "sha256": "b" * 64}), mock.patch.object(
                shared.wc, "cache_lock", return_value=contextlib.nullcontext()), mock.patch.object(
                lane, "prepare_auto_tree_t0", return_value={"plain": plain, "tree": tree, "report": {}}), mock.patch.object(
                shared, "prepared_receipt", return_value={}), mock.patch.object(shared, "read_json", return_value=manifest), mock.patch.object(
                shared, "capture_arm", side_effect=capture), mock.patch.object(shared, "corrupt_copy", side_effect=corrupt):
            rc = shared.run(self.tmp, self.tmp / "candidate", self.tmp / "base", self.tmp / "base-info.json",
                            self.tmp / "cache", self.tmp / "out", "unit-orchestration", "corrupt")
        self.assertEqual(rc, 0)
        self.assertEqual([call[0] for call in calls], ["chunk", "hlod", "base", "candidate", "corrupt"])
        self.assertEqual([call[2] for call in calls[:4]], [plain, tree, tree, tree])
        self.assertEqual([call[1].name for call in calls[:4]], ["candidate", "candidate", "base", "candidate"])

    def test_probe_retains_measurement_exports_and_separate_hlod_convergence(self):
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn('"device_vram_ledger"]', gd)
        self.assertIn('_result["shared_layers_settings"]', gd)
        self.assertIn('_result["loaded_file_sha256"] = FileAccess.get_sha256', gd)
        self.assertIn('int(_last_stats["hlod_stream_desired_nodes"]) == int(_last_stats["hlod_stream_resident_nodes"])', gd)
        self.assertIn('int(ss["needed_not_resident_chunks"]) != 0', gd)


if __name__ == "__main__":
    unittest.main()
