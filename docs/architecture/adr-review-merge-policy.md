# ADR: Review and merge policy as practised

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Risk:** this document is R0 (`docs/**`), as are the two governance edits that land with
  it. It changes no code, workflow, branch-protection setting, guard, threshold or gate.
- **Refs:** audit findings J-14 and G-04; owner decision D-7; cleanup-plan task CLN-D07.
- **Related:** [Review policy](../governance/review-policy.md),
  [Agentic engineering](../governance/agentic-engineering.md),
  [GitHub settings](../governance/github-settings.md),
  [Phase-1 guard hardening](adr-phase1-guard-hardening.md) §3 (the earlier R3 disposition
  for a sole collaborator).
- **Verified against:** `b58bbe35d1f` (origin/master). Every file claim below was read at
  that commit.

## Problem

The written review policy asks for more than the repository does.

- The R3 row of the risk-class table in
  [Agentic engineering](../governance/agentic-engineering.md#risk-classes) gave R3, before
  this record, the process "ADR before implementation + two reviews + CODEOWNER and human approval".
  `.agentic/policy.json` lists "Two independent reviews and CODEOWNER + human approval."
  under `risk_classes.R3.evidence_requirements`, and the header comment of
  `.github/CODEOWNERS` says "That rule stands;
  GitHub does not enforce it."
- Branch protection requires 0 approving reviews and no code-owner review
  ([GitHub settings](../governance/github-settings.md), the live-state table for `master`).
  The "Repository enforcement" section of
  [Review policy](../governance/review-policy.md#repository-enforcement) already says the
  R3 two-review rule is "upheld by reviewers and the merging human, not by GitHub".
- Audit finding J-14 (audit base `1cfcf3c1ccb`) sampled 14 merged PRs (#1392, #1391, #1388,
  #1386, #1381, #1380, #1378, #1377, #1373, #1372, #1369, #1368, #1365, #1361). All were
  authored and merged by the repository owner's account. The only second party was the
  Codex bot, whose review state was COMMENTED, never APPROVED. Five of the 14 had no review
  at all, among them the R3 PRs #1386, #1373 and #1365 and the R2 PR #1392.
- Audit finding G-04 records the consequence on the check side: only `agentic-pr-gate` and
  `docs-build` are required, so engine correctness is judged by a person at each merge.

The repository has one human collaborator, so a second human review cannot happen. A
written rule that is never met misleads every reader who relies on it.

## Options considered

| Option | What it is | Cost | What it enables | What it limits |
| --- | --- | --- | --- | --- |
| **A. Second human reviewer for R3** | Set `required_approving_review_count` to 1 and turn on code-owner review. | Needs a second human with write access; until one exists, every R3 PR blocks. A maintainer, not an agent, changes branch protection. | GitHub enforces the R3 review rule; an R3 PR with no approving reviewer cannot merge. | Throughput on R3 depends on a person who does not exist today. |
| **B. Write down the real policy** | Agent author, independent agent reviewers, owner merges or delegates the merge on green CI; drop the two-review claim. | No setting changes. The R3 review rule stays process-only, as it is today. | The text matches what happens, so readers and agents stop citing a rule nobody meets. | No mechanical stop for an R3 PR that merges without review. |

The audit recommended B now and A when a second human joins.

## Owner decision

- **Chosen:** Option B, write down the real policy.
- **Decided by:** the repository owner, 2026-11-10.
- **Record:** an `APPROVED` review by the owner's account (`klausi3D`) on the pull request
  that adds this ADR, and the owner's merge of that pull request. This is the only owner
  record: no separate owner comment or issue records the answer. When this record was
  written the pull request had not been opened, so that review did not yet exist.
- **Acceptance pending:** until that review exists, this record is not accepted. The
  verifier checks it with `gh pr view <PR> --json reviews` (an `APPROVED` review by
  `klausi3D`) and `gh pr view <PR> --json mergedBy` (merged by `klausi3D`), and pastes both
  outputs into the pull request.

The policy, as the owner stated it:

1. An agent authors the change.
2. Independent agent reviewers review it: Claude and Codex.
3. The owner merges, or delegates the merge once CI is fully green.
4. R2 and R3 changes need the owner's OK.
5. The two-human-review claim is dropped until a second human joins.

## What this changes and what it keeps

### Changed in this PR

- [Review policy](../governance/review-policy.md) states the policy above in one sentence,
  and its "Repository enforcement" section no longer names an R3 two-review rule.
- [Agentic engineering](../governance/agentic-engineering.md): the R2 and R3 rows of the
  risk-class table and the "Process only" list state the owner's OK for R2 and R3 and the
  agent reviews, in place of "two reviews + CODEOWNER and human approval".

### Kept unchanged

- **No check is weakened.** No required check, branch-protection setting, guard, threshold
  or baseline changes. The two-review and CODEOWNER rule was never enforced
  (see the live-state table in [GitHub settings](../governance/github-settings.md)); this record moves the text to match the
  settings, not the settings to match a looser text.
- **Which layers are process-only.** As before, the review layers (correctness, domain and
  human disposition) are upheld by reviewers and the merging human, not by GitHub.
- **Compensating control for R3 PRs with no human reviewer.** The owner chose the owner's OK
  on every R2 and R3 change, on top of the agent reviews. It is a process control: nothing
  in GitHub enforces it.
- **No voting, and blockers stay open.** `docs/governance/review-policy.md` ("No voting")
  still holds: a well-founded blocker stays open until it is fixed or a human waives it with
  a written reason.

### Cleanup-plan process rule (CR-11, not part of the D-7 answer)

This rule comes from the cleanup plan (its review finding CR-11), not from the owner's D-7
answer. It is not existing policy: `docs/governance/review-policy.md` does not yet state a
review-round budget. It applies to cleanup-plan tasks only.

- The review budget for a cleanup-plan task is three rounds.
- When the budget runs out with a blocker or an unmet acceptance criterion still open, the
  task goes to the owner. Nothing is moved to an issue automatically.
- Only findings the owner dispositions as non-blockers become follow-up issues.

### Consequences

- J-14's required action "an R3 PR with no reviewer cannot merge" is **not** met under
  Option B. The agent reviews and the owner's OK are the controls, and both are process.
  J-14 is closed by the text and the settings agreeing, not by a new mechanical stop.
- G-04 is unchanged by this record: the set of required checks is a separate question.
- When a second human with write access joins, Option A becomes available. Adopting it
  needs a new decision, a branch-protection change applied by a maintainer, and an update
  to [GitHub settings](../governance/github-settings.md).
- Cleanup-plan tasks CLN-MS02 and CLN-MS03 depend on this record and may start once it is
  merged.

## Follow-up: files that still carry the old claim

These files are outside this task's scope. Each still states the two-review or CODEOWNER
rule, or says agents never merge, and so disagrees with the owner decision above until its follow-up lands.

| File | Old claim | Follow-up |
| --- | --- | --- |
| `.github/CODEOWNERS`, header comment | R3 needs "two independent reviews plus CODEOWNER approval"; "That rule stands". | CLN-MS03 |
| `.agentic/policy.json`, `risk_classes.R3.evidence_requirements` | R3 evidence: "Two independent reviews and CODEOWNER + human approval." Editing this file is R3. | to-file issue: align R3 review wording with the D-7 ADR (R3 for policy.json) |
| `.claude/skills/gs-change-evidence/SKILL.md` and its mirror `.agents/skills/gs-change-evidence/SKILL.md`, step 4 "Collect evidence by class" | R3 needs "two reviews plus human approval". | same to-file issue |
| `docs/architecture/adr-phase1-guard-hardening.md`, §3 "The R3 obligations disposition" | Quotes the R3 two-review requirement as the rule its §3 disposition answers. | same to-file issue |
| `docs/governance/release-acceptance-bar.md`, §9.1 (one place) and §10.1 (two places) | A manifest-ledger edit needs "two reviews and CODEOWNER approval". | same to-file issue |
| `AGENTS.md`, "Working rules", "Humans own the merge" | "Agents never auto-merge". The owner may delegate the merge on fully green CI (owner decision, item 3). | same to-file issue |

Older ADRs that describe the R3 process of their time (for example
`adr-editor-download-picker.md`, `adr-import-importance-pruning.md`,
`adr-overflow-drop-telemetry.md`) are records and are not rewritten.

## Out of scope

- Changing branch protection, required checks or `docs/governance/github-settings.md`.
- Editing `.agentic/policy.json`, `.github/CODEOWNERS` or the skills.
- Which checks are required (G-04).
- Any code, workflow or test change.
