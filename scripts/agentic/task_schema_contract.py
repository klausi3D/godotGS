#!/usr/bin/env python3
"""Pinned defining constraints for the agentic task-contract schema."""

from __future__ import annotations

from typing import Any


REQUIRED_ROOT_PROPERTIES = {
    "schema_version",
    "task_id",
    "github_issue",
    "title",
    "baseline_sha",
    "risk_class",
    "owned_paths",
    "forbidden_paths",
    "dependencies",
    "problem_statement",
    "non_goals",
    "invariants",
    "acceptance_criteria",
    "validation_commands",
    "evidence_requirements",
    "rollback_plan",
}
OPTIONAL_ROOT_PROPERTIES = {"design_record", "stacked_on"}
ALL_ROOT_PROPERTIES = REQUIRED_ROOT_PROPERTIES | OPTIONAL_ROOT_PROPERTIES

STRING_PROPERTIES = {
    "task_id",
    "github_issue",
    "title",
    "baseline_sha",
    "problem_statement",
    "rollback_plan",
    "design_record",
}
STRING_ARRAY_PROPERTIES = {
    "owned_paths",
    "forbidden_paths",
    "dependencies",
    "non_goals",
    "invariants",
    "acceptance_criteria",
    "validation_commands",
    "evidence_requirements",
}


def _require_exact_string_list(value: Any, expected: set[str], path: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(value, list):
        return [f"{path}: must be an array of unique strings"]
    if any(not isinstance(entry, str) for entry in value):
        errors.append(f"{path}: entries must be strings")
    strings = [entry for entry in value if isinstance(entry, str)]
    if len(set(strings)) != len(strings):
        errors.append(f"{path}: entries must be unique")
    actual = set(strings)
    if missing := expected - actual:
        errors.append(f"{path}: missing {sorted(missing)}")
    if unexpected := actual - expected:
        errors.append(f"{path}: unexpected {sorted(unexpected)}")
    return errors


def _require_exact_properties(value: Any, expected: set[str], path: str) -> list[str]:
    if not isinstance(value, dict):
        return [f"{path}: must be an object"]
    actual = set(value)
    errors: list[str] = []
    if missing := expected - actual:
        errors.append(f"{path}: missing {sorted(missing)}")
    if unexpected := actual - expected:
        errors.append(f"{path}: unexpected {sorted(unexpected)}")
    return errors


def _require_value(properties: Any, name: str, key: str, expected: Any) -> list[str]:
    property_schema = properties.get(name) if isinstance(properties, dict) else None
    actual = property_schema.get(key) if isinstance(property_schema, dict) else None
    if actual != expected:
        return [f"$.properties.{name}.{key}: expected {expected!r}, got {actual!r}"]
    return []


def _reject_unexpected_keywords(value: Any, allowed: set[str], path: str) -> list[str]:
    if not isinstance(value, dict):
        return []
    unexpected = set(value) - allowed
    return [f"{path}: unexpected schema keywords {sorted(unexpected)}"] if unexpected else []


def validate_task_schema_contract(schema: Any) -> list[str]:
    """Reject task schemas that weaken or malformedly redefine dispatch contracts."""
    if not isinstance(schema, dict):
        return ["$: schema must be an object"]

    errors: list[str] = []
    if schema.get("type") != "object":
        errors.append("$.type: must be 'object'")
    if schema.get("additionalProperties") is not False:
        errors.append("$.additionalProperties: must be false")
    errors.extend(
        _reject_unexpected_keywords(
            schema,
            {"$schema", "$id", "title", "description", "type", "additionalProperties", "required", "properties"},
            "$",
        )
    )
    errors.extend(_require_exact_string_list(schema.get("required"), REQUIRED_ROOT_PROPERTIES, "$.required"))

    properties = schema.get("properties")
    errors.extend(_require_exact_properties(properties, ALL_ROOT_PROPERTIES, "$.properties"))
    errors.extend(_require_value(properties, "schema_version", "type", "integer"))
    errors.extend(_require_value(properties, "schema_version", "const", 1))
    schema_version = properties.get("schema_version") if isinstance(properties, dict) else None
    errors.extend(_reject_unexpected_keywords(schema_version, {"type", "const"}, "$.properties.schema_version"))
    errors.extend(_require_value(properties, "risk_class", "type", "string"))
    errors.extend(_require_value(properties, "risk_class", "enum", ["R0", "R1", "R2", "R3"]))
    risk_class = properties.get("risk_class") if isinstance(properties, dict) else None
    errors.extend(_reject_unexpected_keywords(risk_class, {"type", "enum"}, "$.properties.risk_class"))

    for name in sorted(STRING_PROPERTIES):
        errors.extend(_require_value(properties, name, "type", "string"))
        property_schema = properties.get(name) if isinstance(properties, dict) else None
        errors.extend(
            _reject_unexpected_keywords(property_schema, {"type", "description"}, f"$.properties.{name}")
        )
    for name in sorted(STRING_ARRAY_PROPERTIES):
        errors.extend(_require_value(properties, name, "type", "array"))
        property_schema = properties.get(name) if isinstance(properties, dict) else None
        errors.extend(
            _reject_unexpected_keywords(
                property_schema,
                {"type", "description", "items"},
                f"$.properties.{name}",
            )
        )
        items = property_schema.get("items") if isinstance(property_schema, dict) else None
        errors.extend(_reject_unexpected_keywords(items, {"type"}, f"$.properties.{name}.items"))
        item_type = items.get("type") if isinstance(items, dict) else None
        if item_type != "string":
            errors.append(f"$.properties.{name}.items.type: expected 'string', got {item_type!r}")

    stacked = properties.get("stacked_on") if isinstance(properties, dict) else None
    if not isinstance(stacked, dict):
        errors.append("$.properties.stacked_on: must define an object schema")
    else:
        errors.extend(
            _reject_unexpected_keywords(
                stacked,
                {"type", "description", "additionalProperties", "properties"},
                "$.properties.stacked_on",
            )
        )
        if stacked.get("type") != "object":
            errors.append("$.properties.stacked_on.type: must be 'object'")
        if stacked.get("additionalProperties") is not False:
            errors.append("$.properties.stacked_on.additionalProperties: must be false")
        stacked_properties = stacked.get("properties")
        errors.extend(
            _require_exact_properties(
                stacked_properties,
                {"base_pr", "base_sha"},
                "$.properties.stacked_on.properties",
            )
        )
        for name in ("base_pr", "base_sha"):
            property_schema = stacked_properties.get(name) if isinstance(stacked_properties, dict) else None
            errors.extend(
                _reject_unexpected_keywords(
                    property_schema,
                    {"type", "description"},
                    f"$.properties.stacked_on.properties.{name}",
                )
            )
            actual = property_schema.get("type") if isinstance(property_schema, dict) else None
            if actual != "string":
                errors.append(
                    f"$.properties.stacked_on.properties.{name}.type: expected 'string', got {actual!r}"
                )
    return errors
