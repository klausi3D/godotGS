#!/usr/bin/env python3
"""Bind the canonical PR GPU lane to the exact reviewed run; absence fails closed.

The workflow provides job/step outcomes, not a caller-selected list of required
checks. No GitHub token, network client, or self-hosted execution is needed here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REQUIRED_STEPS = ("build", "pipeline", "module", "headless", "streaming", "postflight")
REQUIRED_HASHES = ("binary", "headless_report", "streaming_report")
BINDINGS = ("checkout_sha", "head_sha", "base_sha", "run_id", "run_attempt")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True,
                          capture_output=True, encoding="utf-8").stdout.strip()


def classify(base: str) -> str:
    """Execute the classifier AND policy from the immutable review base."""
    if not re.fullmatch(r"[0-9a-f]{40}", base):
        raise ValueError("Missing or invalid immutable review base")
    paths = git("diff", "--name-only", "--no-renames", f"{base}...HEAD").splitlines()
    with tempfile.TemporaryDirectory(prefix="gs-evidence-policy-") as tmp:
        directory = Path(tmp)
        script = directory / "classify_change.py"
        policy = directory / "policy.json"
        script.write_text(git("show", f"{base}:scripts/agentic/classify_change.py"), encoding="utf-8")
        policy.write_text(git("show", f"{base}:.agentic/policy.json"), encoding="utf-8")
        result = subprocess.run([sys.executable, str(script), "--policy", str(policy),
                                 "--format", "json", "--paths", *paths], check=True,
                                capture_output=True, encoding="utf-8")
    risk = json.loads(result.stdout)["risk_class"]
    if risk not in ("R0", "R1", "R2", "R3"):
        raise ValueError(f"Unknown risk class: {risk!r}")
    return risk


def bindings_from_env() -> dict[str, str]:
    values = {key: os.environ.get("GS_EVIDENCE_" + key.upper(), "") for key in BINDINGS}
    for key in ("checkout_sha", "head_sha", "base_sha"):
        if not re.fullmatch(r"[0-9a-f]{40}", values[key]):
            raise ValueError(f"Missing or invalid {key}")
    for key in ("run_id", "run_attempt"):
        if not re.fullmatch(r"[1-9][0-9]*", values[key]):
            raise ValueError(f"Missing or invalid {key}")
    return values


def step_outcomes() -> dict[str, str]:
    return {step: os.environ.get("GS_STEP_" + step.upper(), "") for step in REQUIRED_STEPS}


def validate_receipt(receipt: dict, expected: dict[str, str]) -> None:
    if receipt.get("schema_version") != 1:
        raise ValueError("Missing or unsupported evidence receipt schema")
    for key in BINDINGS:
        if not expected.get(key) or receipt.get(key) != expected[key]:
            raise ValueError(f"Evidence does not match {key}")
    for step in REQUIRED_STEPS:
        if receipt.get("steps", {}).get(step) != "success":
            raise ValueError(f"Required step did not execute successfully: {step}")
    for key in REQUIRED_HASHES:
        digest = receipt.get("sha256", {}).get(key)
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError(f"Missing or invalid evidence hash: {key}")


def verdict(risk: str, jobs: dict[str, str], receipt: dict | None,
            expected: dict[str, str]) -> str:
    if jobs.get("pr-evidence-policy") != "success":
        raise ValueError("Immutable-base policy job did not execute successfully")
    if risk not in ("R0", "R1", "R2", "R3"):
        raise ValueError("No valid immutable-base risk classification")
    if risk in ("R0", "R1"):
        return f"GPU evidence not required by base policy ({risk}); not a GPU pass."
    for job in ("guards", "module-validation"):
        if jobs.get(job) != "success":
            raise ValueError(f"Required evidence job did not execute successfully: {job}")
    if receipt is None:
        raise ValueError("Required GPU evidence receipt is absent")
    validate_receipt(receipt, expected)
    return f"Canonical PR GPU evidence validated for {expected['head_sha']} ({risk})."


def write_receipt(path: Path, binary: Path) -> None:
    values = bindings_from_env()
    if git("rev-parse", "HEAD") != values["checkout_sha"]:
        raise ValueError("Built checkout does not match the workflow checkout SHA")
    sources = {
        "binary": binary,
        "headless_report": ROOT / "tests/runtime/runtime_validation_report_headless_ci.json",
        "streaming_report": ROOT / "tests/runtime/runtime_validation_report_streaming_gpu_ci.json",
    }
    hashes = {}
    for name, source in sources.items():
        with source.open("rb") as handle:
            hashes[name] = hashlib.file_digest(handle, "sha256").hexdigest()
    receipt = {"schema_version": 1, **values, "steps": step_outcomes(), "sha256": hashes}
    validate_receipt(receipt, values)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--classify-base")
    modes.add_argument("--write-receipt", type=Path)
    modes.add_argument("--verify", action="store_true")
    modes.add_argument("--self-test", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    try:
        if args.self_test:
            return subprocess.run([sys.executable, str(ROOT / "tests/ci/test_pr_gpu_evidence.py")], check=False).returncode
        if args.classify_base:
            risk = classify(args.classify_base)
            output = os.environ.get("GITHUB_OUTPUT")
            if output:
                with open(output, "a", encoding="utf-8") as handle:
                    handle.write(f"risk_class={risk}\n")
            print(f"Immutable-base risk class: {risk}")
        elif args.write_receipt:
            if args.binary is None:
                raise ValueError("--binary is required to produce evidence")
            write_receipt(args.write_receipt, args.binary)
        else:
            risk = os.environ.get("GS_EVIDENCE_RISK_CLASS", "")
            receipt = None
            if args.receipt and args.receipt.is_file():
                receipt = json.loads(args.receipt.read_text(encoding="utf-8-sig"))
            jobs = {"pr-evidence-policy": os.environ.get("GS_JOB_POLICY", ""),
                    "guards": os.environ.get("GS_JOB_GUARDS", ""),
                    "module-validation": os.environ.get("GS_JOB_MODULE", "")}
            # R0/R1 do not promise a GPU measurement; only R2/R3 require binding fields.
            expected = bindings_from_env() if risk in ("R2", "R3") else {}
            print(verdict(risk, jobs, receipt, expected))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"GPU evidence rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
