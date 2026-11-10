# AGENTS.md — GodotGS

Instructions for any AI coding agent (and any human) working in this repository.
Keep it short; deeper rules live in the linked canonical docs and in the nested
`AGENTS.md` files closer to the code.

GodotGS is a **fork of the Godot engine**. Almost all project-specific work lives
under `modules/gaussian_splatting/`. Treat everything else as upstream Godot.

## Repository map

| Path | What it is |
| --- | --- |
| `modules/gaussian_splatting/` | The Gaussian Splatting module — where work happens. Has its own `AGENTS.md`. |
| `modules/gaussian_splatting/renderer/`, `shaders/` | GPU resources, render pipeline, GLSL. Each has its own `AGENTS.md`. |
| `tests/` | CI guards, runtime harness, benchmarks, GDScript/C++ tests. Has its own `AGENTS.md`. |
| `.github/workflows/` | CI workflows incl. self-hosted runners. Has its own `AGENTS.md`. |
| `.agentic/` | Machine-readable agent control plane (risk policy, ownership, task/review contracts, roles). |
| `.claude/`, `.codex/`, `.agents/skills/` | Tool adapters: Claude Code and Codex settings, hooks, subagents, and on-demand skills (`.agents/skills/` mirrors `.claude/skills/`; a test enforces it). |
| `docs/governance/` | Canonical process docs. |
| `docs/agent_memory/` | **Legacy. Not source of truth.** Frozen coordination memory; active issues live in GitHub Issues. |
| `servers/`, `scene/`, `core/`, `editor/`, `platform/`, `drivers/`, `main/` | Upstream Godot. Touch only with strong justification. |

## Canonical docs — consult when the task needs them

Open these on demand and read only the section you need. Do not preload them.

- [Contribution standards](docs/governance/contribution-standards.md) — mandatory rules for every merged change (short; read before your first commit).
- [Build / test / CI reference](docs/reference/build-test-ci.md) — when you build or pick checks.
- [Agentic engineering](docs/governance/agentic-engineering.md) — roles, task contracts, risk classes, worktree isolation.
- [Review policy](docs/governance/review-policy.md) — when you review or prepare a PR for review.
- Module entry points: [`ARCHITECTURE.md`](modules/gaussian_splatting/ARCHITECTURE.md) (subsystem map) and [`READING_ORDER.md`](modules/gaussian_splatting/READING_ORDER.md) (an onboarding map, not a checklist).

When docs conflict, the two canonical pages named in `CONTRIBUTING.md`
(contribution-standards + build-test-ci) win.

## Context economy

Context is the scarcest resource. Spend it on the task:

- **Search, then read windows.** Many module sources and test headers are
  1.5k–7k lines. Grep for the symbol, then read about 100–200 lines around it. Never
  read a large file whole.
- **Scope searches.** Search `modules/gaussian_splatting/` or `tests/` rather than the
  whole tree. [`.ignore`](.ignore) keeps translations, `thirdparty/`, generated
  reports and legacy memory out of default `rg` searches. To override, use
  `rg --no-ignore-dot` or an explicit path.
- **Keep verbose output out.** Run builds and test lanes so that only failures,
  summaries and the tail reach you. `scripts/agentic/condense_output.py` does this
  and keeps the full log. Search full logs instead of reading them.
- **Delegate wide exploration and long check runs** to a subagent that returns a
  summary, if your tool supports subagents.
- **Do not open** `docs/agent_memory/`, `docs/reports/`, generated `*.gen.h`, or
  `.site/` for current facts.

## Compact instructions

When a conversation is compacted or summarized, preserve:

- the task and its scope limits
- the worktree path, branch and base SHA
- every modified file
- each command run, with its exit code and outcome (pass, fail or not run)
- open review blockers and unresolved questions

Drop file contents that can be re-read and full logs that can be re-searched.

## Working rules

- **Small, reviewable changes.** One task → one branch → one worktree. Do not
  bundle unrelated work. Do not expand scope without a new task.
- **Never revert or "clean up" another agent's or the user's uncommitted work.**
  Work in your own worktree; leave other working trees alone.
- **Always anchor to a base.** Record the base commit SHA you started from; review
  and evidence are evaluated against the immutable `base..head` diff.
- **Stacked PRs** must state their base PR and base SHA so reviewers never grade a
  moving base or only the top diff.
- **Reviewers do not implement.** A reviewing agent reports findings on a fixed
  diff and changes no code. A verifying agent runs checks and changes no code.
- **Humans own the merge.** Agents never auto-merge and never dismiss a review
  blocker; a blocker stays open until fixed or a human waives it with a reason.
- **Do not commit ephemeral agent artifacts** — transcripts, session IDs, scratch
  notes, dirty-worktree dumps, local task instances. See
  [contribution standards](docs/governance/contribution-standards.md).
- **Do not weaken a guard, baseline, threshold, or gate to make a check pass.**
- **Absence of a signal is never a passing signal.** "Did not run", "could not
  run", "found nothing to check" and "ran and passed" must be distinguishable.
  Before reporting a check as green, ask: *what would a failure have to look like
  for this check to see it?* If the answer is "it couldn't", it is not evidence
  yet. This is the most common defect class in this repo's guards and CI — the
  recurring shapes and the mandatory practices are catalogued in
  [evidence integrity](docs/governance/evidence-integrity.md). Read it before
  writing or reviewing a guard.
- **Evidence by risk.** Higher-risk changes (renderer/shaders/persistence/engine)
  require runtime/GPU evidence and extra review — see the risk classes (R0–R3) in
  [agentic engineering](docs/governance/agentic-engineering.md), mirrored
  machine-readably in `.agentic/policy.json`.

## Upstream Godot boundary

Do **not** scatter `AGENTS.md` into generic upstream directories (`servers/`,
`scene/`, `core/`, `editor/`, `platform/`). Keep the delta to upstream Godot
small; changes there are highest risk and need maintainer (CODEOWNER) review.

## Nested instructions

More specific `AGENTS.md` files refine these rules for their subtree. They add
detail; they never copy or weaken what is written here. Always read the most
specific `AGENTS.md` for the code you are touching.
