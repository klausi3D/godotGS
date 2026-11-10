# ADR: v1.0 scope for splat animation and collision (decision D-6)

- **Status:** Accepted — decided by the owner on 2026-11-10; the owner's approving review on this PR is the acceptance record.
- **Date:** 2026-11-10.
- **Risk:** this document is R0 (`docs/**`). It changes no code. The work it schedules is R3:
  animation in the render path touches the instance buffer, sort input, HLOD and
  persistence; collision proxies add an import format and a cache version.
- **Refs:** decision D-6 of the 2026-11 codebase audit; finding I-13 (issue #1186); the
  audit capability rows "Keyframe / 4D animation" and "Collision / raycast proxies".
- **Related ADRs:** [One route, many demand producers](adr-one-route-many-producers.md)
  (decision 1: static, movable and interactive objects in one frame);
  [Hierarchical LOD for streamed splat worlds](adr-hlod-streaming.md).
- **Paths** are relative to `modules/gaussian_splatting/`. Each claim below was re-read at
  `b58bbe35d1f`.

This record makes two separate decisions, animation and collision. Each has its own options
and its own entry under [Owner decision](#owner-decision).

## 1. Problem

### 1.1 Animation has no render consumer

`GaussianAnimationStateMachine` is a registered class (`animation/animation_state_machine.cpp`,
1,119 lines, plus `animation/keyframe_interpolator.cpp`, 360 lines, and
`core/gaussian_data_animation.cpp`). `GaussianData` binds `set_animation_state_machine`,
`update_animation` and the `get_animated_*` samplers, so scripts can sample it. No part of the
renderer, the nodes or the shaders reads it; [Animation](../features/animation.md) states
that nothing renders it. The only engine reader is the HLOD runtime cut
(`lod/hlod_runtime_cut.cpp`), which refuses a source that has an animation:
"Source animation and runtime overlays are unsupported". The serializer still writes and
reads an `ANIMATION_DATA` chunk (`persistence/gaussian_scene_serializer.cpp`).

What moves on screen today is node transforms, a project-wide wind field and at most
`GS_MAX_SPHERE_EFFECTORS` (4) sphere effectors per renderer. There is no keyframed or
skinned splat animation (I-13).

### 1.2 There is no collision proxy

No code in the module generates collision or raycast geometry from splats, and no page under
`docs/features/` or `docs/user/` describes one. Splats are not physics geometry. A
first-person exploration game needs walkable surfaces on a scanned house.

## 2. Options

### 2.1 Animation

- **Option A1: consume the state machine in the render path.**
  - Cost: an R3 project. A design ADR for rigid-part versus skinned animation, GPU sampling,
    the sort input, HLOD integration, persistence versioning and a per-frame byte budget,
    then several R3 slices.
  - Enables: keyframed and 4D splat animation in games; the existing 1,500 lines gain a
    consumer.
  - Limits: the HLOD refusal for animated sources must be lifted for animated objects to
    share a frame with a streamed world.
- **Option A2: remove the machinery until a game needs it.**
  - Cost: a staged removal of a public class, its `GaussianData` bindings, its persistence
    chunk, tests and docs.
  - Enables: less unreachable code; the HLOD refusal clause goes away.
  - Limits: no splat animation in v1.0; re-adding it later starts from a design, not from
    this code.

**Audit recommendation: A2**, remove the unused animation machinery.

### 2.2 Collision

- **Option C1: bake mesh collision proxies at import.**
  - Cost: an R3 project: proxy kind, storage format, importer option, cache version,
    per-chunk proxies for streamed worlds.
  - Enables: walkable scanned scenes without manual work per level.
  - Limits: proxy quality depends on the bake; thin or noisy scan regions need a fallback.
- **Option C2: level designers author collision by hand.**
  - Cost: no engine work; manual work in every scene.
  - Enables: exact control where it matters.
  - Limits: does not scale to large scanned worlds.

**Audit recommendation: C1**, because baking is cheaper than hand authoring for a scanned
house.

## Owner decision

### Animation (D-6a)

- **Option chosen:** **A1, keep animation and wire it into the render path.**
- **Decided by:** the owner, 2026-11-10.
- **Recorded in:** the owner's approving review on the pull request that adds this ADR.
- **Rejected:** A2, the audit recommendation. Removal would have deleted about 1,500 lines
  with no consumer and the HLOD refusal clause, at the price of no splat animation in v1.0.
  The owner keeps animation in v1.0 and accepts the R3 render work it needs.
- **Follow-up:** CLN-SH06 (epic). CLN-SH06-1 writes the animation render ADR first, then
  CLN-SH06-2 to CLN-SH06-5 implement it.

### Collision (D-6b)

- **Option chosen:** **C1, bake collision proxies at import.**
- **Decided by:** the owner, 2026-11-10.
- **Recorded in:** the owner's approving review on the pull request that adds this ADR.
- **Rejected:** C2. Hand authoring needs no engine work but does not scale to scanned
  worlds.
- **Follow-up:** CLN-SH04 (epic). CLN-SH04-1 writes the collision-proxy bake ADR first, then
  CLN-SH04-2 to CLN-SH04-4 implement it.

## 3. Consequences

### 3.1 For the codebase

- `GaussianAnimationStateMachine`, its `GaussianData` bindings and the `ANIMATION_DATA`
  persistence chunk stay. Nothing is deprecated or removed under this decision.
- The HLOD runtime cut's refusal of animated sources becomes a work item of the animation
  track, not a permanent limit.
- Collision proxies become an import output. The PLY import cache is versioned
  (`PLY_CACHE_VERSION` in `io/ply_loader.cpp`), so the bake needs a version bump; the
  collision-proxy ADR (CLN-SH04-1) fixes the format.

### 3.2 For the cleanup plan

| Task | Effect |
| --- | --- |
| CLN-SH06 and children | Confirmed. |
| CLN-SH04 and children | Confirmed. |
| CLN-AR09-6, CLN-AR09-7, CLN-AR09-8 (animation removal) | Void. |
| Finding I-13 | Moves from CLN-AR09 to CLN-SH06: it is closed by rendering the animation, not by removing it. |

## 4. Out of scope

- Any code, shader, format or test change. Each follow-up task cites this ADR.
- The animation render design: rigid-part or skinned, where sampling runs, how animated
  sources join the HLOD route, the persistence version and the byte budget (CLN-SH06-1).
- The collision proxy design: mesh or other proxy kind, storage, per-chunk proxies, size
  budget (CLN-SH04-1).
- The other slices of the CLN-AR09 dead-code epic (CLN-AR09-1 to CLN-AR09-5).
