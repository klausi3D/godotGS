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

As practised, an agent authors the change, independent agent reviewers (Claude and Codex)
review it, the repository owner merges it or delegates the merge once CI is fully green, R2
and R3 changes need the owner's OK, and no second human review is required until a second
human maintainer joins; see the
[review and merge policy ADR](../architecture/adr-review-merge-policy.md).

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
  check runs -- as the **immutable base** copy of the script (#1167) -- against the
  PR's own changed paths and the base copy of `.agentic/policy.json`. An author's self-declared class is **not consumed by CI**:
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
  boundary must be documented and human-approved. Recorded relaxation: Production
  Gates readiness enforcement (#1164) enforces `897_907_ready`, `900_ready` and
  `815_ready`; `943_ready`, `871_ready` and `902_ready` are reported, not enforced,
  until #1241 (maintainer decision 2026-10-13; the report-only set may only shrink).
  Recorded relaxation (user decision 2026-11-01, owner: repository owner @klausi3D): the shipped
  **Windows archives are not executed by any gating lane**. The Windows editor zip and the
  Windows export template zip are built on GitHub-hosted `windows-latest` and attested, and
  `release_candidate_gate` / `publish_release` wait for and require both builds, but the only
  lane that runs the Windows exe, `export_smoke_windows`, needs a Windows GPU runner that no
  longer exists. It is **skipped unless the repository variable `GODOTGS_WINDOWS_GPU_RUNNER`
  is `true`** and is in no `needs:` list and no gate `if:`. The gating export smoke is
  `export_smoke_linux` (the Linux template, executed on the RTX 3080 runner). Retirement path:
  register a Windows GPU runner labelled `self-hosted, Windows, X64, gpu, d3d12`, set
  `GODOTGS_WINDOWS_GPU_RUNNER=true`, then add `export_smoke_windows` to the `needs:` and `if:` of
  both gates in the same change and remove this exception; the Windows contract (same script,
  `--target windows`, same negative control) is kept intact for that day.
  Recorded relaxation (maintainer approval 2026-11-07, owner: repository owner @klausi3D,
  "CI diet"): **per-merge release builds and duplicate gate runs moved to nightly; up to
  one-day detection window for regressions on master accepted.** Concretely: no workflow runs
  self-hosted work on a push to `master`; `release_builds.yml` lost its `pull_request` and
  branch-`push` triggers (it runs nightly on master HEAD, on `v*` tags and on dispatch); the
  duplicate Production Gates `guards` job and the guard phase inside the GPU job are gone (the
  guard phase runs once per PR in the required `agentic-pr-gate`, and in full nightly); the
  path-gated guard self-tests (`run_module_tests.py --guard-self-tests auto`) are skipped on
  PRs whose diff cannot affect them; the GPU job is skipped on PRs that change no code, and the
  two slow streaming scenarios are skipped on PRs that cannot affect streaming, HLOD or the
  renderer. Every one of these runs, unfiltered, in the nightly `baseline_qa.yml` /
  `release_builds.yml` / `gaussian_shader_validation.yml` runs on master HEAD; no threshold,
  baseline or gate predicate changed. What the window costs: a regression that only appears
  when two individually green PRs combine, a release-packaging or export-smoke regression, or
  a guard self-test failure that a PR's path gate skipped is reported by the next nightly
  instead of on the merge. Retirement path: turn on `strict` required checks or the merge
  queue (every workflow already wires `merge_group`), or put a lane back on `push`, in a
  change that updates this record. Layout and the coverage-move table:
  `.github/workflows/README.md` ("CI layout") and `docs/reference/build-test-ci.md`.

## Repository enforcement

The branch-protection and required-check settings that back this policy are listed
in [GitHub settings](github-settings.md); they are applied manually by a
maintainer. They enforce less than this page asks for. Branch protection requires
the `agentic-pr-gate` and `docs-build` checks and resolved conversations, but **0 approving reviews**
and **no code-owner review**. Layers 2 to 4 above, the owner's OK for R2 and R3, and
the blocker and waiver rules are therefore upheld by reviewers and the merging human,
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
