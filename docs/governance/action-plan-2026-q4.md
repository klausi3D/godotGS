# GodotGS: Audit Double-Check and Action Plan

Date: 2026-10-04. Verified against `master` = `ae5554978ed86a24e2a7331a9a188ce381a01482`
(identical to branch `claude/focused-bohr-o96dn1`). The review changed no code; its output is this document and the issues listed in the Status section.

## Context

The user supplied a long external-agent analysis of GodotGS (progress analysis, 44-contract
audit matrix, docs audit, competitive comparison). The request: double-check it against the
real code, then turn it into a concrete action plan "to become the best Gaussian splat
renderer".

Method used here:

1. Inline reading of every file the audit cites (LOD, eviction, serializer, raster stage,
   projection flip, world clear, SPZ/PLY colour path, shader decode, CI gate, benchmark JSON,
   HLOD ADR, acceptance bar, known-limitations page) at HEAD.
2. GitHub state: all 285 open issues listed, the relevant ones read (#1053, #1052, #1056,
   #1131, #54, #600, #774, #951, #952), workflow runs on master listed.
3. Read-only agent workflows. The container allows two concurrent agents, so a first
   36-claim workflow was stopped after its first two verdicts (C01, C02) agreed with the hand
   reading. What ran to completion: six grouped verifiers for the claims not checked by hand
   (sorter and barrier, SH/jitter/alpha, atlas and distance bound, harness and CI, director
   and materialisation and hardening, multiview and projection), seven subsystem finders
   (renderer and lifecycle, shaders, streaming, import and persistence, CI evidence,
   performance, feature surface) with one adversarial judge per P0/P1 finding, and four web
   research agents for the competitive claims. Every agent finding used below was either
   re-read in the code here or is labelled finder-reported.

Headline result, stated first:

- The external audit is **largely correct on code facts** and **almost entirely non-novel**.
  Every renderer/streaming/persistence defect it lists as P0/P1 already exists as a GitHub
  issue filed by the project's own 2026-09 forensic audit, most with the same file:line
  evidence: LOD prefix truncation = #1053, eviction LRU inversion = #1052, SPZ +0.5 and raw
  PLY legacy decode = #1056, raw-storage save = #774 (+#606), raster dispatch silent = #981,
  `clear()` without `changed` = #1002, sorter grow residual = #983, cap flicker = #1131,
  overlap records = #54 (accepted limitation), TAA ghosting = #1025 (accepted limitation),
  required check only `agentic-pr-gate` = #889 and `docs/governance/github-settings.md`.
- The audit's **priorities partly contradict decisions the maintainer has already taken**.
  The HLOD ADR (`docs/architecture/adr-hlod-streaming.md`, accepted 2026-10-02) is the
  designated fix for #1053 and #1131 and explicitly rejects "pass the skip stride to the
  GPU" and "importance prefix as the format" as rebuild-forcing shortcuts. The colour
  contract fix is already designed in `docs/architecture/adr-splat-colour-encoding.md`
  (accepted 2026-09-23); #1054 (signed SH) landed in 5fc3b655, #1056 (DC contract) is the
  open half.
- What the audit **did not have** and this plan adds: 38 findings that are tracked nowhere
  (Part 2, N-1..N-38), ten of them P1. The judge-confirmed P1 defects so far: the splat
  screen centre is quantised to whole pixels on the right half of every 1080p frame (N-4),
  every SPZ v2 file loads with wrong orientations (N-14), the "High Quality" import preset
  keeps the first 40 percent of a 2.5M file instead of thinning it (N-15), and the binning
  pass projects every splat twice per frame (N-30). Also: the band-3 SH basis has two sign
  errors (N-1), orthographic cameras get the perspective Jacobian (N-5), the `flip_y`
  convention mirrors the cull frustum for off-axis cameras (C06), XR viewports pay full cost
  and fail every frame (C34), 2DGS and per-splat animation are bound APIs with no renderer
  (N-34, N-36), and a large share of the host and GPU evidence is advisory or unenforced
  (N-2, N-12, N-26). Plus a sequencing that respects the accepted ADRs and the public-alpha
  bar, acceptance criteria per item, and a verified competitive position including the Godot
  addon ecosystem the audit never looked at.

## Part 1: Claim-by-claim verdicts

Legend: CONFIRMED = code at HEAD does what the audit says. PARTIAL = right in substance,
wrong or outdated in detail. REFUTED = code contradicts the claim. Tracked = existing issue.

### 1.1 Defect claims

| # | Audit claim | Verdict | Tracked | Evidence at HEAD and corrections |
| --- | --- | --- | --- | --- |
| C01 | Streaming LOD is prefix truncation | CONFIRMED | #1053 (P1), HLOD ADR | `core/streaming_visibility_controller.cpp:939-943` `effective_count = count / skip`; bake cuts chunks as contiguous source slices (`io/streaming_chunk_bake.cpp:119-120`) and the Morton index array is identity (`:100-102`). No importance ordering anywhere, so a prefix is a spatial sub-block. The ADR's own Problem section (lines 36-43) states this. Fix is **not** a stride: ADR S1a bakes importance-ordered node payloads and S2a replaces `update_chunk_lod_parameters` with a budgeted cut. |
| C02 | Forced visible eviction evicts nearest chunk first | CONFIRMED | #1052 (P1) | `visible_chunk_indices` insertion-sorted nearest-first (`streaming_visibility_controller.cpp:585-600`); `_build_visible_chunk_list` touches each in that order (`gaussian_streaming.cpp:3328-3339`); `touch_chunk_use` = `++chunk_load_counter` (`streaming_eviction_controller.cpp:30-31`); visible victim = smallest stamp (`:194-201`). be213406 added a page-fit filter (`:186-192`) that narrows but does not reorder. Non-primary LRU has the same ordering (`:284-296`). |
| C03 | Full save copies raw storage without read lock | CONFIRMED | #774 (P1), #606 (P2) | `persistence/gaussian_scene_serializer.cpp:527,543`; `get_gaussian_storage()` only calls `_debug_check_raw_storage_access` (`core/gaussian_data.h:679-682`). |
| C04 | GSF loses high-order SH and 2D mode | CONFIRMED | #600 closed by #618 (warn on lossy save) | Closed as "warn + document", not fixed. No open issue tracks a lossless schema. Plan item F-2 reopens this as a v2 format slice. |
| C05 | Raster dispatch returns 0 on success and failure; caller ignores it | CONFIRMED | #981 (P2) | `renderer/tile_render_rasterizer_stage.cpp:160-168,193-195,213`; `tile_renderer.cpp:1269-1293` discards the value; `_finalize_frame()` follows at `:326`. Textures are cleared before `compute_list_begin` (`:188-190`), so a failed begin publishes an empty frame as rendered. |
| C06 | `flip_y` negates only `columns[1][1]`; wrong for off-axis / shifted orthographic | CONFIRMED, UNTRACKED, numerically proven | none (issue search for frustum_offset / flip_y / asymmetric returns nothing) | `renderer/gaussian_splat_renderer.cpp:594,606,2013-2014`. Upstream applies `Projection::set_depth_correction(flip_y)` (`core/math/projection.cpp:787-801`, `m[5] = -1`) as a LEFT-multiplied correction (`render_scene_data_rd.cpp:42-45`), which negates the whole clip-Y output row. Reproduced in python with Godot's `set_frustum` formula (l=-0.5, r=1.0, b=-0.3, t=0.8, n=0.1, f=100): the two flips differ in exactly `columns[2][1]` (+0.4545 vs -0.4545), so the view point (0,0,-1) lands at NDC y = -0.4545 for splats and +0.4545 for meshes, a 0.909 NDC offset = `2(t+b)/(t-b)`. Shifted orthographic differs in `columns[3][1]` (NDC delta 1.0). Consequences: splats displaced vertically against meshes under `Camera3D.PROJECTION_FRUSTUM` with `frustum_offset.y != 0` (`renderer_scene_cull.cpp:2730-2736`), and the GPU culler extracts planes from the single-entry-flipped matrix (`gpu_culler.cpp:1667`), giving a vertically MIRRORED cull volume. Depth paths unaffected. Fix is one line per site: `correction * projection`. The project knows (`test_view_transform.h:448-458`) and has not decided. |
| C07 | `GaussianSplatWorld::clear()` emits no `changed` | CONFIRMED | #1002 (P2), disclosed on the known-limitations page | `core/gaussian_splat_world.cpp:305-319`. Reachable from script only. |
| C08 | Raw PLY load uses legacy sigmoid decode | CONFIRMED | #1056 (P1) | `io/ply_loader.cpp:1091-1105` stores `SH_C0 * f_dc` without tagging; `render_meta` default 0 = `LEGACY_BIAS` (`core/gaussian_data.h:132`); shader `gs_sh_binning.glsl:151-158`. The comment "We add 0.5 here" is false. #1056 has runtime proof (0.600 vs 0.779 for f_dc=+1). |
| C09 | SPZ colour contract inconsistent | CONFIRMED (worse than stated) | #1056 (P1), colour ADR | `io/spz_loader.cpp:639-643` stores `byte/255`, `:381-382` tags `LINEAR_RGB`, shader adds `+0.5`. Niantic encodes `byte = 255*(0.5 + 0.15*dc)`; correct display = `0.5 + 0.2821*dc`. A mid-grey splat (byte 128) renders as 1.0 (white) in GodotGS; contrast slope is 0.15 instead of 0.282 on top of the offset. Only tested against an in-repo inverse writer, never a reference file. |
| C10 | SPZ v4 unsupported | CONFIRMED, UNTRACKED | none | `spz_loader.cpp:47,202` accept v2/v3 only; gzip only; no zstd in `thirdparty/`. |
| C18 | Only `agentic-pr-gate` is required; it does not enforce GPU evidence | CONFIRMED | #889, #1017, #897 | `docs/governance/github-settings.md:19` records the live protection (`["agentic-pr-gate"]`, 0 approvals). The workflow's own comment says evidence is "NOT enforced against a PR anywhere in CI" (`.github/workflows/agentic_pr_gate.yml:79-83`). #1017: `interfaces/**` (sorter, culler, rasterizer, compositor) grades R1. |
| C20 | Benchmark data stale | CONFIRMED (and the number means less than the audit implies) | #523, #790, #842, f38cf76 | `docs/assets/data/benchmark_suite_report.json` `timestamp_utc = 2026-07-19T02:17:35Z`; dense lane 12.34 FPS / 87.47 ms P99 at `node_visible_splats_max = 4,900,000`, lane weight 0.0; `gpu_timing_available = false` on every lane. The dense lane is **196 `GaussianSplatNode3D` instances of a 25k synthetic spiral** (`tests/fixtures/benchmark_asset_manifest.json` lane_metadata), so it measures multi-node submission as much as rasterisation and is not comparable with a single 6.1M-splat scan. The suite was broken from 2026-09-20 until f38cf76 (2026-09-25). |
| C21 | HLOD ADR exists, nothing implemented | CONFIRMED | #1131, #1053, ADR | ADR status line: "Nothing here is implemented yet; section 9 is the slice plan" (8 PRs, ~9,100 LOC + 5,350 LOC tests, seven R3). |
| C27 | Harness guards vacuous green; sRGB test still excluded although the test says fixed | CONFIRMED (the exclusion is the stale half) | #703, #891; #643 closed | Required batches fail on 0 cases, 0 assertions, missing case audit, hollow cases (`run_gpu_harness.py:1110-1233`). REQUIRED: CompositorHazard, RendererPipeline, Lifetime, OutputCompositor, RendererSceneTree, WorldSceneTree, SceneDirectorSceneTree, GpuSorting. ADVISORY: TileRenderer, MemoryStream, Streaming, Integration, NodeSceneTree. Issue #643 was closed 2026-07-25 by merged PR #776, which fixed the renderer but never touched the harness or the waiver manifest, so the only sRGB regression test has not executed in CI since 2026-07-26. `test_gpu_harness_deferred_contract.py` checks waiver/exclude bijection only, never issue state or `expires_utc`. 63 `[RequiresGPU]` cases are unbatched (manifest backlog; #906/#907/#910). |
| C28 | Open-world proof is `continue-on-error` | CONFIRMED (plus a worse sibling) | #1045, #1016, #1051 | `.github/workflows/gaussian_production_gates.yml:492-497`: the only job that runs `open_world_corridor_proof` is schedule/opt-in and job-level `continue-on-error: true`; the workflow's own comment (`:670-673`) records the 2026-09-21 nightly failing while `gh run list` showed green. **New:** the "Enforce readiness booleans" step (`:462-482`) runs only when the `workflow_dispatch` input `enforce_gpu_readiness` is true (default false, `:62-66`), so on every pull_request/push/merge_group run the `summary.json` readiness booleans are collected and uploaded but never enforced. `docs_pages.yml:164` soft-fails `release_acceptance.py` (#1099) and `:95/:203` swallow `build_documentation.py --engine-patch` failures with `|| true`. The proof itself is sound (C13). |
| C13 | Needed-set metrics; the proof fails when the set stalls | CONFIRMED | #1125 residual | `gaussian_streaming.cpp:3309-3350` computes the set; `streaming_queue_pressure_controller.cpp:30-54` accumulates stall seconds only while unserved and without net progress; `benchmark_suite_lane.gd:1233-1386` windows it; `tests/runtime/run_benchmark.py:382-404` contracts (`residency_ratio >= 0.70`, `no_progress_frames <= 6`, `scan_starved_frames <= 6`, `vram_cap_hit_frames <= 0`), null metric fails closed, `main()` returns 1; `check_renderer_release_gates.py:1393-1429` re-evaluates the same contract for a candidate. Residual: `queue_pressure_frames <= 32` counts any backlog over the whole run incl. warm-up (#1125; fixture carries 8156). |
| C33 | Docs still carry false claims (`.splat`, `GaussianSplat3D`, COLMAP, `get_streaming_system`, ...) | REFUTED (fixed since) | docs series S1-S10, 2026-10-01/02 | No hit for `.splat`, `GaussianSplat3D` or COLMAP in `docs/` or `overrides/`; `docs/features/ply-loader.md:23` and `docs/features/streaming.md:114` now state the unbound methods; `streaming.md:93` documents the prefix truncation honestly. The docs half of the external analysis has been executed. |
| C34 | No multiview/XR | CONFIRMED (and it fails loudly rather than refusing cleanly) | none; out of alpha envelope by decision (`release-acceptance-bar.md:491-493`) | For `view_count = 2` the module reads only `scene_data->cam_projection` (`gaussian_splat_renderer.cpp:2426`), which for stereo is the engine's single COMBINED frustum (`renderer_scene_render.cpp:52-180`); `view_projection[v]` / eye offsets are never read (zero hits). The pre-upscale hook is gated `view_count == 1` (`render_forward_clustered.cpp:2688`), so multiview falls to the legacy post-scene hook after tonemap; the destination is a 2-layer `TEXTURE_TYPE_2D_ARRAY`, the compute composite refuses it, and the graphics fallback's `framebuffer_create` fails (`rendering_device.cpp:2933`, layers 2 != view_count 1) with an RD error plus a module error every frame. Net: full cull/sort/raster cost paid with the wrong projection, no splats reach either eye, per-frame error spam, no warning keyed on `view_count`. No test sets `view_count != 1`. |
| C35 | Linux has no GPU evidence, macOS none | CONFIRMED | #596, #1015, limitations page | `release-acceptance-bar.md:42-44`: Linux lane runs neither `qa` nor `sorting`; macOS `build-supported`, unvalidated. |

### 1.2 Progress claims (positive)

| # | Audit claim | Verdict | Evidence |
| --- | --- | --- | --- |
| C11 | 1,024-splat atlas pages, byte-bound VRAM budget | CONFIRMED | `ATLAS_PAGE_SPLATS = 1024`; best-fit smallest-run allocation with lowest-address tie-break; exact-boundary coalescing on insert; page ceiling clamped by `(budget_bytes - aux) / page_bytes` before the persistent buffer is created; evict-until-fit bounded by the per-frame eviction budget; pages/largest-free-run telemetry. All GLSL indexes via `ChunkMetaGPU.atlas_base/splat_count` published from the allocator run; remaining `CHUNK_SIZE` uses are count bounds, not strides. Eviction never releases an `upload_pending` run. #1135 confirmed from code (a blocked eviction sets `eviction_blocked` for the rest of the frame). **Evidence gap:** the five #1088 doctests live in the `[Streaming Pipeline]` module lane, which `tests/ci/run_module_tests.py:222` declares with `strict = False` ("advisory lane, continuing", `:3888`), so an allocator regression cannot fail CI (#519). |
| C12 | Demand bounded by draw distance | PARTIAL | Discovery (grid box clamp and per-chunk near-distance cull), load scan, needed set, zero-visible recovery, predictive prefetch and the visible branch of the sync drain all honour `load_distance_limit`. #1141 confirmed: instance-asset residency requests load every requested chunk with no distance test (`render_streaming_orchestrator.cpp:626`, `streaming_atlas.cpp:268`), and the sync drain's predictive branch admits queued chunks by centre distance only (`gaussian_streaming.cpp:4616-4624`). Limit is 0 = unbounded when the culler does not exist or LOD is disabled, and for direct-API users. With several nodes on one renderer the last `set_lod_max_distance` wins. Its #1087 tests are also in the advisory lane. |
| C13 | Needed-set metrics | CONFIRMED | `gaussian_streaming.cpp:3309-3350` publishes `last_needed_*`; a7800538 and e736aaed rework the corridor proof. Residual: #1125 (queue_pressure_frames semantics). |
| C14 | Main RenderingDevice hand-back fix | CONFIRMED | 0e78f528 (#1077, #1145), backport of godotengine/godot#123391; no CI lane asserts it yet (#1148). |
| C15 | Shadows fail-closed | CONFIRMED (commit) | 7a9b170f "fail-closed splat shadow pass, write no depth it did not rasterize". Shadow casting itself is still a design item (#1095); splats cast no shadows today (limitations page). |
| C16 | Tile overflow setting honoured, loud | CONFIRMED (commit) | 58add91f (#1137). #1139: CI never exercises the shipped per-tile cap. |
| C17 | Painterly lit through the scene lighting producer | CONFIRMED | eed9879e (#851, PR #1078). |
| C19 | Production gates green on HEAD; Release-CI runtime red only at contention postflight | CONFIRMED | Runs on `ae555497`: Gaussian Production Gates success (37196960968), Docs success, Release-CI Runtime Evidence failure (37198854208, schedule). In that run every step through "Run release-ci runtime evidence lane (windows-vulkan)" and the report upload succeeded; the only failing step is 14, "Postflight - GPU contention verdict (#875)". The run was voided for contention, not failed on content. Preceding pushes (0e78f528, 58add91f, c4abb0c8) have green Production Gates, Release Builds, Baseline QA and Shader Validation. |
| C22 | Sorter path: fail-closed on unsorted translucency, bounded backoff, replace-before-retire on grow, RIDs republished | CONFIRMED | `renderer/sort_fallback_policy.h:181-192` (fail-closed default), `:321-335` (backoff 60..1800 frames, never latches), `tile_renderer.cpp:309-311,1179` (frame rejected), `tile_render_resources.cpp:1501-1522` (create first, retire only on success), `render_sorting_orchestrator.cpp:380,462-466,498-501` (republish on both exits). No sorter-less translucent draw path (painterly and shadow pass share `TileRenderer::render`). Residuals, all tracked: #983 (grow whose key/value buffers fail still loses both and is not counted as a grow failure), #978 (`validate()` does not bound `max_overlap_records` by device size), #926 (two init exits clear `sorter_needs_rebuild`). **Evidence gap:** the four GPU tests for this behaviour sit in the `TileRenderer` harness batch, which is NOT in `REQUIRED_BATCHES`; only the #980 republish test (`WorldSceneTree` batch) is required. |
| C23 | Raster early-exit flag reset after the barrier; no remaining shared-memory race | CONFIRMED | `shaders/tile_rasterizer_compute.glsl:239-284`: reset at 252 sits between barriers 241 and 264, read at 284 after barrier 281; every shared write/read pair is barrier-separated; bare `barrier()` is spec-correct for shared memory in compute. `tile_binning.glsl` and `tile_resolve.glsl` declare no shared variables. Residual: no test or lint guards the ordering (0fcefee changed the shader only). |
| C24 | SH stored signed (SNORM10, per-splat scale) with a GPU reference test | PARTIAL (storage holds; the band-3 basis is wrong, see Part 2 N-1) | Packer `renderer/gaussian_gpu_layout.cpp` (`encode_sh_snorm10`, q = round(v*511/scale) clamped, scale fp32 in `sh_dc.w`) and decoder `gs_sh_binning.glsl` (`bitfieldExtract` sign-extension, step scale/511) match bit for bit; band-1 basis and view direction match Inria after #1063. Host round-trip test in the strict module lane; GPU readback test for band 1 in the `RendererSceneTree` batch, which IS required. Quantized route truncates band 2 to 3 of 5 coefficients and drops band 3 (consistent on both sides, undocumented to users). No test covers band 2/3 basis values. |
| C25 | TAA jitter enters the projection once and keys the render cache | CONFIRMED | Engine fills `scene_data->cam_projection` unjittered (`renderer_scene_render_rd.cpp:1373`); the module jitters only in `build_render_projection` (`+jitter*w` on clip x/y, same as `RenderSceneDataRD::update_ubo`); `tile_binning.glsl:569-570` focal lengths unaffected. Cache key covers painterly flag, viewport size, camera and GPU projection, view transform, content generation, cull/colour-grading/lighting signatures, depth validity; exposure is hashed in the grading signature. Test `test_view_transform.h:356` plus cache-refusal test `test_renderer_pipeline.h:5397`. **The off-axis flip divergence is known to the project**: `test_view_transform.h:448-458` records it and declines to assert it "because asserting it would pin a behaviour nobody has decided on". No issue exists. |
| C26 | Pre-upscale composite writes source-over alpha | CONFIRMED | `viewport_blit.glsl` (all three format variants) writes `src.a + dst.a*(1-src.a)` when `destination_has_alpha`, which the host sets to `pre_upscale_phase` (`output_compositor.cpp:1795`); painterly composite and legacy `copy_to_fb_rect` use ONE/ONE_MINUS_SRC_ALPHA blend. The legacy post-scene phase (Forward Mobile, multiview, reflection probes) still writes 1.0 by design. GPU test `test_output_compositor_composite_hazard.h:855` is in the required `OutputCompositor` batch. |
| C30 | `submit_world_submission` applies renderer mutations outside `world_mutex` | CONFIRMED | Three phases: arbitration under a scoped `ThreadOwnedMutexLock(world_mutex)` (`gaussian_splat_scene_director.cpp:2383-2423`), unlocked apply (`:2428-2430`, the helper reports a violation if the lock is held, `:930`), re-validation R1-R4 under a fresh scope (`:2461-2546`), deferred queue flush after unlock (`:2564`). The only null-queue call (`_get_or_create_world(world, false)`, `:521`) skips the block containing the inline fallback, so that fallback is unreachable; every production entry carries a `RendererContractWorkQueue`. No RenderingDevice lock is taken inside a `world_mutex` section. The stale "one remaining known violation" comment is wrong; the audit read the code correctly. Still live, all tracked: #701 (deferral window observable by the render thread; R3 treats a concurrent release as a free slot), #863 (`GaussianSplatWorld3D` ignores ENTER/EXIT_WORLD), #870 (cached renderer never rebound on a World3D switch). Static guard `tests/ci/check_renderer_contract_boundary.py` exists. |
| C31 | Asset materialisation is transactional; payloads are sealed | PARTIAL | `populate_gaussian_data` (`gaussian_splat_asset.cpp:1649-1780`) stages, checks every lane size after allocation and publishes once; `populate_from_asset` the same; sealing through `populate_mutex` with `copy_from` as the sanctioned re-seal path. Not transactional by design: `populate_from_gaussian_data` resets the asset to an empty coherent state on allocation failure and emits `changed` (`:1971-2039`). **New P2 (N-24):** a hot reload through `Resource::copy_from` (the `reload_asset` path, `node_3d.cpp:1980-1981`, `CACHE_MODE_REPLACE`) rewrites the lanes via packed setters that never bump `payload_version` (only `:2037` and `:2218` do), so the director's cached `GaussianData` keeps the old payload. |
| C32 | Malformed PLY/SPZ fail closed; non-finite data rejected before upload | PARTIAL | Loader bounding confirmed in detail (PLY count 1..2^30, u64 extent proof, list properties rejected, big-endian swapped, ASCII pre-validated; SPZ header, u64 payload arithmetic, exact consumption). The finite validator runs on the file-load paths and on `GaussianData::create/update_gpu_buffer`. **New P2 (N-25):** the resident-atlas publisher (`resident_instance_contract_publisher.cpp:671,700`), `gpu_buffer_manager.cpp:683`, `gpu_memory_stream.cpp:440` and `streaming_upload_pipeline.cpp:1333` pack `get_gaussian_storage()` without it; the non-quantized packer copies position/opacity/scale/rotation verbatim (`gaussian_gpu_layout.cpp:102-124`); `GaussianSplatRenderer::set_gaussian_data` and the `.gsplatworld` loader (`gaussian_splat_world_io.cpp:924`) perform no finiteness check. A NaN from `GaussianSplatNode3D.set_splat_data` or a runtime edit reaches the GPU on the default node route. None of the malformed-corpus tests are in a required GPU batch. |

### 1.3 Where the audit misleads

- It presents #1053/#1052/#1056/#774/#981/#1002 as discoveries. They are the project's own
  issues, several with runtime proof the audit did not cite.
- It recommends "pass the skip stride to the GPU" for LOD. The accepted HLOD ADR rejects
  that (section "What would force a rebuild", lines 791-811). Doing it would waste an R2 PR.
- Its numeric scores (7.3/10 etc.) and the star matrix are not measurements. They are
  dropped here.
- "Dense benchmark at 4.9M splats" is a synthetic lane with weight 0.0 and no GPU timing;
  it is not a renderer benchmark anyone should quote, in either direction.
- The docs-site findings were already executed by the S1-S10 docs series between
  2026-10-01 and 2026-10-02 (commits 6a9d7217 ... efb2c8bf); re-verified in C33.

## Part 2: Findings the audit missed

Each finding carries the tracked issue or "none", and says whether it was re-read in the
code here, judge-confirmed, or is finder-reported only. The first table holds the items
re-read here; the later tables group the finder reports by subsystem.

| # | Finding | Sev | Tracked | Evidence |
| --- | --- | --- | --- | --- |
| N-1 | Band-3 SH basis disagrees with the Inria/3DGS reference: indices 11 and 13 are sign-flipped and index 14 is doubled. Re-derived here: Inria `SH_C3[2] = -0.4570 * y * (4zz-xx-yy)` equals `+0.457*y*(1-5zz)` on a unit direction, GodotGS writes `-SH_C3_2 * y * (1-5zz)` with `SH_C3_2 = +0.4570`; same for index 13 with x; index 14 uses `SH_C3_1 = 2.8906` where Inria uses 1.4453. Bands 0-2 and band-3 indices 9, 10, 12, 15 match. In the 12-slot unquantized layout slots 8-11 hold band-3 indices 9-12, so the index-11 sign error is reachable on every degree-3 PLY rendered with `sh_bands = 3`; 13 and 14 are latent. | P2 | none (search returns only closed #1054, #1063) | `shaders/includes/gs_sh_binning.glsl:68-71,127-133`; mirror in `gaussian_splat_common_inc.glsl` (dead path, #1064) |
| N-2 | Much of the renderer's and streaming's host-test evidence cannot fail CI. In `tests/ci/run_module_tests.py:190-262` the lanes `GaussianSplatting [untagged]`, `[Renderer]`, `TileRenderer`, `GPU Memory Stream`, `Streaming Pipeline` and `[requires-RD]` are declared `strict = False` (only `[DataAuthority]`, `[Lifetime]` and a few tagged lanes such as `[SHEncoding]` are strict); the `Streaming Pipeline` lane holds every #1087/#1088 host test. On the GPU side the `TileRenderer` harness batch with the four #586 sorter fail-closed tests is not in `REQUIRED_BATCHES`, and the #1050 barrier ordering has no test or lint at all | P1 (evidence) | #519, #705, #906 | `tests/ci/run_module_tests.py:222,3888`; `tests/ci/run_gpu_harness.py` REQUIRED_BATCHES; `tile_renderer_regression_test.cpp:3271-3359` |
| N-3 | Quantized streaming route silently reduces SH to band 1 plus 3 of 5 band-2 coefficients (6 slots) with no user-facing disclosure | P3 | none found | `renderer/gaussian_gpu_layout.cpp` quantized packer (6-slot synthesis), per V2 verifier |
| N-4 | Splat screen centre is stored as fp16 in pixel units in the raster payload: ulp is 1.0 px for x in [1024, 2048) and 2.0 px above, so on any 1080p frame the right half renders every splat centre snapped to integer pixels (always 0.5 px off the pixel centre), 4K up to 1 px. Binning pads its bounds by the quantisation error (`quant_pad`) so coverage stays consistent, but the raster evaluates the conic at the snapped centre: softer, shimmering thin content on the right/bottom, and the sub-pixel TAA jitter (0.25 px) is largely nullified there. Re-read here; **adversarial judge: not refuted (0.86)**: both payload layouts quantise, `viewport_size` is the full internal size at `scaling_3d_scale = 1.0`, no later stage re-derives the centre, and the project moved depth from f16 to fp32 for exactly this shimmer class (`tile_projection_common.glsl:13-16`). The judge rated the residual P2 (visible softness/shimmer, not a wrong image); this document keeps P1 because it affects half of every 1080p frame on default settings and defeats TAA there. | P1 (judge-confirmed; judge residual P2) | none (an internal record `GS-AUDIT-SHD-005` is cited in #1053's cross-review, no issue) | `shaders/includes/tile_projection_common.glsl:30-32` (`packHalf2x16(screen_pos)`), `tile_binning.glsl:559` (pixel units), `:838-841`, `:1091` (`quant_pad`), `tile_raster_common.glsl:400,509,564,692` |
| N-5 | Orthographic cameras are accepted end to end (`depth_compute.glsl` ortho branch, `tile_resolve.glsl:187` `gs_is_ortho`, culler passes `is_orthogonal()`), but `project_gaussian_2d` always applies the perspective Jacobian (`z_inv = -1/safe_depth`, `focal * view.xy * z_inv^2`), so an orthographic view shrinks every splat footprint by 1/depth^2. The editor's orthogonal viewport modes and any `PROJECTION_ORTHOGONAL` Camera3D render a dotted point cloud. Re-read here; **judge: not refuted (0.9), P1**: positions and depth stay correct (so it looks like "dust at depth" rather than a total failure), and because `focal_x/y` are derived from `projection[0][0]/[1][1]` the `focal_length_reject` (`tile_binning.glsl:576`) fires for every splat once the ortho size exceeds the viewport height (1080 units at 1080p), leaving the frame empty. Nothing in the module or the engine hook gates on `cam_orthogonal`. | P1 (judge-confirmed) | none | `shaders/tile_binning.glsl:569-576,618-655`; `tile_render_stages.cpp:253-258` |
| N-6 | `gs_pack_color_r11g11b10` truncates the fp16 mantissa (`>> 4u`, `>> 5u`) instead of rounding: every payload colour is biased dark by up to 1.4 LSB (R/G) and 3.2 LSB (B) at 8 bit; the zero-mean dither cannot cancel a one-sided bias; white renders ~(252,252,250). | P3 | none | `tile_projection_common.glsl:45-68`; `tile_raster_common.glsl:94` |
| N-7 | No NaN/Inf guard on the payload colour: `max(NaN, 0)` is undefined in GLSL, the R11G11B10 pack preserves the fp16 NaN exponent, and the compute raster accumulates it into every pixel of the footprint. A degenerate light direction or an extreme grade on one splat blacks out its whole footprint. | P3 | none | `tile_binning.glsl:1408`, `tile_projection_common.glsl:47`, `tile_raster_common.glsl:432,471` |
| N-8 | `OneSweepSort` scatter places with `atomicAdd(local_offsets[digit])`, which is not stable, so its 4-pass LSD radix sort orders keys only by the top 8 bits. Latent today: #635 records that OneSweep and Bitonic are unreachable because `requires_indirect` is hardcoded; becomes live the day #635 is "fixed" without noticing this. | P3 (latent) | none; related #635 | `renderer/gpu_sorter.cpp:3197-3227,3419-3429` |
| N-9 | Nodes mutate the engine-side `RendererRD::GaussianSplatStorage` directly from the scene thread (`gaussian_set_renderer`, `gaussian_set_aabb`, `gaussian_set_casts_shadow`, `gaussian_free` from ENTER/EXIT_TREE, transform updates and destructors) instead of through the RenderingServer command queue; the storage is `mutable RID_Owner<GaussianSplat>` with the default `THREAD_SAFE = false` holding a plain `Ref<GaussianSplatRenderer>`, and `render_scene` reads it on the render thread. Under `thread_model = 2` (the starter template's model) freeing a node mid-frame races a `Ref` release against the render thread's read; worse, at EXIT_TREE the queued `instance_set_base(render_instance, RID())` lands AFTER the direct `gaussian_free`, so the in-flight frame's cull list can still name a base whose storage slot is being destructed (`gaussian_splat_node_3d.cpp:2459-2468`, `renderer_scene_render_rd.cpp:1507`). Re-read here (storage header and both node call sites); the finder rated it P2, this document rates it P1 because the starter template ships `thread_model = 2` and the limitations page records that no CI lane renders a splat node in that mode. Not judged (below the finder's P1 bar); needs a TSAN or provoked-free reproduction before a fix lands. | P1 (thread_model 2; re-read, not judged) | none; same family as #1077/#1133 | `servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.h:54`, `.cpp:66-80`; `nodes/gaussian_splat_node_3d.cpp:2444-2488`; `nodes/gaussian_splat_world_3d.cpp:755-775` |
| N-10 | Cached-render reuse is ON by default in code (`output_compositor.h:289`, `.cpp:133`) while `doc_classes/GaussianSplatRenderer.xml:1036` documents `default="false"`; its reuse key omits raster inputs that have no `invalidate_cached_render()` on their setters (painterly knobs other than enabled, debug/preview toggles, Jacobian diagnostics, low-pass filter, pipeline features), so a static camera can keep showing the previous parameters after an inspector edit. Doc mismatch verified here; key coverage per finder, judge pending. | P2 | none; related #114 | `interfaces/output_compositor.h:134-140,289`, `.cpp:133,165-168` |
| N-11 | `render_sorted_splats()` (shadow-pass entry) passes a default-constructed `StageMetrics`, and `RasterCompositeStage::execute` prefers `metrics->sort.sorted_count` over the snapshot whenever metrics is non-null, so the raster can receive `sorted_splat_count = 0` and take the zero-splat path. Structure verified here (`render_pipeline_stages.cpp:1816-1823`, `gaussian_splat_renderer.cpp:2703-2708`); whether the sort stage fills `stage_metrics.sort` earlier in the same pass is for the judge. If real, it is a concrete mechanism behind "splats cast no shadows" (#1089/#1095). | P3-P2 | #1089/#1095 (new angle) | see left |
| N-12 | The Production Gates collect `summary.json` readiness booleans (`897_907_ready`, `900_ready`, `871_ready`, `815_ready`, `902_ready`) on every run but enforce them only when the `workflow_dispatch` input `enforce_gpu_readiness` is true, whose default is false; on pull_request, push and merge_group runs a false boolean passes. Verified here. | P1 (evidence) | none | `.github/workflows/gaussian_production_gates.yml:62-66,462-463` |
| N-13 | The GPU-harness waiver guard checks only that excludes and waivers are a bijection; it never checks the waiver's issue state or `expires_utc`, which is how the #643 waiver outlived its fix by ten weeks | P2 (evidence) | none | `tests/ci/test_gpu_harness_deferred_contract.py:283-311`; `run_gpu_harness.py:102-114` |
| N-14 | SPZ v2 rotations are decoded as signed `int8 / 127` (`spz_loader.cpp:670-677`) while the reference packs them unsigned with a 127.5 offset (`q * +-127.5 + 127.5`, Niantic `packQuaternionFirstThree`, confirmed in 3.1): a zero component (byte 127/128) decodes as +1.0 / -1.008 and signs invert for every byte above 127, so every v2 file loads with garbage orientations, silently (the identity quaternion saturates all three components and lands on the `sum_sq >= 1` branch with w = 0, a 180-degree rotation). The only test is a writer that inverts the loader, and the only SPZ import test uses identity rotations. Re-read here; **judge: not refuted (0.8)**; the external encoding premise is confirmed by the Niantic source read in 3.1. | P1 (judge-confirmed) | none (search returns only closed #461) | `io/spz_loader.cpp:663-691`; `tests/synthetic_spz_writer.cpp:49-53`; `tests/test_spz_importer.h:36` |
| N-15 | Importer `max_splats` is a file-order PREFIX cut whenever `density_multiplier` is 1.0: `merge_density = density_multiplier < 0.999 && final_count < original_count` (`resource_importer_ply.cpp:353`), so the write loop takes indices `0..final_count-1`. The built-in "High Quality" preset is `max_splats = 1,000,000, density_multiplier = 1.0` (`io/gaussian_import_preset.cpp:75-76`): a 2.5M-splat scan imported with it keeps the first 40 percent of the file, i.e. the SfM seed region plus whatever densified first, and the log reports success. Re-read here. The alpha's pinned presets (Ultra uncapped, Desktop 0.7 density) avoid it, which is why nobody has seen it. **Judge: not refuted (0.86)**; the project's own `docs/architecture/adr-import-importance-pruning.md:126-143` describes exactly this head-truncation and froze it as a back-compat invariant instead of fixing it, so this is a design decision to revisit, not an oversight. | P1 (judge-confirmed) | none (#456 added importance pruning but left this path) | `io/resource_importer_ply.cpp:94-100,353-355,368-370,436-441`; `resource_importer_spz.cpp:311,399-404` |
| N-16 | `GaussianData::save_to_file` / `GaussianSplatAsset::save_to_file` (PLY writer) drop all view-dependent SH (no `f_rest_*`) and write `f_dc = sh_dc / SH_C0` regardless of the DC-encoding tag, so an SPZ-origin payload exports +0.5 too bright and a degree-3 edit round-trips to flat colour, with no warning (the GSF writer at least warns, #600). | P2 | none | `core/gaussian_data_io.cpp:333,380` per finder |
| N-17 | `gaussian_splat_merge_sources` bakes the source rotation into quaternions and normals but `memcpy`s SH bands 1-3 unrotated, so any merged world with a rotated source has view-dependent colour answering to the wrong camera direction. | P2 | none | `core/gaussian_splat_merge_utils.cpp:365-384` per finder |
| N-18 | `.gsplatworld`, `.gsplatcache` and `.gsf` payloads are raw `memcpy` of the authoring `Gaussian` struct with no layout fingerprint; only `sizeof == 144` pins them, and the header comment records the layout already changed once at constant size. A same-size field swap misdecodes every existing file without any check firing. | P3 (latent, data integrity) | none (#572 is mtime freshness) | `io/gaussian_splat_world_io.cpp:24,404,765` per finder |
| N-19 | The SPZ `antialiased` flag reaches import metadata (`spz_antialiased`) and stops there; nothing in materialisation or the GPU flags consumes it (ties to Q-1). | P3 | none | `io/spz_loader.h:84`, `io/resource_importer_spz.cpp:555` |
| N-24 | Hot reload via `Resource::copy_from` does not reach the director's cached `GaussianData`: the packed lane setters never bump `payload_version`, and `edited_version` is TOOLS-only and never bumped by a load; the node keeps rendering the pre-reload payload until something else resubmits. | P2 | none | `core/gaussian_splat_asset.cpp:881-890,2037,2218`; `nodes/gaussian_splat_node_3d.cpp:1980-1981` (V5 verifier) |
| N-25 | The finite validator is bypassed on the default resident route: the resident publisher, GPU buffer manager, memory stream and streaming upload pack raw storage verbatim; `set_gaussian_data` and the `.gsplatworld` loader check nothing. A NaN set from GDScript or produced by a runtime edit reaches `cov3d` and the sort keys. | P2 | none (related #606) | `renderer/resident_instance_contract_publisher.cpp:671,700`; `renderer/gaussian_gpu_layout.cpp:102-124`; `io/gaussian_splat_world_io.cpp:924` (V5 verifier) |

Feature-surface findings (inventory from code; the three P1 items go to a judge):

| # | Finding | Sev | Tracked | Evidence |
| --- | --- | --- | --- | --- |
| N-34 | `is_2d_mode` / 2DGS surfel mode is a public flag with no renderer consumer: `GaussianSplatNode3D.set_splat_data(..., is_2d_mode)`, `GaussianData.set_2d_mode` and the `GS_GPU_ASSET_FLAG_IS_2D` chunk flag exist, but no shader reads `GS_INSTANCE_FLAG_IS_2D` / `GS_ASSET_FLAG_IS_2D` (only the constant definitions in `gs_instance_layout.glsl`); `project_gaussian_2d` always builds a 3D covariance. The only effect is that `save_to_file` writes normal columns. **Judge: not refuted (0.85), severity P2**: the flag is plumbed to the GPU (`_get_instance_flags`, `resident_instance_contract_publisher.cpp:557`, `streaming_global_atlas_registry.cpp:42-43`) and never read; `GaussianData.xml:124,380` promise "oriented 2D surfels"; and the PLY loader sets 2D mode whenever `nx/ny/nz` columns exist (`ply_loader.cpp:563-566,892-897`), which stock INRIA PLYs carry as zeros, so most imported PLYs are silently flagged 2D and rendered as 3D anyway. The "zero third scale culled" detail in the finder text is not how real 2DGS PLYs fail | P2 (judge-confirmed, feature honesty) | none | `shaders/includes/gs_instance_layout.glsl:28,34`; `ply_loader.cpp:563-566,892-897`; `doc_classes/GaussianData.xml:124,380` |
| N-35 | Third-order SH is truncated on the GPU: `PackedSphericalHarmonics::MAX_ENCODED_COEFFICIENTS = 12` for a 15-triplet degree-3 asset, so basis indices 13-15 never contribute on the resident path, and the quantized atlas keeps 6 slots (band 1 plus 3 of 5 band-2 triplets, nothing of band 3). The shader is written for the full set and `rendering/sh_bands` advertises 3. Corroborates N-3 and bounds N-1's reachable error to index 11. **Judge: not refuted (0.86), severity P2**: `select_sh_coefficients()` stores 3 + 9, every production packer passes the default limit so per-splat metadata carries `high_count = 9`, the shader loop covers `basis[4..12]` only; quantized path 3 + 3 (band 2 loses `basis[7..8]`, band 3 entirely); `SHConfig` default is `SH_BAND_3`, so every imported degree-3 PLY on default settings is affected; nothing warns. l=3 terms are small in trained scenes, hence P2 | P2 (judge-confirmed, undisclosed quality loss) | none (#1054 is the sign clamp) | `renderer/gaussian_gpu_layout.h:259` (`MAX_ENCODED_COEFFICIENTS`), `select_sh_coefficients()`, `GS_QUANTIZED_SH_ENCODED_SLOTS = 6`, `sh_config.h:43` |
| N-36 | `GaussianAnimationStateMachine` (~40 bound methods) and `GaussianData.update_animation / get_animated_*` have no render consumer: nothing in `nodes/`, `renderer/`, `interfaces/` or `editor/` references them; authored keyframes never reach the GPU unless a script copies sampled values back per splat per frame | P2 (feature honesty) | none (#599, #1106 are different) | grep across the module (finder) |
| N-37 | `doc_classes/GaussianSplatNode3D.xml` defaults disagree with code for `temporal_blend` (0.5 vs 0.9), `seed` (0 vs 1337), `update_mode`, `preview_enabled`, `show_lod_spheres`, `overlay_opacity`, `wind_enabled`, `wind_direction`; `check_doc_classes_complete.py` checks presence and brief only | P3 (docs) | none | `doc_classes/GaussianSplatNode3D.xml` vs the node's constructor and setters (finder) |
| N-38 | Shadow CASTING into the Godot shadow atlas exists end to end (`gaussian_set_casts_shadow` -> `renderer_scene_render_rd.cpp:1516-1519` -> `render_forward_clustered.cpp:200-258`, omni cube included), which the limitations page entry "Splats cast no shadows" (#1089/#1095) does not say. N-11 (default `StageMetrics` zeroing `sorted_splat_count` on the shadow entry) is a candidate mechanism for the pass rendering nothing. Needs a GPU run to settle; recorded as a lead, not a verdict | lead | #1089, #1095 | finder (feature surface) + N-11 (renderer finder) |

Re-read here for N-34..N-38: `IS_2D` occurs in `shaders/` and `compute/` only as the two
constant definitions (`gs_instance_layout.glsl:28,34`); `MAX_ENCODED_COEFFICIENTS = 12`
(`gaussian_gpu_layout.h:259`) against 15 triplets for degree 3; the animation API is
referenced outside `core/` and `tests/` only by the persistence layer, `register_types.cpp`
and its own files; `renderer_scene_render_rd.cpp:1516-1517` does read `gaussian_get_casts_shadow`.

Feature inventory (code-verified by the finder): PLY ascii/binary LE/BE, SPZ v2/v3, import
presets with importance pruning and per-chunk quantization, SH 0-3 import, GPU SH to band 3
with amortisation, Mip-Splatting low-pass with alpha rescale, shadow casting and receiving,
clustered direct lighting, sphere effectors and wind, colour grading with bake/restore,
editor hot reload, multi-asset merge, streaming worlds. PARTIAL: orthographic cameras, SH3 on
GPU, PLY export, per-splat animation, editing (paint overlay only). ABSENT: 2DGS rendering,
.splat/.ksplat/SOG/compressed-PLY/glTF importers, SPZ v4, SPZ writer, selection / delete /
crop / transform API, collision export, XR/multiview, `get_aabb` binding (#1024).

Performance findings (finder-reported with code evidence; the two P1 items go to a judge):

| # | Finding | Sev | Tracked | Evidence |
| --- | --- | --- | --- | --- |
| N-29 | Two full 64-bit radix sorts per moving-camera frame: the instance pipeline pre-sorts every visible splat by (depth, atlas_index) through the 16-pass sorter, then the tile pipeline re-sorts every overlap record by (tile, depth, tie). With `global_sort_enabled` forced true the pre-sorted order is consumed only as a projection-buffer slot and a 16-bit tie-break, neither of which needs depth order. Estimated 15-25 percent of the top GPU cost plus 7 descriptor sets and 64 dispatches of CPU recording. **Judge: not refuted (0.82)**, impact P2: in the 64-bit path the tile key already carries the full fp32 depth, so the pre-sort rank only breaks bit-identical depths; no stage on the default tile route reads the pre-sorted order, and `depth_compute.glsl:236` already writes the identity permutation, so the sort can simply be skipped there | P1 perf (judge-confirmed, impact P2) | none (#20, #481 generic) | `compute/depth_compute.glsl:235-236`; `interfaces/gpu_sorting_pipeline.cpp:2887,3181`; `tile_renderer.cpp:506,1109`; `tile_binning.glsl:352-357,712,1422,1460` |
| N-30 | `tile_binning.glsl` is one `main()` compiled twice: COUNT and EMIT each run the full projection (splat + instance + chunk-meta loads, wind, `project_gaussian_2d`, eigen, conic rebuilds, tile rect) per visible splat, and every overlap record costs two contended per-tile atomics; COUNT hands EMIT only a 1-bit visibility word. State of the art projects once and writes payload + per-splat tile count. **Judge: not refuted (0.88)**, residual rated P2: `global_sort_enabled` is forced true every frame (`tile_renderer.cpp:506`), so every frame runs COUNT -> prefix -> EMIT, and a descriptor-invalidating overlap resize re-runs COUNT a third time (`:1010`); record sizes confirmed by static_asserts (PackedGaussian 128 B, quantized 80 B, instance 112 B, chunk meta 64 B) | P1 perf (judge-confirmed, impact P2) | #20 (scope only) | `shaders/tile_binning.glsl:703-1181,819,991,994,1201,1444`; `tile_renderer.cpp:506,897-1065` |
| N-31 | Radix pass count is pinned to `key_bits = 64` (16 passes of 4 bits) although live bits are 32 depth + 13 tile + tie; `tie_bits = MIN(16, 32 - tile_bits)` pads the tie-break to the maximum instead of the minimum, so 4 of 16 passes order records that already compare unequal on depth | P2 (perf) | #20, #957 | `renderer/gpu_sorter.cpp:1681,2740`; `tile_renderer.cpp:2370-2372` |
| N-32 | The compute rasterizer declares ~41 KB of shared memory per 256-thread workgroup (`SPLATS_PER_TILE = 1024` records of 36 B plus indices), which caps occupancy at one workgroup per CU on 64 KB-LDS parts and cannot compile at all on 16-32 KB devices; `shader_compilation_helper.cpp` only warns and the renderer silently falls back to the fragment path | P2 (perf/portability) | none | `shaders/tile_rasterizer_compute.glsl:27,132,139`; `renderer/tile_renderer.h:37` |
| N-33 | About 20 descriptor sets are created and freed per frame on the hot path (6 sorter pass sets plus indirect set per sort, twice; binning lighting set for COUNT and EMIT; three resolve sets; prefix params) | P3 (perf) | none | `gpu_sorter.cpp:1361-1409,2671,2846`; `tile_render_binning.cpp:616-623`; `tile_render_resolve.cpp:1143-1156` |

Finder strengths worth recording (performance): the production frame path is GPU-driven with
no synchronous readback (indirect dispatch from GPU counts); param UBO uploads are hash-gated;
the compute raster stages indices and payloads cooperatively and exits saturated tiles early;
SH is evaluated once per visible splat in EMIT; the 80-byte quantized layout carries SNORM10
SH; overlap drops are counted via a sticky signal.

CI / evidence findings (finder-reported, consistent with N-12/N-13 which were re-read here):

| # | Finding | Sev | Tracked | Evidence |
| --- | --- | --- | --- | --- |
| N-26 | `collect_production_evidence.ps1` runs the module tests but no readiness boolean reads the result (`902_ready` is built from the four runtime scripts only), `943_ready` is excluded from both enforcement lists, and the step ends with "Evidence collection completed." regardless of sub-command exit codes | P2 (evidence) | #897 partially | `tests/ci/collect_production_evidence.ps1:303,601,919` |
| N-27 | The same script judges runtime scenarios by exit code and its only skip detector regex (`Skipping .*headless mode`) matches none of the scenarios' actual `[RUNTIME_SKIP] ... quit(0)` text, so a skipped run counts as passed | P3 (evidence) | #891 adjacent | `collect_production_evidence.ps1:184,380,460` |
| N-28 | `agentic-pr-gate` pins `.agentic/policy.json` to the base SHA but runs `scripts/agentic/classify_change.py` (and its tests) from the PR head, and `scripts/agentic/**` is classified R0; a PR editing renderer code plus the classifier can grade itself R0 | P3 (evidence) | none (#887/#812/#888 closed cover other shapes) | `.github/workflows/agentic_pr_gate.yml:113,136`; `scripts/agentic/classify_change.py:57` |

Finder strengths worth recording (CI): `run_gpu_harness.py` gate is a composite of rc,
parsed failures, empty/zero-assertion required batches and parse failures;
`run_runtime_validation.py` requires a positive `[RUNTIME_PASS]` bound to the registry name;
the contention postflight counts only samples from its own sampler; `validate_automation.py`
rejects hollow jobs; `finite_math_guard` proves the doctest case ran.

Streaming findings (finder-reported, below the P1 bar so not judged; not re-read here except
where stated):

| # | Finding | Sev | Tracked | Evidence |
| --- | --- | --- | --- | --- |
| N-20 | Predictive prefetch loads go through the same admission path as needed loads and can reach the visible-eviction fallback: an off-screen predicted chunk can evict an on-screen one while health stays "ok" | P2 | none (adjacent #1052, #1135) | `streaming_visibility_controller.cpp:1157,983`; `gaussian_streaming.cpp:3422` |
| N-21 | `cancel_chunk_jobs()` filters only the queues; a pack job already dequeued by a worker survives, the chunk's run is released, and if the chunk is re-queued into the same run the stale upload is accepted and stages a second retirement ticket that rolls back the valid upload | P2 (race, multi-asset) | none (adjacent #55) | `streaming_upload_pipeline.cpp:468,1634`; `streaming_atlas.cpp:222` |
| N-22 | A chunk whose payload read fails permanently (truncated `.gsplatworld`, short read) is rolled back with no counter, no warning and no per-chunk failure memo, then re-queued every frame; the nearest failing chunks monopolise the pack slots | P2 (silent failure) | #56 (new angle: it retries forever, the issue says never) | `streaming_upload_pipeline.cpp:1318,958`; `gaussian_streaming.cpp:3186` |
| N-23 | `StagedFileChunkPayloadSource` caches one `FileAccess` per `Thread::ID` and never evicts dead thread ids; every pack-thread restart (tuning change, re-init) leaks open handles | P3 | none | `streaming_chunk_payload_source.cpp:324-334`; `streaming_upload_pipeline.cpp:339` |

Finder strengths worth recording (streaming): page-run writes are bounded to the chunk's own
run before the GPU copy; asset generation stamps are checked at every consumer; stride-flip
hazards fail closed with counters; workers touch `GaussianData` only via snapshots under the
read lock; re-init quiesces the async path first; chunks with a retirement ticket are never
evicted mid-flight.

Finder strengths worth recording (import/persistence): payload extents are proven with u64
overflow checks before allocation in both loaders; SH band ordering (channel-major PLY to
coefficient-major) and quaternion component order are consistent end to end; the opacity
contract (PLY logit to sigmoid at load, SPZ byte already sigmoid, asset persists logits) is
coherent.

Finder strengths worth recording (renderer): async readbacks are `callable_mp` on RefCounted
owners; synchronous readbacks are centralised behind `safe_buffer_get_data` and gated to
bootstrap paths; stage failures cascade explicitly (sort SKIPPED with `skip_cause_stage`); the
shadow pass only blits depth it rasterised itself; renderer teardown stays off `world_mutex`;
`RenderThreadDispatcher::notify_completed` rejects never-issued request ids.

Finder strengths worth recording (shaders): COUNT/EMIT determinism is structural, the
rasterizer re-clamps GPU-produced counts against buffer length, the alpha-support threshold
is one shared constant across binning and raster, depth travels as raw fp32 into 64-bit sort
keys, and the composite handles sRGB EOTF and premultiplication in the right order.

Judge coverage: every finding the finders rated P0/P1 received one adversarial judge
(N-4, N-5, N-14, N-15, N-29, N-30, N-34, N-35). All eight verdicts are in and none was
refuted (confidence 0.80-0.90). Residual severity per judge: P1 for N-5, N-14, N-15; P2
for N-4, N-29, N-30, N-34, N-35. Where this document keeps P1 against a judge's P2 (N-4) it
says so in the row. Findings rated P2/P3 by
their finder (N-6..N-11, N-16..N-28, N-31..N-33, N-36..N-38) were not judged; those marked
"re-read here" were checked in the code in this session, the rest are finder-reported.

## Part 3: Competitive position (verified against primary sources, 2026-10-04)

### 3.1 Niantic SPZ (github.com/nianticlabs/spz) - all audit claims CONFIRMED

- Current version 4 (`load-spz.h`: `LATEST_SPZ_HEADER_VERSION = 4`, `MIN_ZSTD_SPZ_HEADER_VERSION = 4`):
  32-byte plaintext header (magic `NGSP` 0x5053474e, version, numPoints, shDegree,
  fractionalBits, flags, numStreams, tocByteOffset), optional ILV extension records
  (flags & 0x2, e.g. `SPZ_ADOBE_coordinate_system`), TOC, six independently zstd-compressed
  attribute streams; SH degree 0-4; v1-v3 gzip read-only. Announced 2026-05-05. Latest
  commit 2026-08-05 (int32 overflow fix for >35M gaussians).
- Colour: `splat-utils.h:15 colorScale = 0.15f`; encode `load-spz.cc:391`
  `toUint8(c * (0.15*255) + 0.5*255)`; decode `:464/:591` `((byte/255) - 0.5) / 0.15`;
  display `0.5 + 0.282095 * dc`. This settles C09: GodotGS stores the byte as the display
  colour and then adds 0.5 again. Alpha bytes are `sigmoid(opacity)*255` (GodotGS decode
  `byte/255` is consistent for linear opacity); scale `(log+10)*16` matches; v2 rotations
  3 bytes, v3+ smallest-three 4 bytes (GodotGS stride logic matches).
- SuperSplat exports SPZ v4 by default (`src/ui/export-popup.ts:538,605`), v3 as legacy.
  PlayCanvas engine reads only v4 and ignores SH band 4.
- GodotGS gaps confirmed by the agent reading the local loader: v2/v3 only, gzip only,
  `sh_degree > 3` rejected, no extensions, no coordinate-system metadata, no writer, hard cap
  32,777,216 points / 512 MiB. The importer already records `spz_antialiased` metadata but
  nothing consumes it.

### 3.2 PlayCanvas engine + SuperSplat - claims CONFIRMED or PARTIALLY_CONFIRMED

- SuperSplat 3.0 (blog 2026-09-09): projection, culling, compaction, radix sort and draw
  submission on the GPU (WebGPU compute); idle heap on a 4.4M-splat scene 1,557 MB -> 105 MB.
  Also: stochastic alpha mode shown on ~20M gaussians, sphere brush, GPU histograms, colour
  grading of selections, chunked streaming exports.
- Streamed SOG: binary spatial subdivision; LEAF nodes carry multiple LOD levels covering the
  same region, a viewer picks one level per leaf (audit said "per node": partially right).
  `GSplatParams` defaults: `splatBudget` 1,000,000, distance-based LOD mode default since
  engine 2.23, `antiAlias` flag ("compensation for splat data generated with anti-aliasing
  during training"), `stochastic`, `twoDimensional`.
- Grace Cathedral (blog 2026-07-20): ~3.5M splats drawn on desktop, 1.4M on mobile,
  streamed LOD within 400 m plus a static far set.
- Engine timeline: 2.19 (May 2026) WebGPU radix sort incl. OneSweep on NVIDIA; 2.20 (June
  2026) removed the pure compute renderer in favour of a hybrid raster renderer with compute
  projector, `GSPLAT_AA`, directional shadow CASTING from splats; 2.21 (July 2026)
  `KHR_gaussian_splatting` glTF import and SPZ via external parser; 2.23 (2026-10-01)
  cached splat shadows. Splats cast shadows but do not receive them; relighting is a separate
  proxy-mesh technique. Stereo XR, Apple Vision Pro, Quest supported.
- `KHR_gaussian_splatting` is "Complete, Ratified by the Khronos Group" (README);
  `splat-transform` reads PLY, compressed PLY, SOG, streamed SOG, SPZ v2-4, SPLAT, KSPLAT,
  LCC/LCC2 and writes PLY, SOG, streamed SOG, SPZ, GLB, HTML viewer, voxel collision.
- Where GodotGS is ahead of PlayCanvas (agent assessment, evidence-backed): native engine
  lighting with shadow RECEIVE, TAA/FSR2 jitter integration, painterly and effector systems,
  byte-accurate VRAM budgeting with a page atlas, a tile-based compute rasterizer with
  64-bit keys, and the self-hosted GPU visual-gate CI (no equivalent found in PlayCanvas
  repos).
- Where GodotGS is behind: hierarchical LOD streaming (shipped there, ADR here), XR, glTF,
  format breadth and SPZ correctness, antialiasing flag, editing tools, platform breadth,
  published benchmarks and memory figures.

### 3.3 UnityGaussianSplatting, Unreal/web renderers, Godot alternatives - audit claims CONFIRMED

- Unity (aras-p): PLY + SPZ, D3D12/Vulkan/Metal, VR (Quest 3/Pro, Vive, Varjo), rectangle
  selection, move, delete, merge, cutouts, modified-PLY export, world-space bake with SH
  rotation. README figure verbatim: bicycle 6.1M splats, 1200x797, RTX 3080 Ti, "6.8ms
  (147FPS) - 4.5ms rendering, 1.1ms sorting, 0.8ms splat view calc. 1.3GB VRAM"; M1 Max
  Metal 21.5 ms. Author: "not planning any significant further developments" (Dec 2023);
  last release v1.1.1 2025-04-09, last commit 2025-10-17.
- Spark 2.0 (World Labs, three.js, MIT, 3.7k stars, releases through 2026-10-01): continuous
  "LoD splat tree" against a per-frame budget of 500K-2.5M splats, GPU virtual memory with a
  16M-splat LRU pool, chunked `.RAD` streaming (64K-splat chunks over HTTP Range), 73M and
  106M-splat scenes on WebGL2 devices; reads PLY, SPZ, SPLAT, KSPLAT, SOG; SparkXr.
- Cesium: 3D Tiles hierarchical LOD for splats (blog 2026-04-27, 110M-splat example) with
  glTF `KHR_gaussian_splatting` + SPZ compression as payload. three.js r186 (2026-09-08)
  has a native `GaussianSplat` object with a glTF loader extension. Babylon.js: SPZ, SOG,
  experimental octree/SOG LOD streaming, CPU worker sort.
- Unreal: XVERSE XScene-UE (Niagara-based, automatic LOD since 2023-12, inactive since
  2025-07-30); Volinga (UE 5.8+, 4DGS sequences, DLSS, sublevel streaming, Windows + NVIDIA
  only, paid); Luma plugin unverifiable (primary sources unreachable).
- **Godot alternatives (new information the audit did not have):**
  - GDGS (`ReconWorldLab/godot-gaussian-splatting`): GDScript addon, Godot 4.3+, created
    2026-03-10, 289 stars, Asset Library #5076, releases to 2026-07-30. Two backends: a
    tile-based compute rasterizer with a (tile | depth) radix sort (the same design as
    GodotGS, in GDScript/GLSL through `RenderingDevice`) and a sorted-quad raster backend
    with MSAA, VR/multiview and Mobile/Compatibility support. Ships SOG, glTF
    `KHR_gaussian_splatting`, USDZ import, collision generation (voxelisation / marching
    cubes), relighting with shadow casting, a standalone Quest 3 viewer and a Swift/macOS
    port. No LOD, no streaming, no antialiasing, no SPZ.
  - `ambitiouscat/godot-gauss-splat`: C++ GDExtension for Godot 4.7, created 2026-08-10,
    0 stars, claims SSE-driven hierarchical streaming LOD with zstd tilesets and HTTPS range
    delivery (unverified by third parties).
  - `2Retr0/GodotGaussianSplatting` (inactive since 2024-08): GDScript compute, tile-based,
    publishes "RTX 3060 Ti, 108 FPS at 1920x1080, 3.07 GB VRAM" on bicycle.
  - haztro and others: GDScript + compute radix sort, PLY only, no LOD/XR/editing.

### 3.4 gsplat, Hierarchical 3DGS, Mip-Splatting, glTF status - audit claims CONFIRMED

- gsplat `rasterization()` (main): `sparse_grad`, `distributed`, `rasterize_mode` classic |
  antialiased, `camera_model` pinhole | ortho | fisheye | ftheta, OpenCV distortion, rolling
  shutter, `rasterization_2dgs`; 2026 additions: Gaussian ID rasterization, fp16 SH with
  arbitrary channels, experimental HiGS fused macro-tile inference path, AccuTile ellipse
  tile tests. CUDA only.
- H3DGS README: `--budget` in MB (default 16000) and `tau` granularity in the viewer; TODO
  list still says the dataset must fit in CPU memory, disk streaming "soon", viewer storage
  "unoptimized". Last commit 2025-06-10. Non-commercial licence.
- Mip-Splatting (CVPR 2024): 3D smoothing filter variance 0.2/nu_k^2 baked per splat at
  training; 2D Mip filter replaces the 0.3 px dilation with variance 0.1 and the opacity
  compensation `sqrt(|S| / |S + 0.1 I|)`. gsplat's antialiased mode keeps `eps2d = 0.3` with
  the compensation. Consequence for GodotGS: an `antialiased` flag needs a configurable 2D
  epsilon (0.1 vs 0.3) plus the compensation, which `tile_binning.glsl` already applies
  unconditionally with 0.35 (see Q-1).
- `KHR_gaussian_splatting`: RC merged 2026-01-27, press release 2026-02-03, status changed
  to "Complete, Ratified" 2026-09-03; implemented by CesiumJS, PlayCanvas 2.21, three.js
  r186, GDGS. The spec is deliberately minimal: single ellipse kernel, perspective only
  ("non-perspective projections is undefined"), display-referred unlit colour, SH 0-3.

### 3.5 What this means for "the best Gaussian splat renderer"

- GodotGS's defensible lead is unique and real: native engine lighting with shadow receive,
  TAA/FSR2 integration, painterly and effector systems, a byte-budgeted paged atlas, a
  vendor-agnostic Vulkan compute path, and a GPU visual-gate CI that no surveyed project has.
- The lead is not where users first look. Every surveyed ecosystem now ships hierarchical
  LOD (Spark, PlayCanvas, Cesium, XVERSE), XR, antialiasing flags, glTF/SOG/SPZ v4
  interchange and some editing. In Godot itself, GDGS is an installable addon with XR, SOG,
  glTF, collision and shadow casting, while GodotGS needs a custom engine build.
- The two Godot-native things nobody else offers are an engine-integrated renderer (the
  fork) and a production evidence culture. The plan below therefore keeps the architecture,
  closes the correctness and evidence gaps first (Phase 0), ships HLOD (Phase 1), proves
  speed on peer assets (Phase 2), and then buys the table-stakes features (Phases 3-6).
  A distribution decision (fork-only vs. a GDExtension or upstreamed hooks, see R-7) is
  the one strategic question this plan cannot settle from code.

## Part 4: Action plan

### Principles (non-negotiable, from AGENTS.md and the acceptance bar)

1. No re-architecture. The accepted ADRs (HLOD, colour encoding, overflow telemetry,
   single-route coexistence) are the design; work is slices against them.
2. Every slice: reproducer -> regression test -> smallest fix -> failure injection ->
   GPU evidence on the immutable base. Risk class per `.agentic/policy.json`.
3. Never weaken a gate to get green. Absence of a signal is not a pass.
4. The public alpha ships first (bar section 10); v1.0 items follow. Items below are tagged
   ALPHA (blocks or materially de-risks the alpha), V1 (needed for "ship a commercial game"),
   LEAD (needed to be the best renderer, beyond v1.0 parity).

### Phase 0: close the correctness and evidence holes (4-6 weeks, parallelisable)

Suggested order, by user impact per hour: P0-1 and P0-1b (colour, SPZ v2, importer cap),
P0-9 and P0-15 (make existing evidence count, so everything after lands under a gate),
P0-11 (payload centre), P0-10 (band-3 basis), P0-13 (thread_model 2 race), P0-2 to P0-5
(the tracked P1/P2 issues), P0-16 and P0-17, then P0-6, P0-12, P0-14, P0-7, P0-8. Each item
is one PR on its own branch with the base SHA recorded.

| ID | Item | Tracked | Risk | Acceptance criterion |
| --- | --- | --- | --- | --- |
| P0-1 | DC colour contract: tag `LINEAR_RGB` in `PLYLoader`, convert SPZ bytes to `SH_C0`-scaled centred DC `(byte/255 - 0.5)/0.15 * SH_C0`, fix the v2 rotation decode to `(byte - 127.5) / 127.5` (N-14), add reference `.spz` (v2 and v3) and `.ply` golden fixtures generated by the Niantic reference encoder, GPU readback test | #1056, colour ADR slices, N-14 | R3 (format) | Same scene via import, raw load, PLY, SPZ v2 and SPZ v3 renders identical pixels (eps 1/255) on GPU readback; a mid-grey Niantic SPZ splat reads 0.5; a v2 fixture with known orientations matches its PLY twin. ALPHA |
| P0-1b | Importer cap semantics: make the stride subsample apply whenever `final_count < original_count` (drop the `density < 0.999` clause) in both importers, or route `max_splats` through the #456 importance pruning; log which rule applied | NEW issue (N-15) | R1 | A 2.5M-splat scan imported with the "High Quality" preset keeps splats from the whole file extent (coverage mask equals the Ultra import's at reduced density), not the first 1M. ALPHA |
| P0-2 | Eviction recency: stamp all visible chunks of one frame with the frame generation; tie-break farthest-first; same for non-primary LRU | #1052 | R2 | CPU doctest from the issue: 8 visible chunks, forced admission evicts the farthest. ALPHA |
| P0-3 | Save snapshot: `GaussianData::capture_persistence_snapshot()` under `RWLockRead`; serializer writes only the snapshot; same for world save and GPU hierarchy build | #774, #606 | R3 (persistence) | Two-worker stress (edit + `save_scene`) with the existing #773-style harness shows no torn record; TSAN-clean on Linux CPU lane. ALPHA |
| P0-4 | Raster dispatch result: typed `RasterDispatchResult {Error, submitted}`; `_dispatch_rasterization` marks the frame stage failed and `_finalize_frame` refuses to publish | #981 | R2 | Failure injection after uniform-set creation yields an invalid frame plus a stage-failure metric, never a cleared texture published as rendered. ALPHA |
| P0-5 | `GaussianSplatWorld::clear()` emits `changed`; `get_metadata()` returns a copy or documents the aliasing | #1002, #1007 | R1 | After `clear()` the director's visible splat count is 0 without manual reapply (SceneTree test). ALPHA |
| P0-6 | Projection flip contract: build the render and cull projections as `correction * projection` with `set_depth_correction(flip_y, false, false)` (full clip-Y row), shadow pass included; CPU tests for symmetric, `set_frustum` off-axis, shifted orthographic | NEW issue | R2 | CPU reference == renderer matrix for all three; GPU visual oracle with `Camera3D.frustum_offset != 0`. V1 (XR prerequisite) |
| P0-7 | Close the #1087 bypasses (instance residency requests, sync prefetch drain) | #1141 | R2 | No chunk beyond the effective render distance is ever requested; corridor lane asserts it. ALPHA |
| P0-8 | Evidence routing: add `interfaces/**` and the decomposed streaming files to the R2 glob and GPU-evidence prefix list | #1017, #897 | R3 (policy) | `classify_change.py` grades a sorter-only diff R2. ALPHA |
| P0-9 | Make the evidence that exists count: promote the `TileRenderer` GPU batch (the four #586 sorter tests) into `REQUIRED_BATCHES` after resolving its stale sRGB exclusion (#643); make the `[Streaming Pipeline]` module lane strict (it holds every #1087 and #1088 host test and currently prints "advisory lane, continuing"); add a shader-lint or GPU case for the #1050 barrier ordering | #906, #705, #643, #519 | R2 (CI) | A regression in sorter rejection, atlas allocation, distance bounding or barrier ordering turns the Production Gates or the module-tests job red; `run_module_tests.py` has no `strict = False` lane left whose tests guard a shipped contract. ALPHA |
| P0-10 | Band-3 SH basis: align `basis[11]`, `basis[13]`, `basis[14]` in `gs_sh_binning.glsl` (and the dead mirror, or delete it per #1064) with the Inria constants; add a host test that evaluates all 16 basis functions against a reference table on 22 directions, and extend the GPU readback test to bands 2 and 3 | NEW issue (N-1) | R2 | Degree-3 real scan renders within 1/255 of a CPU reference evaluation at six view directions; base fails on the index-11 term. ALPHA |
| P0-11 | Payload centre precision: store the screen centre as `packUnorm2x16(screen_pos / viewport_size)` (uniform 1/65535 of the viewport, 0.06 px at 4K, no layout change) or as two fp32 words; drop the `quant_pad` compensation; while in the file, round-to-nearest in `gs_pack_color_r11g11b10` and add the NaN/Inf guard before packing | NEW issues (N-4, N-6, N-7) | R2 (layout-sync guard) | Static-camera visual oracle at 1920x1080: left and right halves have identical sharpness statistics; TAA jitter moves splat centres by the jittered amount at x = 1800; white reference wall reads 255,255,255. ALPHA |
| P0-12 | Orthographic projection: branch `project_gaussian_2d` on the ortho flag already derived in `tile_resolve.glsl` (`J = diag(focal_x, focal_y)`, no 1/z terms, and an ortho-aware focal range check instead of `focal_length_reject`) | NEW issue (N-5, judge-confirmed P1) | R2 | Editor top/front/side ortho views and a `PROJECTION_ORTHOGONAL` Camera3D render splats at the same footprint as the perspective reference at matched pixel scale; an ortho size larger than the viewport height still renders. ALPHA (the editor's own orthogonal views hit it on every scan) |
| P0-13 | Thread safety of the engine hook: route every `GaussianSplatStorage` mutation through the RenderingServer command queue (or make `gaussian_owner` `RID_Owner<GaussianSplat, true>` and guard the `Ref` swap), and add the windowed `thread_model = 2` lane that renders a real `GaussianSplatNode3D` and frees it mid-run (#1148) | NEW issue (N-9), #1148 | R3 (engine delta under `servers/`) | TSAN-clean node add/remove loop under `thread_model = 2`; the lane fails on the base if the race is provoked (free during render). ALPHA (the starter template ships `thread_model = 2`) |
| P0-14 | Render-cache key completeness: hash every input `render_tile_fallback` and the painterly path consume (painterly config, debug/preview toggles, Jacobian diagnostics, low-pass filter, pipeline features) or call `invalidate_cached_render()` from each setter; fix the `GaussianSplatRenderer.xml` default to match the code | NEW issue (N-10) | R2 | Static camera, inspector edit of any of those parameters changes the next frame; the default documented equals the default shipped. ALPHA |
| P0-16 | Finite-check at the GPU boundary, not only at file load: run `all_render_fields_finite` (or sanitise as the quantized packer does) in `pack_gaussians_range`, `set_gaussian_data`, the `.gsplatworld` loader and `GaussianSplatNode3D.set_splat_data`, counting rejections | NEW issue (N-25), #606 | R2 | A GDScript `set_splat_data` with one NaN position renders every other splat and reports one rejected record; no NaN reaches `cov3d` (GPU counter stays zero). ALPHA |
| P0-17 | Hot-reload contract: bump `payload_version` from every lane setter (or from `copy_from`) so a `CACHE_MODE_REPLACE` reload invalidates the director's cached payload; SceneTree test that re-saves an asset and asserts the rendered content changes | NEW issue (N-24) | R1 | Re-saving a `.gaussiansplat` while a node renders it changes the frame within two frames without a manual resubmit. ALPHA |
| P0-15 | Enforce what is collected: default `enforce_gpu_readiness` to true (or drop the input and always enforce) so the readiness booleans fail the Production Gates on PR/push/merge_group; make `collect_production_evidence.ps1` exit non-zero on any failed sub-command, feed `module_tests` and `943_ready` into `902_ready`, and match the real `[RUNTIME_SKIP]`/`[RUNTIME_PASS]` markers; make the waiver guard check issue state and `expires_utc` and retire the #643 waiver; promote the open-world proof from `continue-on-error` to a blocking scheduled job with the #875 contention postflight as its only void path; run `classify_change.py` from the base SHA like the policy | NEW issues (N-12, N-13, N-26..N-28), #1045, #1051, #1016, #897 | R3 (CI workflow surface) | A `summary.json` with any readiness boolean false, a failed module-test lane inside the evidence step, or a `[RUNTIME_SKIP]` scenario turns a push run red; a waiver whose issue is closed fails `test_gpu_harness_deferred_contract.py`; a corridor-proof exit code 1 produces a red workflow run; a PR that edits the classifier is graded by the base classifier. ALPHA |

### Phase 1: streaming becomes real (HLOD Stage 1, 8-10 weeks)

Follow the ADR's slice plan verbatim (`adr-hlod-streaming.md` section 9): S1a format v2 +
bake, S1b per-node choice, S2a CPU cut (closes #1131, #1053 for HLOD content, replaces
`update_chunk_lod_parameters`), S2b top-level instance tree + camera-relative
`depth_compute` rewrite, S3 node streaming on the #1134 pages, S4 telemetry, S5 resident
route, S6 cross-fade. Seven of eight are R3.

Parallel evidence track (ALPHA, all tracked): promote the corridor/open-world lanes to
`real_chunked` or bound the alpha's streaming promise in words (#1016 open decision);
strict streaming coverage (#1051, #519, #778, #763); streaming stress and visual smoke
(#883, #786); the 50M chunked asset; O(total_chunks) work (#320, folded into the cut).

Pre-S3 streaming hygiene (small, independent of HLOD, from Part 2 N-20..N-23): thread an
admission intent through `queue_chunk_load()` so prefetch never evicts visible chunks; a
per-chunk upload sequence checked at pack completion and retirement so a cancelled in-flight
job cannot stage a second retirement ticket; a per-chunk failure memo with bounded retry and
a counter for payload reads that fail; a bounded `FileAccess` pool instead of the
per-thread-id cache. Acceptance: a doctest per item in a STRICT lane (see P0-9).

Acceptance (ADR section 8, thresholds fixed in the ADR, never lowered silently): test 1
static-frame flicker oracle 0/300 differing frames at a binding cap with the base failing
299/300; test 3 bake invariants (no node over 16,384 splats, moment matching, v1 still
loads); tests 5-7 mixed-size thrash, coverage under streaming, corridor at 1 GiB with
`vram_cap_hit_frames <= 0` and `residency_ratio >= 0.70`; test 8 real-scan PSNR within
1.0 dB / SSIM within 0.01 of the ADR table; test 11 `tau` 1 px vs 2 px A/B table before S3
is accepted. Single-vendor blind spot (NVIDIA only) stays disclosed until R-1/R-2 land.

### Phase 2: performance parity program (starts in parallel, 6-8 weeks)

The dense resident path is unmeasured since July and the only published number
(12.3 FPS at 4.9M synthetic splats spread over 196 node instances, no GPU timing) says
nothing about single-scan rasterisation speed. The bar (section 3) requires a
peer-relative, out-of-band comparison for v1.0 and names the trap: extras must be made
inactive by scene composition, never by zeroing `direct_light_scale`.

| ID | Item | Tracked | Acceptance |
| --- | --- | --- | --- |
| PF-1 | Peer benchmark corpus: Mip-NeRF 360 Bicycle (6.1M) and Garden at 1200x797 and 1080p, SH3, single mono camera, lights/shadows/effectors inactive by scene composition (bar section 3.2), full resident; publish parity and feature-cost figures | bar section 3, #523 | GodotGS, UnityGaussianSplatting (published 6.8 ms on a 3080 Ti) and SuperSplat on the same files; per-stage ms (cull, sort, bin, raster, composite), VRAM, SSIM vs reference render. V1 |
| PF-2 | Fix the benchmark suite so it measures what it names: GPU timestamps required on the dense lane, lane weights non-zero, node-count lane, baselines persisted and gated on delta | #523, #790, #842, #530, #899 | Dense lane reports `gpu_timing_available = true`; a 10 percent regression fails the lane. ALPHA for integrity, V1 for parity |
| PF-3 | Sort pipeline, in this order of expected gain: (a) drop the redundant instance depth pre-sort on the global-sort tile route (N-29: identity `sorted_indices`, tie-break from `gaussian_idx ^ hash(instance_id)`); (b) size the radix pass count from the live key bits and cap `tie_bits` at 3-4 (N-31: 16 -> 12 passes); (c) parallelise the serial `wg_prefix`/`bin_prefix` passes (#539); (d) cache the sorter's pass descriptor sets for the lifetime of their buffers (N-33); (e) only then re-evaluate OneSweep after fixing its unstable scatter (N-8, #635) | N-29, N-31, N-33, N-8, #539, #635, #957, #20 | Sort ms at 6.1M visible splats within 2x of the Unity figure (1.1 ms on a 3080 Ti) on equivalent hardware; each step lands with a before/after GPU-timestamp table on the dense lane and a pixel-identical visual oracle. V1 |
| PF-3b | Binning: project once. COUNT writes the finished `ProjectedGaussian` payload plus clamped tile rect and `ellipse_sigma2`; EMIT consumes them and adds only SH/lighting colour; replace the per-record atomic cursor with a per-splat prefix (N-30) | N-30, #20 | Binning GPU time on the dense lane drops by at least 30 percent with a bit-identical image; `tile_binning.glsl` no longer runs `project_gaussian_2d` twice per splat. V1 |
| PF-3c | Compute raster occupancy and portability: decouple the shared-memory batch window from `MAX_SPLATS_PER_TILE` (`GS_RASTER_BATCH` 256, or derived from `LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE` at compile time) so 4-8 workgroups fit per CU and the compute path builds on 32 KB devices; turn the compile WARN into a counted fallback reason | N-32 | Raster GPU time on the dense close-up lane improves measurably (target 1.5x); the compute path engages on a 32 KB-shared-memory device or reports why not. V1 |
| PF-4 | Resubmit cost: dirty-field tracking so a one-field world change does not re-upload 1M splats | #1008 | One-field change under 5 ms at 1M. V1 |
| PF-5 | Capacity: retire the ~1.8M-visible-splat overlap-record ceiling by consuming `overflow_flag` and sizing from the HLOD cut's footprint estimate (ADR section 6.5) | #54, #1044, #1147 | A default-configured 1080p scene above 1.8M visible splats renders without a band. V1 |

### Phase 3: interoperability and persistence (4-6 weeks, R3)

| ID | Item | Tracked | Acceptance |
| --- | --- | --- | --- |
| F-1 | SPZ v4: vendor `nianticlabs/spz` (MIT, C++, zlib+zstd, 62 commits, last 2026-08-05) under `thirdparty/` behind one explicit `GaussianCloud -> GaussianData` conversion that owns the colour contract (`dc = (byte/255-0.5)/0.15`, display `0.5 + SH_C0*dc`); read v1-v4 (NGSP header, TOC, six zstd streams), SH 0-3 with degree 4 dropped under a warning, `antialiased` flag into render meta, ILV extensions incl. coordinate system, and an SPZ writer for export | NEW issue | A SuperSplat default (v4) export loads and matches its PLY twin to eps 1/255 on GPU readback; the hand-written v2/v3 parser is deleted; the 32M-point cap goes. V1 |
| F-2 | GSF v2: versioned chunk types for the high-order SH sidecar, `is_2d_mode`, DC encoding tag, antialiased flag, future sidecars; lossless roundtrip test over every render field | reopen #600 | Byte/epsilon-exact roundtrip of all fields; v1 files still load with the existing warning. V1 |
| F-3 | glTF `KHR_gaussian_splatting` import (ratified; PlayCanvas ships it since engine 2.21, July 2026) plus read-only compressed PLY and SOG so SuperSplat-edited content enters Godot without re-export | NEW issue | Fixtures exported by `splat-transform` (GLB, SOG, compressed PLY) load and pass the visual parity test against their PLY twins. LEAD |
| F-4 | Importer dedup and a single import core for all formats | #573 | One code path sets `dc_encoding`, SH encoding, 2D flag. V1 |
| F-5 | PLY export parity: write `f_rest_*` for every stored band in channel-major 3DGS order, convert DC per the encoding tag, snapshot under the read lock; rotate SH bands 1-3 when the merge bakes a source rotation | NEW issues (N-16, N-17) | A degree-3 asset survives import -> edit -> `save_to_file` -> re-import with every SH coefficient within fp32 tolerance; a merged world with a 90-degree source renders the same highlights as the two unmerged nodes. V1 |
| F-6 | Layout fingerprint for every raw-struct payload (`.gsplatworld`, `.gsplatcache`, `.gsf`): a constexpr layout version derived from `offsetof` static_asserts, written to the header and checked on load | NEW issue (N-18) | A same-size field swap makes every existing file fail to load with a clear error instead of misdecoding. V1 |

### Phase 4: rendering quality beyond parity (LEAD, 8-12 weeks)

| ID | Item | Tracked | Acceptance |
| --- | --- | --- | --- |
| Q-1 | Antialiasing contract: the 2D low-pass with Mip-Splatting alpha rescale already exists (`shaders/tile_binning.glsl:680-695`, `:1208-1211`, always on). Make the alpha rescale conditional on a per-asset `antialiased` flag (models trained classic vs. with the 3D filter differ in energy), import the flag from SPZ v4, expose it on the importer, and A/B both model classes against reference renders | NEW issue | Classic-trained and antialiased-trained reference scenes each match their reference render within 0.5 dB PSNR; no shimmer on the thin-structure QA scene at 4x zoom-out. |
| Q-2 | TAA motion vectors / velocity write-back for splats (accepted residual today) | #1025 (needs-adr) | Trail under 0.25 px at 1.5 m/s camera speed on the QA path. |
| Q-3 | Multiview/XR. Step 1 (ALPHA, small): refuse cleanly when `view_count != 1`: skip the GS pass with one `WARN_PRINT_ONCE` keyed on view count and a frame-stage reason, instead of paying full cull/sort/raster with the combined frustum and failing the composite every frame. Step 2 (LEAD, ADR): per-view projection from `view_projection[v]` / eye offsets, one raster per eye or a layered raster, composite into the 2D-array target via the pre-upscale hook | NEW issue (C34), needs ADR for step 2 | Step 1: an XR project logs one line and renders meshes normally. Step 2: stereo QA scene renders both eyes with correct parallax; GDGS's Quest 3 viewer is the bar to meet. |
| Q-4 | Shadow casting: first run the existing atlas path on hardware and test the N-11 lead (the shadow entry's default `StageMetrics` zeroing the sorted count), then the #1095 design (directional first); sky-irradiance ambient (#1061); reconcile the limitations page entry with what the code does | #1095, #1089, #1061, N-11, N-38 | Splats cast shadows onto meshes and themselves without self-darkening; evidence on a real scan. |
| Q-5 | Resolve-time vs per-splat lighting decision; delete the dead mode or wire a setting | #531, #1014, #1083 | One lighting path with a measured cost table. |
| Q-6 | Full degree-3 SH on the GPU: raise `MAX_ENCODED_COEFFICIENTS` to 15 (PackedGaussian 128 -> 144 B, or pack two more SNORM10 words into existing padding), update `sh_encoded[12]` in binning and raster and the layout-sync guard; document the quantized atlas's SH reduction in the setting's description | NEW issue (N-35, N-3) | A degree-3 scan renders bit-identical to a CPU reference evaluation of all 16 basis functions at six view directions; `rendering/sh_bands = 3` means three bands. |
| Q-7 | 2DGS honesty, then support: step 1 warn once that `is_2d_mode` is metadata-only and clamp (not reject) a zero third scale so 2DGS-trained PLYs render; step 2 (ADR) a surfel path with ray-splat intersection and normal-aligned footprint | NEW issue (N-34) | Step 1: a 2DGS PLY renders without degenerate covariances and the log says what 2D mode does. Step 2: parity with the 2DGS reference renderer on its test scenes. |
| Q-8 | Per-splat animation: either route `update_animation()` samples into the runtime overlay on the GPU or remove the bound API and docs that promise it | NEW issue (N-36) | Keyframed splats move on screen without a script copying positions per frame, or the API is gone. |

### Phase 5: platforms and release machinery (V1)

| ID | Item | Tracked |
| --- | --- | --- |
| R-1 | Linux GPU lane with `qa` and `sorting`; Linux export template attached; Linux export smoke like Windows | #596, #1015 |
| R-2 | macOS/Metal: build + one smoke on real hardware, or state unsupported | bar section 2 |
| R-3 | Required-check decision: make a GPU-evidence aggregate required for R2/R3 same-repo PRs (or record explicitly why not); keep `agentic-pr-gate` for forks | #889, #962, #823 |
| R-4 | Code signing and SHA sidecar fix | #1015, #632 |
| R-5 | Upstream re-baseline onto Godot 4.6.3/4.7.2 | #943-947 |
| R-6 | Real screenshots and clips from the exact candidate; compatibility evidence | #184, #182 (P0) |
| R-7 | Distribution decision (maintainer): keep fork-only distribution (nightly editor + templates), or carve the engine hooks (`servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.*`, `renderer_scene_render_rd.cpp` hook, `render_forward_clustered.cpp` pre-upscale hook) into an upstreamable patch so the module can ship as a GDExtension or be re-baselined cheaply (#943-947). GDGS's adoption curve (289 stars in six months as an addon) is the evidence this question matters. | #943-947, new ADR |
| R-8 | Collision generation from splat data (voxelisation / marching cubes, as GDGS ships) and shadow CASTING per Q-4, because both are now expected of a Godot splat node | NEW issue, #1095 |

### Phase 6: editing v1 (LEAD)

Selection (GPU id buffer), delete/hide, transform, crop volume, merge, re-export to PLY/SPZ
with SH rotation. Tracked nowhere today; needs an ADR. Acceptance: round-trip a SuperSplat
edit session's equivalent inside the Godot editor on a 1M-splat scan.

### Status (2026-10-04)

Every untracked finding and feature item above now has a GitHub issue, filed with the
evidence lines from this document and labelled by area and priority:

| Plan item / record | Issue |
| --- | --- |
| N-4 fp16 screen centre (P0-11) | #1153 |
| N-14 SPZ v2 rotation decode (P0-1) | #1154 |
| N-15 importer prefix cut (P0-1b) | #1155 |
| N-5 orthographic Jacobian (P0-12) | #1156 |
| N-1 band-3 SH basis (P0-10) | #1157 |
| N-35 / N-3 SH coefficient truncation (Q-6) | #1158 |
| C06 flip_y row (P0-6) | #1159 |
| C34 multiview / XR (Q-3) | #1160 |
| N-9 GaussianSplatStorage thread race (P0-13) | #1161 |
| N-10 render-cache key and doc default (P0-14) | #1162 |
| N-11 / N-38 shadow-entry metrics, casting path (Q-4) | #1163 |
| N-12 / N-26 / N-27 readiness enforcement (P0-15) | #1164 |
| N-13 / C27 waiver guard, stale #643 exclusion (P0-9, P0-15) | #1165 |
| N-2 advisory lanes and batches (P0-9) | #1166 |
| N-28 classifier from PR head (P0-15) | #1167 |
| N-6 / N-7 colour pack rounding and NaN guard (P0-11) | #1168 |
| N-8 OneSweep unstable scatter (PF-3e) | #1169 |
| N-16 PLY export drops SH (F-5) | #1170 |
| N-17 merge does not rotate SH (F-5) | #1171 |
| N-18 layout fingerprint (F-6) | #1172 |
| N-19 / Q-1 antialiased flag | #1173 |
| N-24 hot reload payload_version (P0-17) | #1174 |
| N-25 finite check bypass (P0-16) | #1175 |
| N-20 prefetch evicts visible chunks | #1176 |
| N-21 cancel vs in-flight pack job | #1177 |
| N-22 permanent payload failure requeue | #1178 |
| N-23 FileAccess per thread id | #1179 |
| N-29 redundant depth pre-sort (PF-3a) | #1180 |
| N-30 COUNT/EMIT double projection (PF-3b) | #1181 |
| N-31 radix pass count (PF-3b) | #1182 |
| N-32 raster shared memory (PF-3c) | #1183 |
| N-33 per-frame descriptor sets (PF-3d) | #1184 |
| N-34 2DGS flag no-op (Q-7) | #1185 |
| N-36 animation state machine no consumer (Q-8) | #1186 |
| N-37 doc-class defaults | #1187 |
| F-1 SPZ v4 | #1188 |
| F-2 GSF v2 | #1189 |
| F-3 glTF / SOG / compressed PLY import | #1190 |
| PF-1 / PF-2 peer benchmark corpus | #1191 |
| R-7 distribution decision | #1192 |
| R-8 collision generation | #1193 |
| Phase 6 editing v1 | #1194 |

Items already tracked keep their existing issues (listed in the tables above). Every
engineering item is its own task, branch and PR per `AGENTS.md`; this document is R0.

## Verification of this plan's claims

- Every file:line above was read at `ae555497`; re-run
  `git show ae555497:<path> | sed -n '<range>p'` to confirm.
- Issue numbers were read via the GitHub API on 2026-10-04; `#` references resolve at
  `https://github.com/klausi3D/godotGS/issues/<n>`.
- Agent verdicts (grouped claim verifiers, subsystem finders with adversarial judges, web
  research) were produced in the review session and folded into Parts 1-3; where a verdict
  rests on an agent's reading alone the row says "finder-reported".
- Competitive facts cite primary sources (GitHub READMEs and source files, official blogs
  and docs) as of 2026-10-04 and were checked for internal consistency against each other.
