# Renderer Lifetime Ownership

Work package #352 (Renderer Lifetime Proof). First written against `b7e7f05a77` (2026-05-22).

> **Status: re-verified against the code on 2026-10-01 (`eff00db450c`).** The first version
> cited about 155 `file:line` anchors, and most had drifted. This revision cites symbols
> (`Class::method`, `Class::member`, plus the file) instead of line numbers. It also corrects
> claims that no longer hold. Claims that were not re-checked are marked **unverified**.
> Destruction orders below were read from the function bodies at that commit. They were not
> exercised at run time for this revision.
>
> Follow-up PRs of #352 that have landed since the first version:
>
> - Streaming failed-init guards, #383. Its commit message names a separate "PR 1b" for the
>   director half-initialization that caused the 602-event cascade. No landed commit for PR 1b
>   was found.
> - PR 3: the lifetime-proof fixture `tests/test_renderer_lifetime_proof.h`, #386.
> - PR 4: explicit `SharedWorld` teardown, #387, squashed into #386.
> - PR 5: the `OutputCompositor` framebuffer LRU bound, #388.
> - PR 6: release of module-owned static `StringName`s, #389.

## Purpose

This document lists the GPU-resource owners inside the `gaussian_splatting` module. For each
one it records who creates the resource, who destroys it, whether destruction is idempotent, and
which mutexes or threading rules guard the owner. It exists because leaks such as the F6 reload
leak and failed-init cascades can only be reasoned about against one ownership map.

The doc complements:

- [`modules/gaussian_splatting/ARCHITECTURE.md`](../../modules/gaussian_splatting/ARCHITECTURE.md)
  — subsystem composition and data flow.
- [`modules/gaussian_splatting/MEMORY_SUBSYSTEM.md`](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
  — VRAM-budget policy and persistent-buffer sizing.
- [`docs/architecture/stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md)
  — stage-level write ownership for the decomposition work package (#356). That page is a
  2026-05-20 snapshot.

## How to read this doc

Every owner subsection has six fields:

1. **Owner identity**: the class and the file that declares it.
2. **What it owns**: the concrete GPU resources by member name.
3. **Created where**: the function that allocates them, with thread context where known.
4. **Destroyed where**: the destructor or shutdown function, its release order, and whether a
   second call is safe (and which guard makes it safe).
5. **Threading / locking**: the mutexes the owner uses.
6. **Known leak vectors / open gaps**.

Paths are relative to `modules/gaussian_splatting/`.

## Top-level ownership graph

```
GaussianSplatManager (singleton)                    core/gaussian_splat_manager.h
├── primary_local_device / shared_submission_device (std::atomic<RenderingDevice *>)
├── gaussian_buffers (HashMap<ObjectID, BufferEntry>), buffer_lookup, gaussian_buffer_owner_devices
├── dynamic_asset_cache, dynamic_asset_owner_devices
├── active_nodes (HashSet<ObjectID>)
└── submission_mutex (static SafeBinaryMutex)

GaussianSplatSceneDirector (singleton)              core/gaussian_splat_scene_director.h
└── world_registry (WorldRegistry)
    └── worlds: HashMap<RID, SharedWorld>
        └── SharedWorld
            ├── renderer_owner (RendererLifecycleOwner) ── Ref<GaussianSplatRenderer> renderer
            ├── instance_store (InstanceStore)          ── asset_records
            ├── sphere_effector_store, submission_store
            └── lod_cache_owner (LODCacheOwner)

GaussianSplatRenderer                               renderer/gaussian_splat_renderer.h
├── subsystem_state (SubsystemState)                renderer/render_types/render_facade_state_types.h
│   ├── Ref<RenderDeviceManager> device_manager
│   ├── Ref<DebugOverlaySystem>, Ref<InteractiveStateManager>, Ref<TileRasterizer> rasterizer
│   ├── Ref<GPUCuller> gpu_culler, Ref<OutputCompositor> output_compositor
│   ├── Ref<GPUSortingPipeline> sorting_pipeline, Ref<OverflowAutoTuner>
│   └── Ref<PainterlyRenderer>, Ref<PainterlyMaterialManager>
├── tile_renderer_state (TileRendererState)         ── Ref<TileRenderer> renderer
├── test_data_state (TestDataState)                 ── test vertex/position/scale/rotation/SH buffers
├── shadow_output_compositor (Ref<OutputCompositor>), shadow_blit_state (ShadowBlitState)
├── render_thread_dispatcher (std::unique_ptr<IRenderThreadDispatcher>)
├── resource_orchestrator (RenderResourceOrchestrator)
│   ├── resource_state (ResourceState)              ── Ref<GPUBufferManager> buffer_manager,
│   │                                                  deletion_queue, 8 instance-pipeline buffers,
│   │                                                  5 resident_* contract buffers
│   └── pipeline_state (GaussianSplatRenderer::PipelineState) ── gaussian_shader(_version/_source)
├── data orchestrator (RenderDataOrchestrator)
│   └── streaming_state (StreamingState)            renderer/render_types/render_state_types.h
│       ├── Ref<GaussianMemoryStream> memory_stream
│       ├── Ref<GaussianStreamingSystem> current_streaming_system
│       └── registered_gaussian_buffer (handed back to the manager on teardown)
└── sorting orchestrator (RenderSortingOrchestrator)
    └── sorting_state (SortingState)                ── Ref<IGPUSorter> gpu_sorter

GaussianStreamingSystem                             core/gaussian_streaming.h
├── persistent_buffer (RID; chunks occupy buffer_slot ranges inside it)
├── quantization_buffer (plain member)
├── upload_pipeline (StreamingUploadPipeline: pack threads, pending uploads)
└── global_atlas_registry (StreamingGlobalAtlasRegistry)
    └── asset_meta_buffer, chunk_meta_buffer, asset_chunk_index_buffer

TileRenderer                                        renderer/tile_renderer.h
├── TileRenderTargets, TileShaderResources, TileGlobalSortResources (Ref<IGPUSorter> sorter)
├── TileUniformBuffers, TileProjectionBuffers, TileSHCacheBuffers
├── TileSubpixelHistoryBuffers, TileSubpixelVisibilityBuffers
└── TileResourceController (tracks output colour/depth)

GaussianSplatNode3D (per node, scene side)          nodes/gaussian_splat_node_3d.h
├── Ref<GaussianSplatRenderer> renderer, Ref<GaussianData> renderer_data
├── render_instance (RS instance RID), gaussian_base (RS base RID)
└── cached_viewport_render_target / _render_texture / _texture
```

## Owners

### GaussianSplatManager

1. **Owner identity:** `GaussianSplatManager` (`core/gaussian_splat_manager.h`), a process-wide
   singleton (`GaussianSplatManager::singleton`). The constructor fails if a singleton already
   exists.
2. **What it owns:** `primary_local_device` and `shared_submission_device`
   (`std::atomic<RenderingDevice *>`). The buffer registry `gaussian_buffers`, with
   `buffer_lookup` and `gaussian_buffer_owner_devices`. `dynamic_asset_cache` and
   `dynamic_asset_owner_devices`. `active_nodes`. The static `submission_mutex`. The deferred
   device-destroy dispatcher `local_device_destroy_dispatcher` and its pending device pointers.
3. **Created where:**
   - `initialize_gaussian_splatting_module()` (`register_types.cpp`), at the SCENE level: `memnew`,
     then `initialize_module()`, then `add_singleton("GaussianSplatManager")`.
   - The constructor runs under `GS_STARTUP_SCOPE("manager_construct")`. It *requests* the local
     devices through `_request_primary_local_device()` and `_request_shared_local_device()`
     (scopes `device_request_primary` / `device_request_shared`). These post
     `_create_local_device_on_render_thread` to the render thread, unless RenderDoc compatibility
     mode is on. The devices themselves are created on the render thread.
   - `GaussianSplatManager::get_primary_rendering_device()` returns the main RenderingServer
     device. It falls back to `primary_local_device` only when that is unavailable.
4. **Destroyed where:**
   - Module shutdown (`uninitialize_gaussian_splatting_module()`) calls `remove_singleton`, then
     `finalize_module()`, then `memdelete`. The director is deleted *before* the manager (#628).
   - `GaussianSplatManager::finalize_module()` is a soft shutdown: `_release_registered_resources()`,
     then `active_nodes.clear()` under `active_nodes_mutex`, then `_disconnect_frame_callbacks()`.
   - `~GaussianSplatManager()` then runs, in order:
     1. `_disconnect_frame_callbacks()`
     2. `active_nodes.clear()` under `active_nodes_mutex`
     3. `_release_registered_resources()`
     4. `_destroy_local_devices()`
     5. `singleton = nullptr` (only if `singleton == this`)
   - **Idempotent:** yes.
     - `_release_registered_resources()` returns early once `registered_resources_released` is
       set and both maps are empty. Entries whose owner device is unknown are kept and retried on
       the next call.
     - `_disconnect_frame_callbacks()` returns early when `frame_callbacks_connected` is false.
     - `_destroy_local_devices()` is single-shot because it `exchange(nullptr)`s both device
       atomics and returns if both were already null. The level-4 mutexes protect the dispatch
       handoff, not idempotency.
5. **Threading / locking:** this is the owner of the L1–L4 lock hierarchy, documented in the
   header comment of `core/gaussian_splat_manager.h`:
   - L1: static `submission_mutex`.
   - L2: `resource_maps_mutex` (the buffer and dynamic-asset maps).
   - L3: `active_nodes_mutex`.
   - L4: `local_device_destroy_request_mutex` and `local_device_destroy_pending_mutex`.

   Development builds validate the order at run time through the `GS_LOCK_ORDER_GUARD` macro
   (`_GSLockLevelGuard`, thread-local `_gs_held_lock_level`). The header comment still calls this
   `_gs_lock_level_guard`.
6. **Known leak vectors / open gaps:**
   - `_dispatch_local_device_destroy_on_render_thread()` returns false in three cases: no
     RenderingServer, already on the render thread, or render loop disabled. In those cases
     `_destroy_local_devices_immediate()` runs on the calling thread.
   - If the dispatch is submitted but times out, it returns true with a warning and does not
     destroy anything immediately. Destruction is left to `_destroy_local_devices_on_render_thread()`.
     If that callback never runs, the local devices are not freed. This follows from reading the
     code and was **not observed** at run time.

### GaussianSplatSceneDirector

1. **Owner identity:** `GaussianSplatSceneDirector` (`core/gaussian_splat_scene_director.h`),
   a singleton. The constructor sets `singleton` only if it is still null, and reports no error
   otherwise.
2. **What it owns:** `world_registry` (`WorldRegistry`). It holds `worlds`, a
   `HashMap<RID, SharedWorld>` keyed by scenario, introduced by #747 (#610 S8). Each
   `SharedWorld` holds:
   - `scenario`;
   - `renderer_owner` (`RendererLifecycleOwner`), whose `renderer` member is the strong
     `Ref<GaussianSplatRenderer>`. Releasing this reference is how a world drops its renderer and
     the renderer's whole GPU graph;
   - `instance_store` (`InstanceStore`, which holds `asset_records`);
   - `sphere_effector_store`, `submission_store` and `lod_cache_owner`.

   The old `scene_effector_multi_match_warned_nodes` member no longer exists.
3. **Created where:**
   - `initialize_gaussian_splatting_module()`, SCENE level, after the manager: `memnew`, then
     `add_singleton("GaussianSplatSceneDirector")`.
   - `SharedWorld` entries are created lazily by
     `GaussianSplatSceneDirector::_get_or_create_world_for_scenario()` through
     `world_registry.insert`.
   - When a renderer is required and none exists, `RendererLifecycleOwner::create()` constructs
     it. GPU `initialize()` is deferred to `_initialize_world_renderer()`.
   - If no `RenderingDevice` is available, the function logs an error once and returns the
     inserted entry with no renderer. Other code paths must tolerate a world without a renderer.
4. **Destroyed where:**
   - `~GaussianSplatSceneDirector()` calls `world_registry.clear()`, then `singleton = nullptr`
     (only if `singleton == this`). The F6 comment about releasing every `SharedWorld` is still
     there. The destructor does **not** take `world_mutex`.
   - Per-world release paths:
     - `_prune_world_if_unused()` moves the renderer reference into the caller's deferred-release
       list and erases the entry. Callers: `unregister_instance`, `update_sphere_effector`,
       `unregister_sphere_effector`, `release_world_submission`, and the public
       `try_prune_world_if_unused` (which `submit_world_submission` and the `NOTIFICATION_PREDELETE`
       handlers of `GaussianSplatNode3D` and `GaussianSplatWorld3D` use).
     - `teardown_world_for_scenario()` (#387) runs this order:
       1. Under `world_mutex`: clear `instance_store` and `sphere_effector_store`, reset
          `submission_store`, `renderer_owner.release()` into a local, erase the world.
       2. Unlock, then `clear_world_submission_contract()`.
       3. The local reference drops, so `~GaussianSplatRenderer` runs outside the lock.

       A missing entry returns early, which makes the call idempotent.
     - `release_all_worlds()` snapshots the keys under the lock and calls
       `teardown_world_for_scenario` for each one. At `eff00db450c` only the GPU test runner
       (`tests/gs_gpu_test_runner.cpp`) calls it. Module shutdown does not; it relies on
       `memdelete` → `world_registry.clear()`.
   - **Idempotent:** yes. `clear()` on an empty registry is a no-op, and
     `teardown_world_for_scenario` returns early for a missing world.
5. **Threading / locking:**
   - `world_mutex` is a `GaussianSplatting::ThreadOwnedMutex`, a recursive mutex that records
     its owner, and is taken through `ThreadOwnedMutexLock`.
   - `world_submission_apply_mutex` is always taken before `world_mutex`, and only in
     `submit_world_submission`.
   - Renderer references are released outside `world_mutex` by design (#611/#628; see
     `RendererContractWorkQueue`).
   - The director does not directly call into the manager's L1–L4 locks while holding
     `world_mutex`. Its one manager call is `get_primary_rendering_device()`, which takes no
     manager mutex. However, `RendererLifecycleOwner::create()` runs the `GaussianSplatRenderer`
     constructor under `world_mutex`. Whether that constructor reaches a manager lock
     transitively is **unverified**, and so is whether every director method takes the lock.
6. **Known leak vectors / open gaps:**
   - **F6 reload leak.** A director that is never destroyed keeps every `SharedWorld`, and so
     every renderer, alive. Since #387 there are explicit per-world release paths (the list above
     and the PREDELETE prune), and the lifetime fixture covers the F6 scenario through
     `teardown_world_for_scenario`. Module shutdown still releases worlds only through
     `memdelete`, without the lock.

### GaussianSplatRenderer

1. **Owner identity:** `GaussianSplatRenderer` (`renderer/gaussian_splat_renderer.h`), a
   `RefCounted` that also implements `IRenderer`, `ISortResultSink` and `ISortBufferHostContext`.
2. **What it owns:** its state buckets do not all live on the renderer object; see the graph
   above. The buckets are:
   - On the renderer: `SubsystemState` (the ten `Ref<>` sub-owners), `TileRendererState`,
     `TestDataState` (vertex, position, scale, rotation and SH buffers), `ShadowBlitState`
     (`shader_source`, `shader`, `shader_owner`, `pipeline_cache`, `sampler`, `sampler_owner`)
     and `shadow_output_compositor`.
   - In `RenderResourceOrchestrator`: `ResourceState`. It holds `buffer_manager`, the
     `deletion_queue`, eight instance-pipeline buffers (`instance_buffer`,
     `instance_grading_buffer`, `instance_visible_chunk_buffer`, `instance_splat_ref_buffer`,
     `instance_counter_buffer`, `instance_chunk_dispatch_buffer`, `instance_indirect_count_buffer`,
     `instance_count_buffer`) and five resident contract buffers (`resident_atlas_gaussian_buffer`,
     `resident_asset_meta_buffer`, `resident_asset_chunk_index_buffer`,
     `resident_chunk_meta_buffer`, `resident_quantization_buffer`). It also holds `PipelineState`
     (`gaussian_shader`, `gaussian_shader_version`, `gaussian_shader_source`,
     `gaussian_shader_initialized`).
   - In `RenderDataOrchestrator`: `StreamingState` (`memory_stream`, `current_streaming_system`,
     `registered_gaussian_buffer`).
   - In `RenderSortingOrchestrator`: `SortingState` (`gpu_sorter`).
3. **Created where:**
   - The constructor `GaussianSplatRenderer(RenderingDevice *)`, under
     `GS_STARTUP_SCOPE("renderer_construct")`, is called from
     `GaussianSplatSceneDirector::RendererLifecycleOwner::create()`.
   - The constructor eagerly instantiates every `SubsystemState` sub-owner except `rasterizer`,
     including `device_manager`, which it also initializes.
   - `buffer_manager` is created in the `RenderResourceOrchestrator` constructor and
     `memory_stream` in the `RenderDataOrchestrator` constructor.
   - `tile_renderer_state.renderer` and `rasterizer` are created in
     `RenderResourceOrchestrator::create_gpu_resources_safe()`, which also initializes
     `GPUBufferManager` under `GS_STARTUP_SCOPE("gpu_buffer_manager_init")`.
   - `shadow_output_compositor` is created on demand by `_ensure_shadow_output_compositor()`.
4. **Destroyed where:**
   - `~GaussianSplatRenderer()` runs, in order:
     1. Disconnect the ProjectSettings `settings_changed` handler.
     2. `GaussianRenderingDiagnostics::unregister_renderer(this)`.
     3. `GaussianSplattingPerformanceMonitors::unregister_splat_renderer(this)`.
     4. `_dispatch_call_on_render_thread_blocking(_teardown_on_render_thread, …, allow_timeout = false)`.
        If the dispatch was submitted, return.
     5. Otherwise, call `_teardown_resources()` synchronously.
   - `GaussianSplatRenderer::_teardown_resources()` is the full release path. Its order:
     1. `teardown_resources_started.compare_exchange_strong(false → true)`. Return if it was
        already set.
     2. `_release_shared_dynamic_asset()`, then `deletion_queue.flush_all()`.
     3. Primary `output_compositor`: `clear_cached_framebuffers()`,
        `clear_viewport_blit_resources()`, `shutdown()`, `unref()`.
     4. `shadow_output_compositor`: the same four calls.
     5. `shadow_blit_state.clear(rd)`.
     6. `registered_gaussian_buffer`: forget its owner, then
        `GaussianSplatManager::unregister_gaussian_buffer()`, then reset it.
     7. TileRenderer: `_forget_tile_renderer_outputs()`, `cleanup()`, `unref()`.
     8. `test_data_state.mesh_instances.clear()`.
     9. `current_streaming_system.unref()`.
     10. `memory_stream->shutdown()`, then `unref()`.
     11. `_invalidate_static_chunk_caches(true)`.
     12. `gpu_culler`, `interactive_state_manager`, `debug_overlay_system`: `shutdown()` then
         `unref()` each, in that order.
     13. `sorting_pipeline`: rebind sink and host context, `release_sort_buffers()`,
         `shutdown()`, `unref()`.
     14. `painterly_renderer->free_painterly_resources(this)`, then `unref()`.
     15. `buffer_manager.unref()`.
     16. `gpu_sorter->shutdown()`, then `unref()`.
     17. `_release_resident_contract_buffers()`: frees the five resident contract buffers
         through their recorded owner (an RID with no owner is just nulled), then zeroes the
         atlas counters.
     18. Only if the device is non-null: `version_free` the gaussian shader, then
         `_free_owned_resource()` on the eight instance-pipeline buffers and then the five
         TestData buffers.
     19. `memdelete(gaussian_shader_source)`.
     20. `device_manager->shutdown()`, snapshot its counts for the lifetime fixture, then
         `unref()`. The device manager is released last.

     `painterly_material_manager`, `overflow_auto_tuner`, `rasterizer` and the orchestrators are
     not released explicitly. They drop when the renderer's members are destroyed.
   - **Idempotent:** yes. The `teardown_resources_started` atomic guarantees one entry.
5. **Threading / locking:**
   - Teardown is dispatched to the render thread when possible (see
     [Thread-safety contracts](#thread-safety-contracts)).
   - The renderer owns no process-wide mutex. It owns a per-instance `render_thread_dispatcher`,
     whose `RenderThreadDispatcher` has its own `dispatch_mutex`.
   - Sub-owners that submit GPU work take `GaussianSplatManager::ScopedSubmissionLock` through
     `acquire_submission_lock()`.
6. **Known leak vectors / open gaps:**
   - If the renderer `Ref<>` is leaked (for example through a leaked director),
     `_teardown_resources()` never runs and every sub-owner leaks with it.
   - When `GPUBufferManager` initialization fails in `create_gpu_resources_safe()`, the
     orchestrator only marks `buffer_manager_initialized = false`. The partial-allocation
     rollback is inside `GPUBufferManager::create_buffers()`, which calls `cleanup_buffers()`.

### RenderDeviceManager

1. **Owner identity:** `RenderDeviceManager` (`interfaces/render_device_manager.h`), a
   `RefCounted` that implements `IRenderDeviceManager`.
2. **What it owns:**
   - The device pointers `main_rd`, `local_rd`, `submission_rd` and the flag `owns_local_rd`.
   - Tracking maps: `resource_owner_map`, `resource_owner_instance_id_map`,
     `resource_ownership_map`, `resource_label_map`, `resource_type_map`, `texture_owner_map`.
   - Shutdown counters `last_shutdown_tracked_resource_count`,
     `last_shutdown_owned_resource_count` and `last_shutdown_borrowed_resource_count`, with their
     accessors.
   - The trace buffers `texture_trace` and `cross_device_ops`, capped at `MAX_TRACE_ENTRIES`
     (1000).
3. **Created where:** instantiated and initialized (`RenderDeviceManager::initialize()`) in the
   `GaussianSplatRenderer` constructor.
4. **Destroyed where:**
   - `~RenderDeviceManager()` calls `shutdown()`.
   - `RenderDeviceManager::shutdown()` runs, in order:
     1. Snapshot the tracked, owned and borrowed counts.
     2. `ERR_PRINT` if any owned resources remain.
     3. Clear the six maps and both traces.
     4. Null `local_rd` if it is owned and differs from `main_rd`.
     5. Null all three device pointers.
     6. Clear `owns_local_rd`.
   - **Idempotent:** for resources, yes. There is no guard, but a second run finds empty maps.
     A second run does overwrite the `last_shutdown_*` counters with zero, and the destructor
     always runs one after the renderer's explicit call. The renderer therefore snapshots the
     counts before `unref()`.
5. **Threading / locking:** no internal mutex. It is used from the renderer's owning thread
   context.
6. **Known leak vectors / open gaps:** the `ERR_PRINT` in `shutdown()` is the only built-in
   signal for un-freed owned RIDs. The lifetime fixture asserts on the snapshot through
   `GaussianSplatRenderer::test_get_last_teardown_rdm_owned_count()`.

### OutputCompositor

1. **Owner identity:** `OutputCompositor` (`interfaces/output_compositor.h`), a `RefCounted`
   that implements `IOutputCompositor`.
2. **What it owns:**
   - Two framebuffer caches in `OutputCacheState`: `cached_framebuffers` (keyed by texture ID)
     and `framebuffer_validation_cache`. Both are LRU-capped at
     `MAX_CACHED_FRAMEBUFFER_FORMATS` (8) since #388.
   - `viewport_blit_variants`, `viewport_blit_samplers`, `viewport_blit_shader_source` and
     `srgb_format_cache`.
   - `viewport_blit_scratch`, an LRU with `VIEWPORT_BLIT_SCRATCH_MAX_PER_KIND` (4) entries per
     kind.
   - `final_render_texture`, plus a `Ref<RenderDeviceManager> device_manager`.
3. **Created where:**
   - The primary compositor is instantiated in the `GaussianSplatRenderer` constructor; the
     shadow compositor in `_ensure_shadow_output_compositor()`.
   - `initialize()` is called by `RenderOutputOrchestrator`, by the raster stage, and by the
     shadow path.
   - Framebuffers are created lazily by `get_cached_framebuffer()`, and scratch entries by
     `_ensure_viewport_blit_scratch()`.
4. **Destroyed where:**
   - `~OutputCompositor()` calls `shutdown()`.
   - `OutputCompositor::shutdown()` runs, in order:
     1. `clear_cached_framebuffers()`
     2. `clear_viewport_blit_resources()`
     3. Delete `viewport_blit_shader_source`.
     4. Reset `final_render_texture`, `output_cache` and the reuse flag.
     5. `initialized = false`.
     6. `rd = nullptr`.

     The renderer calls both clears itself before `shutdown()`, so they run twice.
   - **Idempotent:** yes, but only because the clears over empty maps are no-ops. Nothing
     guards it, and `is_initialized()` does not gate `shutdown()`.
5. **Threading / locking:** single owner, no internal mutex. The renderer serializes calls on the
   render thread.
6. **Known leak vectors / open gaps:** the unbounded framebuffer-cache vector from the first
   version was fixed by #388. The eviction is in `_evict_oldest_cached_framebuffer_if_needed()`
   and `_evict_oldest_framebuffer_validation_if_needed()`.

### GaussianStreamingSystem

1. **Owner identity:** `GaussianStreamingSystem` (`core/gaussian_streaming.h`), a `RefCounted`.
2. **What it owns:**
   - `persistent_buffer` and its right-sizing counters (`streaming_initial_capacity`,
     `streaming_current_capacity`, `streaming_max_capacity`, `streaming_grow_count`).
   - Chunks hold no RID of their own. A `StreamingChunk` (`core/streaming_asset_types.h`) records
     a `buffer_slot` inside `persistent_buffer` and its `upload_device`.
   - Quantization state is plain members (`quantization_buffer`, `quantization_buffer_size`,
     `quantization_gpu_data`, …). There is no `StreamingQuantization` aggregate.
     `_release_quantization_buffer()` is defined in `core/streaming_quantization.cpp`.
   - The `frame_data[RING_BUFFER_FRAMES]` ring (3 frames), `atlas_allocator` (CPU slot
     bookkeeping), `global_atlas_registry`, and `upload_pipeline` (`StreamingUploadPipeline`,
     which owns the pack threads and pending uploads).
3. **Created where:**
   - The constructor runs first.
   - `GaussianStreamingSystem::initialize()` allocates `persistent_buffer` under
     `GS_STARTUP_SCOPE("streaming_persistent_buffer_alloc")`, then builds the atlas registry
     (`streaming_atlas_build_cpu`) and syncs it to the GPU (`streaming_atlas_sync_gpu`).
   - `initialize_empty()` also allocates `persistent_buffer`.
   - `_grow_persistent_buffer()` grows it.
4. **Destroyed where:**
   - `~GaussianStreamingSystem()` runs, in order:
     1. `_layout_hint_reset_state(this)`.
     2. `_stop_pack_threads()`, which delegates to `upload_pipeline`.
     3. `_clear_pending_uploads()`.
     4. Resolve `rd`: `primary_device_override`, then `last_upload_device`, then
        `GaussianSplatManager::get_primary_rendering_device()`, else null.
     5. `_release_persistent_buffer(rd, "destructor")`.
     6. `global_atlas_registry.cleanup(rd)`.
     7. `_release_quantization_buffer(rd, "destructor", false)`.

     The per-chunk free loop of the first version no longer exists.
   - **Idempotent:** yes. `_release_persistent_buffer()` returns early on an invalid RID and
     always clears the RID and size. `_release_quantization_buffer()` returns early on an
     invalid RID.
5. **Threading / locking:** pack threads are stopped first, in the destructor. Whether the rest
   of the system is single-threaded is **unverified**.
6. **Known leak vectors / open gaps:** if `rd` cannot be resolved,
   `_release_persistent_buffer()` and `_release_quantization_buffer()` each `WARN_PRINT` and drop
   the RID without freeing it. The upstream owner that destroyed the device first is charged with
   the leak.

### StreamingGlobalAtlasRegistry

1. **Owner identity:** `StreamingGlobalAtlasRegistry`
   (`core/streaming_global_atlas_registry.h`). It is not `RefCounted`. It is stored by value as
   `GaussianStreamingSystem::global_atlas_registry` and is a friend of that class.
2. **What it owns:**
   - `asset_meta_buffer`, `chunk_meta_buffer` and `asset_chunk_index_buffer`, each with a size
     member.
   - CPU mirrors (`asset_meta_cpu`, `chunk_meta_cpu`, `asset_chunk_index_cpu`) and dirty
     tracking.
   - `global_atlas_state` (`GlobalAtlasState`). It republishes those RIDs plus
     `atlas_gaussian_buffer` and `quantization_buffer`, which are borrowed aliases of the parent
     system's buffers and not owned by the registry.
3. **Created where:** `StreamingGlobalAtlasRegistry::sync_to_gpu()`, called from
   `GaussianStreamingSystem::initialize()`, `initialize_empty()` and
   `_run_streaming_frame_pipeline()`.
4. **Destroyed where:**
   - `StreamingGlobalAtlasRegistry::cleanup(RenderingDevice *)` is called from the parent's
     destructor, and from the re-initialization paths in `initialize()` / `initialize_empty()`
     after a `safe_submit_and_sync`.
   - Its order:
     1. Release the three buffers. Each is freed only if valid; with a null device it
        `WARN_PRINT`s instead of freeing. Each RID and size is then reset.
     2. Clear the CPU mirrors and dirty state.
     3. Zero `global_atlas_state` and the per-asset maxima.
   - **Idempotent:** yes (an `is_valid()` guard per buffer).
5. **Threading / locking:** owned exclusively by `GaussianStreamingSystem`. No locking of its
   own.
6. **Known leak vectors / open gaps:** the parent's null-device case. Handles are dropped with a
   warning per buffer, not silently.

### GPUBufferManager

1. **Owner identity:** `GPUBufferManager` (`renderer/gpu_buffer_manager.h`), a `RefCounted`.
2. **What it owns:**
   - `buffer_sets[BUFFER_COUNT]` (double-buffered, `BUFFER_COUNT` = 2). Each `BufferSet` holds
     the `gaussian_buffer`, `sort_key_buffer`, `sorted_indices_buffer` and `fence` RIDs.
   - `uniform_buffer`, plus `uniform_buffer_device`.
   - `GPUBufferManager::DeferredDeletionQueue` is only a nested type. The manager owns no
     instance of it. The only instance is the renderer's `ResourceState::deletion_queue`.
3. **Created where:**
   - Instantiated in the `RenderResourceOrchestrator` constructor.
   - Initialized by `GPUBufferManager::initialize()` from
     `RenderResourceOrchestrator::create_gpu_resources_safe()`, under
     `gpu_buffer_manager_init`. `initialize()` first cleans up any existing buffers, then calls
     `create_buffers()`, which rolls back with `cleanup_buffers()` on a partial failure.
4. **Destroyed where:**
   - `~GPUBufferManager()` calls `cleanup_buffers()`, which runs:
     1. If nothing is allocated (`_has_allocated_resources()`), reset state and return.
     2. If there is a device, flush the pending submission and `safe_submit_and_sync`.
     3. `_destroy_buffer_set()` for each set.
     4. Free `uniform_buffer`.
     5. `_reset_state(true)`.

     Steps 3 and 4 run under `ScopedSubmissionLock`.
   - The renderer drops its reference in `_teardown_resources()`.
   - **Idempotent:** yes (the `_has_allocated_resources()` early return).
5. **Threading / locking:** allocation and free take `GaussianSplatManager::ScopedSubmissionLock`
   (manager L1).
6. **Known leak vectors / open gaps:** the renderer calls `deletion_queue.flush_all()` near the
   start of `_teardown_resources()`. An RID queued for deferred deletion after that point is not
   freed by the queue. This is the contract the lifetime fixture is meant to protect.

### GaussianMemoryStream

1. **Owner identity:** `GaussianMemoryStream` (`renderer/gpu_memory_stream.h`), a `RefCounted`.
2. **What it owns:**
   - `buffers[BUFFER_COUNT]` (`BUFFER_COUNT` = 3). Each `StreamBuffer` holds one `gpu_buffer`
     and its allocation device (`gpu_allocation_device`, `gpu_allocation_device_id`).
   - `gpu_memory_pool` (CPU-side `MemoryPool` sub-allocation over the buffers).
   - A `Ref<RenderDeviceManager> device_manager`.
3. **Created where:** instantiated in the `RenderDataOrchestrator` constructor.
   `GaussianMemoryStream::initialize()` creates the three buffers through `_create_buffer()`. It
   is called from `RenderDataOrchestrator::update_gpu_buffers_with_real_data()` and from the
   streaming orchestrator.
4. **Destroyed where:**
   - `~GaussianMemoryStream()` calls `shutdown()`. The renderer calls `shutdown()` then
     `unref()` in `_teardown_resources()`.
   - `GaussianMemoryStream::shutdown()` runs:
     1. Return if `rd` is null.
     2. `wait_for_all_uploads()`.
     3. `_destroy_buffer()` for each slot, freeing through `device_manager` when set.
     4. Clear the pool.
     5. `rd = nullptr`.
   - **Idempotent:** yes (the null-`rd` early return).
5. **Threading / locking:** `buffer_mutex` and `pool_mutex` guard the buffers and the pool.
   `StreamBuffer::state` and the ring indices are atomics.
6. **Known leak vectors / open gaps:** none known beyond the device-ordering note under
   `GaussianStreamingSystem`.

### GPUCuller

1. **Owner identity:** `GPUCuller` (`interfaces/gpu_culler.h`), a `RefCounted` that implements
   `ICuller`.
2. **What it owns:**
   - `shader`, `frustum_shader_version` and `pipeline`. `shader` comes from the shader version
     and is released by `version_free`, not freed directly.
   - Buffers `param_buffer`, `counter_buffer`, `visible_buffer`, `distance_buffer`,
     `importance_buffer`, `consolidated_buffer` and `instance_param_buffer`, with their owner
     devices `resource_device` and `instance_resource_device`.
   - `instance_uniform_set_cache`, `batched_readback` (`Ref<BatchedAsyncReadback>`) and the CPU
     staging vectors.
   - `culling_state.gpu_visible_indices_buffer` is a borrowed alias of the instance
     `visible_chunk_buffer`, not owned.
3. **Created where:**
   - Instantiated in the renderer constructor.
   - `GPUCuller::initialize()` returns early if already initialized. It calls `_ensure_shader()`
     and creates and initializes `batched_readback`.
   - Buffers come from `_ensure_buffers()` and `_ensure_instance_param_buffer()`.
4. **Destroyed where:**
   - `~GPUCuller()` calls `shutdown()`. The renderer calls `shutdown()` then `unref()`.
   - `GPUCuller::shutdown()` runs:
     1. `batched_readback->shutdown()`, then `unref()`, if valid.
     2. `_release_resources()`.
     3. `rd = nullptr`.
     4. `initialized = false`.
   - `_release_resources()` runs:
     1. Reset the async-readback state.
     2. `_invalidate_instance_uniform_set_cache()`.
     3. `version_free` the shader.
     4. Only if `resource_device` is set: free the pipeline and the seven buffers
        (`instance_param_buffer` on its own device).
     5. Zero all handles and both device pointers.
   - **Idempotent:** yes, but not through `initialized`, which gates only `initialize()`. A
     second call finds no valid handles and a null `resource_device`.
5. **Threading / locking:** per-frame state is used on the render thread. `BatchedAsyncReadback`
   has **no** internal synchronization (no mutex or atomics in
   `renderer/batched_async_readback.*`). The first version claimed it did; that was wrong.
6. **Known leak vectors / open gaps:**
   - If `resource_device` is null, `_release_resources()` zeroes the buffer and pipeline handles
     without freeing them and without a warning.
   - The instance uniform-set cache matches on device, shader, every input RID and
     `param_buffer`, and is invalidated on a miss, on a device change and on release. All of
     these are internal to the culler.

### GPUSorter (`IGPUSorter` implementations)

1. **Owner identity:** `IGPUSorter` (`renderer/gpu_sorter.h`, a `RefCounted`) with the concrete
   classes `BitonicSort`, `RadixSort` and `OneSweepSort` (same header), created by
   `GPUSorterFactory::create_sorter`. Two owners hold sorters: `SortingState::gpu_sorter` and
   `TileGlobalSortResources::sorter`.
2. **What it owns:**
   - `BitonicSort`: `bitonic_shader`, `bitonic_pipeline`, `uniform_set`.
   - `RadixSort`: shaders and pipelines per `RadixVariant`, histogram/prefix/temporary key-value
     buffers, and indirect-dispatch resources.
   - `OneSweepSort`: four shaders and pipelines, plus the `global_histogram`, `digit_histogram`,
     `chained_scan` and temporary key-value buffers.
3. **Created where:** `BitonicSort::initialize()`, `RadixSort::initialize()` and
   `OneSweepSort::initialize()`. `GS_STARTUP_SCOPE("sorter_create_variant")` is inside
   `RadixSort::create_variant()`.
4. **Destroyed where:**
   - Each destructor calls `shutdown()`. The renderer calls `gpu_sorter->shutdown()` then
     `unref()`. `TileRenderer::cleanup()` shuts down its own sorter through
     `global_sort_resources.release`.
   - `BitonicSort::shutdown()` checks two things: whether `resource_device` is still a live
     device (`_device_is_active`), and, when one was recorded, the device generation
     (`ResourceOwnerMismatchContract`). If valid, it `SAFE_FREE`s the pipeline, shader and
     uniform set. If stale, it zeroes them without freeing. It does not wait for in-flight work.
   - `RadixSort::shutdown()` and `OneSweepSort::shutdown()` first call `wait_for_completion()`,
     then make the same validity check. Radix frees through `_cleanup_partial_init()`.
   - **Idempotent:** yes. RIDs are freed under `is_valid()` / `SAFE_FREE`, and `resource_device`
     is nulled, so a second call takes the stale branch.
5. **Threading / locking:** `IGPUSorter::is_sorting` (a `std::atomic<bool>` on the base class)
   signals an in-flight sort. Device validation uses `ResourceOwnerMismatchContract`
   (`renderer/resource_owner_mismatch_contract.h`).
6. **Known leak vectors / open gaps:**
   - The stale-device branches zero RIDs without freeing. This is by design, because the device
     is gone.
   - `_device_is_active()` returns true when both the manager and the RD singleton are gone,
     which trusts the stored pointer at shutdown.

### TileRenderer

1. **Owner identity:** `TileRenderer` (`renderer/tile_renderer.h`), a `RefCounted`, held as
   `TileRendererState::renderer`.
2. **What it owns:** resource controllers declared in `renderer/tile_render_resources.h`:
   - `TileRenderTargets`: output, depth, normal and resolved textures (plus their `_external`
     variants), `tile_framebuffer` and its format.
   - `TileResourceController`: tracked colour and depth outputs and the `tracked_device_manager`.
   - `TileShaderResources`: the binning, binning-count, prefix (three passes), raster,
     compute-raster and resolve shaders and pipelines, with their shader-source owners.
   - `TileGlobalSortResources`: the `sorter` (`Ref<IGPUSorter>`), keys/values buffers, tile
     counts and ranges, prefix-total, indirect-dispatch and workgroup-sum/offset buffers.
   - `TileUniformBuffers`, `TileProjectionBuffers`, `TileSHCacheBuffers`,
     `TileSubpixelHistoryBuffers` and `TileSubpixelVisibilityBuffers`.
3. **Created where:**
   - The constructor, then `TileRenderer::initialize()`.
   - `initialize()` sets the resource device, then `_compile_tile_shaders()`, then
     `_ensure_resources()`, then an eager raster-pipeline create. The eager create runs only when
     `rendering/gaussian_splatting/init/eager_raster_pipeline` is set. The lazy fallback is
     `GS_STARTUP_SCOPE("first_frame_raster_pipeline_create")` in
     `TileRenderer::TileRasterizerStage::dispatch_tile_rasterizer()`.
4. **Destroyed where:**
   - `~TileRenderer()` clears the adaptive-overlap runtime state, unregisters from the
     performance monitors, and calls `cleanup()`.
   - `TileRenderer::cleanup()` runs, in order:
     1. `clear_output_resource_tracking()`, then clear the adaptive-overlap state.
     2. `device = _get_resource_device()`.
     3. If `device` is set, release in this order: projection, SH-cache, subpixel-history and
        subpixel-visibility buffers; debug-stat buffers; `global_sort_resources` (sorter
        `shutdown()` and buffers); `tile_framebuffer`; the resolve and shadow samplers; the
        fallback lighting buffers; uniform buffers. Otherwise, `uniform_buffers.reset_state()`.
     4. `_release_compiled_shaders()`, which runs whether or not `device` is set.
     5. Invalidate the descriptor cache.
     6. Reset the controllers' state. With no device, `global_sort_resources.reset_state(true)`,
        which drops the sorter `Ref`; the sorter's destructor still calls `shutdown()`.
     7. `_destroy_output_textures()`.
     8. Reset frame, grid, metrics and config state, and null the resource and submission
        devices.
   - The renderer drives it: `_forget_tile_renderer_outputs()`, `cleanup()`, `unref()`.
   - **Idempotent:** yes. `cleanup()` nulls the resource device at the end, so a second call
     takes the no-device branch, and the controllers free through `is_valid()` guards.
5. **Threading / locking:** single owner; relies on the renderer's render-thread ordering.
6. **Known leak vectors / open gaps:** `_get_resource_device()` returns the device recorded at
   `initialize()`. It does not consult the manager singleton. The no-device branch is reached only
   for a renderer that was never initialized, or on a repeat `cleanup()`. In that branch the
   uniform, projection, SH, subpixel and global-sort buffer handles are reset without being freed.
   Shaders and pipelines are still released.

### GaussianSplatNode3D

1. **Owner identity:** `GaussianSplatNode3D` (`nodes/gaussian_splat_node_3d.h`), a `Node3D`.
2. **What it owns:**
   - `renderer` (`Ref<GaussianSplatRenderer>`) and `renderer_data` (`Ref<GaussianData>`). These
     are strong references into the director's `SharedWorld`.
   - `render_instance` (RenderingServer instance RID) and `gaussian_base` (RenderingServer base
     RID).
   - `cached_viewport_render_target`, `cached_viewport_render_texture` and
     `cached_viewport_texture`.
   - `color_grading`, `splat_asset` and `runtime_asset`.
3. **Created where:**
   - `gaussian_base` is created in the constructor through `_ensure_gaussian_base()`, and again
     by `_update_render_instance()` / `_update_bounds()` when needed. `render_instance` is
     created by `_update_render_instance()`.
   - `_notification_enter_tree()`:
     1. Register with `GaussianSplatManager::register_node()`, only if the manager exists.
     2. `_update_bounds()`, `_update_visibility()`, `_update_cached_render_target(…)`.
     3. `_ensure_renderer()`, `_update_render_instance()`, `_register_shared_renderer()`.
     4. `_update_debug_hud_visibility()`.
   - `_notification_enter_world()`:
     1. `_ensure_renderer()`, `_update_render_instance()`, `_register_shared_renderer()`.
     2. `_replay_color_grading_if_pending()`.
     3. Apply the renderer debug settings.
     4. `_update_debug_hud_visibility()`, `notify_property_list_changed()`.
4. **Destroyed where:**
   - `~GaussianSplatNode3D()`: `_disconnect_viewport_observers()`, `_clear_asset()`,
     `_release_gaussian_base()`, `_clear_parent_visibility_tracking()`, then `RS::free` of
     `render_instance` if valid.
   - `_notification_exit_tree()` runs, in order:
     1. Destroy the debug HUD control.
     2. Mark the node not visible and `_update_visibility()`.
     3. `_clear_parent_visibility_tracking()`.
     4. `_update_cached_render_target(nullptr)`.
     5. `release_renderer_settings_ownership()`.
     6. `_unbind_renderer_binding_record()` (#839).
     7. `_unregister_shared_renderer()`.
     8. `_release_gaussian_base()`.
     9. Free `render_instance`.
     10. `GaussianSplatManager::unregister_node()`, if the manager exists.
   - `NOTIFICATION_EXIT_WORLD` unbinds the renderer binding record and unregisters the shared
     renderer.
   - `NOTIFICATION_PREDELETE` runs: unbind, `renderer.unref()`, `_unregister_shared_renderer()`,
     then `GaussianSplatSceneDirector::try_prune_world_if_unused()` for the last known scenario.
   - **Idempotent:** yes. Guards are `render_instance.is_valid()` and the early return in
     `_release_gaussian_base()`.
5. **Threading / locking:** main thread only (the scene-tree contract).
   `GaussianSplatManager::register_node()` takes `active_nodes_mutex` (manager L3).
6. **Known leak vectors / open gaps:** `_notification_exit_tree()` deliberately keeps the renderer
   reference so that a re-attached node reuses it. See the comment there about
   `grading_pushed_for_current_data`. The reference is dropped in `NOTIFICATION_PREDELETE`.

## Known leak vectors

- **F6 reload leak.** A director that is not destroyed keeps every `SharedWorld`, and so every
  renderer, alive. Since #387 there are explicit per-world release paths
  (`teardown_world_for_scenario`, `release_all_worlds`, the prune paths and the PREDELETE prune),
  and the lifetime fixture covers the scenario. Module shutdown still relies on `memdelete` of
  the director. See [GaussianSplatSceneDirector](#gaussiansplatscenedirector).
- **Failed-init crash cascade.** `_teardown_resources()` must be safe on partially constructed
  state; the `teardown_resources_started` guard prevents double entry. #383 states that the
  602-event cascade came from director half-initialization when device acquisition failed, and
  added streaming failed-init guards. It filed the director fix as a separate "PR 1b", and no landed
  commit for PR 1b was found. Two things are **unverified**: whether the cascade is fully closed,
  and whether every sub-owner's `shutdown()` is safe when `initialize()` never ran. The
  lifetime fixture (#386) measures device-manager owned/tracked counts and GS-owned bytes, not a
  counter per owner.
- **OutputCompositor framebuffer caches.** Fixed by #388 (LRU-capped at 8 entries).
- **Orphan StringNames at exit.** `GSStartupTraceScope::phase` is a `StringName`, built in the
  `GSStartupTraceScope` constructor. The copies that live until exit are in
  `GSStartupTrace::totals_usec` and `insertion_order`. Since #389,
  `GSStartupTrace::release_module_strings()` clears them at module unregister, alongside
  `gs::release_module_string_names()` and `release_pipeline_feature_set_module_strings()`. The
  `GS_STARTUP_SCOPE` sites:

  | Scope | Function |
  | --- | --- |
  | `module_register` | `initialize_gaussian_splatting_module()` |
  | `manager_construct`, `device_request_primary`, `device_request_shared` | `GaussianSplatManager::GaussianSplatManager()` |
  | `renderer_construct` | `GaussianSplatRenderer::GaussianSplatRenderer(RenderingDevice *)` |
  | `gpu_buffer_manager_init` | `RenderResourceOrchestrator::create_gpu_resources_safe()` |
  | `sorter_create_variant` | `RadixSort::create_variant()` |
  | `first_frame_raster_pipeline_create` | `TileRenderer::TileRasterizerStage::dispatch_tile_rasterizer()` |
  | `shader_compile_binning`, `shader_compile_prefix`, `shader_compile_raster` | `ShaderCompilationManager::compile_binning_shaders()` / `compile_prefix_shaders()` / `compile_raster_shaders()` |
  | `streaming_persistent_buffer_alloc`, `streaming_atlas_build_cpu`, `streaming_atlas_sync_gpu` | `GaussianStreamingSystem::initialize()` |
  | `asset_prefetch_parallel` | `GaussianSplatAsset::prefetch_gaussian_data_parallel()` |
  | `asset_populate_gaussian_data` | `GaussianSplatAsset::populate_gaussian_data()` |
  | `ply_payload_parse` | `PLYLoader::load_file()` |

- **Null-device destruction.** Several release paths drop handles when they have no device:
  - `~GaussianStreamingSystem` and `StreamingGlobalAtlasRegistry::cleanup` drop RIDs with a
    `WARN_PRINT`.
  - `GPUCuller::_release_resources()` drops its handles silently when `resource_device` is null.
  - The sorters' stale-device branches zero RIDs by design.
  - `TileRenderer::cleanup()` drops buffer handles only when it was never initialized or has
    already been cleaned up.

  The first two become leaks when the device is destroyed before its users.

## Thread-safety contracts

- **`GaussianSplatManager` L1–L4 lock hierarchy.** The rules are in the header comment of
  `core/gaussian_splat_manager.h`, with the constants `LOCK_LEVEL_SUBMISSION` and the rest.
  - L1 `submission_mutex` (static, outermost) serializes GPU submission.
  - L2 `resource_maps_mutex` guards the buffer and dynamic-asset maps.
  - L3 `active_nodes_mutex` guards the registered-node set.
  - L4 `local_device_destroy_request_mutex` and `local_device_destroy_pending_mutex` (innermost)
    guard the local-device teardown handoff.

  Never acquire a lower-numbered lock while holding a higher-numbered one. Two notes:
  - One method nests two locks: `_dispatch_local_device_destroy_on_render_thread()` takes the
    pending mutex while holding the request mutex. Both are L4, and only the outer one goes
    through the guard.
  - The destructor takes each lock in turn and releases it before taking the next. Its inline
    comment names a mutex (`local_device_destroy_dispatch_mutex`) that does not exist.

  Development builds validate the order through `GS_LOCK_ORDER_GUARD`.
- **`GaussianSplatSceneDirector::world_mutex`** (`ThreadOwnedMutex`) guards the world
  registry. Two exceptions:
  - The destructor clears the registry without taking it.
  - `submit_world_submission` takes `world_submission_apply_mutex` first.

  Renderer references are released outside the lock.
- **Renderer teardown idempotency** is enforced by the `std::atomic<bool>`
  `GaussianSplatRenderer::teardown_resources_started`, flipped with `compare_exchange_strong` at
  the top of `_teardown_resources()`. Any new teardown caller must go through that function.
- **Render-thread dispatch** is brokered by `IRenderThreadDispatcher`
  (`interfaces/render_thread_dispatcher.h`). `RenderThreadDispatcher` holds the
  `dispatch_mutex`, the semaphore and the request state.
  - `RenderThreadDispatcher::dispatch_call_on_render_thread_blocking()` returns false without
    dispatching in three cases: no RenderingServer, the caller is already on the render thread,
    or the render loop is disabled. Only then does the renderer destructor fall back to a
    synchronous `_teardown_resources()`.
  - Once submitted with `allow_timeout = false`, the call waits indefinitely (logging stalls
    every 5 s) and returns true. So the synchronous fallback never runs after a submission.
- **Per-sub-owner locking** is documented per owner above. Sub-owners do not introduce
  process-global locks. The exceptions to "no mutex" are `GaussianMemoryStream` (`buffer_mutex`,
  `pool_mutex`) and the submission lock taken by `GPUBufferManager`.

## Cross-references

- [`docs/architecture/stage-first-ownership-inventory.md`](stage-first-ownership-inventory.md)
  — stage-level write ownership for #356 (a 2026-05-20 snapshot).
- [`modules/gaussian_splatting/ARCHITECTURE.md`](../../modules/gaussian_splatting/ARCHITECTURE.md)
  — subsystem map and data flow.
- [`modules/gaussian_splatting/MEMORY_SUBSYSTEM.md`](../../modules/gaussian_splatting/MEMORY_SUBSYSTEM.md)
  — VRAM budget policy and persistent-buffer right-sizing (PR #344).
- [`modules/gaussian_splatting/READING_ORDER.md`](../../modules/gaussian_splatting/READING_ORDER.md)
  — recommended reading sequence for new contributors.
- Related work-package issues and PRs:
  - **#352**: Renderer Lifetime Proof (this work package). Follow-ups #383, #386, #387, #388
    and #389.
  - **#327**: earlier teardown-ordering hardening.
  - **#298**: RID leak inventory (the predecessor inventory).
  - **#316**: render-thread dispatch contract.
  - **#628**: renderer teardown outside `world_mutex`, and the reversed module teardown order.
