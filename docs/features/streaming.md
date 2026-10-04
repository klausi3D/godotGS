# Streaming

## Purpose

Load and evict Gaussian splat chunks on demand so that datasets larger than available VRAM can be rendered without running out of memory. `GaussianStreamingSystem` manages an atlas of fixed-size GPU chunk slots, performing frustum culling, predictive prefetch, and distance-based LOD to keep the visible portion of a scene resident. The renderer creates and drives this system internally for `GaussianSplatWorld3D` submissions; scripts observe it through diagnostics (see [Monitoring](#monitoring-streaming-performance)).

## When to use streaming

| Scenario | Recommendation |
| --- | --- |
| Dataset fits entirely in VRAM (under ~256 MB) | Streaming adds overhead; load the dataset directly through `GaussianSplatNode3D`. |
| Dataset exceeds available VRAM or contains more than 500K splats | Bake it to an uncompressed `.gsplatworld` and render it with `GaussianSplatWorld3D` (see [Enabling streaming](#enabling-streaming)). |
| Camera moves continuously through a large environment | Keep predictive prefetch enabled (default) to preload chunks along the camera path. |
| Multiple Gaussian assets share a single world | Bake them into one `.gsplatworld` with the [bake workflow](../workflows/GSPLATWORLD_BAKE.md) so one atlas shares VRAM across them. |

## Source residency model

Streaming behavior depends on the format of the source asset:

| Source | Residency | Chunk payloads | Notes |
| --- | --- | --- | --- |
| Uncompressed `.gsplatworld` | Metadata plus file-backed `StagedFileChunkPayloadSource` by default | Loaded on demand from the source file | Only format that supports file-backed streaming reads without resident `GaussianData`. Explicit resident loads/materialization are compatibility paths. |
| Compressed `.gsplatworld` | Resident only | None — full gaussian array is decoded into memory at load | Format is resident-only and has no "out-of-core" streaming path; the streaming system still manages visibility, LOD, and VRAM residency. |
| `.gsplatworld` loaded through `ResourceFormatLoaderGaussianSplatWorld::load_resident()` | Resident only | None | C++-only compatibility path (not exposed to GDScript) for callers that explicitly request resident CPU data, such as the `.gsplatcache` reader. |
| `.gsplatcache` | Resident only | None | Internal PLY importer cache, owned by the source PLY identity tuple. It is not a streamable world format. |
| `.ply` / `.spz` via `GaussianSplatNode3D` | Resident only | None | Direct instance nodes always publish a resident submission hint. |

`GaussianStreamingSystem` can run for any of these sources, but "streaming" usually means GPU residency and LOD management. Out-of-core source loading currently exists only for normal uncompressed `.gsplatworld` loads.

Generic `ResourceSaver::save()` preserves streamability for source-backed worlds: a normal save/load/save round trip writes uncompressed `.gsplatworld` data and does not leave the in-memory resource materialized as resident `GaussianData`. Compressed output is a resident-only export choice because the current gzip payload has no random-access chunk blocks.

Runtime diagnostics expose the actual payload mode separately from route policy. `GaussianSplatRenderer.get_render_stats()` (reach the renderer with `GaussianSplatWorld3D.get_renderer()`) includes `payload_mode`, `payload_streamable`, `payload_source_active`, `resident_payload_active`, and `payload_resident_only_reason`; a streaming route policy with a compressed world still reports `payload_mode = "resident_only"`.

## Upload completion contract

Streaming chunks move through separate CPU and GPU states. A chunk may have a CPU-packed payload and an atlas slot reservation while `upload_pending=true`, but it is not renderable until the upload retirement ticket reaches `gpu_retired`. `is_loaded` and `gpu_resident` are the renderability signals; atlas metadata and visible buffer-space indices must ignore chunks that are still pending retirement.

Completion modes are intentionally explicit:

| Mode | Contract |
| --- | --- |
| `local_rd_submit_sync` | Local RenderingDevice uploads are submitted and synced before the ticket retires. |
| `main_rd_frame_delay_barrier` | Main RenderingDevice uploads cannot use a true fence here, so retirement waits for `get_frame_delay()` plus a safety frame. This is a conservative frame barrier, not a GPU fence claim. |
| `timeline_unavailable_frame_delay` | Reserved fallback name for paths where a timeline/fence API is not available and frame-delay retirement is used instead. |

Pending uploads keep their atlas slots reserved until retirement or failure rollback. Diagnostics expose `pending_upload_reserved_bytes`, `pending_upload_reserved_slots`, `pending_upload_retirement_tickets`, `retired_upload_*_this_frame`, `failed_upload_retirements`, and `last_upload_completion_mode` so benchmark evidence cannot silently omit upload lifetime state.

## Enabling streaming

There is **no** `streaming/enabled` project setting. An earlier revision of this page told
you to confirm one was `true`; no such key exists anywhere in the module, so the
instruction could not be followed.

**Streaming is opt-in through `GaussianSplatWorld3D`, and only through it.** A direct
`GaussianSplatNode3D` is resident by contract: it always pins
`SUBMISSION_RESIDENCY_HINT_RESIDENT` and **deliberately ignores** `route_policy`
(`GaussianSplatNode3D::_register_instance_in_director`). So
`rendering/gaussian_splatting/streaming/route_policy` selects the backend for **world
submissions only**, and then only when the payload is actually streamable — see the
residency table above.

| Step | Action | Implementation reference |
| --- | --- | --- |
| 1 | Use a `GaussianSplatWorld3D`. A bare `GaussianSplatNode3D` never streams, whatever the policy says. | `GaussianSplatNode3D::_register_instance_in_director` |
| 2 | Leave `rendering/gaussian_splatting/streaming/route_policy` at its default `1` (streaming); `0` forces the resident route. | `GaussianSplatManager::initialize_module`, `GaussianSplatWorld3D::_register_shared_renderer` |
| 3 | Assign an uncompressed `.gsplatworld` (`GaussianSplatWorld`) to the node's `world` property. With `auto_apply_on_ready` (default `true`) the world is submitted when the node becomes ready; otherwise call `apply_world()`. | `GaussianSplatWorld3D::set_world`, `GaussianSplatWorld3D::apply_world` |
| 4 | Confirm the route at runtime: `get_renderer().get_render_stats()["payload_mode"]` and the `gaussian_splatting/streaming_*` Performance monitors (see below). | `GaussianSplatRenderer::get_render_stats` |

You do not call `GaussianStreamingSystem.initialize()` or `update_streaming()` yourself: the renderer owns and drives the streaming system for world submissions. Those methods are bound, but a `GaussianStreamingSystem` you create with `.new()` is not connected to any renderer and draws nothing.

## VRAM budget configuration

The streaming system keeps resident chunks in one GPU atlas buffer, allocated in pages of `ATLAS_PAGE_SPLATS` (1 024) splats. Each resident chunk owns one contiguous run of `ceil(count / 1024)` pages, so atlas memory follows the resident splats rather than the chunk count (a chunk holds at most `CHUNK_SIZE`, 65 536, splats). The atlas buffer, including its growth, is clamped to `rendering/gaussian_splatting/streaming/vram_budget_mb` in bytes. `max_chunks_in_vram` stays a resident-chunk-count backstop. When no free run fits an incoming chunk, admission evicts least-recently-used chunks until one does, at most `max_evictions_per_frame` per frame; a chunk that is on screen is only evicted when that eviction alone makes the incoming chunk fit. The VRAM regulator steers atlas occupancy to the atlas ceiling minus one full-size chunk of headroom, so the budget can actually be filled (#1088). Budget limits can be configured globally or per quality tier.

| Project setting | Default | Description | Implementation reference |
| --- | --- | --- | --- |
| `rendering/gaussian_splatting/streaming/max_upload_mb_per_frame` | `128` | Maximum MB uploaded to VRAM in a single frame. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/max_upload_mb_per_slice` | `16` | Maximum MB per upload slice within a frame. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/max_chunk_loads_per_frame` | `16` | Cap on chunk load operations per frame. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/eviction_hysteresis_frames` | `5` | Frames a chunk must be unused before eviction is considered. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/max_evictions_per_frame` | `4` | Maximum chunk evictions per frame. | `GaussianSplatManager::initialize_module` |

Quality tier presets (`rendering/gaussian_splatting/quality/tier_preset`) override streaming budgets when `tier_apply_streaming_budgets` is enabled. See [quality_tier_config.h](../../modules/gaussian_splatting/core/quality_tier_config.h) for tier-specific values.

## Chunk LOD behavior

Chunks transition between LOD levels based on distance from the camera. The system uses an Octree-GS-inspired approach that reduces the SH band level and raises the splat skip factor as distance grows. What the GPU draw path actually honours is listed per parameter below.

| Per-chunk LOD parameter | Effect | Implementation reference |
| --- | --- | --- |
| `current_lod_level` / `target_lod_level` | Drive LOD transitions. | `StreamingVisibilityController::update_chunk_lod_parameters` |
| `sh_band_level` (0 -- 3) | 0 = DC only, 3 = full spherical harmonics. Honoured on the GPU through the per-chunk SH limit in the chunk metadata. | `StreamingGlobalAtlasRegistry::update_chunk_meta_entry` |
| `splat_skip_factor` | 1 = render all. A factor of N sets the chunk's `effective_count` to `count / N`, and the GPU draws **the first `count / N` splats of each chunk** in stored order: a prefix, not every Nth splat. Which splats survive depends on the order the chunk was written in, so the reduction is not spatially uniform. This is a known quality limitation. | `StreamingVisibilityController::update_chunk_lod_parameters`, `StreamingGlobalAtlasRegistry::update_chunk_meta_entry` |
| `opacity_multiplier` | Distance-based opacity fade. Computed per chunk and reported as `min_opacity_multiplier` in `get_lod_debug_stats()`; no consumer on the GPU draw path was found at this revision. | `StreamingVisibilityController::update_chunk_lod_parameters` |

The strided variants `GaussianStreamingSystem::get_visible_gaussians()` and `get_visible_indices()` (every Nth splat, with opacity fade) exist in C++ but have no callers; they do not describe what is rendered.

LOD blending is configured through project settings, read by `LODBlendConfig::load_from_project_settings` when the streaming configuration reloads: `rendering/gaussian_splatting/lod/blend_enabled` (default `true`) and `rendering/gaussian_splatting/lod/blend_distance` (default `5.0`). `GaussianStreamingSystem::set_lod_blend_enabled()` and `set_lod_blend_distance()` are inline C++ helpers only; they are not bound and cannot be called from GDScript.

## Predictive prefetch

When the camera is in motion, the system predicts where the camera will be and begins loading chunks ahead of time.

| Project setting | Default | Description | Implementation reference |
| --- | --- | --- | --- |
| `rendering/gaussian_splatting/streaming/predictive_prefetch_enabled` | `true` | Enables velocity-based prefetch. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/prefetch_lookahead_distance` | `10.0` | Distance (in world units) to look ahead for prefetch. | `GaussianSplatManager::initialize_module` |
| `rendering/gaussian_splatting/streaming/max_prefetch_loads_per_frame` | `6` | Prefetch load cap per frame. | `GaussianSplatManager::initialize_module` |

## Monitoring streaming performance

### GDScript queries

No bound API returns the `GaussianStreamingSystem` that a renderer uses (there is no `get_streaming_system()`). Scripts read the live system through two bound surfaces:

- the module's custom `Performance` monitors (`gaussian_splatting/streaming_*`, `gaussian_splatting/vram_*`, `gaussian_splatting/lod_*`), registered at module start-up by `GaussianSplattingPerformanceMonitors`. They report on the active renderer that has a streaming system, process-wide, not per node. They also appear in the editor's Debugger > Monitors tab.
- `GaussianSplatRenderer.get_render_stats()` on the renderer returned by `GaussianSplatWorld3D.get_renderer()` (shared by the Gaussian nodes of one `World3D`) for the payload/route fields listed above.

```gdscript
extends Node

## Assign a GaussianSplatWorld3D in the inspector (optional; monitors work without it).
@export var splat_world: GaussianSplatWorld3D

const STREAMING_MONITORS := [
	"gaussian_splatting/streaming_loaded_chunks",
	"gaussian_splatting/streaming_visible_chunks",
	"gaussian_splatting/streaming_vram_usage_mb",
	"gaussian_splatting/streaming_visible_count",
	"gaussian_splatting/streaming_effective_splat_count",
	"gaussian_splatting/vram_budget_warning_active",
	"gaussian_splatting/lod_splat_skip_factor",
]

func _ready() -> void:
	var timer := Timer.new()
	timer.wait_time = 1.0
	timer.autostart = true
	timer.timeout.connect(_print_streaming_state)
	add_child(timer)

func _print_streaming_state() -> void:
	for monitor_id in STREAMING_MONITORS:
		if Performance.has_custom_monitor(monitor_id):
			print(monitor_id, " = ", Performance.get_custom_monitor(monitor_id))

	if splat_world == null:
		return
	var renderer := splat_world.get_renderer()
	if renderer == null:
		return  # No renderer until the world has been applied on a device-backed viewport.
	var stats := renderer.get_render_stats()
	print("payload_mode = ", stats.get("payload_mode"), ", streamable = ", stats.get("payload_streamable"))
```

### `GaussianStreamingSystem` methods (standalone instances only)

These methods are bound on `GaussianStreamingSystem`, but because no bound API hands out the renderer's instance, they only apply to an instance you create yourself. Such an instance is not connected to a renderer. For the live system, use the monitors above.

| Method | Returns | Description | Implementation reference |
| --- | --- | --- | --- |
| `get_vram_usage()` | `int` | Total VRAM bytes consumed by loaded chunks. | `GaussianStreamingSystem::get_vram_usage` |
| `get_loaded_chunks()` | `int` | Number of chunks currently resident in VRAM. | `GaussianStreamingSystem::get_loaded_chunks` |
| `get_pending_upload_retirement_slots()` | `int` | Atlas slots reserved by chunks that are not yet GPU-retired/renderable. | `GaussianStreamingSystem::get_pending_upload_retirement_slots` |
| `get_pending_upload_retirement_bytes()` | `int` | Pending upload payload bytes reserved against streaming admission diagnostics. | `GaussianStreamingSystem::get_pending_upload_retirement_bytes` |
| `get_visible_count()` | `int` | Splat count visible this frame after culling. | `GaussianStreamingSystem::get_visible_count` |
| `get_effective_splat_count()` | `int` | Splat count after LOD reduction is applied. | `GaussianStreamingSystem::get_effective_splat_count` |
| `get_streaming_analytics()` | `Dictionary` | Detailed analytics including pack/upload timing. | `GaussianStreamingSystem::get_streaming_analytics` |
| `get_chunk_culling_stats()` | `Dictionary` | Total, visible, culled, and loaded chunk counts. | `GaussianStreamingSystem::get_chunk_culling_stats` |
| `get_vram_debug_stats()` | `Dictionary` | VRAM regulator state and budget utilization. | `GaussianStreamingSystem::get_vram_debug_stats` |
| `get_lod_debug_stats()` | `Dictionary` | LOD level distribution and transition counts. | `GaussianStreamingSystem::get_lod_debug_stats` |
| `is_vram_budget_warning_active()` | `bool` | True when VRAM usage approaches the configured budget. | `GaussianStreamingSystem::is_vram_budget_warning_active` |

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| No splats visible after loading a large dataset | Chunks have not finished uploading; initial load is in progress. | Wait several frames and check that the `gaussian_splatting/streaming_loaded_chunks` monitor is increasing. Increase `max_chunk_loads_per_frame` for faster initial population. | `GaussianStreamingSystem::get_loaded_chunks` |
| VRAM usage exceeds expectations | Quality tier override is raising the VRAM budget ceiling. | Check `rendering/gaussian_splatting/quality/tier_preset` and `tier_apply_streaming_budgets`. | `GaussianSplatManager::initialize_module` |
| Chunks pop in and out frequently | Eviction hysteresis is too low or VRAM budget is too tight. | Increase `eviction_hysteresis_frames` and raise `max_upload_mb_per_frame` so the system can keep more chunks loaded. | `GaussianSplatManager::initialize_module` |
| Zero visible splats persist for many frames | Camera is outside the bounding boxes of all chunks, or recovery is not triggering. | Verify camera position is within the dataset bounds. Check `rendering/gaussian_splatting/streaming/zero_visible_recovery_mode`. | `GaussianSplatManager::initialize_module` |
| Prefetch does not load chunks ahead of camera | Prefetch is disabled or lookahead distance is too small. | Set `predictive_prefetch_enabled` to `true` and increase `prefetch_lookahead_distance`. | `GaussianSplatManager::initialize_module` |
| LOD transitions cause visible flickering | LOD blend is disabled or blend distance is too short. | Set `rendering/gaussian_splatting/lod/blend_enabled` to `true` and raise `rendering/gaussian_splatting/lod/blend_distance`. | `LODBlendConfig::load_from_project_settings` |
