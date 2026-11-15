#!/usr/bin/env python3
"""D3-6 phase 1: red contract tests; stdlib only, no engine invocation.

The small run43602195987 extracts now include real paired T0 observations;
they omit trace binaries and cannot certify a complete local paired execution.
The run43629131401 extract retains complete real content-reference/runtime maps
and migration draw counts; tests plug these into the synthetic policy envelope,
not a replay claiming that the original UNUSABLE GPU run was VALID.
Other native probe inputs below come from the REAL BV-07 trace/result and D3-5 telemetry captures, with provenance
in fixtures/bv07/README.md and fixtures/d3_5/README.md (CI run 43580666400).
Summaries are built by the real producer-side Python helpers: summarise_path,
trace_integrity, route_telemetry_summary and screenshot_record. We do not author
a replacement probe format or edit the checked-in captures.

The NEW comparison-owned envelope (prepared/observed/migration) is a constructed
policy scenario, not a captured producer output. In particular its T0 identities,
GPU drawing, queue/publication observations and tiny screenshot files are test
preconditions, NOT measurements from those captures. D3-5 used a CPU upload sink;
BV-07 used T-smoke on a 3090. Neither is usable paired T0 evidence. Phase 2 must
collect the independent observations, including native bounds and publication
counters, on the 3080 or return UNUSABLE/NOT_RUN. A test below removes
those observations explicitly; telemetry alone cannot certify GPU evidence.

Envelope contract: prepared describes the ONE T0; each arm has source receipts,
requested settings, independent observed settings/health/queues, a native probe
summary and an on-disk trace. Migration has immutable binary identities and the
same V4 digest for both runs. compare() returns diagnostic validity only; CLI
writes comparison.json/.md, and a separate --require-report invocation binds the
report to run_id/control and fails closed. Missing execution is NOT_RUN (exit 3),
executed but bad/incomplete evidence is UNUSABLE (exit 2), VALID is exit 0.
"""

from __future__ import annotations

import base64
import copy
import gzip
import hashlib
import importlib.util
import json
import re
import struct
import sys
import tempfile
import unittest
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


ps = _load("probe_summary")
rp = _load("run_probe")
driver = _load("compare_shared_layers_t0")
shared = _load("shared_layers_t0")
workflow_pins = _load("test_dispatch_lane")

PRE_D3 = "f2f19bcb580cc131615b3fa262a11d99e6abc70c"
CANDIDATE = "6343fdba2690b1066123bb299f65110ff5e0e285"
WORKFLOW = ROOT / ".github/workflows/beyond_vram_flythrough.yml"
DRIVER = "tests/runtime/flythrough/compare_shared_layers_t0.py"
FORK_GUARD = ("github.event_name != 'pull_request' || "
              "github.event.pull_request.head.repo.full_name == github.repository")

# Policy, not an inventory derived from whichever metrics happened to arrive.
# Missing optional measurements must occupy a slot with measured:false + reason.
METRICS = {
    "correctness": ("exit_code", "final_marker", "actual_route", "read_failures", "decode_failures",
                    "stale_generation_publications", "nonresident_drawn_nodes", "overflow_events", "drop_events"),
    "io": ("stored_bytes", "requests", "queue_latency_ns", "read_latency_ns", "throughput_bytes_s",
           "cancellations", "retained_completion_peak_bytes", "admission_waits", "reads_refused_busy"),
    "decode_pack": ("verify_ns", "decode_span_ns", "digest_ns", "decode_remainder_ns", "pack_ns",
                    "splats", "pages", "worker_captures", "main_thread_captures"),
    "cpu_memory": ("os_peak_rss_bytes", "active_bytes", "stored_bytes", "mailbox_bytes", "ready_bytes",
                   "shadow_bytes", "retained_bytes", "active_bytes_peak", "stored_bytes_peak",
                   "mailbox_bytes_peak", "ready_bytes_peak", "shadow_bytes_peak", "retained_bytes_peak",
                   "oversized_node_events"),
    "upload": ("bytes_per_frame", "total_bytes", "unique_bytes", "slices", "queue_residence_ns",
               "completion_latency_ns", "staging_allowance_bytes"),
    "rendering": ("first_complete_frame", "stop_convergence", "teleport_convergence", "drawn_nodes",
                  "drawn_splats", "drawn_fraction", "screenshots"),
    "performance": ("moving_frame_p95_ms", "moving_frame_p99_ms", "per_segment", "frame_coherent_gpu",
                    "device_ledger_vram_bytes", "external_vram_bytes"),
}


def _sha(label):
    return hashlib.sha256(label.encode("ascii")).hexdigest()


def _captured(name):
    return json.loads((HERE / "fixtures" / name).read_text(encoding="utf-8"))


class ComparisonContract(unittest.TestCase):
    def test_sync_cut_reporting_preserves_missing_counter_and_measured_zero(self):
        absent = driver._route_metrics({})["performance"]["overlap_sync_cuts"]
        self.assertIs(absent["measured"], False)
        self.assertEqual(absent["status"], "not_measured")
        self.assertIsNone(absent["value"])
        source = shared.sync_cut_measurements({"overlap_sync_cuts": {"count": 0, "events": []}})
        present = driver._route_metrics({"optional_measurements": {"overlap_sync_cuts": source}})
        self.assertIs(present["performance"]["overlap_sync_cuts"]["measured"], True)
        self.assertEqual(present["performance"]["overlap_sync_cuts"]["value"]["count"], 0)
        missing = shared.sync_cut_measurements({})
        missing_report = driver._route_metrics({"optional_measurements": {"overlap_sync_cuts": missing}})
        self.assertIs(missing_report["performance"]["overlap_sync_cuts"]["measured"], False)

    @classmethod
    def setUpClass(cls):
        cls.trace_bytes = gzip.decompress((HERE / "fixtures/bv07/tsmoke_path.trace.bin.gz").read_bytes())
        cls.result = _captured("bv07/tsmoke_path.result.json")
        cls.manifest = _captured("bv07/tsmoke.manifest.json")
        cls.hlod_capture = _captured("d3_5/d3_5_hlod_test_results.json")
        cls.chunk_capture = _captured("d3_5/d3_5_chunk_test_results.json")["skipped"]
        cls.migration_capture = _captured("d3_6/run43602195987_migration.json")
        cls.content_capture = _captured("d3_6/run43629131401_content.json")
        cls.path = rp.probe_path.resolve_path(cls.manifest)
        cls.truth = rp.probe_groundtruth.build_groundtruth(
            cls.manifest, cls.path, rp.probe_path.checkpoints(cls.path), rp.CAMERA, rp.ASPECT)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.tmp = Path(temporary.name)
        self.experiment = self.make_experiment()

    def make_arm(self, route, prepared):
        directory = self.tmp / route
        directory.mkdir()
        trace_path = directory / "trace.bin"
        trace_path.write_bytes(self.trace_bytes)
        record = (self.chunk_capture if route == "chunk" else {
            "source_generation": self.hlod_capture["source_generation"],
            "payload_telemetry": self.hlod_capture["completed"]["payload_telemetry"],
        })
        # Native telemetry is unchanged real producer output. The two source
        # generation IDs deliberately differ; equality is required WITHIN a run.
        generation = record["source_generation"]
        result = {**copy.deepcopy(self.result), "route": route, "source_generation": generation,
                  "payload_telemetry": copy.deepcopy(record["payload_telemetry"])}
        run = {"result": result, "trace": str(trace_path)}
        trace, integrity = rp.trace_integrity(run)
        self.assertEqual(integrity["status"], ps.OK)
        probe = rp.summarise_path(trace, run, self.path, self.truth, 1, self.manifest)
        self.assertEqual(probe["payload_telemetry"]["status"], ps.OK)
        self.assertEqual(probe["payload_telemetry"], ps.route_telemetry_summary(
            record["payload_telemetry"], route=route, source_generation=generation))

        # File existence tests, not claimed viewport captures. Build the native
        # screenshot summary with its real Python producer, not an invented shape.
        shots = directory / "screenshots"
        shots.mkdir()
        png = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=")
        for name in ("moving.png", "checkpoint.png"):
            (shots / name).write_bytes(png)
        probe["screenshots"] = rp.screenshot_record({
            "dir": str(shots), "moving_every_n": 100,
            "taken": [{"name": name, "file": name} for name in ("moving.png", "checkpoint.png")],
        })
        settings = {
            "camera_path_sha256": _sha("one T0 camera path"),
            "camera_poses": copy.deepcopy(self.path["segments"]),
            "resolution": [1280, 720], "codec": copy.deepcopy(prepared["codec"]),
            "sh_bands": 3, "visible_splats": 4_000_000, "drawn_splats": 4_000_000,
            "timing_windows": {"moving_passes": 2, "load_deadline_s": 90, "hold_s": 2},
            "node_readback": copy.deepcopy(self.result["node_readback"]),
            "project_settings_readback": copy.deepcopy(self.result["project_settings_readback"]),
        }
        # New comparison protocol, explicitly constructed preconditions. These
        # MUST come from process observations in phase 2, never the request plan.
        observed = {
            "camera_application": shared.camera_application(trace, self.path),
            "settings": copy.deepcopy(settings), "source_generation": generation,
            "final_marker": True, "exit_code": 0,
            "runtime": {"hlod_stream_paged": route == "hlod", "hlod_stream_page_store_active": route == "hlod",
                        "resident_materialized_fallback": False, "gpu_resident_drawn_nodes": 1},
            "failures": {"stale_generation_publications": 0, "nonresident_drawn_nodes": 0,
                         "overflow_events": 0, "drop_events": 0, "digest_verification_failures": 0,
                         "hlod_stream_failed_total": 0, "hlod_stream_late_dropped_total": 0},
            "queues": {
                owner: {"bytes": 0, "peak_bytes": 1024, "byte_bound": 8192,
                        "jobs": 0, "peak_jobs": 1, "job_bound": 16}
                for owner in ("retained", "ready")
            },
            "saturation": {"hlod_stream_stall_frames": 0, "hlod_stream_blocked_groups": 0,
                           "reads_refused_busy": 0, "admission_waits": 0, "upload_hold_frames": 0},
        }
        source = {k: copy.deepcopy(prepared[k]) for k in (
            "input_file_sha256", "generator_identity", "codec", "tree_file_sha256", "tree_identity")}
        source["loaded_file_sha256"] = prepared["input_file_sha256" if route == "chunk" else "tree_file_sha256"]
        return {"execution": "RAN", "source": source, "requested": settings, "observed": observed,
                "probe": probe, "trace": {"file": str(trace_path), "integrity": integrity},
                "artifact_root": str(directory),
                "cache": {"label": "cold", "independently_prepared": True, "repeat_label": "repeat"},
                "optional_measurements": {
                    "os_peak_rss_bytes": ps.missing("not sampled in this unit-test replay"),
                    # #1441 native renderer placeholder; cannot become measured zero VRAM.
                    "gpu_memory_usage_mb": self.hlod_capture["completed"]["gpu_memory_usage_mb"],
                }}

    def make_experiment(self):
        prepared = {
            "tier": "T0", "splat_count": 4_194_304, "extent_m": [512, 512],
            "input_file_sha256": _sha("one synthetic plain GSQ T0"),
            "generator_identity": {"gen_sha": _sha("generator"), "source_fingerprint": _sha("sources")},
            "codec": {"encoding": "gsq", "sh_bands": 3, "per_chunk_quantization": False},
            "tree_file_sha256": _sha("derived V4 GSQ tree"),
            "tree_identity": {"tree_sha": _sha("tree"), "source_fingerprint": _sha("tree sources")},
            "w13": {"available": True, "passed": True, "node_count": 1, "nodes_verified": 1},
        }
        arms = {route: self.make_arm(route, prepared) for route in ("chunk", "hlod")}
        migration = {
            "base_commit": PRE_D3, "candidate_commit": CANDIDATE,
            "arms": {name: {"execution": "RAN", "binary_commit": commit, "exit_code": 0,
                            "binary_sha256": _sha(f"optimized binary {commit}"),
                            "final_marker": True, "trace_file": arms["hlod"]["trace"]["file"],
                            "input_file_sha256": prepared["tree_file_sha256"],
                            # Constructed migration policy observations, using
                            # real captured checkpoint counts. This is not a
                            # captured old/new execution; see the module docstring.
                            "payload_behavior": {
                                "route": "hlod", "loaded_file_sha256": prepared["tree_file_sha256"],
                                "drawn_counts": copy.deepcopy(self.migration_capture["arms"][name]["payload_behavior"]["drawn_counts"]),
                                "decoded_bytes": ps.missing("no common native decoded-byte observation"),
                            },
                            "settings_readback": copy.deepcopy(arms["hlod"]["observed"]["settings"])}
                     for name, commit in (("base", PRE_D3), ("candidate", CANDIDATE))},
        }
        for arm in migration["arms"].values():
            arm["camera_application"] = copy.deepcopy(arms["hlod"]["observed"]["camera_application"])
        for name, arm in migration["arms"].items():
            native = self.migration_capture["arms"][name]
            behavior = arm["payload_behavior"]
            behavior["native_payload"] = copy.deepcopy(native["final_render_stats"])
            behavior["world_payload"] = copy.deepcopy(native["world_payload_readback"])
            behavior["route_evidence"] = copy.deepcopy(native["payload_behavior"]["route_evidence"])
        # Constructed comparison-owned content contract, NOT native producer
        # telemetry or a captured digest fixture. Real captures remain untouched.
        prepared["content_references"] = {
            route: {"0": _sha(f"{route} decoded node zero"), "1": _sha(f"{route} decoded node one")}
            for route in ("chunk", "hlod")}
        for route, arm in arms.items():
            arm["content_digests"] = copy.deepcopy(prepared["content_references"][route])
        migration["arms"]["candidate"]["payload_behavior"]["content_digests"] = copy.deepcopy(
            prepared["content_references"]["hlod"])
        # Default is a passing policy scenario. The captured C2 jitter is replayed
        # separately below and must fail; do not bless it as identity here.
        migration["arms"]["candidate"]["payload_behavior"]["drawn_counts"] = copy.deepcopy(
            migration["arms"]["base"]["payload_behavior"]["drawn_counts"])
        return {"schema": "godotgs-shared-layers-t0-experiment/1", "run_id": "unit-policy-scenario",
                "control": "none", "prepared": prepared, "arms": arms, "migration": migration}

    def arm(self, name="hlod"):
        return self.experiment["arms"][name]

    @contextmanager
    def scenario(self, **labels):
        """Keep failing subtests independent while the implementation is red."""
        original = copy.deepcopy(self.experiment)
        with self.subTest(**labels):
            try:
                yield
            finally:
                self.experiment = original

    def assert_verdict(self, verdict, reason=None):
        report = driver.compare(self.experiment)
        self.assertEqual(report.get("verdict"), verdict, {k: report.get(k) for k in ("verdict", "reasons", "control_verdict")})
        self.assertEqual(report.get("exit_code"), {"VALID": 0, "UNUSABLE": 2, "NOT_RUN": 3}[verdict], report)
        self.assertIs(report.get("report_only"), True)
        if reason:
            self.assertIn(reason, [item.get("code") for item in report.get("reasons", [])], report.get("reasons"))
        return report

    def test_valid_paired_policy_scenario_is_report_only(self):
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        self.assertEqual(report["migration"]["base_commit"], PRE_D3)
        text = driver.render_markdown(report)
        self.assertIn("report-only", text.lower())
        self.assertNotIn("Tier A PASS", text)
        self.assertNotIn("regression_threshold", report)

    def use_scoped_observation_policy(self):
        """New constructed policy envelope; NOT a capture of new engine fields.

        The original native trace/stage captures stay intact. The generic queue
        and settings validators build the comparison-owned observation records;
        C++ doctests separately exercise the native exports and their wiring.
        """
        for route in ("chunk", "hlod"):
            arm = self.arm(route)
            queues = {}
            for owner in ("retained", "ready"):
                queues[owner] = {"owner": ("hlod_shared_reservation_ledger" if route == "hlod" else
                                          "payload_read_service" if owner == "retained" else "chunk_upload_window"),
                                 "bytes": 0, "peak_bytes": 1024, "byte_bound": 8192,
                                 "jobs": 0, "peak_jobs": 1, "job_bound": 16}
                absent = "job_bound" if route == "hlod" or owner == "retained" else "byte_bound"
                queues[owner][absent] = None
                queues[owner][absent + "_reason"] = "no independent limit at this native owner"
                if route == "hlod":
                    queues[owner].update(byte_bound=driver.HLOD_CONFIG["configured_read_byte_cap"],
                        peak_byte_bound=driver.HLOD_CONFIG["configured_read_byte_cap"], bound_violations=0)
            validated = ps.queue_observations_summary(queues, route=route)
            self.assertEqual(validated["status"], ps.OK)
            arm["observed"]["queues"] = {key: validated[key] for key in ("retained", "ready")}
            raw = copy.deepcopy(arm["probe"]["payload_telemetry"])
            raw.update(queue_observations=queues, stale_generation_publications=0,
                       digest_verification_failures=0, reads_refused_busy=0, admission_waits=0)
            saturation = {"reads_refused_busy": 0, "admission_waits": 0, "upload_hold_frames": 0}
            if route == "hlod":
                raw["upload_hold_frames"] = 0
                raw.update(driver.HLOD_CONFIG)
                raw.update(oversized_reservation_bytes=0, live_reservations=0)
            else:
                saturation["upload_hold_frames"] = {"measured": False, "applicable": False,
                    "scope": "chunk", "reason": "no HLOD upload hold on this route"}
                for key in driver.HLOD_ONLY_FAILURES:
                    arm["observed"]["failures"][key] = {"measured": False, "applicable": False,
                        "scope": "chunk", "reason": "no HLOD residency owner on this route"}
            arm["observed"]["saturation"] = saturation
            arm["native"] = {"payload_telemetry": raw}
            arm["requested"]["camera"] = copy.deepcopy(rp.CAMERA)
            arm["observed"]["settings"]["camera"] = copy.deepcopy(rp.CAMERA)
            if route == "hlod":
                settings = arm["observed"]["settings"]
                trace = ps.read_trace(Path(arm["trace"]["file"]))
                arm["native"]["shared_layers_settings_samples"] = [
                    {"row": index, **{key: copy.deepcopy(settings[key]) for key in (
                        "project_settings_readback", "node_readback", "camera", "resolution",
                        "sh_bands", "visible_splats", "drawn_splats")}} for index in range(len(trace.rows))]
        for arm in self.experiment["migration"]["arms"].values():
            arm["settings_readback"]["camera"] = copy.deepcopy(rp.CAMERA)

    def test_native_scoped_limits_and_hlod_row_settings_have_a_valid_path(self):
        self.use_scoped_observation_policy()
        self.assert_verdict("VALID")

    def test_scoped_queues_cannot_hide_missing_limits_or_changed_observations(self):
        for mutation in (lambda a: a["native"]["payload_telemetry"]["queue_observations"]["ready"].pop("byte_bound"),
                         lambda a: a["observed"]["queues"]["retained"].update(bytes=2),
                         lambda a: a["native"].pop("shared_layers_settings_samples"),
                         lambda a: a["native"]["shared_layers_settings_samples"].pop()):
            with self.scenario():
                self.use_scoped_observation_policy()
                mutation(self.arm())
                self.assert_verdict("UNUSABLE")

    def test_native_stable_cut_age_is_convergence_not_payload_saturation(self):
        self.use_scoped_observation_policy()
        self.arm()["observed"]["convergence"] = {"hlod_stream_stall_frames": 300}
        self.assert_verdict("VALID")
        self.arm()["native"]["payload_telemetry"]["admission_waits"] = 1
        self.arm()["observed"]["saturation"]["admission_waits"] = 1
        report = self.assert_verdict("VALID")
        self.assertEqual(report["metrics"]["hlod"]["io"]["admission_waits"]["value"], 1)

    def test_reintroduced_route_specific_cap_override_is_a_settings_mismatch(self):
        for key in ("configured_read_byte_cap", "normal_retained_byte_bound"):
            with self.scenario(key=key):
                self.use_scoped_observation_policy()
                self.arm()["native"]["payload_telemetry"][key] = 512 * 1024 * 1024
                self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_legal_single_oversized_bound_is_not_a_changed_config_or_escape(self):
        self.use_scoped_observation_policy()
        raw = self.arm()["native"]["payload_telemetry"]
        raw.update(oversized_reservation_bytes=12 * 1024 * 1024, live_reservations=1)
        for owner in ("retained", "ready"):
            queue = raw["queue_observations"][owner]
            queue.update(bytes=12 * 1024 * 1024, byte_bound=12 * 1024 * 1024,
                         peak_bytes=12 * 1024 * 1024, peak_byte_bound=12 * 1024 * 1024, jobs=1)
            self.arm()["observed"]["queues"][owner] = copy.deepcopy(queue)
        self.assert_verdict("VALID")
        raw.update(oversized_reservation_bytes=0, live_reservations=0)
        for owner in ("retained", "ready"):
            queue = raw["queue_observations"][owner]
            queue.update(bytes=0, byte_bound=driver.HLOD_CONFIG["normal_retained_byte_bound"], jobs=0)
            self.arm()["observed"]["queues"][owner] = copy.deepcopy(queue)
        self.assert_verdict("VALID")
        # A later escaped allocation must remain visible even below an earlier
        # legal oversized peak; it cannot be hidden by the historical maximum.
        for source in (self.arm()["native"]["payload_telemetry"]["queue_observations"], self.arm()["observed"]["queues"]):
            source["retained"]["bound_violations"] = 1
        self.assert_verdict("UNUSABLE", "queue_saturation")

    def progress_policy(self):
        """Constructed progress policy over the real producer's existing deadline."""
        for route in ("chunk", "hlod"):
            for settings in (self.arm(route)["requested"], self.arm(route)["observed"]["settings"]):
                settings["timing_windows"]["settle"] = copy.deepcopy(rp.SETTLE)
        for arm in self.experiment["migration"]["arms"].values():
            arm["settings_readback"]["timing_windows"]["settle"] = copy.deepcopy(rp.SETTLE)
        deadline = rp.SETTLE["stop_deadline_s"]
        samples = [{"row": index, "t_usec": int(index * deadline * 1_000_000), "route": "hlod", "measured": True,
            "demand_outstanding": True, "progress_counts": {"read_completions": 1, "pages_decoded": 1,
                "upload_completions": 0, "upload_bytes": 0}} for index in (0, 1)]
        record = {"measured": True, "window": "stop_deadline_s", "deadline_s": deadline,
                  "elapsed_usec": int(deadline * 1_000_000), "deadline_missed": True,
                  "samples": samples, "no_forward_progress": True}
        self.arm()["observed"]["forward_progress"] = [record]
        return record

    def test_no_forward_progress_for_existing_settle_deadline_is_unusable(self):
        record = self.progress_policy()
        report = self.assert_verdict("UNUSABLE", "queue_saturation")
        self.assertTrue(report["forward_progress"]["hlod"]["windows"][0]["no_forward_progress"])
        record["samples"][-1]["progress_counts"]["upload_bytes"] = 1
        record["no_forward_progress"] = False
        self.assert_verdict("VALID")  # Partial transfer progress also counts.

    def test_short_or_incomplete_progress_window_cannot_claim_a_stall(self):
        record = self.progress_policy()
        record["samples"][-1]["t_usec"] -= 1
        record["no_forward_progress"] = False
        self.assert_verdict("VALID")
        record["no_forward_progress"] = True
        self.assert_verdict("UNUSABLE", "invalid_metric")
        record.update(measured=False, reason="initial native snapshot absent")
        self.assert_verdict("UNUSABLE", "missing_metric")

    def test_separate_saturation_control_requires_native_held_ready_work(self):
        self.use_scoped_observation_policy()
        self.experiment["control"] = "saturate"
        control = copy.deepcopy(self.arm())
        control["native"]["payload_telemetry"]["upload_hold_frames"] = 1
        control["native"]["payload_telemetry"]["queue_observations"]["ready"]["bytes"] = 512
        self.experiment["control_receipt"] = {"status": "FORCED", "arm": control}
        self.assert_verdict("VALID", "queue_saturation")
        control["native"]["payload_telemetry"]["upload_hold_frames"] = 0
        self.assert_verdict("UNUSABLE", "control_not_observed")

    def test_corruption_control_proves_the_modified_v4_was_loaded_and_read(self):
        self.experiment["control"] = "corrupt"
        arm = copy.deepcopy(self.arm())
        corrupted_sha = _sha("actual corrupted policy copy")
        arm["source"]["loaded_file_sha256"] = corrupted_sha
        arm["probe"]["payload_telemetry"].update(captures_failed=1)
        self.experiment["control_receipt"] = {"status": "FORCED", "arm": arm,
            "source_sha256": self.experiment["prepared"]["tree_file_sha256"], "corrupt_sha256": corrupted_sha}
        self.assert_verdict("VALID", "payload_failure")
        arm["source"]["loaded_file_sha256"] = self.experiment["prepared"]["tree_file_sha256"]
        self.assert_verdict("UNUSABLE", "control_not_observed")

    def test_native_captures_without_independent_gpu_and_queue_observations_are_unusable(self):
        # A CPU-sink capture must never become a VALID paired GPU experiment.
        self.arm().pop("observed")
        self.assert_verdict("UNUSABLE", "missing_observation")

    def test_required_metric_table_has_every_route_and_area(self):
        report = driver.compare(self.experiment)
        self.assertEqual(set(report.get("metrics", {})), {"chunk", "hlod"})
        for route in ("chunk", "hlod"):
            for area, keys in METRICS.items():
                for key in keys:
                    with self.subTest(route=route, area=area, metric=key):
                        rec = report["metrics"][route][area][key]
                        self.assertIsInstance(rec.get("measured"), bool)
                        if not rec["measured"]:
                            self.assertTrue(rec.get("reason"))
                            self.assertIsNone(rec.get("value"))

    def test_metric_values_come_from_real_route_producer_and_time_weighted_helper(self):
        report = driver.compare(self.experiment)
        self.assertIn("hlod", report.get("metrics", {}))
        for route in ("chunk", "hlod"):
            native = self.arm(route)["probe"]
            metrics = report["metrics"][route]
            self.assertEqual(metrics["io"]["stored_bytes"]["value"], native["payload_telemetry"]["stored_bytes_read"])
            for q in (95, 99):
                self.assertEqual(metrics["performance"][f"moving_frame_p{q}_ms"]["value"],
                                 native["passes"][0]["moving_frame_interval"][f"p{q}_time_weighted"]["value"])

    def test_missing_arm_is_not_run_and_never_valid(self):
        for route in ("chunk", "hlod"):
            with self.scenario(route=route):
                self.experiment["arms"].pop(route)
                self.assert_verdict("NOT_RUN", "missing_arm")

    def test_declared_unexecuted_arm_is_not_run(self):
        self.arm()["execution"] = "NOT_RUN"
        self.assert_verdict("NOT_RUN", "missing_arm")

    def test_source_provenance_mismatches_are_unusable(self):
        for key in ("input_file_sha256", "generator_identity", "codec", "tree_file_sha256", "tree_identity", "loaded_file_sha256"):
            for route in ("chunk", "hlod"):
                with self.scenario(route=route, key=key):
                    source = self.arm(route)["source"]
                    source[key] = "different"
                    self.assert_verdict("UNUSABLE", "provenance_mismatch")

    def test_matching_wrong_provenance_is_checked_against_the_prepared_input(self):
        for route in ("chunk", "hlod"):
            self.arm(route)["source"]["input_file_sha256"] = _sha("same wrong input in both arms")
        self.assert_verdict("UNUSABLE", "provenance_mismatch")

    def test_missing_source_identity_is_unusable(self):
        for key in ("input_file_sha256", "generator_identity", "codec", "tree_file_sha256", "tree_identity"):
            with self.scenario(key=key):
                self.arm()["source"].pop(key)
                self.assert_verdict("UNUSABLE", "provenance_mismatch")

    def test_wrong_t0_size_or_extent_is_unusable(self):
        for key, value in (("splat_count", 262144), ("extent_m", [128, 128]), ("tier", "A")):
            with self.scenario(key=key):
                self.experiment["prepared"][key] = value
                self.assert_verdict("UNUSABLE", "wrong_t0")

    def test_missing_failed_or_partial_w13_is_unusable(self):
        for w13 in (None, {"available": False, "reason": "not run"},
                    {"available": True, "passed": False},
                    {"available": True, "passed": True, "node_count": 2, "nodes_verified": 1}):
            with self.scenario(w13=w13):
                self.experiment["prepared"]["w13"] = w13
                self.assert_verdict("UNUSABLE", "w13_unproven")

    def test_every_fixed_setting_must_be_read_back_and_match(self):
        for key in tuple(self.arm()["observed"]["settings"]):
            for missing in (True, False):
                with self.scenario(key=key, missing=missing):
                    settings = self.arm()["observed"]["settings"]
                    if missing:
                        settings.pop(key)
                    else:
                        settings[key] = {"different": True}
                    self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_matching_readbacks_cannot_override_the_frozen_request(self):
        for route in ("chunk", "hlod"):
            self.arm(route)["observed"]["settings"]["drawn_splats"] = 1_000_000
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_settings_drift_during_measured_frames_is_unusable(self):
        self.arm()["probe"]["passes"][0]["settings_measured"] = {
            "status": ps.INVALID, "settings_drifted": True, "reason": "mutated held frame"}
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_requested_route_does_not_override_actual_route(self):
        for route, wrong in (("chunk", "hlod"), ("hlod", "chunk")):
            with self.scenario(route=route):
                self.arm(route)["probe"]["payload_telemetry"]["route"] = wrong
                self.assert_verdict("UNUSABLE", "wrong_route")

    def test_hlod_requires_paged_streaming_without_materialized_fallback(self):
        for key, bad in (("hlod_stream_paged", False), ("hlod_stream_page_store_active", False),
                         ("resident_materialized_fallback", True)):
            with self.scenario(key=key):
                runtime = self.arm()["observed"]["runtime"]
                runtime[key] = bad
                self.assert_verdict("UNUSABLE", "resident_fallback")

    def test_missing_trace_is_unusable_for_an_executed_arm(self):
        Path(self.arm()["trace"]["file"]).unlink()
        self.assert_verdict("UNUSABLE", "missing_trace")

    def test_torn_trace_cannot_trust_a_precomputed_ok_summary(self):
        with Path(self.arm()["trace"]["file"]).open("ab") as stream:
            stream.write(b"torn")
        self.assert_verdict("UNUSABLE", "invalid_trace")

    def test_empty_trace_is_unusable(self):
        Path(self.arm()["trace"]["file"]).write_bytes(b"")
        self.assert_verdict("UNUSABLE", "invalid_trace")

    def test_missing_engine_frame_cannot_trust_precomputed_trace_coverage(self):
        path = Path(self.arm()["trace"]["file"])
        trace = ps.read_trace(path)
        payload = path.read_bytes()
        header_end = 12 + struct.unpack_from("<I", payload, 8)[0]
        stride = struct.calcsize("<" + trace.types)
        # Delete a real captured row, retaining a syntactically valid trace.
        path.write_bytes(payload[:header_end + stride] + payload[header_end + 2 * stride:])
        self.assert_verdict("UNUSABLE", "invalid_trace")

    def test_missing_telemetry_is_unusable(self):
        for route in ("chunk", "hlod"):
            with self.scenario(route=route):
                self.arm(route)["probe"].pop("payload_telemetry")
                self.assert_verdict("UNUSABLE", "missing_telemetry")

    def test_required_metric_removal_cannot_trust_cached_ok_status(self):
        for route, keys in (("chunk", ("read_requests", "decode_nsec", "captures_on_worker_threads")),
                            ("hlod", ("stored_bytes_read", "pack_nsec", "ready_bytes_peak", "upload_slices"))):
            for key in keys:
                with self.scenario(route=route, key=key):
                    block = self.arm(route)["probe"]["payload_telemetry"]
                    block.pop(key)
                    self.assertEqual(block["status"], ps.OK)
                    self.assert_verdict("UNUSABLE", "missing_metric")

    def test_stale_epoch_and_source_generation_are_unusable(self):
        for route in ("chunk", "hlod"):
            for key in ("collection_epoch", "source_generation"):
                with self.scenario(route=route, key=key):
                    block = self.arm(route)["probe"]["payload_telemetry"]
                    block[key] += 1
                    self.assert_verdict("UNUSABLE", "stale_generation")

    def test_missing_independent_live_generation_is_unusable(self):
        self.arm()["observed"].pop("source_generation")
        self.assert_verdict("UNUSABLE", "stale_generation")

    def test_nonzero_read_decode_and_capture_failures_are_unusable(self):
        for route in ("chunk", "hlod"):
            for key in ("read_failures", "page_failures", "captures_failed"):
                with self.scenario(route=route, key=key):
                    block = self.arm(route)["probe"]["payload_telemetry"]
                    block[key] = 1
                    self.assert_verdict("UNUSABLE", "payload_failure")

    def test_nonzero_publication_nonresident_overflow_and_drop_counters_are_unusable(self):
        for route in ("chunk", "hlod"):
            for key in tuple(self.arm(route)["observed"]["failures"]):
                with self.scenario(route=route, key=key):
                    failures = self.arm(route)["observed"]["failures"]
                    failures[key] = 1
                    self.assert_verdict("UNUSABLE", "correctness_failure")

    def test_absent_failure_counter_is_not_zero(self):
        for key in tuple(self.arm()["observed"]["failures"]):
            with self.scenario(key=key):
                self.arm()["observed"]["failures"].pop(key)
                self.assert_verdict("UNUSABLE", "missing_metric")

    def test_nonzero_process_exit_or_missing_final_marker_is_unusable(self):
        for key, bad in (("exit_code", 1), ("final_marker", False)):
            with self.scenario(key=key):
                self.arm()["observed"][key] = bad
                self.assert_verdict("UNUSABLE", "process_failure")

    def test_no_gpu_resident_drawn_data_is_unusable(self):
        self.arm()["observed"]["runtime"]["gpu_resident_drawn_nodes"] = 0
        self.assert_verdict("UNUSABLE", "no_gpu_residency")

    def test_zero_hlod_reads_or_gsq_work_is_unusable(self):
        for key in ("stored_bytes_read", "read_requests", "pages_decoded", "splats_decoded", "captures_on_worker_threads"):
            with self.scenario(key=key):
                block = self.arm()["probe"]["payload_telemetry"]
                block[key] = 0
                self.assert_verdict("UNUSABLE", "no_payload_work")

    def test_queue_bytes_and_jobs_above_bound_are_unusable_even_after_drain(self):
        for owner in ("ready", "retained"):
            for key, bound in (("bytes", "byte_bound"), ("peak_bytes", "byte_bound"),
                               ("jobs", "job_bound"), ("peak_jobs", "job_bound")):
                with self.scenario(owner=owner, key=key):
                    queue = self.arm()["observed"]["queues"][owner]
                    queue[key] = queue[bound] + 1
                    self.assert_verdict("UNUSABLE", "queue_saturation")

    def test_full_but_bounded_queues_with_admission_waits_are_valid(self):
        for owner in ("retained", "ready"):
            queue = self.arm()["observed"]["queues"][owner]
            queue.update(bytes=queue["byte_bound"], peak_bytes=queue["byte_bound"],
                         jobs=queue["job_bound"], peak_jobs=queue["job_bound"])
        self.arm()["observed"]["saturation"].update(admission_waits=9, reads_refused_busy=3)
        self.assert_verdict("VALID")

    def test_missing_queue_bound_is_unusable(self):
        for owner in ("ready", "retained"):
            for key in ("bytes", "peak_bytes", "byte_bound", "jobs", "peak_jobs", "job_bound"):
                with self.scenario(owner=owner, key=key):
                    self.arm()["observed"]["queues"][owner].pop(key)
                    self.assert_verdict("UNUSABLE", "missing_metric")

    def test_held_ready_observation_is_unusable(self):
        self.arm()["observed"]["saturation"]["upload_hold_frames"] = 1
        self.assert_verdict("UNUSABLE", "queue_saturation")

    def test_nonfinite_or_negative_counter_is_unusable(self):
        for value in (-1, float("nan"), float("inf"), True):
            with self.scenario(value=value):
                self.arm()["probe"]["payload_telemetry"]["read_failures"] = value
                self.assert_verdict("UNUSABLE", "invalid_metric")

    def test_corrupt_control_is_red_from_observed_verification_failure(self):
        self.experiment["control"] = "corrupt"
        # Fault receipt in the new protocol; producer telemetry mutation below
        # models the observable failed stored/decode verification, not a success.
        self.arm()["probe"]["payload_telemetry"].update(
            page_failures=1, captures_failed=1, last_failure_reason="stored digest mismatch")
        self.assert_verdict("UNUSABLE", "payload_failure")

    def test_decoded_digest_failure_is_unusable(self):
        self.arm()["observed"]["failures"]["digest_verification_failures"] = 1
        self.assert_verdict("UNUSABLE", "correctness_failure")

    def test_saturate_control_is_red_from_observed_queue_bound(self):
        self.experiment["control"] = "saturate"
        queue = self.arm()["observed"]["queues"]["ready"]
        queue["byte_bound"] = queue["peak_bytes"] - 1
        self.assert_verdict("UNUSABLE", "queue_saturation")

    def test_control_label_alone_does_not_prove_a_negative_control(self):
        for control in ("corrupt", "saturate"):
            with self.scenario(control=control):
                self.experiment["control"] = control
                self.assert_verdict("UNUSABLE", "control_not_observed")

    def test_missing_or_failed_screenshots_are_unusable(self):
        for value in (None, rp.screenshot_record(None),
                      rp.screenshot_record({"dir": "screenshots", "taken": []}),
                      rp.screenshot_record({"dir": "screenshots", "taken": [{"error": "readback failed"}]})):
            with self.scenario(shots=value):
                self.arm()["probe"]["screenshots"] = value
                self.assert_verdict("UNUSABLE", "missing_screenshots")

    def test_screenshot_claim_without_file_is_unusable(self):
        (self.tmp / "hlod/screenshots/moving.png").unlink()
        self.assert_verdict("UNUSABLE", "missing_screenshots")

    def test_independent_cold_preparation_and_repeat_labels_are_required(self):
        for key, value in (("independently_prepared", False), ("label", "warm"), ("repeat_label", None)):
            with self.scenario(key=key):
                self.arm()["cache"][key] = value
                self.assert_verdict("UNUSABLE", "cache_unproven")

    def test_missing_migration_control_is_not_run(self):
        for key in (None, "base", "candidate"):
            with self.scenario(key=key):
                if key is None:
                    self.experiment["migration"] = None
                else:
                    self.experiment["migration"]["arms"].pop(key)
                self.assert_verdict("NOT_RUN", "missing_migration")

    def test_migration_requires_immutable_pred3_and_candidate_binary_identities(self):
        for name, value in (("base", CANDIDATE), ("candidate", PRE_D3)):
            with self.scenario(name=name):
                arm = self.experiment["migration"]["arms"][name]
                arm["binary_commit"] = value
                self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_migration_requires_two_measured_binary_digests(self):
        for absent in (True, False):
            with self.scenario(absent=absent):
                base = self.experiment["migration"]["arms"]["base"]
                if absent:
                    base.pop("binary_sha256")
                else:
                    base["binary_sha256"] = self.experiment["migration"]["arms"]["candidate"]["binary_sha256"]
                self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_migration_requires_exactly_the_same_v4_payload_and_settings(self):
        for key, value in (("input_file_sha256", self.experiment["prepared"]["input_file_sha256"]),
                           ("settings_readback", {"drawn_splats": 1_000_000})):
            with self.scenario(key=key):
                arm = self.experiment["migration"]["arms"]["base"]
                arm[key] = value
                self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_migration_cannot_be_a_success_record_without_execution_or_trace(self):
        for key, value in (("execution", "NOT_RUN"), ("exit_code", 1), ("final_marker", False),
                           ("trace_file", str(self.tmp / "absent-migration.bin"))):
            with self.scenario(key=key):
                arm = self.experiment["migration"]["arms"]["base"]
                arm[key] = value
                self.assert_verdict("NOT_RUN" if key == "execution" else "UNUSABLE", "migration_unproven")

    def test_unmeasured_values_and_zero_vram_are_never_measured_zero(self):
        report = driver.compare(self.experiment)
        self.assertIn("hlod", report.get("metrics", {}))
        for route in ("chunk", "hlod"):
            for area, key in (("performance", "device_ledger_vram_bytes"), ("performance", "external_vram_bytes"),
                              ("cpu_memory", "os_peak_rss_bytes"), ("decode_pack", "digest_ns")):
                rec = report["metrics"][route][area][key]
                self.assertIs(rec["measured"], False, rec)
                self.assertIsNone(rec.get("value"), rec)
                self.assertTrue(rec.get("reason"), rec)

    def test_markdown_renders_not_measured_with_reason_never_zero(self):
        # New driver report format, deliberately prescribed here (not a fixture
        # purporting to come from an existing producer).
        report = {"verdict": "VALID", "report_only": True, "reasons": [], "metrics": {
            route: {area: {key: {"measured": False, "reason": "measurement unavailable"} for key in keys}
                    for area, keys in METRICS.items()} for route in ("chunk", "hlod")}}
        text = driver.render_markdown(report)
        self.assertIn("not measured", text.lower())
        self.assertIn("measurement unavailable", text)
        self.assertNotRegex(text, r"\|\s*0(?:\.0+)?(?:\s*(?:MB|ms|bytes))?\s*\|")

    def test_cli_writes_machine_report_and_human_report(self):
        source = self.tmp / "experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "comparison"
        self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out)]), 0)
        self.assertTrue((out / "comparison.json").is_file(), "CLI must invoke comparison and write its report")
        report = json.loads((out / "comparison.json").read_text(encoding="utf-8"))
        self.assertEqual(report["verdict"], "VALID")
        self.assertTrue((out / "comparison.md").is_file())

    def test_cli_unusable_evidence_is_nonzero_and_still_writes_report(self):
        self.arm()["observed"]["runtime"]["gpu_resident_drawn_nodes"] = 0
        source = self.tmp / "experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "comparison"
        self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out)]), 2)
        self.assertTrue((out / "comparison.json").is_file())
        self.assertEqual(json.loads((out / "comparison.json").read_text())["verdict"], "UNUSABLE")

    def test_skip_compare_control_missing_report_fails_closed(self):
        report = self.tmp / "comparison.json"
        # The workflow invokes ONLY finalization, just as when the compare step
        # has been deleted; no mocked comparison or hand-written success file.
        with mock.patch.object(driver, "compare", wraps=driver.compare) as compare:
            code = driver.main(["--require-report", str(report), "--run-id", self.experiment["run_id"],
                                "--control", "skip_compare"])
        compare.assert_not_called()
        self.assertEqual(code, 3, "no comparison invocation must remain NOT_RUN, never VALID")

    def test_final_verdict_rejects_missing_malformed_stale_or_unusable_report(self):
        path = self.tmp / "comparison.json"
        for content in ("{", "{}", json.dumps({"verdict": "VALID", "run_id": "old-run", "control": "none"}),
                        json.dumps({"verdict": "VALID", "run_id": self.experiment["run_id"], "control": "none"}),
                        json.dumps({"verdict": "UNUSABLE", "run_id": self.experiment["run_id"], "control": "none"})):
            with self.subTest(content=content):
                path.write_text(content, encoding="utf-8")
                self.assertNotEqual(driver.main(["--require-report", str(path), "--run-id", self.experiment["run_id"]]), 0)

    def test_final_verdict_accepts_only_the_current_real_comparison_report(self):
        source = self.tmp / "experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "comparison"
        driver.main(["--experiment", str(source), "--out", str(out)])
        report = out / "comparison.json"
        self.assertTrue(report.is_file())
        self.assertEqual(driver.main(["--require-report", str(report), "--run-id", self.experiment["run_id"]]), 0)
        self.assertNotEqual(driver.main(["--require-report", str(report), "--run-id", self.experiment["run_id"],
                                         "--control", "skip_compare"]), 0)

    def test_migration_common_behavior_cannot_be_omitted_or_have_payload_failures(self):
        for missing in (True, False):
            with self.scenario(missing=missing):
                base = self.experiment["migration"]["arms"]["base"]
                if missing:
                    base.pop("payload_behavior")
                    self.assert_verdict("UNUSABLE", "migration_unproven")
                else:
                    base["payload_behavior"]["native_payload"]["hlod_stream_payload_read_failures_total"] = 1
                    self.assert_verdict("UNUSABLE", "payload_failure")

    def use_real_migration_observations(self):
        for name, arm in self.experiment["migration"]["arms"].items():
            native = self.migration_capture["arms"][name]
            behavior = copy.deepcopy(native["payload_behavior"])
            # Source receipts are constructed policy inputs in this envelope;
            # only the observations come from the real run. Keep that explicit.
            behavior["loaded_file_sha256"] = self.experiment["prepared"]["tree_file_sha256"]
            behavior["native_payload"] = copy.deepcopy(native["final_render_stats"])
            behavior["world_payload"] = copy.deepcopy(native["world_payload_readback"])
            arm["payload_behavior"] = behavior

    def test_real_same_binary_variation_is_reported_without_claiming_draw_identity(self):
        self.use_real_migration_observations()
        report = self.assert_verdict("UNUSABLE", "content_unproven")
        self.assertIn("migration_identity_mismatch", [r["code"] for r in report["reasons"]])
        observations = report["migration_observations"]
        self.assertEqual([(r["pass_index"], r["checkpoint"], r["candidate_minus_base"])
                          for r in observations["draw_differences"]], [(0, "C2", 2631), (1, "C7", -248069)])
        self.assertTrue(all(r["within_binary_variation_observed"] for r in observations["draw_differences"]))
        self.assertEqual([r["assessment"] for r in observations["draw_differences"]], ["mismatch", "inconclusive"])
        self.assertEqual({(r["arm"], r["checkpoint"]) for r in observations["within_binary_changes"]},
                         {("base", "C7"), ("candidate", "C2")})
        reads = observations["native_payload"]["hlod_stream_payload_reads_total"]
        self.assertEqual(reads["candidate_minus_base"]["value"], -11)
        self.assertIs(observations["decode"]["base"]["decoded_digest"]["measured"], False)
        self.assertIn("Same-binary cold/repeat draw observation", driver.render_markdown(report))

    def test_self_consistent_cross_binary_draw_difference_still_fails(self):
        self.use_real_migration_observations()
        for row in self.experiment["migration"]["arms"]["candidate"]["payload_behavior"]["drawn_counts"]:
            if row["checkpoint"] == "C1":
                row["drawn_splats"] += 1  # Same difference in both passes.
        self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_missing_or_zero_common_payload_work_is_unproven(self):
        for key in driver.MIGRATION_WORK:
            for value in (None, 0, ps.missing("not exported")):
                with self.scenario(key=key, value=value):
                    self.experiment["migration"]["arms"]["base"]["payload_behavior"]["native_payload"][key] = value
                    self.assert_verdict("UNUSABLE", "migration_unproven")

    def test_common_payload_failures_cannot_be_hidden_by_draw_variation(self):
        for key in driver.MIGRATION_FAILURES:
            with self.scenario(key=key):
                self.use_real_migration_observations()
                self.experiment["migration"]["arms"]["base"]["payload_behavior"]["native_payload"][key] = 1
                self.assert_verdict("UNUSABLE", "payload_failure")

    def test_two_migration_arms_cannot_share_the_same_missing_checkpoint(self):
        for arm in self.experiment["migration"]["arms"].values():
            arm["payload_behavior"]["drawn_counts"].pop()
        self.assert_verdict("UNUSABLE", "migration_unproven")

    def test_missing_common_payload_failure_is_not_zero(self):
        for key in driver.MIGRATION_FAILURES:
            with self.scenario(key=key):
                self.experiment["migration"]["arms"]["base"]["payload_behavior"]["native_payload"].pop(key)
                self.assert_verdict("UNUSABLE", "migration_unproven")

    def test_migration_world_shape_and_page_capacity_are_required(self):
        for key in ("world_payload", "route_evidence"):
            with self.scenario(key=key):
                self.experiment["migration"]["arms"]["base"]["payload_behavior"].pop(key)
                self.assert_verdict("UNUSABLE", "migration_unproven")
        with self.scenario(key="capacity escape"):
            native = self.experiment["migration"]["arms"]["base"]["payload_behavior"]["native_payload"]
            native["hlod_stream_pages_resident"] = native["hlod_stream_pages_capacity"] + 1
            self.assert_verdict("UNUSABLE", "payload_failure")

    def test_runtime_content_contract_requires_independent_per_node_match(self):
        # Synthetic contract coverage complements the complete captured-map
        # regression below; these values are not claimed as producer output.
        for route in ("chunk", "hlod", "candidate"):
            with self.scenario(route=route):
                digests = (self.arm(route)["content_digests"] if route != "candidate" else
                           self.experiment["migration"]["arms"][route]["payload_behavior"]["content_digests"])
                digests["0"], digests["1"] = digests["1"], digests["0"]
                self.assert_verdict("UNUSABLE", "content_mismatch")

    def use_captured_content_digests(self):
        """Only digest observations are real here; other preconditions stay synthetic."""
        capture = self.content_capture
        self.experiment["prepared"].update(copy.deepcopy(capture["prepared"]))
        for route in ("chunk", "hlod"):
            arm = self.arm(route)
            arm["content_digests"] = copy.deepcopy(capture["arms"][route]["content_digests"])
            # Keep the existing synthetic-envelope telemetry preconditions, and
            # cross-check the real native map rather than duplicating the arm's
            # comparison-envelope map as its own supposed producer evidence.
            telemetry = copy.deepcopy(arm["probe"]["payload_telemetry"])
            telemetry["content_digests"] = copy.deepcopy(
                capture["arms"][route]["native"]["payload_telemetry"]["content_digests"])
            arm["native"] = {"payload_telemetry": telemetry}
        for name in ("base", "candidate"):
            self.experiment["migration"]["arms"][name]["payload_behavior"]["content_digests"] = copy.deepcopy(
                capture["migration"]["arms"][name]["payload_behavior"]["content_digests"])

    def test_captured_runtime_content_digests_match_preparation(self):
        self.use_captured_content_digests()
        report = self.assert_verdict("VALID")
        for route, count in (("chunk", 1284), ("hlod", 614)):
            identity = report["content_identity"][route]
            self.assertEqual(identity["status"], "MATCHED")
            self.assertEqual(identity["nodes_checked"], count)
            self.assertEqual(identity["reference_nodes"], count)
        migration = report["migration"]["content_identity"]
        self.assertEqual(migration["status"], "NOT_MEASURED")
        self.assertEqual(migration["base"]["status"], "NOT_MEASURED")
        self.assertEqual(migration["candidate"]["status"], "MATCHED")
        self.assertEqual(migration["candidate"]["nodes_checked"], 614)
        self.assertEqual(migration["candidate"]["reference_nodes"], 614)
        self.assertIsNone(self.content_capture["migration"]["arms"]["base"]["payload_behavior"]["content_digests"])

        for route, swap in (("hlod", True), ("chunk", False)):
            with self.scenario(route=route, swap=swap):
                digests = self.arm(route)["content_digests"]
                first = next(iter(digests))
                second = next(node for node, digest in digests.items() if digest != digests[first])
                replacement = {first: digests[second]}
                if swap:
                    replacement[second] = digests[first]
                # Malformed-input mutations use only captured values. Change
                # both copies of the observed runtime map so preparation, not
                # merely disagreement between duplicate exports, catches it.
                digests.update(replacement)
                self.arm(route)["native"]["payload_telemetry"]["content_digests"].update(replacement)
                bad = self.assert_verdict("UNUSABLE", "content_mismatch")
                self.assertTrue(any(r["code"] == "content_mismatch" and r.get("arm") == route
                                    for r in bad["reasons"]))
                self.assertEqual(bad["content_identity"][route]["status"], "MISMATCH")
                self.assertEqual(bad["content_identity"][route]["mismatched_nodes"], sorted(replacement))
        with self.scenario(mutation="missing migration candidate runtime map"):
            self.experiment["migration"]["arms"]["candidate"]["payload_behavior"].pop("content_digests")
            bad = self.assert_verdict("UNUSABLE", "content_unproven")
            self.assertTrue(any(r["code"] == "content_unproven" and r.get("arm") == "candidate"
                                for r in bad["reasons"]))
            self.assertEqual(bad["migration"]["content_identity"]["candidate"]["status"], "NOT_MEASURED")

    def test_captured_candidate_only_c4_c7_draw_jitter_is_rejected(self):
        self.use_captured_content_digests()
        for name in ("base", "candidate"):
            self.experiment["migration"]["arms"][name]["payload_behavior"]["drawn_counts"] = copy.deepcopy(
                self.content_capture["migration"]["arms"][name]["payload_behavior"]["drawn_counts"])
        report = self.assert_verdict("UNUSABLE", "migration_identity_mismatch")
        differences = report["migration_observations"]["draw_differences"]
        self.assertEqual({(d["checkpoint"], d["pass_index"], d["assessment"]) for d in differences},
                         {("C4", 1, "mismatch"), ("C7", 1, "mismatch")})
        for checkpoint in ("C4", "C7"):
            for name, distinct in (("base", 1), ("candidate", 2)):
                rows = self.content_capture["migration"]["arms"][name]["payload_behavior"]["drawn_counts"]
                self.assertEqual(len({row["drawn_splats"] for row in rows if row["checkpoint"] == checkpoint}), distinct)
        self.assertEqual(report["content_identity"]["chunk"]["status"], "MATCHED")
        self.assertEqual(report["content_identity"]["hlod"]["status"], "MATCHED")
        self.assertEqual(report["migration"]["content_identity"]["candidate"]["status"], "MATCHED")

    def test_content_evidence_absence_or_malformed_cannot_pass(self):
        for route in ("chunk", "hlod", "candidate"):
            for value in (None, {}, [], {"0": "not a digest"}, {"unknown": _sha("bytes")}):
                with self.scenario(route=route, value=value):
                    arm = (self.arm(route) if route != "candidate" else
                           self.experiment["migration"]["arms"][route]["payload_behavior"])
                    arm["content_digests"] = value
                    self.assert_verdict("UNUSABLE", "content_mismatch" if isinstance(value, dict) and "unknown" in value
                                        else "content_unproven")

    def test_reference_cannot_be_missing_or_arm_self_certified(self):
        for value in (None, {}, {"hlod": {"0": "malformed"}}):
            with self.scenario(value=value):
                self.experiment["prepared"]["content_references"] = value
                self.assert_verdict("UNUSABLE", "content_reference_unproven")

    def test_native_content_conflict_or_rewritten_export_cannot_pass(self):
        with self.scenario(mutation="conflicting repeat"):
            self.arm()["content_digests"]["0"] = "conflicting_runtime_digests"
            self.assert_verdict("UNUSABLE", "content_mismatch")
        with self.scenario(mutation="rewritten native export"):
            self.use_scoped_observation_policy()
            self.arm()["native"]["payload_telemetry"]["content_digests"] = {"0": _sha("different native bytes")}
            self.assert_verdict("UNUSABLE", "content_mismatch")

    def test_decoded_subset_is_verified_by_identity_without_requiring_all_nodes(self):
        self.arm()["content_digests"].pop("1")
        report = self.assert_verdict("VALID")
        self.assertEqual(report["content_identity"]["hlod"]["nodes_checked"], 1)

    def test_migration_content_scope_explicitly_excludes_unobservable_base(self):
        report = self.assert_verdict("VALID")
        identity = report["migration"]["content_identity"]
        self.assertEqual(identity["status"], "NOT_MEASURED")
        self.assertEqual(identity["candidate"]["status"], "MATCHED")
        self.assertIn("NOT_MEASURED", driver.render_markdown(report))

    def test_candidate_c7_one_is_outside_observed_base_variation(self):
        self.use_real_migration_observations()
        for row in self.experiment["migration"]["arms"]["candidate"]["payload_behavior"]["drawn_counts"]:
            if row["checkpoint"] == "C7":
                row["drawn_splats"] = 1
        report = self.assert_verdict("UNUSABLE", "migration_identity_mismatch")
        self.assertTrue(any(r["checkpoint"] == "C7" and r["assessment"] == "mismatch"
                            for r in report["migration_observations"]["draw_differences"]))

    def test_stable_base_candidate_only_c2_jitter_is_rejected(self):
        self.use_real_migration_observations()
        self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_candidate_inside_base_range_is_visible_inconclusive(self):
        for row in self.experiment["migration"]["arms"]["candidate"]["payload_behavior"]["drawn_counts"]:
            if row["checkpoint"] == "C7":
                row["drawn_splats"] = 400_000
        report = self.assert_verdict("VALID")
        differences = report["migration_observations"]["draw_differences"]
        self.assertTrue(differences)
        self.assertTrue(all(d["assessment"] == "inconclusive" for d in differences))
        self.assertIn("inconclusive", driver.render_markdown(report))

    def set_detected_control(self, control):
        if control == "saturate":
            self.use_scoped_observation_policy()
        self.experiment["control"] = control
        arm = copy.deepcopy(self.arm())
        receipt = {"status": "FORCED", "arm": arm}
        if control == "corrupt":
            receipt.update(source_sha256=self.experiment["prepared"]["tree_file_sha256"],
                           corrupt_sha256=_sha("changed V4 bytes"))
            arm["source"]["loaded_file_sha256"] = receipt["corrupt_sha256"]
            arm["probe"]["payload_telemetry"].update(page_failures=1)
        else:
            arm["native"]["payload_telemetry"]["upload_hold_frames"] = 1
        self.experiment["control_receipt"] = receipt
        return arm

    def write_and_require(self, control):
        source = self.tmp / "control-experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "control-comparison"
        code = driver.main(["--experiment", str(source), "--out", str(out)])
        final = driver.main(["--require-report", str(out / "comparison.json"),
                             "--run-id", self.experiment["run_id"], "--control", control])
        return code, final, out / "comparison.json"

    def test_detected_controls_succeed_only_with_bound_usable_main_arms(self):
        for control in ("corrupt", "saturate"):
            with self.scenario(control=control):
                self.set_detected_control(control)
                report = self.assert_verdict("VALID")
                self.assertEqual(report["control_verdict"], "DETECTED")
                code, final, path = self.write_and_require(control)
                self.assertEqual((code, final), (0, 0))
                self.assertNotEqual(driver.main(["--require-report", str(path), "--run-id", "another-run",
                                                "--control", control]), 0)
                self.assertNotEqual(driver.main(["--require-report", str(path), "--run-id", self.experiment["run_id"],
                                                "--control", "saturate" if control == "corrupt" else "corrupt"]), 0)
                self.arm("chunk")["content_digests"] = {}
                self.assert_verdict("UNUSABLE", "content_unproven")

    def test_control_crash_missing_trace_and_unobserved_cannot_succeed(self):
        for control in ("corrupt", "saturate"):
            for mutation in ("crash", "trace", "not_run", "unobserved"):
                with self.scenario(control=control, mutation=mutation):
                    arm = self.set_detected_control(control)
                    if mutation == "crash":
                        arm["observed"]["exit_code"] = 1
                    elif mutation == "trace":
                        arm["trace"]["file"] = "absent.trace.bin"
                    elif mutation == "not_run":
                        self.experiment["control_receipt"]["status"] = "NOT_RUN"
                    else:
                        arm["probe"]["payload_telemetry"]["page_failures"] = 0
                        arm["observed"]["saturation"]["upload_hold_frames"] = 0
                        if control == "saturate":
                            arm["native"]["payload_telemetry"]["upload_hold_frames"] = 0
                    report = driver.compare(self.experiment)
                    self.assertNotEqual(report["verdict"], "VALID")
                    self.assertEqual(report["control_verdict"], "NOT_RUN" if mutation == "not_run" else "NOT_DETECTED")
                    code, final, _ = self.write_and_require(control)
                    self.assertEqual((code, final), (3, 3) if mutation == "not_run" else (2, 2))

    def test_saturate_control_hold_without_native_ready_work_is_not_detected(self):
        arm = self.set_detected_control("saturate")
        arm["native"]["payload_telemetry"].pop("queue_observations")
        arm["observed"]["saturation"]["upload_hold_frames"] = 1
        report = self.assert_verdict("UNUSABLE", "control_not_observed")
        self.assertEqual(report["control_verdict"], "NOT_DETECTED")

    def test_detected_control_never_launders_main_arm_drops(self):
        for control in ("corrupt", "saturate"):
            with self.scenario(control=control):
                self.set_detected_control(control)
                self.arm("chunk")["observed"]["failures"]["drop_events"] = 1
                report = self.assert_verdict("UNUSABLE", "correctness_failure")
                self.assertEqual(report["control_verdict"], "DETECTED")

    def test_real_overlap_record_loss_keeps_each_route_unusable(self):
        capture = _captured("d3_6/run43602195987_drops.json")
        for route, data in capture["arms"].items():
            with self.scenario(route=route):
                self.arm(route)["observed"]["failures"]["drop_events"] = data["whole_run"]["drop_events"]
                report = self.assert_verdict("UNUSABLE", "correctness_failure")
                self.assertEqual(report["metrics"][route]["correctness"]["drop_events"]["value"],
                                 data["whole_run"]["drop_events"])

    def test_actual_camera_application_must_be_present_and_match(self):
        for value in (None, {"matches": False, "reason": "changed camera path"}):
            with self.scenario(value=value):
                self.arm()["observed"]["camera_application"] = value
                self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_corruption_receipt_is_checked_without_damaging_clean_measurements(self):
        arm = self.set_detected_control("corrupt")
        arm["probe"]["payload_telemetry"].update(page_failures=1, last_failure_reason="stored digest mismatch")
        self.assert_verdict("VALID", "payload_failure")
        self.assertEqual(self.arm()["probe"]["payload_telemetry"]["page_failures"], 0)

    def test_unavailable_saturation_hook_is_distinct_not_run(self):
        self.experiment["control"] = "saturate"
        self.experiment["control_receipt"] = {"status": "NOT_RUN", "reason": "no script binding for upload hold"}
        self.assert_verdict("NOT_RUN", "control_not_run")

    def test_failed_native_capture_keeps_failure_proof_through_lossy_summary(self):
        arm = self.set_detected_control("corrupt")
        raw = arm["probe"]["payload_telemetry"]
        raw.update(page_failures=1, captures_failed=1, last_failure_reason="stored digest mismatch")
        summary = ps.route_telemetry_summary(raw, route="hlod", source_generation=arm["observed"]["source_generation"])
        self.assertEqual(summary["status"], ps.INVALID)
        self.assertNotIn("page_failures", summary, "real validator deliberately emits a compact invalid record")
        arm["native"] = {"payload_telemetry": raw}
        arm["probe"]["payload_telemetry"] = summary
        self.assert_verdict("VALID", "payload_failure")

    def test_final_verdict_rejects_changed_trace_bytes_without_a_row_gap(self):
        source = self.tmp / "experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "comparison"
        self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out)]), 0)
        path = Path(self.arm()["trace"]["file"])
        trace = ps.read_trace(path)
        payload = bytearray(path.read_bytes())
        header_end = 12 + struct.unpack_from("<I", payload, 8)[0]
        values = list(trace.rows[0])
        values[trace.index["px"]] += 1
        struct.pack_into("<" + trace.types, payload, header_end, *values)
        path.write_bytes(payload)
        self.assertEqual(driver.main(["--require-report", str(out / "comparison.json"),
                                      "--run-id", self.experiment["run_id"]]), 2)

    def test_contention_postflight_is_bound_and_missing_signal_fails_closed(self):
        source = self.tmp / "experiment.json"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        out = self.tmp / "comparison"
        # These are explicit malformed policy records, not purported native
        # contention captures. An absent/CLEAN-less verdict must fail closed.
        for record in ({}, {"verdict": "BUSY"}):
            with self.subTest(record=record):
                path = self.tmp / "postflight.json"
                path.write_text(json.dumps(record), encoding="utf-8")
                self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out),
                                              "--contention-report", str(path)]), 2)


    # ---- #1445: wall-clock and game-relevant frame time, side by side

    def frame_breakdown_block(self, hitch=False):
        """A probe_summary.frame_breakdown block computed from constructed rows (the arithmetic is
        the point): 8 clean 4 ms frames and one holding a 400 ms viewport capture. NOT a captured
        trace: the captured BV-07 trace every arm replays predates the breakdown columns."""
        fields = ["row", "t_usec", "pass_index", "segment", "state", "s_m", *ps.BREAKDOWN_COLUMNS]
        rows, t = [(0, 1_000_000, 0, 2, ps.STATE_MOVING, 0.0, -1, -1, -1, -1, -1, 1.0, 10)], 1_000_000
        for i in range(1, 10):
            capture = 400_000 if i == 9 else 0
            engine = 300_000 if hitch and i == 5 else 3600
            t += 300 + capture + 100 + engine
            rows.append((i, t, 0, 2, ps.STATE_MOVING, 1.5 * i, 300, capture, 100, engine, int(capture > 0), 1.0, 10 + i))
        trace = ps.Trace(fields, "qqqqqd" + "qqqqqdq", rows, 0)
        block = ps.frame_breakdown(trace, list(range(10)))
        self.assertEqual(block["status"], ps.OK, block)
        return block

    def use_frame_breakdown(self, hitch=False):
        block = self.frame_breakdown_block(hitch)
        for route in ("chunk", "hlod"):
            for record in self.arm(route)["probe"]["passes"]:
                record["moving_frame_breakdown"] = copy.deepcopy(block)
                record["measured_frame_breakdown"] = copy.deepcopy(block)
        for arm in self.experiment["migration"]["arms"].values():
            arm["frame_time"] = ps.frame_time_passes(self.arm("hlod")["probe"])
        return block

    def test_frame_time_reports_wall_clock_and_game_relevant_side_by_side(self):
        block = self.use_frame_breakdown()
        report = self.assert_verdict("VALID")
        self.assertEqual(set(report["frame_time"]), {"chunk", "hlod", "migration_base", "migration_candidate"})
        for arm, passes in report["frame_time"].items():
            self.assertEqual(len(passes), 1, arm)
            self.assertEqual(passes[0]["moving_frame_breakdown"], block, arm)
            # The old wall-clock block is carried unchanged beside the new one.
            self.assertEqual(passes[0]["moving_frame_interval"],
                             self.arm("chunk" if arm == "chunk" else "hlod")["probe"]["passes"][0]["moving_frame_interval"])
        self.assertIn("game-relevant", report["measurement_scopes"]["frame_time"])
        text = driver.render_markdown(report)
        for arm in ("chunk", "hlod", "migration_base", "migration_candidate"):
            # frames | wall p50/p95/p99 | game p50/p95/p99 | harness | shares | GPU | slow frames.
            # Time-weighted: the one capture frame holds over half the wall time, so it is the
            # wall-clock median too, while the engine needed 3.6 ms for every frame.
            self.assertRegex(text, rf"\| {arm}, cold, moving \| 9 \| 404\.00 / 404\.00 / 404\.00 \| "
                                   r"3\.60 / 3\.60 / 3\.60 \| 400\.40 / 400\.40 / 400\.40 \| "
                                   r"2\.1 / 5\.4 / 0\.2 / 91\.7 / 0\.6 \| not measured / not measured \| 1 / 1 / 0 \(0\) \|")
            self.assertIn(f"{arm} cold: 0 game-relevant frame(s) over 50 ms", text)
        # The existing wall-clock line stays, now saying what it contains.
        self.assertRegex(text, r"chunk cold: moving frame p95/p99 ms = \S+ / \S+ \(wall clock, probe work included")

    def test_a_game_relevant_hitch_is_named_with_its_row_and_changes_no_verdict(self):
        self.use_frame_breakdown(hitch=True)
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        text = driver.render_markdown(report)
        self.assertIn("hlod cold: 1 game-relevant frame(s) over 50 ms in the moving + checkpoint rows, 0 of them "
                      "right after a viewport capture; the longest 1:", text)
        self.assertIn("- row 5 (segment 2, moving): engine frame 300.0 ms, wall 300.4 ms, renderer CPU 1.0 ms, "
                      "GPU not measured ms", text)
        self.assertRegex(text, r"\| hlod, cold, moving \| 9 \| [^|]+\| 300\.00 / 300\.00 / 300\.00 \|")

    def test_frame_time_without_a_breakdown_is_not_measured_and_changes_no_verdict(self):
        # The captured trace has no breakdown columns (a driver older than #1445), and the
        # constructed migration arms carry no frame-time record at all.
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        for route in ("chunk", "hlod"):
            record = report["frame_time"][route][0]
            self.assertEqual(record["moving_frame_breakdown"]["status"], ps.MISSING)
            self.assertNotIn("game_frame", record["moving_frame_breakdown"])
            self.assertEqual(record["moving_frame_interval"]["p95_time_weighted"]["status"], ps.OK)
        self.assertEqual(report["frame_time"]["migration_base"], [])
        text = driver.render_markdown(report)
        table = text[text.index("## Frame time: wall clock and game-relevant"):]
        self.assertIn("| chunk, cold, moving | not measured: the trace has no frame breakdown columns", table)
        self.assertIn("(driver older than #1445)", table)
        self.assertIn("| migration_base | not measured: the arm carries no frame-time record |", table)
        # Wall clock is still reported for the moving rows; nothing unmeasured reads as a number.
        wall = report["frame_time"]["chunk"][0]["moving_frame_interval"]
        self.assertIn(f"| not measured / {wall['p95_time_weighted']['value']:.2f} / "
                      f"{wall['p99_time_weighted']['value']:.2f} |", table)
        for line in table.splitlines():
            if line.startswith("| migration_"):
                self.assertNotRegex(line, r"\d", line)
        self.assertNotRegex(table, r"\|\s*0(?:\.0+)?\s*\|")
        # A report written before the block existed renders the same way, never as zeros.
        report.pop("frame_time")
        self.assertIn("| hlod | not measured: the arm carries no frame-time record |", driver.render_markdown(report))


class ScreenshotCadence(unittest.TestCase):
    def test_run_arm_requests_every_400_moving_frames(self):
        # Stop at plan construction: exercising the caller needs no engine or
        # fabricated native probe results, and covers every route/control arm.
        class PlanCaptured(Exception):
            pass

        for route, control in (("chunk", "none"), ("hlod", "none"), ("hlod", "saturate")):
            with self.subTest(route=route, control=control), tempfile.TemporaryDirectory() as directory:
                with mock.patch.object(shared.rp, "path_plan", side_effect=PlanCaptured) as make_plan:
                    with self.assertRaises(PlanCaptured):
                        shared.run_arm(Path("binary"), Path("world"), route, {}, {}, {}, {},
                                       Path(directory), control=control)
                self.assertEqual(make_plan.call_args.kwargs["screenshots"]["moving_every_n"], 400)


class WorkflowContract(unittest.TestCase):
    def setUp(self):
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def job(self, text=None):
        text = self.text if text is None else text
        self.assertTrue("\n  shared-layers-t0:\n" in text, "D3-6 diagnostic job has not been implemented")
        return workflow_pins._job_block(text, "shared-layers-t0")

    def test_dispatch_mode_and_test_only_control_choices_exist(self):
        mode = self.text.split("      mode:\n", 1)[1].split("      tier:", 1)[0]
        self.assertTrue('          - "shared_layers_t0"' in mode, "shared_layers_t0 dispatch choice is missing")
        self.assertRegex(self.text, r"(?m)^      shared_layers_control:")
        block = re.search(r"(?ms)^      shared_layers_control:\n(.*?)(?=^      \w+:|^\S|\Z)", self.text).group(1)
        for control in ("none", "corrupt", "saturate", "skip_compare"):
            self.assertRegex(block, rf'(?m)^          - [\"\x27]?{control}[\"\x27]?\s*$')
        self.assertRegex(block, r'(?m)^        default:\s*[\"\x27]?none[\"\x27]?\s*$')

    def test_job_is_dispatch_only_fork_guarded_on_the_3080_with_read_only_permissions(self):
        job = self.job()
        condition = workflow_pins.WorkflowContract.job_if(job)
        self.assertIn(FORK_GUARD, condition)
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("inputs.mode == 'shared_layers_t0'", condition)
        evaluator = workflow_pins.D25AutoTreeWorkflow()
        self.assertTrue(evaluator.enabled(condition, mode="shared_layers_t0"))
        self.assertFalse(evaluator.enabled(condition, mode="shared_layers_t0", fork=True))
        for mode in ("flythrough", "auto_tree", "real_scan", "watchdog"):
            self.assertFalse(evaluator.enabled(condition, mode=mode), mode)
        self.assertIn("    runs-on: [self-hosted, Linux, X64, gpu, rtx3080]", job)
        text = "\n".join(job)
        self.assertRegex(text, r"(?m)^    permissions:\s*\n      contents: read\s*$")
        self.assertNotRegex(text, r"(?m)^      [\w-]+: write\s*$")

    def test_comparison_step_invokes_driver_on_both_arms(self):
        steps = workflow_pins._steps(self.job())
        invocations = [s for s in steps if DRIVER in s.get("run", "") and "--experiment" in s["run"]]
        self.assertEqual(len(invocations), 1)
        self.assertIn("--out", invocations[0]["run"])
        self.assertNotIn("continue-on-error", invocations[0])

    def assert_finalization(self, steps):
        compare = [i for i, s in enumerate(steps) if DRIVER in s.get("run", "") and "--experiment" in s["run"]]
        final = [(i, s) for i, s in enumerate(steps) if DRIVER in s.get("run", "") and "--require-report" in s["run"]]
        self.assertEqual(len(final), 1, "an independent final step must require the comparison report")
        index, step = final[0]
        condition = step.get("if", "").removeprefix("${{").removesuffix("}}").strip()
        self.assertEqual(condition, "always()", "finalization cannot depend on a compare step that may be removed")
        self.assertIn("--run-id", step["run"])
        self.assertIn("--control", step["run"])
        self.assertNotRegex(step["run"], r"\|\|\s*(?:true|exit\s+0)|set\s+\+e")
        self.assertNotIn("continue-on-error", step)
        if compare:
            self.assertGreater(index, compare[0])

    def test_independent_final_verdict_runs_always_and_fails_closed(self):
        self.assert_finalization(workflow_pins._steps(self.job()))

    def test_removed_comparison_invocation_keeps_the_fail_closed_final_step(self):
        steps = workflow_pins._steps(self.job())
        remaining = [s for s in steps if not (DRIVER in s.get("run", "") and "--experiment" in s["run"])]
        self.assertLess(len(remaining), len(steps), "the comparison mutation must remove actual execution")
        self.assert_finalization(remaining)

    def test_uploads_are_confined_to_synthetic_diagnostic_artifacts(self):
        job = "\n".join(self.job())
        self.assertNotIn("GS_TESTDATA", job)
        self.assertNotIn("scan_dir", job)
        uploads = [s for s in workflow_pins._steps(job.splitlines()) if "upload-artifact" in s.get("uses", "")]
        self.assertTrue(uploads, "the paired synthetic report and screenshots must be retained")
        # Inspect the full scoped with:path blocks; _steps intentionally omits
        # nested with fields. Uploads must use the dedicated scratch output.
        for block in re.split(r"(?m)^      - ", job):
            if "upload-artifact" not in block:
                continue
            paths = re.findall(r"(?ms)^          path:\s*(.*?)(?=^          \w[\w-]*:|\Z)", block)
            self.assertTrue(paths)
            for path in paths:
                self.assertIn("SHARED_LAYERS_OUT", path)
                self.assertNotRegex(path, r"GS_TESTDATA|GS_REAL_SCAN|scan_dir|bicycle|\$\{\{\s*github\.workspace\s*\}\}")
        self.assertNotRegex(job, r"\*\*/\*\.ply|\*\*/\*\.gsres")

    def test_workflow_inventory_documents_the_new_mode(self):
        readme = (ROOT / ".github/workflows/README.md").read_text(encoding="utf-8")
        self.assertTrue("mode=shared_layers_t0" in readme, "workflow inventory must describe the paired diagnostic")
        self.assertTrue(PRE_D3 in readme, "inventory must identify the immutable migration binary")

    def test_unit_test_is_registered_with_the_existing_guard_lane(self):
        spec = importlib.util.spec_from_file_location("d36_module_runner", ROOT / "tests/ci/run_module_tests.py")
        runner = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = runner
        spec.loader.exec_module(runner)
        self.assertIn(Path(__file__).resolve(), {p.resolve() for p in runner.BEYOND_VRAM_LANE_TEST_SCRIPTS})


if __name__ == "__main__":
    unittest.main()
