#!/usr/bin/env python3
"""Trusted universal commit evidence policy; proposed gate is inactive until owner setup."""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
POLICY_VERSION = "gpu-code-evidence-v1"
ENVIRONMENT = "trusted-gpu-verdict"
TARGET_BRANCH = "master"
SHA = re.compile(r"[0-9a-f]{40}\Z")
MANIFEST = "scripts/agentic/gpu_commit_policy.json"


def require_sha(value: object) -> str:
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ValueError("Missing or invalid immutable SHA")
    return value


def eligible_commit(api, repo: str, sha: str) -> dict:
    """Read canonical immutable object data; never use receipts or grafted local views."""
    require_sha(sha)
    commit = api(f"repos/{repo}/git/commits/{sha}")
    if commit.get("sha") != sha:
        raise ValueError("Canonical commit identity mismatch")
    parents = commit.get("parents")
    if not isinstance(parents, list) or len(parents) != 1:
        raise ValueError("SUCCESS requires a single-parent commit; merge/synthetic tips are ineligible")
    if not isinstance(parents[0], dict):
        raise ValueError("Canonical parent identity is absent")
    require_sha(parents[0].get("sha"))
    return commit


def _git(*args: str, data: bytes | None = None) -> bytes:
    return subprocess.run(["git", *args], cwd=ROOT, input=data, check=True,
                          capture_output=True, timeout=90).stdout


def policy_bundle(ref: str = "HEAD") -> dict[str, bytes]:
    """Collect tracked execution/configuration dependencies from immutable Git objects."""
    raw_manifest = _git("show", f"{ref}:{MANIFEST}")
    manifest = json.loads(raw_manifest)
    if manifest.get("version") != POLICY_VERSION:
        raise ValueError("Unsupported trusted policy version")
    exact = manifest["files"]
    prefixes = manifest["prefixes"]
    if (not isinstance(exact, list) or not isinstance(prefixes, list)
            or MANIFEST not in exact or any(not isinstance(p, str) for p in exact + prefixes)):
        raise ValueError("Invalid enforcement dependency manifest")
    entries = []
    for line in _git("ls-tree", "-r", "-z", ref).split(b"\0"):
        if not line:
            continue
        metadata, name = line.split(b"\t", 1)
        mode, kind, object_id = metadata.split()
        path = name.decode("utf-8")
        if path in exact or any(path.startswith(prefix) for prefix in prefixes):
            if kind != b"blob" or mode not in (b"100644", b"100755"):
                raise ValueError("Policy dependencies must be regular tracked files")
            entries.append((path, mode, object_id))
    missing = set(exact) - {entry[0] for entry in entries}
    if missing:
        raise ValueError("Missing policy definitions: " + ", ".join(sorted(missing)))
    response = _git("cat-file", "--batch", data=b"\n".join(entry[2] for entry in entries) + b"\n")
    files = {}
    cursor = 0
    for path, mode, expected in entries:
        end = response.index(b"\n", cursor)
        object_id, kind, size = response[cursor:end].split()
        if object_id != expected or kind != b"blob":
            raise ValueError("Invalid immutable dependency object")
        cursor = end + 1
        size = int(size)
        content = response[cursor:cursor + size]
        cursor += size + 1
        files[path] = mode + b"\0" + content
    return files


def context_for(bundle: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    for name, content in sorted(bundle.items()):
        digest.update(name.encode("utf-8") + b"\0" + str(len(content)).encode("ascii") + b"\0" + content)
    return POLICY_VERSION + "-" + digest.hexdigest()


def require_producer_pin(head: str, trusted: dict[str, bytes]) -> None:
    require_sha(head)
    _git("fetch", "--no-tags", "origin", head)
    proposed = policy_bundle(head)
    if proposed != trusted:
        raise ValueError("Proposed enforcement definitions differ from trusted policy; reviewed owner bootstrap required")


def positive_integer(value: object) -> int:
    if isinstance(value, bool) or not str(value).isdigit() or int(value) <= 0:
        raise ValueError("Missing expected dedicated publisher App identity")
    return int(value)


def enforcement(api, repo: str, head: str, context: str, app_id: object, *,
                bootstrap: dict | None = None, run_id: str | None = None,
                run_attempt: str | None = None) -> str:
    """Proof of real owner settings; registration allowance never bypasses strict."""
    require_sha(head)
    app_id = positive_integer(app_id)
    repository = api(f"repos/{repo}")
    if repository.get("default_branch") != TARGET_BRANCH:
        raise ValueError("Unsupported default/target branch")
    rulesets = api(f"repos/{repo}/rulesets")
    if not isinstance(rulesets, list) or rulesets:
        raise ValueError("Unmodelled rulesets; effective enforcement is ambiguous")
    branch = api(f"repos/{repo}/branches/{TARGET_BRANCH}")
    if branch.get("protected") is not True:
        raise ValueError("Target is not protected")
    base = require_sha(branch.get("commit", {}).get("sha"))
    compared = api(f"repos/{repo}/compare/{base}...{head}")
    if compared.get("merge_base_commit", {}).get("sha") != base:
        raise ValueError("Current target is not an ancestor of the proposed head")
    protection = api(f"repos/{repo}/branches/{TARGET_BRANCH}/protection")
    checks = protection.get("required_status_checks") or {}
    if (checks.get("strict") is not True
            or protection.get("enforce_admins", {}).get("enabled") is not True
            or protection.get("allow_force_pushes", {}).get("enabled") is not False
            or protection.get("allow_deletions", {}).get("enabled") is not False):
        raise ValueError("Strict/admin/append-only protection is not actually enabled")
    required = checks.get("checks")
    if not isinstance(required, list):
        raise ValueError("Required source-App checks are missing")
    names = {item.get("context") for item in required}
    if not {"agentic-pr-gate", "docs-build"}.issubset(names):
        raise ValueError("Existing required agentic/docs gates must remain required")
    matching = [item for item in required if item.get("context") == context]
    if matching:
        if len(matching) != 1 or matching[0].get("app_id") != app_id:
            raise ValueError("Required universal context has the wrong publisher App")
    elif bootstrap != {"head_sha": head, "context": context,
                       "run_id": run_id, "run_attempt": run_attempt}:
        raise ValueError("Universal context is NOT ACTIVE; no exact owner bootstrap binding")
    environment = api(f"repos/{repo}/environments/{ENVIRONMENT}")
    if environment.get("deployment_branch_policy") != {"protected_branches": False, "custom_branch_policies": True}:
        raise ValueError("Credential environment must restrict explicit branch types")
    policies = api(f"repos/{repo}/environments/{ENVIRONMENT}/deployment-branch-policies")
    entries = policies.get("branch_policies")
    if (policies.get("total_count") != 1 or not isinstance(entries, list) or len(entries) != 1
            or entries[0].get("name") != TARGET_BRANCH or entries[0].get("type") != "branch"):
        raise ValueError("Credential environment must allow only master branch, never tags/PR refs")
    return base
