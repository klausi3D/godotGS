# Architecture Overview

This page is the canonical high-level architecture entrypoint.

<figure class="gs-flow" markdown="1">
  <ol class="gs-flow__steps" aria-label="Source assets to viewport">
    <li><strong>Source files</strong><small>Import or load</small></li>
    <li><strong>Scene state</strong><small>Nodes, assets and worlds</small></li>
    <li><strong>Renderer</strong><small>Cull → sort → raster</small></li>
    <li><strong>Viewport</strong><small>Composite the result</small></li>
  </ol>
  <figcaption>Assets enter scene state, then the renderer processes visible splats and composites them into the viewport.</figcaption>
</figure>

## Subsystem Map

The module is broader than the renderer-only path. The authoritative build list in
[SCsub](../../modules/gaussian_splatting/SCsub) currently compiles these module source
directories for the normal module build:

- [animation](../../modules/gaussian_splatting/animation/)
- [asset_management](../../modules/gaussian_splatting/asset_management/)
- [compute](../../modules/gaussian_splatting/compute/)
- [core](../../modules/gaussian_splatting/core/)
- [interfaces](../../modules/gaussian_splatting/interfaces/)
- [io](../../modules/gaussian_splatting/io/)
- [lod](../../modules/gaussian_splatting/lod/)
- [logger](../../modules/gaussian_splatting/logger/)
- [nodes](../../modules/gaussian_splatting/nodes/)
- [painterly](../../modules/gaussian_splatting/painterly/)
- [persistence](../../modules/gaussian_splatting/persistence/)
- [resources](../../modules/gaussian_splatting/resources/)
- [renderer](../../modules/gaussian_splatting/renderer/)

Editor builds also compile [editor](../../modules/gaussian_splatting/editor/), test
builds compile [tests](../../modules/gaussian_splatting/tests/), and shader headers are
generated from the compute/shader SCsubs before module objects are built.

- Registration/lifecycle: [../../modules/gaussian_splatting/register_types.cpp](../../modules/gaussian_splatting/register_types.cpp)
- Build metadata consistency: [../../modules/gaussian_splatting/CMakeLists.txt](../../modules/gaussian_splatting/CMakeLists.txt), [../../modules/gaussian_splatting/tests/check_build_metadata_consistency.py](../../modules/gaussian_splatting/tests/check_build_metadata_consistency.py)
- Render stage contracts: [../../modules/gaussian_splatting/renderer/render_types/render_pipeline_io_types.h](../../modules/gaussian_splatting/renderer/render_types/render_pipeline_io_types.h), [../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp](../../modules/gaussian_splatting/renderer/render_pipeline_stages.cpp)
- CI/release evidence: [../../.github/workflows/gaussian_production_gates.yml](../../.github/workflows/gaussian_production_gates.yml), [../../.github/workflows/release_builds.yml](../../.github/workflows/release_builds.yml)

## Source-of-Truth Hierarchy

When public architecture, compatibility, or release wording conflicts with implementation
or automation, trust the sources in this order:

1. Build membership: `modules/gaussian_splatting/SCsub`, checked against `CMakeLists.txt`.
2. Public runtime/editor surface: `modules/gaussian_splatting/register_types.cpp`.
3. Render behavior: stage result contracts, renderer stage implementations, and runtime diagnostics tests.
4. Evidence claims: GitHub Actions workflows plus current run artifacts.
5. Roadmap and planning docs: useful intent, but not evidence for current behavior.

## Detailed Architecture Docs

- [Render pipeline details](render-pipeline.md)
- [Lighting and shadows details](lighting-system.md)
- [Unified Gaussian pipeline refactor plan](gaussian-pipeline-unification-plan.md)
- [Gaussian pipeline deprecation and deletion plan](gaussian-pipeline-deprecation-deletion-plan.md)
- [Refactor phase runner](refactor-phase-runner.md)
- [Renderer refactor memory journal](gaussian-renderer-refactor-memory.md)
- [Module-wide architecture map](../../modules/gaussian_splatting/ARCHITECTURE.md)
- [Memory and residency invariants](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)

## Data Flow (High Level)

1. Source asset is imported/loaded.
2. Node and asset state are registered with runtime systems.
3. Visibility, sorting, and raster/composite stages execute.
4. Debug/performance counters are emitted for diagnostics.

## Debugging and Performance

- [Timing metrics reference](../timing_metrics_reference.md)
- [Recurring issues](../troubleshooting/recurring-issues.md)
