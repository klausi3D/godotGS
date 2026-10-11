# Agentic Engineering

How AI coding agents work in this repository. The goal is **specification-driven**,
not chat-driven, development: one agent plans, one implements, deterministic
systems plus independent reviewers verify, and a human decides the merge.

This page is the human-readable source of truth for roles and risk classes. A
machine-readable mirror lives under `.agentic/` (`policy.json`, `ownership.json`,
schemas, templates, roles), validated by `scripts/agentic/`. Most of what this
page describes is process, not tooling: see
[What is enforced and what is process](#what-is-enforced-and-what-is-process). See
also [`AGENTS.md`](../../AGENTS.md), [contribution standards](contribution-standards.md),
and the [review policy](review-policy.md).

## Principles

- **Specs over chat.** Work starts from a task contract (a small, explicit
  specification), not from a conversation transcript. The contract — not the chat
  history — is what gets reviewed and reproduced.
- **Immutable base.** Every task records the base commit SHA. Implementation,
  evidence, and review are all evaluated against the fixed `base..head` diff.
- **Reproducible evidence.** Claims (tests pass, perf improved, leak fixed) are
  backed by commands anyone can re-run. Missing hardware is reported as "not run",
  never silently as "passed".
- **No ephemeral state in git.** Session IDs, transcripts, scratch notes, and
  dirty-worktree dumps are never committed (see [contribution standards](contribution-standards.md)).

## Roles

Each role is a separate context with a narrow mandate; full prompts live in
`.agentic/roles/`.

| Role | Mandate | May change code? |
| --- | --- | --- |
| **Planner** | Read-only investigation; writes the task contract. | No |
| **Implementer** | Implements exactly one task in its own worktree, within scope. | Yes, in scope only |
| **Verifier** | Runs guards/tests/harness/benchmarks and reports results, incl. skips. | No |
| **Correctness reviewer** | Independent review of the immutable diff for correctness. | No |
| **GPU/performance reviewer** | Adds host↔shader/sync/lifetime/bounds/timing/VRAM/benchmark-method review. | No |

- The **implementer may not weaken** acceptance criteria, baselines, thresholds,
  or gates to pass.
- A **reviewer never implements**, gets a fresh context, sees the fixed
  `base..head` diff (and ideally not the implementer's log), and reports
  uncertainty and missing evidence as findings.
- A **human owns the merge** and is the only one who can waive a blocker, with a
  written reason.

## Task contracts

A task contract (`.agentic/templates/task.json`, schema
`.agentic/schemas/task.schema.json`) captures: the problem, base SHA, risk class,
owned and forbidden paths, dependencies, non-goals, invariants, acceptance
criteria, validation commands, evidence requirements, and a rollback plan. One
contract → one branch → one worktree.

## Milestone programs and Codex goals

A milestone program (`.agentic/schemas/program.schema.json`) groups existing
GitHub issues and pull requests into a dependency-ordered execution program. Each
milestone has one concrete `objective`, agent-achievable completion criteria, and
explicit human gates. Use that objective as the goal text for a dedicated Codex
session. Do not set a token budget unless the human owner explicitly supplies one.

The program manifest is not a status tracker and does not replace task contracts:

- GitHub Issues and pull requests remain the live status authority.
- The coordinator re-queries issue, PR, check, review, and base state before each
  dispatch.
- The implementer creates a fresh task contract with the immutable dispatch base.
- A milestone goal ends at human-disposition readiness; it never authorizes an
  agent to merge, waive a blocker, change product semantics, or publish a release.
- Planner, implementer, verifier, and reviewer roles remain separate for every
  child task, even when one coordinator owns the milestone goal.

Validate a program with:

```bash
python scripts/agentic/validate_program.py --program .agentic/programs/<program>.json
```

## Worktree isolation and parallel work

- Each implementer works in its **own git worktree** so concurrent work never
  collides and no agent disturbs another's (or the user's) uncommitted changes.
- Parallel work is allowed **only across non-overlapping ownership packages**
  (see `.agentic/ownership.json`, which binds domains to real paths). Two agents
  must not edit the same paths on the same branch.

## Stacked PRs

Stacking is allowed but must be explicit: a stacked PR states its **base PR and
base SHA** so a reviewer never grades a moving base or only the top-of-stack diff.

## Risk classes

Risk drives required roles, checks, and evidence. The machine-readable definition
is `.agentic/policy.json`; `scripts/agentic/classify_change.py` derives the class
from the changed paths:

- Each path takes the highest class among the `classification.rules` globs it
  matches, and the PR takes the highest class among its paths.
- **Any path that matches no rule is R3** (`classification.default_unclassified`).
  This covers every unlisted path, not only sensitive ones: for example
  `scripts/docs/check_links.py`, any other `scripts/**/*.py` outside
  `scripts/agentic/` and the docs-site tooling below,
  `.gitignore` and `LICENSE.txt` all classify as R3.
- **Docs-site build and publish tooling is R1** (maintainer decision,
  [#1212](https://github.com/klausi3D/godotGS/issues/1212)). It
  is not R0, because it decides what the public site builds and publishes. It is
  not R3, because a required check covers each entry, or the entry cannot change
  what is published:
    - `docs-build`, a required check on `master` since 2026-10-08, builds with
      `mkdocs.yml` and `overrides/**`. It fails on a broken strict MkDocs build.
    - `overrides/**` may not contain script.
      `tests/ci/check_overrides_no_inline_script.py` fails on any `<script` tag
      (any case), any inline event-handler attribute (`on*=`), any `javascript:`
      URL, any `<iframe`, `<frame`, `<object` or `<embed` element, any `srcdoc`
      attribute, any `<meta http-equiv="refresh">` that carries a URL, and any
      file that is not `.html` or `.css` under `overrides/`. The tag, handler,
      element, `srcdoc` and refresh checks run on the raw source and again after
      HTML entity decoding, so an entity-encoded payload in an attribute is
      caught too. It runs
      in the `--guard-only` lane of the required `agentic-pr-gate`, and the guard
      script itself is R3 machinery. Script for the site lives in
      `docs/assets/javascripts/` (R3, below), loaded through `extra_javascript`.
    - No workflow runs `scripts/build_docs_site.py`, a local wrapper.
      `scripts/docs/release_acceptance.py` is report-only in `docs-build`, and
      `deploy` does not run it. Neither can change what is published.
    - `docs/requirements*.txt` (the docs toolchain inputs) are raised from R0
      (`docs/**`).
    - Code the `deploy` job executes, and `docs/assets/javascripts/**`, are R3 by
      their own rule, so neither `docs/**` (R0), `modules/gaussian_splatting/**`
      (R1) nor a later broader glob can lower them. `deploy` checks out with the
      default persisted credential and `contents: write`, then runs
      `scripts/stage_public_docs.py` on every deploy,
      `scripts/docs/redirect_cpp_api_to_latest.py` on `v*` tags, and
      `modules/gaussian_splatting/tests/check_project_settings_manifest.py`, which
      `scripts/generate_project_settings_reference.py` loads with `exec_module`.
      `mkdocs.yml` loads the JavaScript into readers' browsers.
      `tests/agentic/test_stage_public_docs.py` and
      `tests/agentic/test_cpp_api_redirect.py` check the scripts' output, and the
      strict MkDocs build neither runs nor inspects the JavaScript, so no check
      can catch an added side effect, such as a use of the write token.
    - `docs/requirements-lock.txt` is R3, by its own rule. `docs-build` and
      `deploy` install it, and `deploy` installs it with a write token in git
      config. The hashes only authenticate what the lock selects, and a green
      `docs-build` only proves that the install succeeded. No CI job installs
      `docs/requirements.txt` or `docs/requirements-site.txt` directly. They are
      the inputs the lock is compiled from.
    - `scripts/check_docs_media_budget.py` stays R3. `deploy` runs it right before
      publishing, and no test checks the budget script itself.
    - `scripts/build_documentation.py` and `scripts/docs/check_doxygen_output.py`
      stay R3. `docs-build` runs the PR's own copies, so a change that skips a
      generator (its committed output then stays unchanged and the freshness diff
      passes) or that accepts an empty Doxygen shell still passes, and no test
      checks either script.
- **A diff that touches `.agentic/policy.json` is forced to the top class**, R3
  (`SELF_REFERENTIAL_PATHS` in the classifier), although the rules list
  `.agentic/**` as R0. The rest of `.agentic/` stays R0.
- An empty changed-path set is R3, and renames are reported as both the deleted
  source and the added destination (`--no-renames`).

The "Required process" column is policy. Only the class derivation is enforced
by tooling; see
[What is enforced and what is process](#what-is-enforced-and-what-is-process).

| Class | Scope | Required process |
| --- | --- | --- |
| **R0** | Docs and agentic governance only. | Deterministic checks + one review. |
| **R1** | Local module/test changes with no GPU/persistence/engine risk. | Guards + targeted tests + correctness review. |
| **R2** | Renderer, shaders, compute, GPU sort, streaming, performance, VRAM. | R1 + GPU/performance review + runtime/GPU evidence. |
| **R3** | Godot-engine delta outside the module; persistence/file formats; release/security workflows; public API/compat; any unclassified path. | ADR before implementation + two reviews + CODEOWNER and human approval. None of these is enforced by branch protection (0 required approvals, code-owner review off). |

**What CI actually does with the risk class.** The required `agentic-pr-gate` check
derives the class from the PR's own diff (`classify_change.py --base-ref <PR base>`)
and publishes it, together with that class's `evidence_requirements` and
`deterministic_checks`, to the job summary. The derivation fails closed: an
unresolvable base ref fails the check, an unreadable base copy of
`.agentic/policy.json` fails the check (the gate classifies with the base copy, never
the PR's own), and an empty changed-path set is classified as
`classification.default_unclassified` (R3), not R0. The class itself is **not** a
failure condition: an R3 PR passes the gate exactly as an R0 PR does, and nothing
checks that the class's evidence or reviews were produced.

An author's **self-declared** class is *not consumed by CI today*. The
higher-of-the-two rule is implemented in `check_pr_contract.py`, but that script only
runs against the shipped fixture `.agentic/templates/task.json`, because the
repository has no per-PR contract source (a task-contract instance is a local agent
artifact and [`AGENTS.md`](../../AGENTS.md) forbids committing those). So a declared
class is a review-time convention, not an enforced one, and per-PR scope
(`owned_paths` / `forbidden_paths`) and evidence contracts are not enforced at all.
Wiring a contract source is the Phase-2 contract-source ADR; the limit is recorded in
[GitHub settings](github-settings.md) and in `.github/workflows/README.md`
(`GS-AUDIT-TEST-001`).

## What is enforced and what is process

Live branch protection for `master`, read with
`gh api repos/klausi3D/godotGS/branches/master/protection` on 2026-10-08: two
required checks (`agentic-pr-gate`, `docs-build`), `enforce_admins: true`, conversation resolution
required, `required_approving_review_count: 0`, `require_code_owner_reviews: false`,
no rulesets. [GitHub settings](github-settings.md) has the full table. That API,
not this page, is the source of truth.

**Enforced by tooling** (a PR cannot merge while these fail):

- A pull request is required, every review conversation must be resolved, and
  force pushes and branch deletion are refused (branch protection).
- The `agentic-pr-gate` check passes. It runs the automation-validator tests and
  the workflow contract check, `validate_repo_contract.py --strict-hierarchy` (the
  control plane is consistent, and the AGENTS.md files and the
  `docs/governance/` pages it lists exist), the `tests/agentic` suite, the
  documentation link check, and `run_module_tests.py --guard-only`.
- The `docs-build` check passes (`docs_pages.yml`). It regenerates the docs
  artifacts and fails when a committed generator output is stale, link-checks the
  docs including generated output, stages the public tree, checks the media budget
  and runs `mkdocs build --strict`. Its release-acceptance step is report-only.
- The risk class is derived from the PR's own diff against the base policy, and
  the derivation fails closed (above). The class is published, not acted on.

**Process only** (no setting or check enforces it; reviewers and the merging
human uphold it):

- Every review requirement in the risk-class table: one review for R0, the
  correctness review, the GPU/performance review for R2+, and for R3 the two
  reviews and the CODEOWNER and human approval. With 0 required approvals and
  code-owner review off, a PR with no approval merges once the gate is green, its
  conversations are resolved, and no "Request changes" review is outstanding.
  `required_pull_request_reviews` is enabled, so such a review from someone with
  write access holds the PR until that reviewer approves or the review is
  dismissed, and resolving its threads does not clear it. Today the only
  collaborator is the repository owner, who cannot request changes on a PR they
  authored, so on owner-authored PRs no one can place this hold.
  `.github/CODEOWNERS` names owners but does not block.
- The R3 design record (ADR or design-change issue) before implementation, and the
  evidence each class lists (`evidence_requirements`). `adr_required` and the
  rollback plan are checked by `check_pr_contract.py`, which runs only against the
  shipped fixture.
- The declared risk class, per-PR scope (`owned_paths` / `forbidden_paths`), and
  task and review contracts.
- Role separation (planner, implementer, verifier, reviewers), one task per branch
  and worktree, recorded base SHAs, and the stacked-PR base statement.
- "A human owns the merge": nothing stops an account with write access, including
  an agent using one, from merging a PR whose gate is green.
- Not weakening guards, baselines or thresholds. Some individual guards are
  shrink-only ratchets, but no general check exists.

## Legacy coordination data

`docs/agent_memory/` is a frozen legacy system and is **not** current status.
Active issues live in GitHub Issues; the board migrates there incrementally and
its history remains in git.
