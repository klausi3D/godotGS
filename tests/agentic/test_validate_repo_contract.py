#!/usr/bin/env python3
"""Unit tests for scripts/agentic/validate_repo_contract.py.

Uses synthetic roots (a copy of the real .agentic/ tree plus stub files) so the
tests do not depend on any single branch having the full AGENTS.md hierarchy.
"""

from __future__ import annotations

import copy
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "agentic" / "validate_repo_contract.py"
spec = importlib.util.spec_from_file_location("validate_repo_contract", SCRIPT)
assert spec and spec.loader
vrc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vrc)


def _make_valid_root(base: Path) -> Path:
    root = base / "repo"
    root.mkdir()
    shutil.copytree(ROOT / ".agentic", root / ".agentic")
    for rel in vrc.CONTROL_PLANE_FILES + vrc.HIERARCHY_FILES:
        path = root / rel
        if not path.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("stub\n", encoding="utf-8")
    return root


class ValidateRepoContractTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = _make_valid_root(Path(self._tmp.name))
        self._commit_lookup = mock.patch.object(vrc, "_git_commit_exists", return_value=True)
        self._commit_lookup.start()

    def tearDown(self):
        self._commit_lookup.stop()
        self._tmp.cleanup()

    def test_valid_root_passes(self):
        self.assertEqual(vrc.validate_repo_contract(self.root), [])
        self.assertEqual(vrc.validate_repo_contract(self.root, strict_hierarchy=True), [])

    def test_missing_role_file_fails(self):
        (self.root / ".agentic" / "roles" / "planner.md").unlink()
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("planner" in e for e in errors))

    def test_missing_control_plane_file_fails_by_default(self):
        (self.root / "scripts" / "agentic" / "classify_change.py").unlink()
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("classify_change.py" in e for e in errors))

    def test_missing_hierarchy_passes_default_fails_strict(self):
        (self.root / "AGENTS.md").unlink()
        # Default scope ignores the wider hierarchy, so the control plane is still valid.
        self.assertEqual(vrc.validate_repo_contract(self.root), [])
        # Strict scope requires it.
        strict_errors = vrc.validate_repo_contract(self.root, strict_hierarchy=True)
        self.assertTrue(any("AGENTS.md" in e for e in strict_errors))

    def test_invalid_json_fails(self):
        (self.root / ".agentic" / "policy.json").write_text("{ not valid json", encoding="utf-8")
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("invalid JSON" in e for e in errors))

    def test_session_id_artifact_fails(self):
        readme = self.root / ".agentic" / "README.md"
        readme.write_text(
            readme.read_text(encoding="utf-8") + "\nagent-build-ci: 019d0571-b295-7a1c-9f3e-0123456789ab\n",
            encoding="utf-8",
        )
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("session-id" in e for e in errors))

    def test_template_schema_mismatch_fails(self):
        # Break the task template so it no longer matches its schema.
        (self.root / ".agentic" / "templates" / "task.json").write_text(
            '{"schema_version": 1}', encoding="utf-8"
        )
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("task.json does not match" in e for e in errors))

    def test_program_template_semantic_mismatch_fails(self):
        path = self.root / ".agentic" / "templates" / "program.json"
        program = json.loads(path.read_text(encoding="utf-8"))
        program["milestones"][0]["objective"] = "   "
        path.write_text(json.dumps(program), encoding="utf-8")

        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("program.json is invalid" in error and "objective" in error for error in errors))

    def test_program_template_repository_references_must_be_usable(self):
        path = self.root / ".agentic" / "templates" / "program.json"
        original = json.loads(path.read_text(encoding="utf-8"))
        cases = (
            (".agentic/templates/missing.json", "does not exist"),
            ("../outside.json", "must stay inside the repository"),
            (".agentic/templates/review.json", "does not match task schema"),
        )

        for template_path, expected_error in cases:
            with self.subTest(template_path=template_path):
                program = copy.deepcopy(original)
                program["dispatch"]["task_contract_template"] = template_path
                path.write_text(json.dumps(program), encoding="utf-8")

                errors = vrc.validate_repo_contract(self.root)
                self.assertTrue(
                    any("program.json is invalid" in error and expected_error in error for error in errors)
                )

    def test_non_object_program_schema_fails(self):
        path = self.root / ".agentic" / "schemas" / "program.schema.json"
        path.write_text("[]", encoding="utf-8")

        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("program.schema.json" in error and "must be an object" in error for error in errors))

    def test_non_object_program_template_fails(self):
        path = self.root / ".agentic" / "templates" / "program.json"
        path.write_text("null", encoding="utf-8")

        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("program.json is invalid" in error and "expected type object" in error for error in errors))

    def test_program_schema_defining_constraints_cannot_be_removed(self):
        path = self.root / ".agentic" / "schemas" / "program.schema.json"
        original = json.loads(path.read_text(encoding="utf-8"))

        mutations = []
        mutations.append(("vacuous schema", {}))

        missing_root_requirement = copy.deepcopy(original)
        missing_root_requirement["required"].remove("schema_version")
        mutations.append(("root requirement", missing_root_requirement))

        missing_dispatch_requirement = copy.deepcopy(original)
        missing_dispatch_requirement["properties"]["dispatch"]["required"].remove("heavy_process_limit")
        mutations.append(("dispatch requirement", missing_dispatch_requirement))

        missing_milestone_requirement = copy.deepcopy(original)
        missing_milestone_requirement["properties"]["milestones"]["items"]["required"].remove("human_gates")
        mutations.append(("milestone requirement", missing_milestone_requirement))

        missing_work_item_requirement = copy.deepcopy(original)
        work_item_schema = missing_work_item_requirement["properties"]["milestones"]["items"]["properties"][
            "work_items"
        ]["items"]
        work_item_schema["required"].remove("purpose")
        mutations.append(("work-item requirement", missing_work_item_requirement))

        missing_version_const = copy.deepcopy(original)
        del missing_version_const["properties"]["schema_version"]["const"]
        mutations.append(("schema-version const", missing_version_const))

        missing_dependency_items = copy.deepcopy(original)
        del missing_dependency_items["properties"]["milestones"]["items"]["properties"][
            "depends_on"
        ]["items"]
        mutations.append(("dependency item type", missing_dependency_items))

        permissive_root = copy.deepcopy(original)
        permissive_root["additionalProperties"] = True
        mutations.append(("root additional properties", permissive_root))

        missing_live_status_const = copy.deepcopy(original)
        del missing_live_status_const["properties"]["dispatch"]["properties"][
            "live_status_requery_required"
        ]["const"]
        mutations.append(("live-status const", missing_live_status_const))

        expanded_work_item_kind = copy.deepcopy(original)
        kind_schema = expanded_work_item_kind["properties"]["milestones"]["items"]["properties"][
            "work_items"
        ]["items"]["properties"]["kind"]
        kind_schema["enum"].append("free_form")
        mutations.append(("work-item kind enum", expanded_work_item_kind))

        for label, mutated in mutations:
            with self.subTest(label=label):
                path.write_text(json.dumps(mutated), encoding="utf-8")
                errors = vrc.validate_repo_contract(self.root)
                self.assertTrue(
                    any("program.schema.json contract" in error for error in errors),
                    f"schema contract accepted mutation: {label}",
                )

    def test_invalid_program_fails(self):
        path = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        program = json.loads(path.read_text(encoding="utf-8"))
        program["milestones"][1]["depends_on"] = ["MISSING"]
        path.write_text(json.dumps(program), encoding="utf-8")
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("unknown milestone" in e for e in errors))

    def test_non_object_program_fails(self):
        path = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        path.write_text("[]", encoding="utf-8")
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("must be an object" in e for e in errors))

    def test_duplicate_program_id_fails(self):
        source = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        duplicate = self.root / ".agentic" / "programs" / "duplicate.json"
        duplicate.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")

        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("duplicate program_id" in error for error in errors))

    def test_at_least_one_concrete_program_manifest_is_required(self):
        program_dir = self.root / ".agentic" / "programs"
        (program_dir / "continuation-2026-08.json").unlink()

        empty_errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("concrete program manifest" in error for error in empty_errors))

        program_dir.rmdir()
        missing_errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("concrete program manifest" in error for error in missing_errors))

    def test_non_file_program_manifest_does_not_satisfy_requirement(self):
        program_dir = self.root / ".agentic" / "programs"
        (program_dir / "continuation-2026-08.json").unlink()
        (program_dir / "fake.json").mkdir()

        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("concrete program manifest" in error for error in errors))

    def test_unresolvable_program_snapshot_fails(self):
        path = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        program = json.loads(path.read_text(encoding="utf-8"))
        program["planning_snapshot_sha"] = "f" * 40
        path.write_text(json.dumps(program), encoding="utf-8")
        with mock.patch.object(vrc, "_git_commit_exists", return_value=False):
            errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("planning_snapshot_sha" in e and "does not resolve" in e for e in errors))

    def test_program_task_contract_template_must_exist(self):
        path = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        program = json.loads(path.read_text(encoding="utf-8"))
        program["dispatch"]["task_contract_template"] = ".agentic/templates/missing.json"
        path.write_text(json.dumps(program), encoding="utf-8")
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("task_contract_template" in e and "does not exist" in e for e in errors))

    def test_program_task_contract_template_must_match_task_schema(self):
        path = self.root / ".agentic" / "programs" / "continuation-2026-08.json"
        program = json.loads(path.read_text(encoding="utf-8"))
        program["dispatch"]["task_contract_template"] = ".agentic/templates/review.json"
        path.write_text(json.dumps(program), encoding="utf-8")
        errors = vrc.validate_repo_contract(self.root)
        self.assertTrue(any("task_contract_template" in e and "task schema" in e for e in errors))


if __name__ == "__main__":
    unittest.main()
