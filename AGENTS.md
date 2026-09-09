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
| `docs/governance/` | Canonical process docs. |
| `docs/agent_memory/` | **Legacy. Not source of truth** (see below). |
| `servers/`, `scene/`, `core/`, `editor/`, `platform/`, `drivers/`, `main/` | Upstream Godot. Touch only with strong justification. |

## Canonical docs — read before working

- [Contribution standards](docs/governance/contribution-standards.md) — mandatory rules for every merged change.
- [Build / test / CI reference](docs/reference/build-test-ci.md) — how to build and which checks to run.
- [Agentic engineering](docs/governance/agentic-engineering.md) — roles, task contracts, risk classes, worktree isolation.
- [Review policy](docs/governance/review-policy.md) — deterministic + independent + GPU/domain review, human disposition.
- Module entry points: [`modules/gaussian_splatting/READING_ORDER.md`](modules/gaussian_splatting/READING_ORDER.md), [`ARCHITECTURE.md`](modules/gaussian_splatting/ARCHITECTURE.md).

When docs conflict, the two canonical pages named in `CONTRIBUTING.md`
(contribution-standards + build-test-ci) win.

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
  machine-readably in `.agentic/policy.json` (added by a sibling PR in this
  foundation series; until it lands, use the human-readable risk classes in the
  agentic-engineering doc).

## Upstream Godot boundary

Do **not** scatter `AGENTS.md` into generic upstream directories (`servers/`,
`scene/`, `core/`, `editor/`, `platform/`). Keep the delta to upstream Godot
small; changes there are highest risk and need maintainer (CODEOWNER) review.

## Legacy memory

`docs/agent_memory/COORDINATOR_MEMORY.md` and `GAUSSIAN_ISSUE_BOARD.md` are a
legacy multi-agent coordination system. **Do not treat them as current project
status.** Active issues live in GitHub Issues; durable rules live in the docs
linked above. The issue board is frozen and migrates to GitHub incrementally.

## Nested instructions

More specific `AGENTS.md` files refine these rules for their subtree. They add
detail; they never copy or weaken what is written here. Always read the most
specific `AGENTS.md` for the code you are touching.
