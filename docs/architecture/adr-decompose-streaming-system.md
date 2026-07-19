# ADR: Decompose `GaussianStreamingSystem` into owned components

- **Status:** Proposed (design note only — no production code changes in this PR).
  Adoption of any migration slice below requires separate, individually reviewed PRs.
- **Risk class:** the ADR itself is R0 (docs). The migration it proposes is **R2–R3**:
  the streaming runtime is persistence-adjacent VRAM-budget machinery, the pack path is
  multi-threaded, and the atlas stride is a GPU-payload contract (`.agentic/policy.json`
  classifies `core/gaussian_streaming.*` and the renderer atlas layout as elevated risk).
  Each slice needs runtime/GPU evidence and independent review before merge.
- **Base:** `origin/master` @ `384c2c6ad8d` (re-anchored in review round 2; the round-1
  base was `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce`. Every streaming file cited below is
  byte-identical between the two bases — only `nodes/gaussian_splat_world_3d.cpp` anchors
  moved, and they are re-cited against the new base).
- **Related issues:** #222 (init dedup), #513 (stride-flip hazard), #543 (peak-memory),
  #563 (MEMORY_SUBSYSTEM.md drift), #582 (SH-capture heap overflow), #591 (duplicate
  sizing/clamp helpers), #606 (raw-storage snapshot contract). **Sibling ADR:**
  `gs/adr-decompose-scene-director` (same decomposition series).
- **Anchoring note:** every `file:line` below is against the base SHA above. Lines drift;
  re-anchor before implementing.

## Context / problem

> **Correction (review round 2) — the class was undercounted by ~32%.** An earlier revision
> sized `GaussianStreamingSystem` as "138 methods in one TU." It is **166 out-of-line method
> definitions spread across five translation units**, so every "one big file" intuition about
> the migration cost is wrong by a third:
>
> | TU | `GaussianStreamingSystem::` definitions | LOC |
> | --- | ---: | ---: |
> | `core/gaussian_streaming.cpp` | **140** (138 + ctor `:272` + dtor `:300`) | 4,647 |
> | `core/streaming_lod_policy.cpp` | 8 | 204 |
> | `core/streaming_diagnostics_surface.cpp` | 8 | 897 |
> | `core/streaming_quantization.cpp` | 7 | 444 |
> | `core/streaming_atlas.cpp` | 3 | 214 |
> | **Total** | **166** | **6,406** (+ 591 in `gaussian_streaming.h` = **6,997**) |
>
> The two counts an earlier note gave — "138 in one TU" and "165 across 6" — are both wrong;
> the second is off by one on each axis. Recount before sizing any slice: the class surface
> is what S4/S5 must move, and four of the five TUs are already *satellite* TUs that reach
> into the same private state (§1c), so the ownership move is wider than the LOC suggests.

`core/gaussian_streaming.cpp` is the largest single TU of the module's largest god-class
(4,647 LOC; `gaussian_streaming.h` is 591 LOC). An earlier split (ISSUE-006, see the include block at
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

> **Correction (review round 2) — this is a style divergence, not a latent bug.** An earlier
> revision called the unguarded `:3873` decrement "a concrete divergence the duplication
> already hides" and made fixing it a headline benefit of S1. That overstates it.
> `_unload_chunk` early-returns unless the chunk is actually loaded:
>
> ```
> // gaussian_streaming.cpp:3849-3851
> if (chunk_idx >= asset_chunks.size() || !asset_chunks[chunk_idx].is_loaded) {
>     return;
> }
> ```
>
> The other three sites carry their `if (> 0)` guard because they decrement inside a loop
> under `if (chunk.is_loaded)` — the *same* precondition, expressed defensively. Under **I1**
> (`loaded_chunks_count` == count of `is_loaded` chunks) the counter is provably ≥ 1 whenever
> `:3873` runs, so all four sites are **behaviorally identical today**. Unifying them is
> worth doing — it removes a reader's obligation to re-derive that argument at each site, and
> it is free once `on_chunk_released` exists — but it is a **style/robustness unification, not
> a bug fix**, and S1 must not be justified or graded as if it fixes live underflow. The real
> #513 exposure is the *stride* used for the `vram_usage` decrement (I4), not the counter.

The load side is duplicated too: `:3811-3812` (production) and `:2436-2437` (test helper).

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

### 1e. Live-path reachability — most default scenes execute **zero** streaming code

> **Added in review round 2. This reframes the whole ADR and every characterization-test plan
> built on it.** The decomposition is still worth doing, but *how it can be validated* changes.

Backend selection is **world-scoped, and only one of the two node surfaces honours it**:

| Surface | Residency hint published | Honours `rendering/gaussian_splatting/streaming/route_policy`? |
| --- | --- | --- |
| `GaussianSplatWorld3D` | derived from the setting (`nodes/gaussian_splat_world_3d.cpp:506-514`) | **yes** — `GS_ROUTE_STREAMING` → `SUBMISSION_RESIDENCY_HINT_STREAMING`, else `…_RESIDENT` |
| `GaussianSplatNode3D` | **hard-pinned `SUBMISSION_RESIDENCY_HINT_RESIDENT`** at `nodes/gaussian_splat_node_3d.cpp:2451-2464` (`_register_in_director`) **and again** at `:2508-2509` (`_update_instance_params_in_director`) | **no** — the in-code comment states route policy is *"deliberately ignored here to keep backend selection world-scoped"* |

Consequences that this ADR's slices must respect:

1. **A scene built from plain `GaussianSplatNode3D` nodes never enters the streaming system
   at all**, no matter what `route_policy` is set to. That is the default authoring shape for
   single-asset content, so "most default node scenes execute zero streaming code" is the
   normal case, not an edge case.
2. **Any characterization harness fixtured on a `GaussianSplatNode3D` is vacuous by
   construction.** It will report green while asserting nothing about `chunks`, `budget`,
   `atlas_allocator`, or `persistent_buffer`, because none of them is ever touched. This is
   exactly the module's recurring "green test that executes nothing" failure mode, and it
   would silently certify S1–S6 as behavior-preserving without exercising a single line they
   move. **Streaming fixtures must be built on `GaussianSplatWorld3D` with `route_policy`
   pinned to `GS_ROUTE_STREAMING`**, and each suite must assert a non-zero streaming counter
   (e.g. `get_vram_debug_stats().loaded_chunks_count > 0`) *before* its behavioral assertions,
   so an accidentally-resident fixture fails loudly instead of passing vacuously.
3. **It bounds the blast radius, and therefore the risk class of the early slices.** The
   R2–R3 rating stands for S4/S5 (they move state the render thread reads on the world-backed
   route), but the population actually exposed to a streaming regression is "scenes using
   `GaussianSplatWorld3D` with `route_policy = 1`", not "all scenes". Slice risk arguments
   should say so rather than implying module-wide exposure.
4. **It is also the reason the §3a headless `tick_streaming_only` path matters so much**: with
   the direct-node route pinned resident, the headless main-thread path is a
   disproportionately large share of the streaming code that actually runs under test.

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

### 2d. Interaction with #591 — cross-route sizing helper **REJECTED**

> **Reversed in review round 2 (owner call).** The previous revision proposed that
> `ChunkResidencyLedger::snapshot()` + `get_buffer_capacity_splats()` "become the shared sizing
> source both routes can call." **That is rejected.** The resident and streaming routes are
> deliberately disjoint, and a shared sizing source re-couples them.

The evidence for the rejection:

- **The two routes derive their splat budget from different, unrelated quantities.** The
  resident publisher sizes from `atlas_gaussian_count` plus a hard
  `instance_count × dispatch_chunk_count × max_chunk_splats` floor taken with `MAX`
  (`resident_instance_contract_publisher.cpp:800-814`). The streaming orchestrator sizes from
  the regulator's working set, `MIN(get_effective_max_chunks(), dispatch_chunk_count)`
  (`render_streaming_orchestrator.cpp:1765-1794`). One takes a maximum over a structural
  requirement; the other takes a minimum against a live budget. They are not two spellings of
  one policy.
- **The resident route holds no reference to the streaming system at all** — `grep` for
  `streaming_system` in `resident_instance_contract_publisher.cpp` returns only comments
  explaining that `GaussianSplatNode3D` is *"resident-only by contract"*. Routing its sizing
  through `ChunkResidencyLedger::snapshot()` / `get_buffer_capacity_splats()` would introduce a
  **new** dependency from the resident route onto streaming-owned state — the precise coupling
  this ADR exists to remove, added one layer out. Combined with §1e (the direct-node route is
  hard-pinned resident and never boots the streaming system), the resident route would end up
  depending on a subsystem it is guaranteed never to instantiate.
- **The duplication #591 names is genuinely small.** What is actually identical is the ~4-line
  `sort_cap` clamp (`max_sort_elements > 0 ? … : UINT32_MAX`, then `MIN`). #591's own text
  concedes *"sizing policies deliberately differ"* and rates itself **low severity, no live
  bug**.

**Therefore:** this ADR takes **no dependency on #591**, and S6 no longer lists "#591 shared
helper" as a by-product. #591 remains open and is **decidable independently** of this
decomposition, on these terms:

| Option | What it shares | Cost |
| --- | --- | --- |
| **A — close #591 as won't-fix** | nothing | Two ~4-line clamps stay duplicated; a future `max_sort_elements` semantic change must be applied twice. Cheapest; preserves route disjointness completely. |
| **B — extract only the clamp** as a free function over scalars (`clamp_to_sort_cap(uint64_t, const GpuSortingConfig&)`) in a header both routes already include | the clamp expression, and nothing else — no ledger, no snapshot, no streaming state | Small and route-neutral: it takes plain integers, so it creates no dependency in either direction. Does **not** address the buffer-population duplication, which is policy and must stay separate. |
| **C — shared sizing source via the ledger** (the previous revision's proposal) | budget/capacity state | **Rejected above.** |

Recommendation for the owner: **B if #591 is to be actioned at all, otherwise A.** Either way
it is out of scope for S1–S6.

## 3. Threading model — per-API contracts, not a blanket render-thread assertion

> **Correction (review round 1).** An earlier revision of this ADR asserted that
> "`update_streaming` has exactly one production caller" and proposed adding a
> `DEV_ENABLED` **render-thread assert** to `update_streaming`, `register_asset`,
> `unregister_asset`, and `begin/finalize_residency_requests`. **Both halves were wrong**,
> and implementing them as written would have converted currently-valid calls into
> crashes. The corrected analysis and the per-API contract table below replace that
> proposal. The evidence is recorded in §3a because the mistake is easy to repeat.

### 3a. Why a blanket render-thread assert is wrong (verified against the base SHA)

Three independent facts refute "render thread only":

1. **There are two production call paths, and one of them is the main thread.**
   Besides the render-thread path (`render_streaming_orchestrator.cpp:1675`, inside
   `render_streaming_frame`, `:1463`), `update_streaming` is also reached at
   `render_streaming_orchestrator.cpp:2406` via `tick_streaming_only`. That path is
   entered from `GaussianSplatNode3D::_update_viewport_render_state`
   (`nodes/gaussian_splat_node_3d.cpp:1604-1605`, guarded by
   `OS::get_singleton()->has_feature("headless")`) ← `update_splats` (`:1453`) ←
   `process_gaussian_render` (`:1633`) ← `GaussianSplatManager::_process_active_nodes_main_thread`
   (`core/gaussian_splat_manager.cpp:660`), which is scheduled with
   `callable_mp(...).call_deferred()` (`:623`) and therefore runs on the **main thread**.
   `GaussianSplatRenderer::tick_streaming_only` (`renderer/gaussian_splat_renderer.cpp:2547-2563`)
   performs **no render-thread dispatch** — it calls the orchestrator directly on the
   caller's thread. The same path reaches `register_asset` / `unregister_asset`, because
   `sync_instance_pipeline_assets` (`render_streaming_orchestrator.cpp:986`, calls at
   `:1426`/`:1450`) is invoked from both `:1646` (render thread) and `:2376`
   (`tick_streaming_only`, main thread).
   In headless there is no render thread at all, so `RenderingServer::is_on_render_thread()`
   returns false and the proposed assert would fire on **every headless frame** — including
   the production-gates runtime harness and exported headless builds.

2. **These entry points are public, `ClassDB`-bound script API.**
   `GaussianStreamingSystem` is `GDREGISTER_CLASS`'d (`register_types.cpp:96`) and binds
   `initialize`, `update_streaming`, `begin_residency_requests`, `request_chunk_residency`,
   `request_asset_residency`, `finalize_residency_requests`, `begin_frame`, `end_frame`
   and the whole diagnostics surface (`core/gaussian_streaming.cpp:494-533`). A GDScript
   caller runs on the main thread by default. Adding a render-thread assert to a bound
   method is a **breaking public-API change**, not the documentation of an existing
   invariant.

3. **The module's own doctests call them from the doctest (main) thread** —
   e.g. `tests/test_gpu_streaming.cpp:1362-1600` and
   `tests/test_gaussian_streaming_lifecycle.cpp:259-877` call `register_asset`,
   `begin_residency_requests`, `finalize_residency_requests`, `unregister_asset` and
   `update_streaming` directly. A `DEV_ENABLED` render-thread assert would fail the
   module test suite, and "make the test pass" would then mean weakening the new assert —
   which the working rules forbid.

The real invariant is **not** thread *identity*; it is **serialization**: the render-facing
state has exactly one *active* caller at a time, whichever thread that caller is on. That is
what the design must encode.

### 3b. Per-API thread contracts

Every public entry point is assigned exactly one of three classes. This table is the
contract; §6's invariant list is what a slice is graded against.

| Class | Meaning | Enforcement |
| --- | --- | --- |
| **`[SERIALIZED]`** | Mutates render-facing state (`chunks`, budget/ledger, `atlas_allocator`, `persistent_buffer`, `asset_registry`). Callable from any thread, but **never concurrently with another `[SERIALIZED]` call on the same instance**. The caller (orchestrator or script) provides the serialization. | Debug **single-active-caller token** (§3c) — *not* a thread-identity assert. |
| **`[SNAPSHOT-SAFE]`** | Runs on a pack worker. Touches only a value snapshot handed to it; mutates no owner state. | Existing `pack_mutex` queue boundary; reviewed as "takes no `system` reference". |
| **`[READ-ONLY]`** | Diagnostics/getters. Safe to call concurrently with other `[READ-ONLY]` calls. Raced against a `[SERIALIZED]` call the consequence **differs by return type** — see the two sub-classes below; this distinction is load-bearing and was previously collapsed into "may observe a torn view". | Scalars: advisory, documented. `Dictionary` returns: **unresolved — see D-READONLY.** |
| &nbsp;&nbsp;↳ **scalar counters** | `get_vram_usage` (bound `gaussian_streaming.cpp:509`) → `_get_total_vram_usage_bytes` (`:2703`) reads `persistent_buffer_size` (`:2718`) and `budget.vram_usage` (`:2719`); `get_vram_debug_stats` (bound `:524`). Both are plain non-atomic members (`streaming_runtime_state.h:21`, `gaussian_streaming.h:91`) written by `[SERIALIZED]` paths at `gaussian_streaming.cpp:3811-3812` and `:3873-3877`. Racing yields a **stale or torn scalar**. | Advisory; acceptable. Returns a `BudgetSnapshot`/value copy once §2a lands. |
| &nbsp;&nbsp;↳ **`analytics_snapshot` (`Dictionary`)** | `get_streaming_analytics` (bound `:514`) returns `analytics_snapshot` (`streaming_diagnostics_surface.cpp:829-831`; member at `gaussian_streaming.h:168`). `end_frame` **reassigns that same `Dictionary`** (`streaming_diagnostics_surface.cpp:106+`). Copying a `Dictionary` refs a `SafeRefCount` through a **non-atomic `_p` pointer** (`core/variant/dictionary.cpp:43-44`, ref `:287`, unref/free `:345`) while the writer swaps it. That is **potential use-after-free, not a stale read** — and "return a value copy" does *not* fix it, because the copy is itself the racing operation. | **Open — D-READONLY.** |

Assignment of the current public surface:

| Entry point | Class | Notes |
| --- | --- | --- |
| `update_streaming` (`gaussian_streaming.h:214`) | `[SERIALIZED]` | Render thread via `render_streaming_frame`; **main thread** via `tick_streaming_only` in headless. Both are valid. |
| `register_asset` / `unregister_asset` (`:315-316`) | `[SERIALIZED]` | Same two paths via `sync_instance_pipeline_assets`. |
| `begin_residency_requests` / `finalize_residency_requests` (`:217`,`:220`) | `[SERIALIZED]` | Bracket a request generation; must not interleave with another bracket. |
| `request_chunk_residency` / `request_asset_residency` (`:218-219`) | `[SERIALIZED]` | Valid only *inside* an open begin/finalize bracket. |
| `initialize` / `initialize_empty` / `attach_memory_stream` | `[SERIALIZED]` | Additionally: must not run concurrently with any other call on the instance. |
| `set_chunk_payload_source` / `detach_source_data` | `[SERIALIZED]` | Mutates `asset_registry`. |
| `begin_frame` / `end_frame` (bound at `gaussian_streaming.cpp:507-508`) | `[SERIALIZED]` | Bound, and render-facing. `begin_frame` (`:3888-3899`) advances `current_frame_idx`, bumps `total_frame_count`, clears `frame.visible_chunks`, and calls `_reset_per_frame_counters()` (`:3893`) + `_process_upload_retirements()` (`:3895`). `end_frame` writes `analytics_snapshot` (`streaming_diagnostics_surface.cpp:102-106+`), reading `budget.vram_usage` (`:108`) and `persistent_buffer_size` (`:118`). Driven from **both** paths, exactly like `update_streaming`: render (`render_streaming_orchestrator.cpp:1612` begin / `:1587` end) and headless `tick_streaming_only` (`:2372` begin / `:2386`,`:2409` end). |
| `initialize_with_device`, `update_primary_asset_data`, `set_config_overrides`, `set_io_chunk_layout_hints`, `set_primary_chunk_layout` (`gaussian_streaming.h:201-208`) | `[SERIALIZED]` | **Public but *not* `ClassDB`-bound** — reachable only from C++, but the renderer drives them: `render_streaming_orchestrator.cpp:929` (`set_config_overrides`), `:938` (`initialize_with_device`), `:1285`/`:1311` (`set_primary_chunk_layout`), `:1348` (`set_io_chunk_layout_hints`). They mutate the same `chunks`/budget/`atlas_allocator`/`asset_registry` state as the bound surface: `update_primary_asset_data` (`gaussian_streaming.cpp:964-982`) calls `atlas_allocator.release_slot(...)` (`:968`), decrements `budget.loaded_chunks_count` (`:973`), subtracts from `vram_usage` (`:977`) and adds to `evicted_bytes_total` (`:978`). It is reached transitively via `set_primary_chunk_layout` → `:1097`, not called directly by the orchestrator. Note it is one of §1b's four release loops, so under S1 it becomes an `on_chunk_released` caller — and therefore also a §3c re-entrancy site. |
| `pack_thread_func` / `build_pending_upload_from_pack_job` (`streaming_upload_pipeline.cpp:427`) | `[SNAPSHOT-SAFE]` | Consumes a self-contained `PackJob` (`streaming_upload_pipeline.h:35-47`); the `&system` it is handed is nominal and must be removed by S2. |
| **Config / visibility mutators** — bound: `set_chunk_frustum_culling_enabled`, `set_chunk_frustum_padding` (`gaussian_streaming.cpp:517`, `:519`); unbound-but-public: `clear_config_overrides` (`gaussian_streaming.h:206`) and the inline LOD/SH setters `set_lod_blend_enabled`, `set_lod_blend_distance`, `set_global_sh_band_level` (`gaussian_streaming.h:281-290`) | `[SERIALIZED]` | These write `visibility.*` / config state (`visibility.lod_blend_config`, `visibility.global_sh_band_level`) that the streaming frame **reads** while running, so a script or C++ caller mutating them concurrently with `update_streaming` races the same substrate as the residency ledger. Cheap inline setters, but the token contract is about *when* they may run, not how expensive they are. |
| `get_vram_*`, `get_streaming_analytics`, `get_*_debug_stats`, `has_asset`, `get_visible_count` | `[READ-ONLY]` | Bound to script; must stay callable from the main thread. |

> **The table is a snapshot; S6 must not trust it as exhaustive.** Three successive reviews
> have each found further unclassified mutators — `begin_frame`/`end_frame`, the five
> `gaussian_streaming.h:201-208` mutators, and the config/visibility setters above — which is
> strong evidence that hand-maintaining this list does not converge. **S6 must therefore
> *derive* the classified set mechanically rather than transcribe it:** enumerate every
> `ClassDB::bind_method` target in `_bind_methods` plus every public non-`const` member
> function of `GaussianStreamingSystem`, and fail the build (or the guard) on any name that
> carries no `[SERIALIZED]` / `[SNAPSHOT-SAFE]` / `[READ-ONLY]` tag. Invariant **I6** is then
> checkable by construction, and this table becomes documentation of the derivation rather
> than its source of truth. A `const`-correctness pass is the cheap approximation: a public
> method that mutates must be non-`const`, so the enumeration is mostly mechanical.

**No entry point is `[RENDER-THREAD-ONLY]`.** If a future slice wants to introduce that
class, it must first prove the headless `tick_streaming_only` path and the script bindings
are gone — and that is a separate, breaking-change task, not part of this decomposition.

### 3c. What to assert instead — a single-active-caller token

Replace the rejected render-thread assert with a `DEV_ENABLED`-only **re-entrancy /
concurrency detector** that encodes serialization without constraining identity:

```
// DEV_ENABLED only; zero cost in release.
class SerializedAccessToken {          // member of ChunkResidencyLedger (§2a)
    std::atomic<uint64_t> active_caller{0};   // Thread::get_caller_id() of the in-flight call
    uint32_t depth = 0;                       // re-entrancy depth for the HOLDING thread only
    // enter(): CAS 0 -> caller_id. If the CAS succeeds, depth = 1.
    //          If the CAS fails and the holder is a DIFFERENT thread
    //                  -> ERR: "concurrent [SERIALIZED] access".
    //          If the CAS fails and the holder is the SAME thread
    //                  -> ++depth (legal nesting; see below). NOT an error.
    // exit():  if (--depth == 0) store 0.
};
```

**Same-thread nesting is legal and must not be reported.** An earlier revision of this
section made same-thread re-entry an error. That was wrong: composed with §3d's rule that
*every mutating ledger method scopes the token*, it would fire on the **normal** path, not
on a violation. One `update_streaming` call (`gaussian_streaming.cpp:2459`) reaches both
ledger folds through `_run_streaming_frame_pipeline` (`:2539`):

- **release side** — `_evict_for_vram_budget` (`:2586`) → `eviction_controller.evict_*` →
  `system._unload_chunk` (`streaming_eviction_controller.cpp:197,211,314`) → the budget
  decrements at `gaussian_streaming.cpp:3873-3877`, i.e. §2a's `on_chunk_released`.
- **load side** — `_load_visible_chunks` / `_process_upload_queue` (`:2600`,`:2614`) →
  `_complete_chunk_load_common` (`:3788`, `:3451`) → `budget.loaded_chunks_count++` /
  `budget.vram_usage +=` at `:3811-3812`, i.e. §2a's `on_chunk_loaded`.

So a DEV build would ERR on every eviction and every chunk load in a perfectly serialized
frame. The depth counter above keeps the property that actually matters — **at most one
*thread* inside the ledger at a time** — while permitting the entry-point → ledger-method
nesting the design requires. (Equivalent alternative, if the owner prefers no counter:
ledger methods `DEV_ASSERT` the token is *already held by the current thread* rather than
acquiring it, making the entry-point scope the sole acquirer. That is stricter — it also
catches a ledger mutation reached without going through a `[SERIALIZED]` entry point — but
it forbids direct ledger use from tests, so it is recorded as an option, not the default.)

- Every `[SERIALIZED]` entry point takes an RAII `SerializedAccessScope` at the top.
- It catches the failure that actually threatens correctness (two threads mutating
  `budget`/`chunks` at once, or re-entrancy through a callback) and is **agnostic** to
  which thread is the caller — so the headless main-thread path, the render-thread path,
  the script path, and the doctests all pass unchanged.
- It is a **detector, not a lock**: it never blocks and never serializes. It adds no lock over
  render-facing state, per the module `AGENTS.md` rule, and leaves the existing two streaming
  locks (§3e) untouched.
- Once §2a lands, the token lives on `ChunkResidencyLedger` and every mutating ledger method
  scopes it — turning "single writer by convention" into "single writer, checked."

The pack-worker boundary is **unchanged** by this ADR: it is already correct by snapshot,
and §3d records why.

### 3d. The pack-worker boundary (unchanged, documented)
- **The pack workers are the one true concurrency boundary, and it is already correct by
  snapshot**: `pack_thread_func` (`streaming_upload_pipeline.cpp:427`) dequeues a self-contained
  `PackJob` (carries `Ref<GaussianData> data_ref` + copied `source_indices`,
  `streaming_upload_pipeline.h:35-47`) under `pack_mutex`, calls
  `build_pending_upload_from_pack_job(job, scratch)` — which takes **only the job snapshot, no
  `system`** — and enqueues the result under `pack_mutex`. The worker never mutates `chunks`,
  `budget`, `atlas_allocator`, or `persistent_buffer`. The `&system` it is handed
  (`pack_thread_func(system, …)`) is *nominal* — the snapshot boundary is what keeps it safe.
- **All shared-state mutation (slot release, budget deltas, buffer_update) happens on the
  serialized caller's thread** inside `process_upload_queue` / `_begin_chunk_upload` /
  eviction — i.e. inside the `update_streaming` call, on whichever thread made it (render
  thread on the `render_streaming_frame` path, main thread on the headless
  `tick_streaming_only` path). No pack worker ever performs these mutations.

**Decision — document the per-API contract, detect violations, add no lock:**

1. Document in `gaussian_streaming.h` (per method) and MEMORY_SUBSYSTEM.md: *"`GaussianStreamingSystem`
   render-facing state (`chunks`, `budget`/ledger, `atlas_allocator`, `persistent_buffer`,
   `asset_registry`) has exactly one active caller at a time. That caller is the render thread
   on the `render_streaming_frame` path and the **main thread** on the headless
   `tick_streaming_only` path; both are supported. The only cross-thread channel is the
   `pack_mutex`-guarded pack/upload queue, which carries value snapshots only."*
   Every public method carries its `[SERIALIZED]` / `[SNAPSHOT-SAFE]` / `[READ-ONLY]` tag
   from the §3b table in its header doc comment.
2. Add the `DEV_ENABLED` `SerializedAccessScope` (§3c) to every `[SERIALIZED]` entry point.
   It is a **detector, not a lock, and not a thread-identity assert** — it must not reject
   the headless main-thread path, the script-bound path, or the doctests.
3. When residency mutation moves into `ChunkResidencyLedger` (§2a), the ledger owns the token:
   every mutating ledger method scopes it, turning "single caller by convention" into
   "single writer, checked." Because a `[SERIALIZED]` entry point reaches those ledger
   methods **within the same call** (§3c), the scope must be re-entrant by depth for the
   holding thread; a flat "same thread ⇒ error" token would fail on the normal path.

**D-READONLY (open — owner decision).** §3b classifies diagnostics getters as `[READ-ONLY]`
and documents them as advisory: a caller racing a `[SERIALIZED]` call may read a stale or
torn value. For the **scalar** counters that is a deliberate, defensible trade — the cost of
synchronizing them is real and the consumers are HUDs and tests. It is recorded here so it
is an accepted trade rather than an omission.

`analytics_snapshot` is **not** in that category, and this is the part that needs a decision
rather than a disclosure. It is a `Dictionary`, so a racing read is a refcount race on a
non-atomic pointer (`core/variant/dictionary.cpp:43-44`), i.e. undefined behavior, not a
torn view. The three dispositions:

| Option | What it means | Cost |
| --- | --- | --- |
| **A — publish under a swap** | `end_frame` builds the new `Dictionary` locally and publishes it through an atomically-swapped shared pointer; readers take a ref off the published pointer. | One indirection + a small allocation per frame; no new lock over render-facing state, so §3e is unaffected. |
| **B — reclassify** | `get_streaming_analytics` becomes `[SERIALIZED]`-adjacent: documented as "must not be called concurrently with `end_frame`", enforced by the §3c token. | Free, but it makes a **bound script getter** unsafe-by-contract, which is close to the public-API break §3a rejected. |
| **C — accept** | Document the race and move on. | Free, and wrong: it is UB, and the current wording ("torn view") understates it. Listed for completeness only. |

This ADR does **not** pick one. Whichever is chosen must become an invariant so a slice can
be graded on it; A is the only option that keeps the bound getter safe from script without a
contract break.

No lock is added over the serialized state (the module `AGENTS.md` forbids inventing a
second lock over the same data). The streaming subsystem's existing lock inventory is
unchanged — see §3e, which corrects the "sole lock" claim an earlier revision made.

### 3e. The streaming lock inventory — there are **two** locks, not one

> **Correction (review round 2).** Both an earlier revision of this ADR and its round-1
> invariant **I9** asserted *"`pack_mutex` remains the only lock in the streaming subsystem."*
> **That was false on the day it was written**, which is worse than having no invariant at all:
> a grep guard written to enforce "exactly one lock" would have failed immediately, and the
> likely reaction would have been to weaken the guard rather than to fix the statement.

The streaming subsystem declares exactly two locks (plus one semaphore), verified by grep over
`core/gaussian_streaming.*` and `core/streaming_*`:

| Lock | Declared at | Guards | Cross-thread role |
| --- | --- | --- | --- |
| `pack_mutex` | `core/streaming_upload_pipeline.h:261` | the pack-job and pending-upload queues | the pack-worker ↔ serialized-caller channel (§3d) |
| `file_mutex` | `core/streaming_chunk_payload_source.h:97` (`mutable`) | the per-thread `FileAccess` cache and the I/O byte counters in `ChunkPayloadSource` | serializes file I/O for payload reads; guards **no** render-facing state |

They are disjoint by design: `file_mutex` protects an I/O-side cache inside the payload source
and never touches `chunks`, `budget`, `atlas_allocator`, or `persistent_buffer`. `pack_semaphore`
(`streaming_upload_pipeline.h:262`) is a worker wake signal, not a mutex.

The invariant worth holding is therefore the one now stated as **I9**: *no **new** lock over
render-facing state, and no streaming path acquires the director's `world_mutex`.* The second
clause is currently true — `world_mutex` appears **only** in
`core/gaussian_splat_scene_director.cpp` and in no streaming TU or the streaming orchestrator —
and it is the clause that actually protects against the lock-order inversion the sibling ADR
(`adr-decompose-scene-director`) is fighting.

## 4. Staged migration (CI green at every step)

Each slice is an independently reviewable PR; none weakens a guard. Slices 1–3 are pure
refactors provable byte-identical; 4–6 shift ownership behind the same external behavior.

1. **S1 — extract `ChunkResidencyLedger` behind the current struct (no behavior change).**
   Wrap `BudgetState` in the ledger, route the five free functions (§1a) through it. Replace the
   four duplicated loops (§1b) with `on_chunk_released`, unifying the `:3873` guard divergence
   (a style unification — **not** a bug fix; see the §1b correction).
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
6. **S6 — tag every public entry point with its §3b class, add the `SerializedAccessScope`
   detector (§3c)**, and rewrite the stale MEMORY_SUBSYSTEM.md layout/component sections
   (#563) to match the new ownership map. Update `renderer-lifetime-ownership.md` cross-refs.
   S6 **no longer carries #591** — the cross-route helper is rejected in §2d.
   *Evidence:* the full module test suite passes **unchanged** with `DEV_ENABLED` on (proving
   the detector does not reject the doctest main-thread callers), plus a headless
   `tick_streaming_only` run (proving the main-thread production path is not rejected).
   A slice that changes any test to accommodate the detector is rejected.

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
→ S6 (tags + detector + #563 doc rewrite). #591 is **not** in this chain (§2d).

## Consequences

- **Positive:** one writer for resident-bytes accounting (kills the four duplicated loops and
  unifies the `:3873` guard divergence), friendship deleted (S5), the per-API thread contract
  documented and detected rather than assumed, and #222/#513-budget/#563 close as by-products
  on the same spine. (#591 is explicitly *not* one of them — §2d.)
- **Negative / cost:** six sequenced PRs gated behind two pre-req bug fixes; S4–S5 touch hot
  render-thread paths (R2–R3) and need GPU/runtime evidence per slice. The ledger adds one
  indirection on the load/evict path (negligible; these are not per-splat).
- **Explicitly out of scope here:** merging the resident and streaming orchestrator *policies*
  (#591 keeps distinct sizing), the #543 chunked-staging/evict-then-grow work (orthogonal peak
  memory), and any GPU atlas layout change.

## 6. Invariant list — what every slice is graded against

These are checkable. A slice that violates one is rejected regardless of whether CI is green;
a slice that cannot show the evidence in §7 for the invariants it touches is "not run", never
"passed".

| # | Invariant | How it is checked |
| --- | --- | --- |
| **I1** | `vram_usage` == Σ bytes of chunks currently marked resident, and `loaded_chunks_count` == count of `is_loaded` chunks, at every `update_streaming` exit. | `get_vram_debug_stats` parity assertion in the streaming tests; must hold before and after each slice. |
| **I2** | After S1, `ChunkResidencyLedger` is the **only** code that writes `loaded_chunks_count`, `vram_usage`, `evicted_bytes_total`, or any `pending_*`/`retired_*` counter. | Grep guard: zero writes to those fields outside the ledger TU. Mechanically checkable; add it as a CI guard in S1. |
| **I3** | There is **exactly one** `loaded_chunks_count` decrement site (inside the ledger) and it clamps rather than wraps. **Note:** this is a *structural* invariant, not a bug fix — see the §1b correction: today's four sites are behaviorally identical because every one of them is reached only when the chunk is `is_loaded`, so under I1 the counter is provably ≥ 1. I3 is graded as "one site exists", not as "an underflow was fixed." | Follows from I2 + a unit test that over-releasing clamps and reports, rather than wrapping. |
| **I4** | `vram_usage` is decremented with the **stride the chunk was loaded at**, never a recomputed current stride. | Unit test: load at stride A, flip effective stride to B, release, assert `vram_usage` returns to its pre-load value (#513's budget half). |
| **I5** | A slot release and its byte decrement are one operation — no code path releases an atlas slot without the paired ledger call, or vice versa. | `invariant_slot_ownership_violations` (`streaming_runtime_state.h:118`) stays 0 across all streaming tests. |
| **I6** | Every public entry point carries exactly one §3b class tag in its header doc, and the set of `[SERIALIZED]` methods equals the set that mutates render-facing state. | Review checklist + a doc/code parity guard in S6 (tag present for every public method). |
| **I7** | **No `[SERIALIZED]` entry point asserts thread identity.** The headless `tick_streaming_only` main-thread path, the `ClassDB`-bound script path, and the doctest callers all remain valid. | Module test suite passes unchanged with `DEV_ENABLED`; headless run of the `tick_streaming_only` path produces no new errors. This invariant exists specifically to prevent re-introducing the rejected blanket assert. |
| **I8** | No pack worker reads owner state. `build_pending_upload_from_pack_job` and every worker entry take only value snapshots; after S2 no worker signature takes `GaussianStreamingSystem &`. | Signature review + grep: zero `&system` parameters on worker-thread functions. |
| **I9** | **No slice adds a NEW lock over render-facing state** (`chunks`, budget/ledger, `atlas_allocator`, `persistent_buffer`, `asset_registry`), and **no streaming path acquires the director's `world_mutex`**. The two existing streaming locks stay at two, each keeping its current scope. | Grep guard: `Mutex`/`RWLock` declarations in the streaming TUs stay at exactly the two named below, and `world_mutex` has zero references outside `core/gaussian_splat_scene_director.cpp`. |
| **I10** | Friendship strictly decreases. No slice adds a `friend` grant; S5 removes the six named grants (`gaussian_streaming.h:31-34`, `streaming_visibility_controller.h:54`, `streaming_global_atlas_registry.h:23`). | Grep guard on `friend class` count in the streaming headers; monotonically non-increasing, zero after S5. |
| **I11** | No GPU payload layout changes in S1–S6; the atlas stride guard and the layout-sync guard are untouched. | `run_module_tests.py --guard-only` green; guard files unmodified in the diff. |
| **I12** | External behavior is preserved: same chunks resident, same eviction order, same visible count for a fixed camera path. | Byte-identical `get_streaming_analytics` / `get_vram_debug_stats` on a fixed scene before and after. |

## 7. Evidence a slice must produce

Every slice states which invariants it touches and attaches, at minimum:

1. **Guard lane:** `run_module_tests.py --guard-only` green, plus any new guard the slice adds
   (I2, I6, I9, I10 are guard-shaped and should land *with* the slice that makes them true).
2. **Targeted tests:** the streaming suites (`test_gpu_streaming.cpp`,
   `test_gaussian_streaming_lifecycle.cpp`) green **without modification**. Modifying an
   existing test to accommodate a slice is a review blocker unless the diff shows the old
   assertion encoded a bug being fixed, with a written reason.
3. **Fixture non-vacuity (mandatory — see §1e):** any scene-level or characterization fixture a
   slice adds must be built on **`GaussianSplatWorld3D` with `route_policy` pinned to
   `GS_ROUTE_STREAMING`**. A `GaussianSplatNode3D` fixture is hard-pinned resident
   (`gaussian_splat_node_3d.cpp:2451-2464`, `:2508-2509`) and therefore exercises **no**
   streaming code — such a fixture is vacuous and its green result is not evidence. Every
   streaming fixture must additionally assert a **non-zero** streaming counter (e.g.
   `get_vram_debug_stats().loaded_chunks_count > 0`) before its behavioral assertions, so a
   fixture that silently fell back to the resident route fails instead of passing empty.
4. **Accounting parity (I1, I12):** `get_vram_debug_stats` + `get_streaming_analytics` captured
   on a fixed camera path on the immutable base and on the head, diffed and attached.
5. **Runtime/GPU evidence (R2 slices — S1, S2, S4, S5):** the streaming lanes of the runtime
   harness on the GPU runner, with peak VRAM and overflow counters. Agents cannot raster
   locally; a slice without runner output reports "not run", never "passed".
6. **Threading evidence (S6 only):** module tests with `DEV_ENABLED` on, plus a headless
   `tick_streaming_only` run, demonstrating I7 — the detector fires on neither.
7. **Base anchoring:** the base SHA, and confirmation that the `file:line` anchors used were
   re-verified against it (they drift; see the anchoring note in the header).

No slice may weaken a guard, threshold, or baseline to pass. If a slice cannot hold an
invariant, it is split or the invariant is renegotiated in a follow-up to this ADR — not
silently dropped.
