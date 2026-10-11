"""Bounded CPU controls for the genuine Editor producer; no Godot invocation."""
from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class CanonicalEditorWiring(unittest.TestCase):
    def setUp(self):
        self.module = load("live_editor_module_control", ROOT / "tests/ci/run_module_tests.py")

    def run_lane(self, name):
        result = self.module.GodotRunResult(False, False, "controlled missing summary", 9)
        with mock.patch.object(self.module, "_run_godot", return_value=result) as ordinary, \
                mock.patch.object(self.module, "_run_live_editor", return_value=result, create=True) as editor, \
                contextlib.redirect_stdout(io.StringIO()):
            self.module._execute_lane("OWN_FAKE_NO_GODOT", name, ["--headless", "--test"],
                                      True, "strict", False, {}, self.module.DoctestTotals())
        return ordinary, editor

    def test_actual_canonical_editor_consumer_uses_real_runtime(self):
        ordinary, editor = self.run_lane("GaussianSplatting [Editor]")
        editor.assert_called_once()
        ordinary.assert_not_called()

    def test_other_canonical_lane_preserves_ordinary_producer(self):
        ordinary, editor = self.run_lane("GaussianSplatting [Config]")
        ordinary.assert_called_once()
        editor.assert_not_called()


if __name__ == "__main__":
    unittest.main()
