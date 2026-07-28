#!/usr/bin/env python3
"""Unit tests for runtime renderer-proof report contracts."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "runtime" / "run_runtime_validation.py"
spec = importlib.util.spec_from_file_location("run_runtime_validation", SCRIPT)
assert spec and spec.loader
runtime_validation = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime_validation
spec.loader.exec_module(runtime_validation)


def _result(name: str, metrics: dict[str, object], status: str = "passed"):
    return runtime_validation.TestResult(
        name=name,
        command=["godot", "--script", "test.gd"],
        duration=0.1,
        exit_code=0,
        stdout="",
        stderr="",
        status=status,
        reasons=[],
        metrics=metrics,
    )


class RuntimeRendererProofContractTests(unittest.TestCase):
    def test_required_renderer_proof_passes_with_canonical_pass(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [
                _result(
                    "Canonical Node Asset Render",
                    {
                        "renderer_proof_kind": "canonical_node_asset",
                        "renderer_proof_status": "passed",
                        "asset_path": "res://tests/fixtures/test_splats.ply",
                        "visible_splats_max": 1024,
                        "visual_luma_variance_max": 0.01,
                    },
                )
            ],
            required=True,
        )

        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["passed"], 1)
        self.assertEqual(summary["failure_reasons"], [])

    def test_required_renderer_proof_fails_when_unavailable(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [
                _result(
                    "Canonical Node Asset Render",
                    {
                        "renderer_proof_kind": "canonical_node_asset",
                        "renderer_proof_status": "skipped_unavailable",
                        "reason": "local RenderingDevice required",
                    },
                    status="skipped",
                )
            ],
            required=True,
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["passed"], 0)
        self.assertEqual(summary["unavailable"], 1)
        self.assertTrue(summary["failure_reasons"])

    def test_required_renderer_proof_fails_without_proof_metrics(self) -> None:
        summary = runtime_validation._build_renderer_proof_summary(
            [_result("Unrelated Runtime Test", {"status": "passed"})],
            required=True,
        )

        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["total"], 0)
        self.assertTrue(any("No renderer proof metrics" in reason for reason in summary["failure_reasons"]))

    def test_summary_schema_accepts_renderer_proof_object(self) -> None:
        summary = {
            "total": 1,
            "passed": 1,
            "failed": 0,
            "skipped": 0,
            "duration": 0.1,
            "tests": [
                {
                    "name": "Canonical Node Asset Render",
                    "status": "passed",
                    "reasons": [],
                    "command": ["godot"],
                    "duration": 0.1,
                    "exit_code": 0,
                    "metrics": {},
                    "output_tail": [],
                }
            ],
            "renderer_proof": runtime_validation._build_renderer_proof_summary([], required=False),
        }

        self.assertEqual(runtime_validation._validate_summary_schema(summary), [])


class RuntimeCrashDiagnosticRetentionTests(unittest.TestCase):
    """#787: a crashed scenario must keep the output that names why it died.

    The nightly GPU streaming lane died on a fatal out-of-bounds trap. Godot prints the
    identifying diagnostic immediately before aborting, the harness captured it, and the
    summary then dropped it -- reporting instead the FIRST stderr line, a benign startup
    warning. These cases pin the tail so that cannot recur.
    """

    # The real shape of the #787 crash: benign warning first, fatal diagnostic last.
    CRASH_STDERR = "\n".join(
        [
            "ERROR: Can't create an accessibility driver, accessibility support disabled!",
            "WARNING: [Streaming] Clamping effective max chunks from 128 to 48.",
            "FATAL: Index p_index = 4096 is out of bounds (size() = 0).",
            "   at: VectorWriteProxy<PackedGaussian>::operator[] (core/templates/vector.h:54)",
        ]
    )

    def _crashed(self):
        return runtime_validation.TestResult(
            name="GPU Streaming Stress",
            command=["godot", "--script", "test_gpu_streaming_stress.gd"],
            duration=134.4,
            exit_code=3221226505,  # 0xC0000409, the __fastfail(7) exit
            stdout="",
            stderr=self.CRASH_STDERR,
            status="failed",
            reasons=["ERROR: Can't create an accessibility driver, accessibility support disabled!"],
            metrics={},
        )

    def test_crash_summary_retains_the_fatal_line_not_just_the_first(self) -> None:
        summary = runtime_validation.summarise([self._crashed()])
        tail = summary["tests"][0]["output_tail"]

        joined = "\n".join(tail)
        self.assertIn("FATAL: Index p_index = 4096 is out of bounds (size() = 0).", joined)
        self.assertIn("vector.h:54", joined)
        # The laundered first line must not be the only thing preserved.
        self.assertNotEqual(tail, summary["tests"][0]["reasons"])

    def test_passing_scenario_carries_an_empty_tail(self) -> None:
        summary = runtime_validation.summarise([_result("Interactive State", {})])
        self.assertEqual(summary["tests"][0]["output_tail"], [])

    def test_tail_is_bounded(self) -> None:
        noisy = runtime_validation.TestResult(
            name="Noisy",
            command=["godot"],
            duration=1.0,
            exit_code=1,
            stdout="\n".join(f"line {i}" for i in range(500)),
            stderr="FATAL: the last line",
            status="failed",
            reasons=["boom"],
            metrics={},
        )
        tail = noisy.output_tail()
        self.assertLessEqual(len(tail), runtime_validation.OUTPUT_TAIL_LINES)
        self.assertEqual(tail[-1], "FATAL: the last line")

    def test_schema_rejects_a_failed_scenario_with_no_output_tail(self) -> None:
        summary = runtime_validation.summarise([self._crashed()])
        summary["tests"][0]["output_tail"] = []
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(
            any("output_tail" in error for error in errors),
            f"schema accepted a failed scenario with no retained output: {errors}",
        )

    def test_schema_rejects_a_missing_output_tail(self) -> None:
        summary = runtime_validation.summarise([self._crashed()])
        del summary["tests"][0]["output_tail"]
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(
            any("output_tail" in error for error in errors),
            f"schema accepted a summary with no output_tail key: {errors}",
        )


if __name__ == "__main__":
    unittest.main()
