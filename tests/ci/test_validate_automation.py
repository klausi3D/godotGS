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

# These fixtures exercise TRIGGER shapes, so the job body is a stub. It still has
# to be an executable job: check_ci_workflow() now rejects a job with neither
# `runs-on` nor `uses`, because a populated `jobs` mapping alone does not make a
# workflow runnable. Previously the stub was `{}`, which relied on job bodies
# never being inspected -- the exact gap that check closes.
_EXECUTABLE_JOB = {"runs-on": "ubuntu-latest"}


class _FakeYaml(types.SimpleNamespace):
    class BaseLoader:
        pass

    class SafeLoader:
        def construct_mapping(self, node: object, deep: bool = False) -> object:
            del node, deep
            return {}

    @staticmethod
    def load(text: str, Loader: object) -> object:
        typed_scalars = isinstance(Loader, type) and issubclass(
            Loader, _FakeYaml.SafeLoader
        )
        preserves_on_key = typed_scalars and (
            Loader.construct_mapping is not _FakeYaml.SafeLoader.construct_mapping
        )
        if "INVALID_FOR_TEST" in text:
            raise ValueError("synthetic invalid YAML")
        if "EMPTY_FOR_TEST" in text:
            return None
        if "SCALAR_FOR_TEST" in text:
            return "workflow"
        if "LIST_FOR_TEST" in text:
            return ["workflow"]
        if "NO_TRIGGER_FOR_TEST" in text:
            return {"name": "valid", "jobs": {"test": _EXECUTABLE_JOB}}
        if "EMPTY_TRIGGER_FOR_TEST" in text:
            return {"name": "valid", "on": {}, "jobs": {"test": _EXECUTABLE_JOB}}
        if "NULL_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": None if typed_scalars else "null",
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "TILDE_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": None if typed_scalars else "~",
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "BOOL_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": False if typed_scalars else "false",
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "NUMBER_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": 0 if typed_scalars else "0",
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "NULL_LIST_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": [None] if typed_scalars else ["null"],
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "SCALAR_TRIGGER_FOR_TEST" in text:
            return {"name": "valid", "on": "push", "jobs": {"test": _EXECUTABLE_JOB}}
        if "LIST_TRIGGER_FOR_TEST" in text:
            return {
                "name": "valid",
                "on": ["push", "pull_request"],
                "jobs": {"test": _EXECUTABLE_JOB},
            }
        if "KEY_PRESERVATION_FOR_TEST" in text:
            key: object = "on" if preserves_on_key else True
            return {"name": "valid", key: "push", "jobs": {"test": _EXECUTABLE_JOB}}
        return {
            "name": "valid",
            "on": {"pull_request": {}},
            "jobs": {"test": _EXECUTABLE_JOB},
        }

    @staticmethod
    def safe_load(text: str) -> object:
        document = _FakeYaml.load(text, Loader=_FakeYaml.BaseLoader)
        if isinstance(document, dict) and "on" in document:
            document[True] = document.pop("on")
        return document


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

    def test_workflow_without_nonempty_trigger_fails_closed(self) -> None:
        for marker in ("NO_TRIGGER_FOR_TEST", "EMPTY_TRIGGER_FOR_TEST"):
            with self.subTest(marker=marker):
                contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = marker
                temp_dir = self._root_with_workflows(contents)

                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
                    self.assertFalse(validate_automation.check_ci_workflow())

    def test_workflow_with_invalid_scalar_trigger_fails_closed(self) -> None:
        markers = (
            "NULL_TRIGGER_FOR_TEST",
            "TILDE_TRIGGER_FOR_TEST",
            "BOOL_TRIGGER_FOR_TEST",
            "NUMBER_TRIGGER_FOR_TEST",
            "NULL_LIST_TRIGGER_FOR_TEST",
        )
        for marker in markers:
            with self.subTest(marker=marker):
                contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = marker
                temp_dir = self._root_with_workflows(contents)

                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
                    self.assertFalse(validate_automation.check_ci_workflow())

    def test_supported_trigger_shapes_remain_valid(self) -> None:
        for marker in ("SCALAR_TRIGGER_FOR_TEST", "LIST_TRIGGER_FOR_TEST", "name: valid"):
            with self.subTest(marker=marker):
                contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = marker
                temp_dir = self._root_with_workflows(contents)

                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
                    self.assertTrue(validate_automation.check_ci_workflow())

    def test_duplicate_mapping_keys_are_rejected(self) -> None:
        """PyYAML keeps the LAST duplicate; GitHub rejects the document.

        A workflow with two `on:` or two `jobs:` blocks therefore parsed cleanly
        and satisfied every structure check below, while Actions refused to run
        it -- the workflow disappears and the required validator stays green.
        """
        header = ["name: v", "on:", "  push:", "    branches: [master]"]
        executable = ["jobs:", "  build:", "    runs-on: ubuntu-latest",
                      "    steps:", "      - run: echo hi"]
        good = "\n".join(header + executable) + "\n"
        dupes = {
            "duplicate jobs": "\n".join(header + executable + executable) + "\n",
            "duplicate on": "\n".join(header + ["on:", "  workflow_dispatch:"] + executable) + "\n",
        }
        for label, body in dupes.items():
            with self.subTest(shape=label):
                contents = {name: good for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertFalse(
                        validate_automation.check_ci_workflow(),
                        f"a workflow with {label} passed; Actions would refuse it",
                    )

    def test_a_launcher_key_with_no_usable_value_is_rejected(self) -> None:
        """Key presence is not a launcher. `runs-on:` null or `[]` names no runner."""
        header = ["name: v", "on:", "  push:", "    branches: [master]", "jobs:"]
        good = "\n".join(header + ["  build:", "    runs-on: ubuntu-latest",
                                   "    steps:", "      - run: echo hi"]) + "\n"
        bad = {
            "runs-on null": "\n".join(header + ["  build:", "    runs-on:"]) + "\n",
            "runs-on empty list": "\n".join(header + ["  build:", "    runs-on: []"]) + "\n",
            "runs-on empty string": "\n".join(header + ["  build:", '    runs-on: ""']) + "\n",
            "uses null": "\n".join(header + ["  build:", "    uses:"]) + "\n",
            "runs-on empty mapping": "\n".join(header + ["  build:", "    runs-on: {}"]) + "\n",
            "runs-on mapping no fields": "\n".join(header + ["  build:", "    runs-on:", "      flavour: big"]) + "\n",
            "runs-on labels empty": "\n".join(header + ["  build:", "    runs-on:", "      labels: []"]) + "\n",
            "runs-on mapping with a stray field": "\n".join(header + ["  build:", "    runs-on:", "      group: g", "      flavour: big"]) + "\n",
            "both launchers": "\n".join(
                header + ["  build:", "    runs-on: ubuntu-latest",
                          "    uses: ./.github/workflows/other.yml"]) + "\n",
        }
        for label, body in bad.items():
            with self.subTest(shape=label):
                contents = {name: good for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertFalse(
                        validate_automation.check_ci_workflow(),
                        f"a job with {label} was accepted; it launches nothing",
                    )

        # Every valid launcher shape must still pass, or this check is a false
        # positive that would reject real workflows in this tree.
        ok = {
            "runs-on string": "    runs-on: ubuntu-latest\n    steps:\n      - run: echo hi",
            "runs-on label list": "    runs-on: [self-hosted, Windows, gpu]\n    steps:\n      - run: echo hi",
            "runs-on group mapping": "    runs-on:\n      group: g\n      labels: [x]\n    steps:\n      - run: echo hi",
            "runs-on group only": "    runs-on:\n      group: g\n    steps:\n      - run: echo hi",
            "runs-on labels only": "    runs-on:\n      labels: [self-hosted]\n    steps:\n      - run: echo hi",
            "reusable uses": "    uses: ./.github/workflows/other.yml",
        }
        for label, tail in ok.items():
            with self.subTest(shape=label):
                body = "\n".join(header + ["  build:"]) + "\n" + tail + "\n"
                contents = {name: good for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertTrue(
                        validate_automation.check_ci_workflow(),
                        f"a valid launcher ({label}) was rejected",
                    )

    def test_an_unrecognised_trigger_event_is_rejected(self) -> None:
        """`on: pus` parses as a fine string and fires for nothing."""
        header = ["name: v"]
        jobs = ["jobs:", "  build:", "    runs-on: ubuntu-latest",
                "    steps:", "      - run: echo hi"]
        good = "\n".join(header + ["on:", "  push:", "    branches: [master]"] + jobs) + "\n"
        typos = {
            "scalar typo": "\n".join(header + ["on: pus"] + jobs) + "\n",
            "mapping typo": "\n".join(header + ["on:", "  pus:"] + jobs) + "\n",
            "list typo": "\n".join(header + ["on: [push, pul_request]"] + jobs) + "\n",
        }
        for label, body in typos.items():
            with self.subTest(shape=label):
                contents = {name: good for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertFalse(
                        validate_automation.check_ci_workflow(),
                        f"an unrecognised event ({label}) was accepted",
                    )

        # Real event names must still pass. Without this the check above would be
        # satisfied by a validator that rejects every trigger.
        for event_body in (
            "on: push",
            "on: [push, pull_request]",
            "on:\n  schedule:\n    - cron: '0 0 * * *'",
            "on:\n  workflow_dispatch:",
        ):
            with self.subTest(shape=event_body.splitlines()[0]):
                body = "\n".join(header) + "\n" + event_body + "\n" + "\n".join(jobs) + "\n"
                contents = {name: good for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertTrue(
                        validate_automation.check_ci_workflow(),
                        f"a real event was rejected: {event_body!r}",
                    )

    def test_hollow_job_bodies_are_rejected(self) -> None:
        """A populated `jobs` mapping is not an executable workflow.

        A job body truncated to `null`/`{}`, or one carrying only metadata, leaves
        the OUTER mapping non-empty while GitHub Actions can run nothing, so a
        workflow could lose its whole executable definition with this required
        validator still green. Uses REAL yaml rather than _FakeYaml, so the shapes
        parsed here are the ones GitHub would actually parse.
        """
        header = ["name: v", "on:", "  push:", "    branches: [master]", "jobs:"]

        def wf(*job_lines: str) -> str:
            return "\n".join(header + list(job_lines)) + "\n"

        hollow = {
            "null body": wf("  build:"),
            "empty body": wf("  build: {}"),
            "metadata only": wf("  build:", "    name: no runs-on", "    timeout-minutes: 5"),
        }
        executable = wf("  build:", "    runs-on: ubuntu-latest", "    steps:", "      - run: echo hi")
        reusable = wf("  build:", "    uses: ./.github/workflows/other.yml")

        for label, body in hollow.items():
            with self.subTest(shape=label):
                contents = {name: executable for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertFalse(
                        validate_automation.check_ci_workflow(),
                        f"a {label} job passed validation; that workflow cannot run",
                    )

        # Both executable shapes must still pass. Without this half the check
        # above would be satisfied by a validator that rejects everything, and a
        # `uses:` reusable-workflow caller has no `runs-on` by design.
        for label, body in (("runs-on", executable), ("reusable uses", reusable)):
            with self.subTest(shape=label):
                contents = {name: executable for name in CURRENT_WORKFLOW_NAMES}
                contents["agentic_pr_gate.yml"] = body
                temp_dir = self._root_with_workflows(contents)
                with temp_dir, mock.patch.object(
                    validate_automation, "ROOT_DIR", Path(temp_dir.name)
                ):
                    self.assertTrue(
                        validate_automation.check_ci_workflow(),
                        f"an executable job ({label}) was rejected",
                    )

    def test_loader_preserves_literal_on_mapping_key(self) -> None:
        contents = {name: "name: valid\n" for name in CURRENT_WORKFLOW_NAMES}
        contents["agentic_pr_gate.yml"] = "KEY_PRESERVATION_FOR_TEST"
        temp_dir = self._root_with_workflows(contents)

        with temp_dir, mock.patch.object(
            validate_automation, "ROOT_DIR", Path(temp_dir.name)
        ), mock.patch.dict(sys.modules, {"yaml": _FakeYaml()}):
            self.assertTrue(validate_automation.check_ci_workflow())

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
