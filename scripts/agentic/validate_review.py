#!/usr/bin/env python3
"""Validate an agentic review result against ``.agentic/schemas/review.schema.json``.

Beyond the schema (required fields, enums), this enforces the review-policy rules
that a schema cannot express: non-empty SHAs and that every ``blocker``/``high``
finding carries a concrete ``required_action``.

Exit code is non-zero if the review is invalid.

Examples
--------
    python scripts/agentic/validate_review.py --review .agentic/templates/review.json
    cat review.json | python scripts/agentic/validate_review.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCHEMA = ROOT / ".agentic" / "schemas" / "review.schema.json"


# --------------------------------------------------------------------------- #
# Minimal JSON-Schema-lite validator (shared by the agentic tooling).
# Supports: type, const, enum, required, properties, additionalProperties:false,
# and array items. This is intentionally small; it is not a full JSON Schema
# engine, only enough for the agentic task/review contracts.
# --------------------------------------------------------------------------- #
def _type_ok(value: Any, type_name: Any) -> bool:
    if isinstance(type_name, list):
        return any(_type_ok(value, item) for item in type_name)
    if type_name == "string":
        return isinstance(value, str)
    if type_name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if type_name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if type_name == "boolean":
        return isinstance(value, bool)
    if type_name == "array":
        return isinstance(value, list)
    if type_name == "object":
        return isinstance(value, dict)
    if type_name == "null":
        return value is None
    return True


def validate_instance(instance: Any, schema: Any, path: str = "$") -> list[str]:
    errors: list[str] = []
    if not isinstance(schema, dict):
        return [f"{path}: schema must be an object"]
    type_name = schema.get("type")
    if type_name is not None and not _type_ok(instance, type_name):
        errors.append(f"{path}: expected type {type_name}, got {type(instance).__name__}")
        return errors

    if "const" in schema and instance != schema["const"]:
        errors.append(f"{path}: expected {schema['const']!r}, got {instance!r}")
    if "enum" in schema:
        enum = schema["enum"]
        if not isinstance(enum, list):
            errors.append(f"{path}: schema enum must be an array")
        elif instance not in enum:
            errors.append(f"{path}: {instance!r} is not one of {enum}")

    if isinstance(instance, dict):
        properties = schema.get("properties", {})
        if not isinstance(properties, dict):
            errors.append(f"{path}: schema properties must be an object")
            properties = {}
        required_entries = schema.get("required", [])
        if not isinstance(required_entries, list):
            errors.append(f"{path}: schema required must be an array")
            required_entries = []
        for required in required_entries:
            if not isinstance(required, str):
                errors.append(f"{path}: schema required entries must be strings")
            elif required not in instance:
                errors.append(f"{path}: missing required property '{required}'")
        if schema.get("additionalProperties", True) is False:
            for key in instance:
                if key not in properties:
                    errors.append(f"{path}: unexpected property '{key}'")
        for key, value in instance.items():
            if key in properties:
                errors.extend(validate_instance(value, properties[key], f"{path}.{key}"))

    if isinstance(instance, list) and "items" in schema:
        items = schema["items"]
        if not isinstance(items, dict):
            errors.append(f"{path}: schema items must be an object")
        else:
            for index, item in enumerate(instance):
                errors.extend(validate_instance(item, items, f"{path}[{index}]"))

    return errors


def validate_review(review: dict[str, Any], schema: dict[str, Any]) -> list[str]:
    errors = validate_instance(review, schema, "$")

    # Policy rules a schema cannot express.
    for field in ("base_sha", "head_sha"):
        value = review.get(field)
        if isinstance(value, str) and not value.strip():
            errors.append(f"$.{field}: must not be empty")

    findings = review.get("findings")
    if isinstance(findings, list):
        for index, finding in enumerate(findings):
            if not isinstance(finding, dict):
                continue
            severity = finding.get("severity")
            action = finding.get("required_action")
            if severity in ("blocker", "high") and not (isinstance(action, str) and action.strip()):
                errors.append(
                    f"$.findings[{index}]: severity '{severity}' requires a non-empty required_action"
                )
    return errors


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--review", type=Path, help="Path to the review JSON (default: read stdin).")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA, help="Path to review.schema.json.")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    with open(args.schema, encoding="utf-8") as handle:
        schema = json.load(handle)

    if args.review:
        raw = args.review.read_text(encoding="utf-8")
    else:
        raw = sys.stdin.read()

    try:
        review = json.loads(raw)
    except json.JSONDecodeError as exc:
        print(f"invalid JSON: {exc}", file=sys.stderr)
        return 1

    errors = validate_review(review, schema)
    if errors:
        print("Review is INVALID:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("Review is valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
