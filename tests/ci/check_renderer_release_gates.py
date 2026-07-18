#!/usr/bin/env python3
"""Validate renderer evidence and public-alpha release gate contracts."""

from __future__ import annotations

import argparse
import datetime as _dt
import fnmatch
import hashlib
import importlib.util
import json
import math
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "docs" / "reference" / "renderer_release_gate_manifest.json"
TEST_CASE_RE = re.compile(r'TEST_CASE\("([^"]*\[RequiresGPU\][^"]*)"')
TAG_RE = re.compile(r"\[([^\]]+)\]")
PUBLIC_ALPHA_REQUIRED_FLAGS = (
    "disallow_path_filter_downgrade",
    "disallow_manual_downgrade",
    "disallow_missing_gpu_runner_downgrade",
    "disallow_open_world_advisory_only",
)
PREFERRED_ISSUE_CLASSIFICATIONS = (
    "blocking",
    "accepted_alpha_limitation",
    "deferred",
)
UNSUPPORTED_WORKFLOW_CLAIMS = (
    "must_enforce_readiness",
    "manual_input_may_disable",
    "path_filter_may_disable",
    "linux_only_release_allowed",
    "unmatched_release_files_allowed",
)
EXPECTED_WORKFLOW_ENFORCEMENT_SCOPE = "workflow-presence-and-required-job-markers"


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as fh:
        return json.load(fh)


def _repo_path(root: Path, value: str) -> Path:
    return root / value


def _repo_relative_path(root: Path, value: Any, label: str) -> tuple[Path | None, list[str]]:
    if not isinstance(value, str) or not value.strip():
        return None, [f"{label} must be a non-empty repo-relative path"]
    path = Path(value)
    if path.is_absolute():
        return None, [f"{label} must be repo-relative: {value}"]
    if ".." in path.parts:
        return None, [f"{label} must not escape the repository: {value}"]

    candidate = (root / path).resolve()
    repo_root = root.resolve()
    try:
        candidate.relative_to(repo_root)
    except ValueError:
        return None, [f"{label} must stay inside the repository: {value}"]
    return candidate, []


def _repo_relative_path_exists(root: Path, value: Any, label: str) -> list[str]:
    path, failures = _repo_relative_path(root, value, label)
    if failures:
        return failures
    if path is None or not path.exists():
        return [f"{label} missing: {value}"]
    return []


def _extract_requires_gpu_tests(root: Path) -> list[dict[str, Any]]:
    tests_root = root / "modules" / "gaussian_splatting" / "tests"
    tests: list[dict[str, Any]] = []
    for path in sorted(tests_root.rglob("*")):
        if path.suffix not in {".h", ".cpp"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for match in TEST_CASE_RE.finditer(text):
            name = match.group(1)
            tests.append(
                {
                    "path": path.relative_to(root).as_posix(),
                    "line": text[: match.start()].count("\n") + 1,
                    "name": name,
                    "tags": TAG_RE.findall(name),
                }
            )
    return tests


def _requires_gpu_snapshot(tests: Iterable[dict[str, Any]]) -> tuple[int, str]:
    rows = [f"{item['path']}\0{item['name']}" for item in tests]
    payload = "\n".join(rows).encode("utf-8")
    return len(rows), hashlib.sha256(payload).hexdigest()


def _load_gpu_harness_batches(root: Path, script_rel: str) -> dict[str, tuple[str, ...]]:
    script, failures = _repo_relative_path(root, script_rel, "gpu_harness_policy.script")
    if failures or script is None:
        raise RuntimeError("; ".join(failures))
    spec = importlib.util.spec_from_file_location("godotgs_gpu_harness_contract", script)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to import {script_rel}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return {batch.name: tuple(batch.filters) for batch in module.BATCHES}


def _filter_matches(filter_pattern: str, test_name: str) -> bool:
    """Match a doctest --test-case pattern against a test-case name.

    #329: this used `fnmatch.fnmatchcase`, which is NOT doctest's grammar.
    doctest treats only `*` and `?` as special (see doctest's wildcard matcher);
    `fnmatch` additionally treats `[...]` as a character class. Every doctest
    name here is bracket-heavy ("[GaussianSplatting][Node][RequiresGPU] ..."),
    so the two disagree badly: under fnmatch the Integration, OutputCompositor
    and TileRenderer batches each matched 0 tests while really matching 2, 3
    and 3. That silently under-counts `minimum_test_cases`, so promoting any
    of those batches to REQUIRED would have failed the gate with a bogus
    "matches 0 tests".

    It also forced batch authors to contort their filters into bracket-free
    phrases purely to keep fnmatch and doctest agreeing (see the comments on
    the RendererPipeline / Lifetime batches). With real doctest semantics a
    filter can just say what it means.
    """
    regex = "".join(
        ".*" if ch == "*" else ("." if ch == "?" else re.escape(ch))
        for ch in filter_pattern
    )
    return re.fullmatch(regex, test_name, re.DOTALL) is not None


def _is_broad_required_filter(filter_pattern: str) -> bool:
    compact = filter_pattern.replace("*", "").replace("?", "")
    return compact in {"[RequiresGPU]", "][RequiresGPU]", "RequiresGPU"}


def _tracked_actual_pngs(root: Path) -> list[str]:
    try:
        result = subprocess.run(
            ["git", "ls-files", "tests/visual_baselines/*.actual.png", "tests/visual_baselines/**/*.actual.png"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode == 0:
            return [line for line in result.stdout.splitlines() if line.strip()]
    except OSError:
        pass
    return [p.relative_to(root).as_posix() for p in (root / "tests" / "visual_baselines").rglob("*.actual.png")]


def _deferred_requires_gpu_tests(
    manifest: dict[str, Any],
    tests: Iterable[dict[str, Any]],
) -> list[dict[str, Any]]:
    # #329: deferral used to be purely tag-based ("every [SceneTree] or
    # [Importer] RequiresGPU case is deferred"). That is no longer true: the
    # [SceneTree] corpus now runs in the NodeSceneTree / WorldSceneTree /
    # SceneDirectorSceneTree batches of run_gpu_harness.py, and only the
    # individually-broken cases are still deferred.
    #
    # So the deferred set is the union of:
    #   * tag-based deferral   (deferred_tags_any)   — a whole family that
    #     cannot run at all; [Importer] is the only remaining one, and
    #   * name-based deferral  (deferred_test_names_any) — individual cases
    #     carved out of an otherwise-running batch via BatchSpec.excludes.
    #
    # Keeping both means "which tests are deferred" has exactly one machine-
    # readable answer, instead of living in a Python comment that drifts (this
    # issue's original defect: a comment claiming 26 while the truth was 28).
    snapshot = manifest.get("requires_gpu_test_snapshot", {})
    deferred_tags = set(snapshot.get("deferred_tags_any", []))
    deferred_names = set(snapshot.get("deferred_test_names_any", []))
    return [
        item
        for item in tests
        if deferred_tags.intersection(item["tags"]) or item["name"] in deferred_names
    ]


def _validate_manifest_basics(root: Path, manifest: dict[str, Any]) -> list[str]:
    failures: list[str] = []

    if manifest.get("schema_version") != 1:
        failures.append("manifest schema_version must be 1")

    for ref in manifest.get("references", []):
        failures.extend(_repo_relative_path_exists(root, ref, "reference path"))

    known_limitations = manifest.get("known_limitations_page")
    if not known_limitations:
        failures.append("known limitations page is missing")
    else:
        known_limitations_failures = _repo_relative_path_exists(root, known_limitations, "known limitations page")
        if known_limitations_failures:
            failures.append("known limitations page is missing")
            failures.extend(known_limitations_failures)

    return failures


def _validate_public_alpha_predicate(manifest: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    predicate = manifest.get("public_alpha_predicate", {})
    for flag in PUBLIC_ALPHA_REQUIRED_FLAGS:
        if predicate.get(flag) is not True:
            failures.append(f"public_alpha_predicate.{flag} must be true")
    allowed = predicate.get("required_issue_query", {}).get("allowed_classifications", [])
    if not isinstance(allowed, list) or set(allowed) != set(PREFERRED_ISSUE_CLASSIFICATIONS):
        failures.append(
            "public_alpha_predicate.required_issue_query.allowed_classifications must be "
            f"{list(PREFERRED_ISSUE_CLASSIFICATIONS)!r}"
        )
    return failures


def _manifest_issue_classifications(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    ledger = manifest.get("public_alpha_issue_ledger", {})
    tracked = ledger.get("tracked_issues", []) if isinstance(ledger, dict) else []
    classifications: dict[str, dict[str, Any]] = {}
    for issue in tracked:
        if not isinstance(issue, dict):
            continue
        number = issue.get("number")
        if number is None:
            continue
        classifications[str(number)] = issue
    return classifications


def _validate_issue_evidence_requirements(number: Any, classification: dict[str, Any]) -> list[str]:
    evidence_required = classification.get("evidence_required")
    if not isinstance(evidence_required, list) or not evidence_required:
        return [f"candidate issue #{number} classification missing evidence_required"]
    if any(not isinstance(item, str) or not item.strip() for item in evidence_required):
        return [f"candidate issue #{number} evidence_required entries must be non-empty strings"]
    return []


def _validate_public_alpha_issue_ledger(root: Path, manifest: dict[str, Any]) -> list[str]:
    ledger = manifest.get("public_alpha_issue_ledger")
    if not isinstance(ledger, dict):
        return ["public_alpha_issue_ledger must be a JSON object"]

    failures: list[str] = []
    allowed = ledger.get("allowed_statuses", [])
    if not isinstance(allowed, list) or set(allowed) != set(PREFERRED_ISSUE_CLASSIFICATIONS):
        failures.append(
            "public_alpha_issue_ledger.allowed_statuses must be "
            f"{list(PREFERRED_ISSUE_CLASSIFICATIONS)!r}"
        )

    tracked = ledger.get("tracked_issues", [])
    if not isinstance(tracked, list) or not tracked:
        failures.append("public_alpha_issue_ledger.tracked_issues must be a non-empty list")
        tracked = []

    seen: set[Any] = set()
    for issue in tracked:
        if not isinstance(issue, dict):
            failures.append(f"public_alpha_issue_ledger issue must be a JSON object: {issue!r}")
            continue
        number = issue.get("number")
        if not isinstance(number, int) or number <= 0:
            failures.append(f"public_alpha_issue_ledger issue number must be positive integer: {issue!r}")
            continue
        if number in seen:
            failures.append(f"public_alpha_issue_ledger duplicate issue #{number}")
        seen.add(number)

        status = issue.get("status")
        if status not in PREFERRED_ISSUE_CLASSIFICATIONS:
            failures.append(f"public_alpha_issue_ledger issue #{number} has invalid status {status!r}")
            continue
        for field in ("title", "goal"):
            if not issue.get(field):
                failures.append(f"public_alpha_issue_ledger issue #{number} missing {field}")
        failures.extend(_validate_issue_evidence_requirements(number, issue))
        if status == "accepted_alpha_limitation":
            failures.extend(_candidate_issue_accepted_limitation_failures(root, manifest, str(number), issue))
        if status == "deferred" and not issue.get("rationale"):
            failures.append(f"public_alpha_issue_ledger issue #{number} deferred classification missing rationale")

    return failures


def _validate_external_status_check_policy(manifest: dict[str, Any]) -> list[str]:
    policy = manifest.get("external_status_check_policy")
    if not isinstance(policy, dict):
        return ["external_status_check_policy must be a JSON object"]

    failures: list[str] = []
    required_status_checks = policy.get("branch_protection_required_status_checks", [])
    public_alpha_status_checks = policy.get("required_for_public_alpha", [])
    if not isinstance(required_status_checks, list):
        return ["external_status_check_policy.branch_protection_required_status_checks must be a list"]
    if not isinstance(public_alpha_status_checks, list):
        return ["external_status_check_policy.required_for_public_alpha must be a list"]
    for field, checks in (
        ("branch_protection_required_status_checks", required_status_checks),
        ("required_for_public_alpha", public_alpha_status_checks),
    ):
        for index, check in enumerate(checks):
            if not isinstance(check, str) or not check:
                failures.append(f"external_status_check_policy.{field}[{index}] must be a non-empty string")
    required_contexts = {check for check in required_status_checks if isinstance(check, str)}
    public_alpha_required = {check for check in public_alpha_status_checks if isinstance(check, str)}
    non_blocking = policy.get("non_blocking_checks", [])
    if not isinstance(non_blocking, list):
        return ["external_status_check_policy.non_blocking_checks must be a list"]

    qlty_non_blocking = False
    qlty_required = "qlty check" in required_contexts or "qlty check" in public_alpha_required
    for check in non_blocking:
        if not isinstance(check, dict):
            failures.append(f"external non-blocking check must be a JSON object: {check!r}")
            continue
        context = check.get("context")
        if not isinstance(context, str) or not context:
            failures.append(f"external non-blocking check context must be a non-empty string: {check!r}")
            continue
        if context == "qlty check":
            qlty_non_blocking = True
        if context in required_contexts or context in public_alpha_required or check.get("required_for_public_alpha") is True:
            failures.append(f"external check {context!r} cannot be both non-blocking and public-alpha required")
        if check.get("classification") != "deferred":
            failures.append(f"external non-blocking check {context!r} must use deferred classification")
        for field in ("issue", "reason", "decision_evidence", "local_gate_behavior"):
            if not check.get(field):
                failures.append(f"external non-blocking check {context!r} missing {field}")
    if not qlty_non_blocking and not qlty_required:
        failures.append("external_status_check_policy must classify qlty check as required or non-blocking")
    return failures


def _validate_requires_gpu_snapshot(
    manifest: dict[str, Any],
    tests: Iterable[dict[str, Any]],
) -> list[str]:
    failures: list[str] = []
    test_list = list(tests)
    count, digest = _requires_gpu_snapshot(test_list)
    snapshot = manifest.get("requires_gpu_test_snapshot", {})
    if count != snapshot.get("test_count") or digest != snapshot.get("sha256"):
        failures.append(
            "RequiresGPU test snapshot drift: "
            f"current count={count} sha256={digest}, "
            f"manifest count={snapshot.get('test_count')} sha256={snapshot.get('sha256')}"
        )

    deferred = _deferred_requires_gpu_tests(manifest, test_list)
    if len(deferred) != snapshot.get("deferred_count"):
        failures.append(
            f"deferred RequiresGPU count drift: current={len(deferred)} "
            f"manifest={snapshot.get('deferred_count')}"
        )
    if deferred and snapshot.get("closure_policy", "").find("blocks") < 0:
        failures.append("deferred RequiresGPU tests must be declared closure blockers")
    return failures


def _validate_deferred_requires_gpu_waivers(root: Path, manifest: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    for waiver in manifest.get("deferred_requires_gpu_waivers", []):
        for field in ("test_name", "issue_url", "owner", "expires_utc", "risk", "mitigation", "docs_path"):
            if not waiver.get(field):
                failures.append(f"deferred waiver missing {field}: {waiver!r}")
        docs_path = waiver.get("docs_path")
        if docs_path:
            failures.extend(_repo_relative_path_exists(root, docs_path, f"deferred waiver docs_path {docs_path}"))
    return failures


def _required_gpu_batch_filter_failures(name: Any, filters: tuple[str, ...]) -> list[str]:
    failures: list[str] = []
    for pattern in filters:
        if _is_broad_required_filter(pattern):
            failures.append(f"required GPU batch {name} uses broad filter: {pattern}")
    return failures


def _required_gpu_batch_match_failures(
    batch: dict[str, Any],
    name: Any,
    filters: tuple[str, ...],
    test_names: list[str],
) -> list[str]:
    matched = [case for case in test_names if any(_filter_matches(pattern, case) for pattern in filters)]
    if len(matched) < int(batch.get("minimum_test_cases", 1)):
        return [f"required GPU batch {name} matches {len(matched)} tests"]
    return []


def _required_gpu_batch_reference_failures(
    root: Path,
    batch: dict[str, Any],
    name: Any,
) -> list[str]:
    failures: list[str] = []
    for ref in batch.get("reference_artifacts", []):
        ref_failures = _repo_relative_path_exists(root, ref, f"required GPU batch {name} reference")
        for failure in ref_failures:
            if "missing" in failure:
                failures.append(f"required GPU batch {name} reference missing: {ref}")
            else:
                failures.append(f"required GPU batch {name} reference invalid: {failure}")
    return failures


def _validate_required_gpu_batch(
    root: Path,
    batch: dict[str, Any],
    name: Any,
    filters: tuple[str, ...],
    test_names: list[str],
) -> list[str]:
    failures: list[str] = []
    failures.extend(_required_gpu_batch_filter_failures(name, filters))
    failures.extend(_required_gpu_batch_match_failures(batch, name, filters, test_names))
    failures.extend(_required_gpu_batch_reference_failures(root, batch, name))
    return failures


def _validate_gpu_harness_policy(
    root: Path,
    manifest: dict[str, Any],
    tests: Iterable[dict[str, Any]],
) -> list[str]:
    failures: list[str] = []
    gpu_policy = manifest.get("gpu_harness_policy", {})
    try:
        batches = _load_gpu_harness_batches(root, gpu_policy.get("script", ""))
    except Exception as exc:
        failures.append(f"unable to load GPU harness batches: {exc}")
        batches = {}
    test_names = [item["name"] for item in tests]
    for batch in gpu_policy.get("required_batches", []):
        name = batch.get("name")
        filters = batches.get(name)
        if not filters:
            failures.append(f"required GPU batch missing from harness: {name}")
            continue
        failures.extend(_validate_required_gpu_batch(root, batch, name, filters, test_names))
    return failures


def _validate_visual_acceptance(root: Path, manifest: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    visual = manifest.get("visual_acceptance", {})
    if visual.get("tracked_actual_png_allowed") is False:
        tracked_actual = _tracked_actual_pngs(root)
        if tracked_actual:
            failures.append(f"tracked .actual.png artifacts are forbidden: {tracked_actual}")
    for reference in visual.get("blocking_references", []):
        if reference.get("required"):
            path_value = reference.get("path", "")
            ref_failures = _repo_relative_path_exists(root, path_value, "blocking visual reference")
            for failure in ref_failures:
                if "missing" in failure:
                    failures.append(f"blocking visual reference missing: {path_value}")
                else:
                    failures.append(f"blocking visual reference invalid: {failure}")
    return failures


def _workflow_policy_scope_failures(workflow_policy: dict[str, Any]) -> list[str]:
    if workflow_policy.get("machine_enforcement_scope") == EXPECTED_WORKFLOW_ENFORCEMENT_SCOPE:
        return []
    return [
        f"workflow_policy.machine_enforcement_scope must be {EXPECTED_WORKFLOW_ENFORCEMENT_SCOPE!r}"
    ]


def _validate_required_workflow(root: Path, workflow: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    workflow_file = workflow.get("file", "")
    text_path, path_failures = _repo_relative_path(root, workflow_file, "required workflow")
    if path_failures:
        return path_failures
    if not text_path.exists():
        return [f"required workflow missing: {workflow_file}"]

    text = text_path.read_text(encoding="utf-8", errors="ignore")
    for marker in workflow.get("required_jobs", []):
        if marker not in text:
            failures.append(f"required workflow marker {marker!r} missing from {workflow_file}")
    for claim in UNSUPPORTED_WORKFLOW_CLAIMS:
        if claim in workflow:
            failures.append(f"workflow policy claim {claim!r} is not machine-enforced for {workflow_file}")
    return failures


def _validate_workflow_policy(root: Path, manifest: dict[str, Any]) -> list[str]:
    workflow_policy = manifest.get("workflow_policy", {})
    failures = _workflow_policy_scope_failures(workflow_policy)
    for workflow in workflow_policy.get("required_workflows", []):
        failures.extend(_validate_required_workflow(root, workflow))

    return failures


# Cross-field invariant binding for lifetime advisory fields. Each entry
# maps an advisory_field -> (required strict scenario, count threshold key).
# Used at schema-check time to require thresholds_counts to declare the
# threshold and advisory_fields_strict_for to require the field, and at
# runtime to look up the threshold from the advisory field. Keeping the
# table shared means both sides cannot drift apart (Codex P2 review on
# PR #390).
ADVISORY_FIELD_STRICT_BINDINGS: dict[str, tuple[str, str]] = {
    # advisory_field -> (required strict scenario, count threshold key)
    "stringname_orphan_delta": ("stringname_orphans", "stringname_orphans_max"),
}


def _validate_lifetime_accounting_proof_schema(manifest: dict[str, Any]) -> list[str]:
    section = manifest.get("lifetime_accounting_proof")
    if section is None:
        return ["lifetime_accounting_proof section is missing"]
    if not isinstance(section, dict):
        return ["lifetime_accounting_proof must be a JSON object"]

    failures: list[str] = []
    marker = section.get("stdout_marker")
    if not isinstance(marker, str) or not marker:
        failures.append("lifetime_accounting_proof.stdout_marker must be a non-empty string")

    required_scenarios = section.get("required_scenarios")
    if not isinstance(required_scenarios, list) or not required_scenarios:
        failures.append("lifetime_accounting_proof.required_scenarios must be a non-empty list")
    else:
        for index, name in enumerate(required_scenarios):
            if not isinstance(name, str) or not name:
                failures.append(
                    f"lifetime_accounting_proof.required_scenarios[{index}] must be a non-empty string"
                )
        # Codex P2 review on PR #390 (comment #3295128412): the element-type
        # loop above accepts duplicate scenario names, so a manifest typo
        # that replaces "failed_init" with a second "renderer_instance"
        # silently disables coverage for the dropped scenario -- the
        # runtime walks scenarios via membership in required_scenarios, so
        # the duplicated entry is matched twice while failed_init is never
        # required to appear in stdout, and validate_lifetime_accounting_proof
        # passes a stdout artifact that never reports failed_init at all.
        # Reject duplicates at contract time and name them so the operator
        # can locate the typo immediately.
        seen: list[str] = []
        duplicates: list[str] = []
        for name in required_scenarios:
            if not isinstance(name, str) or not name:
                continue
            if name in seen and name not in duplicates:
                duplicates.append(name)
            else:
                seen.append(name)
        if duplicates:
            failures.append(
                f"lifetime_accounting_proof.required_scenarios contains duplicate "
                f"entries: {duplicates}; every scenario name must be unique so that "
                f"a typo replacing one scenario with a copy of another cannot "
                f"silently disable coverage for the dropped scenario"
            )

    metric_fields = section.get("scenario_metric_fields", {})
    if not isinstance(metric_fields, dict):
        failures.append("lifetime_accounting_proof.scenario_metric_fields must be a JSON object")
        metric_fields = {}
    else:
        for scenario, field in metric_fields.items():
            if not isinstance(scenario, str) or not scenario:
                failures.append(
                    "lifetime_accounting_proof.scenario_metric_fields keys must be non-empty strings"
                )
                continue
            if not isinstance(field, str) or not field:
                failures.append(
                    f"lifetime_accounting_proof.scenario_metric_fields[{scenario!r}] must be a non-empty string"
                )

    thresholds_bytes = section.get("thresholds_bytes")
    if not isinstance(thresholds_bytes, dict) or not thresholds_bytes:
        failures.append("lifetime_accounting_proof.thresholds_bytes must be a non-empty JSON object")
    else:
        for scenario, value in thresholds_bytes.items():
            if not isinstance(scenario, str) or not scenario:
                failures.append("lifetime_accounting_proof.thresholds_bytes keys must be non-empty strings")
                continue
            if not _is_json_number(value) or value < 0:
                failures.append(
                    f"lifetime_accounting_proof.thresholds_bytes[{scenario!r}] must be a non-negative number"
                )

    thresholds_counts = section.get("thresholds_counts", {})
    if not isinstance(thresholds_counts, dict):
        failures.append("lifetime_accounting_proof.thresholds_counts must be a JSON object")
        thresholds_counts = {}
    else:
        for name, value in thresholds_counts.items():
            if not isinstance(name, str) or not name:
                failures.append("lifetime_accounting_proof.thresholds_counts keys must be non-empty strings")
                continue
            if not _is_json_number(value) or value < 0:
                failures.append(
                    f"lifetime_accounting_proof.thresholds_counts[{name!r}] must be a non-negative number"
                )

    advisory_fields = section.get("advisory_fields", [])
    if not isinstance(advisory_fields, list):
        failures.append("lifetime_accounting_proof.advisory_fields must be a list")
    else:
        for index, name in enumerate(advisory_fields):
            if not isinstance(name, str) or not name:
                failures.append(
                    f"lifetime_accounting_proof.advisory_fields[{index}] must be a non-empty string"
                )

    sentinel = section.get("advisory_sentinel_value")
    if sentinel is not None and not _is_json_number(sentinel):
        failures.append("lifetime_accounting_proof.advisory_sentinel_value must be numeric or omitted")

    strict_for_raw = section.get("advisory_fields_strict_for", {})
    strict_for_valid: dict[str, list[str]] = {}
    if not isinstance(strict_for_raw, dict):
        failures.append(
            "lifetime_accounting_proof.advisory_fields_strict_for must be a JSON object mapping scenario -> [field, ...]"
        )
    else:
        for scenario, fields in strict_for_raw.items():
            if not isinstance(scenario, str) or not scenario:
                failures.append(
                    "lifetime_accounting_proof.advisory_fields_strict_for keys must be non-empty strings"
                )
                continue
            if not isinstance(fields, list) or not fields:
                failures.append(
                    f"lifetime_accounting_proof.advisory_fields_strict_for[{scenario!r}] must be a non-empty list of field names"
                )
                continue
            valid_fields: list[str] = []
            for index, field_name in enumerate(fields):
                if not isinstance(field_name, str) or not field_name:
                    failures.append(
                        f"lifetime_accounting_proof.advisory_fields_strict_for[{scenario!r}][{index}] must be a non-empty string"
                    )
                    continue
                valid_fields.append(field_name)
            if valid_fields:
                strict_for_valid[scenario] = valid_fields

    # Cross-field invariant: any advisory field that is bounded by a
    # thresholds_counts entry at runtime MUST appear in
    # advisory_fields_strict_for under the threshold's scenario; otherwise a
    # manifest can silently drop the field from the entry and the count
    # threshold goes unenforced (Codex P2 review on PR #390). The binding
    # is declared once at module scope as ADVISORY_FIELD_STRICT_BINDINGS
    # so the schema validator and the runtime validator look up the same
    # advisory_field -> (scenario, count_threshold_key) mapping.
    advisory_fields_set: set[str] = set()
    if isinstance(advisory_fields, list):
        advisory_fields_set = {name for name in advisory_fields if isinstance(name, str) and name}
    required_scenarios_set: set[str] = set()
    if isinstance(required_scenarios, list):
        required_scenarios_set = {
            name for name in required_scenarios if isinstance(name, str) and name
        }
    for advisory_field, (required_scenario, count_threshold_key) in ADVISORY_FIELD_STRICT_BINDINGS.items():
        if advisory_field not in advisory_fields_set:
            continue
        # Codex P2 review on PR #390 (round 6, comment #3295080072): the
        # bound scenario must appear in required_scenarios whenever the
        # advisory field is listed. Otherwise removing the scenario from
        # required_scenarios silently disables orphan-threshold
        # enforcement even though advisory_fields_strict_for and
        # thresholds_counts still validate: the runtime only walks
        # advisory fields for scenarios that actually appear, so dropping
        # the scenario from required_scenarios means the entry is never
        # required to exist and the count threshold is never compared.
        # Reject at contract-check time, parallel to the round-3/4/5
        # invariants and reusing the same ADVISORY_FIELD_STRICT_BINDINGS
        # source of truth.
        if required_scenario not in required_scenarios_set:
            failures.append(
                f"lifetime_accounting_proof.required_scenarios must include "
                f"{required_scenario!r} when advisory_fields lists "
                f"{advisory_field!r}; otherwise the orphan-threshold "
                f"enforcement is silently disabled by a manifest typo or "
                f"accidental scenario removal (bound scenario not required "
                f"(manifest invariant))"
            )
            continue
        # Codex P2 review on PR #390: the count threshold must be declared
        # whenever the advisory field is listed. A missing threshold would
        # let the runtime silently skip enforcement (arbitrarily large
        # orphan deltas pass as long as passed=true). Reject the manifest
        # at contract-check time instead.
        if not isinstance(thresholds_counts, dict) or count_threshold_key not in thresholds_counts:
            failures.append(
                f"lifetime_accounting_proof.thresholds_counts must declare "
                f"{count_threshold_key!r} when advisory_fields lists "
                f"{advisory_field!r}; otherwise the orphan-count guard is "
                f"silently disabled by a manifest typo or accidental key removal"
            )
            continue
        strict_fields_for_scenario = strict_for_valid.get(required_scenario, [])
        if advisory_field not in strict_fields_for_scenario:
            failures.append(
                f"lifetime_accounting_proof.advisory_fields_strict_for must require "
                f"{advisory_field!r} under scenario {required_scenario!r} when "
                f"advisory_fields lists {advisory_field!r} and thresholds_counts declares "
                f"{count_threshold_key!r}; otherwise the count threshold is silently "
                f"disablable by omitting the field from the entry"
            )

    # Cross-field invariant: every required scenario that declares a metric
    # field (i.e., appears in scenario_metric_fields) MUST also have a
    # thresholds_bytes entry. Otherwise the runtime byte-threshold check
    # silently no-ops (the `if byte_threshold is not None` branch in
    # validate_lifetime_accounting_proof falls through), and a manifest
    # typo or accidental key removal can disable the byte guard while still
    # passing the lifetime gate with arbitrarily large rd_bytes_leaked
    # values (Codex P2 review on PR #390; bot verified the exploit by
    # removing asset_attach_detach from thresholds_bytes and feeding a
    # huge leak value -- validate_lifetime_accounting_proof returned
    # success).
    required_scenarios_list: list[str] = []
    if isinstance(required_scenarios, list):
        required_scenarios_list = [
            name for name in required_scenarios if isinstance(name, str) and name
        ]
    metric_fields_dict: dict[str, Any] = (
        metric_fields if isinstance(metric_fields, dict) else {}
    )
    thresholds_bytes_dict: dict[str, Any] = (
        thresholds_bytes if isinstance(thresholds_bytes, dict) else {}
    )
    for scenario in required_scenarios_list:
        if scenario not in metric_fields_dict:
            # Scenarios without a declared metric field (e.g.
            # stringname_orphans, which is count-only) do not participate
            # in byte-threshold enforcement and so do not require an
            # entry in thresholds_bytes.
            continue
        if scenario not in thresholds_bytes_dict:
            failures.append(
                f"lifetime_accounting_proof.thresholds_bytes must declare an entry for "
                f"required scenario {scenario!r} because scenario_metric_fields binds it "
                f"to metric {metric_fields_dict[scenario]!r}; otherwise the byte-leak "
                f"guard is silently disabled by a manifest typo or accidental key removal "
                f"(byte threshold not declared (manifest invariant))"
            )

    for field in ("test_binary_lane", "gpu_harness_batch"):
        value = section.get(field)
        if value is not None and (not isinstance(value, str) or not value):
            failures.append(f"lifetime_accounting_proof.{field} must be a non-empty string when present")

    return failures


def validate_contract(root: Path, manifest: dict[str, Any]) -> list[str]:
    tests = _extract_requires_gpu_tests(root)
    failures: list[str] = []
    failures.extend(_validate_manifest_basics(root, manifest))
    failures.extend(_validate_public_alpha_predicate(manifest))
    failures.extend(_validate_public_alpha_issue_ledger(root, manifest))
    failures.extend(_validate_external_status_check_policy(manifest))
    failures.extend(_validate_requires_gpu_snapshot(manifest, tests))
    failures.extend(_validate_deferred_requires_gpu_waivers(root, manifest))
    failures.extend(_validate_gpu_harness_policy(root, manifest, tests))
    failures.extend(_validate_visual_acceptance(root, manifest))
    failures.extend(_validate_workflow_policy(root, manifest))
    failures.extend(_validate_lifetime_accounting_proof_schema(manifest))
    return failures


def _find_lane(rows: Iterable[dict[str, Any]], lane_id: str) -> dict[str, Any] | None:
    for row in rows:
        if row.get("lane_id") == lane_id:
            return row
    return None


def _rows_from_report(report: Any) -> list[dict[str, Any]]:
    if isinstance(report, list):
        return [row for row in report if isinstance(row, dict)]
    if isinstance(report, dict):
        # "lane_results" is the key the benchmark suite runner actually writes
        # (run_benchmark.py main()); accept it alongside the legacy aliases.
        for key in ("lanes", "results", "rows", "lane_results"):
            value = report.get(key)
            if isinstance(value, list):
                return [row for row in value if isinstance(row, dict)]
    return []


def _load_candidate_json(root: Path, rel_path: Any, label: str) -> tuple[Any | None, list[str]]:
    if not rel_path:
        return None, [f"candidate evidence missing {label}"]
    path, path_failures = _repo_relative_path(root, rel_path, f"candidate {label}")
    if path_failures or path is None:
        return None, path_failures
    try:
        return _load_json(path), []
    except OSError as exc:
        return None, [f"candidate {label} unreadable: {rel_path} ({exc})"]
    except json.JSONDecodeError as exc:
        return None, [f"candidate {label} invalid JSON: {rel_path} ({exc})"]


def _issue_rows_from_snapshot(snapshot: Any) -> list[dict[str, Any]] | None:
    if isinstance(snapshot, list):
        return [row for row in snapshot if isinstance(row, dict)]
    if isinstance(snapshot, dict):
        for key in ("issues", "nodes", "items", "rows"):
            value = snapshot.get(key)
            if isinstance(value, list):
                return [row for row in value if isinstance(row, dict)]
    return None


def _parse_time(value: str) -> _dt.datetime | None:
    try:
        return _dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (AttributeError, ValueError):
        return None


def _to_utc_aware(value: _dt.datetime) -> _dt.datetime:
    if value.tzinfo is None:
        return value.replace(tzinfo=_dt.timezone.utc)
    return value.astimezone(_dt.timezone.utc)


def _is_json_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def _candidate_selector_failures(manifest: dict[str, Any], evidence: dict[str, Any]) -> list[str]:
    predicate = manifest.get("public_alpha_predicate", {})
    expected_channel = predicate.get("channel")
    tag_patterns = predicate.get("accepted_tag_patterns", [])
    release_channel = evidence.get("release_channel") or evidence.get("channel")
    release_tag = evidence.get("release_tag") or evidence.get("tag")

    if _candidate_selector_matches(expected_channel, tag_patterns, release_channel, release_tag):
        return []

    expected = _candidate_selector_expected(expected_channel, tag_patterns)
    if release_channel or release_tag:
        return [
            "candidate selector is not public-alpha: "
            f"expected {expected}, got release_channel={release_channel!r} release_tag={release_tag!r}"
        ]
    return [f"candidate selector missing: expected {expected}"]


def _candidate_selector_matches(
    expected_channel: Any,
    tag_patterns: Iterable[Any],
    release_channel: Any,
    release_tag: Any,
) -> bool:
    if expected_channel and release_channel == expected_channel:
        return True
    if release_tag and any(fnmatch.fnmatchcase(str(release_tag), str(pattern)) for pattern in tag_patterns):
        return True
    return False


def _candidate_selector_expected(expected_channel: Any, tag_patterns: Iterable[Any]) -> str:
    expected = f"release_channel={expected_channel}"
    if tag_patterns:
        expected += f" or release_tag matching {tag_patterns}"
    return expected


def _validate_candidate_deferred_waivers(
    manifest: dict[str, Any],
    tests: Iterable[dict[str, Any]],
) -> list[str]:
    deferred = _deferred_requires_gpu_tests(manifest, tests)
    deferred_names = {item["name"] for item in deferred}
    waivers = manifest.get("deferred_requires_gpu_waivers", [])
    waiver_counts: dict[Any, int] = {}
    failures: list[str] = []
    for waiver in waivers:
        if not isinstance(waiver, dict):
            failures.append(f"candidate deferred RequiresGPU waiver must be an object: {waiver!r}")
            continue
        test_name = waiver.get("test_name")
        if not test_name:
            failures.append(f"candidate deferred RequiresGPU waiver missing test_name: {waiver!r}")
            continue
        waiver_counts[test_name] = waiver_counts.get(test_name, 0) + 1

    waiver_names = set(waiver_counts)
    for name in sorted(deferred_names - waiver_names):
        failures.append(f"candidate deferred RequiresGPU test missing waiver: {name}")
    for name in sorted(waiver_names - deferred_names):
        failures.append(f"candidate deferred RequiresGPU waiver does not match a deferred test: {name}")
    for name, count in sorted(waiver_counts.items()):
        if count > 1:
            failures.append(f"candidate deferred RequiresGPU waiver duplicated for {name}: {count}")
    return failures


def _normalize_sha(value: Any) -> str:
    return str(value).strip().lower()


def _candidate_expected_commit_failures(evidence: dict[str, Any], expected_commit: Any) -> list[str]:
    """Bind the evidence bundle to the commit that is actually being published.

    Without this, ``--mode candidate`` only proves the bundle is *internally*
    consistent (its recorded artifact digests match its recorded
    ``godot_binary_commit``). A bundle produced for an older, fully validated
    commit stays internally consistent forever, so it would authorize publishing
    a *different*, never-validated commit's binaries. The release workflow passes
    ``--expected-commit ${{ github.sha }}``; anything but an exact match fails
    closed (#593 supply-chain binding).
    """
    if not expected_commit:
        return []
    expected = _normalize_sha(expected_commit)
    actual = _normalize_sha(evidence.get("commit") or "")
    if not actual:
        # _candidate_commit_metadata_failures already reports the missing field;
        # still fail here so an empty commit can never satisfy the binding.
        return [f"candidate evidence has no commit to bind to the published commit {expected}"]
    if actual != expected:
        return [
            "candidate evidence commit does not match the commit being published: "
            f"evidence commit={actual} published commit={expected}. "
            "The evidence bundle must be regenerated for the commit under release."
        ]
    return []


def _candidate_expected_artifact_sha_failures(
    evidence: dict[str, Any],
    expected_artifact_shas: dict[str, str] | None,
) -> list[str]:
    """Bind the evidence bundle to the exact bytes that will be shipped.

    ``_candidate_artifact_integrity_failures`` only re-hashes the file the bundle
    itself points at, which lives next to the bundle. The archives that
    ``publish_release`` uploads are built by this workflow run and never touch
    that path, so a stale-but-consistent bundle would validate while completely
    different bytes shipped. The release workflow hashes the archives it just
    built and passes them as ``--artifact-sha <group>=<sha256>``; every named
    group must exist in the bundle and record exactly that digest.
    """
    if not expected_artifact_shas:
        return []
    failures: list[str] = []
    artifacts = evidence.get("artifacts")
    if not isinstance(artifacts, dict):
        return [
            "candidate evidence has no artifacts object to bind the built release "
            f"archives to: {sorted(expected_artifact_shas)}"
        ]
    for group in sorted(expected_artifact_shas):
        built_sha = _normalize_sha(expected_artifact_shas[group])
        artifact = artifacts.get(group)
        if not isinstance(artifact, dict):
            failures.append(
                f"candidate evidence is missing artifact group {group}; cannot bind the "
                f"built release archive (sha256={built_sha}) to the evidence bundle"
            )
            continue
        recorded_sha = _normalize_sha(artifact.get("sha256") or "")
        if not recorded_sha:
            failures.append(f"candidate artifact {group} records no sha256 to bind against")
            continue
        if recorded_sha != built_sha:
            failures.append(
                f"candidate artifact {group} sha256 does not match the archive built for this "
                f"release: evidence={recorded_sha} built={built_sha}. The validated bytes are "
                "not the bytes being published."
            )
    return failures


def _parse_artifact_sha_arg(value: str) -> tuple[str, str]:
    group, sep, digest = str(value).partition("=")
    group = group.strip()
    digest = digest.strip().lower()
    if not sep or not group or not digest:
        raise argparse.ArgumentTypeError(f"--artifact-sha expects <group>=<sha256>, got {value!r}")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise argparse.ArgumentTypeError(f"--artifact-sha digest must be a hex sha256, got {digest!r}")
    return group, digest


def _candidate_commit_metadata_failures(evidence: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if not evidence.get("commit"):
        failures.append("candidate evidence missing commit")
    commit_time_value = evidence.get("commit_time_utc")
    if not commit_time_value:
        failures.append("candidate evidence missing commit_time_utc")
    elif _parse_time(commit_time_value) is None:
        failures.append(f"candidate evidence has invalid commit_time_utc: {commit_time_value!r}")
    return failures


def _validate_candidate_artifact_group(
    root: Path,
    group: Any,
    artifact: Any,
    required_fields: Iterable[Any],
    commit: Any,
    commit_time: _dt.datetime | None,
) -> list[str]:
    failures: list[str] = []
    if not isinstance(artifact, dict):
        return [f"candidate artifact group missing: {group}"]
    failures.extend(_candidate_artifact_required_field_failures(group, artifact, required_fields))
    failures.extend(_candidate_artifact_integrity_failures(root, group, artifact))
    failures.extend(_candidate_artifact_commit_failures(group, artifact, commit))
    failures.extend(_candidate_artifact_mtime_failures(group, artifact, commit_time))
    return failures


def _candidate_artifact_required_field_failures(
    group: Any,
    artifact: dict[str, Any],
    required_fields: Iterable[Any],
) -> list[str]:
    failures: list[str] = []
    for field in required_fields:
        if not artifact.get(field):
            failures.append(f"candidate artifact {group} missing {field}")
    return failures


def _candidate_artifact_integrity_failures(
    root: Path,
    group: Any,
    artifact: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    raw_path = artifact.get("path")
    if not raw_path:
        return failures

    artifact_path, path_failures = _candidate_artifact_path(root, raw_path, group)
    if path_failures:
        return path_failures
    if artifact_path is None:
        return [f"candidate artifact {group} path missing: {raw_path}"]
    if not artifact_path.exists():
        return [f"candidate artifact {group} path missing: {raw_path}"]
    if not artifact_path.is_file():
        return [f"candidate artifact {group} path is not a file: {raw_path}"]

    expected_sha = artifact.get("sha256")
    if not expected_sha:
        return failures
    try:
        actual_sha = hashlib.sha256(artifact_path.read_bytes()).hexdigest()
    except OSError as exc:
        return [f"candidate artifact {group} cannot be read for sha256: {raw_path} ({exc})"]
    if str(expected_sha).lower() != actual_sha:
        failures.append(
            f"candidate artifact {group} sha256 mismatch: "
            f"expected {expected_sha} got {actual_sha}"
        )
    return failures


def _candidate_artifact_path(root: Path, raw_path: Any, group: Any) -> tuple[Path | None, list[str]]:
    return _repo_relative_path(root, raw_path, f"candidate artifact {group} path")


def _candidate_artifact_commit_failures(
    group: Any,
    artifact: dict[str, Any],
    commit: Any,
) -> list[str]:
    if commit and artifact.get("godot_binary_commit") != commit:
        return [f"candidate artifact {group} built from stale commit {artifact.get('godot_binary_commit')}"]
    return []


def _candidate_artifact_mtime_failures(
    group: Any,
    artifact: dict[str, Any],
    commit_time: _dt.datetime | None,
) -> list[str]:
    mtime = _parse_time(artifact.get("godot_binary_mtime_utc", ""))
    if commit_time and mtime and _to_utc_aware(mtime) < _to_utc_aware(commit_time):
        return [f"candidate artifact {group} binary predates commit"]
    return []


def _validate_candidate_artifacts(root: Path, manifest: dict[str, Any], evidence: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    artifacts = evidence.get("artifacts", {})
    if not isinstance(artifacts, dict):
        return ["candidate artifacts must be a JSON object"]
    required_groups = manifest.get("artifact_requirements", {}).get("required_groups", [])
    required_fields = manifest.get("artifact_requirements", {}).get("each_artifact_requires", [])
    commit = evidence.get("commit")
    commit_time = _parse_time(evidence.get("commit_time_utc", ""))

    for group in required_groups:
        artifact = artifacts.get(group)
        failures.extend(_validate_candidate_artifact_group(root, group, artifact, required_fields, commit, commit_time))

    return failures


def _candidate_gpu_batch_count_failures(
    name: Any,
    cases: dict[str, Any],
    required: dict[str, Any],
) -> list[str]:
    total = cases.get("total", 0)
    if not _is_json_number(total):
        return [f"candidate GPU batch {name} test_cases.total must be numeric"]
    if total < int(required.get("minimum_test_cases", 1)):
        return [f"candidate GPU batch {name} matched zero/too few tests"]
    return []


def _candidate_gpu_batch_execution_failures(
    name: Any,
    batch: dict[str, Any],
    cases: dict[str, Any],
    assertions: dict[str, Any],
    required: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    if batch.get("timed_out") and not required.get("allow_timeout", False):
        failures.append(f"candidate GPU batch {name} timed out")
    if batch.get("summary_parse_ok") is not True:
        failures.append(f"candidate GPU batch {name} summary did not parse")

    case_failed = cases.get("failed", 0)
    assertion_failed = assertions.get("failed", 0)
    rc = batch.get("rc", 0)
    invalid_counters = False
    if not _is_json_number(case_failed):
        failures.append(f"candidate GPU batch {name} test_cases.failed must be numeric")
        invalid_counters = True
    if not _is_json_number(assertion_failed):
        failures.append(f"candidate GPU batch {name} assertions.failed must be numeric")
        invalid_counters = True
    if not _is_json_number(rc):
        failures.append(f"candidate GPU batch {name} rc must be numeric")
        invalid_counters = True
    if not invalid_counters and (case_failed or assertion_failed or rc != 0):
        failures.append(f"candidate GPU batch {name} failed")
    return failures


def _candidate_gpu_batch_policy_failures(
    name: Any,
    batch: dict[str, Any],
    cases: dict[str, Any],
    required: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    skipped = cases.get("skipped", 0)
    if not _is_json_number(skipped):
        failures.append(f"candidate GPU batch {name} test_cases.skipped must be numeric")
    elif not required.get("allow_skips", False) and skipped:
        failures.append(f"candidate GPU batch {name} skipped tests")
    rid_leak_bytes = batch.get("rid_leak_bytes", 0)
    if not _is_json_number(rid_leak_bytes):
        failures.append(f"candidate GPU batch {name} rid_leak_bytes must be numeric")
    elif not required.get("allow_rid_leaks", False) and rid_leak_bytes:
        failures.append(f"candidate GPU batch {name} reported RID leaks")
    failures.extend(_candidate_gpu_batch_lifetime_failures(name, batch, required))
    return failures


def _candidate_gpu_batch_lifetime_failures(
    name: Any,
    batch: dict[str, Any],
    required: dict[str, Any],
) -> list[str]:
    """Reject candidate GPU reports that recorded a non-passing lifetime scenario.

    run_gpu_harness.py scrapes the fixture's [GS-LIFETIME] lines and records every
    scenario that reported ``passed != true`` (a real failure, a mark_skipped(), or
    a scope-exited-without-finalize) into the batch's ``lifetime_failed_scenarios``
    list (and escalates them at harness runtime via ``required_lifetime_failures``).
    The byte-leak / skip-count gates above cannot see these: doctest counts a
    fixture skip as PASSED and a non-byte (pipeline/shader/uniform-set) RID leak
    adds zero ``rid_leak_bytes`` — so a candidate report carrying a lifetime
    skip/failure would otherwise PASS ``--mode candidate`` even though the harness
    itself failed the batch. Fold the field into the candidate gate so the release
    decision matches the harness decision (Codex PR #419 round-2 Finding 1).

    Enforced whenever the manifest forbids EITHER skips or RID leaks for the batch
    (``allow_skips=false`` or ``allow_rid_leaks=false``): a non-passing lifetime
    scenario is simultaneously a hollow-coverage skip and a potential non-byte RID
    leak, so either prohibition is sufficient to reject it. A batch that explicitly
    allows both still tolerates the field, matching the harness's REQUIRED-only
    escalation.
    """
    enforce = (not required.get("allow_skips", False)) or (not required.get("allow_rid_leaks", False))
    if not enforce:
        return []
    raw = batch.get("lifetime_failed_scenarios", [])
    if raw is None:
        return []
    if not isinstance(raw, list):
        return [f"candidate GPU batch {name} lifetime_failed_scenarios must be a list"]
    scenarios = [str(entry) for entry in raw if str(entry).strip()]
    if not scenarios:
        return []
    return [
        f"candidate GPU batch {name} reported non-passing lifetime scenario(s): "
        f"{scenarios}"
    ]


def _validate_candidate_gpu_batch(
    name: Any,
    batch: dict[str, Any],
    required: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    cases = batch.get("test_cases", {})
    assertions = batch.get("assertions", {})
    if not isinstance(cases, dict):
        failures.append(f"candidate GPU batch {name} test_cases must be a JSON object")
        cases = {}
    if not isinstance(assertions, dict):
        failures.append(f"candidate GPU batch {name} assertions must be a JSON object")
        assertions = {}
    failures.extend(_candidate_gpu_batch_count_failures(name, cases, required))
    failures.extend(_candidate_gpu_batch_execution_failures(name, batch, cases, assertions, required))
    failures.extend(_candidate_gpu_batch_policy_failures(name, batch, cases, required))
    return failures


def _validate_candidate_gpu_report(root: Path, manifest: dict[str, Any], evidence: dict[str, Any]) -> list[str]:
    gpu_report_path = evidence.get("gpu_harness_report")
    report, load_failures = _load_candidate_json(root, gpu_report_path, "gpu_harness_report")
    if load_failures:
        return load_failures
    if not isinstance(report, dict):
        return [f"candidate gpu_harness_report must be a JSON object: {gpu_report_path}"]

    failures: list[str] = []
    batch_rows = report.get("batches", [])
    if not isinstance(batch_rows, list):
        failures.append(f"candidate GPU report batches must be a list: {gpu_report_path}")
        batch_rows = []

    batches: dict[Any, dict[str, Any]] = {}
    for index, batch in enumerate(batch_rows):
        if not isinstance(batch, dict):
            failures.append(f"candidate GPU report batch at index {index} must be a JSON object: {gpu_report_path}")
            continue
        batch_name = batch.get("name")
        if not isinstance(batch_name, str):
            failures.append(f"candidate GPU report batch at index {index} name must be a string: {gpu_report_path}")
            continue
        batches[batch_name] = batch
    for required in manifest.get("gpu_harness_policy", {}).get("required_batches", []):
        name = required.get("name")
        batch = batches.get(name)
        if not batch:
            failures.append(f"candidate GPU report missing required batch: {name}")
            continue
        failures.extend(_validate_candidate_gpu_batch(name, batch, required))
    return failures


def _candidate_lane_timeout_failures(lane_id: str, row: dict[str, Any]) -> list[str]:
    if row.get("timed_out"):
        return [f"candidate benchmark lane timed out: {lane_id}"]
    return []


def _candidate_lane_required_field_failures(
    lane_id: str,
    row: dict[str, Any],
    required_non_null: Iterable[str],
) -> list[str]:
    failures: list[str] = []
    for field in required_non_null:
        if row.get(field) is None:
            failures.append(f"candidate benchmark lane {lane_id} has null/missing {field}")
    return failures


def _candidate_lane_visual_failures(
    lane_id: str,
    row: dict[str, Any],
    visual_rules: dict[str, Any],
) -> list[str]:
    failures: list[str] = []
    capture_count = row.get("capture_count", 0)
    capture_reference_match_count = row.get("capture_reference_match_count")
    capture_count_valid = _is_json_number(capture_count)
    capture_match_valid = _is_json_number(capture_reference_match_count)
    if not capture_count_valid:
        failures.append(f"candidate benchmark lane {lane_id} capture_count must be numeric")
    elif capture_count < visual_rules.get("capture_count_min", 1):
        failures.append(f"candidate benchmark lane {lane_id} has no visual captures")
    if not capture_match_valid:
        failures.append(f"candidate benchmark lane {lane_id} capture_reference_match_count must be numeric")
    elif capture_count_valid and capture_reference_match_count != capture_count:
        failures.append(f"candidate benchmark lane {lane_id} did not match all references")
    if row.get("capture_ssim_min") is None:
        failures.append(f"candidate benchmark lane {lane_id} has null capture_ssim_min")
    if row.get("capture_psnr_min") is None:
        failures.append(f"candidate benchmark lane {lane_id} has null capture_psnr_min")
    capture_threshold_pass_count = row.get("capture_threshold_pass_count")
    if capture_count_valid and capture_threshold_pass_count is not None:
        if not _is_json_number(capture_threshold_pass_count):
            failures.append(f"candidate benchmark lane {lane_id} capture_threshold_pass_count must be numeric")
        elif capture_threshold_pass_count != capture_count:
            failures.append(f"candidate benchmark lane {lane_id} did not pass all visual thresholds")
    if row.get("visual_reference_match") is False:
        failures.append(f"candidate benchmark lane {lane_id} failed visual reference match")
    return failures


def _candidate_lane_identity_failures(lane_id: str, row: dict[str, Any], commit: Any) -> list[str]:
    failures: list[str] = []
    if commit:
        if row.get("commit_sha") != commit:
            failures.append(f"candidate benchmark lane {lane_id} commit_sha does not match candidate commit")
        if row.get("godot_binary_commit") != commit:
            failures.append(f"candidate benchmark lane {lane_id} godot_binary_commit does not match candidate commit")
    return failures


def _candidate_lane_execution_status_failures(lane_id: str, row: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    exit_code = row.get("exit_code")
    if exit_code is not None:
        if not _is_json_number(exit_code):
            failures.append(f"candidate benchmark lane {lane_id} exit_code must be numeric")
        elif exit_code != 0:
            failures.append(f"candidate benchmark lane {lane_id} exited with {exit_code}")
    for field in ("report_valid", "visible_output_valid", "proof_valid", "lane_valid"):
        if row.get(field) is False:
            failures.append(f"candidate benchmark lane {lane_id} has {field}=false")
    return failures


def _candidate_lane_gpu_timing_failures(lane_id: str, row: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    gpu_time = row.get("gpu_time_frame_ms")
    gpu_available = row.get("gpu_timing_available")
    gpu_time_is_number = isinstance(gpu_time, (int, float)) and not isinstance(gpu_time, bool)
    if gpu_available is True and (not gpu_time_is_number or gpu_time <= 0):
        failures.append(f"candidate benchmark lane {lane_id} has invalid GPU time")
    # The selected-route runner (run_benchmark.py) emits the timing source as
    # "gpu_frame_time_source"; the in-engine benchmark suite's overall block emits the same
    # value as "gpu_time_frame_source" (overall-schema naming, see benchmark_suite_lane.gd /
    # benchmark_suite_report.json). Accept either spelling so a produced report's explicit
    # "unavailable" marker is honoured regardless of which producer shaped the row.
    gpu_timing_source = row.get("gpu_frame_time_source")
    if gpu_timing_source is None:
        gpu_timing_source = row.get("gpu_time_frame_source")
    if gpu_available is not True and gpu_timing_source != "unavailable":
        failures.append(f"candidate benchmark lane {lane_id} lacks explicit GPU timing unavailability")
    return failures


def _candidate_lane_route_failures(lane_id: str, row: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    if row.get("cpu_fallback_route") and not row.get("fallback_route_allowed"):
        failures.append(f"candidate benchmark lane {lane_id} used unallowed CPU/fallback route")
    if row.get("cpu_sort_route_detected") and not row.get("fallback_route_allowed"):
        route_uid = row.get("cpu_sort_route_uid")
        failures.append(f"candidate benchmark lane {lane_id} used forbidden CPU sort route {route_uid!r}")
    fallback_count = row.get("sort_total_route_fallback_count")
    if fallback_count is not None:
        if not _is_json_number(fallback_count):
            failures.append(f"candidate benchmark lane {lane_id} sort_total_route_fallback_count must be numeric")
        elif fallback_count > 0 and not row.get("fallback_route_allowed"):
            failures.append(f"candidate benchmark lane {lane_id} used unallowed sort fallback routes")
    return failures


def _validate_candidate_benchmark_lane(
    lane_id: str,
    row: dict[str, Any],
    required_non_null: Iterable[str],
    visual_rules: dict[str, Any],
    commit: Any,
) -> list[str]:
    failures: list[str] = []
    failures.extend(_candidate_lane_timeout_failures(lane_id, row))
    failures.extend(_candidate_lane_identity_failures(lane_id, row, commit))
    failures.extend(_candidate_lane_execution_status_failures(lane_id, row))
    failures.extend(_candidate_lane_required_field_failures(lane_id, row, required_non_null))
    failures.extend(_candidate_lane_visual_failures(lane_id, row, visual_rules))
    failures.extend(_candidate_lane_gpu_timing_failures(lane_id, row))
    failures.extend(_candidate_lane_route_failures(lane_id, row))
    return failures


def _validate_candidate_benchmark_report(root: Path, manifest: dict[str, Any], evidence: dict[str, Any]) -> list[str]:
    benchmark_report_path = evidence.get("benchmark_report")
    report, load_failures = _load_candidate_json(root, benchmark_report_path, "benchmark_report")
    if load_failures:
        return load_failures

    failures: list[str] = []
    rows = _rows_from_report(report)
    required_non_null = manifest.get("benchmark_acceptance", {}).get("required_fields_non_null", [])
    visual_rules = manifest.get("visual_acceptance", {}).get("candidate_rules", {})
    commit = evidence.get("commit")
    for lane_id in manifest.get("benchmark_acceptance", {}).get("candidate_required_lanes", []):
        row = _find_lane(rows, lane_id)
        if row is None:
            failures.append(f"candidate benchmark lane missing: {lane_id}")
            continue
        failures.extend(_validate_candidate_benchmark_lane(lane_id, row, required_non_null, visual_rules, commit))
    return failures


def _candidate_issue_rows(evidence: dict[str, Any], issues: Any | None) -> list[dict[str, Any]] | None:
    issue_rows = _issue_rows_from_snapshot(issues)
    if issue_rows is not None:
        return issue_rows
    return _issue_rows_from_snapshot(evidence.get("issue_snapshot", evidence.get("issues", evidence.get("open_issues"))))


def _candidate_resolved_manifest_issue_rows(evidence: dict[str, Any]) -> list[dict[str, Any]] | None:
    if "resolved_manifest_issues" not in evidence:
        return []
    return _issue_rows_from_snapshot(evidence.get("resolved_manifest_issues"))


def _candidate_issue_label_names(issue: dict[str, Any]) -> set[Any]:
    return {
        label.get("name", label) if isinstance(label, dict) else label
        for label in issue.get("labels", [])
    }


def _candidate_issue_is_open(issue: dict[str, Any]) -> bool:
    return str(issue.get("state", "OPEN")).upper() == "OPEN"


def _candidate_issue_is_closed(issue: dict[str, Any]) -> bool:
    return str(issue.get("state", "")).upper() == "CLOSED"


def _candidate_issue_is_relevant(policy: dict[str, Any], labels: set[Any]) -> bool:
    classification_any = set(policy.get("classification_labels_any", []))
    if classification_any:
        return bool(classification_any.intersection(labels))
    blocking_any = set(policy.get("blocking_labels_any", []))
    alpha_p1_all = set(policy.get("alpha_relevant_p1_labels_all", []))
    return bool(blocking_any.intersection(labels)) or alpha_p1_all.issubset(labels)


def _validate_candidate_issue_classification(
    root: Path,
    manifest: dict[str, Any],
    number: str,
    classification: Any,
) -> list[str]:
    if not classification:
        return [f"candidate issue #{number} missing alpha classification"]
    if not isinstance(classification, dict):
        return [f"candidate issue #{number} alpha classification must be a JSON object"]

    status = classification.get("status")
    handlers = {
        "blocking": _candidate_issue_blocking_failures,
        "accepted_alpha_limitation": _candidate_issue_accepted_limitation_failures,
        "deferred": _candidate_issue_deferred_failures,
    }
    handler = handlers.get(status)
    if handler:
        return handler(root, manifest, number, classification)
    return [f"candidate issue #{number} has invalid classification {status!r}"]


def _candidate_issue_blocking_failures(
    _root: Path,
    _manifest: dict[str, Any],
    number: str,
    _classification: dict[str, Any],
) -> list[str]:
    return [f"candidate issue #{number} is still blocking"]


def _candidate_issue_accepted_limitation_failures(
    root: Path,
    manifest: dict[str, Any],
    number: str,
    classification: dict[str, Any],
) -> list[str]:
    failures = _validate_issue_evidence_requirements(number, classification)
    docs_path = classification.get("docs_path")
    known_limitations = manifest.get("known_limitations_page")
    if not docs_path:
        failures.append(f"candidate issue #{number} accepted limitation missing docs_path")
        return failures
    docs_path_failures = _repo_relative_path_exists(
        root,
        docs_path,
        f"candidate issue #{number} accepted limitation docs_path",
    )
    if docs_path_failures:
        failures.extend(docs_path_failures)
        return failures
    if docs_path != known_limitations:
        failures.append(
            f"candidate issue #{number} accepted limitation must use known_limitations_page "
            f"{known_limitations!r}, got {docs_path!r}"
        )
        return failures
    return failures


def _candidate_issue_deferred_failures(
    _root: Path,
    _manifest: dict[str, Any],
    number: str,
    classification: dict[str, Any],
) -> list[str]:
    failures = _validate_issue_evidence_requirements(number, classification)
    if not classification.get("rationale"):
        failures.append(f"candidate issue #{number} deferred classification missing rationale")
    return failures


def _candidate_issue_is_manifest_resolved(
    number: str,
    issue: dict[str, Any],
    classification: dict[str, Any],
) -> bool:
    return str(issue.get("number")) == number and _candidate_issue_is_closed(issue) and classification.get("status") == "blocking"


def _validate_omitted_manifest_blockers(
    manifest_classifications: dict[str, dict[str, Any]],
    issue_rows: list[dict[str, Any]],
    resolved_rows: list[dict[str, Any]],
) -> list[str]:
    snapshot_issue_numbers = {
        str(issue.get("number"))
        for issue in issue_rows
        if issue.get("number") is not None
    }
    resolved_by_number = {
        str(issue.get("number")): issue
        for issue in resolved_rows
        if issue.get("number") is not None
    }

    failures: list[str] = []
    for number, classification in sorted(manifest_classifications.items()):
        if number in snapshot_issue_numbers or classification.get("status") != "blocking":
            continue
        proof = resolved_by_number.get(number)
        if proof is None:
            failures.append(
                f"candidate issue snapshot missing manifest-tracked blocking issue #{number}; "
                "include it in issue_snapshot or resolved_manifest_issues"
            )
            continue
        if not _candidate_issue_is_closed(proof):
            failures.append(f"candidate resolved_manifest_issues issue #{number} must have state CLOSED")
    return failures


def _validate_candidate_issues(
    root: Path,
    manifest: dict[str, Any],
    evidence: dict[str, Any],
    issues: Any | None,
) -> list[str]:
    issue_rows = _candidate_issue_rows(evidence, issues)
    if issue_rows is None:
        return ["candidate issue snapshot missing: pass --issues-json or embed issue_snapshot/issues"]

    failures: list[str] = []
    policy = manifest.get("public_alpha_predicate", {}).get("required_issue_query", {})
    classifications = evidence.get("issue_classifications", {})
    if not isinstance(classifications, dict):
        return ["candidate issue_classifications must be a JSON object"]
    manifest_classifications = _manifest_issue_classifications(manifest)
    resolved_rows = _candidate_resolved_manifest_issue_rows(evidence)
    if resolved_rows is None:
        failures.append("candidate resolved_manifest_issues must be an issue snapshot object or list")
        resolved_rows = []
    failures.extend(_validate_omitted_manifest_blockers(manifest_classifications, issue_rows, resolved_rows))

    for issue in issue_rows:
        number = str(issue.get("number"))
        manifest_classification = manifest_classifications.get(number)
        if manifest_classification and _candidate_issue_is_manifest_resolved(number, issue, manifest_classification):
            continue
        if not _candidate_issue_is_open(issue):
            continue
        labels = _candidate_issue_label_names(issue)
        if not _candidate_issue_is_relevant(policy, labels) and manifest_classification is None:
            continue
        classification = manifest_classification or classifications.get(number)
        if isinstance(classification, dict):
            status = classification.get("status")
            if status in {"accepted_alpha_limitation", "deferred"} and number not in manifest_classifications:
                failures.append(
                    f"candidate issue #{number} {status} classification must be tracked in public_alpha_issue_ledger"
                )
                continue
        failures.extend(_validate_candidate_issue_classification(root, manifest, number, classification))
    return failures


def validate_candidate(
    root: Path,
    manifest: dict[str, Any],
    evidence: dict[str, Any],
    issues: Any | None = None,
    expected_commit: str | None = None,
    expected_artifact_shas: dict[str, str] | None = None,
) -> list[str]:
    failures = validate_contract(root, manifest)
    if not isinstance(evidence, dict):
        failures.append("candidate evidence must be a JSON object")
        return failures

    failures.extend(_candidate_selector_failures(manifest, evidence))
    failures.extend(_candidate_commit_metadata_failures(evidence))
    failures.extend(_candidate_expected_commit_failures(evidence, expected_commit))
    failures.extend(_candidate_expected_artifact_sha_failures(evidence, expected_artifact_shas))

    known_limitations = manifest.get("known_limitations_page")
    if known_limitations:
        known_limitations_failures = _repo_relative_path_exists(
            root,
            known_limitations,
            "candidate known limitations page",
        )
        if known_limitations_failures:
            failures.append("candidate known limitations page is missing")
            failures.extend(known_limitations_failures)

    tests = _extract_requires_gpu_tests(root)
    failures.extend(_validate_candidate_deferred_waivers(manifest, tests))
    failures.extend(_validate_candidate_artifacts(root, manifest, evidence))
    failures.extend(_validate_candidate_gpu_report(root, manifest, evidence))
    failures.extend(_validate_candidate_benchmark_report(root, manifest, evidence))
    failures.extend(_validate_candidate_issues(root, manifest, evidence, issues))

    return failures


def _coerce_lifetime_stdout_lines(stdout_artifact_or_path: Any) -> list[str]:
    if stdout_artifact_or_path is None:
        return []
    if isinstance(stdout_artifact_or_path, (list, tuple)):
        return [str(line) for line in stdout_artifact_or_path]
    if isinstance(stdout_artifact_or_path, Path):
        try:
            return stdout_artifact_or_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError as exc:
            raise RuntimeError(f"lifetime stdout artifact unreadable: {stdout_artifact_or_path} ({exc})") from exc
    if isinstance(stdout_artifact_or_path, str):
        # A file path cannot contain newlines and cannot have a component
        # longer than NAME_MAX (255 on Linux); strings that violate either
        # are definitely raw content. Skip the path probe in those cases so
        # we don't trip OSError [Errno 36] ENAMETOOLONG on Linux, which is
        # what Path.exists() does when the argument is too long to stat
        # (Windows tolerates this silently, masking the bug locally).
        looks_like_path = (
            "\n" not in stdout_artifact_or_path
            and all(len(part) <= 255 for part in stdout_artifact_or_path.split("/"))
            and all(len(part) <= 255 for part in stdout_artifact_or_path.split("\\"))
        )
        if looks_like_path:
            try:
                candidate = Path(stdout_artifact_or_path)
                if candidate.exists() and candidate.is_file():
                    try:
                        return candidate.read_text(encoding="utf-8", errors="ignore").splitlines()
                    except OSError as exc:
                        raise RuntimeError(f"lifetime stdout artifact unreadable: {candidate} ({exc})") from exc
            except OSError:
                # Defensive: any other path-probe error (permissions,
                # weird filesystem state) — treat as content rather than
                # surfacing a confusing path-related error for raw stdout.
                pass
        return stdout_artifact_or_path.splitlines()
    raise RuntimeError(f"lifetime stdout artifact has unsupported type: {type(stdout_artifact_or_path).__name__}")


def _parse_lifetime_lines(marker: str, lines: Iterable[str]) -> tuple[list[dict[str, Any]], list[str]]:
    entries: list[dict[str, Any]] = []
    failures: list[str] = []
    for raw_line in lines:
        if not raw_line.startswith(marker):
            continue
        payload_text = raw_line[len(marker) :].strip()
        if not payload_text:
            continue
        try:
            payload = json.loads(payload_text)
        except json.JSONDecodeError as exc:
            failures.append(f"lifetime line is not valid JSON: {payload_text!r} ({exc})")
            continue
        if not isinstance(payload, dict):
            failures.append(f"lifetime line payload must be a JSON object: {payload_text!r}")
            continue
        entries.append(payload)
    return entries, failures


def validate_lifetime_accounting_proof(
    manifest_section: dict[str, Any],
    stdout_artifact_or_path: Any,
) -> tuple[bool, list[str]]:
    """Validate [GS-LIFETIME] JSON lines emitted by the lifetime-proof fixture.

    `manifest_section` is the `lifetime_accounting_proof` block from the manifest.
    `stdout_artifact_or_path` may be a `Path`, a string containing either a
    filesystem path or raw stdout, or an iterable of lines.

    Returns `(passed, reasons)` where `reasons` is the list of human-readable
    failure descriptions (empty when `passed=True`).
    """
    reasons: list[str] = []
    if not isinstance(manifest_section, dict):
        return False, ["lifetime_accounting_proof manifest section must be a JSON object"]

    marker = manifest_section.get("stdout_marker")
    if not isinstance(marker, str) or not marker:
        return False, ["lifetime_accounting_proof.stdout_marker must be a non-empty string"]

    required_scenarios = manifest_section.get("required_scenarios", [])
    if not isinstance(required_scenarios, list) or not required_scenarios:
        return False, ["lifetime_accounting_proof.required_scenarios must be a non-empty list"]

    metric_fields = manifest_section.get("scenario_metric_fields", {}) or {}
    thresholds_bytes = manifest_section.get("thresholds_bytes", {}) or {}
    thresholds_counts = manifest_section.get("thresholds_counts", {}) or {}
    advisory_fields = manifest_section.get("advisory_fields", []) or []
    advisory_sentinel = manifest_section.get("advisory_sentinel_value")
    # advisory_fields_strict_for maps scenario -> [advisory_field, ...] for
    # scenarios where the advisory field MUST be present in the entry. The
    # sentinel value remains a tolerated "not measured this run" signal in
    # strict scenarios (it is the absence of the field, not the sentinel,
    # that masks regressions like the orphan threshold never being enforced).
    advisory_fields_strict_for = manifest_section.get("advisory_fields_strict_for", {}) or {}

    try:
        lines = _coerce_lifetime_stdout_lines(stdout_artifact_or_path)
    except RuntimeError as exc:
        return False, [str(exc)]

    entries, parse_failures = _parse_lifetime_lines(marker, lines)
    reasons.extend(parse_failures)

    by_scenario: dict[str, list[dict[str, Any]]] = {}
    for entry in entries:
        scenario = entry.get("scenario")
        if not isinstance(scenario, str) or not scenario:
            reasons.append(f"lifetime entry missing scenario name: {entry!r}")
            continue
        by_scenario.setdefault(scenario, []).append(entry)

    for scenario in required_scenarios:
        scenario_entries = by_scenario.get(scenario, [])
        count = len(scenario_entries)
        if count == 0:
            reasons.append(f"lifetime scenario missing from stdout: {scenario}")
        elif count > 1:
            # Codex P1 review on PR #390: required scenarios must appear
            # EXACTLY once. Concatenated or stale logs can otherwise
            # contribute entries that satisfy required coverage even
            # when the latest run omitted scenarios. Any duplicate (any
            # mix of passed=true/false) is treated as suspect.
            reasons.append(
                f"lifetime scenario {scenario} reported {count} entries; "
                f"expected exactly one (duplicate lifetime entry -- "
                f"concatenated or stale logs)"
            )

    for scenario, scenario_entries in by_scenario.items():
        for entry in scenario_entries:
            if entry.get("passed") is not True:
                fail_reason = entry.get("fail_reason") or "no fail_reason reported"
                reasons.append(f"lifetime scenario {scenario} reported passed=false: {fail_reason}")
                # Continue with other checks so the failure surface is complete.

            # Byte-threshold check using the scenario-specific metric field.
            byte_threshold = thresholds_bytes.get(scenario)
            metric_field = metric_fields.get(scenario)
            if byte_threshold is not None:
                if not metric_field:
                    reasons.append(
                        f"lifetime scenario {scenario} has bytes threshold but no scenario_metric_fields entry"
                    )
                else:
                    metric_value = entry.get(metric_field)
                    if metric_value is None:
                        reasons.append(
                            f"lifetime scenario {scenario} missing metric field {metric_field!r}"
                        )
                    elif not _is_json_number(metric_value):
                        reasons.append(
                            f"lifetime scenario {scenario} metric {metric_field} must be numeric, got {metric_value!r}"
                        )
                    elif metric_value > byte_threshold:
                        reasons.append(
                            f"lifetime scenario {scenario} {metric_field}={metric_value} exceeds threshold {byte_threshold}"
                        )
            elif metric_field:
                # Codex P2 review on PR #390: defense-in-depth. When the
                # scenario declares a metric field (via
                # scenario_metric_fields) but no corresponding
                # thresholds_bytes entry, the original code silently
                # skipped the byte check entirely -- a manifest typo or
                # accidental key removal could disable the byte guard
                # while still letting the runtime pass arbitrarily large
                # rd_bytes_leaked values. The schema validator normally
                # rejects this at contract time, but if someone edits the
                # schema check out the runtime must still refuse to
                # silently accept entries that report a real numeric
                # value in the metric field with no threshold.
                metric_value = entry.get(metric_field)
                if metric_value is not None and _is_json_number(metric_value):
                    reasons.append(
                        f"lifetime scenario {scenario} {metric_field}={metric_value} "
                        f"cannot be enforced because thresholds_bytes.{scenario} is not "
                        f"declared (byte threshold not declared (manifest invariant); the "
                        f"schema check should have rejected this)"
                    )

            # Strict-required advisory fields: scenarios listed in
            # advisory_fields_strict_for MUST report each named field, even if
            # the value is the sentinel. A silently missing field would
            # otherwise let the count threshold go unenforced (e.g.
            # stringname_orphans dropping stringname_orphan_delta entirely).
            strict_required_for_scenario = advisory_fields_strict_for.get(scenario, []) or []
            for required_field in strict_required_for_scenario:
                if required_field not in entry:
                    reasons.append(
                        f"lifetime scenario {scenario} missing required advisory field {required_field!r} "
                        f"(advisory_fields_strict_for requires it; sentinel {advisory_sentinel!r} is tolerated)"
                    )

            # Advisory-field checks. -1 sentinel means "not measured this run".
            for advisory_field in advisory_fields:
                if advisory_field not in entry:
                    # Absent fields are tolerated for non-strict scenarios.
                    # Strict-required scenarios already failed above.
                    continue
                value = entry.get(advisory_field)
                if advisory_sentinel is not None and value == advisory_sentinel:
                    continue
                # Map advisory field to a thresholds_counts key via the
                # shared ADVISORY_FIELD_STRICT_BINDINGS table. Fall back to
                # the advisory field name itself when no explicit binding
                # is declared (legacy behaviour).
                binding = ADVISORY_FIELD_STRICT_BINDINGS.get(advisory_field)
                if binding is not None:
                    _, count_threshold_key = binding
                else:
                    count_threshold_key = advisory_field
                count_threshold = thresholds_counts.get(count_threshold_key)
                if count_threshold is None:
                    if binding is not None:
                        # Codex P2 review on PR #390: defense-in-depth.
                        # When the manifest binds this advisory field to a
                        # count threshold key but the threshold is missing,
                        # never silently skip enforcement -- the schema
                        # check normally rejects this at contract time,
                        # but if someone edits the schema check out the
                        # runtime must still refuse to pass arbitrarily
                        # large deltas.
                        reasons.append(
                            f"lifetime scenario {scenario} advisory field "
                            f"{advisory_field}={value} cannot be enforced "
                            f"because thresholds_counts.{count_threshold_key} "
                            f"is not declared (manifest invariant; the schema "
                            f"check should have rejected this)"
                        )
                    continue
                if not _is_json_number(value):
                    reasons.append(
                        f"lifetime scenario {scenario} advisory field {advisory_field} must be numeric, got {value!r}"
                    )
                    continue
                if value > count_threshold:
                    reasons.append(
                        f"lifetime scenario {scenario} {advisory_field}={value} exceeds threshold {count_threshold}"
                    )

    return (not reasons), reasons


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("contract", "candidate", "lifetime"), default="contract")
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--candidate-evidence", default=None)
    parser.add_argument("--issues-json", default=None)
    parser.add_argument(
        "--expected-commit",
        default=None,
        help=(
            "Commit SHA the release is publishing (candidate mode). The evidence bundle's "
            "'commit' must equal it exactly, so a stale-but-internally-consistent bundle "
            "cannot authorize publishing a different commit's binaries."
        ),
    )
    parser.add_argument(
        "--artifact-sha",
        action="append",
        default=[],
        metavar="GROUP=SHA256",
        type=_parse_artifact_sha_arg,
        help=(
            "Bind an evidence artifact group to the sha256 of the archive actually built for "
            "this release (candidate mode, repeatable). The bundle must record exactly this "
            "digest for that group."
        ),
    )
    parser.add_argument(
        "--lifetime-stdout",
        default=None,
        help="Path to stdout artifact containing [GS-LIFETIME] JSON lines (required for --mode lifetime).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    manifest_path = Path(args.manifest).resolve()
    root = manifest_path.parents[2]
    manifest = _load_json(manifest_path)

    if args.mode == "contract":
        failures = validate_contract(root, manifest)
    elif args.mode == "lifetime":
        if not args.lifetime_stdout:
            print("--lifetime-stdout is required in lifetime mode", file=sys.stderr)
            return 2
        # Codex P2 review on PR #390 (comment #3294976919): run the schema
        # validator BEFORE invoking validate_lifetime_accounting_proof so
        # standalone lifetime runs cannot silently accept a malformed
        # manifest. Without this, e.g. removing advisory_fields_strict_for
        # or thresholds_counts.stringname_orphans_max would slip past the
        # lifetime gate because the runtime treats the bound advisory
        # field as optional. The strictest interpretation -- no opt-out
        # flag -- is intentional: any workflow that invokes lifetime mode
        # by itself (local checks, CI lifetime jobs) must satisfy the
        # same schema invariants that contract mode enforces.
        schema_failures = _validate_lifetime_accounting_proof_schema(manifest)
        if schema_failures:
            print("Renderer release gate lifetime schema check failed:")
            for failure in schema_failures:
                print(f" - {failure}")
            return 1
        section = manifest.get("lifetime_accounting_proof")
        if not isinstance(section, dict):
            print("manifest is missing lifetime_accounting_proof section", file=sys.stderr)
            return 2
        passed, reasons = validate_lifetime_accounting_proof(section, Path(args.lifetime_stdout))
        failures = reasons if not passed else []
    else:
        if not args.candidate_evidence:
            print("--candidate-evidence is required in candidate mode", file=sys.stderr)
            return 2
        evidence = _load_json(Path(args.candidate_evidence))
        issues = _load_json(Path(args.issues_json)) if args.issues_json else None
        artifact_shas: dict[str, str] = {}
        for group, digest in args.artifact_sha:
            previous = artifact_shas.get(group)
            if previous is not None and previous != digest:
                print(
                    f"--artifact-sha specified twice for {group} with different digests "
                    f"({previous} vs {digest})",
                    file=sys.stderr,
                )
                return 2
            artifact_shas[group] = digest
        failures = validate_candidate(
            root,
            manifest,
            evidence,
            issues,
            expected_commit=args.expected_commit,
            expected_artifact_shas=artifact_shas,
        )

    if failures:
        print("Renderer release gate check failed:")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print(f"Renderer release gate {args.mode} check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
