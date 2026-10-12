#!/usr/bin/env python3
"""Produce/verify complete exact-commit evidence. No risk/path/label exemptions."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
REQUIRED_STEPS = ("build", "capture", "pipeline", "module", "headless", "streaming", "gpu_harness", "postflight")
UNIVERSAL_ARTIFACT_FILES = ("pr-gpu-evidence.json", "module-lanes.json", "headless-report.json", "streaming-report.json", "gpu-harness-report.json")
REPORT_SOURCES = {
    "module-lanes.json": "artifacts/module-lanes.json",
    "headless-report.json": "tests/runtime/runtime_validation_report_headless_ci.json",
    "streaming-report.json": "tests/runtime/runtime_validation_report_streaming_gpu_ci.json",
    "gpu-harness-report.json": "artifacts/gpu-harness-report.json",
}
GPU_FATAL_FIELDS = ("empty_required_batches", "zero_assertion_required_batches", "hollow_required_cases",
                    "case_audit_missing_batches", "case_audit_mismatch_batches", "zero_assertion_advisory_batches",
                    "vacuous_advisory_cases", "summary_parse_failures", "required_lifetime_failures", "timed_out_batches")
GPU_SCALAR_FIELDS = ("max_batch_rc", "parsed_failures", "total_rid_leak_bytes", "supervisor_exit")


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


def validate_runtime_report(report: dict, profile: str, binary_path: str, config: dict | None = None) -> None:
    runtime = load_module("universal_runtime_runner", ROOT / "tests/runtime/run_runtime_validation.py")
    if config is None:
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


def nonnegative_integer(value: object) -> bool:
    return type(value) is int and value >= 0


def _validate_gpu_row_identity(row: dict, spec, harness) -> None:
    shape = harness.BatchResult(spec.name, spec.filters, spec.excludes).to_dict()
    if set(shape) - set(row):
        raise ValueError("GPU batch fields were removed")
    if row["filters"] != list(spec.filters) or row["excludes"] != list(spec.excludes):
        raise ValueError("GPU catalogue filters/exclusions differ from fixed policy")
    for key in ("timed_out", "summary_parse_ok", "case_assert_audit_ok"):
        if type(row[key]) is not bool:
            raise ValueError("GPU row flag has invalid type")
    if type(row["rc"]) is not int or row["rc"] != 0 or row["timed_out"]:
        raise ValueError("GPU batch failed or timed out")


def _validate_gpu_row_audit_fields(row: dict) -> None:
    for key in ("rid_leak_bytes", "cases_started", "zero_assert_reported"):
        if not nonnegative_integer(row[key]):
            raise ValueError("GPU audit/leak counter has invalid type")
    for key in ("stdout_tail", "stderr_tail", "status"):
        if not isinstance(row[key], str):
            raise ValueError("GPU batch output has invalid type")
    for key in ("lifetime_failed_scenarios", "zero_assertion_cases", "env_skipped_cases"):
        if not isinstance(row[key], list) or any(not isinstance(item, str) for item in row[key]):
            raise ValueError("GPU per-case audit has invalid type")
    if not isinstance(row["wall_seconds"], (int, float)) or isinstance(row["wall_seconds"], bool) or not math.isfinite(row["wall_seconds"]) or row["wall_seconds"] < 0:
        raise ValueError("GPU batch duration has invalid type")


def _validate_gpu_row_counts(row: dict) -> None:
    for key, fields in (("test_cases", ("total", "passed", "failed", "skipped")), ("assertions", ("total", "passed", "failed"))):
        counts = row[key]
        if not isinstance(counts, dict) or set(counts) != set(fields) or any(not nonnegative_integer(counts[field]) for field in fields):
            raise ValueError("GPU summary counters have invalid shape/types")
        if counts["failed"] or counts["total"] != counts["passed"] + counts["failed"]:
            raise ValueError("GPU summary counters contradict success")
    if row["rid_leak_bytes"]:
        raise ValueError("GPU batch leaked RID bytes")


def _reconcile_gpu_row_stdout(row: dict, spec, harness) -> None:
    # Reparse all rows; required rows additionally obey assertive minimum/no-skip rules.
    parsed = harness.BatchResult(spec.name, spec.filters, spec.excludes)
    harness._parse_summary(row["stdout_tail"], parsed)
    actual = parsed.to_dict()
    fields = ("test_cases", "assertions", "status", "summary_parse_ok", "rid_leak_bytes",
              "lifetime_failed_scenarios", "zero_assertion_cases", "case_assert_audit_ok",
              "cases_started", "zero_assert_reported", "env_skipped_cases")
    if any(actual[key] != row[key] for key in fields):
        raise ValueError("GPU row contradicts canonical stdout parser/audit")


def validate_gpu_row(row: dict, spec, required: dict | None, harness, candidate) -> None:
    _validate_gpu_row_identity(row, spec, harness)
    _validate_gpu_row_audit_fields(row)
    _validate_gpu_row_counts(row)
    _reconcile_gpu_row_stdout(row, spec, harness)
    if required is not None:
        failures = candidate._validate_candidate_gpu_batch(spec.name, row, required)
        if (failures or row["status"] != "SUCCESS" or row["cases_started"] != row["test_cases"]["total"]
                or row["env_skipped_cases"] or row["assertions"]["passed"] <= 0):
            raise ValueError("Required GPU batch lacks complete assertive coverage: " + "; ".join(failures))
    elif row["test_cases"]["total"] > 0:
        if (row["assertions"]["total"] <= 0 or not row["case_assert_audit_ok"]
                or row["zero_assert_reported"] != len(row["zero_assertion_cases"])):
            raise ValueError("Advisory batch violates canonical assertion audit")
        for case in row["zero_assertion_cases"]:
            if case not in row["env_skipped_cases"] and spec.name + "/" + case not in harness.VACUOUS_CASE_ALLOWLIST:
                raise ValueError("Advisory case silently verified nothing without canonical allowance")


def _validate_gpu_report_header(report: dict, binary_path: str, harness) -> None:
    if (report.get("schema_version") != harness.REPORT_SCHEMA_VERSION or report.get("baseline_mode") != "compare"
            or report.get("totals_authoritative") is not True
            or str(PureWindowsPath(report.get("godot_binary", ""))).casefold() != str(PureWindowsPath(binary_path)).casefold()):
        raise ValueError("GPU report schema/baseline/binary/authority mismatch")
    for field in GPU_SCALAR_FIELDS:
        if type(report.get(field)) is not int or report[field] != 0:
            raise ValueError("GPU fatal scalar missing, invalid or nonzero: " + field)
    for field in GPU_FATAL_FIELDS:
        if type(report.get(field)) is not list or report[field]:
            raise ValueError("GPU fatal array missing, invalid or nonempty: " + field)
    for field in ("allowlisted_vacuous_cases", "env_skipped_advisory_cases"):
        if type(report.get(field)) is not list or any(not isinstance(item, str) for item in report[field]):
            raise ValueError("GPU advisory metadata missing or invalid")


def _canonical_gpu_report_rows(report: dict, harness) -> list[dict]:
    rows = report.get("batches")
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows) or [row.get("name") for row in rows] != [spec.name for spec in harness.BATCHES]:
        raise ValueError("GPU report does not contain the full ordered canonical catalogue")
    return rows


def validate_gpu_report(report: dict, binary_path: str, manifest: dict | None = None) -> None:
    harness = load_module("universal_gpu_harness", ROOT / "tests/ci/run_gpu_harness.py")
    candidate = load_module("universal_renderer_candidate", ROOT / "tests/ci/check_renderer_release_gates.py")
    if manifest is None:
        manifest = json.loads((ROOT / policy.GPU_MANIFEST).read_text(encoding="utf-8"))
    required_rows = manifest["gpu_harness_policy"]["required_batches"]
    required = {row["name"]: row for row in required_rows}
    if len(required) != len(required_rows) or set(required) != set(harness.REQUIRED_BATCHES):
        raise ValueError("GPU manifest disagrees with authoritative required catalogue")
    _validate_gpu_report_header(report, binary_path, harness)
    rows = _canonical_gpu_report_rows(report, harness)
    failures = candidate._candidate_gpu_report_verdict_failures(report, "bound GPU report")
    if failures:
        raise ValueError("GPU supervisor verdict failed: " + "; ".join(failures))
    for row, spec in zip(rows, harness.BATCHES):
        validate_gpu_row(row, spec, required.get(spec.name), harness, candidate)
    # Aggregate counters never substitute for actual required per-row parser evidence.
    totals = {family + "_" + field: sum(row[family][field] for row in rows)
              for family, fields in (("test_cases", ("total", "passed", "failed", "skipped")),
                                     ("assertions", ("total", "passed", "failed"))) for field in fields}
    if report.get("totals") != totals:
        raise ValueError("GPU aggregate counters contradict actual batch rows")


def _validate_universal_receipt_identity(receipt: dict, head: str, context: str,
                                       run_id: str, run_attempt: str) -> None:
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


def _validate_universal_binary_binding(receipt: dict) -> tuple[dict, str]:
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
    return hashes, binary_path


def _validate_universal_report_digests(reports: dict[str, bytes], hashes: dict) -> None:
    if set(reports) != set(REPORT_SOURCES):
        raise ValueError("Actual report/ledger bytes are absent")
    for name, content in reports.items():
        if hashlib.sha256(content).hexdigest() != hashes.get(name):
            raise ValueError("Report bytes do not match receipt digest")


def _validate_universal_measurement_window(receipt: dict, reports: dict[str, bytes]) -> None:
    started = datetime.fromisoformat(receipt["measurement_started_utc"])
    ended = datetime.fromisoformat(receipt["measurement_finished_utc"])
    gpu = json.loads(reports["gpu-harness-report.json"])
    gpu_start = datetime.fromisoformat(gpu["timestamp_started_utc"])
    gpu_end = datetime.fromisoformat(gpu["timestamp_finished_utc"])
    if (any(value.tzinfo is None for value in (started, ended, gpu_start, gpu_end))
            or not started <= gpu_start <= gpu_end <= ended <= datetime.now(timezone.utc)):
        raise ValueError("GPU report does not belong to this fresh measurement window")


def validate_universal_receipt(receipt: dict, reports: dict[str, bytes], *, head: str,
                               context: str, run_id: str, run_attempt: str, bundle: dict[str, bytes] | None = None) -> None:
    policy.require_sha(head)
    _validate_universal_receipt_identity(receipt, head, context, run_id, run_attempt)
    hashes, binary_path = _validate_universal_binary_binding(receipt)
    _validate_universal_report_digests(reports, hashes)
    validate_module_ledger(json.loads(reports["module-lanes.json"]))
    config = policy.bundle_json(bundle, policy.RUNTIME_CONFIG) if bundle is not None else None
    manifest = policy.bundle_json(bundle, policy.GPU_MANIFEST) if bundle is not None else None
    validate_runtime_report(json.loads(reports["headless-report.json"]), "headless-ci", binary_path, config)
    validate_runtime_report(json.loads(reports["streaming-report.json"]), "streaming-gpu-ci", binary_path, config)
    validate_gpu_report(json.loads(reports["gpu-harness-report.json"]), binary_path, manifest)
    _validate_universal_measurement_window(receipt, reports)


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
               "measurement_finished_utc": datetime.now(timezone.utc).isoformat(),
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
    modes.add_argument("--self-test", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--measurement", type=Path)
    args = parser.parse_args()
    try:
        if args.self_test:
            for report in ({}, {"schema_version": 1, "batches": []}):
                try:
                    validate_gpu_report(report, "C:/fixture/godot.exe")
                except (ValueError, KeyError, TypeError):
                    continue
                raise ValueError("Incomplete GPU report was accepted")
            print("Universal checker rejects absent GPU proof; portable discrimination suite remains required.")
        elif args.verify:
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
