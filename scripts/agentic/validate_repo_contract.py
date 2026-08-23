#!/usr/bin/env python3
"""Validate that the agentic control plane is internally consistent.

Checks (against ``--root``, default: repository root):

* all required ``.agentic/`` and ``scripts/agentic/`` control-plane files exist;
* the JSON files parse;
* the templates validate against their schemas;
* every role referenced by ``policy.json`` exists as a role file and is listed in
  ``policy.json``'s ``roles``;
* the classification rules reference only known classes;
* no session-id / transcript artifacts have leaked into ``.agentic/``.

By default this validates only the self-contained control plane, so it passes on a
branch that adds ``.agentic/`` + ``scripts/agentic/`` without the wider AGENTS.md /
governance-doc hierarchy. Pass ``--strict-hierarchy`` to additionally require the
``AGENTS.md`` files and ``docs/governance/`` docs (use on a fully merged tree).

Exit code is non-zero if anything is inconsistent.
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

# Self-contained control plane: always required.
CONTROL_PLANE_FILES = [
    ".agentic/README.md",
    ".agentic/policy.json",
    ".agentic/ownership.json",
    ".agentic/schemas/task.schema.json",
    ".agentic/schemas/review.schema.json",
    ".agentic/schemas/program.schema.json",
    ".agentic/templates/task.json",
    ".agentic/templates/review.json",
    ".agentic/templates/program.json",
    ".agentic/roles/planner.md",
    ".agentic/roles/implementer.md",
    ".agentic/roles/verifier.md",
    ".agentic/roles/correctness-reviewer.md",
    ".agentic/roles/gpu-performance-reviewer.md",
    "scripts/agentic/classify_change.py",
    "scripts/agentic/check_pr_contract.py",
    "scripts/agentic/validate_review.py",
    "scripts/agentic/validate_program.py",
    "scripts/agentic/validate_repo_contract.py",
]

# Wider hierarchy: only required under --strict-hierarchy (a fully merged tree).
HIERARCHY_FILES = [
    "AGENTS.md",
    "modules/gaussian_splatting/AGENTS.md",
    "modules/gaussian_splatting/renderer/AGENTS.md",
    "modules/gaussian_splatting/shaders/AGENTS.md",
    "tests/AGENTS.md",
    ".github/workflows/AGENTS.md",
    "docs/governance/agentic-engineering.md",
    "docs/governance/review-policy.md",
    "docs/governance/github-settings.md",
]

JSON_FILES = [
    ".agentic/policy.json",
    ".agentic/ownership.json",
    ".agentic/schemas/task.schema.json",
    ".agentic/schemas/review.schema.json",
    ".agentic/schemas/program.schema.json",
    ".agentic/templates/task.json",
    ".agentic/templates/review.json",
    ".agentic/templates/program.json",
]

# Concrete session-id / agent-session UUID format (as used by the legacy
# coordinator memory, e.g. "019d0571-b295-..."). Prose like "session IDs" is fine;
# this matches an actual leaked identifier value.
SESSION_ID_RE = re.compile(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b")

PROGRAM_SCHEMA_OBJECT_CONTRACTS = (
    (
        "$",
        (),
        {
            "schema_version",
            "program_id",
            "title",
            "planning_snapshot_sha",
            "status_authority",
            "dispatch",
            "milestones",
        },
    ),
    (
        "$.dispatch",
        ("properties", "dispatch"),
        {
            "task_contract_template",
            "implementation_wip_limit",
            "heavy_process_limit",
            "live_status_requery_required",
            "invariants",
        },
    ),
    (
        "$.milestones.items",
        ("properties", "milestones", "items"),
        {
            "id",
            "title",
            "github_milestone",
            "coordinator_issue",
            "objective",
            "depends_on",
            "work_items",
            "agent_completion_criteria",
            "human_gates",
        },
    ),
    (
        "$.milestones.items.work_items.items",
        ("properties", "milestones", "items", "properties", "work_items", "items"),
        {"kind", "ref", "purpose"},
    ),
)

_ROOT_PROPERTIES = ("properties",)
_DISPATCH_PROPERTIES = _ROOT_PROPERTIES + ("dispatch", "properties")
_MILESTONE_PROPERTIES = _ROOT_PROPERTIES + ("milestones", "items", "properties")
_WORK_ITEM_PROPERTIES = _MILESTONE_PROPERTIES + ("work_items", "items", "properties")

PROGRAM_SCHEMA_VALUE_CONTRACTS = (
    ("$.schema_version.type", _ROOT_PROPERTIES + ("schema_version", "type"), "integer"),
    ("$.schema_version.const", _ROOT_PROPERTIES + ("schema_version", "const"), 1),
    ("$.program_id.type", _ROOT_PROPERTIES + ("program_id", "type"), "string"),
    ("$.title.type", _ROOT_PROPERTIES + ("title", "type"), "string"),
    ("$.planning_snapshot_sha.type", _ROOT_PROPERTIES + ("planning_snapshot_sha", "type"), "string"),
    ("$.status_authority.type", _ROOT_PROPERTIES + ("status_authority", "type"), "string"),
    ("$.dispatch.task_contract_template.type", _DISPATCH_PROPERTIES + ("task_contract_template", "type"), "string"),
    (
        "$.dispatch.implementation_wip_limit.type",
        _DISPATCH_PROPERTIES + ("implementation_wip_limit", "type"),
        "integer",
    ),
    ("$.dispatch.heavy_process_limit.type", _DISPATCH_PROPERTIES + ("heavy_process_limit", "type"), "integer"),
    (
        "$.dispatch.live_status_requery_required.type",
        _DISPATCH_PROPERTIES + ("live_status_requery_required", "type"),
        "boolean",
    ),
    (
        "$.dispatch.live_status_requery_required.const",
        _DISPATCH_PROPERTIES + ("live_status_requery_required", "const"),
        True,
    ),
    ("$.dispatch.invariants.type", _DISPATCH_PROPERTIES + ("invariants", "type"), "array"),
    ("$.dispatch.invariants.items.type", _DISPATCH_PROPERTIES + ("invariants", "items", "type"), "string"),
    ("$.milestones.type", _ROOT_PROPERTIES + ("milestones", "type"), "array"),
    ("$.milestones.items.id.type", _MILESTONE_PROPERTIES + ("id", "type"), "string"),
    ("$.milestones.items.title.type", _MILESTONE_PROPERTIES + ("title", "type"), "string"),
    ("$.milestones.items.github_milestone.type", _MILESTONE_PROPERTIES + ("github_milestone", "type"), "string"),
    ("$.milestones.items.coordinator_issue.type", _MILESTONE_PROPERTIES + ("coordinator_issue", "type"), "string"),
    ("$.milestones.items.objective.type", _MILESTONE_PROPERTIES + ("objective", "type"), "string"),
    ("$.milestones.items.depends_on.type", _MILESTONE_PROPERTIES + ("depends_on", "type"), "array"),
    ("$.milestones.items.depends_on.items.type", _MILESTONE_PROPERTIES + ("depends_on", "items", "type"), "string"),
    ("$.milestones.items.work_items.type", _MILESTONE_PROPERTIES + ("work_items", "type"), "array"),
    (
        "$.milestones.items.agent_completion_criteria.type",
        _MILESTONE_PROPERTIES + ("agent_completion_criteria", "type"),
        "array",
    ),
    (
        "$.milestones.items.agent_completion_criteria.items.type",
        _MILESTONE_PROPERTIES + ("agent_completion_criteria", "items", "type"),
        "string",
    ),
    ("$.milestones.items.human_gates.type", _MILESTONE_PROPERTIES + ("human_gates", "type"), "array"),
    ("$.milestones.items.human_gates.items.type", _MILESTONE_PROPERTIES + ("human_gates", "items", "type"), "string"),
    ("$.milestones.items.work_items.items.kind.type", _WORK_ITEM_PROPERTIES + ("kind", "type"), "string"),
    (
        "$.milestones.items.work_items.items.kind.enum",
        _WORK_ITEM_PROPERTIES + ("kind", "enum"),
        ["issue", "pull_request", "design"],
    ),
    ("$.milestones.items.work_items.items.ref.type", _WORK_ITEM_PROPERTIES + ("ref", "type"), "string"),
    ("$.milestones.items.work_items.items.purpose.type", _WORK_ITEM_PROPERTIES + ("purpose", "type"), "string"),
)


def _nested_value(value: Any, path: tuple[str, ...]) -> Any:
    current = value
    for key in path:
        if not isinstance(current, dict) or key not in current:
            return None
        current = current[key]
    return current


def _validate_program_schema_contract(schema: dict[str, Any]) -> list[str]:
    """Pin the program schema constraints that make manifests enforceable."""
    errors: list[str] = []
    for label, path, expected_required in PROGRAM_SCHEMA_OBJECT_CONTRACTS:
        node = _nested_value(schema, path)
        if not isinstance(node, dict):
            errors.append(f"{label}: must define an object schema")
            continue
        if node.get("type") != "object":
            errors.append(f"{label}.type: must be 'object'")
        if node.get("additionalProperties") is not False:
            errors.append(f"{label}.additionalProperties: must be false")

        required = node.get("required")
        declared_required = (
            {entry for entry in required if isinstance(entry, str)}
            if isinstance(required, list)
            else set()
        )
        missing_required = expected_required - declared_required
        if missing_required:
            errors.append(f"{label}.required: missing {sorted(missing_required)}")

        properties = node.get("properties")
        missing_properties = (
            expected_required - set(properties) if isinstance(properties, dict) else expected_required
        )
        if missing_properties:
            errors.append(f"{label}.properties: missing {sorted(missing_properties)}")

    for label, path, expected in PROGRAM_SCHEMA_VALUE_CONTRACTS:
        actual = _nested_value(schema, path)
        if actual != expected:
            errors.append(f"{label}: expected {expected!r}, got {actual!r}")
    return errors


def _git_commit_exists(root: Path, sha: str) -> bool:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "cat-file", "-e", f"{sha}^{{commit}}"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return completed.returncode == 0


def _load_validate_instance():
    path = Path(__file__).with_name("validate_review.py")
    spec = importlib.util.spec_from_file_location("validate_review", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_instance


validate_instance = _load_validate_instance()


def _load_program_validators():
    path = Path(__file__).with_name("validate_program.py")
    spec = importlib.util.spec_from_file_location("validate_program", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_program, module.validate_repository_references


validate_program, validate_repository_references = _load_program_validators()


def validate_repo_contract(root: Path, strict_hierarchy: bool = False) -> list[str]:
    errors: list[str] = []

    # 1. Required files exist (control plane always; hierarchy only when strict).
    required = list(CONTROL_PLANE_FILES)
    if strict_hierarchy:
        required += HIERARCHY_FILES
    for rel in required:
        if not (root / rel).is_file():
            errors.append(f"missing required file: {rel}")

    # 2. JSON files parse.
    parsed: dict[str, Any] = {}
    json_files = list(JSON_FILES)
    program_dir = root / ".agentic" / "programs"
    program_paths = (
        sorted(path for path in program_dir.glob("*.json") if path.is_file())
        if program_dir.is_dir()
        else []
    )
    if not program_paths:
        errors.append("missing concrete program manifest: .agentic/programs/*.json")
    json_files.extend(str(path.relative_to(root)).replace("\\", "/") for path in program_paths)
    for rel in json_files:
        path = root / rel
        if not path.is_file():
            continue
        try:
            parsed[rel] = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"invalid JSON in {rel}: {exc}")

    # 3. Templates validate against their schemas.
    pairs = [
        (".agentic/templates/task.json", ".agentic/schemas/task.schema.json"),
        (".agentic/templates/review.json", ".agentic/schemas/review.schema.json"),
    ]
    for template_rel, schema_rel in pairs:
        if template_rel in parsed and schema_rel in parsed:
            for error in validate_instance(parsed[template_rel], parsed[schema_rel], "$"):
                errors.append(f"{template_rel} does not match {schema_rel}: {error}")

    program_schema = parsed.get(".agentic/schemas/program.schema.json")
    task_schema = parsed.get(".agentic/schemas/task.schema.json")
    if ".agentic/schemas/program.schema.json" in parsed and not isinstance(program_schema, dict):
        errors.append(".agentic/schemas/program.schema.json is invalid: $: must be an object")
    if isinstance(program_schema, dict):
        for error in _validate_program_schema_contract(program_schema):
            errors.append(f".agentic/schemas/program.schema.json contract: {error}")

        program_template_rel = ".agentic/templates/program.json"
        program_template = parsed.get(program_template_rel)
        if program_template_rel in parsed:
            for error in validate_program(program_template, program_schema):
                errors.append(f"{program_template_rel} is invalid: {error}")
            for error in validate_repository_references(
                program_template,
                root,
                task_schema,
                validate_snapshot_reference=False,
            ):
                errors.append(f"{program_template_rel} is invalid: {error}")

        program_id_paths: dict[str, str] = {}
        for rel, instance in parsed.items():
            if not rel.startswith(".agentic/programs/"):
                continue
            if not isinstance(instance, dict):
                errors.append(f"{rel} is invalid: $: must be an object")
                continue
            program_id = instance.get("program_id")
            if isinstance(program_id, str):
                previous_path = program_id_paths.get(program_id)
                if previous_path is not None:
                    errors.append(
                        f"{rel} is invalid: duplicate program_id {program_id!r}; "
                        f"already declared by {previous_path}"
                    )
                else:
                    program_id_paths[program_id] = rel
            for error in validate_program(instance, program_schema):
                errors.append(f"{rel} is invalid: {error}")
            reference_errors = validate_repository_references(
                instance,
                root,
                task_schema,
                commit_exists=lambda sha: _git_commit_exists(root, sha),
            )
            for error in reference_errors:
                errors.append(f"{rel} is invalid: {error}")

    policy = parsed.get(".agentic/policy.json")
    if isinstance(policy, dict):
        roles = policy.get("roles", [])
        # 4. Each declared role has a role file.
        for role in roles:
            if not (root / ".agentic" / "roles" / f"{role}.md").is_file():
                errors.append(f"policy role '{role}' has no .agentic/roles/{role}.md")
        # 5. required_roles reference known roles.
        for cls, config in policy.get("risk_classes", {}).items():
            for role in config.get("required_roles", []):
                if role not in roles:
                    errors.append(f"risk class {cls} requires unknown role '{role}'")
        # 6. Classification references known classes.
        classification = policy.get("classification", {})
        ordering = classification.get("ordering", [])
        if classification.get("default_unclassified") not in ordering:
            errors.append("classification.default_unclassified is not in ordering")
        for rule in classification.get("rules", []):
            if rule.get("class") not in ordering:
                errors.append(f"classification rule has unknown class '{rule.get('class')}'")

    # 7. No leaked session-id / transcript artifacts under .agentic/.
    agentic_dir = root / ".agentic"
    if agentic_dir.is_dir():
        for path in sorted(agentic_dir.rglob("*")):
            if path.is_file() and path.suffix in (".md", ".json"):
                text = path.read_text(encoding="utf-8", errors="ignore")
                if SESSION_ID_RE.search(text):
                    errors.append(f"possible session-id/transcript artifact in {path.relative_to(root)}")

    return errors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT, help="Repository root to validate.")
    parser.add_argument(
        "--strict-hierarchy",
        action="store_true",
        help="Also require the AGENTS.md hierarchy and docs/governance docs (fully merged tree).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    errors = validate_repo_contract(args.root, strict_hierarchy=args.strict_hierarchy)
    if errors:
        print("Agentic control plane is INCONSISTENT:")
        for error in errors:
            print(f"  - {error}")
        return 1
    print("Agentic control plane is consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
