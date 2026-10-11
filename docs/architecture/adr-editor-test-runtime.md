# ADR: Original Editor tests borrow the genuine editor runtime

Status: accepted design; implementation and native qualification pending.
Risk: R3. Human CODEOWNER review is required for the upstream test-listener seam.

## Problem

The strict Gaussian Editor corpus selects 23 original cases. Its headless
`--test` producer creates EditorSettings and a mock SceneTree but no EditorNode.
The advanced import dialog therefore returns with its existing skip message.
Parenting the dialog in the mock tree does not supply its genuine inspector and
viewport dependencies. An offscreen GPU test device is also insufficient.

## Decision

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

## Validation and limits

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
