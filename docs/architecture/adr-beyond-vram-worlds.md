# ADR: Worlds beyond VRAM — compressed streaming, chunk streaming and HLOD as one design

- **Status:** proposed. Implementation follows the roadmap order in §10, and each code PR
  carries its own risk class. This record is a design only: it changes no code, and no build
  or GPU run backs it.
- **Date:** 2026-11-04.
- **Risk:** this document is R0 (`docs/**`). It is the design record that the R3 packages it
  schedules depend on (`docs/governance/agentic-engineering.md`). Those R3 packages are
  BV-01, BV-02, BV-03a/b, BV-05, BV-06a/b, BV-07, BV-08b, BV-09, BV-13a/b, BV-14, BV-C1a/b,
  BV-C3, BV-C4, BV-C5a, BV-C6, BV-C7, BV-16a/b/c, BV-17, BV-18a/b, BV-19, BV-20a–d,
  BV-21a/b, BV-26a, BV-27, BV-30, BV-31, BV-32, BV-33 (upstream engine), BV-34b and
  BV-35a–d (§10).
- **Base:** `44836e1d6a5` (origin/master, the merge of #1293). Every `file:line` below was
  read at this commit.
- **Refs:** #1131, #1053, #320, #1051, #54.
- **Related ADR:** [Hierarchical LOD for streamed splat worlds](adr-hlod-streaming.md)
  (cited below as "HLOD ADR"). Its §16 points here for Package G at scale.
- **New requirement (maintainer, 2026-11-03):** a game must not ship hundreds of GB. Large
  worlds therefore use a **compressed, streamable on-disk format**, and so does the test
  world. Uncompressed data is kept only where it is needed: quality references, byte-identity
  proofs and one bounded early probe (D-13).
- **Paths** are relative to `modules/gaussian_splatting/` unless they start with `docs/`,
  `tests/`, `.github/`, `drivers/` or `servers/`, or are marked "repo root". Short forms:
  `world.cpp` = `core/gaussian_splat_world.cpp`; `publisher.cpp` =
  `renderer/resident_instance_contract_publisher.cpp`; `paged.cpp` =
  `io/gaussian_splat_world_paged.cpp`.
- **Citations:** ✓ means the cited lines were re-read at `44836e1d6a5` for this record
  (Appendix A). A citation without ✓ was carried from the design research and not re-read.
- **Labels:** **[M]** measured (source named), **[D]** derived from cited constants,
  **[E]** estimate, **UNVERIFIED** not confirmed.
- **Naming:** milestones are **M1, M1c, M2, M3, M4, M5** everywhere. D-1's architectural
  steps are **Steps A/B/C**. Roadmap phases are **P0–P6** (§10).

---

## 1. Context and goals

### 1.1 Problem

GodotGS has never rendered a world larger than GPU memory. The largest stress tier is 2.5M
splats (`tests/runtime/test_gpu_streaming_stress.gd:69`, `tier_2_5m` at `:96-97` ✓). The
20M/50M/100M corridor stages are `planned_unstaged` (`tests/runtime/open_world_chunked_asset_ladder.py:118-119`,
`:194` ✓) and resident: the 20M corridor is an in-memory world merged from 800 instances of
one asset (`tests/examples/godot/test_project/scripts/benchmark_open_world_stage_contract.gd:44-79` ✓).
The corridor lane has never produced a result: every reachable scheduled run of
`gaussian_production_gates.yml` timed out before the lane's first frame (for example run
40298399722; Refs #1045) [M].

Two partial systems exist:

1. **Chunk streaming** (`GaussianStreamingSystem` + `StagedFileChunkPayloadSource`). It
   streams **uncompressed** plain v3 out of core. Its distance LOD saves no VRAM (#1053), it
   has per-frame O(N) residue (#320), its gate counters are stubs in optimized builds (D-5),
   and one buffer holds the atlas.
2. **HLOD** (V4 paged packages, `NodeResidency` + `HlodNodeStreamer`). It streams nodes, but
   under pilot caps of 1,048,576 **leaf splats**, 1,572,864 payload splats, 32,768 nodes,
   512 MiB GPU and 1 GiB CPU (`core/gaussian_splat_hlod_runtime_data.h:14-18` ✓;
   `world.cpp:1191-1194` ✓), **plus a 2 GiB staging ceiling on the streamed atlas**
   (`renderer/resident_atlas_budget.h:36` ✓, applied through `atlas_limit` at
   `publisher.cpp:1716` ✓). Its V4 writer needs a resident world. It is a second residency
   instance, which HLOD ADR §14 rules out (`docs/architecture/adr-hlod-streaming.md:1076-1079` ✓).

**No streamable compressed format exists.**

- The only world compression (flag bit 4, `kFlagCompressed`, `io/gaussian_splat_world_io.cpp:58` ✓)
  gzips the whole gaussian section as one blob. It is resident-only (`:340-352` ✓) and
  bounded by `INT32_MAX` decompressed bytes (`:65-81`, `:1573` ✓), about 14.9M splats [D].
- Importer "compression" options change no stored bytes. They only set
  `GaussianSplatAsset::compression_flags` (`io/resource_importer_spz.cpp:37-41` ✓,
  `io/resource_importer_ply.cpp:136-146` ✓); outside the importers only the asset itself and
  editor code reference the field (grep ✓).
- Disk today costs 292 B/splat for SH3 plain v3, so a 201M-splat world is 54.8 GiB [D].

### 1.2 Goals

| # | Goal | Source |
|---|---|---|
| G-a | Render a physically stored, deterministic SH3 world several times larger than VRAM, on the 3080 runner and the 3090 workstation. | maintainer requirement |
| G-b | Chunk streaming and HLOD form **one design**: one residency contract, one byte ledger, one page pool. | maintainer requirement; HLOD ADR §14 |
| G-c | Run the fly-through at the **tuned settings**: 4M visible cap, 400 m far plane, 4096 MB budget, 320 chunks. Production defaults are reported separately. | maintainer requirement |
| G-d | Frame targets: 3080 p95 ≤ 25 ms and p99 ≤ 33.3 ms; 3090 p95 ≤ 16.7 ms and p99 ≤ 20 ms. **Never loosened.** §5.1 states plainly what reaching them takes, and §10 schedules calibration and gating **for both devices**. | maintainer requirement |
| G-e | The out-of-core writer is a reusable `io/` API, and it **carries the page codec**. | maintainer requirement |
| G-f | Use an optimized build and a persistent world cache on the VM. The approved cache of about 60 GiB **is sufficient**; no VM disk growth is needed (§5.3). | maintainer requirement |
| **G-g** | **A shippable, compressed, streamable world format** shared by plain chunks and V4 HLOD nodes: about 30–38 B/splat at SH3 [E], against 288–292 B today. | new requirement |
| G-h | A path to HLOD ADR Package G: ≥ 100 km, ≥ 200M physical SH3 splats, about 1M drawn. | `docs/architecture/adr-hlod-streaming.md:1081-1088` ✓ |

### 1.3 Non-goals

- Real-scan beyond-VRAM content. It waits on #1294 and on tiled ingest.
- Package F (editable regions).
- Multi-package worlds (several world packages per scenario). Node instances are **not** a
  non-goal: they coexist with the world in the same frame per
  [One route, many demand producers](adr-one-route-many-producers.md).
- Retrained or neural compression (not decodable per page at streaming rates; see the
  compression survey in "External prior art").
- Any change to S1b rules or to the #1290 waiver. **The HLOD bake rule stays unchanged.**
- A GPU-resident compact layout (a stride-class atlas with shader decode). It is Package G
  stage 2 (P6).

---

## 2. Decisions

Each decision lists the alternatives considered and why they were rejected.

### D-1 One residency system, grown from `NodeResidency`, reached in Steps A → B → C

**Target.** Each device has one `ResidencyManager`. It owns:

- one segmented page pool: K ≥ 1 storage buffers, each ≤ min(device
  `maxStorageBufferRange`, `UINT32_MAX`), with 1,024-splat pages
  (`renderer/hlod_node_streamer.h:39` ✓);
- one byte ledger;
- one upload and retirement path;
- one `PayloadReadService` with codec decode.

The unit is a **run** keyed by `(asset_key, NodeKey, NodeVersion)`:

- an HLOD node is a run of ≤ 16 pages;
- a legacy chunk is a degenerate leaf node, a run of ≤ 64 pages.

Selectors are separate **demand producers**: the HLOD `ViewSet` cut, legacy chunk
visibility and the movable node instances (rows 1..N of one draw list, per
[One route, many demand producers](adr-one-route-many-producers.md)); light views and extra
camera views follow. The producer interface takes several producers per frame from the start, so
Steps A and B do not harden around one producer.

**The seed is `lod/hlod_node_residency`.** It already has whole-group admission, tickets with
generations, retirement delay, protection, and eviction outside the closure → LRU → finest
first (`lod/hlod_node_residency.h:45-320` ✓). It does not yet scale: `request()` scans every
node to cancel stale loads (`lod/hlod_node_residency.cpp:285-290` ✓), `stats()` walks every
node (`:543-565` ✓), and `evict_for` collects and sorts every reclaimable node
(`:179-196` ✓).

**Threading.** Every mutation of residency state belongs to the **render thread**, which is
how the existing code is documented (`lod/hlod_node_residency.h:35` ✓;
`renderer/hlod_node_streamer.h:20-21` ✓). The main thread submits only view specs. Reader,
decode and pack workers hand results over through bounded single-producer/single-consumer
queues. This is invariant I-11.

**Steps:**

- **Step A (M1–M2).** The first beyond-VRAM runs use today's chunk streamer, changed only for:
    - evidence and telemetry;
    - shared layers that survive: the ledger, `PayloadReadService`, the page codec, release
      counters, GPU timing;
    - blockers the probe proves.
- **Step B (M3–M4).** HLOD at scale runs on the scaled `NodeResidency`. Its pool is sized by
  the streaming budget inside the device ledger (D-3).
- **Step C (M5).** `NodeResidency` becomes the `ResidencyManager` with K segments. Primary
  chunk worlds become degenerate-node demand on it, and the chunk streamer's primary atlas,
  allocator and regulator are retired.

| Alternative | Why rejected |
|---|---|
| Two systems permanently | Violates HLOD ADR §14. Two ledgers would compete for one device. |
| Unify first | An R3 rewrite would come before any data, and the pool would be designed blind. |
| Segmented atlas in the chunk streamer | Not needed at the tuned settings, and it would be built in the component Step C retires. |
| Chunk streamer as the seed | Its unit is a fixed 64k chunk with LOD as a draw count, it has the regulator ratchet, and it has no per-node generations. |

### D-2 The world on disk: one content definition, **GSQ-compressed** in both containers

One seeded, counter-hash, **re-iterable** generator (§6) defines the content. Every tier is
stored **compressed with the GSQ v1 page codec (D-12)** in one or both containers:

| Encoding | Tiers | Role |
|---|---|---|
| **Plain v3 + paged GSQ chunks** (new flag bit 7, D-12) | T0, A | Chunk route. Ships tree-less worlds. Flat arm from M1c onward. |
| **V4 + GSQ node records** (codec field, D-12) | T0, A, AB, AB-span | HLOD route. Ships HLOD worlds. HLOD arm from M3 onward. |
| Plain v3 **uncompressed** | T0 (identity), T-ref (generated per run, transient), **A once for the M1 probe** (deleted at M1c) | Quality references (the U arm), byte-identity proofs, and a codec-free scale probe (D-13). |
| V4 **uncompressed** | small identity worlds only (≤ 1M leaves) | Byte-identity proofs (§4.5). |

Both compressed containers fit the approved cache together (§5.3). So the **flat arm stays
the chunk route on the runner for the whole program**. No "leaves-only cut over V4"
workaround and no disk-option choice is needed.

**Tier AB** is a single V4 package (u32 counts suffice; about 80k nodes against a reader bound
of 2^20, `kMaxNodes` at `io/gaussian_splat_world_paged.h:57` ✓). It is never plain v3.

| AB alternative | Why rejected |
|---|---|
| SH2 | HLOD ADR §14 requires SH3 capacity. |
| Two plain v3 files | Multi-world in one budget does not work: a second live world submission in a scenario is rejected (`core/gaussian_splat_scene_director.cpp:2441-2449` ✓), and each renderer's streaming system takes the full budget. Chunk-only work would be retired at Step C. |
| Lifting the 357.9M SH3 per-file cap (`io/gaussian_splat_world_io.cpp:1617-1624` ✓) | An R3 loader change for a route being converged. With paged GSQ chunks the SH section disappears, so the cap does not apply to the compressed container anyway (D-12). |

Generated worlds **never enter `res://`**. The importer HLOD-bakes non-empty plain worlds
(`io/resource_importer_gsplatworld.cpp:431` ✓), so worlds are loaded by absolute path.

### D-3 What replaces the 512 MiB cap, the 2 GiB staging ceiling on the streamed pool, and the other pilot caps

Each replacement keeps fail-closed refusal with a distinct reason, has boundary tests at the
bound and bound+1, and carries an explicit maintainer disposition in an R3 PR.

| Cap | Replacement | Scope / why this is not a loosening |
|---|---|---|
| `MAX_LEAVES` 1,048,576 and `MAX_PAYLOAD` 1,572,864 | **Kept for resident captures** (`world.cpp:1290-1294` ✓). **Removed from paged admission** (`world.cpp:1191-1194` ✓), which charges no payload (`:1200-1201` ✓). | They bound nothing on the paged route. The 1 GiB `CPU_BYTE_LIMIT` is kept. |
| `MAX_NODES` 32,768 | min(reader `kMaxNodes` = 2^20, the node count whose tables and per-view `CutHistory` workspace fit the CPU ledger) | Derived. Still refused above the bound. |
| `GPU_BYTE_LIMIT` 512 MiB | **Replaced only as the streamed page-budget term** of `hlod_stream_page_budget` (`renderer/resident_atlas_budget.h:118-120` ✓). It becomes the configured streaming budget (tuned 4096 MB, production 2048 MB) clamped by the device ledger. **It is kept for whole-bundle admission**, `hlod_candidate_fits` (`publisher.cpp:1741`, `:1800` ✓), so medium scenes keep their current route. | The 0.60 × device ledger (`renderer/resident_atlas_budget.h:73-80` ✓) is kept. |
| **`kResidentStagingCeilingBytes` 2 GiB, through `atlas_limit`** (`renderer/resident_atlas_budget.h:36`, `:48-63` ✓; `publisher.cpp:1716` ✓; used at `:1779` ✓) | **The streamed pool is no longer clamped by it.** The streamed pool is bounded by the streaming budget, the device ledger and the per-buffer API limit (≤ `UINT32_MAX`, or K segments after Step C). **The 2 GiB ceiling stays for the resident whole-bundle route**, where it bounds a single upload. Boundary tests sit at 2 GiB and 2 GiB + 1 page. | The ceiling exists for one-shot uploads. A streamed pool uploads per run under the per-frame byte cap. |
| Sort capacity = whole-tree `leaf_count` (`publisher.cpp:1685` ✓) | pow2(drawn budget) | Only the drawn set is sorted. |
| HLOD per-visible scratch sized from `max_visible_splats` (`renderer/tile_renderer.cpp:4074-4114` ✓) | Sized from the drawn budget. History and the SH cache stay indexed by pool record, so they scale with **pool size** (≈ 9 B × pool records). This is stated, not hidden. | — |
| Cut `splat_budget` = min(leaf_count, `max_splats`) (`publisher.cpp:1657-1658` ✓, `:2038` ✓) | min(`drawn_budget_splats`, visible cap, pool records). This needs the **new setting** from BV-16c. | — |
| Offline job: 1 worker, 1 job | Per-worker job contexts. Limits come per bucket from the frozen HLOD ADR §15.2 job manifest. | — |
| `materialize_paged_resident` fallback (`nodes/gaussian_splat_world_3d.cpp:664-674` ✓) | **Refusal only above the resident limit.** Worlds that fit RAM keep the warn-and-load fallback. Larger worlds would OOM today, so no working scene breaks. | Closes the HLOD ADR §14.8 stop condition (silent whole-world materialization). |
| A second HLOD view (multi-view) | **Refused with `hlod_multiview_budget_undefined`** until per-view sub-budgets and a deterministic Desired merge exist (P6). This refusal is an interim only, and it keeps its typed reason; light views for splat shadow casters are not deferred (see P6). The drawn budget is a **per-frame total**. | The benchmark has one view. This prevents closure starvation. |

### D-4 Frame time: an honest model. HLOD caps the drawn count; the 3090 targets are at risk until F is measured

**Measured anchor** [M] (3090, 1080p, Mip-NeRF 360 bicycle at about 2.04M visible, wg 512,
optimized build; #539, measurement of 2026-10-09): frame ≈ 33 ms, overlap sort 13.9 ms. The
rest of the split from the same runs — raster 8.9, emit/count/prefix/resolve 3.3, **untimed
≈ 7** (cull, projection, depth pre-sort) and a present gap of 3–4 ms — is not recorded in the
repository or an issue: UNVERIFIED.

**Model:** mean GPU frame ≈ F + k·N, with N = drawn splats in millions; p95 ≈ 1.25 × mean [E];
the 3080 is ≈ 1.2× slower [E].

- **F ≈ 10.5 ms** (present gap 3.5 + untimed 7) **is the honest starting value.** No
  measurement shows that #1180 or #1181 removes the untimed term. They may remove part of it,
  but that is UNVERIFIED.
- k ≈ 8 ms/M for leaf-like content after #539, #1180 and #1181 [E].
- **Interior-heavy HLOD cuts cost more per splat**, because merged interiors are larger and
  produce more overlap records (≈ 12 against ≤ 8 records per splat, §5.2) → **k ≈ 12 ms/M**
  [E].
- k ≈ 5 ms/M is the optimistic benchmark-density case (UNVERIFIED).

**Predicted p95 at a 1.0M drawn budget** [D from the model], as 3090 / 3080 in ms. "miss"
marks a value above the target (3090 p95 16.7 ms; 3080 p95 25 ms):

| F \ k | 5 | 8 | 12 |
|---|---|---|---|
| **3.5** | 10.6 / 12.8 | 14.4 / 17.3 | 19.4 miss / 23.3 |
| **7** | 15.0 / 18.0 | 18.8 miss / 22.5 | 23.8 miss / 28.5 miss |
| **10.5** | 19.4 miss / 23.3 | 23.1 miss / 27.8 miss | **28.1 miss / 33.8 miss** |

**What this says plainly:**

- On today's evidence (F ≈ 10.5, interior k ≈ 12), **neither target is met at 1.0M drawn**.
- **3090 p95 ≤ 16.7 ms needs a mean ≤ 13.4 ms.** That requires both **F ≤ ≈ 5 ms** and
  **k·N ≤ ≈ 8 ms**. With interior k = 12 that means N ≤ ≈ 0.7M.
- Reducing F means three things:
    1. timestamping the untimed passes (BV-04b);
    2. #1180 and #1181;
    3. finding and removing the 3–4 ms present gap, which is not yet attributed. Its cause is
       UNVERIFIED, and it has its own investigation item, BV-24b.
- **3080 p95 ≤ 25 ms** needs a mean ≤ 16.7 ms on the 3080, which is ≤ 13.9 ms in 3090 terms.
  That is the same order of condition, slightly looser.
- **The 4M visible cap as a drawn count misses both targets by about 2–3×.** Under this design
  the 4M cap becomes a **never-binding ceiling** (G10h hard), and the gated workload is the
  HLOD drawn budget (§12 item 2).

**Decisions:**

1. **The 4M visible cap stays** as a ceiling that must never bind on the HLOD arm (G10h = 0,
   hard). That rules out #1131 there.
2. **New setting `drawn_budget_splats`** (BV-16c, before the first HLOD run). It is frozen in
   the thresholds `settings` block by **BV-26a before any HLOD-arm data exists**. The rule is
   pre-registered now:

    > N_frozen = the largest N ≤ 1.0M whose 3090 p95 the model predicts ≤ 16.7 ms, using F
    > and k_interior measured at M1/M1c. F is measured on T0 at ≈ 0 drawn; k is fit from the
    > sweep of 0.25/0.5/1/2/4M drawn, with records per splat per level measured on T0 V4.
    >
    > If N_frozen < 0.5M, the program stops and returns to the maintainer, because quality
    > would be inadequate.

    This uses only flat-route and T0 data, never HLOD-arm results, so it is pre-registration.
3. **τ, the SH read bands (SH3 < 50 m, SH1 50–150 m, SH0 > 150 m) and the codec bit depths**
   are frozen in the same block.
4. **Lowering the drawn budget, raising τ, moving the SH bands nearer or coarsening codec bits
   counts as a loosening** of P1. The guard permits it only when G11q and Q-codec are **hard
   and passing at the new value**, with a maintainer waiver line (§8.4).
5. **The frame targets gate the HLOD arm.** The flat arm is printed against the same values,
   as information only (§12 item 3).
6. **The 3090 is calibrated and gated** on Tier AB (BV-30 → BV-32). The 3090 does not wait for
   Step C.
7. **If M4 is not met** (P1 hard and passing on the 3080 HLOD arm, and likewise on the 3090),
   M4 is reported **"not met"** with its numbers. The next actions are, in order:
    - measure F and k;
    - remove fixed cost (present gap, untimed passes);
    - reduce records per splat at the bake through an interior splat-size bound (with a
      quality gate);
    - the 144 B SH layout (BV-C8);
    - a compact GPU layout (P6).

    The targets never move.

| Alternative | Why rejected |
|---|---|
| 4M drawn | Predicted ≈ 35–45 ms. |
| Lowering the visible cap | Contradicts the maintainer's setting, and the cap would bind and flicker (#1131). |
| Gating the flat arm | It has holes (#1053) and is not the product route. |
| A per-device drawn budget | The two devices would show different content quality, and the evidence would not compare. Kept as a fallback only if the frozen rule yields < 0.5M (§12 item 2). |

### D-5 Telemetry in optimized builds

`PackTelemetry::exchange_and_reset()` returns `{}` without `DEV_ENABLED`
(`core/streaming_upload_pipeline.h:90`, `:222` ✓). So `last_upload_chunks` is always 0
(`core/streaming_diagnostics_surface.cpp:244`, `:455` ✓).

**Precise effect:**

- `upload_stalled` needs queues > 0, `upload_chunks_this_frame == 0` **and**
  `loaded_this_frame == 0` (`core/streaming_diagnostics_surface.cpp:530-533` ✓).
- Only the **broad** forced-sync guard reads the constant
  (`core/gaussian_streaming.cpp:4580-4585` ✓); the narrow guard (`:4571-4576` ✓) does not.
- Both can over-trip in release builds.

**Decision (BV-04a):**

- Gate-consumed counters become release-build atomics: upload chunks and bytes, read bytes,
  decode bytes, and read/decode/pack/upload latency histograms.
- The 30-frame predicate is kept byte-for-byte; only its input is fixed.
- Gates use an additional wall-clock stall metric.

**BV-04b** adds GPU timing completeness:

- binning validity;
- per-frame-serial validity;
- timestamps for the untimed passes (cull, projection, pre-sort, composite), **needed to
  measure F**;
- records per splat per HLOD level.

### D-6 The regulator thrash ratchet

`_detect_thrashing` (`core/streaming_vram_regulator.cpp:416-429` ✓) triggers a 10 %
`max_chunks` cut and **`lod_distance_multiplier` × 1.1 up to 2.0** (`:359-372` ✓), at most once
per 30 frames (`MIN_FRAMES_BETWEEN_ADJUSTMENTS`, `core/streaming_vram_regulator.h:99` ✓;
checked at `.cpp:348` ✓).

Decisions:

1. **V7** freezes the effective budget, the chunk cap, **`lod_distance_multiplier`**, the drawn
   budget and τ in every measured frame. Any drift makes the run INVALID with reason
   `settings_drifted`.
2. **M1 is a probe.** If any regulator event fires, its frame and quality numbers are published
   labelled `settings_drifted`, which is a finding and not a baseline.
3. **BV-08a lands before M1c.** It measures thrash as visible-set refetch within a
   **wall-clock window of K ms**, never a frame count. It has a live negative control at two
   frame rates: a starved budget must trip it at both 60 and 200 fps.
4. Production auto-regulation stays on. This changes production behaviour: the detector no
   longer fires on legitimate fly-through churn. It is a defect fix (false positives), proven
   by the negative control, so it is decided here rather than asked.

### D-7 V4 out-of-core write: spill-and-assemble in canonical BFS order, keeping every V4 writer refusal

The format forces this two-step write: records are sequential in node order
(`paged.cpp:623-626` ✓) and `content_generation` hashes every body (`:971-975` ✓). The
validator needs only parent-before-child and contiguous child ranges
(`core/gaussian_splat_hlod_tree.cpp:109-110`, `:170-171` ✓). The in-core bake emits BFS
(`core/gaussian_splat_hlod_bake.cpp:549-564` ✓).

**Decision (BV-18a/b):**

1. Bodies are spilled per `(bucket, depth)` segment as **GSQ-compressed pages** (W14). The
   spill record carries **the node metrics computed from the decoded runtime form**:
   footprint, `dc_encoding`, `homogeneous_dc`, `center_exact`, and eps/radius.
2. Topology fixes BFS order, offsets and `content_generation` from body digests over the
   **stored** bytes.
3. Assembly runs in BFS order and deletes each segment once it is copied. Peak disk ≈ 1 ×
   output + one segment [D]. The scratch is already compressed, so this is small (§5.3).
4. **W13:** after assembly the writer re-opens the file through `GaussianSplatHlodPagedSource`,
   decodes every node through the reader's relative → absolute round trip, recomputes the
   metrics, and compares them field by field with the spill record.

**V4 refusals kept byte-for-byte** in the out-of-core path:

| Reason | Where |
|---|---|
| `v4_center_reconstruction_not_exact` | `paged.cpp:999-1002` ✓ |
| `v4_metrics_disagree_with_snapshot` → reapplied as `v4_metrics_disagree_with_spill` (same fields, same refusal class) | `paged.cpp:1003-1008` ✓ |
| `v4_payload_record_size_inconsistent` | `paged.cpp:617-621` ✓ |
| `v4_payload_record_misplaced` | `paged.cpp:623-626` ✓ |
| `v4_gaussian_layout_mismatch` | `paged.cpp:349-351` ✓ |
| `v4_payload_record_header_mismatch` with all-zero tail | `paged.cpp:771-775` ✓ |
| reserved-zero node bytes (`v4_nonzero_reserved_node_record`) | `paged.cpp:579-582` ✓ |
| `v4_file_changed_length` | `paged.cpp:751-753` ✓ |
| per-page index digests, `content_generation` | — |
| W11 reader bounds (`kMaxNodes`, `kMaxHighOrder`, `kMaxNodeSplats`, metadata ≤ 16 MiB) | `paged.cpp:389-398` ✓; `io/gaussian_splat_world_paged.h:57-58` ✓; `core/gaussian_splat_hlod_tree.h:45` ✓ |

**Crash handling.** Scratch and temp files live under the cache key directory. The prime job
sweeps orphans under `flock` before it starts. A crash mid-assembly is **not resumable** and
needs a re-bake; that is stated, not hidden.

| Alternative | Why rejected |
|---|---|
| Patch-and-rehash | No producer has node-order arrival. |
| V5 | Not needed: progressive deletion plus compression makes the cost small. |
| Subtree-contiguous order | Breaks byte identity with the in-core bake. |

### D-8 Precision and the AB-span tier

- The benchmark worlds stay within 2–4 km of the origin (ULP ≤ 0.49 mm [D]).
- The chunk route is declared safe to ≤ 8 km, with a `float_precision_risk` telemetry flag
  beyond that.
- GSQ positions are **page-relative** (D-12), so the codec adds no range limit. SPZ, by
  contrast, stores absolute 24-bit fixed point with 12 fractional bits, ±2,048 m [D]
  (`thirdparty/spz/load-spz.cc:351-357` ✓).
- At Step C, degenerate chunk nodes receive node-relative payloads plus per-draw camera
  offsets (HLOD ADR §14.3, package C2). No separate chunk-relative work is done.
- **AB-span** is the HLOD ADR Package G capacity and precision tier. It holds **Tier AB
  content (402,653,184 SH3) spread over ≥ 100 km**, so it is 2.75× VRAM on the 3090 and
  satisfies V3 without exemption. It runs on the HLOD route only, after the origin-shift guard
  is removed on GPU evidence (P6).

### D-9 Not built now

| Item | Reason |
|---|---|
| Multi-world or multi-package worlds | No need. |
| Lazy V4 index pages | Only needed above about 1M nodes. |
| Prefix-LOD residency | Replaced by the cut. GSQ sub-blocks in importance order would make it cheap later. |
| io_uring | — |
| The quantized 160 B route | The codec decodes into 176 B on the async path, so the broken sync-only route is sidestepped (`core/gaussian_streaming.cpp:4597-4605` ✓; the route is also deferred for file-backed worlds, `:1794-1797`, `:2106-2108` ✓). The defect gets its own issue (P6). |
| `ShardedWorldWriter` | — |
| VQ-SH | It needs a k-means pass and a resident codebook, which is a larger quality risk. It is a v2 codec candidate. |
| GPU-side decode | Upload is not the binding resource. |

### D-10 Binaries

- **The producer binary** is `target=editor optimize=speed_trace dev_build=no tests=yes`. It
  runs the generator and bake doctests, the controls' release-build mutation proofs, and M1–M3
  measurement, labelled `binary_kind: editor_tests`.
- **The gated measurement binary is a maintainer decision** (§12 item 1; decided, see "User
  decisions recorded 2026-11-03").
- The recommendation is `template_release optimize=speed_trace dev_build=no` running an
  exported pack. It lands in BV-27, before any P1 calibration. Reasons:
    - it is what a game ships;
    - `TOOLS_ENABLED` and `TESTS_ENABLED` hooks sit in hot paths (for example
      `publisher.cpp:1702`, `:1725` ✓).
- The `editor_tests` versus template A/B runs on **Tier A**, not T0.

### D-11 One device VRAM ledger owner

`renderer/device_vram_ledger` apportions memory to the pool, the sort and overlap buffers (from
the drawn budget or the visible cap), the per-view buffers, the HLOD scratch (by pool records),
and the engine and driver reserves. It also owns the resident object atlas, the instance region
of the HLOD bundle and the shadow-caster scratch, so the world and the objects share one ledger.

- The GS-owned share stays at 0.60 × device.
- It is enforced on the HLOD route and report-only on the chunk route until Step C.
- `VK_EXT_memory_budget` (live per-heap budget and usage; R3 engine work in
  `drivers/vulkan/` and `servers/rendering/`) is **deferred** to P6. G2b already covers the
  external peak, and G2d is **not in the gate table** until it lands.

### D-12 Compressed streaming format: GSQ v1 page codec, decoded on the CPU into today's GPU layout

- **Unit.** A page is one plain-v3 chunk (≤ 65,536 splats) or one V4 node payload (≤ 16,384,
  `kMaxNodeSplats`, `core/gaussian_splat_hlod_tree.h:45` ✓). It is split into **4,096-splat
  sub-blocks**, each its own zstd frame, kept in the page's importance order so that a prefix
  is a valid sample.
- **Header.** Each page has a 64 B header plus constants of ≤ 192 B: codec id, count, stored SH
  degree, the sub-block table, per-attribute dequant ranges, and a SHA-256 of the stored
  bytes.
- **Layout inside a sub-block.** SoA, byte-planed, with positions delta-coded in the caller's
  order. The codec never reorders (`io/gsq/gsq_codec.h:5-6` ✓, `io/gsq/gsq_codec.cpp:559-565`
  ✓). **Spatial order is the writer's job:** BV-C3 sorts spatially (Morton) before encoding
  (decision 2026-11-07, #1347). How that sort coexists with the importance-ordered prefix
  (§6.2 "Per-chunk order") is fixed in BV-C3.
- **Page constants.** The fields D-12 does not quantize (area, DC alpha, stroke age, brush
  axes, `painterly_meta`, `render_meta`) are stored once per page. The codec refuses a page
  whose splats differ in them with `gsq_heterogeneous_field` (`io/gsq/gsq_codec.cpp:361-366`,
  `:434-438` ✓). Normals are the exception: they take per-splat planes when they differ
  (header flag bit 1). **The BV-C3 page builder must group splats with equal page constants
  into pages.** Any remaining failure must be visible, as an error or a counted fallback,
  never silent, and painterly content must stay encodable (decision 2026-11-07).
- **Quantization** (pre-registered defaults, frozen in `settings`):

| Attribute | Encoding |
|---|---|
| Position | u16×3 page-relative, or 3 × 21 bit when the step exceeds 0.25 mm |
| Scale | log scale, 3 × 10 bit per-page range |
| Rotation | smallest-three, 32 bit |
| Opacity | unorm16 |
| DC | R11G11B10, per-page range |
| SH | bits 6/5/5 by band |
| Normal | only when flagged |

- **Accepted error, conditional.** Rotation error ≤ ≈ 0.28° (smallest-three, 3 × 10 bit plus a
  2-bit index; bound and test in `tests/test_gsq_codec.h:211-213` ✓) and SH stored as 1 B per
  coefficient (one byte plane per coefficient whatever the band's bit depth,
  `io/gsq/gsq_codec.h:39` ✓) are accepted (decision 2026-11-07). The condition: the visual
  real-scan check of #1357 passes before GSQ becomes a default.
- **Size.** Positions take 21 bits whenever 16 bits would be coarser than the 0.25 mm step,
  i.e. on any page wider than ≈ 16.4 m (`io/gsq/gsq_codec.cpp:510` ✓), rounded to 3 B per
  axis. So 23 B of geometry plus DC and 45 B of SH3 give **68 B fixed width**
  (`io/gsq/gsq_codec.cpp:289-291` ✓; 65 B only on pages narrow enough for 16-bit
  positions), then **≈ 30–38 B after zstd** [E] (BV-C2 measures it).
- **Decode location.** The decode runs **in the pack workers**, straight into the existing
  176 B `PackedGaussian` (`renderer/gaussian_gpu_layout.h:340` ✓).
    - Shaders are unchanged and the async path is kept.
    - Godot's `Compression::decompress` serialises every caller on one global mutex and DCtx
      (repo root `core/io/compression.cpp:45-48`, `:200-217` ✓). The codec therefore calls
      zstd directly with **one `ZSTD_DCtx` per worker**, using the module's existing zstd
      include path (the SPZ build environment, `SCsub:20-21` ✓). That works with builtin
      zstd; system-zstd builds are UNVERIFIED.
- **Containers:**
    - **V4:** a codec-id field. The header reserved word and record tail are zero today and
      enforced zero (`paged.cpp:579-582`, `:771-775` ✓). The exact placement is fixed in
      BV-C4, and a test proves that **old readers refuse** it. `GAUSSIAN_STRUCT_LAYOUT_VERSION`
      keeps meaning the Gaussian struct layout and is not reused as a codec id.
      `stored_bytes` becomes the compressed size. The record SHA-256 covers the stored bytes.
    - **Plain v3:** a new flag bit 7 `kFlagPagedChunks`. Bits 0–5 are the plain flags and bit 6
      is HLOD (`io/gaussian_splat_world_format.h:15-16` ✓); unknown flags are refused by older
      readers (`:34-35` ✓). Sections become page-aligned GSQ records. The chunk table gains
      offset, stored size, extent and digest. **The index lists disappear** (pages are
      chunks), which removes 4 B/splat on disk and 16 B/splat of RAM (four per-splat `u32`
      index copies, for example `core/streaming_asset_types.h:153-155` ✓). The SH section, and
      with it the 357.9M SH3 cap, does not exist in this container.
- **Determinism.** Encoding uses integer-exact quantization with `-ffp-contract=off`-safe code.
  A cross-platform golden decoded digest is checked on Windows/MSVC and Linux/GCC.
- **Semantic read validation stays:** counts, sizes, finite ranges, decoded positions within the
  page AABB plus 3σ, with `gsq_*` reasons.
- **Quality** is gated by the U/C/S/D arms (Q-codec, §8.3): uncompressed, codec, SPZ control,
  degraded control.
- **The GPU SH layout of 144 B** (15 SNORM10 words instead of 24 for SH ≤ 3; 9 words are dead
  today, `renderer/gaussian_gpu_layout.h:312-319` ✓) is BV-C8, in P4, off the critical path.
  It fits 22 % more records in the same VRAM with no quality risk.

| Alternative | Why rejected |
|---|---|
| SPZ as the container | One zstd frame per attribute for the whole scene gives no random access (`thirdparty/spz/load-spz.cc:111-121` ✓), and positions are absolute ±2 km. Its quantizers are reused as test oracles and as the S control arm. |
| Whole-section gzip (bit 4) | Resident-only, ≤ 14.9M splats. |
| A neural or retrained codec | Not decodable per page at streaming rates. |
| LZ4 or meshoptimizer | LZ4 is not vendored (a new repo-root `thirdparty/` dependency, R3); meshoptimizer is built by the separate `modules/meshoptimizer`, which creates a module dependency. |
| GPU decode | Optimises upload, which is not binding. |
| A compact GPU layout now | Needs a stride-class atlas and shader decode, has an uncertain frame-time sign, and sits in P6. |

### D-13 One uncompressed scale probe (M1), then compressed everywhere

- **M1** runs Tier A **uncompressed** plain v3: 54.8 GiB, within the approved cache, with
  nothing else cached at the time (§5.3).
- **Why:** it needs no codec work, so it delivers scale evidence (O(N) residue, RAM, the
  regulator, the first > 2 GiB single atlas buffer) about 3 weeks earlier. It also separates
  scale effects from codec effects. U-versus-C on the same route at scale is the confound
  control for disk throughput and pack CPU.
- **Then:** the file is deleted when compressed Tier A is primed. M1c is the first compressed
  run, and **every gated run is compressed**.
- No code is built only for M1: the uncompressed writer path is needed for identity proofs and
  references anyway.

### D-14 Cold-cache method

A compressed Tier A of about 7 GiB fits in the 24 GB page cache, so cold runs must be produced
and proven, not assumed.

1. After generation, and **after every per-run verify**, `fdatasync` and then
   `posix_fadvise(POSIX_FADV_DONTNEED)` on every world file. This needs no privilege for one's
   own clean pages. Alternatively, verify with `O_DIRECT`.
2. **Residency check:** `mincore()` through Python `mmap` + `ctypes` (no root needed) must show
   ≤ 1 % of world pages resident before a cold pass. Otherwise the run is INVALID with reason
   `cache_warm`.
3. **V10:** on cold segments, `/proc/<pid>/io read_bytes` ≥ 0.9 × the manifest's expected
   unique stored bytes. Otherwise the run is INVALID with reason `cache_warm`.
4. **Host cache.** `O_DIRECT` throughput on a never-read region is compared against a re-read.
   If the host caches the VM disk, every cold result carries `host_cache: present`, and "cold"
   means *guest-cold* only. G5 and G6t verdicts carry that label.
5. A **warm pass** is reported separately and never merged with the cold one.

---

## 3. Target architecture

### 3.1 Components

```
 offline (prime job, producer binary)
   Generator (re-iterable) ─► gs_world_writer (io/, R3) ─[GSQ encoder, W12 encode→decode→validate]─► plain v3 paged-GSQ world
                          └─► BucketPartitioner ─► TiledHlodBake (core/, R3) ─► spill (GSQ pages + metrics) ─► assemble ─► V4 GSQ package
                                           files + sidecar manifest (stored + decoded digests per page)
 runtime (render-thread owner)
   WorldPackage (io/)   : open → Info + IndexView (V4: index only; plain: chunk table only)
   PayloadReadService   : positional reads on a reader pool; pooled handles; cancel; bounded bytes in flight
   Decode/pack workers  : GSQ decode (per-worker ZSTD_DCtx) → PackedGaussian 176 B → bounded ready queue
   ResidencyManager     : seed NodeResidency; ledger ◄ DeviceVramLedger; SegmentedPagePool (K=1 until Step C)
                          state lists + intrusive LRU per protection class; tickets/generations; retire queue
   Demand producers     : HLOD ViewSet/CutHistory | node instances (rows 1..N) | legacy chunk visibility (until Step C)
                          → one draw list, one sort, one ledger per view
   Draw list            : DrawnNodeGPU {instance_id, chunk_id, fade, reserved, camera_offset[4]} 32 B
                          (renderer/gaussian_gpu_layout.h:269-278; segment id goes in `reserved` at Step C)
   Renderer             : depth/projection → COUNT/EMIT → overlap sort → raster; scratch sized from the drawn budget
   Telemetry            : release atomics + per-frame-serial GPU timings → binary frame trace (file)
```

The `DrawnNodeGPU` layout was re-read at `renderer/gaussian_gpu_layout.h:269-278` ✓. Its
`instance_id` carries real instance rows (world = 0, node instances = 1..N), not only the
identity world.

### 3.2 Interfaces

| Component | Interface | Class |
|---|---|---|
| `gs_world_writer::Writer` | §4.3 | `io/`, R3 |
| `gs_page_codec` | `encode(page_in, params) → bytes + stored_digest`; `decode(bytes, dctx, out_packed176 \| out_gaussian) → decoded_digest`; `validate(bytes)` | `io/`, R3 |
| `BucketPartitioner` | `histogram`, `frontier(B_max)`, `partition`. A re-iterable source skips the partition scratch. | `io/`, R3 |
| `TiledHlodBake` | `bake_package(source, job_manifest, out) → Report` | `core/gaussian_splat_hlod_*`, R3 by design |
| `PayloadReadService` | `submit(file, off, len, ticket)`, `cancel`, `stats` | `io/`, R3 |
| `ResidencyManager` | `begin_frame`, `protect`, `request`, `complete`, `evict/compact`, `resident_mask`, `ledger` (render thread only) | `lod/` + `renderer/`, R2; the layout-sync parts are R3 |
| `DeviceVramLedger` | `reserve(cat, bytes)`, `headroom`, `snapshot` | `renderer/`, R2 |

### 3.3 Invariants

| # | Invariant |
|---|---|
| I-1 | No per-frame O(N_world) CPU work. A scaling test runs at 10k, 100k and 1M nodes. |
| I-2 | Whole-run admission. An unplaceable run is skipped, not waited for. Compaction starts after R = 8 skips. |
| I-3 | Protected closure = pinned ∪ drawn ∪ ancestors ∪ fades ∪ in-flight ∪ retiring. The budget is checked against allocated pages. |
| I-4 | Complete-or-nothing publication. The first READY frame is the root or the bootstrap frontier. It holds **per producer**: a producer that is not ready never blanks another (a loading world still draws the objects, and a refused object producer never blanks the world). |
| I-5 | Stale-generation completions are dropped and counted. |
| I-6 | Ledger honesty. Every resize pre-flights old + new. |
| I-7 | Drawn ≤ drawn budget ≤ visible cap. On the HLOD arm the visible cap never binds. |
| I-8 | No silent whole-world materialization. |
| I-9 | Every gate-consumed counter exists in release builds **and** has a forcing control (§8.5). |
| I-10 | Deterministic ties by `(instance_key, NodeKey)`. Tested with ≥ 2 producers (world + node instances). |
| **I-11** | A single owner, the render thread, for every residency mutation. Workers communicate only through bounded queues. |
| **I-12** | Each page is encode → decode → validated by the writer before it is appended. Readers never trust a digest in place of semantic validation. |

### 3.4 Per-frame data flow (render thread)

1. **`begin_frame`.** Process completed upload tickets:
    - generation check;
    - a run becomes **RESIDENT once its upload command has been recorded ahead of this frame's
      draws**;
    - publish `resident_mask`.

    **Freed pages are reused only after the frame-delay retirement** (frames in flight). That
    delay gates reuse, not visibility.
2. **Selection** over `resident_mask` produces Drawn (a complete cut), Desired (missing children
   of blocked refinements, by e·b) and Keep. It is monotone under caps and costs
   O(visited log visited).
3. **Protection** of the closure (I-3).
4. **Requests.** Per-frame caps on request count, eviction count and upload bytes ≤ staging
   ring / frames in flight. Each request: ledger check, eviction outside the closure, skip if
   nothing fits, ticket → `PayloadReadService`, and cancellation of stale tickets.
5. **Workers.** Positional read of the compressed page (≈ 1.5–1.9 MB per 49k chunk, ≈ 0.5 MB per
   16k node [E]). SHA-256 check of the stored bytes, which costs ≈ 8× less CPU than today's
   uncompressed record hash (measured at M3). GSQ decode and pack into 176 B. Bounded ready
   queue. Upload into the run.
6. **Draw list** in cut order, then the renderer.
7. **Telemetry** to the frame trace.

**Coupling.** Cut and residency interact only through `resident_mask`, the Desired order and
the closure. On the HLOD arm a missing child keeps its parent drawn, so there are no holes;
starvation shows as coarser frames and a slower settle. On the flat arm a missing chunk is a
hole.

---

## 4. Data formats and the `io/` writer API

### 4.1 Formats

| Format | Status | Use |
|---|---|---|
| Plain v3, uncompressed | Unchanged | Identity proofs, U arm, T-ref, the M1 probe |
| **Plain v3 + bit 7 paged GSQ chunks** | New (BV-C5a, R3) | Compressed chunk worlds |
| V4, uncompressed | Unchanged | Identity proofs |
| **V4 + GSQ codec field** | New (BV-C4, R3) | Compressed HLOD worlds |
| Sidecar `world.manifest.json` | New | Schema; generator id and `generator_params_sha`; `bake_sha`; writer and codec versions; per-page stored and decoded digests; totals; ground truth (§6.4); the `.complete` digest |

Old readers refuse every new file. Each container has a test that proves it.

### 4.2 Writer modes

| Target | SourceMode | Codec | Producer |
|---|---|---|---|
| `PLAIN_V3` | REITERABLE | none or GSQ | generator |
| `PLAIN_V3` | SINGLE_PASS | none or GSQ | converters, a future importer path (P6) |
| `PAGED_V4` | SINGLE_PASS (spill-and-assemble) | none or GSQ | `TiledHlodBake` |

### 4.3 API

```cpp
namespace gs_world_writer {
enum class Target : uint8_t { PLAIN_V3, PAGED_V4 };
enum class SourceMode : uint8_t { REITERABLE, SINGLE_PASS };
struct CodecParams { uint32_t codec_id /*0 none, 1 GSQ v1*/; uint8_t sh_bits[3]; uint8_t opacity_bits; float pos_step_m; int zstd_level; };
struct Limits { uint64_t max_ram_bytes, max_scratch_bytes, max_output_bytes, deadline_usec, min_free_disk_margin; }; // 0 = refuse
class Writer {
public:
  Error begin_plain(const String &final_path, const PlainSpec &, const CodecParams &, SourceMode, const Limits &, gs_job::Context * = nullptr);
  Error begin_paged(const String &final_path, const PagedSpec &, const CodecParams &, SourceMode, const Limits &, gs_job::Context * = nullptr);
  Error append_chunk(const ChunkDesc &, const Gaussian *, const Vector3 *sh_high, uint32_t count);
  Error append_node_body(uint32_t bucket, uint32_t depth, uint64_t build_id, const Gaussian *, const Vector3 *, uint32_t count, const NodeMetrics &);
  Error set_topology(const NodeTopology &final_bfs);
  uint32_t current_pass() const;  Error finalize();  void abort();  void request_cancel();
  Progress get_progress() const;  const Report &get_report() const;
};
}
```

**Durability, per OS.** The long-lived `GSAtomicFileTransaction` owns a **native handle**:

- Linux: `open` / `write` / `fdatasync` / `close` / `rename`, then fsync the directory.
- Windows: `CreateFileW` with share-read, `WriteFile`, `FlushFileBuffers` on the same handle,
  `CloseHandle`, then `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`
  (`io/gs_atomic_file_writer.cpp:101-125` ✓). Windows has no directory fsync.

It does not use Godot `FileAccess` for the long-lived write. `FileAccess` adds its own backup
temp, and on Windows it opens `_SH_DENYRW`, which blocks a second flushing handle. The
precedent for module-local native code is `io/gs_atomic_file_writer.cpp:12-17` ✓.

**Cancellation** uses an atomic flag, checked every ≤ 1 MiB.

### 4.4 Writer invariants

The base invariants W1–W11 are kept:

| # | Invariant |
|---|---|
| W1 | Nothing exists at the final path until `finalize()` returns OK; on error, cancel, deadline or destructor the temp and scratch files are removed and a pre-existing final file stays byte-intact. |
| W2 | Monotonic growth: every write lands at or past the current end, except the final header rewrite. |
| W3 | Plain section order: gaussians in chunk order, then SH in the same order. |
| W4 | Declared means exact: chunk counts, chunk number and SH element count must match, else `finalize()` fails with `count_mismatch`. |
| W5 | Bounded RAM: chunk table + one copy slice + one SHA-256 context per open section; index lists are never held. |
| W6 | Validity at append time, with the loaders' rules. |
| W7 | Determinism: output bytes are a pure function of the spec and the appended stream. |
| W8 | Digests per chunk and per file, in the sidecar manifest. |
| W9 | Durability (§4.3). |
| W10 | Progress is monotonic; cancel is observed within ≤ 1 MiB of I/O and returns a distinct reason. |
| W11 | Per-file caps refused at `begin_*`, including every reader bound; the writer never produces a file its reader refuses. |

New ones:

| # | Invariant |
|---|---|
| W12 | Each page is encode → decode → semantically validated before it is appended. |
| W13 | V4 post-assembly: reopen, decode every node, recompute metrics, compare field by field. The D-7 refusals are kept. |
| W14 | SINGLE_PASS spill is compressed pages. |
| W15 | The Report carries both digests per page (stored and decoded canonical). |
| W16 | Codec parameters are written into the manifest and must equal the frozen `settings` codec block for gated tiers. Otherwise the cache is invalid. |

### 4.5 Byte-identity and equivalence proofs

| Path | Proof |
|---|---|
| Plain v3, codec = none | The writer equals `ResourceSaver::save` for seeds {1, 2, 3} × SH {0, 1, 3} × chunks on/off × metadata on/off, in both modes. Negative controls: 1-ULP radius change, metadata key order, a single byte, skipping pass 2. |
| V4, codec = none | `TiledHlodBake` equals in-core `bake_hlod` + `write_package` **only on worlds ≤ 1M leaves**, the limit where `write_package`'s resident capture is admitted (`world.cpp:1290-1294` ✓). **B_max is forced small (64k)** so that each world has many buckets and a multi-level stitch. Worlds: holzbank, baum3 (800k), and a synthetic 1M world. No test-only cap bypass is built. |
| Codec = GSQ | The tiled bake's **decoded canonical digest per node** equals `encode` + `decode` of the in-core bake output through the same writer, on the same worlds. Plain GSQ decoded output equals the uncompressed U decode within the per-attribute error bounds. The golden digest matches across platforms. |
| At scale (201M) | No identity claim. W13 recompute plus a full reader round trip, with the stored and decoded digests recorded in the manifest. |

---

## 5. Budgets per device

### 5.1 Frame-time model

See D-4 for the sensitivity table. The decisive measurements are:

- F, on T0 at ≈ 0 drawn, optimized, M1;
- k per content class from the drawn sweep (BV-07);
- records per splat per HLOD level (BV-04b on T0 V4).

Until they exist, **the 3090 targets are "at risk"** and the 3080 targets are "plausible only
if F ≤ ≈ 7 ms".

### 5.2 VRAM and RAM, tuned settings, 1080p (decode to 176 B, so VRAM is unchanged by the codec)

| Item | Flat arm (chunk route) | HLOD arm (drawn budget N_frozen ≤ 1.0M) | Label |
|---|---|---|---|
| Pool / atlas | 3,520 MiB (320 × 64 pages × 180,224 B) | ≤ 4,095 MiB (streaming budget; staging ceiling exempt per D-3; ledger-clamped) | [D] |
| Per-visible buffers (refs, depth sort, projection, visibility) | ≈ 318 MiB (4M) | ≈ 80 MiB (pow2 of 1M) | [D] |
| SH cache + subpixel history (per pool record ≈ 9 B) | ≈ 180 MiB | ≈ 216 MiB at 24M records | [D] |
| Overlap sort, steady / bootstrap | 855 / 2,670 MiB before E3; demand-driven after BV-10 | ≈ 340 MiB at ≈ 12 records/splat (interior) | [D]/[E] |
| Textures, tiles, staging, engine | ≈ 0.5 GiB | ≈ 0.5 GiB | [E] |
| Driver and others | 3080 VM 0.3–0.6; 3090 0.5–1.5 GiB | same | [E] |
| **Total, 3080** | ≈ 5.6–5.9 steady; ≈ 7.4–7.7 bootstrap before E3 | ≈ 5.5–6.1 GiB | [E] |
| **Total, 3090** | ≈ 5.8–6.8 GiB | ≈ 5.7–7.0 GiB | [E] |
| With BV-C8 (144 B) | pool holds 22 % more records | same | [D] |

"E3" is the demand-driven overlap bootstrap delivered by BV-10.

| RAM | Flat, M1 probe (uncompressed) | Flat, GSQ (M1c+) | HLOD, A | HLOD, AB | Label |
|---|---|---|---|---|---|
| Index / topology | 3.0 GiB steady (4 × u32 per splat), 3.9 GiB load peak | **≈ 2–4 MB** (chunk table only; pages are chunks) | ≈ 36 MB | ≈ 72 MB | [D] |
| I/O, decode, pack in flight | ≈ 90 MB + unbounded ready queue (bounded after BV-08c) | ≈ 4 × (1.9 MB stored + 8.65 MB packed) ≈ 42 MB | ≈ 16 × (0.6 + 2.9 MB) ≈ 56 MB | same | [D] |
| Godot and the rest | 1.5–2.5 GiB | 1.5–2.5 GiB | 1.5–2.5 GiB | same | [E] |
| Page cache (OS) | world ≫ RAM | world ≈ 7 GiB: **can be fully cached** (D-14) | ≈ 8 GiB, likewise | ≈ 16 GiB | [D] |
| G4 limit | ≤ 12 GiB | ≤ 12 GiB | ≤ 12 GiB | ≤ 16 GiB | kept |

### 5.3 Disk (compressed sizes are [E] until BV-C2; the fixed-width upper bound with no zstd gain is in brackets)

Compressed sizes per tier:

| Tier / encoding | Size | Label |
|---|---|---|
| A plain v3 uncompressed (M1 only) | 54.8 GiB | [D] |
| **A plain GSQ** | **5.6–7.1 GiB** [12.75] | [E]/[D] |
| **A V4 GSQ** (interior SH3, D-9: no interior truncation yet) | **7.9–8.7 GiB** [15.3–16.9] | [E]/[D] |
| **AB V4 GSQ** | **15.8–17.4 GiB** [30.5–33.9] | [E]/[D] |
| **AB-span V4 GSQ** | 15.8–17.4 GiB [30.5–33.9] | [E]/[D] |
| T0 (4.19M): U plain / GSQ plain / GSQ V4 | 1.14 / 0.12–0.15 / 0.16–0.18 GiB | [D]/[E] |
| T-ref (≤ 12M per checkpoint, uncompressed), **generated per run, one at a time, deleted after use** | ≤ 3.3 GiB transient | [D] |
| Compressed bake spill (per segment) + assembly | ≈ 1 segment + small | [E] |
| Synthetic-content caveat | Hash-noise SH would compress poorly, so the generator uses spatially correlated SH fields, and the ratio is **reported separately from real scans** | — |

**Runner `gpu-linux-3080` (≈ 228 GB):**

| Item | GB | Label |
|---|---|---|
| OS, tools, runner | 30 | [E], measured by `df` in BV-06a before M1 |
| Dev scons cache (`cache_limit=50`, GiB) | 53.7 | config, unchanged |
| Optimized scons cache (`cache_limit=15`) | 16.1 | proposal |
| Workspace, bin, artifacts | 20 | [E] |
| Enforced free margin (25 GiB) | 26.8 | proposal |
| **Subtotal without worlds** | **146.6** | |
| **During M1:** A uncompressed + T0 | 58.8 + 1.5 → **207 GB total** | fits (21 GB slack beyond the margin) |
| **Steady from M1c:** A plain GSQ + A V4 GSQ + T0 + T-ref transient + spill | 7.6 + 9.3 + 1.5 + 3.5 + 1 → **≈ 169.5 GB total** (worst case fixed width at 68 B: ≈ 185 GB) | fits |
| **World cache** | ≈ 21.5 GiB steady (≤ 34 GiB worst); 56 GiB during M1 | **within the approved ≈ 60 GiB** |

**Result: no VM disk growth and no new cache approval.**

- The preflight refuses with "DID NOT RUN: disk" if the measured `df` leaves less than the
  margin.
- The cache is regenerated delete-first under `flock`, and the lane reports
  `NOT_RUN: cache absent` meanwhile.

**Workstation data drive (93 GB free [M], `df` on 2026-11-03):**

- Planned use: AB V4 GSQ 17–19 GB, A plain + V4 GSQ ≈ 17 GB, T-ref transient 3.5 GB, spill
  ≈ 2 GB → ≈ 40 GB.
- AB-span (+17–19 GB) **replaces** AB when it runs.
- With the 25 GiB margin the usable space is ≈ 66 GB, so it fits.
- The same "DID NOT RUN: disk" preflight applies on the workstation.
- The workstation's system drive is never used.
- This uses the maintainer's machine, which is part of §12 item 4.

### 5.4 Throughput (re-derived with compression)

The plain splat density is 43.05/m² (§6.2). The frontal width ranges from 645 m (sector chord
2r·sin(θ/2) at a 400 m far plane and 107.5° horizontal FOV) to 1,091 m (planar far-plane edge
at 400 m) [D]; both bounds are carried.

| Case | New splats/s | Disk, uncompressed (292 B) | **Disk, GSQ (35 B)** | Decode + pack CPU (2 workers) | Upload (176 B) |
|---|---|---|---|---|---|
| 60 m/s | 1.67–2.82M | 0.49–0.82 GB/s | **58–99 MB/s** | ≈ 0.2–0.5 core decode + pack (pack 6–16M/s per worker [E]) | 0.29–0.50 GB/s |
| 120 m/s | 3.33–5.64M | 0.97–1.65 GB/s | **117–197 MB/s** | ≈ 0.4–1.0 core + pack; pack may bind | 0.59–0.99 GB/s |
| Teleport refill (≤ 14.6M splats, 2.4 GiB resident) | once | 4.3 GB → ≈ 6–9 s at 0.5–0.7 GB/s | **0.51 GB → ≈ 0.5–1.7 s at 0.3–1 GB/s** | ≈ 0.5–1.2 s pack (2 workers) | 2.57 GB at ≤ 128 MB/frame ≈ 0.35 s at 60 fps |

**Consequence:** disk stops being the binding resource; **pack and decode CPU binds** at
120 m/s. G6t ≤ 6 s (3080) becomes plausible even on a slow VM disk, but it is still
statistical (§8.3). BV-06a measures disk (`dd` with `O_DIRECT`; `fio` only if present), and
BV-C2 measures decode and pack MB/s per core.

---

## 6. Test world specification

### 6.1 Tiers

| Tier | Splats | Extent | Encodings | Host | ×VRAM (176 B GPU-equivalent) | Purpose |
|---|---|---|---|---|---|---|
| T0 | 4,194,304 | 512 × 512 m | U plain, GSQ plain, GSQ V4, U V4 (identity) | both | fits | controls, F/k sweep, identity, smoke |
| T-ref | ≤ 12M per checkpoint | the checkpoint's full-detail frustum sub-world | U plain, generated per run | both | fits | U references for both arms |
| **A** | 201,326,592 | 2,048 × 2,048 m | U plain (M1 only), **GSQ plain, GSQ V4** | runner | 3.30× of 10 GiB | primary benchmark |
| **AB** | 402,653,184 (A + B) | 4,096 × 2,048 m | **GSQ V4** | workstation | 2.75× of 24 GiB | 3090 tier |
| **AB-span** | 402,653,184 | AB's 128 tiles on deterministic sites over ≥ 100 × 100 km | **GSQ V4** | workstation | 2.75× | HLOD ADR Package G capacity and precision (P6) |

**V3** (≥ 2.5× VRAM, correct device class) applies to every gated tier. **No exemption is
needed.**

### 6.2 Content, Tier A (exact total)

| Element | Spec | Splats |
|---|---|---|
| Towns | 6 × 40 × 40 m at 800/m² | 7,680,000 |
| Corridor walls | 2 walls × 1,800 m × 4 m at 300/m² | 4,320,000 |
| Foliage class (low-alpha translucent; #1290 failure mode) | 64 clusters × 30 × 30 m at 200/m² | 11,520,000 |
| Beacons | 4 per tile × 64 tiles × 4,096 | 1,048,576 |
| **Open plain** | the remainder, over 4,194,304 − 9,600 − 21,600 − 57,600 = 4,105,504 m², i.e. **43.05/m²** | **176,758,016** |
| **Total** | | **201,326,592** (exact; asserted by the generator) |

- **Chunk grid.** Cells are aligned to the HLOD lattice: 32 m = 2^5 m, origin-aligned, ≈ 44,083
  plain splats per cell [D].
- **Dense content** subdivides by powers of two until each cell holds ≤ 65,536 splats: 8 m
  cells in towns (51,200 each), 16 m cells for foliage (51,200), wall cells as needed.
- **Chunk count** ≈ 4,096 base cells plus ≈ 300–500 subdivision cells [E]. **The exact count is
  computed by the generator and recorded in the manifest.** The text claims no fixed 4,096.
- **Per-chunk order.** Chunks are in Morton order. Splats inside a chunk follow a stratified,
  importance order, matching the GSQ sub-block prefix order.
- **SH** is degree 3 from spatially correlated noise fields, so the content is compressible and
  view-dependent.
- **Splat scale** is 3–6 px at 10 m, with ≤ 8 records per leaf splat.
- **AB** = A plus a second, distinct-seed 2,048 m half with the same composition.

### 6.3 Generator

The generator is a counter-hash of `(seed, tile, chunk, index)`, so it is re-iterable and
region-addressable.

- `generator_params_sha` = sha256 of all generator parameters (densities, areas, seeds,
  SH-field parameters, scale distribution).
- `gen_sha` = sha256(generator source + `generator_params_sha` + writer and codec version +
  codec params).
- For V4, **`bake_sha`** = sha256(bake, writer and codec sources + the frozen HLOD ADR §15.2 job
  manifest).
- The V4 cache key is `gen_sha + bake_sha`.
- **After the first prime, the top-level world digests are committed to the thresholds
  provenance.** V2 then checks the cache against the repo, not against itself.

### 6.4 Ground truth in the manifest

The manifest records:

- per-chunk and per-node counts, AABBs, and stored and decoded digests;
- beacons;
- per checkpoint:
    - the frustum chunk set;
    - the expected drawn count;
    - the expected beacons;
    - the T-ref spec;
    - the **oracle cut at the frozen drawn budget** (deterministic by I-10) for Tier A **and
      Tier AB**;
- the oracle's expected unique uploaded bytes over the path, per arm (V9).

---

## 7. Fly-through benchmark

### 7.1 Configuration

- 1920×1080, vsync off, Vulkan; Xorg :0 on the runner.
- Tuned settings plus the frozen block: `drawn_budget_splats`, τ, SH bands, codec params.
- **Binary kind** recorded (`editor_tests` up to M3; the gated binary per §12 item 1 from
  BV-27).
- A production-defaults pass, informational and never merged.
- Quantized streaming is excluded.
- Upload per frame is clamped to ≤ staging ring / frames in flight (BV-08c).

### 7.2 Path

Pose is a function of path distance on an arc-length Catmull-Rom spline. **`path_sha256` is
frozen in the thresholds file.**

| Seg | Kind | Speed / extent |
|---|---|---|
| S0 | cold hold P0, capture C0 | until ready + 5 s |
| S1 | corridor walk, ±15° yaw | 6 m/s, 240 m |
| S2 | corridor drive | 25 m/s, 750 m |
| S3 | stop, C1 | settle |
| S4 | diagonal across the plain | 60 m/s, 1,500 m |
| S5 | burst | 120 m/s, 600 m |
| S6 | stop, C2 | settle |
| S7 | reversal | 60 m/s, 900 m |
| S8 | town 360° yaw, C3 | 10 m/s, 24°/s |
| S9 | teleport > 1.5 km, C4 | settle |
| S10 | climb to 150 m, pitch −45°, C5 | 20 s |
| S11 | along tile seams, C6 | 15 m/s, 450 m |
| S12 | teleport to P0, C7 | settle |

### 7.3 Passes

1. **Controls first** (§8.5). Each asserts a specific expected `(gate, verdict, reason)`
   tuple. The run is INVALID only when a control's observed tuple **differs** from its
   expected tuple. That covers:
   - a wrong verdict or a wrong reason;
   - an unexpected INVALID;
   - a control harness failure or crash.

   A control whose expected tuple is itself INVALID (C-off, C-blind, C-settings, C-warm)
   **passes** when it ends with exactly that INVALID and reason. It proves the validity
   check fires.
2. **Pass A, paced, cold, then warm.** Wall-clock movement. A frame trace in a file with
   `rows_written == frames_rendered`. Runs for the HLOD arm (from M3) and the flat arm (chunk
   route on plain GSQ).
3. **Pass B, settle** at C0–C7. The run pumps until a condition holds or a wall-clock deadline
   passes:
    - flat: missing chunks = 0;
    - HLOD: **the drawn cut equals the manifest's oracle cut at the frozen drawn budget**, with
      no "or budget-bound" escape.

    Then it captures, generates that checkpoint's T-ref, renders the references in fresh
    processes, scores at full resolution with ROI, and deletes the T-ref.
4. **Codec arms U/C/S/D** on T0 and on two T-ref subsets, plus ≥ 3 real-scan fixtures, all
   through the same streaming route and decode path. U is file-backed streaming, never
   resident.

### 7.4 References

| Arm | Reference | Measures |
|---|---|---|
| Flat | T-ref U, same route, same LOD | streaming and codec correctness |
| HLOD | T-ref U, full detail, LOD off, depth-slab composited by the same binary; slab noise floor measured on T0 | per-band far quality, **codec + HLOD together**; Q-codec isolates the codec share |
| Codec | U, C, S, D arms | codec loss against the uncompressed source and against SPZ |

### 7.5 Run time

Typical weekly run, warm, [E] until measured:

| Step | Minutes |
|---|---|
| Checkout and tools | 2 |
| Contention preflight | 0.5–15 |
| Build | 2 warm, 25–45 cold, ×2 if the template binary is chosen |
| Verify ≈ 17 GiB + fadvise + mincore | 0.5–1 |
| Controls | ≈ 15 |
| Pass A (2 arms × cold and warm) | ≈ 24 |
| Pass B (8 checkpoints with T-ref generation and slab references) | 20–30 |
| Codec arms | ≈ 8 |
| Post | 2 |
| **Total** | **≈ 75–100 typical, ≤ 150 job limit** |

---

## 8. Acceptance gates

### 8.1 Principles

- **Verdicts:** PASS, FAIL, INVALID, VOID (contention, exit 75) and NOT_RUN. **There is no
  PENDING status.** A gate whose prerequisite has not landed is **absent from the thresholds
  file**. It is added when the prerequisite lands, which is a tightening. The verdict module
  maps any unknown status to INVALID.
- **Two gate classes:**
    - **Correctness** gates are hard from their first run.
    - **Statistical** gates have pre-registered fixed values, are report-only during
      calibration, and are promoted once **if every calibration run passes**. **Promotion is
      one-way.** The guard fails any hard → report-only change.
- **Timing and image-quality gates are statistical.** Their values are fixed now.

### 8.2 Validity (hard; failure → INVALID)

| ID | Condition |
|---|---|
| V1 | Contention CLEAN. The PSI recorder is reported, not gating, until calibrated. |
| V2 | Manifest digests match **the digests committed in the thresholds provenance**. Loaded counts equal the manifest. A mismatch quarantines the key; it is not deleted. |
| V3 | Tier matches device class; world ≥ 2.5 × VRAM (GPU-equivalent). `min_vram_ratio` is frozen. |
| V4 | Per-frame GPU-timing validity in the window, used **only for the R breakdown**. P1 uses the wall-clock interval, which is never missing. |
| V5 | All segments ran; path coverage 1.0; pose ≤ 1 mm at holds; `rows_written == frames_rendered`. |
| V6 | No throttling. |
| V7 | Effective budget, chunk cap, `lod_distance_multiplier`, drawn budget, τ, SH bands and codec params equal the frozen settings in every measured frame. |
| V8 | Binary identity: kind, optimize, `dev_build`, sha256, commit. |
| V9 | Not vacuous, **per arm**. Flat: unique uploaded ≥ 1.5 × VRAM and evictions ≥ 1,000. HLOD: unique uploaded ≥ 0.9 × the oracle's expected unique bytes **and** ≥ 1.5 × pool bytes, with ≥ the oracle-derived fraction of leaf nodes fetched. Both: peak resident ≤ 15 % of world; at every checkpoint, drawn ≥ 50 % of the manifest's expected count; in moving windows, a pre-registered per-segment drawn floor. |
| **V10** | Cold pass proven cold (D-14): mincore ≤ 1 % before, `read_bytes` ≥ 0.9 × expected stored bytes. `host_cache` label recorded. |

### 8.3 Gates

The settle condition is defined in §7.3.

| ID | Criterion | 3080 (A) | 3090 (AB) | Arm | Class |
|---|---|---|---|---|---|
| G1 | Exit 0, final marker, no DEVICE_LOST, OOM or allocation failure | 0 | 0 | both | correctness |
| G2a | Engine usage ≤ effective budget, every frame | 100 % | 100 % | both | correctness |
| G2b | External peak `memory.used` ≤ total − 512 MiB | yes | yes | both | correctness |
| G2c | Process VRAM delta | ≤ 8.0 GiB | ≤ 12.0 GiB | both | correctness (**BV-14 depends on BV-10**) |
| G3 | Ledger: reservations ≤ GS ledger, no unledgered resize | yes | yes | HLOD | correctness |
| G4 | Peak RSS | ≤ 12 GiB | ≤ 16 GiB | both | correctness |
| G5 | Cold start to first complete frame | ≤ 20 s | ≤ 15 s | both | **statistical** (labelled `host_cache`) |
| G6 | Settle after a stop | ≤ 3.0 s | ≤ 2.0 s | both | **statistical** |
| G6t | Settle after a teleport | ≤ 6.0 s | ≤ 4.0 s | both | **statistical** (labelled `host_cache`) |
| G7 | Needed data evicted during a hold | 0 | 0 | both | correctness |
| G8 | Bad-category frames after ready | 0 | 0 | both | correctness |
| G9 | Read, retirement, generation, invariant and decode (`gsq_*`) counters | 0 | 0 | both | correctness |
| G10 | Overflow drops at checkpoints | 0 | 0 | both | correctness |
| G10h | While moving, both (a) `overflowed_splats` (renderer overflow) and (b) `hlod_cut_visible_cap_bound_frames` (frames where the selector stopped at the cut cap **and** the visible cap was the minimum term of min(drawn, visible cap, pool records)). The selector enforces that min (`lod/hlod_runtime_selection.cpp:154-160`), so a working selector never overflows the renderer even when the cap binds. (a) alone therefore cannot show that the visible cap never constrained selection; (b) is the binding signal, published by BV-16c. | 0 / 0 | 0 / 0 | HLOD | correctness |
| G11 | Flat: PSNR / SSIM vs T-ref U | ≥ 40 dB / ≥ 0.98 | same | flat | statistical |
| G11h | HLOD coverage: no reference-covered pixel uncovered; no node drawn with its ancestor outside a fade pair; **static-frame flicker oracle = 0** at a binding budget, with `hlod_budget_bound_frames` > 0. **BV-21b depends on BV-25 (#1240)** | 0/0/0 | same | HLOD | correctness |
| G11q | HLOD per-band PSNR / SSIM vs the slab reference ≥ a **pre-registered absolute floor per band, minus the measured slab noise floor** | frozen in BV-26a | same | HLOD | statistical |
| R-rel | HLOD far-band vs flat far-band | — | — | both | report-only |
| **Q-codec** | Per band, C vs U: PSNR ≥ 40 dB, SSIM ≥ 0.99. On real scans with ground truth: ΔPSNR(C − U) ≥ −0.20 dB, ΔSSIM ≥ −0.005. C no worse than S by more than 0.1 dB. **D must FAIL** | same | same | codec | statistical; D-FAIL is correctness |
| G12 | Beacons found with the right id | 100 %, ≤ 4 px | same | both | correctness |
| G13 | C7 vs C0 PSNR | ≥ 45 dB | same | both | statistical |
| G14 | Frames > 250 ms outside 1 s after a teleport | 0 | 0 | both | statistical |
| G15 | Frames > 50 ms while moving | ≤ 1.0 % | ≤ 0.5 % | both | statistical |
| **P1** | **Wall-clock frame interval p95 / p99, time-weighted, over moving windows of pass A only** (holds and 1 s after each teleport excluded; those are covered by G6/G6t/G14) | **≤ 25 / ≤ 33.3 ms** | **≤ 16.7 / ≤ 20 ms** | **HLOD** gated; flat printed against the same values | statistical, promoted at BV-31 (3080) and BV-32 (3090) |
| P1-seg | Worst-segment p95 / p99 (S5, S7) | — | — | HLOD | report-only (pre-registered) |
| R | Pass breakdown, read, decode and upload latency, cut histograms, refetch, scheduler CPU, disk, cache state, records per splat per level, codec ratio | — | — | both | report-only |

**G2d** (live heap budget) is **not listed** until BV-12 lands (P6).

### 8.4 Thresholds file and the no-loosening guard

`tests/runtime/flythrough/flythrough_thresholds.json` freezes, per gate:

- `value`, `op`, `class`, `arm`, `tier`, `source_field`;
- **window definition**, **exclusions**, **percentile method** (time-weighted, nearest-rank).

It also freezes the following globals:

- `path_sha256`;
- `generator_params_sha`;
- `min_vram_ratio`;
- world digests (after the first prime);
- `settings`:
    - the four tuned settings;
    - **`drawn_budget_splats`, τ, SH bands;**
    - **codec params;**
    - **reference method** (slab count, noise-floor method).

`tests/ci/check_flythrough_thresholds.py` compares against the immutable base
(`GS_PR_BASE_SHA` / `GITHUB_BASE_REF`, then `origin/master`, never HEAD). It **fails** on any
of:

- a loosened value;
- a removed key, tier or gate;
- a changed `op`, `arm`, window, exclusion, percentile method or `source_field`;
- **any hard → statistical or report-only change**;
- `min_vram_ratio` lowered;
- `path_sha256` or `generator_params_sha` changed without an accompanying world re-prime
  **and** gate re-calibration record;
- non-finite values;
- an unresolvable base;
- **any `settings` edit outside the allowlist.**

The allowlist is a direction table: an edit may only *increase* work or fidelity:

- drawn budget ↑;
- τ ↓;
- SH bands farther;
- codec bits ↑;
- visible cap, far plane and chunk count are frozen (maintainer decisions).

An edit in the work-reducing direction is permitted only when G11q and Q-codec are **hard and
passing at the new value**, and a `waiver` line (maintainer, date, reason, evidence URL) is
present.

The **tightening rule** applies only to the non-user statistical gates (G5, G6, G6t, G14,
G15, G11, G13, G11q, Q-codec). It is **operator-aware**, keyed on each gate's frozen `op`, and
it can only move a threshold in the stricter direction:

- **Upper-bound gates** (`op` is `≤`: G5, G6, G6t, G14, G15):
  new = min(old, calibrated p95 × 1.15). The value never rises.
- **Lower-bound gates** (`op` is `≥`: G11, G13, G11q, and every Q-codec row, including the
  negative ΔPSNR/ΔSSIM floors): new = max(old, calibrated p5 − m). Here m ≥ 0 is a
  per-gate absolute margin in the gate's own unit (dB, SSIM). m is pre-registered in the
  thresholds file before calibration, and it is frozen like every other guarded field. The
  value never falls. A multiplicative margin is never applied to a floor, because on a
  negative delta it loosens. Example: −0.20 dB × 1.15 = −0.23 dB.
- **Proposals in the wrong direction:** if a calibrated proposal would move a threshold
  looser (min/max leaves `old`), it is recorded in the calibration record and not applied.
  The no-loosening guard rejects any thresholds diff in which an `≤` value rises or a `≥`
  value falls. This holds whatever produced the diff.

**P1 values are the
maintainer's targets and are never auto-tightened.**

**The guard, not the risk class, is the control**: the class itself is not a failure condition
(`docs/governance/agentic-engineering.md:184-185` ✓). The guard is wired into
`run_module_tests.py --guard-only`, which runs in the required `agentic-pr-gate`. On schedule
and dispatch the base equals HEAD, so the guard is vacuous there and is documented as
informational. Mutation proofs:

- raise a value;
- delete a key;
- demote a class;
- change an arm;
- edit `drawn_budget` down without a waiver;
- use a bogus base;
- delete the wiring.

Each must turn the guard red.

### 8.5 Controls: one forcing control per gate family

Every control asserts its exact tuple. Every *other* gate in the control process must be
evaluable (not INVALID). A harness stub that always exits 2 must yield run INVALID (unit test).

| Control | How forced | Expected |
|---|---|---|
| C-starve | T0, budget 256 MB | `G6 FAIL settle_deadline_missed` |
| C-off | streaming disabled | `V9 INVALID not_streaming` |
| C-blind | camera in a sealed box | `V9 INVALID drawn_below_floor` |
| C-lod | LOD bias ×8 | `G11 FAIL psnr_below` (flat); `G11q FAIL band_far` (HLOD) |
| C-drop | T0 variant with one chunk/node record removed from the expected set | `G6 FAIL missing_chunks` / `G11h FAIL uncovered_pixels` |
| C-shift / C-flip | T0 variant, tile +1 m / Y-flip | `G12 FAIL beacon_position` / `beacon_code` |
| C-read | T0 copy with a corrupted GSQ page | `G9 FAIL gsq_digest_mismatch` (read_errors > 0) |
| C-decode | T0 copy with valid digest but inverted ranges | `G9 FAIL gsq_range_invalid` |
| C-cap (flat) | visible cap 100k | `G10 FAIL overflow_at_checkpoint` |
| C-cap-h (HLOD) | visible cap 100k, below the frozen drawn budget | `G10h FAIL visible_cap_bound`. This proves signal (b). The selector caps the cut at 100k, so no renderer overflow is expected. |
| C-revisit | C7 rendered at P0 + 1 m | `G13 FAIL revisit_psnr` |
| C-settings | effective cap changed at runtime | `V7 INVALID settings_drifted` |
| C-warm | fadvise skipped | `V10 INVALID cache_warm` |
| C-codec-D | degraded codec arm | `Q-codec FAIL` (correctness) |
| C-thrash | BV-08a doctest at two frame rates | detector fires |
| G10h signal (a), renderer overflow on the HLOD arm | the renderer's capacity is forced below an **already-selected** cut, with no change to the selector's budget | release-build doctest mutation proof, as in the next row: `overflowed_splats` increments and the verdict module reports `G10h FAIL overflowed_splats` |
| G7, G8, G9 (retire/generation/invariant), G3, G2a | not forcible live without hooks in shipping code | **release-build doctest mutation proofs** (`dev_build=no tests=yes`): each counter is shown to increment under its injected fault, and the verdict module is shown to FAIL on a captured report with that counter non-zero. The PR records it. |

---

## 9. CI and evidence

### 9.1 Workflow `.github/workflows/beyond_vram_flythrough.yml` (R3)

- **Triggers:** `schedule "0 9 * * 0"` and `workflow_dispatch` (`tier`, `arm`,
  `prime_world_cache`, `controls_only`). No PR or push triggers.
- **Permissions:** `contents: read`, with `actions: read` **on the watchdog job only**. Fork
  guard. No `continue-on-error`.
- **Concurrency:**
    - `group: bvw-${{ github.event_name }}`, `cancel-in-progress: false`.
    - The real protection against master pushes is that **no other workflow uses this group**.
      The existing groups are per-workflow (`.github/workflows/gaussian_production_gates.yml:85`,
      `.github/workflows/release_builds.yml:151` ✓, …).
    - Per-event groups mean a dispatch can never replace a queued scheduled run. Pending
      dispatches can replace each other, which is documented.
    - Prime and flythrough are serialised on the cache by `flock`. The single runner runs one
      job at a time anyway.
- **Jobs:**
    1. `prime-world-cache` (dispatch only, 240 min):
        - sweep orphans;
        - delete the old key;
        - generate (plain GSQ, plus V4 GSQ via the bake);
        - verify, fadvise, write `.complete`.
    2. `flythrough` (150 min, per-step timeouts):
        - `check-tools` (fio optional);
        - contention preflight;
        - **driver limits logged** (`vulkaninfo`: `maxStorageBufferRange`, `maxBufferSize`);
        - build(s), `build_info` assert;
        - disk/cache preflight + verify + fadvise + mincore;
        - controls → main → codec arms;
        - postflight, verdict, upload (all `if: always()`);
        - exit mapping: PASS 0, FAIL 1, INVALID 2, NOT_RUN 3, VOID 75.
    3. `liveness-watchdog` (Mondays): before M2 it keys on "a terminal verdict that is not
       VOID"; from M2, on PASS within 9 days, two consecutive VOIDs, or a run queued > 20 h,
       cancelled or replaced.
- **BV-06a** is a minimal dispatch-only version (build, `df`, `dd` with `O_DIRECT`, vulkaninfo,
  generate, probe, upload) so that M1 does not wait for the full workflow. A manual VM run is
  rejected: no interactive VM access is assumed (UNVERIFIED), and runner work must be
  attributable to a workflow run.

### 9.2 Cache layout on the runner

```
/opt/actions-runner/godotgs-scons-cache-opt/   (cache_limit=15)
/opt/actions-runner/godotgs-world-cache/{CACHE_FORMAT,.lock}
   <world_id>/<gen_sha12>[-<bake_sha12>]/{world.*, world.manifest.json, .complete, .scratch/, .tmp/}
   quarantine/<key>/                            (moved here on V2 mismatch; deleted only by prime)
```

### 9.3 Evidence

- **Per-run artifact:** `verdict.json`, `summary.json` (`godotgs-beyond-vram-flythrough/2`,
  which adds `binary_kind`, `codec{id, params, ratio, decoded_digest_set}`,
  `cache{cold_proven, host_cache}`, `settings_frozen_sha`), the frame trace and its summary,
  `nvidia_smi.csv`, `contention.json`, `build_info.json`, PNG digests, and the thresholds
  sha256.
- **3090 bundle:**
    - It is **kept out of git**, in the workstation's evidence directory under
      `flythrough/<date>-<commit12>-rtx3090/`, or as a release asset.
    - Only `docs/evidence/flythrough-index.md` (R0) is committed, holding the bundle's
      `SHA256SUMS` digest and the summary JSON. No PNGs are committed
      (`tests/AGENTS.md:115-116` ✓).
    - **Signing:** a detached **GPG signature by the maintainer** over `SHA256SUMS`, which
      covers `summary.json` and every artifact.
    - `tests/runtime/flythrough/verify_bundle.py` checks the signature against a committed
      public-key fingerprint, then every hash.
- **Workstation precheck** (refuses to start, kills nothing):
    - no foreign `godot*` or `Runner.Worker`;
    - required localhost ports free;
    - **a GitHub API query shows no in-progress job on `gpu-linux-3080`**, because whether the
      VM shares the workstation's host is UNVERIFIED;
    - data-drive preflight.

---

## 10. Phased roadmap

**Conventions:**

- Effort is in engineer-days [E].
- **★ marks the critical path.**
- "Rev" is the review multiplier: R3 ×1.75 (two independent reviews, CODEOWNER, Codex rounds,
  human disposition), R2 ×1.4 (GPU evidence plus a gpu-performance reviewer), R0/R1 ×1.2.
- **Every R3 PR depends on an ADR:** BV-00a for `io/`, codec, cache and gates; BV-00b for HLOD
  at scale.
- Classes follow the design, never the cheaper glob. `tests/runtime/flythrough/**` is
  registered R3 by BV-01, so every lane and harness PR is R3.
- Issues are cited with "Refs".

### P0: decisions

| # | PR | Class | Deps | Eff. |
|---|---|---|---|---|
| ★BV-00a | ADR: writer API, GSQ codec + containers, world cache, cold-cache method, gates/guard semantics (§4, D-12–D-14, §8) | R0 | — | 2 |
| BV-00b | HLOD ADR §16 "Package G at scale": caps policy D-3, tiled bake, residency Steps A–C, frame model D-4 | R0 | — | 2 |
| ★BV-01 | Register `tests/runtime/flythrough/**`, the thresholds file, the guard and the verdict module as R3; ownership | R3 | BV-00a | 0.5 |

**BV-00a and BV-00b landed together in one PR:** this record carries the full content of
both, and HLOD ADR §16 points here for the BV-00b decisions.

### P1: uncompressed scale probe → **M1**

| # | PR | Class | Deps | Eff. | Survives? |
|---|---|---|---|---|---|
| ★BV-02 | `GSAtomicFileTransaction` (native handle, per-OS durability) + sliced SH write | R3 | 00a | 2–2.5 | yes |
| ★BV-03a | Writer `PLAIN_V3` core (both modes) + identity + negative controls | R3 | 02 | 3–4 | yes |
| BV-03b | Writer limits, preflight, cancel, progress, report | R3 | 03a | 2–3 | yes |
| BV-04a | Release-build streaming counters; effective-settings, regulator and LOD-multiplier telemetry | R2 | — | 2 | yes |
| BV-04b | GPU timing completeness, untimed-pass timestamps (F), records per splat per level | R2 | — | 2–3 | yes |
| ★BV-05 | Generator: T0, A, T-ref spec; exact content table; correlated SH; manifest; `generator_params_sha` | R3 by design | 03a | 4–5 | yes |
| ★BV-06a | Minimal dispatch workflow: opt build, `df`, `dd`, vulkaninfo, generate, probe, upload | R3 | 01 | 1.5–2 | yes (grows into 06b) |
| ★BV-07 | Probe lane: arc-length path, binary trace, pumps, F/k drawn sweep on T0 | R3 (flythrough) | 04a, 04b, 05 | 3–4 | yes |
| **M1** | **Dispatch: Tier A uncompressed (54.8 GiB) on the 3080**; report-only | | | runner ≈ 2 h | |

**M1 proves:**

- a world 3.3× VRAM streams on the shipping chunk route, in an optimized build, for the whole
  path;
- RSS stays bounded; real eviction;
- disk throughput measured;
- release-build latencies;
- the first > 2 GiB single atlas buffer (3.5 GiB) on GPU;
- **measured F and k**;
- whether the regulator fires (labelled `settings_drifted` if it does).

**M1 does not prove** targets, compression, far quality or HLOD.

### P1b: probe-proven blockers (budget 6–9 d)

| # | PR | Class | Eff. | Survives? |
|---|---|---|---|---|
| BV-08a | Thrash detector as a wall-clock refetch window, with two-frame-rate negative control | R2 | 1.5–2 | until Step C |
| BV-08b | `PayloadReadService` (positional reads, pool, pooled handles, cancellation) | R3 | 3–5 | yes |
| BV-08c | Bounded ready queue + upload clamp | R2 | 1–1.5 | yes |

### P2: compressed chunk world → **M1c**, then hard correctness gates → **M2**

| # | PR | Class | Deps | Eff. |
|---|---|---|---|---|
| ★BV-C1a | GSQ codec core: encode/decode one page, SoA/byte planes, sub-block zstd with per-worker DCtx, ranges, digests | R3 | 00a | 3–4 |
| BV-C1b | Codec hardening: fuzzed malformed pages, cross-platform golden digest, SPZ smallest-three oracle, error-bound tests | R3 | C1a | 1.5–2 |
| BV-C2 | Ratio + decode/pack throughput probe on real scans + generator subsets (replaces every [E] ratio) | R1 | C1a | 1–2 |
| ★BV-C3 | Writer codec option, W12–W16 | R3 | 03a, C1a | 2–3 |
| ★BV-C5a | Plain v3 bit 7 paged-GSQ chunks: reader + writer; old-reader refusal test | R3 | C3 | 3–4 |
| ★BV-C5b | Codec-aware staged source: decode in async pack workers → 176 B; async path asserted from telemetry | R2 | C5a, 08b | 2–3 |
| ★BV-C7 | Prime compressed Tier A (plain GSQ); delete the M1 file; manifest with codec | R3 by design | C5b, 05 | 1–1.5 |
| BV-08a | (must land before M1c) | | | |
| **M1c** | **First compressed beyond-VRAM run: Tier A plain GSQ (≈ 6–7 GiB) on the 3080**, report-only | | | |
| ★BV-09 | Thresholds file + full-semantics guard (§8.4) + pure verdict module + wiring + mutation proofs (fixture captured from M1) | R3 | 01, M1 | 3–4 |
| ★BV-10 | E3 demand-driven overlap bootstrap + old+new pre-flight; E4 accounting | R2 | — | 2–3 |
| BV-11 | `DeviceVramLedger` | R2 | — | 3–4 |
| ★BV-13a | Lane pass A complete + controls (§8.5) + release-build doctest proofs | R3 | 07, 09 | 3–4 |
| ★BV-13b | Pass B + settle oracle + T-ref per run + slab references + quality module | R3 | 13a | 3 |
| BV-C6 | Codec arms U/C/S/D + Q-codec | R3 | C5b, 13b | 2–3 |
| ★BV-06b | Full workflow: schedule, watchdog, cache layout, quarantine, cold-cache method (D-14), concurrency | R3 | 06a | 2–3 |
| ★BV-14 | Correctness gates hard on the flat arm | R3 | 06b, 10, 13b | 1 |
| **M2** | **Weekly lane, compressed Tier A, hard correctness gates on the flat arm** | | | |

### P3: HLOD at scale → **M3**

| # | PR | Class | Deps | Eff. |
|---|---|---|---|---|
| ★BV-15 | `NodeResidency` with no per-frame O(N); scaling test | R2 | — | 3–5 |
| ★BV-16a | Paged admission without splat caps; node bound from the ledger | R3 | 00b | 2–3 |
| BV-16b | Sort capacity from the cut; `materialize_paged_resident` refusal above the resident limit; multi-view refusal | R3 | 16a | 2–3 |
| ★BV-16c | **`drawn_budget_splats` setting**: settings manifest, cut cap = min(drawn, visible, pool records), scratch sized from it, the binding-term counter `hlod_cut_visible_cap_bound_frames` (G10h b), boundary tests | R3 (settings + cut) | 16a | 2–3 |
| ★BV-17 | Streamed pool = streaming budget within the ledger: replaces 512 MiB **and the 2 GiB staging ceiling** for the streamed page budget only; whole-bundle route unchanged; boundary tests at 2 GiB / 2 GiB + 1 page | R3 | 11, 16a | 2–3 |
| ★BV-18a | Writer `PAGED_V4` spill (compressed, with metrics) + BFS assembly with progressive deletion | R3 | C3 | 3 |
| ★BV-18b | W13 metrics recompute, kept V4 refusals, identity at ≤ 1M leaves | R3 | 18a | 2 |
| ★BV-C4 | V4 GSQ codec field: reader decode in `read_node_payload` + pooled handles; old-reader refusal | R3 | C1a, 18a | 3–4 |
| ★BV-19 | Offline-job overhead (atomic cancel) + per-worker contexts | R3 | 00b | 3–4 |
| ★BV-20a | Histogram, frontier, partition | R3 | 18a | 3 |
| ★BV-20b | Forced-root subtree bake | R3 | 20a, 19 | 3–4 |
| ★BV-20c | Stitch with leaf-range prefix reads | R3 | 20b | 3–4 |
| ★BV-20d | Assemble + identity (B_max 64k) + HLOD ADR §15.3 steps 1–2 failure controls | R3 | 20c, 18b | 3–4 |
| ★BV-25 | #1240 COUNT/EMIT hazard + static-frame oracle | R2 | — | 2–4 |
| ★BV-26a | Freeze `drawn_budget_splats` (D-4 rule, from M1/M1c F/k), τ, SH bands in `settings` **before any HLOD data** | R3 | 09, 16c, M1c | 0.5–1 |
| ★BV-21a | Prime job: V4 GSQ bake on the runner, `gen_sha + bake_sha` key, quarantine | R3 | 20d, C4, 06b | 2 |
| ★BV-21b | Lane HLOD arm; oracle cuts (A); G3, G10h, G11h hard; P1, G11q calibrating | R3 | 21a, 15, 17, 25, 26a | 2–3 |
| **M3** | **First HLOD-at-scale physical run: Tier A V4 GSQ (≈ 8–9 GiB, 201M SH3) on the 3080**, also run in GS-COMBINED-1 variant B once that gate exists | | | |

### P4: frame targets on both devices → **M4**

| # | PR | Class | Deps | Eff. |
|---|---|---|---|---|
| ★BV-22 | #539 parallel radix prefix | R2 | — | 3–5 |
| ★BV-23 | #1180 drop the redundant pre-sort | R2 | — | 3–5 |
| ★BV-24 | #1181 deduplicate the COUNT/EMIT projection | R2 | — | 3–5 |
| ★BV-24b | Present-gap and untimed fixed-cost investigation + fix (F) | R2 | 04b | 2–4 |
| BV-26b | SH read bands at runtime (HLOD route) | R2 | 21b | 2 |
| BV-C8 | 144 B SH layout for SH ≤ 3 (layout-sync) | R2 | — | 3–4 |
| ★BV-27 | Gated measurement binary per §12 item 1 (template build + export step in the workflow) | R3 | 06b | 1.5–2 |
| ★BV-30 | Tier AB V4 GSQ on the workstation: bake, oracle cuts (AB), bundle tooling, signing, verifier, precheck | R3 (evidence tooling) | 21a | 2–3 |
| ★BV-31 | 3080 calibration (5 dispatches, HLOD arm only; no flat-arm calibration) + promotion of P1, G5–G6t, G11, G11q, G13–G15 and Q-codec where they pass | R3 | 22–24b, 26b, 27 | 1 + runner |
| ★BV-32 | **3090 calibration (5 workstation runs on AB) + promotion** | R3 | 30, 31 | 1 + maintainer's time |
| **M4** | **Met only if P1 is hard and passing on the 3080 (A) and the 3090 (AB), on the HLOD arm**, with the same run also passing in GS-COMBINED-1 variant B ([One route, many demand producers](adr-one-route-many-producers.md) §9) once that gate exists. A world-only pass is reported but does not meet M4. Otherwise "not met", with the numbers. | | | |

BV-22 to BV-25 start on day 1 in parallel.

### P5: one residency system → **M5**

| # | PR | Class | Deps | Eff. |
|---|---|---|---|---|
| BV-33 | **Upstream RD limit `LIMIT_MAX_STORAGE_BUFFER_RANGE`** (`RenderingDeviceDriverVulkan::limit_get`, `drivers/vulkan/rendering_device_driver_vulkan.cpp:5865-5957`, lists no such limit, and the file never reads `maxStorageBufferRange` ✓) + Android/macOS compile evidence | R3 engine | 00b | 1.5–2 |
| BV-34a | `ResidencyManager` promoted from `NodeResidency` (device scope, render-thread owner) | R2 | 15, 17 | 4–5 |
| BV-34b | Segmented pool, K ≤ 4 fixed bindings; splat ref 3 SH + 2 segment + 27 index; segment in `DrawnNodeGPU.reserved`; fail closed below the RD limit | R3 (layout-sync) | 33, 34a | 3–5 |
| BV-35a | Legacy chunk visibility as a demand producer | R3 | 34a | 3 |
| BV-35b | Node-relative chunk payloads (HLOD ADR package C2, for chunks) | R3 | 35a | 3–4 |
| BV-35c | `DrawnNodeGPU` draw list for chunks (#1131) | R3 | 35b | 3 |
| BV-35d | Retire the chunk streamer's primary atlas and regulator | R3 | 35c | 3 |
| **M5** | **One residency system**; both arms on it; plain GSQ chunk regression on the runner | | | |

### P6: after M5 or on need

- AB-span 100 km and origin-shift guard removal (HLOD ADR Package C evidence).
- BV-12 `VK_EXT_memory_budget` + G2d.
- Release-gate artifact group (outside `required_groups`).
- Interior SH truncation at bake.
- The quantized async-stride defect.
- Compact GSQ-GPU layout.
- Importer GSQ output (§12 item 5).
- Issue: decorative importer compression flags.
- Multi-view sub-budgets for extra camera views. The light-view sub-budget that real splat
  shadow casters need is **not** deferred here: it is scheduled with the caster work before M3
  ([One route, many demand producers](adr-one-route-many-producers.md), decision 6).
- Lazy index pages.
- #1302.

**Issues to file** for deferred items:

- the decorative importer compression flags and the false editor memory estimate;
- the quantized async-stride defect;
- the `VK_EXT_memory_budget` follow-up to closed #321;
- multi-view HLOD sub-budgets.

### Effort, review and runner time

Effort per phase [D, sums of the rows above]:

| Phase | Engineer-days | With review factor |
|---|---|---|
| P0 | 4.5 | ≈ 6 |
| P1 | 20–25.5 | ≈ 32–40 |
| P1b | 5.5–8.5 | ≈ 8–13 |
| P2 | 33.5–44.5 | ≈ 54–72 |
| P3 | 43.5–57 | ≈ 72–95 |
| P4 | 24–36 | ≈ 36–55 |
| P5 | 21–26 | ≈ 35–44 |
| **Total** | **≈ 152–202 engineer-days** | **≈ 245–325 review-weighted days** |

**Runner hours [E]:**

| Item | Hours |
|---|---|
| ≈ 55 master merges × 1–1.5 h | 55–83 |
| ≈ 30 R2 GPU-evidence runs × ≈ 1 h | ≈ 30 |
| ≈ 8 primes × 1–2.5 h (the V4 bake is 45–90 min after BV-19) | 8–20 |
| 5 calibrations × 1.5 h | 7.5 |
| ≈ 25 weekly runs × 1.5 h | ≈ 38 |
| **Total** | **≈ 140–180 runner-hours** over the program |

Batch merges per phase. Stacked chains get GPU CI once at the end.

**Workstation:** 5 + ≈ 4 runs × ≈ 1.5 h of the maintainer's machine time, plus signing.

**Elapsed time.** One implementer per PR, in parallel. The binding constraint is a single human
approver at ≈ 3–4 R3 dispositions per week (≈ 45 R3 PRs) and one runner.

| Milestone | Elapsed |
|---|---|
| **M1** | ≈ 5–7 weeks |
| **M1c** | ≈ 8–10 weeks (the codec chain C1a → C3 → C5a → C5b runs in parallel with P1) |
| **M2** | ≈ 10–12 weeks |
| **M3** | ≈ 16–20 weeks (BV-18a → 20a–d → 21 serial, ≈ 22 d × 1.75) |
| **M4** | ≈ 20–25 weeks, met or not met |
| **M5** | ≈ 30–36 weeks |

---

## 11. Risks

| # | Risk | Mitigation |
|---|---|---|
| K1 | M1 finds chunk-route scale bugs | Fix only probe-proven ones, preferring shared layers; P1b is capped at 10 d, then re-plan. |
| K2 | Host cache makes cold runs fast | D-14 `host_cache` label; G5/G6t are statistical. |
| K3 | Codec ratio worse than estimated | The fixed-width upper bound still fits every disk budget (§5.3); BV-C2 measures before freezing. |
| K4 | Frame targets unreachable: F ≈ 10.5 ms, interior k ≈ 12 | D-4 rule; BV-24b; M4 "not met" path; targets never moved. |
| K5 | Tiled bake not identical in-core | Forced-root variant; identity at ≤ 1M with B_max 64k. |
| K6 | Bake cost | BV-19 first; the job manifest refuses rather than OOMs. |
| K7 | Sorter/pool resize transient | E3 (BV-10), I-6. |
| K8 | First > 2 GiB single buffer (3.5 GiB atlas) never run on GPU before M1 | vulkaninfo limits logged; M1 is the test; BV-33/34b fallback. |
| K9 | Codec decode CPU binds at 120 m/s | Per-worker DCtx; decode straight to 176 B; pack worker count is a frozen setting; measured in BV-C2. |
| K10 | Encoder non-determinism (fast-math, MSVC vs GCC) | Integer-exact quantization; golden digest on both OSes; `-ffp-contract=off`-safe code. |
| K11 | System-zstd builds lack symbols, or the ABI differs | UNVERIFIED; builtin zstd for our builds; a link test in BV-C1a. |
| K12 | Step C touches the alpha envelope | Last phase; compressed plain regression on the runner throughout. |
| K13 | Synthetic quality does not transfer | HLOD ADR §14.8 separation; real-scan codec arms; foliage class. |
| K14 | Single human approver and single runner | The elapsed figures already include them; batch merges. |
| K15 | The program drifts into infrastructure | Every PR names its Package E/G or issue link; P4 fixes help every alpha scene; the codec is a shipping requirement. |

---

## 12. Decisions put to the maintainer

These are the questions this design put to the maintainer, with the recommendation given at
the time. The answers are recorded in "User decisions recorded 2026-11-03" below and are
binding. Everything else is decided above with reasons: the codec scope (D-12), the bit depths
(frozen, pre-registered and measured), the plain route (D-12: bit 7), the codec thresholds
(Q-codec values pre-registered), uncompressed Tier A only for M1 (D-13), the 144 B layout in P4
with the compact layout in P6, cold-cache semantics (D-14), and the regulator fix (D-6).
**Disk:** no VM growth and no cache increase are needed.

1. **Which binary the gated frame-time numbers come from.**
    - Recommendation: **`template_release optimize=speed_trace dev_build=no`** running an
      exported pack, from BV-27, before any P1 calibration. The editor+tests binary stays the
      producer and the early probe binary, labelled as such.
    - Cost: a second build (≈ 25–45 min cold) and an export step per run.
    - Alternative: gate the editor binary and disclose that the numbers are editor-target
      numbers.
2. **The gated workload is an HLOD drawn budget, not the 4M visible cap.**
    - At 4M drawn, the model predicts both targets missed by about 2–3×.
    - Proposal: the 4M cap becomes a never-binding ceiling (hard gate), and P1 is gated at
      `drawn_budget_splats` = the largest N ≤ 1.0M that the model, fed with M1-measured F and
      k, predicts meets 3090 p95 ≤ 16.7 ms. The value is frozen before any HLOD data.
    - If that N < 0.5M, the program stops and asks the maintainer again. The options then
      would be per-device budgets or accepting a 3090 miss; the targets themselves are never
      changed by the program.
    - **Recommendation: approve.**
3. **The frame targets gate the HLOD arm only.** The flat chunk arm is printed against the same
   numbers for information.
    - Recommendation: **yes**. The flat arm cannot meet them by construction: prefix LOD,
      holes, up to 4M drawn.
4. **3090 runs on the maintainer's workstation.** This means:
    - about 9 runs × ≈ 1.5 h (5 calibration plus about one per milestone);
    - ≈ 40–60 GB of the 93 GB free on the workstation's data drive;
    - the maintainer's GPG key to sign each bundle (§9.3).
    - Recommendation: **yes**. Without it the 3090 targets remain unevidenced and M4 can be met
      only for the 3080.
5. **Shipping path for real content.** Should PLY/SPZ imports write GSQ worlds?
    - Doing so bumps the importer format versions and forces a re-import of every project.
    - Recommendation: **an opt-in importer option in P6, made default only after Q-codec is
      hard and passing on real scans**.
    - Until then, shipped large worlds come from the `io/` writer or converter path.
6. **Maintainer disposition for the HLOD cap replacements** (D-3): the streamed 512 MiB term,
   the streamed 2 GiB staging term, paged leaf/payload caps, node bound, sort sizing, refusal
   above the resident limit.
    - Recommendation: **approve the policy once, here**, so that BV-16a/b/c and BV-17 carry a
      routine disposition line. Device-wide limits (0.60 ledger, 1 GiB CPU) and the
      resident-route caps stay unchanged.

Release-gate integration moves to M4 and is not a decision now.

## User decisions recorded 2026-11-03

The user (maintainer) answered the §12 questions on 2026-11-03. These decisions are binding:

1. **Gated frame times** come from `template_release optimize=speed_trace` running an exported
   pack, from the corresponding PR onward. The editor+tests binary remains the producer and
   early-probe binary.
2. **Workload.** Approved as proposed. The frame targets gate the HLOD arm at a drawn-splat
   budget N. N is the largest value ≤ 1.0M that the model, fed with M1 measurements, predicts
   will meet 3090 p95. If N < 0.5M, the user is asked again. The 4M cap is a ceiling that must
   never bind. The flat chunk arm is report-only against the same targets.
3. **3090 workstation runs.** Approved: about 9 runs of about 1.5 h, 40–60 GB on the
   workstation's data drive, GPG-signed bundles. 3090 Tier AB is calibrated and gated at M4.
4. **HLOD pilot caps.** One-time approval to replace them with ledger-derived budgets: the
   512 MiB and 2 GiB streamed-pool terms, the paged leaf and payload caps, the node bound, and
   sort sizing. They are still refused at the limit and boundary-tested, one PR each. The
   2 GiB ceiling stays on the resident route.
5. **Importer.** Accepted by default: GSQ output is opt-in first. It becomes the default only
   once the codec quality gate is hard and passing on real scans.

**Note (2026-11-04).** The maintainer decided that the world, static and movable objects,
interactive objects, lights and shadows must work in the same frame, and accepted
[One route, many demand producers](adr-one-route-many-producers.md). This record was amended
to match, with minimal edits in eight places: the §1.3 non-goal (node instances removed from it;
multi-package worlds stay out of scope), the D-1 producer set, the §3.1 producer line and
`DrawnNodeGPU.instance_id`, the D-11 ledger scope, the multi-view refusal (D-3, as an interim) together
with the P6 light-view sub-budget, invariant I-4 (per producer), invariant I-10 (tested with
≥ 2 producers), and M3/M4, which are also run in GS-COMBINED-1 variant B. **No threshold or gate in §8 changed.**

---

## Review history

This design went through two independent reviews before it was recorded here: a technical
review (2 P1, 11 P2, 12 P3 findings) and a plan review (8 P1, 22 P2, 12 P3 findings). Every
P1 and P2 finding is dispositioned into the decisions, gates and roadmap above. One plan
finding was only partly accepted: the minimal dispatch workflow (BV-06a) brings M1 forward,
but a manual VM run was rejected for the reason given in §9.1.

## External prior art

- Niantic SPZ (format, quantizers, S control arm): https://github.com/nianticlabs/spz
- 3DGS compression survey: https://w-m.github.io/3dgs-compression-survey/ and
  https://arxiv.org/abs/2407.09510
- PlayCanvas SOG format: https://developer.playcanvas.com/user-manual/gaussian-splatting/formats/sog/
  and https://blog.playcanvas.com/compressing-gaussian-splats/
- https://arxiv.org/abs/2312.13299 and https://arxiv.org/abs/2501.12255
- Khronos `KHR_gaussian_splatting`:
  https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_gaussian_splatting
- zstd (decode-speed table): https://github.com/facebook/zstd
- Kerbl et al. 2024, hierarchical 3D Gaussians for very large datasets:
  https://repo-sam.inria.fr/fungraph/hierarchical-3d-gaussians/
- Octree-GS: https://city-super.github.io/octree-gs/
- Nanite (Karis, SIGGRAPH 2021 Advances): https://advances.realtimerendering.com/s2021/

---

## Appendix A: citation checks at `44836e1d6a5`

Every ✓ above was re-read in a checkout at `44836e1d6a5`. The load-bearing ones:

| Claim | Verified at | Result |
|---|---|---|
| HLOD pilot caps 1,048,576 / 1,572,864 / 32,768 / 1 GiB / 512 MiB | `core/gaussian_splat_hlod_runtime_data.h:14-18` | confirmed |
| Paged admission applies leaf/payload/node caps but charges no payload | `world.cpp:1191-1194`, `:1200-1201` | confirmed |
| Resident capture applies the same caps | `world.cpp:1290-1294` | confirmed |
| 2 GiB staging ceiling clamps `atlas_limit`, and only lowers it | `renderer/resident_atlas_budget.h:36`, `:48-63`; `publisher.cpp:1714-1716` | confirmed |
| Streamed page budget takes atlas, HLOD-owned cap and ledger terms | `renderer/resident_atlas_budget.h:118-120`; called at `publisher.cpp:1779-1781` | confirmed |
| Cut splat budget = min(leaf_count, `max_splats`), streamed further clamped by atlas records | `publisher.cpp:1657-1658`, `:2028-2038` | confirmed |
| `GPU_BYTE_LIMIT` also used for whole-bundle admission | `publisher.cpp:1714`, `:1741`, `:1800` | confirmed |
| Sort capacity from whole-tree `leaf_count` | `publisher.cpp:1685` | confirmed |
| `TESTS_ENABLED` hooks in the publication path | `publisher.cpp:1702`, `:1725` | confirmed |
| V4 writer refusals `center_reconstruction_not_exact`, `metrics_disagree_with_snapshot` | `paged.cpp:983-1008` | confirmed |
| V4 record size/placement, layout, reserved-zero, length checks | `paged.cpp:349-351`, `:579-582`, `:617-626`, `:751-753`, `:766-775` | confirmed |
| `content_generation` hashes every body | `paged.cpp:971-975` | confirmed |
| Tree validator: parent precedes child; contiguous child range | `core/gaussian_splat_hlod_tree.cpp:109-110`, `:170-171` | confirmed |
| In-core bake emits BFS | `core/gaussian_splat_hlod_bake.cpp:549-564` | confirmed |
| Reader bounds `kMaxNodes` = 2^20, `kMaxHighOrder` = 21, `kMaxNodeSplats` = 16,384 | `io/gaussian_splat_world_paged.h:57-58`; `core/gaussian_splat_hlod_tree.h:45` | confirmed |
| Godot zstd decompress serialised by a global mutex and one DCtx | repo root `core/io/compression.cpp:45-48`, `:200-217` | confirmed |
| Module already has the zstd include path (SPZ environment) | `SCsub:20-21` | confirmed |
| `PackedSphericalHarmonics` has 24 encoded words; `PackedGaussian` is 176 B | `renderer/gaussian_gpu_layout.h:312-319`, `:340` | confirmed |
| `DrawnNodeGPU` is 32 B with a `reserved` word | `renderer/gaussian_gpu_layout.h:269-278` | confirmed |
| Residency and streamer state are render-thread owned | `lod/hlod_node_residency.h:35`; `renderer/hlod_node_streamer.h:20-21` | confirmed |
| `NodeResidency` per-frame O(N) scans | `lod/hlod_node_residency.cpp:285-290`, `:543-565`, `:179-196` | confirmed |
| HLOD scratch sized from `max_visible_splats`; SH cache indexed per atlas record | `renderer/tile_renderer.cpp:4074-4114` | confirmed |
| RD has no storage-buffer-range limit | `drivers/vulkan/rendering_device_driver_vulkan.cpp:5865-5957`; no `maxStorageBufferRange` in the file | confirmed |
| Release builds stub `PackTelemetry` | `core/streaming_upload_pipeline.h:90`, `:222` | confirmed |
| `upload_stalled` also requires `loaded_this_frame == 0` | `core/streaming_diagnostics_surface.cpp:530-533` | confirmed |
| Only the broad forced-sync guard reads `last_upload_chunks` | `core/gaussian_streaming.cpp:4571-4576`, `:4580-4585` | confirmed |
| Quantized route forced to sync; deferred for file-backed worlds | `core/gaussian_streaming.cpp:4597-4605`, `:1794-1797`, `:2106-2108` | confirmed |
| Regulator thrash ratchet, 30-frame spacing | `core/streaming_vram_regulator.cpp:348`, `:359-372`, `:416-429`; `.h:99` | confirmed |
| `materialize_paged_resident` warn-and-load fallback | `nodes/gaussian_splat_world_3d.cpp:664-674` | confirmed |
| Plain flags 0–5, HLOD bit 6, unknown flags refused | `io/gaussian_splat_world_format.h:15-16`, `:34-35` | confirmed |
| Whole-section gzip flag, resident-only, `INT32_MAX` bound | `io/gaussian_splat_world_io.cpp:58`, `:65-81`, `:340-352`, `:1573` | confirmed |
| Per-file SH count cap | `io/gaussian_splat_world_io.cpp:1617-1624` | confirmed |
| Importer HLOD-bakes non-empty plain worlds | `io/resource_importer_gsplatworld.cpp:431` | confirmed |
| Native atomic replace | `io/gs_atomic_file_writer.cpp:12-17`, `:101-125` | confirmed |
| Second live world submission rejected | `core/gaussian_splat_scene_director.cpp:2441-2449` | confirmed |
| SPZ: one frame per stream; absolute 12-bit-fraction positions | `thirdparty/spz/load-spz.cc:111-121`, `:351-357` | confirmed |
| No existing workflow uses a shared GPU concurrency group | `grep -A1 '^concurrency:' .github/workflows/*.yml` | confirmed |

**Still UNVERIFIED:** all codec ratios and decode speeds (BV-C2 measures them), the
frame-time split beyond sort and total frame (D-4), runner `df` and disk throughput, host
caching of the VM disk, 3080 Vulkan limits, the present-gap cause, records per splat at
benchmark density, system-zstd linking, and template binaries loading absolute-path worlds.
