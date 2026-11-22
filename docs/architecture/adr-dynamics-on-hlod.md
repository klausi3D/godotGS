# ADR: Dynamics on HLOD streaming (static world tree, per-splat semantic word, LOD-stable wind, region overrides)

- **Status:** **Accepted (owner decisions, 2026-11-20).** The owner approved the *direction* on
  2026-11-19 ("without it the large-world architecture makes no sense for games"). On
  2026-11-20 the owner accepted the recommended option of every open decision (O1–O5, O7–O10);
  O6 stays open with an interim rule. The decisions are recorded in §11, and the program
  decision with its kill criteria in §0. D1–D8 (§5) are accepted. The design detail marked
  **[P]** is what the work packages implement; each package carries its own evidence.
  Acceptance of this record does not establish implementation or runtime acceptance.
- **Date:** 2026-11-20 (drafted 2026-11-19).
- **Risk class:** the file is R0 (`docs/**`). It is the **R3 design record** for the work it
  describes. That work changes the persisted `Gaussian` layout (`GAUSSIAN_STRUCT_LAYOUT_VERSION`),
  the GSQ page codec, the HLOD merge key, the renderer and its shaders, and the public node API.
- **Builds on:** [adr-hlod-streaming.md](adr-hlod-streaming.md) (§6.5, §6.9, §14.1, §14.4.1,
  §14.5), [adr-one-route-many-producers.md](adr-one-route-many-producers.md) (§4.5, §4.6, §8
  GR-02/03/07/09/15, §9 GS-COMBINED-1) and [adr-v1-animation-collision-scope.md](adr-v1-animation-collision-scope.md)
  (D-6a: animation is kept and wired into rendering, through CLN-SH06; D-6b: collision proxies are
  baked, through CLN-SH04).
- **Tracking:** Refs #1499 (shader identity keyed by atlas address), #1245 (SH cache shared
  across instances), #1424 (omni and spot lights cast no splat shadows on HLOD), #1473 (asset-set
  change resets world residency), #1500 (pre-M3 batch), #1312 (picking stub), #1502 (SH colour
  cache serves the previous tenant's colour after atlas page reuse; WP1), #487.
- **Inputs:** an internal design check and deviation report (2026-11-19). They are not
  repository documents. Figures taken from them are marked as such.
- **Source baseline:** `origin/master` = **`265951b5ca2`**. Every `file:line` below was read at that
  commit. Paths are relative to `modules/gaussian_splatting/` unless they start with `docs/`,
  `tests/` or `.github/`.
- **Line drift at landing:** this record lands on `9faecfb155a` (#1500). Between the two commits,
  `shaders/`, `compute/`, `lod/`, `nodes/`, `io/gsq/`, `core/gaussian_splat_hlod_merge.cpp` and
  `core/quality_tier_config.cpp` are unchanged, so those citations hold. #1500 changed
  `renderer/resident_instance_contract_publisher.cpp` (`publisher.cpp`),
  `renderer/tile_renderer.cpp` and `core/gaussian_streaming.h`. Citations into those three files
  can be off by up to a few hundred lines on `9faecfb155a`. For example, the wind/effector refusal
  is at `publisher.cpp:2064` and `request_sh_amortization` at `publisher.cpp:2778`; the
  `sh_cache_needs_full_update` sites in `tile_renderer.cpp` are at `:605` and `:4165`.

**Claim markers** (every factual claim carries one):

| Marker | Meaning |
| --- | --- |
| **[V]** | Verified: read in code at the cited `file:line` on the baseline. |
| **[ADR]** | Stated in an accepted ADR or in a repository doc, not re-checked in code here. |
| **[I]** | Inferred: follows from verified facts but was not measured or run. |
| **[P]** | Proposed: a decision or design of this ADR. |
| **[Ext]** | External practice (papers, engines). Not re-verified here. |

---

## 0. Program decision (owner, 2026-11-20)

- **HLOD is the route.** Dynamics are built on the HLOD route described here. There is no return
  to the chunk-streaming route; route retirement stays as decided in
  [one-route §12 (D-2)](adr-one-route-many-producers.md).
- **Kill criteria.** The architecture is re-opened if any of these is met:
  1. Tier A does not settle on HLOD.
  2. Frame p95 on the 3080 is above 25 ms, and profiling cannot explain it.
  3. GS-GAME-1 (§7) cannot be reached with this architecture.
- A kill criterion is met only by a measured result with its evidence. A run that did not
  happen, or that is INVALID, meets none of them and passes none of them.

## 1. Problem

The HLOD route is now the one route for streamed worlds ([one-route §12](adr-one-route-many-producers.md)).
It draws the world and the node instances in one draw list and one sort [ADR], and it never
draws an unsafe hole [ADR]. But for a game it is static.

1. **Wind or one effector un-streams the whole world.**
   - A V4 paged world is materialised into memory and handed to the resident route when project
     wind is on or the project has a sphere effector: `nodes/gaussian_splat_world_3d.cpp:722-723`
     and `:728-729` set `hlod_paged_deformation_or_instance_domain`, and `:737` calls
     `materialize_paged_resident` [V].
   - The same project-global test refuses the HLOD route in `_hlod_auto_route_refusal()`
     (`nodes/gaussian_splat_world_3d.cpp:646`, `:657`) and in
     `GaussianSplatRenderer::hlod_streaming_unsupported_reason()`
     (`renderer/resident_instance_contract_publisher.cpp:1867-1869`) [V].
   - One waving bush is therefore enough to load a 55 GiB world into memory. For Tier A this
     means wind is impossible [I].
2. **Wind is not a game wind model.**
   - Stiffness is derived from opacity: "Reuse opacity as a rough 'stiffness' signal until a
     dedicated attribute exists" (`shaders/includes/gs_deformation.glsl:238-240`) [V].
   - The phase gets a random offset of 0 to 2π per splat (`gs_deformation.glsl:242-245`) [V].
   - The seed of that offset is the **atlas address**: `atlas_index ^ instance_id * 0x9e3779b9`
     (`compute/depth_compute.glsl:201`) and `gaussian_idx ^ ...` (`shaders/tile_binning.glsl:992`),
     where `gaussian_idx` is the atlas index (`tile_binning.glsl:896`) [V].
   - The splat shadow caster applies no deformation: `shaders/gs_shadow_caster.glsl` has no match
     for `wind|deformation|effector` [V].
   - Nothing inflates a bound for the displacement: a search for a wind margin or inflation in the
     module finds nothing [V, search not exhaustive].
3. **Splats carry no semantics.**
   - The merge key is the cell coordinate plus the DC-encoding bit, and nothing else
     (`core/gaussian_splat_hlod_merge.cpp:238-239`) [V].
   - Every other field of a merged splat is copied from the heaviest member: `r_out = p_rep.gaussians[best]`
     (`merge.cpp:467`) [V].
   - The leaf's source index exists only during the bake (`core/gaussian_splat_hlod_bake.cpp:564`,
     `:606`) and is dropped (`core/gaussian_splat_world.cpp:1051`) [V].
   - There is therefore no runtime way to say "this house", "this tree" or "this bush waves".
4. **No runtime recolour survives LOD and streaming.**
   - The highlight is one global uniform per renderer (`shaders/includes/tile_raster_common.glsl:561-563`) [V].
   - The per-instance row has opacity, wind and effector fields but no colour
     (`shaders/includes/gs_instance_layout.glsl:29-38`) [V].
   - Picking is a stub that returns no hit (`interfaces/interactive_state_manager.cpp:169-177`) [V].
   - F1 edits replace at most 64 leaves (`lod/hlod_edit.h:48`), and the ancestors of an edited
     leaf go stale (`lod/hlod_edit.h:7-12`) [V]. A recolour through F1 therefore forces full detail
     and does not scale over distance [I].
5. **Shader state keyed by atlas address breaks under streaming.**
   - The wind seed, the sort tie-break (`compute/depth_compute.glsl:271`), the subpixel history
     (`shaders/tile_binning.glsl:1182-1190`) and the **SH colour cache**
     (`shaders/tile_binning.glsl:1445`) all index by atlas address [V].
   - Atlas pages are reused: `lod/hlod_node_residency.cpp:110` and `:232` release slots, and
     `:694` and `:874` allocate them to the next node through the best-fit allocator
     (`core/streaming_atlas.cpp:66-88`) [V].
   - The SH cache case is a **live defect on master** under SH amortization (§6.5). It is not only
     a future relocation hazard as #1499 states.

This ADR decides how dynamics live on top of the HLOD design **without** changing the cut, the
residency or the V4 page structure.

## 2. Decision drivers

1. **Game first** (owner, 2026-11-04): streamed world, static, dynamic and interactive objects,
   and lighting in **one** frame. No feature may silently make the world exclusive or resident.
2. **Every dynamic effect works at every LOD and survives eviction, readmission and compaction.**
   An effect that depends on which atlas page a splat sits on is a defect.
3. **Re-bake at most once.** A Tier A bake is 24,290 nodes and 9.87 GB (run 43288178399, from the
   deviation report). Changes to the codec or the bake sources invalidate the world cache
   (`tests/runtime/flythrough/world_cache.py:84-103` lists `io/gsq/gsq_codec.*` among the
   generator sources; `:105-120` lists the bake and auto-tree sources as tree sources) [V].
   Every format-relevant change therefore goes into **one** format step.
4. **Fail closed, never drop silently.** GSQ v1 already refuses heterogeneous page constants
   (`io/gsq/gsq_codec.cpp:436-437`) [V]. A new per-splat field must be carried or refused, never
   zeroed.
5. **Evidence integrity.** Every acceptance row needs a control that fails on the old behaviour
   in the same job (`docs/governance/evidence-integrity.md`) [ADR].

## 3. Current state reused unchanged

| Part | Status | Evidence |
| --- | --- | --- |
| Budgeted cut, "coarser rather than a hole", V4 pages, residency, ledger | kept as is | adr-hlod-streaming §6, §14 [ADR] |
| One draw list and one sort for the world and instances; instance tail at the end of the atlas | kept | `renderer/resident_instance_contract_publisher.cpp:1403-1418` [V] |
| A transform change re-uploads rows only | kept | `publisher.cpp:1748-1750` [V] |
| Per-instance colour grading applied **after** the SH cache | the pattern the override table copies | `shaders/tile_binning.glsl:1468-1469`, `InstanceGradingGPU` at `gs_instance_layout.glsl:134-140` [V] |
| A wind time sampled once per frame and shared by every pass | kept; the caster joins it | `core/gs_project_settings.h:274-285` [V] |
| F1 leaf edits for local destruction | kept; F2 rebake stays open | `lod/hlod_edit.h:4-19` [V] |

### 3.1 Free bits (verified)

| Where | Field | Size | State |
| --- | --- | --- | --- |
| GPU raw record `PackedGaussian` (176 B) | `_pad_rotation_align` @28 | 32 bit | Written as `0u` (`renderer/gaussian_gpu_layout.cpp:236`). Read by no shader; mirrored in `compute/depth_compute.glsl:28`, `shaders/tile_binning.glsl:63`, `shaders/tile_rasterizer*.glsl`, `shaders/gs_shadow_caster.glsl:22` [V]. |
| GPU quantized record `PackedGaussianQuantized` (160 B) | `_tail_padding[2]` @152 | 2 × 32 bit | Zeroed (`gaussian_gpu_layout.cpp:494-495`) [V]. `_pre_sh_padding` @30 is a further 16 bits (`gaussian_gpu_layout.h:403`) [V]. |
| Persisted authoring `Gaussian` (144 B) | `_padding` @116 (float), `_padding2[2]` @136 | 4 B + 8 B | Pinned by `GS_PIN_GAUSSIAN_FIELD` (`core/gaussian_data.h:244`, `:248`). Layout version 1 (`:216`) [V]. |
| V4 payload record | `count * 144` Gaussian bytes | — | Carries the authoring struct as is [ADR §14.4.1]. |
| GSQ v1 page (the Tier A codec) | none per splat | — | It encodes position, scale, rotation, opacity, DC, SH and normals as planes. `painterly_meta`/`render_meta` are **page constants** (`io/gsq/gsq_codec.h:28`), and a page whose splats differ in them is refused (`gsq_codec.cpp:361-366`, `:436-437`). `_padding`/`_padding2` are not encoded, and the decoded digest excludes padding (`gsq_codec.h:121-123`) [V]. |
| Instance row `InstanceDataGPU` | `lod.y` "reserved" | 32 bit | `shaders/includes/gs_instance_layout.glsl:35` [V]. |

**Consequence [I]:** the GPU side has a free 32-bit word per splat on both routes. The *persisted*
side does not: a word written into `Gaussian::_padding` today would survive a v3/V4-NONE file, but
GSQ would silently drop it. A codec step is unavoidable.

## 4. Options

| Option | Summary | Verdict |
| --- | --- | --- |
| **A. Static tree plus a semantic word plus instances** | The world tree is static. Each splat carries a 32-bit semantic word (region ID, wind weight). Wind and effectors are functions of rest position and the word. Recolour goes through a region-indexed override table. Moving or deforming content is extracted into instances at bake time. | **Chosen [P]** |
| B. Stable per-splat identity in the shader (`NodeKey`, index) and per-splat override buffers | Gives every splat an identity, plus side buffers keyed by it. | Rejected: merged splats have no identity, so the effects would not hold across LOD. It also needs dense identity-keyed side buffers for billions of splats. Kept only for the sort tie-break (§6.5). |
| C. Dynamic content in the tree, with F1/F2 rebake on change | Moving or recoloured content stays in the world tree and is rebaked when it changes. | Rejected: every change forces full detail and an upload, scales with the edit and not with the screen, and F2 does not exist [V `lod/hlod_edit.h:11-12`]. |
| D. Keep wind on the resident route only | Wind stays where it works today. | Rejected: it contradicts driver 1. A Tier A world cannot be resident [I]. |

Practice elsewhere [Ext]: H3DGS and Octree-GS hierarchies are static. SC-GS drives motion as a
field over a few control nodes, evaluated at the splat position. Nanite allows world-position
offset (WPO) only with a declared *Max WPO Displacement* for cluster bounds, and Fortnite turns
WPO off with distance. SpeedTree bakes wind weights at every LOD level. Unreal's World Partition
HLOD covers static actors only. Every one of them separates bundled static content from movable
content, and makes world dynamics a function of position plus a baked attribute with a declared
bound. Option A does the same.

---

## 5. Decision overview (accepted 2026-11-20)

The owner accepted D1–D8 with the decisions in §11.

| # | Decision | Section |
| --- | --- | --- |
| D1 | **Authoring rule:** the baked world tree is static. Anything that moves, deforms or is changed interactively *per object* is an instance. Dynamic islands are extracted at bake time. | §6.1 |
| D2 | **Semantic word:** 32 bits per splat (region ID, wind weight, reserved bits, a valid bit), persisted through layout version 2 and GSQ v2, uploaded into the existing GPU padding. The merge key never mixes regions. Wind weight is aggregated like colour. | §6.2 |
| D3 | **Wind model:** phase from rest position only (no identity jitter), baked wind weight instead of opacity, bound inflation from a declared max displacement, the caster deforms with the same time sample, a distance fade, and per-row opt-in instead of the project-global refusal. Delivered as an **S–M step without a format bump** first, then the format step. | §6.3 |
| D4 | **Region overrides:** a GPU table indexed by region ID (tint, emissive, opacity multiplier, flags), applied at shading time, in the depth pass for hide, and in the caster. A public API on world and instance nodes. | §6.4 |
| D5 | **Atlas-keyed state:** content-following caches (SH colour, subpixel history) are invalidated whenever an atlas run is (re)assigned. Identity values (wind phase, tie-break) no longer use the atlas address. This splits #1499 item 1. | §6.5 |
| D6 | **Rigid objects:** the shadow uses the current frame's rows (fixes the one-frame lag). A4/A5 are triaged before tolerances are set. Object LOD and proxies are out of scope for the first gate. | §6.6 |
| D7 | **Skinning and deformation** are out of scope, and go through the instance route in the CLN-SH06 animation render ADR. | §6.7 |
| D8 | **GS-GAME-1** is the vertical-slice gate for "game alpha". | §7 |

---

## 6. Design

### 6.1 Authoring rule: world versus instance (D1)

**Rule [P]:** *The baked world tree is static. Anything that moves, deforms or is interactively
changed per object is an instance.*

| Content | Where it lives | Why |
| --- | --- | --- |
| Terrain, buildings, rocks, the static parts of vegetation | world tree | static geometry; cut and streaming apply |
| Vegetation that **sways in place** (grass, leaves, branches) | world tree, with a baked wind weight | the displacement is a bounded function of rest position and the word (§6.3); no identity needed |
| Regions recoloured, highlighted, hidden or made emissive *as a whole* (a house turning red, a lit window, a quest marker) | world tree, with a region ID | the override table (§6.4) works at every LOD |
| Local destruction of up to 64 leaves within leaf bounds | world tree plus an F1 edit | `lod/hlod_edit.h:14-19` [V] |
| Doors, vehicles, props, pickups, anything with a transform that changes | **instance** (`GaussianSplatNode3D`) | rigid transform per row; fully resident in the instance tail (`publisher.cpp:1403-1418`) [V] |
| Falling trees, destructible houses, characters | **instance**, plus a later deformation path | §6.7 |
| Large continuous changes (a flood, seasonal repaint) | **out of scope** | would need a rebake (F2) or content swaps |

**Bake-time extraction of dynamic islands [P]:**

1. Authoring marks islands with volumes (`GaussianSplatDynamicIslandVolume3D`, a box or convex
   hull plus an optional asset name) or with a per-splat source property (`island_id` in PLY).
2. `bake_hlod()` and the auto-tree (D2-3 driver) remove the island splats **before** the leaf
   partition. Each island is written as its own asset (plain v3, or V4 when large) with an
   `islands[]` manifest in the world metadata: name, asset path, rest transform, bounds.
3. A helper places one `GaussianSplatNode3D` per island at its rest transform. The game then
   moves, hides or swaps it like any other instance.
4. **Hole policy:** a scan has no data behind an extracted object. The bake reports the area
   uncovered by the extraction, and authoring decides whether to fill it, which is a
   content-pipeline task and not engine work [I].
5. Islands count against the instance tail and the shared ledger (one-route §4.5) [ADR]. Many
   islands need instance LOD and proxies (§6.6.3).

### 6.2 Per-splat semantic word (D2)

#### 6.2.1 Layout [P]

One `uint32` per splat, the same bits everywhere it is stored:

| Bits | Field | Meaning |
| --- | --- | --- |
| 0–15 | `region_id` | 0 = default region of the asset. 1–65,535 = authored regions. The scope is per asset, so world and object assets each have their own space. |
| 16–23 | `wind_weight` | unorm8: 0 = rigid, 255 = full sway. |
| 24–27 | `wind_class` | Reserved for a flutter or frequency band (trunk, branch, leaf). Written as 0 now; readers ignore it. |
| 28–30 | reserved | Must be 0. Readers refuse non-zero (a future meaning must not be misread). |
| 31 | `semantic_valid` | 1 = baked semantic data. 0 = legacy content: region 0, and wind falls back to the row weight (§6.3). |

Why 16 region bits: an override table with 65,536 entries × 32 B = 2 MiB is affordable
resident [I]. 20 bits would cost 32 MiB per world. Decided: 16 bits (O3).

**Where it is stored [P]:**

| Store | Location | Change |
| --- | --- | --- |
| Authoring `Gaussian` | `_padding` @116 becomes `uint32_t semantic` | `GAUSSIAN_STRUCT_LAYOUT_VERSION` 1 → 2, pin assert updated (`core/gaussian_data.h:216`, `:244`). Layout-1 files keep loading, and their word is **forced to 0**, not read: before #687 the padding was not reliably zero (comment at `gaussian_data.h:150-158`) [V]. |
| GSQ | **GSQ v2** (codec id 2): adds 4 byte planes of `semantic` per sub-block. The planes are spatially coherent, so zstd should compress them to near nothing [I, to be measured]. | v1 stays readable, with the word = 0. |
| V4 | unchanged container; the payload record already names the struct layout [ADR §14.4.1] | The node record gets `max_wind_weight` (u8) in a reserved word [ADR: "reserved words must be zero"; the exact word is not verified in code here]. |
| GPU raw | `PackedGaussian._pad_rotation_align` | `pack_gaussian()` writes `src.semantic` instead of `0u` (`gaussian_gpu_layout.cpp:236`). The upload checksum already hashes the word (comment `:232-235`) [V]. |
| GPU quantized | `PackedGaussianQuantized._tail_padding[0]` | Same bits. |

#### 6.2.2 Merge rules [P]

1. **Never merge across region IDs.**
   - The 64-bit sort key is full: three 21-bit cell coordinates plus bit 63 for the DC encoding
     (`merge.cpp:20`, `:239`) [V], so the region ID cannot be packed into it.
   - Each sort entry instead gets a second field (`region_id`), compared **after** the cell key.
     For single-region content the order, and therefore the merge output, stays bit-identical to
     today except for the word itself [I; the test must show it].
2. **The region ID is inherited exactly.** Cells are region-pure, so `r_out = gaussians[best]`
   (`merge.cpp:467`) already copies the correct ID [V for the copy; I for purity under rule 1].
3. **The wind weight is a weighted mean.** The merge does not copy it from the winner. It uses
   the moment weights the merge already uses for colour, `w = opacity × s_max × s_mid`
   (`merge.cpp:368`) [V], and the result is rounded to the nearest unorm8.
4. **`semantic_valid` is the AND of the members.** `wind_class` comes from the member with the
   largest summed weight.
5. **The node gets `max_wind_weight` = max over its payload** (not over its subtree). It sets the
   node's displacement bound (§6.3.3).
6. **Interior nodes' compression cost:**
   - Region boundaries merge less, so interior nodes grow. The cost is not measured.
   - Estimated single-digit percent for hundreds of regions [I, design check §1.2].
   - It must be measured on T0 with the existing §5.2 setup before the Tier A rebake (acceptance
     row F3).

#### 6.2.3 Persistence and the timing constraint [P]

- **One format step** (WP3, §8) carries all of: layout v2, GSQ v2, the merge key, the wind-weight
  aggregation, `max_wind_weight`, bake input for regions, wind weights and islands, and the
  auto-tree changes. These are exactly the sources the world cache keys on
  (`tests/runtime/flythrough/world_cache.py:88-120`) [V]. Landing them one by one would invalidate
  the Tier A cache several times.
- **Interaction with the auto-tree:** `tree_sha` covers the codec block and the source fingerprint
  [ADR, `core/gaussian_splat_hlod_auto_tree.h:40-48`, V for the comment]. GSQ v2 changes the codec
  id, so every cached tree becomes stale by key. This is intended, and it must fail closed rather
  than serve a v1 tree [I].
- **Timing:**
  - M3 (the Tier A fly-through) measures the cut, which the word does not change in kind.
  - Decided (O1): run M3 on the current format now; freeze format-relevant changes after M3;
    land WP3 as the single format step; then make the **production** Tier A bake once, on
    layout v2 and GSQ v2.
  - The rejected alternative, landing WP3 before M3, would have delayed M3 by an M-size R3
    package.

### 6.3 Wind model for games (D3)

#### 6.3.1 Displacement [P]

```
d(x_rest, t) = dir × A_row × W(splat) × F(x_rest, t) × fade(distance, level)
F(x, t)      = sin(k·x.xz + ω·t + φ_noise(x)) (+ optional gust term)
```

- **Phase from rest position only.**
  - `φ_noise` is low-frequency world-space noise (a hash of a coarse lattice cell, smoothly
    interpolated), never a per-splat hash.
  - The identity jitter (`gs_deformation.glsl:242-243`) is removed, and so are the seed
    parameters in `depth_compute.glsl:201` and `tile_binning.glsl:992` [V for current code].
- **LOD stability [I]:**
  - A merged splat sits at the weighted mean of its members (`merge.cpp:468`) [V].
  - For a field whose wavelength is much longer than the node extent, its displacement equals the
    weighted mean of its children's to first order.
  - For coarse nodes this no longer holds, so `fade` drives the weight to 0 above a level or
    distance (Fortnite's WPO distance cutoff [Ext]). Coarse nodes with weight 0 are allowed.
- **`W(splat)`:**
  - With `semantic_valid`, it is the baked `wind_weight`.
  - Without it (legacy), a **world** row does not sway under active project wind (O4). Wind is
    opt-in per world: a legacy world that opts in, and every instance row, uses the row weight
    times today's opacity heuristic (`gs_deformation.glsl:239-240`). This keeps the resident
    route visually close for old content that asks for wind [P].
- **Effectors:** the same rule applies. Their per-effector seed (`stable_seed + i * 0x9e3779b9`,
  `gs_deformation.glsl:148`) [V] is replaced by a position hash.
- **Identity flutter:** an optional small leaf-level amplitude keyed by `(node id, index in node)`.
  Decided: none for now (O5); revisit after GS-GAME-1.

#### 6.3.2 Removing the project-global refusal [P]

- Wind and sphere effectors are removed from:
  - the paged fallback list (`nodes/gaussian_splat_world_3d.cpp:722-723`, `:728-729`);
  - `_hlod_auto_route_refusal()` (`:646`, `:657`);
  - `hlod_streaming_unsupported_reason()` (`publisher.cpp:1867-1869`) [V for the current sites].
- The decision becomes per row:
  - The world row and every instance row carry their own `wind_mode`. The existing field is
    `InstanceDataGPU.params.w` (`gs_instance_layout.glsl:33`) [V].
  - `GaussianSplatWorld3D.wind_mode` is **AUTO** by default: the internal row mode
    `SEMANTIC_INHERIT` inherits project wind for each splat with `semantic_valid`, using its baked
    weight; legacy splats have `W = 0`. Thus mixed payloads are handled per splat and a legacy world
    stays still (O4). **ON** maps to `INHERIT`, opting legacy splats into the opacity heuristic;
    **OFF** maps to `FORCE_DISABLED`, disabling wind even on semantic splats. No bool alias is kept.
- Effectors carry a row mask; the existing `effect_params.z` is the "scene effector mask bits"
  (`gs_instance_layout.glsl:37-38`) [V].
- One-route §4.6 ("each domain is lifted in its own PR (F5)") [ADR]: this is that PR for wind and
  effectors. Painterly and the shadow instance filter stay refused.

#### 6.3.3 Bound inflation (Max displacement) [P]

- `D_max = strength × max(row intensity) × (1 + effector max push)` is a frame-global bound,
  because `|d| ≤ strength × intensity × resistance` with `resistance ≤ 1`
  (`gs_deformation.glsl:240-246`) [V].
- **Without the format step:** every node of a wind-enabled row gets `D_max` added to its AABB
  for frustum culling and for the cut's screen-size and record estimates.
- **With the format step:** the inflation is `D_max × max_wind_weight(node) / 255`.
  Only topology flag bit 5 (the payload's semantic-valid AND) permits this reduction; legacy or
  mixed payloads retain full `D_max`, including when the stored wind byte is zero. After scaling
  each own-payload envelope, ancestor bounds still enclose displaced children for safe traversal.
- When `D_max` changes, it changes the cut input. This is acceptable, because the cut already
  reacts to the camera every frame [I].
- **Cost [I]:** slightly more overlap records and conservative culling. It must be measured in
  GS-GAME-1 (row P1).

#### 6.3.4 Shadow-caster consistency [P]

- The caster (`shaders/gs_shadow_caster.glsl`, which today has no deformation [V]) includes
  `gs_deformation.glsl` and is given the **same** per-frame wind time
  (`compute_wind_time_seconds`, `gs_project_settings.h:283-285`) [V].
- Its draw list may be the previous main view's (§6.6.1). Positions are still deformed with the
  current frame's time, so colour and shadow sway in phase.
- The caster honours `hide` (§6.4).

#### 6.3.5 Two steps

| Step | Content | Format | Effect |
| --- | --- | --- | --- |
| **W-S (S–M)** | Position phase without jitter; per-row opt-in, global refusal removed; global `D_max` inflation; caster deformation; distance and level fade; tie-break and caches per §6.5 | **none** | Wind runs on streamed worlds immediately, uniformly per row (legacy weight) |
| **W-F (M, inside WP3)** | Baked wind weight; `max_wind_weight` per node; node-scaled inflation | layout v2, GSQ v2, rebake | Trunks and rocks stay still while leaves sway; tighter bounds |

### 6.4 Runtime recolour, tint, emissive and hide (D4)

**Table [P]:**
- An SSBO `RegionOverrideGPU[]` of exactly **32 B** entries (binding owner decision):
  bytes 0–15: fp32 `vec4 tint = (tint.rgb, tint_strength)`;
  bytes 16–23: `uvec2 emissive_opacity = (packHalf2x16(emissive.rg), packHalf2x16(emissive.b, opacity_mul))`;
  bytes 24–27: `uint flags` (HIDE, NO_WIND, HIGHLIGHT, NO_SHADOW);
  bytes 28–31: zero `uint padding`. CPU/Resource values remain floats. On upload emissive is
  clamped to [0, 65504] and opacity to [0, 1]. A full 65,536-entry segment uses **2 MiB** (65,536 × 32 B).
- Each asset gets a segment. The instance row's `lod.y` (`gs_instance_layout.glsl`)
  [V] holds the segment base, with `0xFFFFFFFF` for none. The splat's entry is
  `table[base + min(region_id, segment_len - 1)]`; entry 0 supplies the whole-asset default.
  CPU upload materializes that default in every region without an explicit override. Separate placements
  have independent segments even when they share an asset. `placement_pad.x` stores `segment_len` and
  `placement_pad.y` retains the dense-remap generation formerly written in `lod.y`. This uses existing
  tail words and keeps the instance ABI at 128 B; no params or push-constant block grows.
- Uploads go as dirty ranges from the CPU and take effect in the **same** frame they are set,
  before the shadow passes and main view are recorded. The table and rows are frozen together for the server frame.

**Application points [P]:**

| Effect | Where | Why there |
| --- | --- | --- |
| `HIDE`, `opacity_mul == 0` | `compute/depth_compute.glsl`, before the sort-key write (`:268-273`) [V for the location] | Hidden splats cost no sort records (acceptance row R3). |
| `opacity_mul` | `tile_binning.glsl` base opacity (`:1423-1424`) [V] | The same spot as the instance opacity. |
| tint and highlight | after the SH cache read and next to `apply_color_grading_binning` (`tile_binning.glsl:1468-1469`) [V] | The cached SH colour is ungraded by design (comment `:1459`) [V], so the override acts without SH-cache latency. |
| emissive | added after lighting, not before (so lights do not modulate it) | `tile_binning.glsl`, after the direct/ambient lighting block and before finite-color rejection and projected-color packing |
| `HIDE`, `NO_SHADOW` | caster | the shadow of a hidden region disappears too |
| `NO_WIND` | `W(splat) = 0` | freezes a region, for example a wrecked tree |

**Why it survives [I]:**
- The region ID travels inside the payload through V4 pages, eviction, readmission and
  compaction.
- The table is resident and indexed by content, not by address.
- Every LOD level carries the same ID because cells never mix regions (§6.2.2).

**Limits [I]:**
- It is no finer than the baked regions. Free painting at arbitrary positions remains an F1 edit.
- Region boundaries inside merged coarse splats cannot exist, by construction. A region therefore
  "pops" only where the cut switches levels, as colour does today.

**API surface [P]** (bound methods; names are proposals):

```gdscript
# World (GaussianSplatWorld3D) and object (GaussianSplatNode3D) expose the same surface.
func get_region_id(name: StringName) -> int          # from the baked region manifest; -1 if unknown
func get_region_names() -> PackedStringArray
func set_region_override(region: int, override: GaussianSplatRegionOverride) -> void
func clear_region_override(region: int) -> void
func clear_all_region_overrides() -> void
# Object-level shorthand: region 0 is the whole asset.
func set_tint(color: Color, strength: float) -> void
func set_emissive(color: Color) -> void
func set_hidden(hidden: bool) -> void                # distinct from Node3D.visible (that removes the row)
```

- `GaussianSplatRegionOverride` is a `Resource` with `tint`, `tint_strength`, `emissive`,
  `opacity`, `wind_enabled`, `cast_shadow` and `highlight`.
- Region names come from bake metadata: a `regions[]` manifest in the V4 metadata JSON and in
  plain-v3 metadata. Volume IDs are NOT stable across edits: game code resolves names with `get_region_id(name)`, never hard-coded numbers.
- Picking (#1312) later returns `{instance, region_id}` from a per-pixel ID output. It is listed,
  not designed here.
- **Authoring of regions [P]:** region volumes at bake (`GaussianSplatRegionVolume3D`, a named
  box or convex hull, first match wins) or a per-splat PLY property `region_id`. Volumes are the
  primary path because scans have no labels.

### 6.5 Atlas-address-keyed state (D5)

#### 6.5.1 Inventory [V]

| State | Key today | Site | Class |
| --- | --- | --- | --- |
| wind and effector seed | `atlas_index ^ instance*φ` / `gaussian_idx ^ ...` | `compute/depth_compute.glsl:201`, `shaders/tile_binning.glsl:992`, `gs_deformation.glsl:148` | identity value |
| sort tie-break | `atlas_index` | `compute/depth_compute.glsl:270-272` | identity value |
| subpixel hysteresis | slot `gaussian_idx` (world) or `hash(gaussian_idx ^ instance)` (instances), with a key hash | `shaders/tile_binning.glsl:1182-1190` | content-following cache |
| SH colour cache | `sh_color_cache.colors[gaussian_idx]`, **no key** | `shaders/tile_binning.glsl:1441-1452`, write `:1460-1461` | content-following cache |

#### 6.5.2 SH colour cache: stale colour after page reuse, verified mechanism [V], visibility [I]

1. On the HLOD route `gaussian_idx` is the atlas record index (`tile_binning.glsl:896`) [V].
2. The cached value is used whenever `use_cache && !sh_force_update` holds, the phase does not
   match, and the cached word is non-zero (`tile_binning.glsl:1441-1449`). There is no tag or key
   [V].
3. `sh_force_update` (`params.sh_config[3]`, `renderer/tile_render_stages.cpp:331`) is driven by
   `sh_cache_needs_full_update` [V]. That flag is set only by:
   - a cache resize (`renderer/tile_renderer.cpp:604-605`, `:4176-4178`);
   - a mode or divisor change (`:607-608`, `:4165`);
   - an SH band change, which also clears the buffer (`renderer/tile_render_stages.cpp:316-319`).
   No residency event sets it. `clear_color_cache` has exactly one caller, the band change
   (`tile_render_stages.cpp:318`) [V].
4. HLOD residency returns evicted runs to the allocator (`lod/hlod_node_residency.cpp:110`,
   `:232`) and hands them to the next admitted node (`:694`, `:874`). The allocator is best fit
   over freed runs (`core/streaming_atlas.cpp:66-88`) [V].
5. The HLOD route does use the cache:
   - `scratch_params.request_sh_amortization = features->enable_sh_amortization`
     (`publisher.cpp:2558`), checked against the admitted capacity (`tile_renderer.cpp:584-590`)
     [V].
   - It is off by default (`pipeline/enable_sh_amortization` defaults to false,
     `renderer/pipeline_feature_set.cpp:154`; the default tier is `custom`,
     `core/gaussian_splat_manager.cpp:1161`) [V].
   - It is **on in every named quality tier**, with divisors 2–8
     (`core/quality_tier_config.cpp:43-44`, `:61-62`, ...), because
     `tier_apply_pipeline_toggles` defaults to true (`gaussian_splat_manager.cpp:1162`) [V].
6. **Consequence [I]:**
   - A node admitted into reused pages reads the evicted tenant's colour for each splat whose
     phase does not match, for up to `divisor − 1` frames (7 at tier `low`), at full opacity.
   - The node is drawn at full opacity: `DrawnNodeGPU.fade` is "written as 1.0 and not read yet"
     (`gs_instance_layout.glsl:123`) [V].
   - Under thrash (#1483: 25,523 evictions in one run, from the deviation report), this happens
     continuously.
   - It needs no wind and no relocation, so #1499's statement that none of its items is live on
     master does **not** hold for the SH cache under amortization.
   - It is not measured here. The repro is in #1502; the WP1 fix carries its own red-then-green
     test.
7. Same class, smaller [I]:
   - The subpixel hysteresis of a new tenant can inherit "was visible" when the key hash collides,
     because the key is a hash of the address, not of the content. Its effect is only a threshold
     choice for one frame.
   - The chunk route uses the same allocator (`core/gaussian_streaming.h:28`) [V include only].
     D5 deletes it.

#### 6.5.3 Decision [P]

- **Content-following caches are invalidated on (re)assignment.**
  - Whenever residency assigns an atlas run to a node for an upload, or as a compaction
    destination, the renderer clears `sh_color_cache` (and the subpixel history) for exactly that
    record range. The clear goes into the same command stream as the upload, before the first draw
    that references it.
  - `0` is already the "not cached" sentinel (`tile_binning.glsl:1447`) [V]. This costs one
    `buffer_clear` of `4 × count` bytes per upload [I].
  - A tag buffer was rejected: it costs +4 B per atlas record and per-read compares. Identity
    keying was rejected: there is no dense identity index.
- **Identity values do not use the address.**
  - Wind and effectors use rest position (§6.3.1).
  - The sort tie-break becomes a stable composite key (world: node, local; above 131,072 nodes at L=14, fold node IDs with `node & ((1 << (31-L)) - 1)` and report `tie_world_node_folded`; instances: ObjectID rank, chunk, local); pairwise order between surviving instances never changes, where
    `local_index = atlas_index − node.first_record`. The node index is in `DrawnNodeGPU.chunk_id`
    (`gs_instance_layout.glsl:122`) [V]. The node's first record comes from the chunk meta [I].
    For instance rows the hash adds the instance's stable placement key [ADR §14.1].
- **Relation to #1499:**
  - This ADR closes item 1 by splitting it. Content-following state follows the address and is
    invalidated on assignment. Identity state no longer depends on the address.
  - This is also the precondition for compaction of drawn nodes (#1498), and it fixes the live SH
    case now.
  - Items 2 (per-view `begin_frame`) and 3 (`finish_upload` without a GPU fence) stay in #1499.
    The cache clear only needs submission order, not a fence [I].
- **#1245** (instances sharing cache entries; view direction mixed): related but separate. The
  recommended fix there is to bypass the cache for asset records referenced by more than one row.
  It is not decided here.

### 6.6 Rigid moving objects (D6)

#### 6.6.1 One-frame shadow lag

- The cause is verified in the code comment:
  - "Forward+ renders the shadow passes of a frame BEFORE that frame's GS main view, so a shadow
    pass consumes the record of the previous main view: the draw list, rows and atlas the GPU
    still holds" (`renderer/hlod_shadow_caster_plan.h:10-12`) [V].
  - A moving object's splat shadow is therefore one frame behind, an offset of
    velocity × frame time [I].
- **Decision [P], "rows current, list previous":**
  - Instance rows (transforms, overrides, wind time) are uploaded at the frame's sync point,
    before Godot's shadow passes.
  - The caster keeps the previous view's *node list*, so it needs no new cut, but reads the
    *current* rows.
  - An object that entered view this frame casts from the next frame on. This is accepted and
    reported as a counter.
- Omni and spot casters are refused today (`hlod_shadow_caster_plan.h:42`, #1424) [V]. The same
  caster code is touched, so #1424 lands first (§8).
- The caster refuses quantized payloads (`hlod_shadow_caster_plan.h:44`) [V]. GS-GAME-1 runs the
  raw payload, or that refusal is lifted in its own PR.

#### 6.6.2 A4 and A5 (GS-COMBINED-1, run 44532905736, from the deviation report)

- **A4, penetration depth order: 185 of 590 frames fail.**
  - The verdict says "as variant A", so it fails on the chunk route as well [ADR, report].
  - Splats are sorted by centre, so world/object interpenetration is only approximately correct
    in 3DGS [I].
  - It is not caused by this ADR, and nothing here fixes it.
  - **Decision [P]:** triage the reference first (renderer versus CPU reference), then write a
    tolerance contract with a reason. GS-GAME-1 consumes that contract. It does not invent one.
- **A5, transform offset: 81 of 600 frames, 0.5–1.2 px, worst on the fastest object.**
  - The hypothesis is one frame of transform latency [I, unproven].
  - If the triage confirms it, the "rows current" change in §6.6.1 is the fix, and A5 becomes
    acceptance row O2.
- Both stay owned by GS-COMBINED-1. This ADR only orders them before GS-GAME-1.

#### 6.6.3 Object LOD and proxies: out of scope for the first gate [P]

- Objects have no LOD levels: `lod_count = 1`, although `GS_MAX_ASSET_LODS = 8` exists [ADR,
  HLOD ADR §1]. There are no instance proxies (HLOD ADR §6.9, stage 2+) [ADR].
- GS-GAME-1 uses 32 objects, which the tail holds [I]. 1,000 trees or crowds would not fit.
- That work is a follow-up package (WP8, M–L) under HLOD ADR §6.9, with its own gate. Until then,
  the one-route §4.5 budget rule refuses objects whole and explicitly [ADR].

### 6.7 Skinning and deforming splats (D7): out of scope

- Nothing renders animation today: "CPU sampling only; nothing renders it"
  (`docs/features/animation.md:9`) [V].
- The owner kept animation and requires render wiring (D-6a, CLN-SH06; the animation render ADR
  is CLN-SH06-1) [ADR]. That ADR owns skinning.
- This ADR constrains it in four ways [P]:
  1. Deforming content is always an **instance**, never world-tree content (§6.1).
  2. Deformation runs in `depth_compute` before the instance transform. It uses the same time
     sample as wind and must be applied by the caster too.
  3. Bounds come from the skeleton or the control nodes, declared like `D_max`.
  4. Per-splat skinning data lives in a side buffer of the instance region, not in the semantic
     word.
- The design check's sizing (linear blend skinning with ≤ 4 bones, about 16 B per splat; SC-GS
  control nodes [Ext]) is input to CLN-SH06-1, not a decision here.
- Collision proxies are D-6b and CLN-SH04 [ADR]. Islands (§6.1) are natural proxy owners.

---

## 7. Acceptance: GS-GAME-1 vertical slice (D8)

**Scene [P]:**
- The T0 world as **V4 streamed** under atlas pressure: about 35 % atlas, a drawn budget of 500k,
  raw payload.
- 32 rigid objects on scripted paths, plus the **spawn** of a new asset at t = 10 s (needs the
  #1473 fix from #1500).
- One mesh character.
- Two vegetation regions with baked wind weight, and one rock region with weight 0.
- Three regions (near, mid, far) recoloured every 2 s, one region hidden every 4 s.
- Four omni lights plus a directional light with shadows.
- A camera path with one teleport and one return-to-pose segment, at 1080p.

**Hardware (O6, open):** a real 8 GB mid-range card (RTX 3060/4060 class) is preferred. Until
one is available, the 3080 runner runs with the ledger limited to 8 GB, and every report says
**explicitly** that it is an approximation, never the mid-range result.

**Pass criteria [P]** (every row has a forcing control that must turn it red in the same job):

| ID | Measure | Pass | Control (must fail) |
| --- | --- | --- | --- |
| P1 | Frame time with valid GPU timestamps (`*_valid`, GR-18) | Hard gate (O7): p95 ≤ 25 ms, p99 ≤ 33.3 ms (the GR-15 3080 numbers); max ≤ 50 ms at spawn. **Target:** p95 ≤ 16.7 ms, **always reported next to the gate**, never instead of it. | a scripted 30 ms GPU stall |
| P2 | Dynamics overhead: dynamics on versus off, ratio over identical samples (percentiles are not additive) | ≤ +1.5 ms p95 | — (ratio row) |
| H1 | Holes: `nonresident_drawn`; G16 missing-node fraction after settle | 0 / 0 % | a forced non-resident draw |
| W1 | **Wind identity oracle** (below) | **0 changed frames** | forcing switch: address-keyed phase and tie-break restored |
| W2 | Wind LOD consistency: parent displacement versus the weighted mean of its children, below the fade level | ≤ 0.5 px | fade disabled at coarse levels |
| W3 | Wind shadow phase: caster displacement versus colour displacement | ≤ 1 shadow texel | caster without deformation |
| W4 | Rigid region: splats with weight 0 move | 0 px | legacy opacity stiffness forced |
| C1 | SH cache after readmission: pixels of a readmitted node versus a divisor-1 render of the same pose and drawn set | 0 changed pixels in the first frame after admission | invalidation switched off |
| R1 | Recolour coverage: ID mask per pixel versus the expected region, at every LOD level | 100 % of region pixels tinted, 0 foreign pixels | override applied by address |
| R2 | Recolour latency | effect in the frame it was set; survives far → evict → near | upload deferred by one frame |
| R3 | Hide | 0 sort records for the hidden region; no shadow | hide applied only in raster |
| S1 | Spawn | the world's `drawn_nodes` do not fall to the root LOD | #1473 forced (asset-set reset) |
| O1 | A4 | passes the triaged tolerance contract | — |
| O2 | A5 / shadow lag | ≤ 0.5 px object offset; caster versus colour of the moving object ≤ 1 texel | rows uploaded after shadow |
| L1 | Lights: lit splats (L1/L2) | pass | — |
| L2 | Omni and spot splat shadows | pass after #1424. Before that, **reported as a known fail**, never skipped. | — |
| M1 | VRAM ledger versus external VRAM; upload bytes per second | within budget; upload ≤ a threshold fixed before the first run | — |
| F3 | Region-key compression cost (T0, §5.2 setup) | interior bytes ≤ +10 % versus single-region | — |

**Wind identity oracle (W1) [P]:**
1. Freeze wind time with the existing `rendering/gaussian_splatting/animation/wind_time_scale = 0`
   (read at `core/gs_project_settings.h:270`, applied at `:283-285`) [V]. Displacement is then
   static but non-zero.
2. Hold the camera static at pose A, settle, and record frame A and the drawn node-key digest A.
3. Force eviction of the drawn set (page-budget churn). Pre-occupy the freed runs so readmission
   lands on **different** pages, then let it resettle.
4. If the drawn digest differs from A, the verdict is **INVALID**, not pass: HLOD ADR §6.4
   accepts path-dependent drawn sets [ADR].
5. Otherwise, compare against frame A: changed frames must be 0 (the static-frame oracle of #1050
   [I; established in that issue, not re-run here]).
6. Run once with SH amortization at divisor 8 (this also covers C1) and once with it off.
7. The subpixel hysteresis is legitimately path-dependent, so the oracle runs with hysteresis
   disabled, or after ≥ 2 frames, and says which.
8. **There is no "red on master" for a streamed world:** on master, wind forces full residency
   (§1). The negative control is therefore the forcing switch in the same build: the
   address-keyed seed and tie-break restored. It must give > 0 changed frames.

---

## 8. Work packages and order [P]

| WP | Content | Effort | Risk | Rebake | Depends on |
| --- | --- | --- | --- | --- | --- |
| WP0 | This ADR and the owner decisions in §11 | S | R0 | — | — |
| **WP1** | SH colour cache cleared on atlas run (re)assignment (#1502; lands with this record, O10). Subpixel history and the tie-break from `(node index, local index)` follow if not in that change | **S** | R3 (renderer/shader) | no | — (can run in parallel with M3; it touches no world-cache source) |
| **WP2 (W-S)** | Position phase without jitter; per-row wind and effector opt-in, global refusal removed; global `D_max` bound inflation; caster deformation with the frame's wind time; distance and level fade | **S–M** | R3 | no | WP1 (for W1); #1424 merged first (same caster code) |
| **WP3 (format step)** | Layout v2 (`semantic`); GSQ v2 plane; V4 `max_wind_weight`; merge key `(cell, region)` and wind-weight aggregation; bake input (region and wind volumes, PLY properties); island extraction; pack into GPU padding; F3 measurement on T0 | **M** (M–L with tools) | R3 (format, bake) | **yes, once** | M3 done; format freeze between M3 and WP3 |
| WP4 | Override table, application points, caster hide, public API (§6.4), doc classes | M | R3 + public API | no (needs WP3 data) | WP3 |
| WP5 | Island authoring and helper (placement manifest → nodes) | S–M | R2 (editor) / R3 (bake side is inside WP3) | — | WP3 |
| WP6 | Shadow rows current (§6.6.1); A5 fix if the triage confirms latency | M | R3 | no | #1424; A4/A5 triage |
| WP7 | GS-GAME-1 lane, harness, forcing controls | M–L | R3 (gate machinery) | — | WP1–WP6, #1473 (#1500) |
| WP8 | Instance LOD and proxies (HLOD ADR §6.9) | M–L | R3 | instance assets | after GS-GAME-1 |
| — | Skinning and deformation | L | R3 | — | separate ADR, CLN-SH06-1 |

**Order relative to the program [P]:**
1. #1501 and #1500, then **M3** (Tier A on HLOD). If M3 fails, dynamics work is moot. WP0 and WP1
   run in parallel.
2. **#1424** (omni and spot casters), then **WP2**. From this point wind runs on streamed worlds
   with a uniform weight.
3. **WP3** as the single format step, then the **production Tier A bake**, once.
4. WP4, WP5 and WP6, then **WP7 (GS-GAME-1)** as the gate for "game alpha". WP8 and skinning
   follow.
5. **D5 (chunk-route deletion)** blocks none of this, and none of it blocks D5. The chunk route
   cannot draw a world next to objects (#788) [ADR]. But:
   - Between D5 and WP2 there is **no wind on streamed worlds at all**. Release notes must say so.
   - The HLOD-versus-chunk frame-time comparison must be saved before D5 (deviation report §4.6).

---

## 9. Format and version bumps (all in WP3) [P]

| Item | From | To | Old files |
| --- | --- | --- | --- |
| `GAUSSIAN_STRUCT_LAYOUT_VERSION` | 1 | 2 (`_padding` → `uint32_t semantic`) | Layout 1 loads with the word forced to 0. |
| GSF canonical payload schema | 1 (132-byte records) | 2 (136-byte records carrying `semantic`) | Schema 1 loads with semantic forced to 0. |
| GSQ codec id | 1 | 2 (adds semantic planes) | v1 decodes with the word = 0. |
| V4 node record | reserved word = 0; topology flag bit 5 = 0 | `max_wind_weight` in byte 120; flag bit 5 = AND of payload `semantic_valid` | A valid rigid node has flag bit 5 set and wind byte 0; legacy nodes have the flag clear. Remaining reserved bits stay zero. |
| Auto-tree `tree_sha` | codec v1 | codec v2 | Stale by key; rebuilt. |
| World cache (CI) | sources as today | changed generator and tree sources | Invalidated once (`world_cache.py:84-120`) [V]. |

Readers refuse non-zero reserved semantic bits (28–30), with a distinct reason.

## 10. What a cheaper version would force to redo [I]

- **Region ID packed into `painterly_meta`:** GSQ v1 refuses per-splat differences
  (`gsq_codec.cpp:436-437`) [V], and it collides with painterly.
- **Word stored in `_padding` without GSQ v2:** silently zeroed by the Tier A codec, a vacuous pass.
- **Wind weight copied from the merge winner** (today's behaviour for every non-moment field):
  coarse nodes sway like their heaviest member.
- **The region ID left out of the merge key:** overrides bleed across regions at coarse LOD.
  Fixing that later means a rebake.
- **The seed moved to `(NodeKey, index)` with the jitter kept:** merged splats still get new
  phases at every LOD switch, so wind is not LOD-stable.
- **Inflation without a declared bound:** splats leave their node bounds, and the cut and culling
  drop them.

## 11. Owner decisions (2026-11-20)

On 2026-11-20 the owner told the team to go ahead with implementing the improved plan, and
accepted the recommended option of each question below. O6 stays open.

| # | Decision |
| --- | --- |
| **O1** | **Decided (a).** M3 runs on the current format. Then a format freeze, WP3 as the single format step, and **one** production Tier A bake. |
| **O2** | **Decided: accept, with islands.** The world tree is static, per-object dynamics are instances, and dynamic islands are extracted at bake time (§6.1). |
| **O3** | **Decided: 16 bits** of region ID. |
| **O4** | **Decided (a).** A world without baked semantics does not sway under active project wind. Wind is opt-in per world. |
| **O5** | **Decided: no identity flutter for now.** Revisit after GS-GAME-1. |
| **O6** | **Open.** A real 8 GB card is preferred. Until one is available, GS-GAME-1 reports the 3080 with an 8 GB ledger, explicitly labelled as an approximation. "Game alpha" is not claimed on the approximation alone. |
| **O7** | **Decided (a).** Hard gate p95 ≤ 25 ms and p99 ≤ 33.3 ms. 16.7 ms is the target, and it is **always** reported next to the gate. |
| **O8** | **Decided: out.** No object LOD or proxies in the first gate; WP8 follows GS-GAME-1. |
| **O9** | **Decided: rows current.** The shadow-lag fix uses current-frame rows only (§6.6.1). |
| **O10** | **Decided: now.** WP1 (#1502) lands with this record, in parallel with M3. |

The questions, options and reasons as presented for the decision:

| # | Question | Options | Recommendation |
| --- | --- | --- | --- |
| **O1** | When does the format step land relative to M3 and the production Tier A bake? | (a) M3 on the current format now; freeze; WP3; one production bake. (b) WP3 before M3. (c) Defer WP3 and bake Tier A as production now. | **(a).** M3 tests the cut, not the payload. (c) bakes 10 GB twice. |
| **O2** | Accept the authoring rule "world tree = static; per-object dynamics = instance", including bake-time island extraction and its hole policy? | accept / accept without islands (manual split only) / reject | **Accept, with islands.** Scans have no separate objects otherwise. |
| **O3** | Region-ID width | 16 bits (2 MiB table) / 20 bits (32 MiB) / 12 bits plus 4 more wind bits | **16.** Regions are coarse; objects use instances. |
| **O4** | Default for a world without baked semantics when project wind is on | (a) it does not sway (opt-in per world); (b) it sways with the legacy opacity heuristic | **(a).** Avoids rocks and houses swaying in old content; it is explicit. |
| **O5** | Identity flutter (per-splat micro-motion keyed by `(node, index)`) | none / leaf level only, later | **None in WP2;** revisit after GS-GAME-1. |
| **O6** | Hardware for GS-GAME-1 | buy or borrow an 8 GB card for the runner / 3080 with an 8 GB ledger cap as the approximation | **A real 8 GB card** before "game alpha" is claimed. The approximation is fine for development. |
| **O7** | Frame-time gate | (a) hard gate at the GR-15 3080 numbers plus a reported 16.7 ms target; (b) a 16.7 ms hard gate now | **(a).** HLOD T0 alone is already at p95 15.4 ms on the 3080 (deviation report §3), so (b) would be a known fail. |
| **O8** | Object LOD and proxies in the first gate? | out (32 objects) / in (≥ 500 instances) | **Out;** WP8 after GS-GAME-1. |
| **O9** | Shadow lag fix scope | rows current (§6.6.1) / a full light-view cut | **Rows current.** The light-view cut is a later slice of the HLOD ADR. |
| **O10** | SH-cache fix priority | WP1 now (parallel to M3) / with WP2 | **Now:** it is a live defect in every named quality tier, and it is S. |

## 12. What was not verified

- **No runtime measurement.** No claim here was run on a GPU. The SH-cache stale colour is
  verified as a mechanism in code; its visibility is inferred.
- The V4 node record's reserved words were taken from HLOD ADR §14.4.1, not read in
  `io/gaussian_splat_world_paged.*`.
- The node's first atlas record in chunk meta (needed for the local index) was not located.
- The emissive application point in the lighting applier was not located.
- The GSQ decode path leaving `_padding` at 0 is inferred from the header comment
  (`gsq_codec.h:121-123`) and from the absence of `_padding` in `gsq_codec.cpp`. The decoder was
  not traced.
- The compression cost of region-pure cells (F3), the GSQ v2 plane size and the overlap-record
  cost of the bound inflation are all unmeasured.
- The A4/A5 figures, Tier A sizes and #1483 eviction counts come from the deviation report and
  its runs, not from runs re-read here.
- External practice ([Ext]) was not re-checked.
- No independent second review of the design was obtained before acceptance.
