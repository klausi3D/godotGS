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
import subprocess
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


class NegativeControlPresetTests(PresetStateTestCase):
    """`--expect-stock-template-failure` must actually write an EMPTY template path.

    The whole control rests on this substitution. If it silently kept writing the
    template path the control would run the *positive* configuration and report
    the reassuring answer, which is worse than not having it.
    """

    def test_default_writes_the_template_path(self) -> None:
        generated = smoke._write_preset(ROOT / "bin", self.backup)
        text = generated.read_text(encoding="utf-8")
        self.assertIn(f'custom_template/release="{(ROOT / "bin").resolve().as_posix()}"', text)

    def test_empty_string_is_honoured_rather_than_treated_as_unset(self) -> None:
        # `""` is falsy, so a truth test here would fall back to the template
        # path and quietly turn the negative control into a second positive run.
        generated = smoke._write_preset(ROOT / "bin", self.backup, custom_template_release="")
        self.assertIn('custom_template/release=""', generated.read_text(encoding="utf-8"))


# What the editor actually prints when it refuses the export because no template
# can be resolved. Assembled from the engine sources this control keys off:
#   editor/editor_node.cpp                     -- the untranslated refusal prefix
#   editor/export/editor_export_platform.cpp   -- the translated reason + the path
#   editor/file_system/editor_paths.h          -- export_templates_folder
#   platform/windows/export/export_plugin.cpp  -- windows_release_x86_64.exe
GENUINE_REJECTION_OUTPUT = """\
Godot Engine v4.5.stable.custom_build - https://godotengine.org
Registering GaussianSplatting types
ERROR: Cannot export project with preset "Export Smoke" due to configuration errors:
No export template found at the expected path:
C:/Users/runner/AppData/Roaming/Godot/export_templates/4.5.stable/windows_debug_x86_64.exe
No export template found at the expected path:
C:/Users/runner/AppData/Roaming/Godot/export_templates/4.5.stable/windows_release_x86_64.exe

   at: _dispatch_export (editor/editor_node.cpp:1268)
ERROR: Project export for preset "Export Smoke" failed.
"""

# The same refusal on a runner whose editor language is not English: every TTR()
# string is translated, the vformat() prefix and the interpolated path are not.
# This is the case that rules out matching the English message text.
GENUINE_REJECTION_OUTPUT_TRANSLATED = """\
ERROR: Cannot export project with preset "Export Smoke" due to configuration errors:
Keine Exportvorlage unter dem erwarteten Pfad gefunden:
C:/Users/runner/AppData/Roaming/Godot/export_templates/4.5.stable/windows_release_x86_64.exe
ERROR: Project export for preset "Export Smoke" failed.
"""

# A configuration error that is NOT the one this control is about. It reaches the
# same refusal prefix, so the prefix alone cannot be the whole signal.
UNRELATED_CONFIG_ERROR_OUTPUT = """\
ERROR: Cannot export project with preset "Export Smoke" due to configuration errors:
A texture format must be selected to export the project. Please select at least one texture format.
ERROR: Project export for preset "Export Smoke" failed.
"""

UNRELATED_RESOURCE_ERROR_OUTPUT = """\
ERROR: Cannot open file 'res://tests/fixtures/synthetic_cube.ply'.
   at: _load (core/io/resource_loader.cpp:283)
ERROR: Failed to load resource. Export aborted.
"""

TIMEOUT_OUTPUT = "Godot Engine v4.5.stable.custom_build\nEditor started, importing...\n"


class NegativeControlOutcomeTests(unittest.TestCase):
    """The outcomes of `--expect-stock-template-failure`, and which of them pass.

    The rule this class pins: the control passes only on a *positive finding*
    (a proven missing-template rejection, or a produced stock binary). "The
    export did not succeed" is not a finding -- a timeout, a crash and an
    unrelated resource error all satisfy it. An earlier version classified every
    nonzero exit as `export_rejected` and reported green, so the discrimination
    test could itself pass vacuously: the one thing it exists to rule out.

    Both directions are asserted here on purpose. Making unrelated failures red
    is only worth something if the genuine rejection is still green -- otherwise
    the fix could "pass" by failing everything.
    """

    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)
        self.here = Path(self.stack.name)

    def _binary(self, name: str, *, gs_symbols: bool) -> Path:
        path = self.here / name
        body = b"MZ" + b"\x00" * 64
        if gs_symbols:
            body += b"".join(smoke.REQUIRED_TEMPLATE_SYMBOLS)
        path.write_bytes(body)
        return path

    @property
    def _absent(self) -> Path:
        return self.here / "never-written.exe"

    # --- the one failure mode that is allowed to pass ------------------------

    def test_the_expected_missing_template_rejection_passes(self) -> None:
        outcome, detail = smoke.negative_control_outcome(1, self._absent, GENUINE_REJECTION_OUTPUT)
        self.assertEqual(outcome, "export_rejected")
        self.assertIn("could not resolve an export template", detail)
        self.assertEqual(
            smoke._negative_control_verdict(1, self._absent, GENUINE_REJECTION_OUTPUT), 0
        )

    def test_the_rejection_is_recognised_on_a_non_english_runner(self) -> None:
        # The reason line is TTR()-translated; the refusal prefix and the
        # interpolated template path are not. Keying off the English message
        # text would make this control machine-dependent.
        outcome, _ = smoke.negative_control_outcome(
            1, self._absent, GENUINE_REJECTION_OUTPUT_TRANSLATED
        )
        self.assertEqual(outcome, "export_rejected")

    def test_a_stock_binary_is_detected_and_the_control_passes(self) -> None:
        exported = self._binary("stock.exe", gs_symbols=False)
        outcome, detail = smoke.negative_control_outcome(0, exported, "")
        self.assertEqual(outcome, "stock_template_detected")
        self.assertIn("#825", detail)
        self.assertEqual(smoke._negative_control_verdict(0, exported, ""), 0)

    # --- everything else must be red -----------------------------------------

    def test_a_timeout_is_not_a_rejection(self) -> None:
        outcome, detail = smoke.negative_control_outcome(
            smoke.TIMEOUT_RETURNCODE, self._absent, TIMEOUT_OUTPUT, timed_out=True
        )
        self.assertEqual(outcome, "unrelated_failure")
        self.assertIn("TIMED OUT", detail)
        self.assertEqual(
            smoke._negative_control_verdict(
                smoke.TIMEOUT_RETURNCODE, self._absent, TIMEOUT_OUTPUT, timed_out=True
            ),
            smoke.EXIT_FAIL,
        )

    def test_a_timeout_is_still_a_timeout_when_the_output_looks_like_a_rejection(self) -> None:
        # A run that printed the refusal and then hung was still killed, not
        # refused; the flag is authoritative over the text.
        outcome, _ = smoke.negative_control_outcome(
            smoke.TIMEOUT_RETURNCODE, self._absent, GENUINE_REJECTION_OUTPUT, timed_out=True
        )
        self.assertEqual(outcome, "unrelated_failure")

    def test_the_timeout_flag_is_not_inferred_from_the_exit_code(self) -> None:
        # 124 is a legal exit code for a real process. Without the explicit flag
        # a genuine rejection that happened to exit 124 would be misfiled.
        outcome, _ = smoke.negative_control_outcome(
            smoke.TIMEOUT_RETURNCODE, self._absent, GENUINE_REJECTION_OUTPUT
        )
        self.assertEqual(outcome, "export_rejected")

    def test_a_crash_is_not_a_rejection(self) -> None:
        # 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) as Windows reports it.
        outcome, detail = smoke.negative_control_outcome(3221225477, self._absent, "")
        self.assertEqual(outcome, "unrelated_failure")
        self.assertEqual(smoke._negative_control_verdict(3221225477, self._absent, ""), smoke.EXIT_FAIL)
        self.assertIn("no missing-template rejection", detail)

    def test_an_unrelated_resource_error_is_not_a_rejection(self) -> None:
        outcome, _ = smoke.negative_control_outcome(1, self._absent, UNRELATED_RESOURCE_ERROR_OUTPUT)
        self.assertEqual(outcome, "unrelated_failure")

    def test_an_unrelated_configuration_error_is_not_a_rejection(self) -> None:
        # Reaches the same untranslated refusal prefix, so the prefix on its own
        # is not sufficient evidence.
        outcome, _ = smoke.negative_control_outcome(1, self._absent, UNRELATED_CONFIG_ERROR_OUTPUT)
        self.assertEqual(outcome, "unrelated_failure")

    def test_a_template_path_without_a_refusal_is_not_a_rejection(self) -> None:
        # The other half: the path can appear in ordinary chatter, so it is not
        # sufficient either.
        chatter = "Loading templates from C:/Users/runner/AppData/Roaming/Godot/export_templates\n"
        outcome, _ = smoke.negative_control_outcome(1, self._absent, chatter)
        self.assertEqual(outcome, "unrelated_failure")

    def test_a_zero_exit_that_produced_no_binary_establishes_nothing(self) -> None:
        # Previously classified as `export_rejected`: an export that claims
        # success and produces nothing is not a detection of anything.
        outcome, detail = smoke.negative_control_outcome(0, self._absent, "")
        self.assertEqual(outcome, "unrelated_failure")
        self.assertIn("SUCCESS", detail)

    def test_a_gs_binary_from_an_empty_preset_fails_the_control(self) -> None:
        # Nothing in the chain noticed, so the positive run's green says nothing.
        exported = self._binary("gs.exe", gs_symbols=True)
        outcome, _ = smoke.negative_control_outcome(0, exported, "")
        self.assertEqual(outcome, "undetected")
        self.assertEqual(smoke._negative_control_verdict(0, exported, ""), smoke.EXIT_FAIL)

    def test_a_gs_binary_is_still_undetected_when_the_export_also_failed(self) -> None:
        exported = self._binary("gs.exe", gs_symbols=True)
        outcome, _ = smoke.negative_control_outcome(1, exported, GENUINE_REJECTION_OUTPUT)
        self.assertEqual(outcome, "undetected")

    # --- the "no binary was produced" claim must be verified, not inferred ----

    def test_a_failed_export_that_left_a_binary_is_not_reported_as_producing_none(self) -> None:
        exported = self._binary("partial.exe", gs_symbols=False)
        outcome, detail = smoke.negative_control_outcome(1, exported, GENUINE_REJECTION_OUTPUT)
        self.assertEqual(outcome, "unrelated_failure")
        self.assertIn("LEFT A BINARY", detail)
        self.assertNotIn("no binary exists", detail)
        self.assertEqual(
            smoke._negative_control_verdict(1, exported, GENUINE_REJECTION_OUTPUT), smoke.EXIT_FAIL
        )

    def test_the_absence_claim_is_read_off_the_filesystem(self) -> None:
        _, detail = smoke.negative_control_outcome(1, self._absent, GENUINE_REJECTION_OUTPUT)
        self.assertIn("no binary exists", detail)
        self.assertIn("checked on disk", detail)

    # --- structural -----------------------------------------------------------

    def test_every_outcome_is_declared(self) -> None:
        # A new outcome nobody listed must not slip through the verdict.
        exported = self._binary("stock.exe", gs_symbols=False)
        cases = (
            (1, None, GENUINE_REJECTION_OUTPUT),
            (0, exported, ""),
            (0, None, ""),
            (1, None, ""),
        )
        for returncode, path, output in cases:
            with self.subTest(returncode=returncode, output=bool(output)):
                outcome, _ = smoke.negative_control_outcome(returncode, path, output)
                self.assertIn(outcome, smoke.NEGATIVE_CONTROL_OUTCOMES)

    def test_the_verdict_uses_a_pass_list_not_a_fail_list(self) -> None:
        # Pins the inversion: an unrecognised outcome must be red. The previous
        # form (`if outcome == "undetected"`) passed everything else by default.
        self.assertTrue(
            set(smoke.NEGATIVE_CONTROL_PASSING_OUTCOMES).issubset(smoke.NEGATIVE_CONTROL_OUTCOMES)
        )
        self.assertEqual(
            sorted(set(smoke.NEGATIVE_CONTROL_OUTCOMES) - set(smoke.NEGATIVE_CONTROL_PASSING_OUTCOMES)),
            ["undetected", "unrelated_failure"],
        )

    def test_the_evidence_helper_requires_both_halves(self) -> None:
        self.assertEqual(smoke.missing_template_rejection_evidence(""), [])
        self.assertEqual(smoke.missing_template_rejection_evidence(UNRELATED_CONFIG_ERROR_OUTPUT), [])
        evidence = smoke.missing_template_rejection_evidence(GENUINE_REJECTION_OUTPUT)
        self.assertIn(smoke.EXPORT_REJECTION_MARKER, evidence)
        self.assertTrue(any(marker in evidence for marker in smoke.MISSING_TEMPLATE_MARKERS))


class CommandResultTests(unittest.TestCase):
    """`_run` must record a kill, not encode it in the exit code."""

    def test_a_normal_run_is_not_marked_as_timed_out(self) -> None:
        result = smoke._run(
            [sys.executable, "-c", "raise SystemExit(3)"],
            cwd=ROOT,
            timeout=120,
            label="exit 3",
        )
        self.assertEqual(result.returncode, 3)
        self.assertFalse(result.timed_out)

    def test_a_process_that_merely_exits_124_is_not_marked_as_timed_out(self) -> None:
        # The reason the flag exists: 124 is an ordinary exit code.
        result = smoke._run(
            [sys.executable, "-c", f"raise SystemExit({smoke.TIMEOUT_RETURNCODE})"],
            cwd=ROOT,
            timeout=120,
            label="exit 124",
        )
        self.assertEqual(result.returncode, smoke.TIMEOUT_RETURNCODE)
        self.assertFalse(result.timed_out)

    def test_a_killed_process_is_marked_as_timed_out(self) -> None:
        result = smoke._run(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            cwd=ROOT,
            timeout=2,
            label="hang",
        )
        self.assertEqual(result.returncode, smoke.TIMEOUT_RETURNCODE)
        self.assertTrue(result.timed_out)

    def test_combined_output_survives_empty_streams(self) -> None:
        result = smoke.CommandResult(["x"], 0, "", "", timed_out=False)
        self.assertEqual(result.combined_output, "\n")


class NegativeControlFlagTests(unittest.TestCase):
    """The CI step passes this flag by name; argparse has to know it.

    Checked through the real parser rather than by grepping the source: a rename
    would otherwise surface as an argparse error inside a self-hosted CI step
    whose log nobody reads until the lane is already red.
    """

    def test_the_flag_is_registered_on_the_real_parser(self) -> None:
        script = ROOT / "tests" / "runtime" / "run_export_smoke.py"
        result = subprocess.run(
            [sys.executable, str(script), "--help"],
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--expect-stock-template-failure", result.stdout)
        self.assertIn("--require-binaries", result.stdout)

    def test_an_unknown_flag_is_still_rejected(self) -> None:
        # Discrimination for the test above: `--help` printing something is only
        # evidence if a bogus flag does not also sail through. Deliberately NOT a
        # prefix of the real flag -- argparse accepts unambiguous abbreviations,
        # so a near-miss spelling would be accepted and would then start a real
        # multi-minute export from inside a unit test.
        script = ROOT / "tests" / "runtime" / "run_export_smoke.py"
        result = subprocess.run(
            [sys.executable, str(script), "--not-a-real-flag"],
            capture_output=True,
            text=True,
            errors="replace",
            check=False,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
