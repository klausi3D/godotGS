# ADR: Hierarchical LOD for streamed splat worlds (budgeted cut, merged nodes, node streaming)

- **Status:** **Accepted (maintainer, 2026-10-02)** — option D, with the decisions recorded
  in §11. This revision also answers the independent review of `7fa337d4a41` (§13). Nothing
  here is implemented yet; §9 is the slice plan the implementation follows.
- **Risk class:** this file is R0 (`docs/**`). It is the **R3 design record** that
  `docs/governance/agentic-engineering.md` requires before implementation, because the
  decision changes an on-disk format (`.gsplatworld`), importer format versions, the renderer
  and its shaders.
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
- **Verified against:** `origin/master` = **`efb2c8bfba7`**. Every `file:line` below was read
  at that commit unless it names the #1088 branch, which was read at **`f3ddae2a162`**
  (PR #1134's head `feec67befa6` merges #1130 into it; the allocator code is unchanged).
- **Measurements:** §5. All numbers come from scratch scripts run for this ADR (§5.7 says how
  to reproduce them); none of that code is in this PR.

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
| Prefix draw `splat_count = effective_count` | `streaming_global_atlas_registry.cpp:440-444` | Kept only for a pinned node that is still uploading (§6.6) |
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

Distances are camera-to-centre. "Cut splats" is what every column draws. The `sat` and `hyb`
columns for holzbank and baum3 are from the re-run with single-child cells copied unchanged
(§6.3, implementer question (d)); the fix moved `sat` by at most 0.02 dB and `hyb` by at most 0.1 dB, because a lone small
splat in a merge cell is sub-pixel at the distances where that node is drawn. The baum2 rows
are still from the run before the fix (its re-run is queued behind other GPU work). **Bold** is the best
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
counts are not reported.

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
  The node table is 96 B per node (under 0.01% of the payload).
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
Slices S1a/S1b land the bake in C++ with unit tests, and S2's and S3's evidence re-run §5.2 and
§5.4 against the real implementation; those, not these tables, are the acceptance numbers.

## 6. Decision

**Accepted 2026-10-02: D, hierarchical LOD, is the alpha architecture**, staged as in §9. The
parts below are normative for stage 1 unless marked *stage 2+*. The maintainer's decisions are
listed in §11 and folded into the subsections named there. This revision also answers the
independent review of `7fa337d4a41` (13 findings); §13 maps each finding to its fix.

### 6.1 Tree: a fixed, world-aligned octree (decision 5)

- **World grid.** Every tree lives on a grid anchored in the world, not in the data: a stored
  world origin `O` (double precision) and a root cell edge `S0 = 2^k` metres, the smallest
  power of two that covers the content. A cell is addressed by integers `(level, ix, iy, iz)`;
  its edge is `S0 / 2^level` and its corner is `O + (ix, iy, iz) x edge`, exactly. Adding a
  splat changes which cells exist, never where a cell's boundaries are, so (a) a world can be
  baked **out of core** region by region and stitched at the top, (b) an edit changes only its
  own branch, and (c) cell edges halve per level, which is what makes a geometric error per
  level meaningful. k-d trees and BVHs (H3DGS uses a per-chunk BVH) give none of these.
- **Children are the eight octants.** Empty octants are not stored. Flat content therefore
  occupies four of them and **behaves as a quadtree automatically**, with no data-dependent
  choice of split axis. (The prototype measured in §5 split only along "long" axes of the
  data's bounding box; that rule is withdrawn because it moves every split plane when one
  splat is added. Re-baking §5's numbers under the fixed grid is part of S1a's evidence.)
- **Subdivision rule.** A cell with more than 16,384 splat centres is subdivided. A cell with at
  most 16,384 becomes a leaf. Sibling leaves that together fit 16,384 are grouped into one
  leaf, consecutively in octant order; the grouping depends only on the content of that
  parent, so it stays local to the branch.
- **Coincident centres.** Subdivision stops at a minimum cell edge `s_min = S0 / 2^24` (or
  when all centres in a cell are bit-identical). A cell that still holds more than 16,384
  centres is emitted as an **index-split group**: its splats, ordered by
  `(Morton code at full precision, source index)`, are cut into `ceil(N / 16,384)` sibling
  leaves that share the cell's bounds and carry the flag `split_by_index`. **The bake never
  emits a node over 16,384 splats (16 pages, within #1134's 64-page cap).** Interior nodes are
  bounded the same way (§6.3).
- **Node bounds come from the node's own payload** (centres plus three sigma of each splat,
  after merging and inflation), stored as a tight AABB and sphere next to the cell address.
  Culling and the error distance use these; the cell address is only for structure.
- **World chunks are not the leaves.** Today's 10 m grid chunks reach 22,618 splats; the bake
  partitions by the octree instead.

### 6.2 Node payload and positions (decision 6)

- Every node stores **≤ 16,384 splats**, contiguous in the file and **ordered by importance
  inside the node** (`opacity x sigma_a x sigma_b`, the A-area order of §5.2). A prefix of
  whole pages is therefore the best available subset of that node. Prefixes are drawn only for
  a pinned node that is still uploading (§6.6), never as the LOD mechanism.
- Interior payloads are plain splats in the same 144 B layout. The renderer cannot tell a
  merged splat from an original, so a better bake is a re-import, not a format or runtime
  change.
- **Positions are node-relative from stage 1.** A splat's position is stored as float32
  relative to the **centre of its node's cell**; the cell's world position is exact (integer
  cell address, double origin). On the GPU, the cut writes each drawn node's **camera-relative
  offset** (computed on the CPU in double, stored as float32) into `ChunkMetaGPU`, and
  `depth_compute` adds it to the local position. `ChunkMetaGPU` has four words that no shader
  reads (`lod_level` at `renderer/gaussian_gpu_layout.h:197`, written but unread, and `pad0..2`
  at `:200-202`): three carry the offset, one carries the S6 fade factor, so the struct stays
  64 B. The float32 payload is justified because its error depends on the cell, not the world:

  | World extent | Absolute float32 (today) | Node-relative float32, leaf cells ≤ 128 m | Interior node, cell ≤ 16 km (its `eps` ≥ 250 m) |
  | --- | --- | --- | --- |
  | 1 km | 61 µm | ≤ 7.6 µm | — |
  | 100 km | 7.8 mm | ≤ 7.6 µm | ≤ 1 mm |
  | 10,000 km | 1 m | ≤ 7.6 µm | ≤ 1 mm |

  (float32 resolution at magnitude `x` is `2^(floor(log2 x) - 23)`; positions within a cell are
  at most half the edge plus the largest splat's overhang.) Absolute float32 is visible at
  100 km for close-up detail and unusable at planetary scale; node-relative float32 is
  constant. Quantized node-relative positions (the 80 B path) are a stage-2 compression on the
  same frame of reference.

### 6.3 Interior nodes: two candidates, chosen per node (decision 2)

- **Merge cell and error.** Each interior node is built from its children. Its merge cell
  `eps` starts at `cell edge / 64` and at least twice the children's `eps`, and is raised until
  the merged candidate is at most a quarter of the children's total and at most 16,384 splats.
  `eps` is stored as the node's **geometric error**.
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
  order), orthographic, with a **pixel size of `eps / tau_bake`**, where `tau_bake` is the
  provisional `tau` (1 px until S3's A/B sets the default; a different default is a re-bake via
  `bake_rule_version`). The view set (S1 implementer question (b)) is **22 views**: the 6 axis
  directions, the 8 cube-corner directions (oblique), and 8 grazing directions at 10°
  elevation every 45° of azimuth, because the dominant open-world view is grazing and on flat
  content the axis views see it only edge-on or top-down. `E` is the mean over views and pixels
  of the squared difference in premultiplied RGBA, so holes and colour errors both count. The
  lower `E` wins; within 1%, `merged` wins.
- **Cost against the 2x limit (estimate).** The reference is rendered once per view and shared
  by both candidates; per node that is about `(4 + 1 + 1) x 22 ≈ 130` splat-renders per merged
  splat, or about 26 per source splat once the ~20% interior share is applied, at a few pixels
  each. The grazing and oblique views are rendered at half resolution (they are the
  coverage check; the axis views carry the colour detail), which brings the estimate to about
  the cost of the merge itself. **S1b's acceptance limit stays at most 2x the merge-only bake.**
  If it is exceeded, the pre-declared order of reductions is: drop the oblique corner views to
  4, then lower the grazing resolution again; if 2x is still missed, the numbers go to the
  maintainer (decision 4's rule, §8).
- **Storage.** Each node record carries a 2-bit representation kind (`leaf`, `merged`,
  `selected`, reserved), the chosen and the rejected candidate's `E` (float32 each), and the
  file carries `bake_rule_version`. The renderer ignores all of it; it exists for diagnostics,
  the visual gate, and re-deciding only the nodes whose margin was small.

### 6.4 The cut and the drawn set (findings 2, 4, 10)

The cut is computed **on the CPU** each frame, over all instances at once (§6.9). What it
produces is the **drawn set**, and every budget and cap applies to the drawn set, never to an
ideal cut.

- **Refinement order.** Start from the top-level tree's roots (§6.9). Pop the frontier node with
  the largest **priority** `p = e x b`, where `e = eps x f_px / max(dist - radius, epsilon)` is
  its projected error and `b = h` (1.25) if the node was refined in the previous frame, else 1
  (this damps flapping at the budget frontier). Ties are broken by `(instance key, node id)`
  (§6.9), never by RID or scene order.
- **A node is refined only if** (1) `e > tau` (hard switch) — or, with S6, it enters the fade
  band (§6.7); (2) **all its frustum-visible children are resident and complete** (§6.6); and
  (3) the drawn set after the refinement still fits **every cap** in §6.5.
- **A node that fails (2) is drawn itself and the loop continues.** No ancestor is ever drawn
  for a missing descendant, so a region is never drawn twice: **the invariant is that no drawn
  node has a drawn ancestor**, except the two levels of a fade pair (§6.7). Residency requests
  for the children are still issued (§6.6).
- **The loop stops at the first cap refusal (3).** The drawn set is then a prefix of a fixed
  refinement order, so for a fixed residency state **raising a budget never makes any node
  coarser**. (The first version allowed later, smaller refinements to proceed after a refusal;
  the review's counterexample showed that is not monotonic per node, so it is withdrawn. The
  cost is some unused budget, measured in S2.)
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
| Visible chunks / nodes | `max_visible_chunks`, `frustum_cull.glsl:143`, `overflowed_chunks`; sized at `render_streaming_orchestrator.cpp:1803-1808` | **Bounded.** The drawn node count, fade pairs included, is a cut cap; the buffer is sized from the configured node cap. |
| Global overlap records | `max_overlap_records`, tile binning (overflow ADR Channel A, #54) | **Bounded by estimate.** Each node bakes a footprint `F = sum (6 sigma_a)(6 sigma_b)` (m²); the cut keeps `sum (count + F f_px² / (d² x tile_area))` ≤ 80% of the effective record budget. The estimate is conservative, not exact, so `overflow_splats_clamped` stays the tripwire. |
| Per-tile raster records | `GS_MAX_RASTER_SPLATS_PER_TILE` = 12,288 per 16 px tile under the default `gpu_preset="high"` (#1137) | **Residual.** It depends on depth complexity per tile, which node-level estimates cannot bound. HLOD lowers it where #1137 bites (far, dense content becomes fewer splats), but merged splats are larger, so S2's evidence reports peak records per tile at the A/B views. Its drop must be counted and warned (#1137's fix); test 1 asserts it is zero. |

- **Content without a tree** (a `.gsplatworld` v1 not re-imported) has no coarser
  representation. Its fallback moves into **S2** (it was S4): whole chunks are dropped
  farthest first, ties by `(instance key, chunk id)`, with `WARN_PRINT_ONCE` and a counter.
  S2 claims #1131 fixed for all content only with this in place.
- **Remaining order dependence, fixed in the slice that exposes it:** the sort tie-break is
  `atlas_index` (`depth_compute.glsl:234-235`), which instances of one asset share, so equal
  depths fall back to atomic order; S5 makes it `(instance key, atlas index)`. The wind seed is
  `atlas_index` (`:172`), which changes when a node is re-admitted elsewhere in the atlas; S3
  makes it `(node id, index in node)`.

### 6.6 Streaming nodes into #1134's pages (finding 5)

- **The streaming unit is a tree node.** The page allocator and byte budget are #1134's.
- **Whole-run admission.** A node's whole run (≤ 16 pages) is allocated on admission and its
  pages upload in payload order. A node never changes size, so it is never released and
  re-allocated for refinement and never needs a second run. Refinement between levels is a
  different node with its own run.
- **The protected closure.** The byte budget is checked against the **allocated pages** of the
  closure `pinned ∪ drawn ∪ ancestors(drawn) ∪ fade pairs ∪ in-flight runs`, not against the
  drawn set alone. Ancestors stay resident because a refined node's parent must be available
  when the camera moves back. Only nodes outside the closure may be evicted.
- **Pinned levels.** The top levels (down to the first level whose total payload exceeds 5% of
  the budget) are pinned and uploaded before the first frame is drawn; a pinned node still
  uploading draws its uploaded prefix, so the first frame is never empty.
- **Requests.** For every node the ideal cut would refine but (2) of §6.4 blocked, its missing
  children are requested, in refinement-priority order, within the per-frame upload byte
  budget.
- **An unplaceable run is skipped, not waited for.** If no contiguous run fits after evicting
  nodes outside the closure (within the frame's eviction budget), the request is skipped for
  this frame and the parent keeps being drawn (adopting #1135's skip). After `R` (= 8)
  consecutive skips of the same node the streamer runs **incremental compaction**: it relocates
  unprotected nodes with GPU buffer copies, at most a fixed number of pages per frame, until the
  largest free run fits. Fixed 16-page slots are the fallback if compaction proves too costly
  in S3; the format does not change either way.
- **Eviction priority:** outside the closure first, then least recently drawn, then finest
  level first.
- **Accounting** comes from the allocated run, as in #1134.

### 6.7 Transitions: cross-fade (decision 4, slice S6; finding 10)

- **One band.** A node's fade band is `tau < e ≤ h x tau` (h = 1.25). Without S6 the node is
  hard-switched at the band's upper edge `h x tau` (refined above it, coarse below it), which is
  the hysteresis. With S6 the switch becomes a blend over the same band: the node and its
  children are drawn together with `t = (e - tau) / ((h - 1) x tau)`, children at opacity
  `x t`, the node at `x (1 - t)`. `t` is a function of the camera, so a parked camera holds a
  static blend and the oracle still reads 0 frames differing.
- **A fade starts only when the children are complete** (§6.4 condition (2)).
- **Under a budget,** fades are admitted after all full refinements, in priority order, while
  the pair fits every cap; a fade that does not fit is drawn as its coarse node (t = 0), i.e. a
  hard switch.
- **Plumbing and cost.** The fade factor is the fourth spare `ChunkMetaGPU` word (§6.2). It has
  to reach the place where splat opacity is read, which today knows only the splat's instance
  and atlas index, so S6 adds a node index to the per-splat reference: about **4 B per visible
  splat, ~20 MB at 5 M visible splats**, plus the layout-sync guard update. That is the R3
  shader change of S6.
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
  bounds and, in stage 1, **no payload**. The cut starts at the top-level roots and reaches an
  instance's asset tree only through visible top-level cells, so its cost is O(visited), not
  O(instances).
- **Error under a transform.** An instance's node error is scaled by the instance's uniform
  scale, and distances are taken in world space.
- **Stage 2+: instance proxies.** Far groups of instances need merged proxy payloads in the
  top-level cells (true HLOD across instances). The top-level node record reserves the payload
  fields, so adding proxies is a bake change, not a format change.

## 7. Format and version bumps

- **`.gsplatworld` v2** (`kWorldVersion` 1 → 2, `io/gaussian_splat_world_io.cpp:24`; the loader
  rejects any other version at `:541`):
  - header: flag `kFlagHasHlod` (next free bit after `kFlagResidentPayload`, `:38`); the world
    origin `O` (3 x float64); the root edge exponent `k`; `bake_rule_version`;
  - a **node table**, 96 B per node: cell address `(level, ix, iy, iz)` (1 + 3 x 4 B), tight
    AABB (24 B) and sphere radius, geometric error `eps`, parent index, first child and child
    count, payload first splat (64-bit) and count, height, flags (representation kind,
    `split_by_index`), the chosen and the rejected candidate's `E`, and the remaining bytes reserved for
    stage-2 fields (per-node SH degree, quantization block);
  - an optional **top-level instance table** for worlds that place instanced assets (§6.9):
    instance key, asset reference, transform, and the top-level node it hangs from;
  - the gaussian payload **reordered so every node is contiguous**, each importance-ordered,
    with **node-relative positions** (§6.2). The v1 per-chunk index lists (`:425-469`) are not
    written; they forced scattered reads per chunk.
- **The v2 loader still reads v1.** `.gsplatcache` files are written through the world saver
  (`io/ply_loader.cpp:98-105`), so v1 caches exist on disk; rejecting them would only force a
  re-parse, but accepting them is free. v1 content renders through the tree-less fallback
  (§6.5).
- **`ResourceImporterGSplatWorld::get_format_version` 2 → 3**
  (`io/resource_importer_gsplatworld.h:27`). Today the importer copies the file
  (`io/resource_importer_gsplatworld.cpp:327`); from v3 it bakes v1 sources into a v2 imported
  copy. The source file is not touched.
- **Runtime-built worlds are not baked implicitly (S1 implementer question (c), agreed by the
  coordinator).** `GaussianSplatContainer::export_world_resource`
  (`nodes/gaussian_splat_container.cpp:128`) keeps producing a v1-shaped world. S1a adds an
  opt-in **`GaussianSplatWorld.bake_hlod()`**, and the corridor lane calls it after export
  (S2 makes that change to the lane, so the A/B of test 11 runs on a baked world).
- **PLY / SPZ importers** bump in S5, when the resident route adopts the tree: PLY
  `get_format_version` 11 → 12 (`io/resource_importer_ply.h:103`), SPZ 8 → 9
  (`io/resource_importer_spz.h:61`). `PLY_CACHE_VERSION` (3, `io/ply_loader.cpp:35`) does not
  change. Re-read these values when implementing; intervening importer PRs move them.

## 8. Test plan

Each item names the slice that lands it (§9). **Thresholds are never lowered silently
(decision 4):** if a criterion fails, the implementer first improves the implementation; if it
is still not reachable, they report the numbers to the maintainer for an explicit decision.

1. **Static-frame flicker oracle at a binding cap (S2, the #1131 acceptance test).** #1131's
   Run A: the corridor-style world (baked with `bake_hlod()`), static camera, cap 120,000
   against ~320,000 visible demand; wait for streaming idle (180 frames without a load),
   capture 300 frames. **Pass: 0 frames differ from frame 0, and none of
   `instance_count_overflow_events`, `overflow_splats_clamped` (tile binning and, once #1137
   counts it, the per-tile raster cap) or `overflowed_chunks` moves.** **Non-vacuity:** S2's
   `hlod_budget_bound_frames` counter must rise by 300 in the window, proving the budget bound.
   Base must fail (299/300 today). A second run parks the camera inside a fade band (S6).
2. **Cut properties (S2, CPU unit tests):**
   - determinism: the same inputs give the same drawn set across runs and insertion orders;
   - every cap holds for the **drawn set**, with fade pairs and prefixes counted;
   - no drawn node has a drawn ancestor (outside fade pairs);
   - **monotonicity:** for a fixed residency state and history, raising any budget never makes
     a node coarser (true by construction of the stop-at-first-refusal rule; a property test
     with randomized trees checks it);
   - **fixed point:** a static camera reaches the same drawn set within 2 frames from at least
     five different prior drawn sets.
3. **Bake unit tests (S1a):** deterministic output; **no node over 16,384 splats**, including
   a fixture with 40,000 coincident centres (index-split group); node bounds contain the
   node's own payload at three sigma; moment matching preserves the weighted mean and second
   moment **before inflation**, and inflation is at most 1.25x per axis; a single-child cell
   is copied bit-exactly; interior overhead ≤ 35% on the fixtures; the world grid is stable
   (adding one splat changes only the cells on its path); node-relative positions round-trip
   within float32 resolution of the cell; a v2 file round-trips; a v1 file still loads.
4. **Importer re-import (S1a):** a v1 `.gsplatworld` re-imports once after the bump and gains
   a tree; the source file is unchanged; `bake_hlod()` on a runtime-built world gives the same
   tree as the importer.
5. **Mixed-size thrash test (S3):** a synthetic tree with node sizes from 1 to 16 pages, a byte
   budget at 60% of demand, a camera sweeping back and forth. Every requested node is admitted
   within **N frames, where N is computed before the run** from the configured upload bytes per
   frame, the eviction budget and the compaction rate; the atlas never exceeds its budget;
   no frame blocks on an unplaceable run.
6. **Coverage under streaming (S3):** cold start with the budget binding; no frame has a
   node-shaped hole (coverage mask against the warm frame) **and** no frame draws a node
   together with one of its ancestors outside a fade pair.
7. **Corridor lane (S3):** `vram_cap_hit_frames <= 0` at 1 GiB, `residency_ratio >= 0.70`,
   every overflow counter of test 1 at zero, with GPU evidence.
8. **Visual gate on real scans (S2):** §5.2 re-run on the three scans against the real
   implementation. **Thresholds are fixed now, from §5.2, not from the run they judge:** in
   every row, the implementation's PSNR must be at least the prototype's value for the same
   method minus 1.0 dB, and SSIM at least the prototype's minus 0.01.
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
    engine, since S2's runtime cut does not exist yet). In every row it must be within 0.5 dB
    of the better of `sat` and A-area, or better; the choice is deterministic; the bake with the
    choice takes at most 2x the merge-only bake. If the 0.5 dB criterion fails, the
    implementer improves the metric or the candidates first; if it is still not reachable, the
    numbers go to the maintainer for an explicit decision (decision 4).

Single-vendor blind spot: all GPU evidence so far is NVIDIA. Atomic order differs by vendor,
which is why the oracle must pass by construction (no atomic decides membership), not by luck
on one GPU.

## 9. Staged slice plan

One PR per slice, each against `master`, each stating its base SHA.

### Stage 1 — the alpha (seven PRs)

| Slice | Content | Risk | Size (estimate) |
| --- | --- | --- | --- |
| **S1a** | Format v2 (node table, instance table, header) + bake core: world-aligned octree with index-split groups, `sat` merge with the single-child identity, node-relative positions, node bounds from payload, importance-ordered contiguous payload; v1 → v2 in the world importer; opt-in `GaussianSplatWorld.bake_hlod()`; tests 3–4 | R3 | ~1,900 LOC + ~1,000 LOC tests |
| **S1b** | Per-node choice: `selected` candidate from leaf prefixes, CPU rasterizer, 22-view metric, representation kind and errors in the record; test 12 | R3 | ~900 + ~500 |
| **S2** | Global CPU cut over all instances with the top-level instance tree, drawn set, every cap of §6.5, stop-at-first-refusal, priority hysteresis, the `tau` setting, camera-relative node offsets in `ChunkMetaGPU`, `max_visible_splats` sized once, the tree-less fallback, `hlod_budget_bound_frames`, corridor lane calls `bake_hlod()`; tests 1, 2, 8 | R3 | ~2,500 + ~1,500 |
| **S3** | Node streaming on #1134: whole-run admission, protected-closure accounting, pinned levels, requests in priority order, skip + bounded retry + incremental compaction, eviction priority, stable wind seed; tests 5–7 and the **`tau` A/B (test 11) as an acceptance criterion** | R3 | ~1,400 + ~900 |
| **S4** | Telemetry: cut size, depth histogram, fade-band size, compaction and skip counts | R2 | ~200 + ~150 |
| **S5** | Resident route: bake at PLY/SPZ import (format bumps), all nodes resident, joins the global cut, instance-key sort tie-break, retire the #420 clamp; test 10 | R3 | ~900 + ~550 |
| **S6** | Cross-fade: one band, `t` from the error, fades admitted after refinements, fade factor in `ChunkMetaGPU`, node index in the per-splat reference, layout-sync guard; test 9 | R3 | ~800 + ~450 |

Stage 1 is **about 8,600 LOC of production code and 5,050 LOC of tests over seven PRs, six of
them R3.** The growth since the previous revision (~6,600 + 3,800 over six PRs) comes from
decisions 5–7 (world grid with index splits, node-relative positions, global instance cut and
top-level tree) and from the review's drawn-set, cap and closure rules. S1 is split into S1a
and S1b for reviewability; S6 can land after S2 in parallel with S3. The tree is as deep as the
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
- **Out-of-core bake** for worlds that do not fit in RAM, stitched at the top levels; the
  world-aligned grid makes it possible without a format change.

### What would force a rebuild if stage 1 were done more cheaply

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
   §5.2 was re-measured.

Still open, not blocking stage 1: whether the corridor lane keeps its 120,000 cap, which binds
by about 50x and under the cut means "draw a coarse world"; S3's A/B data informs it.

## 12. What was not verified

- **No production code exists.** All quality numbers come from rendering prototype outputs
  through the engine; the bake is a Python prototype.
- **The prototype predates the world-aligned grid (decision 5):** §5's trees split at the
  data's bounding-box midpoints. S1a re-runs §5.2 and §5.5 on the fixed grid.
- **Not prototyped at all:** the per-node choice and its 22-view metric (cost in §6.3 is an
  estimate), hysteresis, the byte-budget refusal, the drawn-set and closure rules, the
  error-band fade (§5.6 used three fixed steps), node-relative positions, the top-level
  instance tree and the overlap-record estimate. `hlod.cut` in the prototype has only the
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
