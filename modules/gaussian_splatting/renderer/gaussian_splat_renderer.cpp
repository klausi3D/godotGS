#include "gaussian_splat_renderer.h"
#include "../core/gs_project_settings.h"
#include "servers/rendering/rendering_server_default.h"
#include "gpu_debug_utils.h"
#include "render_pipeline_stages.h"
#include "render_device_orchestrator.h"
#include "render_debug_state_orchestrator.h"
#include "render_diagnostics_orchestrator.h"
#include "render_sorting_orchestrator.h"
#include "render_streaming_orchestrator.h"
// RenderCullingOrchestrator removed (ISSUE-016) — merged into RenderQualityOrchestrator.
#include "render_quality_orchestrator.h"
#include "render_config_orchestrator.h"
#include "render_instancing_orchestrator.h"
#include "render_resource_orchestrator.h"
#include "render_data_orchestrator.h"
#include "render_output_orchestrator.h"
#include "instance_pipeline_contract.h"
#include "pipeline_io_contracts.h"
#include "resident_instance_contract_publisher.h"
#include "../logger/gs_debug_trace.h"
#include "../logger/startup_trace.h"
#include "core/object/callable_method_pointer.h"

using GaussianSplatting::ScopedGpuMarker;
#include "core/error/error_macros.h"
#include "core/config/project_settings.h"
#include "core/object/object.h"
#include "core/math/math_defs.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/variant/variant.h"
#include "core/templates/hash_set.h"
#include "core/math/random_pcg.h"
#include "core/templates/hashfuncs.h"
#include "core/typedefs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
// Note: scene/ includes not available in modules - using fallback rendering
// #include "scene/resources/primitive_meshes.h"
#include "servers/rendering/rendering_server_globals.h"
#include <limits>


#include "servers/rendering_server.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/performance_monitors.h"
#include "../interfaces/debug_overlay_system.h"
#include "../interfaces/interactive_state_manager.h"
#include "../interfaces/tile_rasterizer.h"
#include "../interfaces/gpu_culler.h"
#include "../interfaces/output_compositor.h"
#include "../interfaces/gpu_sorting_pipeline.h"
#include "../interfaces/overflow_auto_tuner.h"
#include "../interfaces/painterly_material_manager.h"
#include "gpu_buffer_raii.h"
#include "gpu_memory_stream.h"
#include "gpu_sorter.h"
#include "gaussian_gpu_layout.h"
#include "rendering_diagnostics.h"
#include "shader_compilation_helper.h"
#include "quantization_config.h"
#include "sorting_config.h"
#include "gpu_sorting_config.h"
#include "sorting_contract.h"
#include "../logger/gs_logger.h"
#include "../shaders/gaussian_splat.glsl.gen.h"
#include "../shaders/gs_shadow_blit.glsl.gen.h"
#include <cstdint>
#include <algorithm>
#include <memory>
#include <type_traits>
#include <initializer_list>

#include "core/object/callable_method_pointer.h"

#ifndef kLogFrameDebug
#define kLogFrameDebug 0
#endif

namespace {

constexpr uint32_t FRUSTUM_PLANE_COUNT = 6;
static constexpr real_t SORT_TRANSFORM_TOLERANCE = 1e-4f;

// Project settings helpers provided by gs_project_settings.h (gs::settings namespace).
static bool _get_bool_setting(ProjectSettings *ps, const StringName &name, bool fallback) {
    return gs::settings::get_bool(ps, name, fallback);
}

static int _get_int_setting(ProjectSettings *ps, const StringName &name, int fallback) {
    return static_cast<int>(gs::settings::get_uint(ps, name, static_cast<uint32_t>(fallback)));
}

static int _metadata_int(const Dictionary &p_metadata, const StringName &p_key, int p_default) {
    if (!p_metadata.has(p_key)) {
        return p_default;
    }
    const Variant value = p_metadata[p_key];
    if (value.get_type() == Variant::FLOAT) {
        return int((double)value);
    }
    return int(value);
}

static double _metadata_double(const Dictionary &p_metadata, const StringName &p_key, double p_default) {
    if (!p_metadata.has(p_key)) {
        return p_default;
    }
    const Variant value = p_metadata[p_key];
    if (value.get_type() == Variant::INT) {
        return double(int64_t(value));
    }
    return (double)value;
}

static bool _asset_requests_full_fidelity_runtime(const Ref<GaussianSplatAsset> &p_asset) {
    if (p_asset.is_null()) {
        return false;
    }
    const Dictionary import_metadata = p_asset->get_import_metadata();
    const int import_max_splats = _metadata_int(import_metadata, StringName("max_splats"), -1);
    const double density_multiplier = _metadata_double(import_metadata, StringName("density_multiplier"), 1.0);
    return import_max_splats == 0 && density_multiplier >= 0.999;
}

static bool _renderer_requests_conservative_full_fidelity_runtime(const GaussianSplatRenderer *p_renderer,
        const Ref<GaussianSplatAsset> &p_active_asset) {
    if (_asset_requests_full_fidelity_runtime(p_active_asset)) {
        return true;
    }
    if (!p_renderer) {
        return false;
    }
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (!director) {
        return false;
    }

    LocalVector<InstanceAssetRegistration> instance_assets;
    // Use the stable-superset collection: a quality decision over "any registered asset wants
    // full fidelity?" should not flicker as nodes pass in/out of the frustum, and the helper
    // doesn't care about per-frame visibility -- it only looks at the requests_full_fidelity_runtime
    // flag of each asset.
    director->collect_registered_assets_for_renderer(p_renderer, instance_assets);
    if (instance_assets.is_empty()) {
        return false;
    }
    for (const InstanceAssetRegistration &entry : instance_assets) {
        if (entry.requests_full_fidelity_runtime) {
            return true;
        }
    }
    if (p_active_asset.is_null()) {
        return true;
    }
    const Ref<GaussianData> active_data = p_active_asset->get_gaussian_data();
    if (active_data.is_null()) {
        return true;
    }
    for (const InstanceAssetRegistration &entry : instance_assets) {
        if (entry.data.is_null()) {
            continue;
        }
        if (entry.data == active_data) {
            return false;
        }
    }
    return true;
}

// Convert SH band level (0-3) to coefficient count limit
static uint8_t _sh_band_to_coeff_limit(int p_band) {
    // SH bands: 0 -> 1 coeff (DC only), 1 -> 4, 2 -> 9, 3 -> 16
    int clamped = CLAMP(p_band, 0, 3);
    return static_cast<uint8_t>((clamped + 1) * (clamped + 1));
}

static void _initialize_lighting_project_settings_defaults() {
    ProjectSettings *ps = ProjectSettings::get_singleton();
    if (!ps) {
        return;
    }

    static const StringName direct_light_scale_path("rendering/gaussian_splatting/lighting/direct_light_scale");
    static const StringName indirect_sh_scale_path("rendering/gaussian_splatting/lighting/indirect_sh_scale");
    static const StringName shadow_strength_path("rendering/gaussian_splatting/lighting/shadow_strength");
    static const StringName shadow_bias_scale_path("rendering/gaussian_splatting/lighting/shadow_receiver_bias_scale");
    static const StringName shadow_bias_min_path("rendering/gaussian_splatting/lighting/shadow_receiver_bias_min");
    static const StringName shadow_bias_max_path("rendering/gaussian_splatting/lighting/shadow_receiver_bias_max");

    if (!ps->has_setting(direct_light_scale_path)) {
        ps->set_setting(direct_light_scale_path, 0.5f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(direct_light_scale_path, 0.5f);

    if (!ps->has_setting(indirect_sh_scale_path)) {
        ps->set_setting(indirect_sh_scale_path, 1.0f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(indirect_sh_scale_path, 1.0f);

    if (!ps->has_setting(shadow_strength_path)) {
        ps->set_setting(shadow_strength_path, 1.0f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(shadow_strength_path, 1.0f);

    if (!ps->has_setting(shadow_bias_scale_path)) {
        ps->set_setting(shadow_bias_scale_path, 0.2f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(shadow_bias_scale_path, 0.2f);

    if (!ps->has_setting(shadow_bias_min_path)) {
        ps->set_setting(shadow_bias_min_path, 0.0f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(shadow_bias_min_path, 0.0f);

    if (!ps->has_setting(shadow_bias_max_path)) {
        ps->set_setting(shadow_bias_max_path, 0.0f); // GS_CI_ALLOW_RENDER_PATH_SETTING_MUTATION
    }
    ps->set_initial_value(shadow_bias_max_path, 0.0f);
}

class FrameLogSettingsRegistry {
public:
    static FrameLogSettingsRegistry &get_singleton() {
        static FrameLogSettingsRegistry instance;
        return instance;
    }

    void ensure_initialized() {
        if (initialized) {
            return;
        }
        ProjectSettings *ps = ProjectSettings::get_singleton();
        if (!ps) {
            return;
        }
        Callable callback = callable_mp_static(&FrameLogSettingsRegistry::_on_project_settings_changed);
        if (!ps->is_connected("settings_changed", callback)) {
            ps->connect("settings_changed", callback);
        }
        initialized = true;
        refresh();
    }

    bool is_frame_log_enabled() {
        ensure_initialized();
        return enable_all_debug || enable_frame_logging;
    }

    int get_frame_log_frequency() {
        ensure_initialized();
        return frame_log_frequency;
    }

private:
    static void _on_project_settings_changed() {
        get_singleton().refresh();
    }

    void refresh() {
        enable_all_debug = false;
        enable_frame_logging = false;
        frame_log_frequency = 300;
        if (ProjectSettings *ps = ProjectSettings::get_singleton()) {
            enable_all_debug = gs::settings::get_bool(ps,
                    "rendering/gaussian_splatting/debug/enable_all_debug", false);
            enable_frame_logging = gs::settings::get_bool(ps,
                    "rendering/gaussian_splatting/debug/enable_frame_logging", false);
            frame_log_frequency = static_cast<int>(gs::settings::get_uint(ps,
                    "rendering/gaussian_splatting/debug/frame_log_frequency",
                    static_cast<uint32_t>(frame_log_frequency)));
        }
        if (enable_all_debug && frame_log_frequency <= 0) {
            frame_log_frequency = 1;
        }
    }

    bool initialized = false;
    bool enable_all_debug = false;
    bool enable_frame_logging = false;
    int frame_log_frequency = 300;
};

static bool _is_frame_log_enabled() {
#ifdef GS_SILENCE_LOGS
    // Compile-time silencing for performance testing
    return false;
#elif kLogFrameDebug
    // Compile-time override for active development
    return true;
#elif defined(DEBUG_ENABLED)
    // PROD-1 (#626): Respect project setting even in debug builds
    // Default is false - set rendering/gaussian_splatting/debug/enable_frame_logging to true if needed
    return FrameLogSettingsRegistry::get_singleton().is_frame_log_enabled();
#else
    return false;
#endif
}

static int _get_frame_log_frequency() {
    return FrameLogSettingsRegistry::get_singleton().get_frame_log_frequency();
}

static bool _should_log_frame(uint64_t p_frame) {
    if (!_is_frame_log_enabled()) {
        return false;
    }
    const int freq = _get_frame_log_frequency();
    if (freq <= 0) {
        return false;
    }
    return p_frame == 0 || (p_frame % static_cast<uint64_t>(freq) == 0);
}

static void _trace_render_path(bool p_enabled, uint64_t p_frame, bool p_streaming_enabled, bool p_streaming_ready) {
    if (!p_enabled) {
        return;
    }
    const char *mode = (p_streaming_enabled && p_streaming_ready) ? "streaming" : "resident";
    GS_LOG_RENDERER_DEBUG(vformat("[RENDER-PATH] frame=%s mode=%s streaming_enabled=%s streaming_ready=%s",
            String::num_uint64(p_frame),
            mode, p_streaming_enabled ? "yes" : "no", p_streaming_ready ? "yes" : "no"));
}

static String _streaming_not_ready_route_uid(const char *p_state_token) {
    return String("COMMON.SKIP.STREAMING_NOT_READY.") + String(p_state_token ? p_state_token : "UNKNOWN");
}

static String _resident_not_feasible_route_uid(const String &p_reason) {
    String suffix = p_reason.strip_edges();
    if (suffix.is_empty()) {
        suffix = "unknown";
    }
    suffix = suffix.to_upper().replace(" ", "_").replace(":", "_");
    return String("COMMON.SKIP.RESIDENT_NOT_FEASIBLE.") + suffix;
}

static bool _is_typed_streaming_not_ready_route(const String &p_route_uid) {
    return p_route_uid.begins_with("COMMON.SKIP.STREAMING_NOT_READY.");
}

static String _streaming_not_ready_reason_code(const String &p_route_uid) {
    static const String prefix = "COMMON.SKIP.STREAMING_NOT_READY.";
    if (!p_route_uid.begins_with(prefix)) {
        return "streaming_not_ready_unknown";
    }
    const String suffix = p_route_uid.substr(prefix.length()).to_lower();
    if (suffix.is_empty()) {
        return "streaming_not_ready_unknown";
    }
    return String("streaming_not_ready_") + suffix;
}

static bool _has_primary_resident_render_data(const GaussianSplatRenderer &p_renderer) {
    const GaussianSplatRenderer::SceneState &scene_state = p_renderer.get_scene_state();
    if (scene_state.gaussian_data.is_valid() && scene_state.gaussian_data->get_count() > 0) {
        return true;
    }

    const GaussianSplatRenderer::ResourceState &resource_state = p_renderer.get_resource_state();
    if (!resource_state.buffer_manager.is_valid() || !resource_state.buffer_manager_initialized) {
        return false;
    }
    return resource_state.buffer_manager->get_gaussian_count() > 0;
}

static bool _projection_nearly_equal(const Projection &p_a, const Projection &p_b, real_t p_tolerance = 1e-6f) {
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			const real_t diff = Math::abs(p_a.columns[col][row] - p_b.columns[col][row]);
			if (diff > p_tolerance) {
				return false;
			}
		}
	}
	return true;
}

} // namespace

class GaussianSplatRenderer::ScopedShadowPassState {
	GaussianSplatRenderer &renderer;
	ViewState &view_state;
	Size2i saved_manual_viewport;
	RD::DataFormat saved_manual_format = RD::DATA_FORMAT_MAX;
	Transform3D saved_camera_transform;
	Projection saved_camera_projection;
	bool saved_using_scene_data = false;
	Ref<OutputCompositor> saved_output_compositor;
	bool saved_shadow_instance_filter = false;

public:
	ScopedShadowPassState(GaussianSplatRenderer &p_renderer, const ShadowPassDescriptor &p_descriptor,
			const Ref<OutputCompositor> &p_shadow_output_compositor) :
			renderer(p_renderer),
			view_state(p_renderer.get_view_state()),
			saved_manual_viewport(p_renderer.get_view_state().manual_viewport_override),
			saved_manual_format(p_renderer.get_view_state().manual_viewport_format_override),
			saved_camera_transform(p_renderer.get_view_state().last_camera_to_world_transform),
			saved_camera_projection(p_renderer.get_view_state().last_camera_projection),
			saved_using_scene_data(p_renderer.get_view_state().using_scene_data),
			saved_output_compositor(p_renderer.subsystem_state.output_compositor),
			saved_shadow_instance_filter(p_renderer.shadow_instance_filter_enabled) {
		view_state.manual_viewport_override = p_descriptor.atlas_rect.size;
		view_state.manual_viewport_format_override = p_descriptor.viewport_format;
		view_state.last_camera_to_world_transform = p_descriptor.light_transform;
		view_state.last_camera_projection = p_descriptor.light_projection;
		view_state.using_scene_data = false;
		renderer.subsystem_state.output_compositor = p_shadow_output_compositor;
		renderer.shadow_instance_filter_enabled = true;
	}

	~ScopedShadowPassState() {
		view_state.manual_viewport_override = saved_manual_viewport;
		view_state.manual_viewport_format_override = saved_manual_format;
		view_state.last_camera_to_world_transform = saved_camera_transform;
		view_state.last_camera_projection = saved_camera_projection;
		view_state.using_scene_data = saved_using_scene_data;
		renderer.subsystem_state.output_compositor = saved_output_compositor;
		renderer.shadow_instance_filter_enabled = saved_shadow_instance_filter;
	}
};

const char *GaussianSplatRenderer::_shadow_failure_reason_label(ShadowRenderFailureReason p_reason) {
	switch (p_reason) {
		case ShadowRenderFailureReason::NONE:
			return "SHADOW_RENDERED";
		case ShadowRenderFailureReason::INVALID_ATLAS_RECT:
			return "SHADOW_FAIL_INVALID_ATLAS_RECT";
		case ShadowRenderFailureReason::NO_RENDERING_DEVICE:
			return "SHADOW_FAIL_NO_DEVICE";
		case ShadowRenderFailureReason::SHADOW_COMPOSITOR_UNAVAILABLE:
			return "SHADOW_FAIL_COMPOSITOR_UNAVAILABLE";
		case ShadowRenderFailureReason::RASTERIZER_UNAVAILABLE:
			return "SHADOW_FAIL_RASTERIZER_UNAVAILABLE";
		case ShadowRenderFailureReason::DEPTH_TEXTURE_INVALID:
			return "SHADOW_FAIL_DEPTH_INVALID";
		case ShadowRenderFailureReason::DEPTH_OWNER_ALIAS_INVALID:
			return "SHADOW_FAIL_DEPTH_OWNER_ALIAS_INVALID";
		case ShadowRenderFailureReason::BLIT_FAILED:
			return "SHADOW_FAIL_BLIT";
	}
	return "SHADOW_FAIL_UNKNOWN";
}

bool GaussianSplatRenderer::_finish_shadow_render_result(const ShadowRenderResult &p_result) {
	last_shadow_render_result = p_result;
	if (last_shadow_render_result.route_label.is_empty()) {
		last_shadow_render_result.route_label = _shadow_failure_reason_label(last_shadow_render_result.failure_reason);
	}
	return last_shadow_render_result.success;
}

Error GaussianSplatRenderer::get_active_data_source(const SceneState &p_scene_state,
        const StreamingState &p_streaming_state,
        const SortingState &p_sorting_state,
        const InstancePipelineBuffers *p_instance_buffers,
        InstanceBackendPolicy p_instance_backend_policy,
        const ResourceState &p_resource_state,
        const SubsystemState &p_subsystem_state,
        SplatDataSource &r_source,
        String &r_error) {
    r_source = SplatDataSource();
    r_error = String();

    (void)p_scene_state;
    (void)p_resource_state;

    if (p_instance_buffers != nullptr &&
            (p_instance_backend_policy == InstanceBackendPolicy::RESIDENT ||
                    p_instance_backend_policy == InstanceBackendPolicy::STREAMING)) {
        if (!p_instance_buffers->atlas_gaussian_buffer.is_valid()) {
            r_error = "Instance atlas buffer unavailable";
            return ERR_UNAVAILABLE;
        }
        if (!p_subsystem_state.sorting_pipeline.is_valid()) {
            r_error = "Sorting pipeline unavailable";
            return ERR_UNAVAILABLE;
        }

        RID sort_indices = p_subsystem_state.sorting_pipeline->get_sort_indices_buffer();
        if (!sort_indices.is_valid()) {
            r_error = "Sorted indices buffer unavailable";
            return ERR_UNAVAILABLE;
        }

        r_source.source_name = p_instance_backend_policy == InstanceBackendPolicy::RESIDENT
                ? SplatDataSource::kSourceResidentInstance
                : SplatDataSource::kSourceStreaming;
        r_source.gaussian_buffer = p_instance_buffers->atlas_gaussian_buffer;
        r_source.sorted_indices = sort_indices;
        r_source.splat_count = p_sorting_state.sorted_splat_count;
        r_source.total_gaussians = p_instance_buffers->atlas_gaussian_count;
        if (r_source.total_gaussians == 0 && r_source.splat_count > 0) {
            r_source.total_gaussians = r_source.splat_count;
        }
        return OK;
    }

    if (!p_streaming_state.current_streaming_system.is_valid()) {
        r_error = "Streaming system unavailable";
        return ERR_UNCONFIGURED;
    }

    const GlobalAtlasState &atlas_state = p_streaming_state.current_streaming_system->get_global_atlas_state();
    if (!atlas_state.atlas_gaussian_buffer.is_valid()) {
        r_error = "Instance atlas buffer unavailable";
        return ERR_UNAVAILABLE;
    }
    if (!p_subsystem_state.sorting_pipeline.is_valid()) {
        r_error = "Sorting pipeline unavailable";
        return ERR_UNAVAILABLE;
    }

    RID sort_indices = p_subsystem_state.sorting_pipeline->get_sort_indices_buffer();
    if (!sort_indices.is_valid()) {
        r_error = "Sorted indices buffer unavailable";
        return ERR_UNAVAILABLE;
    }

    r_source.source_name = SplatDataSource::kSourceStreaming;
    r_source.gaussian_buffer = atlas_state.atlas_gaussian_buffer;
    r_source.sorted_indices = sort_indices;
    r_source.splat_count = p_sorting_state.sorted_splat_count;
    if (r_source.splat_count == 0) {
        r_source.splat_count = p_streaming_state.current_streaming_system->get_visible_count();
    }
    r_source.total_gaussians = atlas_state.atlas_gaussian_count;
    if (r_source.total_gaussians == 0 && r_source.splat_count > 0) {
        r_source.total_gaussians = r_source.splat_count;
    }

    return OK;
}

GaussianSplatRenderer::DataSourcePlan GaussianSplatRenderer::build_data_source_plan(const SceneState &p_scene_state,
        const StreamingState &p_streaming_state,
        const SortingState &p_sorting_state,
        const InstancePipelineBuffers *p_instance_buffers,
        InstanceBackendPolicy p_instance_backend_policy,
        const ResourceState &p_resource_state,
        const SubsystemState &p_subsystem_state) {
    // Delegate to RenderPipelineStages (T4-PR3)
    return RenderPipelineStages::build_data_source_plan(p_scene_state, p_streaming_state, p_sorting_state,
            p_instance_buffers, p_instance_backend_policy, p_resource_state, p_subsystem_state);
}

void GaussianSplatRenderer::apply_data_source_plan(const DataSourcePlan &p_plan, PerformanceMetrics &p_metrics,
        const ResourceState &p_resource_state) {
    // Delegate to RenderPipelineStages (T4-PR3)
    RenderPipelineStages::apply_data_source_plan(p_plan, p_metrics, p_resource_state);
}

GaussianSplatRenderer::RenderFramePlan GaussianSplatRenderer::build_frame_plan(const SceneState &p_scene_state,
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
        const RenderRouteDecision *p_authoritative_route_decision) {
    // Delegate to RenderPipelineStages (T4-PR3)
    return RenderPipelineStages::build_frame_plan(p_scene_state, p_streaming_state, p_sorting_state,
            p_instance_buffers, p_instance_backend_policy, p_resource_state, p_subsystem_state, p_pipeline_features, p_has_render_data, p_cull_skip_reason,
            p_sort_skip_reason, p_cull_skip_reason_code, p_sort_skip_reason_code, p_set_skip_metrics,
            p_clear_cull_state_on_skip, p_authoritative_route_decision);
}

Projection GaussianSplatRenderer::build_cull_projection(RenderDataRD *p_render_data, const Projection &p_projection) const {
	Projection cull_projection = p_projection;
	if (p_render_data && p_render_data->scene_data && p_render_data->scene_data->flip_y) {
		// Frustum plane extraction must use the same flip convention as cull/sort paths.
		cull_projection.columns[1][1] = -cull_projection.columns[1][1];
	}
	return cull_projection;
}

bool GaussianSplatRenderer::validate_cull_projection_contract(RenderDataRD *p_render_data, const Projection &p_projection,
		const Projection &p_cull_projection, const char *p_context) {
	const Projection expected = build_cull_projection(p_render_data, p_projection);
	if (_projection_nearly_equal(expected, p_cull_projection)) {
		return true;
	}

	get_performance_state().metrics.cull_projection_contract_mismatch_count++;
	const String context = p_context ? String(p_context) : String("unknown");
	WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] Cull projection contract mismatch in %s; expected shared flip_y-aware cull projection.",
			context));
	return false;
}

void GaussianSplatRenderer::_forget_tile_renderer_outputs() {
    if (subsystem_state.rasterizer.is_valid()) {
        subsystem_state.rasterizer->clear_output_resource_tracking();
    }
    if (subsystem_state.output_compositor.is_valid()) {
        subsystem_state.output_compositor->invalidate_cached_render();
    }
}

void GaussianSplatRenderer::_warn_tile_depth_copy_incompatible() {
    WARN_PRINT_ONCE("[GaussianSplatRenderer] Tile depth copy contract not satisfied; depth compositing is deterministically disabled for this frame");
}

GaussianSplatRenderer::FrameStateProvider::FrameStateProvider(GaussianSplatRenderer *p_renderer,
        const RenderFrameContext::FrameDeps *p_deps) :
        renderer_view(p_renderer),
        renderer_mut(p_renderer),
        deps(p_deps) {
}

GaussianSplatRenderer::FrameStateProvider::FrameStateProvider(const GaussianSplatRenderer *p_renderer,
        const RenderFrameContext::FrameDeps *p_deps) :
        renderer_view(p_renderer),
        renderer_mut(nullptr),
        deps(p_deps) {
}

OutputCompositor *GaussianSplatRenderer::FrameStateProvider::get_output_compositor() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->output_compositor) {
        return deps->output_compositor;
    }
    return renderer_view->get_subsystem_state().output_compositor.ptr();
}

GPUCuller *GaussianSplatRenderer::FrameStateProvider::get_gpu_culler() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->gpu_culler) {
        return deps->gpu_culler;
    }
    return renderer_view->get_subsystem_state().gpu_culler.ptr();
}

PainterlyRenderer *GaussianSplatRenderer::FrameStateProvider::get_painterly_renderer() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->painterly_renderer) {
        return deps->painterly_renderer;
    }
    return renderer_view->get_subsystem_state().painterly_renderer.ptr();
}

GPUSortingPipeline *GaussianSplatRenderer::FrameStateProvider::get_sorting_pipeline() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->sorting_pipeline) {
        return deps->sorting_pipeline;
    }
    return renderer_view->get_subsystem_state().sorting_pipeline.ptr();
}

RenderingDevice *GaussianSplatRenderer::FrameStateProvider::get_rendering_device() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->rendering_device) {
        return deps->rendering_device;
    }
    return renderer_view->get_device_state().rd;
}

const GaussianSplatRenderer::SceneState &GaussianSplatRenderer::FrameStateProvider::get_scene_state() const {
    static SceneState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->scene_state) {
        return *deps->scene_state;
    }
    return renderer_view->get_scene_state();
}

const GaussianSplatRenderer::StreamingState &GaussianSplatRenderer::FrameStateProvider::get_streaming_state() const {
    static StreamingState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->streaming_state) {
        return *deps->streaming_state;
    }
    return renderer_view->get_streaming_state();
}

const GaussianSplatRenderer::DebugState &GaussianSplatRenderer::FrameStateProvider::get_debug_state_view() const {
    static DebugState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->debug_state) {
        return *deps->debug_state;
    }
    return renderer_view->get_debug_state();
}

const GaussianSplatRenderer::SortingState &GaussianSplatRenderer::FrameStateProvider::get_sorting_state_view() const {
    static SortingState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->sorting_state) {
        return *deps->sorting_state;
    }
    return renderer_view->get_sorting_state();
}

const GaussianSplatRenderer::RenderConfig &GaussianSplatRenderer::FrameStateProvider::get_render_config_view() const {
    static RenderConfig fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->render_config) {
        return *deps->render_config;
    }
    return renderer_view->get_render_config();
}

const GaussianSplatRenderer::JacobianDebugConfig &GaussianSplatRenderer::FrameStateProvider::get_jacobian_debug_view() const {
    static JacobianDebugConfig fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->jacobian_debug) {
        return *deps->jacobian_debug;
    }
    return renderer_view->get_jacobian_debug();
}

const GaussianSplatRenderer::ResourceState &GaussianSplatRenderer::FrameStateProvider::get_resource_state_view() const {
    static ResourceState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->resource_state) {
        return *deps->resource_state;
    }
    return renderer_view->get_resource_state();
}

const GaussianSplatRenderer::FrameState &GaussianSplatRenderer::FrameStateProvider::get_frame_state_view() const {
    static FrameState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->frame_state) {
        return *deps->frame_state;
    }
    return renderer_view->get_frame_state();
}

const GaussianSplatRenderer::PerformanceState &GaussianSplatRenderer::FrameStateProvider::get_performance_state_view() const {
    static PerformanceState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->performance_state) {
        return *deps->performance_state;
    }
    return renderer_view->get_performance_state();
}

const GaussianSplatRenderer::SubsystemState &GaussianSplatRenderer::FrameStateProvider::get_subsystem_state_view() const {
    static SubsystemState fallback;
    ERR_FAIL_NULL_V(renderer_view, fallback);
    if (deps && deps->subsystem_state) {
        return *deps->subsystem_state;
    }
    return renderer_view->get_subsystem_state();
}

GaussianSplatRenderer::SortingState &GaussianSplatRenderer::FrameStateProvider::get_sorting_state_mut() {
    static SortingState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->sorting_state) {
        return *deps->sorting_state;
    }
    return renderer_mut->get_sorting_state();
}

GaussianSplatRenderer::StreamingState &GaussianSplatRenderer::FrameStateProvider::get_streaming_state_mut() {
    static StreamingState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->streaming_state) {
        return *deps->streaming_state;
    }
    return renderer_mut->get_streaming_state();
}

GaussianSplatRenderer::DebugState &GaussianSplatRenderer::FrameStateProvider::get_debug_state_mut() {
    static DebugState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->debug_state) {
        return *deps->debug_state;
    }
    return renderer_mut->get_debug_state();
}

GaussianSplatRenderer::RenderConfig &GaussianSplatRenderer::FrameStateProvider::get_render_config_mut() {
    static RenderConfig fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->render_config) {
        return *deps->render_config;
    }
    return renderer_mut->get_render_config();
}

GaussianSplatRenderer::ResourceState &GaussianSplatRenderer::FrameStateProvider::get_resource_state_mut() {
    static ResourceState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->resource_state) {
        return *deps->resource_state;
    }
    return renderer_mut->get_resource_state();
}

GaussianSplatRenderer::FrameState &GaussianSplatRenderer::FrameStateProvider::get_frame_state_mut() {
    static FrameState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->frame_state) {
        return *deps->frame_state;
    }
    return renderer_mut->get_frame_state();
}

GaussianSplatRenderer::PerformanceState &GaussianSplatRenderer::FrameStateProvider::get_performance_state_mut() {
    static PerformanceState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->performance_state) {
        return *deps->performance_state;
    }
    return renderer_mut->get_performance_state();
}

GaussianSplatRenderer::SubsystemState &GaussianSplatRenderer::FrameStateProvider::get_subsystem_state_mut() {
    static SubsystemState fallback;
    DEV_ASSERT(renderer_mut != nullptr);
    ERR_FAIL_NULL_V(renderer_mut, fallback);
    if (deps && deps->subsystem_state) {
        return *deps->subsystem_state;
    }
    return renderer_mut->get_subsystem_state();
}

const PipelineFeatureSet *GaussianSplatRenderer::FrameStateProvider::get_pipeline_features() const {
    ERR_FAIL_NULL_V(renderer_view, nullptr);
    if (deps && deps->pipeline_features) {
        return deps->pipeline_features;
    }
    return &renderer_view->pipeline_features_effective;
}

const GaussianSplatRenderer::RenderFramePlan *GaussianSplatRenderer::FrameStateProvider::get_frame_plan() const {
    return deps ? deps->frame_plan : nullptr;
}

bool GaussianSplatRenderer::_dispatch_call_on_render_thread_blocking(
        const Callable &p_callable, bool *r_dispatched, bool p_allow_timeout, uint64_t *r_request_id) {
    ERR_FAIL_NULL_V(render_thread_dispatcher, false);
    return render_thread_dispatcher->dispatch_call_on_render_thread_blocking(
            p_callable, r_dispatched, p_allow_timeout, r_request_id, "[GaussianSplatRenderer] Render-thread dispatch");
}

void GaussianSplatRenderer::_notify_render_thread_dispatch_completed(uint64_t p_request_id) {
    ERR_FAIL_NULL(render_thread_dispatcher);
    render_thread_dispatcher->notify_completed(p_request_id);
}

GaussianSplatRenderer::GaussianSplatRenderer(RenderingDevice *p_device) {
    GS_STARTUP_SCOPE("renderer_construct");
    // State buckets: frame_context_manager (view + frame metrics), device state (orchestrator-owned),
    // resource orchestrator state, streaming state (data orchestrator), debug_state_orchestrator, diagnostics_orchestrator.
    RenderingDevice *device = p_device;
    if (!device) {
        if (GaussianSplatManager *manager = GaussianSplatManager::get_singleton()) {
            device = manager->get_primary_rendering_device();
        }
    }

    _initialize_lighting_project_settings_defaults();
    render_thread_dispatcher = std::make_unique<RenderThreadDispatcher>();

    // Initialize modular interface subsystems (Phase 8 migration)
    subsystem_state.device_manager.instantiate();
    subsystem_state.device_manager->initialize(device);  // Pass current device (may be null, will acquire later)
    subsystem_state.debug_overlay_system.instantiate();
    subsystem_state.debug_overlay_system->initialize();
    subsystem_state.interactive_state_manager.instantiate();
    // Note: subsystem_state.interactive_state_manager->initialize() requires RenderingDevice,
    // will be called when device becomes available
    subsystem_state.gpu_culler.instantiate();
    // Respect project setting for LOD - don't hardcode to true
    bool lod_enabled_setting = gs::settings::get_bool(ProjectSettings::get_singleton(),
            "rendering/gaussian_splatting/lod/enabled", true);
    subsystem_state.gpu_culler->get_config().lod_enabled = lod_enabled_setting;
    subsystem_state.gpu_culler->get_config().lod_bias = 1.0f;
    subsystem_state.gpu_culler->get_config().frustum_culling = true;
    subsystem_state.gpu_culler->get_config().temporal_coherence = true;
    // Note: subsystem_state.gpu_culler->initialize() requires RenderingDevice,
    // will be called when device becomes available
    subsystem_state.output_compositor.instantiate();
    subsystem_state.output_compositor->set_device_manager(subsystem_state.device_manager);
    // Phase 15: Wire up callbacks for texture format and owner lookup
    subsystem_state.output_compositor->set_texture_format_callback([this](RenderingDevice *p_device, RID p_texture) -> RD::TextureFormat {
        return _get_texture_format(p_device, p_texture);
    });
    // Note: subsystem_state.output_compositor->initialize() requires RenderingDevice,
    // will be called when device becomes available
    subsystem_state.sorting_pipeline.instantiate();
    subsystem_state.sorting_pipeline->set_device_manager(subsystem_state.device_manager);
    subsystem_state.sorting_pipeline->set_manage_buffers(true);
    // Note: subsystem_state.sorting_pipeline->initialize() requires RenderingDevice,
    // will be called when device becomes available
    subsystem_state.overflow_auto_tuner.instantiate();
    subsystem_state.overflow_auto_tuner->set_baselines(subsystem_state.gpu_culler->get_config().importance_cull_baseline, subsystem_state.gpu_culler->get_state().tiny_splat_screen_radius_baseline);
    subsystem_state.painterly_renderer.instantiate();
    subsystem_state.painterly_material_manager.instantiate();
    // Note: subsystem_state.painterly_material_manager->initialize() requires RenderingDevice,
    // will be called when device becomes available

    // Don't instantiate painterly material here to avoid "default value" warning
    // It will be lazy-initialized when first accessed via set_painterly_material()

    // Test splat buffers remain empty until explicitly populated for debugging.
    test_data_state.positions.clear();
    test_data_state.colors.clear();
    test_data_state.scales.clear();
    subsystem_state.gpu_culler->get_state().sort_cache_angle_cos_threshold = Math::cos(Math::deg_to_rad(5.0f));
    float position_threshold = 0.05f;
    subsystem_state.gpu_culler->get_state().sort_cache_position_threshold_sq = position_threshold * position_threshold;
    if (ProjectSettings *project_settings = ProjectSettings::get_singleton()) {
        Callable settings_changed = callable_mp(this, &GaussianSplatRenderer::_mark_streaming_route_policy_dirty);
        if (!project_settings->is_connected("settings_changed", settings_changed)) {
            project_settings->connect("settings_changed", settings_changed);
        }
    }

	// Initialize orchestrators.
	pipeline_stages = std::make_unique<RenderPipelineStages>(this);
	RenderDebugStateOrchestrator::Dependencies debug_state_dependencies;
	debug_state_dependencies.renderer = this;
	debug_state_dependencies.tile_renderer = &tile_renderer_state.renderer;
	debug_state_dependencies.debug_overlay_system = &subsystem_state.debug_overlay_system;
	debug_state_dependencies.jacobian_debug = &jacobian_debug;
	debug_state_dependencies.runtime_ports.dump_pipeline_trace_to_file = &GaussianSplatRenderer::dump_pipeline_trace_to_file;
	debug_state_dependencies.runtime_ports.resolve_resource_owner = &GaussianSplatRenderer::get_resource_owner;
	debug_state_orchestrator = std::make_unique<RenderDebugStateOrchestrator>(debug_state_dependencies);

	RenderDiagnosticsOrchestrator::Dependencies diagnostics_dependencies;
	diagnostics_dependencies.renderer = this;
	diagnostics_dependencies.debug_state_orchestrator = debug_state_orchestrator.get();
	diagnostics_dependencies.build_device_capability_report = [this]() { return _build_device_capability_report(); };
	diagnostics_dependencies.runtime_ports.update_gpu_pass_metrics_from_tile_renderer =
			&GaussianSplatRenderer::update_gpu_pass_metrics_from_tile_renderer;
	diagnostics_orchestrator = std::make_unique<RenderDiagnosticsOrchestrator>(diagnostics_dependencies);
	pipeline_stages->set_debug_state_orchestrator(debug_state_orchestrator.get());
	pipeline_stages->set_diagnostics_orchestrator(diagnostics_orchestrator.get());
    device_orchestrator = std::make_unique<RenderDeviceOrchestrator>(
            this, subsystem_state.device_manager.ptr(), subsystem_state.sorting_pipeline.ptr(),
            [this](const CrossDeviceOperation &p_operation) {
                if (diagnostics_orchestrator) {
                    diagnostics_orchestrator->record_cross_device_operation(p_operation);
                }
            },
            [this](const RenderingError &p_error) {
                if (diagnostics_orchestrator) {
                    diagnostics_orchestrator->record_rendering_error(p_error);
                }
            },
            [this]() {
                if (diagnostics_orchestrator) {
                    diagnostics_orchestrator->emit_runtime_diagnostics_if_requested();
                }
            });

    device_orchestrator->initialize_device_state(device);
    if (!get_device_state().rd) {
        GS_LOG_WARN_DEFAULT("[GaussianSplatRenderer] Local RenderingDevice unavailable; GPU operations will be deferred until device is available");
    }

    RenderQualityOrchestrator::Dependencies quality_dependencies;
    quality_dependencies.renderer = this;
    quality_dependencies.gpu_culler = subsystem_state.gpu_culler.ptr();
    quality_dependencies.test_data_state = &get_test_data_state();
    quality_dependencies.runtime_ports.refresh_gpu_sorter = &GaussianSplatRenderer::refresh_gpu_sorter;
    quality_dependencies.runtime_ports.track_resource_owner = &GaussianSplatRenderer::track_resource_owner;
    quality_dependencies.runtime_ports.get_streaming_state_mut =
            static_cast<StreamingState &(GaussianSplatRenderer::*)()>(&GaussianSplatRenderer::get_streaming_state);
    quality_dependencies.runtime_ports.get_streaming_state_view =
            static_cast<const StreamingState &(GaussianSplatRenderer::*)() const>(&GaussianSplatRenderer::get_streaming_state);
    quality_orchestrator = std::make_unique<RenderQualityOrchestrator>(quality_dependencies);

    RenderSortingOrchestrator::Dependencies sorting_dependencies;
    sorting_dependencies.renderer = this;
    sorting_dependencies.gpu_culler = subsystem_state.gpu_culler.ptr();
    sorting_dependencies.sorting_pipeline = subsystem_state.sorting_pipeline.ptr();
    sorting_dependencies.performance_settings = &get_performance_settings();
    sorting_dependencies.test_data_state = &get_test_data_state();
    sorting_dependencies.device_state = &get_device_state();
    sorting_dependencies.cull_for_view =
            [this](const Transform3D &p_world_to_camera_transform, const Projection &p_projection,
                    const Size2i &p_viewport_size) {
                return _cull_for_view(p_world_to_camera_transform, p_projection, p_viewport_size);
            };
    sorting_dependencies.record_rendering_error = [this](const RenderingError &p_error) {
        if (diagnostics_orchestrator) {
            diagnostics_orchestrator->record_rendering_error(p_error);
        }
    };
    sorting_dependencies.ensure_rendering_device = [this](const char *p_context) {
        return ensure_rendering_device(p_context);
    };
    sorting_orchestrator = std::make_unique<RenderSortingOrchestrator>(sorting_dependencies);

    RenderConfigOrchestrator::Dependencies config_dependencies;
    config_dependencies.interactive_state_manager = &subsystem_state.interactive_state_manager;
    config_dependencies.painterly_renderer = &subsystem_state.painterly_renderer;
    config_dependencies.invalidate_cached_render = [this]() { invalidate_cached_render(); };
    config_dependencies.default_grading_changed_callable =
            callable_mp(this, &GaussianSplatRenderer::_on_renderer_default_grading_changed);
    config_dependencies.invalidate_grading_via_director = [this]() {
        if (GaussianSplatSceneDirector *d = GaussianSplatSceneDirector::get_singleton()) {
            d->invalidate_grading_for_renderer(this);
        }
    };
    config_dependencies.apply_interactive_state = [this](GaussianSplatRenderer::InteractiveState s) -> bool {
        return subsystem_state.interactive_state_manager.is_valid() &&
                subsystem_state.interactive_state_manager->apply_renderer_state(this, s);
    };
    config_orchestrator = std::make_unique<RenderConfigOrchestrator>(config_dependencies);

    RenderInstancingOrchestrator::Dependencies instancing_dependencies;
    instancing_dependencies.renderer = this;
    instancing_dependencies.output_compositor = subsystem_state.output_compositor.ptr();
    instancing_dependencies.pipeline_stages = pipeline_stages.get();
    instancing_dependencies.prepare_render_frame_context =
            [this](RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
                    const Projection &p_projection, const Projection &p_render_projection,
                    bool p_defer_render_buffers_commit, RenderFrameContext &r_context) {
                _prepare_render_frame_context(p_render_data, p_world_to_camera_transform, p_projection,
                        p_render_projection, p_defer_render_buffers_commit, r_context);
            };
    instancing_dependencies.render_sorted_splats =
            [this](RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
                    const Projection &p_projection, const Projection &p_render_projection,
                    bool p_defer_render_buffers_commit) {
                render_sorted_splats(p_render_data, p_world_to_camera_transform, p_projection,
                        p_render_projection, p_defer_render_buffers_commit);
            };
    instancing_orchestrator = std::make_unique<RenderInstancingOrchestrator>(instancing_dependencies);
    RenderResourceOrchestrator::Dependencies resource_dependencies;
    resource_dependencies.renderer = this;
    resource_dependencies.device_state = &get_device_state();
    resource_dependencies.performance_settings = &get_performance_settings();
    resource_dependencies.painterly_config = &get_painterly_config();
    resource_dependencies.debug_config = &get_debug_config();
    resource_dependencies.test_data_state = &get_test_data_state();
    resource_dependencies.tile_renderer_state = &get_tile_renderer_state();
    resource_dependencies.subsystem_state = &get_subsystem_state();
    resource_dependencies.pipeline_features_effective = &pipeline_features_effective;
    resource_dependencies.pipeline_features_warning_cache = &pipeline_features_warning_cache;
    resource_dependencies.runtime_ports.ensure_rendering_device = &GaussianSplatRenderer::ensure_rendering_device;
    resource_dependencies.runtime_ports.get_submission_device = &GaussianSplatRenderer::get_submission_device;
    resource_dependencies.runtime_ports.get_main_rendering_device = &GaussianSplatRenderer::get_main_rendering_device;
    resource_dependencies.runtime_ports.refresh_gpu_sorter = &GaussianSplatRenderer::refresh_gpu_sorter;
    resource_dependencies.runtime_ports.track_resource_owner = &GaussianSplatRenderer::track_resource_owner;
    resource_dependencies.runtime_ports.free_owned_resource = &GaussianSplatRenderer::free_owned_resource;
    resource_orchestrator = std::make_unique<RenderResourceOrchestrator>(resource_dependencies);
    RenderDataOrchestrator::Dependencies data_dependencies;
    data_dependencies.renderer = this;
    data_dependencies.debug_config = &get_debug_config();
    data_dependencies.performance_settings = &get_performance_settings();
    data_dependencies.culling_config = &get_subsystem_state().gpu_culler->get_config();
    data_dependencies.release_shared_dynamic_asset = [this]() { _release_shared_dynamic_asset(); };
    data_dependencies.acquire_rendering_device = [this]() { return _acquire_rendering_device(); };
    data_dependencies.invalidate_static_chunk_caches = [this](bool p_free_rids) { _invalidate_static_chunk_caches(p_free_rids); };
    data_dependencies.runtime_ports.invalidate_cached_render = &GaussianSplatRenderer::invalidate_cached_render;
    data_orchestrator = std::make_unique<RenderDataOrchestrator>(data_dependencies);
    streaming_orchestrator = std::make_unique<RenderStreamingOrchestrator>(
            RenderStreamingOrchestratorDependencies{this, data_orchestrator.get(), device_orchestrator.get()});
    RenderOutputOrchestrator::Dependencies output_dependencies;
    output_dependencies.renderer = this;
    output_dependencies.output_compositor = subsystem_state.output_compositor.ptr();
    output_dependencies.painterly_renderer = subsystem_state.painterly_renderer.ptr();
    output_dependencies.gpu_culler = subsystem_state.gpu_culler.ptr();
    output_dependencies.view_state = &get_view_state();
    output_dependencies.test_data_state = &get_test_data_state();
    output_dependencies.runtime_ports.create_gpu_resources = [this]() { _create_gpu_resources_safe(); };
    output_dependencies.runtime_ports.ensure_rendering_device = &GaussianSplatRenderer::ensure_rendering_device;
    output_dependencies.runtime_ports.get_texture_format = &GaussianSplatRenderer::get_texture_format;
    output_dependencies.runtime_ports.set_active_viewport_format = &GaussianSplatRenderer::set_active_viewport_format;
    output_dependencies.runtime_ports.set_manual_viewport_format = &GaussianSplatRenderer::set_manual_viewport_format;
    output_dependencies.runtime_ports.get_resource_owner = &GaussianSplatRenderer::get_resource_owner;
    output_dependencies.runtime_ports.render_gaussians = &GaussianSplatRenderer::render_gaussians;
    output_orchestrator = std::make_unique<RenderOutputOrchestrator>(output_dependencies);

    GaussianRenderingDiagnostics::ensure_singleton();
    if (GaussianRenderingDiagnostics::get_singleton()) {
        GaussianRenderingDiagnostics::get_singleton()->register_renderer(this);
    }

    // Register with performance monitors for streaming/VRAM telemetry
    GaussianSplattingPerformanceMonitors::create_singleton();
    if (GaussianSplattingPerformanceMonitors *monitors = GaussianSplattingPerformanceMonitors::get_singleton()) {
        monitors->register_splat_renderer(this);
    }
}

bool GaussianSplatRenderer::ensure_sort_rendering_device(const char *p_context) {
    return ensure_rendering_device(p_context);
}

RenderingDevice *GaussianSplatRenderer::get_sort_rendering_device() const {
    return get_device_state().rd;
}

SortExternalBufferState GaussianSplatRenderer::get_sort_external_buffer_state() const {
    SortExternalBufferState state;
    const ResourceState &resource_state = get_resource_state();
    if (!resource_state.buffer_manager.is_valid() || !resource_state.buffer_manager_initialized) {
        return state;
    }

    GPUBufferManager::BufferHandle manager_keys = resource_state.buffer_manager->get_sort_key_handle();
    GPUBufferManager::BufferHandle manager_indices = resource_state.buffer_manager->get_sorted_indices_handle();
    if (!manager_keys.is_valid() || !manager_indices.is_valid() || manager_keys.device != manager_indices.device) {
        return state;
    }

    state.keys_buffer = manager_keys.buffer;
    state.indices_buffer = manager_indices.buffer;
    state.device = manager_keys.device;
    state.capacity = resource_state.buffer_manager->get_buffer_capacity();
    state.valid = true;
    return state;
}

bool GaussianSplatRenderer::resize_sort_state_byte_vectors(uint32_t p_cpu_capacity, uint32_t p_key_stride_bytes, const char *p_context) {
    const uint64_t key_bytes_u64 = uint64_t(p_cpu_capacity) * uint64_t(p_key_stride_bytes);
    const uint64_t index_bytes_u64 = uint64_t(p_cpu_capacity) * uint64_t(sizeof(uint32_t));
    const uint64_t byte_limit = MIN(uint64_t(std::numeric_limits<uint32_t>::max()), uint64_t(std::numeric_limits<int>::max()));
    if (key_bytes_u64 > byte_limit || index_bytes_u64 > byte_limit) {
        GS_LOG_WARN_DEFAULT(vformat("[GaussianSplatRenderer] %s requested oversized renderer sort buffers (capacity=%s key_stride=%s key_bytes=%s index_bytes=%s limit=%s)",
                String(p_context),
                String::num_uint64(p_cpu_capacity),
                String::num_uint64(p_key_stride_bytes),
                String::num_uint64(key_bytes_u64),
                String::num_uint64(index_bytes_u64),
                String::num_uint64(byte_limit)));
        return false;
    }

    SortingState &sorting_state = get_sorting_state();
    const int key_bytes = int(key_bytes_u64);
    const int index_bytes = int(index_bytes_u64);
    if (sorting_state.sort_key_bytes.size() != key_bytes) {
        sorting_state.sort_key_bytes.resize(key_bytes);
    }
    if (sorting_state.sort_index_bytes.size() != index_bytes) {
        sorting_state.sort_index_bytes.resize(index_bytes);
    }
    return true;
}

void GaussianSplatRenderer::set_sort_buffer_binding_state(bool p_keys_external, bool p_indices_external,
        bool p_pipeline_managed, uint32_t p_capacity) {
    SortingState &sorting_state = get_sorting_state();
    sorting_state.sort_keys_external = p_keys_external;
    sorting_state.sort_indices_external = p_indices_external;
    sorting_state.sort_buffers_pipeline_managed = p_pipeline_managed;
    sorting_state.sort_buffer_capacity = p_capacity;
}

void GaussianSplatRenderer::clear_sort_buffer_binding_state() {
    SortingState &sorting_state = get_sorting_state();
    sorting_state.sort_buffer_capacity = 0;
    sorting_state.local_sort_buffer_capacity = 0;
    sorting_state.culled_position_capacity = 0;
    sorting_state.sort_key_bytes.clear();
    sorting_state.sort_index_bytes.clear();
    sorting_state.culled_position_bytes.clear();
    sorting_state.sort_keys_external = false;
    sorting_state.sort_indices_external = false;
    sorting_state.sort_buffers_pipeline_managed = false;
}

void GaussianSplatRenderer::publish_sorted_indices(const SortPublicationPayload &p_payload) {
    const uint32_t available_splats = static_cast<uint32_t>(p_payload.sorted_indices.size());
    if (!subsystem_state.gpu_culler.is_valid()) {
        return;
    }

    GPUCuller::CullingState &cull_state = subsystem_state.gpu_culler->get_state();
    if (static_cast<uint32_t>(cull_state.culled_indices.size()) != available_splats) {
        cull_state.culled_indices.resize(available_splats);
    }

    SortingState &sorting_state = get_sorting_state();
    sorting_state.sort_index_bytes.resize(int(available_splats * sizeof(uint32_t)));
    uint32_t *final_indices = reinterpret_cast<uint32_t *>(sorting_state.sort_index_bytes.ptrw());
    for (uint32_t i = 0; i < available_splats; i++) {
        const uint32_t index = p_payload.sorted_indices[i];
        cull_state.culled_indices[i] = index;
        final_indices[i] = index;
    }

    if (p_payload.sort_indices_buffer.is_valid() && !sorting_state.sort_index_bytes.is_empty()) {
        RenderingDevice *target_device = get_resource_owner(p_payload.sort_indices_buffer,
                p_payload.default_device ? p_payload.default_device : get_device_state().rd);
        if (!target_device) {
            target_device = p_payload.default_device ? p_payload.default_device : get_device_state().rd;
        }
        if (target_device) {
            target_device->buffer_update(p_payload.sort_indices_buffer, 0,
                    sorting_state.sort_index_bytes.size(), sorting_state.sort_index_bytes.ptr());
        }
    }

    sorting_state.sorted_splat_count = available_splats;
    get_frame_state().visible_splat_count.store(available_splats, std::memory_order_release);
    get_performance_state().metrics.rendered_splat_count =
            get_frame_state().visible_splat_count.load(std::memory_order_acquire);
}

GaussianSplatRenderer::~GaussianSplatRenderer() {
    if (ProjectSettings *project_settings = ProjectSettings::get_singleton()) {
        Callable settings_changed = callable_mp(this, &GaussianSplatRenderer::_mark_streaming_route_policy_dirty);
        if (project_settings->is_connected("settings_changed", settings_changed)) {
            project_settings->disconnect("settings_changed", settings_changed);
        }
    }
    if (GaussianRenderingDiagnostics::get_singleton()) {
        GaussianRenderingDiagnostics::get_singleton()->unregister_renderer(this);
    }

    // Unregister from performance monitors
    if (GaussianSplattingPerformanceMonitors *monitors = GaussianSplattingPerformanceMonitors::get_singleton()) {
        monitors->unregister_splat_renderer(this);
    }

    bool dispatch_submitted = false;
    if (_dispatch_call_on_render_thread_blocking(
                callable_mp(this, &GaussianSplatRenderer::_teardown_on_render_thread),
                &dispatch_submitted,
                false)) {
        return;
    }
    _teardown_resources();
}

// #392 / Codex PR #419 Finding 1: process-static storage for the most recent
// renderer teardown's RDM last-shutdown owned/tracked RID counts. Test-only
// instrumentation; see the header for the rationale.
bool GaussianSplatRenderer::test_last_teardown_rdm_counts_valid = false;
uint32_t GaussianSplatRenderer::test_last_teardown_rdm_owned_count = 0;
uint32_t GaussianSplatRenderer::test_last_teardown_rdm_tracked_count = 0;

void GaussianSplatRenderer::test_reset_last_teardown_rdm_counts() {
    test_last_teardown_rdm_counts_valid = false;
    test_last_teardown_rdm_owned_count = 0;
    test_last_teardown_rdm_tracked_count = 0;
}

bool GaussianSplatRenderer::test_has_last_teardown_rdm_counts() {
    return test_last_teardown_rdm_counts_valid;
}

uint32_t GaussianSplatRenderer::test_get_last_teardown_rdm_owned_count() {
    return test_last_teardown_rdm_owned_count;
}

uint32_t GaussianSplatRenderer::test_get_last_teardown_rdm_tracked_count() {
    return test_last_teardown_rdm_tracked_count;
}

void GaussianSplatRenderer::_teardown_resources() {
    bool expected = false;
    if (!teardown_resources_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    _release_shared_dynamic_asset();
    get_resource_state().deletion_queue.flush_all();

    StreamingState &streaming_state_ref = get_streaming_state();

    if (subsystem_state.output_compositor.is_valid()) {
        subsystem_state.output_compositor->clear_cached_framebuffers();
        subsystem_state.output_compositor->clear_viewport_blit_resources();
        subsystem_state.output_compositor->shutdown();
        subsystem_state.output_compositor.unref();
    }
    if (shadow_output_compositor.is_valid()) {
        shadow_output_compositor->clear_cached_framebuffers();
        shadow_output_compositor->clear_viewport_blit_resources();
        shadow_output_compositor->shutdown();
        shadow_output_compositor.unref();
    }
    shadow_output_device_id = 0;
    if (get_device_state().rd) {
        shadow_blit_state.clear(get_device_state().rd);
    }
    // Phase 8: frustum cull resources now managed by GPUCuller interface

    if (streaming_state_ref.registered_gaussian_buffer.is_valid()) {
        forget_resource_owner(streaming_state_ref.registered_gaussian_buffer);
        GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
        if (manager) {
            manager->unregister_gaussian_buffer(streaming_state_ref.registered_gaussian_buffer);
        }
        streaming_state_ref.registered_gaussian_buffer = RID();
        streaming_state_ref.registered_gaussian_data_id = ObjectID();
    }

    // Clean up tile renderer
    if (tile_renderer_state.renderer.is_valid()) {
        _forget_tile_renderer_outputs();
        tile_renderer_state.renderer->cleanup();
        tile_renderer_state.renderer.unref();
    }

    // Clean up mesh instances (disabled - scene classes not available in modules)
    test_data_state.mesh_instances.clear();

    // Clean up streaming system
    if (streaming_state_ref.current_streaming_system.is_valid()) {
        streaming_state_ref.current_streaming_system.unref();
    }
    if (streaming_state_ref.memory_stream.is_valid()) {
        streaming_state_ref.memory_stream->shutdown();
        streaming_state_ref.memory_stream.unref();
    }

    _invalidate_static_chunk_caches(true);

    // Clean up modular interface subsystems
    if (subsystem_state.gpu_culler.is_valid()) {
        subsystem_state.gpu_culler->shutdown();
        subsystem_state.gpu_culler.unref();
    }
    if (subsystem_state.interactive_state_manager.is_valid()) {
        subsystem_state.interactive_state_manager->shutdown();
        subsystem_state.interactive_state_manager.unref();
    }
    if (subsystem_state.debug_overlay_system.is_valid()) {
        subsystem_state.debug_overlay_system->shutdown();
        subsystem_state.debug_overlay_system.unref();
    }
    if (subsystem_state.sorting_pipeline.is_valid()) {
        subsystem_state.sorting_pipeline->set_sort_result_sink(this);
        subsystem_state.sorting_pipeline->set_sort_buffer_host_context(this);
        subsystem_state.sorting_pipeline->release_sort_buffers();
        subsystem_state.sorting_pipeline->shutdown();
        subsystem_state.sorting_pipeline.unref();
    }

    if (subsystem_state.painterly_renderer.is_valid()) {
        subsystem_state.painterly_renderer->free_painterly_resources(this);
        subsystem_state.painterly_renderer.unref();
    }
    // Clean up GPU buffer manager
    if (get_resource_state().buffer_manager.is_valid()) {
        get_resource_state().buffer_manager.unref();
    }
    get_resource_state().buffer_manager_initialized = false;

    if (get_sorting_state().gpu_sorter.is_valid()) {
        get_sorting_state().gpu_sorter->shutdown();
        get_sorting_state().gpu_sorter.unref();
    }

    _release_resident_contract_buffers();

    // Clean up GPU resources
    if (get_device_state().rd) {
        ResourceState &resource_state = get_resource_state();
        if (get_pipeline_state().gaussian_shader_source && get_pipeline_state().gaussian_shader_version.is_valid()) {
            get_pipeline_state().gaussian_shader_source->version_free(get_pipeline_state().gaussian_shader_version);
            get_pipeline_state().gaussian_shader_version = RID();
        }
        get_pipeline_state().gaussian_shader = RID();
        get_pipeline_state().gaussian_shader_initialized = false;
        _free_owned_resource(get_device_state().rd, resource_state.instance_buffer);
        resource_state.instance_buffer_capacity = 0;
        _free_owned_resource(get_device_state().rd, resource_state.instance_grading_buffer);
        resource_state.instance_grading_buffer_capacity = 0;
        _free_owned_resource(get_device_state().rd, resource_state.instance_visible_chunk_buffer);
        resource_state.instance_visible_chunk_capacity = 0;
        _free_owned_resource(get_device_state().rd, resource_state.instance_splat_ref_buffer);
        resource_state.instance_splat_ref_capacity = 0;
        _free_owned_resource(get_device_state().rd, resource_state.instance_counter_buffer);
        _free_owned_resource(get_device_state().rd, resource_state.instance_chunk_dispatch_buffer);
        _free_owned_resource(get_device_state().rd, resource_state.instance_indirect_count_buffer);
        _free_owned_resource(get_device_state().rd, resource_state.instance_count_buffer);
        _free_owned_resource(get_device_state().rd, test_data_state.vertex_buffer);
        _free_owned_resource(get_device_state().rd, test_data_state.position_buffer);
        _free_owned_resource(get_device_state().rd, test_data_state.scale_buffer);
        _free_owned_resource(get_device_state().rd, test_data_state.rotation_buffer);
        _free_owned_resource(get_device_state().rd, test_data_state.sh_buffer);
        // Phase 15: painterly_stroke_density_buffer now managed by PainterlyMaterialManager

    }

    if (get_pipeline_state().gaussian_shader_source) {
        memdelete(get_pipeline_state().gaussian_shader_source);
        get_pipeline_state().gaussian_shader_source = nullptr;
    }

    // Keep device manager alive until all owned RID frees have completed.
    if (subsystem_state.device_manager.is_valid()) {
        subsystem_state.device_manager->shutdown();
        // #392 / Codex PR #419 Finding 1: snapshot the RDM's just-recorded
        // last-shutdown owned/tracked RID counts into the process-static cell
        // BEFORE the RDM is unref()'d (and destroyed). The lifetime fixture
        // reads these back after the renderer is gone to feed
        // record_rdm_shutdown_counts(), restoring COUNT coverage for
        // renderer-owned RIDs (pipelines/shaders/uniform-sets/samplers/
        // framebuffers) that leak without adding buffer/texture BYTES.
        test_last_teardown_rdm_owned_count =
                subsystem_state.device_manager->get_last_shutdown_owned_resource_count();
        test_last_teardown_rdm_tracked_count =
                subsystem_state.device_manager->get_last_shutdown_tracked_resource_count();
        test_last_teardown_rdm_counts_valid = true;
        subsystem_state.device_manager.unref();
    }
}

void GaussianSplatRenderer::_teardown_on_render_thread(uint64_t p_request_id) {
    _teardown_resources();
    _notify_render_thread_dispatch_completed(p_request_id);
}

void GaussianSplatRenderer::_release_resident_contract_buffers() {
    ResourceState &resource_state = get_resource_state();

    auto release_owned_resident_buffer = [this](RID &r_buffer, uint32_t &r_buffer_size) {
        if (r_buffer.is_valid()) {
            RenderingDevice *owner = get_resource_owner(r_buffer, nullptr);
            if (owner != nullptr) {
                _free_owned_resource(owner, r_buffer);
            } else {
                r_buffer = RID();
            }
        }
        r_buffer_size = 0;
    };

    release_owned_resident_buffer(resource_state.resident_atlas_gaussian_buffer,
            resource_state.resident_atlas_gaussian_buffer_size);
    release_owned_resident_buffer(resource_state.resident_asset_meta_buffer,
            resource_state.resident_asset_meta_buffer_size);
    release_owned_resident_buffer(resource_state.resident_chunk_meta_buffer,
            resource_state.resident_chunk_meta_buffer_size);
    release_owned_resident_buffer(resource_state.resident_asset_chunk_index_buffer,
            resource_state.resident_asset_chunk_index_buffer_size);

    resource_state.instance_pipeline_atlas_generation = 0;
    resource_state.resident_atlas_gaussian_count = 0;
    resource_state.resident_dispatch_chunk_count = 0;
    resource_state.resident_max_chunk_splats = 0;
    resource_state.resident_atlas_reduced = false;
    resource_state.resident_atlas_source_count = 0;
    resource_state.resident_atlas_keep_ratio = 1.0f;
}

void GaussianSplatRenderer::_release_shared_dynamic_asset() {
    StreamingState &streaming_state_ref = get_streaming_state();

    if (!streaming_state_ref.shared_dynamic_asset_handle.is_valid()) {
        return;
    }

    if (GaussianSplatManager *manager = GaussianSplatManager::get_singleton()) {
        manager->release_dynamic_asset(streaming_state_ref.shared_dynamic_asset_handle);
    }

    streaming_state_ref.shared_dynamic_asset_handle = GaussianSplatManager::SharedDynamicAssetHandle();
}

// Phase 13: _bind_methods() moved to gaussian_splat_renderer_bindings.cpp

void GaussianSplatRenderer::_notification(int p_what) {
    // Notification handling disabled for Object-based implementation
    // Will be called manually from module initialization
}

void GaussianSplatRenderer::_mark_streaming_route_policy_dirty() {
    MutexLock lock(route_policy_cache_mutex);
    cached_streaming_route_policy_dirty = true;
}

void GaussianSplatRenderer::_refresh_streaming_route_policy_cache_locked() {
    // Caller holds route_policy_cache_mutex.
    if (!cached_streaming_route_policy_dirty) {
        return;
    }
    ProjectSettings *project_settings = ProjectSettings::get_singleton();
    cached_streaming_route_policy = gs::settings::get_streaming_route_policy(project_settings);
    cached_streaming_route_policy_source = gs::settings::get_streaming_route_policy_source(project_settings);
    cached_streaming_route_policy_dirty = false;
}

void GaussianSplatRenderer::_refresh_streaming_route_policy_cache() {
    MutexLock lock(route_policy_cache_mutex);
    _refresh_streaming_route_policy_cache_locked();
}

void GaussianSplatRenderer::_snapshot_streaming_route_policy(int &r_policy, String &r_source) {
    // Refresh-if-dirty and copy the cache out atomically so a concurrent refresh on
    // another thread cannot tear the policy/source pair (the String especially).
    MutexLock lock(route_policy_cache_mutex);
    _refresh_streaming_route_policy_cache_locked();
    r_policy = cached_streaming_route_policy;
    r_source = cached_streaming_route_policy_source;
}

GaussianSplatRenderer::RuntimeFidelityPolicy GaussianSplatRenderer::build_runtime_fidelity_policy(
        const SceneState &p_scene_state, const PerformanceSettings &p_performance_settings) const {
    RuntimeFidelityPolicy policy;
    int route_policy = gs::settings::GS_ROUTE_STREAMING;
    String route_source;
    const_cast<GaussianSplatRenderer *>(this)->_snapshot_streaming_route_policy(route_policy, route_source);
    policy.requested_route_policy = route_policy;
    policy.requested_route_policy_source = route_source;
    policy.prefer_resident_backend =
            should_prefer_resident_backend(policy.requested_route_policy, &policy.backend_preference_reason);
    policy.preserve_source_fidelity =
            _renderer_requests_conservative_full_fidelity_runtime(this, p_scene_state.active_asset);

	const uint32_t real_count = p_scene_state.gaussian_data.is_valid()
			? uint32_t(MAX(0, p_scene_state.gaussian_data->get_count()))
			: p_scene_state.payload_source_splat_count;
	if (real_count == 0) {
		policy.runtime_budget_splats = 0;
		return policy;
	}

    if (policy.preserve_source_fidelity || p_performance_settings.max_splats <= 0) {
        policy.runtime_budget_splats = real_count;
        return policy;
    }

    policy.runtime_budget_splats = MIN<uint32_t>(real_count, uint32_t(p_performance_settings.max_splats));
    return policy;
}

GaussianSplatRenderer::FrameBackendPlan GaussianSplatRenderer::build_frame_backend_plan(bool p_streaming_ready) const {
	FrameBackendPlan plan;
	plan.runtime_policy = build_runtime_fidelity_policy(get_scene_state(), get_performance_settings());
	const SceneState &scene_state = get_scene_state();
	const bool has_file_backed_payload =
			!scene_state.gaussian_data.is_valid() &&
			scene_state.payload_source_splat_count > 0;
	plan.streaming_requested = (plan.runtime_policy.requested_route_policy == gs::settings::GS_ROUTE_STREAMING) ||
			has_file_backed_payload;
	plan.prefer_resident_backend = plan.runtime_policy.prefer_resident_backend;
	plan.resident_backend_reason = plan.runtime_policy.backend_preference_reason;
	plan.streaming_backend_reason = plan.runtime_policy.backend_preference_reason;
	if (has_file_backed_payload && plan.prefer_resident_backend) {
		plan.prefer_resident_backend = false;
		plan.streaming_backend_reason = "file_backed_payload_requires_streaming";
		plan.resident_backend_reason = "file_backed_payload_requires_streaming";
	}
	plan.streaming_ready = p_streaming_ready;
	plan.should_attempt_streaming_bootstrap =
			plan.streaming_requested && !plan.prefer_resident_backend && !plan.streaming_ready;
	// World submissions (gsplatworld) own the streaming cull/sort/raster
	// contract directly. Carry that ownership bit in the backend plan so
	// the streaming path can reject empty-instance bootstrap instead of
	// inventing a synthetic primary-data instance on the fly. The
	// contract-active flag comes from apply_world_submission_contract(),
	// and the director-side probe catches the case where the contract has
	// not yet been observed by this renderer but a submission is already
	// published.
	const bool world_submission_active = world_submission_contract_active;
	bool director_has_submission = false;
	if (!world_submission_active) {
		if (const GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton()) {
			director_has_submission = director->has_world_submission_for_renderer(this);
		}
	}
	const bool any_world_submission = world_submission_active || director_has_submission;
	plan.has_active_world_submission = any_world_submission;

	// Direct-data callers (`set_gaussian_data()` on a standalone renderer
	// without any SceneDirector / world submission, e.g. tests, tools,
	// editor preview) have no instance to populate
	// `instance_pipeline_instance_cache`. Under the default streaming route
	// policy the streaming backend would then run with an empty instance
	// cache and emit zero splats — the fallback bootstrap instance that
	// used to cover this case was removed when the synthetic-instance shim
	// was pruned (commit de71685b08). Force resident here when we observe
	// direct data on the renderer with no submission, so callers don't
	// need to manually pin `route_policy=resident`.
    if (!plan.prefer_resident_backend && !any_world_submission) {
        const Ref<::GaussianData> &direct_data = get_scene_state().gaussian_data;
        if (direct_data.is_valid() && direct_data->get_count() > 0) {
            plan.prefer_resident_backend = true;
            plan.resident_backend_reason = "direct_data_no_submission";
            plan.should_attempt_streaming_bootstrap = false;
        }
    }
    return plan;
}

void GaussianSplatRenderer::initialize() {
    const uint64_t init_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : 0;
    bool dispatch_submitted = false;
    if (_dispatch_call_on_render_thread_blocking(
                callable_mp(this, &GaussianSplatRenderer::_initialize_on_render_thread),
                &dispatch_submitted)) {
        return;
    }
    if (dispatch_submitted) {
        // A render-thread callback is already queued; avoid racing it with a
        // local fallback initialization path.
        get_resource_state().gpu_initialization_pending = true;
        GS_LOG_RENDERER_WARN("[GaussianSplatRenderer] initialize fallback skipped because render-thread request is still pending");
        return;
    }

    // Manual initialization called from module
    // Try to initialize GPU resources
    const uint64_t resources_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : init_start_usec;
    _create_gpu_resources_safe();
    const double resources_ms = double((OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : resources_start_usec) - resources_start_usec) / 1000.0;

    double sorting_ms = 0.0;
    if (get_device_state().rd) {
        const uint64_t sorting_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : init_start_usec;
        initialize_sorting();
        sorting_ms = double((OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : sorting_start_usec) - sorting_start_usec) / 1000.0;
    }

    double bootstrap_ms = 0.0;
    const uint32_t gaussian_count = get_scene_state().gaussian_data.is_valid()
            ? get_scene_state().gaussian_data->get_count()
            : 0;
    if (get_scene_state().gaussian_data.is_valid()) {
        const uint64_t bootstrap_start_usec = OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : init_start_usec;
        Error data_err = _update_gpu_buffers_with_real_data();
        bootstrap_ms = double((OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : bootstrap_start_usec) - bootstrap_start_usec) / 1000.0;
        if (data_err == OK) {
            get_frame_state().visible_splat_count.store(get_scene_state().gaussian_data->get_count(), std::memory_order_release);
        } else {
            get_frame_state().visible_splat_count.store(0, std::memory_order_release);
            GS_LOG_ERROR_DEFAULT(vformat("[Hello Splat] Failed to activate gaussian data during initialization: %d", data_err));
        }
    } else {
        get_frame_state().visible_splat_count.store(0, std::memory_order_release);
    }

    const double total_init_ms = double((OS::get_singleton() ? OS::get_singleton()->get_ticks_usec() : init_start_usec) - init_start_usec) / 1000.0;
    GS_LOG_RENDERER_INFO(vformat(
            "[LoadTiming][GaussianSplatRenderer] gaussian_count=%d resource_init_ms=%.2f sorting_init_ms=%.2f bootstrap_ms=%.2f total_init_ms=%.2f",
            gaussian_count,
            resources_ms,
            sorting_ms,
            bootstrap_ms,
            total_init_ms));
}

void GaussianSplatRenderer::_initialize_on_render_thread(uint64_t p_request_id) {
    initialize();
    _notify_render_thread_dispatch_completed(p_request_id);
}

void GaussianSplatRenderer::set_painterly_material(const Ref<PainterlyMaterial> &p_material) {
    if (subsystem_state.painterly_renderer.is_valid() && subsystem_state.painterly_renderer->get_material() == p_material) {
        return;
    }

    if (subsystem_state.painterly_renderer.is_valid()) {
        subsystem_state.painterly_renderer->set_material(p_material);
    }

    if (subsystem_state.painterly_material_manager.is_valid()) {
        subsystem_state.painterly_material_manager->set_material(p_material);
    }

    if (subsystem_state.painterly_renderer.is_valid()) {
        subsystem_state.painterly_renderer->mark_material_dirty();
        subsystem_state.painterly_renderer->update_painterly_gpu_resources(this);
    }
}

Ref<PainterlyMaterial> GaussianSplatRenderer::get_painterly_material() const {
    if (subsystem_state.painterly_renderer.is_valid()) {
        return subsystem_state.painterly_renderer->get_material();
    }
    return Ref<PainterlyMaterial>();
}

void GaussianSplatRenderer::update_depth_range(float p_near, float p_far) {
    // Called from renderer_scene_render_rd.cpp before rendering.
    // Currently a no-op stub - depth range is handled via projection matrix.
    (void)p_near;
    (void)p_far;
}

void GaussianRenderFacadeState::ShadowBlitState::clear(RenderingDevice *p_device) {
    if (sampler.is_valid()) {
        RenderingDevice *owner = sampler_owner.device ? sampler_owner.device : p_device;
        if (owner) {
            owner->free(sampler);
        }
    }
    sampler = RID();
    sampler_owner.clear();

    if (shader.is_valid()) {
        RenderingDevice *owner = p_device ? p_device : sampler_owner.device;
        if (owner) {
            owner->free(shader);
        }
    }
    shader = RID();
    pipeline_cache.clear();
    device_id = 0;
}

bool GaussianSplatRenderer::_ensure_shadow_output_compositor(RenderingDevice *p_device) {
    if (!p_device) {
        return false;
    }
    if (!shadow_output_compositor.is_valid()) {
        shadow_output_compositor.instantiate();
        shadow_output_compositor->set_device_manager(subsystem_state.device_manager);
        shadow_output_compositor->set_texture_format_callback([this](RenderingDevice *p_dev, RID p_texture) -> RD::TextureFormat {
            return _get_texture_format(p_dev, p_texture);
        });
    }
    uint64_t device_id = p_device->get_device_instance_id();
    if (shadow_output_device_id != device_id) {
        shadow_output_compositor->shutdown();
        shadow_output_device_id = device_id;
    }
    if (!shadow_output_compositor->is_initialized()) {
        shadow_output_compositor->initialize(p_device);
    }
    return shadow_output_compositor->is_initialized();
}

bool GaussianSplatRenderer::_ensure_shadow_blit_resources(RenderingDevice *p_device) {
    if (!p_device) {
        return false;
    }

    uint64_t device_id = p_device->get_device_instance_id();
    if (shadow_blit_state.device_id != 0 && shadow_blit_state.device_id != device_id) {
        shadow_blit_state.clear(shadow_blit_state.sampler_owner.device ? shadow_blit_state.sampler_owner.device : p_device);
    }
    shadow_blit_state.device_id = device_id;

    if (!shadow_blit_state.shader_source_initialized) {
        if (!shadow_blit_state.shader_source) {
            shadow_blit_state.shader_source = std::make_unique<GsShadowBlitShaderRD>();
        }
        Vector<String> versions;
        versions.push_back(String());
        shadow_blit_state.shader_source->initialize(versions);
        shadow_blit_state.shader_source_initialized = true;
    }

    if (!shadow_blit_state.shader.is_valid()) {
        RID shader_version = shadow_blit_state.shader_source->version_create();
        if (!shader_version.is_valid()) {
            return false;
        }
        Vector<String> stage_sources = shadow_blit_state.shader_source->version_build_variant_stage_sources(shader_version, 0);
        shadow_blit_state.shader_source->version_free(shader_version);
        if (stage_sources.size() <= RD::SHADER_STAGE_FRAGMENT) {
            return false;
        }
        String compile_error;
        shadow_blit_state.shader = ShaderCompilationHelper::compile_graphics_shader_on_device(
                p_device,
                stage_sources[RD::SHADER_STAGE_VERTEX],
                stage_sources[RD::SHADER_STAGE_FRAGMENT],
                "gs_shadow_blit",
                Vector<String>(),
                &compile_error);
        if (!shadow_blit_state.shader.is_valid()) {
            GS_LOG_ERROR_DEFAULT(vformat("[GS Shadow] Failed to compile shadow blit shader: %s", compile_error));
            return false;
        }

        RD::PipelineRasterizationState raster_state;
        raster_state.cull_mode = RD::POLYGON_CULL_DISABLED;
        RD::PipelineDepthStencilState depth_state;
        depth_state.enable_depth_test = true;
        depth_state.enable_depth_write = true;
        depth_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
        RD::PipelineColorBlendState blend_state = RD::PipelineColorBlendState::create_disabled(0);
        shadow_blit_state.pipeline_cache.setup(shadow_blit_state.shader, RD::RENDER_PRIMITIVE_TRIANGLES,
                raster_state, RD::PipelineMultisampleState(), depth_state, blend_state, 0);
    }

    if (!shadow_blit_state.sampler.is_valid() || !shadow_blit_state.sampler_owner.matches(p_device)) {
        if (shadow_blit_state.sampler.is_valid() && shadow_blit_state.sampler_owner.device &&
                !shadow_blit_state.sampler_owner.matches(p_device)) {
            shadow_blit_state.sampler_owner.device->free(shadow_blit_state.sampler);
        }
        RD::SamplerState sampler_state;
        sampler_state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
        sampler_state.min_filter = RD::SAMPLER_FILTER_NEAREST;
        sampler_state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
        sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        sampler_state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
        sampler_state.enable_compare = false;
        shadow_blit_state.sampler = p_device->sampler_create(sampler_state);
        shadow_blit_state.sampler_owner.set(p_device);
    }

    return shadow_blit_state.shader.is_valid() && shadow_blit_state.sampler.is_valid();
}

bool GaussianSplatRenderer::_blit_shadow_depth(RID p_source_depth, RID p_shadow_fb, const Rect2i &p_atlas_rect, bool p_flip_y) {
    RenderingDevice *rd = RenderingDevice::get_singleton();
    if (!rd || !p_source_depth.is_valid() || !p_shadow_fb.is_valid()) {
        return false;
    }
    if (!rd->framebuffer_is_valid(p_shadow_fb) || !rd->texture_is_valid(p_source_depth)) {
        return false;
    }
    if (!_ensure_shadow_blit_resources(rd)) {
        return false;
    }

    UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
    RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
    ERR_FAIL_NULL_V(uniform_set_cache, false);
    ERR_FAIL_NULL_V(material_storage, false);

    RD::Uniform u_source;
    u_source.uniform_type = RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE;
    u_source.binding = 0;
    u_source.append_id(shadow_blit_state.sampler);
    u_source.append_id(p_source_depth);

    RID uniform_set = uniform_set_cache->get_cache(shadow_blit_state.shader, 0, u_source);
    if (!uniform_set.is_valid()) {
        static bool warned_invalid_shadow_blit_uniform_set = false;
        if (!warned_invalid_shadow_blit_uniform_set) {
            GS_LOG_WARN_DEFAULT("[GS Shadow] Failed to acquire shadow blit uniform set; skipping shadow depth blit.");
            warned_invalid_shadow_blit_uniform_set = true;
        }
        return false;
    }
    RD::FramebufferFormatID fb_format = rd->framebuffer_get_format(p_shadow_fb);
    RID pipeline = shadow_blit_state.pipeline_cache.get_render_pipeline(RD::INVALID_ID, fb_format);

    struct ShadowBlitPushConstant {
        float uv_scale_offset[4];
        float invert_depth;
        float pad0;
        float pad1;
        float pad2;
    } push{};

    const float y_scale = p_flip_y ? -1.0f : 1.0f;
    const float y_offset = p_flip_y ? 1.0f : 0.0f;
    push.uv_scale_offset[0] = 1.0f;
    push.uv_scale_offset[1] = y_scale;
    push.uv_scale_offset[2] = 0.0f;
    push.uv_scale_offset[3] = y_offset;
    push.invert_depth = 1.0f;

    RD::DrawListID draw_list = rd->draw_list_begin(p_shadow_fb, RD::DRAW_DEFAULT_ALL, Vector<Color>(), 1.0f, 0, p_atlas_rect);
    rd->draw_list_bind_render_pipeline(draw_list, pipeline);
    rd->draw_list_bind_uniform_set(draw_list, uniform_set, 0);
    rd->draw_list_bind_index_array(draw_list, material_storage->get_quad_index_array());
    rd->draw_list_set_push_constant(draw_list, &push, sizeof(push));
    rd->draw_list_draw(draw_list, true);
    rd->draw_list_end();

    return true;
}

bool GaussianSplatRenderer::render_shadow_depth_map(const Projection &p_light_projection, const Transform3D &p_light_transform,
        const Rect2i &p_atlas_rect, RID p_shadow_framebuffer, bool p_flip_y) {
    ShadowRenderResult result;
    result.atlas_rect = p_atlas_rect;
    static bool logged_once = false;
    if (!logged_once) {
        logged_once = true;
        float z_near = p_light_projection.get_z_near();
        float z_far = p_light_projection.get_z_far();
        WARN_PRINT(vformat("[GS Shadow] render_shadow_depth_map called: atlas_rect=(%d,%d,%d,%d) znear=%f zfar=%f flip_y=%s fb_valid=%s",
                p_atlas_rect.position.x, p_atlas_rect.position.y, p_atlas_rect.size.x, p_atlas_rect.size.y,
                z_near, z_far, p_flip_y ? "true" : "false", p_shadow_framebuffer.is_valid() ? "true" : "false"));
    }
    if (p_atlas_rect.size.x <= 0 || p_atlas_rect.size.y <= 0) {
        result.failure_reason = ShadowRenderFailureReason::INVALID_ATLAS_RECT;
        return _finish_shadow_render_result(result);
    }
    if (!ensure_rendering_device("render_shadow_depth_map")) {
        result.failure_reason = ShadowRenderFailureReason::NO_RENDERING_DEVICE;
        return _finish_shadow_render_result(result);
    }
    RenderingDevice *rd = get_device_state().rd;
    if (!rd) {
        result.failure_reason = ShadowRenderFailureReason::NO_RENDERING_DEVICE;
        return _finish_shadow_render_result(result);
    }

    if (!_ensure_shadow_output_compositor(rd)) {
        result.failure_reason = ShadowRenderFailureReason::SHADOW_COMPOSITOR_UNAVAILABLE;
        return _finish_shadow_render_result(result);
    }

    ShadowPassDescriptor shadow_pass;
    shadow_pass.light_projection = p_light_projection;
    shadow_pass.light_transform = p_light_transform;
    shadow_pass.atlas_rect = p_atlas_rect;
    shadow_pass.shadow_framebuffer = p_shadow_framebuffer;
    shadow_pass.flip_y = p_flip_y;

    Projection projection = p_light_projection;
    Projection render_projection = p_light_projection;
    bool gs_flip_y = !p_flip_y;
    if (gs_flip_y) {
        projection.columns[1][1] = -projection.columns[1][1];
        render_projection.columns[1][1] = -render_projection.columns[1][1];
    }

    ScopedShadowPassState shadow_state_guard(*this, shadow_pass, shadow_output_compositor);

    // #351: the shadow pass is not a route-selection frame. Clear any authoritative
    // decision committed by the main color frame so this pass's stage stamping derives
    // from instance_backend_policy exactly as it did before this change.
    reset_authoritative_route_decision();
    render_sorted_splats(nullptr, p_light_transform.affine_inverse(), projection, render_projection, false,
            RenderPassKind::SHADOW_MAP);

    if (!subsystem_state.rasterizer.is_valid()) {
        WARN_PRINT_ONCE("[GS Shadow] render_shadow_depth_map: rasterizer not valid after render_sorted_splats");
        result.failure_reason = ShadowRenderFailureReason::RASTERIZER_UNAVAILABLE;
        return _finish_shadow_render_result(result);
    }
    RID depth_texture = subsystem_state.rasterizer->get_depth_texture();
    RenderingDevice *depth_owner = subsystem_state.rasterizer->get_depth_texture_owner();
    result.depth_texture = depth_texture;
    result.depth_owner_valid = depth_owner != nullptr;
    if (!depth_texture.is_valid() || !depth_owner) {
        WARN_PRINT_ONCE(vformat("[GS Shadow] render_shadow_depth_map: depth invalid: tex=%s owner=%s",
                depth_texture.is_valid() ? "valid" : "null", depth_owner ? "valid" : "null"));
        result.failure_reason = ShadowRenderFailureReason::DEPTH_TEXTURE_INVALID;
        return _finish_shadow_render_result(result);
    }
    RenderingDevice *main_device = RenderingDevice::get_singleton();
    if (main_device && depth_owner && depth_owner != main_device) {
        if (!main_device->texture_is_valid(depth_texture)) {
            GS_LOG_WARN_DEFAULT("[GS Shadow] Depth texture is not visible on the main RenderingDevice; skipping shadow blit.");
            result.failure_reason = ShadowRenderFailureReason::DEPTH_OWNER_ALIAS_INVALID;
            return _finish_shadow_render_result(result);
        }
        GS_LOG_WARN_DEFAULT("[GS Shadow] Depth texture owner mismatch; using main RenderingDevice alias for shadow blit");
    }

    bool blit_ok = _blit_shadow_depth(depth_texture, p_shadow_framebuffer, p_atlas_rect, p_flip_y);
    result.blit_attempted = true;
    static bool blit_logged = false;
    if (!blit_logged) {
        blit_logged = true;
        WARN_PRINT(vformat("[GS Shadow] Shadow blit result: %s", blit_ok ? "SUCCESS" : "FAILED"));
    }
    result.success = blit_ok;
    result.failure_reason = blit_ok ? ShadowRenderFailureReason::NONE : ShadowRenderFailureReason::BLIT_FAILED;
    return _finish_shadow_render_result(result);
}

void GaussianSplatRenderer::_on_painterly_material_changed() {
    if (subsystem_state.painterly_renderer.is_valid()) {
        subsystem_state.painterly_renderer->mark_material_dirty();
        subsystem_state.painterly_renderer->update_painterly_gpu_resources(this);
    }
}

GaussianSplatRenderer::SortStageSummary GaussianSplatRenderer::sort_gaussians_for_view(
        const Transform3D &p_world_to_camera_transform, IndexDomain p_input_domain) {
    return sorting_orchestrator->sort_gaussians_for_view(p_world_to_camera_transform, p_input_domain);
}

GaussianSplatRenderer::CullStageOutput GaussianSplatRenderer::_cull_for_view(const Transform3D &p_world_to_camera_transform,
        const Projection &p_projection, const Size2i &p_viewport_size) {
    return quality_orchestrator->cull_for_view(p_world_to_camera_transform, p_projection, p_viewport_size);
}

RID GaussianSplatRenderer::_get_painterly_depth_texture() const {
    if (subsystem_state.rasterizer.is_valid() && subsystem_state.rasterizer->has_depth_output()) {
        RID tile_depth = subsystem_state.rasterizer->get_depth_texture();
        if (tile_depth.is_valid()) {
            RenderingDevice *main_device = get_main_rendering_device();
            RenderingDevice *depth_owner = subsystem_state.rasterizer->get_depth_texture_owner();
            if (main_device && depth_owner && depth_owner != main_device) {
                if (!main_device->texture_is_valid(tile_depth)) {
                    GS_LOG_WARN_DEFAULT("[Painterly] Depth texture owner mismatch and no main-device alias; skipping tile depth");
                    return RID();
                }
                GS_LOG_WARN_DEFAULT("[Painterly] Depth texture owner mismatch; using main RenderingDevice alias for painterly depth");
                return tile_depth;
            } else {
                return tile_depth;
            }
        }
    }

    if (subsystem_state.painterly_renderer.is_valid()) {
        PainterlyPassGraph *pass_graph = subsystem_state.painterly_renderer->get_pass_graph();
        if (pass_graph) {
            return pass_graph->get_shared_texture(PainterlyPassGraph::TEXTURE_DEPTH);
        }
    }

    return RID();
}

const Gaussian *GaussianSplatRenderer::_get_streamed_gaussian(uint32_t p_index) const {
    const StreamingState &streaming_state = get_streaming_state();
    if (!streaming_state.use_streamed_data || streaming_state.cached_streamed_gaussians.is_empty()) {
        return nullptr;
    }

    if (const uint32_t *offset = streaming_state.cached_streamed_index_lookup.getptr(p_index)) {
        uint32_t position = *offset;
        if (position < (uint32_t)streaming_state.cached_streamed_gaussians.size()) {
            return &streaming_state.cached_streamed_gaussians[position];
        }
    }

    return nullptr;
}

RID GaussianSplatRenderer::_get_viewport_color_target(RenderSceneBuffersRD *p_render_buffers) {
    if (!p_render_buffers) {
        return RID();
    }

    if (p_render_buffers->has_internal_texture()) {
        RID internal_texture = p_render_buffers->get_internal_texture();
        if (internal_texture.is_valid()) {
            RenderingDevice *main_device = get_main_rendering_device();
            if (!main_device || main_device->texture_is_valid(internal_texture)) {
                if (main_device) {
                    track_resource_owner(internal_texture, main_device);
                }
                return internal_texture;
            }
        }
    }

    RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
    if (texture_storage) {
        RID render_target_rid = p_render_buffers->get_render_target();
        if (render_target_rid.is_valid()) {
            RID render_target_texture = texture_storage->render_target_get_rd_texture(render_target_rid);
            if (render_target_texture.is_valid()) {
                if (RenderingDevice *main_device = get_main_rendering_device()) {
                    track_resource_owner(render_target_texture, main_device);
                }
                return render_target_texture;
            }
        }
    }

    return RID();
}

// _check_dual_state_sync was removed: it was advertised as the canonical-vs-derived
// state guardrail but only null-checked the orchestrators and validated nothing
// (a (void)p_context no-op). Keeping it gave false confidence in a codebase with a
// desync-bug history, so the no-op and its call site were deleted outright rather
// than silently retained. If a real canonical-vs-derived comparison is implemented
// later, gate it on get_debug_config().enable_state_guardrails (still exposed via
// set_debug_state_guardrails_enabled) and reintroduce an explicit check here.

void GaussianSplatRenderer::_set_manual_viewport_format(RD::DataFormat p_format, const char *p_context) {
    (void)p_context;
    get_view_state().manual_viewport_format_override = p_format;
}

void GaussianSplatRenderer::_set_active_viewport_format(RD::DataFormat p_format, const char *p_context) {
    (void)p_context;
    get_view_state().active_viewport_color_format = p_format;
}

void GaussianSplatRenderer::_prepare_render_frame_context(RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
        const Projection &p_projection, const Projection &p_render_projection, bool p_defer_render_buffers_commit,
        RenderFrameContext &r_context) {
    // Delegate to RenderPipelineStages (T4-PR3)
    pipeline_stages->prepare_frame_context(p_render_data, p_world_to_camera_transform, p_projection,
            p_render_projection, p_defer_render_buffers_commit, r_context);
}

void GaussianSplatRenderer::_run_pipeline_entry(const RenderFrameContext &p_frame_context,
        bool p_has_render_data, const String &p_cull_skip_reason, const String &p_sort_skip_reason,
        RenderFallbackReason p_cull_skip_reason_code, RenderFallbackReason p_sort_skip_reason_code,
        bool p_set_skip_metrics, bool p_clear_cull_state_on_skip) {
    // Delegate to RenderPipelineStages (T4-PR3)
    pipeline_stages->execute_frame_entry(p_frame_context, p_has_render_data, p_cull_skip_reason,
            p_sort_skip_reason, p_cull_skip_reason_code, p_sort_skip_reason_code, p_set_skip_metrics,
            p_clear_cull_state_on_skip);
}

void GaussianSplatRenderer::_run_cull_sort_pipeline_frame(RenderDataRD *p_render_data,
        const Transform3D &p_world_to_camera_transform, const Projection &p_projection,
        const Projection &p_render_projection, RenderSceneBuffersRD *p_render_buffers,
        bool p_has_render_data, const String &p_cull_skip_reason, const String &p_sort_skip_reason,
        RenderFallbackReason p_cull_skip_reason_code, RenderFallbackReason p_sort_skip_reason_code,
        bool p_set_skip_metrics, bool p_clear_cull_state_on_skip) {
    StageMetrics stage_metrics{};
    RenderFrameContext frame_context;
    _prepare_render_frame_context(p_render_data, p_world_to_camera_transform, p_projection, p_render_projection,
            p_render_buffers != nullptr, frame_context);
    frame_context.metrics = &stage_metrics;
    FrameStateProvider frame_provider(this, &frame_context.deps);
    const IFrameStateView &state_view = frame_provider;
    IFrameMutationAccess &state_mut = frame_provider;
    frame_context.state_view = &state_view;
    frame_context.mutation_access = &state_mut;
    _run_pipeline_entry(frame_context, p_has_render_data, p_cull_skip_reason, p_sort_skip_reason,
            p_cull_skip_reason_code, p_sort_skip_reason_code, p_set_skip_metrics, p_clear_cull_state_on_skip);
}

void GaussianSplatRenderer::_reset_legacy_streaming_data_path_state() {
    StreamingState &streaming_state = get_streaming_state();
    streaming_state.use_streamed_data = false;
    streaming_state.cached_streamed_gaussians.clear();
    streaming_state.cached_streamed_indices.clear();
    streaming_state.cached_streamed_source_indices.clear();
    streaming_state.cached_streamed_sh_limits.clear();
    streaming_state.cached_streamed_index_lookup.clear();
    streaming_state.current_stream_gpu_buffer = RID();
    streaming_state.streaming_gpu_splat_count = 0;
    streaming_state.streaming_gpu_total_capacity = 0;
    streaming_state.streamed_indices_generation = 0;
    streaming_state.streamed_indices_are_local = false;
    streaming_state.cached_streamed_indices_valid = false;
}

bool GaussianSplatRenderer::_try_render_resident_frame(RenderDataRD *p_render_data,
        const Transform3D &p_world_to_camera_transform, const Projection &p_projection,
        const Projection &p_render_projection, RenderSceneBuffersRD *p_render_buffers,
        String *r_reason) {
    _reset_legacy_streaming_data_path_state();

    if (r_reason) {
        *r_reason = String();
    }

    const bool has_gaussian_dataset = get_scene_state().gaussian_data.is_valid();
    bool has_buffer_manager_data = get_resource_state().buffer_manager.is_valid() &&
            get_resource_state().buffer_manager_initialized;
    if (has_buffer_manager_data) {
        has_buffer_manager_data = get_resource_state().buffer_manager->get_gaussian_count() > 0;
    }

#if defined(DEBUG_ENABLED) || kLogFrameDebug
    // DEBUG: Log instance transform in resident path
    const bool frame_logs_enabled = _is_frame_log_enabled();
    const uint64_t log_frame = get_frame_state().frame_counter;
    const bool should_log_frame = _should_log_frame(log_frame);
    if (should_log_frame) {
        const bool instance_buffers_valid = has_instance_pipeline_buffers();
        GS_LOG_RENDERER_DEBUG(vformat("[RESIDENT-DEBUG] Frame %d: instance_pipeline_buffers_valid=%s",
                log_frame, instance_buffers_valid ? "TRUE" : "FALSE"));
    }
#endif

    if (!has_gaussian_dataset && !has_buffer_manager_data) {
        // The resident contract publisher below builds the atlas from director-registered
        // InstanceRecords even when scene_state has no primary gaussian_data. Asset-backed
        // GaussianSplatNode3D scenes hit this path: they register their asset with the director
        // but never call renderer->set_gaussian_data(). Only skip when the director also has
        // nothing registered for this renderer.
        const GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
        const bool has_director_instances = director != nullptr &&
                director->get_instance_count_for_renderer(this) > 0;
        if (!has_director_instances) {
            if (r_reason) {
                *r_reason = "no_render_data";
            }
            return false;
        }
    }

    String resident_contract_reason = "resident_contract_published";
    const bool resident_contract_ready = _publish_resident_direct_data_contract(&resident_contract_reason);
    _set_instance_backend_diagnostics(InstanceBackendPolicy::RESIDENT,
            resident_contract_ready ? String("resident_contract_published") : resident_contract_reason,
            resident_contract_ready,
            String("atlas_emulation"));

    if (resident_contract_ready) {
        // #351: the resident route is now committed. Produce the authoritative
        // per-frame route decision ONCE here (the true selection point) via the
        // single producer, store it, and derive the debug route_uid from it. The
        // contract publisher above has already set instance_backend_policy=RESIDENT,
        // so build_route_decision yields route_uid INSTANCE.RESIDENT -- identical to
        // what build_frame_plan derived downstream before this change.
        const RenderRouteDecision resident_decision = RenderPipelineStages::build_route_decision(
                InstanceBackendPolicy::RESIDENT, DataSourcePlan(), true, String());
        set_authoritative_route_decision(resident_decision);
        if (debug_state_orchestrator) {
            get_debug_state().route_uid = resident_decision.route_uid;
        }
        LocalVector<Transform3D> instance_transforms;
        instance_transforms.push_back(Transform3D());
        render_instanced(p_render_data, GaussianSplatManager::SharedDynamicAssetHandle(),
                p_world_to_camera_transform, p_projection, p_render_projection, instance_transforms);
        if (r_reason) {
            *r_reason = "resident_contract_published";
        }
        return true;
    }

    if (r_reason) {
        *r_reason = resident_contract_reason;
    }
    return false;
}

void GaussianSplatRenderer::_render_resident_frame(RenderDataRD *p_render_data, const Transform3D &p_world_to_camera_transform,
        const Projection &p_projection, const Projection &p_render_projection, RenderSceneBuffersRD *p_render_buffers) {
    _try_render_resident_frame(p_render_data, p_world_to_camera_transform, p_projection,
            p_render_projection, p_render_buffers, nullptr);
}

void GaussianSplatRenderer::render_scene_instance(RenderDataRD *p_render_data) {
    // Render flow: render_scene_instance -> (streaming ? render_streaming_frame : render_resident_frame)
    // -> RenderPipelineStages::render_sorted_splats_with_context -> raster/composite.

    if (debug_state_orchestrator) {
        DebugState &debug_state = get_debug_state();
        debug_state.route_uid = RenderRouteUID::COMMON_UNSET_ROUTE;
        debug_state.sort_route_uid = RenderRouteUID::COMMON_UNSET_SORT_ROUTE;
    }
    // #351: clear the authoritative per-frame route decision so this frame's
    // selected route branch is the only producer. Downstream build_frame_plan calls
    // derive from instance_backend_policy until the branch publishes the decision.
    reset_authoritative_route_decision();

    get_resource_state().deletion_queue.process_frame();
    // Dual-state-sync guardrail removed (was a no-op); see _check_dual_state_sync deletion note.

    if (get_subsystem_state().output_compositor.is_valid()) {
        auto &output_cache = get_subsystem_state().output_compositor->get_cache_state();
        output_cache.render_buffers_commit_pending = false;
        output_cache.pending_render_buffers_size = Size2i();
        output_cache.pending_painterly_commit = false;
    }

    if (!ensure_rendering_device("render_scene_instance")) {
        if (debug_state_orchestrator) {
            get_debug_state().route_uid = RenderRouteUID::COMMON_FAIL_NO_DEVICE;
        }
        get_frame_state().visible_splat_count.store(0, std::memory_order_release);
        get_frame_state().render_time_ms = 0.0f;
        get_frame_state().sort_time_ms = 0.0f;
        get_sorting_state().sorted_splat_count = 0;
        if (get_subsystem_state().gpu_culler.is_valid()) {
            get_subsystem_state().gpu_culler->get_state().culled_indices.clear();
            get_subsystem_state().gpu_culler->get_state().culled_distances_sq.clear();
            get_subsystem_state().gpu_culler->get_state().culled_importance_weights.clear();
        }
        pipeline_stages->reset_render_state_for_frame();
        if (debug_state_orchestrator) {
            debug_state_orchestrator->store_stage_metrics(StageMetrics());
        }
        if (get_streaming_state().memory_stream.is_valid()) {
            get_streaming_state().memory_stream->end_frame();
        }
        if (get_streaming_state().current_streaming_system.is_valid()) {
            get_streaming_state().current_streaming_system->end_frame();
        }
        return;
    }

    Transform3D cam_transform = get_view_state().last_camera_to_world_transform;
    Projection cam_projection = get_view_state().last_camera_projection;

    RenderSceneBuffersRD *render_buffers_rd = nullptr;
    get_view_state().using_scene_data = false;

#if defined(DEBUG_ENABLED) || kLogFrameDebug
    // DEBUG: Track camera source
    const bool frame_logs_enabled = _is_frame_log_enabled();
    const uint64_t log_frame = get_frame_state().frame_counter;
    const bool should_log_frame = _should_log_frame(log_frame);
    String cam_source = frame_logs_enabled ? "FALLBACK (cached)" : String();
#endif

    if (p_render_data) {
        if (p_render_data->scene_data) {
            cam_transform = p_render_data->scene_data->cam_transform;
            cam_projection = p_render_data->scene_data->cam_projection;
            get_view_state().using_scene_data = true;
#if defined(DEBUG_ENABLED) || kLogFrameDebug
            if (frame_logs_enabled) {
                cam_source = "scene_data (viewport camera)";
            }
#endif
        }

        if (p_render_data->render_buffers.is_valid()) {
            render_buffers_rd = Object::cast_to<RenderSceneBuffersRD>(p_render_data->render_buffers.ptr());
        }
    }
    if (GaussianSplatting::is_debug_frame_logging_enabled()) {
        static int rb_dbg = 0;
        if (++rb_dbg <= 3) {
            GS_LOG_RENDERER_DEBUG(vformat("[RB-DBG] render_buffers_rd=%s", render_buffers_rd ? "valid" : "NULL"));
        }
    }

    get_view_state().last_camera_position = cam_transform.origin;
    get_view_state().last_camera_to_world_transform = cam_transform;
    get_view_state().last_camera_projection = cam_projection;

    // Always derive a view-space transform (world -> camera) once and reuse it throughout the frame.
    Transform3D view_transform = cam_transform.affine_inverse();
    // Apply flip_y only; gaussian pipeline uses linear view-space depth.
    Projection render_projection = cam_projection;
    if (get_view_state().using_scene_data && p_render_data && p_render_data->scene_data && p_render_data->scene_data->flip_y) {
        render_projection.columns[1][1] = -render_projection.columns[1][1];
    }
#if defined(DEBUG_ENABLED) || kLogFrameDebug
    // DEBUG: Log camera source at the configured frame-log interval (and frame 0)
    if (should_log_frame) {
        GS_LOG_RENDERER_DEBUG(vformat("[CAM-SOURCE] Frame %d: %s", log_frame, cam_source));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_transform.origin (WORLD): (%.2f, %.2f, %.2f)", cam_transform.origin.x, cam_transform.origin.y, cam_transform.origin.z));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_transform.basis[0]: (%.4f, %.4f, %.4f)", cam_transform.basis[0][0], cam_transform.basis[0][1], cam_transform.basis[0][2]));
        GS_LOG_RENDERER_DEBUG(vformat("  view_transform.origin: (%.2f, %.2f, %.2f)", view_transform.origin.x, view_transform.origin.y, view_transform.origin.z));
        // Debug projection values - all columns
        GS_LOG_RENDERER_DEBUG(vformat("  cam_projection[0]: (%.4f, %.4f, %.4f, %.4f)", cam_projection.columns[0][0], cam_projection.columns[0][1], cam_projection.columns[0][2], cam_projection.columns[0][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_projection[1]: (%.4f, %.4f, %.4f, %.4f)", cam_projection.columns[1][0], cam_projection.columns[1][1], cam_projection.columns[1][2], cam_projection.columns[1][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_projection[2]: (%.4f, %.4f, %.4f, %.4f)", cam_projection.columns[2][0], cam_projection.columns[2][1], cam_projection.columns[2][2], cam_projection.columns[2][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_projection[3]: (%.4f, %.4f, %.4f, %.4f)", cam_projection.columns[3][0], cam_projection.columns[3][1], cam_projection.columns[3][2], cam_projection.columns[3][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  render_projection[0]: (%.4f, %.4f, %.4f, %.4f)", render_projection.columns[0][0], render_projection.columns[0][1], render_projection.columns[0][2], render_projection.columns[0][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  render_projection[1]: (%.4f, %.4f, %.4f, %.4f)", render_projection.columns[1][0], render_projection.columns[1][1], render_projection.columns[1][2], render_projection.columns[1][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  render_projection[2]: (%.4f, %.4f, %.4f, %.4f)", render_projection.columns[2][0], render_projection.columns[2][1], render_projection.columns[2][2], render_projection.columns[2][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  render_projection[3]: (%.4f, %.4f, %.4f, %.4f)", render_projection.columns[3][0], render_projection.columns[3][1], render_projection.columns[3][2], render_projection.columns[3][3]));
        GS_LOG_RENDERER_DEBUG(vformat("  cam_projection.near/far: %.4f / %.4f", cam_projection.get_z_near(), cam_projection.get_z_far()));
        if (p_render_data && p_render_data->scene_data) {
            GS_LOG_RENDERER_DEBUG(vformat("  scene_data->flip_y: %s", p_render_data->scene_data->flip_y ? "TRUE" : "FALSE"));
        }
    }
#endif

    if (get_subsystem_state().painterly_renderer.is_valid() &&
            get_subsystem_state().painterly_renderer->is_material_dirty()) {
        get_subsystem_state().painterly_renderer->update_painterly_gpu_resources(this);
    }

    // Update interactive state if needed
    if (get_interactive_state_config().state_dirty && get_subsystem_state().interactive_state_manager.is_valid()) {
        get_subsystem_state().interactive_state_manager->update_renderer_state_uniforms(this);
    }

    // Update streaming system if active.
    FrameBackendPlan backend_plan =
            build_frame_backend_plan(get_streaming_state().current_streaming_system.is_valid());
    _set_route_policy_diagnostics(backend_plan.runtime_policy.requested_route_policy,
            backend_plan.runtime_policy.requested_route_policy_source.utf8().get_data());
    static uint64_t render_debug_count = 0;
    const auto &debug_config = get_debug_config();
    const bool trace_enabled = GaussianSplatting::debug_trace_is_enabled();
    const bool render_log_enabled = debug_config.enable_frame_logging ||
            debug_config.enable_data_logging ||
            debug_config.enable_all_debug;
    if (render_log_enabled && (++render_debug_count % 300) == 1) {
        GS_LOG_RENDERER_DEBUG(vformat("[RENDER-DBG] streaming_requested=%s streaming_ready=%s",
                backend_plan.streaming_requested ? "yes" : "no",
                backend_plan.streaming_ready ? "yes" : "no"));
    }
#if defined(DEBUG_ENABLED) || kLogFrameDebug
    if (should_log_frame) {
        _trace_render_path(true, log_frame,
                backend_plan.streaming_requested && !backend_plan.prefer_resident_backend,
                backend_plan.streaming_ready);
    }
#endif
    if (backend_plan.should_attempt_streaming_bootstrap && streaming_orchestrator) {
        if (streaming_orchestrator->ensure_instance_streaming_system(backend_plan)) {
            backend_plan.streaming_ready = get_streaming_state().current_streaming_system.is_valid();
        }
        backend_plan.should_attempt_streaming_bootstrap = false;
    }
    // DEBUG: Track why render_streaming_frame might not be called
    static int render_gate_counter = 0;
    if (trace_enabled && ++render_gate_counter % 60 == 1) {
        GaussianSplatting::debug_trace_record_event("render_gate",
                vformat("streaming_requested=%s streaming_ready=%s",
                        backend_plan.streaming_requested ? "YES" : "no",
                        backend_plan.streaming_ready ? "YES" : "no"),
                false);
    }
    if (backend_plan.prefer_resident_backend) {
        String resident_attempt_reason;
        if (_try_render_resident_frame(p_render_data, view_transform, cam_projection, render_projection,
                    render_buffers_rd, &resident_attempt_reason)) {
            _set_instance_backend_diagnostics(InstanceBackendPolicy::RESIDENT,
                    backend_plan.resident_backend_reason,
                    is_instance_contract_ready(),
                    get_instance_contract_shape());
            return;
        }

        const String resident_rejection_reason =
                vformat("%s_not_feasible:%s", backend_plan.resident_backend_reason, resident_attempt_reason);
        const String resident_rejection_route_uid = _resident_not_feasible_route_uid(resident_attempt_reason);
        // #351 (option B): the single-route / no-alternate-fallback invariants are
        // now carried on the route decision and READ here instead of being enforced
        // by the WARN string alone. The skip decision keeps the struct defaults
        // (single_route_per_frame = true, alternate_backend_fallback_forbidden = true).
        RenderRouteDecision resident_skip_decision;
        resident_skip_decision.valid = true;
        resident_skip_decision.selected_backend = RenderRouteBackend::NONE;
        resident_skip_decision.selected_backend_name =
                GaussianRenderPipeline::render_route_backend_to_string(RenderRouteBackend::NONE);
        resident_skip_decision.route_uid = resident_rejection_route_uid;
        resident_skip_decision.reason = resident_rejection_reason;
        resident_skip_decision.has_render_data = false;
        set_authoritative_route_decision(resident_skip_decision);
        if (debug_state_orchestrator) {
            get_debug_state().route_uid = resident_rejection_route_uid;
        }
        publish_route_skip_stage_metrics(resident_rejection_route_uid, InstanceBackendPolicy::RESIDENT,
                String("resident_not_feasible_") + resident_attempt_reason,
                "Cull skipped: resident route not feasible",
                RenderFallbackReason::DATA_UNAVAILABLE);
        _set_instance_backend_diagnostics(InstanceBackendPolicy::RESIDENT,
                resident_rejection_reason,
                false,
                "atlas_emulation");
        // The invariant flags drive the hard skip: with single_route_per_frame /
        // alternate_backend_fallback_forbidden set we never fall through to an
        // alternate backend on the same frame. Reading them here makes the flags
        // load-bearing while preserving the existing skip+warn behavior.
        const bool enforce_single_route = resident_skip_decision.single_route_per_frame ||
                resident_skip_decision.alternate_backend_fallback_forbidden;
        if (enforce_single_route) {
            WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] Resident route rejected (reason=%s); frame skipped to preserve single-route-per-frame contract.",
                    resident_rejection_reason));
        }
        // The single-route invariant always holds (flag defaults true); skip the frame
        // rather than attempting an alternate backend.
        return;
    }
    if (backend_plan.streaming_requested && backend_plan.streaming_ready) {
        _set_instance_backend_diagnostics(InstanceBackendPolicy::STREAMING,
                backend_plan.streaming_backend_reason, has_instance_pipeline_buffers(), "atlas_emulation");
        // #351: streaming is the committed route for this frame. Produce the
        // authoritative decision ONCE here via the single producer and derive the
        // debug route_uid from it; the streaming render path's downstream
        // build_frame_plan consumes this same decision instead of re-deriving it.
        const RenderRouteDecision streaming_decision = RenderPipelineStages::build_route_decision(
                InstanceBackendPolicy::STREAMING, DataSourcePlan(), true, String());
        set_authoritative_route_decision(streaming_decision);
        if (debug_state_orchestrator) {
            get_debug_state().route_uid = streaming_decision.route_uid;
        }
        const bool streaming_frame_rendered = streaming_orchestrator &&
                streaming_orchestrator->render_streaming_frame(
                        p_render_data, cam_transform, view_transform, cam_projection, render_projection, render_buffers_rd,
                        backend_plan);
        if (streaming_frame_rendered) {
            _set_instance_backend_diagnostics(InstanceBackendPolicy::STREAMING,
                    backend_plan.streaming_contract_ready_reason,
                    is_instance_contract_ready(),
                    get_instance_contract_shape());
            return;
        }
    }
    // Streaming was requested. Either bootstrap failed, ready state was false, or
    // render_streaming_frame() rejected the frame. Hard-fail with the typed skip
    // route rather than silently falling through to the resident render path —
    // that bounce is exactly the fallback-for-fallback sprawl we are retiring.
    if (backend_plan.streaming_requested) {
        String fallback_route_uid;
        if (debug_state_orchestrator) {
            DebugState &debug_state = get_debug_state();
            const bool prior_is_typed = !debug_state.route_uid.is_empty() &&
                    _is_typed_streaming_not_ready_route(debug_state.route_uid) &&
                    debug_state.route_uid != RenderRouteUID::COMMON_SKIP_STREAMING_NOT_READY;
            if (!prior_is_typed) {
                const char *state_token = backend_plan.streaming_ready ? "UNKNOWN" : "MISSING_STREAMING_SYSTEM";
                debug_state.route_uid = _streaming_not_ready_route_uid(state_token);
            }
            fallback_route_uid = debug_state.route_uid;
        }
        if (fallback_route_uid.is_empty()) {
            const char *state_token = backend_plan.streaming_ready ? "UNKNOWN" : "MISSING_STREAMING_SYSTEM";
            fallback_route_uid = _streaming_not_ready_route_uid(state_token);
        }
        const String &reason = backend_plan.streaming_ready
                ? backend_plan.streaming_not_ready_fallback_reason
                : backend_plan.streaming_unavailable_fallback_reason;
        // #351 (option B): record the skip as the authoritative decision (overriding
        // any streaming decision committed above when render_streaming_frame rejected
        // the frame) and read the single-route / no-alternate-fallback invariants from
        // it. Defaults stay true, so the hard skip+warn behavior is preserved.
        RenderRouteDecision streaming_skip_decision;
        streaming_skip_decision.valid = true;
        streaming_skip_decision.selected_backend = RenderRouteBackend::NONE;
        streaming_skip_decision.selected_backend_name =
                GaussianRenderPipeline::render_route_backend_to_string(RenderRouteBackend::NONE);
        streaming_skip_decision.route_uid = fallback_route_uid;
        streaming_skip_decision.reason = reason;
        streaming_skip_decision.has_render_data = false;
        set_authoritative_route_decision(streaming_skip_decision);
        publish_route_skip_stage_metrics(fallback_route_uid, InstanceBackendPolicy::STREAMING,
                _streaming_not_ready_reason_code(fallback_route_uid),
                "Cull skipped: streaming path not ready",
                RenderFallbackReason::STREAMING_DATA_UNAVAILABLE);
        _set_instance_backend_diagnostics(InstanceBackendPolicy::STREAMING, reason,
                has_instance_pipeline_buffers(), "atlas_emulation");
        // The invariant flags drive the hard skip: never fall through to the resident
        // render path on the same frame once streaming was requested. Reading them
        // here makes the flags load-bearing while preserving the existing skip+warn.
        const bool enforce_single_route = streaming_skip_decision.single_route_per_frame ||
                streaming_skip_decision.alternate_backend_fallback_forbidden;
        if (enforce_single_route) {
            WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] Streaming requested but not ready (route=%s); frame skipped to preserve single-route-per-frame contract.",
                    fallback_route_uid));
        }
        // The single-route invariant always holds (flag defaults true); skip the frame
        // rather than bouncing to the resident path.
        return;
    }
    // Streaming was not requested at all (explicit resident policy). Run the
    // resident render path normally; this is not a bounce.
    _render_resident_frame(p_render_data, view_transform, cam_projection, render_projection, render_buffers_rd);
}

void GaussianSplatRenderer::tick_streaming_only(const Transform3D &p_camera_to_world_transform, const Projection &p_projection) {
    // When route policy is resident, skip streaming tick entirely — don't create
    // or update the streaming system.  This eliminates all per-frame streaming
    // overhead (visibility scan, prefetch, eviction, worker threads).
    const FrameBackendPlan backend_plan =
            build_frame_backend_plan(get_streaming_state().current_streaming_system.is_valid());
    if (backend_plan.prefer_resident_backend || !backend_plan.streaming_requested) {
        return;
    }
    if (!streaming_orchestrator) {
        return;
    }

    get_view_state().last_camera_to_world_transform = p_camera_to_world_transform;
    get_view_state().last_camera_projection = p_projection;

    streaming_orchestrator->tick_streaming_only(p_camera_to_world_transform, p_projection, backend_plan);
}

void GaussianSplatRenderer::render_gaussians(RenderDataRD *p_render_data, const PagedArray<RID> &p_instances) {
    render_scene_instance(p_render_data);
    // Force continuous redraw for gaussian splats (sorting depends on camera)
    RenderingServerDefault::redraw_request();
}

void GaussianSplatRenderer::render_sorted_splats(RenderDataRD *p_render_data,
		const Transform3D &p_world_to_camera_transform, const Projection &p_projection, const Projection &p_render_projection,
	bool p_defer_render_buffers_commit, RenderPassKind p_pass_kind) {
	if (debug_state_orchestrator) {
		DebugState &debug_state = get_debug_state();
		debug_state.sort_route_uid = RenderRouteUID::COMMON_UNSET_SORT_ROUTE;
	}
	StageMetrics stage_metrics{};
	RenderFrameContext frame_context;
	_prepare_render_frame_context(p_render_data, p_world_to_camera_transform, p_projection, p_render_projection,
			p_defer_render_buffers_commit, frame_context);
	frame_context.pass_kind = p_pass_kind;
	frame_context.metrics = &stage_metrics;
	FrameStateProvider frame_provider(this, &frame_context.deps);
	const IFrameStateView &state_view = frame_provider;
	IFrameMutationAccess &state_mut = frame_provider;
	frame_context.state_view = &state_view;
	frame_context.mutation_access = &state_mut;
	const InstancePipelineBuffers *instance_buffers = has_instance_pipeline_buffers() ? &get_instance_pipeline_buffers() : nullptr;
	RenderFramePlan frame_plan = build_frame_plan(state_view.get_scene_state(), state_view.get_streaming_state(), state_view.get_sorting_state_view(),
			instance_buffers, get_instance_backend_policy(), state_view.get_resource_state_view(), state_view.get_subsystem_state_view(), state_view.get_pipeline_features(),
			true, String(), String(), RenderFallbackReason::NONE, RenderFallbackReason::NONE, false, false,
			&get_authoritative_route_decision());
    frame_context.deps.frame_plan = &frame_plan;
    DEV_ASSERT(frame_context.deps.frame_plan);
	ERR_FAIL_COND(!frame_context.deps.validate());
	const FrameState &frame_state = state_view.get_frame_state_view();
	const SortingState &sorting_state = state_view.get_sorting_state_view();
	frame_context.snapshot.valid = true;
	frame_context.snapshot.visible_splats = frame_state.visible_splat_count.load(std::memory_order_acquire);
	frame_context.snapshot.sorted_splats = sorting_state.sorted_splat_count;
	frame_context.snapshot.cull_visible_domain = has_instance_pipeline_buffers() ?
			IndexDomain::CHUNK_REF :
			IndexDomain::GAUSSIAN_GLOBAL;
	frame_context.snapshot.sorted_index_domain = has_instance_pipeline_buffers() ?
			IndexDomain::SPLAT_REF :
			IndexDomain::GAUSSIAN_GLOBAL;
	pipeline_stages->render_sorted_splats_with_context(frame_context);
}

RID GaussianSplatRenderer::get_final_texture() const {
    if (subsystem_state.output_compositor.is_valid()) {
        return subsystem_state.output_compositor->get_final_render_texture();
    }
    return RID();
}

bool GaussianSplatRenderer::has_rendered_content() const {
    if (subsystem_state.output_compositor.is_valid()) {
        return subsystem_state.output_compositor->get_has_valid_render()
                && get_frame_state().visible_splat_count.load(std::memory_order_acquire) > 0;
    }
    return false;
}

AABB GaussianSplatRenderer::get_aabb() const {
    if (get_scene_state().gaussian_data.is_valid()) {
        return get_scene_state().gaussian_data->get_aabb();
    }
    return AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)); // Default AABB
}

GaussianSplatRenderer::SceneState &GaussianSplatRenderer::get_scene_state() {
    static SceneState fallback;
    ERR_FAIL_NULL_V(data_orchestrator, fallback);
    return data_orchestrator->access_scene_state_mutable();
}

const GaussianSplatRenderer::SceneState &GaussianSplatRenderer::get_scene_state() const {
    static SceneState fallback;
    ERR_FAIL_NULL_V(data_orchestrator, fallback);
    return data_orchestrator->get_scene_state();
}

GaussianSplatRenderer::RenderConfig &GaussianSplatRenderer::get_render_config() {
    static RenderConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_render_config();
}

const GaussianSplatRenderer::RenderConfig &GaussianSplatRenderer::get_render_config() const {
    static RenderConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_render_config();
}

GaussianSplatRenderer::DeviceState &GaussianSplatRenderer::get_device_state() {
    static DeviceState fallback;
    ERR_FAIL_NULL_V(device_orchestrator, fallback);
    return device_orchestrator->get_device_state();
}

const GaussianSplatRenderer::DeviceState &GaussianSplatRenderer::get_device_state() const {
    static DeviceState fallback;
    ERR_FAIL_NULL_V(device_orchestrator, fallback);
    return device_orchestrator->get_device_state();
}

GaussianSplatRenderer::PipelineState &GaussianSplatRenderer::get_pipeline_state() {
    static PipelineState fallback;
    ERR_FAIL_NULL_V(resource_orchestrator, fallback);
    return resource_orchestrator->get_pipeline_state();
}

const GaussianSplatRenderer::PipelineState &GaussianSplatRenderer::get_pipeline_state() const {
    static PipelineState fallback;
    ERR_FAIL_NULL_V(resource_orchestrator, fallback);
    return resource_orchestrator->get_pipeline_state();
}

GaussianSplatRenderer::PerformanceSettings &GaussianSplatRenderer::get_performance_settings() {
    static PerformanceSettings fallback;
    ERR_FAIL_NULL_V(quality_orchestrator, fallback);
    return quality_orchestrator->get_performance_settings();
}

const GaussianSplatRenderer::PerformanceSettings &GaussianSplatRenderer::get_performance_settings() const {
    static PerformanceSettings fallback;
    ERR_FAIL_NULL_V(quality_orchestrator, fallback);
    return quality_orchestrator->get_performance_settings();
}

GaussianSplatRenderer::CullingConfig &GaussianSplatRenderer::get_culling_config() {
    static CullingConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_culling_config();
}

const GaussianSplatRenderer::CullingConfig &GaussianSplatRenderer::get_culling_config() const {
    static CullingConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_culling_config();
}

GaussianSplatRenderer::PainterlyConfig &GaussianSplatRenderer::get_painterly_config() {
    static PainterlyConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_painterly_config();
}

const GaussianSplatRenderer::PainterlyConfig &GaussianSplatRenderer::get_painterly_config() const {
    static PainterlyConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_painterly_config();
}

GaussianSplatRenderer::InteractiveStateConfig &GaussianSplatRenderer::get_interactive_state_config() {
    static InteractiveStateConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_interactive_state();
}

const GaussianSplatRenderer::InteractiveStateConfig &GaussianSplatRenderer::get_interactive_state_config() const {
    static InteractiveStateConfig fallback;
    ERR_FAIL_NULL_V(config_orchestrator, fallback);
    return config_orchestrator->get_interactive_state();
}

GaussianSplatRenderer::SortingState &GaussianSplatRenderer::get_sorting_state() {
    static SortingState fallback;
    ERR_FAIL_NULL_V(sorting_orchestrator, fallback);
    return sorting_orchestrator->access_sorting_state_mutable();
}

const GaussianSplatRenderer::SortingState &GaussianSplatRenderer::get_sorting_state() const {
    static const SortingState fallback;
    ERR_FAIL_NULL_V(sorting_orchestrator, fallback);
    return sorting_orchestrator->get_sorting_state();
}

GaussianSplatRenderer::StreamingState &GaussianSplatRenderer::get_streaming_state() {
    static StreamingState fallback;
    ERR_FAIL_NULL_V(data_orchestrator, fallback);
    return data_orchestrator->access_streaming_state_mutable();
}

const GaussianSplatRenderer::StreamingState &GaussianSplatRenderer::get_streaming_state() const {
    static StreamingState fallback;
    ERR_FAIL_NULL_V(data_orchestrator, fallback);
    return data_orchestrator->get_streaming_state();
}

GaussianSplatRenderer::ResourceState &GaussianSplatRenderer::get_resource_state() {
    static ResourceState fallback;
    ERR_FAIL_NULL_V(resource_orchestrator, fallback);
    return resource_orchestrator->get_resource_state();
}

const GaussianSplatRenderer::ResourceState &GaussianSplatRenderer::get_resource_state() const {
    static ResourceState fallback;
    ERR_FAIL_NULL_V(resource_orchestrator, fallback);
    return resource_orchestrator->get_resource_state();
}

bool GaussianSplatRenderer::get_submission_residency_hint(int32_t *r_hint, String *r_source) const {
    ERR_FAIL_NULL_V(r_hint, false);

    if (world_submission_contract_active && world_submission_has_residency_hint) {
        *r_hint = world_submission_residency_hint;
        if (r_source) {
            *r_source = "world_submission";
        }
        return true;
    }

    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    if (director == nullptr) {
        if (r_source) {
            *r_source = "director_unavailable";
        }
        return false;
    }

    return director->get_submission_residency_hint_for_renderer(this, r_hint, r_source);
}

bool GaussianSplatRenderer::should_prefer_resident_backend(int p_requested_route_policy, String *r_reason) const {
    if (r_reason) {
        *r_reason = "requested_streaming_policy";
    }

    if (p_requested_route_policy == gs::settings::GS_ROUTE_RESIDENT) {
        if (r_reason) {
            *r_reason = "requested_resident_policy";
        }
        return true;
    }

    int32_t submission_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
    String submission_hint_source;
    if (!get_submission_residency_hint(&submission_hint, &submission_hint_source)) {
        return false;
    }

    if (submission_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT) {
        if (r_reason) {
            *r_reason = submission_hint_source.is_empty()
                    ? String("submission_hint_resident")
                    : vformat("submission_hint_resident:%s", submission_hint_source);
        }
        return true;
    }

    if (r_reason) {
        *r_reason = submission_hint_source.is_empty()
                ? String("submission_hint_streaming")
                : vformat("submission_hint_streaming:%s", submission_hint_source);
    }
    return false;
}

void GaussianSplatRenderer::_set_route_policy_diagnostics(int p_requested_route_policy, const char *p_policy_source) {
    if (!debug_state_orchestrator) {
        return;
    }
    DebugState &debug_state = get_debug_state();
    debug_state.requested_route_policy = gs::settings::get_streaming_route_policy_token(p_requested_route_policy);
    debug_state.requested_route_policy_source = p_policy_source ? String(p_policy_source) : String("default_fallback");
}

void GaussianSplatRenderer::_set_instance_backend_diagnostics(InstanceBackendPolicy p_backend_policy,
        const String &p_reason, bool p_contract_ready, const String &p_contract_shape) {
    if (!debug_state_orchestrator) {
        return;
    }
    DebugState &debug_state = get_debug_state();
    debug_state.instance_backend_policy = instance_backend_policy_to_string(p_backend_policy);
    debug_state.backend_selection_reason = p_reason;
    debug_state.instance_contract_shape = p_contract_shape;
    debug_state.instance_contract_ready = p_contract_ready;
}

bool GaussianSplatRenderer::_publish_resident_direct_data_contract(String *r_reason) {
    return ResidentInstanceContractPublisher::publish_resident_direct_data_contract(this, r_reason);
}

void GaussianSplatRenderer::publish_instance_pipeline_contract(const InstancePipelineBuffers &p_buffers,
        const PublishedInstanceAssetRemap &p_remap, InstanceBackendPolicy p_backend_policy,
        uint64_t p_source_generation, const String &p_contract_shape) {
    if (instance_pipeline_buffers_valid &&
            instance_backend_policy == InstanceBackendPolicy::RESIDENT &&
            p_backend_policy != InstanceBackendPolicy::RESIDENT) {
        _release_resident_contract_buffers();
    }

    instance_pipeline_buffers = p_buffers;
    instance_asset_remap = p_remap;
    instance_backend_policy = p_backend_policy;
    instance_contract_shape = p_contract_shape;
    instance_contract_source_generation = p_source_generation;

    StreamingState &streaming_state = get_streaming_state();
    if (instance_pipeline_buffers.atlas_gaussian_count == 0 &&
            streaming_state.shared_dynamic_asset_handle.is_valid()) {
        instance_pipeline_buffers.atlas_gaussian_count = streaming_state.shared_dynamic_asset_handle.gaussian_count;
    }
    if (instance_pipeline_buffers.atlas_gaussian_count == 0 &&
            instance_pipeline_buffers.max_visible_splats > 0) {
        instance_pipeline_buffers.atlas_gaussian_count = instance_pipeline_buffers.max_visible_splats;
        WARN_PRINT_ONCE("[GaussianSplatRenderer] atlas_gaussian_count missing; using max_visible_splats as bounds.");
    }
    instance_pipeline_buffers_valid = true;
}

void GaussianSplatRenderer::set_instance_pipeline_buffers(const InstancePipelineBuffers &p_buffers) {
    publish_instance_pipeline_contract(p_buffers, instance_asset_remap, instance_backend_policy,
            instance_contract_source_generation, instance_contract_shape);
}

void GaussianSplatRenderer::clear_instance_pipeline_buffers() {
    _release_resident_contract_buffers();
    instance_pipeline_buffers = InstancePipelineBuffers();
    instance_asset_remap.clear();
    instance_pipeline_buffers_valid = false;
    instance_backend_policy = InstanceBackendPolicy::NONE;
    instance_contract_shape = "none";
    instance_contract_source_generation = 0;
}

void GaussianSplatRenderer::publish_route_skip_stage_metrics(const String &p_route_uid,
        InstanceBackendPolicy p_backend_policy, const String &p_cull_route_reason,
        const String &p_cull_stage_reason, RenderFallbackReason p_fallback_reason) {
    get_debug_state().route_uid = p_route_uid;
    get_performance_state().metrics.cull_route_uid = p_route_uid;
    get_performance_state().metrics.cull_route_reason = p_cull_route_reason;
    get_performance_state().metrics.raster_path = "skipped";
    get_frame_state().visible_splat_count.store(0, std::memory_order_release);
    get_frame_state().render_time_ms = 0.0f;
    get_frame_state().sort_time_ms = 0.0f;
    get_sorting_state().sorted_splat_count = 0;

    const String cull_stage_reason = p_cull_stage_reason.is_empty()
            ? String("Cull skipped: route not ready")
            : p_cull_stage_reason;
    const String sort_stage_reason = p_cull_stage_reason.is_empty()
            ? String("Sort skipped: route not ready")
            : vformat("Sort skipped: %s", p_cull_stage_reason);
    StageMetrics metrics = RenderPipelineStages::make_route_skip_metrics(p_route_uid,
            instance_backend_policy_to_string(p_backend_policy), cull_stage_reason, sort_stage_reason,
            p_fallback_reason);
    if (debug_state_orchestrator) {
        debug_state_orchestrator->store_stage_metrics(metrics);
    } else {
        get_debug_state().last_stage_metrics = metrics;
        get_debug_state().last_stage_metrics_valid = true;
    }
}

bool GaussianSplatRenderer::update_instance_buffer(LocalVector<InstanceDataGPU> &p_instances, const PublishedInstanceAssetRemap &p_remap,
        const LocalVector<uint64_t> *p_submission_asset_ids) {
    if (!_ensure_rendering_device("update_instance_buffer")) {
        return false;
    }

    const uint32_t instance_count = p_instances.size();
    if (instance_count == 0) {
        instance_pipeline_buffers.instance_count = 0;
        return true;
    }

    if (!p_remap.valid) {
        WARN_PRINT_ONCE("[GaussianSplatRenderer] Instance buffer upload skipped; published asset remap unavailable.");
        instance_pipeline_buffers.instance_count = 0;
        return false;
    }

    // The GPU InstanceDataGPU::ids[0] field is only 32 bits, so it cannot carry the
    // full 64-bit host submission identity once ObjectIDs differ above the low 32 bits.
    // Prefer the parallel 64-bit submission-id array (collision-free key into the
    // published remap); fall back to the truncated ids[0] tag only when the caller has
    // no 64-bit identities to supply (e.g. legacy/test callers with small ids).
    const bool use_wide_keys = p_submission_asset_ids != nullptr &&
            p_submission_asset_ids->size() == instance_count;
    if (p_submission_asset_ids != nullptr && p_submission_asset_ids->size() != instance_count) {
        WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] submission_asset_ids size (%u) != instance count (%u); "
                                "falling back to 32-bit ids[0] keys.",
                p_submission_asset_ids->size(), instance_count));
    }

    const uint32_t published_generation = p_remap.generation == 0
            ? 1u
            : uint32_t(p_remap.generation & uint64_t(UINT32_MAX));
    for (uint32_t i = 0; i < instance_count; i++) {
        const uint64_t incoming_asset_id = use_wide_keys
                ? (*p_submission_asset_ids)[i]
                : uint64_t(p_instances[i].ids[0]);
        uint32_t dense_id = 0u;
        if (const uint32_t *mapped_dense_id = p_remap.asset_to_dense_id.getptr(incoming_asset_id)) {
            dense_id = *mapped_dense_id;
        } else {
            WARN_PRINT_ONCE(vformat("[GaussianSplatRenderer] Instance asset_id %s is not published; using primary resident asset.",
                    String::num_uint64(incoming_asset_id)));
        }
        p_instances[i].ids[0] = dense_id;
        p_instances[i].lod[1] = published_generation;
    }

    RenderingDevice *rd = get_device_state().rd;
    if (!rd) {
        WARN_PRINT_ONCE("[GaussianSplatRenderer] Instance buffer upload skipped; rendering device not available.");
        instance_pipeline_buffers.instance_count = 0;
        return false;
    }

    ResourceState &resource_state = get_resource_state();
    if (!resource_state.instance_buffer.is_valid() || resource_state.instance_buffer_capacity < instance_count) {
        const uint32_t new_capacity = next_power_of_2(MAX(instance_count, (uint32_t)1));
        const uint64_t buffer_size = static_cast<uint64_t>(new_capacity) * sizeof(InstanceDataGPU);
        if (resource_state.instance_buffer.is_valid()) {
            _free_owned_resource(rd, resource_state.instance_buffer);
        }
        resource_state.instance_buffer = rd->storage_buffer_create(buffer_size);
        if (!resource_state.instance_buffer.is_valid()) {
            resource_state.instance_buffer_capacity = 0;
            WARN_PRINT_ONCE("[GaussianSplatRenderer] Failed to allocate instance buffer.");
            instance_pipeline_buffers.instance_count = 0;
            return false;
        }
        rd->set_resource_name(resource_state.instance_buffer, "GS_InstanceBuffer");
        track_resource_owner(resource_state.instance_buffer, rd);
        resource_state.instance_buffer_capacity = new_capacity;
    }

    const uint64_t upload_size = static_cast<uint64_t>(instance_count) * sizeof(InstanceDataGPU);
    rd->buffer_update(resource_state.instance_buffer, 0, upload_size, p_instances.ptr());
    instance_pipeline_buffers.instance_buffer = resource_state.instance_buffer;
    instance_pipeline_buffers.instance_count = instance_count;

    return true;
}

bool GaussianSplatRenderer::update_instance_grading_buffer(const LocalVector<InstanceGradingGPU> &p_gradings) {
    // Sibling SSBO to instance_buffer. Must always resolve to a valid RID — the shader
    // binds it unconditionally at binding 20. When the caller passes zero rows (e.g.
    // transient empty world) we still allocate a 1-row buffer and write a neutral default
    // so the shader's indexed read is well-defined.
    if (!_ensure_rendering_device("update_instance_grading_buffer")) {
        return false;
    }

    RenderingDevice *rd = get_device_state().rd;
    if (!rd) {
        WARN_PRINT_ONCE("[GaussianSplatRenderer] Grading buffer upload skipped; rendering device not available.");
        return false;
    }

    ResourceState &resource_state = get_resource_state();

    const uint32_t row_count = MAX(p_gradings.size(), (uint32_t)1);
    if (!resource_state.instance_grading_buffer.is_valid() || resource_state.instance_grading_buffer_capacity < row_count) {
        const uint32_t new_capacity = next_power_of_2(row_count);
        const uint64_t buffer_size = static_cast<uint64_t>(new_capacity) * sizeof(InstanceGradingGPU);
        if (resource_state.instance_grading_buffer.is_valid()) {
            _free_owned_resource(rd, resource_state.instance_grading_buffer);
        }
        resource_state.instance_grading_buffer = rd->storage_buffer_create(buffer_size);
        if (!resource_state.instance_grading_buffer.is_valid()) {
            resource_state.instance_grading_buffer_capacity = 0;
            WARN_PRINT_ONCE("[GaussianSplatRenderer] Failed to allocate instance grading buffer.");
            return false;
        }
        rd->set_resource_name(resource_state.instance_grading_buffer, "GS_InstanceGradingBuffer");
        track_resource_owner(resource_state.instance_grading_buffer, rd);
        resource_state.instance_grading_buffer_capacity = new_capacity;
    }

    if (p_gradings.is_empty()) {
        // Write a single neutral row so shader indexing is always valid.
        InstanceGradingGPU neutral = {};
        neutral.primary[2] = 1.0f; // contrast = 1
        neutral.primary[3] = 1.0f; // saturation = 1
        rd->buffer_update(resource_state.instance_grading_buffer, 0, sizeof(InstanceGradingGPU), &neutral);
    } else {
        const uint64_t upload_size = static_cast<uint64_t>(p_gradings.size()) * sizeof(InstanceGradingGPU);
        rd->buffer_update(resource_state.instance_grading_buffer, 0, upload_size, p_gradings.ptr());
    }

    instance_pipeline_buffers.instance_grading_buffer = resource_state.instance_grading_buffer;
    return true;
}

GaussianSplatRenderer::DebugConfig &GaussianSplatRenderer::get_debug_config() {
    return debug_state_orchestrator->get_config();
}

const GaussianSplatRenderer::DebugConfig &GaussianSplatRenderer::get_debug_config() const {
    return debug_state_orchestrator->get_config();
}

GaussianSplatRenderer::DebugState &GaussianSplatRenderer::get_debug_state() {
    return debug_state_orchestrator->get_state();
}

const GaussianSplatRenderer::DebugState &GaussianSplatRenderer::get_debug_state() const {
    return debug_state_orchestrator->get_state();
}

#ifdef TESTS_ENABLED
void GaussianSplatRenderer::_test_dispatch_noop_callback(uint64_t p_request_id) {
    (void)p_request_id;
}

void GaussianSplatRenderer::test_force_disable_streaming() {
    StreamingState &streaming_state = get_streaming_state();
    streaming_state.use_streamed_data = false;
    streaming_state.streaming_gpu_splat_count = 0;
    streaming_state.streaming_gpu_total_capacity = 0;
    streaming_state.streamed_indices_generation = 0;
    streaming_state.streamed_indices_are_local = false;
    streaming_state.cached_streamed_indices_valid = false;
    get_sorting_state().sorted_splat_count = 0;
    if (streaming_state.current_streaming_system.is_valid()) {
        streaming_state.current_streaming_system->end_frame();
        streaming_state.current_streaming_system.unref();
    }
    streaming_state.cached_streamed_gaussians.clear();
    streaming_state.cached_streamed_indices.clear();
    streaming_state.cached_streamed_source_indices.clear();
    streaming_state.cached_streamed_sh_limits.clear();
    streaming_state.cached_streamed_index_lookup.clear();
    streaming_state.current_stream_gpu_buffer = RID();
    if (streaming_state.memory_stream.is_valid()) {
        streaming_state.memory_stream->shutdown();
    }
}

void GaussianSplatRenderer::test_release_current_streaming_system() {
    StreamingState &streaming_state = get_streaming_state();
    if (streaming_state.current_streaming_system.is_valid()) {
        streaming_state.current_streaming_system->end_frame();
        streaming_state.current_streaming_system.unref();
    }
}

bool GaussianSplatRenderer::test_has_current_streaming_system() const {
    return get_streaming_state().current_streaming_system.is_valid();
}

bool GaussianSplatRenderer::test_has_output_compositor() const {
    return get_subsystem_state().output_compositor.is_valid();
}

bool GaussianSplatRenderer::test_shadow_pass_guard_restores_after_scope() {
    ViewState &view_state = get_view_state();
    const Size2i original_manual_viewport = view_state.manual_viewport_override;
    const RD::DataFormat original_manual_format = view_state.manual_viewport_format_override;
    const Transform3D original_camera_transform = view_state.last_camera_to_world_transform;
    const Projection original_camera_projection = view_state.last_camera_projection;
    const bool original_using_scene_data = view_state.using_scene_data;
    const Ref<OutputCompositor> original_output_compositor = subsystem_state.output_compositor;
    const bool original_shadow_filter = shadow_instance_filter_enabled;

    Ref<OutputCompositor> main_output_compositor;
    main_output_compositor.instantiate();
    Ref<OutputCompositor> shadow_compositor;
    shadow_compositor.instantiate();
    ERR_FAIL_COND_V(!main_output_compositor.is_valid() || !shadow_compositor.is_valid(), false);

    const Size2i saved_manual_viewport(321, 123);
    const RD::DataFormat saved_manual_format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
    const Transform3D saved_camera_transform(Basis(), Vector3(3.0, 4.0, 5.0));
    Projection saved_camera_projection;
    saved_camera_projection.set_perspective(60.0, 1.5, 0.25, 250.0);

    ShadowPassDescriptor descriptor;
    descriptor.light_transform = Transform3D(Basis(), Vector3(-4.0, 2.0, 9.0));
    descriptor.light_projection.set_perspective(45.0, 1.0, 0.1, 100.0);
    descriptor.atlas_rect = Rect2i(8, 16, 64, 32);

    view_state.manual_viewport_override = saved_manual_viewport;
    view_state.manual_viewport_format_override = saved_manual_format;
    view_state.last_camera_to_world_transform = saved_camera_transform;
    view_state.last_camera_projection = saved_camera_projection;
    view_state.using_scene_data = true;
    subsystem_state.output_compositor = main_output_compositor;
    shadow_instance_filter_enabled = false;

    bool guard_applied_expected_state = false;
    {
        ScopedShadowPassState guard(*this, descriptor, shadow_compositor);
        guard_applied_expected_state =
                view_state.manual_viewport_override == descriptor.atlas_rect.size &&
                view_state.manual_viewport_format_override == descriptor.viewport_format &&
                view_state.last_camera_to_world_transform == descriptor.light_transform &&
                _projection_nearly_equal(view_state.last_camera_projection, descriptor.light_projection) &&
                !view_state.using_scene_data &&
                subsystem_state.output_compositor == shadow_compositor &&
                shadow_instance_filter_enabled;
    }

    const bool restored_expected_state =
            view_state.manual_viewport_override == saved_manual_viewport &&
            view_state.manual_viewport_format_override == saved_manual_format &&
            view_state.last_camera_to_world_transform == saved_camera_transform &&
            _projection_nearly_equal(view_state.last_camera_projection, saved_camera_projection) &&
            view_state.using_scene_data &&
            subsystem_state.output_compositor == main_output_compositor &&
            !shadow_instance_filter_enabled;

    view_state.manual_viewport_override = original_manual_viewport;
    view_state.manual_viewport_format_override = original_manual_format;
    view_state.last_camera_to_world_transform = original_camera_transform;
    view_state.last_camera_projection = original_camera_projection;
    view_state.using_scene_data = original_using_scene_data;
    subsystem_state.output_compositor = original_output_compositor;
    shadow_instance_filter_enabled = original_shadow_filter;

    return guard_applied_expected_state && restored_expected_state;
}

RID GaussianSplatRenderer::test_get_cached_render_depth() const {
    const Ref<OutputCompositor> output_compositor = get_subsystem_state().output_compositor;
    return output_compositor.is_valid() ? output_compositor->get_cached_render_depth() : RID();
}

uint32_t GaussianSplatRenderer::test_get_output_blit_variant_count() const {
    const Ref<OutputCompositor> output_compositor = get_subsystem_state().output_compositor;
    return output_compositor.is_valid() ? output_compositor->get_blit_variant_count() : 0;
}

void GaussianSplatRenderer::test_clear_output_viewport_blit_resources() {
    const Ref<OutputCompositor> output_compositor = get_subsystem_state().output_compositor;
    if (output_compositor.is_valid()) {
        output_compositor->clear_viewport_blit_resources();
    }
}

void GaussianSplatRenderer::test_reset_output_viewport_copy_state() {
    const Ref<OutputCompositor> output_compositor = get_subsystem_state().output_compositor;
    if (output_compositor.is_valid()) {
        output_compositor->test_reset_last_viewport_copy_state();
    }
}

void GaussianSplatRenderer::test_integrate_final_output(RenderDataRD *p_render_data, RenderSceneBuffersRD *p_render_buffers,
        const RID &p_final_output, RID &r_render_target, const Size2i &p_viewport_size,
        bool p_defer_commit, bool p_painterly_active, const RID &p_cached_depth) {
    const Ref<OutputCompositor> output_compositor = get_subsystem_state().output_compositor;
    if (output_compositor.is_valid()) {
        output_compositor->integrate_final_output(this, p_render_data, p_render_buffers, p_final_output,
                r_render_target, p_viewport_size, p_defer_commit, p_painterly_active, p_cached_depth);
    }
}

void GaussianSplatRenderer::test_set_test_splats(const Vector<Vector3> &p_positions, const Vector<Vector3> &p_scales) {
    const int count = p_positions.size();
    get_test_data_state().positions.resize(count);
    get_test_data_state().colors.resize(count);
    get_test_data_state().scales.resize(count);

    const int scale_count = p_scales.size();
    for (int i = 0; i < count; i++) {
        get_test_data_state().positions[i] = p_positions[i];
        get_test_data_state().colors[i] = Color(1.0f, 1.0f, 1.0f, 1.0f);
        if (i < scale_count) {
            get_test_data_state().scales[i] = p_scales[i];
        } else {
            get_test_data_state().scales[i] = Vector3(1.0f, 1.0f, 1.0f);
        }
    }

    get_test_data_state().content_generation++;
}

int GaussianSplatRenderer::test_cull_visible_count(const Transform3D &p_world_to_camera_transform,
        const Projection &p_projection, const Size2i &p_viewport_size) {
    get_scene_state().gaussian_data.unref();
    get_subsystem_state().gpu_culler->get_config().last_cull_viewport_size = p_viewport_size;
    Transform3D view_transform = p_world_to_camera_transform.affine_inverse();
    CullStageOutput output = _cull_for_view(view_transform, p_projection, p_viewport_size);
    return static_cast<int>(output.visible_count);
}

GaussianSplatRenderer::SortStageSummary GaussianSplatRenderer::test_sort_for_view(
        const Transform3D &p_world_to_camera_transform, IndexDomain p_input_domain) {
    return sort_gaussians_for_view(p_world_to_camera_transform, p_input_domain);
}

void GaussianSplatRenderer::test_disable_gpu_culler() {
    subsystem_state.gpu_culler.unref();
}

void GaussianSplatRenderer::test_disable_rasterizer() {
    if (tile_renderer_state.renderer.is_valid()) {
        tile_renderer_state.renderer->cleanup();
    }
    tile_renderer_state.renderer.unref();
    tile_renderer_state.init_failed = true;
    subsystem_state.rasterizer.unref();
}

void GaussianSplatRenderer::test_set_render_thread_dispatch_timeout_usec(uint64_t p_timeout_usec) {
    if (render_thread_dispatcher) {
        render_thread_dispatcher->set_timeout_usec(p_timeout_usec);
    }
}

uint64_t GaussianSplatRenderer::test_get_render_thread_dispatch_timeout_usec() const {
    return render_thread_dispatcher ? render_thread_dispatcher->get_timeout_usec() : 0;
}

bool GaussianSplatRenderer::test_dispatch_call_on_render_thread_blocking_without_completion() {
    return _dispatch_call_on_render_thread_blocking(callable_mp(this, &GaussianSplatRenderer::_test_dispatch_noop_callback));
}

bool GaussianSplatRenderer::test_dispatch_call_on_render_thread_blocking_with_completion() {
    return _dispatch_call_on_render_thread_blocking(callable_mp(this, &GaussianSplatRenderer::_notify_render_thread_dispatch_completed));
}

bool GaussianSplatRenderer::test_is_render_thread_dispatch_path_active() const {
    return render_thread_dispatcher && render_thread_dispatcher->is_render_thread_dispatch_path_active();
}

void GaussianSplatRenderer::test_notify_render_thread_dispatch_completed(uint64_t p_request_id) {
    _notify_render_thread_dispatch_completed(p_request_id);
}

uint64_t GaussianSplatRenderer::test_get_render_thread_dispatch_completed_request_id() const {
    return render_thread_dispatcher ? render_thread_dispatcher->get_completed_request_id() : 0;
}
#endif
