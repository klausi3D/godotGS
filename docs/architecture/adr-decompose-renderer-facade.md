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
- **Base:** anchored at `origin/master` `237a4b1cc3965fdbd6f12dec825c0e2077b2e9ce`.
  Builds on the W0 characterization in
  [`stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md) (issue
  #356) — this ADR is the W1/W2 target and migration plan that inventory called for.

---

## 1. Current state — the shared-state bundle map

### 1.1 One mutable bundle, twelve namespaces over it

`GaussianSplatRenderer` (`renderer/gaussian_splat_renderer.{h,cpp}` + bindings,
~5,404 LOC across the three files) holds every per-renderer bucket as a direct
member/alias: `SceneState`, `StreamingState`, `SortingState`, `PipelineState`,
`PerformanceState`, `FrameState`, `ViewState`, `RenderConfig`, `DebugState`,
`DeviceState`, `ResourceState`, `SubsystemState`
(`gaussian_splat_renderer.h:201-334`). The twelve orchestrators do **not** own any of
these; each holds a raw back-pointer `GaussianSplatRenderer *renderer` and reaches
through it:

- `render_{config,data,debug_state,device,diagnostics,instancing,output,quality,resource,sorting,streaming}_orchestrator.h` — every one declares `GaussianSplatRenderer *renderer = nullptr` as a member (verified 12/12, e.g. `render_resource_orchestrator.h:18,47`, `render_streaming_orchestrator.h:12,69`).
- The access path is `FrameStateProvider`, constructed **61 times** from the raw
  renderer pointer across the facade, the stage runner, and all 12 orchestrators
  (e.g. `render_resource_orchestrator.cpp:535` `FrameStateProvider state_provider(renderer);`).

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

Every `FrameStateProvider` getter has a `static SortingState fallback;` /
`static StreamingState fallback;` / … local (`gaussian_splat_renderer.cpp:677-800+`,
one per bucket). These exist only to satisfy the reference-returning signature when
`renderer_view` is null, but they are **function-local statics shared across all
renderer instances and threads**. The mutable variants return a mutable reference to
this shared object (`get_sorting_state_mut()`, `:767-775`). A null-renderer mutation
path would silently write a process-global; the inventory already flags this
(`stage-first-ownership-inventory.md:168`, "FrameStateProvider fallback statics and
broad mutable accessors … split into read-only snapshots plus small mutation sinks").

### 1.3 Hazard B — `frame_plan` borrow is a designed use-after-scope trap (#529)

`RenderFramePlan` is built as a **stack local** and its address is stored into
`frame_context.deps.frame_plan` at three sites:

- `gaussian_splat_renderer.cpp:2591-2595` (resident/main route)
- `render_instancing_orchestrator.cpp:170-181` (instanced route)
- `render_pipeline_stages.cpp:1162` (stage-runner build path)

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

### 1.4 Hazard C — hand-maintained parallel lists with no parity guard (#528, #570, #591)

The single shared `PerformanceMetrics` bundle is zeroed **field-by-field** at three
independent sites, each ~30–40 fields, none generated from the struct:

| Site | Lines | Scope |
| --- | --- | --- |
| `render_pipeline_stages.cpp` | `2040-2084` | raster/GPU metric reset (frame skip path) |
| `render_pipeline_stages.cpp` | `2943-2983` | raster/GPU metric reset (main path, `render_sorted_splats_with_context`) |
| `render_resource_orchestrator.cpp` | `541-568` | GPU-pass metric reset (no-rasterizer branch) |

A new `PerformanceMetrics` field added to the struct but missed at any one site
silently reports **the previous route's value** — a stale-telemetry bug class (#528).
The same hand-list pattern (no parity guard) recurs in the ~100-key diagnostics
Dictionaries built field-by-field in `render_diagnostics_orchestrator.cpp:299ff`
(`_append_production_frame_metrics`) and the route-UID→label maps in
`render_route_labels.cpp`.

**Stage-exit stamping duplication (#570):** four near-identical ~40-line
`StageResult`/`StageIO` skip/fail stamping blocks exist in
`render_sorted_splats_with_context` alone (`render_pipeline_stages.cpp:3005, 3026,
3083, 3114`), and again in `render_instancing_orchestrator.cpp:196-225`. Each repeats
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
// render_pipeline_stages.cpp:1460-1469
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
| `RadixSort` (+ variants) | `1180-2013+` | `1649` histogram, `1734` wg-prefix, `1802` bin-prefix, `1847` scatter, `2173` indirect-dispatch |
| OneSweep variant kernels | `2749-2982` | `2749, 2816, 2912, 2975` |

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
generated headers (`tile_renderer.cpp:30-33`, `#include "../shaders/tile_binning.glsl.gen.h"`
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

- `RenderFrameExecutor` — the per-frame pipeline state machine, `tile_renderer.cpp:270-1352` (~1,080 LOC, the single largest unit; owns validate→params→global-sort→raster→resolve→finalize).
- `initialize`/`cleanup`/`_ensure_resources` lifetime (`1474-1727`, `1783-1878`).
- Shader-defines assembly + compilation orchestration + `_detect_subgroup_support` (`2023-2385`).
- GPU timestamp/timing subsystem (`2426-2757`).
- `_evaluate_raster_path` compute-vs-fragment decision (`2778-2853`).
- Device/descriptor + instance-pipeline-binding cache (`2971-3225`); statistics/density aggregation (`3273-3458`).
- The adaptive-overlap-budget free-function subsystem in the anonymous namespace (`129-266`), which operates on the public runtime-state struct but is not part of `TileAdaptiveController`.

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

### 2.1 Unit A — `FramePlan` becomes an owned value, produced once (fixes 1.2, 1.3)

- Make `RenderFramePlan` a **value member of `RenderFrameContext`** (owned for the
  frame's lifetime), not a borrowed pointer to a caller stack local. `FrameDeps`
  exposes it as `const RenderFramePlan &` / `const RenderFramePlan *` **into the
  owning context**, and `validate()` stops needing an exemption because there is no
  dangling borrow. This deletes the three `&frame_plan` stack-address stores
  (§1.3) and their warning comment.
- Contract: `FramePlan` is **immutable after `build_frame_plan(...)`** for that frame.
  Build it exactly once per frame at the single route-selection site; downstream
  stages read it. (`build_frame_plan` already exists as a pure function taking
  explicit inputs — `gaussian_splat_renderer.h:551-568` — so this is a lifetime/owner
  change, not a logic rewrite.)
- If a future async/deferred stage is genuinely required, back `FramePlan` with a
  pooled allocation carrying a **generation/scope token** the validator checks —
  the alternative #529 proposes. Recommended now: the value-member form (simplest,
  removes the trap outright).

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

- **Kill the `static` fallbacks (§1.2):** capability ports take references to *owned*
  sub-contexts (constructed non-null at frame entry), so no accessor needs a
  reference-returning null fallback. The provider's `static X fallback;` locals are
  deleted; a missing dependency becomes a typed-skip at frame entry, not a silent
  shared-global write.
- `FrameStateProvider` remains **only as a temporary adapter** implementing these
  ports over the existing buckets during migration (the inventory's "temporary adapter
  over smaller snapshots/sinks, not a renamed god object",
  `stage-first-ownership-inventory.md:153`). It is deleted in the final cleanup slice.

### 2.3 Unit C — single-source metric reset + stage-exit helper (fixes 1.4 / #528, #570, #591)

- **Reset:** give `PerformanceMetrics` a `reset()` (or `*this = PerformanceMetrics{};`
  default-init semantics) and call it at all three sites (§1.4). Replace the three
  hand-lists with the one call. Add a **static parity guard** in the spirit of C6/C7
  (`tests/ci/check_gaussian_layout_sync.py`) asserting the reset covers every field —
  or, better, make reset structural so no list exists to drift. The same guard family
  covers the diagnostics-Dictionary key set (`render_diagnostics_orchestrator.cpp:299ff`)
  and the route-label map (`render_route_labels.cpp`).
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
  `gpu_sorter_onesweep.cpp` (OneSweep kernels, `2749-2982`), with
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
| **S1** — metric reset single-source | R2 | `PerformanceMetrics::reset()` + call at the 3 sites (§1.4); add parity guard | Guard passes; doctest asserts reset zeroes every field; frame telemetry diff on GrandmasHouse identical before/after |
| **S2** — stage-exit helper (#570) | R2 | Fold the 4+1 stamping blocks into `stamp_stage_exit(...)` | `StageResult`/`StageIO` byte-identical on the skip/fail paths (unit test snapshots each route UID); runtime route-label telemetry unchanged |
| **S3** — publisher sizing/clamp helper (#591) | R2 | Extract shared clamp + buffer-population; keep distinct sizing inputs | Resident + streaming `InstancePipelineBuffers` fields identical before/after on a streamed and a resident scene; VRAM + sort-cap telemetry unchanged |
| **S4** — StageIO fail-closed (#587) | R2 | `validation_failed` → failed `StageResult`; per-stage branch; doctest | New doctest: poisoned StageIO fails the stage; existing valid frames still render (visual gate on real-scan content); no new skips on GrandmasHouse |
| **S5** — extract sorter GLSL to files + C5 matrix (#525) | **R3** (workflow) | Move embedded kernels to `.glsl`, wire into `compile_shaders.py` + workflow triggers | Shader-validation matrix compiles every sorter permutation green; runtime A/B: bitonic/radix/onesweep each still sort correctly (sorted-key monotonicity + visual gate); SPIR-V byte-compare of extracted vs inline kernel where feasible |
| **S6** — split `gpu_sorter.cpp` into per-algorithm TUs | R2 | Mechanical TU split + `SCsub` (§3.2) | Link-clean (no ODR dup/missing symbol); binary behavior identical (same sorter selected + same output on a fixed scene); enumerate-all-method-defs check |
| **S7** — `FramePlan` becomes owned value (#529) | R2 | Value member on `RenderFrameContext`; delete 3 `&frame_plan` borrows + validator exemption | `validate()` now covers frame_plan; frame output identical on resident + instanced + stage-runner routes; ASan/UBSan clean over the frame path |
| **S8** — per-stage capability ports; delete `static` fallbacks (§1.1, §1.2) | R2 | Introduce `CullPort`/`SortPort`/`RasterPort`/`CompositePort`; `FrameStateProvider` becomes adapter over them | Each stage reads/writes only its port (compile-enforced); no `static` fallback remains; full frame telemetry + visual gate unchanged across all routes |
| **S9** — cleanup: remove the `FrameStateProvider` adapter + shims | R2 | Delete the transitional adapter once all call sites use ports | Facade methods are thin delegations; dependency-rule check; final visual + telemetry parity |
| **S10** — (optional) extract `RenderFrameExecutor` + timing + shader-compile from `tile_renderer.cpp` (§1.7) | R2 | Move the ~1,080-LOC executor, GPU-timing subsystem, and shader-compile orchestration into owned services | Tile frame output + timing telemetry identical on GrandmasHouse; stage delegation unchanged; ships after facade+sorter |

**Cross-cutting evidence rule (renderer = R2/R3):** every slice that touches the
render path provides runtime/GPU evidence measured against the immutable base — the
production-gates runtime harness, GPU harness, and a visual gate on real-scan content
(GrandmasHouse), per `renderer/AGENTS.md` and the "visual validation gate" rule. No
guard, baseline, or threshold is weakened to pass.

**Sequencing rationale:** S1–S4 are pure de-duplication/fail-closed folds that make
the state contract *legible* without moving ownership (inventory W1 "make ownership
explicit"). S5–S6 isolate the sorter (independent of the facade). S7–S9 perform the
actual ownership cut (inventory W2/W3) only after the duplication is gone and tests
pin the route/stage/StageIO contracts — so the risky state-partition lands on a
characterized, guard-protected base. S10 (tile) is optional and last.

---

## 5. Issue-closure mapping

| Issue | Title (short) | Closed/advanced by | Proof |
| --- | --- | --- | --- |
| **#356** | Decompose renderer around owned state | S7–S9 (tracked; full close when facade delegates to owned services) | Facade = thin delegations; per-stage ports; no shared mutable god-bundle |
| **#528** | Hand-maintained ~40-field metric reset ×3, no parity guard | **S1** | Single `reset()` + parity guard; diagnostics-dict + route-label guards |
| **#529** | `frame_plan` borrow exempt from `validate()` — latent UAF | **S7** | Owned value member; validator covers it; 3 stack-borrows deleted |
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

## Decisions the owner needs to make

- **D1 — Approve this ADR-first R0 design and the S1–S9(+S10) ordering?** (Y/N / amend.)
- **D2 — `FramePlan` owner (§2.1):** value member on `RenderFrameContext` (recommended,
  removes #529 outright) vs pooled allocation + generation token? (Recommended: value
  member.)
- **D3 — StageIO fail-closed default (§2.4 / #587):** adopt "poisoned StageIO fails the
  stage" as the default, with per-stage documented exemptions only? (Recommended: yes.)
- **D4 — S5 is R3** (edits `gaussian_shader_validation.yml`): confirm the shader-matrix
  extension + workflow-trigger edit are in scope, or split the workflow edit into its
  own maintainer-gated PR? (Recommended: keep together; it is the point of #525.)
- **D5 — #588 and tile S10** stay separable: #588 gets its own PR; S10 (tile) is
  optional and deferred until facade+sorter land? (Recommended: yes.)

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

## Related docs

- [Stage-first ownership inventory](stage-first-ownership-inventory.md) — the W0
  characterization this ADR builds its target on (#356).
- [Render pipeline architecture](render-pipeline.md) — frame entry, route, stage
  contracts, fallback semantics.
- [Renderer lifetime ownership](renderer-lifetime-ownership.md) — per-owner
  create/destroy/idempotency/threading contracts at file:line precision.
- [Module architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md).
