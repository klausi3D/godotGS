#!/usr/bin/env python3
"""Unit tests for scripts/agentic/check_pr_contract.py."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "check_pr_contract.py"
spec = importlib.util.spec_from_file_location("check_pr_contract", SCRIPT)
assert spec and spec.loader
cpc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpc)

TEMPLATE_PATH = ROOT / ".agentic" / "templates" / "task.json"
POLICY = json.loads((ROOT / ".agentic" / "policy.json").read_text(encoding="utf-8"))
TASK_SCHEMA = json.loads((ROOT / ".agentic" / "schemas" / "task.schema.json").read_text(encoding="utf-8"))
TEMPLATE = json.loads(TEMPLATE_PATH.read_text(encoding="utf-8"))


def _hard(errors):
    return [e for e in errors if not e.startswith("note:")]


class CheckPrContractTest(unittest.TestCase):
    def test_valid_contract_matching_risk(self):
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/logger/logger.cpp"],  # R1, matches declared R1
        )
        self.assertEqual(_hard(errors), [])

    def test_understated_risk_fails(self):
        # Declared R1 but the diff touches the renderer (R2).
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/renderer/gpu_sorter.cpp"],
        )
        self.assertTrue(any("understates" in e for e in _hard(errors)))

    def test_forbidden_path_change_fails(self):
        # Template forbids modules/gaussian_splatting/persistence/**.
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/persistence/incremental_saver.cpp"],
        )
        self.assertTrue(any("forbidden path" in e for e in _hard(errors)))

    def test_out_of_scope_path_fails(self):
        # Same risk class (R1) but outside the declared owned_paths (logger/**).
        errors = cpc.check_contract(
            copy.deepcopy(TEMPLATE),
            POLICY,
            TASK_SCHEMA,
            ["modules/gaussian_splatting/animation/animation_state_machine.cpp"],
        )
        hard = _hard(errors)
        self.assertTrue(any("outside the declared owned_paths" in e for e in hard))
        self.assertFalse(any("understates" in e for e in hard))

    def test_malformed_path_entries_do_not_crash(self):
        # Non-string owned/forbidden entries are caught by the schema; the scope
        # check must skip them rather than raise AttributeError.
        contract = copy.deepcopy(TEMPLATE)
        contract["owned_paths"] = [None, "modules/gaussian_splatting/logger/**"]
        contract["forbidden_paths"] = [123]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("owned_paths" in e or "forbidden_paths" in e for e in errors))

    def test_missing_required_field_fails(self):
        contract = copy.deepcopy(TEMPLATE)
        del contract["rollback_plan"]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("rollback_plan" in e for e in _hard(errors)))

    def test_empty_validation_commands_fails(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["validation_commands"] = []
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("validation_commands" in e for e in _hard(errors)))

    def test_unknown_risk_class_value_fails_schema(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R9"
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, None)
        self.assertTrue(any("risk_class" in e for e in _hard(errors)))

    def test_stacked_pr_requires_base_fields(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["stacked_on"] = {"base_pr": "", "base_sha": ""}
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("stacked_on.base_pr" in e for e in _hard(errors)))
        self.assertTrue(any("stacked_on.base_sha" in e for e in _hard(errors)))

    def test_r3_without_design_record_fails(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R3"
        contract["owned_paths"] = ["servers/rendering/foo.cpp"]
        contract["forbidden_paths"] = []
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["servers/rendering/foo.cpp"])
        self.assertTrue(any("design_record" in e for e in _hard(errors)))

    def test_r3_with_design_record_passes(self):
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R3"
        contract["owned_paths"] = ["servers/rendering/foo.cpp"]
        contract["forbidden_paths"] = []
        contract["design_record"] = "https://github.com/klausi3D/godotGS/issues/123"
        # An R3 contract must commit to the full R3 deterministic checks and evidence.
        contract["validation_commands"] = list(POLICY["risk_classes"]["R3"]["deterministic_checks"])
        contract["evidence_requirements"] = list(POLICY["risk_classes"]["R3"]["evidence_requirements"])
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["servers/rendering/foo.cpp"])
        self.assertEqual(_hard(errors), [])

    def test_missing_deterministic_check_fails(self):
        # A contract that omits its class's deterministic checks must not pass.
        contract = copy.deepcopy(TEMPLATE)
        contract["validation_commands"] = ["true"]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"])
        self.assertTrue(any("deterministic check" in e for e in _hard(errors)))

    def test_missing_evidence_item_fails(self):
        # An R2 contract must carry the R2 policy evidence items, not a placeholder.
        contract = copy.deepcopy(TEMPLATE)
        contract["risk_class"] = "R2"
        contract["owned_paths"] = ["modules/gaussian_splatting/renderer/**"]
        contract["validation_commands"] = list(POLICY["risk_classes"]["R2"]["deterministic_checks"])
        contract["evidence_requirements"] = ["n/a"]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/renderer/x.cpp"])
        self.assertTrue(any("evidence item" in e for e in _hard(errors)))

    def test_agentic_tests_change_requires_agentic_suite(self):
        # A change to the agentic test suite (R1 via tests/**) must require running it.
        contract = copy.deepcopy(TEMPLATE)
        contract["owned_paths"] = ["tests/agentic/**"]
        contract["validation_commands"] = ["python tests/ci/run_module_tests.py --guard-only"]
        errors = cpc.check_contract(contract, POLICY, TASK_SCHEMA, ["tests/agentic/test_classify_change.py"])
        self.assertTrue(any("unittest discover -s tests/agentic" in e for e in _hard(errors)))

    def test_single_segment_glob_does_not_span_directories(self):
        # owned_paths with a single-segment '*' must not match a deeper path.
        contract = copy.deepcopy(TEMPLATE)
        contract["owned_paths"] = ["modules/gaussian_splatting/logger/*"]
        errors = cpc.check_contract(
            contract, POLICY, TASK_SCHEMA,
            ["modules/gaussian_splatting/logger/private/x.cpp"],
        )
        self.assertTrue(any("outside the declared owned_paths" in e for e in _hard(errors)))
        # ...while a single-segment file directly under it is in scope.
        ok = cpc.check_contract(
            contract, POLICY, TASK_SCHEMA, ["modules/gaussian_splatting/logger/x.cpp"],
        )
        self.assertFalse(any("outside the declared owned_paths" in e for e in _hard(ok)))

    def test_main_requires_diff_source(self):
        # A full PR-gate check must not silently skip the cross-check when no diff is given.
        rc = cpc.main(["--contract", str(TEMPLATE_PATH)])
        self.assertEqual(rc, 2)

    def test_empty_paths_fails_closed_instead_of_reporting_compliant(self):
        """`--paths` with no values is a diff source that resolved to nothing.

        It used to inherit classify_change's empty-set fail-open: the computed
        class was R0, so nothing could ever "understate" it and the full gate
        reported compliant with zero risk and zero scope enforcement
        (GS-AUDIT-TEST-002, fourth evidence line). The empty set now classifies as
        the policy default, so an R1 contract is flagged as understating it.
        """
        errors = _hard(cpc.check_contract(copy.deepcopy(TEMPLATE), POLICY, TASK_SCHEMA, []))
        self.assertTrue(
            any("understates" in e for e in errors),
            f"empty --paths must not pass the gate silently; got {errors}",
        )

    def test_main_with_empty_paths_returns_non_zero(self):
        """The same fail-closed behaviour through the real CLI entry point."""
        rc = cpc.main(["--contract", str(TEMPLATE_PATH), "--paths"])
        self.assertEqual(1, rc)

    def test_main_schema_only_opt_out(self):
        # The schema-only escape hatch must be explicit and pass on the valid template.
        rc = cpc.main(["--contract", str(TEMPLATE_PATH), "--schema-only"])
        self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
