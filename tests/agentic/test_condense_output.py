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


def _load():
    spec = importlib.util.spec_from_file_location("condense_output", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


condense_output = _load()


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
        wired = [c for c in commands if "scripts/agentic/condense_output.py" in c and c.rstrip().endswith(" hook")]
        self.assertEqual(len(wired), 1, f"expected exactly one Bash PreToolUse hook running the condenser, got {commands}")


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
