#!/usr/bin/env python3
"""Pinned defining constraints for the agentic milestone-program schema."""

from __future__ import annotations

from typing import Any


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


def validate_program_schema_contract(schema: dict[str, Any]) -> list[str]:
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
        declared_required: set[str] = set()
        if not isinstance(required, list):
            errors.append(f"{label}.required: must be an array of unique strings")
        else:
            required_strings = [entry for entry in required if isinstance(entry, str)]
            if len(required_strings) != len(required):
                errors.append(f"{label}.required: entries must be strings")
            if len(set(required_strings)) != len(required_strings):
                errors.append(f"{label}.required: entries must be unique")
            declared_required = set(required_strings)
        missing_required = expected_required - declared_required
        if missing_required:
            errors.append(f"{label}.required: missing {sorted(missing_required)}")

        properties = node.get("properties")
        declared_properties = set(properties) if isinstance(properties, dict) else set()
        missing_properties = expected_required - declared_properties
        if missing_properties:
            errors.append(f"{label}.properties: missing {sorted(missing_properties)}")
        unexpected_properties = declared_properties - expected_required
        if unexpected_properties:
            errors.append(f"{label}.properties: unexpected {sorted(unexpected_properties)}")

    for label, path, expected in PROGRAM_SCHEMA_VALUE_CONTRACTS:
        actual = _nested_value(schema, path)
        if actual != expected:
            errors.append(f"{label}: expected {expected!r}, got {actual!r}")
    return errors
