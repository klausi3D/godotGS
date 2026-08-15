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
validated by `scripts/agentic/validate_review.py`). Those files are added by a
sibling PR in this foundation series; until the series is merged, follow the parts
already present on your branch. Every review records the `base_sha`/`head_sha` it
judged, the tests/evidence it reviewed, and its **blind spots**. Each finding has a
severity:

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
  same-repo branch. This is the required standard the agentic-foundation series brings
  every self-hosted workflow under (via the per-job fork guard); verify a given
  workflow actually carries the guard rather than assuming the boundary is already
  closed everywhere (see `.github/workflows/README.md`). Any change that relaxes a
  gate or the runner trust boundary must be documented and human-approved.

## Repository enforcement

The branch-protection and required-check settings that back this policy are listed
in [GitHub settings](github-settings.md); they are applied manually by a
maintainer.
