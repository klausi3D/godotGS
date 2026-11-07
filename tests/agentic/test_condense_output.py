"""Contract tests for `scripts/agentic/condense_output.py`.

The hook rewrites plain SCons builds and the repository's Python test runners so
their output is condensed before it reaches an agent's context window. What must
hold for that to be safe:

* only plain, single commands are rewritten -- never a command that already
  pipes, redirects, chains or substitutes, and never one that opts out;
* the rewritten command keeps the wrapped command's exit status (the filter
  itself exits 0, so without ``pipefail`` every failure would read as success);
* the full output is preserved in a log file whose path is printed;
* a condensed view with no matched lines says it is not evidence of success.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "agentic" / "condense_output.py"
SETTINGS = REPO_ROOT / ".claude" / "settings.json"
# Verbatim producer output, never hand-authored (tests/AGENTS.md). How each was
# captured is recorded on the test that reads it.
FIXTURES = Path(__file__).resolve().parent / "fixtures" / "condense_output"


def _load():
    spec = importlib.util.spec_from_file_location("condense_output", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


condense_output = _load()

# Valid (or at least plausible) commands that start like a build/runner but
# carry a construct the rewrite cannot wrap safely. They must pass through.
UNTOUCHED_SHELL_CONSTRUCTS = (
    "scons -j8 # build",
    "scons -j8 #",
    "python tests/ci/run_module_tests.py --guard-only # guard lane",
    "scons -j8 &",
    "scons -j8 && echo ok",
    "scons -j8 || echo failed",
    "scons -j8; echo done",
    "python tests/ci/run_module_tests.py <<'EOF'\n--guard-only\nEOF",
    "python tests/ci/run_module_tests.py <<<x",
    "scons -j8 \\",
    "scons -j8 \\\n  target=editor",
    'scons "unterminated',
    "scons 'unterminated",
    "scons (x)",
    'scons "suffix=$(date +%s)"',
    'scons "suffix=`date`"',
    "scons -j8\r target=editor",
)


def _bash() -> str | None:
    """A bash that can run the Windows/POSIX python we are running under.

    On Windows, `C:\\Windows\\System32\\bash.exe` is WSL and cannot execute a
    Windows interpreter path, so prefer Git Bash there.
    """
    if os.name == "nt":
        candidates = []
        git = shutil.which("git")
        if git:
            # <git root>/cmd/git.exe -> <git root>/bin/bash.exe
            candidates.append(Path(git).resolve().parent.parent / "bin" / "bash.exe")
        candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe")
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate)
        found = shutil.which("bash")
        if found and not any(part in found.lower() for part in ("system32", "windowsapps")):
            return found
        return None
    return shutil.which("bash")


class ClassifyTests(unittest.TestCase):
    def test_plain_build_and_runner_commands_are_rewritten(self) -> None:
        cases = {
            "scons platform=windows target=editor dev_build=yes tests=yes -j16": "scons",
            "python tests/ci/run_module_tests.py --guard-only": "runner",
            "python3 tests/ci/run_module_tests.py --godot-binary bin/godot.exe": "runner",
            "python tests/runtime/run_runtime_validation.py --profile headless-ci": "runner",
            "python3 -u tests/ci/run_gpu_harness.py --batch CompositorHazard": "runner",
            "python tests\\ci\\run_baseline_qa.py --godot bin/godot.exe": "runner",
        }
        for command, kind in cases.items():
            with self.subTest(command=command):
                self.assertEqual(condense_output.classify(command), kind)

    def test_shaped_compound_or_unrelated_commands_are_left_alone(self) -> None:
        for command in (
            "scons -j16 | tail -n 20",
            "scons -j16 > build.log",
            "scons -j16 2>&1",
            "cd modules && scons",
            "scons; echo done",
            "python tests/ci/run_module_tests.py --guard-only || true",
            "echo $(scons --version)",
            "GS_RAW_OUTPUT=1 scons -j16",
            "python tests/ci/check_require_null_deref.py",
            "python -m unittest discover -s tests/agentic",
            "git status",
            "",
        ):
            with self.subTest(command=command):
                self.assertIsNone(condense_output.classify(command))

    def test_constructs_the_rewrite_cannot_carry_are_left_alone(self) -> None:
        # Codex P2 on #1276: a trailing comment swallowed the appended pipeline
        # and the closing parenthesis. Every construct the scanner does not
        # fully understand must pass through untouched.
        for command in UNTOUCHED_SHELL_CONSTRUCTS:
            with self.subTest(command=command):
                self.assertIsNone(condense_output.classify(command))

    def test_quoted_metacharacters_do_not_block_a_rewrite(self) -> None:
        for command in ('scons "extra_suffix=a#b" -j8', "scons 'CCFLAGS=-DX=1 # y'"):
            with self.subTest(command=command):
                self.assertEqual(condense_output.classify(command), "scons")


class WiringTests(unittest.TestCase):
    def test_claude_settings_wire_the_hook_to_this_script(self) -> None:
        # A hook nobody invokes is decorative: assert the settings point at the
        # script under test, in hook mode, for the Bash tool.
        settings = json.loads(SETTINGS.read_text(encoding="utf-8"))
        commands = [
            hook.get("command", "")
            for entry in settings.get("hooks", {}).get("PreToolUse", [])
            if entry.get("matcher") == "Bash"
            for hook in entry.get("hooks", [])
            if hook.get("type") == "command"
        ]
        wired = [c for c in commands if "scripts/agentic/condense_output.py" in c and " hook" in c]
        self.assertEqual(len(wired), 1, f"expected exactly one Bash PreToolUse hook running the condenser, got {commands}")
        # Exit code 2 from a PreToolUse hook BLOCKS the tool call, and python
        # exits 2 when the script is missing: the wiring must fail open.
        self.assertTrue(wired[0].rstrip().endswith(" hook || exit 0"), wired[0])


class HookTests(unittest.TestCase):
    def _payload(self, command: str, tool: str = "Bash") -> str:
        return json.dumps(
            {
                "hook_event_name": "PreToolUse",
                "tool_name": tool,
                "tool_input": {"command": command, "description": "build", "timeout": 600000},
            }
        )

    def test_rewrite_keeps_every_other_input_field(self) -> None:
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": ""}):
            answer = json.loads(condense_output.run_hook(self._payload("scons -j8")))
        updated = answer["hookSpecificOutput"]["updatedInput"]
        self.assertEqual(answer["hookSpecificOutput"]["hookEventName"], "PreToolUse")
        self.assertNotIn("permissionDecision", answer["hookSpecificOutput"])
        self.assertEqual(updated["description"], "build")
        self.assertEqual(updated["timeout"], 600000)
        self.assertIn("set -o pipefail", updated["command"])
        self.assertIn("scons -j8 2>&1 |", updated["command"])
        self.assertIn("filter --kind scons", updated["command"])

    def test_non_matching_payloads_get_an_empty_answer(self) -> None:
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": ""}):
            self.assertEqual(condense_output.run_hook(self._payload("git status")), "{}")
            self.assertEqual(condense_output.run_hook(self._payload("scons", tool="Read")), "{}")
            self.assertEqual(condense_output.run_hook("not json"), "{}")

    def test_hook_mode_fails_open_on_bad_input(self) -> None:
        # A PreToolUse hook that exits 2 blocks the tool call; hook mode must
        # answer "{}" and exit 0 whatever arrives on stdin.
        for stdin in ("not json", "[]", '{"tool_name": "Bash", "tool_input": []}', ""):
            with self.subTest(stdin=stdin):
                completed = subprocess.run(
                    [sys.executable, str(SCRIPT), "hook"],
                    input=stdin,
                    capture_output=True,
                    text=True,
                    timeout=60,
                    env=dict(os.environ, GS_RAW_OUTPUT=""),
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(completed.stdout.strip(), "{}")

    def test_environment_opt_out_disables_rewriting(self) -> None:
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": "1"}):
            self.assertEqual(condense_output.run_hook(self._payload("scons -j8")), "{}")


class CondenseTests(unittest.TestCase):
    def test_failure_lines_and_tail_are_kept(self) -> None:
        lines = [f"[module-tests] noise {i}" for i in range(500)]
        lines[250] = "[module-tests][lane-result] lane=Foo strict=1 outcome=FAIL passed_tests=1"
        lines.append("[module-tests][lane-ledger] lanes=29 fail_outcomes=1")
        out = "\n".join(condense_output.condense(lines, "runner"))
        self.assertIn("outcome=FAIL", out)
        self.assertIn("[lane-ledger] lanes=29", out)
        self.assertIn("noise 249", out, "one line of context before a match")
        self.assertNotIn("noise 100\n", out + "\n")
        self.assertLess(len(out.splitlines()), 120)

    def test_prefixed_unittest_summaries_are_signals(self) -> None:
        # run_module_tests.py prefixes child output with "[module-tests] ".
        lines = ["[module-tests] ..."] * 200 + [
            "[module-tests] Ran 191 tests in 5.518s",
            "[module-tests] FAILED (failures=1)",
        ] + ["[module-tests] trailing"] * 60
        out = "\n".join(condense_output.condense(lines, "runner"))
        head = out.split("[condense] last", 1)[0]
        self.assertIn("Ran 191 tests", head)
        self.assertIn("FAILED (failures=1)", head)

    def test_no_match_is_not_reported_as_success(self) -> None:
        out = "\n".join(condense_output.condense(["compiling a.cpp", "linking"], "scons"))
        self.assertIn("NOT evidence of success", out)
        self.assertIn("linking", out, "the tail is always shown")


def _fixture(name: str) -> list[str]:
    return (FIXTURES / name).read_text(encoding="utf-8").splitlines()


class RealProducerTests(unittest.TestCase):
    """Condensation asserted against verbatim producer output (Codex P1 on #1276).

    The fixtures are captured, not written:

    * ``scons_linuxbsd_editor_dev_excerpt.txt``: the SCons step of GitHub Actions
      run 41362665136 (job "CPU-Only QA Tests (Linux)", 2026-11-01), fetched with
      ``gh run view 41362665136 --log-failed``. Five contiguous slices of the
      SCons output (banner, two GCC warning blocks, the ``core/error/*.cpp``
      compile lines, the end of the build) with only the Actions
      ``<job>\\t<step>\\t<timestamp> `` prefix removed; ANSI colour bytes kept.
    * ``run_baseline_qa_linux_ci_failure.txt``: the ``run_baseline_qa.py`` report
      of the same run (a real "Module Test Suite" failure), same prefix removal.
    * ``run_module_tests_guard_only_windows.txt``: the complete stdout+stderr of
      ``GS_RAW_OUTPUT=1 python tests/ci/run_module_tests.py --guard-only`` on
      Windows Python 3.11 from the #1276 worktree, 2026-11-07 (exit 0); CRLF
      normalised to LF by ``.gitattributes`` (``eol=lf``), nothing else changed.
    """

    def test_real_scons_output_has_no_false_errors_and_counts_colored_warnings(self) -> None:
        lines = _fixture("scons_linuxbsd_editor_dev_excerpt.txt")
        self.assertTrue(any("\x1b[" in line for line in lines), "fixture must keep the producer's ANSI bytes")
        self.assertTrue(any("core/error/error_list.cpp" in line for line in lines))
        out = condense_output.condense(lines, "scons")
        text = "\n".join(out)
        head = text.split("[condense] last", 1)[0]
        # "Compiling core/error/error_list.cpp ..." is a file name, not an error.
        self.assertIn("no failure/error line matched", head)
        self.assertNotIn("error_list.cpp", head)
        # GCC colours "warning:" with an escape that glued a letter to the word;
        # the warnings must still be counted.
        self.assertIn("warning: unused variable 'fc' [-Wunused-variable]", head)
        self.assertIn("5 warning/ERROR/SKIP line(s)", head)
        self.assertIn("scons: done building targets.", text)
        self.assertNotIn("\x1b", text, "the condensed view is plain text")
        self.assertLess(len(out), len(lines) / 2)

    def test_real_guard_only_output_keeps_every_unittest_summary(self) -> None:
        lines = _fixture("run_module_tests_guard_only_windows.txt")
        summaries = [line for line in lines if " Ran " in line and " tests in " in line]
        self.assertGreater(len(summaries), 10, "fixture must be the real guard lane")
        out = condense_output.condense(lines, "runner")
        head = "\n".join(out).split("[condense] last", 1)[0]
        for summary in summaries:
            self.assertIn(summary, head)
        self.assertIn("[module-tests] Guard-only mode complete.", "\n".join(out))
        self.assertLess(len(out), len(lines))

    def test_real_baseline_qa_failure_is_surfaced(self) -> None:
        lines = _fixture("run_baseline_qa_linux_ci_failure.txt")
        out = condense_output.condense(lines, "runner")
        head = "\n".join(out).split("[condense] last", 1)[0]
        self.assertIn("[FAIL] FAIL Module Test Suite (GaussianSplatting) (79.2s)", head)
        self.assertIn("[FAIL] FAILED (79.2s) - Exit code: 1", head)
        self.assertNotIn("no failure/error line matched", head)

    def test_filter_keeps_the_raw_producer_bytes_in_the_full_log(self) -> None:
        raw = (FIXTURES / "scons_linuxbsd_editor_dev_excerpt.txt").read_bytes()
        with tempfile.TemporaryDirectory() as log_dir:
            completed = subprocess.run(
                [sys.executable, str(SCRIPT), "filter", "--kind", "scons"],
                input=raw,
                capture_output=True,
                timeout=60,
                env=dict(os.environ, GS_AGENT_LOG_DIR=log_dir),
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            logs = list(Path(log_dir).glob("scons-*.log"))
            self.assertEqual(len(logs), 1)
            self.assertEqual(logs[0].read_bytes().replace(b"\r\n", b"\n"), raw.replace(b"\r\n", b"\n"))


@unittest.skipIf(_bash() is None, "bash not available: shell syntax of the rewrite NOT checked")
class ShellSyntaxTests(unittest.TestCase):
    """The hook must never turn a valid command into one bash rejects."""

    def _syntax_ok(self, command: str) -> bool:
        completed = subprocess.run([_bash(), "-n", "-c", command], capture_output=True, text=True, timeout=60)
        return completed.returncode == 0

    def _hook_command(self, command: str) -> str:
        payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": command}})
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": ""}):
            answer = json.loads(condense_output.run_hook(payload))
        if not answer:
            return command
        return answer["hookSpecificOutput"]["updatedInput"]["command"]

    def test_every_rewrite_parses_when_the_original_does(self) -> None:
        corpus = list(UNTOUCHED_SHELL_CONSTRUCTS) + [
            "scons platform=windows target=editor dev_build=yes -j16",
            'scons "extra_suffix=a#b" -j8',
            "scons 'CCFLAGS=-DX=1 # y'",
            "scons foo\\ bar",
            "python tests/ci/run_module_tests.py --guard-only",
            'python3 -u tests/ci/run_gpu_harness.py --batch "A B"',
        ]
        checked = 0
        for command in corpus:
            with self.subTest(command=command):
                if not self._syntax_ok(command):
                    continue
                checked += 1
                rewritten = self._hook_command(command)
                self.assertTrue(self._syntax_ok(rewritten), f"hook broke a valid command: {rewritten!r}")
        self.assertGreater(checked, 10, "the corpus must exercise real, parseable commands")

    def test_trailing_comment_regression(self) -> None:
        # The exact Codex P2 repro: before the fix this rewrite failed `bash -n` with exit 2.
        self.assertTrue(self._syntax_ok("scons -j8 # build"))
        self.assertEqual(self._hook_command("scons -j8 # build"), "scons -j8 # build")

    def test_settings_hook_command_fails_open_when_the_script_is_missing(self) -> None:
        settings = json.loads(SETTINGS.read_text(encoding="utf-8"))
        hook_command = settings["hooks"]["PreToolUse"][0]["hooks"][0]["command"]
        payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": "scons -j8"}})
        with tempfile.TemporaryDirectory() as missing_root:
            completed = subprocess.run(
                [_bash(), "-c", hook_command],
                input=payload,
                capture_output=True,
                text=True,
                timeout=60,
                env=dict(os.environ, CLAUDE_PROJECT_DIR=missing_root),
            )
        self.assertEqual(completed.returncode, 0, "exit 2 would block every Bash call: " + completed.stderr)
        self.assertEqual(completed.stdout.strip(), "")


@unittest.skipIf(_bash() is None, "bash not available: exit-status passthrough NOT checked")
class PassthroughTests(unittest.TestCase):
    def _run(self, inner: str) -> subprocess.CompletedProcess[str]:
        command = condense_output.rewrite(inner, "runner")
        with tempfile.TemporaryDirectory() as log_dir:
            env = dict(os.environ, GS_AGENT_LOG_DIR=log_dir)
            completed = subprocess.run(
                [_bash(), "-c", command],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=env,
                timeout=120,
            )
            logs = list(Path(log_dir).glob("runner-*.log"))
            self.assertEqual(len(logs), 1, "exactly one full log must be written")
            full = logs[0].read_text(encoding="utf-8")
        completed.full_log = full  # type: ignore[attr-defined]
        return completed

    def _python(self) -> str:
        return '"' + sys.executable.replace("\\", "/") + '"'

    def test_failing_command_keeps_its_exit_status(self) -> None:
        inner = self._python() + " -c \"import sys; print('case X FAILED'); sys.exit(3)\""
        completed = self._run(inner)
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        self.assertIn("case X FAILED", completed.stdout)
        self.assertIn("full log:", completed.stdout)
        self.assertIn("case X FAILED", completed.full_log)  # type: ignore[attr-defined]

    def test_passing_command_exits_zero_and_keeps_full_output(self) -> None:
        inner = self._python() + " -c \"[print('line', i) for i in range(300)]\""
        completed = self._run(inner)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(completed.full_log.count("\n"), 300)  # type: ignore[attr-defined]
        self.assertLess(len(completed.stdout.splitlines()), 80)


if __name__ == "__main__":
    unittest.main()
