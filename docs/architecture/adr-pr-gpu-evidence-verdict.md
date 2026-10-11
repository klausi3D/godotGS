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

The producer runs the proposed tree, so the trusted verdict only accepts evidence
whose producer definitions are unchanged from the base. These definitions are the
production-gates workflow, the receipt writer, the GPU-contention verdict script and
the runner GPU-environment preflight. A wiring test derives the scripts the producer's
preflight, postflight and receipt steps run and fails if one is not pinned.
A PR that changes any of them fails the verdict and needs maintainer disposition,
because a run cannot certify itself. The test harnesses the steps invoke stay PR
code by design, since they are the subject under test and are covered by review.

The commit status is keyed by SHA alone. The verdict therefore also fails while the
proposed head SHA is the head of another open PR, both before an R0/R1 exemption and
again before publishing success. A verdict bound to one base can then never appear on a
PR against another base. The SHA-keyed concurrency group is kept so pull-request and
lifecycle events for one head still supersede each other.

R0/R1 changes report promptly that GPU evidence is not required; the hosted
consumer does not wait for a GPU runner. Only the GPU evidence requirement is
exempted (maintainer decision, 2026-10-10). R1 module and test PRs still run the
Windows guard and module-validation jobs (build, module tests and runtime steps),
as the earlier path filter did. Those jobs report as their own checks; the R1
verdict does not consume their result. R0 docs and governance PRs skip the
self-hosted jobs, and the hosted `agentic-pr-gate` runs their guards. R2/R3 fork
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
against maintainers rewriting the bootstrap. Strict up-to-date protection or an enforced merge queue is an
activation prerequisite so advancing master cannot retain evidence for a stale base. Partial
reruns must rerun the complete canonical jobs; a receipt from an older attempt is
not accepted as current execution.

The controller publishes `gpu-evidence-gate` as a commit status on the proposed
head SHA. Its Actions job runs on the base SHA for `pull_request_target`, so that
job alone is deliberately not the required context. `statuses: write` is the only
write permission; repository content remains read-only. Failed or missing evidence
publishes a failure. After the resolver publishes `pending`, a separate hosted
cleanup job with an `always()` dependency replaces this run's own pending status
with failure if checkout, Python setup or the controller aborts, including a
controller job timeout. Verification has a 150-minute step budget inside the
185-minute controller job; polling defaults to 9,000 seconds. Cleanup has its own
five-minute job budget and derives the proposed SHA from the immutable event,
independently of controller job outputs. It checks the latest status and its
run URL before replacing a pending verdict; the read and subsequent write are
separate API operations, so a newer status can appear between them.
Whole-workflow cancellation, service unavailability or an unsuccessful final API
call can still prevent publication.
A missing or pending context is not passing evidence. No fork code is checked
out or run. Successful R0/R1 statuses describe their exemption explicitly,
separately from accepted R2/R3 evidence.

Producer `workflow_run` in-progress and completed events reset and revalidate the
proposed-head status, including reruns. PR base edits also reset the verdict.
A merge-queue lifecycle event takes the group's immutable base from the SHA suffix of
its `gh-readonly-queue/<base>/pr-<n>-<base_sha>` ref, the base the receipt binds, and
requires it to be an ancestor of the group head. It never re-reads the moving branch tip.
The metadata bootstrap publishes pending before resolving the current trusted base;
resolution failures publish failure. The consumer selects the
newest same-head run and its current attempt, so a prior successful attempt cannot satisfy a
failed or partial rerun. Other source events (push, scheduled, manual) are ignored.
The portable verdict tests run without third-party packages in the SCons-only
Windows environment. Separate YAML wiring tests run in the required hosted agentic
lane with its existing pinned automation dependency.

All PR edits trigger both producer and controller, including title/body edits.
The consumer rechecks the newest run/attempt and PR bindings after receipt reads;
a delayed lifecycle event cannot restore a superseded successful execution.

## Remediation design and chronology (2026-10-11)

The bounded timeout and exemption corrections above were specified before their
remediation implementation. They preserve the hosted, immutable-base trust
boundary and the evidence requirements. Regression tests inspect the actual
published descriptions and execute the independent cleanup script; negative
controls remove the cleanup dependency/event binding and the hosted test invocation.

The first committed ADR and initial evidence producer/checker implementation
share commit `bc24d29ccfab4d1f15100d9522ab98076edef6b5`. The hosted controller was
added later in `0d589830253c32b8d8d7ad24ea31784abdb732e8`. The original acceptance
statement cites a prior v1 implementation request; a linked prior approved
design-change record has not been verified during this remediation. A human must
provide that record or disposition the initial same-commit design/implementation
gap. Current source checks do not establish retrospective process compliance.

The SHA-wide status design still requires separate human disposition: a closed
PR's success can appear on a subsequent PR using the same head against another
base before asynchronous revalidation starts. Final metadata reads and status
publication are also separate API operations. Checking simultaneously open PRs
does not eliminate these intervals. The earlier absolute claim about
cross-base reuse and the strict-only activation alternative remain unresolved
design findings; the bounded corrections do not authorize activation or imply
atomic PR/base merge authorization.
