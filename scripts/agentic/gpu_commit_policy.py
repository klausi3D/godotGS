#!/usr/bin/env python3
"""Trusted universal commit evidence policy; proposed gate is inactive until owner setup."""
from __future__ import annotations

import hashlib
import importlib.util
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
MIGRATION_VARIABLE = "GS_CODE_EVIDENCE_MIGRATION"
BOOTSTRAP_VARIABLE = "GPU_CODE_EVIDENCE_BOOTSTRAP"
RUNTIME_CONFIG = "tests/runtime/runtime_scenarios.json"
GPU_MANIFEST = "docs/reference/renderer_release_gate_manifest.json"
_verifier_spec = importlib.util.spec_from_file_location("owner_grant_verifier", ROOT / "scripts/agentic/verify_owner_grant.py")
owner_grants = importlib.util.module_from_spec(_verifier_spec)
_verifier_spec.loader.exec_module(owner_grants)


def selected_dependency(path: str, manifest: dict) -> bool:
    return (path in manifest["files"] or any(path.startswith(p) for p in manifest["prefixes"])
            or path.rsplit("/", 1)[-1] in manifest.get("basenames", [])
            or (path.endswith(".py") and any(path.startswith(p) if p else "/" not in path
                for p in manifest.get("build_python_roots", []))))


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
    for key in ("basenames", "build_python_roots"):
        values = manifest.get(key, [])
        if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
            raise ValueError("Invalid fixed build dependency selector")
    entries = []
    for line in _git("ls-tree", "-r", "-z", ref).split(b"\0"):
        if not line:
            continue
        metadata, name = line.split(b"\t", 1)
        mode, kind, object_id = metadata.split()
        path = name.decode("utf-8")
        if selected_dependency(path, manifest):
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


def delegated_dependency(path: str) -> bool:
    """Narrow oracle/fixture families; fixed macros/bootstrap/control never delegate."""
    fixed = {"modules/gaussian_splatting/tests/gs_gpu_test_runner.cpp",
             "modules/gaussian_splatting/tests/gs_gpu_test_runner.h",
             "modules/gaussian_splatting/tests/test_macros.h"}
    if path in fixed:
        return False
    if path in {"tests/test_projection_math.cpp", "tests/servers/rendering/test_renderer_scene_cull.h", GPU_MANIFEST}:
        return True
    if path.startswith("modules/gaussian_splatting/tests/") and path.endswith((".h", ".cpp")):
        return True
    roots = ("tests/runtime/", "tests/examples/", "tests/fixtures/", "tests/visual_baselines/",
             "tests/baseline/", "tests/benchmark/", "modules/gaussian_splatting/tests/synthetic_baselines/")
    return path.startswith(roots) and path.endswith((".gd", ".json", ".png", ".jpg", ".ply",
                                                   ".gsplatcache", ".gsplatworld", ".tres", ".tscn"))


def bundle_json(bundle: dict[str, bytes], path: str) -> dict:
    mode, content = bundle[path].split(b"\0", 1)
    if mode != b"100644":
        raise ValueError("Policy JSON must be a non-executable regular data blob")
    value = json.loads(content)
    if not isinstance(value, dict):
        raise ValueError("Policy configuration must be a JSON object")
    return value


def require_data_invariants(trusted: dict[str, bytes], proposed: dict[str, bytes]) -> None:
    old, new = bundle_json(trusted, GPU_MANIFEST), bundle_json(proposed, GPU_MANIFEST)
    if old["schema_version"] != new.get("schema_version"):
        raise ValueError("Unsupported renderer manifest schema")
    before, after = old["gpu_harness_policy"], new["gpu_harness_policy"]
    if any(before[key] != after.get(key) for key in ("script", "blocking_profile")):
        raise ValueError("Unsupported GPU policy profile")
    old_rows, new_rows = before["required_batches"], after["required_batches"]
    if [row["name"] for row in old_rows] != [row.get("name") for row in new_rows]:
        raise ValueError("Required GPU set/order cannot change by data delegation")
    for previous, current in zip(old_rows, new_rows):
        minimum = current.get("minimum_test_cases")
        if (type(minimum) is not int or minimum < previous["minimum_test_cases"]
                or any(current.get(key) is not False for key in ("allow_skips", "allow_timeout", "allow_rid_leaks"))):
            raise ValueError("Delegated required GPU floors cannot be relaxed")
        references = current.get("reference_artifacts")
        if not isinstance(references, list) or any(path not in proposed for path in references):
            raise ValueError("GPU reference artifacts must be pinned verification inputs")
    old, new = bundle_json(trusted, RUNTIME_CONFIG), bundle_json(proposed, RUNTIME_CONFIG)
    if old["version"] != new.get("version"):
        raise ValueError("Unsupported runtime schema")
    for name in ("headless-ci", "streaming-gpu-ci"):
        previous, current = old["profiles"][name], new["profiles"][name]
        if (current.get("gd_mode") != previous["gd_mode"] or current.get("fail_on_skip") is not True
                or not set(previous["gd_tests"]).issubset(current.get("gd_tests", []))
                or (previous.get("requires_renderer_proof") and current.get("requires_renderer_proof") is not True)
                or current.get("godot_args") != previous.get("godot_args")):
            raise ValueError("Delegated canonical runtime coverage/profile cannot be relaxed")


def environment_record(api, repo: str, name: str) -> dict | None:
    """Read complete canonical environment inventory; never workflow/repository vars."""
    variables = api(f"repos/{repo}/environments/{ENVIRONMENT}/variables")
    entries = variables.get("variables")
    if not isinstance(entries, list) or variables.get("total_count") != len(entries):
        raise ValueError("Incomplete canonical environment variable inventory")
    names = [entry.get("name") for entry in entries]
    if name not in names:
        return None
    if names.count(name) != 1:
        raise ValueError("Ambiguous environment migration record")
    record = api(f"repos/{repo}/environments/{ENVIRONMENT}/variables/{name}")
    if record.get("name") != name:
        raise ValueError("Wrong canonical environment variable")
    record = owner_grants.parse_transport(record["value"])
    if not isinstance(record, dict):
        raise ValueError("Environment approval must be an exact JSON object")
    return record


def owner_binding(api, repo: str, base: str, head: str, app_id: object,
                  trusted: dict[str, bytes], *, kind: str, context: str | None = None) -> dict | None:
    """Environment data is untrusted transport; only immutable-key SSHSIG authorizes."""
    owner_grants.configured_profile(trusted)
    variable = BOOTSTRAP_VARIABLE if kind == "bootstrap" else MIGRATION_VARIABLE
    record = environment_record(api, repo, variable)
    if record is None:
        return None
    repository = api(f"repos/{repo}")
    return owner_grants.verify_grant(record, trusted, kind=kind, repo=repo,
                                    owner=repository.get("owner", {}), base=base, head=head,
                                    app_id=positive_integer(app_id), context=context)


def effective_policy(api, repo: str, base: str, head: str, app_id: object) -> tuple[dict[str, bytes], dict | None]:
    trusted = policy_bundle(base)
    binding = owner_binding(api, repo, base, head, app_id, trusted, kind="migration")
    if binding is None:
        return trusted, None
    target = binding["policy_sha"]
    commit = eligible_commit(api, repo, target)
    if commit["parents"][0]["sha"] != base:
        raise ValueError("Reviewed policy commit must have current B as sole parent")
    _git("fetch", "--no-tags", "origin", target)
    changed = _git("diff", "--name-only", "-z", base, target).split(b"\0")
    if any(not delegated_dependency(name.decode()) for name in changed if name):
        raise ValueError("Data-only migration changed fixed execution or production source")
    proposed = policy_bundle(target)
    if proposed == trusted:
        raise ValueError("Data-only migration requires a genuine new policy/context")
    for path in set(trusted) | set(proposed):
        if trusted.get(path) != proposed.get(path):
            if (not delegated_dependency(path) or path not in proposed
                    or proposed[path].split(b"\0", 1)[0] != (trusted[path].split(b"\0", 1)[0] if path in trusted else b"100644")):
                raise ValueError("Nondelegatable definitions/selection/modes changed")
    require_data_invariants(trusted, proposed)
    if context_for(proposed) != binding["context"]:
        raise ValueError("Authoritative migration context does not match actual policy bytes")
    return proposed, binding


def _validate_enforcement_repository(repository: dict, repo: str) -> None:
    owner = repository.get("owner", {})
    if (repository.get("default_branch") != TARGET_BRANCH or owner.get("type") != "User"
            or owner.get("login", "").casefold() != repo.split("/")[0].casefold()
            or type(owner.get("id")) is not int or owner["id"] <= 0):
        raise ValueError("Unsupported default/target branch")


def _validate_enforcement_protection_flags(protection: dict, checks: dict) -> None:
    if (checks.get("strict") is not True
            or protection.get("enforce_admins", {}).get("enabled") is not True
            or protection.get("allow_force_pushes", {}).get("enabled") is not False
            or protection.get("allow_deletions", {}).get("enabled") is not False):
        raise ValueError("Strict/admin/append-only protection is not actually enabled")


def _validate_enforcement_required_checks(required: list, context: str, app_id: int,
                                         head: str, bootstrap: dict | None,
                                         run_id: str | None, run_attempt: str | None) -> None:
    if not isinstance(required, list):
        raise ValueError("Required source-App checks are missing")
    names = {item.get("context") for item in required}
    if not {"agentic-pr-gate", "docs-build"}.issubset(names):
        raise ValueError("Existing required agentic/docs gates must remain required")
    for name in ("agentic-pr-gate", "docs-build"):
        source = [item for item in required if item.get("context") == name]
        if len(source) != 1 or source[0].get("app_id") != 15368:
            raise ValueError("Existing agentic/docs expected GitHub Actions App must remain unchanged")
    matching = [item for item in required if item.get("context") == context]
    if matching:
        if len(matching) != 1 or matching[0].get("app_id") != app_id:
            raise ValueError("Required universal context has the wrong publisher App")
    elif bootstrap != {"head_sha": head, "context": context,
                       "run_id": run_id, "run_attempt": run_attempt}:
        raise ValueError("Universal context is NOT ACTIVE; no exact owner bootstrap binding")


def _validate_enforcement_deployment_branches(policies: dict) -> None:
    entries = policies.get("branch_policies")
    if (policies.get("total_count") != 1 or not isinstance(entries, list) or len(entries) != 1
            or entries[0].get("name") != TARGET_BRANCH or entries[0].get("type") != "branch"):
        raise ValueError("Credential environment must allow only master branch, never tags/PR refs")


def enforcement(api, repo: str, head: str, context: str, app_id: object, *,
                bootstrap: dict | None = None, run_id: str | None = None,
                run_attempt: str | None = None) -> str:
    """Proof of real owner settings; registration allowance never bypasses strict."""
    require_sha(head)
    app_id = positive_integer(app_id)
    repository = api(f"repos/{repo}")
    _validate_enforcement_repository(repository, repo)
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
    _validate_enforcement_protection_flags(protection, checks)
    required = checks.get("checks")
    _validate_enforcement_required_checks(required, context, app_id, head, bootstrap, run_id, run_attempt)
    environment = api(f"repos/{repo}/environments/{ENVIRONMENT}")
    if environment.get("deployment_branch_policy") != {"protected_branches": False, "custom_branch_policies": True}:
        raise ValueError("Credential environment must restrict explicit branch types")
    policies = api(f"repos/{repo}/environments/{ENVIRONMENT}/deployment-branch-policies")
    _validate_enforcement_deployment_branches(policies)
    return base
