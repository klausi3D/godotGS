---
name: gs-change-evidence
description: Prepare a GodotGS change for review. Covers deriving the risk class, deciding which checks and runtime/GPU evidence it needs, writing honest PR notes, and avoiding evidence-integrity traps. Use before committing, opening a PR, or claiming that a check, perf gain or fix is proven.
---

# Change evidence and PR preparation

Canonical sources. Open the named section only:

- `docs/governance/agentic-engineering.md`, "Risk classes" and "Stacked PRs"
- `docs/governance/evidence-integrity.md`, "The question to ask…" and
  "Required practices". Read these before writing or reviewing a guard.
- `docs/governance/contribution-standards.md`, a 36-line PR checklist
- `.agentic/policy.json`, `risk_classes.<R*>.deterministic_checks` and
  `evidence_requirements`

## Steps

1. **Anchor.** Record the base SHA you branched from
   (`git merge-base HEAD origin/master`). Stacked PRs state their base PR and base SHA.
2. **Classify.** Run `python scripts/agentic/classify_change.py --base-ref <base-sha>`.
   The derived class is the enforced one. Unlisted paths fail closed to R3.
3. **Run that class's `deterministic_checks`.** The commands are listed in
   `.agentic/policy.json` (see the `gs-build-test` skill for lanes and output control).
4. **Collect evidence by class:**
   - R1: the executed commands and their results.
   - R2: runtime/GPU harness or benchmark evidence against the immutable base. Add
     VRAM and frame-time numbers for any performance or memory claim. Rendering-math
     changes also need real-scan visual validation.
   - R3: a design record (ADR or issue) before implementation, back-compat evidence,
     and two reviews plus human approval.
5. **Prove that each new check can fail.** Revert the fix and show RED, restore it and
   show GREEN. Also delete the check's wiring once and show RED.
6. **Write the PR notes:**
   - scope
   - base SHA
   - derived class
   - the exact commands with their outcomes
   - blind spots: vendors or hardware not covered, lanes "not run"

## Honesty rules (short form)

- "Did not run", "found nothing to check" and "passed" are three different results.
  Report them differently.
- A metric measures what its name says. CPU dispatch time is not GPU time.
- Never edit a baseline, threshold or gate manifest to get green.
- Do not commit transcripts, session IDs, logs, `tests/output/` or local task
  instances.
