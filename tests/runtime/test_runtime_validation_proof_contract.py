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
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "runtime" / "run_runtime_validation.py"
spec = importlib.util.spec_from_file_location("run_runtime_validation", SCRIPT)
assert spec and spec.loader
runtime_validation = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime_validation
spec.loader.exec_module(runtime_validation)


class SyntheticAssetFloorWiringTests(unittest.TestCase):
    def test_prep_command_requires_floors_and_forwards_the_binary(self) -> None:
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch.object(runtime_validation.subprocess, "run", return_value=completed) as run:
            runtime_validation.ensure_synthetic_assets("C:/godot/bin/godot.exe")

        command = run.call_args.args[0]
        self.assertIn("--require-asset-floors", command)
        self.assertEqual(command[-2:], ["--godot-binary", "C:/godot/bin/godot.exe"])

    def test_selected_fixture_consumer_passes_binary_to_asset_prep(self) -> None:
        calls: list[str] = []
        with tempfile.TemporaryDirectory() as raw_td:
            argv = [
                "run_runtime_validation.py",
                "--godot-binary",
                "C:/godot/bin/godot.exe",
                "--profile",
                "headless-ci",
                "--skip-cpp",
                "--gd-test",
                "World Streaming Gate",
                "--report-path",
                str(Path(raw_td) / "report.json"),
            ]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(
                        runtime_validation,
                        "ensure_synthetic_assets",
                        side_effect=lambda binary: calls.append(binary),
                    ), \
                    mock.patch.object(runtime_validation, "run_gd_tests", return_value=[]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runtime_validation.main(), 0)

        self.assertEqual(calls, ["C:/godot/bin/godot.exe"])

    def test_cpp_only_skip_gd_reaches_harness_without_asset_prep(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            argv = [
                "run_runtime_validation.py",
                "--godot-binary",
                "C:/definitely/missing/godot.exe",
                "--profile",
                "headless-ci",
                "--cpp-test",
                "Runtime Modifications",
                "--skip-gd",
                "--report-path",
                str(Path(raw_td) / "report.json"),
            ]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(runtime_validation, "ensure_synthetic_assets") as prep, \
                    mock.patch.object(runtime_validation, "run_cpp_harnesses", return_value=[]) as cpp, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runtime_validation.main(), 0)

        prep.assert_not_called()
        cpp.assert_called_once()

    def test_selected_non_fixture_gd_test_does_not_prepare_assets(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            argv = [
                "run_runtime_validation.py",
                "--godot-binary",
                "C:/definitely/missing/godot.exe",
                "--profile",
                "headless-ci",
                "--skip-cpp",
                "--gd-test",
                "Engine Capability Sanity",
                "--report-path",
                str(Path(raw_td) / "report.json"),
            ]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(runtime_validation, "ensure_synthetic_assets") as prep, \
                    mock.patch.object(runtime_validation, "run_gd_tests", return_value=[]) as gd, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runtime_validation.main(), 0)

        prep.assert_not_called()
        gd.assert_called_once()

    def test_future_cpp_fixture_consumer_is_not_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            source = Path(raw_td) / "future_fixture_consumer.cpp"
            source.write_text(
                'constexpr auto asset = "res://tests/fixtures/test_splats.ply";\n',
                encoding="utf-8",
            )
            with mock.patch.dict(
                runtime_validation.CPP_TESTS,
                {"Future Fixture Consumer": source},
            ), self.assertRaisesRegex(RuntimeError, "incomplete or stale"):
                runtime_validation._floor_governed_fixture_consumers(
                    {"C++: Future Fixture Consumer": source}
                )

    def test_ad_hoc_gd_script_with_composed_path_preflights_conservatively(self) -> None:
        with tempfile.TemporaryDirectory(dir=runtime_validation.RUNTIME_DIR) as raw_script_dir, \
                tempfile.TemporaryDirectory() as raw_report_dir:
            script = Path(raw_script_dir) / "test_composed_fixture_path.gd"
            script.write_text(
                'const ASSET := "res://tests/fixtures/" + "test_splats.ply"\n',
                encoding="utf-8",
            )
            argv = [
                "run_runtime_validation.py",
                "--godot-binary",
                "C:/godot/bin/godot.exe",
                "--profile",
                "headless-ci",
                "--skip-cpp",
                "--gd-script",
                str(script.relative_to(runtime_validation.ROOT)),
                "--report-path",
                str(Path(raw_report_dir) / "report.json"),
            ]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(runtime_validation, "ensure_synthetic_assets") as prep, \
                    mock.patch.object(runtime_validation, "run_gd_tests", return_value=[]), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runtime_validation.main(), 0)

        prep.assert_called_once_with("C:/godot/bin/godot.exe")

    def test_registered_contract_covers_indirect_fixture_dependency(self) -> None:
        name = "GDScript: Engine Capability Sanity"
        source = runtime_validation.GDS_TESTS["Engine Capability Sanity"]
        indirect_contract = runtime_validation.ScenarioFixtureContract(
            source=source,
            fixtures=("res://tests/fixtures/test_splats.ply",),
        )
        with mock.patch.dict(
            runtime_validation.SCENARIO_FIXTURE_CONTRACTS,
            {name: indirect_contract},
        ):
            consumers = runtime_validation._floor_governed_fixture_consumers(
                {name: source}
            )

        self.assertEqual(consumers[name], indirect_contract.fixtures)

    def test_selected_unfloored_fixture_reference_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            source = Path(raw_td) / "unfloored_consumer.gd"
            source.write_text(
                'const ASSET := "res://tests/fixtures/unfloored.ply"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(RuntimeError, "without a positive"):
                runtime_validation._floor_governed_fixture_consumers(
                    {"GDScript: Unfloored Consumer": source}
                )

    def test_list_profiles_does_not_require_or_generate_fixtures(self) -> None:
        argv = ["run_runtime_validation.py", "--list-profiles"]
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(runtime_validation, "ensure_synthetic_assets") as prep, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(runtime_validation.main(), 0)
        prep.assert_not_called()


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
            with mock.patch.object(runtime_validation, "ensure_synthetic_assets", lambda *_args: None), \
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
