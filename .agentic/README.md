# `.agentic/` — Agentic Control Plane

Vendor-neutral, machine-readable definitions for how AI coding agents work in this
repository. Any agent tool can consume these files; nothing here depends on a
specific product. The human-readable narrative lives in
`docs/governance/agentic-engineering.md` and `docs/governance/review-policy.md`.

## Contents

| Path | Purpose |
| --- | --- |
| `policy.json` | Risk classes (R0–R3), required roles/checks/evidence per class, and path-based classification rules (fail-closed to R3). |
| `ownership.json` | Agent domains bound to real paths; basis for non-overlapping parallel work. |
| `schemas/task.schema.json` | Schema for a task contract (one unit of work). |
| `schemas/review.schema.json` | Schema for a structured review result. |
| `schemas/program.schema.json` | Schema for dependency-ordered milestone goals. |
| `templates/task.json` | Fillable task-contract template (validates against the schema). |
| `templates/review.json` | Example review result (validates against the schema). |
| `templates/program.json` | Minimal milestone-program example. |
| `programs/*.json` | Durable program dependencies, goal objectives, work references, and human gates. GitHub remains the live status authority. |
| `roles/*.md` | Per-role mandates and hard constraints. |

## Tooling

Validators live in `scripts/agentic/` (Python 3.11, standard library only):

- `validate_repo_contract.py` — checks this directory is internally consistent.
- `classify_change.py` — derives the risk class from changed paths.
- `check_pr_contract.py` — checks a PR's task contract, cross-checking the declared
  risk class against the diff (the higher class wins).
- `validate_review.py` — validates a review result against the review schema.
- `validate_program.py` — validates milestone goals, references, uniqueness, and dependency order.

These are exercised by tests under `tests/agentic/` and by the always-on
`Agentic PR Gate` (`.github/workflows/agentic_pr_gate.yml`). Its
`agentic-pr-gate` job is the one status check that `master` branch protection
requires. On every PR it runs:

- `validate_repo_contract.py --strict-hierarchy`;
- the `tests/agentic` suite;
- `classify_change.py` against the PR's own diff. This fails closed when the base
  cannot be resolved. The derived class is published and never fails the check.

`validate_review.py` and `check_pr_contract.py` run only against the shipped
`templates/`, as a self-test of the validators. No PR's task contract, declared
risk class or path scope is checked in CI. The full split between what is
enforced and what is process is in `docs/governance/agentic-engineering.md`.

## Rules

- These files are **canonical** and version-controlled. Never gitignore them.
- Never commit ephemeral state (session IDs, transcripts, scratch) here or
  anywhere — see `docs/governance/contribution-standards.md`.
- Program manifests never claim current issue/PR state. A dispatcher re-queries
  GitHub and creates a fresh task contract with an immutable base SHA when a work
  item is claimed.
