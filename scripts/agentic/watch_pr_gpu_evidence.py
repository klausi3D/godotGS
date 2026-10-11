#!/usr/bin/env python3
"""Trusted-base consumer for a PR's canonical GPU receipt. Never executes PR code."""
from __future__ import annotations

import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import subprocess
import time
from typing import NamedTuple
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gpu_evidence", ROOT / "tests/ci/check_pr_gpu_evidence.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
JOB_STEPS = {
    "build": "Build Godot editor with tests",
    "pipeline": "Run Gaussian pipeline smoke tests",
    "module": "Run module tests",
    "headless": "Run runtime validation harness (headless structural gate)",
    "streaming": "Run streaming GPU runtime gate (canonical blocking profile, windows-vulkan)",
    "postflight": "Postflight - GPU contention verdict (#875)",
}
MODULE_JOB = "Module Build + Runtime Harness (Windows Self-Hosted)"
GUARD_JOB = "Guards (Render Path + Static Safety)"
# The producer runs the PROPOSED tree, so its step labels and receipt prove
# nothing if the PR rewrote what runs under them. These files define the
# commands behind JOB_STEPS, the receipt, the contention verdict and the GPU
# environment preflight; a PR that changes any of them cannot self-certify and
# needs maintainer disposition. test_pr_gpu_evidence_wiring.py derives the
# scripts the producer's preflight/postflight/receipt steps run and fails if one
# is missing here.
PRODUCER_DEFINITIONS = (
    ".github/workflows/gaussian_production_gates.yml",
    "tests/ci/check_pr_gpu_evidence.py",
    "tests/ci/preflight_runner_gpu_environment.py",
    "tests/ci/runner_gpu_contention.py",
)


class EvidenceOutcome(NamedTuple):
    exit_code: int
    description: str


def api(endpoint: str, *, raw: bool = False, payload: dict | None = None):
    argv = ["gh", "api", endpoint]
    data = None
    if payload is not None:
        argv.extend(["--method", "POST", "--input", "-"])
        data = json.dumps(payload).encode("utf-8")
    result = subprocess.run(argv, input=data, check=True, capture_output=True, timeout=60)
    return result.stdout if raw else json.loads(result.stdout)


def pages(endpoint: str, key: str | None = None) -> list:
    items = []
    page = 1
    while True:
        separator = "&" if "?" in endpoint else "?"
        result = api(f"{endpoint}{separator}per_page=100&page={page}")
        batch = result[key] if key else result
        items.extend(batch)
        if len(batch) < 100:
            return items
        page += 1


def classify_paths(paths: list[str]) -> str:
    # ROOT is the base checkout, never the proposed merge/head tree.
    result = subprocess.run(["python", str(ROOT / "scripts/agentic/classify_change.py"),
                             "--policy", str(ROOT / ".agentic/policy.json"),
                             "--format", "json", "--paths", *paths],
                            check=True, capture_output=True, encoding="utf-8")
    risk = json.loads(result.stdout)["risk_class"]
    if risk not in ("R0", "R1", "R2", "R3"):
        raise ValueError("Unknown base policy risk class")
    return risk


def verify_jobs(jobs: list[dict]) -> dict[str, str]:
    by_name = {job["name"]: job for job in jobs}
    for name in (GUARD_JOB, MODULE_JOB):
        if name not in by_name or by_name[name].get("conclusion") != "success":
            raise ValueError(f"Missing or non-successful actual GitHub job: {name}")
    steps = {step["name"]: step for step in by_name[MODULE_JOB].get("steps", [])}
    for name in JOB_STEPS.values():
        step = steps.get(name)
        if not step or step.get("conclusion") != "success" or not step.get("started_at") or not step.get("completed_at"):
            raise ValueError(f"Required actual GitHub step did not run successfully: {name}")
    return {"pr-evidence-policy": "success", "guards": "success", "module-validation": "success"}


def read_receipt(data: bytes) -> dict:
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        entries = archive.infolist()
        if len(entries) != 1 or entries[0].filename != "pr-gpu-evidence.json" or entries[0].file_size > 65536:
            raise ValueError("Unexpected GPU receipt archive")
        return json.loads(archive.read(entries[0]))


def event_bindings(event: dict) -> tuple:
    pull = event.get("pull_request")
    group = event.get("merge_group")
    if pull:
        base = pull["base"]["sha"]
        head = pull["head"]["sha"]
        number = pull["number"]
        source_event = "pull_request"
    elif group:
        base, head = group["base_sha"], group["head_sha"]
        number = None
        source_event = "merge_group"
    else:
        raise ValueError("Unsupported evidence event")
    return pull, base, head, number, source_event


def changed_paths(repo: str, pull: dict | None, base: str, head: str, number: int | None) -> list[str]:
    if checker.git("rev-parse", "HEAD") != base:
        raise ValueError("Trusted consumer must run from the immutable base checkout")
    if pull:
        current = api(f"repos/{repo}/pulls/{number}")
        if current["head"]["sha"] != head or current["base"]["sha"] != base:
            raise ValueError("PR changed before classification; event is stale")
        files = pages(f"repos/{repo}/pulls/{number}/files")
        if len(files) != pull["changed_files"]:
            raise ValueError("Incomplete PR diff; refusing risk exemption")
        paths = sorted({path for item in files for path in (item["filename"], item.get("previous_filename")) if path})
    else:
        # Fetch an object for data-only diff inspection; never check out or execute it.
        subprocess.run(["git", "fetch", "--no-tags", "origin", head], cwd=ROOT,
                       check=True, capture_output=True, timeout=60)
        paths = checker.git("diff", "--name-only", "--no-renames", base, head).splitlines()
    return paths


def require_trusted_producer(paths: list[str]) -> None:
    changed = sorted(set(paths) & set(PRODUCER_DEFINITIONS))
    if changed:
        raise ValueError("PR changes the evidence producer (" + ", ".join(changed)
                         + "); its own run cannot certify itself. Maintainer disposition required")


def require_unshared_head(repo: str, number: int, head: str) -> None:
    # The status context is keyed by SHA alone, so a verdict for one PR's base
    # would also land on every other open PR with the same head.
    pulls = pages(f"repos/{repo}/commits/{head}/pulls")
    others = sorted(item["number"] for item in pulls
                    if item.get("state") == "open" and item.get("head", {}).get("sha") == head
                    and item.get("number") != number)
    if others:
        raise ValueError("Head SHA is shared with other open PRs " + ", ".join(f"#{n}" for n in others)
                         + "; a SHA-keyed status cannot carry a base-specific verdict")


def run_verdict(repo: str, risk: str, run: dict, expected: dict) -> str:
    attempt = run["run_attempt"]
    jobs = pages(f"repos/{repo}/actions/runs/{run['id']}/attempts/{attempt}/jobs", "jobs")
    outcomes = verify_jobs(jobs)
    artifacts = pages(f"repos/{repo}/actions/runs/{run['id']}/artifacts", "artifacts")
    matching = [item for item in artifacts if item["name"] == f"pr-gpu-evidence-{attempt}" and not item["expired"]]
    if len(matching) != 1 or matching[0]["size_in_bytes"] > 131072:
        raise ValueError("Exact-run receipt artifact is absent or ambiguous")
    receipt = read_receipt(api(f"repos/{repo}/actions/artifacts/{matching[0]['id']}/zip", raw=True))
    return checker.verdict(risk, outcomes, receipt, expected)


def success_is_current(repo: str, source_event: str, run: dict, expected: dict, number: int | None) -> bool:
    # Artifact/API reads can overlap a new run or rerun. A delayed lifecycle
    # event must never republish a superseded success.
    latest_runs = pages(f"repos/{repo}/actions/workflows/gaussian_production_gates.yml/runs?event={source_event}&head_sha={expected['head_sha']}", "workflow_runs")
    if not latest_runs:
        raise ValueError("Canonical run disappeared during validation")
    latest = max(latest_runs, key=lambda value: value["id"])
    if (latest["id"], latest["run_attempt"], latest["status"], latest["conclusion"]) != (run["id"], run["run_attempt"], "completed", "success"):
        return False
    if number is not None:
        current = api(f"repos/{repo}/pulls/{number}")
        if current["head"]["sha"] != expected["head_sha"] or current["base"]["sha"] != expected["base_sha"] or current["merge_commit_sha"] != expected["checkout_sha"]:
            raise ValueError("PR changed while verifying receipt")
        require_unshared_head(repo, number, expected["head_sha"])
    return True


def main() -> EvidenceOutcome:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout-seconds", type=int, default=9000)
    args = parser.parse_args()
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    repo = os.environ["GITHUB_REPOSITORY"]
    pull, base, head, number, source_event = event_bindings(event)
    paths = changed_paths(repo, pull, base, head, number)
    if pull:
        require_unshared_head(repo, number, head)
    risk = classify_paths(paths)
    if risk in ("R0", "R1"):
        if pull:
            # The exemption is published as a SHA-wide success, so recheck the
            # bindings it was classified under right before returning it.
            current = api(f"repos/{repo}/pulls/{number}")
            if current["head"]["sha"] != head or current["base"]["sha"] != base:
                raise ValueError("PR changed during classification; exemption is stale")
            require_unshared_head(repo, number, head)
        print(f"Base policy: {risk}; GPU evidence not required (not a GPU pass).")
        return EvidenceOutcome(0, f"{risk}: GPU evidence not required (not a GPU pass)")
    if pull and pull["head"]["repo"]["full_name"] != repo:
        raise ValueError("R2/R3 fork requires a maintainer-owned validation branch; no fork code executed")
    require_trusted_producer(paths)
    return wait_for_evidence(repo, risk, event, pull, base, head, number, source_event, args.timeout_seconds)


def current_checkout_sha(repo: str, pull: dict | None, base: str, head: str, number: int | None) -> str:
    if not pull:
        return head
    current = api(f"repos/{repo}/pulls/{number}")
    if current["head"]["sha"] != head or current["base"]["sha"] != base:
        raise ValueError("PR head/base changed; this evidence attempt is stale")
    return current["merge_commit_sha"]


def latest_source_run(repo: str, event: dict, pull: dict | None, head: str, source_event: str) -> dict | None:
    runs = pages(f"repos/{repo}/actions/workflows/gaussian_production_gates.yml/runs?event={source_event}&head_sha={head}", "workflow_runs")
    # Lifecycle events revalidate the newest execution. A PR event must not
    # accept a previous same-head execution while a new run is registering.
    if not event.get("source_run_id") and pull and pull.get("updated_at"):
        runs = [run for run in runs if run.get("created_at", "") >= pull["updated_at"]]
    return max(runs, key=lambda value: value["id"]) if runs else None


def wait_for_evidence(repo: str, risk: str, event: dict, pull: dict | None, base: str, head: str,
                      number: int | None, source_event: str, timeout_seconds: int) -> EvidenceOutcome:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        checkout_sha = current_checkout_sha(repo, pull, base, head, number)
        run = latest_source_run(repo, event, pull, head, source_event)
        if run and run["status"] == "completed":
            if run["conclusion"] != "success":
                if event.get("source_run_id"):
                    raise ValueError(f"Canonical workflow did not pass: {run['html_url']}")
                # A newly registered run or lifecycle trigger can replace it.
                time.sleep(20)
                continue
            expected = dict(base_sha=base, head_sha=head, checkout_sha=checkout_sha,
                            run_id=str(run["id"]), run_attempt=str(run["run_attempt"]))
            verdict = run_verdict(repo, risk, run, expected)
            if not success_is_current(repo, source_event, run, expected, number):
                continue
            print(verdict)
            print(run["html_url"])
            return EvidenceOutcome(0, "Trusted GPU evidence accepted")
        time.sleep(20)
    raise ValueError("Timed out waiting for exact-head canonical GPU evidence")


def controller() -> int:
    """Publish the status on the PROPOSED SHA, not pull_request_target's base SHA."""
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    repo = os.environ["GITHUB_REPOSITORY"]
    head = event["pull_request"]["head"]["sha"] if "pull_request" in event else event["merge_group"]["head_sha"]
    if not re.fullmatch(r"[0-9a-f]{40}", head):
        raise ValueError("Invalid proposed SHA")
    endpoint = f"repos/{repo}/statuses/{head}"
    common = dict(context="gpu-evidence-gate",
                  target_url=f"https://github.com/{repo}/actions/runs/{os.environ['GITHUB_RUN_ID']}")
    api(endpoint, payload={**common, "state": "pending", "description": "Waiting for trusted exact-head GPU evidence verdict"})
    try:
        result = main()
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print(f"Trusted GPU evidence verdict rejected: {error}")
        result = EvidenceOutcome(1, "GPU evidence missing, stale, invalid or failed")
    api(endpoint, payload={**common, "state": "success" if result.exit_code == 0 else "failure",
                          "description": result.description})
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(controller())
