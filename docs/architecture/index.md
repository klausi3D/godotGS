# Architecture Documentation

Use this folder for architecture-level documentation and deep technical references.

## Canonical Entry Points

- [Architecture overview](overview.md)
- [Render pipeline architecture](render-pipeline.md)
- [Culling and hierarchy](culling-and-hierarchy.md)
- [Lighting system architecture](lighting-system.md)
- [Gaussian ProjectSettings contract](gaussian-project-settings-contract.md)
- [Unified Gaussian pipeline refactor plan](gaussian-pipeline-unification-plan.md)
- [Resident-instanced renderer contract](gaussian-resident-instanced-contract.md)
- [Gaussian pipeline deprecation and deletion plan](gaussian-pipeline-deprecation-deletion-plan.md)

## Module Deep References

- [Module architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md)
- [Memory subsystem and residency invariants](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
- [Stage-first ownership inventory](stage-first-ownership-inventory.md)

## Architecture Decision Records

ADRs live in this folder as `adr-<slug>.md`. There is no number sequence and no separate
registry: the list below is the only hand-maintained index, and it had **drifted to 1 of 9
entries** before this section was added. A hand-written list of things that must stay
complete is already broken — if it drifts again, generate it from `docs/architecture/adr-*.md`
rather than patching it by hand.

- [Advisory lane result ledger](adr-advisory-lane-ledger.md)
- [Direct editor downloads](adr-editor-download-picker.md)
- [Decompose GaussianSplatNode3D](adr-decompose-node3d.md)
- [Decompose the renderer facade into owned sub-contexts](adr-decompose-renderer-facade.md)
- [Decompose the scene director](adr-decompose-scene-director.md)
- [Decompose the streaming system](adr-decompose-streaming-system.md)
- [Explicit, signed splat colour encoding (SH sign + DC contract)](adr-splat-colour-encoding.md) — *Accepted* (2026-09-23; slice 1 landed in #1062)
- [Complete v1 splat data contract: payloads, snapshots and lossless persistence](adr-v1-splat-data-contract.md)
- [Hierarchical LOD for streamed splat worlds](adr-hlod-streaming.md) — *Accepted* (2026-10-02; Refs #1131; landed in #1293 as a single-instance pilot; scale is designed in the worlds-beyond-VRAM ADR below)
- [Worlds beyond VRAM: compressed streaming, chunk streaming and HLOD as one design](adr-beyond-vram-worlds.md) — *Proposed* (2026-11-04; Refs #1131)
- [One route, many demand producers: the world and node instances in one frame](adr-one-route-many-producers.md) — *Accepted* (maintainer decisions 2026-11-04; Refs #788, #855, #1131; adopts #788 Option 2; route retirement decision D-2, decided by the owner 2026-11-10: one route; acceptance of D-2 pending the owner's approving review and merge of the PR that adds it)
- [What painterly is and where it lives](adr-painterly-direction.md) — *Accepted* (decision D-1, decided by the owner 2026-11-10: per-splat stylisation only; acceptance pending the owner's approving review and merge of the PR that adds it)
- [v1.0 scope for splat animation and collision](adr-v1-animation-collision-scope.md) — *Accepted* (decision D-6, decided by the owner 2026-11-10: animation kept and rendered; collision proxies baked at import; acceptance pending the owner's approving review and merge of the PR that adds it)
- [Import importance pruning](adr-import-importance-pruning.md)
- [Import input hardening](adr-import-input-hardening.md)
- [Overflow drop telemetry](adr-overflow-drop-telemetry.md)
- [Required gate evaluates the PR's own diff](adr-gate-evaluates-pr-diff.md)
- [Phase-1 guard hardening: a closed cluster of remediations](adr-phase1-guard-hardening.md)
- [Single route per frame: world/instance node coexistence](adr-single-route-per-frame-node-coexistence.md)
- [Test quarantine manifest](adr-test-quarantine-manifest.md)
- [The candidate gate must distinguish measured evidence from defaulted evidence](adr-open-world-proof-content-validation.md)
- [Widen the public-alpha envelope to streaming open worlds](adr-alpha-envelope-widened-to-streaming.md)
