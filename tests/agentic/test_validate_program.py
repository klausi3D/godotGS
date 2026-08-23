#!/usr/bin/env python3
"""Unit tests for scripts/agentic/validate_program.py."""

from __future__ import annotations

import copy
import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "validate_program.py"
spec = importlib.util.spec_from_file_location("validate_program", SCRIPT)
assert spec and spec.loader
vp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vp)

SCHEMA = json.loads((ROOT / ".agentic" / "schemas" / "program.schema.json").read_text(encoding="utf-8"))
TEMPLATE = json.loads((ROOT / ".agentic" / "templates" / "program.json").read_text(encoding="utf-8"))


class ValidateProgramTest(unittest.TestCase):
    def test_template_is_valid(self):
        self.assertEqual(vp.validate_program(copy.deepcopy(TEMPLATE), SCHEMA), [])

    def test_invalid_snapshot_sha_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["planning_snapshot_sha"] = "master"
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("planning_snapshot_sha" in error for error in errors))

    def test_empty_goal_objective_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["objective"] = "   "
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("objective" in error for error in errors))

    def test_heavy_process_limit_above_repository_cap_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["dispatch"]["heavy_process_limit"] = 3
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("repository-wide limit is 2" in error for error in errors))

    def test_implementation_wip_limit_above_program_cap_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["dispatch"]["implementation_wip_limit"] = 3
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("implementation WIP limit is 2" in error for error in errors))

    def test_live_status_requery_cannot_be_disabled(self):
        program = copy.deepcopy(TEMPLATE)
        program["dispatch"]["live_status_requery_required"] = False
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("live_status_requery_required" in error for error in errors))

    def test_blank_dispatch_invariant_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["dispatch"]["invariants"] = ["  "]
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("dispatch.invariants[0]" in error for error in errors))

    def test_duplicate_milestone_id_fails(self):
        program = copy.deepcopy(TEMPLATE)
        duplicate = copy.deepcopy(program["milestones"][0])
        duplicate["coordinator_issue"] = "#3"
        duplicate["work_items"][0]["ref"] = "#4"
        program["milestones"].append(duplicate)
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("duplicate milestone id" in error for error in errors))

    def test_unknown_dependency_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["depends_on"] = ["MISSING"]
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("unknown milestone" in error for error in errors))

    def test_non_string_dependency_fails_schema_without_crashing_semantic_validation(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["depends_on"] = [{}]

        errors = vp.validate_program(program, SCHEMA)

        self.assertTrue(any("depends_on[0]" in error for error in errors))

    def test_standalone_validator_reports_non_object_program_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            program_path = Path(temp_dir) / "program.json"
            program_path.write_text("[]", encoding="utf-8")
            stdout = io.StringIO()

            with contextlib.redirect_stdout(stdout):
                exit_code = vp.main(["--program", str(program_path)])

        self.assertEqual(exit_code, 1)
        self.assertIn("Program is INVALID:", stdout.getvalue())
        self.assertIn("$: expected type object, got list", stdout.getvalue())

    def test_standalone_validator_reports_non_object_schema_root(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            program_path = Path(temp_dir) / "program.json"
            schema_path = Path(temp_dir) / "schema.json"
            program_path.write_text(json.dumps(TEMPLATE), encoding="utf-8")
            schema_path.write_text("[]", encoding="utf-8")
            stdout = io.StringIO()

            with contextlib.redirect_stdout(stdout):
                exit_code = vp.main(
                    ["--program", str(program_path), "--schema", str(schema_path)]
                )

        self.assertEqual(exit_code, 1)
        self.assertIn("Program is INVALID:", stdout.getvalue())
        self.assertIn("schema must be an object", stdout.getvalue())

    def test_standalone_validator_rejects_vacuous_custom_schema(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            program_path = Path(temp_dir) / "program.json"
            schema_path = Path(temp_dir) / "schema.json"
            program_path.write_text("{}", encoding="utf-8")
            schema_path.write_text("{}", encoding="utf-8")
            stdout = io.StringIO()

            with contextlib.redirect_stdout(stdout):
                exit_code = vp.main(
                    ["--program", str(program_path), "--schema", str(schema_path)]
                )

        self.assertEqual(exit_code, 1)
        self.assertIn("Program is INVALID:", stdout.getvalue())
        self.assertIn("$schema contract", stdout.getvalue())

    def test_standalone_validator_rejects_unresolvable_snapshot(self):
        program = copy.deepcopy(TEMPLATE)
        program["planning_snapshot_sha"] = "f" * 40

        with tempfile.TemporaryDirectory() as temp_dir:
            program_path = Path(temp_dir) / "program.json"
            program_path.write_text(json.dumps(program), encoding="utf-8")

            for exists, expected_exit in ((True, 0), (False, 1)):
                with self.subTest(commit_exists=exists):
                    stdout = io.StringIO()
                    with mock.patch.object(vp, "_git_commit_exists", return_value=exists):
                        with contextlib.redirect_stdout(stdout):
                            exit_code = vp.main(["--program", str(program_path)])

                    self.assertEqual(exit_code, expected_exit)
                    if not exists:
                        self.assertIn("planning_snapshot_sha does not resolve to a commit", stdout.getvalue())

    def test_standalone_validator_validates_task_contract_template(self):
        cases = (
            (".agentic/templates/missing.json", "does not exist"),
            (".agentic/templates/review.json", "does not match task schema"),
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            program_path = Path(temp_dir) / "program.json"
            for template_path, expected_error in cases:
                with self.subTest(template_path=template_path):
                    program = copy.deepcopy(TEMPLATE)
                    program["dispatch"]["task_contract_template"] = template_path
                    program_path.write_text(json.dumps(program), encoding="utf-8")
                    stdout = io.StringIO()

                    with mock.patch.object(vp, "_git_commit_exists", return_value=True):
                        with contextlib.redirect_stdout(stdout):
                            exit_code = vp.main(["--program", str(program_path)])

                    self.assertEqual(exit_code, 1)
                    self.assertIn(expected_error, stdout.getvalue())

    def test_dependency_must_appear_earlier(self):
        program = copy.deepcopy(TEMPLATE)
        second = copy.deepcopy(program["milestones"][0])
        second["id"] = "M1"
        second["coordinator_issue"] = "#3"
        second["work_items"][0]["ref"] = "#4"
        program["milestones"][0]["depends_on"] = ["M1"]
        program["milestones"].append(second)
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("must appear before" in error for error in errors))

    def test_duplicate_work_item_fails(self):
        program = copy.deepcopy(TEMPLATE)
        second = copy.deepcopy(program["milestones"][0])
        second["id"] = "M1"
        second["coordinator_issue"] = "#3"
        second["depends_on"] = ["M0"]
        program["milestones"].append(second)
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("duplicate work item" in error for error in errors))

    def test_invalid_issue_reference_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["work_items"][0]["ref"] = "issue-2"
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("issue/PR reference" in error for error in errors))

    def test_blank_completion_criterion_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["agent_completion_criteria"] = ["  "]
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("agent_completion_criteria[0]" in error for error in errors))

    def test_blank_human_gate_fails(self):
        program = copy.deepcopy(TEMPLATE)
        program["milestones"][0]["human_gates"] = [""]
        errors = vp.validate_program(program, SCHEMA)
        self.assertTrue(any("human_gates[0]" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
