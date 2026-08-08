#!/usr/bin/env python3
"""Unit tests for tests/ci/resolve_export_template.py (#825).

Why this file exists
--------------------
The first version of `build_windows_export_template` resolved its binaries
inline with `Get-ChildItem -Filter 'godot.windows.template_release*.x86_64.exe'`
and then required exactly one `*.console.exe` among the matches. SConstruct
appends `.console` AFTER the architecture, so the console wrapper is named
`godot.windows.template_release.x86_64.console.exe` and does NOT end in
`.x86_64.exe`. The filter therefore returned the main exe only, the console
count was always zero, and the job threw on every single run -- no Windows
export-template artifact could ever be uploaded.

The downstream check was correct; its input was wrong. So the tests below feed
the resolver the file names SConstruct actually emits, verbatim, rather than a
shape invented for the test.
"""

from __future__ import annotations

import fnmatch
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from resolve_export_template import (  # noqa: E402
    ResolutionError,
    expected_console_template_name,
    expected_template_name,
    is_built_console_wrapper,
    is_built_main_binary,
    resolve,
)

# Verbatim from `ls bin/` after `scons platform=windows target=template_release
# arch=x86_64` in this repository. Do not "tidy" these strings.
REAL_WINDOWS_BIN_FILES = (
    "godot.windows.template_release.x86_64.console.exe",
    "godot.windows.template_release.x86_64.exe",
    "godot.windows.template_release.x86_64.exp",
    "godot.windows.template_release.x86_64.lib",
)
REAL_WINDOWS_MAIN = "godot.windows.template_release.x86_64.exe"
REAL_WINDOWS_CONSOLE = "godot.windows.template_release.x86_64.console.exe"
REAL_LINUX_MAIN = "godot.linuxbsd.template_release.x86_64"

# The glob the broken job used. Kept as a literal so the regression is named.
BROKEN_INLINE_GLOB = "godot.windows.template_release*.x86_64.exe"


def _make_bin_dir(stack: tempfile.TemporaryDirectory, names) -> Path:
    bin_dir = Path(stack.name) / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    for name in names:
        (bin_dir / name).write_bytes(b"stub")
    return bin_dir


class RealWindowsBuildOutputTests(unittest.TestCase):
    """A fixture directory holding exactly the real SConstruct output names."""

    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)
        self.bin_dir = _make_bin_dir(self.stack, REAL_WINDOWS_BIN_FILES)

    def test_resolves_one_main_and_one_console(self) -> None:
        resolved = resolve(self.bin_dir, "windows", "template_release", "x86_64")
        self.assertEqual(resolved.main_name, REAL_WINDOWS_MAIN)
        self.assertEqual(resolved.console_name, REAL_WINDOWS_CONSOLE)
        # Disjoint: the two must never be the same file.
        self.assertNotEqual(resolved.main_path, resolved.console_path)

    def test_staged_names_are_what_godot_looks_up(self) -> None:
        resolved = resolve(self.bin_dir, "windows", "template_release", "x86_64")
        self.assertEqual(resolved.template_name, "windows_release_x86_64.exe")
        self.assertEqual(resolved.console_template_name, "windows_release_x86_64.console.exe")

    def test_console_wrapper_predicate_matches_the_real_name(self) -> None:
        self.assertTrue(
            is_built_console_wrapper(REAL_WINDOWS_CONSOLE, "windows", "template_release", "x86_64")
        )
        self.assertFalse(
            is_built_console_wrapper(REAL_WINDOWS_MAIN, "windows", "template_release", "x86_64")
        )

    def test_main_predicate_never_returns_the_console_wrapper(self) -> None:
        self.assertTrue(is_built_main_binary(REAL_WINDOWS_MAIN, "windows", "template_release", "x86_64"))
        self.assertFalse(
            is_built_main_binary(REAL_WINDOWS_CONSOLE, "windows", "template_release", "x86_64")
        )

    def test_the_original_inline_glob_could_not_see_the_console_wrapper(self) -> None:
        """Pins the exact defect: this is why the Windows job never uploaded.

        If this ever starts passing, SConstruct changed its naming and the
        resolver's assumptions need rechecking.
        """
        self.assertTrue(fnmatch.fnmatch(REAL_WINDOWS_MAIN, BROKEN_INLINE_GLOB))
        self.assertFalse(fnmatch.fnmatch(REAL_WINDOWS_CONSOLE, BROKEN_INLINE_GLOB))

    def test_sidecar_artifacts_are_ignored(self) -> None:
        resolved = resolve(self.bin_dir, "windows", "template_release", "x86_64")
        for suffix in (".exp", ".lib"):
            self.assertFalse(resolved.main_name.endswith(suffix))
            self.assertFalse((resolved.console_name or "").endswith(suffix))


class WindowsFailClosedTests(unittest.TestCase):
    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)

    def test_missing_console_wrapper_is_an_error(self) -> None:
        bin_dir = _make_bin_dir(self.stack, (REAL_WINDOWS_MAIN,))
        with self.assertRaises(ResolutionError) as ctx:
            resolve(bin_dir, "windows", "template_release", "x86_64")
        self.assertIn("console wrapper", str(ctx.exception))

    def test_missing_main_binary_is_an_error(self) -> None:
        bin_dir = _make_bin_dir(self.stack, (REAL_WINDOWS_CONSOLE,))
        with self.assertRaises(ResolutionError):
            resolve(bin_dir, "windows", "template_release", "x86_64")

    def test_ambiguous_main_binary_is_an_error(self) -> None:
        bin_dir = _make_bin_dir(
            self.stack,
            (
                REAL_WINDOWS_MAIN,
                REAL_WINDOWS_CONSOLE,
                "godot.windows.template_release.dev.x86_64.exe",
            ),
        )
        with self.assertRaises(ResolutionError) as ctx:
            resolve(bin_dir, "windows", "template_release", "x86_64")
        self.assertIn("found 2", str(ctx.exception))

    def test_editor_build_is_not_mistaken_for_a_template(self) -> None:
        bin_dir = _make_bin_dir(
            self.stack,
            ("godot.windows.editor.x86_64.exe", "godot.windows.editor.x86_64.console.exe"),
        )
        with self.assertRaises(ResolutionError):
            resolve(bin_dir, "windows", "template_release", "x86_64")

    def test_missing_bin_dir_is_an_error(self) -> None:
        with self.assertRaises(ResolutionError):
            resolve(Path(self.stack.name) / "nope", "windows", "template_release", "x86_64")


class LinuxBuildOutputTests(unittest.TestCase):
    def setUp(self) -> None:
        self.stack = tempfile.TemporaryDirectory()
        self.addCleanup(self.stack.cleanup)

    def test_resolves_main_binary_and_expects_no_console_wrapper(self) -> None:
        bin_dir = _make_bin_dir(self.stack, (REAL_LINUX_MAIN,))
        resolved = resolve(bin_dir, "linuxbsd", "template_release", "x86_64")
        self.assertEqual(resolved.main_name, REAL_LINUX_MAIN)
        self.assertIsNone(resolved.console_path)
        self.assertIsNone(resolved.console_template_name)

    def test_staged_name_uses_a_dot_before_the_architecture(self) -> None:
        self.assertEqual(
            expected_template_name("linuxbsd", "template_release", "x86_64"), "linux_release.x86_64"
        )

    def test_ambiguous_main_binary_is_an_error(self) -> None:
        bin_dir = _make_bin_dir(
            self.stack, (REAL_LINUX_MAIN, "godot.linuxbsd.template_release.dev.x86_64")
        )
        with self.assertRaises(ResolutionError):
            resolve(bin_dir, "linuxbsd", "template_release", "x86_64")


class ExpectedNameFormulaTests(unittest.TestCase):
    """Keep the Python mirror honest against Godot's export plugins."""

    def test_windows_and_linux_separators_differ(self) -> None:
        self.assertEqual(
            expected_template_name("windows", "template_release", "x86_64"), "windows_release_x86_64.exe"
        )
        self.assertEqual(
            expected_template_name("linuxbsd", "template_release", "x86_64"), "linux_release.x86_64"
        )

    def test_debug_target_names(self) -> None:
        self.assertEqual(
            expected_template_name("windows", "template_debug", "x86_64"), "windows_debug_x86_64.exe"
        )
        self.assertEqual(
            expected_console_template_name("windows", "template_debug", "x86_64"),
            "windows_debug_x86_64.console.exe",
        )
        self.assertEqual(
            expected_template_name("linuxbsd", "template_debug", "x86_64"), "linux_debug.x86_64"
        )

    def test_formula_matches_the_engine_source(self) -> None:
        """If upstream changes get_template_file_name(), fail here, not in CI."""
        windows_src = (ROOT / "platform/windows/export/export_plugin.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        linux_src = (ROOT / "platform/linuxbsd/export/export_plugin.cpp").read_text(
            encoding="utf-8", errors="replace"
        )
        self.assertIn('return "windows_" + p_target + "_" + p_arch + ".exe";', windows_src)
        self.assertIn('return "linux_" + p_target + "." + p_arch;', linux_src)
        # The console wrapper Godot resolves next to a template.
        self.assertIn('.get_basename() + ".console.exe"', windows_src)

    def test_non_template_scons_target_is_rejected(self) -> None:
        with self.assertRaises(ResolutionError):
            expected_template_name("windows", "editor", "x86_64")


class RealBinDirectoryTests(unittest.TestCase):
    """Bonus leg: when a real template build is present, resolve it for real.

    Skips when `bin/` holds no template build. Every naming assertion above runs
    unconditionally against the fixture, so this skipping leg adds evidence and
    never carries the contract on its own.
    """

    def test_real_bin_directory_resolves_when_present(self) -> None:
        bin_dir = ROOT / "bin"
        if not bin_dir.is_dir():
            self.skipTest("no bin/ directory in this checkout")
        candidates = [
            p.name
            for p in bin_dir.iterdir()
            if p.is_file() and is_built_main_binary(p.name, "windows", "template_release", "x86_64")
        ]
        if not candidates:
            self.skipTest("no Windows template_release build in bin/")
        resolved = resolve(bin_dir, "windows", "template_release", "x86_64")
        self.assertTrue(resolved.main_path.is_file())
        self.assertIsNotNone(resolved.console_path)
        self.assertTrue(resolved.console_path.is_file())


if __name__ == "__main__":
    unittest.main(verbosity=2)
