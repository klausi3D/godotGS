# ADR: Next program — beyond-VRAM and the audit cleanup plan run in parallel

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Risk:** this document is R0 (`docs/**`). It decides how work is scheduled; it changes no
  code, workflow, guard, threshold or gate.
- **Refs:** audit findings K-01, K-02, K-03, K-11, K-12 and K-14; issues #458, #1315,
  #1364, #1393; owner decision D-8; cleanup-plan task CLN-D08.
- **Related:** [Worlds beyond VRAM](adr-beyond-vram-worlds.md) ("BV ADR"); the
  [continuation program](../programs/godotgs-continuation-2026-08.md) (the plan of record
  before this decision).
- **File name:** the file name was set before the owner answered, when the recommended
  option was an "alpha correctness" program that replaces beyond-VRAM. The owner chose a
  different option (see Owner decision); the name is kept so that the task contracts that cite it still
  resolve.
- **Verified against:** `b58bbe35d1f` (origin/master). Every file claim below was read at
  that commit. Issue states were read with `gh issue view` on 2026-11-10.

## Problem

The audit (base `1cfcf3c1ccb`) found that recent work had moved away from correctness fixes
and that the plan of record no longer described what was being built.

- K-01: of the last 100 merges (2026-10-02 to 2026-11-09), correctness fixes were 19 PRs and
  18.6 % of changed lines; tests, CI and docs were 43.6 % of lines. Since 2026-11-03,
  correctness fixes fell to 3 of 45 PRs. 33 PRs (29 % of lines) belonged to the beyond-VRAM
  (BV) program.
- K-02: the BV program, whose problem statement is "GodotGS has never rendered a world
  larger than GPU memory" ([Worlds beyond VRAM](adr-beyond-vram-worlds.md)), took roughly
  the last six days of work.
- K-12: three recent BV test PRs in the audit snapshot (#1380, #1388, #1391) harden the
  probe lane, while
  #1393 (overlap-record overflow drops records on healthy beyond-VRAM frames) and #1364 (GSQ
  decode too slow for the teleport refill budget) stay open.
- K-11: CI structure took 14 PRs and 20,305 lines in 38 days, and the runner layout changed
  four times in five weeks. The audit rates this finding plausible, derived from titles.
- K-03: `.agentic/programs/continuation-2026-08.json` still names #458 as its status
  authority and has no BV entries; the BV program is tracked only in its ADR and in commit
  titles.
- K-14: the one gate that exercises the game shape, #1315 (GS-COMBINED-1), is open and
  unmerged.

The audit also produced a cleanup plan: a set of CLN-* tasks, ordered in waves, that work
through its findings. The question for the owner was what runs next.

## Options considered

| Option | What it is | Cost | What it enables | What it limits |
| --- | --- | --- | --- | --- |
| **A. Close BV, then alpha correctness** (audit recommendation) | Close BV at M1 plus the frame-time gate in BV ADR §8.4, then run an "alpha correctness" program: the painterly decision (D-1), the stop-gaps (D-4), F-01 and A-01, then #1012, #1016, #320, #786 and #883. | BV stops after M1; the later BV milestones (M1c to M5 in BV ADR §10) wait. | All capacity, including the single GPU runner, goes to the alpha blockers. | Worlds beyond VRAM do not progress until the alpha program ends. |
| **B. Run both in parallel** | BV continues alongside the cleanup plan. | Both compete for the GPU runner and for the streaming source files, so each moves more slowly. | Neither program stops; the cleanup plan starts now. | Cleanup tasks that touch files held by open BV PRs wait. |

## Owner decision

- **Chosen:** Option B, run both in parallel. The BV program continues alongside the
  cleanup plan.
- **Decided by:** the repository owner, 2026-11-10.
- **Record:** an `APPROVED` review by the owner's account (`klausi3D`) on the pull request
  that adds this ADR, and the owner's merge of that pull request. This is the only owner
  record: no separate owner comment or issue records the answer. When this record was
  written the pull request had not been opened, so that review did not yet exist.
- **Acceptance pending:** until that review exists, this record is not accepted. The
  verifier checks it with `gh pr view <PR> --json reviews` (an `APPROVED` review by
  `klausi3D`) and `gh pr view <PR> --json mergedBy` (merged by `klausi3D`), and pastes both
  outputs into the pull request.

The owner did not adopt the audit's recommendation (A). BV is not closed at M1, so this
record defines no "BV closed" condition; BV milestones and their issues continue under the
BV ADR's roadmap (§10) and their own milestones.

## Consequences

### How the two programs share the repository

- **Path locks.** Open BV branches hold streaming source files. The cleanup plan's
  dispatcher takes `external_locks`, the paths changed by open BV PRs, and never dispatches
  a cleanup task whose owned paths include one of them. The task waits until the BV PR
  merges or closes.
- **Shared GPU runner.** Both programs use the same GPU runner. Expect lower throughput on
  GPU-evidence tasks in both programs.

### Cleanup-plan tasks

- **CLN-PR01** (supersede the continuation-2026-08 manifest and add a program manifest) is
  unblocked. Under this decision the manifest records two live programs: BV, as scheduled in
  BV ADR §10, and the cleanup plan, in its own wave order. This record does not reorder
  either.
- **CLN-PR02** (triage the open P1s) is not changed by this record.

### Proposals the findings make that this record does not decide

The owner's answer covers which programs run, not these five proposals. Each proposal below
is stated in full, as the finding makes it, and stays open. The owner has not accepted or
rejected any of them.

**Follow-up owner:** the repository owner decides each proposal. Until then, all five are
carried by one to-file issue, "Owner decision on the audit's K-01, K-02, K-11, K-12 and K-14
proposals (not decided by D-8)", which this record's pull request names in its notes. No
agent decides them.

| Finding | Proposal, in full | Status |
| --- | --- | --- |
| K-01 | A weekly budget for correctness fixes, with a weekly report of fix share and open-P1 count. | Not decided. |
| K-11 | Freeze runner and CI topology until Public Alpha unless a change fixes a red. | Not decided. |
| K-12 | Gate further BV test-lane PRs on #1393 and #1364; done when tier A passes valid with no overflow. | Not decided. |
| K-02 | Add a game-ship scene budget (nodes, splats, lights, painterly) to the status authority, and map each BV milestone to the release-blocker item it retires; done when each open BV issue names the blocker it closes. | Not decided. |
| K-14 | Land #1315 first, then re-point the plan at a Grandma's House scene budget and at the alpha blockers #1012, #1016 and #842, with an owner and a date for each recorded in #458. | Not decided. Cleanup task CLN-PR03 carries #1315; the scene budget and the owners and dates in #458 are carried by no task. |

## Out of scope

- Any change to the BV design, its milestones or its gates. The BV ADR remains the design
  record for that program, with the status it already has.
- The order of tasks inside the cleanup plan.
- Any code, workflow, guard, threshold or gate change.
