#!/usr/bin/env python3
"""A lane that streamed nothing must not report passing streaming evidence (#1016).

The defect this pins is a structural zero read as a measurement.

`benchmark_acceptance.required_fields_non_null` in the release-gate manifest requires
`queue_pressure` and `proof_status` to be non-null on every candidate lane, including
`streaming_corridor` and `city_flyover`. Neither of those lanes streams anything -- both
are parameter presets on the resident-path `lane_template.tscn`, with a grid of
`GaussianSplatNode3D` and no world node -- and yet both satisfied the requirement:

* `modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp:615-625`
  writes `streaming_queue_pressure_frames = static_cast<int64_t>(0)` in the branch taken
  when `perf.streaming_state` is empty. A lane with no streaming system reports zero
  queue pressure, which is byte-identical to a streaming run that had no pressure.
* `_evaluate_large_world_proof_contract` returns `proof_status: "not_applicable"` for any
  lane with no contract, and only `open_world_corridor_proof` has one -- so a lane whose
  manifest `evidence_role` claims it is streaming proof support also claimed the proof
  question did not apply to it.

A third fact makes both worse: `queue_pressure` was never an emitted key at all. Real
rows carry `streaming_queue_pressure_frames`; the manifest asks for `queue_pressure`.
The only bundles that ever passed this check had rows hand-written to the manifest's
names, so the mismatch was never exercised.

These cases pin the properties the fix depends on:

* an unmeasured lane reports `None`, never `0`, and says why;
* a measured lane still reports a real value (the legal route keeps working);
* a lane declaring a `proof_*` evidence role cannot report `not_applicable`;
* a lane that makes no streaming claim still legitimately reports `not_applicable`;
* and, end to end, today's `city_flyover` row shape is now REJECTED by the real
  candidate gate while a genuinely-streaming row is ACCEPTED.

The last pair is the one that matters. A guard that rejects everything is the same bug
as a guard that accepts everything.
"""

from __future__ import annotations

import ast
import importlib.util
import json
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
RUNTIME_DIR = ROOT / "tests" / "runtime"
CI_DIR = ROOT / "tests" / "ci"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# run_benchmark.py imports its sibling helper by name.
sys.path.insert(0, str(RUNTIME_DIR))
_bench = _load_module("_gs_run_benchmark_fc", RUNTIME_DIR / "run_benchmark.py")
_checker = _load_module("_gs_release_gate_fc", CI_DIR / "check_renderer_release_gates.py")


def _resident_lane_result() -> dict[str, Any]:
    """The shape `city_flyover` actually produces today.

    Field values are taken from the published run in
    `docs/assets/data/benchmark_suite_report.json`: streaming telemetry unavailable,
    every streaming counter zero, route on the instance path.
    """
    return {
        "lane_id": "city_flyover",
        "streaming_queue_pressure_active": False,
        "streaming_queue_pressure_frames": 0,
        "proof_status": "not_applicable",
        "proof_valid": True,
        "report": {
            "lane_id": "city_flyover",
            "proof_metrics": {
                "streaming_state_telemetry_available": False,
                "queue_pressure_frames": None,
                "queue_pressure_candidate_frames": None,
            },
        },
    }


def _streaming_lane_result() -> dict[str, Any]:
    """A lane that genuinely drove a chunked world.

    SYNTHETIC. The corridor proof lane has never been executed on a GPU in this
    repository, so this is a shape, not a recorded run. It exists to prove the legal
    route still works.
    """
    return {
        "lane_id": "open_world_corridor_proof",
        "streaming_queue_pressure_active": True,
        "streaming_queue_pressure_frames": 11,
        "proof_status": "pass",
        "proof_valid": True,
        "report": {
            "lane_id": "open_world_corridor_proof",
            "proof_metrics": {
                "streaming_state_telemetry_available": True,
                "queue_pressure_frames": 11,
                "queue_pressure_candidate_frames": 34,
            },
        },
    }


class StreamingEvidenceFailClosedTests(unittest.TestCase):
    def test_unmeasured_queue_pressure_is_none_not_zero(self) -> None:
        result = _resident_lane_result()
        _bench._streaming_evidence_fail_closed(result, "proof_support_boundary_crossing_smoke")
        self.assertIsNone(
            result["queue_pressure"],
            "a lane with no streaming system has not measured zero pressure; it has "
            "measured nothing, and 0 would be indistinguishable from a real measurement",
        )
        self.assertFalse(result["streaming_telemetry_measured"])
        self.assertIn("never measured", result["queue_pressure_reason"])

    def test_measured_queue_pressure_is_reported(self) -> None:
        result = _streaming_lane_result()
        _bench._streaming_evidence_fail_closed(result, "proof_corridor_return_bootstrap")
        self.assertIsInstance(result["queue_pressure"], dict)
        self.assertEqual(result["queue_pressure"]["frames"], 11)
        self.assertEqual(result["queue_pressure"]["source"], "streaming_state")
        self.assertTrue(result["streaming_telemetry_measured"])
        self.assertIsNone(result["queue_pressure_reason"])

    def test_proof_lane_cannot_report_not_applicable(self) -> None:
        result = _resident_lane_result()
        _bench._streaming_evidence_fail_closed(result, "proof_support_corridor_churn_smoke")
        self.assertIsNone(result["proof_status"])
        self.assertIn("claims streaming proof evidence", result["proof_status_reason"])

    def test_non_proof_lane_keeps_not_applicable(self) -> None:
        """`static_baseline` makes no streaming claim, so "not applicable" is the truth.

        Without this case the fix would be indistinguishable from "fail every lane",
        which gets a guard disabled rather than obeyed.
        """
        result = _resident_lane_result()
        result["lane_id"] = "static_baseline"
        _bench._streaming_evidence_fail_closed(result, "low_noise_smoke_reference")
        self.assertEqual(result["proof_status"], "not_applicable")
        self.assertTrue(result["proof_valid"])

    def test_evidence_role_selection_is_derived_not_enumerated(self) -> None:
        for role in (
            "proof_corridor_return_bootstrap",
            "proof_support_corridor_churn_smoke",
            "proof_support_boundary_crossing_smoke",
            # Review finding: these two are also selected and were neither documented
            # nor tested. They are not candidate-required lanes, so no gate reads their
            # proof_status, but the selection is real and must be pinned.
            "proof_support_city_roam_soak_smoke",
            "proof_support_integrated_composite_smoke",
        ):
            self.assertTrue(_bench._lane_declares_proof_evidence(role), role)
        for role in ("low_noise_smoke_reference", "published_baseline", "", None):
            self.assertFalse(_bench._lane_declares_proof_evidence(role), repr(role))

    def test_selection_matches_the_real_manifest_roles(self) -> None:
        """Derive the expected set from the manifest instead of trusting the list above.

        A hand-kept list of roles is the "hand-written list guarding an invariant" shape.
        This reads the shipped manifest and asserts the helper agrees with it, so a new
        proof_* role added to the manifest cannot drift away from the code silently.
        """
        manifest = json.loads(
            (ROOT / "tests/fixtures/benchmark_asset_manifest.json").read_text(encoding="utf-8")
        )
        roles = {
            str(entry.get("evidence_role", ""))
            for entry in manifest.get("lane_metadata", {}).values()
        }
        selected = {role for role in roles if _bench._lane_declares_proof_evidence(role)}
        self.assertEqual(
            selected,
            {role for role in roles if role.startswith("proof")},
            "the helper and the manifest disagree about which roles claim proof evidence",
        )
        self.assertTrue(selected, "no proof_* evidence roles found in the manifest at all")

    def test_proof_status_failure_does_not_fail_the_benchmark_run(self) -> None:
        """Review finding: proof_valid feeds lane_valid and the harness exit code.

        A lane that ran perfectly well and simply has no proof contract must not turn the
        weekly scheduled run red. The candidate gate's required_fields_non_null check on a
        null proof_status is the correct lever; the harness exit code is not.
        """
        result = _resident_lane_result()
        _bench._streaming_evidence_fail_closed(result, "proof_support_corridor_churn_smoke")
        self.assertIsNone(result["proof_status"])
        self.assertTrue(
            result["proof_valid"],
            "setting proof_valid False here would make run_benchmark exit 1 for "
            "streaming_corridor, city_flyover, long_soak and unified_composite on every "
            "quick-profile and scheduled run",
        )

    def test_non_streaming_candidate_lane_keeps_a_non_null_queue_pressure(self) -> None:
        """Codex review: nulling every unmeasured lane would break the gate, not tighten it.

        `static_baseline`, `instance_storm`, `integrity_sentinel` and `parity_fidelity` are
        candidate-REQUIRED lanes that make no streaming claim and never will, and the
        manifest requires `queue_pressure` non-null on every required lane. Nulling it for
        them would make the gate permanently unsatisfiable, which is a broken gate rather
        than a strict one.
        """
        manifest = _checker._load_json(
            ROOT / "docs/reference/renderer_release_gate_manifest.json"
        )
        required = manifest["benchmark_acceptance"]["required_fields_non_null"]
        result = _resident_lane_result()
        result["lane_id"] = "static_baseline"
        _bench._streaming_evidence_fail_closed(result, "low_noise_smoke_reference")
        self.assertIsNotNone(result["queue_pressure"])
        self.assertEqual(result["queue_pressure"]["source"], "not_applicable")
        failures = _checker._candidate_lane_required_field_failures(
            "static_baseline", result, required
        )
        self.assertFalse(
            any("queue_pressure" in item for item in failures),
            f"a lane that makes no streaming claim must still satisfy the gate; got {failures}",
        )

    def test_streaming_state_without_a_pressure_value_is_not_a_measurement(self) -> None:
        """Codex review: the availability flag alone is not the measurement.

        An object with a null `frames` satisfies a non-null check on the OUTER field while
        containing nothing -- the same fail-open shape one level down.
        """
        result = _streaming_lane_result()
        result["report"]["proof_metrics"]["queue_pressure_frames"] = None
        _bench._streaming_evidence_fail_closed(result, "proof_corridor_return_bootstrap")
        self.assertIsNone(result["queue_pressure"])
        self.assertIn("no queue_pressure_frames value", result["queue_pressure_reason"])

    def test_fail_closed_helper_is_actually_wired_into_the_lane_loop(self) -> None:
        """The most common way a check becomes decorative is nothing calling it.

        Every other case here invokes `_streaming_evidence_fail_closed` directly, so all
        of them stay green if the call site is deleted. This case reads the source and
        asserts the helper is invoked from somewhere other than its own definition, and
        that the call passes the lane's evidence role (the argument the proof_status
        branch depends on).
        """
        tree = ast.parse((RUNTIME_DIR / "run_benchmark.py").read_text(encoding="utf-8"))
        call_sites = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "_streaming_evidence_fail_closed"
        ]
        self.assertTrue(
            call_sites,
            "_streaming_evidence_fail_closed is defined but never called: the fail-closed "
            "behaviour would not reach a single real benchmark row",
        )
        self.assertTrue(
            any(len(site.args) >= 2 for site in call_sites),
            "the helper must be called with the lane result AND its evidence role; "
            "without the role, proof_status cannot be judged",
        )

    def test_todays_row_is_rejected_by_the_real_candidate_gate(self) -> None:
        """End to end against the shipped manifest's own required-field list.

        This is the discrimination that matters: the same check must reject the row the
        harness produces today and accept the row a streaming lane would produce.
        """
        manifest = _checker._load_json(
            ROOT / "docs/reference/renderer_release_gate_manifest.json"
        )
        required = manifest["benchmark_acceptance"]["required_fields_non_null"]
        self.assertIn("queue_pressure", required)
        self.assertIn("proof_status", required)

        resident = _resident_lane_result()
        _bench._streaming_evidence_fail_closed(resident, "proof_support_boundary_crossing_smoke")
        failures = _checker._candidate_lane_required_field_failures(
            "city_flyover", resident, required
        )
        self.assertTrue(
            any("queue_pressure" in item for item in failures),
            f"a lane that streamed nothing must fail the queue_pressure requirement; got {failures}",
        )
        self.assertTrue(any("proof_status" in item for item in failures), failures)

        streaming = _streaming_lane_result()
        _bench._streaming_evidence_fail_closed(streaming, "proof_corridor_return_bootstrap")
        streaming_failures = _checker._candidate_lane_required_field_failures(
            "open_world_corridor_proof", streaming, required
        )
        self.assertFalse(
            any("queue_pressure" in item for item in streaming_failures),
            f"a genuinely streaming lane must still pass; got {streaming_failures}",
        )
        self.assertFalse(any("proof_status" in item for item in streaming_failures))


class OpenWorldProofSurfaceReachabilityTests(unittest.TestCase):
    """The lane that produces the open-world proof must be reachable without a human.

    #1045: `open_world_corridor_proof` is the only lane in the repository that drives a
    chunked `GaussianSplatWorld3D`, and the only step that runs it was gated on
    `workflow_dispatch` plus an opt-in input. Every `workflow_dispatch` run of that
    workflow is from March 2026, so the lane had never executed once -- inside a job
    named "Open-World Proof Evidence" that runs weekly on a schedule.

    An evidence surface nothing can trigger produces no evidence. This pins the trigger,
    because the regression is invisible: the job stays green either way.
    """

    WORKFLOW = ROOT / ".github/workflows/gaussian_production_gates.yml"
    PROOF_LANE = "open_world_corridor_proof"

    # Text matching, not a YAML parse, and deliberately so: no workflow in this
    # repository pip-installs PyYAML and `actions/setup-python@v5` provisions a bare
    # tool-cache interpreter, so a module-scope `import yaml` would raise during
    # discovery in the guard lane -- the guard would error instead of reporting a
    # result. The same reasoning, with the measurement, is recorded in
    # tests/ci/test_benchmark_fixture_contract.py:2814-2821.
    STEP_HEADER_PREFIX = "      - name:"
    STEP_KEY_PREFIX = "        "

    def _proof_steps(self) -> list[tuple[str, list[str]]]:
        """Return (step name, step lines) for steps whose body runs the proof lane."""
        text = self.WORKFLOW.read_text(encoding="utf-8")
        blocks: list[tuple[str, list[str]]] = []
        current: list[str] | None = None
        name = ""
        for line in text.splitlines():
            if line.startswith(self.STEP_HEADER_PREFIX):
                if current is not None:
                    blocks.append((name, current))
                name = line[len(self.STEP_HEADER_PREFIX):].strip()
                current = [line]
                continue
            if current is None:
                continue
            if line.strip() and not line.startswith(self.STEP_KEY_PREFIX):
                blocks.append((name, current))
                current = None
                continue
            current.append(line)
        if current is not None:
            blocks.append((name, current))
        return [
            (step_name, lines)
            for step_name, lines in blocks
            if any(f"--lane {self.PROOF_LANE}" in line for line in lines)
        ]

    @staticmethod
    def _condition(lines: list[str]) -> str:
        return " ".join(line.strip() for line in lines if line.strip().startswith("if:"))

    def test_a_step_actually_runs_the_corridor_proof_lane(self) -> None:
        steps = self._proof_steps()
        self.assertTrue(
            steps,
            f"no workflow step invokes --lane {self.PROOF_LANE}; the open_world_proof "
            "artifact would have no producer at all",
        )

    def test_the_corridor_proof_lane_is_reachable_from_a_schedule(self) -> None:
        """Not merely present -- reachable without somebody clicking a button.

        The condition is matched as `github.event_name == 'schedule'`, not as the bare
        word "schedule": a substring test would accept a condition that mentions
        `schedule` inside a clause that can never be true for a scheduled event.
        """
        needle = "github.event_name == 'schedule'"
        unreachable = [
            name for name, lines in self._proof_steps() if needle not in self._condition(lines)
        ]
        self.assertFalse(
            unreachable,
            "these steps run the corridor proof lane but cannot be triggered by a "
            f"scheduled run, so the lane only executes if a human dispatches it: {unreachable}. "
            "That is how the lane went from March 2026 to September 2026 without running once.",
        )

    def test_the_guard_does_not_depend_on_pyyaml(self) -> None:
        """The guard lane installs nothing; an `import yaml` would error, not report."""
        tree = ast.parse(Path(__file__).read_text(encoding="utf-8"))
        imported = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imported.add(node.module.split(".")[0])
        self.assertNotIn(
            "yaml",
            imported,
            "this guard runs in run_module_tests.py --guard-only, whose runner is a bare "
            "tool-cache interpreter with no PyYAML; importing it would make the guard "
            "error instead of report a result",
        )


if __name__ == "__main__":
    unittest.main()
