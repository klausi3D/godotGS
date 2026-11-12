#!/usr/bin/env python3
"""D2-4 real producer contract (CPU only, tests=yes binary selected by CI).

CLI contract under test:
  build_auto_tree.py --godot-binary B --input W --job-manifest J --out V4 --report R
The driver resolves C0..C7 from W's sidecar via probe_path, calls the bound
production selector, and writes a report even on failure. Exit 0 = complete,
1 = failed job, 2 = usage error. No render loop or RenderingDevice is needed.

No selected binary => explicit unittest skip / mode NOT_RUN. A selected binary,
or GS_REQUIRE_AUTO_TREE_BINARY=1, makes every missing prerequisite a FAILURE.
All generated artifacts live in a TemporaryDirectory, never in the repository.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_build_auto_tree import DRIVER, HERE, REPORT_FIELDS, example_job, load_module


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class BinaryContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = os.environ.get("GODOT_BINARY", "")
        cls.required = os.environ.get("GS_REQUIRE_AUTO_TREE_BINARY") == "1"
        if not cls.binary and not cls.required:
            print("D2-4 mode=NOT_RUN: no Godot binary selected", flush=True)
            raise unittest.SkipTest("binary_not_selected: CPU producer contract NOT RUN")
        print(f"D2-4 mode=binary-required CPU-only binary={cls.binary!r}", flush=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="gs_d2_driver_test_")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.world = cls.root / "input/world.gsplatworld"
        cls.built = None

    def setUp(self):
        self.assertTrue(self.binary, "binary_required: GODOT_BINARY was not supplied")
        binary = shutil.which(self.binary)
        self.assertTrue(binary, f"binary_missing: {self.binary}")
        self.binary = str(Path(binary).resolve())
        self.assertTrue(DRIVER.is_file(), "driver_missing: tests/runtime/flythrough/build_auto_tree.py")
        self.assertTrue((HERE / "build_auto_tree.gd").is_file(), "driver_script_missing: build_auto_tree.gd")

    def run_command(self, args):
        try:
            return subprocess.run(args, text=True, capture_output=True, timeout=300, check=False)
        except (OSError, subprocess.TimeoutExpired) as exc:
            self.fail(f"producer_execution_failed: {exc}")

    def generate(self):
        if self.world.is_file():
            return
        result = self.run_command([sys.executable, "-B", str(HERE / "generate_world.py"),
            "--godot-binary", self.binary, "--tier", "T-smoke", "--encoding", "plain_v3_gsq",
            "--out", str(self.world.parent)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(self.world.is_file(), "generator_missing_output")
        manifest = json.loads(self.world.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        self.assertGreater(manifest["world"]["splat_count"], 16384)
        self.assertLessEqual(manifest["world"]["splat_count"], 1000000)

    def invoke(self, name, *, job=None, raw_job=None, input_path=None, missing_job=False):
        self.generate()
        directory = self.root / name
        directory.mkdir(exist_ok=True)
        job_path = directory / "job.json"
        if not missing_job:
            job_path.write_text(raw_job if raw_job is not None else json.dumps(job or example_job()), encoding="utf-8")
        output = directory / "tree.gsplatworld"
        report_path = directory / "report.json"
        result = self.run_command([sys.executable, "-B", str(DRIVER), "--godot-binary", self.binary,
            "--input", str(input_path or self.world), "--job-manifest", str(job_path),
            "--out", str(output), "--report", str(report_path)])
        self.assertTrue(report_path.is_file(), "build_report_missing: " + result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertTrue(REPORT_FIELDS <= report.keys(), REPORT_FIELDS - report.keys())
        self.assertEqual(report["schema"], "godotgs-auto-tree-build-report/1")
        self.assertIsInstance(report["reason"], str)
        json.dumps(report, allow_nan=False)
        return result, report, output, job_path

    def good_builds(self):
        if self.__class__.built is None:
            self.__class__.built = [self.invoke("first"), self.invoke("repeat")]
        for result, report, output, job_path in self.built:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr + str(report))
            self.assertEqual(report["status"], "complete")
            self.assertEqual(report["reason"], "")
            self.assertTrue(output.is_file())
        return self.built

    def test_repeat_build_identity_and_measured_report(self):
        builds = self.good_builds()
        for _, report, output, job_path in builds:
            manifest_path = output.with_suffix(".manifest.json")
            self.assertTrue(manifest_path.is_file(), "sidecar_missing")
            sidecar = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(sidecar["codec"]["codec"], "gsq")
            self.assertEqual(report["output_file_sha256"], sha256(output))
            self.assertEqual(report["output_file_sha256"], sidecar["output_file_sha256"])
            self.assertEqual(report["input_file_sha256"], sha256(self.world))
            self.assertEqual(report["job_manifest_sha256"], sha256(job_path))
            self.assertEqual(report["output_bytes"], output.stat().st_size + manifest_path.stat().st_size)
            self.assertEqual(report["levels"], sidecar["levels"])
            self.assertTrue(report["levels"])
            self.assertEqual(report["height"], max(level["depth"] for level in sidecar["levels"]))
            self.assertEqual(report["node_count"], sidecar["totals"]["node_count"])
            self.assertGreater(report["node_count"], 1)
            for key in ("tree_sha", "content_generation", "source_fingerprint"):
                self.assertEqual(report[key], sidecar[key])
            self.assertRegex(report["tree_sha"], r"^[0-9a-f]{64}$")
            self.assertIsInstance(report["content_generation"], str)
            self.assertGreater(int(report["content_generation"]), 0)
            self.assertGreater(report["wall_time_sec"], 0)
            for field in ("peak_rss_bytes", "scratch_bytes"):
                measurement = report[field]
                self.assertIsInstance(measurement["available"], bool)
                if measurement["available"]:
                    self.assertIsInstance(measurement["source"], str)
                    self.assertIs(type(measurement["value"]), int)
                    self.assertGreaterEqual(measurement["value"], 0)
                else:
                    self.assertIsNone(measurement.get("value"))
                    self.assertTrue(measurement["reason"])
            if report["peak_rss_bytes"]["available"]:
                self.assertGreater(report["peak_rss_bytes"]["value"], 0)
                self.assertIn(report["peak_rss_bytes"]["source"], ("process_rss", "child_ru_maxrss"))
            # Current builder has no scratch measurement; samples cannot certify a peak.
            self.assertIs(report["scratch_bytes"]["available"], False)
            self.assertEqual(report["source_fingerprint_source"], example_job()["source_fingerprint_source"])
        for key in ("content_generation", "output_file_sha256", "tree_sha"):
            self.assertEqual(builds[0][1][key], builds[1][1][key])

    def test_eight_checkpoint_oracle_and_separate_leaves_control(self):
        report = self.good_builds()[0][1]
        path_module = load_module("d2_probe_path", HERE / "probe_path.py")
        sidecar = json.loads(self.world.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        expected = path_module.checkpoints(path_module.resolve_path(sidecar))
        self.assertEqual([name for name, _ in expected], [f"C{i}" for i in range(8)])
        self.assertIs(report["budget_frozen"], False)
        self.assertEqual(report["drawn_budget_source"], example_job()["drawn_budget_source"])
        self.assertEqual(report["drawn_budget_splats"], example_job()["drawn_budget_splats"])
        self.assertEqual(report["oracle"]["camera"], example_job()["oracle"])
        self.assertAlmostEqual(report["oracle"]["focal_pixels"], 960 / math.tan(math.radians(107.5) / 2))
        self.assertIs(report["oracle_complete_all_poses"], True)
        self.assertEqual(report["leaves_only"]["mechanism"], "stale_interiors_unbounded")
        for block in ("oracle", "leaves_only"):
            poses = report[block]["poses"]
            self.assertEqual(len(poses), 8)
            self.assertEqual([p["checkpoint"] for p in poses], [name for name, _ in expected])
            for row, (_, pose) in zip(poses, expected):
                self.assertEqual(row["pose"], pose)
                self.assertIn(row["status"], ("COMPLETE", "EMPTY"))
                self.assertIs(type(row["selected_splats"]), int)
                self.assertIs(type(row["selected_count"]), int)
                self.assertIsInstance(row["budget_bound"], bool)
                self.assertEqual(row["budget"], report["drawn_budget_splats"])
                self.assertIs(row["drawn_le_budget"], row["selected_splats"] <= row["budget"])
                if block == "oracle":
                    self.assertEqual(row["status"], "COMPLETE")
                    self.assertGreater(row["selected_splats"], 0)
                    self.assertGreater(row["selected_count"], 0)
                    self.assertIs(row["drawn_le_budget"], True)
                else:
                    self.assertIs(row["only_leaves"], True)

    def test_failure_controls_have_distinct_reasons(self):
        low_budget = example_job()
        low_budget["drawn_budget_splats"] = 1
        low_ram = example_job()
        low_ram["limits"]["max_ram_bytes"] = 1
        cases = [
            ("missing-input", {"input_path": self.root / "absent.gsplatworld"}, "input_not_found"),
            ("missing-job", {"missing_job": True}, "job_manifest_missing"),
            ("corrupt-job", {"raw_job": "{broken"}, "job_manifest_corrupt"),
            ("invalid-job", {"raw_job": "[]"}, "job_manifest_type"),
            ("builder-error", {"job": low_ram}, "limit_ram"),
            ("budget", {"job": low_budget}, "oracle_budget_infeasible"),
        ]
        reasons = []
        for name, kwargs, reason in cases:
            with self.subTest(control=name):
                result, report, _, _ = self.invoke(name, **kwargs)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertEqual(report["status"], "failed")
                self.assertEqual(report["reason"], reason)
                self.assertIs(report["oracle_complete_all_poses"], False)
                reasons.append(report["reason"])
                if name == "budget":
                    self.assertEqual(len(report["oracle"]["poses"]), 8)
                    self.assertIn("BOOTSTRAP_INFEASIBLE", [p["status"] for p in report["oracle"]["poses"]])
                    # A failed budget does not cap or suppress the S1 control.
                    leaves = report["leaves_only"]["poses"]
                    self.assertEqual(len(leaves), 8)
                    self.assertTrue(any(p["selected_splats"] > 1 and not p["drawn_le_budget"] for p in leaves))
                    self.assertEqual([p["selected_splats"] for p in leaves],
                        [p["selected_splats"] for p in self.good_builds()[0][1]["leaves_only"]["poses"]])
        self.assertEqual(len(reasons), len(set(reasons)))

    def test_leaves_over_budget_does_not_fail_a_complete_main_oracle(self):
        job = example_job()
        # Rule-1 parents contain at most 16384 splats. A deliberately coarse
        # quality request admits the root while visible full-detail leaves cost
        # more. This is a diagnostic control, not a proposed lane tau/budget.
        job["drawn_budget_splats"] = 16384
        job["oracle"]["tau_pixels"] = 1.0e30
        result, report, _, _ = self.invoke("coarse-control", job=job)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + str(report))
        self.assertEqual(report["status"], "complete")
        self.assertIs(report["oracle_complete_all_poses"], True)
        self.assertEqual(len(report["oracle"]["poses"]), 8)
        self.assertTrue(all(p["status"] == "COMPLETE" and p["drawn_le_budget"]
                            for p in report["oracle"]["poses"]))
        leaves = report["leaves_only"]["poses"]
        self.assertEqual(len(leaves), 8)
        self.assertTrue(any(p["selected_splats"] > job["drawn_budget_splats"] and not p["drawn_le_budget"]
                            for p in leaves), "leaves-only forcing control did not exceed the budget")


if __name__ == "__main__":
    unittest.main(verbosity=2)
