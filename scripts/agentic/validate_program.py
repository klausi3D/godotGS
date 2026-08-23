#!/usr/bin/env python3
"""Validate an agentic milestone program and its dependency graph.

The JSON schema checks shape. This validator adds the program invariants that the
repository's JSON-Schema-lite helper cannot express: non-empty goal packets,
immutable planning SHAs, unique milestone/work-item identities, valid GitHub
references, and an acyclic dependency graph whose dependencies appear earlier.

Live issue and PR state is deliberately not fetched here. GitHub remains the
status authority and must be re-queried by the dispatcher.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCHEMA = ROOT / ".agentic" / "schemas" / "program.schema.json"

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
ISSUE_REF_RE = re.compile(r"^#[1-9][0-9]*$")
MILESTONE_URL_RE = re.compile(r"^https://github\.com/[^/]+/[^/]+/milestone/[1-9][0-9]*$")


def _load_schema_validator():
    path = Path(__file__).with_name("validate_review.py")
    spec = importlib.util.spec_from_file_location("validate_review", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_instance


validate_instance = _load_schema_validator()


def _git_commit_exists(sha: str) -> bool:
    try:
        completed = subprocess.run(
            ["git", "-C", str(ROOT), "cat-file", "-e", f"{sha}^{{commit}}"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return completed.returncode == 0


def _non_empty(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def validate_program(program: Any, schema: dict[str, Any]) -> list[str]:
    errors = validate_instance(program, schema, "$")
    if not isinstance(program, dict):
        return errors

    for field in ("program_id", "title", "status_authority"):
        if field in program and not _non_empty(program.get(field)):
            errors.append(f"$.{field}: must not be empty")

    snapshot = program.get("planning_snapshot_sha")
    if isinstance(snapshot, str) and not SHA_RE.fullmatch(snapshot):
        errors.append("$.planning_snapshot_sha: must be a lowercase 40-character commit SHA")

    status_authority = program.get("status_authority")
    if isinstance(status_authority, str) and not ISSUE_REF_RE.fullmatch(status_authority):
        errors.append("$.status_authority: must be a GitHub issue reference such as #458")

    dispatch = program.get("dispatch")
    if isinstance(dispatch, dict):
        for field in ("task_contract_template",):
            if field in dispatch and not _non_empty(dispatch.get(field)):
                errors.append(f"$.dispatch.{field}: must not be empty")
        for field in ("implementation_wip_limit", "heavy_process_limit"):
            value = dispatch.get(field)
            if isinstance(value, int) and not isinstance(value, bool) and value < 1:
                errors.append(f"$.dispatch.{field}: must be at least 1")
        heavy_limit = dispatch.get("heavy_process_limit")
        if isinstance(heavy_limit, int) and not isinstance(heavy_limit, bool) and heavy_limit > 2:
            errors.append("$.dispatch.heavy_process_limit: repository-wide limit is 2")
        implementation_limit = dispatch.get("implementation_wip_limit")
        if isinstance(implementation_limit, int) and not isinstance(implementation_limit, bool) and implementation_limit > 2:
            errors.append("$.dispatch.implementation_wip_limit: implementation WIP limit is 2")
        invariants = dispatch.get("invariants")
        if isinstance(invariants, list) and not invariants:
            errors.append("$.dispatch.invariants: must not be empty")
        elif isinstance(invariants, list):
            for invariant_index, entry in enumerate(invariants):
                if not _non_empty(entry):
                    errors.append(f"$.dispatch.invariants[{invariant_index}]: must not be empty")

    milestones = program.get("milestones")
    if not isinstance(milestones, list):
        return errors
    if not milestones:
        errors.append("$.milestones: must not be empty")
        return errors

    ids: dict[str, int] = {}
    all_work_refs: dict[str, str] = {}
    for index, milestone in enumerate(milestones):
        if not isinstance(milestone, dict):
            continue
        path = f"$.milestones[{index}]"
        milestone_id = milestone.get("id")
        if not _non_empty(milestone_id):
            errors.append(f"{path}.id: must not be empty")
        elif milestone_id in ids:
            errors.append(f"{path}.id: duplicate milestone id '{milestone_id}'")
        else:
            ids[milestone_id] = index

        for field in ("title", "objective"):
            if field in milestone and not _non_empty(milestone.get(field)):
                errors.append(f"{path}.{field}: must not be empty")

        milestone_url = milestone.get("github_milestone")
        if isinstance(milestone_url, str) and not MILESTONE_URL_RE.fullmatch(milestone_url):
            errors.append(f"{path}.github_milestone: must be a GitHub milestone URL")

        coordinator = milestone.get("coordinator_issue")
        if isinstance(coordinator, str) and not ISSUE_REF_RE.fullmatch(coordinator):
            errors.append(f"{path}.coordinator_issue: must be an issue reference such as #948")

        for field in ("work_items", "agent_completion_criteria", "human_gates"):
            value = milestone.get(field)
            if isinstance(value, list) and not value:
                errors.append(f"{path}.{field}: must not be empty")

        for field in ("agent_completion_criteria", "human_gates"):
            value = milestone.get(field)
            if isinstance(value, list):
                for value_index, entry in enumerate(value):
                    if not _non_empty(entry):
                        errors.append(f"{path}.{field}[{value_index}]: must not be empty")

        work_items = milestone.get("work_items")
        if isinstance(work_items, list):
            for work_index, work_item in enumerate(work_items):
                if not isinstance(work_item, dict):
                    continue
                work_path = f"{path}.work_items[{work_index}]"
                ref = work_item.get("ref")
                if isinstance(ref, str):
                    if not ISSUE_REF_RE.fullmatch(ref):
                        errors.append(f"{work_path}.ref: must be an issue/PR reference such as #891")
                    elif ref in all_work_refs:
                        errors.append(
                            f"{work_path}.ref: duplicate work item {ref}; already owned by {all_work_refs[ref]}"
                        )
                    else:
                        all_work_refs[ref] = str(milestone_id)
                if not _non_empty(work_item.get("purpose")):
                    errors.append(f"{work_path}.purpose: must not be empty")

    # Dependencies are deliberately ordered: a dispatcher can scan the manifest
    # top-to-bottom and never encounter an undeclared/moving future dependency.
    for index, milestone in enumerate(milestones):
        if not isinstance(milestone, dict):
            continue
        path = f"$.milestones[{index}].depends_on"
        milestone_id = milestone.get("id")
        dependencies = milestone.get("depends_on")
        if not isinstance(dependencies, list):
            continue
        seen: set[str] = set()
        for dependency in dependencies:
            # Shape errors are already reported by the schema validator. Keep
            # this semantic pass total over malformed input instead of using a
            # possibly unhashable value in the dependency graph.
            if not isinstance(dependency, str):
                continue
            if dependency in seen:
                errors.append(f"{path}: duplicate dependency '{dependency}'")
                continue
            seen.add(dependency)
            if dependency == milestone_id:
                errors.append(f"{path}: milestone cannot depend on itself")
            elif dependency not in ids:
                errors.append(f"{path}: unknown milestone '{dependency}'")
            elif ids[dependency] >= index:
                errors.append(f"{path}: dependency '{dependency}' must appear before '{milestone_id}'")

    return errors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True, help="Path to the milestone program JSON.")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA, help="Path to program.schema.json.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        program = json.loads(args.program.read_text(encoding="utf-8"))
        schema = json.loads(args.schema.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"invalid program input: {exc}", file=sys.stderr)
        return 1

    errors = validate_program(program, schema)
    if isinstance(program, dict):
        snapshot = program.get("planning_snapshot_sha")
        if isinstance(snapshot, str) and SHA_RE.fullmatch(snapshot) and not _git_commit_exists(snapshot):
            errors.append("$.planning_snapshot_sha does not resolve to a commit")
    if errors:
        print("Program is INVALID:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("Program is valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
