"""Contract tests for the OpenAI Codex side of the agent setup.

What must hold:

* `.codex/hooks.json` wires a Bash `PreToolUse` hook to
  `scripts/agentic/condense_output.py hook --codex`, and the configured command
  really produces a Codex-shaped answer when run from a repo subdirectory;
* the Codex answer pairs `updatedInput` with `permissionDecision: "allow"`
  (Codex ignores/rejects `updatedInput` otherwise);
* the portable rewrite survives any shell untouched and `run` keeps the wrapped
  command's exit status and full output;
* `.agents/skills` (Codex) is byte-identical to `.claude/skills` (Claude Code),
  so the two agents never drift apart;
* `.codex/agents/*.toml` parse and carry the fields Codex requires.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "agentic" / "condense_output.py"
HOOKS = REPO_ROOT / ".codex" / "hooks.json"
CLAUDE_SKILLS = REPO_ROOT / ".claude" / "skills"
CODEX_SKILLS = REPO_ROOT / ".agents" / "skills"
CODEX_AGENTS = REPO_ROOT / ".codex" / "agents"


def _load():
    spec = importlib.util.spec_from_file_location("condense_output", SCRIPT)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


condense_output = _load()


def _payload(command: str) -> str:
    return json.dumps(
        {
            "hook_event_name": "PreToolUse",
            "tool_name": "Bash",
            "tool_input": {"command": command},
        }
    )


def _hook_commands() -> list[dict]:
    config = json.loads(HOOKS.read_text(encoding="utf-8"))
    return [
        hook
        for entry in config.get("hooks", {}).get("PreToolUse", [])
        if re.fullmatch(entry.get("matcher", ""), "Bash")
        for hook in entry.get("hooks", [])
        if hook.get("type") == "command"
    ]


class CodexHookProtocolTests(unittest.TestCase):
    def test_codex_answer_allows_with_updated_input(self) -> None:
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": ""}):
            answer = json.loads(
                condense_output.run_hook(_payload("scons -j8"), codex=True)
            )["hookSpecificOutput"]
        self.assertEqual(answer["hookEventName"], "PreToolUse")
        self.assertEqual(answer["permissionDecision"], "allow")
        self.assertIsInstance(answer["updatedInput"]["command"], str)
        self.assertIn(" run --kind scons --hex ", answer["updatedInput"]["command"])

    def test_codex_leaves_unrelated_commands_alone(self) -> None:
        with mock.patch.dict(os.environ, {"GS_RAW_OUTPUT": ""}):
            self.assertEqual(condense_output.run_hook(_payload("git status"), codex=True), "{}")
            self.assertEqual(condense_output.run_hook(_payload("scons | tail"), codex=True), "{}")

    def test_portable_rewrite_has_no_shell_specific_syntax(self) -> None:
        nasty = "python tests/ci/run_module_tests.py --godot-binary 'C:/a b/$x`y\".exe'"
        rewritten = condense_output.rewrite_portable(nasty, "runner")
        prefix, encoded = rewritten.rsplit(" ", 1)
        self.assertEqual(bytes.fromhex(encoded).decode("utf-8"), nasty)
        tail = prefix.split('" ', 1)[1]
        self.assertRegex(tail, r"^[a-z0-9 -]+$", "only shell-neutral characters after the script path")


class CodexRunModeTests(unittest.TestCase):
    def _run(self, inner: str) -> tuple[subprocess.CompletedProcess[str], str]:
        with tempfile.TemporaryDirectory() as log_dir:
            completed = subprocess.run(
                [sys.executable, str(SCRIPT), "run", "--kind", "runner", "--hex", inner.encode().hex()],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                env=dict(os.environ, GS_AGENT_LOG_DIR=log_dir),
                timeout=120,
            )
            logs = list(Path(log_dir).glob("runner-*.log"))
            self.assertEqual(len(logs), 1, "exactly one full log must be written")
            return completed, logs[0].read_text(encoding="utf-8")

    def _python(self) -> str:
        return '"' + sys.executable + '"'

    def test_failing_command_keeps_its_exit_status(self) -> None:
        completed, full = self._run(self._python() + " -c \"import sys; print('case X FAILED'); sys.exit(3)\"")
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        self.assertIn("case X FAILED", completed.stdout)
        self.assertIn("exit status: 3", completed.stdout)
        self.assertIn("case X FAILED", full)

    def test_passing_command_exits_zero_and_keeps_full_output(self) -> None:
        completed, full = self._run(self._python() + " -c \"[print('line', i) for i in range(300)]\"")
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(full.count("\n"), 300)
        self.assertLess(len(completed.stdout.splitlines()), 80)


class CodexHookWiringTests(unittest.TestCase):
    def test_exactly_one_condense_hook_is_wired(self) -> None:
        hooks = _hook_commands()
        wired = [
            h for h in hooks
            if "scripts/agentic/condense_output.py" in h.get("command", "")
            and "'hook','--codex'" in h.get("command", "")
        ]
        self.assertEqual(len(wired), 1, f"expected one Codex condense hook, got {hooks}")
        self.assertIn("'hook','--codex'", wired[0].get("commandWindows", ""))

    def test_configured_command_answers_from_a_subdirectory(self) -> None:
        # Execute the hook exactly as configured (Codex runs it in the session
        # cwd, which may be a subdirectory), so a broken bootstrap is RED here.
        hook = next(h for h in _hook_commands() if "condense_output.py" in h.get("command", ""))
        command = hook.get("commandWindows") if os.name == "nt" else hook["command"]
        interpreter = command.split(" ", 1)[0]
        if shutil.which(interpreter) is None:
            self.skipTest(f"{interpreter} not on PATH: configured hook command NOT executed")
        completed = subprocess.run(
            command,
            shell=True,
            input=_payload("python tests/ci/run_module_tests.py --guard-only"),
            capture_output=True,
            text=True,
            cwd=REPO_ROOT / "tests" / "agentic",
            env=dict(os.environ, GS_RAW_OUTPUT=""),
            timeout=60,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        answer = json.loads(completed.stdout)["hookSpecificOutput"]
        self.assertEqual(answer["permissionDecision"], "allow")
        self.assertIn(" run --kind runner --hex ", answer["updatedInput"]["command"])


class CodexSkillsAndAgentsTests(unittest.TestCase):
    def _tree(self, root: Path) -> dict[str, bytes]:
        return {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }

    def test_codex_skills_mirror_claude_skills(self) -> None:
        claude, codex = self._tree(CLAUDE_SKILLS), self._tree(CODEX_SKILLS)
        self.assertTrue(claude, ".claude/skills is empty")
        self.assertEqual(
            sorted(claude), sorted(codex),
            "skill file sets differ; copy .claude/skills/* to .agents/skills/",
        )
        for rel in claude:
            with self.subTest(file=rel):
                self.assertEqual(claude[rel], codex[rel], f"{rel} drifted between .claude and .agents")

    def test_skill_frontmatter_meets_the_agent_skills_spec(self) -> None:
        for skill in sorted(CODEX_SKILLS.glob("*/SKILL.md")):
            with self.subTest(skill=skill.parent.name):
                text = skill.read_text(encoding="utf-8")
                match = re.match(r"^---\n(.*?)\n---\n", text, re.S)
                self.assertIsNotNone(match, "missing frontmatter")
                fields = dict(
                    line.split(":", 1) for line in match.group(1).splitlines() if ":" in line
                )
                name = fields.get("name", "").strip()
                self.assertEqual(name, skill.parent.name)
                self.assertRegex(name, r"^[a-z0-9-]{1,64}$")
                self.assertTrue(0 < len(fields.get("description", "").strip()) <= 1024)

    def test_codex_agents_parse_with_required_fields(self) -> None:
        files = sorted(CODEX_AGENTS.glob("*.toml"))
        self.assertTrue(files, "no Codex custom agents found")
        names = set()
        for path in files:
            with self.subTest(agent=path.name):
                data = tomllib.loads(path.read_text(encoding="utf-8"))
                for key in ("name", "description", "developer_instructions"):
                    self.assertTrue(str(data.get(key, "")).strip(), f"{key} missing")
                self.assertNotIn("sandbox_mode", data, "per-agent sandbox is not honoured by Codex")
                names.add(data["name"])
        self.assertEqual(names, {"gs_explorer", "gs_verifier"})


if __name__ == "__main__":
    unittest.main()
