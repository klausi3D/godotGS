# Memory Subsystem Guide

Related docs: [ARCHITECTURE](ARCHITECTURE.md), [READING_ORDER](READING_ORDER.md), [ABBREVIATIONS](ABBREVIATIONS.md), [README](README.md)

This module has two distinct GPU memory paths that share data but serve different runtime modes. The goal is to keep budget logic centralized while allowing each path to manage its own buffers.

## High-Level Layout

```
Resident path (non-streaming)
  GaussianSplatRenderer
    -> ResidentInstanceContractPublisher (resident atlas storage: resident_atlas_gaussian_buffer)
    -> GPUBufferManager (sort-key + sorted-indices buffers only; its own gaussian_buffer is dead-by-default)

Streaming path
  GaussianSplatRenderer
    -> GaussianStreamingSystem (visibility + budget)
        -> StreamingUploadPipeline (pack threads + upload queue -> persistent_buffer)
        -> GaussianMemoryStream (optional, diagnostics-only proxy; not an upload path)
```

## Components and Responsibilities

### ResidentInstanceContractPublisher (resident atlas storage)
- **Files**: `renderer/resident_instance_contract_publisher.h`, `renderer/resident_instance_contract_publisher.cpp`
- **Role**: Owns the resident path's real GPU storage. Builds and uploads the
  `resident_atlas_gaussian_buffer` (plus its quantized variant) that the
  instance pipeline actually binds, applying `ResidentAtlasBudget` importance
  subsetting when the asset exceeds the resident VRAM cap.
- **Entry point**: `GaussianSplatRenderer::_publish_resident_direct_data_contract()`
  calls `ResidentInstanceContractPublisher::publish_resident_direct_data_contract()`.

### GPUBufferManager (sort-key / sorted-indices buffers)
- **Files**: `renderer/gpu_buffer_manager.h`, `renderer/gpu_buffer_manager.cpp`
- **Role**: Allocates the double-buffered sort-key and sorted-indices buffers
  used by both paths. Its own resident `gaussian_buffer` allocation is an
  opt-in manual-upload path that production never enables —
  `renderer/render_resource_orchestrator.cpp` always initializes it with
  `allocate_gaussian_buffer=false`, and `upload_gaussian_data()` has no
  production caller (see `renderer/gpu_buffer_manager.cpp:147-155`). Real
  resident Gaussian data comes from `ResidentInstanceContractPublisher` above.
- **Memory tracking**: Provides `get_memory_usage_mb()` as a size estimate, but does **not** enforce budgets.

### StreamingUploadPipeline (streamed uploads)
- **Files**: `core/streaming_upload_pipeline.h`, `core/streaming_upload_pipeline.cpp`
- **Role**: Owned by `GaussianStreamingSystem` (its `upload_pipeline` member).
  Runs pack worker threads that snapshot/compress chunk payloads plus an
  upload queue that writes them into the streaming path's `persistent_buffer`
  (see [Persistent Buffer Right-Sizing](#persistent-buffer-right-sizing)).
  This is the real streaming upload path.
- **Memory tracking**: Exposes pack/upload telemetry (bytes, queue depth, latency) via `PackTelemetry`; does **not** decide budgets.

### GaussianMemoryStream (optional diagnostics proxy)
- **Files**: `renderer/gpu_memory_stream.h`, `renderer/gpu_memory_stream.cpp`
- **Role**: Not part of the production upload path. `GaussianStreamingSystem::attach_memory_stream()`
  stores it as `memory_stream_proxy`, which only receives `begin_frame()`/`end_frame()`
  calls and reports `get_task_debug_state()` for diagnostics — it does not pack
  or upload chunk data itself. Actual streamed uploads flow through
  `StreamingUploadPipeline` above.
- **Memory tracking**: Reports allocated/used MB and efficiency for its own (diagnostics-only) buffers; does **not** decide budgets.

### GaussianStreamingSystem + VRAMBudgetRegulator (budgeting)
- **Files**: `core/gaussian_streaming.h`, `core/gaussian_streaming.cpp`
- **Role**: Owns VRAM budget policy and eviction/LOD decisions. This is the **only** place that regulates VRAM budgets.
- **Key structs**: `VRAMBudgetConfig`, `VRAMBudgetRegulator`, `BudgetState`.
- **Persistent buffer sizing**: see [Persistent Buffer Right-Sizing](#persistent-buffer-right-sizing) below.

## Budget Configuration Flow

1. **Defaults** are defined in ProjectSettings via `core/gaussian_splat_manager.cpp`.
2. **Tier presets** apply caps through `QualityTierConfig` and `GaussianSplatNode3D::_apply_quality_tier_limits`.
3. **Per-node overrides** are assembled in `GaussianSplatNode3D::_apply_renderer_settings` and passed into the streaming system via `ConfigOverrides`.
4. **Streaming system** applies overrides to the `VRAMBudgetRegulator` and drives eviction based on usage.

This flow prevents duplication: only the streaming system enforces VRAM budget policy, while buffer managers expose usage stats.

## Persistent Buffer Right-Sizing

The streaming path keeps **one** GPU storage buffer (`persistent_buffer`,
sized once at `GaussianStreamingSystem::initialize()`) that backs every
resident chunk via the atlas allocator. Sizing policy:

1. Start from `asset_chunks = chunks.size()` — the actual number of chunks
   the loaded asset requires, **not** the regulated maximum.
2. Add a 25 percent growth headroom (`MAX(2, asset_chunks / 4)`) so the first
   eviction-pressure event does not immediately force a grow.
3. Clamp to `MAX(initial_capacity, STREAMING_DEFAULT_MIN_CHUNKS_IN_VRAM)` so
   tiny assets still get a usable working set.
4. Cap at `effective_max_chunks` (the budget regulator's current ceiling) so
   the buffer can never exceed the regulated maximum.

The resulting size is recorded as `streaming_initial_capacity` and the buffer
is named `GS_Streaming_PersistentBuffer` for tooling visibility. See
`GaussianStreamingSystem::initialize` in `core/gaussian_streaming.cpp` for the
exact computation.

### Growth Path

When the atlas allocator reports it cannot fit the requested loaded-chunk
count, `GaussianStreamingSystem::_try_grow_persistent_buffer_for_atlas_pressure`
calls `_grow_persistent_buffer(target)`. Growth:

- Allocates a larger storage buffer on the upload device.
- Copies the live region (`persistent_buffer_size` bytes) from the old
  buffer into the new one via `RenderingDevice::buffer_copy`, so currently
  resident chunks remain valid.
- Frees the old buffer, updates `persistent_buffer_size`, and resizes the
  atlas allocator with `resize_preserve` so existing slot indices stay
  stable across the grow.
- Refuses any grow that would exceed `UINT32_MAX` bytes (the
  `RenderingDevice` 32-bit addressing limit) or the regulated chunk ceiling.

Each successful grow increments `streaming_grow_count`.

### Diagnostics Surface

`RenderDiagnosticsOrchestrator` exposes three persistent-buffer metrics
(`renderer/render_diagnostics_orchestrator.cpp`):

| Metric | Meaning |
| --- | --- |
| `streaming_initial_capacity` | Chunks reserved at init (post right-sizing). |
| `streaming_current_capacity` | Chunks the persistent buffer currently fits. |
| `streaming_grow_count` | Number of in-place grows since init. |

A non-zero `streaming_grow_count` for a stable scene is a tuning signal:
either the asset's chunk count was under-estimated at import time, or the
budget regulator is being driven harder than the initial headroom can
absorb. Neither is a correctness bug; the grow path is the supported
mechanism for handling it.

User-facing summary of these settings and metrics:
[Performance Dashboard](../../docs/performance/index.md).

## When to Use Which Path

- **Resident path** (`ResidentInstanceContractPublisher`): small/medium datasets, no streaming, lower per-frame overhead.
- **Streaming path** (`StreamingUploadPipeline` + `GaussianStreamingSystem`): large datasets, dynamic loading, budget-aware eviction.

## Debugging and Metrics

- **Budget warnings**: `GaussianStreamingSystem::is_vram_budget_warning_active()`
- **Budget stats**: `GaussianStreamingSystem::get_vram_debug_stats()`
- **Stream usage**: `StreamingUploadPipeline::telemetry` (pack/upload bytes, queue depth, latency)
- **Resident usage**: `GPUBufferManager::get_memory_usage_mb()` (sort-key/index buffers only; resident atlas size is tracked via `resident_atlas_gaussian_buffer_size` on the renderer's resource state)

For the per-owner lifetime contract of each buffer in this subsystem (who creates, who destroys, idempotency, threading), see [`docs/architecture/renderer-lifetime-ownership.md`](../../docs/architecture/renderer-lifetime-ownership.md).

## Notes for Future Refactors

- Avoid moving budget logic into `GPUBufferManager` or `GaussianMemoryStream`; the regulator in `core/gaussian_streaming.*` is the single source of truth.
- If a unified memory subsystem is introduced later, preserve the separation between **budget policy** and **buffer allocation**.
- The persistent buffer is **right-sized from the loaded asset**, not from the regulated maximum. Do not regress this to a fixed-cap allocation: large regulated ceilings are common (gigabytes), but most scenes load a fraction of that, and an oversized persistent buffer is pure waste. Growth is wired into the eviction pressure path; trust it.
- Keep `streaming_initial_capacity`, `streaming_current_capacity`, and `streaming_grow_count` flowing through `RenderDiagnosticsOrchestrator`. They are the only signal a user has that the right-sizing heuristic chose well.
