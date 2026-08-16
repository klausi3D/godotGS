#!/usr/bin/env python3
"""Regression tests for the automation-contract validator (refs #894)."""

from __future__ import annotations

import builtins
import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "tests" / "ci" / "validate_automation.py"
SPEC = importlib.util.spec_from_file_location("validate_automation", MODULE_PATH)
assert SPEC and SPEC.loader
validate_automation = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = validate_automation
SPEC.loader.exec_module(validate_automation)


CURRENT_WORKFLOW_NAMES = tuple(sorted(validate_automation.REQUIRED_WORKFLOW_NAMES))


class _FakeYaml(types.SimpleNamespace):
    @staticmethod
    def safe_load(text: str) -> object:
        if "INVALID_FOR_TEST" in text:
            raise ValueError("synthetic invalid YAML")
        if "EMPTY_FOR_TEST" in text:
            return None
        if "SCALAR_FOR_TEST" in text:
            return "workflow"
        if "LIST_FOR_TEST" in text:
            return ["workflow"]
        return {"name": "valid", "jobs": {"test": {}}}


class ValidateAutomationWorkflowTests(unittest.TestCase):
    def _root_with_workflows(self, contents: dict[str, str]) -> tempfile.TemporaryDirectory[str]:
        temp_dir = tempfile.TemporaryDirectory()
        workflow_dir = Path(temp_dir.name) / ".github" / "workflows"
        workflow_dir.mkdir(parents=True)
        for name, text in contents.items():
            (workflow_dir / name).write_text(text, encoding="utf-8")
        return temp_dir

    def test_missing_pyyaml_fails_closed(self) -> None:
        temp_dir = self._root_with_workflows(
            {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
        )
        real_import = builtins.__import__

        def import_without_yaml(name: str, *args: object, **kwargs: object) -> object:
            if name == "yaml":
                raise ImportError("PyYAML intentionally unavailable")
            return real_import(name, *args, **kwargs)

        with temp_dir, mock.patch.object(
            validate_automation, "ROOT_DIR", Path(temp_dir.name)
        ), mock.patch("builtins.__import__", side_effect=import_without_yaml):
            self.assertFalse(validate_automation.check_ci_workflow())

    def test_workflow_set_is_derived_and_includes_yaml_suffix(self) -> None:
        contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
        contents["newly_added.yaml"] = "INVALID_FOR_TEST\n"
        temp_dir = self._root_with_workflows(contents)

        with temp_dir, mock.patch.object(
            validate_automation, "ROOT_DIR", Path(temp_dir.name)
        ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
            self.assertFalse(validate_automation.check_ci_workflow())

    def test_missing_required_workflow_fails_closed(self) -> None:
        contents = {
            name: "name: valid\n"
            for name in CURRENT_WORKFLOW_NAMES
            if name != "gaussian_shader_validation.yml"
        }
        temp_dir = self._root_with_workflows(contents)

        with temp_dir, mock.patch.object(
            validate_automation, "ROOT_DIR", Path(temp_dir.name)
        ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
            self.assertFalse(validate_automation.check_ci_workflow())

    def test_non_workflow_yaml_documents_are_rejected(self) -> None:
        for marker in ("EMPTY_FOR_TEST", "SCALAR_FOR_TEST", "LIST_FOR_TEST"):
            with self.subTest(marker=marker):
                contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = marker
                temp_dir = self._root_with_workflows(contents)

                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
                    self.assertFalse(validate_automation.check_ci_workflow())

    def test_required_gate_installs_parser_and_runs_validator(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "agentic_pr_gate.yml").read_text(
            encoding="utf-8"
        )
        install = (
            "run: python -m pip install --require-hashes "
            "-r tests/ci/requirements-automation.txt"
        )
        unit_test = "run: python tests/ci/test_validate_automation.py -v"
        validation = "run: python tests/ci/validate_automation.py --contracts-only"

        self.assertIn("runs-on: ubuntu-latest", workflow)
        self.assertIn(install, workflow)
        self.assertIn(unit_test, workflow)
        self.assertIn(validation, workflow)
        self.assertLess(workflow.index(install), workflow.index(validation))


if __name__ == "__main__":
    unittest.main()
