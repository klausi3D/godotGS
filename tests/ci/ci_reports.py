#!/usr/bin/env python3
"""Step-summary and provenance helpers for the Linux GPU jobs.

These replace the PowerShell blocks the Windows self-hosted jobs used for
reporting. They only *report*: no verdict is decided here, and every function
degrades to an explicit "not found / could not parse" line rather than to
silence, because an absent report is not an empty one.

    ci_reports.py baseline-qa-summary --trigger T --baseline-mode M --baseline-reason R
    ci_reports.py gpu-harness-summary --mode M --reason R
    ci_reports.py runtime-report-summary [--report PATH]
    ci_reports.py provenance --out PATH --baseline-mode M --baseline-reason R --event E

Output goes to ``$GITHUB_STEP_SUMMARY`` when set, otherwise to stdout.
"""

from __future__ import annotations

import argparse
import datetime
import glob
import json
import os
import platform
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional


def _emit(lines: List[str]) -> None:
    text = "\n".join(lines) + "\n"
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    if target:
        with open(target, "a", encoding="utf-8") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)


def _load_json(path: str) -> Optional[Dict[str, object]]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except FileNotFoundError:
        return None
    except (OSError, ValueError) as exc:
        print(f"::warning::Failed to parse {path}: {exc}")
        return None
    return data if isinstance(data, dict) else None


def baseline_qa_summary(args: argparse.Namespace) -> int:
    lines = [
        "# GPU Baseline Test Results",
        "",
        f"- Trigger: `{args.trigger}`",
        f"- Baseline mode: `{args.baseline_mode}` ({args.baseline_reason})",
        "",
    ]
    data = _load_json("baseline_qa_results.json")
    if data is None:
        lines.append("Test results file not found")
        _emit(lines)
        return 0
    total = data.get("total_tests", 0) or 0
    passed = data.get("passed_tests", 0) or 0
    lines += [
        "## Test Summary",
        "| Metric | Value |",
        "|--------|-------|",
        f"| Total Tests | {total} |",
        f"| Passed | {passed} |",
        f"| Failed | {data.get('failed_tests', 0)} |",
    ]
    if total > 0:
        lines.append(f"| Success Rate | {round(passed / total * 100, 1)}% |")
    lines += ["", "## Test Details"]
    for test in data.get("tests", []) or []:
        status = "PASS" if test.get("success") else "FAIL"
        lines.append(f"- {status} **{test.get('name')}** ({round(float(test.get('duration', 0) or 0), 1)}s)")
    _emit(lines)
    return 0


def gpu_harness_summary(args: argparse.Namespace) -> int:
    lines = [
        "## GPU Harness + Visual Gate",
        "",
        f"- Mode: `{args.mode}` ({args.reason})",
    ]
    provenance = _load_json("tests/visual_baselines/.provenance.json")
    if provenance:
        lines.append(f"- GPU: {provenance.get('gpu')}")
        lines.append(f"- Driver: {provenance.get('driver')}")
        lines.append(f"- Runner: {provenance.get('runner_id')}")
    report = _load_json("tests/ci/gpu_harness_report.json")
    if not report:
        lines += [
            "",
            "*No gpu_harness_report.json could be parsed - the harness may have crashed before "
            "writing the report, or the JSON was malformed.*",
        ]
        _emit(lines)
        return 0
    totals = report.get("totals", {}) or {}
    lines += [
        "",
        "| Metric | Value |",
        "|---|---|",
        f"| Test cases | {totals.get('test_cases_passed')}/{totals.get('test_cases_total')} |",
        f"| Assertions | {totals.get('assertions_passed')}/{totals.get('assertions_total')} |",
        f"| Wall seconds | {report.get('wall_seconds')} |",
        f"| Supervisor exit | {report.get('supervisor_exit')} |",
    ]
    # A timed-out batch produces no doctest summary, so its counts parse as 0 and
    # its leak bytes are partial, yet both are summed into the totals above. Flag
    # it next to the numbers so the table is never read as a complete result (#677).
    timed_out = report.get("timed_out_batches") or []
    if isinstance(timed_out, str):
        timed_out = [timed_out]
    if timed_out:
        names = ", ".join(str(name) for name in timed_out)
        print(f"::warning::GPU harness batches timed out: {names} - totals are incomplete")
        lines += [
            "",
            f"> **:warning: Timed-out batches: {names}**",
            "> These batches produced no doctest summary. The Test cases / Assertions",
            "> totals above EXCLUDE them and must not be quoted as complete coverage,",
            "> and any leak figure from this run is a lower bound, not a clean result.",
        ]
    lines += [
        "",
        "### Per-batch breakdown",
        "| Batch | RC | Cases | Asserts | Status | Wall (s) |",
        "|---|---|---|---|---|---|",
    ]
    for batch in report.get("batches", []) or []:
        cases = batch.get("test_cases", {}) or {}
        asserts = batch.get("assertions", {}) or {}
        lines.append(
            f"| {batch.get('name')} | {batch.get('rc')} | {cases.get('passed')}/{cases.get('total')} | "
            f"{asserts.get('passed')}/{asserts.get('total')} | {batch.get('status')} | {batch.get('wall_seconds')} |"
        )
    if (totals.get("test_cases_failed") or 0) > 0 or (totals.get("assertions_failed") or 0) > 0:
        actuals = glob.glob("tests/visual_baselines/*.actual.png")
        if actuals:
            lines += [
                "",
                f"**Visual-diff captures: {len(actuals)}** (see `gpu-harness-visual-diffs` artifact)",
            ]
    _emit(lines)
    return 0


def runtime_report_summary(args: argparse.Namespace) -> int:
    lines = ["## Release-CI Runtime Evidence (nightly, non-blocking)", ""]
    report = _load_json(args.report)
    if report is None:
        lines.append("- No runtime validation report was produced.")
    else:
        renderer_proof = report.get("renderer_proof") or {}
        lines += [
            "| Metric | Value |",
            "|---|---|",
            f"| Profile | {report.get('profile')} |",
            f"| gd_mode | {report.get('gd_mode')} |",
            f"| Total | {report.get('total')} |",
            f"| Passed | {report.get('passed')} |",
            f"| Failed | {report.get('failed')} |",
            f"| Skipped | {report.get('skipped')} |",
            f"| Renderer proof | {renderer_proof.get('status') if isinstance(renderer_proof, dict) else renderer_proof} |",
            f"| Schema valid | {report.get('schema_valid')} |",
        ]
    _emit(lines)
    return 0


def _run_text(command: List[str]) -> str:
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return ""
    return (completed.stdout or "").strip() if completed.returncode == 0 else ""


def provenance(args: argparse.Namespace) -> int:
    gpu, driver = "unknown", "unknown"
    smi = _run_text(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"])
    if smi:
        first = smi.splitlines()[0].split(",")
        gpu = first[0].strip() or gpu
        if len(first) > 1:
            driver = first[1].strip() or driver
    os_build = platform.platform()
    try:
        for row in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if row.startswith("PRETTY_NAME="):
                os_build = f"{row.split('=', 1)[1].strip().strip(chr(34))} / kernel {platform.release()}"
    except OSError:
        pass
    info = {
        "gpu": gpu,
        "driver": driver,
        "runner_id": os.environ.get("RUNNER_NAME", ""),
        "runner_os_build": os_build,
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "baseline_mode": args.baseline_mode,
        "baseline_mode_reason": args.baseline_reason,
        "harness_commit": _run_text(["git", "rev-parse", "HEAD"]),
        "github_run_id": os.environ.get("GITHUB_RUN_ID", ""),
        "github_event": args.event or os.environ.get("GITHUB_EVENT_NAME", ""),
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(info, indent=2), encoding="utf-8")
    print("Provenance:")
    print(out.read_text(encoding="utf-8"))
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("baseline-qa-summary")
    p.add_argument("--trigger", required=True)
    p.add_argument("--baseline-mode", required=True)
    p.add_argument("--baseline-reason", required=True)
    p.set_defaults(func=baseline_qa_summary)

    p = sub.add_parser("gpu-harness-summary")
    p.add_argument("--mode", required=True)
    p.add_argument("--reason", required=True)
    p.set_defaults(func=gpu_harness_summary)

    p = sub.add_parser("runtime-report-summary")
    p.add_argument("--report", default="tests/runtime/runtime_validation_report.json")
    p.set_defaults(func=runtime_report_summary)

    p = sub.add_parser("provenance")
    p.add_argument("--out", required=True)
    p.add_argument("--baseline-mode", required=True)
    p.add_argument("--baseline-reason", required=True)
    p.add_argument("--event", default="")
    p.set_defaults(func=provenance)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
