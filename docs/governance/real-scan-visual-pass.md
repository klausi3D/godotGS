# Real-Scan Visual Pass

The procedure, evidence-bundle format, location and signer field for the human visual
pass that the [release acceptance bar](release-acceptance-bar.md) makes a gate for the
public alpha and for v1.0 (§6, §9, §10). Tracked as
[#1012](https://github.com/klausi3D/godotGS/issues/1012).

> **Status: draft, not yet executed and not yet approved by a maintainer.** The steps
> and formats below are concrete enough to run, but several policy choices are **open
> decisions**, not settled rules. They are listed in
> [Open decisions](#open-decisions) at the end. Where this page proposes a default for
> one of them, it says *proposed*. Until a maintainer settles them, a bundle produced
> from this page is evidence, and it is not a sign-off.

## What the pass is, and what it is not

Synthetic CI fixtures (2k to 30k splats) keep their regression role, but they do not
support the claim that the product looks right. GPU-001 was absent under production
defaults while every counter reported success, and a QA pin masked it (#903, #921). The
pass puts **a named human's eyes on real-scan content, rendered by the exact bytes a user
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

1. **The candidate binary comes from the release archive, not a local build.** Download
   the editor archive and the export-template archive from the candidate release. Record
   the `sha256` of each, and the `commit=` line of the archive's `BUILD-INFO.txt`
   (written by `.github/workflows/release_builds.yml`). A pass run on a local build says
   nothing about the bytes a user downloads.
2. **Real-scan assets, referenced by hash and never committed.** For each source `.ply`
   or `.spz`, record its file name, `sha256`, splat count, and where a reviewer can
   obtain it. *Proposed:* one scan of at least 1M splats, so that the import and node
   caps below actually bind, plus `Cabin in the forest.ply` (about 130k splats), which
   #921 already used, as a small-scene control.
3. **Import route, preset and node quality are stated.** Record them for each asset.
   Two passes "at defaults" can render very different splat counts, because the
   defaults depend on the route:
   - the module's import dialog preselects the `desktop` preset
     (`modules/gaussian_splatting/editor/gaussian_import_dialog.cpp:973-976`):
     `max_splats = 750000`, `density_multiplier = 0.7`;
   - Godot's automatic import uses preset index 0, which is `ultra`
     (`modules/gaussian_splatting/io/gaussian_import_preset.cpp:8-19`):
     `max_splats = 0`, `density_multiplier = 1.0`, so no import cap applies;
   - a `GaussianSplatNode3D` at its Balanced quality preset caps `max_splat_count` at
     500,000 (`modules/gaussian_splatting/nodes/gaussian_splat_node_helpers.cpp:1680-1682`).

   Record the preset id, `max_splats`, `density_multiplier`, the imported splat count,
   the node quality preset, and the **visible splat count the renderer reported** in
   each configuration. Which route or routes the pass must cover is an
   [open decision](#open-decisions).
4. **Machine.** Record the OS and build, the GPU model, the driver version, the
   rendering driver and API version, and the output resolution. *Proposed:*
   1920×1080.
5. **Shipped composite defaults.** Every configuration runs at
   `rendering/gaussian_splatting/composite/depth_test = true`, the registered default
   (`modules/gaussian_splatting/core/gaussian_splat_manager.cpp:998`). Record any
   project setting that differs from its registered default. A difference that is not
   recorded invalidates the run.

## Configurations

All configurations use Forward+ and a single view (bar §10.1).

| # | Route | Configuration | What it checks |
| --- | --- | --- | --- |
| C1 | Node | Scale 1.0, no upscaler, no TAA | The baseline appearance claim. |
| C2 | Node | Bilinear, scale 0.75 | The GPU-001 defect configuration (#921). |
| C3 | Node | FSR2, scale 1.0, a 28-frame burst with the camera moving | The bar's required FSR2 motion-ghosting check (§6). |
| C4 | Node | TAA on, scale 1.0, a 28-frame burst with the camera moving | The second temporal consumer. |
| C5 | Node | An opaque mesh standing inside the splat cloud | Depth interleaving and the composite occlusion claim. |
| C6 | World | `GaussianSplatWorld3D` with a resident payload, scale 1.0, plus one live payload swap during the run | The world route, which is in the alpha envelope (§10.1). |
| C7 | Streaming | `GaussianSplatWorld3D` with a streamable payload, and a camera path that crosses chunk boundaries so chunks load and evict | Streaming open worlds, in the alpha envelope since #1016. |
| C8 | Node | C1's camera with the exhibition recipe applied | The bar's required exhibition-recipe check (§6). |

Notes on individual rows:

- **C6 and C7 differ by payload, not by node.** `rendering/gaussian_splatting/streaming/route_policy`
  defaults to `1` (streaming) and applies to world submissions only
  (`gaussian_splat_manager.cpp:1005-1008`). A direct `GaussianSplatNode3D` always
  registers as resident. A world payload streams only if it is an uncompressed
  `.gsplatworld` (see [streaming](../features/streaming.md#source-residency-model)).
  Record `route_policy`, and for both rows record `payload_mode` and
  `payload_streamable` from `GaussianSplatRenderer.get_render_stats()`.
- **C7 has to prove that it streamed.** The run fails, as a run, unless
  `payload_streamable` is true and the `gaussian_splatting/streaming_loaded_chunks`
  monitor changes during the camera path. A streaming configuration that quietly ran
  resident proves nothing about streaming.
- **C3 and C4 have to prove that the temporal stage was engaged.** Capture the same
  camera path once without the temporal stage. At least one region has to differ from
  that capture, or the viewport silently refused FSR2 or TAA and the run fails. This
  is the gate #1025's measurement used.
- **C8's recipe is not defined in the repository.** The #921 packet applied AgX
  tonemapping with an exposure of −1.5 stops (`tonemap_exposure = 0.354`) and flagged
  that the exposure control the exhibition actually used is not recorded. That is an
  [open decision](#open-decisions).

### A control frame for every configuration

Each configuration also captures a **splats-hidden control frame** from the same camera.
If a capture is bit-identical to its control, the capture saw no splats. That is a
**failed run**, not a passed one ([evidence integrity](evidence-integrity.md)).

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
| Discrimination | Any capture that matches its control frame, or any engagement proof above that did not hold. |

**A known, accepted limitation is not a failure.** A symptom that matches an entry on
[Known Public Alpha Limitations](../development/known-public-alpha-limitations.md)
*above* its "Proposed, not yet accepted" heading is disclosed. The signer notes it and
does not fail the dimension. An example is the ≈1.5 px TAA trail of #1025 in C4. A
symptom that is in that fenced section, or that is not on the page at all, **is** a
failure.

**A dimension the captures cannot answer is a failure of the procedure, not a pass of
the product.** Recapture it. Do not waive it.

## The evidence bundle

### Location

*Proposed:* one directory per pass, named by date and candidate commit:

```text
docs/evidence/visual/<YYYY-MM-DD>-<commit12>/
├── EVIDENCE_README.md
├── summary.json
├── SIGNOFF.md
└── captures/
    ├── C1_capture.png
    ├── C1_control.png
    ├── C3_motion_00.png … C3_motion_27.png
    └── …
```

Where the bundle lives, and how its images are stored, is an
[open decision](#open-decisions). This directory does not exist yet. Do not name files
`*.actual.png`, because the release gate forbids tracked files of that name under
`tests/visual_baselines/` and the name suggests a baseline comparison, which this is not.

### `summary.json`

One JSON object. Every field is required. `null` is allowed only where the note says so,
and a `null` has to be explained in `EVIDENCE_README.md`.

```json
{
  "schema": "godotgs-realscan-visual-pass/1",
  "candidate": {
    "release_tag": "<v*-alpha* tag>",
    "commit": "<40-hex from BUILD-INFO.txt>",
    "editor_archive": {"name": "…", "sha256": "…"},
    "export_template_archive": {"name": "…", "sha256": "…"}
  },
  "machine": {
    "os": "…", "gpu": "…", "driver": "…",
    "rendering_driver": "vulkan", "api_version": "…",
    "resolution": [1920, 1080]
  },
  "assets": [
    {
      "id": "A1", "file_name": "…", "sha256": "…", "source_splat_count": 0,
      "obtain_from": "…",
      "import_route": "dialog | automatic",
      "import_preset": "desktop", "max_splats": 750000, "density_multiplier": 0.7,
      "imported_splat_count": 0
    }
  ],
  "non_default_project_settings": {"<key>": "<value>"},
  "configurations": [
    {
      "id": "C1", "asset": "A1", "route": "node | world | streaming",
      "node_quality_preset": "balanced", "max_splat_count": 500000,
      "scaling_3d_mode": "off | bilinear | fsr2", "scaling_3d_scale": 1.0,
      "use_taa": false, "route_policy": null,
      "visible_splats": 0,
      "payload_mode": null, "payload_streamable": null,
      "engagement_proof": null,
      "captures": [{"file": "captures/C1_capture.png", "sha256": "…"}],
      "control": {"file": "captures/C1_control.png", "sha256": "…"},
      "capture_equals_control": false
    }
  ]
}
```

`route_policy`, `payload_mode` and `payload_streamable` may be `null` only for node
routes. `engagement_proof` is required for C3, C4 and C7, and records what was compared
and what changed. `capture_equals_control: true` in any row makes the run a failure.

### `EVIDENCE_README.md`

It records the candidate commit and the archive hashes, the name, hash and splat count of
each asset (the asset itself is **not** committed), the reading order for the captures,
every deviation from this page with its reason, and an explicit **proof boundary**: what
this pass shows and what it does not. It shows one machine, one GPU vendor, the listed
assets and these configurations. It does not show other vendors, other platforms, or
multi-node scenes, which are outside the alpha envelope.

### `SIGNOFF.md`

```markdown
# Real-scan visual pass sign-off

- Signer: <full name> (@<github-handle>)
- Date: <YYYY-MM-DD>
- Candidate: <release_tag> at <commit>
- Bundle: docs/evidence/visual/<YYYY-MM-DD>-<commit12>/

| Dimension | Verdict | Note |
| --- | --- | --- |
| Presence | PASS / FAIL | |
| Appearance | PASS / FAIL | |
| Depth interleaving | PASS / FAIL | |
| Temporal | PASS / FAIL | disclosed limitations seen: … |
| World route | PASS / FAIL | |
| Streaming | PASS / FAIL | |
| Exhibition recipe | PASS / FAIL | |
| Discrimination | PASS / FAIL | |

Disposition: ACCEPT | FIX: <dimension and expected result> | SANCTION_BASELINE_UPDATE: <reason>
```

The three dispositions follow the wording of the #921 packet. `ACCEPT` requires every
dimension to be `PASS`.

**The signature is the signer's own commit of `SIGNOFF.md`, cross-posted as a comment on
the release tracking issue.** Nothing else counts as the signature.

## Tooling

No capture tool is committed yet. The #921 packet was produced by a runner kept outside
the repository (`phase2_visual_runner.gd` and `run_visual.py`). *Proposed:* adapt it into
`tests/visual/run_realscan_visual_pass.py`, taking `--godot-binary`, `--asset` and
`--out-dir`, and emitting every configuration, its control and `summary.json` in one
invocation, so that the human reviews and does not operate. That is R1 work in its own
change, and it has to add C4, C6, C7 and C8, which the #921 runner does not have.

*Estimated* time for one pass once that tool exists: about 10 minutes to download,
verify and import; about 30 minutes of unattended capture; about 15 minutes of review;
about 5 minutes to write and commit `SIGNOFF.md`. These are estimates. Nobody has timed
a pass.

## Open decisions

These need a maintainer. This page does not settle them. They are repeated on the PR
that introduced it.

1. **Who may sign.** Only the maintainer, or any CODEOWNER? Does the signer have to be
   someone other than the person who ran the captures?
2. **Where the bundle lives and how the images are stored.** `docs/**` is staged into the
   public docs site (`scripts/stage_public_docs.py` excludes only `agent_memory` and
   `archive`), so the proposed path would publish the bundle. Git LFS is configured only
   for `*.mp4` and `*.webm` (`.gitattributes`), not for PNG. A pass is roughly 100
   full-resolution PNGs. The options include LFS for this path, committing the hashes
   plus reduced images and attaching the full-resolution set to the release, or a path
   outside `docs/`.
3. **Which assets.** Which scan of 1M splats or more? Is its licence compatible with
   being referenced? Where does a reviewer obtain it to reproduce the pass?
4. **Which import route.** Does the pass sign off the dialog route (`desktop`), the
   automatic route (`ultra`), or both?
5. **What the exhibition recipe is.** The tonemapper, the exposure value and the control
   that sets it need to be written down, because the #921 packet had to guess the control.
6. **Streaming content.** Which streamable `.gsplatworld` does C7 use? Bar §10.1 notes
   that the streaming lanes' content is classified `lightweight_smoke`. Is the pass
   required to use content promoted to `real_chunked`?
7. **Platforms and vendors.** Is a Windows pass on a single NVIDIA GPU sufficient for the
   alpha? Linux is `smoke-tested` and runs no GPU lane.
8. **Relationship to the bar's v1.0 work item.** Bar §11 lists "Evidence-bundle format and
   location (§6)" as a v1.0 item, while §6 binds the alpha to a committed bundle. Does
   this page discharge that item once the decisions above are settled, or only the alpha
   half of it?
