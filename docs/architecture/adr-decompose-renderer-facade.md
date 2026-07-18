# ADR: Decompose the `GaussianSplatRenderer` facade into owned sub-contexts

- **Status:** Proposed (design-note-before-implementation). Owner sign-off required
  before any code slice lands. This ADR is characterization + target + staged plan
  only; it changes no production code and builds nothing.
- **Risk class:** **R0 for this document** (`docs/**` — `.agentic/policy.json`
  `path_globs: ["docs/**"]` → R0). The change it *designs* is **R2**
  (`modules/gaussian_splatting/renderer/**`), **escalating to R3** for the one step
  that edits `.github/workflows/gaussian_shader_validation.yml` and for any step that
  touches a serialized/format contract. Every implementation slice carries the risk
  class of the files it edits and needs the matching runtime/GPU evidence and
  independent review (`docs/governance/review-policy.md`).
- **Tracked by:** #356 (decompose renderer orchestration around owned state
  boundaries). **Closes / advances:** #528, #529, #570, #591, #587 (facade/state
  duplication + fail-closed) and #525 (embedded sorter GLSL outside the C5 compile
  matrix). **Related but distinct:** #588 (destructor dispatch can block — a separate
  lifetime bug, not addressed here; see "Issue-closure mapping").
- **Base:** originally anchored at `origin/master`
  `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce`. **Re-anchored in review round 2 to
  `9161d92f349`** (~30 commits later); every `file:line` below was re-verified against that
  master by `grep -n`, and the drifted ones are corrected in place. The re-anchoring is
  material, not cosmetic: PR #627 landed in that window and closed the metric-reset half of
  #528 (§1.4). Anchors that moved are listed in
  §"Anchor re-verification log" so a reviewer can tell correction from silent edit.
  Builds on the W0 characterization in
  [`stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md) (issue
  #356) — this ADR is the W1/W2 target and migration plan that inventory called for.

---

## 1. Current state — the shared-state bundle map

### 1.1 One *aliased* bundle — the buckets are orchestrator-owned and facade-forwarded

> **Correction (review round 1). The previous revision of this section asserted an
> ownership map that is the inverse of the code.** It claimed `GaussianSplatRenderer`
> "holds every per-renderer bucket as a direct member/alias … at
> `gaussian_splat_renderer.h:201-334`" and that "the twelve orchestrators do **not** own
> any of these." Both claims are false, and `h:201-334` contains almost no member
> declarations at all — it is a block of `using` type aliases and nested `struct`
> *definitions*. The only actual member in that range is
> `RenderFrameContextManager frame_context_manager;` (`h:306`). Because every downstream
> slice inherits the ownership map, the corrected map below is normative and the migration
> plan in §4 is re-derived from it.

**Verified ownership map** (`GaussianSplatRenderer`, re-verified on master `9161d92f349` —
every row below still holds, each owning declaration confirmed a **by-value** member):

| Bucket | Actual owner | Owning declaration | Facade forwarder |
| --- | --- | --- | --- |
| `SceneState` | `RenderDataOrchestrator` | `render_data_orchestrator.h:66` | `gaussian_splat_renderer.cpp:2634-2644` |
| `StreamingState` | `RenderDataOrchestrator` | `render_data_orchestrator.h:67` | `:2742-2752` |
| `SortingState` | `RenderSortingOrchestrator` | `render_sorting_orchestrator.h:80` | `:2730-2740` |
| `PipelineState` | `RenderResourceOrchestrator` | `render_resource_orchestrator.h:45` | `:2670-2680` |
| `ResourceState` | `RenderResourceOrchestrator` | `render_resource_orchestrator.h:46` | `:2754-2764` |
| `RenderConfig` | `RenderConfigOrchestrator` | `render_config_orchestrator.h:58` | `:2646-2656` |
| `DeviceState` | `RenderDeviceOrchestrator` | `render_device_orchestrator.h` (`device_state`) | `:2658-2668` |
| `DebugState` | `RenderDebugStateOrchestrator` | via `get_state()` | `:3061-3068` |
| `PerformanceSettings` | `RenderQualityOrchestrator` | via `get_performance_settings()` | `:2682-2692` |
| `FrameState`, `ViewState` | `RenderFrameContextManager` | `gaussian_splat_renderer.h:306` | `frame_context_manager` |
| `PerformanceState` | **facade, direct member** | `gaussian_splat_renderer.h:604` | — |
| `TestDataState` | **facade, direct member** | `h:605` | — |
| `TileRendererState` | **facade, direct member** | `h:610` | — |
| `SubsystemState` | **facade, direct member** | `h:619` | — |
| `ShadowBlitState` | **facade, direct member** | `h:765` | — |

So: **nine of the fourteen buckets are already owned by an orchestrator or by
`frame_context_manager`, by value.** Only five are direct facade members, and they sit at
`h:604-765`, not `h:201-334`. `RenderConfigOrchestrator` in particular is *already* the
clean pattern the ADR was proposing to build — it owns `render_config` and is the **only**
orchestrator header that declares **no** `GaussianSplatRenderer *renderer` back-pointer.

Three further count corrections, all re-verified by grep on master `9161d92f349`:

- There are **11** `render_*_orchestrator.h` headers, not twelve. **10** declare the raw
  back-pointer `GaussianSplatRenderer *renderer` — `render_config_orchestrator.h` does not.
  The claim "verified 12/12" was wrong in both numerator and denominator.
- `FrameStateProvider` is constructed **78** times, not 61 — and **8 of those are outside
  `renderer/`** (`interfaces/painterly_renderer.cpp` ×7, `interfaces/debug_overlay_system.cpp` ×1),
  which the previous scope map missed entirely. Highest concentrations:
  `render_pipeline_stages.cpp` (16), `render_diagnostics_orchestrator.cpp` (14),
  `render_output_orchestrator.cpp` (7).
- There are **40** `static … fallback;` locals in `gaussian_splat_renderer.cpp`, not 18.
  **18** are in the `FrameStateProvider` method bodies (`:678-838`) and **22** are in the
  facade's own bucket accessors (`:2635-2761`). §1.2 previously addressed only the first 18.
  *(Corrected in round 2: the 22 do **not** extend to `:3061-3068`. `get_debug_state`
  (`:3061-3068`) is the one forwarder pair in the block that uses **no** `static … fallback;`,
  so the ADR's uniform "every forwarder has its own static fallback" description is wrong for
  that pair. S8's grep guard must still reach zero across the file.)*

**The actual defect, restated correctly.** The problem is *not* that the facade owns
everything and the orchestrators are namespaces. It is that ownership is real but
**unenforced and doubly aliased**:

1. Each orchestrator owns its bucket by value, but hands out **mutable references** to it
   (`access_scene_state_mutable()`, `render_data_orchestrator.h:56`;
   `access_sorting_state_mutable()`, `render_sorting_orchestrator.h:55`), so the owner
   cannot assert it is the only writer.
2. The facade **re-exports** those references through 22 forwarding accessors, each with its
   own `static` fallback, so every consumer reaches an orchestrator's private state through
   the facade without the orchestrator knowing.
3. `FrameStateProvider` then re-exports them a **third** time as `IFrameStateView` +
   `IFrameMutationAccess` (`gaussian_splat_renderer.h:476-511`), and `FrameDeps` caches raw
   pointers to the same objects (`h:386-435`, field list `:387-405`) as a **fourth** alias.

So a single `SortingState` is reachable as: orchestrator member → facade accessor →
provider getter → `deps.sorting_state` pointer. Four aliases, two of them with `static`
fallbacks, none of them a partition. **The decomposition's job is to collapse the alias
chain and make the existing ownership enforceable — not to invent ownership that is
already there.**

`FrameStateProvider` (`gaussian_splat_renderer.h:476-511`, impl
`gaussian_splat_renderer.cpp:623-800+`) implements **both** `IFrameStateView` (read)
and `IFrameMutationAccess` (write) and every accessor resolves to the *same* renderer
bucket:

```cpp
const StreamingState &FrameStateProvider::get_streaming_state() const {
    static StreamingState fallback;                       // gaussian_splat_renderer.cpp:686-693
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->streaming_state) return *deps->streaming_state;
    return renderer_view->get_streaming_state();          // → the one shared member
}
```

This is exactly the audit finding: **the orchestrators are namespaces over the
facade's state, not ownership boundaries.** A `FrameStateProvider` handed to any stage
exposes 12 read buckets + 8 mutable buckets (`IFrameMutationAccess`,
`gaussian_splat_renderer.h:462-474`) of the single renderer instance, so any stage can
mutate any bucket. There is no compile-time partition of who-writes-what.

### 1.2 Hazard A — the `static` fallback objects are hidden process-global mutable state

There are **40** `static … fallback;` function-local statics in
`gaussian_splat_renderer.cpp`, at **two** levels of the alias chain (§1.1):

- **18 in the `FrameStateProvider` block** (before `:2600`) — one per bucket getter, e.g.
  `static SortingState fallback;`. The mutable variants return a mutable reference to this
  shared object (`get_sorting_state_mut()`, `:767-775`).
- **22 in the facade's own forwarding accessors** (`:2635-2761`; note `get_debug_state` at `:3061-3068` has none) — e.g.
  `GaussianSplatRenderer::get_scene_state()` at `:2634-2638`:
  ```cpp
  static SceneState fallback;
  ERR_FAIL_NULL_V(data_orchestrator, fallback);
  return data_orchestrator->access_scene_state_mutable();
  ```

Both sets are **function-local statics shared across all renderer instances and threads**.
A null-orchestrator or null-renderer mutation path silently writes a process-global that
every renderer in the process observes. The inventory already flags the provider half
(`stage-first-ownership-inventory.md:168`, "FrameStateProvider fallback statics and broad
mutable accessors … split into read-only snapshots plus small mutation sinks"); the facade
half is the larger set and was previously unaccounted for. **Any slice that removes only
the provider's 18 leaves the majority of the hazard in place** — see S8's corrected scope.

### 1.3 Hazard B — `frame_plan` borrow is a designed use-after-scope trap (#529)

`RenderFramePlan` is built as a **stack local** and its address is stored into
`frame_context.deps.frame_plan` at three sites (line anchors corrected):

- `gaussian_splat_renderer.cpp:2595` (resident/main route)
- `render_instancing_orchestrator.cpp:180` (instanced route)
- `render_pipeline_stages.cpp:1173` (stage-runner build path; previously cited as `:1162`)

`RenderFrameContext::FrameDeps::validate()` validates 14 pointers but **deliberately
exempts `frame_plan`** (`gaussian_splat_renderer.h:428-431`), and the instancing site
carries an in-code confession:

```cpp
// render_instancing_orchestrator.cpp:176-179
// Borrow invariant: `frame_plan` is a stack local ... Do not store, defer, or async-pass
// ... RenderFrameDeps::validate() exempts `frame_plan` ... so a use-after-scope here will
// not be caught by the validator.
```

The codebase increasingly threads `RenderFrameContext` across stage boundaries; any
future deferral/async of a stage turns this borrow into silent UB that no guard
catches. It is duplicated at three sites, so the trap is not localized.

**And `RenderFrameContext` is already copied by value** — `render_pipeline_stages.cpp:1134`:

```cpp
// Copy frame context first, then build frame_plan and update deps.
// The provider must be constructed AFTER this so it sees the updated deps.
RenderFrameContext frame_context = p_frame_context;
```

The codebase has already been bitten by this exact class of bug and works around it
in-place (`render_pipeline_stages.cpp:1175-1177`; rebind at `:1178-1182`):

```cpp
// This path copies RenderFrameContext before attaching frame_plan, so any incoming
// provider-backed seams still point at the caller's deps object. Rebind both seams
// to a local provider over the copied deps to avoid stale frame_plan/state pointers.
```

`RenderFrameContext` therefore already contains **self-referential pointers that do not
survive a copy** — `state_view` and `mutation_access` (`h:383-384`) point at stack locals,
and the copy at `:1134` is only safe because `:1178-1182` explicitly rebinds them. This is
the precise reason §2.1's original proposal is unsafe, and it is why the corrected design
below makes the context **non-copyable** rather than adding another self-reference to it.

### 1.4 Hazard C — hand-maintained parallel lists with no parity guard (#528, #570, #591)

> **Superseded in part — review round 2. The metric-reset half of this hazard is FIXED on
> master.** PR **#627** (merged 2026-07-18, closing **#528**) landed after this ADR's base and
> is not reflected in the text below. It also **refuted this ADR's proposed mechanism**. See
> the re-scoping note at the end of this subsection and the corrected **S1** row in §4 before
> planning any work here.

The single shared `PerformanceMetrics` bundle was zeroed **field-by-field** at three
independent sites, none generated from the struct:

| Site | Function | Scope |
| --- | --- | --- |
| `render_pipeline_stages.cpp` | `reset_render_state_for_frame` | frame-skip path: raster tile snapshot + **full** GPU pass timings + timeline |
| `render_pipeline_stages.cpp` | `render_sorted_splats_with_context` | main path: `raster_path="unknown"` + raster tile snapshot + **core** GPU pass timings only |
| `render_resource_orchestrator.cpp` | `update_gpu_pass_metrics_from_tile_renderer` (no-rasterizer branch) | **all** GPU timing groups + readback state; deliberately leaves the monotonic `raster_pipeline_reformats` |

A new `PerformanceMetrics` field added to the struct but missed at any one site
silently reports **the previous route's value** — a stale-telemetry bug class (#528).
The same hand-list pattern (no parity guard) recurs in the ~100-key diagnostics
Dictionaries built field-by-field in `render_diagnostics_orchestrator.cpp`
(`_append_production_frame_metrics`) and the route-UID→label maps in
`render_route_labels.cpp`.

**What #627 landed, and what it refuted.** Verified on master `9161d92f349`:

- **Landed:** five named group helpers on `PerformanceMetrics` —
  `reset_raster_frame_stats()`, `reset_gpu_core_pass_timings()`,
  `reset_gpu_extended_pass_timings()`, `reset_gpu_timeline_metrics()`,
  `reset_gpu_readback_state()` (`renderer/render_types/render_performance_types.h:165-174`),
  each site composing the groups it needs. Plus a fail-closed field-coverage parity guard,
  `tests/ci/check_metric_reset_parity.py`, wired into the `--guard-only` lane
  (`tests/ci/run_module_tests.py:32-33`, `:741`, `:1605`), which requires every struct field
  to be either reset-covered or in an explicit `NOT_RESET_FIELDS` allow-list with a reason
  (93 fields accounted for: 53 covered, 40 explicitly not-per-frame-reset).
- **Refuted:** this ADR's §2.3 proposal to *"give `PerformanceMetrics` a `reset()` (or
  `*this = PerformanceMetrics{};` default-init semantics) and call it at all three sites."*
  #627 checked the three sites field-by-field and found they reset **intentionally different
  subsets** — they are partial resets, not three copies of one full reset. A blanket reset
  would zero cumulative/lifetime counters (`total_frames_rendered`, the deliberately-monotonic
  `raster_pipeline_reformats`), rolling aggregates, and per-stage outputs the sites preserve
  on purpose. **That is a behavior change, not a dedup**, and the ADR's characterization of
  the three sites as interchangeable ~30–40-field lists was the reason it looked safe.
- **NOT landed:** the **diagnostics-dict key-set guard** and the **route-label key-set
  guard**. #528's own body names both as the same class ("no parity guards either"), and
  #627 touched neither `render_diagnostics_orchestrator.cpp` nor `render_route_labels.cpp`;
  no file under `tests/ci/` references either. **This ADR makes both an exit criterion in
  R9**, so R9 is currently unmet by the merged work and S1 is only *partially* discharged.

**Stage-exit stamping duplication (#570):** four near-identical ~40-line
`StageResult`/`StageIO` skip/fail stamping blocks exist in
`render_sorted_splats_with_context` alone (`render_pipeline_stages.cpp:3022, 3027, 3053,
3058` — corrected in round 2 from `3005/3026/3083/3114`), and again in `render_instancing_orchestrator.cpp:196-225`. Each repeats
`make_downstream_skip_result(...)` + `stamp_stage_result_contract(...)` + `_init_stage_io(...)`.
The audit measures ~30–40% of frame-path LOC as this ritual.

**Duplicate contract-publisher sizing (#591):** the resident and streaming routes
hand-roll the *same* `sort_cap` clamp and `InstancePipelineBuffers` population with no
shared helper:

```cpp
// resident_instance_contract_publisher.cpp:808-811
const uint64_t sort_cap = g_gpu_sorting_config.max_sort_elements > 0
        ? uint64_t(g_gpu_sorting_config.max_sort_elements) : uint64_t(UINT32_MAX);
buffers.max_visible_splats = uint32_t(MIN<uint64_t>(max_visible_splats_u64, sort_cap));
```
```cpp
// render_streaming_orchestrator.cpp:1781-1784  (identical clamp, different sizing input)
const uint64_t sort_cap = g_gpu_sorting_config.max_sort_elements > 0
        ? uint64_t(g_gpu_sorting_config.max_sort_elements) : uint64_t(UINT32_MAX);
max_visible_splats_u64 = MIN(max_visible_splats_u64, sort_cap);
```

The *sizing policy* legitimately differs (resident: atlas gaussian count + structural
per-asset chunk max; streaming: regulator working-set), but the **clamp + buffer-field
population is copy-pasted**, so a future `InstancePipelineBuffers` layout change can
land in one route only.

### 1.5 Hazard D — `StageIO.validation_failed` is diagnostic-only (#587)

`_finalize_stage_io` (`render_pipeline_stages.cpp:779-799`) sets
`p_io.validation_failed` when a stage produced impossible counts
(`output_count > input_count`) or a missing output buffer, and records an event. But
the **only consumers** of that flag are the debug overlay, a debug-dict serialization
(`render_debug_state_orchestrator.cpp:168`), and the logger. At the cull site the
frame proceeds regardless:

```cpp
// render_pipeline_stages.cpp:1460-1469 (the _finalize_stage_io call is at :1467)
const bool count_invalid = io.output_count > io.input_count;
const bool buffer_missing = cull_state.gpu_visible_indices_count > 0 && !io.output_buffer.is_valid();
... _finalize_stage_io(renderer, "cull", io, validation);
return result;   // <-- poisoned StageIO does not fail the stage or the frame
```

A structurally-invalid stage output cannot fail the stage — the StageIO contract is
advisory, contradicting the "no silent unsafe fallback" rule in
`renderer/AGENTS.md`.

### 1.6 The `gpu_sorter.cpp` sub-cluster (~3,500 LOC): three algorithms + embedded GLSL

`gpu_sorter.cpp` is one TU holding three sort algorithms plus a factory, **and** the
full GLSL source for each, embedded as runtime `vformat(R"(#version 450 …)")` strings:

| Unit | Approx lines | Embedded GLSL (`R"(#version 450`) |
| --- | --- | --- |
| `BitonicSort` | `539-946` | `596` (compare/swap kernel) |
| `GPUSorterFactory` / policy | `948-1178` | — |
| `RadixSort` (+ variants) | `1180-2013+` | `1656` histogram, `1741` wg-prefix, `1809` bin-prefix, `1854` scatter, `2204` indirect-dispatch |
| OneSweep variant kernels | `2731-3006+` (ctor `2731`, `_bind_methods` `2739`) | `2780, 2847, 2943, 3006` |

These strings are compiled at runtime via `create_compute_shader_from_spirv`
(`gpu_sorter.cpp:138`). The C5 shader-permutation CI
(`.github/workflows/gaussian_shader_validation.yml` → `shaders/compile_shaders.py`)
enumerates an explicit **on-disk** `RUNTIME_SHADER_MATRIX` of `SHADERS_DIR/*.glsl` +
`COMPUTE_DIR/*.glsl` files (`compile_shaders.py:142-684`) and never references
`gpu_sorter`. **Result (#525): the sorter's radix/bitonic/onesweep GLSL is invisible
to the compile matrix** — a syntax or `layout()` break in a sorter kernel ships and
only fails at runtime on the machine that selects that algorithm. (By contrast the
tile pipeline's shaders are already external files in the matrix — see §1.7 — so the
tile cluster does **not** share this defect.)

### 1.7 The `tile_renderer.cpp` sub-cluster (~3,579 LOC): stages already split, orchestration not

Unlike gpu_sorter, `tile_renderer.cpp` embeds **no** GLSL: it pulls shader source from
generated headers (`tile_renderer.cpp:31-34`, `#include "../shaders/tile_binning.glsl.gen.h"`
etc.), and those `.glsl` files are already in the C5 matrix — so the tile cluster has
no #525 exposure. Its per-stage GPU work is **already delegated** to sibling TUs:
`tile_render_binning.cpp`, `tile_render_prefix_scan.cpp`,
`tile_render_rasterizer_stage.cpp`, `tile_render_resolve.cpp`, `tile_render_stages.cpp`
(params), `tile_render_resources.cpp` (`TileResourceController` +
`TileRenderTargets`/buffer lifecycles), and `tile_render_debug_stats.cpp`. The tile
renderer keeps ~30 member-state clusters (`tile_renderer.h:391-481`), including the two
the inventory flags as ex-globals now member-owned: `subgroup_support_cache`
(`tile_renderer.h:450`) and `adaptive_overlap_budget_runtime_state`
(`tile_renderer.h:463-464`). What is **still monolithic** inside `tile_renderer.cpp`:

- `RenderFrameExecutor` — the per-frame pipeline state machine, `tile_renderer.cpp:271-1352` (~1,080 LOC, the single largest unit; owns validate→params→global-sort→raster→resolve→finalize).
- `initialize`/`cleanup`/`_ensure_resources` lifetime (`1474-1727`, `1783-1878`).
- Shader-defines assembly + compilation orchestration + `_detect_subgroup_support` (`2023-2385`).
- GPU timestamp/timing subsystem (`2426-2757`).
- `_evaluate_raster_path` compute-vs-fragment decision (`2778-2853`).
- Device/descriptor + instance-pipeline-binding cache (`2971-3225`); statistics/density aggregation (`3273-3458`).
- The adaptive-overlap-budget free-function subsystem in the anonymous namespace (the anon namespace spans `47-269`), which operates on the public runtime-state struct but is not part of `TileAdaptiveController`.

Tile decomposition is therefore a *continuation* of an already-started split (extract
`RenderFrameExecutor`, the timing subsystem, and shader-compile orchestration into
owned services), not a from-scratch teardown. It is **lower priority** than the facade
and sorter work and is sequenced last (see §4, optional S10).

---

## 2. Target — partition the bundle into owned sub-contexts

**Principle:** replace *one mutable bundle + a broad view/mutation provider* with
**(a) a per-frame immutable plan produced once, (b) resource owners that outlive the
frame, and (c) narrow result sinks a stage may write.** A stage receives only the
capabilities it needs, by type — not the whole renderer.

### 2.1 Unit A — `FramePlan` gets a named owner outside the context (fixes 1.3 / #529)

> **Corrected in review round 1.** The previous revision proposed making `RenderFramePlan`
> a **value member of `RenderFrameContext`** while `FrameDeps` continued to expose it as a
> pointer "into the owning context." Given the by-value copy at
> `render_pipeline_stages.cpp:1134` (§1.3), that design is **actively worse than the status
> quo**: copying the context would produce an object whose `deps.frame_plan` points at the
> *source* context's plan member. The pointer is non-null, so `validate()` — which the same
> revision claimed "now covers frame_plan" — would **pass**, while the consumer silently
> reads another context's plan and dangles as soon as the source dies. A latent
> use-after-scope with a loud comment would have been traded for a silent aliasing bug with
> a green validator. The corrected design below removes the self-reference entirely.

**Design: the plan is owned by a frame-scoped owner that the context borrows from, and the
context is made non-copyable.**

- Introduce `FrameExecution` (name provisional) — a **stack-scoped owner** created once at
  the single route-selection site per frame. It owns, by value: the `RenderFramePlan`, the
  `FrameStateProvider`, and the `RenderFrameContext`. Nothing inside `RenderFrameContext`
  points at anything inside `RenderFrameContext`.
  ```
  class FrameExecution {              // stack-scoped, one per frame per route
      RenderFramePlan  plan_;         // built exactly once, const thereafter
      FrameStateProvider provider_;   // outlives every stage in this frame
      RenderFrameContext ctx_;        // deps.frame_plan == &plan_  (owner is OUTSIDE ctx_)
  public:
      FrameExecution(const FrameExecution &) = delete;   // non-copyable, non-movable
      FrameExecution &operator=(const FrameExecution &) = delete;
      const RenderFramePlan &plan() const { return plan_; }
      RenderFrameContext &context() { return ctx_; }
  };
  ```
- **`RenderFrameContext` becomes non-copyable and non-movable** (`= delete` on the copy and
  move members). This is the load-bearing part: it makes the `:1134` copy a **compile
  error**, forcing that path to be rewritten to take `RenderFrameContext &` — which is what
  the rebinding comment at `:1174-1177` is manually simulating today. Deleting the copy
  also retires the existing `state_view` / `mutation_access` self-reference hazard, not just
  the plan one.
- **`validate()` stops exempting `frame_plan`.** Once the plan's owner outlives the context
  by construction and the context cannot be copied, a null `frame_plan` is a real error and
  the exemption comment (`gaussian_splat_renderer.h:428-431`) is deleted rather than
  reworded.
- **Lifetime statement (normative):** `FramePlan` is created exactly once per frame per
  route, at the route-selection site, by `build_frame_plan(...)`. It is **immutable after
  construction** — expose it downstream only as `const RenderFramePlan &`. Its lifetime is
  exactly the enclosing `FrameExecution` scope. **No stage may store, defer, or async-pass
  it**; a stage that needs plan data past its own scope copies the fields it needs into its
  own result type. (`build_frame_plan` is already a pure function over explicit inputs —
  `gaussian_splat_renderer.h:551-568` — so this is a lifetime/ownership change, not a logic
  rewrite.)
- **If a future async/deferred stage is genuinely required**, the plan moves to a pooled
  allocation carrying a **generation/scope token** that `validate()` checks (the alternative
  #529 proposes) — *not* to a context-embedded value. Deferral must not be added in the same
  slice that changes ownership.

Why not simply keep the borrow and document it harder: the three borrow sites plus the
by-value copy mean the invariant is already stated in three comment blocks and still
unenforceable. Deleting the copy constructor is the only step here that a compiler checks.

### 2.2 Unit B — split `IFrameStateView` / `IFrameMutationAccess` into per-stage capability views (fixes 1.1, 1.2)

Replace the god-provider with small, stage-scoped interfaces resolved from an owned
snapshot, not from the live renderer:

| Capability port | Reads | May mutate | Consumed by |
| --- | --- | --- | --- |
| `CullPort` | scene, streaming, resource, config, `gpu_culler` | `cull_io` sink only | Cull stage |
| `SortPort` | cull outputs, sorting_state (read), sorting_pipeline | `sort_io` + sorted-result sink | Sort stage |
| `RasterPort` | FramePlan, resource, subsystem (rasterizer), config | `raster_io` + raster metrics sink | Raster stage |
| `CompositePort` | raster output, output_compositor, render target | `composite_io` sink | Composite stage |
| `ResourceOwner` | — | owns/frees GPU RIDs (device generation guarded) | resource orchestrator |

- **Kill the `static` fallbacks (§1.2) — all 40, at both levels.** Capability ports take
  references to the *existing* orchestrator-owned buckets (resolved non-null once at frame
  entry), so no accessor needs a reference-returning null fallback. Both the provider's 18
  and the facade's 22 forwarding-accessor statics are deleted; a missing dependency becomes
  a **typed skip at frame entry**, not a silent shared-global write. A slice that deletes
  only one of the two sets does not close this hazard.
- **Narrow the owners' own mutable escapes.** Because the buckets are already
  orchestrator-owned (§1.1), the ports should be resolved from the owning orchestrator, and
  the `access_*_mutable()` escapes (`render_data_orchestrator.h:56,58`,
  `render_sorting_orchestrator.h:55`) shrink to exactly the ports that need them. This is the
  step that converts *nominal* ownership into *enforced* ownership; it is the real content of
  #356's "owned state boundaries," and it is smaller than the previous revision implied
  because the by-value ownership already exists.
- `FrameStateProvider` remains **only as a temporary adapter** implementing these
  ports over the existing buckets during migration (the inventory's "temporary adapter
  over smaller snapshots/sinks, not a renamed god object",
  `stage-first-ownership-inventory.md:153`). It is deleted in the final cleanup slice.

### 2.3 Unit C — single-source metric reset + stage-exit helper (fixes 1.4 / #528, #570, #591)

- **Reset — DONE on master, and the original proposal here was wrong.** This ADR proposed
  *"give `PerformanceMetrics` a `reset()` (or `*this = PerformanceMetrics{};`) and call it at
  all three sites."* **Do not implement that.** Per §1.4, PR #627 established that the three
  sites are intentional *partial* resets of different subsets, so a blanket reset would zero
  cumulative counters and per-stage outputs — a behavior change. The shipped design is five
  **named group helpers** composed per site
  (`render_types/render_performance_types.h:165-174`) plus the fail-closed
  `tests/ci/check_metric_reset_parity.py` coverage guard. That half of #528 is closed;
  nothing remains to do here.
- **Diagnostics + route-label key sets — STILL OPEN.** The same guard family was proposed
  for the ~100-key diagnostics Dictionary (`render_diagnostics_orchestrator.cpp`,
  `_append_production_frame_metrics`) and the route-UID→label map
  (`render_route_labels.cpp`). **Neither guard exists**: no file under `tests/ci/` references
  either symbol. This is the remaining content of S1 and the unmet part of **R9**. The
  metric-reset guard is the model to copy — it parses the producing code and requires each
  key to be either covered or explicitly allow-listed with a reason, failing closed on syntax
  it does not recognize.
- **Stage-exit helper (#570):** one `stamp_stage_exit(route_uid, reason, metrics_deltas,
  io_fields)` consolidating the four+one duplicated ~40-line blocks (§1.4). Behavior is
  a mechanical fold — the produced `StageResult`/`StageIO` must be byte-identical.
- **Sizing/clamp helper (#591):** extract `clamp_visible_to_sort_cap(...)` +
  `populate_instance_pipeline_buffers(...)` shared by both publishers; keep the two
  distinct *sizing-input* computations (atlas vs regulator) at their call sites, since
  those policies legitimately differ. A future layout change then lands in one helper.

### 2.4 Unit D — make `StageIO` failures fail-closed (fixes 1.5 / #587)

`validation_failed` must **force a failed `StageResult`** (a typed skip UID, e.g.
`COMMON_FAIL_STAGE_IO_INVALID`) so a poisoned StageIO skips the frame explicitly and
observably, instead of only feeding the overlay. `_finalize_stage_io` returns the
verdict; the cull/sort/raster/composite sites branch on it (§1.5). Add a doctest that
a poisoned StageIO (`output_count > input_count`, or missing output buffer with
nonzero count) **fails the stage**. If the owner decides diagnostic-only is
intentional for a specific stage, that exemption is documented per-stage with a
reason — fail-closed is the default.

### 2.5 Facade after decomposition

`GaussianSplatRenderer` keeps only: Godot-facing API, frame entry/route selection
(produce the one `RenderRouteDecision` + `FramePlan`), and lifetime of the owned
sub-contexts. Facade methods become **thin delegations** to owned services with
explicit input/output types — the #356 "done when" (facade methods delegate to small
owned services with explicit contracts; new code reviewable by subsystem).

> **Reachability defect (review round 2): S1–S9 cannot deliver this criterion.** Verified on
> master `9161d92f349`:
>
> ```
> grep -rnE '^[A-Za-z_].*GaussianSplatRenderer::[A-Za-z_~]+\(' --include=*.cpp modules/ tests/
> ```
>
> yields **275** `GaussianSplatRenderer::` method **definitions**, of which only **124** live
> in `gaussian_splat_renderer.cpp`. **147 — a majority — are defined in
> `render_*_orchestrator.cpp` TUs** (debug_state 40, quality 23, device 19, config 17, data 13,
> sorting 11, output 10, diagnostics 8, resource 5, instancing 1), with the remaining 4 in
> `interfaces/interactive_state_manager.cpp` (3) and `gaussian_splat_renderer_bindings.cpp` (1).
>
> These are facade methods *physically relocated into orchestrator TUs* while remaining members
> of `GaussianSplatRenderer` and calling facade privates. They are the inverse of the target
> shape: the orchestrator TU is a file-level split, not an ownership boundary, so the method
> still has full access to the facade's state and no explicit input/output contract. **No slice
> in §4 touches them.** S7/S8/S9 restructure the frame path, the provider and the state ports;
> none of them converts a `GaussianSplatRenderer::foo()` defined in
> `render_quality_orchestrator.cpp` into `RenderQualityOrchestrator::foo()` with a typed
> contract. So #356's first "done when" — *"Renderer facade methods delegate to small owned
> services with explicit input/output contracts"* — is **not reachable by this ADR's current
> slices**, and #356 must not be closed on their completion.
>
> This is the subject of **D6**. Two honest dispositions, both of which keep S1–S9 as written:
>
> - **Amend the criterion for this ADR**: this ADR closes the *state-ownership* half of #356
>   (owned sub-contexts, typed ports, no shared mutable god-bundle), and the 147-method
>   relocation is explicitly declared out of scope, with #356 staying open until a separate
>   work-stream lands it.
> - **Add a slice S11** (after S9) that migrates the 147 definitions to real orchestrator
>   member functions. This is large, mechanical, and carries the ODR/PMF hazard of R12 at scale;
>   it should be counted and staged per-orchestrator, not attempted in one diff.
>
> Either way the ADR must **stop implying** that finishing S9 satisfies #356's facade criterion.

---

## 3. `gpu_sorter.cpp` split + shader extraction (closes #525)

Two independent moves, sequenced so each is CI-green:

### 3.1 Extract embedded GLSL to on-disk files in the C5 matrix (closes #525)

- Move each embedded sorter kernel (§1.6) to a `shaders/` (or a new `shaders/sort/`)
  `.glsl` file: `sort_bitonic.glsl`, `sort_radix_histogram.glsl`,
  `sort_radix_wg_prefix.glsl`, `sort_radix_bin_prefix.glsl`, `sort_radix_scatter.glsl`,
  `sort_indirect_dispatch.glsl`, and the OneSweep kernels.
- Where a source is `vformat`-parameterized (e.g. `WORKGROUP_SIZE`, radix bits,
  `local_size_x = %d`), convert the substitutions to **`#define`s supplied at compile**
  (the same mechanism the tile shaders already use), so the file is a standalone,
  compilable unit.
- Add each new file (with its runtime-selectable define permutations: workgroup sizes,
  radix-bit widths, 32/64-bit key layout, subgroup on/off) to
  `compile_shaders.py:RUNTIME_SHADER_MATRIX` and the G4 exhaustive-branch set
  (`compile_shaders.py:684+`). Add `renderer/gpu_sorter.cpp` (or the extracted TUs) to
  the workflow `paths:` triggers so a sorter-shader edit runs the matrix. **This is the
  R3 step** (edits `.github/workflows/gaussian_shader_validation.yml`).
- The C++ side loads these via the existing `shader_compilation_helper` /
  `SPIRVDiskCache` path (like every other module shader) instead of
  `create_compute_shader_from_spirv` on an inline string — gaining the SPIR-V disk
  cache for sorter kernels as a side benefit.

### 3.2 Split the three algorithms into separate TUs

- `gpu_sorter_bitonic.cpp` (`BitonicSort`, §1.6 `539-946`),
  `gpu_sorter_radix.cpp` (`RadixSort` + variants, `1180-2013+`),
  `gpu_sorter_onesweep.cpp` (OneSweep kernels, from `2731`), with
  `gpu_sorter_factory.cpp` retaining `GPUSorterFactory` + policy/probe
  (`948-1178`, plus the `_probe_*`/`select_sort_algorithm` helpers). Shared
  declarations stay in `gpu_sorter.h`.
- **ODR caution** (memory: MSVC PMF/ODR trap, and the #434 extraction lesson):
  when splitting a TU, enumerate **every** method definition of each class into
  exactly one new TU — do not diff-grep. The `_bind_methods`/GDCLASS registrations
  (`BitonicSort::_bind_methods` `:550`, `RadixSort::_bind_methods` `:1189`) must move
  with their class. Add the new `.cpp`s to `SCsub`.

---

## 4. Staged migration — CI-green + behavior-preservation proof per step

Each slice is independently reviewable, keeps CI green, and carries a
behavior-preservation proof appropriate to its risk. Ordering puts the
lowest-risk/highest-safety folds first and the state-ownership cuts last, matching the
inventory's W1→W2→W3 gate.

| Slice | Risk | Change | Behavior-preservation proof |
| --- | --- | --- | --- |
| **S1** *(re-scoped — metric-reset half already merged)* — diagnostics-dict + route-label key-set parity guards | R1 (guard scripts + tests only; no render-path edit) | **Not** `PerformanceMetrics::reset()` — that half shipped in #627 and its blanket-reset form was refuted (§1.4, §2.3). Remaining: a fail-closed key-set parity guard for the ~100-key diagnostics Dictionary (`render_diagnostics_orchestrator.cpp`, `_append_production_frame_metrics`) and for the route-UID→label map (`render_route_labels.cpp`), modeled on `tests/ci/check_metric_reset_parity.py` | Guard fails-without proof (inject an untracked key → CI fails; remove → passes), matching #627's evidence pattern; guard wired into the `--guard-only` lane; **no render-path source edited**, so no GPU evidence is required for this slice |
| **S2** — stage-exit helper (#570) | R2 | Fold the 4+1 stamping blocks into `stamp_stage_exit(...)` | `StageResult`/`StageIO` byte-identical on the skip/fail paths (unit test snapshots each route UID); runtime route-label telemetry unchanged |
| **S3** — publisher sizing/clamp helper (#591) | R2 | Extract shared clamp + buffer-population; keep distinct sizing inputs | Resident + streaming `InstancePipelineBuffers` fields identical before/after on a streamed and a resident scene; VRAM + sort-cap telemetry unchanged |
| **S4** — StageIO fail-closed (#587) | R2 | `validation_failed` → failed `StageResult`; per-stage branch; doctest | New doctest: poisoned StageIO fails the stage; existing valid frames still render (visual gate on real-scan content); no new skips on GrandmasHouse |
| **S5** — extract sorter GLSL to files + C5 matrix (#525) | **R3** (workflow) | Move embedded kernels to `.glsl`, wire into `compile_shaders.py` + workflow triggers | Shader-validation matrix compiles every sorter permutation green; runtime A/B: bitonic/radix/onesweep each still sort correctly (sorted-key monotonicity + visual gate); SPIR-V byte-compare of extracted vs inline kernel where feasible |
| **S6** — split `gpu_sorter.cpp` into per-algorithm TUs | R2 | Mechanical TU split + `SCsub` (§3.2) | Link-clean (no ODR dup/missing symbol); binary behavior identical (same sorter selected + same output on a fixed scene); enumerate-all-method-defs check |
| **S7** — `FramePlan` gets a named owner; `RenderFrameContext` becomes non-copyable (#529) | R2 | Introduce `FrameExecution` (§2.1); `= delete` copy/move on `RenderFrameContext`; rewrite the `:1134` copy to a reference; delete the 3 borrow comments + the validator exemption | Copy-deletion is compile-enforced (the `:1134` copy must fail to compile before it is rewritten — show that build error as evidence); `validate()` now covers `frame_plan`; frame output identical on resident + instanced + stage-runner routes; ASan/UBSan clean over the frame path |
| **S8** — per-stage capability ports; delete **all 40** `static` fallbacks (§1.1, §1.2) | R2 | Introduce `CullPort`/`SortPort`/`RasterPort`/`CompositePort` resolved from the owning orchestrators; narrow `access_*_mutable()`; `FrameStateProvider` becomes adapter over them | Each stage reads/writes only its port (compile-enforced); **zero** `static … fallback;` remain in `gaussian_splat_renderer.cpp` (grep guard, both the 18 provider and 22 facade sets); full frame telemetry + visual gate unchanged across all routes |
| **S9** — cleanup: remove the `FrameStateProvider` adapter + shims | R2 | Delete the transitional adapter once all call sites use ports | Facade methods are thin delegations; dependency-rule check; final visual + telemetry parity |
| **S10** — (optional) extract `RenderFrameExecutor` + timing + shader-compile from `tile_renderer.cpp` (§1.7) | R2 | Move the ~1,080-LOC executor, GPU-timing subsystem, and shader-compile orchestration into owned services | Tile frame output + timing telemetry identical on GrandmasHouse; stage delegation unchanged; ships after facade+sorter |

**Cross-cutting evidence rule (renderer = R2/R3):** every slice that touches the
render path provides runtime/GPU evidence measured against the immutable base — the
production-gates runtime harness, GPU harness, and a visual gate on real-scan content
(GrandmasHouse), per `renderer/AGENTS.md` and the "visual validation gate" rule. No
guard, baseline, or threshold is weakened to pass.

**Sequencing rationale:** S1–S4 are pure de-duplication/fail-closed folds that make
the state contract *legible* without moving ownership (inventory W1 "make ownership
explicit"). **S1's metric-reset half is already done on master (#627); what remains of S1 is
guard-script-only and touches no render-path source, so it is unblocked, R1, and can land in
parallel with S2–S4 rather than gating them.** S5–S6 isolate the sorter (independent of the facade). S7–S9 perform the
actual ownership cut (inventory W2/W3) only after the duplication is gone and tests
pin the route/stage/StageIO contracts — so the risky state-partition lands on a
characterized, guard-protected base. S10 (tile) is optional and last.

---

## 5. Issue-closure mapping

| Issue | Title (short) | Closed/advanced by | Proof |
| --- | --- | --- | --- |
| **#356** | Decompose renderer around owned state | S7–S9 close the **state-ownership** half only. The **facade-delegation** half is **not reachable by S1–S9** (§2.5): 147 of 275 `GaussianSplatRenderer::` method definitions live in `render_*_orchestrator.cpp` TUs as relocated facade members, and no slice converts them. **Do not close #356 on S9.** | Per-stage ports + no shared mutable god-bundle (deliverable). Facade-delegation criterion: pending **D6** — either amended out of this ADR's scope or given a dedicated slice/work-stream |
| **#528** | Hand-maintained ~40-field metric reset ×3, no parity guard | **CLOSED by PR #627** (merged 2026-07-18, after this ADR's original base) — *not* by this ADR | Five composable `reset_*()` group helpers (`render_performance_types.h:165-174`) + fail-closed `tests/ci/check_metric_reset_parity.py` in the `--guard-only` lane. **The blanket `reset()` this ADR proposed was rejected as a behavior change.** The diagnostics-dict + route-label key-set guards named in #528's body did **not** land and remain as the re-scoped **S1** |
| **#529** | `frame_plan` borrow exempt from `validate()` — latent UAF | **S7** | Plan owned by a `FrameExecution` scope *outside* the context; `RenderFrameContext` non-copyable (compile-enforced), so the `:1134` copy hazard goes with it; validator exemption deleted; 3 stack-borrows retired |
| **#570** | Duplicated ~40-line stage-exit stamping | **S2** | One `stamp_stage_exit` helper; byte-identical results |
| **#591** | Dup `InstancePipelineBuffers`/`sort_cap` clamp resident vs streaming | **S3** | Shared clamp + population helper; distinct sizing kept |
| **#587** | `StageIO.validation_failed` diagnostic-only | **S4** | Fail-closed StageResult + doctest |
| **#525** | Embedded sorter GLSL invisible to C5 compile matrix | **S5** | Kernels on-disk in `RUNTIME_SHADER_MATRIX`; permutations compiled in CI |
| **#588** | Destructor render-thread dispatch can block indefinitely | **Not addressed** | Separate lifetime bug (`gaussian_splat_renderer.cpp:1244-1247`); needs a bounded-timeout + synchronous-teardown fallback — recommend a standalone R2 PR, not folded into this decomposition. Noted so the mapping is honest. |

> Note on numbering: the task brief paired the gpu_sorter split with "#588 (embedded
> sorter GLSL outside compile CI)". The embedded-sorter-GLSL issue is actually **#525**
> ("Embedded sorter GLSL (radix/bitonic/remap) is invisible to the C5 shader-permutation
> compile CI"); #588 is the unrelated destructor-dispatch bug. This ADR closes #525 and
> explicitly leaves #588 to a separate change.

---

## Invariant list — what every slice is graded against

Checkable. A slice that violates one is rejected even with green CI.

| # | Invariant | How it is checked |
| --- | --- | --- |
| **R1** | The §1.1 ownership map is accurate and stays accurate: each bucket has exactly one owning declaration, and the ADR's table matches the code. | A CI guard that re-derives the owner of each bucket from the accessor bodies and diffs against the §1.1 table. This ADR was approved on a wrong map once; the guard is the fix. |
| **R2** | No new alias of a bucket is introduced. The alias chain (owner → facade accessor → provider → `deps` pointer) only ever shortens. | Grep guard: count of facade forwarding accessors + provider getters is monotonically non-increasing. |
| **R3** | **`RenderFrameContext` is non-copyable and non-movable** after S7, and no self-referential pointer into it exists (`frame_plan`, `state_view`, `mutation_access` all point at objects that outlive it). | Compile-enforced (`= delete`); a static-assert on `!std::is_copy_constructible_v<RenderFrameContext>`. |
| **R4** | `FrameDeps::validate()` has **no exemptions**. Every pointer it declares is validated, `frame_plan` included. | Read the function; assert the exemption comment is gone and a null-plan case fails. |
| **R5** | `FramePlan` is built exactly once per frame per route, is `const` downstream, and is never stored, deferred, or async-passed. | Grep guard: `build_frame_plan(` call sites == route-selection sites; downstream signatures take `const RenderFramePlan &`. |
| **R6** | Zero `static … fallback;` locals remain in `gaussian_splat_renderer.cpp` after S8 — all 40, not just the provider's 18. | Grep guard, count must reach 0. |
| **R7** | A missing dependency produces a **typed skip with a route UID**, never a write to a shared global and never a silently-continued frame. | S4's fail-closed doctest + the S8 frame-entry skip test. |
| **R8** | After S4, a poisoned `StageIO` (`output_count > input_count`, or missing output buffer with nonzero count) **fails the stage**. Per-stage exemptions exist only where documented with a written reason. | New doctest per §2.4; the exemption list is enumerated in-code. |
| **R9** | `PerformanceMetrics` reset is structural and covers every field **(already satisfied on master by #627 — five composable `reset_*()` group helpers plus the fail-closed `tests/ci/check_metric_reset_parity.py`; note the invariant is *group-composable*, not a single blanket `reset()`, which was deliberately rejected)**; the same must hold for the diagnostics key set and the route-label map — **both still unguarded, and the remaining content of the re-scoped S1**. | Metric half: the merged parity guard + the `test_diagnostics.h` reset doctests. Diagnostics/route-label half: new key-set parity guards with a fails-without proof. |
| **R10** | Stage-exit stamping produces **byte-identical** `StageResult`/`StageIO` before and after the S2 fold, for every route UID. | Snapshot unit test per route UID. |
| **R11** | Sorter GLSL: every runtime-selectable permutation compiles in CI after S5. No kernel remains invisible to the compile matrix. | `compile_shaders.py` matrix green with the new files + permutations; grep guard that no `R"(#version` remains in the sorter TUs. |
| **R12** | The S6 TU split introduces no ODR violation: **every** method definition of each split class lands in exactly one TU (enumerated, not diff-grepped), and `_bind_methods` moves with its class. | Link-clean build + the enumerate-all-method-defs check (per the #434 lesson). |
| **R13** | Frame output is unchanged: identical rendered result and identical telemetry on resident, instanced, and stage-runner routes, for a fixed scene and camera path. | Telemetry diff + visual gate on real-scan content (GrandmasHouse), per slice. |
| **R14** | No guard, baseline, threshold, or coverage bar is lowered in any slice. | Diff review; guard files unmodified except to strengthen. |

## Evidence a slice must produce

Renderer work is **R2, escalating to R3** for S5 (workflow edit) and any serialized/format
contract. Every slice states which invariants it touches and attaches:

1. **Ownership-map evidence (R1, R2):** for any slice that moves state, the re-derived
   ownership table from the head, diffed against §1.1. This ADR's first revision shipped an
   inverted map; no slice is approved without this diff.
2. **Compile-enforcement evidence (R3, R12):** for S7, the **build error** produced by the
   `:1134` copy before it is rewritten — that error is the proof the copy is really gone,
   not merely edited. For S6, the enumerated method-definition list and a clean link.
3. **Guard lane:** `run_module_tests.py --guard-only` green, plus the R5/R6/R11 grep guards,
   landing with the slice that makes them true.
4. **Behavior-preservation proof** as specified per-slice in the §4 table — byte-identical
   `StageResult`/`StageIO`, identical `InstancePipelineBuffers`, identical telemetry.
5. **Runtime/GPU evidence (R13):** the production-gates runtime harness, the GPU harness, and
   a **visual gate on real-scan content (GrandmasHouse)** measured against the immutable base.
   Agents cannot raster locally; a slice without GPU-runner output reports "not run", never
   "passed".
6. **S5 additionally (R3-class):** the shader-validation matrix compiling every sorter
   permutation, plus a runtime A/B showing bitonic/radix/onesweep each still sort correctly
   (sorted-key monotonicity + visual gate), plus SPIR-V byte-compare of extracted vs inline
   kernels where feasible. Maintainer/CODEOWNER review for the workflow edit.
7. **Base anchoring:** base SHA recorded, and confirmation that the `file:line` anchors used
   were re-verified against it. This has now bitten twice — anchors drifted in revision 1
   (e.g. `render_pipeline_stages.cpp:1162` → `:1173`) and again by revision 2, when the base
   went ~30 commits stale and a merged PR (#627) silently invalidated **S1**. See the
   §"Anchor re-verification log". **Every slice must re-verify its own anchors against the
   commit it branches from, and must re-check whether any merged PR has already discharged
   part of its scope — a stale ADR slice is the failure mode this document has actually
   experienced, not a hypothetical one.**

## Decisions the owner needs to make

- **D1 — Approve this ADR-first R0 design and the S1–S9(+S10) ordering?** (Y/N / amend.)
- **D2 — `FramePlan` owner (§2.1):** `FrameExecution` stack-scoped owner **outside** the
  context + non-copyable `RenderFrameContext` (recommended — removes #529 and the existing
  `state_view`/`mutation_access` copy hazard together) vs pooled allocation + generation
  token? The previously-recommended "value member on `RenderFrameContext`" option is
  **withdrawn**: §1.3 shows the context is copied at `render_pipeline_stages.cpp:1134`, which
  would make that design a silent aliasing bug that `validate()` cannot catch.
- **D3 — StageIO fail-closed default (§2.4 / #587):** adopt "poisoned StageIO fails the
  stage" as the default, with per-stage documented exemptions only? (Recommended: yes.)
- **D4 — S5 is R3** (edits `gaussian_shader_validation.yml`): confirm the shader-matrix
  extension + workflow-trigger edit are in scope, or split the workflow edit into its
  own maintainer-gated PR? (Recommended: keep together; it is the point of #525.)
- **D5 — #588 and tile S10** stay separable: #588 gets its own PR; S10 (tile) is
  optional and deferred until facade+sorter land? (Recommended: yes.)
- **D6 — #356's facade-delegation exit criterion (§2.5): amend it, or add a slice for it?**
  Evidence: **147 of 275** `GaussianSplatRenderer::` method definitions are defined in
  `render_*_orchestrator.cpp` TUs while remaining facade members that call facade privates —
  a file-level split, not an ownership boundary. **No slice in §4 touches them**, so
  completing S1–S9 cannot satisfy #356's *"facade methods delegate to small owned services
  with explicit input/output contracts."* The tradeoff:
  - **Amend (recommended).** Declare this ADR's scope to be the state-ownership half; state
    plainly that the 147-method relocation is a **separate work-stream** and keep #356 open
    past S9. Cost: #356 stays open longer and the remaining work is unowned until someone
    files it. Benefit: no slice is graded against a criterion it structurally cannot meet,
    and the S1–S9 sequence stays small and reviewable.
  - **Add S11.** A dedicated post-S9 slice migrating the 147 definitions to real orchestrator
    member functions, staged per-orchestrator (debug_state 40, quality 23, device 19, config
    17, data 13, sorting 11, output 10, diagnostics 8, resource 5, instancing 1). Cost: large
    and mechanical, and it carries the **R12** ODR/PMF hazard at scale — every method
    definition of each class must be enumerated into exactly one TU, per the #434 lesson.
    Benefit: #356 closes on this ADR.

  Either answer is fine; what is not fine is leaving the current implication that S9 closes
  #356. **Also confirm the corrected denominator: 147 of 275, not "147 of 272."**

## Consequences

- **Positive:** the single mutable bundle is replaced by owned sub-contexts a stage
  can only touch through a typed port; the three duplication classes (metric reset,
  stage-exit stamping, publisher sizing) collapse to single sources with parity guards;
  the #529 UAF trap and the `static` fallback globals are deleted; StageIO becomes
  fail-closed; the sorter's GLSL enters the compile matrix and gains the SPIR-V disk
  cache. New renderer code becomes reviewable per-subsystem without reading the whole
  facade (#356 exit).
- **Cost:** S5/S7/S8/S10 need GPU-runner time for the visual + telemetry parity
  evidence (agents cannot raster locally). S6/S8 carry the ODR/PMF hazard that the
  memory notes flag — mitigated by enumerate-all-method-defs and link checks. The
  migration is ~9–10 slices; it is deliberately incremental to keep each `base..head`
  diff small and each behavior-preservation proof self-contained.
- **No gate weakened:** every slice adds or preserves guards; none lowers a baseline,
  threshold, or coverage bar.

## Anchor re-verification log (review round 2)

Every `file:line` in this ADR was re-checked by `grep -n` against master `9161d92f349`. The
ownership map (§1.1), all nine facade forwarders, the three `frame_plan` borrow sites, the
`validate()` exemption, the `access_*_mutable()` escapes, the publisher clamp pair, the
`_finalize_stage_io` definition, `compile_shaders.py:142`/`:684`, the destructor dispatch, and
both `stage-first-ownership-inventory.md` quotes are **unchanged**. The counts (11 headers,
10 back-pointers, 78 provider constructions, 40 static fallbacks) all reproduce. Corrected:

| Item | Was | Now |
| --- | --- | --- |
| `FrameDeps` struct extent | `h:387-405` | `h:386-435` (field list `:387-405`) |
| Rebinding comment, stage runner | `render_pipeline_stages.cpp:1174-1177` | `:1175-1177` (`:1174` is blank) |
| 2nd metric-reset site | `render_pipeline_stages.cpp:2943-2983` | `:2909-2910` — and it is now a `reset_*()` group call, not a hand-list (§1.4) |
| Stage-exit stamping sites | `:3005, 3026, 3083, 3114` | `:3022, 3027, 3053, 3058` |
| Cull `_finalize_stage_io` call | inside `:1460-1469` | the call itself is at `:1467` |
| RadixSort embedded GLSL | `1649, 1734, 1802, 1847, 2173` | `1656, 1741, 1809, 1854, 2204` (uniform +7) |
| OneSweep block + GLSL | `2749-2982`; `2749, 2816, 2912, 2975` | ctor `2731`, `_bind_methods` `2739`; GLSL `2780, 2847, 2943, 3006` |
| `tile_renderer.cpp` shader includes | `:30-33` | `:31-34` (`:30` is `performance_monitors.h`) |
| `RenderFrameExecutor` | `:270-1352` | `:271-1352` |
| Anonymous-namespace subsystem | `:129-266` | the anonymous namespace spans `:47-269` |
| Facade static fallbacks | `:2634-2764` + `:3061-3068` | `:2635-2761` only; `get_debug_state` has none |
| `GaussianSplatRenderer::` definitions | "147 of 272" (review comment) | **147 of 275** |

**Stale anchors in the source itself**, found during this pass and worth fixing independently
of this ADR: `render_instancing_orchestrator.cpp:178` and `render_pipeline_stages.cpp:1171`
both say *"`RenderFrameDeps::validate()` exempts `frame_plan` (see
`gaussian_splat_renderer.h:387-389`)"*. The exemption actually lives at **`h:428-431`**;
`h:387-389` is now the top of the `FrameDeps` field list. S7 deletes these comments anyway,
but until then they misdirect.

## Related docs

- [Stage-first ownership inventory](stage-first-ownership-inventory.md) — the W0
  characterization this ADR builds its target on (#356).
- [Render pipeline architecture](render-pipeline.md) — frame entry, route, stage
  contracts, fallback semantics.
- [Renderer lifetime ownership](renderer-lifetime-ownership.md) — per-owner
  create/destroy/idempotency/threading contracts at file:line precision.
- [Module architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md).
