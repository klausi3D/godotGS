#!/usr/bin/env python3
"""Keep verbose build/test output out of an agent's context window.

Agent-neutral tooling: any agent (or human) can use the ``filter`` mode; the
``hook`` mode speaks the Claude Code ``PreToolUse`` protocol.

Two modes:

``hook``
    ``PreToolUse`` hook for the Bash tool (wired in ``.claude/settings.json``).
    Reads the hook JSON on stdin. If the command is a
    plain SCons build or one of the repository's Python test runners, it is
    rewritten to::

        ( set -o pipefail; <command> 2>&1 | python condense_output.py filter --kind <kind> )

    Anything else is left untouched (the hook prints ``{}``). Commands that
    already pipe, redirect, chain or substitute are never rewritten, and neither
    is a command prefixed with ``GS_RAW_OUTPUT=1`` (per-command opt-out) or any
    command while ``GS_RAW_OUTPUT=1`` is set in Claude Code's environment.

``filter --kind {scons,runner}``
    Reads the combined output on stdin, writes ALL of it to a log file under
    ``tests/output/agent_logs/`` (gitignored; override with ``GS_AGENT_LOG_DIR``),
    and prints a condensed view: the
    failure/error lines with a little context, a count of the weaker signals
    (warnings, engine ``ERROR:`` prints), and the last lines of the run. The full
    log path is always printed so nothing is lost.

``hook --codex``
    The same, in the Codex ``PreToolUse`` protocol (wired in
    ``.codex/hooks.json``): Codex applies ``updatedInput`` only with
    ``permissionDecision: "allow"``, and its shell may be PowerShell, so the
    command is rewritten to the shell-agnostic ``run`` form below.

``run --kind {scons,runner} --hex <hex>``
    Decodes the command, runs it (``shell=True``), condenses its combined output
    like ``filter`` and exits with the command's own exit status.

What the condensed view does NOT claim: a view with no matched lines is not
evidence of success. The exit status of the wrapped command is the verdict;
``pipefail`` passes it through unchanged, because this filter itself exits 0.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LOG_DIR = ROOT / "tests" / "output" / "agent_logs"

# Shell metacharacters that mean "the author already shaped the output or the
# command is compound". Rewriting those could change their meaning, so we don't.
_UNSAFE = re.compile(r"[|<>;&`\n\r]|\$\(")

_KINDS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("scons", re.compile(r"^\s*scons(?:\.bat|\.exe)?(?:\s|$)")),
    (
        "runner",
        re.compile(
            r"^\s*(?:python3?|py)(?:\.exe)?\s+(?:-u\s+)?"
            r"(?:\./)?tests[/\\](?:ci|runtime)[/\\]run_[A-Za-z0-9_]+\.py(?:\s|$)"
        ),
    ),
)

# Strong signals: always shown, with context.
_STRONG = {
    "scons": re.compile(
        r"\berror\b|\bError\b|fatal error|\bLNK\d{4}\b|undefined reference|"
        r"scons: \*\*\*|scons: building terminated|Traceback \(most recent call last\)"
    ),
    "runner": re.compile(
        r"\bFAIL(?:ED)?\b|ADVISORY-RED|NOT-RUN|UNAVAILABLE|QUARANTINE-REJECTED|"
        r"Traceback \(most recent call last\)|AssertionError|\[lane-ledger\]|"
        r"\[doctest\] (?:test cases|assertions)|(?:^|\]\s)Ran \d+ tests?|"
        r"(?:^|\]\s)(?:OK|FAILED)\b|"
        r"RESULT:|\[RUNTIME_FAIL\]|SCRIPT ERROR"
    ),
}
# Weak signals: counted, a few unique samples shown.
_WEAK = re.compile(r"\bwarning\b|\bWARNING\b|\bERROR\b|\bSKIP(?:PED)?\b", re.IGNORECASE)

MAX_STRONG_LINES = 160
CONTEXT = 1
WEAK_SAMPLES = 15
TAIL = {"scons": 15, "runner": 40}


def classify(command: str) -> str | None:
    """Return the filter kind for ``command``, or None when it must not be rewritten."""
    stripped = command.strip()
    if not stripped or stripped.startswith("GS_RAW_OUTPUT=1"):
        return None
    if _UNSAFE.search(stripped):
        return None
    for kind, pattern in _KINDS:
        if pattern.match(stripped):
            return kind
    return None


def _posix(path: str) -> str:
    return path.replace("\\", "/")


def rewrite(command: str, kind: str) -> str:
    python = _posix(sys.executable or "python")
    script = _posix(str(Path(__file__).resolve()))
    return (
        f'( set -o pipefail; {command.strip()} 2>&1 | "{python}" "{script}" '
        f"filter --kind {kind} )"
    )


def rewrite_portable(command: str, kind: str) -> str:
    """Shell-agnostic rewrite for agents whose shell may be PowerShell, cmd or sh.

    The original command travels hex-encoded, so no shell can re-quote or
    re-interpret it; ``run`` decodes it and executes it itself. Only a bare
    interpreter name, one double-quoted path and ``[a-z0-9 -]`` remain, which
    parse identically in bash, PowerShell and cmd.
    """
    interpreter = "python" if os.name == "nt" else "python3"
    script = _posix(str(Path(__file__).resolve()))
    encoded = command.strip().encode("utf-8").hex()
    return f'{interpreter} "{script}" run --kind {kind} --hex {encoded}'


def run_hook(stdin_text: str, codex: bool = False) -> str:
    """Return the hook's JSON answer for one PreToolUse payload.

    ``codex=True`` speaks the Codex protocol: Codex applies ``updatedInput`` only
    together with ``permissionDecision: "allow"``, and its shell may be
    PowerShell, so the portable rewrite is used.
    """
    if os.environ.get("GS_RAW_OUTPUT") == "1":
        return "{}"
    try:
        payload = json.loads(stdin_text)
    except json.JSONDecodeError:
        return "{}"
    if payload.get("tool_name") != "Bash":
        return "{}"
    tool_input = payload.get("tool_input") or {}
    command = tool_input.get("command")
    if not isinstance(command, str):
        return "{}"
    kind = classify(command)
    if kind is None:
        return "{}"
    updated = dict(tool_input)
    if codex:
        updated["command"] = rewrite_portable(command, kind)
        return json.dumps(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "permissionDecision": "allow",
                    "permissionDecisionReason": "condense build/test output (scripts/agentic/condense_output.py)",
                    "updatedInput": updated,
                }
            }
        )
    updated["command"] = rewrite(command, kind)
    return json.dumps(
        {"hookSpecificOutput": {"hookEventName": "PreToolUse", "updatedInput": updated}}
    )


def condense(lines: list[str], kind: str) -> list[str]:
    strong = _STRONG[kind]
    keep: set[int] = set()
    for index, line in enumerate(lines):
        if strong.search(line):
            keep.update(range(max(0, index - CONTEXT), min(len(lines), index + CONTEXT + 1)))
    out: list[str] = []
    selected = sorted(keep)
    if selected:
        out.append(f"[condense] {len(selected)} line(s) around failure/error signals:")
        previous = -2
        for shown, index in enumerate(selected):
            if shown >= MAX_STRONG_LINES:
                out.append(f"[condense] ... {len(selected) - shown} more matched line(s) in the full log")
                break
            if index != previous + 1:
                out.append("  ...")
            out.append(lines[index])
            previous = index
    else:
        out.append(
            "[condense] no failure/error line matched. That is NOT evidence of success: "
            "the exit status is the verdict."
        )

    weak_counts: dict[str, int] = {}
    for index, line in enumerate(lines):
        if index in keep or not _WEAK.search(line):
            continue
        key = line.strip()[:200]
        weak_counts[key] = weak_counts.get(key, 0) + 1
    if weak_counts:
        total = sum(weak_counts.values())
        out.append(
            f"[condense] {total} warning/ERROR/SKIP line(s), {len(weak_counts)} unique; first samples:"
        )
        for key, count in list(weak_counts.items())[:WEAK_SAMPLES]:
            out.append(f"  ({count}x) {key}")

    tail = TAIL[kind]
    out.append(f"[condense] last {min(tail, len(lines))} line(s):")
    out.extend(lines[-tail:])
    return out


def _condense_stream(kind: str, stream, ran: str | None = None) -> None:
    """Log every line of ``stream`` to a file and print the condensed view."""
    log_dir = Path(os.environ.get("GS_AGENT_LOG_DIR") or DEFAULT_LOG_DIR)
    log_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = log_dir / f"{kind}-{stamp}-{os.getpid()}.log"
    lines: list[str] = []
    with open(log_path, "w", encoding="utf-8", newline="\n") as log:
        for raw in stream:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            log.write(line + "\n")
            lines.append(line)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if ran is not None:
        print(f"[condense] ran: {ran}")
    header = (
        f"[condense] {len(lines)} line(s) of {kind} output; full log: {_posix(str(log_path))} "
        f"(set GS_RAW_OUTPUT=1 for raw output)"
    )
    print(header)
    print("\n".join(condense(lines, kind)))
    sys.stdout.flush()


def run_filter(kind: str) -> int:
    _condense_stream(kind, sys.stdin.buffer)
    return 0


def run_command(kind: str, encoded: str) -> int:
    """Run a hex-encoded command, condense its output, return ITS exit status."""
    import subprocess

    command = bytes.fromhex(encoded).decode("utf-8")
    process = subprocess.Popen(
        command,
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
    )
    assert process.stdout is not None
    _condense_stream(kind, process.stdout, ran=command)
    code = process.wait()
    print(f"[condense] exit status: {code}")
    if code < 0:  # POSIX signal
        return 128 - code
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    hook = sub.add_parser("hook", help="PreToolUse hook (JSON on stdin).")
    hook.add_argument("--codex", action="store_true", help="Speak the Codex hook protocol.")
    filt = sub.add_parser("filter", help="Condense output read on stdin.")
    filt.add_argument("--kind", choices=sorted(_STRONG), required=True)
    run = sub.add_parser("run", help="Run a hex-encoded command and condense its output.")
    run.add_argument("--kind", choices=sorted(_STRONG), required=True)
    run.add_argument("--hex", required=True)
    args = parser.parse_args(argv)
    if args.mode == "hook":
        try:
            answer = run_hook(sys.stdin.read(), codex=args.codex)
        except Exception:  # never block or break the agent's command
            answer = "{}"
        print(answer)
        return 0
    if args.mode == "run":
        return run_command(args.kind, args.hex)
    return run_filter(args.kind)


if __name__ == "__main__":
    sys.exit(main())
