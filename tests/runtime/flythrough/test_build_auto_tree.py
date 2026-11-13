#!/usr/bin/env python3
"""D2-4 driver contract, without a binary. Run with python -B.

These tests intentionally precede build_auto_tree.py. Missing implementation is
an assertion in each case, never an import/discovery error. The example is job
INPUT, not a fabricated builder-output fixture. Actual output is checked by the
separate binary suite. Proposed driver API: BuildError.reason,
validate_job_manifest(dict), load_job_manifest(Path), new_report(),
process_peak_rss(builder_report), verify_output_sha256(Path, expected), main(argv).
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
DRIVER = HERE / "build_auto_tree.py"
EXAMPLE = HERE / "auto_tree_job.example.json"
REPORT_FIELDS = {
    "schema", "status", "reason", "wall_time_sec", "peak_rss_bytes",
    "scratch_bytes", "output_bytes", "node_count", "height", "levels",
    "content_generation", "output_file_sha256", "tree_sha", "input_file_sha256",
    "job_manifest_sha256", "source_fingerprint", "source_fingerprint_source",
    "budget_frozen", "drawn_budget_splats", "drawn_budget_source", "oracle",
    "oracle_complete_all_poses", "leaves_only", "w13", "builder_wall_time_sec",
}


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def example_job():
    return json.loads(EXAMPLE.read_text(encoding="utf-8"))


class DriverContract(unittest.TestCase):
    def setUp(self):
        self.assertTrue(DRIVER.is_file(), "driver_missing: tests/runtime/flythrough/build_auto_tree.py")
        self.driver = load_module("d2_build_auto_tree", DRIVER)

    def assert_reason(self, reason, function, *args):
        with self.assertRaises(self.driver.BuildError) as caught:
            function(*args)
        self.assertEqual(caught.exception.reason, reason)

    def test_valid_job_preserves_types_and_values(self):
        job = example_job()
        self.assertEqual(self.driver.validate_job_manifest(job), job)
        self.assertEqual(job, example_job())
        job["limits"]["deadline_sec"] = 120.5  # The builder accepts finite numeric deadlines.
        self.assertEqual(self.driver.validate_job_manifest(job), job)

    def test_unknown_and_missing_keys_fail_closed_at_every_level(self):
        for group in (None, "limits", "oracle"):
            original = example_job() if group is None else example_job()[group]
            for key in original:
                with self.subTest(group=group, missing=key):
                    job = example_job()
                    del (job if group is None else job[group])[key]
                    self.assert_reason("job_manifest_missing_key", self.driver.validate_job_manifest, job)
            job = example_job()
            (job if group is None else job[group])["unexpected"] = 1
            with self.subTest(group=group, unknown=True):
                self.assert_reason("job_manifest_unknown_key", self.driver.validate_job_manifest, job)

    def test_no_numeric_coercion_or_bool_as_int(self):
        for group in (None, "limits", "oracle"):
            values = example_job() if group is None else example_job()[group]
            for key, value in values.items():
                if type(value) not in (int, float):
                    continue
                invalid = [str(value), True, None, [], {}]
                if type(value) is int and key != "deadline_sec":
                    invalid.append(float(value))
                for replacement in invalid:
                    with self.subTest(group=group, key=key, value=replacement):
                        job = example_job()
                        (job if group is None else job[group])[key] = replacement
                        self.assert_reason("job_manifest_type", self.driver.validate_job_manifest, job)

    def test_strings_and_objects_are_strictly_typed(self):
        for key in ("schema", "codec", "drawn_budget_source", "limits", "oracle"):
            job = example_job()
            job[key] = 1
            with self.subTest(key=key):
                self.assert_reason("job_manifest_type", self.driver.validate_job_manifest, job)
        for value in ([], None, "job"):
            self.assert_reason("job_manifest_type", self.driver.validate_job_manifest, value)

    def test_invalid_values_and_nonfinite_limits(self):
        cases = [("schema", "godotgs-auto-tree-job/1"), ("codec", "unknown"),
                 ("worker_count", 0), ("worker_count", 65), ("max_tile_splats", 16384),
                 ("drawn_budget_splats", 0), ("drawn_budget_source", "")]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                job = example_job()
                job[key] = value
                self.assert_reason("job_manifest_value", self.driver.validate_job_manifest, job)
        for key in example_job()["limits"]:
            for value in (0, -1):
                job = example_job()
                job["limits"][key] = value
                self.assert_reason("job_manifest_value", self.driver.validate_job_manifest, job)
        for value in (float("nan"), float("inf"), -float("inf")):
            job = example_job()
            job["limits"]["deadline_sec"] = value
            self.assert_reason("job_manifest_value", self.driver.validate_job_manifest, job)
        for key, value in (("width", 0), ("height", -1), ("keep", "diagonal"),
                           ("fov_deg", 180.0), ("near_m", 0.0), ("far_m", 0.01),
                           ("tau_pixels", 0.0), ("tau_pixels", float("inf"))):
            job = example_job()
            job["oracle"][key] = value
            with self.subTest(camera_field=key, value=value):
                self.assert_reason("job_manifest_value", self.driver.validate_job_manifest, job)

    def test_missing_corrupt_duplicate_key_and_wrong_schema_jobs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "job.json"
            self.assert_reason("job_manifest_missing", self.driver.load_job_manifest, path)
            for text in ('{"schema":', '{"schema":"x","schema":"y"}', '\xff'):
                path.write_bytes(text.encode("latin-1"))
                self.assert_reason("job_manifest_corrupt", self.driver.load_job_manifest, path)
            path.write_text("[]", encoding="utf-8")
            self.assert_reason("job_manifest_type", self.driver.load_job_manifest, path)
            path.write_text(json.dumps(example_job()), encoding="utf-8")
            self.assertEqual(self.driver.load_job_manifest(path), example_job())

    def test_initial_report_has_all_fields_and_never_claims_success(self):
        report = self.driver.new_report()
        self.assertTrue(REPORT_FIELDS <= report.keys(), REPORT_FIELDS - report.keys())
        self.assertEqual(report["schema"], "godotgs-auto-tree-build-report/1")
        self.assertEqual(report["status"], "failed")
        self.assertIsInstance(report["reason"], str)
        self.assertTrue(report["reason"])
        self.assertIs(report["budget_frozen"], False)
        self.assertIs(report["oracle_complete_all_poses"], False)
        for field in ("peak_rss_bytes", "scratch_bytes"):
            self.assertIs(report[field]["available"], False)
            self.assertTrue(report[field]["reason"])
            self.assertIsNone(report[field].get("value"))
        self.assertIs(report["w13"]["available"], False)
        self.assertTrue(report["w13"]["reason"])
        for field in ("wall_time_sec", "output_bytes", "node_count", "height", "builder_wall_time_sec"):
            self.assertIsNone(report[field], field)
        json.dumps(report, allow_nan=False)

    def test_process_rss_does_not_launder_reserved_or_static_memory(self):
        for source in ("unavailable", "godot_static"):
            result = self.driver.process_peak_rss({
                "process_peak_memory_available": source != "unavailable",
                "process_peak_memory_bytes": 1234, "process_peak_memory_source": source,
                "peak_reserved_bytes": 9999,
            })
            self.assertIs(result["available"], False)
            self.assertIsNone(result.get("value"))
            self.assertTrue(result["reason"])
        result = self.driver.process_peak_rss({"process_peak_memory_available": True,
            "process_peak_memory_bytes": 1234, "process_peak_memory_source": "process_rss"})
        self.assertIs(result["available"], True)
        self.assertEqual(result["value"], 1234)
        self.assertEqual(result["source"], "process_rss")
        result = self.driver.process_peak_rss({})
        self.assertIs(result["available"], False)
        self.assertTrue(result["reason"])

    def test_w13_requires_all_nodes_verified_with_strict_counts(self):
        # Malformed metric dictionaries exercise evidence acceptance only;
        # they are not a fixture claiming to capture producer output.
        for builder in ({}, {"node_count": 2}, {"node_count": 2, "nodes_verified": 0},
                        {"node_count": 2, "nodes_verified": 1},
                        {"node_count": 1, "nodes_verified": 2},
                        {"node_count": 2, "nodes_verified": True},
                        {"node_count": 2, "nodes_verified": 2.0}):
            with self.subTest(builder=builder):
                self.assert_reason("builder_w13_incomplete", self.driver.verified_nodes, builder, 2)
        result = self.driver.verified_nodes({"node_count": 2, "nodes_verified": 2}, 2)
        self.assertIs(result["available"], True)
        self.assertIs(result["passed"], True)
        self.assertEqual(result["nodes_verified"], 2)
        self.assertEqual(result["node_count"], 2)
        self.assertIn("nodes_verified", result["source"])

    def test_scratch_measurement_reports_peak_and_cumulative_bytes(self):
        # Unit inputs for measurement extraction, not a serialized producer fixture.
        result = self.driver.scratch_usage({"scratch_bytes_peak": 1234, "scratch_bytes_total": 5678})
        self.assertIs(result["available"], True)
        self.assertEqual(result["value"], 1234)
        self.assertEqual(result["total_bytes"], 5678)
        self.assertEqual(result["source"], "writer_v4_and_frontier")
        self.assertIn("staged V4 output", result["scope"])
        self.assertIn("excludes manifest", result["scope"])

    def test_scratch_measurement_never_coerces_missing_or_invalid_values_to_zero(self):
        result = self.driver.scratch_usage({"peak_reserved_bytes": 1234})
        self.assertIs(result["available"], False)
        self.assertNotIn("value", result)
        self.assertTrue(result["reason"])
        for key in ("scratch_bytes_peak", "scratch_bytes_total"):
            for value in (None, False, True, 0, -1, 1.5, "1234", [], {}):
                with self.subTest(key=key, value=value):
                    values = {"scratch_bytes_peak": 1234, "scratch_bytes_total": 5678, key: value}
                    result = self.driver.scratch_usage(values)
                    self.assertIs(result["available"], False)
                    self.assertNotIn("value", result)
                    self.assertNotIn("total_bytes", result)
                    self.assertTrue(result["reason"])

    def test_scratch_components_survive_missing_legacy_totals(self):
        # Dictionary unit inputs exercise extraction, not a producer format.
        values = {"scratch_spill_bytes_peak": 100, "scratch_spill_bytes_total": 120,
                  "scratch_frontier_bytes_peak": 30, "scratch_frontier_bytes_total": 40,
                  "scratch_combined_bytes_peak": 125, "frontier_ram_bytes_peak": 200}
        result = self.driver.scratch_usage(values)
        self.assertFalse(result["available"])
        self.assertNotIn("value", result)
        self.assertEqual(result["measurements"], {
            key: {"available": True, "value": value} for key, value in values.items()})

    def test_scratch_components_preserve_absence_reasons_and_reject_invalid_values(self):
        reason = "frontier remained in RAM; no frontier scratch was written"
        values = {"scratch_bytes_peak_unavailable_reason": "writer did not start",
                  "scratch_bytes_total_unavailable_reason": "writer did not start",
                  "scratch_frontier_bytes_peak_unavailable_reason": reason}
        result = self.driver.scratch_usage(values)
        self.assertEqual(result["reason"], "writer did not start")
        self.assertEqual(result["measurements"], {
            "scratch_frontier_bytes_peak": {"available": False, "reason": reason}})
        self.assertNotIn("measurements", self.driver.scratch_usage({}))
        for key in ("scratch_spill_bytes_peak", "scratch_spill_bytes_total",
                    "scratch_frontier_bytes_peak", "scratch_frontier_bytes_total",
                    "scratch_combined_bytes_peak", "frontier_ram_bytes_peak"):
            for value in (None, False, True, 0, -1, 1.5, "1234", [], {}):
                with self.subTest(key=key, value=value):
                    component = self.driver.scratch_usage({key: value})["measurements"][key]
                    self.assertFalse(component["available"])
                    self.assertNotIn("value", component)
                    self.assertTrue(component["reason"])

    def test_output_sha_is_recomputed_from_bytes_and_mismatch_fails(self):
        # Arbitrary bytes exercise hashing only; this is not a V4 producer fixture.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bytes"
            path.write_bytes(b"original")
            expected = hashlib.sha256(b"original").hexdigest()
            self.assertEqual(self.driver.verify_output_sha256(path, expected), expected)
            path.write_bytes(b"modified")
            self.assert_reason("output_sha_mismatch", self.driver.verify_output_sha256, path, expected)

    def test_preflight_failure_reports_keep_distinct_reasons_without_launching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, job, report = root / "input", root / "job.json", root / "report.json"
            source.write_bytes(b"preflight only, never passed to Godot")
            cases = [("job_manifest_missing", None, source),
                     ("job_manifest_corrupt", "{broken", source),
                     ("job_manifest_type", "[]", source),
                     ("input_not_found", json.dumps(example_job()), root / "missing")]
            observed = []
            for reason, text, input_path in cases:
                if text is not None:
                    job.write_text(text, encoding="utf-8")
                with self.subTest(reason=reason), \
                        mock.patch("subprocess.run", side_effect=AssertionError("preflight must not launch a child")):
                    code = self.driver.main(["--godot-binary", sys.executable, "--input", str(input_path),
                        "--job-manifest", str(job), "--out", str(root / "out.gsplatworld"),
                        "--report", str(report)])
                    self.assertEqual(code, 1)
                    self.assertTrue(report.is_file(), "failure must publish a JSON report")
                    payload = json.loads(report.read_text(encoding="utf-8"))
                    self.assertEqual(payload["status"], "failed")
                    self.assertEqual(payload["reason"], reason)
                    self.assertIsNone(payload["output_bytes"])
                    observed.append(payload["reason"])
            self.assertEqual(len(observed), len(set(observed)))

    def test_invalid_oracle_response_is_a_named_failure(self):
        # Deliberately malformed responses, not captured producer fixtures.
        poses = [{"checkpoint": "C0", "pose": [0, 0, 0, 0, 0]}]
        for response in (None, {}, {"error": False}, {"error": 0, "poses": []},
                         {"error": 0, "poses": [{"checkpoint": "C0", "pose": poses[0]["pose"], "status": []}]}):
            with self.subTest(response=response):
                self.assert_reason("oracle_report_invalid", self.driver._oracle_block, response, poses, 1)

    def test_leaves_only_control_that_did_not_run_fails(self):
        # #1432 forcing control. Constructed rows in the shape the shared row
        # checks require; only the control predicate varies.
        poses = [{"checkpoint": "C0", "pose": [0, 0, 0, 0, 0]}]

        def control(status, only_leaves, splats):
            row = {"checkpoint": "C0", "pose": poses[0]["pose"], "status": status,
                   "selected_splats": splats, "selected_count": 1 if splats else 0, "budget": 1,
                   "budget_bound": False, "drawn_le_budget": splats <= 1}
            if only_leaves is not None:
                row["only_leaves"] = only_leaves
            return {"error": 0, "reason": "", "mechanism": "stale_interiors_unbounded", "poses": [row]}

        def check(response):
            return self.driver._oracle_block(response, poses, 1, control=True)

        # The legal route: a control that ran, over budget by design, passes.
        for status, splats in (("COMPLETE", 7), ("EMPTY", 0)):
            with self.subTest(legal=status):
                self.assertEqual(check(control(status, True, splats))["poses"][0]["status"], status)
        for status in ("INVALID", "UNAVAILABLE", "BOOTSTRAP_EXHAUSTED", "BOOTSTRAP_ARITHMETIC",
                       "BOOTSTRAP_MISSING_RESIDENCY", "BOOTSTRAP_INFEASIBLE"):
            for only_leaves in (False, True):
                with self.subTest(status=status, only_leaves=only_leaves):
                    self.assert_reason("leaves_control_invalid", check, control(status, only_leaves, 0))
        for only_leaves in (False, None, 1):
            with self.subTest(complete_only_leaves=only_leaves):
                self.assert_reason("leaves_control_invalid", check, control("COMPLETE", only_leaves, 7))

    def test_collect_output_rejects_mismatched_bytes_before_reading_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "corrupt.gsplatworld"
            path.write_bytes(b"corrupt bytes, not a V4 fixture")
            path.with_suffix(".manifest.json").write_text(json.dumps({
                "schema": "godotgs-auto-tree-manifest/1", "output_file_sha256": "0" * 64,
            }), encoding="utf-8")
            report = self.driver.new_report()
            self.assert_reason("output_sha_mismatch", self.driver._collect_output, report, path, example_job(), {})
            self.assertIsNone(report["output_bytes"])
            self.assertEqual(report["status"], "failed")

    def test_collect_output_requires_w13_before_accepting_metrics(self):
        # Orchestration-only test with mocked format/hash checks. This deliberately
        # incomplete producer report must not become evidence of a verified world.
        fingerprint, tree_sha = "a" * 64, "b" * 64
        sidecar = {"schema": "godotgs-auto-tree-manifest/1", "output_file_sha256": "c" * 64,
                   "input_file_sha256": "d" * 64, "source_fingerprint": fingerprint,
                   "tree_sha": tree_sha, "content_generation": "1", "totals": {"node_count": 2},
                   "levels": [{"depth": 0, "node_count": 2, "eps_min": 0, "eps_median": 0, "eps_max": 0}]}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "world.gsplatworld"
            path.touch()
            path.with_suffix(".manifest.json").touch()
            report = self.driver.new_report()
            report["input_file_sha256"] = sidecar["input_file_sha256"]
            with mock.patch.object(self.driver, "_read_json", return_value=sidecar), \
                    mock.patch.object(self.driver, "verify_output_sha256", return_value=sidecar["output_file_sha256"]), \
                    mock.patch.object(self.driver.verify_world_manifest, "compute_tree_sha", return_value=tree_sha):
                self.assert_reason("builder_w13_incomplete", self.driver._collect_output,
                                   report, path, {"source_fingerprint": fingerprint},
                                   {"tree_sha": tree_sha, "node_count": 2})
            self.assertIs(report["w13"]["available"], False)
            self.assertIsNone(report["output_bytes"])

    def test_report_path_collision_cannot_overwrite_input(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.gsplatworld"
            source.write_bytes(b"preserve this input")
            with mock.patch("subprocess.run", side_effect=AssertionError("must not launch")):
                code = self.driver.main(["--godot-binary", sys.executable, "--input", str(source),
                    "--job-manifest", str(root / "job.json"), "--out", str(root / "out.gsplatworld"),
                    "--report", str(source)])
            self.assertEqual(code, 1)
            self.assertEqual(source.read_bytes(), b"preserve this input")



class D25SourceFingerprintContract(unittest.TestCase):
    """D2-5 source identity contracts introduced in tests-only turn 1."""

    def setUp(self):
        self.driver = load_module("d25_build_auto_tree", DRIVER)
        self.cache = load_module("d25_builder_world_cache", HERE / "world_cache.py")

    def job(self):
        job = example_job()
        job["schema"] = "godotgs-auto-tree-job/2"
        job.pop("source_fingerprint", None)
        job.pop("source_fingerprint_source", None)
        return job

    def checkout(self, root):
        self.assertTrue(hasattr(self.cache, "TREE_SOURCES"), "TREE_SOURCES is not implemented")
        self.assertTrue(self.cache.TREE_SOURCES)
        repo = root / "checkout"
        for relative in self.cache.TREE_SOURCES:
            source = HERE.parents[2] / relative
            self.assertTrue(source.is_file(), relative)
            target = repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read_bytes())
        return repo

    def run_driver(self, root, *, repo=None, capture=None, failure_envelope=None):
        source, job, report = root / "input", root / "job.json", root / "report.json"
        # Input bytes never reach a producer: this test checks request provenance,
        # not a valid world or a fabricated producer-output fixture.
        source.write_bytes(b"source provenance test only")
        source.with_suffix(".manifest.json").write_text("{}", encoding="utf-8")
        job.write_text(json.dumps(self.job()), encoding="utf-8")
        argv = ["--godot-binary", sys.executable, "--input", str(source),
                "--job-manifest", str(job), "--out", str(root / "out.gsplatworld"),
                "--report", str(report)]
        if repo is not None:
            argv += ["--repo", str(repo)]

        def stop_at_producer(command, **kwargs):
            self.assertIsNotNone(capture, "invalid sources must prevent producer launch")
            request = Path(command[command.index("--request") + 1])
            payload = json.loads(request.read_text(encoding="utf-8"))
            # Scratch admission measures the output filesystem, never system /tmp.
            self.assertEqual(request.parent.parent, Path(payload["output"]).parent)
            self.assertEqual(Path(payload["scratch"]).parent, request.parent)
            self.assertEqual(Path(command[command.index("--path") + 1]), request.parent)
            capture.append(payload)
            if failure_envelope is not None:
                # Fault injection at the subprocess boundary, not a captured
                # producer fixture or evidence that a real build ran.
                response = Path(command[command.index("--report") + 1])
                response.write_text(json.dumps(failure_envelope), encoding="utf-8")
                return self.driver.subprocess.CompletedProcess(command, 1)
            # Deliberately terminate at the subprocess boundary. No Godot runs.
            raise self.driver.subprocess.TimeoutExpired(command, kwargs["timeout"])

        poses = [(f"C{i}", [0, 0, 0, 0, 0]) for i in range(8)]
        with mock.patch.object(self.driver.probe_path, "resolve_path", return_value=[]), \
                mock.patch.object(self.driver.probe_path, "checkpoints", return_value=poses), \
                mock.patch("subprocess.run", side_effect=stop_at_producer) as producer:
            code = self.driver.main(argv)
        self.assertEqual(code, 1)
        self.assertTrue(report.is_file(), "driver failures must publish their reason")
        return json.loads(report.read_text(encoding="utf-8")), producer.call_count

    def test_builder_refusal_preserves_raw_diagnostics_in_uploaded_report(self):
        builder = {"error": 6, "reason": "limit_ram: injected allocation refusal",
                   "peak_reserved_bytes": 7654321098, "tiles_baked": 0, "pages_decoded": 19,
                   "pages_decoded_digest_checked": 19, "wall_usec": 790000,
                   "detail": "injected diagnostic text"}
        with tempfile.TemporaryDirectory() as directory:
            report, calls = self.run_driver(Path(directory), capture=[], failure_envelope={"builder": builder})
        self.assertEqual(calls, 1)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["reason"], builder["reason"])
        self.assertEqual(report["builder_raw"], builder)
        self.assertEqual(report["builder_wall_time_sec"], 0.79)
        self.assertFalse(report["peak_rss_bytes"]["available"], "reservation is not measured RSS")
        self.assertIsNone(report["output_bytes"])

    def test_envelope_failure_keeps_builder_details_and_absence_is_not_zero(self):
        for builder in ({"error": 6, "reason": "builder detail"}, None):
            envelope = {"reason": "injected bridge failure", "builder": builder}
            with self.subTest(builder=builder), tempfile.TemporaryDirectory() as directory:
                report, _ = self.run_driver(Path(directory), capture=[], failure_envelope=envelope)
                self.assertEqual(report["reason"], envelope["reason"])
                self.assertEqual(report["producer_reason"], envelope["reason"])
                self.assertEqual(report["builder_raw"], builder)
                self.assertIsNone(report["builder_wall_time_sec"])

    def test_failure_preserves_scratch_components_and_tile_progress(self):
        # Fault injection checks orchestration, not the real producer's format.
        builder = {"error": 6, "reason": "limit_scratch", "tiles_planned": 300,
                   "tiles_baked": 214, "scratch_bytes_peak": 12000,
                   "scratch_bytes_total": 15000, "scratch_spill_bytes_peak": 9000,
                   "scratch_spill_bytes_total": 11000, "scratch_combined_bytes_peak": 12000,
                   "scratch_frontier_bytes_peak": 4000, "scratch_frontier_bytes_total": 4000,
                   "frontier_ram_bytes_peak": 5000}
        for bridge_reason in (None, "injected bridge failure"):
            with self.subTest(bridge_reason=bridge_reason), tempfile.TemporaryDirectory() as directory:
                envelope = {"builder": builder}
                if bridge_reason is not None:
                    envelope["reason"] = bridge_reason
                report, calls = self.run_driver(Path(directory), capture=[], failure_envelope=envelope)
                self.assertEqual(calls, 1)
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["reason"], bridge_reason or "limit_scratch")
                self.assertEqual(report["tiles_planned"], 300)
                self.assertEqual(report["tiles_baked"], 214)
                self.assertEqual(report["scratch_bytes"], self.driver.scratch_usage(builder))
                self.assertTrue(report["scratch_bytes"]["available"])
                self.assertEqual(report["scratch_bytes"]["measurements"]["scratch_spill_bytes_peak"]["value"], 9000)
                self.assertEqual(report["builder_raw"], builder)

    def test_v2_job_accepts_no_caller_source_identity(self):
        job = self.job()
        try:
            validated = self.driver.validate_job_manifest(job)
        except self.driver.BuildError as exc:
            self.fail(f"v2 job must derive its source identity; refused with {exc.reason}")
        self.assertEqual(validated, job)
        self.assertNotIn("source_fingerprint", job)
        self.assertNotIn("source_fingerprint_source", job)

    def test_caller_source_fingerprint_is_refused_with_distinct_reason(self):
        for schema in ("godotgs-auto-tree-job/1", "godotgs-auto-tree-job/2"):
            for supplied in ({"source_fingerprint": "a" * 64},
                             {"source_fingerprint_source": "caller assertion"},
                             {"source_fingerprint": "a" * 64,
                              "source_fingerprint_source": "caller assertion"}):
                with self.subTest(schema=schema, supplied=tuple(supplied)):
                    job = self.job()
                    job.update(schema=schema, **supplied)
                    with self.assertRaises(self.driver.BuildError) as caught:
                        self.driver.validate_job_manifest(job)
                    self.assertEqual(caught.exception.reason, "job_manifest_source_fingerprint_forbidden")

    def test_source_fingerprint_io_failure_is_distinct_and_fail_closed(self):
        with mock.patch.object(self.driver.world_cache, "source_fingerprint", side_effect=OSError("unreadable")):
            with self.assertRaises(self.driver.BuildError) as caught:
                self.driver.tree_source_fingerprint(HERE.parents[2])
        self.assertEqual(caught.exception.reason, "tree_source_unreadable")

    def test_repo_sources_supply_the_producer_and_report_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = self.checkout(root)
            expected = self.cache.source_fingerprint(repo, self.cache.TREE_SOURCES)
            captured = []
            report, calls = self.run_driver(root, repo=repo, capture=captured)
            self.assertEqual(calls, 1)
            self.assertEqual(report["reason"], "producer_timeout")
            self.assertEqual(report["source_fingerprint"], expected)
            self.assertEqual(captured[0]["job"]["source_fingerprint"], expected)
            self.assertTrue(report["source_fingerprint_source"])

    def test_tree_source_edit_changes_the_next_producer_fingerprint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = self.checkout(root)
            captured = []
            before, calls = self.run_driver(root, repo=repo, capture=captured)
            self.assertEqual(calls, 1)
            target = repo / self.cache.TREE_SOURCES[0]
            target.write_bytes(target.read_bytes() + b"\n// fingerprint forcing control\n")
            after, calls = self.run_driver(root, repo=repo, capture=captured)
            self.assertEqual(calls, 1)
            self.assertEqual(before["reason"], "producer_timeout")
            self.assertEqual(after["reason"], "producer_timeout")
            self.assertNotEqual(before["source_fingerprint"], after["source_fingerprint"])
            expected = self.cache.source_fingerprint(repo, self.cache.TREE_SOURCES)
            self.assertEqual(after["source_fingerprint"], expected)
            self.assertEqual(captured[-1]["job"]["source_fingerprint"], expected)

    def test_missing_tree_source_fails_before_producer_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = self.checkout(root)
            (repo / self.cache.TREE_SOURCES[0]).unlink()
            report, calls = self.run_driver(root, repo=repo)
            self.assertEqual(calls, 0)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["reason"], "tree_source_missing")
            self.assertIsNone(report["output_bytes"])

    def test_repo_defaults_to_driver_checkout_root(self):
        self.assertTrue(hasattr(self.cache, "TREE_SOURCES"), "TREE_SOURCES is not implemented")
        expected = self.cache.source_fingerprint(HERE.parents[2], self.cache.TREE_SOURCES)
        with tempfile.TemporaryDirectory() as directory:
            captured = []
            report, calls = self.run_driver(Path(directory), capture=captured)
            self.assertEqual(calls, 1)
            self.assertEqual(report["reason"], "producer_timeout")
            self.assertEqual(report["source_fingerprint"], expected)
            self.assertEqual(captured[0]["job"]["source_fingerprint"], expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
