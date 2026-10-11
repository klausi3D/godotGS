# Review Policy

How changes are reviewed before a human merges them. Reviews layer four signals;
a change advances only when each required layer is satisfied. See
[agentic engineering](agentic-engineering.md) for roles and risk classes.

## The four layers

1. **Deterministic review.** Compiler/build, guards, targeted tests, contract
   checks, runtime harness, and (for R2+) GPU evidence / benchmarks. These are the
   non-negotiable, reproducible gates — see `docs/reference/build-test-ci.md`.
2. **Independent correctness review.** A reviewer with a fresh context evaluates
   the immutable `base..head` diff for correctness, threading/locking, resource
   lifetime, error/abort paths, back-compat, test quality, and scope creep. The
   reviewer changes no code.
3. **Domain review (R2+).** For renderer/shader/streaming/performance changes, a
   GPU/performance reviewer additionally checks host↔shader contracts, buffer
   bounds and zero paths, device-generation/resource ownership, synchronization
   and async readback, timing honesty, VRAM, and benchmark methodology.
4. **Human disposition.** A human maintainer decides the merge, resolves
   conflicting findings, and is the only party that can waive a blocker — with a
   written reason recorded on the PR.

## Findings

Reviews are emitted as structured findings (`.agentic/schemas/review.schema.json`,
validated by `scripts/agentic/validate_review.py`). CI runs that validator only
against the shipped fixture `.agentic/templates/review.json`; no PR is required to
carry a review file, so the format is a convention. Every review records the
`base_sha`/`head_sha` it judged, the tests/evidence it reviewed, and its **blind
spots**. Each finding has a severity:

| Severity | Meaning |
| --- | --- |
| `blocker` | Must be fixed (or waived by a human) before merge. |
| `high` | Strongly expected to be fixed; needs an explicit decision. |
| `medium` / `low` | Should be addressed or consciously deferred. |
| `note` | Informational. |

`blocker` and `high` findings must carry a concrete `required_action`.

## Rules

- **No voting.** Reviewers do not outvote each other. A well-founded blocker stays
  open until it is fixed or a human waives it; a second approval does not clear it.
- **Reviewers do not implement**, and verifiers do not fix — they report.
- A **reconciler** (human or agent) may deduplicate or merge overlapping findings
  but may **not** silently drop or downgrade a blocker.
- The risk class that sets the required review layers is **derived from the diff**
  by `scripts/agentic/classify_change.py`, which the required `agentic-pr-gate`
  check runs against the PR's own changed paths and the **immutable base** copy of
  `.agentic/policy.json`. An author's self-declared class is **not consumed by CI**:
  the higher-of-the-two cross-check exists in `check_pr_contract.py`, but that
  script only runs against the shipped fixture, because the repository has no
  per-PR contract source (Phase-2 contract-source ADR). Treat a declared class as a
  review-time convention and the derived class as the enforced one — see
  [agentic engineering](agentic-engineering.md) and `.github/workflows/README.md`
  for the same statement of the limit.
- **Untrusted fork code** must never be validated on persistent self-hosted runners;
  GPU/Windows validation happens only after a maintainer moves the change onto a
  same-repo branch. As of 2026-10-01, every self-hosted job either carries the per-job fork guard or
  does not run on `pull_request` events at all (see `.github/workflows/README.md`).
  `tests/ci/test_release_builds_runner_trust.py` (run by `--guard-only`) enforces
  this for `release_builds.yml` only; for the other workflows, check a new or changed
  self-hosted job at review time. Any change that relaxes a gate or the runner trust
  boundary must be documented and human-approved.

## Repository enforcement

The branch-protection and required-check settings that back this policy are listed
in [GitHub settings](github-settings.md); they are applied manually by a
maintainer. They enforce less than this page asks for. Branch protection requires
the `agentic-pr-gate` and `docs-build` checks and resolved conversations, but **0 approving reviews**
and **no code-owner review**. Layers 2 to 4 above, the R3 two-review rule, and the
blocker and waiver rules are therefore upheld by reviewers and the merging human,
not by GitHub. The full split is in
[agentic engineering](agentic-engineering.md#what-is-enforced-and-what-is-process).
Two mechanical backstops remain. Required conversation resolution: an open review
thread blocks the merge until someone resolves it. Anyone with write access,
including the PR's author, can resolve a thread, so this proves that someone
dispositioned the thread, not that a reviewer agreed. And because
`required_pull_request_reviews` is enabled, an outstanding "Request changes"
review from someone with write access holds the PR until that reviewer approves
or the review is dismissed, even with 0 required approvals; resolving its threads
does not clear it. The repository owner is today the only collaborator and cannot
request changes on a PR they authored, so on owner-authored PRs this second
backstop cannot be triggered.

## Proposed universal commit GPU evidence

The [R3 redesign](../architecture/adr-pr-gpu-evidence-verdict.md#universal-commit-code-redesign-2026-10-11)
adds a fixed-policy exact-commit evidence contract: every supported same-repo/master
PR needs full build, strict module tests, the full actual canonical GPU
catalogue (currently 13 batches, 8 required) and canonical runtime execution.
Advisory RD lane selection alone cannot qualify GPU proof. A fifth report binds
real batch stdout, assertion/lifetime/leak audit and fatal supervisor fields. R0/R1 paths and mutable labels cannot exempt this context. Existing
advisory batch/completion-marker severity stays unchanged, and optional visual,
export, performance and human acceptance remain separate obligations.

Canonical GitHub commit data must establish exactly one parent on every SUCCESS
and bootstrap path. Ordinary merge tips therefore need a normal final source
commit or rebase and fresh proof; an ineligible test-merge status can safely block
an eligible head through status precedence. Accepting those operational costs is
an owner activation decision. The proposed gate uses a dedicated source App,
master-only credential environment and actual strict/admin/append-only protection.
It is **NOT ACTIVE** until the [activation checklist](github-settings.md#universal-commit-gpu-evidence-activation-pending)
and real hosted/native positive/negative controls are complete. Local mocked tests
and source review do not establish enforced GPU evidence.

Two independent fixed-diff reviews and human/CODEOWNER disposition remain required.
The first v1 ADR and implementation shared one commit; a prior approved design
record has not been verified. A human must address that historical chronology gap;
the separately committed redesign does not claim retrospective compliance.

Later oracle/fixture policy changes require two fixed-diff reviews of separate
policy-only `T` and measured `S`, plus human/CODEOWNER acceptance. Exact canonical
personal-owner environment JSON authorizes data-only P2 delegation; unchanged
trusted default validators consume approved data without executing proposed
privileged code. Genuine new P2 proof precedes explicit owner retirement of P1
and required P2 activation, preserving strict/agentic/docs/App protection. Editable
comments only corroborate the authoritative payload. Fixed control/build/bootstrap
changes remain fail-closed separate maintenance scope. See the
[policy supplement](../architecture/adr-pr-gpu-evidence-verdict.md#canonical-gpu-completion-and-data-only-policy-migration-2026-10-11).
