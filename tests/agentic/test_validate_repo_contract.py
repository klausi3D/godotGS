#!/usr/bin/env python3
"""Unit tests for scripts/agentic/validate_repo_contract.py.

Uses synthetic roots (a copy of the real .agentic/ tree plus stub files) so the
tests do not depend on any single branch having the full AGENTS.md hierarchy.
"""

from __future__ import annotations

import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

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

    def tearDown(self):
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


if __name__ == "__main__":
    unittest.main()
