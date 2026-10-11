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
COLLECTOR = Path(os.environ.get("GS_QA_COLLECTOR_SOURCE", ROOT / "tests/ci/collect_production_evidence.ps1"))
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
            launch_fault = ""
            if mode == "qa_launch_fail":
                # A real launch failure after successful verifier completion:
                # retire only our stub when the collector reads its verifier
                # log. Delegate every read to the real PowerShell cmdlet.
                launch_fault = (
                    'function Get-Content { param([string]$LiteralPath, [switch]$Raw, [object]$ErrorAction)\n'
                    'if ($Raw) { Microsoft.PowerShell.Management\\Get-Content -LiteralPath $LiteralPath -Raw }\n'
                    'else { Microsoft.PowerShell.Management\\Get-Content -LiteralPath $LiteralPath }\n'
                    f'if ((Split-Path -Leaf $LiteralPath) -eq "qa_import_verify.log") {{ Remove-Item -LiteralPath "{godot}" }}\n'
                    '}\n'
                )
            wrapper.write_text(
                f'$PID | Set-Content -Encoding ASCII "{temp / "powershell-pid.txt"}"\n'
                f'$PSVersionTable.PSVersion.ToString() | Set-Content -Encoding ASCII "{temp / "powershell-version.txt"}"\n'
                'Import-Module Microsoft.PowerShell.Utility\n'
                'Import-Module Microsoft.PowerShell.Management\n'
                'function Get-Date { param([string]$Format) '
                f'if ($Format) {{ return "{TIMESTAMP}" }} '
                'return Microsoft.PowerShell.Utility\\Get-Date }\n'
                f'function New-Guid {{ return [guid]"{GUID}" }}\n' + launch_fault +
                f'& "{script}" -Root "{project}" -GodotBinary "{godot}" '
                f'-OutputRoot "{output}" -RuntimeLoops 0 -SkipPainterly '
                + ("-SkipQA" if skip else "") + "\nexit $LASTEXITCODE\n"
            )
            env = dict(os.environ, GS_QA_TRACE=str(trace), GS_QA_MODE=mode,
                       GS_QA_MARKER=PRODUCER_MARKER)
            env["PATH"] = str(tools) + os.pathsep + env["PATH"]
            input_bytes = {"collector.ps1": script.read_bytes(), "launch.ps1": wrapper.read_bytes()}
            input_bytes.update({p.name: p.read_bytes() for p in tools.glob("*.cmd")})
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
                (receipt / "inputs").mkdir()
                for name, content in input_bytes.items():
                    (receipt / "inputs" / name).write_bytes(content)
                shutil.copytree(run, receipt / "outputs")
                (receipt / "stdout.txt").write_text(process.stdout)
                (receipt / "stderr.txt").write_text(process.stderr)
                (receipt / "native-calls.txt").write_text("\n".join(calls) + "\n")
                (receipt / "receipt.json").write_text(json.dumps({
                    "classification": "SYNTHETIC_ACTUAL_POWERSHELL_ORCHESTRATION_ONLY",
                    "source_sha256": hashlib.sha256(COLLECTOR.read_bytes()).hexdigest(),
                    "input_sha256": {name: hashlib.sha256(content).hexdigest() for name, content in input_bytes.items()},
                    "powershell": POWERSHELL, "argv": process.args, "exit": process.returncode,
                    "powershell_sha256": hashlib.sha256(Path(POWERSHELL).read_bytes()).hexdigest(),
                    "powershell_pid": int((temp / "powershell-pid.txt").read_text()),
                    "powershell_version": (temp / "powershell-version.txt").read_text().strip(),
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

    def test_missing_native_exit_rejected_despite_working_verifier(self):
        # Execute the real helper, not an imitation. A working later verifier
        # cannot turn an import that never launched into a successful import.
        source = COLLECTOR.read_text(encoding="utf-8-sig")
        helper = source[source.index("function Invoke-LoggedCommand {"):
                        source.index("function Get-ResultByName {")]
        if "RequireNativeExit" not in helper:
            self.fail("QA native invocation must require its own fresh exit code")
        with tempfile.TemporaryDirectory(prefix="gs_qa_launch_") as temporary:
            temp = Path(temporary)
            script = temp / "failure.ps1"
            result = temp / "result.json"
            script.write_text(
                'Set-StrictMode -Version Latest\n$ErrorActionPreference = "Stop"\n' + helper +
                '\n$global:LASTEXITCODE = 0\n'
                f'$missing = Invoke-LoggedCommand -Name qa_import -Command @("{temp / "missing.exe"}") '
                f'-WorkingDirectory "{temp}" -LogPath "{temp / "import.log"}" -RequireNativeExit\n'
                '$command = @($env:ComSpec, "/c", "echo ' + PRODUCER_MARKER + '")\n'
                f'$healthy = Invoke-LoggedCommand -Name qa_import_verify -Command $command '
                f'-WorkingDirectory "{temp}" -LogPath "{temp / "verify.log"}" -RequireNativeExit\n'
                f'@($missing, $healthy) | ConvertTo-Json | Set-Content -Encoding UTF8 "{result}"\n'
            )
            process = subprocess.run(
                [POWERSHELL, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                 "-File", str(script)], capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            records = json.loads(result.read_text(encoding="utf-8-sig"))
            self.assertFalse(records[0]["passed"], "launch failure must not inherit native exit zero")
            self.assertIsNotNone(records[0]["error"])
            self.assertIsNone(records[0]["exit_code"], "no fabricated native completion code")
            self.assertTrue(records[1]["passed"])
            self.assertEqual(records[1]["exit_code"], 0)
            raw_log = (temp / "verify.log").read_bytes()
            decoded_log = raw_log.decode("utf-16" if raw_log.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
            self.assertIn(PRODUCER_MARKER, decoded_log)
            evidence = os.environ.get("GS_QA_PROBE_EVIDENCE_DIR")
            if evidence:
                receipt = Path(evidence) / (self._testMethodName + "_" + uuid.uuid4().hex)
                shutil.copytree(temp, receipt)
                (receipt / "stdout.txt").write_text(process.stdout)
                (receipt / "stderr.txt").write_text(process.stderr)

    def test_qa_failed_launch_has_no_completed_native_or_results(self):
        _, _, summary, _, markdown = self.run_collector("qa_launch_fail", stale=True)
        self.assertTrue(summary["qa_invocation_attempted"])
        self.assertFalse(summary["qa_actually_invoked"])
        self.assertFalse(summary["qa_runner_passed"])
        record = next(r for r in summary["results"] if r["name"] == "qa_runner")
        self.assertIsNone(record["exit_code"])
        self.assertIsNotNone(record["error"])
        self.assertIn("NOT_COMPLETED", markdown)
        self.assertRegex(markdown, r"QA results JSON:\s+NOT_RUN")
        self.assertNotIn("999", markdown)


if __name__ == "__main__":
    unittest.main(verbosity=2)
