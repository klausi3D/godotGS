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
- **Verified against:** `b58bbe35d1f` (origin/master). Every `file:line` below was read at
  that commit.

## Problem

The written review policy asks for more than the repository does.

- `docs/governance/agentic-engineering.md:173` gives R3 the process "ADR before
  implementation + two reviews + CODEOWNER and human approval".
  `.agentic/policy.json:72` lists "Two independent reviews and CODEOWNER + human approval."
  as an R3 evidence requirement, and `.github/CODEOWNERS:9-12` says "That rule stands;
  GitHub does not enforce it."
- Branch protection requires 0 approving reviews and no code-owner review
  (`docs/governance/github-settings.md:27-28`). `docs/governance/review-policy.md:106-110`
  already says the R3 two-review rule is "upheld by reviewers and the merging human, not by
  GitHub".
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
- **Record:** the owner's approving review on the pull request that adds this ADR. No
  separate owner comment or issue records the answer; if the owner adds one, its URL goes
  here.

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
  (`docs/governance/github-settings.md:27-28`); this record moves the text to match the
  settings, not the settings to match a looser text.
- **Which layers are process-only.** As before, the review layers (correctness, domain and
  human disposition) are upheld by reviewers and the merging human, not by GitHub.
- **Compensating control for R3 PRs with no human reviewer.** The owner chose the owner's OK
  on every R2 and R3 change, on top of the agent reviews. It is a process control: nothing
  in GitHub enforces it.
- **No voting, and blockers stay open.** `docs/governance/review-policy.md` ("No voting")
  still holds: a well-founded blocker stays open until it is fixed or a human waives it with
  a written reason. The cleanup plan's review budget is three rounds. When the budget runs
  out with a blocker or an unmet acceptance criterion still open, the task goes to the owner;
  nothing is moved to an issue automatically. Only findings the owner dispositions as
  non-blockers become follow-up issues.

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
| `.github/CODEOWNERS:9-12` | R3 needs "two independent reviews plus CODEOWNER approval"; "That rule stands". | CLN-MS03 |
| `.agentic/policy.json:72` | R3 evidence: "Two independent reviews and CODEOWNER + human approval." Editing this file is R3. | to-file issue: align R3 review wording with the D-7 ADR (R3 for policy.json) |
| `.claude/skills/gs-change-evidence/SKILL.md:31` and its mirror `.agents/skills/gs-change-evidence/SKILL.md:31` | R3 needs "two reviews plus human approval". | same to-file issue |
| `docs/architecture/adr-phase1-guard-hardening.md:293` | Quotes the R3 two-review requirement as the rule its §3 disposition answers. | same to-file issue |
| `docs/governance/release-acceptance-bar.md:416`, `:620`, `:636` | A manifest-ledger edit needs "two reviews and CODEOWNER approval". | same to-file issue |
| `AGENTS.md:80` | "Agents never auto-merge". The owner may delegate the merge on fully green CI (owner decision, item 3). | same to-file issue |

Older ADRs that describe the R3 process of their time (for example
`adr-editor-download-picker.md`, `adr-import-importance-pruning.md`,
`adr-overflow-drop-telemetry.md`) are records and are not rewritten.

## Out of scope

- Changing branch protection, required checks or `docs/governance/github-settings.md`.
- Editing `.agentic/policy.json`, `.github/CODEOWNERS` or the skills.
- Which checks are required (G-04).
- Any code, workflow or test change.
