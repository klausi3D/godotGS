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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout-seconds", type=int, default=10800)
    args = parser.parse_args()
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    repo = os.environ["GITHUB_REPOSITORY"]
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
    risk = classify_paths(paths)
    if risk in ("R0", "R1"):
        print(f"Base policy: {risk}; GPU evidence not required (not a GPU pass).")
        return 0
    if pull and pull["head"]["repo"]["full_name"] != repo:
        raise ValueError("R2/R3 fork requires a maintainer-owned validation branch; no fork code executed")
    deadline = time.monotonic() + args.timeout_seconds
    while time.monotonic() < deadline:
        if pull:
            current = api(f"repos/{repo}/pulls/{number}")
            if current["head"]["sha"] != head or current["base"]["sha"] != base:
                raise ValueError("PR head/base changed; this evidence attempt is stale")
            checkout_sha = current["merge_commit_sha"]
        else:
            checkout_sha = head
        runs = pages(f"repos/{repo}/actions/workflows/gaussian_production_gates.yml/runs?event={source_event}&head_sha={head}", "workflow_runs")
        if runs:
            run = max(runs, key=lambda value: value["id"])
            if run["status"] == "completed":
                if run["conclusion"] != "success":
                    raise ValueError(f"Canonical workflow did not pass: {run['html_url']}")
                attempt = run["run_attempt"]
                jobs = pages(f"repos/{repo}/actions/runs/{run['id']}/attempts/{attempt}/jobs", "jobs")
                outcomes = verify_jobs(jobs)
                artifacts = pages(f"repos/{repo}/actions/runs/{run['id']}/artifacts", "artifacts")
                matching = [item for item in artifacts if item["name"] == f"pr-gpu-evidence-{attempt}" and not item["expired"]]
                if len(matching) != 1 or matching[0]["size_in_bytes"] > 131072:
                    raise ValueError("Exact-run receipt artifact is absent or ambiguous")
                receipt = read_receipt(api(f"repos/{repo}/actions/artifacts/{matching[0]['id']}/zip", raw=True))
                expected = dict(base_sha=base, head_sha=head, checkout_sha=checkout_sha,
                                run_id=str(run["id"]), run_attempt=str(attempt))
                print(checker.verdict(risk, outcomes, receipt, expected))
                print(run["html_url"])
                return 0
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
        result = 1
    api(endpoint, payload={**common, "state": "success" if result == 0 else "failure",
                          "description": "Trusted GPU evidence accepted" if result == 0 else "GPU evidence missing, stale, invalid or failed"})
    return result


if __name__ == "__main__":
    raise SystemExit(controller())
