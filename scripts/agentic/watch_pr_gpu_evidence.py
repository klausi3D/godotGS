#!/usr/bin/env python3
"""Hosted trusted-policy consumer; universal gate is NOT ACTIVE without owner setup."""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import NamedTuple
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


checker = load_module("gpu_evidence", ROOT / "tests/ci/check_universal_gpu_evidence.py")
policy = load_module("gpu_commit_policy", ROOT / "scripts/agentic/gpu_commit_policy.py")
JOB_STEPS = {
    "build": "Build Godot editor with tests",
    "capture": "Capture measured commit and binary before runtime",
    "pipeline": "Run Gaussian pipeline smoke tests",
    "module": "Run module tests",
    "headless": "Run runtime validation harness (headless structural gate)",
    "streaming": "Run streaming GPU runtime gate (canonical blocking profile, windows-vulkan)",
    "postflight": "Postflight - GPU contention verdict (#875)",
    "receipt": "Bind canonical GPU evidence to reviewed commit",
    "upload": "Upload canonical PR GPU receipt",
}
MODULE_JOB = "Module Build + Runtime Harness (Windows Self-Hosted)"
GUARD_JOB = "Guards (Render Path + Static Safety)"


class EvidenceOutcome(NamedTuple):
    exit_code: int
    description: str
    verified_run: tuple[int, int] | None = None
    measured_sha: str | None = None
    policy_context: str | None = None


def api(endpoint: str, *, raw: bool = False, payload: dict | None = None):
    argv = ["gh", "api", endpoint]
    env = dict(os.environ)
    if payload is not None:
        credential = os.environ.get("GS_PUBLISH_TOKEN")
    elif "/protection" in endpoint or "/environments/" in endpoint or endpoint.endswith("/rulesets"):
        credential = os.environ.get("GS_PROTECTION_TOKEN")
    else:
        credential = os.environ.get("GH_TOKEN")
    if not credential:
        raise ValueError("Required scoped API credential is absent; no token fallback")
    env["GH_TOKEN"] = credential
    data = None
    if payload is not None:
        argv.extend(["--method", "POST", "--input", "-"])
        data = json.dumps(payload).encode("utf-8")
    result = subprocess.run(argv, input=data, check=True, capture_output=True, timeout=60, env=env)
    return result.stdout if raw else json.loads(result.stdout)


def pages(endpoint: str, key: str | None = None) -> list:
    items = []
    page = 1
    while True:
        separator = "&" if "?" in endpoint else "?"
        result = api(f"{endpoint}{separator}per_page=100&page={page}")
        batch = result[key] if key else result
        if not isinstance(batch, list):
            raise ValueError("Invalid paginated API response")
        items.extend(batch)
        if len(batch) < 100:
            return items
        page += 1


def verify_jobs(jobs: list[dict]) -> None:
    for name in (GUARD_JOB, MODULE_JOB):
        matching = [job for job in jobs if job.get("name") == name]
        if len(matching) != 1 or matching[0].get("conclusion") != "success":
            raise ValueError(f"Missing or non-successful actual GitHub job: {name}")
        if name == MODULE_JOB:
            for step_name in JOB_STEPS.values():
                steps = [step for step in matching[0].get("steps", []) if step.get("name") == step_name]
                if (len(steps) != 1 or steps[0].get("conclusion") != "success"
                        or not steps[0].get("started_at") or not steps[0].get("completed_at")):
                    raise ValueError(f"Required actual GitHub step did not run successfully: {step_name}")


def read_receipt(data: bytes) -> tuple[dict, dict[str, bytes]]:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = checker.UNIVERSAL_ARTIFACT_FILES
        entries = archive.infolist()
        if (len(entries) != len(names) or {item.filename for item in entries} != set(names)
                or any(item.file_size > 4 * 1024 * 1024 for item in entries)):
            raise ValueError("Unexpected universal receipt archive")
        files = {name: archive.read(name) for name in names}
        receipt = json.loads(files.pop("pr-gpu-evidence.json"))
        return receipt, files


def event_head(event: dict) -> tuple[str, dict]:
    pull = event.get("pull_request")
    if not isinstance(pull, dict):
        raise ValueError("Initial universal profile supports PRs targeting master only")
    return policy.require_sha(pull.get("head", {}).get("sha")), pull


def current_pull(repo: str, pull: dict, head: str) -> dict:
    number = pull.get("number")
    if not isinstance(number, int) or isinstance(number, bool) or number <= 0:
        raise ValueError("Missing current PR identity")
    current = api(f"repos/{repo}/pulls/{number}")
    if (current.get("state") != "open" or current.get("head", {}).get("sha") != head
            or current.get("base", {}).get("ref") != policy.TARGET_BRANCH
            or current.get("base", {}).get("repo", {}).get("full_name") != repo):
        raise ValueError("Unsupported or changed PR head/target")
    if current.get("head", {}).get("repo", {}).get("full_name") != repo:
        raise ValueError("Fork requires an exact-S maintainer-owned validation branch; no self-hosted fork execution")
    return current


def proof_settings(event: dict) -> tuple[dict[str, bytes], str, dict | None]:
    trusted = policy.require_sha(event.get("trusted_base_sha"))
    if checker.git("rev-parse", "HEAD") != trusted:
        raise ValueError("Controller must execute only the selected immutable trusted default checkout")
    bundle = policy.policy_bundle(trusted)
    context = policy.context_for(bundle)
    raw = os.environ.get("GS_CODE_EVIDENCE_BOOTSTRAP", "")
    bootstrap = json.loads(raw) if raw else None
    if bootstrap is not None and not isinstance(bootstrap, dict):
        raise ValueError("Invalid owner bootstrap configuration")
    return bundle, context, bootstrap


def live_proof(repo: str, head: str, context: str, bootstrap: dict | None = None,
               run: dict | None = None) -> str:
    policy.eligible_commit(api, repo, head)
    return policy.enforcement(api, repo, head, context, os.environ.get("GS_PUBLISH_APP_ID"),
                              bootstrap=bootstrap, run_id=str(run["id"]) if run else None,
                              run_attempt=str(run["run_attempt"]) if run else None)


def latest_source_run(repo: str, head: str, event: dict) -> dict | None:
    runs = pages(f"repos/{repo}/actions/workflows/gaussian_production_gates.yml/runs?event=pull_request&head_sha={head}", "workflow_runs")
    valid = [run for run in runs if run.get("head_sha") == head and run.get("event") == "pull_request"]
    if not valid:
        return None
    latest = max(valid, key=lambda value: value["id"])
    if event.get("source_run_id") is not None:
        if latest["id"] < event["source_run_id"]:
            return None
    return latest


def run_verdict(repo: str, run: dict, head: str, context: str) -> None:
    attempt = run["run_attempt"]
    verify_jobs(pages(f"repos/{repo}/actions/runs/{run['id']}/attempts/{attempt}/jobs", "jobs"))
    artifacts = pages(f"repos/{repo}/actions/runs/{run['id']}/artifacts", "artifacts")
    matching = [item for item in artifacts if item["name"] == f"pr-gpu-evidence-{attempt}" and not item["expired"]]
    if len(matching) != 1 or matching[0]["size_in_bytes"] > 12 * 1024 * 1024:
        raise ValueError("Exact-run universal receipt artifact is absent or ambiguous")
    receipt, reports = read_receipt(api(f"repos/{repo}/actions/artifacts/{matching[0]['id']}/zip", raw=True))
    checker.validate_universal_receipt(receipt, reports, head=head, context=context,
                                       run_id=str(run["id"]), run_attempt=str(attempt))


def main() -> EvidenceOutcome:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout-seconds", type=int, default=9000)
    args = parser.parse_args()
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    repo = os.environ["GITHUB_REPOSITORY"]
    head, pull = event_head(event)
    bundle, context, bootstrap = proof_settings(event)
    current_pull(repo, pull, head)
    policy.eligible_commit(api, repo, head)
    policy.require_producer_pin(head, bundle)
    deadline = time.monotonic() + args.timeout_seconds
    while time.monotonic() < deadline:
        current_pull(repo, pull, head)
        run = latest_source_run(repo, head, event)
        if run and run.get("status") == "completed":
            if run.get("conclusion") != "success":
                raise ValueError("Newest canonical run is not successful")
            live_proof(repo, head, context, bootstrap, run)
            run_verdict(repo, run, head, context)
            newest = latest_source_run(repo, head, event)
            if newest is None or (newest["id"], newest["run_attempt"], newest["status"], newest["conclusion"]) != (run["id"], run["run_attempt"], "completed", "success"):
                raise ValueError("Run changed during artifact verification")
            current_pull(repo, pull, head)
            live_proof(repo, head, context, bootstrap, run)
            return EvidenceOutcome(0, "Exact commit full module/GPU/runtime evidence accepted",
                                   (run["id"], run["run_attempt"]), head, context)
        time.sleep(30)
    raise ValueError("Timed out waiting for universal canonical evidence")


def publish_status(repo: str, head: str, context: str, state: str, description: str,
                   *, event: dict | None = None, proof: EvidenceOutcome | None = None) -> None:
    if state == "success":
        # Shared last boundary for EVERY success path, including owner bootstrap.
        if event is None or proof is None or proof.measured_sha != head or proof.policy_context != context:
            raise ValueError("SUCCESS has no trusted event binding")
        bundle, exact_context, bootstrap = proof_settings(event)
        if exact_context != context:
            raise ValueError("Policy changed before SUCCESS publication")
        policy.require_producer_pin(head, bundle)
        current_pull(repo, event["pull_request"], head)
        run = latest_source_run(repo, head, event)
        if (not run or (run["id"], run["run_attempt"]) != proof.verified_run
                or run.get("status") != "completed" or run.get("conclusion") != "success"):
            raise ValueError("Canonical run changed before SUCCESS publication")
        live_proof(repo, head, context, bootstrap, run)
    api(f"repos/{repo}/statuses/{head}", payload={"context": context, "state": state,
        "description": description, "target_url": f"https://github.com/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}"})


def controller() -> int:
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    repo = os.environ["GITHUB_REPOSITORY"]
    head, _ = event_head(event)
    _, context, _ = proof_settings(event)
    publish_status(repo, head, context, "pending", "Waiting for universal exact-commit evidence")
    try:
        result = main()
        publish_status(repo, head, context, "success" if result.exit_code == 0 else "failure", result.description, event=event, proof=result)
        return result.exit_code
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"Universal GPU evidence rejected: {error}")
        publish_status(repo, head, context, "failure", "Full evidence absent, invalid, stale, or gate NOT ACTIVE")
        return 1


def cleanup() -> int:
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    head, _ = event_head(event)
    repo = os.environ["GITHUB_REPOSITORY"]
    statuses = pages(f"repos/{repo}/commits/{head}/statuses")
    expected_url = f"https://github.com/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}"
    # Recover this run's immutable policy namespace even if master/policy moved
    # after the controller began. No controller outputs are needed after timeout.
    latest_by_context = {}
    for status in statuses:
        context = status.get("context", "")
        suffix = context.removeprefix(policy.POLICY_VERSION + "-")
        if len(suffix) != 64 or any(char not in "0123456789abcdef" for char in suffix):
            continue
        prior = latest_by_context.get(context)
        if prior is None or status["id"] > prior["id"]:
            latest_by_context[context] = status
    for context, latest in latest_by_context.items():
        if latest.get("state") == "pending" and latest.get("target_url") == expected_url:
            # Read/write is not atomic; failure is conservative, never SUCCESS.
            publish_status(repo, head, context, "failure", "Controller aborted before completing full evidence")
    return 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--cleanup"]:
        raise SystemExit(cleanup())
    raise SystemExit(controller())
