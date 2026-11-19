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
import re
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
    def test_sync_cuts_absent_counter_is_not_measured_never_zero(self):
        for result in ({}, {"overlap_sync_cuts": {"events": []}}):
            measured = shared.sync_cut_measurements(result)
            self.assertEqual(measured["status"], "not_measured")
            self.assertNotEqual(measured.get("count"), 0)

    def test_sync_cuts_report_native_frames_windows_and_cpu_wait(self):
        # Constructed policy inputs, not a captured engine-format fixture.
        source = {"count": 2, "events": [
            {"count": 1, "frame_serial": 42, "cpu_wall_ms": 3.5, "window": "teleport", "segment": "S12", "pass_index": 0},
            {"count": 2, "frame_serial": 81, "cpu_wall_ms": 1.25, "window": "moving", "segment": "S3", "pass_index": 1}]}
        measured = shared.sync_cut_measurements({"overlap_sync_cuts": source})
        self.assertEqual(measured["count"], 2)
        self.assertEqual(measured["frames"], [42, 81])
        self.assertEqual(measured["windows"], ["teleport", "moving"])
        self.assertEqual(measured["cpu_wall_ms_per_cut"], [3.5, 1.25])
        self.assertEqual(shared.sync_cut_measurements({"overlap_sync_cuts": {"count": 0, "events": []}})["count"], 0)
        source["events"].pop()
        self.assertEqual(shared.sync_cut_measurements({"overlap_sync_cuts": source})["status"], "not_measured")

    def test_sync_cuts_production_wiring_is_present(self):
        # Static wiring proof only; the strict host cases exercise policy behavior.
        root = HERE.parents[2]
        tile = (root / "modules/gaussian_splatting/renderer/tile_renderer.cpp").read_text(encoding="utf-8")
        self.assertTrue(re.search(r"const auto measurement = GaussianSplatting::overlap_measurement_decision\(", tile),
                        "chunk renderer must call the host-tested measurement decision")
        self.assertIn("const bool overlap_sync_measurement = measurement.sync;", tile)
        self.assertIn("overlap_cut_sample ? false : overlap_sync_measurement", tile)
        self.assertIn('"overlap_sync_cuts": sync_cut_measurements(result)',
                      (HERE / "shared_layers_t0.py").read_text(encoding="utf-8"))
        probe = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn("_sample_sync_cuts(stats)", probe)
        self.assertIn('_result["overlap_sync_cuts"] =', probe)
        self.assertIn('stats.get("overlap_sync_cut_transform")', probe)
        self.assertIn('native_transform.is_equal_approx(sample["transform"])', probe)

    def test_sync_cuts_native_metrics_retains_the_report(self):
        source = {"overlap_sync_cuts": {"count": 0, "events": []}}
        metrics = shared.native_metrics(source, {}, None, "chunk", {})
        self.assertEqual(metrics["overlap_sync_cuts"]["count"], 0)
        source.pop("overlap_sync_cuts")
        self.assertEqual(shared.native_metrics(source, {}, None, "chunk", {})["overlap_sync_cuts"]["status"], "not_measured")

    def test_rss_timeline_and_peak_location_reach_the_arm_measurements(self):
        # #1466. Sampler rows through the producer-side summary; the trace rows are constructed.
        rss = {"peak_rss_bytes": shared.ps.metric(4 << 30, unit="bytes"),
               **shared.ps.rss_timeline([(0.2, 1 << 30, 1 << 30), (0.4, 2 << 30, 4 << 30), (0.6, 2 << 30, 4 << 30)], 1000.0, 0.2)}
        fields = ["row", "engine_frame", "t_usec", "phase", "pass_index", "segment", "state", "drawn_splats"]
        trace = shared.ps.Trace(fields, "q" * len(fields), [(0, 7, 2_250_000, 0, 0, 1, 0, 0), (1, 8, 2_500_000, 0, 0, 1, 0, 0)], 0)
        result = {"start_unix_s": 1000.0, "start_usec": 2_000_000, "apply_usec": 2_100_000}
        metrics = shared.native_metrics(result, {"rss": rss}, trace, "chunk", {})
        self.assertEqual(metrics["os_rss_timeline"]["points"], [[0.2, 1 << 30], [0.4, 2 << 30], [0.6, 2 << 30]])
        location = metrics["os_rss_peak_location"]
        self.assertEqual(location["status"], shared.ps.OK)
        self.assertEqual((location["value"]["position"], location["value"]["rows"]["first"]["row"],
                          location["value"]["rows"]["last"]["row"]), ("within_trace", 0, 1))
        self.assertEqual(location["value"]["peak_bytes"], 4 << 30)
        # No sampler block, no probe clock, no trace: each is not measured with its reason, never a row 0.
        absent = shared.native_metrics({}, {}, None, "chunk", {})
        self.assertIs(absent["os_rss_timeline"]["measured"], False)
        self.assertEqual(absent["os_rss_peak_location"]["status"], shared.ps.MISSING)
        no_clock = shared.native_metrics({"start_usec": 2_000_000}, {"rss": rss}, trace, "chunk", {})
        self.assertEqual(no_clock["os_rss_peak_location"]["status"], shared.ps.MISSING)
        self.assertIn("clock", no_clock["os_rss_peak_location"]["reason"])
        self.assertEqual(shared.native_metrics(result, {"rss": rss}, None, "chunk", {})["os_rss_peak_location"]["status"],
                         shared.ps.MISSING)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)

    def captured(self, name):
        return json.loads((HERE / "fixtures" / name).read_text(encoding="utf-8"))

    def settle_capture_1454(self, name):
        # Selected native result.json fields from run43717101341, arm paths
        # chunk/, migration/base/, migration/candidate/. Read with python -I;
        # samples within forward_progress are omitted. Tests never read the
        # evidence directory. Verdict/cache additions below are policy inputs,
        # not fields claimed to have existed in these producer captures.
        checkpoints = {
            "chunk": {"checkpoint": "C0", "segment": "S0", "pass_index": 0,
                      "drawn_splats": 598694, "hlod_drawn_nodes": None,
                      "rendered_splat_count_source_frame": 1342,
                      "render_frame_counter": 1344, "pose_first_frame": 0,
                      "settle": {"deadline_s": 90.0, "frames": 327, "pose_error_m": 0.0,
                                 "settle_usec": 2162194, "settled": True,
                                 "forward_progress": {"deadline_s": 90.0,
                                                      "deadline_missed": False,
                                                      "elapsed_usec": 2171690}}},
            "base": {"checkpoint": "C0", "segment": "S0", "pass_index": 0,
                     "drawn_splats": 603152, "hlod_drawn_nodes": 221,
                     "rendered_splat_count_source_frame": -1,
                     "render_frame_counter": -1, "pose_first_frame": -1,
                     "settle": {"deadline_s": 90.0, "frames": 13644, "pose_error_m": 0.0,
                                "settle_usec": -1, "settled": False,
                                "reason": "settle_deadline_missed",
                                "forward_progress": {"deadline_s": 90.0,
                                                     "deadline_missed": True,
                                                     "elapsed_usec": 90003056}}},
            "candidate": {"checkpoint": "C0", "segment": "S0", "pass_index": 0,
                          "drawn_splats": 603152, "hlod_drawn_nodes": 221,
                          "rendered_splat_count_source_frame": 2410,
                          "render_frame_counter": 2413, "pose_first_frame": 0,
                          "settle": {"deadline_s": 90.0, "frames": 1702, "pose_error_m": 0.0,
                                     "settle_usec": 8001288, "settled": True,
                                     "forward_progress": {"deadline_s": 90.0,
                                                          "deadline_missed": False,
                                                          "elapsed_usec": 8016402}}},
        }
        route = "chunk" if name == "chunk" else "hlod"
        return {"ok": True, "reason": "plan completed", "route": route,
                "checkpoints": [copy.deepcopy(checkpoints[name])]}

    def test_pre_d3_pin_requires_immutable_provenance_base(self):
        # Policy pin: f2f19bcb580cc131615b3fa262a11d99e6abc70c plus only #1453.
        # Historical captured binary identities must not be rewritten.
        self.assertEqual(shared.PRE_D3, "5a438363cd6939bca1041d738e2bfa10f2b024f2")

    def test_payload_behavior_keeps_settle_and_count_provenance_receipts(self):
        for name in ("chunk", "base", "candidate"):
            with self.subTest(arm=name):
                result = self.settle_capture_1454(name)
                # Constructed readback policy isolates settle/provenance from
                # unavailable-cache-state handling; the raw capture stays intact.
                result["static_sort_cache"] = {"enabled": False,
                                               "applicable": result["route"] == "chunk"}
                native_cp = result["checkpoints"][0]
                receipt = shared.payload_behavior(result, None)["drawn_counts"][0]
                for field in ("settle", "rendered_splat_count_source_frame",
                              "render_frame_counter", "pose_first_frame"):
                    self.assertEqual(receipt.get(field), native_cp[field], field)
                self.assertEqual(receipt.get("evidence_verdict"),
                                 "checkpoint_settle_deadline_missed" if name == "base" else "VALID")
                # The retained receipt must not alias the native producer record.
                receipt["settle"]["settled"] = "changed"
                self.assertIs(native_cp["settle"]["settled"], name != "base")

    def test_residency_boundaries_use_payload_behavior_and_survive_migration(self):
        # Construct the new boundary envelope from existing captured snapshots;
        # these are not claimed to be captured pass endpoints.
        native = self.captured("d3_6/run43602195987_migration.json")["arms"]
        content = self.captured("d3_6/run43629131401_content.json")["migration"]["arms"]
        for name in ("base", "candidate"):
            with self.subTest(arm=name):
                snapshot = copy.deepcopy(native[name])
                snapshot["payload_telemetry"] = {"content_digests": copy.deepcopy(
                    content[name]["payload_behavior"]["content_digests"])}
                result = {"residency_pass_endpoints": [{"pass_index": 0,
                          "start": copy.deepcopy(snapshot), "end": copy.deepcopy(snapshot)}]}
                behavior = shared.payload_behavior(result, None)
                entry = behavior["residency_pass_endpoints"][0]
                self.assertEqual(entry["pass_index"], 0)
                for edge in ("start", "end"):
                    self.assertEqual(entry[edge], shared.payload_behavior(snapshot, None))
                arm = {"execution": "RAN", "observed": {"exit_code": 0, "final_marker": True, "settings": {}},
                       "source": {"loaded_file_sha256": "a" * 64}, "trace": {"file": "trace.bin"},
                       "payload_behavior": behavior, "cache": {}, "probe": {}}
                migration = shared.migration_arm(arm, {"commit": "c" * 40, "sha256": "d" * 64})
                self.assertEqual(migration["payload_behavior"]["residency_pass_endpoints"], [entry])
                entry["end"]["native_payload"]["hlod_stream_evicted_total"] = -1
                self.assertEqual(result["residency_pass_endpoints"][0]["end"], snapshot)
        self.assertNotIn("residency_pass_endpoints", shared.payload_behavior({}, None))

    def test_residency_probe_snapshots_bracket_passes_without_advancing_frames(self):
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        snapshot = gd.split("func _residency_snapshot()", 1)[1].split("\n\n\nfunc ", 1)[0]
        for forbidden in ("await ", "_frame(", "_pump", "_last_stats ="):
            self.assertNotIn(forbidden, snapshot)
        self.assertIn("renderer.get_render_stats()", snapshot)
        self.assertIn('payload.get("content_digests") is Dictionary', snapshot)
        path = gd.split("func _run_path()", 1)[1].split("\n\n\nfunc ", 1)[0]
        self.assertIn('_result["residency_pass_endpoints"] = residency_pass_endpoints', path)
        start = path.index('"start": _residency_snapshot()')
        self.assertLess(path.index("for pass_index in range(passes):"), start)
        self.assertLess(start, path.index("for si in range(segs.size()):"))
        self.assertLess(path.index("seg_log.append(entry)"), path.index('residency_entry["end"] = _residency_snapshot()'))

    def test_observations_deadline_miss_survives_successful_process_completion(self):
        result = self.settle_capture_1454("base")
        observed = shared.observations(result, 0)
        self.assertIs(observed["final_marker"], True)
        self.assertEqual(observed["exit_code"], 0)
        evidence = observed.get("checkpoint_evidence", {})
        self.assertEqual(evidence.get("verdict"), "UNUSABLE")
        self.assertIn("checkpoint_settle_deadline_missed", evidence.get("reasons", []))
        self.assertIs(result["ok"], True)
        self.assertEqual(result["reason"], "plan completed")

    def test_run_arm_propagates_deadline_miss_without_discarding_capture(self):
        path = shared.rp.probe_path.resolve_path(self.captured("bv07/tsmoke.manifest.json"))
        prepared = {key: {} for key in ("input_file_sha256", "generator_identity",
                                        "tree_file_sha256", "tree_identity")}
        prepared["codec"] = {"sh_bands": 3}
        for route in ("chunk", "hlod"):
            with self.subTest(route=route):
                result = self.settle_capture_1454("base")
                # Route mutation deliberately tests this module's receipt policy;
                # this is not claimed to be a second captured engine execution.
                result["route"] = route
                output = self.tmp / route
                with mock.patch.object(shared.rp, "trace_integrity", return_value=(None, {})):
                    arm = shared.run_arm(self.tmp / "binary", self.tmp / "world", route,
                        prepared, path, {}, {}, output, prepare_cold=lambda world: {},
                        process=lambda *args: {"result": result, "trace": str(output / "trace.bin"),
                                               "exit_code": 0, "timed_out": False})
                self.assertEqual(arm["execution"], "RAN")
                self.assertIs(arm["observed"]["final_marker"], True)
                evidence = arm["observed"].get("checkpoint_evidence", {})
                self.assertEqual(evidence.get("verdict"), "UNUSABLE")
                self.assertIn("checkpoint_settle_deadline_missed", evidence.get("reasons", []))
                cp = arm["payload_behavior"]["drawn_counts"][0]
                self.assertEqual(cp.get("evidence_verdict"), "checkpoint_settle_deadline_missed")
                self.assertEqual(cp.get("settle"), result["checkpoints"][0]["settle"])
                persisted = json.loads((output / "arm.json").read_text(encoding="utf-8"))
                self.assertEqual(persisted["observed"].get("checkpoint_evidence"), evidence)
                self.assertEqual(persisted["payload_behavior"]["drawn_counts"][0], cp)

    def test_migration_arm_keeps_checkpoint_evidence_and_actual_cache_receipt(self):
        # Constructed normalized receipt contract; native settle/count fields
        # come unchanged from the selected producer capture above.
        result = self.settle_capture_1454("base")
        checkpoint = {**result["checkpoints"][0],
                      "evidence_verdict": "checkpoint_settle_deadline_missed"}
        evidence = {"verdict": "UNUSABLE", "reasons": ["checkpoint_settle_deadline_missed"],
                    "checkpoints": [checkpoint]}
        cache = {"enabled": True, "applicable": False}
        arm = {"execution": "RAN", "observed": {"exit_code": 0, "final_marker": True,
               "settings": {}, "checkpoint_evidence": evidence, "static_sort_cache": cache},
               "trace": {"file": "trace.bin"}, "source": {"loaded_file_sha256": "0" * 64},
               "payload_behavior": {"drawn_counts": [checkpoint]}, "cache": {}, "probe": {}}
        receipt = shared.migration_arm(arm, {"commit": "c" * 40, "sha256": "d" * 64})
        self.assertIs(receipt["final_marker"], True)
        self.assertEqual(receipt.get("checkpoint_evidence"), evidence)
        self.assertEqual(receipt.get("static_sort_cache"), cache)
        self.assertEqual(receipt["payload_behavior"]["drawn_counts"][0], checkpoint)

    def test_static_cache_receipts_distinguish_chunk_exclusion_and_hlod_bypass(self):
        # New probe readback contract, constructed deliberately. enabled=None
        # means unavailable getter; zero hits never substitutes for that state.
        for route in ("chunk", "hlod"):
            for enabled in (True, False, None):
                with self.subTest(route=route, enabled=enabled):
                    result = self.settle_capture_1454("chunk" if route == "chunk" else "candidate")
                    cache = {"enabled": enabled, "applicable": route == "chunk"}
                    result["static_sort_cache"] = copy.deepcopy(cache)
                    result["final_render_stats"] = {"sort_cache_hits": 0}
                    reason = ("static_sort_cache_unsupported" if enabled is True else
                              "static_sort_cache_state_unavailable" if enabled is None else None)
                    verdict = reason if route == "chunk" and reason else "VALID"
                    observed = shared.observations(result, 0)
                    self.assertEqual(observed.get("static_sort_cache"), cache)
                    self.assertIs(observed["final_marker"], True)
                    evidence = observed.get("checkpoint_evidence", {})
                    self.assertEqual(evidence.get("verdict"), "VALID" if verdict == "VALID" else "UNUSABLE")
                    if verdict != "VALID":
                        self.assertIn(verdict, evidence.get("reasons", []))
                    cp = shared.payload_behavior(result, None)["drawn_counts"][0]
                    self.assertEqual(cp.get("static_sort_cache"), cache)
                    self.assertEqual(cp.get("evidence_verdict"), verdict)

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
        self.assertEqual(result["camera_path_sha256"], shared.camera_path_sha256(policy["shared_layers_settings"]["camera_poses"]))
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

    def test_real_run_camera_signed_zero_is_one_identity_and_changed_pose_is_not(self):
        # Exact table-row extracts from run43602195987, not an invented producer.
        capture = self.captured("d3_6/run43602195987_camera.json")
        requested, process = capture["requested_row"], capture["process_row"]
        self.assertNotEqual(shared.object_sha(requested), shared.object_sha(process))
        self.assertEqual(shared.camera_path_sha256(requested), shared.camera_path_sha256(process))
        self.assertNotEqual(capture["requested_original_path_sha256"], capture["process_original_path_sha256"])
        self.assertIs(capture["camera_application"]["matches"], True)
        changed = copy.deepcopy(process)
        changed[1] += 1
        self.assertNotEqual(shared.camera_path_sha256(requested), shared.camera_path_sha256(changed))
        # Same f32 zero even when rounding creates it; no coordinate tolerance.
        self.assertEqual(shared.camera_path_sha256([-1e-50]), shared.camera_path_sha256([0.0]))

    def test_process_camera_digest_cannot_be_replaced_with_a_requested_digest(self):
        row = self.captured("d3_6/run43602195987_camera.json")["process_row"]
        result = {"shared_layers_settings": {"camera_poses": [row], "camera_path_sha256": "deliberately wrong"}}
        self.assertEqual(shared.process_settings(result)["camera_path_sha256"], "deliberately wrong")
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn('"camera_path_sha256": _camera_path_sha256(_plan.get("path", {}).get("segments"))', gd)
        self.assertIn('JSON.stringify(_camera_hash_value(segments)).sha256_text()', gd)

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

    def test_queue_mapping_preserves_native_owner_and_absent_independent_bounds(self):
        # Constructed interpreter-policy input, not a claimed engine capture.
        # Native C++ tests prove these fields are exported from their owners.
        queues = {name: {"owner": "hlod_shared_reservation_ledger", "bytes": 13, "peak_bytes": 17,
                         "byte_bound": 100, "jobs": 1, "peak_jobs": 2, "job_bound": None,
                         "job_bound_reason": "shared bytes; no independent job bound"}
                  for name in ("retained", "ready")}
        result = {"route": "hlod", "payload_telemetry": {"queue_observations": queues,
                  "stale_generation_publications": 0, "digest_verification_failures": 0,
                  "admission_waits": 0, "upload_hold_frames": 0},
                  "shared_layers_payload_peaks": {"upload_hold_frames": 4, "stale_generation_publications": 1}}
        observed = shared.observations(result, 0)
        self.assertEqual(observed["queues"], queues)
        self.assertIsNone(observed["queues"]["ready"]["job_bound"])
        self.assertEqual(observed["failures"]["stale_generation_publications"], 1)
        self.assertEqual(observed["saturation"]["upload_hold_frames"], 4)
        result["payload_telemetry"]["stale_generation_publications"] = 3
        self.assertEqual(shared.observations(result, 0)["failures"]["stale_generation_publications"], 3)
        observed["queues"]["ready"]["bytes"] = 99
        self.assertEqual(queues["ready"]["bytes"], 13)

    def test_chunk_hlod_only_observations_are_scoped_as_inapplicable_never_zero(self):
        observed = shared.observations({"route": "chunk"}, 0)
        for key in ("nonresident_drawn_nodes", "hlod_stream_failed_total", "hlod_stream_late_dropped_total",
                    "hlod_stream_failed_permanent_total", "hlod_stream_failed_retry_later_total",
                    "hlod_stream_retry_later_waiting_nodes"):
            marker = observed["failures"][key]
            self.assertIs(marker["measured"], False)
            self.assertIs(marker["applicable"], False)
            self.assertEqual(marker["scope"], "chunk")
            self.assertTrue(marker["reason"])
        self.assertNotIn("applicable", observed["failures"]["stale_generation_publications"])

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

    def test_content_digest_envelope_preserves_native_map_without_filling_absence(self):
        # Constructed interpreter-contract input; this is not a producer capture.
        digests = {"0": "a" * 64, "9": "b" * 64}
        native = {"payload_telemetry": {"content_digests": digests}}
        behavior = shared.payload_behavior(native, None)
        self.assertEqual(behavior.get("content_digests"), digests)
        behavior["content_digests"]["0"] = "changed"
        self.assertEqual(digests["0"], "a" * 64)
        self.assertIsNone(shared.payload_behavior({}, None).get("content_digests"))
        metrics = shared.native_metrics(native, {}, None, "hlod", {})
        self.assertEqual(metrics.get("content_digests"), digests)

    def test_preparation_reference_validation_uses_page_count_not_alignment(self):
        # Deliberately malformed partial-file corpus, not a producer fixture or
        # evidence of successful extraction from a real GSQ world. Its declared
        # digest set disagrees with its table. The parser must reach that error
        # using page_count, rather than mistaking the adjacent alignment field
        # for a count. Successful producer-format extraction needs runner capture.
        plain = self.tmp / "plain.gsplatworld"
        table = bytearray(400)
        struct.pack_into("<I4BfiIIQ", table, shared.vm.PREFIX_BYTES,
                         shared.vm.CODEC_GSQ_V1, 6, 5, 5, 16, 0.00025, 3,
                         2, shared.vm.PAGE_ALIGNMENT, 176)
        table[176 + 48:176 + 80] = bytes.fromhex("ab" * 32)
        table[288 + 48:288 + 80] = bytes.fromhex("cd" * 32)
        plain.write_bytes(table)
        source = {"codec": {"page_count": 2, "decoded_digest_set_sha256":
                  shared.hashlib.sha256(bytes.fromhex("ab" * 32 + "cd" * 32)).hexdigest()}}
        tree = {"nodes": [{"decoded_sha256": "1" * 64}, {"decoded_sha256": "2" * 64}]}
        w13 = {"available": True, "passed": True, "nodes_verified": 2, "node_count": 2}
        for bad in ({}, {**w13, "nodes_verified": 1}, {**w13, "passed": False}):
            with self.subTest(w13=bad), self.assertRaisesRegex(ValueError, "content reference"):
                shared.content_references(plain, source, tree, bad)
        table[176 + 48] ^= 1
        plain.write_bytes(table)
        with self.assertRaisesRegex(ValueError, "content reference GSQ decoded digest set differs"):
            shared.content_references(plain, source, tree, w13)

    def test_real_migration_payload_observations_are_retained_without_decode_invention(self):
        capture = self.captured("d3_6/run43602195987_migration.json")
        for name, arm in capture["arms"].items():
            # Real selected result.json fields; field selection is documented in
            # the extract provenance. No missing field is filled with zero.
            behavior = shared.payload_behavior(arm, None)
            self.assertEqual(behavior["route"], "hlod", name)
            self.assertEqual(behavior["world_payload"], arm["world_payload_readback"])
            for key, value in arm["final_render_stats"].items():
                if key.startswith("hlod_stream_"):
                    self.assertEqual(behavior["native_payload"][key], value)
            for key in ("decoded_bytes", "decoded_digest", "decoded_pages", "decoded_splats"):
                self.assertIs(behavior[key]["measured"], False)
                self.assertTrue(behavior[key]["reason"])

    def test_real_overlap_record_drops_reach_the_arm_failure_observation(self):
        capture = self.captured("d3_6/run43602195987_drops.json")
        path = shared.rp.probe_path.resolve_path(self.captured("bv07/tsmoke.manifest.json"))
        trace_path = self.tmp / "trace.bin"
        trace_path.write_bytes(gzip.decompress((HERE / "fixtures/bv07/tsmoke_path.trace.bin.gz").read_bytes()))
        trace = shared.ps.read_trace(trace_path)
        prepared = {key: {} for key in ("input_file_sha256", "generator_identity", "tree_file_sha256", "tree_identity")}
        prepared["codec"] = {"sh_bands": 3}
        native = self.captured("bv07/tsmoke_path.result.json")
        for route, drops in capture["arms"].items():
            # Combine independent real producer extracts to test the mapping,
            # without claiming this is a newly captured paired execution.
            probe = {"overlap_drops_whole_run": drops["whole_run"]}
            with mock.patch.object(shared.rp, "trace_integrity", return_value=(trace, {})), mock.patch.object(
                    shared.rp, "summarise_path", return_value=probe):
                # Exercise the route-independent mapping through the HLOD branch:
                # this older BV-07 trace has no chunk-residency proof columns.
                arm = shared.run_arm(self.tmp / "binary", self.tmp / "world", "hlod", prepared, path, {}, {},
                    self.tmp / route, prepare_cold=lambda world: {}, process=lambda *args: {
                        "result": native, "trace": str(trace_path), "exit_code": 0, "timed_out": False})
            self.assertEqual(arm["observed"]["failures"]["drop_events"], drops["whole_run"]["drop_events"])
            self.assertGreater(arm["observed"]["failures"]["drop_events"], 0)

    def test_cpu_sink_capture_cannot_claim_actual_migration_hlod_route(self):
        native = self.captured("d3_5/d3_5_hlod_test_results.json")["completed"]
        self.assertIs(native["hlod_active"], False)
        result = {"final_render_stats": native, "route": "hlod"}
        self.assertIsNone(shared.payload_behavior(result, None)["route"])

    def test_migration_arm_keeps_the_frame_time_blocks_and_never_invents_them(self):
        # #1445. Constructed arm envelope (this module's own shape); the blocks are opaque here.
        blocks = {"moving_frame_interval": {"frames": 3}, "moving_frame_breakdown": {"status": "OK", "frames": 3},
                  "measured_frame_breakdown": {"status": "MISSING", "reason": "driver older than #1445"}}
        arm = {"execution": "RAN", "observed": {"exit_code": 0, "final_marker": True, "settings": {}},
               "trace": {"file": "trace.bin"}, "source": {"loaded_file_sha256": "0" * 64},
               "payload_behavior": {}, "cache": {},
               "probe": {"passes": [{"pass_index": 0, "label": "first", "segments": {"S1": {}}, **blocks}]}}
        identity = {"commit": "c" * 40, "sha256": "d" * 64}
        self.assertEqual(shared.migration_arm(arm, identity)["frame_time"],
                         [{"pass_index": 0, "label": "first", **blocks}])
        # An arm without a path summary (no trace) has no record: the report renders "not measured".
        for probe in ({}, {"passes": None}, None):
            self.assertEqual(shared.migration_arm({**arm, "probe": probe}, identity)["frame_time"], [])
        older = shared.migration_arm({**arm, "probe": {"passes": [{"pass_index": 0, "label": "first"}]}}, identity)
        self.assertEqual(older["frame_time"], [{"pass_index": 0, "label": "first", "moving_frame_interval": None,
                                                "moving_frame_breakdown": None, "measured_frame_breakdown": None}])

    def test_capture_arm_forwards_the_real_saturation_probe_plan(self):
        with mock.patch.object(shared, "run_arm", return_value={}) as run:
            shared.capture_arm(self.tmp / "candidate", self.tmp / "world", "hlod", {}, {}, {}, {},
                               self.tmp / "saturate", shared.PRE_D3, control="saturate")
        self.assertEqual(run.call_args.kwargs.get("control"), "saturate")

    def test_only_saturation_arm_sets_the_forcing_plan_and_all_arms_keep_shipped_caps(self):
        path = shared.rp.probe_path.resolve_path(self.captured("bv07/tsmoke.manifest.json"))
        prepared = {key: {} for key in ("input_file_sha256", "generator_identity", "tree_file_sha256", "tree_identity")}
        prepared["codec"] = {"sh_bands": 3}
        calls = []
        digests = {"7": "a" * 64}  # interpreter-contract input, not a captured producer
        def process(binary, plan, out, timeout):
            calls.append(copy.deepcopy(plan))
            return {"result": {"payload_telemetry": {"content_digests": digests}},
                    "trace": str(out / "trace.bin"), "exit_code": 1, "timed_out": False}
        with mock.patch.object(shared.rp, "trace_integrity", return_value=(None, {})):
            for control in ("none", "saturate"):
                arm = shared.run_arm(self.tmp / "candidate", self.tmp / "world", "hlod", prepared, path, {}, {},
                               self.tmp / control, process=process, prepare_cold=lambda world: {}, control=control)
                self.assertEqual(arm.get("content_digests"), digests)
                self.assertEqual(arm["payload_behavior"]["content_digests"], digests)
        self.assertNotIn("shared_layers_control", calls[0])
        self.assertEqual(calls[1]["shared_layers_control"], "saturate")
        for plan in calls:
            self.assertIs(plan["project_settings"].get(
                "rendering/gaussian_splatting/diagnostics/payload_content_digests"), True)
            self.assertEqual({key: value for key, value in plan["project_settings"].items()
                              if key != "rendering/gaussian_splatting/diagnostics/payload_content_digests"},
                             shared.rp.control_settings(None))
            self.assertEqual(plan["node"]["lod_bias"], 1.0)

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
        def capture(binary, world, route, prepared, path, truth, source_manifest, out, expected_commit, control="none"):
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

    def test_saturation_control_runs_a_fifth_hlod_arm_after_all_clean_measurements(self):
        import dispatch_lane as lane
        manifest = self.captured("bv07/tsmoke.manifest.json")
        plain, tree = self.tmp / "plain/world.gsplatworld", self.tmp / "tree/world.gsplatworld"
        calls = []
        # Controller preconditions, intentionally not native probe-format fixtures.
        def capture(binary, world, route, prepared, path, truth, source_manifest, out, expected_commit, control="none"):
            calls.append((out.name, world, route, control))
            return {"execution": "RAN", "source": {"loaded_file_sha256": "policy digest"},
                    "observed": {"exit_code": 0, "final_marker": True, "settings": {}}, "payload_behavior": {},
                    "trace": {"file": str(out / "trace.bin")}, "cache": {},
                    "native": {"shared_layers_control": {"hook_applied": control == "saturate", "observed": True}}}
        with mock.patch.object(shared.subprocess, "run", return_value=mock.Mock(stdout="a" * 40 + "\n")), mock.patch.object(
                shared, "verified_binary", side_effect=lambda binary, info, expected: {"commit": expected, "sha256": "b" * 64}), mock.patch.object(
                shared.wc, "cache_lock", return_value=contextlib.nullcontext()), mock.patch.object(
                lane, "prepare_auto_tree_t0", return_value={"plain": plain, "tree": tree, "report": {}}), mock.patch.object(
                shared, "prepared_receipt", return_value={}), mock.patch.object(shared, "read_json", return_value=manifest), mock.patch.object(
                shared, "capture_arm", side_effect=capture):
            rc = shared.run(self.tmp, self.tmp / "candidate", self.tmp / "base", self.tmp / "base-info.json",
                            self.tmp / "cache", self.tmp / "out", "unit-orchestration", "saturate")
        self.assertEqual(rc, 0)
        self.assertEqual([call[0] for call in calls], ["chunk", "hlod", "base", "candidate", "saturate"])
        self.assertEqual(calls[-1][1:], (tree, "hlod", "saturate"))
        receipt = json.loads((self.tmp / "out/experiment.json").read_text())["control_receipt"]
        self.assertEqual(receipt["status"], "FORCED")
        self.assertIs(receipt["observed"]["hook_applied"], True)

    def test_probe_retains_measurement_exports_and_separate_hlod_convergence(self):
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        self.assertIn('"device_vram_ledger"]', gd)
        self.assertIn('_result["shared_layers_settings"]', gd)
        self.assertIn('_result["loaded_file_sha256"] = FileAccess.get_sha256', gd)
        # Desired counts missing demand; resident counts the entire cache.
        self.assertIn('int(_last_stats["hlod_stream_desired_nodes"]) == 0', gd)
        self.assertNotIn('int(_last_stats["hlod_stream_desired_nodes"]) == int(_last_stats["hlod_stream_resident_nodes"])', gd)
        self.assertIn('if not bool(_last_stats.get("hlod_active", false)):', gd)
        self.assertIn('int(ss["needed_not_resident_chunks"]) != 0', gd)
        self.assertIn('_result["shared_layers_settings_samples"] = _shared_layers_settings_samples', gd)
        self.assertIn('renderer.call("_test_set_shared_layers_saturation", true)', gd)
        self.assertIn('int(payload.get("upload_hold_frames", 0)) > 0 and int(payload.get("ready_bytes", 0)) > 0', gd)
        self.assertIn('renderer.call("_test_set_shared_layers_saturation", false)', gd)

    def test_probe_applies_the_common_settings_without_a_diagnostic_capacity_override(self):
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        settings_loop = gd[gd.index('\tvar settings: Dictionary = _plan.get("project_settings", {})',
                                    gd.index('func _run()')):gd.index('\tProjectSettings.emit_signal("settings_changed")',
                                    gd.index('func _run()'))]
        self.assertIn('ProjectSettings.set_setting(key, settings[key])', settings_loop)
        self.assertNotIn('shared_layers_retained_mb', gd)
        self.assertNotIn('shared_layers_retained_mb', (HERE / "shared_layers_t0.py").read_text(encoding="utf-8"))

    def test_real_capture_settle_windows_are_retained_without_inventing_progress(self):
        native = self.captured("bv07/tsmoke_path.result.json")
        windows = shared.forward_progress_windows(native)
        self.assertEqual(shared.observations(native, 0)["forward_progress"], windows)
        expected = {(entry["pass_index"], entry["id"]) for entry in native["segments"] if "settle" in entry}
        self.assertEqual({(entry["pass_index"], entry["segment"]) for entry in windows}, expected)
        self.assertEqual(len(windows), len(expected))
        self.assertTrue(windows)
        for entry in windows:
            self.assertIs(entry["measured"], False)
            self.assertTrue(entry["reason"])
            self.assertNotIn("no_forward_progress", entry)

    def test_progress_receipts_prefer_all_segment_windows_and_preserve_native_samples(self):
        # Constructed controller policy, not a fixture claiming native output.
        cold = {"measured": True, "window": "cold_deadline_s", "samples": [{"completion_count": 1}]}
        stop = {"measured": False, "reason": "no native initial observation"}
        result = {"segments": [
            {"id": "cold", "pass_index": 0, "settle": {"settled": True, "forward_progress": cold}},
            {"id": "stop", "pass_index": 0, "settle": {"settled": True, "forward_progress": stop}}],
            "checkpoints": [{"segment": "stop", "checkpoint": "C0", "pass_index": 0,
                             "settle": {"settled": True, "forward_progress": stop}}]}
        windows = shared.forward_progress_windows(result)
        self.assertEqual(len(windows), 2)
        self.assertEqual(windows[0]["window"], "cold_deadline_s")
        self.assertEqual(windows[0]["samples"], cold["samples"])
        windows[0]["samples"].clear()
        self.assertTrue(cold["samples"])

    def test_progress_wiring_uses_existing_deadlines_and_read_decode_partial_upload_progress(self):
        gd = (HERE / "probe_world.gd").read_text(encoding="utf-8")
        begin = gd.index('func _shared_layers_progress_snapshot()')
        end = gd.index('func _pump_for(', begin)
        progress = gd[begin:end]
        for field in ("reads_completed_ok", "pages_decoded", "upload_completions", "upload_bytes",
                      "upload_chunks_total", "upload_bytes_total", "needed_not_resident_chunks"):
            self.assertIn('"' + field + '"', progress)
        self.assertIn('int(deadline_s * 1000000.0)', progress)
        self.assertIn('progress["observed_usec"] = observed_usec', progress)
        self.assertIn('and observed_usec >= int(deadline_s * 1000000.0) and demand_throughout and not any_progress', progress)
        self.assertIn('progress_samples.append(_shared_layers_progress_snapshot())', progress)
        self.assertIn('await _pump_settle(float(settle_cfg.get(key, 10.0)), key)', gd)
        self.assertNotIn('admission_waits', progress)
        self.assertNotIn('reads_refused_busy', progress)


class PressureVariant(unittest.TestCase):
    """#1471: the unconstrained T0 keeps the whole tree resident, so its eviction and
    budget checks cannot fail. The pressure variant caps the HLOD atlas and the drawn
    budget through existing settings; these tests cover its plumbing only."""

    T0_TREE_PAGES = 5136  # run 43742475749: hlod_stream_pages_capacity of the unconstrained T0

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)

    def request(self, pages=T0_TREE_PAGES):
        with mock.patch.object(shared, "tree_page_count", return_value=pages):
            return shared.pressure_request(self.tmp / "tree.gsplatworld")

    def test_page_arithmetic_constants_are_the_engine_ones(self):
        root = HERE.parents[2] / "modules/gaussian_splatting/renderer"
        layout = (root / "gaussian_gpu_layout.h").read_text(encoding="utf-8")
        record = re.search(r"static_assert\(sizeof\(PackedGaussian\) == (\d+),", layout)
        self.assertIsNotNone(record, "PackedGaussian size pin not found")
        self.assertEqual(shared.PRESSURE_RECORD_BYTES, int(record.group(1)))
        streamer = (root / "hlod_node_streamer.h").read_text(encoding="utf-8")
        page = re.search(r"DEFAULT_PAGE_SPLATS = (\d+);", streamer)
        self.assertIsNotNone(page, "HLOD page size not found")
        self.assertEqual(shared.PRESSURE_PAGE_SPLATS, int(page.group(1)))
        # The cap must be a setting the publisher actually reads for the streamed atlas.
        publisher = (root / "resident_instance_contract_publisher.cpp").read_text(encoding="utf-8")
        self.assertIn(f'"{shared.PRESSURE_ATLAS_SETTING}"', publisher)
        self.assertIn("ledger.atlas_limit(atlas_override)", publisher)
        # The uncompressed stride only holds while the lane keeps quantization off.
        self.assertEqual(shared.rp.TUNED_SETTINGS["rendering/gaussian_splatting/compression/per_chunk_quantization"], 0)

    def test_tree_pages_are_counted_from_the_v4_node_directory(self):
        # Constructed directory (the offsets corrupt_copy and verify_v4 read), NOT a V4 writer
        # fixture: it shares the parser's layout assumption and proves only the arithmetic and
        # the refusals. No engine binary exists in this lane to capture a real tree, so the
        # assumption itself is checked where a real tree exists: on every run, against the
        # producer's sidecar totals (the mismatch cases below), and by the comparison, which
        # requires the native atlas capacity to land inside the band this count defines.
        corpus = bytearray(4096 + 65536)
        struct.pack_into("<II", corpus, 0, 0x57505347, 4)
        struct.pack_into("<I", corpus, 40, 3)
        for index, splats in enumerate((1, 1024, 1025)):
            struct.pack_into("<QI", corpus, 4096 + 64 + index * 256 + 88, 0, splats)
        tree = self.tmp / "tree.gsplatworld"
        sidecar = tree.with_suffix(".manifest.json")

        def totals(nodes=3, leaves=1025, interiors=1025):
            sidecar.write_text(json.dumps({"totals": {"node_count": nodes, "leaf_splat_count": leaves,
                                                      "interior_splat_count": interiors}}), encoding="utf-8")
        tree.write_bytes(corpus)
        totals()
        self.assertEqual(shared.tree_page_count(tree), 1 + 1 + 2)
        # A directory read that disagrees with the producer's totals is refused, never used.
        for wrong in ({"nodes": 4}, {"leaves": 1024}, {"interiors": 0}):
            with self.subTest(wrong=wrong):
                totals(**wrong)
                with self.assertRaises(ValueError):
                    shared.tree_page_count(tree)
        for text in ("{}", json.dumps({"totals": {"node_count": 3}}),
                     json.dumps({"totals": {"node_count": 3, "leaf_splat_count": None, "interior_splat_count": 2050}})):
            with self.subTest(sidecar=text):
                sidecar.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    shared.tree_page_count(tree)
        totals()
        struct.pack_into("<QI", corpus, 4096 + 64 + 256 + 88, 0, 0)
        tree.write_bytes(corpus)
        with self.assertRaises(ValueError):
            shared.tree_page_count(tree)
        tree.write_bytes(b"not a v4 file")
        with self.assertRaises(ValueError):
            shared.tree_page_count(tree)

    def test_request_caps_the_t0_atlas_inside_the_band_and_the_drawn_budget_at_250k(self):
        request = self.request()
        self.assertEqual(request["name"], "pressure")
        self.assertEqual(request["tree_pages"], self.T0_TREE_PAGES)
        self.assertEqual(request["atlas_setting"], shared.PRESSURE_ATLAS_SETTING)
        self.assertEqual(request["atlas_fraction_band"], [0.20, 0.25])
        self.assertEqual(request["drawn_budget_splats"], 250_000)
        page_bytes = shared.PRESSURE_PAGE_SPLATS * shared.PRESSURE_RECORD_BYTES
        self.assertEqual(request["requested_atlas_pages"], (request["atlas_override_mb"] << 20) // page_bytes)
        fraction = request["requested_atlas_pages"] / request["tree_pages"]
        self.assertTrue(0.20 <= fraction <= 0.25, fraction)
        self.assertEqual(request["chunk_arm"]["role"], "unconstrained_reference")
        self.assertTrue(request["chunk_arm"]["reason"])

    def test_request_refuses_a_tree_the_whole_mib_setting_cannot_cap_in_band(self):
        for pages in (0, 1, 10):
            with self.subTest(pages=pages), self.assertRaises(ValueError):
                self.request(pages)

    def run_arm(self, route, pressure, result=None):
        path = shared.rp.probe_path.resolve_path(json.loads(
            (HERE / "fixtures/bv07/tsmoke.manifest.json").read_text(encoding="utf-8")))
        prepared = {key: {} for key in ("input_file_sha256", "generator_identity", "tree_file_sha256", "tree_identity")}
        prepared["codec"] = {"sh_bands": 3}
        plans = []

        def process(binary, plan, out, timeout):
            plans.append(copy.deepcopy(plan))
            return {"result": result or {}, "trace": str(out / "trace.bin"), "exit_code": 1, "timed_out": False}
        extra = {} if pressure is None else {"pressure": pressure}
        with mock.patch.object(shared.rp, "trace_integrity", return_value=(None, {})):
            arm = shared.run_arm(self.tmp / "candidate", self.tmp / "world", route, prepared, path, {}, {},
                                 self.tmp / f"{route}-{len(extra)}", process=process,
                                 prepare_cold=lambda world: {}, **extra)
        return arm, plans[0]

    def test_only_the_pressure_variant_changes_the_plan_and_both_routes_get_the_same_policy(self):
        request = self.request()
        for route in ("chunk", "hlod"):
            with self.subTest(route=route):
                arm, plan = self.run_arm(route, None)
                self.assertNotIn(shared.PRESSURE_ATLAS_SETTING, plan["project_settings"])
                self.assertEqual(plan["node"]["max_splat_count"], shared.rp.TUNED_VISIBLE_CAP)
                self.assertNotIn("pressure", arm)
                arm, plan = self.run_arm(route, request)
                self.assertEqual(plan["project_settings"][shared.PRESSURE_ATLAS_SETTING], request["atlas_override_mb"])
                self.assertEqual(plan["node"]["max_splat_count"], 250_000)
                self.assertEqual(arm["requested"]["visible_splats"], 250_000)
                self.assertEqual(arm["requested"]["project_settings_readback"][shared.PRESSURE_ATLAS_SETTING],
                                 request["atlas_override_mb"])
                self.assertEqual(arm["pressure"]["route"], route)
                persisted = json.loads((Path(arm["artifact_root"]) / "arm.json").read_text(encoding="utf-8"))
                self.assertEqual(persisted["pressure"], arm["pressure"])

    def test_pressure_disables_object_tail_for_comparable_migration_capacity_only(self):
        setting = "rendering/gaussian_splatting/hlod/object_tail_reservation_percent"
        request = self.request()
        self.assertEqual(request["object_tail_setting"], setting)
        self.assertEqual(request["object_tail_reservation_percent"], 0)
        self.assertIsInstance(request["object_tail_reason"], str)
        self.assertTrue(request["object_tail_reason"].strip())

        for variant in ("pressure", "unconstrained"):
            rc, calls, experiment = self.orchestrate(variant)
            self.assertEqual(rc, 0)
            self.assertEqual([name for name, _ in calls], ["chunk", "hlod", "base", "candidate"])
            if variant == "pressure":
                self.assertEqual(experiment["variant"], request)
            for name, extra in calls:
                with self.subTest(variant=variant, arm=name):
                    route = "chunk" if name == "chunk" else "hlod"
                    _, plan = self.run_arm(route, extra.get("pressure"))
                    if variant == "pressure":
                        self.assertEqual(plan["project_settings"][setting], 0)
                    else:
                        self.assertNotIn(setting, plan["project_settings"])

    def test_absent_pressure_observations_stay_not_measured_never_zero(self):
        request = self.request()
        observed = shared.pressure_observations({}, None, "hlod", request)
        for key in ("atlas_pages_capacity", "evicted_total", "budget_bound_frames", "payload_reads_total", "rereads"):
            self.assertIs(observed[key].get("measured"), False, key)
            self.assertTrue(observed[key].get("reason"), key)
        self.assertEqual(observed["constraint"], "applied")
        self.assertEqual(observed["settles"], [])
        chunk = shared.pressure_observations({}, None, "chunk", request)
        self.assertEqual(chunk["constraint"], "not_applied")
        self.assertEqual(chunk["role"], "unconstrained_reference")
        self.assertIs(chunk["evicted_chunks"].get("measured"), False)
        self.assertNotIn("evicted_total", chunk)

    def test_pressure_observations_copy_native_counters_and_teleport_settle_receipts(self):
        # Constructed counters around the documented result shape; the settle receipts are the
        # real BV-07 capture's, which predates forward-progress receipts (kept as not measured).
        result = json.loads((HERE / "fixtures/bv07/tsmoke_path.result.json").read_text(encoding="utf-8"))
        result["final_render_stats"] = {"hlod_stream_pages_capacity": 1152, "hlod_stream_pages_resident": 1100,
                                        "hlod_stream_evicted_total": 41, "hlod_budget_bound_frames": 7,
                                        "hlod_stream_payload_reads_total": 700}
        result["payload_telemetry"] = {"content_digests": {str(i): "a" * 64 for i in range(614)}}
        observed = shared.pressure_observations(result, None, "hlod", self.request())
        self.assertEqual(observed["atlas_pages_capacity"], 1152)
        self.assertEqual(observed["evicted_total"], 41)
        self.assertEqual(observed["budget_bound_frames"], 7)
        self.assertEqual(observed["rereads"], 700 - 614)
        kinds = {(s["pass_index"], s["id"]): s["kind"] for s in result["segments"]}
        teleports = [cp for cp in result["checkpoints"] if kinds[(cp["pass_index"], cp["segment"])] == "teleport"]
        self.assertTrue(teleports)
        rows = [row for row in observed["settles"] if row["kind"] == "teleport"]
        self.assertEqual(len(rows), len(teleports))
        self.assertEqual(rows[0]["frames"], teleports[0]["settle"]["frames"])
        self.assertEqual(rows[0]["settle_ms"], teleports[0]["settle"]["settle_usec"] / 1000.0)
        self.assertEqual(rows[0]["drawn_splats"], teleports[0]["drawn_splats"])
        self.assertIs(rows[0]["streaming_work"].get("measured"), False)
        teleports[0]["settle"]["forward_progress"] = {"measured": True, "progress_deltas": {"upload_completions": 9}}
        again = shared.pressure_observations(result, None, "hlod", self.request())
        self.assertEqual([r for r in again["settles"] if r["kind"] == "teleport"][0]["streaming_work"],
                         {"upload_completions": 9})

    def orchestrate(self, variant, control="none"):
        import dispatch_lane as lane
        manifest = json.loads((HERE / "fixtures/bv07/tsmoke.manifest.json").read_text(encoding="utf-8"))
        plain, tree = self.tmp / "plain/world.gsplatworld", self.tmp / "tree/world.gsplatworld"
        calls = []

        def capture(binary, world, route, prepared, path, truth, source_manifest, out, expected_commit, **extra):
            calls.append((out.name, extra))
            return {"execution": "RAN", "source": {"loaded_file_sha256": "policy digest"},
                    "observed": {"exit_code": 0, "final_marker": True, "settings": {}}, "payload_behavior": {},
                    "trace": {"file": str(out / "trace.bin")}, "cache": {}, "native": {}}
        with mock.patch.object(shared.subprocess, "run", return_value=mock.Mock(stdout="a" * 40 + "\n")), mock.patch.object(
                shared, "verified_binary", side_effect=lambda binary, info, expected: {"commit": expected, "sha256": "b" * 64}), mock.patch.object(
                shared.wc, "cache_lock", return_value=contextlib.nullcontext()), mock.patch.object(
                lane, "prepare_auto_tree_t0", return_value={"plain": plain, "tree": tree, "report": {}}), mock.patch.object(
                shared, "prepared_receipt", return_value={}), mock.patch.object(shared, "read_json", return_value=manifest), mock.patch.object(
                shared, "tree_page_count", return_value=self.T0_TREE_PAGES), mock.patch.object(
                shared, "capture_arm", side_effect=capture):
            rc = shared.run(self.tmp, self.tmp / "candidate", self.tmp / "base", self.tmp / "base-info.json",
                            self.tmp / "cache", self.tmp / "out", "unit-orchestration", control, variant=variant)
        return rc, calls, json.loads((self.tmp / "out/experiment.json").read_text(encoding="utf-8"))

    def test_run_gives_all_four_arms_the_same_pressure_request_and_records_it(self):
        rc, calls, experiment = self.orchestrate("pressure")
        self.assertEqual(rc, 0)
        self.assertEqual([name for name, _ in calls], ["chunk", "hlod", "base", "candidate"])
        request = self.request()
        for name, extra in calls:
            self.assertEqual(extra.get("pressure"), request, name)
        self.assertEqual(experiment["variant"], request)

    def test_unconstrained_run_is_unchanged(self):
        rc, calls, experiment = self.orchestrate("unconstrained")
        self.assertEqual(rc, 0)
        self.assertNotIn("variant", experiment)
        for name, extra in calls:
            self.assertNotIn("pressure", extra, name)

    def test_pressure_with_a_negative_control_or_an_unknown_variant_is_not_run(self):
        for variant, control in (("pressure", "corrupt"), ("pressure", "saturate"), ("tighter", "none")):
            with self.subTest(variant=variant, control=control):
                rc, calls, experiment = self.orchestrate(variant, control)
                self.assertEqual(rc, 3)
                self.assertEqual(calls, [])
                self.assertIn("variant", experiment["orchestration_error"])

    def test_dispatch_command_forwards_the_variant_and_defaults_to_unconstrained(self):
        import dispatch_lane as lane
        base = ["shared-layers-t0", "--repo", str(self.tmp), "--godot-binary", str(self.tmp / "candidate"),
                "--base-binary", str(self.tmp / "base"), "--base-build-info", str(self.tmp / "base-info.json"),
                "--cache-root", str(self.tmp / "cache"), "--out", str(self.tmp / "out"), "--run-id", "unit"]
        for extra, expected in (([], "unconstrained"), (["--variant", "pressure"], "pressure")):
            with self.subTest(extra=extra), mock.patch.object(shared, "run", return_value=0) as run:
                self.assertEqual(lane.main(base + extra), 0)
                self.assertEqual(run.call_args.kwargs, {"variant": expected})
                self.assertEqual(run.call_args.args[-2:], ("unit", "none"))


if __name__ == "__main__":
    unittest.main()
