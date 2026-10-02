# ADR: Hierarchical LOD for streamed splat worlds (budgeted cut, merged nodes, node streaming)

- **Status:** **Accepted (maintainer, 2026-10-02)** — option D, with the four decisions
  recorded in §11. Nothing here is implemented yet; §9 is the slice plan the implementation
  follows.
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
| Prefix draw `splat_count = effective_count` | `streaming_global_atlas_registry.cpp:440-444` | Kept as the mechanism for progressive node upload (§6.6) |
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
- **Reference dropout (finding).** At the far holzbank view the **full-detail reference
  itself** has a rectangular, tile-aligned black hole that no reduced variant has. It matches
  the tile-capacity drop of `adr-overflow-drop-telemetry.md` Channel A; the overflow counter
  was not read, so the cause is **unverified**. Those 204 pixels are excluded (copied from the
  reference into every variant) before the metrics are taken. It is a second argument for LOD:
  far, dense content at full detail already breaks the rasterizer.
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
- **Prototype bake:** adaptive-axis octree, leaves ≤ 16,384 splats, interior nodes at a 4:1
  splat reduction, merge cells of size `eps` (the node's geometric error); `mass`, `sat` and `mm`
  are the three opacity rules of §6.3, and `hyb` is `sat` with a sparse-cell fallback.

### 5.2 Image quality at equal budget (region-of-change crop, PSNR dB / SSIM)

Distances are camera-to-centre. "Cut splats" is what every column draws. **Bold** is the best
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
| baum3, d1 (32 m) | 4 | 298,483 (37%) | 44.7 / 0.993 | **44.9 / 0.993** | 42.4 / 0.991 | 39.4 / 0.985 | 30.6 / 0.946 | 29.6 / 0.935 |
| baum3, d3 (97 m) | 1 | 308,844 (39%) | 37.2 / 0.973 | 37.4 / 0.973 | 35.8 / 0.964 | **37.8 / 0.976** | 29.7 / 0.909 | 28.5 / 0.886 |
| baum3, d3 (97 m) | 4 | 118,855 (15%) | 32.8 / 0.942 | **33.0 / 0.941** | 30.7 / 0.930 | 31.5 / 0.943 | 23.7 / 0.823 | 24.5 / 0.847 |
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
(`d_max = 500`). Up to 6.4 M splats the tree is really baked; beyond that a model built from a
real single-tile bake is used (it under-predicts the real cut by 23% at `k = 16`, `tau = 2`).

| World | Splats | HLOD `tau=1` | HLOD `tau=2` (nodes) | Flat A, far 500 m | Flat A, no far plane |
| --- | --- | --- | --- | --- | --- |
| 160 m | 6.4 M | 6.4 M | 4.4 M (1,320) | 4.9 M | 4.9 M |
| 320 m | 25.6 M | 16.5 M | 7.0 M (1,752) | 11.3 M | 11.3 M |
| 640 m | 102 M | 26.0 M | 9.5 M (2,169) | 19.1 M | 24.0 M |
| 1.3 km | 410 M | 35.5 M | 11.9 M (2,586) | 19.4 M | 62.4 M |
| 2.6 km | 1.64 B | 44.9 M | 14.4 M (3,003) | 19.4 M (nothing drawn past 500 m) | 216 M |

Read this as follows:

- Flat schemes are **quadratic** in view distance. With a far plane they stop growing only
  because they stop drawing.
- The HLOD cut grows by a **constant per doubling** of world size (about +2.5 M at `tau=2`):
  it is **logarithmic**, not constant. That is inherent in an isotropic screen-space error over
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
cut nodes; a non-resident cut node still draws its resident ancestor, so coverage is never lost.

| Scheme | Splats drawn (mean / max) | Needed MiB (mean / max) | Units (mean / max) | Residency |
| --- | --- | --- | --- | --- |
| Today: whole chunks resident, prefix drawn | 4.46 M / 5.33 M | 1,171 / **1,601** | 637 / 866 | 0.775 |
| A: prefix pages only | 4.46 M / 5.33 M | 660 / 796 | 637 / 866 | 0.924 |
| HLOD `tau=1` | 6.01 M / 7.48 M | 1,054 / **1,310** | 888 / 1,133 | 0.842 |
| HLOD `tau=2` | 3.89 M / 4.52 M | 684 / 794 | 611 / 726 | 0.961 |
| HLOD `tau=2`, budget 2 M | 1.97 M / 2.00 M | 348 / 358 | 323 / 344 | 0.982 |
| HLOD `tau=1`, budget 500 k | 0.50 M / 0.50 M | 91 / 93 | 96 / 105 | 0.996 |
| HLOD `tau=1`, budget 120 k (the lane's cap) | 117 k / 120 k | 23.5 / 26.3 | 33 / 42 | 1.000 |

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

- **Only the opacity cross-fade helps:** about 9 dB better per step, and the largest pixel
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
Stage-1 slice S1 lands the bake in C++ with unit tests, and S2's evidence re-runs §5.2 and §5.4
against the real implementation; those, not this table, are the acceptance numbers.

## 6. Decision

**Accepted 2026-10-02: D, hierarchical LOD, is the alpha architecture**, staged as in §9. The
parts below are normative for stage 1 unless marked *stage 2+*. The maintainer's four decisions
are recorded in §11 and folded into §6.3 (per-node merge choice), §6.4 (`tau` is a setting),
§6.7 (cross-fade in the alpha), §6.8 and §9 (resident route in the alpha).

### 6.1 Tree

- **Adaptive-axis octree** over splat centres, subdivided at the bounding-box midpoint. A node
  splits only along axes whose extent is at least half its longest extent, so terrain-like
  content becomes a quadtree and volumetric content an octree, from the same code.
- **Why not a k-d tree or BVH** (H3DGS uses a per-chunk BVH): midpoint subdivision gives node
  bounds that do not depend on the data, so (a) the world can be baked **out of core** region
  by region and stitched at the top, which billions of splats require, (b) a local edit changes
  only its own branch, and (c) node extents halve per level, which is what makes a geometric
  error per level meaningful. Data-dependent splits give none of these.
- **Leaves** hold ≤ 16,384 splats. When a split produces small octants, Morton-adjacent
  siblings are grouped into one leaf while they fit, so leaves stay between half and full size
  (corridor: 1,787 leaves, mean 11,190, instead of 6,325 leaves of 3,162 without grouping).
- **World chunks are not the leaves.** Today's 10 m grid chunks are up to 22,618 splats; the
  bake partitions leaves by the tree instead.

### 6.2 Node payload

- Every node, leaf or interior, stores **≤ 16,384 splats = 16 pages**, far under #1134's
  64-page per-allocation cap (coordinator caveat (a)). The bake enforces it: an interior node
  whose merge exceeds it coarsens its merge cell until it fits.
- The payload is **contiguous per node** in the file and **ordered by importance inside the
  node** (`opacity x sigma_a x sigma_b`, §5.2), so any prefix of whole pages is the best
  available subset of that node (A-area is the strongest thinning in §5.2). Prefixes are used
  only while a node is still uploading and nothing coarser is resident (§6.6), never as the LOD
  mechanism. As a transition they measured no better than a hard switch (§5.6).
- Interior payloads are plain splats in the same 144 B layout. The renderer cannot tell a
  merged splat from an original. **This is the property that keeps stage 1 from being rebuilt:**
  a better bake (§6.3, stage 2+) is a re-import, not a format or runtime change.

### 6.3 Interior nodes: the merge

- Each interior node is built from its children's payloads. Splats are bucketed on a grid of
  cell `eps`, starting at `extent / 64` and at least twice the children's `eps`, raised until the
  node is at most a quarter of its children's total and at most 16,384 splats. `eps` is stored
  as the node's **geometric error**.
- Splats with `2 sigma_max >= eps` are already this coarse and pass through unchanged.
- Every other cell becomes one splat by **moment matching** (H3DGS): weights
  `w = alpha x sigma_a x sigma_b`; mean and covariance are the weighted mean and the weighted
  mean of `Sigma_i + (mu_i - mu)(mu_i - mu)^T`; DC and higher SH coefficients are the weighted
  means.
- **Opacity is the open part**, and it is measured three ways (§5.2):
  - `mm`, the deleted `SplatClusterer` rule, `alpha = 1 - prod(1 - alpha_i)`;
  - `mass`, alpha-mass conservation: `M = sum alpha_i 2 pi sigma_a sigma_b`; if `M` exceeds the
    merged capacity, inflate the two largest axes by up to 1.6x, then `alpha = min(M / C, 0.99)`;
  - `sat`, a saturating variant: inflation ≤ 1.25x and `alpha = 1 - exp(-M / C)`, which models
    children stacked in depth.
- **`sat` is the merge rule.** It beats `mass` and `mm` in every row of §5.2.
- **Decision 2 (2026-10-02): the representation is chosen per node at bake time.** Every
  interior node is built twice, and the bake keeps the candidate with the lower error:
  - **`merged`**: the `sat` merge above;
  - **`selected`**: importance selection. The node keeps the same number of splats as the
    merged candidate, taken from its children's payloads as original splats: each child gets
    a share proportional to its splat count and keeps its top splats by
    `opacity x sigma_a x sigma_b` (ties by index). This is the A-area method of §5.2.
- **The error metric.** `E(candidate)` compares the candidate with the node's **reference**,
  the union of its children's payloads (what the cut shows when it refines this node):
  - rendered by the bake's own small CPU splat rasterizer (front-to-back alpha compositing,
    SH evaluated per view, deterministic order), **orthographic**, from the **six axis
    directions**, with a **pixel size equal to the node's `eps`**, the scale at which the node
    is shown when it is accepted at `tau` = 1 px. Images are `extent / eps` pixels on a side,
    about 64 to 128;
  - `E` = the mean over the six views and all pixels of the squared difference of
    premultiplied RGBA (`rgb x alpha` and `alpha`), so a hole and a colour error both count;
  - the lower `E` wins; a tie (within 1%) goes to `merged`, which preserves coverage better
    on surfaces (§5.2).

  The reference is the children, not the full-detail leaves, so the cost per node stays
  bounded (at most eight children of at most 16,384 splats). Errors therefore compound by
  level; comparing against leaves is a stage-2 refinement.
- **Cost (estimate, unmeasured).** Per interior node the bake rasterizes the reference (about
  4x the node's count) and two candidates (1x each), six times: about 36 splat-renders per
  interior splat at a few pixels each. Interior splats are ~20% of leaves (§5.5), so this is
  of the order of 100 pixel blends per source splat, which in C++ is expected to add tens of
  percent to the merge bake, not multiples. S1 must measure it; **the acceptance limit is that
  the per-node choice at most doubles the bake time** of the merge alone.
- **Quality acceptance (S1).** Re-run the §5.2 rows with the per-node choice. In every row it
  must be within 0.5 dB of the better of `sat` and A-area, or better (test 12).
- **How the choice is stored:** in the node record (§7): a 2-bit representation kind
  (`leaf`, `merged`, `selected`, one reserved), the chosen candidate's `E` and the rejected
  candidate's `E` as float32, and a file-level `bake_rule_version`. The renderer ignores all of
  it: both candidates are plain splats in the same payload layout. It exists for diagnostics,
  for the visual gate, and so a later bake can re-decide only the nodes whose margin was small.

### 6.4 The cut

- Computed **on the CPU** each frame from the camera: start at the root; a node is accepted
  when its projected error `eps x f_px / max(dist - radius, epsilon)` is ≤ `tau` pixels, or it
  is a leaf; otherwise it is refined into its frustum-visible children.
- **Budgets.** Refinement pops the node with the **largest projected error first** (ties by
  node id). A refinement that would push the cut over the **splat budget**
  (`quality/max_splat_count`) or the **resident byte budget** is not taken; the node stays in
  the cut, merged. The result is a pure function of camera, tree and budgets: **deterministic**.
- **Hysteresis.** A node refined in the previous frame is coarsened only when its error falls
  below `tau / h` (`h` ≈ 1.25), so a camera parked on a threshold does not flap. The cut is then
  a function of camera, tree, budgets and the previous cut; for a static camera it converges in
  one frame and stays fixed.
- The cut replaces `update_chunk_lod_parameters` and the frustum chunk scan for HLOD content.
  Cost is O(visited nodes): the cut, its parents and their culled children, not O(total
  chunks) (#320).
- **Decision 3 (2026-10-02): `tau` is a setting, not a constant.** A project setting
  (`rendering/gaussian_splatting/lod/hlod_tau_px`, in the settings manifest) with a per-node
  override on `GaussianSplatWorld3D`; `quality/lod_bias` multiplies it. Its **default is not
  fixed by this ADR**: S3 runs a real A/B at 1 px vs 2 px (test 11) and the maintainer picks the
  default from that data. Until then the provisional value is 1 px. The splat budget is the
  existing `quality/max_splat_count`.

### 6.5 Never truncate (#1131)

- The renderer's `max_visible_splats` is sized to at least the cut's splat count, and the cut
  never exceeds the budget, so `depth_compute.glsl`'s overflow branch (`:226`) **cannot be
  taken**. The overflow counter of `adr-overflow-drop-telemetry.md` Channel B stays as the
  tripwire: any nonzero value is a bug, asserted by the tests in §8.
- The GPU still drops off-screen and sub-pixel splats inside accepted nodes. That only lowers
  the count and depends only on the camera, so it is deterministic.
- Content without a tree (a `.gsplatworld` v1 loaded without re-import, §7) has no coarser
  representation. Such content is treated as one level; if its demand exceeds the budget, whole
  nodes are dropped **farthest first** (ties by node id), with `WARN_PRINT_ONCE` and a counter.
  That is deterministic and is the documented fallback, not the design.

### 6.6 Streaming nodes into #1134's pages

- **The streaming unit is a tree node.** `StreamingChunk` entries become nodes; their bounds,
  counts and payload offsets come from the node table. The page allocator, byte budget and
  eviction stay as #1134 builds them.
- **Coordinator caveat (b), progressive refinement:** a node's **whole run** (≤ 16 pages) is
  allocated on admission and its pages upload in payload order (importance order). While it is
  incomplete, the cut draws its nearest complete ancestor instead. Only when no ancestor is
  resident (cold start, or a budget too small for the pinned levels) does it draw its uploaded
  prefix (`ChunkMetaGPU.splat_count = uploaded`, the mechanism that exists today), because the
  best available subset beats an empty region. A node never changes size, so it is never
  released and re-allocated for refinement, and it never needs a second run. Refinement
  **between** levels is a different node with its own run.
- **Caveat (c), accounting:** residency bytes are the allocated run, as #1134 does.
- **Coverage first:** the top levels (root down to the first level whose total payload exceeds
  a fixed share of the budget, 5% by default) are pinned. Requests go coarse to fine: a cut
  node's ancestors before the node. A cut node that is not resident yet draws its nearest
  resident ancestor instead, so a streaming miss is blur, never a hole.
- **Eviction priority:** not in the current cut and not an ancestor of it, then least recently
  in the cut, then finest level first.
- **Caveat (d), large nodes starving behind small ones:** with best fit and nodes from 1 to 16
  pages, a 16-page node can wait while small nodes fill holes. The evict-until-fit loop of
  #1134 covers it within a frame's eviction budget; §8 adds a mixed-size thrash test that must
  show every requested node admitted within a bounded number of frames.

### 6.7 Transitions

**Decision 4 (2026-10-02): cross-fade transitions are in the alpha** (slice S6).

- Per-node **opacity cross-fade** over an error band: while a node's projected error moves from
  `tau x h` to `tau`, the cut emits the node and its children together, the children with
  fade `t` and the node with `1 - t`. It is the only fade that measured better than a hard
  switch (§5.6: about 9 dB smaller worst step); dithered and importance-prefix fades did not.
- The fade factor lives in a spare `ChunkMetaGPU` word (`pad0`, `gaussian_gpu_layout.h:200`)
  and must reach the place where splat opacity is read, which today knows only the splat's
  instance and atlas index. S6 therefore extends the per-splat reference (or adds a node id to
  it) and updates the layout-sync guard. That is the R3 shader change of this ADR.
- **Budget:** both levels are drawn during a fade, so the cut counts both against the splat and
  byte budgets. Under a binding budget the band narrows (fewer nodes fade at once); it never
  truncates.
- Without S6 the cut switches hard with the hysteresis of §6.4.

### 6.8 The resident route

**In the alpha (decision 4, 2026-10-02)** as slice S5. The resident route bakes the same tree at
import, keeps every node resident (+18–21% VRAM, §5.5) and runs the same cut, so it **honours
the splat budget** the same way the streaming route does. `max_splat_count` then means the same
on both routes, which resolves #1131's second inconsistency. The #420 importance clamp is
retired in the same slice.

## 7. Format and version bumps

- **`.gsplatworld` v2** (`kWorldVersion` 1 → 2, `io/gaussian_splat_world_io.cpp:24`; the
  loader rejects any other version at `:541`):
  - header flag `kFlagHasHlod` (next free bit after `kFlagResidentPayload`, `:38`);
  - a **node table**, 96 B per node: bounds centre and radius, AABB, geometric error `eps`,
    parent index, first child and child count, payload first splat (64-bit, for billions of
    splats) and count, height, flags with the **2-bit representation kind** (`leaf`, `merged`,
    `selected`, reserved), the chosen and the rejected candidate's error `E` (float32 each),
    and 16 reserved bytes for stage-2 fields (per-node SH degree, quantization block);
  - a header field `bake_rule_version`, so a later bake rule can re-bake on import without a
    format change;
  - the gaussian payload **reordered so every node is contiguous**, leaves first, then
    interior nodes by level, each importance-ordered. The v1 per-chunk index lists
    (`:425-469`) are not written; they forced scattered reads per chunk.
- **The v2 loader still reads v1.** `.gsplatcache` files are written through the world saver
  (`io/ply_loader.cpp:98-105`), so v1 caches already exist on disk; rejecting them would only
  force a re-parse, but accepting them is free.
- **`ResourceImporterGSplatWorld::get_format_version` 2 → 3**
  (`io/resource_importer_gsplatworld.h:27`). Today the importer copies the file
  (`io/resource_importer_gsplatworld.cpp:327`); from v3 it bakes v1 sources into a v2 imported
  copy, so every existing world re-imports once and gains a tree. The source file is not
  touched.
- **Runtime-built worlds** (`GaussianSplatContainer::export_world_resource`,
  `nodes/gaussian_splat_container.cpp:128`, which the corridor lane uses) bake directly.
- **PLY / SPZ importers** bump only in S5, when the resident route adopts the tree: PLY
  `get_format_version` 11 → 12 (`io/resource_importer_ply.h:103`), SPZ 8 → 9
  (`io/resource_importer_spz.h:61`). `PLY_CACHE_VERSION` (3, `io/ply_loader.cpp:35`) does not
  change: decoding does not change. Re-read these values when implementing; intervening
  importer PRs move them.

## 8. Test plan

Each item names the slice that lands it (§9).

1. **Static-frame flicker oracle at a binding cap (S2, the #1131 acceptance test).** #1131's
   Run A: the corridor-style world, static camera, cap 120,000 against ~320,000 visible
   demand, wait for streaming idle (180 frames with no load), capture 300 frames. **Pass:
   0 frames differ from frame 0, and `instance_count_overflow_events` does not move.** Base
   must fail it (299/300 today), so the test discriminates.
2. **Cut determinism and budget (S2, CPU unit tests):** the same camera, tree and budgets give
   the same node list across runs and across insertion orders; the cut never exceeds the splat
   or byte budget; raising the budget never makes the cut coarser anywhere (monotonic).
3. **Bake unit tests (S1):** deterministic output for identical input; every node ≤ 16,384
   splats; moment matching preserves the weighted mean and second moment exactly; interior
   overhead ≤ 35% on the test fixtures; a v2 file round-trips; a v1 file still loads.
4. **Importer re-import (S1):** a v1 `.gsplatworld` re-imports once after the bump and gains
   a tree; the source file is unchanged.
5. **Mixed-size thrash test (S3, coordinator caveat (d)):** a synthetic tree with node sizes
   from 1 to 16 pages, a byte budget at 60% of demand and a camera that sweeps back and forth;
   every requested node is admitted within N frames (N fixed in the test), the atlas never
   exceeds its budget, and `atlas_fit_extra_evictions` stays bounded.
6. **Coverage under streaming (S3):** cold start with the budget binding; every frame draws a
   resident ancestor wherever a cut node is missing (no frame with a node-shaped hole, checked
   by coverage mask against the warm frame).
7. **Corridor lane (S3):** `vram_cap_hit_frames <= 0` at 1 GiB, `residency_ratio >= 0.70`, no
   truncation events, with GPU evidence.
8. **Visual gate on real scans (S2):** §5.2 re-run against the real implementation on the
   three scans; thresholds are set from that run, never lowered to pass.
9. **Transitions (S6):** §5.6's pop measure on a dolly across several switch distances; the
   worst per-frame step with the cross-fade must be at most half that of the hard switch, and
   the static-frame oracle (test 1) must still read 0 with a camera parked inside a fade band.
10. **Resident route (S5):** with `max_splat_count` below the atlas, the drawn count equals the
    cut's count and the flicker oracle reads 0.
11. **`tau` A/B (S3 acceptance, decision 3).** On the corridor lane and the three real scans,
    run `tau` = 1 px and `tau` = 2 px and report, per configuration, from the engine (not a
    simulation): **allocated VRAM** (atlas pages in use x page bytes, and
    `vram_device_buffers_mb`), **residency** (`residency_ratio` and its minimum), **FPS**
    (frame time p50 / p95), **quality** (§5.2's region-of-change PSNR / SSIM against full
    detail at fixed views) and **transition smoothness** (worst per-frame step on a dolly, with
    S6's cross-fade if it has landed, otherwise hard switches, stated). S3 is not accepted
    without this table; the maintainer sets the default from it.
12. **Per-node merge choice (S1, decision 2):** the §5.2 rows re-run with the per-node choice
    are within 0.5 dB of the better of `sat` and A-area in every row; the choice is
    deterministic; the bake with the choice takes at most twice the merge-only bake.

Single-vendor blind spot: all GPU evidence so far is NVIDIA. Atomic order differs by vendor,
which is exactly why the oracle must pass by construction (no atomic decides membership), not
by luck on one GPU.

## 9. Staged slice plan

One PR per slice, each against `master`, each stating its base SHA.

### Stage 1 — the alpha-blocking minimum

| Slice | Content | Risk | Size (estimate) |
| --- | --- | --- | --- |
| **S1** | Format v2 + bake: `core/` tree builder, `sat` merge, importance selection, the **per-node choice** with its CPU rasterizer and error metric (pure functions, host-testable), node table with representation kind and errors, contiguous importance-ordered payload, v1 → v2 in the world importer, bake in `export_world_resource`; tests 3, 4, 12 | R3 | ~2,400 LOC + ~1,200 LOC tests |
| **S2** | Runtime tree + CPU cut with budgets and hysteresis, the `tau` setting (manifest, node override), nodes go through today's chunk residency unchanged (no streaming change yet); `max_visible_splats` sized from the cut; tests 1, 2, 8 | R3 | ~1,300 + ~800 |
| **S3** | Node streaming on #1134: whole-run admission, progressive prefix upload, pinned top levels, coarse-first requests, ancestor fallback, eviction priority; tests 5–7, and the **`tau` A/B (test 11) as an acceptance criterion** | R3 | ~1,100 + ~700 |
| **S4** | Deterministic fallback for tree-less content (§6.5 last bullet) and the telemetry: cut size, depth histogram, budget-bound frames, fade-band size | R2 | ~300 + ~200 |
| **S5** | Resident route: bake at PLY/SPZ import (format bumps), all nodes resident, same cut honouring the splat budget, retire the #420 clamp; test 10 | R3 | ~800 + ~500 |
| **S6** | Cross-fade transitions: fade band in the cut, fade factor in `ChunkMetaGPU`, carried to where opacity is read, layout-sync guard; test 9 | R3 | ~700 + ~400 |

Stage 1 is therefore **about 6,600 LOC of production code and 3,800 of tests over six PRs**,
five of them R3. It was ~5,000 + 2,900 over five PRs before decisions 2 and 4 added the
per-node choice and the cross-fade. S6 can land after S2, in parallel with S3. The levels are not fixed at 2–3: the tree is as deep as the content needs
(holzbank 4 levels, corridor 8, baum2 16 because of far floaters), which costs nothing extra.
S1 alone does not change rendering; S2 is the slice that closes #1131; S3 makes the corridor fit
its budget.

### Stage 2+ — refinements that need no rebuild

- **Foreshortening-aware error** (§5.3): projected node height instead of an isotropic radius.
- **Better bakes:** the per-node error against the full-detail leaves instead of the
  children, more candidate rules, and distillation against renders of the full-detail subtree
  (the full detail is the teacher, so no training views are needed). Re-bake only; the node
  record already carries the representation kind and both errors.
- **GPU cut** if CPU traversal shows up in profiles at billions of splats.
- **Compression:** quantized node payloads (the 80 B path) and per-level SH degree reduction.
- **Out-of-core bake** for worlds that do not fit in RAM, stitched at the top levels.

### What would force a rebuild if stage 1 were done more cheaply

- **Option A (flat importance prefix) as the format.** It has no merged nodes, so the screen-
  bound requirement is unreachable (§5.3) and the format would be replaced.
- **A fixed number of levels baked as separate per-level files.** Billions of splats need an
  unbounded depth; fixed levels cap the world size.
- **Keeping the per-chunk index lists.** Node streaming needs contiguous node payloads; index
  lists make every node load a scattered read.
- **Enforcing the budget on the GPU** (atomics, a depth-histogram cutoff, #1131's option 1):
  it caps without a coarser representation and would be removed once the cut exists.
- **Data-dependent tree splits** (k-d, BVH): they prevent out-of-core and incremental bakes.
- **Merged splats in a different layout from leaves:** every later compression or shader
  change would have to be done twice.

## 10. Reviving the deleted code

**Not worth reviving; reuse the idea, not the code.**

- `SplatClusterer` (deleted in `de8fc43e2c6`, 1,079 + 287 lines) merges colour only (no SH),
  averages rotations by slerp and scales by per-axis RMS **while also** computing a merged
  covariance it never uses for the output shape (its own comment calls it "a simplified
  approach"), uses cluster radii in absolute world units (2/4/8/16 m by level), and is written
  against a per-splat `GaussianData` value type that no longer exists. Its opacity rule is the
  `mm` rule of §5.2, which measured worst of the three.
- `StreamingLODManager` (1,237 + 309 lines) kept one GPU buffer per LOD level for the **whole
  dataset**, so it cannot stream regions, and it was never wired (`de8fc43e2c6` removed it as a
  dead path).
- `AdaptiveLODSystem` (`7205f2d10ef`, 763 + 275 lines) selected splats on the CPU each frame
  from a `Vector<GaussianData>`, which the GPU-driven pipeline cannot use.

What carries over: moment-matched covariance merging (as specified in §6.3, from H3DGS) and
the idea of a budget-driven selection, done here by the cut.

## 11. Maintainer decisions (2026-10-02)

1. **Accepted: HLOD (option D) is the alpha architecture.**
2. **Merge rule: chosen per node at bake time.** Both `sat` and importance selection are
   computed and the lower-error one is kept; the metric, its cost and the per-node storage are
   in §6.3 and §7. The draft had offered `sat` as a default with selection as an import
   option; §5.2's near tie is why the choice is made per node instead.
3. **`tau` is a setting, not fixed now.** S3's acceptance includes a real A/B at 1 px vs 2 px
   on the corridor and the real scans (test 11); the maintainer picks the default from it.
4. **In the alpha: cross-fade transitions (S6) and the resident route (S5)**, which uses the
   tree and honours the splat budget. Stage 1 grows to six PRs (§9).

Still open, not blocking stage 1: whether the corridor lane keeps its 120,000 cap, which binds
by about 50x and under the cut means "draw a coarse world"; S3's A/B data informs it.

## 12. What was not verified

- **No production code exists.** All quality numbers come from rendering prototype outputs
  through the engine; the bake is a Python prototype.
- The **merge is closed-form** and was measured on three real scans and one synthetic world.
  Other content (interiors, thin structures) may rank the rules differently.
- The **residency simulation** models #1134's allocator as bytes only; fragmentation and the
  per-frame eviction budget are not simulated (that is test 5's job).
- **Bake times** are numpy on one core; the C++ bake is unmeasured. The per-node choice
  (decision 2) was **not prototyped**: its cost in §6.3 is an estimate, and its quality is
  bounded only by the two measured candidates. Test 12 is where both are established.
- The **reference dropout** in §5.1 is attributed to the tile-capacity drop by its shape only.
- NVIDIA RTX 3090 only.
