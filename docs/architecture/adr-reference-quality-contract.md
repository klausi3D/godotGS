# ADR: Reference parity at default settings — the quality contract

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
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
| Large-splat fade to 70 % opacity between 128 and 256 px radius (`size_fade`). | `shaders/tile_binning.glsl:1156-1161` | A-03, #1222 |
| Per-tile raster cap `GS_MAX_RASTER_SPLATS_PER_TILE`: records past the cap (the farthest in the tile) are not drawn. The host default is 65536; the `low`, `medium`, `high` and `ultra` presets set 4096, 8192, 12288 and 16384. | `shaders/tile_rasterizer_compute.glsl:187-195`; `renderer/gpu_sorting_config.h:61`; `renderer/gpu_sorting_config.cpp:469`, `:510`, `:544`, `:578` | A-13 (#1137 is closed; the preset values are unchanged at the base) |

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
2. **Stop-gaps are off at default settings.** The aspect clamp, the eigenvalue cap, the
   large-splat fade and any raster cap that drops records do not apply at default settings.
   Where a stop-gap survives as a setting, it is opt-in and its default is off.
3. **Presets.** The quality presets that lower the raster cap ([Problem](#problem)) are handled by CLN-RQ07-1.
   Which stop-gaps a non-default preset may turn on is not decided here.
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

## Consequences

### Tasks and findings this record unblocks

| Item | What changes |
| --- | --- |
| CLN-EV03 (#1012) | Golden-image lane against an independent reference render. Needs the values in [Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes). |
| CLN-RQ03 (#1223, #1222; A-02, A-03) | Aspect clamp opt-in (default 0); eigenvalue cap and large-splat fade removed. |
| CLN-RQ04 (A-05) | Reference EWA Jacobian first. |
| CLN-RQ05 (#526; A-07, A-08) | `exp()` instead of the Schraudolph approximation; subpixel cull on projected area. |
| CLN-RQ06 (A-04) | Near-camera lens fade in metres of view depth; never reject a splat outright. |
| CLN-RQ07, CLN-RQ07-1, CLN-RQ07-2 (A-13, #54; B-03) | Presets stop lowering the raster cap; over-budget drops become deterministic. |
| CLN-RQ08 (#1168; A-12, A-14) | Quantised payload precision. |
| CLN-RQ09 (B-11) | Subgroup early-out independent of subgroup size. |
| CLN-RQ10 (A-11) | HDR values above 1.0 survive the composite decode for float sources. |
| CLN-DOC04 (#1012) | The signed real-scan visual pass, against the reference. |
| CLN-DOC05, CLN-DOC08-4 | Peer comparison and release-bar restructure; both list this record as a dependency. |
| CLN-LT10 | Transparency ordering contract ([Transparency ordering with Godot transparents](#transparency-ordering-with-godot-transparents)). |

The owner's answer confirms the reference-quality chain as written. CLN-RQ03 as written
removes the eigenvalue cap and the large-splat fade instead of keeping them as opt-in
settings; both readings leave them off at default settings, which is what [Decision](#decision) requires.

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
CLN-LT10 writes that contract as its own ADR and tests it; until then the current single-layer
behaviour is the documented state, not a contract.

## Out of scope

- Any code, shader, setting or workflow change; each task in [Consequences](#consequences) cites this record.
- Performance targets; parity at default settings sets image quality, not frame time.
- Platform and vendor coverage; see
  [v1.0 platform and graphics-API targets](adr-v1-platform-targets.md).
- The numeric tolerance, the reference versions and the scene list ([Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes)).

## Open questions for the owner

1. The tolerance: metric, threshold, reference renderer versions and scene list ([Tolerance, reference versions and scenes](#tolerance-reference-versions-and-scenes)).
2. Whether the eigenvalue cap and the large-splat fade are removed (CLN-RQ03 as written) or
   kept as opt-in settings.
3. The transparency ordering contract with Godot transparents ([Transparency ordering with Godot transparents](#transparency-ordering-with-godot-transparents), CLN-LT10).
