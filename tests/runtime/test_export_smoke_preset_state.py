#!/usr/bin/env python3
"""Preset/backup state machine for run_export_smoke.py (#825).

`tests/examples/godot/test_project/export_presets.cfg` is gitignored, so a
developer's own copy would be destroyed *invisibly* -- `git status` never shows
it. The runner therefore refuses to take it over without `--overwrite-preset`,
and moves it to `export_presets.cfg.smoke-backup` for the duration of a run.

That backup is itself state, with three possible starting conditions, and an
earlier version of the fix handled only one of them:

  1. no backup                 -- normal
  2. backup + generated preset -- a previous run died mid-flight
  3. backup + no preset        -- a previous run died between the two moves

In (2) and (3) that version called `backup_path.unlink()` unconditionally,
deleting the developer's only surviving original in exactly the crash-recovery
case the backup existed to serve: the preservation fix had relocated its own
defect from the happy path to the recovery path. These tests pin all three
states, and assert that a refusal leaves every byte on disk untouched.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_export_smoke as smoke  # noqa: E402

ORIGINAL_PRESET_BYTES = "[preset.0]\nname=\"My Own Preset\"\n"
GENERATED_MARKER = "custom_template/release="


class PresetStateTestCase(unittest.TestCase):
    """Repoints the module at a temp project dir so no real config is touched."""

    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)
        self.project_dir = Path(self.stack.name) / "test_project"
        self.project_dir.mkdir(parents=True)

        self._saved_project_dir = smoke.PROJECT_DIR
        smoke.PROJECT_DIR = self.project_dir
        self.addCleanup(self._restore_project_dir)

        self.preset, self.backup = smoke.preset_paths()

    def _restore_project_dir(self) -> None:
        smoke.PROJECT_DIR = self._saved_project_dir

    def _write(self, path: Path, text: str) -> None:
        path.write_text(text, encoding="utf-8")

    def assertUnchanged(self, path: Path, expected: str) -> None:
        self.assertTrue(path.exists(), f"{path.name} was deleted")
        self.assertEqual(path.read_text(encoding="utf-8"), expected, f"{path.name} was rewritten")


class StateOneNoBackupTests(PresetStateTestCase):
    """State 1: no backup file present."""

    def test_clean_project_proceeds(self) -> None:
        self.assertIsNone(smoke.preset_state_error(overwrite_allowed=False))
        self.assertIsNone(smoke.preset_state_error(overwrite_allowed=True))

    def test_existing_preset_refuses_without_the_flag(self) -> None:
        self._write(self.preset, ORIGINAL_PRESET_BYTES)
        refusal = smoke.preset_state_error(overwrite_allowed=False)
        self.assertIsNotNone(refusal)
        self.assertIn("--overwrite-preset", refusal)
        self.assertUnchanged(self.preset, ORIGINAL_PRESET_BYTES)

    def test_existing_preset_proceeds_with_the_flag(self) -> None:
        self._write(self.preset, ORIGINAL_PRESET_BYTES)
        self.assertIsNone(smoke.preset_state_error(overwrite_allowed=True))


class StateTwoBackupAndPresetTests(PresetStateTestCase):
    """State 2: a previous run died between backing up and restoring."""

    def setUp(self) -> None:
        super().setUp()
        self._write(self.backup, ORIGINAL_PRESET_BYTES)
        self._write(self.preset, "generated-by-a-previous-run\n")

    def test_refuses_even_with_the_overwrite_flag(self) -> None:
        for allowed in (False, True):
            with self.subTest(overwrite_allowed=allowed):
                refusal = smoke.preset_state_error(overwrite_allowed=allowed)
                self.assertIsNotNone(refusal, "a leftover backup must never be silently consumed")
                self.assertIn("interrupted", refusal)
                self.assertIn(self.backup.name, refusal)

    def test_refusal_message_says_the_flag_does_not_override_it(self) -> None:
        refusal = smoke.preset_state_error(overwrite_allowed=True)
        self.assertIn("--overwrite-preset does not override this", refusal)

    def test_refusal_touches_neither_file(self) -> None:
        smoke.preset_state_error(overwrite_allowed=True)
        self.assertUnchanged(self.backup, ORIGINAL_PRESET_BYTES)
        self.assertUnchanged(self.preset, "generated-by-a-previous-run\n")

    def test_the_original_defect_would_have_destroyed_the_backup(self) -> None:
        """Names the regression: this is the file the old code unlinked.

        The pre-flight must refuse *before* anything reaches `_write_preset`,
        and `_write_preset` must refuse too if it is ever called anyway.
        """
        self.assertIsNotNone(smoke.preset_state_error(overwrite_allowed=True))
        with self.assertRaises(RuntimeError) as ctx:
            smoke._write_preset(ROOT / "tests" / "runtime" / "run_export_smoke.py", self.backup)
        self.assertIn("backup", str(ctx.exception))
        self.assertUnchanged(self.backup, ORIGINAL_PRESET_BYTES)


class StateThreeBackupOnlyTests(PresetStateTestCase):
    """State 3: a previous run died after moving the preset aside."""

    def setUp(self) -> None:
        super().setUp()
        self._write(self.backup, ORIGINAL_PRESET_BYTES)

    def test_refuses_with_and_without_the_flag(self) -> None:
        for allowed in (False, True):
            with self.subTest(overwrite_allowed=allowed):
                refusal = smoke.preset_state_error(overwrite_allowed=allowed)
                self.assertIsNotNone(refusal)
                self.assertIn(self.backup.name, refusal)

    def test_refusal_leaves_the_only_surviving_copy_alone(self) -> None:
        smoke.preset_state_error(overwrite_allowed=True)
        self.assertUnchanged(self.backup, ORIGINAL_PRESET_BYTES)
        self.assertFalse(self.preset.exists())


class HappyPathRoundTripTests(PresetStateTestCase):
    def test_write_then_restore_returns_the_original_bytes(self) -> None:
        self._write(self.preset, ORIGINAL_PRESET_BYTES)
        self.assertIsNone(smoke.preset_state_error(overwrite_allowed=True))

        generated = smoke._write_preset(ROOT / "bin", self.backup)
        self.assertTrue(self.backup.exists())
        self.assertEqual(self.backup.read_text(encoding="utf-8"), ORIGINAL_PRESET_BYTES)
        self.assertIn(GENERATED_MARKER, generated.read_text(encoding="utf-8"))

        smoke._restore_preset(generated, self.backup)
        self.assertUnchanged(self.preset, ORIGINAL_PRESET_BYTES)
        self.assertFalse(self.backup.exists(), "the backup must be consumed by a successful restore")

    def test_restore_with_no_backup_just_removes_the_generated_preset(self) -> None:
        self.assertIsNone(smoke.preset_state_error(overwrite_allowed=False))
        generated = smoke._write_preset(ROOT / "bin", self.backup)
        self.assertTrue(generated.exists())
        smoke._restore_preset(generated, self.backup)
        self.assertFalse(self.preset.exists())
        self.assertFalse(self.backup.exists())


class BinaryPathResolutionTests(unittest.TestCase):
    """A path validated in one working directory and used from another.

    `--editor-binary` / `--template` are checked with the caller's cwd but the
    subprocess runs with cwd=ROOT, so a relative path that passed the existence
    check would then not be found. Both must come back absolute.
    """

    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)
        self.here = Path(self.stack.name).resolve()
        self.binary = self.here / "godot-stub.exe"
        self.binary.write_bytes(b"stub")

        saved_cwd = Path.cwd()
        os.chdir(self.here)
        self.addCleanup(os.chdir, saved_cwd)

    def test_relative_editor_binary_resolves_against_the_callers_cwd(self) -> None:
        resolved = smoke._resolve_editor("godot-stub.exe")
        self.assertIsNotNone(resolved)
        self.assertTrue(resolved.is_absolute(), f"{resolved} is relative and would break under cwd=ROOT")
        self.assertEqual(resolved, self.binary)

    def test_relative_template_resolves_against_the_callers_cwd(self) -> None:
        resolved = smoke._resolve_template("godot-stub.exe")
        self.assertIsNotNone(resolved)
        self.assertTrue(resolved.is_absolute())
        self.assertEqual(resolved, self.binary)

    def test_missing_relative_path_is_still_rejected(self) -> None:
        self.assertIsNone(smoke._resolve_editor("no-such-binary.exe"))
        self.assertIsNone(smoke._resolve_template("no-such-binary.exe"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
