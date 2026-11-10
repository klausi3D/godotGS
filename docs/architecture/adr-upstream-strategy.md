# ADR: Upstream Godot strategy — hop straight to 4.7

- **Status:** Accepted, 2026-11-10 — decided by the owner on that date; the owner's confirming comment on this PR and its merge confirm the recorded text (see **Confirmation** below).
- **Date:** 2026-11-10.
- **Risk:** this document is R0 (`docs/**`). The rebase it schedules touches the engine
  delta outside the module and is R3, with this ADR as its design record. This record edits
  neither `ENGINE_PATCHES.md`, `GODOT_VERSION` nor `version.py`.
- **Refs:** audit findings F-03, F-08, F-09 and K-13; issues #943, #944, #945, #946, #947,
  #953; owner decision D-5; cleanup-plan task CLN-D05.
- **Related:** the M5 section of the
  [continuation program](../programs/godotgs-continuation-2026-08.md), which planned the
  re-baseline as two stacks, 4.6.3 then 4.7.2.
- **Verified against:** `b58bbe35d1f` (origin/master). Every file claim below was read at
  that commit. Issue titles and states were read with `gh issue view` on 2026-11-10.

## Problem

The fork is based on a Godot release candidate and has no decided target for its next
upstream move.

- `version.py` declares 4.5.0 status `rc`. `GODOT_VERSION` and `ENGINE_PATCHES.md`
  record the base as 4.5.0-rc at `2dd26a027a99` (2025-09-08).
- F-09: the base is 17 commits behind 4.5-stable and 341 behind 4.5.2-stable. Some upstream
  stable changes are already in the tree, but nothing records which.
- F-03: the engine delta is not fully documented. At its base `1cfcf3c1ccb` the audit recorded
  264 added lines in 8 engine files. Re-measured for this record with
  `git diff --numstat 2dd26a027a99 b58bbe35d1f` over the same 8 files, they carry 303 added
  lines (10 + 150 + 3 + 23 + 71 + 2 + 12 + 32; #1396 added engine lines after the audit).
  These lines appear in neither `ENGINE_PATCHES.md` nor `engine_modifications.patch`,
  and the patch's `godot-source/` path prefix makes `git apply` from the checkout root fail.
  No guard reads either file.
- F-08: the rebase costs more than the 38-file table in `ENGINE_PATCHES.md` suggests,
  because of re-indented upstream blocks and a changed `render_scene()` signature.
- K-13: the re-baseline was filed twice, once per target: #944 and #945 for Godot 4.6.3
  (content, then engine hooks) and #946 and #947 for Godot 4.7.2. All four are open.

## Options considered

| Option | What it is | Cost | What it enables | What it limits |
| --- | --- | --- | --- | --- |
| **A. Stay on 4.5.x** | Fast-forward to 4.5.2-stable now (341 commits) and freeze there for the first game; one hop after Public Alpha. | A fast-forward within 4.5.x, plus a freeze rule for which upstream fixes may be picked before the hop. | The smallest engine change before Public Alpha; the 4.5.1 and 4.5.2 fixes are received. | The fork stays on 4.5 through Public Alpha; the larger hop is only deferred. |
| **B. Hop to 4.6, or straight to 4.7** | Rebase the engine delta onto a newer stable line. | The audit estimates a 46-file delta and 400–960 lines of upstream churn in the hot files, plus re-indentation churn. | A current engine base for shipping; one move instead of a 4.5.2 step followed by a later hop. | A large R3 rebase sits on the path, and every rebase before the patch is generated starts with archaeology (F-03). |

The audit recommended A until Public Alpha, then a single hop to whatever is stable then,
and in either case generating the engine patch in CI first (F-03).

## Owner decision

- **Chosen:** Option B, hop straight to 4.7. The intermediate 4.6 step is dropped.
- **Decided by:** the repository owner, 2026-11-10.
- **Accepted:** from 2026-11-10, the date of the owner's decision. This ADR records the
  answer; no separate owner comment or issue records it.
- **Confirmation:** the pull request that adds this ADR is opened from the owner's account
  (`klausi3D`), as every agent PR here is, and GitHub does not let a PR's author approve it
  (see "Require review from Code Owners" in
  [GitHub settings](../governance/github-settings.md)), so an `APPROVED` review cannot be
  obtained. The confirmation is the one practised for the owner's OK: a comment on that
  pull request, posted from `klausi3D`, stating that the owner confirms the recorded text
  matches the decision, followed by the merge of that pull request by `klausi3D`, done by
  the owner or by the coordinating agent under the owner's merge delegation. Agents use the
  same account, so GitHub cannot tell the owner's comment from an agent's; the comment says
  whether the owner posted it or it quotes the OK the owner gave the coordinating agent.
  The verifier checks them with `gh pr view <PR> --json comments` (the confirming comment
  by `klausi3D`) and `gh pr view <PR> --json mergedBy` (merged by `klausi3D`), and pastes
  both outputs into the pull request.

The owner did not adopt the audit's recommendation (A). Option A's freeze rule (which
upstream commits may be picked before the hop) therefore does not apply.

## Consequences

### Issues

- **Kept:** #946 (content re-baseline onto Godot 4.7.2) and #947 (engine-hook port and
  qualification on Godot 4.7.2) are the surviving re-baseline chain.
- **To close:** #944 and #945 (the same two tasks for Godot 4.6.3), one of each pair. This
  PR does not close them. Closing them is a GitHub action taken after this record is merged
  with the owner's approval, with a comment linking this ADR.
- #943 (the re-baseline design issue) and #953 (goal M5) stay open. This record changes them only by
  naming 4.7 as the target.

### Cleanup-plan tasks

- **CLN-EN01 comes first.** The engine patch is generated in CI before the hop, so the hop
  starts from a recorded delta (F-03).
- **CLN-EN03 is re-scoped** from a 4.5.2-stable fast-forward to a 4.7 rebase. It is an
  epic; its eight children CLN-EN03-1..8 keep their roles as planned in the CLN-EN03 epic,
  in this dependency order:
    1. CLN-EN03-1 records the 4.7 base selection, the engine-delta inventory from
       CLN-EN01's generated patch, and the rebase slice plan.
    2. CLN-EN03-2 merges the chosen Godot 4.7.x stable tag, with every engine-delta file
       parked at the upstream version, and moves `GODOT_VERSION`.
    3. CLN-EN03-4 to CLN-EN03-7 re-apply the engine delta in four rebase slices:
       non-rendering engine code; RenderingDevice and the Vulkan driver; scene cull and
       rendering server; the `renderer_rd` delta and `GaussianSplatStorage`.
    4. CLN-EN03-8 ports the module to the Godot 4.7 engine headers and APIs without a
       behaviour change.
    5. CLN-EN03-3 proves the 4.7 base and lands the integration branch. It depends on
       CLN-EN03-8, not on CLN-EN03-2 directly.
- **CLN-EN02** (revert the whitespace churn in the scene cull, F-08) stays a predecessor of
  the rebase; it shrinks the conflict surface. CLN-EN02 covers only the whitespace half of
  F-08 and does not complete it on its own.
- **F-08's second half stays open as a follow-up after CLN-EN03:** move the splat plumbing
  behind a smaller seam, so that `render_scene()` keeps its upstream signature (a struct of
  extra scene data instead of the mid-signature `p_gaussian_splats` parameter) and
  `RendererCompositor` gains a non-pure virtual with a forward-declared opaque type instead
  of a pure virtual returning a `RendererRD::` type. It is filed as the to-file issue
  "Engine seam: smaller render_scene/compositor seam for splat plumbing (F-08, after
  CLN-EN03)" and needs its own ADR. F-08 is done only when both halves are.
- F-09 keeps both parts of its completion condition, now on the 4.7 line:
    1. `GODOT_VERSION` names the commit of a Godot 4.7.x stable tag.
    2. `git diff <tag-commit> HEAD` over the engine directories equals the documented
       engine delta.

### What does not change

- No engine file, version file or patch record is edited by this ADR. The 4.7 rebase is its
  own R3 task with its own evidence.
- No guard, threshold or gate changes.

## Out of scope

- When the hop happens relative to Public Alpha. The owner's answer names the target, not
  the timing; the order of CLN-EN03 in the cleanup plan is unchanged by this record.
- Which 4.7 patch release is the target tag. The surviving issues name 4.7.2; CLN-EN03-1
  records the exact tag.
- Godot 4.8 and later.
