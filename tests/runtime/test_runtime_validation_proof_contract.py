#!/usr/bin/env python3
"""Unit tests for runtime renderer-proof and completion-marker report contracts.

The completion-marker classes below are the discrimination proof for T3
(#891): `passed` must be reachable only from a well-formed, correctly-bound
[RUNTIME_PASS] marker, a clean exit without one must classify as the advisory
`no_completion_marker` status (never `passed`), and a forged marker -- wrong
scenario name, malformed payload, duplicate lines, untracked zero-assertion
claims -- must classify as `failed`. Restoring the pre-#891 pass-as-fall-through
turns several of these RED; that is the point.

This file is executed by run_module_tests.py's guard lane
(_run_runtime_validation_contract_guard); its wiring is pinned by the derived
contract in tests/ci/test_run_module_tests_lane_ledger.py, which a different
runner entry executes.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "runtime" / "run_runtime_validation.py"
BINDINGS = ROOT / "modules" / "gaussian_splatting" / "renderer" / "gaussian_splat_renderer_bindings.cpp"
RENDERER_HEADER = ROOT / "modules" / "gaussian_splatting" / "renderer" / "gaussian_splat_renderer.h"
RENDERER_DOC = ROOT / "modules" / "gaussian_splatting" / "doc_classes" / "GaussianSplatRenderer.xml"
CANONICAL_RENDER_PROOF = ROOT / "tests" / "runtime" / "test_canonical_node_asset_render.gd"
spec = importlib.util.spec_from_file_location("run_runtime_validation", SCRIPT)
assert spec and spec.loader
runtime_validation = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime_validation
spec.loader.exec_module(runtime_validation)


def _without_cpp_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)


def _canonical_deadline_placement_errors(script: str) -> list[str]:
    """Return structural errors that could let an over-budget frame pass."""
    run_body = script.split("func _run() -> void:", 1)[1].split("\n\nfunc _read_renderer_stats", 1)[0]
    proof_loop = run_body.split(
        "\twhile Time.get_ticks_msec() < proof_deadline_msec:", 1
    )[1].split("\n\n\tif stage_failure_seen:", 1)[0]
    lines = proof_loop.splitlines()
    deadline_check = "if Time.get_ticks_msec() >= proof_deadline_msec:"
    errors: list[str] = []

    expected_await_guards = (
        ("\t\tawait process_frame", f"\t\t{deadline_check}"),
        ("\t\t\tawait RenderingServer.frame_post_draw", f"\t\t\t{deadline_check}"),
    )
    for await_line, expected_guard in expected_await_guards:
        await_indices = [index for index, line in enumerate(lines) if line == await_line]
        if len(await_indices) != 1:
            errors.append(f"expected exactly one proof-loop {await_line.strip()!r}")
            continue
        guard_index = await_indices[0] + 1
        if guard_index >= len(lines) or lines[guard_index] != expected_guard:
            errors.append(f"{await_line.strip()} must be followed immediately by the deadline check")
            continue
        guard_indent = len(expected_guard) - len(expected_guard.lstrip("\t"))
        guard_body = []
        for line in lines[guard_index + 1 :]:
            if line and len(line) - len(line.lstrip("\t")) <= guard_indent:
                break
            guard_body.append(line)
        direct_break = "\t" * (guard_indent + 1) + "break"
        if direct_break not in guard_body:
            errors.append(f"the deadline check after {await_line.strip()} must break the proof loop")

    pass_indices = [index for index, line in enumerate(lines) if line.startswith("\t\t\t_pass(")]
    if len(pass_indices) != 1:
        errors.append("expected exactly one passing terminal in the proof loop")
    else:
        pass_index = pass_indices[0]
        expected_terminal_guard = [f"\t\t\t{deadline_check}", "\t\t\t\tbreak"]
        if lines[max(0, pass_index - 2) : pass_index] != expected_terminal_guard:
            errors.append("the passing terminal must be immediately bound by a deadline check")

    if proof_loop.count(deadline_check) != 3:
        errors.append("the proof loop must contain exactly the two post-await and one pre-pass deadline checks")
    return errors


def _canonical_success_guard_errors(script: str) -> list[str]:
    """Return errors that could permit proof after a latched stage failure."""
    run_body = script.split("func _run() -> void:", 1)[1].split("\n\nfunc _read_renderer_stats", 1)[0]
    proof_loop = run_body.split(
        "\twhile Time.get_ticks_msec() < proof_deadline_msec:", 1
    )[1].split("\n\n\tif stage_failure_seen:", 1)[0]
    lines = proof_loop.splitlines()
    pass_indices = [index for index, line in enumerate(lines) if line.startswith("\t\t\t_pass(")]
    if len(pass_indices) != 1:
        return ["expected exactly one passing terminal in the proof loop"]

    success_guard = next(
        (line for line in reversed(lines[: pass_indices[0]]) if line.startswith("\t\tif ")),
        "",
    )
    if "not stage_failure_seen" not in success_guard:
        return ["the passing branch must reject any latched renderer-stage failure"]
    return []


def _canonical_post_draw_stage_resample_errors(script: str) -> list[str]:
    """Return errors that could hide a failure in the just-drawn frame."""
    run_body = script.split("func _run() -> void:", 1)[1].split("\n\nfunc _read_renderer_stats", 1)[0]
    proof_loop = run_body.split(
        "\twhile Time.get_ticks_msec() < proof_deadline_msec:", 1
    )[1].split("\n\n\tif stage_failure_seen:", 1)[0]
    post_draw = proof_loop.split("\t\t\tawait RenderingServer.frame_post_draw", 1)[1]
    before_capture = post_draw.split("\t\t\tvar image := _capture_viewport()", 1)[0]
    required_block = (
        "\t\t\tvar post_draw_stats := _read_renderer_stats()\n"
        "\t\t\t_update_stage_metrics(post_draw_stats)\n"
        "\t\t\tstage_failure_seen = stage_failure_seen or _stage_failed(post_draw_stats)"
    )
    if required_block not in before_capture:
        return ["the drawn frame's stage status must be sampled and latched before visual proof"]
    return []


class RenderedContentBindingContractTests(unittest.TestCase):
    """#941: the fail-closed renderer predicate must be script-reachable.

    The canonical runtime scenario already requires both probe availability and
    observed content. These source checks pin the public binding that lets the
    scenario call the production C++ predicate; deleting only that binding must
    make this suite fail before a costly runtime lane is attempted.
    """

    def test_existing_cpp_predicate_is_bound_once_for_gdscript(self) -> None:
        header = RENDERER_HEADER.read_text(encoding="utf-8")
        self.assertIn("bool has_rendered_content() const override;", header)

        bindings = _without_cpp_comments(BINDINGS.read_text(encoding="utf-8"))
        pattern = re.compile(
            r'ClassDB::bind_method\(\s*D_METHOD\(\s*"has_rendered_content"\s*\)\s*,\s*'
            r'&GaussianSplatRenderer::has_rendered_content\s*\)\s*;'
        )
        self.assertEqual(
            len(pattern.findall(bindings)),
            1,
            "has_rendered_content must have exactly one active ClassDB binding to the existing predicate",
        )

    def test_script_visible_predicate_is_documented(self) -> None:
        documentation = RENDERER_DOC.read_text(encoding="utf-8")
        self.assertEqual(
            len(re.findall(r'<method\s+name="has_rendered_content"\s*>', documentation)),
            1,
            "GaussianSplatRenderer must document the script-visible has_rendered_content method exactly once",
        )

    def test_canonical_proof_uses_a_monotonic_wall_clock_deadline(self) -> None:
        script = CANONICAL_RENDER_PROOF.read_text(encoding="utf-8")
        self.assertNotIn(
            "MAX_PROOF_FRAMES",
            script,
            "rendered-content proof must not expire according to runner-dependent frame throughput",
        )
        self.assertRegex(script, r"const PROOF_TIMEOUT_MSEC\s*:=\s*[1-9][0-9_]*")
        self.assertRegex(
            script,
            r"var proof_started_msec\s*:=\s*Time\.get_ticks_msec\(\)",
        )
        self.assertRegex(
            script,
            r"var proof_deadline_msec\s*:=\s*proof_started_msec\s*\+\s*PROOF_TIMEOUT_MSEC",
        )
        self.assertRegex(
            script,
            r"while Time\.get_ticks_msec\(\)\s*<\s*proof_deadline_msec:",
            "canonical proof must keep pumping frames until success or its monotonic deadline",
        )
        self.assertEqual(
            _canonical_deadline_placement_errors(script),
            [],
            "deadline checks must bind both proof-loop awaits and the sole passing terminal",
        )
        self.assertEqual(
            _canonical_success_guard_errors(script),
            [],
            "the passing terminal must be unreachable after any renderer-stage failure",
        )
        self.assertEqual(
            _canonical_post_draw_stage_resample_errors(script),
            [],
            "the just-drawn frame's stage status must be sampled before visual proof can pass",
        )
        run_body = script.split("func _run() -> void:", 1)[1].split("\n\nfunc _read_renderer_stats", 1)[0]
        self.assertEqual(
            len(re.findall(r"^\s*_pass\(", run_body, flags=re.MULTILINE)),
            1,
            "only the in-budget conjunctive branch may emit a passing terminal",
        )
        self.assertIn(
            '_fail("Canonical node asset proof exceeded its wall-clock deadline.")',
            run_body,
            "deadline exhaustion must end fail-closed instead of falling through to pass",
        )

    def test_deadline_placement_contract_rejects_misplaced_checks_even_when_count_is_unchanged(self) -> None:
        script = CANONICAL_RENDER_PROOF.read_text(encoding="utf-8")
        deadline_check = "if Time.get_ticks_msec() >= proof_deadline_msec:"
        process_guard = (
            f"\t\tawait process_frame\n\t\t{deadline_check}\n"
            '\t\t\tmetrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec\n'
            "\t\t\tbreak"
        )
        frame_post_draw_guard = (
            f"\t\t\tawait RenderingServer.frame_post_draw\n\t\t\t{deadline_check}\n"
            '\t\t\t\tmetrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec\n'
            "\t\t\t\tbreak"
        )
        terminal_guard = f"\t\t\t{deadline_check}\n\t\t\t\tbreak\n\t\t\t_pass("
        duplicated_terminal_guard = f"\t\t\t{deadline_check}\n\t\t\t\tbreak\n{terminal_guard}"

        process_mutated = script.replace(
            process_guard,
            "\t\tawait process_frame",
            1,
        ).replace(terminal_guard, duplicated_terminal_guard, 1)
        frame_post_draw_mutated = script.replace(
            frame_post_draw_guard,
            "\t\t\tawait RenderingServer.frame_post_draw",
            1,
        ).replace(terminal_guard, duplicated_terminal_guard, 1)
        terminal_mutated = script.replace(
            process_guard,
            f"{process_guard}\n\t\t{deadline_check}\n\t\t\tbreak",
            1,
        ).replace(terminal_guard, "\t\t\t_pass(", 1)
        nested_break_mutated = script.replace(
            "\t\t\tmetrics[\"proof_elapsed_msec\"] = Time.get_ticks_msec() - proof_started_msec\n"
            "\t\t\tbreak",
            "\t\t\tmetrics[\"proof_elapsed_msec\"] = Time.get_ticks_msec() - proof_started_msec\n"
            "\t\t\tif false:\n"
            "\t\t\t\tbreak",
            1,
        )

        mutations = (
            ("process_frame", process_mutated, "process_frame must be followed immediately"),
            ("frame_post_draw", frame_post_draw_mutated, "frame_post_draw must be followed immediately"),
            ("passing terminal", terminal_mutated, "passing terminal must be immediately bound"),
            ("nested deadline break", nested_break_mutated, "must break the proof loop"),
        )
        for label, mutated, expected_error in mutations:
            with self.subTest(label=label):
                self.assertEqual(
                    mutated.count(deadline_check),
                    3,
                    "negative control must retain the old count-based contract",
                )
                errors = _canonical_deadline_placement_errors(mutated)
                self.assertTrue(
                    any(expected_error in error for error in errors),
                    f"placement contract accepted a deadline check moved away from {label}: {errors}",
                )

    def test_success_guard_rejects_a_latched_stage_failure(self) -> None:
        script = CANONICAL_RENDER_PROOF.read_text(encoding="utf-8")
        mutated = script.replace(
            "if not stage_failure_seen and visible >= MIN_VISIBLE_SPLATS",
            "if visible >= MIN_VISIBLE_SPLATS",
            1,
        )

        self.assertNotEqual(mutated, script, "negative control must remove the stage-failure guard")
        self.assertTrue(
            _canonical_success_guard_errors(mutated),
            "success contract accepted proof after a latched renderer-stage failure",
        )

    def test_post_draw_stage_failure_resample_cannot_be_removed(self) -> None:
        script = CANONICAL_RENDER_PROOF.read_text(encoding="utf-8")
        mutated = script.replace(
            "\t\t\tstage_failure_seen = stage_failure_seen or _stage_failed(post_draw_stats)\n",
            "",
            1,
        )

        self.assertNotEqual(mutated, script, "negative control must remove the post-draw failure latch")
        self.assertTrue(
            _canonical_post_draw_stage_resample_errors(mutated),
            "post-draw contract accepted visual proof without latching the completed frame's stage status",
        )


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
            "no_completion_marker": 0,
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
                    "completion": {"marker_present": True, "assertions": 3},
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

    def test_schema_accepts_a_silent_timeout_with_no_output(self) -> None:
        """A timeout that produced no output is a valid failure, not a schema violation.

        run_command() synthesizes `Timed out after Ns` with exit code 124 and possibly empty
        streams, so requiring a nonempty tail purely because a reason exists would flag every
        silent timeout.
        """
        timed_out = runtime_validation.TestResult(
            name="GPU Streaming Stress",
            command=["godot"],
            duration=300.0,
            exit_code=124,
            stdout="",
            stderr="",
            status="failed",
            reasons=["Timed out after 300s"],
            metrics={},
        )
        summary = runtime_validation.summarise([timed_out])
        self.assertEqual(summary["tests"][0]["output_tail"], [])
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertEqual(
            [e for e in errors if "output_tail" in e],
            [],
            f"silent timeout wrongly rejected: {errors}",
        )

    def test_schema_still_rejects_an_empty_tail_for_a_real_process_exit(self) -> None:
        """The exemption must not swallow the case it was written for."""
        summary = runtime_validation.summarise([self._crashed()])
        summary["tests"][0]["output_tail"] = []
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(
            any("output_tail" in e for e in errors),
            f"crash exit {summary['tests'][0]['exit_code']} should still require a tail: {errors}",
        )

    def test_schema_rejects_a_missing_output_tail(self) -> None:
        summary = runtime_validation.summarise([self._crashed()])
        del summary["tests"][0]["output_tail"]
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(
            any("output_tail" in error for error in errors),
            f"schema accepted a summary with no output_tail key: {errors}",
        )


def _raw_result(name: str, *, exit_code: int = 0, stdout: str = "", stderr: str = ""):
    """A result as run_command() returns it: unclassified, status defaults to failed."""
    return runtime_validation.TestResult(
        name=name,
        command=["godot", "--script", "test.gd"],
        duration=0.1,
        exit_code=exit_code,
        stdout=stdout,
        stderr=stderr,
    )


def _classify(result, allowlist=None):
    return runtime_validation._classify_result(
        result,
        fail_on_skip=True,
        allow_skip_tests=set(),
        zero_assertion_allowlist=allowlist or {},
    )


def _marker(scenario: str, assertions, extra: dict | None = None) -> str:
    payload = {"scenario": scenario, "assertions": assertions}
    payload.update(extra or {})
    return f"{runtime_validation.PASS_MARKER} {json.dumps(payload)}"


def _allow_entry(scenario: str, expires_utc: str = "2999-01-01T00:00:00Z") -> dict:
    return {
        scenario: {
            "scenario": scenario,
            "reason": "test entry",
            "issue_url": "https://github.com/klausi3D/godotGS/issues/891",
            "owner": "test",
            "expires_utc": expires_utc,
        }
    }


class CompletionMarkerClassificationTests(unittest.TestCase):
    """#891: `passed` is reachable only from a valid, correctly-bound marker."""

    def test_clean_exit_without_marker_is_not_passed(self) -> None:
        """The TEST-007 defect shape: prints nothing, exits 0. Must not read as pass.

        Restoring the pre-#891 fall-through (`result.status = "passed"`) turns
        this RED -- mutation direction (a) of the acceptance evidence.
        """
        result = _classify(_raw_result("Interactive State", stdout="ran fine\n"))
        self.assertEqual(result.status, runtime_validation.NO_COMPLETION_MARKER_STATUS)
        self.assertNotEqual(result.status, "passed")
        self.assertTrue(result.reasons, "advisory result must carry a printed reason")
        self.assertEqual(result.completion.get("marker_present"), False)

    def test_valid_marker_is_passed_and_records_assertions(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Interactive State", 12) + "\n")
        )
        self.assertEqual(result.status, "passed")
        self.assertEqual(result.completion.get("assertions"), 12)
        self.assertEqual(result.completion.get("marker_present"), True)

    def test_marker_bound_to_another_scenario_fails(self) -> None:
        """A copy-pasted emitter must be loud: the marker is non-transferable."""
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Engine Capability Sanity", 12))
        )
        self.assertEqual(result.status, "failed")
        self.assertTrue(any("mismatch" in reason for reason in result.reasons))

    def test_malformed_payload_fails(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=f"{runtime_validation.PASS_MARKER} not-json")
        )
        self.assertEqual(result.status, "failed")

    def test_empty_payload_fails(self) -> None:
        result = _classify(_raw_result("Interactive State", stdout=runtime_validation.PASS_MARKER))
        self.assertEqual(result.status, "failed")

    def test_missing_assertions_field_fails(self) -> None:
        payload = json.dumps({"scenario": "Interactive State"})
        result = _classify(
            _raw_result("Interactive State", stdout=f"{runtime_validation.PASS_MARKER} {payload}")
        )
        self.assertEqual(result.status, "failed")

    def test_non_integer_assertions_fails(self) -> None:
        for bad in ("12", 1.5, True, -1, None):
            with self.subTest(assertions=bad):
                result = _classify(
                    _raw_result("Interactive State", stdout=_marker("Interactive State", bad))
                )
                self.assertEqual(result.status, "failed")

    def test_duplicate_markers_fail(self) -> None:
        line = _marker("Interactive State", 12)
        result = _classify(_raw_result("Interactive State", stdout=f"{line}\n{line}\n"))
        self.assertEqual(result.status, "failed")

    def test_mid_line_marker_token_is_not_a_completion_proof(self) -> None:
        """Codex round 1 (PR #915): a line merely CONTAINING the token -- an
        engine log echo, a scenario quoting its own docs -- must not mint a
        pass. Only a line beginning with the marker (the shape both real
        emitters produce, verified against captured producer output) counts."""
        echoed = f"engine log: {_marker('Interactive State', 12)}"
        result = _classify(_raw_result("Interactive State", stdout=echoed))
        self.assertEqual(result.status, runtime_validation.NO_COMPLETION_MARKER_STATUS)
        self.assertNotEqual(result.status, "passed")

    def test_leading_whitespace_marker_still_counts(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout="  " + _marker("Interactive State", 12))
        )
        self.assertEqual(result.status, "passed")

    def test_marker_does_not_override_nonzero_exit(self) -> None:
        result = _classify(
            _raw_result("Interactive State", exit_code=3, stdout=_marker("Interactive State", 12))
        )
        self.assertEqual(result.status, "failed")

    def test_marker_does_not_override_fail_marker(self) -> None:
        stdout = f"{runtime_validation.FAIL_MARKER} boom\n{_marker('Interactive State', 12)}"
        result = _classify(_raw_result("Interactive State", stdout=stdout))
        self.assertEqual(result.status, "failed")

    def test_marker_does_not_override_skip_marker(self) -> None:
        stdout = f"{runtime_validation.SKIP_MARKER} headless\n{_marker('Interactive State', 12)}"
        result = _classify(_raw_result("Interactive State", stdout=stdout))
        self.assertEqual(result.status, "failed")  # fail_on_skip=True in _classify


class ZeroAssertionAllowlistTests(unittest.TestCase):
    """#891 / ADR section 4.3: tracked, expiring, both-directions-loud."""

    def test_zero_assertions_without_entry_fails(self) -> None:
        result = _classify(
            _raw_result(
                "Interactive State",
                stdout=_marker("Interactive State", 0, {"no_assertions_reason": "why"}),
            )
        )
        self.assertEqual(result.status, "failed")

    def test_zero_assertions_without_reason_fails_even_with_entry(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Interactive State", 0)),
            allowlist=_allow_entry("Interactive State"),
        )
        self.assertEqual(result.status, "failed")

    def test_zero_assertions_with_entry_and_reason_passes(self) -> None:
        result = _classify(
            _raw_result(
                "Interactive State",
                stdout=_marker("Interactive State", 0, {"no_assertions_reason": "why"}),
            ),
            allowlist=_allow_entry("Interactive State"),
        )
        self.assertEqual(result.status, "passed")
        self.assertEqual(result.completion.get("allowlisted_zero_assertions"), True)

    def test_expired_entry_no_longer_exempts(self) -> None:
        result = _classify(
            _raw_result(
                "Interactive State",
                stdout=_marker("Interactive State", 0, {"no_assertions_reason": "why"}),
            ),
            allowlist=_allow_entry("Interactive State", expires_utc="2020-01-01T00:00:00Z"),
        )
        self.assertEqual(result.status, "failed")
        self.assertTrue(any("expired" in reason for reason in result.reasons))

    def test_entry_for_an_asserting_scenario_fails_as_stale(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Interactive State", 5)),
            allowlist=_allow_entry("Interactive State"),
        )
        self.assertEqual(result.status, "failed")
        self.assertTrue(any("Stale" in reason for reason in result.reasons))

    def test_config_validation_rejects_unknown_scenario(self) -> None:
        with self.assertRaises(ValueError):
            runtime_validation._validate_zero_assertion_allowlist(
                [
                    {
                        "scenario": "No Such Scenario",
                        "reason": "r",
                        "issue_url": "u",
                        "owner": "o",
                        "expires_utc": "2999-01-01T00:00:00Z",
                    }
                ]
            )

    def test_config_validation_rejects_missing_fields_and_duplicates(self) -> None:
        entry = {
            "scenario": "Interactive State",
            "reason": "r",
            "issue_url": "u",
            "owner": "o",
            "expires_utc": "2999-01-01T00:00:00Z",
        }
        for missing in ("reason", "issue_url", "owner", "expires_utc"):
            with self.subTest(missing=missing):
                broken = {k: v for k, v in entry.items() if k != missing}
                with self.assertRaises(ValueError):
                    runtime_validation._validate_zero_assertion_allowlist([broken])
        with self.assertRaises(ValueError):
            runtime_validation._validate_zero_assertion_allowlist([entry, dict(entry)])
        with self.assertRaises(ValueError):
            runtime_validation._validate_zero_assertion_allowlist([{**entry, "extra": "x"}])
        with self.assertRaises(ValueError):
            runtime_validation._validate_zero_assertion_allowlist(
                [{**entry, "expires_utc": "not-a-date"}]
            )

    def test_config_validation_rejects_an_expired_entry_on_every_run(self) -> None:
        """Codex round 1 (PR #915): expiry must be loud at config load, not only
        when a profile happens to select the exempted scenario."""
        with self.assertRaises(ValueError):
            runtime_validation._validate_zero_assertion_allowlist(
                [
                    {
                        "scenario": "Interactive State",
                        "reason": "r",
                        "issue_url": "u",
                        "owner": "o",
                        "expires_utc": "2020-01-01T00:00:00Z",
                    }
                ]
            )

    def test_load_scenario_config_itself_rejects_an_expired_entry(self) -> None:
        """Codex round 2 (PR #915): the direct-validator test above cannot see
        `_load_scenario_config` dropping its call to the validator (the shipped
        allowlist is empty, so the load path would stay green). This drives the
        LOAD PATH with a config whose only profile does not select the exempted
        scenario -- deleting the validator call in _load_scenario_config turns
        this RED."""
        config = {
            "version": 1,
            "default_profile": "p",
            "profiles": {
                "p": {"cpp_tests": [], "gd_tests": ["Engine Capability Sanity"], "godot_args": []}
            },
            "zero_assertion_allowlist": [
                {
                    "scenario": "Interactive State",
                    "reason": "r",
                    "issue_url": "u",
                    "owner": "o",
                    "expires_utc": "2020-01-01T00:00:00Z",
                }
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "scenarios.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            with self.assertRaises(ValueError):
                runtime_validation._load_scenario_config(config_path)

    def test_shipped_scenario_config_validates(self) -> None:
        """The committed runtime_scenarios.json must satisfy its own contract."""
        config = runtime_validation._load_scenario_config(
            runtime_validation.DEFAULT_SCENARIO_CONFIG
        )
        self.assertIn("zero_assertion_allowlist", config)

    def test_all_six_profiles_pin_fail_on_skip_explicitly(self) -> None:
        """ADR section 4.4: the mode-coupled implicit default is retired."""
        config = runtime_validation._load_scenario_config(
            runtime_validation.DEFAULT_SCENARIO_CONFIG
        )
        profiles = config["profiles"]
        self.assertGreaterEqual(len(profiles), 6)
        for name, profile in profiles.items():
            with self.subTest(profile=name):
                self.assertIn(
                    "fail_on_skip",
                    profile,
                    f"profile '{name}' relies on the mode-coupled implicit default",
                )


class CompletionSummarySchemaTests(unittest.TestCase):
    def test_advisory_status_is_schema_legal_and_counted(self) -> None:
        result = _classify(_raw_result("Interactive State", stdout="ran\n"))
        summary = runtime_validation.summarise([result])
        self.assertEqual(summary["no_completion_marker"], 1)
        self.assertEqual(summary["failed"], 0)
        self.assertEqual(runtime_validation._validate_summary_schema(summary), [])

    def test_summary_missing_no_completion_marker_field_is_a_schema_error(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Interactive State", 1))
        )
        summary = runtime_validation.summarise([result])
        del summary["no_completion_marker"]
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(any("no_completion_marker" in error for error in errors))

    def test_missing_completion_key_is_a_schema_error(self) -> None:
        result = _classify(
            _raw_result("Interactive State", stdout=_marker("Interactive State", 1))
        )
        summary = runtime_validation.summarise([result])
        del summary["tests"][0]["completion"]
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(any("completion" in error for error in errors))

    def test_buckets_must_partition_the_total(self) -> None:
        result = _classify(_raw_result("Interactive State", stdout="ran\n"))
        summary = runtime_validation.summarise([result])
        summary["no_completion_marker"] = 0  # scenario now counted nowhere
        errors = runtime_validation._validate_summary_schema(summary)
        self.assertTrue(any("buckets" in error.lower() for error in errors))

    def test_registered_script_keeps_its_registry_name(self) -> None:
        """--gd-script runs of registered scenarios must classify under the
        registry name the emitted marker is bound to."""
        script = runtime_validation.GDS_TESTS["Interactive State"]
        resolved = runtime_validation._resolve_gd_test_map([str(script)])
        self.assertIn("Interactive State", resolved)


class AdvisoryLadderEndToEndTests(unittest.TestCase):
    """ADR section 4.7 item 2: both exit-expression terms cleared in ONE run.

    Drives main() with run_command stubbed, so the scenario 'ran, produced no
    completion marker' end to end: recorded in the report, counted in the
    summary, printed, summary['failed'] == 0, schema_valid true, exit code 0.
    Deleting the advisory branch (or re-adding the pass fall-through) flips
    these assertions -- they are the wiring-level mutation oracle.
    """

    def _run_main(self, stdout_for_scenario) -> tuple[int, dict, str]:
        with tempfile.TemporaryDirectory() as tmp:
            report_path = Path(tmp) / "report.json"

            def fake_run_command(name, command, *, cwd, timeout):
                return runtime_validation.TestResult(
                    name=name,
                    command=list(command),
                    duration=0.01,
                    exit_code=0,
                    stdout=stdout_for_scenario(name),
                    stderr="",
                )

            argv = [
                "run_runtime_validation.py",
                "--profile",
                "headless-ci",
                "--skip-cpp",
                "--fail-on-skip",
                "--godot-binary",
                "fake-godot",
                "--report-path",
                str(report_path),
            ]
            printed = io.StringIO()
            with mock.patch.object(runtime_validation, "ensure_synthetic_assets", lambda: None), \
                    mock.patch.object(runtime_validation, "_godot_binary_is_available", lambda binary: None), \
                    mock.patch.object(runtime_validation, "run_command", fake_run_command), \
                    mock.patch.object(sys, "argv", argv), \
                    contextlib.redirect_stdout(printed):
                exit_code = runtime_validation.main()
            report = json.loads(report_path.read_text(encoding="utf-8"))
            return exit_code, report, printed.getvalue()

    def test_missing_marker_is_advisory_recorded_and_does_not_fail_the_run(self) -> None:
        exit_code, report, output = self._run_main(lambda name: "scenario ran, said nothing\n")
        self.assertEqual(exit_code, 0, "ladder step 1 must not fail the run")
        self.assertEqual(report["failed"], 0)
        self.assertTrue(report["schema_valid"], report.get("schema_errors"))
        self.assertGreater(report["no_completion_marker"], 0)
        statuses = {entry["status"] for entry in report["tests"]}
        self.assertEqual(statuses, {runtime_validation.NO_COMPLETION_MARKER_STATUS})
        self.assertNotIn("passed", statuses)
        self.assertIn("[ADVISORY]", output, "an advisory result must be visibly printed")

    def test_bound_markers_pass_end_to_end(self) -> None:
        exit_code, report, _output = self._run_main(lambda name: _marker(name, 3) + "\n")
        self.assertEqual(exit_code, 0)
        self.assertEqual(report["no_completion_marker"], 0)
        self.assertEqual(report["failed"], 0)
        self.assertEqual(report["passed"], report["total"])
        self.assertTrue(report["schema_valid"], report.get("schema_errors"))
        for entry in report["tests"]:
            self.assertEqual(entry["completion"].get("assertions"), 3)

    def test_forged_marker_fails_end_to_end(self) -> None:
        exit_code, report, _output = self._run_main(
            lambda name: _marker("Some Other Scenario", 3) + "\n"
        )
        self.assertEqual(exit_code, 1, "a forged marker must fail the run even in step 1")
        self.assertGreater(report["failed"], 0)


if __name__ == "__main__":
    unittest.main()
