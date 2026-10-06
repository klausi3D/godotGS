# ADR: fail-closed GPU evidence verdict for pull requests

- Status: accepted for implementation by the v1 implementation request (2026-10-06).
- Risk: R3 (CI policy and the merge boundary).
- Base: `ab74e332aa8264ab6d0bfb2dc45ee83778a9e293`.

## Decision

A separate hosted `gpu-evidence-gate` runs on `pull_request_target` and merge
groups. It checks out only the immutable base; proposed PR/fork code is never
executed. Classification and verification execute the trusted base implementation.
GitHub API reads supply changed paths, actual job/step outcomes and
receipt artifacts. Gaussian Production Gates emits the receipt on every PR and
merge-group event without branch/path filters.
Classification failures, empty diffs and absent policy results never exempt a PR.

For required evidence, both the guard and Windows module-validation jobs must
actually finish successfully. A receipt produced only after the build, pipeline
smoke, full module tests, both runtime profiles and GPU-contention postflight
succeed binds their step outcomes and the binary/report hashes to the checkout
SHA, reviewed head SHA, review base, workflow run ID and attempt. The trusted hosted
verdict downloads the exact producer run/attempt receipt and checks every binding
and the actual GitHub job/step execution. The source workflow also performs an
additional same-run receipt check, but that PR-controlled consumer is not the
required trusted verdict.

R0/R1 changes report promptly that GPU evidence is not required; the hosted
consumer does not wait for a GPU runner, and PR hardware jobs are skipped. R2/R3 fork
changes fail the hosted verdict without running fork code on a self-hosted
runner; a maintainer must move the change onto a same-repository branch. A runner
unavailability label cannot turn missing evidence into a passing verdict.

This verdict certifies the canonical streaming runtime lane, not all v1 visual,
Linux, performance or human acceptance requirements. Existing production-evidence
collection and readiness gates retain their separate roles.

## Activation and rollback

The new context must be added to branch protection **after** this workflow is
merged and the context has reported. Existing required checks stay required.
Until then the verdict is an additional PR check, not enforced merge protection.
Humans retain merge and waiver authority. Roll back with a focused revert and
explicitly disclose the resulting loss of enforcement; never silently remove a
required context to make a failed run pass.

## Validation

Unit tests reject skipped/failed jobs and steps, absent receipts, wrong SHAs,
wrong run/attempt, and missing hashes. Wiring tests assert the actual producer,
artifact transfer, postflight ordering and always-running terminal job. Run the
existing automation validator and guard suite without changing thresholds.

The workflow bootstrap and changes to its trusted implementation require human
and CODEOWNER review. Adding the status context is not equivalent to installing a
GitHub ruleset with an immutable organization-required workflow. This repository
uses status-based protection; the plan does not claim adversarial protection
against maintainers rewriting the bootstrap. Strict up-to-date protection is
recommended so advancing master cannot retain evidence for a stale base. Partial
reruns must rerun the complete canonical jobs; a receipt from an older attempt is
not accepted as current execution.

The controller publishes `gpu-evidence-gate` as a commit status on the proposed
head SHA. Its Actions job runs on the base SHA for `pull_request_target`, so that
job alone is deliberately not the required context. `statuses: write` is the only
write permission; repository content remains read-only. Failed or missing evidence
publishes a failure. Bootstrap/API failures leave a missing or pending context,
which does not satisfy required protection. No fork code is checked out or run.
