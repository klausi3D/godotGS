#!/usr/bin/env python3
"""Produce/verify complete exact-commit evidence. No risk/path/label exemptions."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
REQUIRED_STEPS = ("build", "capture", "pipeline", "module", "headless", "streaming", "postflight")
UNIVERSAL_ARTIFACT_FILES = ("pr-gpu-evidence.json", "module-lanes.json", "headless-report.json", "streaming-report.json")
REPORT_SOURCES = {
    "module-lanes.json": "artifacts/module-lanes.json",
    "headless-report.json": "tests/runtime/runtime_validation_report_headless_ci.json",
    "streaming-report.json": "tests/runtime/runtime_validation_report_streaming_gpu_ci.json",
}


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


policy = load_module("gpu_commit_policy", ROOT / "scripts/agentic/gpu_commit_policy.py")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True,
                          capture_output=True, encoding="utf-8").stdout.strip()


def file_hash(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def binary_components(binary: Path) -> dict[str, str]:
    """The Windows console launcher executes its companion editor executable."""
    paths = [binary.resolve()]
    if str(paths[0]).lower().endswith(".console.exe"):
        paths.append(Path(str(paths[0])[:-len(".console.exe")] + ".exe"))
    return {str(path): file_hash(path) for path in paths}


def current_identity() -> tuple[str, str]:
    head = policy.require_sha(os.environ.get("GS_EVIDENCE_HEAD_SHA"))
    if git("rev-parse", "HEAD") != head:
        raise ValueError("Measured checkout must be the exact proposed commit, not a synthetic alias")
    git("diff", "--quiet")
    git("diff", "--cached", "--quiet")
    return head, policy.context_for(policy.policy_bundle(head))


def start_measurement(path: Path, binary: Path) -> None:
    head, context = current_identity()
    for relative in REPORT_SOURCES.values():
        target = ROOT / relative
        if target.exists():
            target.unlink()
    started = datetime.now(timezone.utc).isoformat()
    state = {"head_sha": head, "policy_context": context, "binary_path": str(binary.resolve()),
             "binary_sha256": file_hash(binary), "binary_components_sha256": binary_components(binary),
             "started_utc": started}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")


def validate_module_ledger(report: dict) -> None:
    runner = load_module("universal_module_runner", ROOT / "tests/ci/run_module_tests.py")
    expected = runner._build_module_test_runs(True)
    if report.get("schema_version") != runner.LANE_LEDGER_SCHEMA_VERSION or report.get("lane_loop_exit_code") != 0:
        raise ValueError("Module lane ledger is not a successful actual full run")
    lanes = report.get("lanes")
    if not isinstance(lanes, list) or [entry.get("lane") for entry in lanes] != [name for name, _, _ in expected]:
        raise ValueError("Required full/GPU lane selection is absent or different")
    for lane, (_, _, strict) in zip(lanes, expected):
        if lane.get("strict") is not strict or lane.get("outcome") == runner.LANE_OUTCOME_NOT_RUN or lane.get("ended_run") is not False:
            raise ValueError("A selected lane was skipped or ended the run")
        if strict and (lane.get("outcome") != runner.LANE_OUTCOME_PASS
                       or lane.get("summary_reported") is not True
                       or lane.get("exit_code_reported") is not True
                       or lane.get("exit_code") != 0 or lane.get("zero_coverage") is not False
                       or lane.get("passed_tests", 0) <= 0 or lane.get("passed_assertions", 0) <= 0
                       or lane.get("failed_tests") != 0 or lane.get("failed_assertions") != 0
                       or lane.get("skipped_markers") != 0):
            raise ValueError("Required strict/GPU lane did not execute successful coverage")


def validate_runtime_report(report: dict, profile: str, binary_path: str) -> None:
    runtime = load_module("universal_runtime_runner", ROOT / "tests/runtime/run_runtime_validation.py")
    config = json.loads((ROOT / "tests/runtime/runtime_scenarios.json").read_text(encoding="utf-8"))
    expected = config["profiles"][profile]
    errors = runtime._validate_summary_schema(report)
    if errors or report.get("schema_valid") is not True or report.get("profile") != profile or report.get("gd_mode") != expected["gd_mode"]:
        raise ValueError("Canonical runtime report schema/profile/mode mismatch")
    tests = report["tests"]
    if (len(tests) != len(expected["gd_tests"])
            or {entry["name"] for entry in tests} != set(expected["gd_tests"])):
        raise ValueError("Canonical runtime profile selected tests are absent/different")
    if report.get("failed") != 0 or report.get("skipped") != 0 or report.get("total") != len(tests):
        raise ValueError("Runtime report failed/skipped required execution")
    for entry in tests:
        command = entry.get("command", [])
        if not command or str(PureWindowsPath(command[0])).casefold() != str(PureWindowsPath(binary_path)).casefold():
            raise ValueError("Runtime report executed a different binary path")
    statuses = [entry["status"] for entry in tests]
    if (any(status not in ("passed", runtime.NO_COMPLETION_MARKER_STATUS) for status in statuses)
            or report.get("passed") != statuses.count("passed")
            or report.get("no_completion_marker") != statuses.count(runtime.NO_COMPLETION_MARKER_STATUS)):
        raise ValueError("Runtime report outcome counters contradict actual test outcomes")
    # Preserve the existing completion-marker ladder's advisory status. This
    # gate certifies the canonical profile, not a silent threshold/ladder flip.
    if expected.get("requires_renderer_proof"):
        proof = report.get("renderer_proof", {})
        if proof.get("required") is not True or proof.get("status") != "passed" or proof.get("passed", 0) <= 0:
            raise ValueError("Required runtime renderer proof is absent or failed")


def validate_universal_receipt(receipt: dict, reports: dict[str, bytes], *, head: str,
                               context: str, run_id: str, run_attempt: str) -> None:
    policy.require_sha(head)
    if receipt.get("schema_version") != 2:
        raise ValueError("Universal evidence schema is required")
    expected = {"head_sha": head, "checkout_sha": head, "policy_context": context,
                "run_id": run_id, "run_attempt": run_attempt}
    for name, value in expected.items():
        if not value or receipt.get(name) != value:
            raise ValueError(f"Universal evidence does not match {name}")
    for name in REQUIRED_STEPS:
        if receipt.get("steps", {}).get(name) != "success":
            raise ValueError(f"Actual required step is absent/non-successful: {name}")
    hashes = receipt.get("sha256", {})
    if not re.fullmatch(r"[0-9a-f]{64}", hashes.get("binary", "")):
        raise ValueError("Missing measured binary hash")
    binary_path = receipt.get("binary_path")
    if (receipt.get("binary_before_sha256") != hashes["binary"] or not isinstance(binary_path, str)
            or not (PureWindowsPath(binary_path).is_absolute() or PurePosixPath(binary_path).is_absolute())):
        raise ValueError("Binary changed or its actual runtime path is absent")
    components = receipt.get("binary_components_sha256")
    expected_components = {binary_path}
    if binary_path.lower().endswith(".console.exe"):
        expected_components.add(binary_path[:-len(".console.exe")] + ".exe")
    if (not isinstance(components, dict) or set(components) != expected_components
            or receipt.get("binary_components_before_sha256") != components
            or components.get(binary_path) != hashes["binary"]
            or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
                   for value in components.values())):
        raise ValueError("Actual launcher/editor components changed or are not completely bound")
    if set(reports) != set(REPORT_SOURCES):
        raise ValueError("Actual report/ledger bytes are absent")
    for name, content in reports.items():
        if hashlib.sha256(content).hexdigest() != hashes.get(name):
            raise ValueError("Report bytes do not match receipt digest")
    validate_module_ledger(json.loads(reports["module-lanes.json"]))
    validate_runtime_report(json.loads(reports["headless-report.json"]), "headless-ci", binary_path)
    validate_runtime_report(json.loads(reports["streaming-report.json"]), "streaming-gpu-ci", binary_path)


def write_receipt(directory: Path, binary: Path, measurement: Path) -> None:
    head, context = current_identity()
    state = json.loads(measurement.read_text(encoding="utf-8"))
    if (state.get("head_sha") != head or state.get("policy_context") != context
            or state.get("binary_path") != str(binary.resolve()) or state.get("binary_sha256") != file_hash(binary)
            or state.get("binary_components_sha256") != binary_components(binary)):
        raise ValueError("Measured commit/policy/binary changed during execution")
    started = datetime.fromisoformat(state["started_utc"]).timestamp()
    reports = {}
    for name, relative in REPORT_SOURCES.items():
        path = ROOT / relative
        if path.stat().st_mtime < started:
            raise ValueError("Stale report predates this measurement")
        reports[name] = path.read_bytes()
    run_id, attempt = os.environ.get("GITHUB_RUN_ID", ""), os.environ.get("GITHUB_RUN_ATTEMPT", "")
    if not all(re.fullmatch(r"[1-9][0-9]*", value) for value in (run_id, attempt)):
        raise ValueError("Missing exact source run/attempt")
    receipt = {"schema_version": 2, "head_sha": head, "checkout_sha": head,
               "policy_context": context, "run_id": run_id, "run_attempt": attempt,
               "binary_path": state["binary_path"], "binary_before_sha256": state["binary_sha256"],
               "binary_components_before_sha256": state["binary_components_sha256"],
               "binary_components_sha256": binary_components(binary),
               "measurement_started_utc": state["started_utc"],
               "steps": {name: os.environ.get("GS_STEP_" + name.upper(), "") for name in REQUIRED_STEPS},
               "sha256": {"binary": file_hash(binary), **{name: hashlib.sha256(content).hexdigest() for name, content in reports.items()}}}
    validate_universal_receipt(receipt, reports, head=head, context=context, run_id=run_id, run_attempt=attempt)
    directory.mkdir(parents=True, exist_ok=True)
    for name, content in reports.items():
        (directory / name).write_bytes(content)
    (directory / "pr-gpu-evidence.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--start-measurement", type=Path)
    modes.add_argument("--write-receipt", type=Path)
    modes.add_argument("--verify", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--measurement", type=Path)
    args = parser.parse_args()
    try:
        if args.verify:
            receipt = json.loads((args.verify / "pr-gpu-evidence.json").read_text(encoding="utf-8"))
            reports = {name: (args.verify / name).read_bytes() for name in REPORT_SOURCES}
            head, context = current_identity()
            validate_universal_receipt(receipt, reports, head=head, context=context,
                                       run_id=os.environ.get("GITHUB_RUN_ID", ""),
                                       run_attempt=os.environ.get("GITHUB_RUN_ATTEMPT", ""))
        elif args.binary is None:
            raise ValueError("--binary is required")
        elif args.start_measurement:
            start_measurement(args.start_measurement, args.binary)
        elif args.measurement is None:
            raise ValueError("--measurement is required")
        else:
            write_receipt(args.write_receipt, args.binary, args.measurement)
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"Universal evidence rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
