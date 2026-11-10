# ADR: What painterly is and where it lives (decision D-1)

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Risk:** this document is R0 (`docs/**`). It changes no code. The work it schedules is R3:
  per-splat stylisation edits binning and raster shaders, and moving the setting off
  `GaussianSplatNode3D` changes public API.
- **Refs:** decision D-1 of the 2026-11 codebase audit; findings I-01, I-02, I-04, I-05, I-06,
  I-07 and F-11; issues #323 (inert painterly material data) and #1001 (blend strength).
- **Related ADRs:** [One route, many demand producers](adr-one-route-many-producers.md), whose
  decision 1 forbids features that work only with one node.
- **Paths** are relative to `modules/gaussian_splatting/`. Each claim below was re-read at
  `b58bbe35d1f`.

## 1. Problem

GodotGS is for making games, and its first game needs a painterly look across a streamed
world and the objects in it. Today the look has three problems.

1. **It is set per node and dies in a shared scene.** `GaussianSplatNode3D` binds a
   `painterly/*` property group (`painterly/enabled`, `painterly/edge_threshold`,
   `painterly/stroke_opacity`, `painterly/stroke_width`, `painterly/temporal_blend`,
   `painterly/seed`) in `nodes/gaussian_splat_node_3d.cpp`. The renderer is shared by every
   splat node in a `World3D`, and `apply_renderer_settings()` in
   `nodes/gaussian_splat_node_helpers.cpp` turns painterly off whenever
   `_is_renderer_shared_with_other_content()` is true: more than one instance, or any world
   submission. A world plus one object therefore never shows the look, and nothing warns
   (I-01).
2. **It is a screen-space filter, not stylised splats.** The live effect is a 3×3 luma Sobel
   pass (`shaders/sobel_outline.glsl`) followed by a blur along the edge tangent
   (`shaders/brush_accumulate.glsl`), both registered in `shaders/compile_shaders.py`. The
   `PainterlyMaterial` palette textures, noise LUTs and stroke density are uploaded and
   cached in `interfaces/painterly_renderer.cpp` (`cached_palette_textures`,
   `cached_noise_luts`, `cached_stroke_density_buffer`) but no pass binds them. The
   per-splat payload fields (palette ids, brush axes, painterly flags, stroke ages) are
   stored and saved, and a comment in `shaders/tile_binning.glsl` states that nothing in any
   shader reads them. `shaders/includes/painterly_features.glsl` is included only by
   `shaders/gaussian_splat.frag.glsl`, which no pipeline compiles; a test reads it as text
   (I-04).
3. **Its knobs do not do what they say.** `painterly/temporal_blend` is passed to the
   renderer as a display gamma (I-05). The composite blend strength is the constant `1.0`
   in `PainterlyRenderer` (I-06). `PainterlyRenderer::populate_painterly_gbuffer()` is a
   second hand-written `TileRenderParams` producer that diverges from the main one (I-07).
   The HLOD streaming route refuses painterly outright:
   `GaussianSplatRenderer::hlod_streaming_unsupported_reason()` returns
   `unsupported_view_or_material` when painterly is on (I-02).

F-11 (renderer-wide settings exposed as per-node properties that only the settings-owner
node applies) waits on the same question: where painterly settings live.

## 2. Options

The options are those of decision D-1 in the audit.

### Option A: keep the screen-space look as a camera or environment effect

The Sobel and brush passes stay. The setting moves off the node to `WorldEnvironment` or a
dedicated environment resource.

- **Cost:** small. Remove the per-node ownership and the force-off.
- **Enables:** a painterly look on the world and all objects within weeks.
- **Limits:** it is an edge filter. It does not stylise splats, and the palette, noise and
  brush material features stay unimplemented.

### Option B: per-splat stylisation

Brush shape, orientation, palette and noise are applied in binning and raster, driven by
`PainterlyMaterial`.

- **Cost:** an R3 rendering project: a design ADR plus months of work.
- **Enables:** a look that no shipped engine offers. The audit's survey of the state of the
  art found no shipped painterly splat renderer (marked unconfirmed there).
- **Limits and risk:** no peer exists to copy, so the target look needs reference images
  that the owner signs. Nothing new reaches the screen until the first slice lands.

### Option C: both

Do A now, and B as a v1.0 track with its own ADR.

- **Cost:** A's cost plus B's, and A's work is thrown away when B replaces it.
- **Enables:** the game and a world-plus-object scene get a painterly look now; B follows.

**Audit recommendation: C.** In every option the setting leaves `GaussianSplatNode3D`.

## Owner decision

- **Option chosen:** **B, per-splat stylisation only.** There is no interim screen-space
  effect at camera or environment level.
- **Decided by:** the owner, 2026-11-10.
- **Recorded in:** the owner's answer in the 2026-11-10 session. No owner issue comment,
  review or other public record is linked yet. **Acceptance is pending:** the decision
  becomes binding only when the owner approves and merges the pull request that adds this
  ADR, and that approving review is then the acceptance record.
- **Rejected:**
  - **A**, because it keeps an edge filter as the product look and leaves the material
    features unimplemented. Its advantage, a look on world plus objects within weeks, is
    given up.
  - **C (the recommendation)**, because its interim half is work on a pass that B replaces.
    The trade-off accepted: no painterly look in a world-plus-object scene until per-splat
    stylisation lands.

## 3. Decision

1. **Painterly is per-splat stylisation** driven by `PainterlyMaterial`: palette, noise,
   brush axis and stroke density are bound in binning and raster.
2. **Ownership moves off `GaussianSplatNode3D`.** Per-splat stylisation must work on the
   world and on objects in the same frame, so a per-node setting that the shared renderer
   overrides cannot stay. The ownership part of the PT02 epic stays in the plan.
3. **The existing Sobel and brush pass stays as it is** until per-splat stylisation replaces
   it. It gets no further investment: no new knobs, no fixes to its existing knobs, no
   migration to an environment-level effect.
4. **Per-splat stylisation needs its own R3 design ADR** before any shader change (CLN-PT05-1).
   This record does not specify the shader design.
5. **Reference images are a prerequisite.** The owner signs a set of reference images that
   define the target look before any implementation slice of per-splat stylisation starts.
   CLN-PT05-1 proposes the scene set and the command that produces the images; the owner
   signs both.

### Migration of existing scenes

Under every option the `painterly/*` properties leave `GaussianSplatNode3D`. Scenes that set
`painterly/enabled` then need one of two treatments:

- keep `painterly/enabled` as a deprecated alias that maps to the new owner and warns once; or
- remove it, with a release note.

[API stability](../development/api-stability.md) allows removal without a deprecation period
before v1.0. The owner has not chosen between the two. The task that moves ownership
proposes one in its PR, and the owner decides there.

## 4. Consequences for the cleanup plan

| Task | Effect |
| --- | --- |
| CLN-PT05 (epic) and CLN-PT05-1, -2, -3 | Confirmed. Per-splat stylisation is the painterly track. PT05-1 (design ADR with signed reference images) comes first. |
| CLN-PT02 (epic) | Ownership part stays: the setting leaves `GaussianSplatNode3D`. |
| CLN-PT02-1 | Stays as the ownership task. Its contract assumes an environment-level screen-space owner (A or C); it must be re-scoped to option B before dispatch. |
| CLN-PT02-2 (gamma and `temporal_blend` naming) | Void: polish of the screen-space pass. |
| CLN-PT02-3 (blend strength, #1001) | Void: polish of the screen-space pass. |
| Findings I-05, I-06 (and F-10) | Move from PT02 to PT05: the knobs disappear when PT05 replaces the pass. |

Findings and their route after this decision:

- **I-01** (force-off in a shared scene): CLN-PT02, ownership move.
- **I-02** (HLOD route refuses painterly): the HLOD refusal list, see
  [One route, many demand producers](adr-one-route-many-producers.md).
- **I-04** (inert material data): CLN-PT05.
- **I-05**, **I-06**: CLN-PT05.
- **I-07** (second `TileRenderParams` producer): CLN-RT02.
- **F-11** (renderer-wide settings on nodes): CLN-RS01. Painterly settings no longer sit on
  the node, so F-11 no longer has to account for them.

## 5. Out of scope

- Any code, shader, setting or test change. Each follow-up task cites this ADR.
- The per-splat shader design, data layout and memory rule (CLN-PT05-1).
- The new owner of the painterly settings: material, project setting or another resource
  (the ownership task, under this decision).
- The compatibility treatment of `painterly/enabled` (see Migration above).
- Whether the HLOD route lifts its painterly refusal for the existing screen-space pass, or
  only for per-splat stylisation. This decision forbids new investment in the pass; the
  owner has not ruled on the HLOD-route task that touches it.
