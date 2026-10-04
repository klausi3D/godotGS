#ifndef GAUSSIAN_RENDER_PIPELINE_STAGES_H
#define GAUSSIAN_RENDER_PIPELINE_STAGES_H

#include "gaussian_splat_renderer.h"

#include <memory>

class RenderDebugStateOrchestrator;
class RenderDiagnosticsOrchestrator;

class RenderPipelineStages {
public:
	explicit RenderPipelineStages(GaussianSplatRenderer *p_renderer);
	~RenderPipelineStages();

	using StageResult = GaussianSplatRenderer::StageResult;
	using DataSourcePlan = GaussianSplatRenderer::DataSourcePlan;
	using RenderRouteDecision = GaussianSplatRenderer::RenderRouteDecision;
	using RenderFramePlan = GaussianSplatRenderer::RenderFramePlan;
	using RenderFrameContext = GaussianSplatRenderer::RenderFrameContext;
	using RenderFallbackReason = GaussianSplatRenderer::RenderFallbackReason;
	using StageMetrics = GaussianSplatRenderer::StageMetrics;
	using SceneState = GaussianSplatRenderer::SceneState;
	using StreamingState = GaussianSplatRenderer::StreamingState;
	using SortingState = GaussianSplatRenderer::SortingState;
	using InstanceBackendPolicy = GaussianSplatRenderer::InstanceBackendPolicy;
	using InstancePipelineBuffers = GaussianSplatRenderer::InstancePipelineBuffers;
	using ResourceState = GaussianSplatRenderer::ResourceState;
	using SubsystemState = GaussianSplatRenderer::SubsystemState;
	using PerformanceMetrics = GaussianSplatRenderer::PerformanceMetrics;
	using PipelineFeatureSet = ::PipelineFeatureSet;

	// Frame planning static helpers (moved from GaussianSplatRenderer)
	static DataSourcePlan build_data_source_plan(const SceneState &p_scene_state,
			const StreamingState &p_streaming_state,
			const SortingState &p_sorting_state,
			const InstancePipelineBuffers *p_instance_buffers,
			InstanceBackendPolicy p_instance_backend_policy,
			const ResourceState &p_resource_state,
			const SubsystemState &p_subsystem_state);
	static void apply_data_source_plan(const DataSourcePlan &p_plan, PerformanceMetrics &p_metrics,
			const ResourceState &p_resource_state);
	static void stamp_stage_result_contract(StageResult &r_result, const char *p_stage_name,
			const String &p_route_uid, GaussianSplatRenderer::IndexDomain p_input_domain,
			GaussianSplatRenderer::IndexDomain p_output_domain, uint32_t p_input_count,
			uint32_t p_output_count);
	static StageResult make_downstream_skip_result(const char *p_stage_name,
			const StageResult &p_upstream_result, const String &p_reason,
			GaussianSplatRenderer::RenderFallbackReason p_fallback_reason);
	static StageMetrics make_route_skip_metrics(const String &p_route_uid,
			const String &p_selected_backend, const String &p_cull_reason,
			const String &p_sort_reason, GaussianSplatRenderer::RenderFallbackReason p_fallback_reason);
		static StageResult make_composite_copy_result(bool p_copy_attempted, bool p_copy_success,
				const String &p_copy_error, bool p_copy_degraded, const String &p_degradation_reason,
				bool p_depth_test_honored, bool p_viewport_copy_success, bool p_strict_depth_contract_required);
	static void finalize_stage_contracts(StageMetrics &r_metrics, const RenderFramePlan &p_frame_plan);
	// Fills the raster's sorted count / sort time / index domain: from the metrics' sort
	// block only when a sort stage in this pass set `did_sort`, otherwise from the frame
	// snapshot (#1163).
	static void resolve_raster_sort_input(const RenderFrameContext &p_context,
			GaussianSplatRenderer::RasterStageInput &r_input);
	// Single producer for RenderRouteDecision (#351). Both the route selection site
	// (GaussianSplatRenderer::render_scene_instance) and build_frame_plan derive the
	// per-frame route contract from here so there is exactly one decision per frame.
	static RenderRouteDecision build_route_decision(
			InstanceBackendPolicy p_instance_backend_policy,
			const DataSourcePlan &p_data_source,
			bool p_has_render_data,
			const String &p_no_data_reason);
	static RenderFramePlan build_frame_plan(const SceneState &p_scene_state,
			const StreamingState &p_streaming_state,
			const SortingState &p_sorting_state,
			const InstancePipelineBuffers *p_instance_buffers,
			InstanceBackendPolicy p_instance_backend_policy,
			const ResourceState &p_resource_state,
			const SubsystemState &p_subsystem_state,
			const PipelineFeatureSet *p_pipeline_features,
			bool p_has_render_data,
			const String &p_cull_skip_reason,
			const String &p_sort_skip_reason,
			RenderFallbackReason p_cull_skip_reason_code,
			RenderFallbackReason p_sort_skip_reason_code,
			bool p_set_skip_metrics,
			bool p_clear_cull_state_on_skip,
			// When valid, the authoritative per-frame decision produced at the route
			// selection site is consumed verbatim instead of being re-derived. Pass
			// nullptr (or an invalid decision) for paths with no selection-site
			// decision (light/shadow pass, no-render-data early path, unit tests).
			const RenderRouteDecision *p_authoritative_route_decision = nullptr);

	// Frame context preparation and entry (moved from GaussianSplatRenderer)
	void prepare_frame_context(RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
			const Projection &p_projection, const Projection &p_render_projection, bool p_defer_render_buffers_commit,
			RenderFrameContext &r_context);
	void execute_frame_entry(const RenderFrameContext &p_frame_context,
			bool p_has_render_data, const String &p_cull_skip_reason, const String &p_sort_skip_reason,
			RenderFallbackReason p_cull_skip_reason_code, RenderFallbackReason p_sort_skip_reason_code,
			bool p_set_skip_metrics, bool p_clear_cull_state_on_skip);

	StageResult execute_cull_stage(const GaussianSplatRenderer::CullStageInput &p_input,
			GaussianSplatRenderer::CullStageOutput &r_output);
	StageResult execute_sort_stage(const GaussianSplatRenderer::SortStageInput &p_input,
			GaussianSplatRenderer::SortStageOutput &r_output);
	void render_sorted_splats_with_context(const GaussianSplatRenderer::RenderFrameContext &p_context);
	void reset_render_state_for_frame(const GaussianSplatRenderer::IFrameStateView *p_state_view = nullptr,
			GaussianSplatRenderer::IFrameMutationAccess *p_mutation_access = nullptr);
	void set_debug_state_orchestrator(RenderDebugStateOrchestrator *p_debug_state_orchestrator);
	void set_diagnostics_orchestrator(RenderDiagnosticsOrchestrator *p_diagnostics_orchestrator);

private:
	struct CullStage;
	struct SortStage;
	struct RasterStage;
	struct CompositeStage;
	struct RasterCompositeStage;

	GaussianSplatRenderer *renderer = nullptr;
	RenderDebugStateOrchestrator *debug_state_orchestrator = nullptr;
	RenderDiagnosticsOrchestrator *diagnostics_orchestrator = nullptr;
	std::unique_ptr<CullStage> cull_stage;
	std::unique_ptr<SortStage> sort_stage;
	std::unique_ptr<RasterStage> raster_stage;
	std::unique_ptr<CompositeStage> composite_stage;
	std::unique_ptr<RasterCompositeStage> raster_composite_stage;

	GaussianSplatRenderer::CullStageOutput cull_for_view(const Transform3D &p_world_to_camera_transform,
			const Projection &p_projection, const Size2i &p_viewport_size);
	GaussianSplatRenderer::SortStageSummary sort_for_view(const Transform3D &p_world_to_camera_transform,
			GaussianSplatRenderer::IndexDomain p_input_domain);
	void reset_debug_overlay_metrics(float p_sort_ms);
	void store_stage_metrics(const GaussianSplatRenderer::StageMetrics &p_metrics);
	void clear_stage_metrics();
	void increment_frame_counter();
	void finalize_frame_metrics(uint64_t p_frame_start_usec);

	bool execute_raster_composite_pipeline(const GaussianSplatRenderer::RenderFrameContext &p_context,
			uint64_t p_frame_start_usec, GaussianSplatRenderer::RasterStageOutput &r_raster_output,
			StageResult &r_raster_result, StageResult &r_composite_result, float &r_composite_time_ms,
			bool &r_composite_executed);

	void log_stage_result(const char *p_stage_label, const StageResult &p_result) const;
};

#endif
