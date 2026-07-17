# ADR: Decompose `GaussianStreamingSystem` into owned components

- **Status:** Proposed (design note only — no production code changes in this PR).
  Adoption of any migration slice below requires separate, individually reviewed PRs.
- **Risk class:** the ADR itself is R0 (docs). The migration it proposes is **R2–R3**:
  the streaming runtime is persistence-adjacent VRAM-budget machinery, the pack path is
  multi-threaded, and the atlas stride is a GPU-payload contract (`.agentic/policy.json`
  classifies `core/gaussian_streaming.*` and the renderer atlas layout as elevated risk).
  Each slice needs runtime/GPU evidence and independent review before merge.
- **Base:** `origin/master` @ `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce`.
- **Related issues:** #222 (init dedup), #513 (stride-flip hazard), #543 (peak-memory),
  #563 (MEMORY_SUBSYSTEM.md drift), #582 (SH-capture heap overflow), #591 (duplicate
  sizing/clamp helpers), #606 (raw-storage snapshot contract). **Sibling ADR:**
  `gs/adr-decompose-scene-director` (same decomposition series).
- **Anchoring note:** every `file:line` below is against the base SHA above. Lines drift;
  re-anchor before implementing.

## Context / problem

`core/gaussian_streaming.cpp` is the largest god-class in the module (4,647 LOC;
`gaussian_streaming.h` is 591 LOC). An earlier split (ISSUE-006, see the include block at
`gaussian_streaming.h:15-26`) moved code into per-concern translation units and introduced
seven "controller" types:

| Unit | Header | Shape today |
| --- | --- | --- |
| `StreamingVisibilityController` | `streaming_visibility_controller.h` | member `visibility`; **bidirectional `friend`** |
| `StreamingEvictionController` | `streaming_eviction_controller.h` | member `eviction_controller` |
| `StreamingUploadPipeline` | `streaming_upload_pipeline.h` | member `upload_pipeline`; owns pack threads |
| `StreamingGlobalAtlasRegistry` | `streaming_global_atlas_registry.h` | member `global_atlas_registry`; **`friend`** |
| `StreamingVRAMRegulator` / `VRAMBudgetRegulator` | `streaming_vram_regulator.h` | referenced from `BudgetState` |
| `StreamingQuantization` | `streaming_quantization.h` | helpers |
| `ResidencyBudgetController` | `residency_budget_controller.h` | **pure static, typed I/O — the good pattern** |

The audit's finding is that **decomposition moved code to TUs, not ownership**. Four of the
seven controllers reach back into `GaussianStreamingSystem`'s *private* state — either by
taking `GaussianStreamingSystem &system` and touching its members through friendship, or by
being declared `friend` so `GaussianStreamingSystem` reaches into *them*. The private surface
that used to be one class's internals is now a shared mutable substrate aliased by five TUs.
The header even lists four `friend class` grants at `gaussian_streaming.h:31-34` plus two more
via the controllers' own `friend class GaussianStreamingSystem` (`streaming_visibility_controller.h:54`,
`streaming_global_atlas_registry.h:23`). Coupling **widened**: a field like `budget.vram_usage`
can now be mutated from any of those TUs, and the compiler enforces nothing.

`ResidencyBudgetController` (`residency_budget_controller.h:6-57`) is the counter-example and
the model for this ADR: it is a **stateless pure-static policy** — `AdmissionContext` in,
`AdmissionGate` out (`compute_admission_gate`, `decide_admission`). It touches no
`GaussianStreamingSystem` state at all; the caller reads state, builds a typed value, and
applies the returned decision. That is "owns nothing, borrows via a value type." The other
controllers should converge toward "owns its state, communicates via typed messages/returns."

## 1. Current friend-coupling map (the shared mutable substrate)

### 1a. `BudgetState budget` — the central shared field

`BudgetState` (`streaming_runtime_state.h:18-34`) is a plain struct member of
`GaussianStreamingSystem` (`gaussian_streaming.h:76`). Its ten mutable counters
(`loaded_chunks_count`, `vram_usage`, `evicted_bytes_total`, `chunks_loaded_this_frame`,
`pending_upload_bytes`, `pending_upload_slots`, `retired_upload_bytes_this_frame`,
`retired_upload_slots_this_frame`, `failed_upload_retirements`, `vram_chunk_cap_hit_this_frame`)
are written from **three different TUs and four+ distinct code shapes**:

- **`gaussian_streaming.cpp` free functions** (file-scope, take `BudgetState &r_budget` by
  reference): `_subtract_pending_upload_bytes` (`:139`), `_release_pending_upload_slot`
  (`:145`), `_release_failed_upload_retirement` (`:152`), `_release_cancelled_upload_retirement`
  (`:165`), `_record_successful_upload_retirement` (`:198`). These are a *partial* move toward
  the target — they operate on the value — but they are free functions, not methods of an
  owner, so nothing prevents a fifth caller from open-coding the same mutation.
- **`gaussian_streaming.cpp` methods**, open-coded: the load-side delta at
  `_complete_chunk_load_common` (`:3811-3812`), the per-frame resets at `:567-571`, `:812-816`,
  `:2759-2762`, `:637`/`:863`, and the eviction/unload decrements (next section).
- **`streaming_visibility_controller.cpp:250`** reads `system.budget.loaded_chunks_count`
  directly to gate zero-visible recovery.

There is no single method that "loads N bytes into the budget" or "releases a chunk from the
budget." The invariant *"`vram_usage` == Σ resident chunk bytes and `loaded_chunks_count` ==
count of `is_loaded` chunks"* is maintained by convention across every call site.

### 1b. The duplicated slot-release + budget-decrement loop (the #591-adjacent dup)

The same "release the atlas slot, decrement `loaded_chunks_count`, subtract `vram_usage`,
accumulate `evicted_bytes_total`" idiom is **open-coded four times**, near-identically:

| Site | Function | Guarded `--`? |
| --- | --- | --- |
| `gaussian_streaming.cpp:964-982` | `update_primary_asset_data` | yes (`if (>0)`) |
| `gaussian_streaming.cpp:1421-1438` | `register_asset` (rebuild path) | yes |
| `gaussian_streaming.cpp:1480-1498` | `unregister_asset` | yes |
| `gaussian_streaming.cpp:3853-3877` | `_unload_chunk` | **no — bare `budget.loaded_chunks_count--` at `:3873`** |

The fourth copy at `:3873` decrements **without** the `> 0` underflow guard the other three
carry — a concrete divergence the duplication already hides, and exactly the failure mode
#513 warns about (stride flips → `vram_usage` decrements skew, silently clamped at 0). The
load side is duplicated too: `:3811-3812` (production) and `:2436-2437` (test helper).

Separately, `StreamingUploadPipeline` reaches directly into `system.atlas_allocator` to
release slots from **eight** sites — `streaming_upload_pipeline.cpp:710, 858, 1575, 1583,
1670, 1680, 1778, 1786` — plus `system.persistent_buffer`, `system.persistent_buffer_size`,
`system.budget.vram_regulator` (`:555, 558, 562, 568, 611, 655, 797, 815-825, 1011-1033`).
The allocator and the budget counters are logically one resource (a slot *is* chunk bytes),
but their mutations live in two TUs with no shared authority, so a release in the pipeline and
a decrement in the core can drift.

This is the #591 pattern (duplicate sizing/clamp helpers across the resident and streaming
routes) at the intra-class scale: **no shared helper, so a future layout change can land on
one copy only.**

### 1c. Controllers that mutate `GaussianStreamingSystem` private state

- **`StreamingUploadPipeline`** — nearly every method takes `GaussianStreamingSystem &system`
  (`streaming_upload_pipeline.h:276-285`) and mutates `system.atlas_allocator`,
  `system.persistent_buffer`, `system.budget`, and calls back into private methods
  (`system._rollback_pending_chunk` at `:492`, `system._assert_chunk_state_invariant` at `:494`,
  `system._has_pending_upload_retirement` at `:1590`, `system._get_asset_state`/`_get_asset_chunks`
  at `:480/:485`). It also **owns the pack worker threads** (§3).
- **`StreamingVisibilityController`** — `friend class GaussianStreamingSystem`
  (`streaming_visibility_controller.h:54`) *and* takes `GaussianStreamingSystem &system` in
  `handle_zero_visible_chunk_recovery`, `update_chunk_visibility`,
  `update_chunk_lod_blend_factors`, `update_chunk_lod_parameters`,
  `prefetch_chunks_at_predicted_position` (`:109-131`). Bidirectional friendship: each class
  reaches into the other.
- **`StreamingEvictionController`** — `evict_least_recently_used(system, …)`,
  `evict_non_primary_lru(system)`, `ensure_resident_tracking(system)`
  (`streaming_eviction_controller.h:39-48`) walk and mutate `system.chunks` / asset chunks and
  drive slot release + budget decrement through the core.
- **`StreamingGlobalAtlasRegistry`** — `friend class GaussianStreamingSystem`
  (`streaming_global_atlas_registry.h:23`); `build_cpu_state(system)`,
  `update_chunk_meta_entry(system, …)`, `mark_chunk_meta_dirty(system, …)`,
  `sync_to_gpu(system, rd)` (`:44-48`) read `system` chunk/asset topology to rebuild GPU meta.

### 1d. Why "friend decomposition" failed to reduce coupling

1. **Friendship is all-or-nothing.** A `friend` grant exposes *every* private member, so the
   narrow interface the split intended (each controller sees only what it needs) is not
   expressible. The header even documents working *around* this at
   `gaussian_streaming.h:409-418`: test forwarders exist "because returning the registry by
   reference would expose private fields the registry's friendship with this class doesn't
   grant onward" — i.e. friendship is already too coarse to compose.
2. **`&system` parameters are ambient authority.** Passing `GaussianStreamingSystem &system`
   into a controller method is functionally identical to leaving the code in the god-class: the
   callee can touch anything. The LOC moved TUs; the *reachability graph* did not shrink.
3. **No ownership boundary means no invariant boundary.** Because `budget`, `chunks`,
   `atlas_allocator`, and `persistent_buffer` are all reachable from five TUs, no single TU can
   assert "I am the only writer of this field," so the resident-bytes invariant is unenforced
   and the four duplicated loops (1b) are the visible cost.

## 2. Target: components with explicit ownership + narrow interfaces

Principle: **each unit owns its state and exposes a narrow, typed interface; cross-unit
communication is by value (typed context/result structs or return values), not by handing out
`GaussianStreamingSystem &` or `friend` access.** `ResidencyBudgetController` is the reference.

### 2a. `ChunkResidencyLedger` — single budget-mutation authority

Introduce one owner of the resident-bytes accounting. It **owns** `BudgetState` (moved out of
`GaussianStreamingSystem` as a private member of the ledger) and the `GaussianAtlasAllocator`,
because a slot and its bytes are one resource. Its entire mutation surface is a handful of
typed methods:

```
class ChunkResidencyLedger {                 // owns BudgetState + GaussianAtlasAllocator
    // The ONLY code that increments/decrements loaded_chunks_count / vram_usage.
    void on_chunk_loaded(uint32_t asset_id, uint32_t chunk_id,
                         uint32_t buffer_slot, uint64_t chunk_bytes);
    ReleaseResult on_chunk_released(uint64_t chunk_key, uint64_t chunk_bytes,
                         bool was_loaded, bool release_slot);   // folds all four §1b loops
    void on_upload_staged(uint64_t bytes);                       // += pending_*
    void on_upload_retired(const RetirementTicket &t);           // folds §1a free fns
    void on_upload_failed(const RetirementTicket &t);
    void reset_per_frame_counters();
    BudgetSnapshot snapshot() const;         // read-only view for diagnostics/visibility
};
```

- The four duplicated loops (1b) collapse to one `on_chunk_released` call each; the underflow
  divergence at `:3873` is fixed once, structurally.
- The file-scope free functions (`_subtract_pending_upload_bytes`, `_release_*_retirement`,
  `_record_successful_upload_retirement`) become ledger methods; the `BudgetState &r_budget`
  parameter disappears because the ledger *is* the budget.
- Read-only consumers (visibility's `system.budget.loaded_chunks_count` at
  `visibility_controller.cpp:250`, all `get_vram_*` accessors) take a `BudgetSnapshot` value,
  not the live struct. This closes MEMORY_SUBSYSTEM.md's stated goal — *"the only place that
  regulates VRAM budgets"* (`MEMORY_SUBSYSTEM.md:34-36`) — which today is aspirational.
- The `VRAMBudgetRegulator` (policy) stays separate from the ledger (accounting), preserving
  the *"budget policy vs buffer allocation"* separation MEMORY_SUBSYSTEM.md:123-124 demands.

### 2b. `ChunkResidencyStore` — owner of chunk topology

The `chunks` vector, `asset_registry`, and per-asset chunk lists (`gaussian_streaming.h:80-87`)
become an owned store with typed lookups (`get_chunk(asset_id, chunk_id) -> ChunkRef`,
iteration callbacks) instead of raw `LocalVector<StreamingChunk> &` handed to friends. Eviction
and the registry consume `ChunkRef` / a read cursor; they no longer take `&system`.

### 2c. Convert the four `&system` controllers to typed collaborators

| Unit | New interface (value in, value/effect out) |
| --- | --- |
| `StreamingEvictionController` | `EvictionPlan plan(const EvictionContext&)` → core applies the plan via `ChunkResidencyLedger`. No `&system`; no slot release inside the controller. |
| `StreamingVisibilityController` | Takes `ChunkResidencyStore` read cursor + camera; returns a `VisibilityResult`. Drop the bidirectional `friend`. |
| `StreamingUploadPipeline` | Keeps ownership of pack threads and queues; its interface to residency becomes `UploadCompletion` events consumed by the ledger on the render thread (§3), replacing the 8 direct `system.atlas_allocator.release_slot` sites with ledger calls. |
| `StreamingGlobalAtlasRegistry` | `build/sync` take a `ChunkResidencyStore` read view instead of `friend`. |

Each conversion is a mechanical "replace member access `system.X` with a passed value or a
ledger call," which keeps diffs reviewable and behavior byte-identical when the extracted
methods are pure moves.

### 2d. Interaction with #591

The resident/streaming duplicate `sort_cap` clamp + buffer-population (#591,
`resident_instance_contract_publisher.cpp:800-814` vs `render_streaming_orchestrator.cpp:1770-1794`)
is the same anti-pattern one layer out. The `ChunkResidencyLedger::snapshot()` +
`get_buffer_capacity_splats()` become the shared sizing source both routes can call, so #591's
"extract a shared sizing/clamp helper" lands naturally on top of this ADR rather than as a
separate island. This ADR does **not** merge the two routes' *policies* (they legitimately
differ — atlas count + structural chunks vs regulator working set); it gives them one helper.

## 3. Threading model — make the implicit contract explicit and asserted

Today the render-thread single-caller assumption is **implicit and unasserted**:

- **`update_streaming` has exactly one production caller**: the render orchestrator, at
  `render_streaming_orchestrator.cpp:1675` and `:2406` (`tick_streaming_only`, `:2338`).
  There is **no `is_on_render_thread()` / `is_main_thread()` assert** in `update_streaming`
  (`gaussian_streaming.cpp:2459`) or in the orchestrator tick — the single-caller discipline is
  a convention, not a contract. (Contrast `gaussian_data.cpp:198`,
  `gaussian_splat_asset.cpp:197/1275/1307`, which *do* assert `Thread::is_main_thread()`.)
- **The pack workers are the one true concurrency boundary, and it is already correct by
  snapshot**: `pack_thread_func` (`streaming_upload_pipeline.cpp:427`) dequeues a self-contained
  `PackJob` (carries `Ref<GaussianData> data_ref` + copied `source_indices`,
  `streaming_upload_pipeline.h:35-47`) under `pack_mutex`, calls
  `build_pending_upload_from_pack_job(job, scratch)` — which takes **only the job snapshot, no
  `system`** — and enqueues the result under `pack_mutex`. The worker never mutates `chunks`,
  `budget`, `atlas_allocator`, or `persistent_buffer`. The `&system` it is handed
  (`pack_thread_func(system, …)`) is *nominal* — the snapshot boundary is what keeps it safe.
- **All shared-state mutation (slot release, budget deltas, buffer_update) happens on the
  render thread** inside `process_upload_queue` / `_begin_chunk_upload` / eviction, i.e. the
  `update_streaming` caller.

**Decision — state and assert the contract:**

1. Document in `gaussian_streaming.h` and MEMORY_SUBSYSTEM.md: *"`GaussianStreamingSystem`
   render-facing state (`chunks`, `budget`/ledger, `atlas_allocator`, `persistent_buffer`,
   `asset_registry`) is single-threaded on the render thread. The only cross-thread channel is
   the `pack_mutex`-guarded pack/upload queue, which carries value snapshots only."*
2. Add a cheap `DEV_ENABLED` assert at the top of `update_streaming` and the other mutating
   entry points (`register_asset`, `unregister_asset`, `begin/finalize_residency_requests`)
   that they run on the render thread (via the orchestrator's device/thread identity, matching
   the existing `is_on_render_thread` checks in `gaussian_splat_manager.cpp:387/463`). This is a
   **new assert, not a new lock** — it encodes the invariant that already holds and satisfies
   the module `AGENTS.md` rule "document the lock/thread that protects any shared field."
3. When residency mutation moves into `ChunkResidencyLedger` (§2a), the ledger is the natural
   home for the single-writer assertion: every mutating method asserts render-thread identity,
   turning "single caller by convention" into "single writer by construction."

No lock is added over render-thread-only state (the module `AGENTS.md` forbids inventing a
second lock over the same data). `pack_mutex` remains the sole streaming lock.

## 4. Staged migration (CI green at every step)

Each slice is an independently reviewable PR; none weakens a guard. Slices 1–3 are pure
refactors provable byte-identical; 4–6 shift ownership behind the same external behavior.

1. **S1 — extract `ChunkResidencyLedger` behind the current struct (no behavior change).**
   Wrap `BudgetState` in the ledger, route the five free functions (§1a) through it. Replace the
   four duplicated loops (§1b) with `on_chunk_released`. Fixes the `:3873` underflow divergence.
   *Evidence:* `tests/runtime` streaming tests + `get_vram_debug_stats` parity;
   `test_gpu_streaming.cpp` VRAM-accounting cases must be unchanged.
2. **S2 — fold the pipeline's 8 direct `atlas_allocator.release_slot` sites** into ledger
   `on_chunk_released`/`on_upload_failed` calls so slot+bytes move as one. *Evidence:* invariant
   counters (`invariant_slot_ownership_violations`, `streaming_runtime_state.h:118`) stay 0.
3. **S3 — resolve #222 init dedup** by extracting the shared `_reset_runtime_state()` from
   `initialize` (`gaussian_streaming.cpp:535-773`) and `initialize_empty` (`:782-950`). Do this
   *after* S1 so the ledger reset is a single call in the shared helper. Closes #222.
4. **S4 — `ChunkResidencyStore`:** move `chunks`/`asset_registry` behind typed accessors;
   convert `StreamingEvictionController` to `plan(EvictionContext) -> EvictionPlan` (drop
   `&system`).
5. **S5 — convert `StreamingVisibilityController` and `StreamingGlobalAtlasRegistry` to read
   views;** delete the two `friend class GaussianStreamingSystem` grants and narrow the four in
   `gaussian_streaming.h:31-34`. This is the step that actually *removes* friendship.
6. **S6 — add the render-thread asserts (§3)** and rewrite the stale MEMORY_SUBSYSTEM.md
   layout/component sections (#563) to match the new ownership map. Update
   `renderer-lifetime-ownership.md` cross-refs.

Guards that must stay green throughout: `run_module_tests.py --guard-only` (layout-sync guard
covers mirror structs incl. `ChunkQuantizationGPU`/`RenderParams`), the cull-signature parity
guard, and the shader-permutation compile guard. Because no GPU payload layout changes in S1–S6,
the atlas stride guard is untouched.

## 5. Sequencing with #582 (SH capture) and #513 (stride-flip)

Both pending fixes touch code this ADR reorganizes; **land them first, then refactor onto the
fixed baseline** — refactoring under them would force a rebase of a security fix and a
data-integrity fix through a large ownership move.

### #582 — SH-capture heap overflow (payload code) → **before S1**
#582 is a memory-safety fix on the untrusted-asset boundary in
`streaming_chunk_payload_source.cpp` (64-bit `sh_byte_count` vs `uint32_t`-truncated `resize`,
overrun in `_read_exact`). The pack path this ADR keeps intact consumes exactly that capture
API: `_pack_chunk_data` calls `capture_indexed_chunk_snapshot` / `capture_chunk_snapshot`
(`gaussian_streaming.cpp:3590-3616`), and the async worker builds its snapshot the same way
(`build_pending_upload_from_pack_job`). **Sequence:** merge #582 first. Then, in S1's snapshot
boundary documentation (§3), cite the now-hardened capture contract as the reason the worker is
safe. This also dovetails with #606 (raw-storage snapshot contract): the ledger/store
refactor should route *all* worker reads through `capture_*` snapshots, never raw storage — so
S4/S5 are the natural place to also close #606's three raw-reference call sites if scheduled
together (out of scope to fix here, but the store's read-view API should not expose raw
`GaussianData` storage to any worker).

### #513 — stride-flip hazard (GPU payload contract) → **before S1, or fold into S1**
#513 is the latent hazard where `_refresh_quantization_dc_compatibility` flips the effective
stride 80B↔144B (`_atlas_gaussian_stride_bytes`, `gaussian_streaming.cpp:3554-3562`) while
chunks are already resident, so the shader reinterprets 80B payloads at 144B stride *and* the
budget decrements at `:1491`/`:3874` use the *current* stride, skewing `vram_usage`. This ADR's
`ChunkResidencyLedger` is the correct home for #513's fix: because the ledger becomes the single
writer of `vram_usage`, it can record the **stride each chunk was loaded at** and decrement with
*that* stride, not the current one — structurally preventing the budget skew half of #513. The
GPU-corruption half (force evict/repack on effective-stride change) is a separate behavioral fix
that must land in `_refresh_quantization_dc_compatibility` **before** S4 moves chunk ownership,
so the evict-all path has a stable `chunks` owner to walk. **Sequence:** fix #513's evict/repack
in the current structure first; capture the load-time stride when S1 introduces
`on_chunk_loaded(…, chunk_bytes)` (pass bytes, not a recomputed stride) so the ledger is
immune to later flips by construction.

**Net ordering:** #582 → #513 (evict/repack) → **S1** (ledger, capturing load-time bytes,
absorbing #513's budget half) → S2 → **#222/S3** → S4 (+ optionally #606) → S5 (delete friends)
→ S6 (asserts + #563 doc rewrite + #591 shared helper).

## Consequences

- **Positive:** one writer for resident-bytes accounting (kills the four duplicated loops and
  the `:3873` underflow), friendship deleted (S5), the render-thread contract asserted not
  assumed, and #222/#513-budget/#563/#591 close as by-products on the same spine.
- **Negative / cost:** six sequenced PRs gated behind two pre-req bug fixes; S4–S5 touch hot
  render-thread paths (R2–R3) and need GPU/runtime evidence per slice. The ledger adds one
  indirection on the load/evict path (negligible; these are not per-splat).
- **Explicitly out of scope here:** merging the resident and streaming orchestrator *policies*
  (#591 keeps distinct sizing), the #543 chunked-staging/evict-then-grow work (orthogonal peak
  memory), and any GPU atlas layout change.
