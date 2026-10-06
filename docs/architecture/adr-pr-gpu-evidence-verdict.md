# ADR: fail-closed GPU evidence verdict for pull requests

- Status: accepted for implementation by the v1 implementation request (2026-10-06).
- Risk: R3 (CI policy and the merge boundary).
- Base: `ab74e332aa8264ab6d0bfb2dc45ee83778a9e293`.

## Decision

Gaussian Production Gates reports an always-running hosted `gpu-evidence-gate`
job on every PR and merge-group event, without branch or path filters. The
immutable base classifier and policy determine whether R2/R3 evidence is required.
Classification failures, empty diffs and absent policy results never exempt a PR.

For required evidence, both the guard and Windows module-validation jobs must
actually finish successfully. A receipt produced only after the build, pipeline
smoke, full module tests, both runtime profiles and GPU-contention postflight
succeed binds their step outcomes and the binary/report hashes to the checkout
SHA, reviewed head SHA, review base, workflow run ID and attempt. The hosted
verdict downloads that receipt from its own run and checks every binding.

R0/R1 changes explicitly report that GPU evidence is not required. R2/R3 fork
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
