# ADR: fail-closed GPU evidence verdict for pull requests

- Status: universal redesign prepared locally; implementation/evidence remain under review and owner activation is pending. Historical v1 sections below are superseded.
- Risk: R3 (CI policy and the merge boundary).
- Original v1 base: `ab74e332aa8264ab6d0bfb2dc45ee83778a9e293`.
- Redesign integration base: `012960050d29427bd00a42e1cd86840806d6bf5c`.

## Historical v1 decision (superseded by the proposed redesign)

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
again before publishing success. The original design claimed that a verdict bound to one base could never appear
on another PR/base. That claim was disproved by sequential reuse after closure
and the non-atomic read/status boundary; this historical mechanism is superseded. The SHA-keyed concurrency group is kept so pull-request and
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

At the bounded P2 stage, the SHA-wide design remained unresolved: a closed
PR's success can appear on a subsequent PR using the same head against another
base before asynchronous revalidation starts. Final metadata reads and status
publication are also separate API operations. Checking simultaneously open PRs
does not eliminate these intervals. The earlier absolute claim about
cross-base reuse and the strict-only activation alternative remain unresolved
design findings. The universal redesign below replaces that acceptance contract;
the bounded corrections alone never authorized activation or implied atomic
PR/base merge authorization.

## Universal commit-code redesign (2026-10-11)

This design is recorded in a separate commit before implementing its production
changes. Two independent agents reviewed the conservative profile; local source
and configuration preparation is authorized. Human/CODEOWNER acceptance, real
producer evidence and owner activation remain separate. This design does not
retroactively repair the initial ADR/implementation chronology described above.

### Acceptance contract

A new context with a distinct name beyond case certifies unconditional full canonical
build/module-GPU/runtime execution of the exact immutable commit `S` carrying
its status, under a fixed trusted policy `P`. It never certifies a private PR
number or mutable labels, paths or base-diff classification. Every accepted
commit, including docs/governance changes, needs the same complete evidence.
No R0/R1 exemption or runner-unavailability label can produce success. Existing
risk classification can remain informational outside this acceptance decision.

GitHub may evaluate a head commit or a test-merge commit. A measurement of `S`
never means another head executed. Equal source trees do not imply identical
binaries: Godot embeds the actual Git hash and commit timestamp. Those inputs
stay bound to the measured `S`.

The initial profile permits **SUCCESS only for commits with exactly one parent**.
Every publisher, lifecycle/rerun and bootstrap path uses the same guard over
canonical immutable GitHub commit-object data. Caller receipts, PR metadata and
local Git graft/replace views cannot establish parent eligibility. Synthetic
test merges have two parents, so they cannot gain success in this new context,
even when adopted as a future PR head. This removes both historical synthetic
head fallback and inverse synthetic-selection collisions without assumptions
about parent order or pre-activation target history. Pending/failure on an
ineligible synthetic commit may safely block another PR through merge-status
precedence; HEAD fallback is conditional, not a guaranteed availability path.

An ordinary merge tip is also ineligible until a normal final source commit or
rebase supplies a fully measured single-parent tip. Do not force-rewrite ancestry
or add unexplained empty commits. A final real correction after integration
preserves ancestry and naturally satisfies this constraint. The operational cost
is explicit and requires owner acceptance before activation.

### Trusted producer and immutable policy

Use `gpu-code-evidence-v1-<policy_digest>`, with a deterministic digest of the
trusted enforcement definitions, dependency manifest, action revisions, selected
lanes/runtime profiles and thresholds. Enumerate the execution/configuration
dependency closure and test it against real wiring. Policy changes select a new
context; old success cannot satisfy it. Producer definitions proposed by a PR
must match the trusted policy bundle, so changes to its own enforcement cannot
self-certify. Fixed execution changes require separate owner maintenance scope; narrowly
reviewed oracle/data changes use the noncircular transition in the supplement.

Build/guard/runtime producers explicitly check out the exact event head `S`,
not a synthetic `github.sha`. Mandatory module execution includes `--gpu` and
a fresh lane ledger proving selected strict module execution. Actual GPU
qualification additionally requires the full canonical harness/report in the
supplement below; advisory RD selection alone does not provide it. Required
build, pipeline, module-GPU, headless, streaming and contention postflight steps
must actually succeed. Optional visual/export/performance/readiness lanes retain
separate contracts; this context does not claim their completion.

Existing advisory module batches and the runtime completion-marker observation
ladder retain their established severity. This gate certifies required strict/GPU
coverage and the canonical blocking profile, not every advisory scenario assertion
or the legacy collector raw-log leak filter.

The receipt binds `head_sha == checkout_sha == S`, policy identity, exact
run/attempt and the actual binary selected by runtime commands. Hash the built
binary before runtime and after postflight and reject changes. When commands use
the Windows console launcher, bind its companion editor executable as well;
hashing only the launcher would omit the engine actually running. Remove stale
reports before capture. Include canonical report/ledger bytes in the exact-run
artifact; the trusted consumer checks their hashes, required batch coverage,
blocking verdicts and actual GitHub job/step outcomes. Well-formed hash strings
and skipped jobs are insufficient. Downloaded artifacts are never executed on
the privileged hosted consumer.

Forks stay off persistent self-hosted runners. A maintainer-owned branch must
validate the exact same `S` under `P`; rebasing creates another commit requiring
its own measurement. Unsupported fork/queue/target cases fail closed.

### Enforced target and publisher boundary

Initially support the canonical protected default/target branch `master` only.
Before SUCCESS, resolve its live tip, require it to be an ancestor of the current
head, and read back actual strict up-to-date/admin enforcement, blocked force
pushes/deletions and the existing required `agentic-pr-gate`/`docs-build`. In
steady state require the exact policy context and dedicated expected publisher
App ID. Missing/ambiguous rules, unknown rulesets, inaccessible proof or false
strict fail closed. A base advance outside the measured head after the final API
read is blocked by GitHub strict enforcement; an advance already contained in
the head adds no untested code. Read-to-POST remains non-atomic and is not claimed
to authorize a PR/base transaction. Repository administrators changing the
trusted policy/protection remain the owner trust boundary.

The dedicated publisher App has repository-scoped statuses-write permission.
A distinct read-only proof credential has Administration(read) for branch
protection and Actions(read) for environment/deployment-policy inspection.
Provisioning/installing either credential is an owner action. App keys and proof
credentials are environment secrets in `trusted-gpu-verdict`, restricted to the
exact `master` branch-type workflow ref: no PR merge refs, topic branches, tags
or wildcard fallback. Privileged hosted jobs run only trusted immutable default
code with pinned actions and never execute a proposed checkout. Producer/build
jobs receive neither credential nor that environment. Read back environment
restrictions; ordinary repository-secret fallback is prohibited.

Independent hosted timeout cleanup remains bounded and conservative. API/service
outage or whole-workflow cancellation can prevent terminal publication. Checking
a latest status before replacing this run's pending status is not an atomic
read/write operation. None of these failure/pending limits constitute success.

### Reviewed staged activation

Source preparation does not change GitHub settings or install a gate. Current
observed strict protection is false and this context is **NOT ACTIVE**.

The implementation migration must first enter trusted default code under the
existing gates, after independent source/native review; changed policy definitions
cannot self-certify against their preceding bundle. A matching single-parent
candidate can then register the newly trusted context.

1. Owner enables strict/admin enforcement while preserving required agentic/docs
   checks and reads it back.
2. Owner approves a named immutable bootstrap commit/policy/run. A genuine
   complete single-parent receipt plus strict/ancestry/trust proof may register
   the new context before it is required; this is not an exemption or bypass.
3. Owner adds the exact context with the dedicated expected App ID, preserving
   existing gates, then verifies readback and actual hosted positive/negative
   cases. GitHub requires a recent successful check for registration; do not
   assume pending-only bootstrap works.
4. Retire the one-shot bootstrap allowance. Later oracle/fixture changes use
   the exact data-only transition below: genuine new-policy proof precedes explicit
   owner retirement/replacement of the old requirement, rather than impossible overlap.

Final owner approval applies to concrete activation/settings/credentials, after
complete reviewable source/configuration is prepared. Humans alone merge.

### Required negative controls

Use canonical one-/two-parent commit fixtures and actual publisher wiring to
reject historical synthetic-as-head, inverse selection and pre-activation rewind
models. Mutate the parent predicate, its SUCCESS/ bootstrap wiring, policy pins
and unconditional lane selection to prove RED/restored GREEN. Cover strict false,
wrong App/context, unavailable enforcement API, new head/base, mutable metadata,
wrong checkout/run/attempt, stale/changed binary, missing report bytes, skipped
GPU batches, corrupt report verdicts, fork safety and cleanup failure limits.
Local mocked checks do not establish hosted enforcement or native GPU execution.

Official contracts: [required status selection and registration](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks),
[protected-branch strict/App enforcement](https://docs.github.com/en/repositories/configuring-branches-and-merges-in-your-repository/managing-protected-branches/about-protected-branches),
[protected branch REST permissions](https://docs.github.com/en/rest/branches/branch-protection),
[environment REST permissions](https://docs.github.com/en/rest/deployments/environments),
[deployment-ref policy](https://docs.github.com/en/actions/reference/workflows-and-actions/deployments-and-environments),
and [test-merge regeneration](https://github.blog/changelog/2026-02-19-changes-to-test-merge-commit-generation-for-pull-requests/).
Race/collision cases above are design inferences, not live exploitation evidence.

## Canonical GPU completion and data-only policy migration (2026-10-11)

Status: approved design for local preparation, before this supplement's production
implementation. Two independent reviews covered the GPU, closure and corrected
owner authority. Native/hosted proof, human/CODEOWNER acceptance and activation
remain pending. This separate design commit preserves the earlier chronology;
it does not retroactively establish a design before the original v1 implementation.

### Actual GPU profile and report consumption

The prior module `--gpu` selection includes advisory requires-RD lanes that can
skip without a RenderingDevice. It is not actual GPU-batch proof. Add the existing
`run_gpu_harness.py` unconditionally for supported same-repository master PRs,
with the bound executable, a fresh report, `--baseline-mode compare` and the full
catalogue (currently 13 batches; 8 required). Do not narrow `--batch` or relax
timeouts, skips, baselines or thresholds. Streaming runtime remains additional
evidence, rather than a replacement for GPU batches.

A fifth bound report must accompany the receipt and actual successful hosted
step metadata. The unchanged trusted consumer checks complete ordered catalogue,
filters/exclusions, required manifest agreement/minima, all current fatal fields
and their types, authoritative totals and each required row's actual stdout
through the canonical parser. Reconcile counters, lifetime/leak and assertion
audit markers. Missing, hollow, skipped, timed-out or altered evidence fails.
Doctest's filter-exclusion count remains benign; declared advisory exclusions and
skip severity retain the canonical supervisor contract. Directly launch the new
checker and its negative controls from the existing required guard; an inventory
exception is not a fix for a checker that never runs.

### Fixed repository execution and oracle closure

Pin all canonical tests and fixture/project inputs, Gaussian native test oracles,
GPU bootstrap/listener/macros, `main/main.cpp`, doctest, the renderer manifest,
all `SCsub` definitions, root build Python and build-tree Python under
`core`, `main`, `editor`, `drivers`, `platform`, `modules`, `servers`, `scene` and
`misc/utility`. This includes test library definitions, module discovery/generated
registration, SCons flags and builders. Select future matching files, regular
tracked blob bytes and modes. All workflows and privileged Python/action revisions
are fixed. Ordinary production C++/GLSL implementations remain measured candidate
code, rather than being frozen to default. External compiler/interpreter/driver
and hardware are explicit trusted-runner/runtime prerequisites, not a claimed
complete byte attestation of the platform.

Fixed executable/control inputs are nondelegatable. Changes to them after
activation fail closed and need separate owner maintenance scope. This availability
cost includes unrelated upstream test headers included by the canonical registry.
Compiled Gaussian oracle changes and canonical fixture/GDScript/reference data
may use the bounded transition below; macros, GPU runner/listener/registration,
Python, workflow/action and unknown executable definitions cannot.

### Noncircular data-only transition

Let `B` be current protected default executing privileged code, `T` a separate
immutable policy-only commit with canonical single parent `B`, and `S` the measured
candidate. `T` differs from `S`; its diff admits only narrowly selected oracle or
fixture data. Privileged code always executes from `B`. It reads approved `T`
bytes/hashes/JSON as data and never imports or executes proposed Python/YAML.
All nondelegatable definitions and selector bytes/modes equal `B`. The effective
`P2` combines unchanged execution with approved oracle/data bytes, and the full
selected bundle in `S` equals `P2`. Existing required sets, no-skip/no-timeout/
no-leak rules and minima cannot be weakened; unsupported schema/profile changes
fail closed. Fixed privileged validators must consume the effective approved data,
not silently continue to use old default configuration.

The exact JSON migration record read from the canonical `trusted-gpu-verdict`
environment-variable endpoint is the sole authority. Support only a personal-owner
repository: GitHub requires its owner to configure environments and create their
variables. Bind current `B`, `T`, `S`, exact policy/context, dedicated App, genuine
source run/attempt and expiry. No repository-variable, workflow-env, comment or
receipt fallback. A read-only proof credential needs Administration(read),
Actions(read) and Environments(read); the publisher remains statuses(write) only.
No environment-write credential may reach untrusted code.

A public owner-authored comment can corroborate the exact record digest but is
not an owner signature: another repository writer can edit its text while the
original creator identity persists. Such an edit cannot redirect the authoritative
environment payload; changed, missing or unreadable corroboration fails closed.
Organization/custom-role authority is outside this supported profile. Every final
SUCCESS repeats canonical parent, producer-policy, complete environment record,
current-base, expiry, App, exact run and full evidence checks.

After fixed-diff independent and human/CODEOWNER review, an owner may configure
this exact record. Genuine complete `S/P2` evidence may register only the new `P2`
context before it is required; it never produces an old `P1` success. The owner
then replaces `P1` with `P2` in one required-checks update, preserving strict,
existing agentic/docs/source App and other protection, and verifies readback
before permitting merge. `P1` is explicitly retired; requiring impossible overlap
would prevent the policy change from entering default. This settings update and
its readback are not a cross-API transaction or compare-and-swap. The human merges
`S`, default now carries `P2`, and the owner retires the one-shot record. A target
advance invalidates the old record; no generic bootstrap pin bypass is supplied.

Expiry limits publication, not the truth of immutable measured evidence. Owner
configuration changes between read and POST remain an administrative trust
boundary. Once-minted App credentials expire after one hour while polling permits
150 minutes; fresh completed-event controllers ordinarily recover, but hosted
availability must be measured. Whole-workflow cancellation and API outages can
still prevent publication; missing/failure never becomes success.

Required controls include changed C++ assertions/fixtures, unauthorized execution
or selector changes, relaxed/dropped policy, wrong or expired B/T/S/context/App/
run binding, edited-owner-comment redirection, stale/changed report bytes,
missing actual GPU step/report/audit, and real guard-launch deletion with RED and
restored GREEN. Portable fixture observations are not native GPU qualification.
Existing optional visual/export/performance and legacy collector raw-leak-filter
limits remain separate; this design claims only its explicit canonical profile.

Primary authority contracts: [environment variables](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-variables),
[environment configuration](https://docs.github.com/en/actions/how-tos/deploy/configure-and-manage-deployments/manage-environments),
[environment variable API](https://docs.github.com/en/rest/actions/variables#get-an-environment-variable),
and [comment editing](https://docs.github.com/en/communities/moderating-comments-and-conversations/managing-disruptive-comments).

## 2026-10-11 isolated critical-file risk classification supplement

The privileged owner/policy implementation (`scripts/agentic/gpu_commit_policy.py`),
its dependency manifest (`scripts/agentic/gpu_commit_policy.json`) and the universal
receipt validator (`tests/ci/check_universal_gpu_evidence.py`) each belong to the
existing R3 critical CI gate machinery rule. Classifying the complete feature as
R3 does not establish that future changes to any one of these files receive R3
review and evidence obligations. The proposed correction adds these exact paths
and tests each independently through the actual classifier. It preserves the
other risk rules and changes no evidence threshold or activation requirement.
This supplement is committed before that classification implementation; the
original initial ADR chronology gap remains unchanged.

## 2026-10-11 signed owner-grant authority redesign (v9)

This proposed correction supersedes the v7 claim that canonical environment JSON
provides owner-only authority. Environment configuration and REST environment
variable mutation have different documented access rules. A repository writer
may edit an original owner comment without changing its original creator, so
matching that creator and a changed body digest does not authenticate the payload.
That source authority defect remains open until implementation, evidence and
independent source review complete. The explored server-edit-history alternative
(v8) was not implemented; this design relies on standard signatures.

Both bootstrap and data-only migration require an exact owner-signed binding:
version, distinct grant kind, canonical repository/owner/target, current trusted
base B, policy B or separate reviewed T, measured S, policy context, publisher App,
producer run/attempt and UTC expiry. Environment variables carry only the binding
and public OpenSSH signature. No unsigned fallback, original-comment identity or
mutable approval metadata supplies authority. Sorted compact UTF-8 binding bytes
are signed and verified under namespace `godotgs-owner-grant-v1`.

A fixed trusted-default verifier invokes standard `ssh-keygen -Y verify` with a
fixed principal, allowed-signers content derived only from an immutable trusted
Ed25519 public key, a pinned absolute executable path/hash and bounded execution.
There is no custom cryptography or transport-supplied executable/key/options. The
helper and key/tool configuration are fixed nondelegatable policy dependencies
and R3 machinery. The owner private key stays offline and never enters CI, the
repository or agent tools. Only temporary synthetic test keys are used locally.

Initial configuration deliberately has no owner key or approved tool hash. All
SUCCESS paths, including ordinary already-required same-policy execution, fail
closed until a separately reviewed owner maintenance landing supplies actual
public key/tool pins. Neither a placeholder key nor this source commit activates
the gate. Hosted executable drift stops availability and requires reviewed
nondelegatable maintenance with a fresh policy context; a signed data-only grant
cannot update privileged code, keys or verifier pins. Linked libraries and the
GitHub-hosted runtime remain the explicit external toolchain trust boundary.

The same authority helper guards bootstrap, migration and final SUCCESS rechecks.
Every success still requires real fresh full GPU/strict module/runtime receipt,
canonical single-parent identity, immutable producer policy, current strict base
ancestry, expected App and exact master-only credential environment. A signature
authenticates approval and cannot substitute for execution. Changes after the
final API read do not establish atomic revocation or a PR/base transaction.

Actual synthetic Linux OpenSSH tests must distinguish valid proof from altered
bytes/signatures, wrong keys/namespaces, replayed or mismatched bindings and expired
grants. Independent removal of signature verification, shared bootstrap/migration
checks and the final recheck must fail. Required hosted Linux coverage must execute
real crypto; native Windows portable checks are reported separately. Initial
activation still needs reviewed configured B, a meaningful same-policy one-parent
PR S with genuine full native producer evidence, the owner's offline exact grant,
then actual publisher registration and required-context/App/strict readback.
Humans own source acceptance, the original pre-ADR chronology disposition, actual
key/configuration approval and merge. Native/hosted activation remains NOT_RUN.

This permanent design section is committed before the new authority behavior.
The standard protocol is specified by the [OpenSSH ssh-keygen manual](https://man.openbsd.org/ssh-keygen).
