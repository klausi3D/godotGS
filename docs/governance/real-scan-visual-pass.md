# Real-Scan Visual Pass

The procedure, evidence-bundle format, location and signer field for the human visual
pass that the [release acceptance bar](release-acceptance-bar.md) makes a gate for the
public alpha and for v1.0 (§6, §9, §10). Tracked as
[#1012](https://github.com/klausi3D/godotGS/issues/1012).

> **Status: not yet executed.** The policy decisions this page needed were made on
> 2026-09-27 by the coordinator, on the maintainer's delegation. They are recorded in
> [Decisions](#decisions) at the end. One item is still open: **C7 (streaming) is
> blocked on [#1075](https://github.com/klausi3D/godotGS/issues/1075).** Until the
> corridor world runs, no pass can reach `ACCEPT`, and a pass run before then is
> evidence, not a sign-off.

## What the pass is, and what it is not

Synthetic CI fixtures (2k to 30k splats) keep their regression role, but they do not
support the claim that the product looks right. GPU-001 was absent under production
defaults while every counter reported success, and a QA pin masked it (#903, #921). The
pass puts **the maintainer's eyes on real-scan content, rendered by the exact bytes a user
downloads**, across the alpha envelope (bar §10.1).

- It is a **human judgement**. Bar §9 lists it under "human-signed", not machine-checked.
  No green lane, script exit code or agent assertion can stand in for the signature.
- It does **not** replace automated pixel coverage. An FSR2 row in
  `qa_composite_production_defaults.gd` is separate work. Neither substitutes for the
  other.
- It does **not** feed the candidate gate today. The gate's ten artifact groups
  (bar §9.1) do not include this bundle, so a missing or failed pass does not stop a
  tag mechanically. Holding the release for it is a human obligation.

## Prerequisites (recorded, not assumed)

Every item below is **written into the bundle**. A value that is not written down counts
as not controlled.

1. **The candidate comes from the release, not a local build.** Download the Windows
   editor archive, the export-template archive and their `.sha256` sidecars from the
   candidate release, and check each archive against its sidecar. Record:
   - the `sha256` of each archive;
   - the `commit=` line of the editor archive's own `BUILD-INFO.txt`, which is packed
     into the zip (`.github/workflows/release_builds.yml:775-792`);
   - the commit the release tag points to (`git rev-list -n 1 <tag>`).

   The editor commit and the tag commit must match.

   **The export template carries no commit record of its own.** The template zip holds
   only the two executables (`release_builds.yml:979`), and its
   `EXPORT-TEMPLATE-INFO.txt` is not a release asset (`:962-979`, and the publish list
   at `:1718-1725`). Its commit is bound by the publish step instead. That step refuses
   a template whose info commit differs from the commit being published (`:1449-1457`).
   So record the template's `sha256`, checked against its sidecar, together with the
   tag commit. Do not look for a template `BUILD-INFO.txt`, because none exists.

   A pass run on a local build says nothing about the bytes a user downloads.

   **The current release workflow publishes before any pass can run.** Every `v*` tag
   maps to a stable, non-prerelease publish (`release_builds.yml:183-188`), and the
   release is created with `draft: false` (`:1700`). There is no hold point between the
   build and the publish. So until a draft or candidate promotion step exists (#1085),
   the pass runs **before tagging**, on the workflow artifacts of a `workflow_dispatch`
   or nightly run of the **exact candidate commit**. The tag build is then a rebuild of
   the same commit, not the same bytes. The bundle records the pre-tag run id and its
   artifact hashes, and the proof boundary says that the signed bytes and the published
   bytes are two builds of one commit.
2. **Real-scan assets, referenced by hash and never committed.** For each source `.ply`,
   record its file name, `sha256`, splat count, and where a reviewer can obtain it:

   | Role | File | Splats | SHA-256 |
   | --- | --- | --- | --- |
   | Primary: at least 1M splats, so that the dialog route's import cap and the node cap bind | `baum-mit-wiese2.ply` (1,984,001,532 bytes) | 8,000,000 | `75eb67b5fa25bc957cb3ecafecf365de3acd8de4b3dc84f3993fa3ea8c197908` |
   | Small-scene control | `holzbank-clean.ply` (62,578,379 bytes) | 252,326 | `86416da54b70832b0e474ff61175d1df30f8726267da25d58762179eb35b40e8` |

   Both come from the maintainer's GrandmasHouse capture library (`assets/ply/`).
   The coordinator named `holzbank-clean.ply` as an own capture, on the maintainer's
   behalf. `baum-mit-wiese2.ply` is the largest standard 3DGS scan in that library (62
   properties per splat, SH degree 3). It is a **maintainer-confirmed own scan,
   2026-09-27**, cleared to be referenced as the release asset. The splat counts are the
   PLY headers'
   `element vertex` values. The hashes were taken on 2026-09-27.

   The bundle gives the obtain-from location. The assets themselves stay outside the
   repository.
3. **Both import routes, stated.** Every node-route configuration runs **once per import
   route**, and the bundle labels each run, for example `C1-auto` and `C1-dialog`. The
   defaults depend on the route:
   - **Automatic import** (Godot's import on file drop) uses preset index 0, which is
     `ultra` (`modules/gaussian_splatting/io/gaussian_import_preset.cpp:8-19`):
     `max_splats = 0`, `density_multiplier = 1.0`, so no import cap applies.
   - **The import dialog** preselects the `desktop` preset
     (`modules/gaussian_splatting/editor/gaussian_import_dialog.cpp:973-976`):
     `max_splats = 750000`, `density_multiplier = 0.7`.
   - A `GaussianSplatNode3D` at its Balanced quality preset caps `max_splat_count` at
     500,000 (`modules/gaussian_splatting/nodes/gaussian_splat_node_helpers.cpp:1680-1682`).

   **On the automatic route, the node cap does not bind.** An asset imported with
   `max_splats = 0` and `density_multiplier` of 1.0 or more is treated as requesting
   full fidelity (`renderer/gaussian_splat_renderer.cpp:127-134`), and
   `build_runtime_fidelity_policy()` then budgets the asset's whole count instead of the
   node's `max_splat_count` (`:1617-1622`). So every `*-auto` run of the primary asset
   is budgeted at all 8,000,000 splats, not 500,000. The signer should read memory and
   overflow behaviour on those runs with that in mind.

   For each run, record the preset id, `max_splats`, `density_multiplier`, the imported
   splat count, the node quality preset, the **effective runtime budget**, and the
   **visible splat count the renderer reported**.
4. **Machine.** Record the OS and build, the GPU model, the driver version, the rendering
   driver and API version, and the output resolution, which is 1920×1080. **One Windows
   machine with one NVIDIA GPU is enough for the alpha.** That is a single-vendor blind
   spot, and the bundle's proof boundary has to say so (see below).
5. **Shipped composite defaults.** Every configuration runs at
   `rendering/gaussian_splatting/composite/depth_test = true`, the registered default
   (`modules/gaussian_splatting/core/gaussian_splat_manager.cpp:998`). Record any project
   setting that differs from its registered default. A difference that is not recorded
   invalidates the run.

## Configurations

All configurations use Forward+ and a single view (bar §10.1).

| # | Route | Configuration | What it checks |
| --- | --- | --- | --- |
| C1 | Node | Scale 1.0, no upscaler, no TAA | The baseline appearance claim. |
| C2 | Node | Bilinear, scale 0.75 | The GPU-001 defect configuration (#921). |
| C3 | Node | FSR2, scale 1.0, a 28-frame burst with the camera moving | The bar's required FSR2 motion-ghosting check (§6). |
| C4 | Node | TAA on, scale 1.0, a 28-frame burst with the camera moving | The second temporal consumer. |
| C5 | Node | An opaque mesh standing inside the splat cloud | Depth interleaving and the composite occlusion claim. |
| C6 | World | `GaussianSplatWorld3D` with a resident payload, scale 1.0, and one live swap from payload A to a different payload B during the run | The world route, which is in the alpha envelope (§10.1). |
| C7 | Streaming | `GaussianSplatWorld3D` streaming the open-world corridor world (`open_world_corridor_20m`), with a camera path that crosses chunk boundaries so chunks load **and evict** | Streaming open worlds, which stay in the alpha envelope. **Blocked on #1075.** |
| C8 | Node | C1's camera with the exhibition recipe | The bar's required exhibition-recipe check (§6). |
| C9 | Multi-node | **v1.0 only.** Several `GaussianSplatNode3D` instances of the primary and control assets in one scene, with overlapping screen coverage and one opaque mesh between them | The multi-node scope that the v1.0 envelope adds (bar §5). It is not run for an alpha pass. |

Notes on individual rows:

- **C6 and C7 differ by payload, not by node.** `rendering/gaussian_splatting/streaming/route_policy`
  defaults to `1` (streaming) and applies to world submissions only
  (`gaussian_splat_manager.cpp:1005-1008`). A direct `GaussianSplatNode3D` always
  registers as resident. A world payload streams only if it is an uncompressed
  `.gsplatworld` (see [streaming](../features/streaming.md#source-residency-model)).
  Record `route_policy`, and for both rows record `payload_mode` and
  `payload_streamable` from `GaussianSplatRenderer.get_render_stats()`.
- **C6 has to prove the swap happened.** Payloads A and B must have different hashes,
  and both are recorded. Capture at least one frame **before** and one **after** the
  swap, each with its pose-matched control. **The before and after captures share one
  camera pose**: the camera stays still across the swap, and the bundle records the pose
  once for both. The splat region must differ between the before and after captures.
  Otherwise a camera move could supply the difference while payload A is still on
  screen. A bundle with no post-swap capture, or with A and B
  identical, fails the run.
- **C7 has to prove that it streamed and evicted.** The run fails, as a run, unless all
  of these hold:
  - `payload_streamable` is true;
  - `gaussian_splatting/streaming_chunks_loaded_this_frame`
    (`modules/gaussian_splatting/core/performance_monitors.cpp:336-337`) is non-zero on
    at least one frame during the path, which shows loads directly rather than
    inferring them;
  - `gaussian_splatting/streaming_chunks_evicted_this_frame` (`:338-339`) is non-zero on
    at least one frame.

  Record the per-frame series of both monitors, and of
  `gaussian_splatting/streaming_loaded_chunks`. A streaming run that quietly ran
  resident, never loaded, or never evicted, has not exercised what the Streaming dimension judges.
- **C7 content and its interim.** C7 uses the corridor world as soon as
  [#1075](https://github.com/klausi3D/godotGS/issues/1075) makes it run. Until then, C7
  may be captured on the `lightweight_smoke` streaming content only as a labelled
  **interim**, `C7-interim-lightweight-smoke`. It does not substitute for C7: while C7
  has only its interim capture, the Streaming dimension's verdict is `BLOCKED #1075`,
  and the pass cannot reach `ACCEPT`.
- **C3 and C4 have to prove that the temporal stage was engaged, and the proof has to
  discriminate.** Moving-camera GS output is very nearly, but not exactly,
  frame-deterministic (see #1025 on
  [Known Public Alpha Limitations](../development/known-public-alpha-limitations.md)).
  So "some pixel differs" is not enough. Follow the #1025 measurement:
  1. Replay the camera path **twice** with no temporal stage, and record the maximum
     per-pixel difference between the two replays as the **noise floor**.
  2. Choose an **in-frame control region**: a static mesh area with no splats in it.
  3. The temporal run must move that control region, relative to the no-temporal
     replay, by more than the noise floor. Otherwise the viewport may have silently
     refused FSR2 or TAA, and the run fails.

  Record the noise floor, the region, and the measured difference.
- **C8, the exhibition recipe.** The WorldEnvironment uses the **AgX** tonemapper
  (`tonemap_mode = 4`, `TONE_MAPPER_AGX`) with `tonemap_exposure = 0.354`, which is 2^−1.5, that is
  −1.5 stops. This is the recipe the #921 packet recorded. It is set on the
  `Environment` resource, not through camera attributes or GS colour grading. Record
  both values.

### Controls: pose-matched, one per judged frame

Every judged capture has a **splats-hidden control frame from the same camera pose**:

- for a still configuration, one control;
- for a motion burst (C3, C4, and the C7 path), one control per frame, captured by
  replaying the same path with splats hidden and paired by frame index;
- for C6, one control for the pre-swap capture and one for the post-swap capture.

If a capture is bit-identical to its paired control, that capture saw no splats. That
is a **failed run**, not a passed one ([evidence integrity](evidence-integrity.md)).
Comparing a frame to a control from a different pose proves nothing, because the
background differs anyway.

## What the signer judges

| Dimension | A failure |
| --- | --- |
| Presence | In any configuration, splats are absent, or the frame shows only background while the counters report success. **This is an automatic fail**: the alpha "may not ship with splats missing under its own default settings" (bar §10). |
| Appearance | Blown highlights, crushed blacks, or a colour cast that the source capture does not have. A visible sRGB/linear double decode (the shape of #930). |
| Depth interleaving (C5) | Splats draw over the occluder, or a halo or gap appears at the silhouette. |
| Temporal (C3, C4) | Trailing, smearing or detail dissolving across the 28 frames. Splats swimming against static meshes. |
| World route (C6) | Content does not update after the payload swap (the shape of #862), or the wrong content is shown. |
| Streaming (C7) | Holes where chunks should be, chunks that pop in or out in view, stale content after an eviction, or a scene that never becomes visually ready (the shape of #786). |
| Exhibition recipe (C8) | Splats and meshes graded differently, rather than as one consistent look. |
| Multi-node (C9, v1.0 only) | A node missing, nodes sorting or occluding each other wrongly where their coverage overlaps, or one node's content drawn with another's transform. |
| Discrimination | Any capture that matches its paired control, or any engagement proof above that did not hold. |

**Verdicts per dimension:** `PASS`, `FAIL`, `KNOWN #N` or `BLOCKED #N`.

**`KNOWN #N` is available only for an issue that has been admitted as an accepted
limitation.** That means it appears in `public_alpha_issue_ledger` in
`docs/reference/renderer_release_gate_manifest.json` with
`status: accepted_alpha_limitation`. On 2026-09-27 that is #1025, the ≈1.5 px TAA trail in
C4, and #54. Being listed on the known-limitations page is **not** enough. That page
says itself that most of its entries are disclosures that have not cleared admission.

If a pass reproduces the symptom of any other listed entry, the dimension is `FAIL`, and
the note names the entry. If the entry says it was not reproduced, the signer also posts
the reproduction on its issue. A reproduction changes the evidence that kept that entry
out of the blocker set.

**A dimension the captures cannot answer is a failure of the procedure, not a pass of
the product.** Recapture it. Do not waive it.

## The evidence bundle

### Location and storage

One directory per pass, at the **repository root**, outside the published docs:

```text
evidence/visual/<YYYY-MM-DD>-<commit12>/
├── EVIDENCE_README.md
├── summary.json
├── SIGNOFF.md
└── captures/
    ├── C1-auto_capture.png
    ├── C1-auto_control.png
    ├── C3-auto_motion_00.png … C3-auto_motion_27.png
    ├── C3-auto_control_00.png … C3-auto_control_27.png
    └── …
```

- **This path is not published.** The docs site is staged from `docs/` only: the
  `--source` default is `docs` (`scripts/stage_public_docs.py:252`), and MkDocs builds
  from the staged copy (`mkdocs.yml:6`, `docs_dir: .site/public-docs`).
- **Committed images are reduced, lossless PNGs** with a long edge of 1280 px or less.
  The **full-resolution set** is attached to the candidate's GitHub release as one
  archive, for example `realscan-visual-pass-<commit12>.zip`, with its own `sha256`.
  `summary.json` records the SHA-256 of **every full-resolution image and every reduced
  image**. So the committed bundle binds the release attachment image by image, and
  someone who downloads the attachment can verify it.
- **There is no LFS change.** `.gitattributes` keeps LFS for `*.mp4` and `*.webm` only.
- Do not name files `*.actual.png`. The release gate forbids tracked files of that name
  under `tests/visual_baselines/`, and the name suggests a baseline comparison, which
  this is not.

### `summary.json`

One JSON object. Every field is required. `null` is allowed only where the note says so,
and every `null` has to be explained in `EVIDENCE_README.md`.

```json
{
  "schema": "godotgs-realscan-visual-pass/2",
  "operator": "<who ran the captures>",
  "signer": "<the maintainer>",
  "candidate": {
    "stage": "public-alpha | v1.0",
    "release_tag": "<v*-alpha* for the alpha; v1.* for v1.0>",
    "tag_commit": "<40-hex, git rev-list -n 1 <tag>>",
    "editor_build_info_commit": "<40-hex from the editor zip's BUILD-INFO.txt; must equal tag_commit>",
    "editor_archive": {"name": "…", "sha256": "…", "sidecar_verified": true},
    "export_template_archive": {"name": "…", "sha256": "…", "sidecar_verified": true}
  },
  "full_resolution_attachment": {"release_asset": "…zip", "sha256": "…"},
  "machine": {
    "os": "…", "gpu": "…", "driver": "…",
    "rendering_driver": "vulkan", "api_version": "…",
    "resolution": [1920, 1080]
  },
  "assets": [
    {
      "id": "A1", "file_name": "baum-mit-wiese2.ply",
      "sha256": "75eb67b5fa25bc957cb3ecafecf365de3acd8de4b3dc84f3993fa3ea8c197908",
      "source_splat_count": 8000000, "obtain_from": "…",
      "imports": [
        {"route": "automatic", "preset": "ultra", "max_splats": 0, "density_multiplier": 1.0, "imported_splat_count": 0},
        {"route": "dialog", "preset": "desktop", "max_splats": 750000, "density_multiplier": 0.7, "imported_splat_count": 0}
      ]
    }
  ],
  "world_payloads": [
    {
      "id": "W1", "file_name": "….gsplatworld", "sha256": "…",
      "derived_from": "<asset id or fixture id>", "derivation": "<tool, version and settings>",
      "compressed": false, "chunk_count": 0
    }
  ],
  "non_default_project_settings": {"<key>": "<value>"},
  "configurations": [
    {
      "id": "C1-auto", "route": "node | world | streaming | multi_node",
      "asset": "A1", "import_route": "automatic", "world_payloads": [],
      "node_quality_preset": "balanced", "max_splat_count": 500000,
      "runtime_budget_splats": 8000000,
      "scaling_3d_mode": "off | bilinear | fsr2", "scaling_3d_scale": 1.0,
      "use_taa": false, "route_policy": null,
      "visible_splats": 0,
      "payload_mode": null, "payload_streamable": null,
      "engagement_proof": null,
      "frames": [
        {
          "capture": {"file": "captures/C1-auto_capture.png", "sha256_reduced": "…", "sha256_full": "…"},
          "control": {"file": "captures/C1-auto_control.png", "sha256_reduced": "…", "sha256_full": "…"},
          "capture_equals_control": false
        }
      ]
    }
  ]
}
```

Field rules:

- The fields depend on the route. Leave the other route's fields `null` rather than
  inventing values:
  - `asset`, `import_route`, `node_quality_preset` and `max_splat_count` are for `node`
    and `multi_node` configurations. They are `null` for `world` and `streaming`, which
    consume world payloads.
  - `route_policy`, `payload_mode` and `payload_streamable` are for `world` and
    `streaming` configurations. They are `null` for `node` and `multi_node`.
  - `runtime_budget_splats` and `visible_splats` are required for every route.
  - A `multi_node` configuration lists one entry per node in a `nodes[]` array, each
    with its own node fields.
- `world_payloads` lists the payload ids a world or streaming configuration used. For
  C6 that is **both** A and B, in swap order.
- `engagement_proof` is required for C3, C4, C6 and C7:
  - for C3 and C4: the noise floor, the control region and the measured difference;
  - for C6: the before/after frame pair, the shared camera pose and the difference;
  - for C7: the per-frame series of `streaming_chunks_loaded_this_frame`,
    `streaming_chunks_evicted_this_frame` and `streaming_loaded_chunks`.
- `frames` pairs every judged capture with its pose-matched control.
  `capture_equals_control: true` in any frame makes the run a failure.

### `EVIDENCE_README.md`

It records:

- the tag commit and the archive hashes;
- the name, hash and splat count of each asset, and of each world payload with its
  derivation (the assets themselves are **not** committed);
- who ran the captures and who signs;
- the reading order for the captures;
- every deviation from this page, with its reason;
- an explicit **proof boundary**: what this pass shows and what it does not.

The proof boundary says that the pass shows **one Windows machine with one NVIDIA GPU**,
the listed assets and these configurations. It shows nothing about other GPU vendors,
Linux or macOS, or, for an alpha pass, multi-node scenes, which are outside the alpha
envelope. If C7 ran only as its interim, or the signed bytes were a pre-tag build (see
the prerequisites), the proof boundary says that too.

### `SIGNOFF.md`

```markdown
# Real-scan visual pass sign-off

- Signer: <maintainer's full name> (@<github-handle>)
- Captures run by: <name> (@<github-handle>)
- Date: <YYYY-MM-DD>
- Candidate: <release_tag> at <tag_commit>
- Bundle: evidence/visual/<YYYY-MM-DD>-<commit12>/

| Dimension | Verdict | Note |
| --- | --- | --- |
| Presence | PASS / FAIL | |
| Appearance | PASS / FAIL | |
| Depth interleaving | PASS / FAIL | |
| Temporal | PASS / FAIL / KNOWN #N | |
| World route | PASS / FAIL | |
| Streaming | PASS / FAIL / BLOCKED #1075 | |
| Exhibition recipe | PASS / FAIL | |
| Discrimination | PASS / FAIL | |
| Multi-node (v1.0 only) | PASS / FAIL / N/A for an alpha pass | |

Disposition: ACCEPT | FIX: <dimension and expected result> | SANCTION_BASELINE_UPDATE: <reason>
```

The three dispositions follow the wording of the #921 packet. `ACCEPT` requires every
dimension to be `PASS` or `KNOWN #N`, for a ledgered accepted limitation. Multi-node may
also be `N/A`, but only in an alpha pass. Any `FAIL` or `BLOCKED` rules `ACCEPT` out.

**The signer is the maintainer, and only the maintainer.** The maintainer may also run
the captures. It is a solo project, and the bundle records who did each. **The signature
is the maintainer's own commit of `SIGNOFF.md`, cross-posted as a comment on the release
tracking issue.** Nothing else counts as the signature.

## Tooling

No capture tool is committed yet. The #921 packet was produced by a runner kept outside
the repository (`phase2_visual_runner.gd` and `run_visual.py`). The plan is to adapt it
into `tests/visual/run_realscan_visual_pass.py`, taking `--godot-binary`, `--asset` and
`--out-dir`, and emitting in one invocation every configuration, its pose-matched
controls, the reduced and full-resolution images, and `summary.json`, so that the
maintainer reviews and does not operate. That is R1 work in its own change. The #921
runner lacks C4, C6, C7, C8, both import routes, and the per-frame controls.

*Estimated* time for one pass once that tool exists: about 15 minutes to download,
verify and import under both routes; about 45 minutes of unattended capture; about 20
minutes of review; about 5 minutes to write and commit `SIGNOFF.md`. These are
estimates. Nobody has timed a pass.

## Decisions

Decided on 2026-09-27 by the coordinator, on the maintainer's delegation.

1. **Signer:** the maintainer only. The maintainer may also run the captures, and the
   bundle records who did both.
2. **Location and storage:** `evidence/visual/<date>-<commit12>/` at the repository root,
   which is not published. Committed images are reduced, lossless PNGs with a long edge
   of 1280 px or less. The full-resolution set is attached to the GitHub release. The
   SHA-256 of every image, at both sizes, is committed. There is no LFS change.
3. **Asset:** the largest own real scan available, which is `baum-mit-wiese2.ply` at 8M
   splats, a maintainer-confirmed own scan (2026-09-27). Assets are referenced by hash
   and never committed.
4. **Import routes:** both, automatic (`ultra`) and dialog (`desktop`).
5. **Exhibition recipe:** AgX, with `tonemap_exposure = 0.354` (−1.5 stops), as the #921
   packet recorded.
6. **Streaming:** streaming stays in the alpha envelope, so the pass needs a real
   streaming configuration: the corridor world, once #1075 makes it run. Until then C7
   is `BLOCKED #1075`. `lightweight_smoke` is allowed only as a labelled interim, never
   as a substitute.
7. **Platform:** Windows plus one NVIDIA GPU is enough for the alpha. That is a stated
   single-vendor blind spot.
8. **Scope:** this page discharges the **alpha half** of bar §11's "Evidence-bundle
   format and location (§6)" item. v1.0 still needs the bundle to be machine-checked for
   freshness and hashes (bar §9).
