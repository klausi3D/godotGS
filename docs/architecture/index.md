# Architecture Documentation

Use this folder for architecture-level documentation and deep technical references.

## Canonical Entry Points

- [Architecture overview](overview.md)
- [Render pipeline architecture](render-pipeline.md)
- [Culling and hierarchy](culling-and-hierarchy.md)
- [Lighting system architecture](lighting-system.md)
- [Gaussian ProjectSettings contract](gaussian-project-settings-contract.md)
- [Unified Gaussian pipeline refactor plan](gaussian-pipeline-unification-plan.md)
- [Resident-instanced renderer contract](gaussian-resident-instanced-contract.md)
- [Gaussian pipeline deprecation and deletion plan](gaussian-pipeline-deprecation-deletion-plan.md)

## Module Deep References

- [Module architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md)
- [Memory subsystem and residency invariants](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
- [Stage-first ownership inventory](stage-first-ownership-inventory.md)

## Architecture Decision Records

ADRs live in this folder as `adr-<slug>.md`. There is no number sequence and no separate
registry: the list below is the only hand-maintained index, and it had **drifted to 1 of 9
entries** before this section was added. A hand-written list of things that must stay
complete is already broken — if it drifts again, generate it from `docs/architecture/adr-*.md`
rather than patching it by hand.

- [Advisory lane result ledger](adr-advisory-lane-ledger.md)
- [Decompose GaussianSplatNode3D](adr-decompose-node3d.md)
- [Decompose the renderer facade into owned sub-contexts](adr-decompose-renderer-facade.md)
- [Decompose the scene director](adr-decompose-scene-director.md)
- [Decompose the streaming system](adr-decompose-streaming-system.md)
- [Explicit, signed splat colour encoding (SH sign + DC contract)](adr-splat-colour-encoding.md) — *Accepted* (2026-09-23; slice 1 landed in #1062)
- [Hierarchical LOD for streamed splat worlds](adr-hlod-streaming.md) — *Accepted* (2026-10-02; Refs #1131; not yet implemented)
- [Import importance pruning](adr-import-importance-pruning.md)
- [Import input hardening](adr-import-input-hardening.md)
- [Overflow drop telemetry](adr-overflow-drop-telemetry.md)
- [Required gate evaluates the PR's own diff](adr-gate-evaluates-pr-diff.md)
- [Phase-1 guard hardening: a closed cluster of remediations](adr-phase1-guard-hardening.md)
- [Single route per frame: world/instance node coexistence](adr-single-route-per-frame-node-coexistence.md)
- [Test quarantine manifest](adr-test-quarantine-manifest.md)
- [The candidate gate must distinguish measured evidence from defaulted evidence](adr-open-world-proof-content-validation.md)
- [Widen the public-alpha envelope to streaming open worlds](adr-alpha-envelope-widened-to-streaming.md)

- [Original Editor tests borrow the genuine editor runtime (2026-10-11)](#adr-original-editor-runtime-2026-10-11)
- [Preserve behavior when extracting validation helpers (2026-10-12)](#adr-preserve-validation-helper-behavior-2026-10-12)
- [Real Editor test fixture inputs (2026-10-12)](#adr-real-editor-test-fixture-inputs-2026-10-12)

## ADR: Original Editor runtime (2026-10-11) {#adr-original-editor-runtime-2026-10-11}


Status: accepted design; implementation and native qualification pending.
Risk: R3. Human CODEOWNER review is required for the upstream test-listener seam.

### Problem

The strict Gaussian Editor corpus selects 23 original cases. Its headless
`--test` producer creates EditorSettings and a mock SceneTree but no EditorNode.
The advanced import dialog therefore returns with its existing skip message.
Parenting the dialog in the mock tree does not supply its genuine inspector and
viewport dependencies. An offscreen GPU test device is also insufficient.

### Decision

Launch a disposable normal editor with an owned empty project. User arguments
after `--` select the TESTS-only module runner; normal engine bootstrap is intact.
Enter doctest directly from the owner-thread `process_frame` signal, outside both
message-queue flushes. The upstream listener has an explicit scoped borrowed mode
that preserves real servers, queues, editor settings and SceneTree; its default
mock mode stays unchanged. Select the original filters and assertions unchanged.

Seven dialog fixtures capture both genuine plugin-owned ObjectIDs before either
constructor, preserve singleton publication before reentrant UI actions, attach
only fully constructed owned fixtures to the real GUI, and audit ownership before
fallback cleanup. Original methods still use their actual fixture instances.

Observe the original preview requests at actual scheduling, callback entry and
completion. The original test-body flush must close its own callback before case
cleanup; later draining cannot repair that proof. Hold totals provisional until
a bounded final drain, safe logging restoration, normal child exit and full EOF.

A bounded error handler is active before doctest through cleanup. It records
errors even during the two original global print-suppression windows. Only an
actual successful injection CAS on the original owner, phase, fixture and input
may classify its own exact source-bound emission as expected. Other worker,
duplicate, overflow or disconnected events invalidate the whole Editor row.
Handler callbacks use bounded atomic/POD storage without allocation, module calls,
queue pumping, logging or another lock under the core global error lock.

### Validation and limits

Preserve all other canonical rows, skip/strictness gates, owner signatures and
activation boundaries. Required controls cover real worker errors during each
suppression window, observer and canonical wiring removal, singleton restoration,
parenting, unsafe queue entry, original callback/drain and logger restoration.
Static or mocked controls are not native editor/GPU evidence. Runtime qualification
requires all 23 real cases, actual assertion counts, zero skips, real callback and
error receipts, source/binary binding, normal exit and complete raw EOF.

The disposable editor retains existing deterministic RNG reseeding. Original
thumbnail cache cleanup is confined to its owned project/user data. Borrowed
preview workers are not stopped or replaced. No shipping ABI, format, shader,
bootstrap or signed-policy semantics change. Two fixed source reviews and human
maintainer disposition remain required before merge.

## ADR: Preserve validation helper behavior (2026-10-12) {#adr-preserve-validation-helper-behavior-2026-10-12}

Status: accepted design; helper implementation and qualification pending.
Risk: R3 for critical evidence, owner enforcement, Editor lifetime and shipping paths.

### Problem and decision

Complex validation methods obscure the individual checks needed to reject invalid
evidence. Extract small helpers from contiguous existing blocks, in separately
reviewed units: pure Python guards first, TESTS-only Editor code next, and the
shipping thumbnail cache last. Commit this decision before implementing helpers.

Preserve predicates, strict types, cardinality, literals, failure messages,
short-circuit and first-failure order, dependency calls and return values. Keep
shipping registration rules conservative. Preserve original Editor assertions,
filters, callback closure, queue checks, logging restoration, observer disposal
and fresh deadline checks after work and publication. Thumbnail helpers retain
the complete observation scope and existing cache order, counters and output.
Owner signatures, activation, trust policy, thresholds and baselines are unchanged.

### Task and acceptance

Freeze each unit's base, patch and original source before review. Compare old and
new actual Python functions on positive and adversarial inputs, including ordered
dependency calls and exact failures; delete a required predicate or helper call
and prove the independently wired consumer rejects it. Preserve C++ condition and
action order and TESTS/TOOLS boundaries. Shipping helper extraction needs compiler,
cache and image evidence; source-token equality cannot establish that behavior.

Run the existing guards and fresh quality checks on each final head. Native Editor
acceptance still requires all 23 original cases, positive assertions, zero skips,
actual callback/error/lifetime receipts and complete EOF. Synthetic models and
style-check success do not supply runtime evidence. Two independent reviews,
upstream CODEOWNER and human disposition remain required; no suppression, guard
relaxation, new owner authorization or runtime optimization is part of this task.

## ADR: Real Editor test fixture inputs (2026-10-12) {#adr-real-editor-test-fixture-inputs-2026-10-12}

Status: proposed design; separate fixture implementation and native qualification pending.
Risk: R3 for the canonical Editor evidence path.

### Problem

Godot `Dictionary` assignment shares its backing map. An Editor test that edits
a refreshed option set must therefore create a distinct map before changing it,
so the original option set still describes the first dialog state. The original
assertions remain the behavioral oracle.

Normal import-dialog previews read the source PLY or SPZ file even when the test
provides an asset with baseline metadata. A disposable Editor project must
supply valid source files at the original resource paths before these dialogs
run. An asset object or option dictionary does not supply those file bytes.

### Decision and implementation phases

First, create the advanced import-settings test's refreshed option map with
`initial_options.duplicate()`. Its current values are strings, integers and a
boolean, so a shallow duplicate gives the required independent map. Keep the
original dialog operations, asset and reimport overrides, expected values and
assertions. This fixture correction requires its own fixed source review and
native validation after this design is separately committed.

Second, prepare exactly `res://test_splats.ply` and `res://test_splats.spz` in the
fresh, owned disposable Editor project. Freeze the producer and staging design
before implementing this phase. Use the existing native synthetic SPZ writer
for SPZ and a bounded native PLY fixture producer. Retain the actual producer
source and binary identity, input values, output paths, byte counts and hashes.
Preserve the produced bytes when staging them; refuse missing, substituted,
malformed or changed inputs. Native producer outputs are required before
claiming that provenance. A constructed format model supplies no such claim.

Validate the exact staged bytes with the real `PLYLoader` and `SPZLoader` from
the same candidate binary. Require successful loads, nonempty Gaussian data and
the producer's declared count and data shape. The input preparation must fail
closed before running the original corpus when this prerequisite is missing.
The native fixture producer, loader checks and launcher integration require a
separate fixed source review; this decision leaves that implementation pending.

### Acceptance and scope

Retain the canonical case selection, every original action and assertion, and
the strict unexpected-error verdict. Missing files are fixture failures. Keep
the original Editor ownership, queue, callback, observer, allocation-injection,
logging, lifetime and deadline requirements. Validate the entire original
corpus on the actual candidate Editor, with positive assertions, no execution
skips, full raw output and EOF, normal child exit and owned-process closure.

Match native evidence to its source, binary and disposable project. Preserve
the original failing evidence and record later validation separately. Source
models, fixture metadata and loader success alone cannot qualify the full
Editor row. Independent source reviews and human/CODEOWNER disposition remain
required under the [review policy](../governance/review-policy.md).

This maintenance decision preserves expected outcomes and the existing evidence
classification and authorization contracts. Runtime warnings, environment
failures and process contention retain their existing independent verdicts.
