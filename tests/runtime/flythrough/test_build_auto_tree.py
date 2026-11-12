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
    "oracle_complete_all_poses", "leaves_only",
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
        for key in ("schema", "codec", "source_fingerprint", "source_fingerprint_source",
                    "drawn_budget_source", "limits", "oracle"):
            job = example_job()
            job[key] = 1
            with self.subTest(key=key):
                self.assert_reason("job_manifest_type", self.driver.validate_job_manifest, job)
        for value in ([], None, "job"):
            self.assert_reason("job_manifest_type", self.driver.validate_job_manifest, value)

    def test_invalid_values_and_nonfinite_limits(self):
        cases = [("schema", "godotgs-auto-tree-job/2"), ("codec", "unknown"),
                 ("worker_count", 0), ("worker_count", 65), ("max_tile_splats", 16384),
                 ("drawn_budget_splats", 0), ("source_fingerprint", "A" * 64),
                 ("source_fingerprint", "a" * 63), ("source_fingerprint", "g" * 64),
                 ("drawn_budget_source", ""), ("source_fingerprint_source", "")]
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
        for field in ("wall_time_sec", "output_bytes", "node_count", "height"):
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



if __name__ == "__main__":
    unittest.main(verbosity=2)
