#!/usr/bin/env python3
"""Check a pull request's task contract against the agentic policy.

Validates the contract against ``.agentic/schemas/task.schema.json`` and enforces
the rules a schema cannot express:

* required fields are present **and non-empty**;
* the author's declared ``risk_class`` is cross-checked against the risk class
  derived from the changed paths (``classify_change``) - the **higher** class
  wins, so a PR cannot under-declare its risk;
* the effective risk class's policy requirements (rollback plan, evidence) are met;
* a stacked PR declares both its base PR and base SHA.

Exit code is non-zero if the contract is non-compliant.

Examples
--------
    python scripts/agentic/check_pr_contract.py --contract task.json --base-ref master
    python scripts/agentic/check_pr_contract.py --contract task.json --paths modules/gaussian_splatting/renderer/x.cpp
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_POLICY = ROOT / ".agentic" / "policy.json"
DEFAULT_TASK_SCHEMA = ROOT / ".agentic" / "schemas" / "task.schema.json"

# Fields that must be present AND non-empty (beyond schema presence checks).
NON_EMPTY_STRING_FIELDS = ("task_id", "github_issue", "title", "baseline_sha", "rollback_plan", "problem_statement")
NON_EMPTY_ARRAY_FIELDS = ("owned_paths", "invariants", "acceptance_criteria", "validation_commands")


def _load_sibling(name: str):
    path = Path(__file__).with_name(f"{name}.py")
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


classify_change = _load_sibling("classify_change")
validate_review = _load_sibling("validate_review")


def check_contract_document(contract: Any, task_schema: dict[str, Any]) -> list[str]:
    """Validate the schema and schema-independent task-contract semantics."""
    errors = validate_review.validate_instance(contract, task_schema, "$")
    if not isinstance(contract, dict):
        return errors

    for field in NON_EMPTY_STRING_FIELDS:
        value = contract.get(field)
        if isinstance(value, str) and not value.strip():
            errors.append(f"$.{field}: must not be empty")
    for field in NON_EMPTY_ARRAY_FIELDS:
        value = contract.get(field)
        if isinstance(value, list):
            if not value:
                errors.append(f"$.{field}: must not be empty")
            for index, entry in enumerate(value):
                if not isinstance(entry, str) or not entry.strip():
                    errors.append(f"$.{field}[{index}]: must be a non-blank string")
    return errors


def check_contract(
    contract: dict[str, Any],
    policy: dict[str, Any],
    task_schema: dict[str, Any],
    changed_paths: list[str] | None,
) -> list[str]:
    errors = check_contract_document(contract, task_schema)

    ordering = policy["classification"]["ordering"]
    rank = {cls: index for index, cls in enumerate(ordering)}
    declared = contract.get("risk_class")

    # 3. Cross-check declared risk class against the diff.
    effective = declared
    if changed_paths is not None:
        computed, _ = classify_change.classify_paths(changed_paths, policy)
        if declared in rank and computed in rank:
            if rank[computed] > rank[declared]:
                errors.append(
                    f"$.risk_class: declared {declared} understates computed {computed} "
                    f"from the changed paths; use the higher class"
                )
                effective = computed

    # 3b. Enforce scope: changed paths must stay inside owned_paths and never match
    # forbidden_paths (the implementer constraint the contract is meant to enforce).
    # Only string globs/paths are considered; malformed non-string entries are caught
    # by the schema check above and must not crash this gate.
    if changed_paths is not None:
        owned = [g for g in (contract.get("owned_paths") or []) if isinstance(g, str)]
        forbidden = [g for g in (contract.get("forbidden_paths") or []) if isinstance(g, str)]
        for raw in changed_paths:
            if not isinstance(raw, str):
                continue
            norm = classify_change._norm(raw)
            if any(classify_change._matches(norm, classify_change._norm(g)) for g in forbidden):
                errors.append(f"$.forbidden_paths: changed path '{norm}' matches a forbidden path")
            elif owned and not any(classify_change._matches(norm, classify_change._norm(g)) for g in owned):
                errors.append(f"$.owned_paths: changed path '{norm}' is outside the declared owned_paths")

    # 4. Effective-class policy requirements.
    class_policy = policy.get("risk_classes", {}).get(effective)
    if class_policy:
        if class_policy.get("rollback_required") and not str(contract.get("rollback_plan", "")).strip():
            errors.append(f"$.rollback_plan: required for risk class {effective}")
        # The contract must carry the class's policy evidence items, not just any
        # non-empty list (otherwise an R2/R3 task could pass with ['n/a']).
        declared_evidence = {e for e in (contract.get("evidence_requirements") or []) if isinstance(e, str)}
        for item in class_policy.get("evidence_requirements", []):
            if item not in declared_evidence:
                errors.append(f"$.evidence_requirements: risk class {effective} requires evidence item: '{item}'")
        if class_policy.get("adr_required") and not str(contract.get("design_record", "")).strip():
            errors.append(
                f"$.design_record: risk class {effective} requires a linked design record "
                f"(ADR / design-change issue) before implementation"
            )
        # The contract must commit to actually running the class's deterministic
        # checks; otherwise a high-risk task could declare validation_commands: ["true"].
        declared_cmds = {c for c in (contract.get("validation_commands") or []) if isinstance(c, str)}
        for command in class_policy.get("deterministic_checks", []):
            if command not in declared_cmds:
                errors.append(
                    f"$.validation_commands: risk class {effective} requires the deterministic "
                    f"check '{command}'"
                )

    # 5. Stacked PR completeness.
    stacked = contract.get("stacked_on")
    if isinstance(stacked, dict):
        for field in ("base_pr", "base_sha"):
            if not str(stacked.get(field, "")).strip():
                errors.append(f"$.stacked_on.{field}: required for a stacked PR")

    return errors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--contract", type=Path, required=True, help="Path to the task contract JSON.")
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY, help="Path to policy.json.")
    parser.add_argument("--task-schema", type=Path, default=DEFAULT_TASK_SCHEMA, help="Path to task.schema.json.")
    parser.add_argument("--paths", nargs="*", help="Explicit changed paths for the risk cross-check.")
    parser.add_argument("--base-ref", help="Git ref to diff HEAD against for the risk cross-check.")
    parser.add_argument(
        "--schema-only",
        action="store_true",
        help="Validate the contract document alone, WITHOUT the risk/scope cross-check. "
        "Not a full PR-gate check; must be requested explicitly.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    policy = classify_change.load_policy(args.policy)
    with open(args.task_schema, encoding="utf-8") as handle:
        task_schema = json.load(handle)
    contract = json.loads(args.contract.read_text(encoding="utf-8"))

    changed_paths: list[str] | None
    if args.paths is not None:
        changed_paths = args.paths
    elif args.base_ref:
        changed_paths = classify_change.git_changed_paths(args.base_ref)
    elif args.schema_only:
        # Explicit opt-out: validate the document only (no risk/scope enforcement).
        changed_paths = None
    else:
        print(
            "error: a full PR-gate check needs the diff — pass --paths or --base-ref "
            "(or --schema-only to validate the contract document without the risk/scope "
            "cross-check)",
            file=sys.stderr,
        )
        return 2

    errors = check_contract(contract, policy, task_schema, changed_paths)

    # "note:" lines are advisory; treat only hard errors as failures.
    hard_errors = [error for error in errors if not error.startswith("note:")]
    notes = [error for error in errors if error.startswith("note:")]

    for note in notes:
        print(note)
    if hard_errors:
        print("PR contract is NON-COMPLIANT:")
        for error in hard_errors:
            print(f"  - {error}")
        return 1

    print("PR contract is compliant.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
