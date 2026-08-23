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


def _load_validate_instance():
    path = Path(__file__).with_name("validate_review.py")
    spec = importlib.util.spec_from_file_location("validate_review", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_instance


validate_instance = _load_validate_instance()


def _load_validate_program():
    path = Path(__file__).with_name("validate_program.py")
    spec = importlib.util.spec_from_file_location("validate_program", path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.validate_program


validate_program = _load_validate_program()


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
    if program_dir.is_dir():
        json_files.extend(str(path.relative_to(root)).replace("\\", "/") for path in sorted(program_dir.glob("*.json")))
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
        (".agentic/templates/program.json", ".agentic/schemas/program.schema.json"),
    ]
    for template_rel, schema_rel in pairs:
        if template_rel in parsed and schema_rel in parsed:
            for error in validate_instance(parsed[template_rel], parsed[schema_rel], "$"):
                errors.append(f"{template_rel} does not match {schema_rel}: {error}")

    program_schema = parsed.get(".agentic/schemas/program.schema.json")
    if isinstance(program_schema, dict):
        for rel, instance in parsed.items():
            if not rel.startswith(".agentic/programs/") or not isinstance(instance, dict):
                continue
            for error in validate_program(instance, program_schema):
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
