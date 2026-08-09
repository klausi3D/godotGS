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
- [Decompose GaussianSplatNode3D](adr-decompose-node3d.md)
- [Decompose the renderer facade into owned sub-contexts](adr-decompose-renderer-facade.md)
- [Decompose the scene director](adr-decompose-scene-director.md)
- [Decompose the streaming system](adr-decompose-streaming-system.md)
- [Import importance pruning](adr-import-importance-pruning.md)
- [Import input hardening](adr-import-input-hardening.md)
- [Overflow drop telemetry](adr-overflow-drop-telemetry.md)
- [Single route per frame: world/instance node coexistence](adr-single-route-per-frame-node-coexistence.md)
- [Test quarantine manifest](adr-test-quarantine-manifest.md)
