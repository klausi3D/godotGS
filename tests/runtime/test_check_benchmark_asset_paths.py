#!/usr/bin/env python3
"""Mutation tests for runtime PLY-reference floor coverage (refs #895)."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "runtime" / "check_benchmark_asset_paths.py"
spec = importlib.util.spec_from_file_location("check_benchmark_asset_paths", SCRIPT)
assert spec and spec.loader
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)


class RuntimeAssetReferenceFloorTests(unittest.TestCase):
    def test_declared_runtime_fixture_reference_is_covered(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            script = Path(raw_td) / "runtime_probe.gd"
            script.write_text(
                'const ASSET := "res://tests/fixtures/test_splats.ply"\n',
                encoding="utf-8",
            )
            violations = guard._runtime_reference_violations(
                script,
                {"res://tests/fixtures/test_splats.ply": 10000},
            )

        self.assertEqual(violations, [])

    def test_undeclared_runtime_fixture_reference_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw_td:
            script = Path(raw_td) / "runtime_probe.gd"
            script.write_text(
                'const ASSET := "res://tests/fixtures/unfloored.ply"\n',
                encoding="utf-8",
            )
            violations = guard._runtime_reference_violations(
                script,
                {"res://tests/fixtures/test_splats.ply": 10000},
            )

        self.assertEqual(len(violations), 1)
        self.assertIn("unfloored.ply", violations[0])
        self.assertIn("no ASSET_MIN_SPLAT_COUNTS floor", violations[0])

    def test_live_runtime_consumers_are_nonempty_and_all_floor_backed(self) -> None:
        candidates = guard._iter_runtime_candidate_files()
        self.assertTrue(candidates, "runtime PLY-reference scan covered no files")

        referenced = 0
        violations: list[str] = []
        for path in candidates:
            text = path.read_text(encoding="utf-8")
            referenced += len(guard.HARDCODED_PLY_RE.findall(text))
            violations.extend(
                guard._runtime_reference_violations(path, guard.ASSET_MIN_SPLAT_COUNTS)
            )

        self.assertGreater(referenced, 0, "runtime scan found no PLY references")
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
