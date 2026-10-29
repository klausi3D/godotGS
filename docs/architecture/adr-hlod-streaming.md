# ADR: Hierarchical LOD for streamed splat worlds (budgeted cut, merged nodes, node streaming)

- **Status:** **Accepted (maintainer, 2026-10-02)** — option D, with the decisions recorded
  in §11. The **2026-10-25 integration amendment (§14)** defines the approved next stages
  for streaming, precision and locally editable large worlds. Acceptance of this design
  does not establish implementation or runtime acceptance; source code and GitHub PRs carry
  those separately. §9 retains the original slices and their gates; §14.7 maps the extension
  onto them. The earlier independent review responses remain in §13. The **offline cost
  disposition in §15 was accepted by the maintainer on 2026-10-25**: only explicitly selected
  offline prepared-content jobs replace the relative ≤2× bake-cost requirement with a
  predeclared, finite and enforced resource/deadline/publication contract. This design
  approval alone does not accept S1b or advance packages B–G.
- **Risk class:** this file is R0 (`docs/**`). It is the **R3 design record** that
  `docs/governance/agentic-engineering.md` requires before implementation, because the
  decision changes an on-disk format (`.gsplatworld`), importer format versions, the renderer
  and its shaders.
- **Format identity update (2026-10-11):** The version assignments below record
  the original HLOD decision. New exports use world v3 (plain112/HLOD192) and
  importer version5 under the approved
  [world-layout compatibility decision](adr-import-input-hardening.md#adr-world-layout-identity-and-hlod-version-compatibility-2026-10-11).
  Released plain v1 and HLOD v2 remain readable; the unmerged plain-v2 prototype
  requires re-export. HLOD node layout, bake semantics and geometry stay unchanged.
- **Tracking:** Refs #1131 (non-deterministic truncation at `quality/max_splat_count`).
  Builds on #1088 / PR #1134 (atlas pages + byte budget) and #1087 / PR #1130 (distance-bounded
  demand). Related: #1053 (LOD prefix truncation holes), #1016 (streaming in the alpha
  envelope).
- **Maintainer decisions this ADR implements (2026-10-02):**
  1. Distance-LOD streaming is a public-alpha blocker.
  2. When the splat cap binds, the count is reduced **only through LOD/streaming, never by
     truncation**.
  3. The target is streamed open worlds of many millions up to billions of splats in real
     time; rendering cost must scale with the **screen**, not with world size or view distance.
- **Historical source baseline (§§1–13):** `origin/master` = **`efb2c8bfba7`**. Every
  `file:line` below was read
  at that commit unless it names the #1088 branch, which was read at **`f3ddae2a162`**
  (PR #1134's head `feec67befa6` merges #1130 into it; the allocator code is unchanged).
- **Measurements:** §5 records the original prototype measurements (§5.7 describes their
  reproduction). They are not measurements of the integrated architecture in §14. The source
  baseline for this amendment is `36f73d36f8daa0b4bb710d1e172a8f9f3b8af5d8`; it adds
  requirements, not new measured results.

## 1. Problem

There are three defects, and they share one cause: the renderer has no way to show distant
content at lower detail except by drawing a shorter prefix of each chunk.

1. **Truncation flicker (#1131).** When the streaming route's visible-splat cap binds, splats
   claim slots with `atomicAdd` (`compute/depth_compute.glsl:225-227`), and the visible chunk
   list is itself unordered (`compute/frustum_cull.glsl:143`). Which splats survive the cap is
   decided by GPU scheduling. #1131 measured 299 of 300 static frames differing from frame 0
   whenever the cap bound, and 0 when it did not.
2. **LOD by prefix truncation (#1053).** Per-chunk distance LOD sets
   `effective_count = count / 2^L` (`core/streaming_visibility_controller.cpp:871-886`, levels
   from `lod/lod_config.cpp:179-209`), and the GPU draws that **prefix** in stored order
   (`core/streaming_global_atlas_registry.cpp:440-444`, `depth_compute.glsl:140`). Stored order
   is Morton order for the primary asset (`core/gaussian_streaming.cpp:1864-1890`) and octree
   query order for world grid chunks (`core/gaussian_splat_merge_utils.cpp:106-128`). Either
   way the prefix is a spatial sub-block, so distant chunks lose whole regions. §5.2 measures
   it.
3. **Whole chunks are still loaded.** LOD changes only what is drawn. The asset format has
   room for 8 LOD levels (`GS_MAX_ASSET_LODS = 8`, `renderer/gaussian_gpu_layout.h:11`), but
   only level 0 is ever filled (`lod_count = 1` at
   `renderer/resident_instance_contract_publisher.cpp:555` and
   `core/gaussian_streaming.cpp:1344`). The per-instance LOD the scene director computes
   (`core/gaussian_splat_scene_director.cpp:1507`, written at `:1658`) is clamped to that one
   level on the GPU (`frustum_cull.glsl:106`). `lod_mask` is status only
   (`core/gaussian_streaming.cpp:1099`).

A fourth inconsistency comes from #1131: **the resident route never applies the cap.**
`max_visible_splats = MAX(min(atlas, max_splats), instances x dispatch_chunks x max_chunk_splats)`
(`renderer/resident_instance_contract_publisher.cpp:855-864`), and that product is at least
the atlas.

And a structural limit: with flat chunks, the number of visible chunks grows with the square of
the view distance. §5.3 shows the draw count of any flat scheme growing with world size, and
§5.2 shows that thinning without merging leaves holes and see-through regions at distance.

## 2. Decision drivers

1. **Never truncate.** A binding budget must produce the same image every frame for a static
   camera (the #1050 static-frame oracle must read 0 differing frames).
2. **Screen-bound cost.** Splats drawn per frame must not grow with world area. A logarithmic
   term in view-distance range is acceptable; a quadratic one is not.
3. **Coverage at distance.** Coarse levels must keep opaque surfaces opaque: no holes, no
   see-through.
4. **Bounded VRAM.** Residency must fit a byte budget (#1088/#1134) with coarse fallback, so a
   streaming miss shows coarse content, never a hole.
5. **Build once.** The stage-1 format and runtime must not need rebuilding when later stages
   improve the bake, the error metric, transitions or compression.
6. **One policy for both routes.** The resident route must honour the same budget the same way.
7. **Alpha scope.** Stage 1 must be the smallest slice that meets 1–6.

## 3. Current state (what the design reuses or replaces)

| Piece | Where (at `efb2c8bfba7`) | Fate |
| --- | --- | --- |
| Per-chunk distance level (Octree-GS formula) | `lod/lod_config.cpp:179-199`, `streaming_visibility_controller.cpp:871-886` | Replaced by the cut (§6.4) |
| Prefix draw `splat_count = effective_count` | `streaming_global_atlas_registry.cpp:440-444` | Historical upload preview only (§6.6); not complete coverage or readiness (§14.2) |
| GPU chunk meta, 64 B with `lod_level` + 3 pads | `renderer/gaussian_gpu_layout.h:187-202` | A node is a chunk; one pad carries the fade factor (S6) |
| Asset LOD ranges (8 levels) | `gaussian_gpu_layout.h:151-173` | Not used; the cut emits a flat node list |
| Atomic truncation at the cap | `depth_compute.glsl:225-227`, `compute/instance_count_clamp.glsl:44-47` | Becomes unreachable: the CPU cut guarantees `count <= cap` (§6.5) |
| Importance metric `opacity x max scale` | `core/gaussian_importance.h:32-44` | Superseded inside nodes by `opacity x area` (§5.2) |
| Resident atlas importance clamp (#420) | `renderer/resident_atlas_budget.h` | Superseded by the cut on the resident route (stage 1, S5) |
| World chunks: 10 m grid cells, index lists | `core/gaussian_splat_merge_utils.cpp:43-160`, `io/gaussian_splat_world_io.cpp:425-469` | Become the leaf partition input; payload becomes contiguous per node |
| #1088 page allocator: 1,024-splat pages, best fit, coalescing, ≤ 64 pages per run | branch `f3ddae2a162`, `core/streaming_atlas.{h,cpp}` | Kept unchanged; nodes are allocations (§6.6) |

## 4. Options

- **A — flat importance prefix (the coordinator's first proposal).** Order splats inside each
  chunk by importance at bake time, so `count / 2^L` is a valid subset; load only the prefix
  pages; raise levels by distance until the cap fits.
- **B — revive the deleted merged-splat pyramid** (`SplatClusterer` + `StreamingLODManager`,
  removed in `de8fc43e2c6`; `AdaptiveLODSystem`, removed in `7205f2d10ef`).
- **C — uniform hash thinning** at load or draw time (`hash(index) < keep_ratio`).
- **D — hierarchical LOD (chosen).** A spatial tree over the world. Leaves are full-detail
  chunks; interior nodes hold pre-baked **merged** splats that approximate their subtree. Each
  frame a **cut** through the tree is chosen by screen-space error under a splat budget and a
  byte budget; the budget is met by coarsening the cut, never by truncation. Tree **nodes** are
  the streaming unit in #1134's page allocator. Importance order **within** a node is kept as
  an ingredient (progressive upload, transitions), not as the LOD mechanism.

The prior art D draws on, and what is taken from each:

- *Hierarchical 3D Gaussians* (Kerbl et al., SIGGRAPH 2024): interior nodes by weighted
  **moment matching** of the children (mean, covariance, SH), a cut by projected node size
  ("granularity"), and parameter interpolation between levels. Its interior nodes are further
  **optimised against training views**; we have none, so §5.2 measures closed-form merges only.
- *Octree-GS* (Ren et al., 2024): the octree level formula already in `lod_config.h`. Its
  anchors are decoded by MLPs, which this renderer rejects (no per-splat networks), so only the
  spatial structure is relevant.
- *CityGaussian* (Liu et al., ECCV 2024) and *LODGE* (2025): block-wise LoD for large scenes,
  built by pruning/compression per block, and distance-driven block selection. They support
  "block = streaming unit, levels per block"; D generalises that to a tree so the block size
  can grow with distance.

## 5. Measurements

### 5.1 Method

- **Renderer:** the engine itself (RTX 3090, Vulkan, `speed_trace` build `ac039ac315a`), a
  throwaway project, one `GaussianSplatNode3D` per variant, black background, linear tonemap,
  960x540, vertical FOV 60°. Each variant is a PLY of exactly the splats the method would draw.
  The reference and every variant are loaded the same way
  (`GaussianSplatAsset.load_from_file` on a PLY written by the same encoder) and drawn by the
  same resident path, so the only difference is the splat set.
- **Determinism check:** variants whose cut is all leaves reproduce the reference
  byte-for-byte (PSNR reported as 100), so any difference below is the method, not noise.
- **Metrics:** PSNR and SSIM from the repo's own `BenchmarkVisualMetrics`
  (`tests/examples/godot/test_project/scripts/benchmark_visual_metrics.gd`, run headless,
  copied verbatim with `class_name` removed). That tool downsamples to 256 px and averages over
  the whole frame, so mostly-black frames read high. Every table therefore uses the
  **region-of-change crop**: the bounding box of pixels that differ by more than 8/255 in *any*
  variant of that view (the same crop for every method), padded by 8 px.
- **Reference dropout (finding, now #1137).** At the far holzbank view the **full-detail
  reference itself** has a rectangular, tile-aligned black hole that no reduced variant has.
  #1137 has since traced it with the overflow counters: the per-tile raster cap
  (`GS_MAX_RASTER_SPLATS_PER_TILE` = 12,288 under the default `gpu_preset="high"`) drops the
  farthest records of the densest tiles, without a warning. Those 204 pixels are excluded
  (copied from the reference into every variant) before the metrics are taken. It is a second
  argument for LOD, and a cap the cut must respect (§6.5).
- **Assets:** `holzbank-clean-centered.ply` (252,326 splats, a bench on grass, an object scan),
  `baum-mit-wiese3.ply` (800,000, tree and meadow, sparse, many floaters) and
  `baum-mit-wiese2.ply` (8,000,000; by name a denser capture of the same scene), all from the maintainer's
  GrandmasHouse project; the corridor is rebuilt exactly as
  `benchmark_open_world_stage_contract.gd` builds `open_world_corridor_20m`.
- **Equal budget.** Every method draws the same number of splats in the same region. The HLOD
  cut is computed first; for every interior node in the cut, its leaves share that node's splat
  count in proportion to their size, and each thinning method picks that many of the leaf's
  original splats: **A-area** (top `opacity x sigma_a x sigma_b`, the two largest axes),
  **A-max** (top `opacity x sigma_max`, today's `gaussian_importance`), **C** (smallest
  `hash(index)`), **M** (Morton prefix, today's behaviour).
- **Prototype bake:** octree split at the midpoints of the data's bounding box (only along
  axes at least half the longest; this rule is **withdrawn** by decision 5 in favour of a
  world-aligned grid, §6.1), leaves ≤ 16,384 splats, interior nodes at a 4:1 splat reduction,
  merge cells of size `eps` (the node's geometric error); `mass`, `sat` and `mm`
  are the three opacity rules of §6.3, and `hyb` is `sat` with a sparse-cell fallback.

### 5.2 Image quality at equal budget (region-of-change crop, PSNR dB / SSIM)

Distances are camera-to-centre. "Cut splats" is what every column draws. All rows are from the re-run with single-child cells copied unchanged (§6.3, implementer
question (d)). The fix moved `sat` by at most 0.02 dB and `hyb` by at most 0.1 dB on holzbank
and baum3, and both by at most 0.003 dB on baum2 (identical at the precision shown), because a
lone small splat in a merge cell is sub-pixel at the distances where that node is drawn. **Bold** is the best
in the row. `hyb` is `sat` plus "keep the most important child unchanged in sparse cells".
`mass` and `mm` were also run (`mm` was the worst merge everywhere and was dropped from the
later runs); A-max (today's `gaussian_importance`) was 0.3–2.9 dB below A-area on every run that had it,
which is why nodes are ordered by area, not by the largest axis.

| Asset, view | `tau` | Cut splats (share) | HLOD `sat` | HLOD `hyb` | HLOD `mass` | A-area | C uniform | M Morton (today) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| holzbank, d2 (16 m) | 4 | 156,422 (62%) | **39.4 / 0.954** | 39.4 / 0.954 | 37.7 / 0.943 | 34.1 / 0.940 | 25.6 / 0.822 | 20.0 / 0.741 |
| holzbank, d6 (47 m) | 1 | 156,422 (62%) | 29.4 / 0.905 | 29.4 / 0.905 | 29.0 / 0.897 | **29.7 / 0.904** | 26.1 / 0.792 | 22.5 / 0.716 |
| holzbank, d6 (47 m) | 4 | 12,190 (5%) | 31.1 / 0.927 | **31.1 / 0.927** | 29.2 / 0.913 | 26.4 / 0.884 | 16.9 / 0.497 | 15.3 / 0.498 |
| baum3, d1 (32 m) | 1 | 525,173 (66%) | 46.1 / 0.995 | 46.1 / 0.995 | 45.7 / 0.995 | **47.1 / 0.996** | 40.9 / 0.987 | 40.0 / 0.985 |
| baum3, d1 (32 m) | 4 | 298,483 (37%) | 44.7 / 0.993 | **44.8 / 0.993** | 42.4 / 0.991 | 39.4 / 0.985 | 30.6 / 0.946 | 29.6 / 0.935 |
| baum3, d3 (97 m) | 1 | 308,844 (39%) | 37.2 / 0.973 | 37.3 / 0.973 | 35.8 / 0.964 | **37.8 / 0.976** | 29.7 / 0.909 | 28.5 / 0.886 |
| baum3, d3 (97 m) | 4 | 118,855 (15%) | 32.8 / 0.942 | **33.0 / 0.942** | 30.7 / 0.930 | 31.5 / 0.943 | 23.7 / 0.823 | 24.5 / 0.847 |
| baum3, d8 (258 m) | 1 | 191,798 (24%) | 32.3 / 0.957 | 32.7 / 0.958 | 30.4 / 0.943 | **40.3 / 0.988** | 27.6 / 0.868 | 28.8 / 0.903 |
| baum3, d8 (258 m) | 4 | 59,554 (7%) | 23.8 / 0.755 | 24.0 / 0.758 | 22.4 / 0.721 | **28.7 / 0.896** | 22.7 / 0.651 | 22.9 / 0.704 |
| baum2, d3 (46 m) | 2 | 1,134,205 (14%) | 38.2 / 0.991 | 38.2 / 0.991 | 37.5 / 0.990 | 41.7 / 0.995 | **43.5 / 0.995** | 41.9 / 0.993 |
| baum2, d8 (122 m) | 2 | 441,336 (6%) | 35.1 / 0.978 | 35.1 / 0.979 | 34.2 / 0.975 | **37.0 / 0.980** | 32.1 / 0.953 | 32.2 / 0.953 |

What the table says:

1. **Today's Morton prefix (M) is last or near last everywhere except inside the dense baum2
   view**, and at a 5% budget it falls to 15 dB / SSIM 0.50 on the bench, with most of the
   grass patch missing in the contact sheet. This is #1053, measured.
2. **Uniform thinning (C) loses coverage**: on the bench at 5% it is as bad as Morton
   (16.9 dB / 0.50; the grass patch becomes a see-through speckle). It is competitive only inside dense
   vegetation (baum2 at 46 m), where any surviving splat hides the gap.
3. **Merging (`sat`) is best on surfaces and at coarse cuts:** +5.3 dB over A-area on the bench
   at 16 m and +4.7 dB at 47 m with 5% of the splats, +5.3 dB on the tree at 32 m.
4. **Importance selection (A-area) is best on distant vegetation:** +8.0 dB on baum3 at
   258 m, +3.5 dB on baum2 at 46 m. There, cells hold many small, separated, high-contrast
   splats (blades, leaves, floaters), and one moment-matched Gaussian per cell is a blob where
   the original had texture.
5. **No closed-form rule dominates.** Mean shortfall from the row's best is about 2 dB for
   both `sat` and A-area; the worst is −8.0 dB for `sat` and −5.5 dB for A-area. The opacity
   rule matters: `sat` beats `mass` in every row (0.4–2.3 dB), `mass` over-opacifies stacked
   foliage, `mm` more so. A sparse-cell fallback (`hyb`) gained at most 0.4 dB.
6. **Coverage (driver 3) holds for both candidates.** Where Morton and uniform fall to SSIM
   0.50–0.75, `sat` and A-area stay at 0.88 or above, except baum3 at 258 m with 7% of the
   splats, where `sat` drops to 0.76.

Contact sheets (reference, each method, and |difference| x4) were saved for every row; they
are scratch output and are not committed.

### 5.3 Splats per frame against world size

`k x k` tiles of `synthetic_spiral.ply` (25,000 splats per 10 m tile), 1920x1080, FOV 75°,
camera 9 m above one corner looking 10° down across the diagonal, so the whole world is in
front of it. HLOD cut with no far plane; the flat schemes use today's distance levels
(`d_max = 500`). **Only the 160 m row is a real bake** (6.4 M splats). Every larger world uses
a model built from a real single-tile bake; at the one point where both exist (160 m,
`tau = 2`) the model predicts 4.4 M splats against the real 5.71 M, **23% low**, and 1,320
nodes against the real 442. Model rows are therefore **modelled lower bounds**, and their node
counts are not reported. With one comparison point, "likely under-predicts" is what the evidence
supports; the lower-bound reading is an inference from that point, not a proof.

| World | Splats | HLOD `tau=1` | HLOD `tau=2` | Flat A, far 500 m | Flat A, no far plane |
| --- | --- | --- | --- | --- | --- |
| 160 m (real bake) | 6.4 M | 6.4 M (512 nodes) | 5.71 M (442 nodes) | 4.9 M | 4.9 M |
| 320 m (model) | 25.6 M | ≥ 16.5 M | ≥ 7.0 M | 11.3 M | 11.3 M |
| 640 m (model) | 102 M | ≥ 26.0 M | ≥ 9.5 M | 19.1 M | 24.0 M |
| 1.3 km (model) | 410 M | ≥ 35.5 M | ≥ 11.9 M | 19.4 M | 62.4 M |
| 2.6 km (model) | 1.64 B | ≥ 44.9 M | ≥ 14.4 M | 19.4 M (nothing drawn past 500 m) | 216 M |

Read this as follows:

- Flat schemes are **quadratic** in view distance. With a far plane they stop growing only
  because they stop drawing.
- In the model, the HLOD cut grows by a **constant per doubling** of world size (about +2.5 M
  at `tau=2`): it is **logarithmic**, not constant. The flat-scheme columns are exact counts;
  the HLOD columns beyond 160 m are lower bounds. That is inherent in an isotropic screen-space error over
  a ground plane seen from low height: every octave of distance contributes a constant number
  of nodes of constant angular width. A foreshortening-aware error (§9, stage 2) removes most
  of it; the **budget** caps it absolutely in every case.
- These absolute numbers are high because the test world is unusually dense (250 splats per
  m² and a 9 m camera). They are about scaling, not about a frame-time promise.

### 5.4 Corridor at a 1 GiB budget

`open_world_corridor_20m` (20 M splats; 1,560 grid chunks, mean 12,820, max 22,618), the
`streaming_corridor` camera path for one full 20 s sweep at 30 fps, 1920x1080, FOV 75°, far
500 m (#1130). Bytes are #1088 pages (1,024 splats x 144 B). Residency is an LRU cache of
1 GiB loading at most 18 MiB per frame (today's lane: about 10 loads of a 12.8k-splat chunk),
loading ancestors before descendants. `Residency` is resident needed units / needed units,
the definition PR #1112 gives the lane (not yet on `master`). For HLOD the needed units are the
cut nodes. (Under §6.4's rule a node is refined only when its children are resident, so in
the real design the drawn set is always resident; this simulation predates that rule.)

| Scheme | Splats drawn (mean / max) | Needed MiB (mean / max) | Units (mean / max) | Residency |
| --- | --- | --- | --- | --- |
| Today: whole chunks resident, prefix drawn | 4.46 M / 5.33 M | 1,171 / **1,601** | 637 / 866 | 0.775 |
| A: prefix pages only | 4.46 M / 5.33 M | 660 / 796 | 637 / 866 | 0.924 |
| HLOD `tau=1` | 6.01 M / 7.48 M | 1,054 / **1,310** | 888 / 1,133 | 0.842 |
| HLOD `tau=2` | 3.89 M / 4.52 M | 684 / 794 | 611 / 726 | 0.961 |
| HLOD `tau=2`, budget 2 M | 1.97 M / 2.00 M | 348 / 358 | 323 / 344 | 0.982 |
| HLOD `tau=1`, budget 500 k | 0.50 M / 0.50 M | 91 / 93 | 96 / 105 | 0.996 |
| HLOD `tau=1`, budget 120 k (the lane's cap) | 117 k / 120 k | 23.5 / 26.3 | 33 / 42 | 1.000 |

- These runs used the `mass` merge rule and count **resident cut nodes** only; they do not
  check whether a missing node's ancestor was resident (`residency_ratio_min` was ≤ 0.002 in
  four of the five HLOD runs, at cold start), and they do not model fragmentation or the
  per-frame eviction budget (§6.6 and test 5 cover those).
- **Does the corridor fit 1 GiB with the cut? Yes, from `tau = 2` or with any splat budget.**
  At `tau = 1`, the strictest setting, it does not: 1.28 GiB at peak. Today's whole-chunk
  residency peaks at 1.56 GiB.
- The **lane's cap of 120,000 binds by a factor of about 50** at `tau = 1` (about 37 against
  what today's levels draw). Under the cut it costs
  nothing to honour: the cut coarsens, residency reaches 1.000, and nothing is truncated.
- Interior nodes add **21.0%** splats and **22.4%** pages to the corridor's storage.
- For scale: the coordinator's estimate is that 1 GiB holds 375–480 full-detail chunks today.

### 5.5 Bake cost and size

Prototype bake (numpy, one core, leaves ≤ 16,384, 4:1 per level, `mass` rule; `sat`, `hyb`
and `mm` are within ±10%):

| Content | Splats | SH | Nodes (leaves) | Height | Tree build | Merge bake | Interior overhead |
| --- | --- | --- | --- | --- | --- | --- | --- |
| holzbank | 252 k | 3 | 33 (23) | 3 | 0.07 s | 1.0 s | +19.0% |
| baum-mit-wiese3 | 800 k | 3 | 153 (106) | 10 | 0.56 s | 3.0 s | +19.8% |
| baum-mit-wiese2 | 8.0 M | 3 | 1,210 (852) | 15 | 13.6 s | 45.8 s | +18.3% |
| corridor 20 M | 20.0 M | 0 | 2,725 (1,787) | 7 | 10.9 s | 49.0 s | +21.0% (+22.4% in pages) |
| tiled spiral worlds | 0.1–6.4 M | 0 | — | — | — | 0.2–17 s | +11.9% to +18.0% |

- **Size: about +20%.** A 4:1 reduction per level bounds the interior at 1/3 of the leaves
  in theory; real content lands at 18–22% because coarse levels stop shrinking once nodes are
  small and because pass-through splats are not merged. Page rounding adds about one point.
  The node table is 128 B per node (under 0.01% of the payload).
- **Cost: 3–7 µs per splat in numpy**, roughly linear (20 M splats without SH: 60 s; 8 M
  with SH: 59 s).
  Ordering every node's payload by importance adds a sort per node (7.4 s for 20 M splats in
  numpy). The C++ bake is not written, so its cost is unmeasured; it runs once, at import or
  at `export_world_resource`, never per frame.
- **Depth is content-driven.** baum-mit-wiese2 is 15 levels deep only because far floaters
  stretch its bounds; the cut never visits those chains at normal views.

### 5.6 Transitions (popping)

Same view, two cuts that differ by one step of `tau` (holzbank at 15.6 m, `tau` 2 → 4,
10 nodes replaced by 4; baum-mit-wiese3 at 96.7 m, `tau` 1 → 2, 23 nodes replaced by 8). The
switch is rendered as a hard switch and as three-step fades (t = 0.25, 0.5, 0.75):
`xfade` scales the leaving nodes' opacity by `1 - t` and the arriving nodes' by `t`; `dither`
keeps a hash-chosen fraction of each; `iprefix` keeps the most important fraction of each
(the payload-prefix fade, which needs no shader change). Per-frame change is the worst single
step.

| Transition | holzbank worst step (PSNR / p99.9 abs diff) | baum3 worst step |
| --- | --- | --- |
| Hard switch | 52.2 dB / 11 | 47.3 dB / 20 |
| Opacity cross-fade | **60.8 dB / 4** | **56.4 dB / 7** |
| Hash dither | 51.2 dB / 12 | 51.1 dB / 13 |
| Importance-prefix fade | 50.1 dB / 12 | 45.6 dB / 22 |

- **Only the opacity cross-fade helps:** about 9 dB better per step (part of that is simply
  three intermediate states against one hard step; test 9 also bounds the mid-fade state), and the largest pixel
  step falls by about 3x. Its intermediate states stay close to both ends (holzbank: 54–62 dB
  against the fine state), so the double-transparency dip of a naive cross-fade is small here.
- **Dithered and prefix fades are no better than a hard switch.** Halfway through, each removes
  coverage from both levels at once, and that change is as large as the switch itself.
- A hard switch is already a small change at these thresholds (47–52 dB), because the cut
  switches a node when its error is about `tau` pixels. Hysteresis keeps it from flapping.

### 5.7 Reproducing

The scratch scripts (PLY I/O, tree, bake, cut, the equal-budget thinning, the corridor and
scaling simulations, the render harness and the metric runner) are not committed; they are
measurement code, not production code. The method above is complete enough to rebuild them.
Slices S1a/S1b land the bake in C++ with unit tests, and S2a's and S3's evidence re-run §5.2 and
§5.4 against the real implementation; those, not these tables, are the acceptance numbers.

## 6. Decision

**Accepted 2026-10-02: D, hierarchical LOD, is the alpha architecture**, staged as in §9. The
parts below are normative for stage 1 unless marked *stage 2+*. The maintainer's decisions are
listed in §11 and folded into the subsections named there. This revision also answers the
independent review of `7fa337d4a41` (13 findings); §13 maps each finding to its fix.

### 6.1 Tree: a fixed, world-aligned octree (decision 5)

- **One global lattice.** Every tree lives on a lattice fixed by the **world frame**, not by
  the data: an origin `O` (double precision; the world's coordinate origin, `(0, 0, 0)` unless
  the world sets one) and power-of-two cell edges. A cell is addressed by an **edge exponent**
  `e` (edge = `2^e` m, signed) and **signed 64-bit** indices `(ix, iy, iz)`; its corner is
  `O + (ix, iy, iz) x 2^e`, exactly. The root is the smallest lattice cell that contains the
  content; content outside it **adds parent levels on the same lattice** (new cells with larger
  `e`), it never re-anchors existing cells. Adding a splat therefore changes which cells exist,
  never where a cell's boundaries are, so (a) a world can be baked **out of core** region by
  region and stitched at the top, (b) an edit changes only its own branch, and (c) cell edges
  halve per level, which is what makes a geometric error per level meaningful. k-d trees and
  BVHs (H3DGS uses a per-chunk BVH) give none of these. 64-bit indices cover planetary extents
  down to the smallest cell edge; S1a bounds them to `|i| < 2^52` so a cell centre
  `O + (i + 0.5) 2^e` is exact in double, which still reaches `2^36` m at the `2^-16` m floor.
- **Origin-centred root (ratified 2026-10-04, decision 9).** The planes through `O` are cell
  boundaries at every scale, so content that straddles one of them, as every scan centred on
  its origin does, lies in no single lattice cell, and "the smallest lattice cell that contains
  the content" has no answer. Such a root is the cube `[O - 2^e, O + 2^e)` for the smallest `e`
  that holds every centre. It is stored with node flag bit 4 (`origin_centred_root`) and the
  address `(e, 0, 0, 0)`; its centre is `O` and its edge `2^(e+1)`. Its children are the
  ordinary lattice cells `(e, {-1, 0}^3)`, so everything below the root is the lattice. Its
  grouped and index-split children are addressed by the cube and carry the same flag (one
  encoding per tree; the validator rejects the other). Consumers must take a node's frame from
  the node (address and flag), never from the address alone. Otherwise the root is the smallest
  lattice cell holding every centre.
- **Growth and local edits.** A lattice root survives growth as a child of the new root. An
  origin-centred root grows into a larger cube; its interior children keep their addresses
  (`floor(-1 / 2) = -1`) and become grandchildren. Only its **direct leaves** (including the
  grouped and split leaves addressed by the old cube) are re-addressed or regrouped, which is
  about what a lattice root costs when it is itself a leaf. **Every node at depth 2 or more is
  unchanged by root growth**, and every node off a new splat's path is unchanged by an edit
  inside the root (both are S1a tests).
- **Children are the eight octants.** Empty octants are not stored. Flat content therefore
  occupies four of them and **behaves as a quadtree automatically**, with no data-dependent
  choice of split axis. (The prototype measured in §5 split only along "long" axes of the
  data's bounding box; that rule is withdrawn because it moves every split plane when one
  splat is added. Re-baking §5's numbers under the fixed grid is part of S1a's evidence.)
- **Subdivision rule.** A cell with more than 16,384 splat centres is subdivided. A cell with at
  most 16,384 becomes a leaf. Sibling leaves that together fit 16,384 are grouped into one
  leaf, consecutively in octant order; the grouping depends only on the content of that
  parent, so it stays local to the branch. A **grouped leaf is addressed by its parent cell**
  (flag `grouped_leaf`) and its positions are relative to that parent cell's centre.
- **Coincident centres.** Subdivision stops at a minimum cell edge of `2^-16` m (about
  15 µm, an absolute lattice level), or when all centres in a cell are bit-identical. A cell that still holds more than 16,384
  centres is emitted as an **index-split group**: its splats, ordered by
  `(Morton code at full precision, source index)`, are cut into `ceil(N / 16,384)` sibling
  leaves that share the cell's bounds and carry the flag `split_by_index`. A group of more than 8
  leaves gets intermediate interior nodes on the same cell (also `split_by_index`), 8 children
  at most per node. **The bake never emits a node over 16,384 splats (16 pages, within #1134's
  64-page cap).** Interior nodes are bounded the same way (§6.3).
- **Node bounds come from the node's own payload** (centres plus three sigma of each splat,
  after merging and inflation), **united with the children's bounds**, so a parent always
  contains its children (conservative for culling and for the error distance). They are
  stored node-relative, rounded outward with the same double arithmetic the loader's validator
  uses, so containment holds exactly; the sphere is about the AABB centre. Culling and the
  error distance use these; the cell address is only for structure.
  The loader rejects radii below any stored AABB half-axis extent or the distance to
  a child's AABB centre plus that child's radius. This is not a requirement to
  enclose every AABB corner: an isotropic three-sigma sphere is smaller than its box
  half-diagonal. S1's resident leaf-chunk adapter conservatively enlarges its runtime
  sphere to at least the AABB half-diagonal, as the streaming adapter already does.
  It also accounts for rounding of the emitted float centre at large coordinates:
  exact stored and runtime AABB corners are bounded about that centre, the serialized
  sphere is padded by its centre displacement, and the runtime radius rounds upward.
- **World chunks are not the leaves.** Today's 10 m grid chunks reach 22,618 splats; the bake
  partitions by the octree instead.

### 6.2 Node payload and positions (decision 6)

- Every node stores **≤ 16,384 splats**, contiguous in the file and **ordered by importance
  inside the node** (`opacity x sigma_a x sigma_b`, the A-area order of §5.2). A prefix of
  whole pages is therefore an importance subset of that node, not a coverage guarantee.
  The historical pinned-upload preview (§6.6) is never the LOD mechanism and does not
  qualify a node for the integrated drawn set (§14.2).
- Interior payloads are plain splats in the same 144 B layout. The renderer cannot tell a
  merged splat from an original, so a better bake is a re-import, not a format or runtime
  change.
- **Positions are node-relative from stage 1.** A splat's position is stored as float32
  relative to the **centre of its node's cell** (the parent cell for a grouped leaf); the
  cell's position is exact (integer lattice address, double origin).
- **Why float32 is enough, for any cell.** The storage error is about `edge x 2^-24` (the
  float32 resolution at half the edge, plus overhang). A node is drawn only when its error
  `eps ≥ edge / 64` projects to at most `tau` pixels, so the storage error projects to about
  `tau x 64 x 2^-24 ≈ 4e-6 x tau` pixels, **independent of the cell's size and of the world's
  extent**. For orientation, with a 128 m leaf cell (an example, not a rule; sparse leaves can
  be larger):

  | World extent | Absolute float32 (today) | Node-relative float32, 128 m leaf cell | Interior node, 16 km cell |
  | --- | --- | --- | --- |
  | 1 km | 61 µm | ≤ 7.6 µm (15 µm once overhang passes 64 m) | — |
  | 100 km | 7.8 mm | same | ≤ 1 mm, against an `eps` ≥ 250 m |
  | 10,000 km | 1 m | same | same |

  Absolute float32 is visible at 100 km for close-up detail and unusable at planetary scale.
  Quantized node-relative positions (the 80 B path) are a stage-2 compression on the same frame
  of reference.
- **GPU frames (finding R1 of the follow-up review).** Per-node constants and per-draw values
  are kept apart, because `ChunkMetaGPU` is indexed by chunk and **shared by every instance of
  an asset** (`depth_compute.glsl:139` reads `chunks[visible_chunk.chunk_id]`; the instance
  comes separately from `visible_chunk.instance_id`):
  - **Per node, shared (S2a):** the node's cell centre in **asset space** goes into three of
    `ChunkMetaGPU`'s four unread words (`lod_level` at `renderer/gaussian_gpu_layout.h:197`,
    written but unread, and `pad0..2` at `:200-202`), so the struct stays 64 B.
    `depth_compute` forms the asset-space position as `centre + relative` and applies the
    instance transform as today. That is exact enough for assets up to tens of km.
  - **Drawn list in S2a (before `DrawnNodeGPU` exists).** For HLOD content the CPU skips
    `frustum_cull.glsl` and uploads the drawn set itself into the existing
    `visible_chunk_buffer` as today's 8 B `VisibleChunkRefGPU {instance_id, chunk_id}`
    entries (`renderer/gaussian_gpu_layout.h:222-225`), in the cut's deterministic order, and
    writes the visible-chunk count that `compute/instance_chunk_dispatch.glsl:26` reads to size
    the indirect dispatch. No GPU atomic decides which nodes are drawn. S2b widens that same
    entry into `DrawnNodeGPU`.
  - **Per drawn entry (S2b):** a `DrawnNodeGPU` record per entry of the drawn set, replacing
    today's 8 B `VisibleChunkRefGPU` for HLOD content: instance id, chunk id, the
    **camera-relative offset** `R s c + T - camera` (computed on the CPU in double, stored as
    float32) and the S6 **fade** factor; 32 B per drawn node, written by the CPU in the cut's
    deterministic order. With it, `depth_compute` applies the instance rotation and scale to
    the node-relative position only, adds the offset, and runs the view transform, frustum
    planes, distance cull, wind and effectors in **camera-relative space**. That shader rewrite
    is S2b's scope (§9).

### 6.3 Interior nodes: two candidates, chosen per node (decision 2)

- **Merge cell and error.** Each interior node is built from its children. Its merge cell
  `eps` starts at `cell edge / 64` and at least twice the children's `eps`, and is raised (by a
  factor of 1.25 per step, as the prototype) until the merged candidate is at most a quarter of
  the children's total and at most 16,384 splats. `eps` is stored as the node's **geometric
  error**. The merge cells are anchored at the node's cell corner, so they are world-aligned too.
- **Merge cells never mix DC encodings.** `sh_dc` is decoded as `dc + 0.5` (`linear_rgb`) or
  `1.5 sigmoid(dc) - 0.25` (`legacy_bias`, `shaders/includes/gs_sh_binning.glsl`), and worlds
  merged from several assets carry both, so the encoding is part of the cell key: splats with
  different encodings are never averaged together.
- **Candidate `merged` (`sat`).** Splats with `2 sigma_max >= eps` pass through unchanged.
  Every other cell becomes one splat by **moment matching** (H3DGS): weights
  `w = alpha x sigma_a x sigma_b`; mean, covariance (`Sigma_i + (mu_i - mu)(mu_i - mu)^T`), DC
  and SH as `w`-weighted means. Opacity: alpha mass `M = sum alpha_i 2 pi sigma_a sigma_b`
  against the merged capacity `C = 2 pi Sigma_a Sigma_b`; if `M > C` the two largest axes are
  inflated by `k = min(sqrt(M / C), 1.25)`, then `alpha = min(0.99, 1 - exp(-M / C))`.
  **A cell with a single child is copied unchanged (identity).** The prototype measured in the
  first version of §5.2 applied the formula to single-child cells too, which maps a lone
  splat's 0.9 to 0.59; that was a bug, and §5.2 was re-measured with it fixed (S1 implementer
  question (d)).
- **Candidate `selected`.** Importance selection **from the full-detail leaves**, which is
  exactly the A-area method §5.2 measured (S1 implementer question (a)): the node keeps as many
  splats as the merged candidate; each descendant leaf contributes a share proportional to its
  splat count (largest remainders, ties by leaf id), and the share is the **prefix of that
  leaf's importance-ordered payload**. Because leaf payloads are already ordered, building the
  candidate is a copy of prefixes, O(node size).
- **The error metric `E`.** Both candidates are rendered against the node's **reference**, the
  union of its children's payloads (what the cut shows when it refines the node), by the bake's
  own small CPU splat rasterizer (front-to-back compositing, SH evaluated per view, fixed
  order), orthographic. **Rule 3 (S1b selector revision, stack PR 12):** a node is drawn whenever
  its geometric error projects to at most `tau` pixels, and `tau` is a runtime budget, so one
  stored decision has to serve every `tau` the ADR evaluates (1, 2 and 4 px, §5.2). Each
  candidate is therefore rendered at the pixel size **`eps / tau` for `tau` in {1, 2, 4}**
  (rule 2 used `tau` = 1 only, which is blind to the finer pitch at which a node is shown under
  `tau` = 4, where a sparse subset exposes holes that a coarse pixel hides). The three per-`tau`
  errors are combined by their **geometric mean**, because the acceptance gate reports PSNR, a
  logarithm of the error: choosing the lower geometric mean minimises the expected dB loss over
  the budgets. Each per-`tau` error is floored at the MSE of 8-bit output rounding alone,
  `1 / (12 * 255^2)`, since smaller errors are invisible and would send the logarithm to minus
  infinity. The stored `error_chosen`/`error_rejected` are these geometric means, so the rule
  version is 3 and rule-2 worlds are not comparable. The colour space is deliberately
  **unchanged**: the splat raster output is an sRGB-encoded display-referred buffer that the
  compositor decodes to linear and the tonemapper re-encodes, so the blended values the proxy
  compares are already in the space the gate's sRGB PSNR measures (viewport_blit.glsl,
  `source_decode_srgb`). The view set (S1 implementer question (b)) is **22 views**: the 6 axis
  directions, the 8 cube-corner directions (oblique), and 8 grazing directions at 10°
  elevation every 45° of azimuth, because the dominant open-world view is grazing and on flat
  content the axis views see it only edge-on or top-down. `E` is the mean over views and pixels
  of the squared difference in premultiplied RGBA, so holes and colour errors both count. The
  lower `E` wins; within 1%, `merged` wins (the tie factor is unchanged).
- **Cost against the 2x limit (estimate).** The reference is rendered once per view and shared
  by both candidates; per node that is about `(4 + 1 + 1) x 22 ≈ 130` splat-renders per merged
  splat, or about 26 per source splat once the ~20% interior share is applied, at a few pixels
  each. The grazing and oblique views are rendered at half resolution (they are the
  coverage check; the axis views carry the colour detail), which brings the estimate to about
  the cost of the merge itself. **Canonical imports, the current public bake path and runtime
  edit rebakes retain the limit of at most 2x the merge-only bake.** For those routes, if it is
  exceeded, the pre-declared order of reductions remains: drop the oblique corner views to 4,
  then lower the grazing resolution again; if 2x is still missed, the numbers go to the
  maintainer (decision 4's rule, §8). **Only explicitly selected offline prepared-content jobs
  use the accepted §15 contract in place of this relative cost limit.** They retain the
  original 22-view metric and all quality requirements; the finite job limits and their
  enforcement must be implemented and validated before route-specific S1b acceptance.
- **Storage.** Each node record carries a 2-bit representation kind (`leaf`, `merged`,
  `selected`, reserved), the chosen and the rejected candidate's `E` (float32 each), and the
  file carries `bake_rule_version`. The renderer ignores all of it; it exists for diagnostics,
  the visual gate, and re-deciding only the nodes whose margin was small.

### 6.4 The cut and the drawn set (findings 2, 4, 10)

The cut is computed **on the CPU** each frame, over all instances at once (§6.9). What it
produces is the **drawn set**, and every budget and cap applies to the drawn set, never to an
ideal cut. For the integrated route, §14.2 adds a complete initial frontier before this
refinement loop; a root that does not fit must not short-circuit that search.

- **Refinement order.** Start from the top-level tree's roots (§6.9); **top-level nodes are
  expanded first, ahead of every asset node and regardless of priority**, down to the visible
  instance roots. Then pop the frontier node with the largest **priority** `p = e x b`, where `e = eps x f_px / max(dist - radius, epsilon)` is
  its projected error and `b = h` (1.25) if the node was refined in the previous frame, else 1
  (this damps flapping at the budget frontier). Ties are broken by `(instance key, node id)`
  (§6.9), never by RID or scene order.
- **A node is refined only if** (1) **`e > h x tau`, or it was refined in the previous frame
  and `e > tau`** (threshold hysteresis, h = 1.25; with S6 the band `tau < e ≤ h x tau` is a
  fade instead, §6.7); (2) **all its frustum-visible children are resident and complete** (§6.6); and
  (3) the drawn set after the refinement still fits **every cap** in §6.5.
- **A node that fails (2) is drawn itself and the loop continues.** No ancestor is ever drawn
  for a missing descendant, so a region is never drawn twice: **the invariant is that no drawn
  node has a drawn ancestor**, except the two levels of a fade pair (§6.7). Residency requests
  for the children are still issued (§6.6).
- **The loop stops at the first cap refusal (3).** The drawn set is then a prefix of a fixed
  refinement order, so for a fixed residency state **raising a budget never makes any node
  coarser**. (The first version allowed later, smaller refinements to proceed after a refusal;
  the review's counterexample showed that is not monotonic per node, so it is withdrawn. The
  cost is some unused budget, measured in S2a.)
- **History.** Because of `b`, the drawn set depends on the previous frame's drawn set as well
  as on camera, tree, residency and budgets. It is deterministic, and for a static camera it
  reaches a fixed point within two frames from any prior drawn set (test 2); the same pose
  reached along different paths may show different drawn sets, which is accepted.
- The cut replaces `update_chunk_lod_parameters` and the frustum chunk scan for HLOD content;
  cost is O(visited nodes), not O(total chunks) (#320).
- **`tau` is a setting (decision 3):** `rendering/gaussian_splatting/lod/hlod_tau_px` in the
  settings manifest, with a per-node override on `GaussianSplatWorld3D`; `quality/lod_bias`
  multiplies it. Its default is set by the maintainer from S3's A/B (test 11); provisional 1 px.

### 6.5 Every GPU cap, and never truncate (findings 2, 3, 12)

The drawn set must fit every capacity on the GPU path. Each is either **bounded by the cut**
or a **declared residual** with an always-on counter that test 1 asserts is zero.

| Capacity | Where | Treatment |
| --- | --- | --- |
| Visible splats | `max_visible_splats`, `depth_compute.glsl:225-227` (Channel B) | **Bounded.** The effective splat budget is `min(quality/max_splat_count, max_sort_elements, allocated buffer capacity)` (the resident publisher already applies `sort_cap`, `resident_instance_contract_publisher.cpp:865-868`). `max_visible_splats` is sized **once** from it, not per frame. The drawn set's count, including fade pairs and prefixes, is ≤ it. |
| Visible chunks / nodes | `max_visible_chunks`, `frustum_cull.glsl:143`, `overflowed_chunks`; sized at `render_streaming_orchestrator.cpp:1803-1808` | **Bounded.** The drawn node count, fade pairs included, is a cut cap; the buffer is sized from the configured node cap. For HLOD content the list is written by the CPU in the cut's order (S2b's `DrawnNodeGPU`), not appended by GPU atomics. |
| Global overlap records | `max_overlap_records`, tile binning (overflow ADR Channel A, #54) | **Admission estimate.** Each node bakes a footprint `F = sum (6 sigma_a)(6 sigma_b)` (m²); the cut keeps `sum (count + F f_px² / (d² x tile_area))` ≤ 80% of the effective record budget. The distance `d` is the distance to the near surface of the node's bounding sphere, floored at 0.25 x the sphere radius. The floor is active whenever the camera is closer than 1.25 x the radius to the node centre: inside the sphere and in the shell just outside it, where it lowers the estimate against the plain near-surface distance (holzbank z = 6.5: 916,997 to 592,368 records, against 458,641 measured). It is continuous in the camera position and does not change the estimate beyond the shell. Without it a camera inside a sphere gets an unbounded estimate and no frontier is ever admitted (1e17 records at 2 m inside the holzbank bench against 5.5e5 measured). This is an admission estimate without a guaranteed upper bound, so `overflow_splats_clamped` stays the tripwire. |
| Per-tile raster records | `GS_MAX_RASTER_SPLATS_PER_TILE` = 12,288 per 16 px tile under the default `gpu_preset="high"` (#1137) | **Residual.** It depends on depth complexity per tile, which node-level estimates cannot bound. HLOD lowers it where #1137 bites (far, dense content becomes fewer splats), but merged splats are larger, so S2a's evidence reports peak records per tile at the A/B views. Its drop must be counted and warned (#1137's fix); test 1 asserts it is zero. |

- **Historical alpha admission fallback.** Every instance's asset root node is pinned
  (§6.6). If the visible instance roots alone exceed a cap, the original S2a contract drops
  whole instances **farthest first**, ties by instance key, with `WARN_PRINT_ONCE` and a
  counter. This counted loss remains an explicit legacy fallback; it is not complete
  coverage. Prepared worlds opting into §14 instead require a complete initial frontier
  and explicit admission failure when none fits. This amendment does not change the
  existing legacy route or waive its loss counters.
- **Content without a tree** (a `.gsplatworld` v1 not re-imported) has no coarser
  representation. Its fallback moves into **S2a** (it was S4): whole chunks are dropped
  farthest first, ties by `(instance key, chunk id)`, with `WARN_PRINT_ONCE` and a counter.
  S2a claims #1131 fixed for all content only with this in place.
- **Remaining order dependence, fixed in the slice that exposes it:** the sort tie-break is
  `atlas_index` (`depth_compute.glsl:234-235`), which instances of one asset share, so equal
  depths fall back to atomic order; S5 makes it `(instance key, atlas index)`. The wind seed is
  `atlas_index` (`:172`), which changes when a node is re-admitted elsewhere in the atlas; S3
  makes it `(node id, index in node)`.

### 6.6 Streaming nodes into #1134's pages (finding 5)

- **The streaming unit is a tree node.** The page allocator and byte budget are #1134's.
- **Whole-run admission.** A node's whole run (≤ 16 pages) is allocated on admission and its
  pages upload in payload order. An immutable node payload never changes size; refinement
  between levels uses a different node and run. In §14, edits create a new version with a
  separate run; overlapping old/new versions count against the budget until retirement.
- **The protected closure.** The byte budget is checked against the **allocated pages** of the
  closure `pinned ∪ drawn ∪ ancestors(drawn) ∪ fade pairs ∪ in-flight runs`, not against the
  drawn set alone. Ancestors stay resident because a refined node's parent must be available
  when the camera moves back. Only nodes outside the closure may be evicted.
- **Pinned levels.** The original alpha plan pins every asset's root and the top levels
  down to the first level whose total payload exceeds 5% of the budget. Its partially
  uploaded pinned-node prefix is a loading preview, not proof of a non-empty *complete*
  first frame. In the integrated route (§14.2), the complete bootstrap frontier and required
  fallback closure must fit and finish validation/upload before readiness; a partial
  prefix never enters its drawn set. The 5% heuristic cannot override that requirement.
- **Requests.** For every node the ideal cut would refine but (2) of §6.4 blocked, its missing
  children are requested, in refinement-priority order, within the per-frame upload byte
  budget.
- **An unplaceable run is skipped, not waited for.** If no contiguous run fits after evicting
  nodes outside the closure (within the frame's eviction budget), the request is skipped for
  this frame and the parent keeps being drawn (adopting #1135's skip). After `R` (= 8)
  consecutive skips of the same node the streamer runs **incremental compaction**, at most a
  fixed number of pages per frame, until the largest free run fits. Compaction may move
  **protected** nodes too: a node is copied to its new run with a GPU buffer copy, stays drawn
  from the old run until the copy has completed, and its `atlas_base` switches between frames;
  the old run is freed after the switch (both runs count against the budget meanwhile).
  **If test 5 still fails, fixed 16-page slots are the outcome** (no fragmentation, at the cost
  of page waste for small nodes); the format does not change either way.
- **Eviction priority:** outside the closure first, then least recently drawn, then finest
  level first.
- **Accounting** comes from the allocated run, as in #1134.

### 6.7 Transitions: cross-fade (decision 4, slice S6; finding 10)

- **One band.** A node's band is `tau < e ≤ h x tau` (h = 1.25). Without S6 it is the
  threshold hysteresis of §6.4 (1): refine above `h x tau`, stay refined while above `tau`.
  With S6 the switch becomes a blend over the same band: the node and its
  children are drawn together with `t = (e - tau) / ((h - 1) x tau)`, children at opacity
  `x t`, the node at `x (1 - t)`. `t` is a function of the camera, so a parked camera holds a
  static blend and the oracle still reads 0 frames differing.
- **A fade starts only when the children are complete** (§6.4 condition (2)).
- **Under a budget,** fades are admitted after all full refinements, in priority order, while
  the pair fits every cap; a fade that does not fit is drawn as its coarse node (t = 0), i.e. a
  hard switch.
- **Plumbing and cost.** The fade factor is per **drawn entry** (a parent and its fading
  children, and two placements of one asset, need different values), so it lives in S2b's
  `DrawnNodeGPU` record (§6.2), not in the shared `ChunkMetaGPU`. It has to reach the place
  where splat opacity is read, which today knows only the splat's instance and atlas index, so
  S6 adds the **draw-entry index** to the per-splat reference: about **4 B per visible splat,
  ~20 MB at 5 M visible splats**, plus the layout-sync guard update. That is the R3 shader
  change of S6.
- §5.6 measured a fade of three intermediate states against one hard step, so part of its
  ~9 dB advantage is simply more, smaller steps. Test 9 therefore also bounds the mid-fade
  state against both ends.

### 6.8 The resident route (decision 4, slice S5)

The resident route bakes the same tree at import, keeps every node resident (+18–21% VRAM,
§5.5) and joins the same global cut, so it **honours the splat budget** like the streaming
route. `max_splat_count` then means the same on both routes, which resolves #1131's second
inconsistency. The #420 importance clamp is retired in the same slice.

### 6.9 Instances: one global budget and a top-level tree (decision 7)

- **One cut for everything.** All instances on both routes share one drawn set, so the splat
  budget, the node cap and the byte budget are **global**.
- **Instance key.** Every instance has a stable key, a hash of the asset's resource UID and the
  instance's stable placement identity (its node path, or its index in a world's instance
  table), not its RID or scene order. It breaks ties in §6.4 and in the sort (§6.5).
- **Top-level tree.** Instances are placed in their own world-aligned octree by their
  world-space bounds (the same grid rule as §6.1). Its leaves reference instance roots (the
  root node of the instance's asset tree, transformed); its interior nodes carry aggregated
  bounds and, in stage 1, **no payload**. Having no payload, they have no error or priority:
  the cut **expands them first, unconditionally** (§6.4), down to the visible instance roots,
  and only then starts the priority loop. Its cost is O(visited top-level cells + cut), not
  O(instances).
- **Error under a transform.** An instance's node error is scaled by the instance's uniform
  scale, and distances are taken in world space.
- **Stage 2+: instance proxies.** Far groups of instances need merged proxy payloads in the
  top-level cells (true HLOD across instances). The top-level node record already has the
  payload fields, so proxies need no layout change. They do need a **reader** change: S1a's
  validator refuses top-level payloads and non-leaf kinds (fail closed), so files with proxies
  must be gated (a format version or a `bake_rule_version` the reader checks), decided when
  proxies are designed.

## 7. Format and version bumps

- **`.gsplatworld` v2** (`kWorldVersion` 1 → 2, `io/gaussian_splat_world_io.cpp:24`; the loader
  rejects any other version at `:541`). Note (2026-10-05): v2 is now taken by #1172 (layout
  fingerprint word in the header), so S1a bumps to the next free version and its reader must
  accept every earlier one; re-read the live constants when implementing:
  - header: flag `kFlagHasHlod` (next free bit after `kFlagResidentPayload`, `:38`); the
    lattice origin `O` (3 x float64, from the world frame); `bake_rule_version`;
  - a **node table**, **128 B per node**: lattice address (edge exponent `e` as int8, signed
    int64 `ix, iy, iz`), tight AABB (node-relative, 6 x float32) and sphere radius, geometric
    error `eps`, parent index, first child and child count, payload first splat (64-bit) and
    count, height, flags (representation kind, `split_by_index`, `grouped_leaf`), the chosen
    and the rejected candidate's `E` (float32 each), and the remaining bytes (about 30)
    reserved for stage-2 fields (per-node SH degree, quantization block, overlap footprint);
  - an optional **top-level instance table** for worlds that place instanced assets (§6.9):
    instance key, asset reference, transform, and the top-level node it hangs from;
  - the gaussian payload **reordered so every node is contiguous**, each importance-ordered,
    with **node-relative positions** (§6.2). The v1 per-chunk index lists (`:425-469`) are not
    written; they forced scattered reads per chunk.
- **Delta for S1a against the format described at `a1da9be37bf`** (the S1 implementer is
  coding against that head):
  1. the cell address is `(e: int8, ix, iy, iz: int64, signed)` on the global lattice, not
     `(level, ix, iy, iz: uint32)` from a data-sized root;
  2. the header drops the root edge exponent `k`; `O` comes from the world frame;
  3. the node record is **128 B**, not 96 B;
  4. new flag `grouped_leaf`: a grouped leaf is addressed by, and its positions are relative
     to, its parent cell;
  5. the minimum cell edge is the absolute lattice level `2^-16` m, not `S0 / 2^24`;
  6. new reserved field for the per-node overlap footprint (§6.5), which S2a fills.

  The per-draw `DrawnNodeGPU` record, the per-node centre in `ChunkMetaGPU`, the instance
  rules and the threshold hysteresis are **runtime only** and do not touch the file.
- **Recorded in S1a (#1149)**, beyond the delta above:
  - `kFlagHasHlod` is header flag bit 6. The v2 header is the 104 B v1 header (version 2,
    chunk fields zero) plus an 80 B extension: `O` (3 x float64), `bake_rule_version`, the
    interior splat count, and offset and count of the node, top-level node and instance
    tables, then two reserved words.
  - Sections: gaussians (leaf payload, then interior payload), SH in the same order, node
    table, top-level node table, instance table (128 B records), metadata.
    Every present section must fit the file, start after the 184 B header and be disjoint
    from every other section. The compressed Gaussian extent includes its size prefix.
    Disjoint reordered sections remain readable; the writer's section order is not a loader gate.
  - Before publication, every node's leaf or interior payload is checked for finite render/SH
    fields and three-sigma containment in its stored AABB and sphere. Streamable files use
    at most one node of validation scratch, not a resident world copy. The bound comparison
    allows only node-relative serialization rounding (half a float ULP plus double arithmetic
    rounding). Reconstructed node centres, AABB endpoints/sizes and payload positions must fit
    finite runtime coordinates. Flagged metadata must be read completely and parse as a UTF-8
    JSON dictionary; malformed metadata is an error, not an empty dictionary fallback.
  - The header world AABB becomes the engine instance AABB through `GaussianSplatWorld3D`, so it
    must enclose the root node's world-space AABB. The writer unites the world's bounds with the
    root, rounded outward to float32, and `bake_hlod()` publishes the same bounds on the live world.
    The loader refuses non-finite, negative or non-enclosing bounds.
  - Node flags: bits 0–1 the representation kind, bit 2 `split_by_index`, bit 3 `grouped_leaf`,
    bit 4 `origin_centred_root` (§6.1).
  - **Reserved bytes:** the node record's 32 reserved bytes are the stage-2 extension area, so
    readers ignore them. Reserved words in the header and the instance record have no designated
    use and must be zero. `overlap_footprint` is finite and `>= 0` (0 until S2a).
  - **Worlds without a tree are still written as v1**, byte-identical, so `.gsplatcache` files
    and tree-less exports do not change.
  - The leaf section is still handed to today's runtime in absolute float32 (resident loads
    convert; file-backed loads apply a per-leaf frame in the payload source) until S2a draws
    node-relative positions.
  - Importer: v2 sources are fully validated before they can replace a previous import; an
    in-place import (destination is the source) is copied, not baked; **a failed bake falls
    back to the plain tree-less copy with a warning** (`hlod_skip_reason = "bake_failed"`),
    so a world that imported before the bump still imports. While baking, the importer holds
    three full-sized payload copies (source, snapshot, bake working set), plus merged nodes,
    scratch and allocator overhead, then two full-sized copies while saving. This is copy
    accounting, not a peak-RSS bound. The actual Windows editor importer at `50254cdace5`
    used a peak working set of 8.11 GiB on the 8M SH3 scan (2.304 GB source payload,
    Gaussian and SH sections combined; 3.78 times that payload). A rejected file-backed
    `bake_hlod()` does not materialize or otherwise publish a replacement world, and a
    successful bake publishes its complete state before notifying observers.
  - The 2D flag is carried through the bake and the format, not refused: the PLY loader sets
    it for any PLY with normals, which standard 3DGS exports carry, and no shader reads it.
  - A tree records its leaf payload's content revision; the saver refuses a tree whose payload
    was edited in place afterwards (its merged nodes would be stale).
- **The v2 loader still reads v1.** `.gsplatcache` files are written through the world saver
  (`io/ply_loader.cpp:98-105`), so v1 caches exist on disk; rejecting them would only force a
  re-parse, but accepting them is free. v1 content renders through the tree-less fallback
  (§6.5).
- **Version numbers (decision 10, 2026-10-04).** HLOD takes `.gsplatworld` v2 and world importer
  v3. The colour-encoding ADR's DC-contract slice (#1056), which had planned the same numbers,
  takes the next ones when it lands.
- **`ResourceImporterGSplatWorld::get_format_version` 2 → 3**
  (`io/resource_importer_gsplatworld.h:27`). Today the importer copies the file
  (`io/resource_importer_gsplatworld.cpp:327`); from v3 it bakes v1 sources into a v2 imported
  copy. The source file is not touched.
- **Runtime-built worlds are not baked implicitly (S1 implementer question (c), agreed by the
  coordinator).** `GaussianSplatContainer::export_world_resource`
  (`nodes/gaussian_splat_container.cpp:128`) keeps producing a v1-shaped world. S1a adds an
  opt-in **`GaussianSplatWorld.bake_hlod()`**, and the corridor lane calls it after export
  (S2a makes that change to the lane, so the A/B of test 11 runs on a baked world).
- **PLY / SPZ importers** bump in S5, when the resident route adopts the tree: PLY
  `get_format_version` 11 → 12 (`io/resource_importer_ply.h:103`), SPZ 8 → 9
  (`io/resource_importer_spz.h:61`). `PLY_CACHE_VERSION` (3, `io/ply_loader.cpp:35`) does not
  change. Re-read these values when implementing; intervening importer PRs move them.

## 8. Test plan

Each item names the slice that lands it (§9). **Thresholds are never lowered silently
(decision 4):** if a criterion fails, the implementer first improves the implementation; if it
is still not reachable, they report the numbers to the maintainer for an explicit decision.

1. **Static-frame flicker oracle at a binding cap (S2a, the #1131 acceptance test).** #1131's
   Run A: the corridor-style world (baked with `bake_hlod()`), static camera, cap 120,000
   against ~320,000 visible demand; wait for streaming idle (180 frames without a load),
   capture 300 frames. **Pass: 0 frames differ from frame 0, and none of
   `instance_count_overflow_events`, `overflow_splats_clamped` (tile binning and, once #1137
   counts it, the per-tile raster cap) or `overflowed_chunks` moves.** **Non-vacuity:** S2a's
   `hlod_budget_bound_frames` counter must rise by 300 in the window, proving the budget bound.
   Base must fail (299/300 today). A second run parks the camera inside a fade band (S6).
2. **Cut properties (S2a; the top-level item in S2b; CPU unit tests):**
   - determinism: the same inputs give the same drawn set across runs and insertion orders;
   - every cap holds for the **drawn set**, with fade pairs and prefixes counted;
   - no drawn node has a drawn ancestor (outside fade pairs);
   - **monotonicity:** for a fixed residency state and history, raising any budget never makes
     a node coarser (true by construction of the stop-at-first-refusal rule; a property test
     with randomized trees checks it);
   - **fixed point:** a static camera reaches the same drawn set within 2 frames from at least
     five different prior drawn sets;
   - **top level:** payload-less top-level nodes are always expanded before any asset node, and
     no visible instance is left undrawn under a binding budget unless the visible instance
     roots alone exceed a cap, in which case the legacy fallback drops instances farthest
     first, counted; integrated-route coverage additionally requires §14.2 admission;
   - **threshold hysteresis:** a node at `tau < e ≤ h x tau` keeps its previous state.
3. **Bake unit tests (S1a):** deterministic output; **no node over 16,384 splats**, including
   a fixture with 40,000 coincident centres (index-split group); node bounds contain the
   node's own payload at three sigma; moment matching preserves the weighted mean and second
   moment **before inflation**, and inflation is at most 1.25x per axis; a single-child cell
   is copied bit-exactly; interior overhead ≤ 35% on the fixtures; the world grid is stable
   (adding one splat changes only the cells on its path); node-relative positions round-trip
   within float32 resolution of the cell; a v2 file round-trips; a v1 file still loads.
4. **Importer re-import (S1a):** a v1 `.gsplatworld` re-imports once after the bump and gains
   a tree; the source file is unchanged; `bake_hlod()` on a runtime-built world gives the same
   tree as the importer. Ordinary source-path loading must resolve the editor-generated
   `.import` sidecar to that tree, while direct format-loader calls must still validate the
   raw source. After an actual editor import, run
   `godot --headless --path <project> --script <repo>/tests/runtime/test_hlod_import_remap.gd -- <source> <imported-artifact>`.
   This manual integration test checks public dispatch, explicit v2 loading, a sidecar-free
   raw-v1 control and source preservation; it is not a substitute for the guard lane.
5. **Mixed-size thrash test (S3):** a synthetic tree with node sizes from 1 to 16 pages, a byte
   budget at 60% of demand, a camera sweeping back and forth. Every requested node is admitted
   within **N frames, where N is computed before the run** from the configured upload bytes per
   frame, the eviction budget and the compaction rate; the atlas never exceeds its budget;
   no frame blocks on an unplaceable run.
6. **Coverage under streaming (S3):** cold start with the budget binding; no frame has a
   node-shaped hole (coverage mask against the warm frame) **and** no frame draws a node
   together with one of its ancestors outside a fade pair. Report loading previews
   separately; a prefix is not a passing coverage frame. The integrated route also checks
   readiness, including an inside-root camera, against §14.2 before accepting its first frame.
7. **Corridor lane (S3):** `vram_cap_hit_frames <= 0` at 1 GiB, `residency_ratio >= 0.70`,
   every overflow counter of test 1 at zero, with GPU evidence.
8. **Visual gate on real scans (S2a):** §5.2 re-run on the three scans against the real
   implementation. **Thresholds are fixed now, from the §5.2 table committed in this ADR**
   (the head that is merged), **not from the run they judge** and not from S1a's re-run on the
   world grid: in every row, the implementation (the per-node choice) must reach PSNR ≥ the
   better of the `sat` and A-area columns minus 1.0 dB, and SSIM ≥ that column's SSIM minus
   0.01. All rows, baum2 included, are post-fix values.
9. **Transitions (S6):** a dolly through several switch distances at 60 fps. The worst
   per-frame step with the cross-fade must be at most half that of the hard switch, **and**
   every mid-fade frame must be at least as close to both endpoints as the endpoints are to
   each other (no double-transparency dip larger than the switch itself).
10. **Resident route (S5):** with `max_splat_count` below the atlas, the drawn count equals the
    drawn set's count, the flicker oracle reads 0, and two instances of one asset at equal
    depth render identically across 300 frames (sort tie-break).
11. **`tau` A/B (S3 acceptance, decision 3).** On the corridor lane and the three real scans,
    run `tau` = 1 px and 2 px, each **unbudgeted and with the byte budget binding**, and report
    from the engine, not a simulation: **allocated VRAM** (atlas pages in use x page bytes, and
    `vram_device_buffers_mb`), **residency** (`residency_ratio` and its minimum), **FPS**
    (frame time p50 / p95), **quality** (§5.2's region-of-change PSNR / SSIM against full
    detail at fixed views), **transition smoothness** (test 9's measure; hard switches if S6
    has not landed, stated) and peak records per tile (#1137). S3 is not accepted without this
    table; the maintainer sets the default from it.
12. **Per-node choice (S1b, decision 2):** the §5.2 rows re-run with the per-node choice, in the
    prototype harness (a test-only cut that writes the drawn set as a PLY and renders it in the
    engine, since S2a's runtime cut does not exist yet). In every row it must be within 0.5 dB
    of the better of `sat` and A-area, or better; the choice is deterministic. The bake with the
    choice takes at most 2x the merge-only bake for canonical imports, the current public bake
    path and runtime edit rebakes. Only explicitly selected offline prepared-content jobs
    replace that relative cost criterion with the finite, predeclared and enforced contract
    in §15. Offline S1b acceptance before package B requires the implemented job contract
    and §15.3 steps 1–2, plus every unchanged S1b quality gate: the original 22 views,
    SH0–4/DC semantics and every quality row. All other tests remain required at their
    respective stages. If the 0.5 dB criterion fails, the implementer improves the metric or
    the candidates first; if it is still not reachable, the numbers go to the maintainer for
    an explicit decision (decision 4).

Single-vendor blind spot: all GPU evidence so far is NVIDIA. Atomic order differs by vendor,
which is why the oracle must pass by construction (no atomic decides membership), not by luck
on one GPU.

## 9. Staged slice plan

One PR per slice, each stating its immutable base SHA. A stack also states its base PR;
review covers the fixed diff. These are the original alpha slices, not a completion report.
The integrated large-world sequence is §14.7 and retains their acceptance dependencies.

### Stage 1 — the alpha (eight PRs)

| Slice | Content | Risk | Size (estimate) |
| --- | --- | --- | --- |
| **S1a** | Format v2 (node table, instance table, header) + bake core: world-aligned octree on the global lattice (signed 64-bit addresses, 128 B node record) with index-split groups and grouped leaves, `sat` merge with the single-child identity, node-relative positions, node bounds from payload, importance-ordered contiguous payload; v1 → v2 in the world importer; opt-in `GaussianSplatWorld.bake_hlod()`; tests 3–4 | R3 | ~1,900 LOC + ~1,000 LOC tests |
| **S1b** | Per-node choice: `selected` candidate from leaf prefixes, CPU rasterizer, 22-view metric, representation kind and errors in the record; test 12 | R3 | ~900 + ~500 |
| **S2a** | Global CPU cut over all instances (flat instance list), drawn set, every cap of §6.5, stop-at-first-refusal, threshold and priority hysteresis, the `tau` setting, per-node asset-space centre in `ChunkMetaGPU` (node-relative positions drawn), `max_visible_splats` sized once, the tree-less and over-cap instance fallbacks, `hlod_budget_bound_frames`, corridor lane calls `bake_hlod()`; **closes #1131**; tests 1, 2 (without top level), 8 | R3 | ~1,600 + ~1,000 |
| **S2b** | Top-level instance tree with first-expansion rule; per-draw `DrawnNodeGPU` record written by the CPU; **camera-relative shader rewrite** of `depth_compute` (instance R·s on the relative position, offset add, view transform, frustum planes, distance cull, wind and effectors in camera space); test 2 (top level) and a far-from-origin precision test | R3 | ~1,400 + ~800 |
| **S3** | Node streaming on #1134: whole-run admission, protected-closure accounting, pinned levels, requests in priority order, skip + bounded retry + incremental compaction, eviction priority, stable wind seed; tests 5–7 and the **`tau` A/B (test 11) as an acceptance criterion** | R3 | ~1,400 + ~900 |
| **S4** | Telemetry: cut size, depth histogram, fade-band size, compaction and skip counts | R2 | ~200 + ~150 |
| **S5** | Resident route: bake at PLY/SPZ import (format bumps), all nodes resident, joins the global cut, instance-key sort tie-break, retire the #420 clamp; test 10 | R3 | ~900 + ~550 |
| **S6** | Cross-fade: one band, `t` from the error, fades admitted after refinements, fade factor in the per-draw `DrawnNodeGPU` record (S2b), draw-entry index in the per-splat reference, layout-sync guard; test 9 | R3 | ~800 + ~450 |

Stage 1 is **about 9,100 LOC of production code and 5,350 LOC of tests over eight PRs, seven
of them R3.** The growth since `a1da9be37bf` (~8,600 + 5,050 over seven PRs) is the
camera-relative shader rewrite and the per-draw record (follow-up review R1), the top-level
expansion rule, and splitting S2 into S2a and S2b for reviewability. S1a, S1b, S2a and S2b are
sequential; S6 can land after S2b in parallel with S3. The tree is as deep as the
content needs; there is no fixed number of levels.

### Stage 2+ — refinements that need no rebuild

- **Foreshortening-aware error** (§5.3): projected node height instead of an isotropic radius.
- **Better bakes:** the per-node error against the full-detail leaves instead of the children,
  more candidate rules, distillation against renders of the full-detail subtree (the full
  detail is the teacher, so no training views are needed). Re-bake only.
- **Instance proxies** in the top-level tree (§6.9). Bake only; the fields are reserved.
- **GPU cut** if CPU traversal shows up in profiles at billions of splats.
- **Compression:** quantized node-relative payloads (the 80 B path) and per-level SH degree
  reduction.
- **Out-of-core bake** for worlds that do not fit in RAM, stitched at the top levels.
  The lattice remains reusable, but bounded paged startup and preparation now require the
  explicit V4 contract in §14.4; the old assumption that no format change is needed does
  not cover that extension.

### What would force a rebuild if stage 1 were done more cheaply

- **Per-draw values in a per-asset record** (`ChunkMetaGPU`): instances of one asset would
  share a camera offset and a fade (follow-up review R1).
- **Option A (flat importance prefix) as the format.** No merged nodes, so the screen-bound
  requirement is unreachable (§5.3).
- **A fixed number of levels baked as separate per-level files.** Billions of splats need an
  unbounded depth.
- **Keeping the per-chunk index lists.** Node streaming needs contiguous node payloads.
- **Enforcing the budget on the GPU** (atomics, a depth-histogram cutoff, #1131's option 1).
- **A data-dependent split rule** (bbox midpoints of the data, k-d, BVH): it prevents
  out-of-core and incremental bakes (finding 1, decision 5).
- **Absolute float32 payload positions:** billion-splat worlds spanning tens of km would need a
  payload format change (decision 6).
- **Per-instance budgets or a cut that iterates over all instances:** a joint budget and a
  top-level tree would need a second cut design (decision 7).
- **Budgets checked against the ideal cut instead of the drawn set:** it reopens #1131 through
  ancestor fallbacks (finding 2).
- **Merged splats in a different layout from leaves:** every later compression or shader change
  would have to be done twice.

## 10. Reviving the deleted code

**Not worth reviving; reuse the idea, not the code.**

- `SplatClusterer` (deleted in `de8fc43e2c6`, 1,079 + 287 lines) merges colour only (no SH),
  averages rotations by slerp and scales by per-axis RMS **while also** computing a merged
  covariance it never uses for the output shape (its own comment calls it "a simplified
  approach"), uses cluster radii in absolute world units (2/4/8/16 m by level), and is written
  against a per-splat `GaussianData` value type that no longer exists. Its opacity rule is the
  `mm` rule of §5.2, which measured worst.
- `StreamingLODManager` (1,237 + 309 lines) kept one GPU buffer per LOD level for the **whole
  dataset**, so it cannot stream regions, and it was never wired (`de8fc43e2c6` removed it as a
  dead path).
- `AdaptiveLODSystem` (`7205f2d10ef`, 763 + 275 lines) selected splats on the CPU each frame
  from a `Vector<GaussianData>`, which the GPU-driven pipeline cannot use.

What carries over: moment-matched covariance merging (§6.3, from H3DGS) and the idea of a
budget-driven selection, done here by the cut.

## 11. Maintainer decisions (2026-10-02)

1. **Accepted: HLOD (option D) is the alpha architecture.**
2. **Merge rule: chosen per node at bake time** between `sat` and importance selection, by the
   lower error (§6.3, §7).
3. **`tau` is a setting, not fixed now.** S3's acceptance includes a real A/B at 1 px vs 2 px on
   the corridor and the real scans (test 11); the maintainer picks the default from it.
4. **In the alpha: cross-fade transitions (S6) and the resident route (S5).** And: **no
   threshold is lowered silently.** If test 12's 0.5 dB criterion fails, the implementer
   improves the implementation first; if it is still not reachable, the numbers go to the
   maintainer for an explicit decision. (The maintainer's note said "the 0.5 dB cross-fade
   criterion"; the 0.5 dB criterion is test 12's per-node choice, so the rule is recorded there
   and applied to every threshold in §8, including test 9's.)
5. **Split rule: a fixed, world-aligned octree** (fixed origin, power-of-two cells, integer cell
   coordinates); flat content becomes a quadtree through empty octants; coincident centres
   become index-split groups, and the bake never emits a node over the 16,384-splat cap (§6.1).
6. **Positions are node-relative in stage 1** (§6.2, with precision at 1 km, 100 km and
   10,000 km).
7. **Instances: one global budget across all instances and a top-level hierarchy over
   instances, both in stage 1** (§6.9).
8. **S1 implementer questions:** (a) `selected` is built from the full-detail leaves, the method
   §5.2 measured; (b) 22 views (6 axis, 8 oblique, 8 grazing) within the 2x limit, with a
   pre-declared reduction order; (c) no implicit bake in `export_world_resource`, an opt-in
   `GaussianSplatWorld.bake_hlod()` plus the importer bake, agreed by the coordinator; (d) the
   `sat` single-child mapping was a prototype bug; single-child cells are now the identity and
   §5.2 was re-measured. The later offline-only cost disposition is recorded in §15
   (2026-10-25); the other requirements above remain unchanged.

9. **Origin-centred root ratified (2026-10-04)** for content that straddles a plane through the
   lattice origin (§6.1).
10. **Version numbers (2026-10-04):** HLOD takes `.gsplatworld` v2 and world importer v3; #1056
    takes the next numbers (§7).

Follow-up review of `a1da9be37bf` (one P1, two P2, six P3): resolved in §6.1, §6.2, §6.4–§6.7,
§6.9, §7 (with the exact S1a format delta), §8 and §9; see §13. The S1a implementation review
(#1149) is recorded in §6.1, §6.3, §6.9 and §7.

Still open, not blocking stage 1: whether the corridor lane keeps its 120,000 cap, which binds
by about 50x and under the cut means "draw a coarse world"; S3's A/B data informs it.

## 12. What was not verified

This section records the limits of the original §5 investigation. It is not an inventory of
current code or completed PRs. The integration acceptance requirements are in §14.8.

- **No integrated implementation was measured by that investigation.** Its quality numbers
  came from rendering prototype outputs through the engine; its bake was a Python prototype.
- **The prototype predates the world-aligned grid (decision 5):** §5's trees split at the
  data's bounding-box midpoints. S1a re-runs §5.2 and §5.5 on the fixed grid.
- **Not prototyped at all:** the per-node choice and its 22-view metric (cost in §6.3 is an
  estimate), hysteresis, the byte-budget refusal, the drawn-set and closure rules, the
  error-band fade (§5.6 used three fixed steps), node-relative positions, the per-draw
  record and the camera-relative shader path, the top-level instance tree and the
  overlap-record estimate. `hlod.cut` in the prototype has only the
  tau test and a splat budget.
- **The corridor and scaling simulations used the `mass` rule** and count resident cut nodes
  only; they do not check ancestor coverage (`residency_ratio_min` ≤ 0.002 in four of five
  HLOD runs) and do not model fragmentation or the per-frame eviction budget.
- **Scaling beyond 6.4 M splats is modelled**, and the model under-predicts the one real point
  it was checked against by 23% (§5.3).
- **Bake times** are numpy on one core; the C++ bake is unmeasured.
- NVIDIA RTX 3090 only.

## 13. Review response (independent review of `7fa337d4a41`)

| # | Finding | Resolution |
| --- | --- | --- |
| P1-1 | Split rule is data-dependent; >16,384 coincident centres break the cap | §6.1: world-aligned octree, index-split groups (decision 5); test 3 |
| P1-2 | Drawn set ≠ cut; ancestor fallback double-draws and breaks the bound | §6.4: refine only when all visible children are complete; caps on the drawn set; §6.6 closure; tests 2 and 6 |
| P1-3 | Only Channel B covered | §6.5 table: every cap bounded or a counted residual (incl. #1137); test 1 asserts all counters |
| P2-4 | Monotonicity claim false; hysteresis under a binding budget | §6.4: stop at first refusal (monotone), priority hysteresis, history stated; test 2 fixed-point check |
| P2-5 | Byte accounting is not placement feasibility | §6.6: closure accounting, skip + bounded retry + compaction; test 5's N pre-computed; byte-bound rows in test 11 |
| P2-6 | `selected` ≠ measured method; axis-only views; tau scale | §6.3: `selected` from leaves (= A-area), 22 views incl. grazing, pixel scale `eps / tau_bake`, failure path pre-declared |
| P2-7 | §5.3 160 m row was modelled; model node counts off | §5.3 corrected to the real bake; model rows and the billion-scale figure labelled as a modelled lower bound; PR heading fixed |
| P2-8 | Test integrity (8, 1, 3) | Test 8 thresholds fixed from §5.2; test 1 non-vacuity counter in S2; test 3 asserts moments before inflation |
| P2-9 | Missing rebuild risks (instances, precision, split rule) | §9 list extended; decisions 5–7 put all three in stage 1 |
| P3-10 | Fade band vs hysteresis; fade budget; weak test 9; S6 cost | §6.7: one band, fades after refinements, mid-fade bound in test 9, 4 B per visible splat stated |
| P3-11 | Node bounds from source splats | §6.1: bounds from the node's own payload; test 3 |
| P3-12 | Residual nondeterminism (sort tie-break, wind seed, tree-less content) | §6.5: fixes assigned to S5 and S3; the tree-less fallback moved into S2 |
| P3-13 | §12 completeness; S1 size; test 12 rendering | §12 extended; S1 split into S1a/S1b; test 12 uses a test-only cut writing a PLY |

Follow-up review of `a1da9be37bf`:

| # | Finding | Resolution |
| --- | --- | --- |
| R1 (P1) | Camera offset and fade in the per-asset `ChunkMetaGPU` | §6.2: per-node asset-space centre stays in `ChunkMetaGPU` (shared, correct); offset and fade move to a per-draw `DrawnNodeGPU` record; camera-relative shader rewrite is S2b's scope and size |
| R2 (P2) | Payload-less top-level nodes; undrawn instances | §6.4/§6.9: top-level nodes expanded first; every instance root pinned; over-cap instance roots drop farthest first, counted; test 2 |
| R3 (P2) | Two thresholds; hysteresis gone | §6.4 (1) and §6.7: refine above `h x tau`, stay refined above `tau`; S6 blends over the same band |
| R4 (P3) | Data-derived lattice; grouped-leaf address | §6.1: global lattice from the world frame, root growth adds parents, signed 64-bit indices; grouped leaves addressed by the parent cell |
| R5 (P3) | Precision framing | §6.2: relative argument (`~4e-6 x tau` px for any cell); 128 m labelled an example |
| R6 (P3) | Test 8 column and baseline | Test 8: per-node choice ≥ best of (`sat`, A-area) − 1.0 dB from this ADR's committed table; baum2 rows replaced before merge |
| R7 (P3) | Compaction cannot move protected nodes | §6.6: protected nodes move by copy-then-switch; fixed 16-page slots named as the outcome if test 5 fails |
| R8 (P3) | "Modelled lower bound" on one point | §5.3: "likely under-predicts" |
| R9 (P3) | S2 size | S2 split into S2a (closes #1131) and S2b (instance tree, camera-relative space) |

## 14. Integration amendment: streamed, locally editable worlds (2026-10-25)

The maintainer approved this extension for staged implementation. HLOD chooses a complete
representation; streaming supplies its versioned node payloads through the existing page
allocator, upload path and renderer. They share one residency contract. A second independent
streamer, a whole-world resident copy and a permanently resident full-detail edit world are
outside this design.

The first capacity acceptance target is a Cartesian world spanning **at least 100 km**, with
**at least 200 million physically stored full-detail SH3 splats**, plus interior payloads,
and a working target of **about one million drawn splats**. Logical instance multiplication
and metadata-only fixtures do not count as stored capacity. SH0–4 compatibility remains
required; an SH0 capacity run cannot substitute for SH3. The renderer performance target is
120+ native FPS: 8.33 ms for a complete game frame. 1440p and a 3–5 ms GS GPU allocation are
planning assumptions, not measurements or acceptance evidence. Neither the §5 prototypes
nor a CPU oracle establishes those capabilities.

### 14.1 Identity, sets and shared residency

The following terms describe internal contracts, not new registered classes or bound APIs:

| Term | Contract |
| --- | --- |
| `NodeKey` | Stable world/asset identity plus stable node identity; independent of atlas address, array order and request lifetime. Instance placement has a separate stable key. Deleted identities are never reassigned to unrelated content. |
| `NodeVersion` | Immutable payload and bounds for a `NodeKey`, identified by content revision and bake-rule revision. A parent records the exact child versions it represents. |
| `Desired(view)` | Quality-driven target nodes and missing dependencies for one view. May include absent nodes; supplies demand, never drawable addresses. |
| `Resident` | Globally shared, validated and completely uploaded node versions whose GPU lifetime is valid. Allocated or partly uploaded runs are accounted for separately and are not resident-ready. |
| `Drawn(view)` | Complete, non-overlapping cut for an admitted view, made only from eligible resident versions, with budgeted fade pairs as the sole ancestor/descendant overlap. |

The selection input is an immutable snapshot of hierarchy/content revision, active views,
transforms, origin revision, residency and budgets. Each view has its own cut, history and
draw records. There is one physical residency budget across views and instances; shared
payload storage is charged once, while each view's sort, record and draw allocations are
charged where they are actually used. Adding a view must reserve its resources or return
an explicit admission result; it cannot silently degrade another view to an incomplete cut.

Protection is the union over all admitted views of drawn versions, valid fallback coverage,
fade pairs, in-flight work and edit reservations. It includes old and new allocations during
copy/upload/compaction and resources awaiting GPU retirement. Budget checks use allocated
bytes, including SH and allocator rounding, not just selected splat counts. An invalidated
parent can remain allocated for old GPU work but is no longer a valid fallback (§14.5).

A request carries node key/version, world-generation identity and request generation;
publication additionally checks the device generation and upload completion. Reuse the
streamer's existing generation, upload-sequence and retirement boundaries. Cancellation or
an obsolete revision prevents publication; it does not free memory still referenced by I/O
or GPU work. A late completion cannot resurrect an unloaded world, overwritten node or old
device. Rejected completions are counted in two places: `late_dropped_cancelled_total`
(`hlod_stream_late_dropped_cancelled_total`) for the completion of a request that was already
cancelled (once per ticket; a duplicate delivery is loss), which is harmless because the cancel released the node, and `late_dropped_total`
(`hlod_stream_late_dropped_total`) for every other rejected completion, which is real loss and
must stay zero in a gate. Compaction changes an address only after its copy completes and all draw references
switch coherently; stable identities, tie-breaks and wind seeds do not change.

### 14.2 Complete initial frontier and readiness

Before refinement, find a complete frontier for the admitted visible domain. Payload-less
hierarchy nodes are expanded; every visible branch is represented by one eligible payload.
This applies when the camera is inside a root and when a root payload alone exceeds a cap:
bounded deterministic search must consider a complete child frontier that may fit before
declaring failure. Each frontier obeys the splat, node, record and memory limits. The
overlap estimate in §6.5 is still an estimate; measured overflow remains a failing tripwire,
not an exact mathematical bound supplied by this amendment.

Only after establishing this frontier does §6.4's refinement order apply. Its stop at the
first budget refusal and its history/monotonicity tests remain requirements. Initial-search
work has a configured bound and reports exhaustion separately from proof that no frontier
fits. Neither result may produce a partial cut labelled complete. Test the transition between
initial frontiers under changing budgets as well as subsequent refinements; do not assume
the refinement proof also proves initial-frontier monotonicity.

Readiness has explicit outcomes: loading, ready with a complete coarse representation,
not admitted by the configured budgets, or failed validation/I/O. These are semantic states,
not a proposed enum binding. The ready transition requires the entire bootstrap frontier
and its fallback resources to be validated, uploaded and reserved. An uploaded prefix may
be exposed as loading progress by the legacy route, but never satisfies readiness or
coverage. Counted whole-instance drops in §6.5 remain legacy behavior and fail the integrated
coverage contract. If even the minimum complete representation cannot be admitted, report
that condition rather than silently omitting regions.

Refine only when all replacement children required for visible coverage are ready and the
whole transition fits. On a miss, cancellation or failed replacement, retain the valid old
cut. At camera/view changes, establish coverage for the new domain before declaring it ready;
keeping a previous view's cut is not evidence of coverage of newly exposed regions. Views
with exhausted budgets report their state without corrupting already admitted views.

### 14.3 Precise world frame and node-local rendering

Keep global placement as a precise CPU representation: double origin/translation plus the
integer lattice address, with node-local float payloads. Import and preparation must retain
source precision before conversion; casting an already rounded absolute float to double
cannot restore it. The first expansion uses a flat Cartesian frame, not planetary curvature.

S2b's per-draw camera-relative offset is computed in double from that representation and
only then converted to GPU float. Payloads shared by multiple instances/views never store a
view-dependent offset. Depth, culling, SH view direction, sorting, wind and effectors use
consistent frames; host/shader layout checks cover the per-draw contract. No fp16-centre
optimization is implied. The conditional precision estimate in §6.2 is not a substitute for
near-detail tests at large offsets, especially for budget-forced coarse nodes or inside-root
views where its projected-error premise need not hold.

The origin context belongs to the Gaussian Splatting module. Changing it never repositions
unrelated scene nodes; external gameplay and physics origin handling uses explicit
integration hooks. Every origin change increments an origin revision. Draw/cull snapshots and
caches that depend on that frame include the revision and cannot mix old and new offsets.
Stored node-local payloads and stable edit identities do not change on an origin shift.
Two simultaneous views, one perspective and one orthographic, repeated origin shifts and
unload/reload at 0, 1 km and 100 km must preserve the expected relative geometry and
deterministic ties at equal quality.

Implementation status of package C (C1 and C2 landed in code; the GPU evidence and the guard removal are open). C1:
`core/gaussian_splat_world_frame.{h,cpp}` holds the precise representation (int64 lattice address plus double
offset), `WorldOriginContext` (owned by the scene director, revision per change, explicit `OriginShiftHook`) and the
double per-draw offset `camera_relative_offset`; runtime nodes carry a source-precision `precise_center`; `ViewSet`
takes the origin and its revision in the `SelectionSnapshot`, drops histories and the published set on a revision
change, and publishes the per-draw offset in each `DrawRecord`; the cull-config reuse signature includes the
revision. C2: the HLOD route writes one 32 B `DrawnNodeGPU` per drawn node (`instance_id`, `chunk_id`, reserved S6
`fade`, `camera_offset`; `renderer/gaussian_gpu_layout.h`, mirrored in `gs_instance_layout.glsl` and the layout-sync
guard). With `GS_HLOD_RELATIVE_PAYLOAD`, `depth_compute.glsl` and `tile_binning.glsl` form
`p_cam = R s p_node + camera_offset` (`shaders/includes/gs_camera_relative.glsl`; the instance translation is part
of the offset and is not added again) and work in that frame: rotation-only view transform (`w = 0`), frustum
planes of the camera-translated frustum, distance cull and SH view direction from a camera at the origin, wind phase
and sphere effectors through a `frame_anchor` (the scene-frame camera) so they keep their scene-frame meaning. The
node centre in `ChunkMetaGPU` is no longer read by any shader. Every frame-dependent cache of the route is keyed on
`origin_revision` (`InstancePipelineBuffers::hlod_origin_revision`: tile binding identity, instance sort-reuse key;
plus the existing ViewSet histories and cull signature). The `hlod_world_origin_shifted` refusal stays: no
shifted-origin frame has been drawn on a GPU, so the guard is removed only after the end-of-stack evidence
(far-origin near-detail captures, two simultaneous views, an origin shift on a real device).

Camera contract (C1): the scene camera is a float transform, so the camera's precise position is the origin plus
that float. A float camera 100 km from the origin is already rounded to millimetres; the origin must therefore be
kept near the camera, and shifting the origin is what keeps the camera precise. `ViewSpec::camera_precise` carries
an absolute camera when a source with more precision exists. "Source precision" here means the node cell centre
(tree origin plus integer lattice address); splat positions are float at the source.

### 14.4 V4 paged packages and bounded preparation

The approved new package design is **V4**, with a paged hierarchy/index, a bounded bootstrap
directory and independently verifiable node payloads. Its implementation task must confirm
that the version number is still free and specify exact byte layouts before coding. It must
not reinterpret V3 bytes. Released legacy formats keep their strict validation and loading
behavior; early-start semantics are a distinct opt-in format/content path.

File-wide counts and byte offsets are checked unsigned 64-bit quantities. Every range
operation checks overflow, file bounds, section/page overlap, record sizes, schema/SH degree
and configured decode limits before allocation or reading. Index pages and payload records
carry integrity metadata and explicit sizes; integrity checks do not replace semantic
validation of bounds, topology, finite fields and parent/child versions. Validate each page
before exposing its entries and each complete payload before admission. Malformed later
pages fail their requests with a retained valid fallback, or explicit unavailability if
none exists; they cannot be reported as a fully validated world.

Startup reads the header, bounded directory, index pages needed for the initial frontier,
and that frontier's payloads. Full leaf traversal is not a readiness prerequisite. The
readiness report therefore means the admitted representation is valid, not that every
unread byte is valid. Budget and measure index-cache bytes and startup I/O separately from
payload residency. A file source must keep a stable file/content generation so replacement
on disk cannot mix pages from different package versions.

Preparation is out of core: bounded leaf input batches, bounded merge/quality work queues,
paged index output and bottom-up parent production. The selected candidate must still use
the full-detail descendant leaves (§6.3), with bounded reads and stable leaf ranges/IDs;
substituting selected children would change the quality contract. Leaf-range metadata and
its validator belong here, not in a prerequisite-only PR disconnected from their consumer.
SH0–4, DC encoding, source preservation and all S1b quality gates remain in force. The
accepted §15 contract replaces only the relative ≤2× cost gate for explicitly selected
offline prepared-content jobs; its finite limits, failure controls and publication rules
must be implemented and validated. Canonical imports, the current public bake path and
runtime edit rebakes retain their existing contracts.
Preparation reports peak RAM, scratch disk, bytes read/written and elapsed time; a hidden
whole-world `GaussianData` materialization is a failure. Existing canonical imports retain
their behavior; the optimized prepared-package workflow is explicit.

#### 14.4.1 Package E1: implemented V4 slice (version 4 is free; byte layout)

Status: the first slice of package E is implemented; the rest of §14.4 is not (see "Not in E1"). The version number 4
was free (`gs_world_format::select_header` refuses it, so no v1-v3 reader can mistake a V4 file for its own). V4 is
written only by the explicit `GaussianSplatWorld.save_hlod_paged()`; saving, importing and baking keep writing v3.

All integers little endian, every offset a checked unsigned 64-bit quantity, every region 4096-byte aligned and laid out
back to back: header page, index pages, payload records, optional metadata. Reader code: `io/gaussian_splat_world_paged.*`.

| Region | Bytes | Content |
| --- | --- | --- |
| Header | 4096 | magic `GSPW`, version 4, flags (2D, metadata), page/index geometry constants, Gaussian struct layout version, leaf/interior/node counts, SH degree and counts, bake rule, lattice origin, bounds, region extents, declared file length, `content_generation`, SHA-256 of the whole index, SHA-256 of the metadata, SHA-256 of the header itself. All unused bytes must be zero. |
| Index page | 65536 each | 64-byte page header (magic `GSPI`, page, first node, node count, SHA-256 of its own entries) + 255 node records of 256 bytes. The unused tail must be zero. |
| Node record | 256 | v3 topology (cell, flags, bounds, errors, parent/child links, payload range) + footprint (f64), DC encoding, homogeneous-DC and centre-exact flags, payload record offset / stored size / page-rounded size, SHA-256 of the payload record. Reserved words must be zero. |
| Payload record | page aligned | 64-byte header (magic `GSNP`, node, count, SH high-order count, byte sizes, `content_generation`, payload range, struct layout) + `count * 144` Gaussian bytes + `count * high_order * 12` SH bytes. Positions are node-relative exactly like the v3 gaussian section. |
| Metadata | optional | UTF-8 JSON dictionary (same acceptance rule as v3). |

Contracts implemented:

- **Fail closed, distinct reasons.** Open validates the header (magic, version, digest, geometry, ranges, declared vs
  actual length: `v4_truncated_file`, `v4_trailing_bytes`), every index page (identity, digest, zero tail), the index
  digest, the whole topology with the same `gs_hlod_validate_tree` the v2/v3 loader runs, and the strict sequential
  layout of the payload records. Counts and extents are range checked with overflow-safe arithmetic before anything is
  allocated or read. A hash never replaces a semantic check: a re-sealed file with a bad parent link, a non-finite
  position or a footprint that disagrees with its payload is refused by the semantic rule (tests re-seal on purpose).
- **Bounded reads.** A node payload is exactly one record of at most `64 + 16384 * (144 + 12 * 21)` bytes, read by one
  `read_node_payload()` call that opens its own handle (thread safe). The node streamer additionally caps the CPU working
  set of reads in flight (`Config::max_inflight_read_bytes`, default 8 MiB, one read always allowed): the cap, not the
  tree, bounds payload memory.
- **Versioning / replacement.** Every payload record carries the header's `content_generation`; a read also checks the
  file length. A file replaced on disk under an open package is refused (`v4_file_changed_length`,
  `v4_content_generation_mismatch`) instead of mixing versions.
- **Cancellation and late completion** reuse D1's tickets and generations unchanged: a read is started only for a still
  pending ticket, a result of a cancelled, unloaded or other-device ticket is dropped at the upload gate, and
  `shutdown()` waits for the reads in flight. A failed read fails that node (D1's FAILED/retry/permanent states), never
  publishes a partial payload, and never affects another node.
- **Metrics move to the index.** Footprint, DC encoding, homogeneity and the exact-reconstruction check were the reason
  the D1 runtime snapshot scanned (and held) the whole payload. A paged snapshot takes them from the index and holds no
  payload; each read re-derives them from the bytes and compares (`v4_payload_metrics_mismatch`).

Not in E1 (deferred): lazy loading of index pages and a bounded bootstrap directory (E1 reads the whole index at open, at
most 256 B per node, then validates it); early coarse startup; out-of-core preparation (the converter needs the resident
world); leaf ranges; instanced trees; editor import of V4 (the importer refuses version 4, load the file directly); a
sort/tile scratch that no longer scales with the whole tree (GPU memory is unchanged by E1); physical 100 km / 200M
acceptance (§15.3 step 3).

### 14.5 Persistent edits in a reserved active area

The first mutable scope is discrete game edits within a prepared near area, with an
individual tool or small explosion affecting a region of **at most 5 m radius**. Larger
operations require coordinated subtransactions and explicitly defined commit boundaries.
Continuous world-scale deformation, networking and collision simulation are outside this
stage. Rendering exposes coupling hooks; it does not invent collision geometry.

Before accepting an edit, resolve its stable affected IDs and reserve the complete
replacement cut, payload copies, upload overlap, journal space and bounded rebuild work.
Reservation includes siblings/descendants needed to cover regions whose ancestor will become
invalid, across all admitted views, not just splats geometrically inside the tool radius.
If the active area or any reserve is unavailable, defer or reject explicitly before commit;
never partially apply the edit to fit memory.

Each accepted edit creates immutable new leaf versions. At the atomic publication boundary,
the replacement cut is complete and ready, and every ancestor representing an old affected
child version becomes ineligible for new draws and fallback. Old GPU submissions may finish
and retire normally; no submission after the confirmed edit may redisplay the old content.
Retain an eligible complete cut until bottom-up rebuilding produces new parents from the
exact child versions and passes the same bake/quality validation. A failed or superseded
parent job preserves that cut and its reservation; it does not revive the stale parent.
Subsequent edits require fresh admission, so persistent rebuild failure cannot grow memory
without bound. The latency to prepare/commit and the time until coarsening is restored are
separate measurements, with no promise of an unmeasured instant rebuild.

Persistence uses an append-only, versioned journal keyed by stable world/node/splat identity,
with expected prior revisions and complete transaction boundaries. Array indices and atlas
addresses are not identities. Durable commit precedes success acknowledgement and visible
publication; crash recovery replays complete commits idempotently and ignores/reports
incomplete tails without exposing half an edit. Unload/reload resolves the committed versions
before becoming ready. Checkpoint/compaction preserves identity and committed revisions;
parent payloads are replaceable derived data and must match the recovered children.

### 14.6 Public integration contracts

The implementation will bind a small API only after its R3 compatibility review. These are
required operations/results, not currently available method names:

| Boundary | Required inputs and observable results |
| --- | --- |
| World opening | Explicit strict legacy or prepared-package route; progress, generation, readiness/admission result and validation/I/O failure. |
| Views and budgets | Stable view/instance identity, precise frame and configured budgets; admitted state, per-view cut counts and global allocated/reserved/retired bytes. |
| Edit admission | Region, stable operation/target IDs and expected revisions; accepted reservation token, deferred reason or rejection with no changes. |
| Edit commit/cancel | Single-use transaction identity; durable committed revision or no commit; cancellation cannot undo an acknowledged commit. |
| World-change hooks | Committed transaction, affected global bounds and new revisions for gameplay/collision consumers; origin-revision notification without rewriting stable content IDs. |

Hook delivery must distinguish durable commit from GPU presentation and background parent
rebuild. A collision consumer may track its own readiness; rendering readiness does not
claim collision completion. Legacy alpha APIs and resource loading remain compatible.

### 14.7 Dependency-ordered implementation packages

Each package receives its own task contract, immutable base, worktree and focused PR; large
packages may be split into dependent PRs with the same acceptance boundaries. Reuse existing
work after checking its current source and review state. This table is a dependency plan,
not a second status tracker or permission to bypass an open gate. Design acceptance,
including the scoped offline cost disposition in §15, an existing experimental runtime path
or a passing CPU oracle does not establish S1b acceptance or authorize advancement to
dependent runtime slices. S1b must pass test 12 for the content route used: before B, the
offline route requires the implemented §15 job contract validated by §15.3 steps 1–2 plus
every unchanged S1b quality gate. Physical scaling in §15.3 step 3 is the later mandatory
E/G capacity acceptance, using the V4 implementation from E and integrated runtime from G.
Packages B–G also retain their applicable runtime gates.

| Package | Minimal coherent scope and dependency | Required evidence before advancing |
| --- | --- | --- |
| **A — architecture and baseline** | This ADR amendment; separately establish a discriminating renderer baseline. | Independent review of this contract; baseline control fails for the intended defect and candidate exercises the real path. A failing/non-discriminating baseline blocks evidence that relies on it. |
| **B — global selection** | Shared identity/budget contracts, per-view Desired/Drawn sets and complete initial frontier; follows A and accepted S1b for the content route used. Extend S2a using the CPU oracle. | S1b test 12 remains prerequisite: the offline route requires its implemented job contract, §15.3 steps 1–2 and every unchanged S1b quality gate; S2a tests 1, 2 and 8, including inside-root and cap-pressure controls. CPU tests alone do not satisfy GPU or real-scan gates. |
| **C — precise frame** | Precise placement, origin revision and per-draw node-local renderer contract; follows B and maps to S2b. | Far-origin/multi-view/origin-shift tests plus host/shader and runtime/GPU review; preserve SH0–4, ties and geometry. |
| **D — hierarchical residency** | Feed selected node demand into the existing allocator/upload path; whole transitions, cancellation, protection, eviction and compaction; follows C. Start with strict existing files. | S3 tests 5–7 and mandatory test 11, including old/new GPU lifetime controls; report loading, ready and failure separately. |
| **E — large prepared packages** | V4 paged index/payloads, early coarse startup and bounded preparation, including leaf ranges; follows D. | Legacy compatibility, corrupt-page/overflow controls, bounded I/O/RAM, real physical files and first complete-frame captures; physical preparation scaling under §15.3 step 3. All S1b quality gates and the applicable preparation-cost contract (§8 test 12 and §15) still apply. |
| **F — editable regions** | Reserved 5 m edits, immutable revisions, stale-parent exclusion, bottom-up rebuild and durable stable-ID journal; follows E. | Budget exhaustion/rejection, superseded/failed rebuild, crash recovery, multi-view and edit→unload→reload controls, including actual rendered changed regions. |
| **G — integrated capacity and performance** | Complete file→coarse→detail→edit→new coarse→unload→reload run; follows B–F and their gates. | Physical 100 km / 200M SH3 acceptance and matched full-frame/RAM/VRAM/quality measurements (§14.8), completing §15.3 step 3; no unmeasured performance claim. |

S1a → S1b → S2a → S2b remains sequential. S5's resident-route and stable-tie requirements
remain; S6 follows S2b and may progress alongside S3 with disjoint ownership. Telemetry from
S4 is implemented where the measured contracts need it. Apart from the explicit offline-only
cost substitution in §15, splitting the integration or enabling prepared packages changes
none of the original alpha acceptance criteria.

### 14.8 Acceptance and stop conditions

Use small real scans for coverage/quality and physical large packages for capacity; report
them separately. Record exact base/head and binary, hardware/driver, asset hashes/SH degree,
paths, views, resolution and quality semantics. Match those inputs for A/B comparisons.
Measure cold startup, moving views, steady state and the entire edit/reload cycle:

- CPU frame and traversal/request/edit/bake time, GPU stages and total GPU frame, present
  cadence and full game-frame p50/p95/p99; synchronous readback timing is not interactive FPS.
- Peak RAM separately for index cache, payload staging, edit reservations and jobs; physical
  GPU bytes separately for SH/atlas, sort/records/draw buffers, transitions and retired runs.
- Bytes read/written and startup latency, desired/resident/drawn counts, request latency,
  refinement/compaction/rebuild backlog, all overflow/loss counters and allocation failures.
- Coverage masks and real captures for first ready frame, transitions, inside-root views,
  far-origin near detail, two simultaneous views (one perspective and one orthographic),
  committed edits and subsequent reload. Both views must meet their readiness and coverage
  contracts under the shared residency budget.

Stop advancement on holes, stale edited content in new submissions after edit publication,
record loss, unbounded memory or queues, silent whole-world materialization, or a
nondiscriminating test. Already submitted frames may still present after durable edit
acknowledgement (§14.5); that is distinct from publishing a new draw with stale content.
Preserve failing results. If a gate fails, improve the implementation or seek the maintainer's
explicit disposition under §8. Section 15 records only the accepted offline cost substitution;
it waives no quality or runtime gate. A capacity success is not a speedup, and a fast
synthetic run is not real-scan quality acceptance.

<a id="15-proposed-separate-acceptance-for-offline-preparation"></a>

## 15. Separate acceptance for offline preparation

**Status: accepted by the maintainer, 2026-10-25.** The maintainer approved bounded offline
preparation as specified in [PR #1274](https://github.com/klausi3D/godotGS/pull/1274).
This is the explicit disposition required by §8 after the prescribed S1b cost reductions
failed. Sections 6.3, 8 and 14 now apply the scoped cost contract below. Approval or merge
of this documentation alone does not accept S1b or advance packages B–G: the offline job
contract must be implemented and validated, and every other applicable quality and runtime
gate must pass.

The preserved Holzbank diagnostic in [PR #1266](https://github.com/klausi3D/godotGS/pull/1266)
measured the original 22-view candidate at
`41157d67b14ae119b7dd04cb9176a1fbb99e86fb`: median public bake time was 3042.415 ms,
against 308.400 ms for merge-only, or **9.865×**. Both prescribed reductions also missed
the 2× limit (8.288× and 7.773× in separate batches). These are historical CPU preparation
measurements on one scan, not current-head measurements, GPU frame times or evidence for
200M-splat preparation. They remain failures under the original contract against which they
were measured; this disposition does not retroactively pass them.

<a id="151-decision-requested"></a>

### 15.1 Accepted scope

Provide an explicit **offline prepared-content route** whose preparation budget is separate
from its rendering budget. It must be selected by the author; canonical imports, source
preservation, existing formats and the current public bake path keep their contracts.
An offline job cannot silently move into import, first-frame loading or the game loop.

Only this offline route replaces test 12's relative bake-cost criterion with the bounded-job
requirements below. Its merge-only comparison remains a mandatory reported measurement,
with the historical failures retained. The original 22-view metric,
candidate selection, SH0–4/DC handling, deterministic choice, 0.5 dB quality criterion,
all rows/assets and the other tests in §8 remain unchanged. The reduced-view experiments
are not the offline route's default. Runtime acceptance still measures full frame costs,
coverage, transitions and physical residency; an offline preparation pass cannot satisfy
those gates.

The accepted cost substitution applies only after the author explicitly selects this
offline route and freezes its job contract before execution. A run without that contract
or without validated enforcement cannot claim offline-route acceptance. No exception
applies to canonical imports, the current public bake path or runtime edit rebakes.
In particular, the mutable-world rebuild jobs in §14.5 need their own bounded
latency and reservation evidence; they cannot inherit an unrestricted offline schedule.

### 15.2 Bounded job contract

Before launching a preparation experiment, freeze a job manifest alongside its evidence:

| Required input | Acceptance boundary |
| --- | --- |
| Input and implementation identity | Exact source/build hashes, source assets and hashes, SH/DC semantics, complete bake rule, hardware and worker count. Resume verifies the same identities. |
| Finite resource limits | Numeric wall-time deadline, cancellation/drain deadline, peak process RAM, scratch/output bytes, live work/queue limits and worker count, declared before the run. Missing limits refuse admission. GPU workers additionally reserve and report physical GPU bytes. |
| Workload and quality | Fixed asset list, full-detail leaf references and the unchanged views/quality rows. A smaller crop or fewer views cannot substitute for the declared job. |
| Publication target | Separate temporary output; complete validated output is published atomically. Failure, cancellation or disk exhaustion leaves the last valid package and canonical source intact. |

Choose the limits from the intended author's available machine/time/storage allocation,
not from the candidate's observed result. Record the reason for each limit before execution;
this ADR does not invent a universal preparation-time allowance or treat today's small-scan
timing as a capacity model. Exceeding any limit is a failed job, even when its quality passes.
An enforced process deadline may terminate a stuck worker; that is failure, not completion.

Out-of-core preparation in §14.4 must bound input batches, descendant-leaf reads, quality
images, sort work, queues and metadata as well as payloads. Track allocation reservations
before admission; an OS out-of-memory kill is not an acceptable memory limiter. A checkpoint
may resume only complete validated units with matching source and rule identities. If the
first implementation has no resumable checkpoints, state that explicitly and restart into
a new temporary output; never trust a partial package as a checkpoint.

<a id="153-smallest-validation-sequence"></a>

### 15.3 Staged validation sequence

Acceptance proceeds in dependency order. **S1b before package B** requires the implemented
§15 job contract to pass steps 1–2 below, together with every unchanged S1b quality gate in
test 12. **Step 3 remains mandatory for capacity acceptance in packages E and G**: physical
preparation scaling is validated with E's V4 pipeline, followed by the integrated 100 km /
200M SH3 runtime acceptance in G. Passing steps 1–2 cannot substitute for step 3 or any
applicable runtime test. This staging records requirements; it does not accept S1b.

1. **Small real scans:** run the existing S1b quality rows against the original 22-view rule,
   preserving the merge-only comparison. Measure public API wall time and peak RAM, exclusive
   preparation stages, I/O and scratch/output size. Keep CPU cost and image quality separate;
   neither a work counter nor a unit test replaces these measurements.
2. **Failure controls:** exhaust each configured limit, cancel while writing, interrupt and
   restart/resume, and change the input/rule identity. Verify bounded shutdown, no incomplete
   published package and retention of the previous valid output. A failing control blocks
   admission of a larger job.
3. **Physical scaling (packages E and G):** after the smaller gates pass, prepare increasing
   physical datasets under fixed predeclared resource caps using the V4 pipeline. Record
   total input versus simultaneously resident data, peak memory, bytes and elapsed work; synthetic multiplicity
   is a separate diagnostic. Complete the 200M acceptance only with its physical dataset and
   the integrated runtime evidence of §14.8.

For a candidate that claims faster preparation, compare the same source assets, rule,
quality, hardware/build options and worker count; report warmups and per-run measurements.
For rendering, compare the same prepared content and camera path at equal quality and
resolution, measuring CPU/GPU/full-frame percentiles and physical RAM/VRAM separately.
Do not combine these speedups or extrapolate either to an unmeasured world size.

Stop on a quality, coverage, identity, publication or resource failure and retain the
result. Improve the implementation within the frozen contract or request a new disposition;
do not raise limits during a batch. Rollback disables the opt-in preparation route and
retains canonical sources and previously validated packages. This disposition needs no file
migration and provides no new runtime or performance claim.
