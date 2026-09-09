# GodotGS Continuation Program (2026-08)

This program turns the 2026-08-22/23 repository investigation into bounded work
that agents can execute with the repository's task-contract harness. The
machine-readable authority for dependency structure and goal text is
[`continuation-2026-08.json`](../../.agentic/programs/continuation-2026-08.json).
Live status remains in GitHub, coordinated from
[#458](https://github.com/klausi3D/godotGS/issues/458).

## Planning anchor and proof boundary

- Planning snapshot: `873025f5783c765caec08a39a33909d1118b9d60`
  (`origin/master`, re-queried 2026-08-23).
- This SHA is an investigation anchor, not the base for every future task. Each
  claiming agent must fetch and pin its own immutable dispatch base.
- At planning time, nine pull requests were open. Their state can change at any
  moment; re-query heads, bases, checks, reviews, threads, milestones, and merge
  states before action.
- This R1 agentic-test packaging change does not certify a local build, runtime, GPU output,
  performance, or release. Those lanes are **NOT_RUN** here.
- Existing audit issues keep their historical milestones where moving them would
  erase provenance. The `program:prod-ready` label and this manifest provide the
  cross-milestone program query.

## Dependency graph

```text
M0 Trustworthy master ──> M1 Fail-closed proof ──> M2 Data/runtime correctness ──┐
          │                    │                                                 ├─> M4 Public alpha ─> M5 Upstream hops
          └────────────────────┴───────────────> M3 Renderer parity ─────────────┘
```

Read-only investigation may move ahead. Implementation follows the graph unless
a human records an exception in the relevant coordinator issue.

## Using the milestone goals in Codex

Use one dedicated Codex session per active milestone. Ask the agent to create a
goal with the milestone's `objective` copied verbatim from the program manifest or
the linked `[goal Mx]` issue. Omit a token budget unless the owner explicitly sets
one. A session may have only one unfinished goal, so do not replace or stack a
second milestone goal in the same thread.

The goal is deliberately agent-achievable: prepare every child item for human
disposition with the required evidence. Human merges, waivers, support decisions,
visual acceptance, baseline changes, and release publication are milestone exit
gates but are not actions authorized by the goal. Mark the Codex goal complete
only when the manifest's `agent_completion_criteria` are all true; a PR receipt or
partial green lane is not completion.

## Milestones and work packages

### M0 — Trustworthy master

Goal: [#948](https://github.com/klausi3D/godotGS/issues/948) · GitHub milestone:
[M0](https://github.com/klausi3D/godotGS/milestone/14)

Dependency order for the current remediation stacks:

- [#939](https://github.com/klausi3D/godotGS/pull/939) into
  [#933](https://github.com/klausi3D/godotGS/pull/933), then re-verify #933 before
  master disposition.
- [#937](https://github.com/klausi3D/godotGS/pull/937) into
  [#934](https://github.com/klausi3D/godotGS/pull/934), then re-verify #934 before
  master disposition.
- [#932](https://github.com/klausi3D/godotGS/pull/932) and draft
  [#931](https://github.com/klausi3D/godotGS/pull/931) need explicit review/process
  disposition.
- Conflicted [#882](https://github.com/klausi3D/godotGS/pull/882),
  [#873](https://github.com/klausi3D/godotGS/pull/873), and
  [#779](https://github.com/klausi3D/godotGS/pull/779) require a revive-with-new-
  contract or close/supersede decision, not a blind rebase.
- [#941](https://github.com/klausi3D/godotGS/issues/941) repairs the missing
  script-visible rendered-content proof.
- [#792](https://github.com/klausi3D/godotGS/issues/792) and
  [#921](https://github.com/klausi3D/godotGS/issues/921) own the tie-break and
  GPU-001 visual/baseline decisions.

### M1 — Fail-closed proof spine

Goal: [#949](https://github.com/klausi3D/godotGS/issues/949) · GitHub milestone:
[M1](https://github.com/klausi3D/godotGS/milestone/15)

- [#891](https://github.com/klausi3D/godotGS/issues/891): positive runtime
  completion and renderer-proof semantics.
- [#903](https://github.com/klausi3D/godotGS/issues/903): production-default
  composite pixel oracle.
- [#906](https://github.com/klausi3D/godotGS/issues/906): 38 stranded renderer
  GPU cases reach a real batch.
- [#936](https://github.com/klausi3D/godotGS/issues/936): separate runtime and QA
  fixture authority.
- [#523](https://github.com/klausi3D/godotGS/issues/523): attributable optimized
  performance baselines and approved enforcement transition.
- [#889](https://github.com/klausi3D/godotGS/issues/889): keep protection claims
  live-derived and prepare the human settings handoff.

### M2 — Data and runtime correctness

Goal: [#950](https://github.com/klausi3D/godotGS/issues/950) · GitHub milestone:
[M2](https://github.com/klausi3D/godotGS/milestone/16)

- [#862](https://github.com/klausi3D/godotGS/issues/862): resource payload change
  resubmission.
- [#774](https://github.com/klausi3D/godotGS/issues/774): serializer data-lock
  correctness.
- [#773](https://github.com/klausi3D/godotGS/issues/773): incremental baseline
  identity and compatibility.
- [#586](https://github.com/klausi3D/godotGS/issues/586): no silent unsorted
  translucent fallback.

Each is a separate repair. Persistence/API decisions remain R3 and need a design
record before implementation.

### M3 — Renderer parity and recovery

Goal: [#951](https://github.com/klausi3D/godotGS/issues/951) · GitHub milestone:
[M3](https://github.com/klausi3D/godotGS/milestone/17)

- [#926](https://github.com/klausi3D/godotGS/issues/926): remaining sorter-init
  recovery exits.
- [#928](https://github.com/klausi3D/godotGS/issues/928): transparent alpha.
- [#929](https://github.com/klausi3D/godotGS/issues/929): temporal jitter parity.
- [#930](https://github.com/klausi3D/godotGS/issues/930): painterly color-space
  decode.
- [#942](https://github.com/klausi3D/godotGS/issues/942): Forward Mobile design
  and explicit support contract before an implementation child is created.

All visual/render-math work requires real-scan GPU evidence and an independent
GPU/performance review.

### M4 — Public-alpha evidence

Goal: [#952](https://github.com/klausi3D/godotGS/issues/952) · GitHub milestone:
[M4](https://github.com/klausi3D/godotGS/milestone/18)

- [#360](https://github.com/klausi3D/godotGS/issues/360): release bar and accepted
  limitations.
- [#182](https://github.com/klausi3D/godotGS/issues/182): compatibility evidence.
- [#184](https://github.com/klausi3D/godotGS/issues/184): candidate-bound media.

The agent produces a hash-bound release-decision packet. Only the owner may name,
publish, or announce a stable release.

### M5 — Supported Godot re-baseline

Goal: [#953](https://github.com/klausi3D/godotGS/issues/953) · GitHub milestone:
[M5](https://github.com/klausi3D/godotGS/milestone/19)

- [#943](https://github.com/klausi3D/godotGS/issues/943): R3 design and engine-hook
  manifest.
- [#944](https://github.com/klausi3D/godotGS/issues/944): mechanical 4.6.3 content
  adoption.
- [#945](https://github.com/klausi3D/godotGS/issues/945): 4.6.3 hook port and
  qualification.
- [#946](https://github.com/klausi3D/godotGS/issues/946): mechanical 4.7.2 content
  adoption.
- [#947](https://github.com/klausi3D/godotGS/issues/947): 4.7.2 hook port and full
  qualification.

The two mechanical/hook stacks keep upstream provenance reviewable. Godot 4.8
development is outside this program.

## Autonomous dispatch loop

1. Fetch with the platform-correct Git/GitHub tooling and re-query the live
   program label, coordinator issue, child issue, open PRs, reviews, threads, and
   checks.
2. Select the highest-priority dependency-ready child. A milestone number does not
   outrank a ready correctness blocker.
3. Reproduce or re-verify the finding at live HEAD. If already fixed, close or
   supersede it with evidence rather than reimplementing it.
4. Create the task contract from [the task template](../../.agentic/templates/task.json),
   pin the immutable base, classify risk, and identify owned/forbidden paths.
5. Create a fresh branch and worktree. Never develop from a dirty shared checkout.
6. Keep no more than two new implementation PRs active and no more than two heavy
   processes running across the program.
7. Implement the smallest closable slice. Use a separate verifier and independent
   reviewer; add the GPU/performance reviewer for R2/R3.
8. Record exact commands, outputs, binary identity, blind spots, RED controls, and
   restored GREEN results. Missing evidence is NOT_RUN.
9. Open or update the PR without merging, then update the milestone coordinator
   issue with SHA-bound readiness and the next dependency-ready item.

Validate the program package with:

```bash
python scripts/agentic/validate_program.py \
  --program .agentic/programs/continuation-2026-08.json
python scripts/agentic/validate_repo_contract.py
python -m unittest discover -s tests/agentic
```
