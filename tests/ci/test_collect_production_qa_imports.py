#!/usr/bin/env python3
"""Native PowerShell orchestration controls; no Godot/import/GPU qualification.

Run this file directly with Windows Python. Fake .cmd tools exercise the actual
collector, while private temporary files distinguish fresh from stale outputs.
The positive verifier line was captured from baseline QA run 38131297064,
job 114445998782 (2026-10-11); it is a producer-format fixture, not invented
native evidence. Other output is explicitly synthetic control data.
This focused test is not registered in run_module_tests.py's guard lane.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[2]
COLLECTOR = ROOT / "tests/ci/collect_production_evidence.ps1"
POWERSHELL = shutil.which("powershell.exe")
PRODUCER_MARKER = "QA_IMPORTS_VERIFIED count=11 skipped=0"
GUID = "01234567-89ab-cdef-0123-456789abcdef"
TIMESTAMP = "20000101_000000"


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "requires native Windows PowerShell")
class CollectorQAImports(unittest.TestCase):
    def run_collector(self, mode="ok", *, skip=False, stale=False, collision=False):
        with tempfile.TemporaryDirectory(prefix="gs_qa_import_") as temporary:
            temp = Path(temporary)
            project = temp / "repo"
            scripts = project / "tests/ci"
            scripts.mkdir(parents=True)
            script = scripts / "collect_production_evidence.ps1"
            script.write_bytes(COLLECTOR.read_bytes())
            tools = temp / "tools"
            tools.mkdir()
            output = temp / "output"
            run = output / TIMESTAMP
            run.mkdir(parents=True)
            trace = temp / "native-calls.txt"
            if stale:
                stale_json = {"summary": {"passed": 999, "failed": 0}, "results": []}
                (run / "qa_results.json").write_text(json.dumps(stale_json))
                (run / "qa_runner.log").write_text("Invalid ID STALE_PARENT_QA\n")
                older = run / "qa_old"
                older.mkdir()
                (older / "qa_runner.log").write_text("Invalid ID STALE_NESTED_QA\n")
            if collision:
                (run / ("qa_" + GUID)).mkdir()
            # These stubs are orchestration signals, not native module evidence.
            (tools / "git.cmd").write_text("@echo off\necho SYNTHETIC_SOURCE_ID\nexit /b 0\n")
            (tools / "python.cmd").write_text(
                '@echo off\necho PYTHON %*>>"%GS_QA_TRACE%"\necho SYNTHETIC_MODULE_CONTROL\nexit /b 0\n'
            )
            godot = tools / "fake-godot.cmd"
            godot.write_text(
                '@echo off\n'
                'echo GODOT %*>>"%GS_QA_TRACE%"\n'
                'if "%~1"=="--version" (echo SYNTHETIC_VERSION& exit /b 0)\n'
                'if "%~4"=="--import" goto import\n'
                'if "%~4"=="--script" goto verify\n'
                ':args\n'
                'if "%~1"=="" goto runtime\n'
                'if "%~1"=="--qa-output" goto qa\n'
                'shift\ngoto args\n'
                ':import\n'
                'echo SYNTHETIC_IMPORT_CONTROL\n'
                'if "%GS_QA_MODE%"=="import_fail" exit /b 7\n'
                'exit /b 0\n'
                ':verify\n'
                'if "%GS_QA_MODE%"=="verify_fail" exit /b 9\n'
                'if "%GS_QA_MODE%"=="missing" exit /b 0\n'
                'if "%GS_QA_MODE%"=="embedded" (echo prefix %GS_QA_MARKER%& exit /b 0)\n'
                'if "%GS_QA_MODE%"=="malformed" (echo QA_IMPORTS_VERIFIED count=0 skipped=0& exit /b 0)\n'
                'echo %GS_QA_MARKER%\n'
                'if "%GS_QA_MODE%"=="duplicate" echo %GS_QA_MARKER%\n'
                'exit /b 0\n'
                ':qa\n'
                'echo {"summary":{"passed":7,"failed":4},"results":[]} >"%~2"\n'
                'echo SYNTHETIC_QA_CONTROL\n'
                'if "%GS_QA_MODE%"=="qa_fail" exit /b 11\n'
                'exit /b 0\n'
                ':runtime\n'
                'echo SYNTHETIC_RUNTIME_CONTROL\nexit /b 0\n'
            )
            if mode == "launch_fail":
                # A resolvable directory is not a native executable. Runtime calls
                # leave legacy LASTEXITCODE state behind; scoped QA must reject it.
                godot = tools
            wrapper = temp / "launch.ps1"
            wrapper.write_text(
                'function Get-Date { param([string]$Format) '
                f'if ($Format) {{ return "{TIMESTAMP}" }} '
                'return Microsoft.PowerShell.Utility\Get-Date }\n'
                f'function New-Guid {{ return [guid]"{GUID}" }}\n'
                f'& "{script}" -Root "{project}" -GodotBinary "{godot}" '
                f'-OutputRoot "{output}" -RuntimeLoops 0 -SkipPainterly '
                + ("-SkipQA" if skip else "") + "\n"
            )
            env = dict(os.environ, GS_QA_TRACE=str(trace), GS_QA_MODE=mode,
                       GS_QA_MARKER=PRODUCER_MARKER)
            env["PATH"] = str(tools) + os.pathsep + env["PATH"]
            process = subprocess.run(
                [POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                 "-File", str(wrapper)], env=env, capture_output=True, text=True,
                timeout=30,
            )
            calls = trace.read_text().splitlines() if trace.exists() else []
            summary_path = run / "summary.json"
            self.assertTrue(summary_path.exists(), "probe setup/collector abort: " + process.stdout + process.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8-sig"))
            signatures = (run / "signature_hits.txt").read_text(encoding="utf-8-sig")
            markdown = (run / "issue_871.md").read_text(encoding="utf-8-sig")
            evidence = os.environ.get("GS_QA_PROBE_EVIDENCE_DIR")
            if evidence:
                receipt = Path(evidence) / (self._testMethodName + "_" + mode + "_" + uuid.uuid4().hex)
                receipt.mkdir(parents=True)
                shutil.copytree(run, receipt / "outputs")
                (receipt / "stdout.txt").write_text(process.stdout)
                (receipt / "stderr.txt").write_text(process.stderr)
                (receipt / "native-calls.txt").write_text("\n".join(calls) + "\n")
                (receipt / "receipt.json").write_text(json.dumps({
                    "classification": "SYNTHETIC_ACTUAL_POWERSHELL_ORCHESTRATION_ONLY",
                    "source_sha256": hashlib.sha256(COLLECTOR.read_bytes()).hexdigest(),
                    "powershell": POWERSHELL, "argv": process.args, "exit": process.returncode,
                    "mode": mode, "SkipQA": skip, "stale": stale, "collision": collision,
                }, indent=2))
            return process.returncode, calls, summary, signatures, markdown

    @staticmethod
    def phases(calls):
        return ["import" if "--import" in c else "verify" if "verify_qa_imports.gd" in c
                else "qa" if "--qa-output" in c else "other" for c in calls]

    def test_ordered_import_verifier_qa(self):
        rc, calls, summary, _, markdown = self.run_collector()
        self.assertEqual([p for p in self.phases(calls) if p != "other"], ["import", "verify", "qa"],
                         "QA requires actual import then verifier before its invocation")
        self.assertEqual(rc, 0)
        self.assertEqual([r["name"] for r in summary["results"][:5]],
                         ["module_tests", "runtime_interactive_state", "runtime_engine_capabilities",
                          "runtime_gpu_streaming_stress", "runtime_world_streaming_gate"])
        qa = next(r for r in summary["results"] if r["name"] == "qa_runner")
        self.assertIn("qa_" + GUID, qa["log_path"])
        self.assertIn("qa_" + GUID, markdown)
        self.assertNotIn("/" + TIMESTAMP + "/qa_runner.log", markdown.replace("\\", "/"))

    def test_import_failure_blocks_verifier_and_qa(self):
        rc, calls, summary, _, _ = self.run_collector("import_fail")
        self.assertEqual([p for p in self.phases(calls) if p != "other"], ["import"])
        self.assertEqual(rc, 1)
        self.assertEqual(next(r for r in summary["results"] if r["name"] == "qa_import")["exit_code"], 7)
        self.assertFalse(summary["qa_runner_passed"])

    def test_verifier_failure_blocks_qa(self):
        rc, calls, summary, _, _ = self.run_collector("verify_fail")
        self.assertEqual([p for p in self.phases(calls) if p != "other"], ["import", "verify"])
        self.assertEqual(rc, 1)
        self.assertEqual(next(r for r in summary["results"] if r["name"] == "qa_import_verify")["exit_code"], 9)

    def test_actual_zero_without_valid_marker_fails_semantically(self):
        for mode in ("missing", "embedded", "malformed", "duplicate"):
            with self.subTest(mode=mode):
                rc, calls, summary, _, _ = self.run_collector(mode)
                self.assertEqual([p for p in self.phases(calls) if p != "other"], ["import", "verify"])
                self.assertEqual(rc, 1)
                record = next(r for r in summary["results"] if r["name"] == "qa_import_verify")
                self.assertEqual(record["exit_code"], 0, "actual exit must not be rewritten")
                self.assertFalse(record["passed"])

    def test_stale_outputs_not_consumed_or_linked(self):
        for mode, skip in (("missing", False), ("ok", True), ("ok", False)):
            with self.subTest(mode=mode, skip=skip):
                _, _, summary, signatures, markdown = self.run_collector(mode, skip=skip, stale=True)
                self.assertNotIn("STALE_", signatures)
                self.assertNotIn("999", markdown)
                self.assertFalse(summary["qa_streaming_visual_present"])
                self.assertEqual(summary["qa_streaming_scene_count"], 0)

    def test_directory_collision_blocks_all_qa_calls(self):
        rc, calls, summary, _, _ = self.run_collector(collision=True)
        self.assertFalse(any(p != "other" for p in self.phases(calls)))
        self.assertEqual(rc, 1)
        self.assertFalse(summary["qa_runner_passed"])

    def test_skip_qa_preserves_other_calls(self):
        rc, calls, summary, _, _ = self.run_collector(skip=True)
        self.assertEqual(rc, 0)
        self.assertFalse(any(p != "other" for p in self.phases(calls)))
        self.assertEqual(len(summary["results"]), 5)
        self.assertFalse(summary["qa_runner_passed"])

    def test_native_launch_failure_not_masked_by_previous_exit(self):
        rc, calls, summary, _, _ = self.run_collector("launch_fail", stale=True)
        self.assertEqual(rc, 1)
        self.assertFalse(any(p != "other" for p in self.phases(calls)))
        record = next(r for r in summary["results"] if r["name"] == "qa_import")
        self.assertIsNotNone(record["error"])
        self.assertFalse(record["passed"])
        self.assertFalse(summary["qa_runner_passed"])

    def test_fresh_adverse_qa_results_retained(self):
        _, calls, summary, _, markdown = self.run_collector("qa_fail")
        self.assertEqual([p for p in self.phases(calls) if p != "other"], ["import", "verify", "qa"])
        self.assertFalse(summary["qa_runner_passed"])
        self.assertIn("7 / 4", markdown)
        self.assertEqual(next(r for r in summary["results"] if r["name"] == "qa_runner")["exit_code"], 11)


if __name__ == "__main__":
    unittest.main(verbosity=2)
