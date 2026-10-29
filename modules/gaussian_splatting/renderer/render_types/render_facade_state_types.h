/**
 * @file render_facade_state_types.h
 * @brief Remaining GaussianSplatRenderer facade state carrier types.
 */

#ifndef GAUSSIAN_RENDER_FACADE_STATE_TYPES_H
#define GAUSSIAN_RENDER_FACADE_STATE_TYPES_H

#include "../../core/gaussian_splat_hlod_runtime_data.h"
#include "../hlod_node_streamer.h"
#include "core/math/color.h"
#include "core/math/vector3.h"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "../gaussian_gpu_layout.h"
#include "render_pipeline_io_types.h"
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

// All RIDs in owned[] belong to the enclosing ResourceState. Sort buffers remain
// borrowed from its existing sorting subsystem and never appear in owned[].
struct HlodBundle {
    GaussianRenderPipeline::InstancePipelineBuffers buffers;
    RID owned[14];
    uint32_t owned_count = 0;
    uint64_t device_id = 0;
    uint64_t bytes = 0;
    uint64_t generation = 0;
    bool quantized = false;
    Vector<DrawnNodeGPU> draw_entries;
};

struct ResourceState {
    Ref<GaussianSplatHlodRuntimeData> hlod_snapshot;
    String hlod_reason;
    // The reason of the last view the HLOD route refused and how many it refused. hlod_reason is the LAST view's verdict, so
    // a refused second view is otherwise masked by the main view's "active" in the same frame.
    String hlod_last_refusal_reason;
    uint64_t hlod_refused_views = 0;
    float hlod_tau_px = 0.0f;
    uint64_t hlod_owner_key = 0;
    Ref<GaussianSplatHlodRuntimeData> hlod_prepared_snapshot;
    // Package E1: why the last paged provisioning attempt failed (empty = fine); the warmup reasons report it instead of hiding it.
    String hlod_paged_provision_reason;
    HlodBundle hlod_canonical;
    HlodBundle hlod_relative;
    // Node streaming (package D): when the whole tree does not fit the atlas budget only the relative bundle
    // exists (no canonical full-leaf copy), its atlas holds `hlod_stream_atlas_records` splats, and the
    // streamer decides which nodes are resident. Empty/false for the whole-tree bundle.
    bool hlod_streaming = false;
    uint32_t hlod_stream_atlas_records = 0;
    Ref<HlodNodeStreamer> hlod_streamer;
    // Package F1: the last committed edited snapshot. It outlives the streamer and the prepared bundle: a rebuild (device
    // loss, a failed commit, a setting change) serves it again instead of the unedited world, and a frame that cannot be
    // served from it draws nothing with an hlod_edit_* reason (ADR 14.5: no submission after a confirmed edit may
    // redisplay the old content). Cleared when a different world is prepared.
    Ref<GaussianSplatHlodRuntimeData> hlod_committed_edit;
    uint64_t hlod_edit_owner_key = 0;
    uint64_t hlod_edit_forced_frames = 0; // Frames whose cut replaced a stale ancestor.
    uint64_t hlod_edit_blank_frames = 0; // Frames that drew nothing because the edited world could not be served.
    String hlod_edit_event;
    String hlod_edit_probe_result;
    bool hlod_edit_probe_done = false;
    uint32_t hlod_stream_ready_frames = 0;
    uint32_t hlod_stream_loading_frames = 0;
    uint32_t hlod_stream_first_ready_frame = 0;
    uint64_t hlod_stream_frames = 0;
    uint32_t hlod_stream_nonresident_drawn = 0; // Must stay 0: a drawn node that is not resident-ready.
    uint64_t hlod_origin_revision = 0; // Revision of the last uploaded HLOD draw list (diagnostics)
    uint64_t hlod_content_serial = 0;
    uint64_t hlod_budget_bound_frames = 0;
    uint64_t hlod_root_dropped_frames = 0;
    uint64_t hlod_cpu_cut_usec = 0;
    uint64_t hlod_cpu_prepare_usec = 0; // View admission, selection and draw-list upload; CPU elapsed only.
    uint64_t hlod_device_byte_upper_bound = 0;
    uint64_t hlod_view_epoch = 0;
    uint32_t hlod_visited_nodes = 0;
    uint32_t hlod_selected_nodes = 0;
    uint32_t hlod_selected_parents = 0;
    uint64_t hlod_selected_splats = 0;
    double hlod_estimated_records = 0.0;
    bool hlod_active = false;
    bool hlod_frame_active = false;
    uint64_t hlod_tile_stage_refused_frames = 0; // Admitted as active, then refused by the tile stage (drew nothing).
    uint64_t hlod_owned_byte_upper_bound = 0;
    uint64_t hlod_retired_bytes = 0;
    uint64_t hlod_instance_generation = 0;
    uint64_t hlod_grading_generation = 0;
    uint64_t hlod_prepared_owner_key = 0;
    uint32_t hlod_position_bits = 0;
    uint32_t hlod_scale_bits = 0;
    bool hlod_quantize_scales = false;
#ifdef TESTS_ENABLED
    // Failure-only seams at the actual publisher boundaries; no success override.
    std::atomic<int> hlod_test_failure{ 0 };
    std::atomic<uint64_t> hlod_test_failure_hits{ 0 };
    // Lowers the HLOD route's device-ledger limit (0 = off) so a runtime scenario can force a memory refusal. Never raises it.
    std::atomic<uint64_t> hlod_test_route_limit_override{ 0 };
    // Actual resident sort inputs, retained before the post-frame canonical restore.
    // Borrowed RID only; the observer validates lifetime, device and frame before readback.
    RID hlod_test_sort_count_buffer;
    uint64_t hlod_test_sort_frame = 0;
    uint64_t hlod_test_sort_pass_frame = 0;
    uint64_t hlod_test_sort_source = 0;
    uint64_t hlod_test_sort_input_generation = 0;
    uint64_t hlod_test_sort_device = 0;
    uint64_t hlod_test_sort_content_generation = 0;
    bool hlod_test_sort_relative = false;
#endif
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

// The publisher marks a frame active before the tile stage runs. When the tile stage then refuses it (its admitted
// scratch or layout does not match the frame) nothing is drawn: the frame is not active, its reason names the refusal,
// and it is not a ready (or first-ready) streamed frame. No-op for a frame that was not active.
inline void hlod_mark_tile_stage_refused(ResourceState &r_state, const String &p_cause) {
    if (!r_state.hlod_frame_active) { return; }
    r_state.hlod_frame_active = false;
    r_state.hlod_reason = String("hlod_tile_stage_refused:") + (p_cause.is_empty() ? String("tile_render_failed") : p_cause);
    ++r_state.hlod_tile_stage_refused_frames;
    if (r_state.hlod_streaming && r_state.hlod_stream_ready_frames > 0) {
        // An active streamed frame was counted ready in this same frame (prepare_hlod_frame).
        --r_state.hlod_stream_ready_frames;
        if (uint64_t(r_state.hlod_stream_first_ready_frame) == r_state.hlod_stream_frames) { r_state.hlod_stream_first_ready_frame = 0; }
    }
}

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
