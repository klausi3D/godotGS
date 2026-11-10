# ADR: Reference parity at default settings — the quality contract

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Task state:** the decision above is accepted. CLN-D04 is not complete: its tolerance and
  transparency-ordering criteria are unmet and the task stays open after this record merges
  ([What CLN-D04 still requires](#what-cln-d04-still-requires)).
- **Decision:** D-4 of the 2026-11 cleanup audit. Task CLN-D04. Bundled with
  [v1.0 platform and graphics-API targets](adr-v1-platform-targets.md) (D-3); the two
  decisions are independent.
- **Risk:** this document is R0 (`docs/**`). It changes no code, setting, threshold, gate or
  baseline. The shader and settings changes it calls for are separate tasks ([Consequences](#consequences)), each with
  its own risk class and evidence.
- **Refs:** #1012, #1222, #1223, #1137.
- **Base:** `b58bbe35d1f` (origin/master). Every `file:line` below was read at that commit.
  Paths are relative to `modules/gaussian_splatting/` unless they start with `docs/` or `tests/`.

## Problem

The release bar asks a human to sign that real-scan output "looks right"
([Release Acceptance Bar §6](../governance/release-acceptance-bar.md#6-visual-proof)).
Nothing says what "right" is measured against, so the
[real-scan visual pass](../governance/real-scan-visual-pass.md) (#1012) and a golden-image
lane have no pass criterion.

At default settings the image departs from the reference rasterizers through stop-gaps that
the references do not have. The audit found these at `1cfcf3c1ccb`; each was re-read at the
base above:

| Stop-gap | Where | Audit finding |
| --- | --- | --- |
| 10:1 aspect clamp: the minor radius is raised to major/10 and opacity is multiplied by (max/aspect)². The default `max_conic_aspect` is `10.0`. | `shaders/tile_binning.glsl:1019-1034`; `interfaces/rasterizer_interfaces.h:51` | A-02, #1223 |
| Eigenvalue cap `MAX_EIGENVALUE = 10000.0` (σ = 100 px), with no compensation. | `shaders/includes/gs_eigen_binning.glsl:99-107` | A-03, #1222 |
| Large-splat fade to 70 % opacity between 16 × and 32 × `TILE_SIZE` radius (`size_fade`). Production rendering uses the default tile size of 16, so the fade runs from 256 to 512 px. (The shader comments say 128 and 256 px, which assumes a tile size of 8.) | `shaders/tile_binning.glsl:1156-1161`; `renderer/render_pipeline_stages.cpp:2355`; `renderer/tile_renderer.h:36` | A-03, #1222 |
| Per-tile raster cap `GS_MAX_RASTER_SPLATS_PER_TILE`: records past the cap (the farthest in the tile) are not drawn. The default project setting is the `high` preset, so **at default settings the cap is 12288**. The `low`, `medium` and `ultra` presets set 4096, 8192 and 16384. 65536 applies only when no preset is set. | `shaders/tile_rasterizer_compute.glsl:187-195`; `renderer/gpu_sorting_config.cpp:699` (default preset `high`), `:544` (12288), `:469`, `:510`, `:578`, `:139-141` (65536 without a preset), `:113-117` | A-13 (#1137 is closed; the preset values are unchanged at the base) |

The audit's further render-math findings that the reference-quality chain (CLN-RQ03 to
CLN-RQ10) addresses are listed in [Consequences](#consequences).

The image thresholds the audit found (H-11) are the benchmark lane's defaults, SSIM ≥ 0.95 and
PSNR ≥ 30 dB
(`tests/examples/godot/test_project/scenes/benchmark_suite/benchmark_suite_lane.gd:13-14`).
The lane compares captures against a configurable reference directory; nothing defines
these values as a tolerance against a reference renderer, so they are not a quality contract.

## Options considered

These are the options in the audit's decision list (D-4).

### Option A — reference parity at the default quality

The image at default settings matches the reference rasterizers (gsplat and the
graphdeco-inria `gaussian-splatting` rasterizer) within a stated tolerance. Clamps, caps and
fades become opt-in quality settings.

- **Cost:** the stop-gaps in [Problem](#problem) come out of the default path, in a fixed order (the Jacobian
  first, CLN-RQ04), each behind its own image evidence. Some content that the stop-gaps hide
  today, such as needle splats at grazing angles or very large close-up splats, becomes
  visible and may cost frame time.
- **Enables:** a visual pass and a golden-image lane with a pass criterion; real scans look as
  they do in the tools that trained them; a defect shows up as a difference from a reference.
- **Limits:** a game that wants the stop-gaps has to turn them on; the default is no longer
  tuned for any one game.

### Option B — a game-tuned image

The stop-gaps stay in the default, but they are documented, switchable and part of the
visual baseline.

- **Cost:** every stop-gap needs a setting, documentation and a baseline image; the baseline
  is the project's own output.
- **Enables:** no change in the default image; no frame-time risk from removing stop-gaps.
- **Limits:** a signer compares against GodotGS's own earlier output, so a defect that is
  already in the baseline cannot be seen; real scans differ from their training tools by
  design.

### Recommendation

The audit recommended Option A: a visual pass cannot be signed against an image that has no
reference.

## Owner decision

**Option A, reference parity at default settings**, decided by the owner on 2026-11-10. The
owner's words: the image matches gsplat/Inria within a stated tolerance on real scans;
clamps, caps and fades become opt-in. This is the recommended option.

The owner's approving review on the pull request that adds this record is the acceptance
record; there is no separate issue comment. Option B is rejected for the reason in [Options considered](#options-considered): a
self-referential baseline cannot show a defect it already contains.

## Decision

1. **Default settings render with reference parity.** With every quality setting at its
   default, GodotGS's image of a real scan matches the reference render of the same scan, from
   the same camera, within the tolerance ([Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes)).
2. **Stop-gaps that change the image of in-budget content are off at default settings.**
   This covers the aspect clamp, the eigenvalue cap, the large-splat fade and the preset
   raster cap (item 3). Where a stop-gap survives as a setting, it is opt-in and its default
   is off. The capacity limits that the confirmed chain keeps are not covered by this item;
   see [Capacity limits the chain keeps](#capacity-limits-the-chain-keeps).
3. **The default preset's raster cap is a stop-gap at default settings.** The default
   project setting is the `high` preset, whose cap of 12288 drops records
   ([Problem](#problem)). Under item 2, CLN-RQ07-1 is required for parity at default
   settings; it is not optional preset tidying. As written, CLN-RQ07-1 brings every preset
   to the default cap of 65536. Its one exception: a preset may keep a lower cap if the PR
   measures a raster-time regression beyond runner noise, the drop stays a visible warning,
   and the owner approves that choice on that PR. If the owner approves the exception for
   the `high` preset, the default settings keep a record-dropping cap. That PR then has
   to say so against item 2.
4. **The visual pass compares against the reference.** The release bar's §6 signer compares
   the candidate render with the reference render of the same scan and camera, and signs that
   the difference is within tolerance.

## Tolerance, reference versions and scenes

The owner decided that a stated tolerance applies, on real scans, against gsplat and the
Inria rasterizer. The owner did **not** state the numbers. This record does not choose them;
they are open ([Open questions for the owner](#open-questions-for-the-owner)):

- the metric or metrics (for example PSNR, SSIM, LPIPS or FLIP) and their thresholds;
- the reference renderer or renderers and the version of each;
- the scenes and camera paths the comparison covers.

Until the owner records these values, the golden-image lane (CLN-EV03) and the visual pass
(#1012) still have no numeric pass criterion. They are recorded by amending this section, with
the owner's approval.

### Capacity limits the chain keeps

The owner's words are that clamps, caps and fades become opt-in. The reference-quality
chain, which the owner confirmed as written, does not remove every cap from the default
path. Two limits stay at default settings:

- **The per-tile raster cap of 65536.** CLN-RQ07-1 stops presets from lowering the cap.
  Records past 65536 in a tile are still not drawn (`shaders/tile_rasterizer_compute.glsl:187-195`).
- **The over-budget drop.** CLN-RQ07-2 makes over-budget drops deterministic. They are
  still drops, counted and warned (#1137).

The chain's CLN-RQ03 also keeps the viewport-cover radius clamp
(`shaders/tile_binning.glsl:1163-1190`) as a bounded-work guard. It documents the clamp as
reference-absent and files an issue for it.

No task removes these limits or makes them opt-in. This record does not decide whether
parity is measured only within these limits, or whether they must also become opt-in. That
is an open question for the owner ([Open questions for the owner](#open-questions-for-the-owner)), and no task is
created for it until the owner answers.

### What CLN-D04 still requires

The CLN-D04 contract requires this record to state, with numbers, the tolerance metric and
threshold, the reference renderers with versions, and the scene list. It also requires the
transparency-ordering contract against Godot transparents. The owner's answer gives none
of these, and this record does not invent them. Those CLN-D04 criteria are **unmet**. They
are kept as blocked requirements, not out of scope. CLN-D04 stays open after this record
merges. It is complete only when the owner approves the values in an amendment to this
record, or approves an amendment to the CLN-D04 contract.

## Consequences

### Tasks this record unblocks now

These tasks list this record as a dependency and do not depend on CLN-EV03. The
dependencies are those in each task's contract.

| Item | What changes | Other dependencies |
| --- | --- | --- |
| CLN-LT10 | Transparency ordering contract ([Transparency ordering with Godot transparents](#transparency-ordering-with-godot-transparents)). | CLN-EN01 |
| CLN-DOC05 | Peer comparison. | CLN-EV05, CLN-DOC08-4 |
| CLN-DOC08-4 | Release-bar restructure. | CLN-PR05, CLN-DOC02 |
| CLN-RQ08 (#1168; A-12, A-14) | Quantised payload precision. Its contract does not list this record. | CLN-IO03 |

### Tasks still blocked on the owner's tolerance values

CLN-EV03, the golden-image lane, renders against the reference renderer and scenes this
record names, and it does not choose them itself. Until the owner records the values in
[Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes), CLN-EV03 has no pass criterion. Every task below
depends on CLN-EV03, directly or through another task in this table:

| Item | What changes | Blocked through |
| --- | --- | --- |
| CLN-EV03 (#1012) | Golden-image lane against an independent reference render. | the tolerance values |
| CLN-RQ03 (#1223, #1222; A-02, A-03) | Aspect clamp opt-in (default 0); eigenvalue cap and large-splat fade removed. | CLN-EV03, CLN-RQ04 |
| CLN-RQ04 (A-05) | Reference EWA Jacobian first. | CLN-EV03 |
| CLN-RQ05 (#526; A-07, A-08) | `exp()` instead of the Schraudolph approximation; subpixel cull on projected area. | CLN-RQ03 |
| CLN-RQ06 (A-04) | Near-camera lens fade in metres of view depth; never reject a splat outright. | CLN-RQ05 |
| CLN-RQ07, CLN-RQ07-1, CLN-RQ07-2 (A-13, #54; B-03) | Presets stop lowering the raster cap, unless the owner approves a measured lower cap ([Decision](#decision), item 3); over-budget drops become deterministic. | CLN-EV03 |
| CLN-RQ09 (B-11) | Subgroup early-out independent of subgroup size. | CLN-EV03 |
| CLN-RQ10 (A-11) | HDR values above 1.0 survive the composite decode for float sources. | CLN-EV03 |
| CLN-DOC04 (#1012) | The signed real-scan visual pass, against the reference. | CLN-EV03, CLN-RQ07, CLN-RQ09, CLN-RQ10 |

The owner's answer confirms the reference-quality chain as written. CLN-RQ03 removes the
eigenvalue cap and the large-splat fade from the shader, and its mutation proof turns red
if either is restored. The aspect clamp stays as an opt-in setting whose default is off.

Finding D-01 (chunk-route prefix LOD, #1053) was listed as blocked on this decision. The
owner's decision D-2 retires the chunk-streaming route, and CLN-RT07 takes over D-01; this
record adds nothing to it.

### What stays the same

- No setting, shader, threshold, baseline or gate changes in this record.
- The existing SSIM and PSNR benchmark thresholds keep their regression role.
- The bundle format and signer rules in
  [Real-Scan Visual Pass](../governance/real-scan-visual-pass.md) are unchanged.

## Transparency ordering with Godot transparents

The audit's capability matrix records the current state: the splat composite is a single
layer and does not interleave with Godot's transparent objects. The audit listed the ordering
contract as part of D-4. The owner's answer does not state it, and the reference renderers
draw splats alone, so parity does not settle it either. This record does not decide it.
The CLN-D04 contract requires the ordering contract, so that criterion is unmet
([What CLN-D04 still requires](#what-cln-d04-still-requires)). CLN-LT10 writes the ordering
contract as its own ADR and tests it. Until the owner decides it, the current single-layer
behaviour is the documented state, not a contract.

## Out of scope

- Any code, shader, setting or workflow change; each task in [Consequences](#consequences) cites this record.
- Performance targets; parity at default settings sets image quality, not frame time.
- Platform and vendor coverage; see
  [v1.0 platform and graphics-API targets](adr-v1-platform-targets.md).

## Open questions for the owner

1. The tolerance: metric, threshold, reference renderer versions and scene list
   ([Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes)). A CLN-D04 requirement; blocks CLN-EV03 and the tasks after it.
2. The transparency ordering contract with Godot transparents
   ([Transparency ordering with Godot transparents](#transparency-ordering-with-godot-transparents), CLN-LT10). A CLN-D04 requirement.
3. The capacity limits the chain keeps at default settings: the 65536 raster cap, the
   over-budget drop and the viewport-cover radius clamp
   ([Capacity limits the chain keeps](#capacity-limits-the-chain-keeps)). Is parity measured within them, or must they
   also become opt-in? No task exists for them until this is answered.
