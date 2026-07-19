/**
 * @file render_facade_state_types.h
 * @brief Remaining GaussianSplatRenderer facade state carrier types.
 */

#ifndef GAUSSIAN_RENDER_FACADE_STATE_TYPES_H
#define GAUSSIAN_RENDER_FACADE_STATE_TYPES_H

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "../gaussian_gpu_layout.h"
#include "../gpu_buffer_manager.h"
#include "../gpu_performance_monitor.h"
#include <atomic>
#include <cstdint>
#include <memory>

class RendererSceneRenderRD;
class TileRenderer;
class RenderDeviceManager;
class DebugOverlaySystem;
class InteractiveStateManager;
class TileRasterizer;
class GPUCuller;
class OutputCompositor;
class GPUSortingPipeline;
class OverflowAutoTuner;
class PainterlyRenderer;
class PainterlyMaterialManager;
class GsShadowBlitShaderRD;

namespace GaussianRenderFacadeState {

struct DeviceState {
	RendererSceneRenderRD *scene_render = nullptr;
	RenderingDevice *rd = nullptr;
	bool reported_missing_submission_device = false;
	bool reported_missing_render_device = false;
};

struct ResourceState {
	bool gpu_resources_initialized = false;
	bool gpu_initialization_pending = false;
	Ref<GPUBufferManager> buffer_manager;
	bool buffer_manager_initialized = false;
	GPUBufferManager::DeferredDeletionQueue deletion_queue;
	RID instance_buffer;
	uint32_t instance_buffer_capacity = 0;
	// Sibling SSBO to instance_buffer: one InstanceGradingGPU row per instance, indexed
	// by SplatRefGPU.instance_id. Rebuilt alongside the instance buffer whenever instance
	// topology or grading parameters change. Sized to the same capacity (rows) so the
	// shader can always index up to instance_buffer_capacity.
	RID instance_grading_buffer;
	uint32_t instance_grading_buffer_capacity = 0;
	// Bumped whenever the renderer-wide color_grading default changes. Included
	// in the upload fingerprint so renderer-only / direct-data flows (no director
	// SharedWorld) still force a grading SSBO re-upload when the default mutates.
	// Without this, fallback-graded rows keep stale GPU values until an unrelated
	// topology change re-runs the publisher.
	//
	// Atomic: the writer runs under the director's world_mutex (via
	// invalidate_grading_for_renderer) while the fingerprint readers in the
	// streaming orchestrator may run on the render thread without that lock.
	// Relaxed ordering is sufficient because the generation is a monotonic
	// counter used only to detect "did something change since last frame".
	std::atomic<uint64_t> instance_grading_defaults_generation{ 0 };
	RID instance_visible_chunk_buffer;
	uint32_t instance_visible_chunk_capacity = 0;
	RID instance_splat_ref_buffer;
	uint32_t instance_splat_ref_capacity = 0;
	RID instance_counter_buffer;
	RID instance_chunk_dispatch_buffer;
	RID instance_indirect_count_buffer;
	RID instance_count_buffer;
	RID resident_atlas_gaussian_buffer;
	uint32_t resident_atlas_gaussian_buffer_size = 0;
	RID resident_asset_meta_buffer;
	uint32_t resident_asset_meta_buffer_size = 0;
	RID resident_asset_chunk_index_buffer;
	uint32_t resident_asset_chunk_index_buffer_size = 0;
	RID resident_chunk_meta_buffer;
	uint32_t resident_chunk_meta_buffer_size = 0;
	RID resident_quantization_buffer;
	uint32_t resident_quantization_buffer_size = 0;
	uint64_t instance_pipeline_contract_generation = 0;
	uint64_t instance_pipeline_content_generation = 0;
	uint64_t instance_pipeline_contract_fingerprint = 0;
	uint64_t instance_pipeline_upload_generation = 0;
	uint64_t instance_pipeline_upload_fingerprint = 0;
	// Atlas-only sub-generation. The resident publisher splits its source_generation into a
	// per-asset atlas hash (asset list + per-asset content_revision) and a per-frame instance
	// hash (visibility, transform, opacity, wind, grading, etc). When this matches the cached
	// value the publisher skips the expensive pack_gaussians_range loop and the four atlas
	// storage-buffer uploads, going straight to the cheap instance-buffer + grading-buffer
	// refresh. Bumped on the slow path; never bumped on instance-only updates.
	uint64_t instance_pipeline_atlas_generation = 0;
	// Cached on the slow path so the fast (instance-only) path can re-publish the contract
	// without re-running the atlas pack loop. These are derived from atlas content and stay
	// stable as long as instance_pipeline_atlas_generation matches.
	uint32_t resident_atlas_gaussian_count = 0;
	uint32_t resident_dispatch_chunk_count = 0;
	uint32_t resident_max_chunk_splats = 0;
	// Test instrumentation: increments every time the publisher actually re-runs the atlas
	// pack loop. Lets regression tests assert that per-instance state changes (visibility,
	// transform, opacity, etc) do NOT trigger an atlas repack.
	uint64_t resident_atlas_pack_count = 0;
	// Resident atlas VRAM-budget LOD clamp telemetry (#321 follow-up). Cached on the slow
	// path so the fast (instance-only) path re-surfaces the same values. `source_count` is
	// the full atlas splat count; the packed count is resident_atlas_gaussian_count above
	// (== source_count when not reduced); `keep_ratio` is the global importance-thinning
	// fraction applied per chunk. `reduced` is true whenever the atlas was clamped to fit
	// the device VRAM budget (observable, never a silent drop).
	bool resident_atlas_reduced = false;
	uint32_t resident_atlas_source_count = 0;
	float resident_atlas_keep_ratio = 1.0f;
};

struct TestDataState {
	LocalVector<Vector3> positions;
	LocalVector<Color> colors;
	LocalVector<Vector3> scales;
	LocalVector<Object *> mesh_instances;
	RID vertex_buffer;
	RID position_buffer;
	RID scale_buffer;
	RID rotation_buffer;
	RID sh_buffer;
	uint64_t content_generation = 0;
	uint64_t uploaded_generation = 0;
	uint32_t uploaded_count = 0;
};

struct TileRendererState {
	Ref<TileRenderer> renderer;
	GPUPerformanceMonitor gpu_performance_monitor;
	bool init_failed = false;
};

struct SubsystemState {
	Ref<RenderDeviceManager> device_manager;
	Ref<DebugOverlaySystem> debug_overlay_system;
	Ref<InteractiveStateManager> interactive_state_manager;
	Ref<TileRasterizer> rasterizer;
	Ref<GPUCuller> gpu_culler;
	Ref<class OutputCompositor> output_compositor;
	Ref<class GPUSortingPipeline> sorting_pipeline;
	Ref<class OverflowAutoTuner> overflow_auto_tuner;
	Ref<PainterlyRenderer> painterly_renderer;
	Ref<class PainterlyMaterialManager> painterly_material_manager;
};

struct ShadowBlitState {
	bool shader_source_initialized = false;
	uint64_t device_id = 0;
	std::unique_ptr<GsShadowBlitShaderRD> shader_source;
	RID shader;
	// Device the shader was compiled on. Recorded at compile time so teardown can
	// free the shader through its actual owner instead of a caller-supplied device
	// that may not own it (freeing a RID through a non-owning RenderingDevice does
	// not error -- it silently fails to release).
	GaussianSplatting::BufferOwnership shader_owner;
	PipelineCacheRD pipeline_cache;
	RID sampler;
	GaussianSplatting::BufferOwnership sampler_owner;

	void clear(RenderingDevice *p_device);
};

} // namespace GaussianRenderFacadeState

#endif // GAUSSIAN_RENDER_FACADE_STATE_TYPES_H
