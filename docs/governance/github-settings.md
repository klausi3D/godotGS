# GitHub Repository Settings

These are the repository rules that back the [review policy](review-policy.md).
They must be configured **manually** by a maintainer in the GitHub UI / rulesets —
nothing in this repo changes them automatically, so this page is hand-written and
can drift from the live API. It is therefore split into what has been **observed
live** and what is still **intended**.

## Live state for `master` (observed 2026-08-14)

Read back with `gh api repos/klausi3D/godotGS/branches/master/protection`, twice and
byte-identical; the required context was additionally confirmed to be a real,
completed check-run on PR #881, so it is not a phantom name in the settings.

| Setting | Live value |
| --- | --- |
| Required status checks | `["agentic-pr-gate"]` — exactly one context |
| Require branches up to date (`strict`) | `false` |
| Enforce for administrators | `true` |
| Require conversation resolution | `true` |
| Required approving reviews | `0` |
| Force pushes / branch deletion | blocked |
| Rulesets | none |

Earlier revisions of this page said `master` had **no required status checks** and
that `agentic-pr-gate` was merely intended. That was false, and stale in the
dangerous direction: it invited "nothing is enforced anyway" reasoning
(`GS-AUDIT-DOC-003`).

Two consequences worth stating plainly:

- `agentic-pr-gate` is the **only** required check. Every GPU, runtime, visual and
  release lane is advisory at the merge boundary.
- Because `enforce_admins` is `true`, a broken edit to
  `.github/workflows/agentic_pr_gate.yml` blocks **every** merge in this repository,
  including the fix for itself. Change that workflow in the smallest possible
  increments, and confirm the gate is green on the PR that changes it.

### What the required gate does and does not enforce

- **Enforced.** The agentic control plane is consistent *including* the AGENTS.md
  hierarchy and `docs/governance/*` (`validate_repo_contract.py --strict-hierarchy`);
  the `tests/agentic` suite; the documentation link check; the GPU-free
  `run_module_tests.py --guard-only` lane; and the **risk class derived from the
  PR's own diff** — an unresolvable base ref fails the check instead of degrading to
  a different diff, and an empty changed-path set classifies as
  `classification.default_unclassified` (R3), not R0.
- **Published, not enforced.** The derived class's `evidence_requirements` and
  `deterministic_checks` are written to the job summary so the merging human sees,
  for example, that an R2 change owes runtime/GPU evidence. CI does **not** verify
  that the evidence was produced.
- **Not enforced at all.** Per-PR **scope** (`owned_paths` / `forbidden_paths`) and
  evidence contracts. `check_pr_contract.py` runs in CI only as a self-test against
  the shipped fixture `.agentic/templates/task.json`, because this repository has no
  per-PR contract source: a task-contract instance is a local agent artifact, and
  [`AGENTS.md`](../../AGENTS.md) forbids committing those. Choosing a source — a
  PR-body block or a committed contract file — is the **Phase-2 contract-source
  ADR**, tracked separately and deliberately not part of the change that closed the
  gaps above.

## Settings as code (Phase 2)

This page is hand-written, which is exactly how `GS-AUDIT-DOC-003` happened: the
protection was configured after the doc was written and nothing derived the claim
from the live API. Exporting branch protection as code, or adding a scheduled job
that diffs the live API against this page, is a deliberate **Phase-2** item and is
not part of the change that corrected these claims. Until it exists, treat
`gh api repos/<owner>/<repo>/branches/master/protection` as the source of truth and
this page as a dated observation.

## Branch protection / ruleset for `master` — still intended

These are **not** live as of the observation above.

- **Require a pull request before merging** — no direct pushes to `master`.
- **Require at least one approving review from a human maintainer**
  (`required_approving_review_count` is `0` today).
- **Require review from Code Owners** (`.github/CODEOWNERS`). Enable this only
  **after** `.github/CODEOWNERS` has merged; with no owners defined the setting
  cannot request anyone and the R3 escalation below stays unenforced.
- **Dismiss stale approvals** when new commits are pushed.
- **Require branches to be up to date** before merging (or use the merge queue);
  `strict` is `false` today.

Already live, kept here so the intended set stays complete: required status check
`agentic-pr-gate` (shown in the PR UI as `Agentic PR Gate / agentic-pr-gate`),
required conversation resolution, admin enforcement, and blocked force
pushes/deletions.

## Risk-class escalation

- **R3 changes** (Godot-engine delta outside the module, persistence/file formats,
  release/security workflows, public API/compat) require **two approvals** and a
  design record (ADR / design-change issue) before implementation. Enforce via a
  CODEOWNERS ownership of the sensitive paths plus a documented reviewer
  expectation; GitHub rulesets cannot encode "risk class" directly, so the
  required second approval for R3 is a maintainer-enforced convention checked at
  review time.

## Runner trust boundary

- Keep the fork guard on every self-hosted job (see
  `.github/workflows/README.md`). Do not add a ruleset or automation that runs
  fork PR code on a persistent self-hosted runner, and never enable a
  `pull_request_target` privileged checkout.

## Emergency bypass

- Repository admins may bypass protection only for a genuine emergency. Any bypass
  must be recorded (PR comment or incident note) with the reason and a follow-up
  to restore normal flow. Bypass is never the routine path.

## Notes

- Required-check names must match the job's reported check name exactly; if the
  gate's workflow/job name changes, update the required-checks list here and in the
  ruleset.
