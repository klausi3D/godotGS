/**
 * @file render_pipeline_io_types.h
 * @brief Pipeline stage input/output type definitions.
 *
 * Standalone types for the renderer pipeline stages (cull, sort, raster,
 * composite). Extracted from GaussianSplatRenderer to allow orchestrators
 * and pipeline stages to depend on narrow contracts instead of the full
 * renderer header.
 */

#ifndef GAUSSIAN_RENDER_PIPELINE_IO_TYPES_H
#define GAUSSIAN_RENDER_PIPELINE_IO_TYPES_H

#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "render_state_types.h"
#include "../tile_render_types.h"
#include <cstdint>

namespace GaussianRenderPipeline {

using IndexDomain = GaussianRenderState::IndexDomain;
using CullStageOutput = GaussianRenderState::CullStageOutput;

struct RenderFrameSnapshot {
	bool valid = false;
	uint32_t visible_splats = 0;
	uint32_t sorted_splats = 0;
	IndexDomain cull_visible_domain = IndexDomain::UNKNOWN;
	IndexDomain sorted_index_domain = IndexDomain::UNKNOWN;
};

enum class RenderRouteBackend : uint8_t {
	NONE = 0,
	RESIDENT,
	STREAMING,
};

inline const char *render_route_backend_to_string(RenderRouteBackend p_backend) {
	switch (p_backend) {
		case RenderRouteBackend::NONE:
			return "none";
		case RenderRouteBackend::RESIDENT:
			return "resident";
		case RenderRouteBackend::STREAMING:
			return "streaming";
	}
	return "none";
}

struct RenderRouteDecision {
	bool valid = false;
	RenderRouteBackend selected_backend = RenderRouteBackend::NONE;
	String selected_backend_name = "none";
	String route_uid;
	String reason;
	String data_source;
	bool has_render_data = false;
	bool single_route_per_frame = true;
	bool alternate_backend_fallback_forbidden = true;
};

enum class RenderFallbackReason {
	NONE = 0,
	DATA_UNAVAILABLE,
	STREAMING_DATA_UNAVAILABLE,
	GPU_CULLER_UNAVAILABLE,
	NO_VISIBLE_SPLATS,
	RENDERING_DEVICE_UNAVAILABLE,
	RASTER_REUSED_CACHED_RENDER,
	PAINTERLY_UNAVAILABLE,
	PAINTERLY_PASS_GRAPH_UNAVAILABLE,
	PAINTERLY_MATERIAL_UNAVAILABLE,
	PAINTERLY_RENDER_FAILED,
	TILE_FALLBACK_FAILED,
	OUTPUT_COMPOSITOR_UNAVAILABLE,
};

// Note: Named StageStatus to avoid X11/Xlib.h macro conflict (#define Status int)
struct StageResult {
	enum class StageStatus {
		SUCCESS,
		SKIPPED,
		FALLBACK,
		FAILED
	};

	StageStatus status = StageStatus::SUCCESS;
	bool is_error = false;
	RenderFallbackReason fallback_reason = RenderFallbackReason::NONE;
	String reason;
	String stage_name;
	String route_uid;
	IndexDomain input_domain = IndexDomain::UNKNOWN;
	IndexDomain output_domain = IndexDomain::UNKNOWN;
	uint32_t input_count = 0;
	uint32_t output_count = 0;
	String first_failure_stage;
	String skip_cause_stage;
	bool depth_test_honored = true;
	bool degraded = false;
	String degradation_reason;
	bool used_cached_output = false;
	bool used_cpu_fallback = false;
	bool used_graphics_fallback = false;
	bool overflow_clamped = false;
	bool strict_contract_violation = false;

	bool is_success() const { return status == StageStatus::SUCCESS || status == StageStatus::FALLBACK; }
	bool is_failure() const { return status == StageStatus::FAILED; }
	bool did_fallback() const { return status == StageStatus::FALLBACK; }
	bool was_skipped() const { return status == StageStatus::SKIPPED; }
};

struct StageIO {
	uint64_t frame_id = 0;
	uint32_t input_count = 0;
	uint32_t output_count = 0;
	RID input_buffer;
	RID output_buffer;
	bool validated = false;
	bool validation_failed = false;
	String validation_error;
};

struct SortStageOutput {
	bool did_sort = false;
	uint32_t sorted_count = 0;
	uint32_t input_count = 0;
	float sort_time_ms = 0.0f;
	IndexDomain input_domain = IndexDomain::UNKNOWN;
	IndexDomain output_domain = IndexDomain::UNKNOWN;
};

struct SplatDataSource {
	static constexpr const char *kSourceNone = "none";
	static constexpr const char *kSourceUnavailable = "Unavailable";
	static constexpr const char *kSourceBufferManager = "GPUBufferManager";
	static constexpr const char *kSourceStreaming = "StreamingGPU";
	static constexpr const char *kSourceResidentInstance = "ResidentInstanceAtlas";
	static constexpr const char *kSourceCpuData = "gaussian_data";

	RID gaussian_buffer;
	RID sorted_indices;
	uint32_t splat_count = 0;
	uint32_t total_gaussians = 0;
	const char *source_name = kSourceUnavailable;
};

enum class InstanceBackendPolicy : uint8_t {
	NONE = 0,
	STREAMING,
	RESIDENT,
};

inline const char *instance_backend_policy_to_string(InstanceBackendPolicy p_policy) {
	switch (p_policy) {
		case InstanceBackendPolicy::NONE:
			return "none";
		case InstanceBackendPolicy::STREAMING:
			return "streaming";
		case InstanceBackendPolicy::RESIDENT:
			return "resident";
	}
	return "none";
}

struct PublishedInstanceAssetRemap {
	// Key: full 64-bit host submission asset identity (asset Node3D ObjectID).
	// Value: sequential dense atlas slot id (collision-free, fits uint32_t and is
	// what lands in the GPU InstanceDataGPU::ids[0] field). The key must stay
	// 64-bit so two assets whose ObjectIDs collide in the low 32 bits map to
	// distinct dense slots instead of aliasing the same rendered asset.
	HashMap<uint64_t, uint32_t> asset_to_dense_id;
	uint64_t generation = 0;
	bool valid = false;

	void clear() {
		asset_to_dense_id.clear();
		generation = 0;
		valid = false;
	}
};

struct InstancePipelineBuffers {
	RID instance_buffer;
	// Per-instance color grading SSBO (InstanceGradingGPU rows). Parallel to instance_buffer.
	RID instance_grading_buffer;
	RID asset_meta_buffer;
	RID asset_chunk_index_buffer;
	RID chunk_meta_buffer;
	RID visible_chunk_buffer;
	RID splat_ref_buffer;
	RID sort_key_buffer;
	RID sort_value_buffer;
	RID quantization_buffer;
	bool quantization_required = false;
	RID atlas_gaussian_buffer;
	uint32_t atlas_gaussian_count = 0;
	RID counter_buffer;
	RID chunk_dispatch_buffer;
	RID indirect_count_buffer;
	RID instance_count_buffer;
	uint32_t instance_count = 0;
	uint32_t dispatch_chunk_count = 0;
	uint32_t max_visible_chunks = 0;
	uint32_t max_visible_splats = 0;
	uint32_t max_chunk_splats = 0;
	bool world_submission_active = false;
};

struct DataSourcePlan {
	SplatDataSource source;
	String error;
	bool using_real_data = false;
};

struct RasterStageOutput {
	RID color;
	RID depth;
	Size2i internal_size;
	bool painterly_active = false;
	bool reused_cached_render = false;
	float render_time_ms = 0.0f;
	String raster_path = "unknown";
	uint32_t sorted_splat_count = 0;
	uint64_t content_generation = 0;
	uint64_t shader_defines_hash = 0;
};

struct StageMetrics {
	CullStageOutput cull;
	StageResult cull_result;
	SortStageOutput sort;
	StageResult sort_result;
	RasterStageOutput raster;
	StageResult raster_result;
	float composite_time_ms = 0.0f;
	bool composite_executed = false;
	StageResult composite_result;
	StageIO cull_io;
	StageIO sort_io;
	StageIO raster_io;
	StageIO composite_io;
	String route_uid;
	String selected_route_backend;
	String first_failure_stage;
	String skip_cause_stage;
	int32_t first_failure_instance_index = -1;
	int32_t first_skipped_instance_index = -1;
	bool has_degradation = false;
	bool composite_depth_test_honored = true;
	String degradation_reason;
};

struct PipelineEvent {
	String stage;
	String message;
	String route_uid;
	uint32_t input_count = 0;
	uint32_t output_count = 0;
	bool is_error = false;
	RenderFallbackReason fallback_reason = RenderFallbackReason::NONE;
};

/**
 * @struct PainterlyCompositePushConstant
 * @brief GPU push constants for painterly compositing shader.
 *
 * Mirrors the `CompositePush` block in shaders/painterly_composite.glsl.
 * `pad` is load-bearing: SPIRV-Reflect reports a push-constant block's size
 * rounded UP to a 16-byte multiple (thirdparty/spirv-reflect/spirv_reflect.c:2785-2803,
 * SPIRV_DATA_ALIGNMENT = 16), Godot adopts that value as the pipeline's required
 * push-constant size (servers/rendering/rendering_device_commons.cpp:1292) and
 * then rejects any draw that does not supply EXACTLY that many bytes
 * (servers/rendering/rendering_device.cpp:4745). The payload scalars occupy 40
 * bytes, so the reflected requirement is 48; pushing the unpadded size made
 * every painterly composite draw fail (#986 blocker B). Keep sizeof() a
 * multiple of 16.
 */
struct PainterlyCompositePushConstant {
	float inv_viewport_size[2];
	float depth_epsilon;
	float blend_strength;
	float z_near;
	float z_far;
	// From the single host derivation in interfaces/gs_scene_depth_linearize.h,
	// the same one the compute blit uses. This struct previously carried raw
	// camera-projection columns and the shader linearized scene depth its own
	// way; that never matched the depth buffer, so the composite's occlusion
	// guard discarded every splat fragment once it became reachable (#986).
	float depth_linearize_mul;
	float depth_linearize_add;
	int32_t depth_is_orthogonal;
	// Mirrors `rendering/gaussian_splatting/composite/depth_test`. The compute
	// composite honours that setting; this pass used to depth-test
	// unconditionally, which nobody noticed while it could not run at all.
	int32_t depth_test_enabled;
	float pad[2];
};
static_assert(sizeof(PainterlyCompositePushConstant) == 48,
		"PainterlyCompositePushConstant must match the reflected CompositePush size: the "
		"payload rounded up to the 16-byte SPIR-V push-constant block alignment. The "
		"multiple-of-16 rule itself is enforced for every registered push-constant struct "
		"by tests/ci/check_gaussian_layout_sync.py.");

/**
 * @struct RenderFramePlan
 * @brief Planning data that determines the render path for a frame.
 */
struct RenderFramePlan {
	RenderRouteDecision route_decision;
	DataSourcePlan data_source;
	bool has_render_data = false;
	bool set_skip_metrics = false;
	bool clear_cull_state_on_skip = false;
	GaussianSplatting::ComputeRasterPolicy compute_raster_policy = GaussianSplatting::ComputeRasterPolicy::Default;
	String cull_skip_reason;
	String sort_skip_reason;
	RenderFallbackReason cull_skip_reason_code = RenderFallbackReason::NONE;
	RenderFallbackReason sort_skip_reason_code = RenderFallbackReason::NONE;
};

} // namespace GaussianRenderPipeline

#endif // GAUSSIAN_RENDER_PIPELINE_IO_TYPES_H
