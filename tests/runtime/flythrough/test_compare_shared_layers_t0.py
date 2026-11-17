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

PRE_D3 = "5a438363cd6939bca1041d738e2bfa10f2b024f2"
PRE_D3_SOURCE_1454 = "f2f19bcb580cc131615b3fa262a11d99e6abc70c"
PROVENANCE_BASE_1454 = "5a438363cd6939bca1041d738e2bfa10f2b024f2"
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
    "decode_pack": ("verify_ns", "record_verify_ns", "decode_span_ns", "digest_ns", "decode_remainder_ns", "pack_ns",
                    "node_load_ns", "nodes_loaded", "splats", "pages", "worker_captures", "main_thread_captures"),
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
        for arm in self.experiment["migration"]["arms"].values():
            self.apply_migration_admissibility_policy(arm)

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
        # Constructed admissibility for this comparison-owned policy envelope,
        # not a claim about the legacy producer. Frame scalars are the modern
        # run43717101341 candidate C2 receipt also used by checkpoint_policy_1454.
        result["static_sort_cache"] = {"enabled": False, "applicable": route == "chunk"}
        for checkpoint in result["checkpoints"]:
            checkpoint.update(actual_route=route, pose_first_frame=6966,
                              rendered_splat_count_source_frame=6971, render_frame_counter=6973)
        run = {"result": result, "trace": str(trace_path)}
        trace, integrity = rp.trace_integrity(run)
        self.assertEqual(integrity["status"], ps.OK)
        probe = rp.summarise_path(trace, run, self.path, self.truth, 1, self.manifest)
        self.assertEqual(probe["payload_telemetry"]["status"], ps.OK)
        self.assertEqual(probe["payload_telemetry"], ps.route_telemetry_summary(
            record["payload_telemetry"], route=route, source_generation=generation))
        if route == "hlod":
            # Constructed comparison-policy inputs, not fields from the legacy capture.
            base = driver.HLOD_CONFIG["configured_read_byte_cap"]
            probe["payload_telemetry"].update(
                normal_retained_concurrency=1, normal_retained_job_charge=base // 2,
                normal_retained_output_charge=base // 2, normal_retained_byte_bound=base)

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
                base = driver.HLOD_CONFIG["configured_read_byte_cap"]
                raw.update(normal_retained_concurrency=1, normal_retained_job_charge=base // 2,
                           normal_retained_output_charge=base // 2, normal_retained_byte_bound=base,
                           oversized_reservation_bytes=0, live_reservations=0)
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

    def test_admission_waits_are_reported_as_not_comparable_across_routes(self):
        # #1466: hlod counts ledger refusals, chunk only what passes its callers' pre-gates.
        self.use_scoped_observation_policy()
        self.arm()["native"]["payload_telemetry"]["admission_waits"] = 2860
        self.arm()["observed"]["saturation"]["admission_waits"] = 2860
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        rows = {route: report["metrics"][route]["io"]["admission_waits"] for route in ("chunk", "hlod")}
        self.assertEqual((rows["chunk"]["value"], rows["hlod"]["value"]), (0, 2860))
        for route, row in rows.items():
            self.assertIs(row["measured"], True)
            self.assertIs(row["comparable_across_routes"], False)
            self.assertEqual(row["scope"], driver.ADMISSION_WAIT_SCOPES[route])
        self.assertNotEqual(rows["chunk"]["scope"], rows["hlod"]["scope"])
        comparability = report["route_comparability"]["admission_waits"]
        self.assertEqual(comparability["status"], "not_comparable")
        self.assertIn("pre-gates", comparability["reason"])
        text = driver.render_markdown(report)
        self.assertIn("## Admission waits across routes (#1466)", text)
        self.assertIn("`io / admission_waits` is not comparable between chunk and hlod:", text)
        line = next(l for l in text.splitlines() if l.startswith("| io / admission_waits |"))
        self.assertIn("| 0 (upload-pipeline refusals at the pack in-flight cap only", line)
        self.assertIn("| 2860 (node-streamer ledger refusals", line)
        self.assertEqual(line.count("not comparable with"), 2)
        # The other back-pressure row keeps one meaning on both routes and is not annotated.
        self.assertNotIn("comparable_across_routes", report["metrics"]["hlod"]["io"]["reads_refused_busy"])

    def test_an_unmeasured_admission_wait_stays_not_measured_and_unusable(self):
        self.use_scoped_observation_policy()
        self.arm("chunk")["native"]["payload_telemetry"].pop("admission_waits")
        self.arm("chunk")["observed"]["saturation"].pop("admission_waits")
        report = self.assert_verdict("UNUSABLE", "missing_metric")
        row = report["metrics"]["chunk"]["io"]["admission_waits"]
        self.assertIs(row["measured"], False)
        self.assertIsNone(row["value"])
        self.assertNotIn("comparable_across_routes", row)
        line = next(l for l in driver.render_markdown(report).splitlines() if l.startswith("| io / admission_waits |"))
        self.assertIn("| not measured: ", line)
        # The cross-route statement does not depend on either value.
        self.assertEqual(report["route_comparability"]["admission_waits"]["status"], "not_comparable")

    def test_host_rss_peak_location_and_timeline_are_reported_without_a_verdict(self):
        # #1466. Sampler rows and the placement go through the producer-side helpers; the values are constructed.
        gib = 1 << 30
        rss = ps.rss_timeline([(0.2, gib, gib), (0.4, 2 * gib, 4 * gib), (0.6, 2 * gib, 4 * gib)], 1000.0, 0.2)
        fields = ["row", "engine_frame", "t_usec", "phase", "pass_index", "segment", "state"]
        trace = ps.Trace(fields, "q" * len(fields), [(0, 7, 2_250_000, 0, 0, 1, ps.STATE_MOVING),
                                                    (1, 8, 2_500_000, 0, 0, 1, ps.STATE_MOVING)], 0)
        result = {"start_unix_s": 1000.0, "start_usec": 2_000_000, "apply_usec": 2_100_000}
        optional = self.arm("hlod")["optional_measurements"]
        optional["os_peak_rss_bytes"] = ps.metric(4 * gib, unit="bytes")
        optional["os_rss_timeline"] = rss["timeline"]
        optional["os_rss_peak_location"] = ps.rss_peak_location(rss, result, trace)
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        hlod, chunk = report["host_rss"]["hlod"], report["host_rss"]["chunk"]
        self.assertEqual(hlod["peak_bytes"]["value"], 4 * gib)
        self.assertEqual(hlod["peak_location"]["value"]["rows"]["first"]["row"], 0)
        self.assertEqual(hlod["timeline"]["points"], [[0.2, gib], [0.4, 2 * gib], [0.6, 2 * gib]])
        # The chunk arm of this scenario exported none of the three: not measured, each with a reason.
        for key in ("peak_bytes", "peak_location", "timeline"):
            self.assertIs(chunk[key]["measured"], False, key)
            self.assertIsNone(chunk[key]["value"], key)
            self.assertTrue(chunk[key]["reason"], key)
        text = driver.render_markdown(report)
        section = text[text.index("## Host RSS: peak, where it sits, timeline (#1466)"):text.index("## Frame time")]
        self.assertIn(f"- hlod: peak 4.00 GiB ({4 * gib} B).", section)
        self.assertIn("  - peak reached [0.2, 0.4] s after process start ([0.1, 0.3] s after apply_world;", section)
        self.assertIn("within_trace, trace rows 0..1 (phase 0, pass 0, segment 1, moving); source os_high_water_mark.", section)
        self.assertIn("0.2 s 1.00 GiB, 0.4 s 2.00 GiB, 0.6 s 2.00 GiB.", section)
        self.assertIn("- chunk: peak not measured: not sampled in this unit-test replay.", section)
        self.assertIn("  - peak location not measured: the arm exported no RSS peak location.", section)
        self.assertIn("  - timeline not measured: the arm exported no RSS timeline.", section)
        # A report written before the block existed renders as not measured, never as zeros.
        report.pop("host_rss")
        self.assertIn("- hlod: peak not measured: measurement unavailable.", driver.render_markdown(report))

    def test_reintroduced_route_specific_cap_override_is_a_settings_mismatch(self):
        for key in ("configured_read_byte_cap", "configured_max_jobs_in_flight",
                    "configured_upload_bytes_per_frame", "normal_retained_byte_bound"):
            with self.scenario(key=key):
                self.use_scoped_observation_policy()
                self.arm()["native"]["payload_telemetry"][key] = 512 * 1024 * 1024
                self.assert_verdict("UNUSABLE", "settings_mismatch")

    def set_retained_derivation(self, k, r, o, *, bound=None):
        """Construct policy inputs and consistent queue observations, not a native capture."""
        raw = self.arm()["native"]["payload_telemetry"]
        base = driver.HLOD_CONFIG["configured_read_byte_cap"]
        normal = max(base, k * (r + o)) if bound is None else bound
        raw.update(normal_retained_concurrency=k, normal_retained_job_charge=r,
                   normal_retained_output_charge=o, normal_retained_byte_bound=normal)
        effective = max(normal, raw["oversized_reservation_bytes"])
        for owner in ("retained", "ready"):
            queue = raw["queue_observations"][owner]
            queue.update(byte_bound=effective, peak_byte_bound=effective)
            self.arm()["observed"]["queues"][owner] = copy.deepcopy(queue)
        return raw

    def test_derived_retained_cap_above_base_and_zero_derivation_are_valid(self):
        base = 8 * 1024 * 1024
        for k, r, o in ((2, 6 * 1024 * 1024, 2 * 1024 * 1024), (16, base, base), (0, 0, 0)):
            with self.scenario(k=k, r=r, o=o):
                self.use_scoped_observation_policy()
                self.set_retained_derivation(k, r, o)
                self.assert_verdict("VALID")

    def test_retained_derivation_survives_summary_and_native_fallback(self):
        self.use_scoped_observation_policy()
        raw = self.set_retained_derivation(2, 6 * 1024 * 1024, 2 * 1024 * 1024)
        summary = ps.route_telemetry_summary(raw, route="hlod", source_generation=raw["source_generation"])
        for key in ("normal_retained_concurrency", "normal_retained_job_charge",
                    "normal_retained_output_charge", "normal_retained_byte_bound"):
            self.assertEqual(summary[key], raw[key])
        self.arm()["probe"]["payload_telemetry"] = summary
        self.arm()["native"].pop("payload_telemetry")
        self.assert_verdict("VALID")

    def test_retained_bound_must_equal_the_exact_derivation(self):
        expected = 2 * (6 * 1024 * 1024 + 2 * 1024 * 1024)
        for delta in (-1, 1):
            with self.scenario(delta=delta):
                self.use_scoped_observation_policy()
                self.set_retained_derivation(2, 6 * 1024 * 1024, 2 * 1024 * 1024, bound=expected + delta)
                self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_concurrency_cannot_exceed_the_pinned_job_cap(self):
        self.use_scoped_observation_policy()
        self.set_retained_derivation(17, 4 * 1024 * 1024, 4 * 1024 * 1024)
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_job_charge_cannot_exceed_the_pinned_read_cap(self):
        self.use_scoped_observation_policy()
        self.set_retained_derivation(1, 8 * 1024 * 1024 + 1, 0)
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_output_charge_cannot_exceed_the_pinned_read_cap(self):
        self.use_scoped_observation_policy()
        self.set_retained_derivation(1, 0, 8 * 1024 * 1024 + 1)
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_derivation_inputs_must_all_be_present(self):
        for key in ("normal_retained_concurrency", "normal_retained_job_charge", "normal_retained_output_charge"):
            with self.scenario(key=key):
                self.use_scoped_observation_policy()
                self.arm()["native"]["payload_telemetry"].pop(key)
                self.assert_verdict("UNUSABLE", "missing_metric")

    def test_retained_derivation_is_required_without_native_queues(self):
        for key in ("normal_retained_concurrency", "normal_retained_job_charge", "normal_retained_output_charge"):
            with self.scenario(key=key):
                raw = self.arm()["probe"]["payload_telemetry"]
                self.assertNotIn("queue_observations", raw)
                raw.pop(key)
                self.assert_verdict("UNUSABLE", "missing_metric")

    def test_retained_bound_is_exact_without_native_queues(self):
        raw = self.arm()["probe"]["payload_telemetry"]
        self.assertNotIn("queue_observations", raw)
        raw["normal_retained_byte_bound"] += 1
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_derivation_is_checked_with_invalid_oversized_ownership(self):
        self.use_scoped_observation_policy()
        raw = self.set_retained_derivation(2, 6 * 1024 * 1024, 2 * 1024 * 1024, bound=16 * 1024 * 1024 + 1)
        raw["oversized_reservation_bytes"] = None
        self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_retained_derivation_inputs_must_be_non_negative_integers(self):
        for key in ("normal_retained_concurrency", "normal_retained_job_charge", "normal_retained_output_charge"):
            for value in (None, -1, 1.0, True, "1"):
                with self.scenario(key=key, value=value):
                    self.use_scoped_observation_policy()
                    self.arm()["native"]["payload_telemetry"][key] = value
                    self.assert_verdict("UNUSABLE", "missing_metric")

    def test_zero_retained_concurrency_requires_zero_charges(self):
        for r, o in ((1, 0), (0, 1)):
            with self.scenario(r=r, o=o):
                self.use_scoped_observation_policy()
                self.set_retained_derivation(0, r, o)
                self.assert_verdict("UNUSABLE", "settings_mismatch")

    def test_queue_bound_cannot_keep_base_when_normal_bound_is_derived(self):
        for owner in ("retained", "ready"):
            with self.scenario(owner=owner):
                self.use_scoped_observation_policy()
                raw = self.set_retained_derivation(2, 6 * 1024 * 1024, 2 * 1024 * 1024)
                raw["queue_observations"][owner]["byte_bound"] = 8 * 1024 * 1024
                self.arm()["observed"]["queues"][owner] = copy.deepcopy(raw["queue_observations"][owner])
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
            queue.update(bytes=0, byte_bound=raw["normal_retained_byte_bound"], jobs=0)
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

    def test_runtime_node_work_is_reported_with_its_own_scope(self):
        # Constructed reporting policy, not measurements attributed to the D3-5 capture.
        payload = self.arm()["probe"]["payload_telemetry"]
        payload.update(record_verify_nsec=12345, node_load_nsec=67890, nodes_loaded=2,
                       record_verify_scope="v4_record_envelope_sha256_and_optional_payload_validation_worker_cpu_sum",
                       node_load_scope="successful_runtime_node_load_wall_sum_including_io_wait")
        report = driver.compare(self.experiment)
        metrics = report["metrics"]["hlod"]["decode_pack"]
        for key, value, scope in (("record_verify_ns", 12345, "record_verify_scope"),
                                  ("node_load_ns", 67890, "node_load_scope"), ("nodes_loaded", 2, None)):
            record = metrics.get(key, {})
            self.assertIs(record.get("measured"), True, record)
            self.assertEqual(record.get("value"), value)
            if scope:
                self.assertEqual(record.get("scope"), payload[scope])
        text = driver.render_markdown(report)
        self.assertIn("| decode_pack / record_verify_ns | not measured: ", text)
        self.assertIn("12345 (" + payload["record_verify_scope"] + ")", text)
        self.assertIn("67890 (" + payload["node_load_scope"] + ")", text)
        self.assertIn("| decode_pack / nodes_loaded | not measured: nodes_loaded not exported by this route or probe | 2 |", text)

    def test_absent_runtime_node_work_is_not_measured_in_json_and_markdown(self):
        report = driver.compare(self.experiment)
        for route in ("chunk", "hlod"):
            for key in ("record_verify_ns", "node_load_ns", "nodes_loaded"):
                record = report["metrics"][route]["decode_pack"].get(key, {})
                self.assertIs(record.get("measured"), False, record)
                self.assertIsNone(record.get("value"))
                self.assertTrue(record.get("reason"))
        lines = driver.render_markdown(report).splitlines()
        for key in ("record_verify_ns", "node_load_ns", "nodes_loaded"):
            row = next((line for line in lines if line.startswith(f"| decode_pack / {key} |")), "")
            self.assertEqual(row.count("not measured: "), 2, row)

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
            self.apply_migration_admissibility_policy(arm)

    def apply_migration_admissibility_policy(self, arm):
        # Preserve captured counts/native work while constructing admissibility
        # for count/range policy tests. Historical JSON remains unchanged and
        # this envelope does not claim that the old base exported provenance.
        source = self.experiment["arms"]["hlod"]["probe"]["passes"][0]["checkpoints"][0]
        keys = ("settle", "pose_first_frame", "rendered_splat_count_source_frame",
                "render_frame_counter", "evidence_verdict", "evidence_reasons", "static_sort_cache")
        for row in arm["payload_behavior"]["drawn_counts"]:
            row.update({key: copy.deepcopy(source[key]) for key in keys})
        arm["static_sort_cache"] = {"enabled": False, "applicable": False}

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
            self.apply_migration_admissibility_policy(self.experiment["migration"]["arms"][name])
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

    def test_usable_pair_preserves_the_one_sided_drawn_node_mismatch(self):
        # #1448 compared the union of measured count fields. An absent optional
        # node count cannot silently erase that existing comparison.
        for side in ("base", "candidate"):
            with self.scenario(side=side):
                self.migration_checkpoint_1454(side).pop("drawn_nodes")
                self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_cut_diagnostics_pass_through_without_affecting_comparison_rules(self):
        before = driver.compare(self.experiment)
        diagnostics = {"hlod_drawn_node_digest": "0123456789abcdef", "hlod_cut_hysteresis_held": [1],
                       "hlod_cut_budget_bound": True, "hlod_cut_first_cap_refused_node": 7,
                       "hlod_cut_residency_missed": [2], "hlod_cut_residency_missed_demanded": [False]}
        for index, ids in enumerate(([1, 2], [1, 4])):
            self.migration_checkpoint_1454("candidate", index).update(diagnostics, hlod_drawn_node_ids=ids)
        row = self.migration_checkpoint_1454("base")
        for key in (*diagnostics, "hlod_drawn_node_ids"):
            row[key] = None
        after = driver.compare(self.experiment)
        for key in ("verdict", "exit_code", "reasons"):
            self.assertEqual(after[key], before[key], key)
        for key in ("draw_differences", "checkpoint_pairs", "within_binary_changes"):
            self.assertEqual(after["migration_observations"][key], before["migration_observations"][key], key)
        observations = after["migration_observations"]["candidate_drawn_id_repeat"]
        c2 = next(record for record in observations if record["checkpoint"] == "C2")
        self.assertEqual(c2["symmetric_difference"], [2, 4])
        receipts = after["checkpoint_evidence"]["checkpoints"]
        candidate = next(record for record in receipts if record["arm"] == "migration_candidate"
                         and record["pass_index"] == 0 and record["checkpoint"] == "C2")
        base = next(record for record in receipts if record["arm"] == "migration_base"
                    and record["pass_index"] == 0 and record["checkpoint"] == "C2")
        for key, value in diagnostics.items():
            self.assertEqual(candidate[key], value)
            self.assertIsNone(base[key])
        self.assertEqual(candidate["hlod_drawn_node_ids"], [1, 2])
        self.assertIsNone(base["hlod_drawn_node_ids"])

    def checkpoint_policy_1454(self):
        """Constructed comparison policy, not an instrumented-base execution.

        Receipt scalars are copied from run 43717101341 migration/candidate
        result.json, cold C2. Applying them to the existing envelope's other
        checkpoint identities/counts is deliberate policy construction. The
        historical captures and their missing provenance remain untouched.
        New cache/verdict fields below prescribe the #1454 comparison contract;
        they were not exported by that captured producer.
        """
        receipt = {
            "settle": {"settled": True, "settle_usec": 26767, "frames": 7,
                       "deadline_s": 15.0, "pose_error_m": 0.0},
            "rendered_splat_count_source_frame": 6971,
            "pose_first_frame": 6966, "render_frame_counter": 6973,
            "evidence_verdict": "VALID",
        }
        for name, arm in self.experiment["migration"]["arms"].items():
            arm["static_sort_cache"] = {"enabled": False, "applicable": False}
            for row in arm["payload_behavior"]["drawn_counts"]:
                row.update(copy.deepcopy(receipt))
                row["static_sort_cache"] = copy.deepcopy(arm["static_sort_cache"])
        for route, arm in self.experiment["arms"].items():
            state = {"enabled": False, "applicable": route == "chunk"}
            arm["observed"]["static_sort_cache"] = copy.deepcopy(state)
            for measured_pass in arm["probe"]["passes"]:
                for row in measured_pass["checkpoints"]:
                    row.update(copy.deepcopy(receipt))
                    row["settled"] = True
                    row["static_sort_cache"] = copy.deepcopy(state)

    def migration_checkpoint_1454(self, name="base", pass_index=0, checkpoint="C2"):
        return next(row for row in self.experiment["migration"]["arms"][name]["payload_behavior"]["drawn_counts"]
                    if row["pass_index"] == pass_index and row["checkpoint"] == checkpoint)

    @staticmethod
    def miss_checkpoint_1454(row):
        # Actual run 43717101341 migration/base cold C2 settle receipt. Keep a
        # deliberately stale VALID summary token: raw evidence must win.
        row["settle"] = {"settled": False, "settle_usec": -1, "frames": 4845,
                         "deadline_s": 15.0, "pose_error_m": 0.0,
                         "reason": "settle_deadline_missed"}
        if "settled" in row:
            row["settled"] = False

    def assert_inconclusive_pair_1454(self, report, reason):
        pairs = report.get("migration_observations", {}).get("checkpoint_pairs", [])
        pair = next((row for row in pairs if row.get("pass_index") == 0 and row.get("checkpoint") == "C2"), {})
        self.assertEqual(pair.get("assessment"), "INCONCLUSIVE", pair)
        self.assertEqual(pair.get("reason"), reason, pair)
        differences = report.get("migration_observations", {}).get("draw_differences", [])
        self.assertFalse(any(row.get("pass_index") == 0 and row.get("checkpoint") == "C2"
                             and row.get("assessment") == "mismatch"
                             for row in differences), differences)

    def test_1454_unsettled_reference_is_unusable_even_with_equal_counts(self):
        self.checkpoint_policy_1454()
        row = self.migration_checkpoint_1454()
        self.assertEqual(row["drawn_splats"], self.migration_checkpoint_1454("candidate")["drawn_splats"])
        self.miss_checkpoint_1454(row)
        report = self.assert_verdict("UNUSABLE", "migration_reference_unsettled")
        self.assert_inconclusive_pair_1454(report, "migration_reference_unsettled")

    def test_1454_missing_reference_settlement_is_unusable(self):
        for missing in ("settle", "settled"):
            with self.scenario(missing=missing):
                self.checkpoint_policy_1454()
                row = self.migration_checkpoint_1454()
                (row if missing == "settle" else row["settle"]).pop(missing)
                self.assert_verdict("UNUSABLE", "migration_reference_unsettled")

    def test_1454_reference_provenance_must_belong_to_the_pose(self):
        changes = [(key, None) for key in ("rendered_splat_count_source_frame", "pose_first_frame", "render_frame_counter")]
        changes += [("rendered_splat_count_source_frame", 6965),
                    ("rendered_splat_count_source_frame", 6974),
                    ("rendered_splat_count_source_frame", -1),
                    ("pose_first_frame", -1), ("rendered_splat_count_source_frame", "6971")]
        for key, value in changes:
            with self.scenario(key=key, value=value):
                self.checkpoint_policy_1454()
                row = self.migration_checkpoint_1454()
                if value is None:
                    row.pop(key)
                else:
                    row[key] = value
                self.assert_verdict("UNUSABLE", "migration_reference_unsettled")

    def test_1454_candidate_admissibility_is_symmetric(self):
        for mutation in ("unsettled", "missing_settle", "missing_settled", "missing_source",
                         "missing_pose_first", "missing_render_frame", "stale_source", "future_source"):
            with self.scenario(mutation=mutation):
                self.checkpoint_policy_1454()
                row = self.migration_checkpoint_1454("candidate")
                if mutation == "unsettled":
                    self.miss_checkpoint_1454(row)
                elif mutation == "missing_settle":
                    row.pop("settle")
                elif mutation == "missing_settled":
                    row["settle"].pop("settled")
                elif mutation == "missing_source":
                    row.pop("rendered_splat_count_source_frame")
                elif mutation == "missing_pose_first":
                    row.pop("pose_first_frame")
                elif mutation == "missing_render_frame":
                    row.pop("render_frame_counter")
                else:
                    row["rendered_splat_count_source_frame"] = 6965 if mutation == "stale_source" else 6974
                report = self.assert_verdict("UNUSABLE", "checkpoint_unsettled")
                self.assert_inconclusive_pair_1454(report, "checkpoint_unsettled")

    def test_1454_paired_chunk_and_hlod_checkpoints_require_admissibility(self):
        for route in ("chunk", "hlod"):
            for mutation in ("unsettled", "missing_settle", "missing_settled", "missing_source",
                             "missing_pose_first", "missing_render_frame", "stale_source", "future_source"):
                with self.scenario(route=route, mutation=mutation):
                    self.checkpoint_policy_1454()
                    row = self.arm(route)["probe"]["passes"][0]["checkpoints"][2]
                    if mutation == "unsettled":
                        self.miss_checkpoint_1454(row)
                    elif mutation == "missing_settle":
                        row.pop("settle")
                        row.pop("settled")
                    elif mutation == "missing_settled":
                        row["settle"].pop("settled")
                        row.pop("settled")
                    elif mutation == "missing_source":
                        row.pop("rendered_splat_count_source_frame")
                    elif mutation == "missing_pose_first":
                        row.pop("pose_first_frame")
                    elif mutation == "missing_render_frame":
                        row.pop("render_frame_counter")
                    else:
                        row["rendered_splat_count_source_frame"] = 6965 if mutation == "stale_source" else 6974
                    self.assert_verdict("UNUSABLE", "checkpoint_unsettled")

    def test_1454_unsettled_base_cannot_widen_the_reference_range(self):
        self.checkpoint_policy_1454()
        base = self.migration_checkpoint_1454()
        base["drawn_splats"] = 100_000  # Constructed range mutation, not a capture.
        self.miss_checkpoint_1454(base)
        for index in (0, 1):
            self.migration_checkpoint_1454("candidate", index)["drawn_splats"] = 150_000
        report = self.assert_verdict("UNUSABLE", "migration_reference_unsettled")
        self.assert_inconclusive_pair_1454(report, "migration_reference_unsettled")
        # The usable repeat pair must still use the admissible base range:
        # 150k was inside the old [100k, 204115] range but is outside [204115].
        self.assertIn("migration_identity_mismatch", [row["code"] for row in report["reasons"]])
        differences = report.get("migration_observations", {}).get("draw_differences", [])
        self.assertTrue(any(row.get("pass_index") == 1 and row.get("checkpoint") == "C2"
                            and row.get("assessment") == "mismatch" for row in differences), differences)

    def test_1454_unsettled_different_counts_are_not_a_migration_mismatch(self):
        self.checkpoint_policy_1454()
        base = self.migration_checkpoint_1454()
        self.miss_checkpoint_1454(base)
        base["drawn_splats"] = 1
        report = self.assert_verdict("UNUSABLE", "migration_reference_unsettled")
        self.assertNotIn("migration_identity_mismatch", [row["code"] for row in report["reasons"]])
        self.assert_inconclusive_pair_1454(report, "migration_reference_unsettled")

    def test_1454_unsettled_candidate_cannot_poison_a_usable_pair_assessment(self):
        self.checkpoint_policy_1454()
        # Constructed count policy: both base samples are admissible. The
        # usable repeat candidate is inside their range; the cold candidate is
        # inadmissible and cannot turn that repeat assessment into a mismatch.
        self.migration_checkpoint_1454("base", 0)["drawn_splats"] = 100_000
        self.migration_checkpoint_1454("base", 1)["drawn_splats"] = 200_000
        cold = self.migration_checkpoint_1454("candidate", 0)
        cold["drawn_splats"] = 999_999
        self.miss_checkpoint_1454(cold)
        self.migration_checkpoint_1454("candidate", 1)["drawn_splats"] = 150_000
        report = self.assert_verdict("UNUSABLE", "checkpoint_unsettled")
        self.assert_inconclusive_pair_1454(report, "checkpoint_unsettled")
        self.assertNotIn("migration_identity_mismatch", [row["code"] for row in report["reasons"]])
        differences = report.get("migration_observations", {}).get("draw_differences", [])
        self.assertTrue(any(row.get("pass_index") == 1 and row.get("checkpoint") == "C2"
                            and row.get("assessment") == "inconclusive" for row in differences), differences)

    def test_1454_usable_pairs_keep_the_1448_drawn_count_rule(self):
        self.checkpoint_policy_1454()
        equal = self.assert_verdict("VALID")
        self.assertEqual(equal.get("checkpoint_evidence", {}).get("verdict"), "VALID")
        for index in (0, 1):
            self.migration_checkpoint_1454("candidate", index)["drawn_splats"] += 1
        self.assert_verdict("UNUSABLE", "migration_identity_mismatch")

    def test_1454_json_reports_each_checkpoint_even_when_counts_equal(self):
        self.checkpoint_policy_1454()
        report = self.assert_verdict("VALID")
        evidence = report.get("checkpoint_evidence", {})
        self.assertEqual(evidence.get("verdict"), "VALID", evidence)
        rows = evidence.get("checkpoints", [])
        self.assertEqual({row.get("arm") for row in rows},
                         {"chunk", "hlod", "migration_base", "migration_candidate"})
        for row in rows:
            self.assertEqual(row.get("evidence_verdict"), "VALID", row)
            for key in ("checkpoint", "pass_index", "settle", "rendered_splat_count_source_frame",
                        "pose_first_frame", "render_frame_counter", "static_sort_cache"):
                self.assertIn(key, row)

    def test_1454_markdown_names_the_checkpoint_deadline_verdict(self):
        self.checkpoint_policy_1454()
        self.miss_checkpoint_1454(self.migration_checkpoint_1454())
        text = driver.render_markdown(driver.compare(self.experiment))
        for token in ("migration_reference_unsettled", "checkpoint_settle_deadline_missed", "INCONCLUSIVE", "C2"):
            with self.subTest(token=token):
                self.assertIn(token, text)

    def test_1454_static_cache_policy_is_checked_for_each_paired_route(self):
        for route in ("chunk", "hlod"):
            for enabled in (True, False, None):
                with self.scenario(route=route, enabled=enabled):
                    self.checkpoint_policy_1454()
                    state = {"enabled": enabled, "applicable": route == "chunk"}
                    self.arm(route)["observed"]["static_sort_cache"] = state
                    for row in self.arm(route)["probe"]["passes"][0]["checkpoints"]:
                        row["static_sort_cache"] = copy.deepcopy(state)
                    verdict = "UNUSABLE" if route == "chunk" and enabled is not False else "VALID"
                    reason = ("static_sort_cache_unsupported" if enabled is True else "static_sort_cache_state_unavailable")
                    report = self.assert_verdict(verdict, reason if verdict == "UNUSABLE" else None)
                    self.assertEqual(report.get("checkpoint_evidence", {}).get("verdict"), verdict)

    def test_1454_cli_unsettled_reference_exits_two_and_keeps_diagnostics(self):
        self.checkpoint_policy_1454()
        self.miss_checkpoint_1454(self.migration_checkpoint_1454())
        source, out = self.tmp / "settle-experiment.json", self.tmp / "settle-comparison"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out)]), 2)
        report = json.loads((out / "comparison.json").read_text(encoding="utf-8"))
        self.assertEqual(report.get("checkpoint_evidence", {}).get("verdict"), "UNUSABLE")
        self.assertTrue((out / "comparison.md").is_file())
        self.assertTrue(self.arm("hlod")["probe"]["screenshots"]["files"])

    def test_1454_require_report_rejects_missing_checkpoint_validation(self):
        self.checkpoint_policy_1454()
        source, out = self.tmp / "settle-experiment.json", self.tmp / "settle-comparison"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        self.assertEqual(driver.main(["--experiment", str(source), "--out", str(out)]), 0)
        path = out / "comparison.json"
        report = json.loads(path.read_text(encoding="utf-8"))
        # Model a formerly valid, correctly sealed report without #1454's
        # validation. A broken checksum must not be what makes this test red.
        report.pop("checkpoint_evidence", None)
        report.pop("report_sha256", None)
        report["report_sha256"] = hashlib.sha256(driver._canonical(report)).hexdigest()
        path.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual(driver.main(["--require-report", str(path), "--run-id", self.experiment["run_id"]]), 2)

    def test_1454_require_report_revalidates_unsettled_checkpoint_with_valid_seal(self):
        self.checkpoint_policy_1454()
        self.miss_checkpoint_1454(self.migration_checkpoint_1454())
        source, out = self.tmp / "settle-experiment.json", self.tmp / "settle-comparison"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        driver.main(["--experiment", str(source), "--out", str(out)])
        self.assertEqual(driver.main(["--require-report", str(out / "comparison.json"),
                                      "--run-id", self.experiment["run_id"]]), 2)

    def test_1454_comparator_pins_the_provenance_base(self):
        self.assertEqual(driver.PRE_D3, PROVENANCE_BASE_1454)

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

    def use_overlap_drop_blocks(self, settle_drops=0, settled_drops=0):
        """Constructed #1457/#1461 inputs (the reporting is the point), built by the producer-side
        helpers: the hlod arm with one raster-cap dropping frame, the chunk arm with checkpoint
        drops in the named window. NOT captured: the captured BV-07 trace predates this telemetry."""
        def window(drops):
            fields = ["row", "pass_index", "segment", "view", "state", "s_m", *ps.DROP_COUNTERS]
            totals, rows = dict.fromkeys(ps.DROP_COUNTERS, 0), []
            for i in range(4):
                totals["drop_signal_samples"] += 1
                if i < drops:
                    for key, step in (("overflow_drop_events", 1), ("binning_drop_events", 1),
                                      ("binning_budget_drop_events", 1), ("sampled_dropped_records_total", 245)):
                        totals[key] += step
                rows.append((i, 0, 3, -1, ps.STATE_SETTLING, 0.0, *[totals[k] for k in ps.DROP_COUNTERS]))
            return ps.overlap_drops(ps.Trace(fields, "qqqqqd" + "q" * len(ps.DROP_COUNTERS), rows, 0), list(range(4)))

        frame = {"frame_serial": 900, "dropped_records": 3000, "raster_cap_dropped_records": 3000,
                 "raster_truncated_tiles": 2, "overlap_records_attempted": 250000, "raster_tile_cap": 16384,
                 "max_tile_records": 18000, "max_tile_x": 47, "max_tile_y": 33, "overlap_allocation_records": 400000}
        event = {"count": 1, "observed_row": 12, "observed_phase": 0, "observed_pass_index": 0,
                 "observed_segment": 4, "observed_state": ps.STATE_MOVING, "frame": frame}
        self.arm("hlod")["optional_measurements"]["overlap_drop_frames"] = ps.drop_frame_summary(
            {"count": 1, "events": [event], "events_not_recorded": 0})
        self.arm("chunk")["optional_measurements"]["overlap_drop_frames"] = ps.drop_frame_summary(
            {"count": 0, "events": [], "events_not_recorded": 0})
        passes = [{"pass_index": 0, "overlap_drops_checkpoints_settle_window": window(settle_drops),
                   "overlap_drops_checkpoints_settled_pose": window(settled_drops)}]
        self.arm("chunk")["optional_measurements"]["checkpoint_drop_split"] = ps.checkpoint_drop_split(passes)

    def test_overlap_drop_report_carries_the_tile_maximum_and_changes_no_verdict(self):
        self.use_overlap_drop_blocks()
        report = self.assert_verdict("VALID")
        self.assertEqual(report["reasons"], [])
        frames = report["overlap_drop_report"]["hlod"]["drop_frames"]
        self.assertEqual((frames["status"], frames["raster_cap_frames"]), (ps.OK, 1))
        self.assertEqual(frames["raster_cap_max_tile_records_peak"]["value"], 18000)
        text = driver.render_markdown(report)
        table = text[text.index("## Overlap drops: where and how large (#1457, #1461)"):]
        self.assertIn("| hlod | pass 0, segment 4, moving | 900 | 3000 (3000) | 18000 | 47, 33 | 16384 | 1616 | "
                      "250000 | 400000 | -150000 |", table)
        self.assertIn("hlod: 1 sampled dropping frames (0 not described, 0 more in the JSON); raster-cap frames 1, "
                      "their largest per-tile record count 18000.", table)
        self.assertIn("| chunk | no sampled frame lost records (0 drop frames) |", table)
        self.assertIn("| chunk, cold | ", table)
        self.assertIn(" | 0 / 0 | 0 / 0 |", table)
        # The hlod arm carries no split here: not measured, never a zero.
        self.assertIn("| hlod | ", table)
        self.assertRegex(table, r"\| hlod \| [^|]+ \| not measured: [^|]+ \| not measured: [^|]+ \|")

    def test_settle_window_and_settled_pose_drops_are_both_failures(self):
        """#1457: the split only labels. `drop_events` keeps the arm UNUSABLE wherever the drops were."""
        for settle, settled in ((1, 0), (0, 1)):
            with self.scenario(settle=settle, settled=settled):
                self.use_overlap_drop_blocks(settle_drops=settle, settled_drops=settled)
                self.arm("chunk")["observed"]["failures"]["drop_events"] = 1
                report = self.assert_verdict("UNUSABLE", "correctness_failure")
                split = report["overlap_drop_report"]["chunk"]["checkpoint_drop_split"]["passes"][0]
                self.assertEqual((split["settle_window"]["drop_events"], split["settled_pose"]["drop_events"]),
                                 (settle, settled))
                table = driver.render_markdown(report)
                self.assertIn(f" | {settle} / {245 * settle} | {settled} / {245 * settled} |", table)

    def test_overlap_drop_report_without_telemetry_is_not_measured(self):
        # The captured arms predate the telemetry: no drop columns, no drop-frame block.
        report = self.assert_verdict("VALID")
        for route in ("chunk", "hlod"):
            entry = report["overlap_drop_report"][route]
            self.assertEqual(entry["drop_frames"]["status"], ps.NOT_MEASURED, entry)
            self.assertEqual(entry["whole_run"]["status"], ps.NOT_MEASURED, entry)
            self.assertNotIn("frames", entry["drop_frames"])
        text = driver.render_markdown(report)
        table = text[text.index("## Overlap drops: where and how large (#1457, #1461)"):]
        for route in ("chunk", "hlod"):
            self.assertRegex(table, rf"\| {route} \| not measured: [^|]+(\| not measured: [^|]+){{9}}\|")
        self.assertNotRegex(table, r"\|\s*0(?:\.0+)?\s*\|")
        # A report written before the block existed renders the same way.
        report.pop("overlap_drop_report")
        self.assertIn("| hlod | not measured: no drop-frame record |", driver.render_markdown(report))

    # ---- #1471: the T0 pressure variant must prove that pressure happened ----

    def test_pressure_drawn_budget_is_pinned_to_250k_in_runner_and_comparator(self):
        for name, module in (("runner", shared), ("comparator", driver)):
            with self.subTest(module=name):
                self.assertEqual(module.PRESSURE_DRAWN_BUDGET_SPLATS, 250_000)

    def use_pressure_policy(self):
        """Constructed policy envelope for the pressure variant, not a captured pressure run.

        The request is built by the real orchestration helper for the measured T0 tree size
        (5,136 pages, run 43742475749), with the owner-decided 250k drawn budget pinned
        independently of the runner constant. The per-arm pressure block is built by the real
        producer-side helper from the captured BV-07 result; the native counters and the
        settle windows' streaming work are constructed preconditions, because no pressure
        run existed when these tests were written.
        """
        with mock.patch.object(shared, "tree_page_count", return_value=5136):
            request = shared.pressure_request(Path("tree.gsplatworld"))
        request["drawn_budget_splats"] = 250_000
        self.experiment["variant"] = copy.deepcopy(request)
        setting, cap = request["atlas_setting"], request["atlas_override_mb"]
        records = [arm[key] for arm in self.experiment["arms"].values() for key in ("requested",)]
        records += [arm["observed"]["settings"] for arm in self.experiment["arms"].values()]
        records += [arm["settings_readback"] for arm in self.experiment["migration"]["arms"].values()]
        for settings in records:
            settings["project_settings_readback"][setting] = cap
            settings["node_readback"]["max_splat_count"] = 250_000
            settings.update(visible_splats=250_000, drawn_splats=250_000)
        self.use_scoped_observation_policy()
        for route in ("chunk", "hlod"):
            result = copy.deepcopy(self.result)
            result["final_render_stats"] = {"hlod_stream_pages_capacity": request["requested_atlas_pages"],
                                            "hlod_stream_pages_resident": 1100, "hlod_stream_evicted_total": 41,
                                            "hlod_budget_bound_frames": 7, "hlod_stream_payload_reads_total": 700}
            kinds = {(s["pass_index"], s["id"]): s["kind"] for s in result["segments"]}
            for checkpoint in result["checkpoints"]:
                if kinds.get((checkpoint["pass_index"], checkpoint["segment"])) == "teleport":
                    checkpoint["settle"]["forward_progress"] = {
                        "measured": True, "progress_deltas": {"upload_completions": 9, "upload_bytes": 4096,
                                                              "read_completions": 9, "pages_decoded": 12}}
            self.arm(route)["pressure"] = shared.pressure_observations(result, None, route, request)
        return request

    def test_pressure_variant_with_pressure_evidence_is_valid_and_reported(self):
        request = self.use_pressure_policy()
        report = self.assert_verdict("VALID")
        self.assertEqual(report["variant"], request)
        hlod = report["pressure"]["hlod"]
        self.assertEqual(hlod["evicted_total"], 41)
        self.assertEqual(hlod["budget_bound_frames"], 7)
        self.assertEqual(hlod["atlas_pages_capacity"], request["requested_atlas_pages"])
        self.assertTrue(0.20 <= hlod["atlas_fraction_observed"] <= 0.25)
        self.assertTrue(all(item["satisfied"] for item in hlod["requirements"]), hlod["requirements"])
        self.assertEqual(report["pressure"]["chunk"]["role"], "unconstrained_reference")
        text = driver.render_markdown(report)
        self.assertIn("## Pressure variant (#1471)", text)
        self.assertIn("unconstrained reference", text)
        self.assertRegex(text, r"\| hlod \| teleport [^|]*\| [^|]*\| \d+ \| [0-9.]+ \|")

    def test_unconstrained_report_carries_no_pressure_block_and_requires_none(self):
        report = self.assert_verdict("VALID")
        self.assertNotIn("variant", report)
        self.assertNotIn("pressure", report)
        self.assertNotIn("Pressure variant", driver.render_markdown(report))

    def test_1483_report_only_pressure_cannot_change_summary_or_comparator_verdict(self):
        # Constructed diagnostic contracts, not captures of a pressure binary.
        # Host/runtime CI separately exercises the new native producer.
        diagnostics = (None, {
            "report_only": True, "verdict": "VALID", "evidence_verdict": "VALID",
            "snapshot": {"values": {"hlod_stream_demanded_closure_pages": 999999,
                                     "hlod_stream_page_budget": 1, "hlod_stream_refusing_gate": 2,
                                     "hlod_stream_pages_free": 0, "hlod_stream_largest_free_run": 0}},
            "settle_window": {"counter_deltas": {
                "hlod_stream_allocation_refused_total": {"measured": True, "value": 999999},
                "hlod_stream_evicted_in_cut_total": {"measured": True, "value": 999999}}},
            "eviction_victims": {"measured": True, "events": [{"sequence": 1, "frame": 2, "node": 3, "class": 0}],
                                  "dropped_count": 10000, "sequence_gaps_total": 20000}},
            {"snapshot": {"values": []}, "settle_window": "malformed", "eviction_victims": None},
            *({"report_only": True, "snapshot": {"values": {"hlod_stream_placement_reserve_pages": value}}}
              for value in (None, 0, 3, 999999, -1)),
            *({"report_only": True, "snapshot": {"values": {"hlod_stream_placement_reserve_state": state,
                                                           "hlod_stream_placement_reserve_pages": pages}}}
              for state, pages in ((None, 999999), (0, None), (0, 999999), (1, 0), (1, 999999))))
        for failure in (False, True):
            experiment = copy.deepcopy(self.experiment)
            if failure:
                experiment["arms"]["hlod"]["observed"]["exit_code"] = 91
            baseline = driver.compare(experiment)
            for receipt in diagnostics:
                with self.subTest(failure=failure, receipt=receipt):
                    changed = copy.deepcopy(experiment)
                    for arm in changed["arms"].values():
                        checkpoints = [cp for measured_pass in arm["probe"]["passes"]
                                       for cp in measured_pass["checkpoints"]]
                        before = ps.checkpoint_evidence_summary(checkpoints)
                        for cp in checkpoints:
                            cp["hlod_pressure"] = copy.deepcopy(receipt)
                        after = ps.checkpoint_evidence_summary(checkpoints)
                        self.assertEqual((after["verdict"], after["reasons"]), (before["verdict"], before["reasons"]))
                        arm["probe"]["hlod_pressure"] = ps.hlod_pressure_summary(checkpoints)
                    report = driver.compare(changed)
                    for key in ("verdict", "exit_code", "reasons"):
                        self.assertEqual(report[key], baseline[key], key)
                    observed = report["hlod_pressure_report"]["hlod"]["checkpoints"][0]["observation"]
                    if receipt is None:
                        self.assertIs(observed["measured"], False)
                        self.assertIsNone(observed["value"])
                    else:
                        self.assertEqual(observed, receipt)
                    markdown = driver.render_markdown(report)
                    self.assertIn("HLOD pressure diagnostics (#1483)", markdown)
                    if receipt is diagnostics[1]:
                        self.assertIn("999999 / 1", markdown)
                        self.assertIn("oversized_latch", markdown)
                        self.assertIn("10000 overwritten; 20000 unobserved", markdown)

    def test_placement_reserve_diagnostics_table_distinguishes_absent_unprepared_and_measured_zero(self):
        # Report-only semantic receipts; no native capture or settlement evidence is claimed.
        key = "hlod_stream_placement_reserve_pages"
        state = "hlod_stream_placement_reserve_state"
        for values, expected_state, expected in (({}, "not measured", "not measured"),
                ({key: 0}, "not measured", "not measured"),
                ({state: None, key: 9}, "not measured", "not measured"),
                ({state: 0, key: None}, "unprepared", "not measured"),
                ({state: 0, key: 9}, "unprepared", "not measured"),
                ({state: 1, key: 0}, "measured", "0"), ({state: 1, key: 3}, "measured", "3"),
                ({state: 1, key: 999999}, "measured", "999999"),
                ({state: 1}, "not measured", "not measured")):
            with self.subTest(values=values):
                receipt = {"report_only": True, "snapshot": {"values": values}}
                report = {"hlod_pressure_report": {"hlod": {"checkpoints": [
                    {"checkpoint": "C5", "pass_index": 0, "observation": receipt}]}}}
                markdown = driver.render_markdown(report)
                headers = next(line for line in markdown.splitlines() if line.startswith("| arm / checkpoint |"))
                columns = [cell.strip().lower() for cell in headers.split("|")[1:-1]]
                self.assertIn("placement reserve pages", columns)
                self.assertIn("placement reserve state", columns)
                rows = [line for line in markdown.splitlines() if line.startswith("| hlod / C5 pass 0 |")]
                self.assertEqual(len(rows), 1)
                cells = [cell.strip() for cell in rows[0].split("|")[1:-1]]
                self.assertEqual(len(cells), len(columns))
                self.assertEqual(cells[columns.index("placement reserve pages")], expected)
                self.assertEqual(cells[columns.index("placement reserve state")], expected_state)

    def test_pressure_variant_without_pressure_evidence_is_a_named_non_pass(self):
        def teleports(arm):
            return [row for row in arm["pressure"]["settles"] if row["kind"] == "teleport"]

        def every_teleport(update):
            return lambda arm: [update(row) for row in teleports(arm)]

        setting = shared.PRESSURE_ATLAS_SETTING
        mutations = {
            "pressure_evidence_missing": lambda a: a.pop("pressure"),
            "pressure_no_evictions": lambda a: a["pressure"].update(evicted_total=0),
            "pressure_evictions_unmeasured": lambda a: a["pressure"].update(
                evicted_total=shared.missing("process did not export hlod_stream_evicted_total")),
            "pressure_no_budget_bound_frames": lambda a: a["pressure"].update(budget_bound_frames=0),
            "pressure_budget_bound_unmeasured": lambda a: a["pressure"].pop("budget_bound_frames"),
            "pressure_atlas_cap_unobserved": lambda a: a["pressure"].update(atlas_pages_capacity=None),
            # The unconstrained T0: the whole tree fits, so the cap never bound.
            "pressure_atlas_cap_out_of_band": lambda a: a["pressure"].update(atlas_pages_capacity=5136),
            "pressure_teleport_settle_trivial": every_teleport(lambda row: row.update(
                streaming_work={"upload_completions": 0, "upload_bytes": 0, "read_completions": 0, "pages_decoded": 0})),
            "pressure_teleport_settle_unmeasured": every_teleport(lambda row: row.update(
                streaming_work=shared.missing("producer did not capture this settle window's progress"))),
            "pressure_teleport_not_converged": lambda a: teleports(a)[0].update(settled=False),
            "pressure_bound_unmeasured": lambda a: a["observed"]["queues"]["ready"].pop("bound_violations"),
            "pressure_failures_unmeasured": lambda a: a["native"]["payload_telemetry"].pop("read_failures"),
            "pressure_not_applied": lambda a: a["observed"]["settings"]["project_settings_readback"].pop(setting),
        }
        for code, mutate in mutations.items():
            with self.scenario(code=code):
                self.use_pressure_policy()
                mutate(self.arm("hlod"))
                report = self.assert_verdict("UNUSABLE", code)
                named = [r for r in report["reasons"] if r["code"] == code]
                self.assertEqual(named[0].get("arm"), "hlod")
                self.assertFalse(all(item["satisfied"] for item in report["pressure"]["hlod"]["requirements"]))
        for label, mutate in (("no teleports", lambda a: a["pressure"].update(
                settles=[row for row in a["pressure"]["settles"] if row["kind"] != "teleport"])),
                              ("frames", every_teleport(lambda row: row.pop("frames"))),
                              ("ms", every_teleport(lambda row: row.update(settle_ms=-0.001)))):
            with self.scenario(teleport=label):
                self.use_pressure_policy()
                mutate(self.arm("hlod"))
                self.assert_verdict("UNUSABLE", "pressure_teleport_settle_unmeasured")

    def test_pressure_request_must_equal_the_comparator_policy(self):
        for label, mutate in (("fraction band", lambda v: v.update(atlas_fraction_band=[0.0, 1.0])),
                              ("drawn budget", lambda v: v.update(drawn_budget_splats=4_000_000)),
                              ("old 1M drawn budget", lambda v: v.update(drawn_budget_splats=1_000_000)),
                              ("tree pages", lambda v: v.pop("tree_pages")),
                              ("name", lambda v: v.update(name="unconstrained")),
                              ("cap", lambda v: v.update(atlas_override_mb=0))):
            with self.scenario(request=label):
                self.use_pressure_policy()
                mutate(self.experiment["variant"])
                self.assert_verdict("UNUSABLE", "pressure_variant_invalid")
        with self.scenario(request="not an object"):
            self.use_pressure_policy()
            self.experiment["variant"] = "pressure"
            self.assert_verdict("UNUSABLE", "pressure_variant_invalid")

    def test_pressure_drawn_budget_must_be_requested_and_read_back_on_both_routes(self):
        for route in ("chunk", "hlod"):
            for budget in (1_000_000, 4_000_000):
                with self.scenario(route=route, budget=budget):
                    self.use_pressure_policy()
                    for arm in self.experiment["arms"].values():
                        arm["requested"]["node_readback"]["max_splat_count"] = budget
                        arm["observed"]["settings"]["node_readback"]["max_splat_count"] = budget
                    for arm in self.experiment["migration"]["arms"].values():
                        arm["settings_readback"]["node_readback"]["max_splat_count"] = budget
                    report = self.assert_verdict("UNUSABLE", "pressure_not_applied")
                    self.assertIn(route, [r.get("arm") for r in report["reasons"] if r["code"] == "pressure_not_applied"])

    def test_pressure_keeps_every_correctness_failure_fatal(self):
        for key in ("nonresident_drawn_nodes", "stale_generation_publications"):
            with self.scenario(key=key):
                self.use_pressure_policy()
                self.arm("hlod")["observed"]["failures"][key] = 1
                if key in self.arm("hlod")["native"]["payload_telemetry"]:
                    self.arm("hlod")["native"]["payload_telemetry"][key] = 1
                self.assert_verdict("UNUSABLE", "correctness_failure")
        with self.scenario(key="bound_violations"):
            self.use_pressure_policy()
            for holder in (self.arm("hlod")["observed"]["queues"],
                           self.arm("hlod")["native"]["payload_telemetry"]["queue_observations"]):
                holder["ready"]["bound_violations"] = 1
            self.assert_verdict("UNUSABLE", "queue_saturation")
        with self.scenario(key="read_failures"):
            self.use_pressure_policy()
            self.arm("hlod")["native"]["payload_telemetry"]["read_failures"] = 1
            self.assert_verdict("UNUSABLE", "payload_failure")

    def write_report(self, *extra):
        source, out = self.tmp / "experiment.json", self.tmp / "report"
        source.write_text(json.dumps(self.experiment), encoding="utf-8")
        code = driver.main(["--experiment", str(source), "--out", str(out), *extra])
        return code, out / "comparison.json"

    def test_cli_binds_the_requested_variant_to_the_experiment_and_the_final_verdict(self):
        run = ["--run-id", self.experiment["run_id"]]
        with self.scenario(case="pressure requested, unconstrained experiment"):
            code, path = self.write_report("--variant", "pressure")
            self.assertEqual(code, 2)
            report = json.loads(path.read_text(encoding="utf-8"))
            self.assertIn("variant_mismatch", [r["code"] for r in report["reasons"]])
            self.assertNotEqual(driver.main(["--require-report", str(path), *run, "--variant", "pressure"]), 0)
        with self.scenario(case="unconstrained requested, pressure experiment"):
            self.use_pressure_policy()
            code, path = self.write_report()
            self.assertEqual(code, 2)
            self.assertIn("variant_mismatch", [r["code"] for r in json.loads(path.read_text(encoding="utf-8"))["reasons"]])
        with self.scenario(case="unconstrained report cannot satisfy a pressure final verdict"):
            code, path = self.write_report()
            self.assertEqual(code, 0)
            self.assertEqual(driver.main(["--require-report", str(path), *run]), 0)
            self.assertNotEqual(driver.main(["--require-report", str(path), *run, "--variant", "pressure"]), 0)
        with self.scenario(case="pressure report"):
            self.use_pressure_policy()
            code, path = self.write_report("--variant", "pressure")
            self.assertEqual(code, 0)
            self.assertEqual(driver.main(["--require-report", str(path), *run, "--variant", "pressure"]), 0)
            self.assertNotEqual(driver.main(["--require-report", str(path), *run]), 0)


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

    VARIANT_FLAG = '--variant "$SHARED_LAYERS_VARIANT"'

    def variant_steps(self, text=None):
        """The three steps that must carry the dispatched variant: orchestration, comparison, final verdict."""
        steps = workflow_pins._steps(self.job(text))
        return {"orchestration": [s for s in steps if "dispatch_lane.py shared-layers-t0" in s.get("run", "")],
                "comparison": [s for s in steps if DRIVER in s.get("run", "") and "--experiment" in s["run"]],
                "final": [s for s in steps if DRIVER in s.get("run", "") and "--require-report" in s["run"]]}

    def test_1471_pressure_variant_is_a_dispatch_choice_defaulting_to_unconstrained(self):
        self.assertRegex(self.text, r"(?m)^      shared_layers_variant:")
        block = re.search(r"(?ms)^      shared_layers_variant:\n(.*?)(?=^      \w+:|^\S|\Z)", self.text).group(1)
        self.assertEqual(re.findall(r'(?m)^          - "(\w+)"\s*$', block), ["unconstrained", "pressure"])
        self.assertRegex(block, r'(?m)^        default: "unconstrained"\s*$')
        self.assertIn("      SHARED_LAYERS_VARIANT: ${{ inputs.shared_layers_variant || 'unconstrained' }}\n",
                      "\n".join(self.job()) + "\n")

    def test_1471_variant_reaches_orchestration_comparison_and_the_final_verdict(self):
        for name, steps in self.variant_steps().items():
            with self.subTest(step=name):
                self.assertEqual(len(steps), 1)
                self.assertIn(self.VARIANT_FLAG, steps[0]["run"])

    def test_1471_pressure_runs_are_named_and_archived_apart_from_unconstrained_ones(self):
        name = next(line for line in self.text.splitlines() if line.startswith("run-name:"))
        self.assertIn("inputs.shared_layers_variant == 'pressure'", name)
        upload = next(line for line in self.job() if line.strip().startswith("name: shared-layers-t0-"))
        self.assertIn("inputs.shared_layers_variant == 'pressure' && 'pressure-' || ''", upload)
        readme = (ROOT / ".github/workflows/README.md").read_text(encoding="utf-8")
        self.assertIn("shared_layers_variant", readme)
        self.assertIn("#1471", readme)

    def test_1454_workflow_pins_the_provenance_base_checkout_and_artifacts(self):
        # Derive locations rather than pinning a job ID that phase 3 may rename.
        pins = re.findall(r"(?m)^\s+PRE_D3_SHA:\s*([0-9a-f]{40})\s*$", self.text)
        self.assertTrue(pins, "migration build/run SHA environments are missing")
        for value in pins:
            with self.subTest(surface="PRE_D3_SHA", value=value):
                self.assertEqual(value, PROVENANCE_BASE_1454)
        with self.subTest(surface="checkout"):
            self.assertRegex(self.text, rf"(?m)^\s+ref:\s*{PROVENANCE_BASE_1454}\s*$")
        artifacts = re.findall(r"(?m)^\s+name:\s*(godot-linuxbsd-editor-tests-[^\s]+)\s*$", self.text)
        migration_artifacts = [value for value in artifacts if "pred3" in value or "provenance" in value]
        self.assertTrue(migration_artifacts, "migration upload/download artifact names are missing")
        for value in migration_artifacts:
            with self.subTest(surface="artifact", value=value):
                self.assertIn(PROVENANCE_BASE_1454, value)

    def test_1454_provenance_base_is_named_and_original_source_is_documented(self):
        readme = (ROOT / ".github/workflows/README.md").read_text(encoding="utf-8")
        self.assertIn(PROVENANCE_BASE_1454, readme)
        self.assertIn(PRE_D3_SOURCE_1454, readme, "retain the original pre-#1453 source identity")
        self.assertIn("#1453", readme)
        names = re.findall(r"(?m)^\s+(?:- )?name:\s*(.+)$", self.text)
        self.assertTrue(any("provenance" in name.lower() and "pre-d3" in name.lower() for name in names),
                        "the migration build must identify pre-D3 + provenance")

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
