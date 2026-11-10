# ADR: One route, many demand producers — the world and node instances in one frame

- **Status:** **Accepted (maintainer decisions, 2026-11-04).** This record adopts
  [#788 Option 2](adr-single-route-per-frame-node-coexistence.md) ("genuine per-submission
  routing, accepted as target, deferred") as the direction and specifies how it is reached on
  the HLOD route first. Acceptance of this design does not establish implementation or runtime
  acceptance; the code PRs in §7 carry their own evidence.
- **Status update (2026-11-10):** §12 adds the route retirement decision (D-2): one route,
  and the chunk-streaming route and the legacy composite are deleted. See
  [Owner decision (D-2)](#owner-decision-d-2).
- **Date:** 2026-11-04.
- **Risk:** this document is R0 (`docs/**`). The implementation PRs it schedules are **R2**
  (`modules/gaussian_splatting/renderer/**`, and the director changes that alter what the
  renderer draws, which are R2 by design). The pick API (a bound method) and the
  runtime-profile scenario (`tests/runtime/run_runtime_validation.py`) are **R3**. The
  automatic-route PR changes existing scene behaviour and is classed R3 (public
  behaviour/compatibility, `docs/governance/agentic-engineering.md`).
- **Refs:** #788, #855, #1131, #1095, #1163.
- **Related ADRs:** [Single route per frame: world/instance node coexistence](adr-single-route-per-frame-node-coexistence.md)
  (#788, "the #788 ADR"); [Hierarchical LOD for streamed splat worlds](adr-hlod-streaming.md)
  ("HLOD ADR"), §6.9 and §14; [Worlds beyond VRAM](adr-beyond-vram-worlds.md) ("BV ADR");
  [Resident instanced contract](gaussian-resident-instanced-contract.md).
- **Base:** `992cb8eb733` (origin/master). Module code is identical between `44836e1d6a5`
  (the merge of #1293) and `992cb8eb733` (`git diff --stat 44836e1d6a5 992cb8eb733 -- modules/`
  is empty), so every `file:line` below holds at both. Each one was re-read at `992cb8eb733` for
  this record.
- **Paths** are relative to `modules/gaussian_splatting/` unless they start with `docs/`, `tests/`
  or `modules/` (repo root). `publisher.cpp` = `renderer/resident_instance_contract_publisher.cpp`;
  `director.cpp` = `core/gaussian_splat_scene_director.cpp`.
- **Evidence status:** every status in this record comes from reading code, git history and
  issue state. **No status rests on a GPU run.** Claims that need a run are marked
  **UNVERIFIED**.

---

## 1. Problem

GodotGS is for making video games. A game frame holds a large streamed world, static objects,
movable objects, interactive objects, lights and shadows at the same time. Today no route draws
a `GaussianSplatWorld3D` and a `GaussianSplatNode3D` in the same frame.

### 1.1 The world row exists only without node instances

`GaussianSplatSceneDirector::build_instance_buffer_for_renderer()` emits the world's identity
row (asset 0) only when `world->instance_store.is_empty()` (`director.cpp:1744`). The grading
rows repeat the rule (`:1967`), and so does the grading generation (`:2114`).
`InstanceStore::is_empty()` is `instances.is_empty()` (`core/gaussian_splat_scene_director.h:494`),
so a **hidden** node removes the world too: the row loop skips hidden records
(`director.cpp:1804`), the empty test does not.

Both canonical routes consume this list:

- **Resident canonical.** The publisher packs the world's data into the atlas at dense id 0
  (`publisher.cpp:304-314`), but no row references it, so its VRAM is spent and nothing draws
  it. The bootstrap row is suppressed whenever a world submission exists (`publisher.cpp:442-449`).
- **Chunk streaming.** The streaming orchestrator builds its rows from the same function
  (`renderer/render_streaming_orchestrator.cpp:801-803`). A node whose asset has no streaming
  slot falls back to the primary dense id (`:160-164`), so it draws world data at the node's
  transform.

### 1.2 The HLOD route is exclusive and single-instance

- `has_exclusive_hlod_world_submission()` requires `instance_store.is_empty()` plus an active
  submission by the owner (`director.cpp:3045-3049`).
- The publisher refuses the route with `hlod_not_exclusive_world_submission` or
  `hlod_paged_not_exclusive_world_submission` (`publisher.cpp:1628-1638`). The same predicate
  invalidates a prepared bundle (`:1380`), names `hlod_exclusive_submission_lost` (`:1480-1481`),
  and is reported per frame under the mislabelled key `deformation_or_instance_domain`
  (`:1930-1932`).
- The bundle build requires exactly one instance and one grading row
  (`instances.size() != 1 || grading.size() != 1` → `hlod_identity_shim_unavailable`,
  `publisher.cpp:1722`). The bundle is built from `instances[0]` and `grading[0]`
  (`:1817-1818`) with `instance_count = 1` (`:1317`). Every draw record names GPU instance 0
  (`make_drawn_node(0u, …)`, `:2196`).
- Any instance-generation change invalidates the prepared bundle (`:1382`), and a transform-only
  update bumps that generation (`director.cpp:1279`). A moving node would rebuild the bundle
  every frame.
- A V4 paged world has no canonical payload. When the HLOD route does not serve, the frame is
  skipped with `hlod_paged_route_unavailable` or the route's reason (`publisher.cpp:279-285`).
  **One node in the scene makes a paged world draw nothing, the node included.**

### 1.3 What is already multi-instance

The GPU side and the selector do not assume one instance:

- `DrawnNodeGPU { instance_id, chunk_id, … }` (`shaders/includes/gs_instance_layout.glsl:108-112`);
  the binning pass checks `selected.instance_id != splat_ref.instance_id`
  (`shaders/tile_binning.glsl:878-879`).
- The camera-relative frame applies the instance's rotation and uniform scale and takes T from the
  per-draw offset (`shaders/includes/gs_camera_relative.glsl:5-11`, `:43-53`;
  `shaders/tile_binning.glsl:900-904`).
- The host forms `camera_offset = R s c + T − camera` in double
  (`core/gaussian_splat_world_frame.h:58-73`).
- `SnapshotAsset` and `SnapshotInstance` carry per-asset payload bytes charged once per asset,
  precise placements and per-instance caps under one physical budget (`lod/hlod_view_set.h:9-19`,
  `:71-93`).

Only the CPU publisher and the director's row builder are wired to one row.

---

## 2. Coexistence history

| Date | Change | Effect on world + nodes in one frame |
|---|---|---|
| 2026-03-18 | `aeeb1475cb3`, the oldest module commit in this repository | One route for everything. Node assets are chunk-streamed per instance. The world's primary data gets a synthetic instance only when the instance cache is empty (`renderer/render_streaming_orchestrator.cpp@aeeb1475cb3:1292-1295`). Instances with chunk streaming work; world + nodes do not. |
| 2026-04-14 | #229 (`7c9a815f030`) | The world-submission row moves into the director as `if (world->instances.is_empty())` (`director.cpp@7c9a815f030:1134`). The either/or now lives in the director. |
| 2026-04-14 | #232 (`2bb260c6e9b`) | `GaussianSplatNode3D` pins the resident hint. Objects lose chunk streaming; in a mixed scene the world's hint decides the route. |
| 2026-04-21 → 06-25 | #280 (`61af553128c`), #363, #416 (`7876b90230a`) | #280 removes the legacy resident fallback. #416 makes `single_route_per_frame` and `alternate_backend_fallback_forbidden` authoritative (`renderer/render_types/render_pipeline_io_types.h:60-61`). A rejected route skips the frame (`renderer/gaussian_splat_renderer.cpp:2668-2670`). |
| 2026-08-10 | #856 (`aa8c3780773`), the #788 ADR | Option 1 (warning) accepted as interim, Option 2 accepted as target and deferred, #855 first. |
| 2026-10-23 | #1267 (`c4204ab3929`) | The HLOD route lands with `has_exclusive_hlod_world_submission`, exclusive from its first commit. |
| 2026-11-03 | #1293 (merge `44836e1d6a5`) | V4 paged worlds have no canonical fallback (`publisher.cpp:279-285`). One node blanks a paged world. |

In the history in this repository, no route ever drew a world submission together with node
instances. What worked, and still works, is chunk streaming of **instance assets** in one sort.
History before `aeeb1475cb3` is not in this repository.

---

## 3. Decisions (maintainer, 2026-11-04)

These decisions are binding.

1. **Product rule.** GodotGS is for making video games. A large streamed world, static objects,
   dynamic or movable single objects, interactive objects, lighting and shadows must all work
   **in the same frame**. A feature that works only with an exclusive world, with a single
   instance or with no other nodes is not acceptable. Any such precondition is a **blocker** that
   goes to the maintainer **before** it is built.
2. **One route, many demand producers** is the target. #788 Option 2 is adopted as the direction.
   The HLOD world cut and the node instances share **one draw list, one depth sort and one byte
   ledger per view**. Instances join the HLOD route as rows 1..N.
3. **Budget.** Visible objects reserve their share first, and the world keeps a **fixed minimum
   share (floor)**. If the objects would exceed their share, that is an **explicit refusal** with
   a distinct reason and a counter. It is never a silent cut.
4. **#855 ordering.** The "#855 first" constraint of #788 §5.2(3) is **waived for the HLOD route
   only**. #855 (the chunk route) is fixed in parallel as follow-up F1.
5. **Automatic route.** A world with an HLOD snapshot takes the HLOD route **automatically**,
   together with the objects. This is a behaviour change and lands in its own PR with a release
   note.
6. **Real splat shadow casters now**, not proxy meshes (#1095, #1163).

---

## 4. Design

### 4.1 Producers, rows and the draw list

Per view there is one draw list (`DrawnNodeGPU[]`), one sort and one byte ledger. Producers write
records into the same relative bundle, which then runs the existing depth, sort, binning and
raster chain once.

- **P0, the world cut.** The existing `ViewSet` selection over the HLOD tree. Row 0 is the world's
  identity row (asset 0).
- **P1..PN, the node instances.** One row per visible `GaussianSplatNode3D`, built by the director
  exactly as today's node loop does, with the world row prepended. In PR1b each instance is drawn
  whole: one draw record per chunk of its asset. F2 puts instances under HLOD.
- **Record order.** World records keep `NodeKey` order; instance records follow, ordered by
  (row, chunk). Ties stay deterministic by `(instance_key, NodeKey)` (BV ADR I-10).
- **Assets.** One `AssetMetaGPU` and one packed payload per distinct asset. Instances of one asset
  share its payload and pay one row each.
- `build_instance_buffer_for_renderer()` is left unchanged by the HLOD PRs, so the canonical
  routes keep their behaviour until F1. The HLOD route gets its own row builder
  (world row + node rows + parallel 64-bit asset ids + precise placements).

### 4.2 Per-row transforms in the camera-relative frame

- **World records:** unchanged. The `ViewSet` forms `DrawRecord::camera_offset` against
  `snapshot.origin` and its revision.
- **Instance records:** an instance asset's payload is asset-local, so `c = 0` and the offset is
  **`T − camera`**, computed **in double** with `PrecisePlacement::from_frame_transform(node
  global transform, snapshot.origin)` and `camera_relative_offset(placement, 0, camera)`
  (`core/gaussian_splat_world_frame.h:58-73`). It is computed once per instance per view and copied
  to each of that instance's records.
- **R and s** come from the instance row, which the shaders already read. The row's translation is
  ignored in relative mode (`shaders/tile_binning.glsl:900-904`).
- **Origin shift:** the `hlod_world_origin_shifted` guard stays (`publisher.cpp:2076`). Instance
  offsets use the same `snapshot.origin_revision` as world offsets, so the two never mix revisions.
- **Non-uniform scale** stays reduced to the maximum axis, as on every route today. It is not new.

### 4.3 Asset-set generation and row generation are separate

Today one instance generation decides whether the prepared bundle is current (`publisher.cpp:1382`),
and a transform update bumps it (`director.cpp:1279`). The design splits it:

- **Asset-set generation** (`get_instance_asset_generation_for_renderer`, `director.cpp:2185`):
  adding or removing an asset changes the packed content and still rebuilds the bundle.
- **Row generation:** transform, visibility and grading changes re-upload the rows only
  (N × row size). Offsets are recomputed every view, as world offsets already are.

**Moving nodes therefore cause no rebuild.** The resident publisher already makes the same split
between its atlas and instance generations.

### 4.4 Readiness: an idle world never hides the objects

Today a streamed world whose cut is not ready publishes an **idle** bundle with draw count 0
(`publisher.cpp:1443-1462`). In the target, idle means *world records = 0, instance records still
drawn*. The whole-tree canonical bundle carries the instance assets and rows too. A world that is
loading never blanks the objects, and BV ADR I-4 (complete-or-nothing) holds **per producer**.

### 4.5 Budget rule

- **One device ledger.** Instance payload, metadata, rows and the extra splat-ref and draw-entry
  capacity are added to `hlod_bundle_bytes` (`publisher.cpp:1206`), so `hlod_candidate_fits`
  charges them before allocation (`:1741`, `:1800`). Shared payload is charged once per distinct
  asset. On streamed and paged worlds the instance region sits at the tail of the relative atlas;
  its bytes enter the fixed bytes of `hlod_stream_page_budget` (`:1774-1779`), so the page budget
  shrinks and the ledger is never exceeded.
- **Per-view caps.** Today the world cut gets `min(leaf_count, max_splats)` (`:1657-1658`, `:2038`).
  The target splits the view caps (splats, draw records, overlap records):
  - The **world floor** `F` is a fixed minimum share of each cap that the world keeps.
  - The **object share** is `caps − F`. Visible objects reserve their need `R` from it first, computed
    on the CPU before selection.
  - The world cut gets `caps − R` (never less than `F`) through `spec.caps`; the `ViewSet` honours it
    or returns `NOT_ADMITTED` with its existing reason.
  - If `R > caps − F`, objects are admitted **in a deterministic order** until the object share is
    used up. The order is screen-space importance; ties are broken by the node's stable instance id,
    so the same view always admits the same objects and no flicker can result (#1131).
    - Each object that is not admitted is refused **explicitly**, as a whole object, never as a partial
      draw. Its reason is `hlod_instance_reserve_exceeds_object_share`.
    - Refusals are counted by `hlod_instance_refused_objects` (per frame) and
      `hlod_instance_reserve_refusal_frames`.
    - The node can query its own refusal state, so gameplay code can react.
    - The world keeps its floor `F` and keeps drawing.
    - Objects are never dropped as a group just because one of them does not fit: objects come first
      (decision 3). Stated per-object priorities that override the importance order are F3.
  - The sort capacity is `max(leaf_count, capacity)` (`:1685`) plus the instance capacity.
- **The value of `F`** is set and justified in PR1b. Exposing it as a project setting is a settings
  change and is R3 (F3); PR1b may ship it as a named constant.

### 4.6 Domains that stay refused on the HLOD route

Wind and sphere effectors (`publisher.cpp:1517-1524`, `:1930-1932`), painterly, the shadow instance
filter, and non-main or multi-view passes (`:1923-1926`) stay refused, each with its typed reason. The
mislabel at `:1930-1932` is fixed: loss of exclusivity no longer exists, and effectors keep their own
key. Each domain is lifted in its own PR (F5). Under decision 1, every such refusal is a listed gap in
§8, not an accepted precondition.

### 4.7 Automatic route

Today HLOD runs only on the resident route. Under `route_policy = streaming` the plan records
`hlod_route_policy_chunk_streaming` (`renderer/gaussian_splat_renderer.cpp:1700-1705`). The runtime cut
is opt-in (`rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled`, default `false`, `lod/lod_config.cpp:400`), and the project
default `route_policy` is streaming (`core/gaussian_splat_manager.cpp:1008`). Under decision 5, a world
that carries an HLOD snapshot takes the HLOD route with its objects without either setting. That PR
states in its release note how the two settings behave afterwards (opt-out or removal).

---

## 5. Alternatives rejected

| Alternative | Why rejected |
|---|---|
| **(b) Merge the HLOD cut into the resident/chunk canonical route** | The canonical route uses absolute float positions, the non-relative shader permutation and 8 B `VisibleChunkRefGPU` with no per-draw offset. Drawing a cut there means adding `DrawnNodeGPU` and the camera-relative frame to it, which re-implements the HLOD route inside it. It would also need page-managed residency in an atlas packed whole per asset. A V4 paged world has no canonical payload at all (`publisher.cpp:279-285`). The whole-tree canonical bundle loses the cut, the parents and the 100 km frame. |
| **(c) Two routes per frame, composited** | #788 §5.2(2): with one colour and one depth per pixel, independent sorts cannot order transparent splats across routes. It contradicts "one depth sort". |
| **Keep exclusivity and warn (#788 Option 1 only)** | Violates decision 1. A warning names the defect; it does not draw the scene. Option 1 stays useful as a diagnostic (GR-21). |
| **Hard-reject world + nodes (#788 Option 3)** | Rejected in #788 §5.3, and it contradicts decision 1. |
| **Proxy meshes as splat shadow casters** | Overruled by decision 6. |
| **Instances take everything first, the world gets the rest** | The world could be starved to nothing by objects. Decision 3 gives the world a floor and refuses the excess explicitly. |
| **#855 first for every route** | Waived for the HLOD route by decision 4: #855 concerns the chunk route's cull, not the HLOD path, and keeping the order would leave paged worlds blank with any node. |

---

## 6. Tests that pin today's exclusivity

They change honestly; none is weakened.

- `tests/runtime/test_hlod_resident_main_view.gd` adds and removes a transient `GaussianSplatNode3D`
  and expects a rebuild. It stays valid, because adding a node changes the asset set. Its comment
  calling the world "exclusive" changes, and a sibling case asserts that a **transform-only** change
  does not rebuild.
- `modules/gaussian_splatting/tests/test_scene_director_submission_scaffolding.h:2235` ("World submission produces identity
  instance in build_instance_buffer_for_renderer") pins the world-only row of `build_instance_buffer_for_renderer`. It is
  unchanged until F1.
- `qa_stream_multi_asset.tscn` stays quarantined (`tests/examples/godot/test_project/scripts/qa_test_runner.gd:78-80`)
  until F1. The HLOD PRs do not prove chunk-route coexistence.
- The reason keys `hlod_not_exclusive_world_submission`, `hlod_paged_not_exclusive_world_submission`,
  `hlod_exclusive_submission_lost` and `hlod_identity_shim_unavailable` are removed with their code
  paths in PR1b. The HLOD ADR lists the first one among the edit-route causes (§14.5.1); PR1b updates
  that list.

---

## 7. PR sequence

| # | PR | Class | Content | Evidence |
|---|---|---|---|---|
| PR0 | This ADR + amendments to the #788 and BV ADRs | R0 | Design record | Docs checks |
| **PR1a** | HLOD bundle becomes multi-row and multi-asset, with N = 0 instances | R2 | `hlod_bundle_bytes`, `hlod_build_bundle`, the prepared-contract check, the remap and the test observation hook take N rows and M assets. Exclusivity stays. | **Byte-identical output**: the existing HLOD tests pass unchanged; the bundle-bytes equality (`publisher.cpp:1330`) holds |
| **PR1b** | World + moving nodes on the HLOD route | R2 | Drop `instance_store.is_empty()` from the HLOD predicate (rename to `has_hlod_world_submission`); drop the single-row check; add the instance producer (§4.1–4.2), the generation split (§4.3), the idle rule (§4.4), the budget rule (§4.5), a per-frame `sort_dispatches_frame` counter and per-source drawn counts | CPU doctests (routing, rows, generations, ledger, budget split, offsets at 1 km and 100 km, draw list). A **C++ `[SceneTree][RequiresGPU]` doctest** with a V4 paged world and 8 moving instances in front of and behind it: `hlod_reason == "active"`, world and instance splat refs both > 0, one sort dispatch, pixel order verified, a background control; it **must be red on base**, proved by a rebuild and run on base. Missing stats fail, never skip. VRAM and frame time at 0 and 8 instances against base; a real-scan visual capture |
| **PR-auto** | Automatic HLOD route (decision 5) | R3 | §4.7, with a release note | Existing scenes before/after; release note |
| F1 | Canonical routes draw the world row with nodes; #855 root-caused | R2 | Always emit row 0 when a submission is renderable (`director.cpp:1744`, `:1967`, `:2114`); remove the primary-dense-id fallback (`render_streaming_orchestrator.cpp:160-164`); un-quarantine `qa_stream_multi_asset.tscn` | Runs in parallel with PR1a/b |
| F2 | Instances under HLOD | R2 (+R3 if the format gains instance-tree data) | Node assets with a baked tree become `SnapshotAsset`/`SnapshotInstance`; top-level instance tree (HLOD ADR §6.9) | |
| F3 | Per-object budget policy | R2/R3 | Per-instance caps, priorities, the floor as a setting, per-instance telemetry | |
| F4 | Picking | R2; R3 for the bound API | `SplatRef.instance_id` → row → `ObjectID`, per frame; a pick that returns a stable instance key | |
| F5 | Domains still refused on the HLOD route | R2 each | Effectors (`frame_anchor`), shadow instance filter, painterly, light views (with decision 6), extra views | |
| F6 | Runtime-profile scenario for world + moving instances | R3 | Registration in `tests/runtime/run_runtime_validation.py` | |

The splat shadow caster work (#1095, #1163) runs under decision 6 alongside this sequence; on the HLOD
route it needs the light-view producer of F5.

---

## 8. Game-rendering requirements GR-01..GR-21

Status vocabulary: **WORKS** (an existing gate or test exercises it; its latest result was not
checked here), **WORKS ALONE ONLY**, **PARTIAL**, **MISSING**. "Combined" means world + movable
objects + lights + shadows in one frame. **Today no requirement is WORKS in the combined scene.**

| ID | Requirement | Today | Key evidence | Path to WORKS |
|---|---|---|---|---|
| GR-01 | Static streamed world larger than VRAM (chunk and HLOD/V4) | PARTIAL | Never rendered above VRAM; HLOD pilot caps (`core/gaussian_splat_hlod_runtime_data.h:14-18`), opt-in (`lod/lod_config.cpp:400`), exclusive (§1.2) | BV program + this ADR |
| GR-02 | Movable objects in the same depth sort as the world | WORKS ALONE ONLY | Breaks with a world on every route (§1) | PR1a/b, F1 |
| GR-03 | Instancing and memory sharing | PARTIAL | Atlas packs each asset once; amortised SH cache shared across instances (#1245) | #1245, node-count benchmark |
| GR-04 | Spawn/despawn and asset swaps without hitches | PARTIAL | Transform churn avoids a repack; a new asset repacks the whole atlas; any spawn blanks a paged world today | §4.3; incremental atlas (BV segmented pool) |
| GR-05 | Picking/raycast against objects and the world | MISSING | `InteractiveStateManager::pick()` returns an empty result (`interfaces/interactive_state_manager.cpp:169-178`); node AABB not bound (#1024) | F4 |
| GR-06 | Collision for objects and the world | MISSING | No physics use in module runtime code | Collider bake + streaming (#1193) |
| GR-07 | Animation/deformation of objects | PARTIAL (rigid, alone) | Rigid + uniform scale; wind/effectors refuse HLOD (`publisher.cpp:1517-1524`) | F5; per-producer deformation flags |
| GR-08 | Dynamic lights on splats | PARTIAL | Shared lighting applier; light caps clamp silently; no lit gate | Light-cap counter; lit gate |
| GR-09 | Splat shadows, cast and received | receive PARTIAL, cast MISSING | "The shadow raster currently rasterizes no splats" (`docs/architecture/lighting-system.md:101`); fail-closed `NO_CASTER_RASTERED` (`renderer/gaussian_splat_renderer.cpp:2071-2089`); HLOD refuses non-main passes (`publisher.cpp:1923-1926`) | Decision 6: real casters (#1095, #1163), HLOD light-view producer |
| GR-10 | Mesh↔splat compositing | WORKS ALONE ONLY | Per-splat scene-depth clip gated with one node; no splat depth write-back (#1025) | Depth write-back |
| GR-11 | Transparency, TAA/FSR, post-processing | PARTIAL | No splat motion vectors (#1025); `transparent_bg` under TAA/FSR2 (#989); static-camera instability (#1240) | Motion vectors, #1240 |
| GR-12 | LOD and streaming for objects and world under one ledger | MISSING | Objects resident-only since #232; HLOD has its own byte limit; no device ledger yet | §4.5; BV D-11 widened |
| GR-13 | Editor workflow | PARTIAL | Same coexistence break in the editor preview; no warning (#788 Option 1 not implemented) | #788 Option 1, PR1b, F4 |
| GR-14 | Export | PARTIAL | Windows template published; Linux template not in the release list; GSQ design only | BV GSQ chain; export smoke |
| GR-15 | Frame targets in the combined scene (3080 p95 ≤ 25 ms / p99 ≤ 33.3 ms; 3090 p95 ≤ 16.7 ms / p99 ≤ 20 ms) | MISSING | Never measured combined; the combination does not render | GR-18, then BV P4 measured in GS-COMBINED-1 |
| GR-16 | Multi-view (split screen, second camera, XR) | PARTIAL | HLOD refuses `view_count != 1` and non-main passes (`publisher.cpp:1923-1926`); XR composite fails (#1160) | Per-view producers under one ledger |
| GR-17 | Several worlds per scenario | MISSING | A second live world loses arbitration (`director.cpp:2444`) | Out of scope here (BV ADR §1.3) |
| GR-18 | Frame-coherent measurement | MISSING | Pass values up to 120 frames old count as valid (`renderer/tile_renderer.cpp:2999`); `frame_gpu_timing_valid = frame_total > 0` (`:3026`); #530 | BV-04b widened |
| GR-19 | Temporal stability | PARTIAL | #1240, #1131 | BV-25 |
| GR-20 | Scene switch and teardown without stalls | PARTIAL | Destructor dispatch can block (#588) | #588 |
| GR-21 | No silent source loss | MISSING | The world row's absence produces no reason (`director.cpp:1744`); the resident skip is a `WARN_PRINT_ONCE` (`renderer/gaussian_splat_renderer.cpp:2669`) | #788 Option 1; per-source `dropped_sources` counter |

---

## 9. GS-COMBINED-1 hard gate

GS-COMBINED-1 is the scene a game uses. It is the acceptance scene for decision 1. Its harness and
lane are **R3** (gate machinery and workflow) and land as their own PR; this section is the spec.

### 9.1 Lane

- It runs on PR, push and schedule, split into **two jobs** so that the required status and the
  runner trust boundary both hold:
  - **The trusted GPU execution job** runs on the 3080 runner and carries the per-job fork guard
    (`github.event_name != 'pull_request' || head.repo.full_name == github.repository`). Untrusted
    fork code never runs on the persistent self-hosted runner (`docs/governance/review-policy.md:59-66`,
    the same guard as `.github/workflows/release_ci_runtime.yml:43`). A fork PR is validated only
    after a maintainer moves it to a same-repository branch.
  - **The required status job** runs on a hosted runner. It `needs:` the execution job and has
    the job-level condition **`if: ${{ always() }}`** and no other. A job with `needs:` is skipped
    when its dependency is skipped or fails, unless its job-level condition overrides that, and a
    skipped job would satisfy a required check. A step-level `if: always()` cannot start a skipped
    job. `always()` is the only job-level condition allowed here, because it can never skip the
    job. Inside the job, a step reads `needs.<execution job>.result` and is fail-closed. It passes
    only if that result is `success` on this head. If the execution job is skipped, as for a fork PR, the status
    fails with the reason `gs_combined_requires_trusted_branch`. If it is cancelled or fails, the
    status fails too.
- Optimized build with tests enabled.
- **Variant A:** a chunk-streamed plain world. **Variant B:** the same content as a V4 paged HLOD world.
  Both run on every trigger, pilot scale first.
- Deterministic: fixed seed, fixed camera spline, 600 frames, fixed timestep, pinned settings file,
  cold cache for variant B.

### 9.2 Content

- The world (≥ 1 km², SH3), from the BV generator.
- 32 `GaussianSplatNode3D` (4 assets × 8 instances) on scripted paths, at least 8 interpenetrating world
  geometry, each with 16 tracer splats at known positions.
- One designated picked object under a scripted cursor ray.
- One skinned mesh character in front of, inside and behind world foliage.
- One directional light with shadows and 4 omni lights with shadows.
- Shadow receivers: a mesh floor under a splat object, splat ground under a mesh, a splat object over the
  splat world.

### 9.3 Assertions (every frame unless stated)

**Fail-closed rule.** Every counter is **present and fresh** (frame serial = current frame) in the
stats. A missing key, a stale serial, a NaN or an empty frame list fails; it is never a skip. The harness
asserts that it ran 600 frames and that each assertion family evaluated on ≥ 590 frames, so a run that
executes nothing fails.

| ID | Family | Assertion |
|---|---|---|
| A1 | Sources | `world_drawn_splats > 0`, and `object_drawn_splats[i] > 0` for every object in the CPU-computed frustum mask |
| A2 | One sort | World and object splats enter one overlap sort: one sort dispatch per view, `sort_input_sources ⊇ {world, objects}`, no second splat composite pass |
| A3 | Route | `route_skip_reason == "none"`; `hlod_reason == "active"` on B; `dropped_sources == 0`; no log line matching `frame skipped`, `not_exclusive` or `identity_shim` |
| A4 | Interpenetration | 8 pixel probes where world and object splats alternate in depth match a CPU-sorted reference within 3/255 |
| A5 | Transforms | Tracer centres within 0.5 px of the CPU projection |
| A6 | Picking | `pick` returns the target whenever it is under the cursor; `partial` only for non-resident regions |
| C1–C3 | Compositing | Character mask in front, inside and behind foliage against a depth-prepass reference |
| L1–L3 | Lights | Each light's ROI luminance follows its intensity ramp (Pearson r ≥ 0.95); `lights_dropped == 0` |
| S1–S4 | Shadows | `shadow_casters_rastered[light] > 0` for every shadowed light every frame; receiver ROI luminance ratio ≤ 0.6 in the three setups; `NO_CASTER_RASTERED == 0` |
| B1 | Budget | Ledger sum equals the device allocation within 2 %, stays within budget; `silent_truncation_frames == 0` |
| T1 | Stability | A 120-frame **frozen-scene** segment with TAA off. Every time-dependent input is frozen for the segment: the camera, object transforms and scripted paths, character animation, light intensities and positions, wind and effectors, and the scene time handed to shaders. Pass: 0 frames differ from the segment's first frame. The harness asserts that the inputs really were frozen (their per-frame digests are equal); a segment whose inputs changed is INVALID, not a pass |
| P1–P3 | Performance | Frame-coherent p95/p99 against GR-15. Report-only until the BV calibration, but with a **hard** validity check that timing is complete for ≥ 99 % of frames |
| V1 | Multi-view (variant job) | Split screen: A1–A4 per half |

### 9.4 Forcing controls (each must make the gate fail, in the same job)

1. Restore `instance_store.is_empty()` exclusivity → A1 or A3 fails.
2. Drop the objects from the sort input (debug flag) → A2 or A4 fails.
3. Disable the shadow caster pass → S1 fails.
4. Delete the stats key `object_drawn_splats` → the fail-closed rule fails.
5. Freeze one pass timestamp for one frame → the P-family validity check fails.
6. Make `pick` return no hit → A6 fails.

A family whose subject is not built yet (for example picking before F4) **fails**; it does not skip.
Until every family passes, GS-COMBINED-1 reports which requirements are not met. That is the honest
status of decision 1.

---

## 10. Consequences

- The #788 ADR's Option 2 is no longer deferred for the HLOD route; its §5.2 constraints are answered
  here: (1) the single-value route API is kept and the HLOD route carries all producers; (2) compositing
  is solved by one sort, not by composite ordering; (3) #855 first is waived for HLOD only.
- The BV ADR no longer lists instanced worlds as a non-goal; its producer set, ledger, milestones and
  invariants are amended to match (dated note in its "User decisions" section).
- Every new precondition of the form "only with an exclusive world, one instance or no other nodes" is
  a blocker for maintainer review before it is built (decision 1).

## 11. What is not verified here

- Which side vanishes on the chunk route in a world + node scene. Code reading says the world row is
  missing; #788 §3.2 says the instance side. One GPU capture with different fixtures on both sides
  settles it (F1).
- Lighting and shadow receive on the HLOD route.
- Whether HLOD enforces the uint16 quantized chunk-id limit once instance chunks are appended.
- The cost of a whole-atlas repack on asset spawn (O(total atlas) by code; not measured).

---

## 12. Route retirement decision (D-2)

- **Status:** Accepted — decided by the owner on 2026-11-10. The owner's approving review and merge of the PR that adds this section confirm that the recorded text matches the decision.
- **Refs:** decision D-2 of the 2026-11 codebase audit; findings I-02, I-03, D-01, D-06, D-07
  and J-19.
- Claims in this section were re-read at `b58bbe35d1f`.

### 12.1 Problem

Sections 1 to 11 decide that the world and node instances share one draw list on the HLOD
route. They do not decide whether the other routes stay. Today a frame can take one of these
paths:

- the **resident canonical** route;
- the **chunk-streaming** route (`renderer/render_streaming_orchestrator.cpp`,
  `core/gaussian_streaming.cpp`, `core/streaming_global_atlas_registry.cpp`), the project
  default under `rendering/gaussian_splatting/streaming/route_policy`;
- the **HLOD** route, for a world with an HLOD snapshot when no refusal applies;
- for compositing, the pre-upscale hook and the **legacy post-scene composite**
  (`interfaces/output_compositor.cpp`, used for forward mobile, multiview and probes).

The HLOD route refuses a setup through `GaussianSplatRenderer::hlod_streaming_unsupported_reason()`
(in `renderer/resident_instance_contract_publisher.cpp`), whose keys
`lod/hlod_auto_route.h` publishes. A refused setup falls back to another route. So every
feature lands on three to four routes, and each route drifts: chunk-route LOD draws a prefix
and does not reduce VRAM (D-01, D-06); a rotated or translated world loses HLOD (D-07); one
wind or effector setting moves a streamed world off HLOD (I-02); splat shadows exist on the
HLOD route only (I-03); the legacy composite is the path that dropped splats for scaled
viewports (J-19).

### 12.2 Options

- **Option A: one route.** The HLOD streaming route, with the resident case as a degenerate
  cut and a single `TileRenderParams` producer (#788 Option 2, as adopted in §3).
  - Cost: finish the migration; delete the chunk route and the legacy composite.
  - Enables: feature parity by construction; one place to test.
  - Limits: every refusal in `hlod_streaming_unsupported_reason()` must be lifted before the
    fallback it protects can be deleted.
- **Option B: keep the routes and require parity.** Every feature PR covers all routes, with
  a parity test.
  - Cost: feature work multiplied on every PR, for the life of the project.
  - Enables: no route is deleted; existing setups keep their path.
  - Limits: parity rests on a test every PR must extend; drift returns where it does not.

**Audit recommendation: A.** The refusal list in `hlod_streaming_unsupported_reason()` is
the work list.

## Owner decision (D-2)

- **Option chosen:** **A, one route.** HLOD streaming is the product route, with the
  resident case as a degenerate cut and a single `TileRenderParams` producer. The
  chunk-streaming route and the legacy composite are deleted.
- **Decided by:** the owner, 2026-11-10.
- **Recorded in:** the owner's answer in the 2026-11-10 session, recorded in this section.
  The owner's approving review and merge of the pull request that adds this section confirm
  that the recorded text matches the decision. Sections 1 to 11 were accepted separately
  (maintainer decisions, 2026-11-04) and are not affected.
- **Rejected:** B. It keeps every existing path working without a migration, at the cost of
  multiplying every feature by the number of routes.

### 12.3 Work list and order

This record schedules no code. The tasks below each cite this section.

1. **Refusal table** (CLN-RT01-1). Each refusal key gets an owning task, and a test derives
   the key set from source so the table cannot drift. At `b58bbe35d1f` the keys of
   `hlod_streaming_unsupported_reason()` are `lod_disabled_canonical_leaves`,
   `shadow_instance_filter`, `unsupported_view_or_material` (painterly),
   `deformation_or_instance_domain` (wind and sphere effectors) and
   `unsupported_instance_transform_domain`. `GaussianSplatWorld3D::_hlod_auto_route_refusal()`
   and `plan_world()` add world-level keys, including `hlod_world_transform_unsupported`.
2. **One `TileRenderParams` producer** (CLN-RT02, I-07).
3. **Lift the refusals** (CLN-RT03, I-02, D-07): rigid world transform (CLN-RT03-1), wind
   (CLN-RT03-2), sphere effectors (CLN-RT03-3), painterly (CLN-RT03-4). The keys without a
   task at this date (`lod_disabled_canonical_leaves`, `shadow_instance_filter`) get one in
   step 1.
4. **Retire the chunk-streaming route** (CLN-RT07, takes D-01 and D-06), after step 2 and the
   streaming evidence task CLN-EV03.
5. **Retire the legacy post-scene composite** (CLN-RT05, J-19), after the platform decision
   D-3.

Splat shadows on the one route (I-03) are the shadow track, CLN-LT02.

### 12.4 Consequences

- CLN-RT04 (chunk-route LOD fix) and its children are void: they assumed the chunk route
  survives. Their findings D-01 and D-06 move to CLN-RT07.
- §3 is unchanged. Decision 4 schedules a parallel fix of #855 on the chunk route (F1 in §7).
  Under this decision the chunk route is deleted, so that fix and the retirement now overlap;
  the owner rules on F1's chunk-route part.
- The [Worlds beyond VRAM](adr-beyond-vram-worlds.md) ADR (Proposed) uses the chunk route as
  the flat arm for tree-less worlds. The beyond-VRAM program continues in parallel (owner
  decision D-8); how tree-less worlds reach the one route is not decided here.
- The [#788 ADR](adr-single-route-per-frame-node-coexistence.md) carries a status line that
  points here.

### 12.5 Out of scope

- Deleting or migrating any route: the tasks in §12.3 do that and carry their own evidence.
- How the resident case becomes a degenerate cut, and when the resident canonical code is
  removed. The owner's answer names two deletions, the chunk route and the legacy composite.
- Scheduling CLN-RT07 against open beyond-VRAM PRs. Under owner decision D-8, a task that owns a
  path an open beyond-VRAM PR holds is not dispatched.
