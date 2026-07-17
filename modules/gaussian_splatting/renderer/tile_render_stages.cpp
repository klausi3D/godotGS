/**
 * tile_render_stages.cpp — TileRenderer::TileRenderParamsBuilder implementations.
 *
 * Companion .cpp for tile_renderer.h / tile_render_stages.h.
 * Contains the GPU parameter builder (build_params, _resolve_param_view).
 *
 * Stage method implementations have been split into dedicated companion files:
 *   tile_render_binning.cpp       — TileBinningStage
 *   tile_render_prefix_scan.cpp   — TilePrefixScanStage
 *   tile_render_rasterizer_stage.cpp — TileRasterizerStage
 *   tile_render_resolve.cpp       — TileResolveStage
 *   tile_render_debug_stats.cpp   — TileRendererDebugStats
 */

#include "tile_renderer.h"
#include "gaussian_gpu_layout.h"
#include "../interfaces/output_compositor_interfaces.h"
#include "tile_lighting_abi.h"
#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/rendering_device.h"

#include "quantization_config.h"
#include "sh_config.h"

namespace {

static float _sanitize_effector_float(float p_value, float p_fallback) {
	return Math::is_finite(p_value) ? p_value : p_fallback;
}

static bool _is_valid_effector_center(const Vector3 &p_center) {
	return Math::is_finite(p_center.x) && Math::is_finite(p_center.y) && Math::is_finite(p_center.z);
}

static void _clear_effector_slot(float (&r_sphere)[4], float (&r_config)[4], float (&r_opacity)[4]) {
	r_sphere[0] = 0.0f;
	r_sphere[1] = 0.0f;
	r_sphere[2] = 0.0f;
	r_sphere[3] = 0.0f;
	r_config[0] = 0.0f;
	r_config[1] = 0.0f;
	r_config[2] = 2.0f;
	r_config[3] = 2.0f;
	r_opacity[0] = 1.0f;
	r_opacity[1] = 0.0f;
	r_opacity[2] = 1.0f;
	r_opacity[3] = 0.0f;
}

static bool _pack_effector_slot(const GaussianSplatting::SphereEffectorRenderData &p_source,
		float (&r_sphere)[4], float (&r_config)[4], float (&r_opacity)[4]) {
	if (!p_source.enabled) {
		return false;
	}
	if (!_is_valid_effector_center(p_source.center)) {
		WARN_PRINT_ONCE("[TileRenderer] Ignoring sphere effector with non-finite center.");
		return false;
	}
	const float radius = MAX(0.0f, _sanitize_effector_float(p_source.radius, 0.0f));
	if (radius <= 0.0f) {
		return false;
	}

	r_sphere[0] = p_source.center.x;
	r_sphere[1] = p_source.center.y;
	r_sphere[2] = p_source.center.z;
	r_sphere[3] = radius;
	r_config[0] = 1.0f;
	r_config[1] = _sanitize_effector_float(p_source.strength, 0.0f);
	r_config[2] = MAX(0.001f, _sanitize_effector_float(p_source.falloff, 2.0f));
	r_config[3] = MAX(0.1f, _sanitize_effector_float(p_source.frequency, 2.0f));
	r_opacity[0] = p_source.affect_position ? 1.0f : 0.0f;
	r_opacity[1] = p_source.affect_opacity ? 1.0f : 0.0f;
	r_opacity[2] = CLAMP(_sanitize_effector_float(p_source.opacity_strength, 1.0f), 0.0f, 1.0f);
	r_opacity[3] = CLAMP(_sanitize_effector_float(p_source.target_opacity, 0.0f), 0.0f, 1.0f);
	return true;
}

static void _build_effector_payload(const TileRenderer::RenderParams &p_params, float (&r_meta)[4],
		float (&r_spheres)[GS_MAX_SPHERE_EFFECTORS][4], float (&r_configs)[GS_MAX_SPHERE_EFFECTORS][4],
		float (&r_opacity_configs)[GS_MAX_SPHERE_EFFECTORS][4]) {
	for (uint32_t i = 0; i < GS_MAX_SPHERE_EFFECTORS; i++) {
		_clear_effector_slot(r_spheres[i], r_configs[i], r_opacity_configs[i]);
	}

	const uint32_t bounded_limit = MIN(p_params.sphere_effector_max_active, GS_MAX_SPHERE_EFFECTORS);
	const bool has_explicit_scene_bindings = p_params.sphere_effector_count > 0u;
	uint32_t requested_count = has_explicit_scene_bindings ? p_params.sphere_effector_count : 0u;
	uint32_t active_count = 0u;
	uint32_t slot_iteration_count = 0u;

	if (p_params.sphere_effector_max_active > GS_MAX_SPHERE_EFFECTORS) {
		WARN_PRINT_ONCE(vformat("[TileRenderer] Requested %d sphere effectors, but this runtime supports at most %d per pass.",
				p_params.sphere_effector_max_active, GS_MAX_SPHERE_EFFECTORS));
	}

	if (has_explicit_scene_bindings) {
		if (requested_count > bounded_limit) {
			WARN_PRINT_ONCE(vformat("[TileRenderer] Truncating sphere effector list from %d to %d active entries for this pass.",
					requested_count, bounded_limit));
		}
		const uint32_t scan_count = MIN(requested_count, bounded_limit);
		slot_iteration_count = scan_count;
		// Pack into slot `i` directly (no compaction) so per-instance masks
		// encoded earlier against the source payload indices remain valid.
		// Invalid slots stay fully zeroed by _clear_effector_slot above —
		// `config[0] == 0` means "disabled" to the shader, so mask bits that
		// point at a skipped slot are inert.
		for (uint32_t i = 0; i < scan_count; i++) {
			if (_pack_effector_slot(p_params.sphere_effectors[i], r_spheres[i], r_configs[i], r_opacity_configs[i])) {
				active_count++;
			}
		}
	} else if (bounded_limit > 0u) {
		GaussianSplatting::SphereEffectorRenderData legacy_effector;
		legacy_effector.enabled = p_params.sphere_effector_enabled;
		legacy_effector.center = p_params.sphere_effector_center;
		legacy_effector.radius = p_params.sphere_effector_radius;
		legacy_effector.strength = p_params.sphere_effector_strength;
		legacy_effector.falloff = p_params.sphere_effector_falloff;
		legacy_effector.frequency = p_params.sphere_effector_frequency;
		legacy_effector.affect_position = p_params.sphere_effector_affect_position;
		legacy_effector.affect_opacity = p_params.sphere_effector_affect_opacity;
		legacy_effector.opacity_strength = p_params.sphere_effector_opacity_strength;
		legacy_effector.target_opacity = p_params.sphere_effector_target_opacity;
		requested_count = legacy_effector.enabled ? 1u : 0u;
		if (_pack_effector_slot(legacy_effector, r_spheres[0], r_configs[0], r_opacity_configs[0])) {
			active_count = 1u;
			slot_iteration_count = 1u;
		}
	}

	// effector_meta[0] is the shader's loop bound over effector slots. With
	// stable indexing (no compaction), this must cover every slot that a
	// per-instance mask bit could point at — not just the packed-valid count
	// — so mask bits for a disabled slot fall through the `config.x <= 0.5`
	// check rather than reading past the iteration bound.
	r_meta[0] = float(slot_iteration_count);
	r_meta[1] = float(GS_MAX_SPHERE_EFFECTORS);
	r_meta[2] = float(requested_count);
	r_meta[3] = has_explicit_scene_bindings ? 1.0f : 0.0f;
	(void)active_count;
}

} // namespace

Vector2i TileRenderer::TileRenderParamsBuilder::_resolve_param_view(const RenderParams &p_params,
		RenderingDevice *p_resource_device, const RID &p_viewport_texture) const {
	Vector2i param_view = p_params.viewport_size;
	if (param_view.x <= 0 || param_view.y <= 0) {
		RenderingDevice *format_device = owner.render_targets.output_texture_owner.device
				? owner.render_targets.output_texture_owner.device
				: p_resource_device;
		RD::TextureFormat format = RD::TextureFormat();
		if (format_device && p_viewport_texture.is_valid() && format_device->texture_is_valid(p_viewport_texture)) {
			format = format_device->texture_get_format(p_viewport_texture);
		}
		param_view = Vector2i(int(format.width), int(format.height));
	}
	if (param_view.x <= 0 || param_view.y <= 0) {
		param_view = owner.grid_state.viewport_size;
	}
	return param_view;
}

TileRenderParamsGPU TileRenderer::TileRenderParamsBuilder::build_params(const RenderParams &p_params,
		uint32_t p_overlap_record_count, uint32_t p_resolved_total_gaussians, RenderingDevice *p_resource_device,
		float p_overlap_keep_ratio) {
	TileRenderParamsGPU params = {};

	Transform3D view_matrix = p_params.world_to_camera_transform;
	// CRITICAL FIX: Always derive camera_transform from view_matrix inverse.
	// The view_matrix includes the instance transform (world_to_camera = instance_transform * camera_view),
	// so using its inverse ensures the camera position is in local space for correct SH view direction.
	Transform3D camera_transform = view_matrix.affine_inverse();

	for (int column = 0; column < 3; column++) {
		Vector3 column_vec = view_matrix.basis.get_column(column);
		params.view_matrix[column * 4 + 0] = column_vec.x;
		params.view_matrix[column * 4 + 1] = column_vec.y;
		params.view_matrix[column * 4 + 2] = column_vec.z;
		params.view_matrix[column * 4 + 3] = 0.0f;
	}
	params.view_matrix[12] = view_matrix.origin.x;
	params.view_matrix[13] = view_matrix.origin.y;
	params.view_matrix[14] = view_matrix.origin.z;
	params.view_matrix[15] = 1.0f;

	// Inverse view matrix (camera-to-local for instanced views).
	for (int column = 0; column < 3; column++) {
		Vector3 column_vec = camera_transform.basis.get_column(column);
		params.inv_view_matrix[column * 4 + 0] = column_vec.x;
		params.inv_view_matrix[column * 4 + 1] = column_vec.y;
		params.inv_view_matrix[column * 4 + 2] = column_vec.z;
		params.inv_view_matrix[column * 4 + 3] = 0.0f;
	}
	params.inv_view_matrix[12] = camera_transform.origin.x;
	params.inv_view_matrix[13] = camera_transform.origin.y;
	params.inv_view_matrix[14] = camera_transform.origin.z;
	params.inv_view_matrix[15] = 1.0f;

	// Check if columns are orthonormal (magnitude should be ~1.0)
	float col0_mag = Math::sqrt(params.view_matrix[0] * params.view_matrix[0] +
			params.view_matrix[1] * params.view_matrix[1] +
			params.view_matrix[2] * params.view_matrix[2]);
	float col1_mag = Math::sqrt(params.view_matrix[4] * params.view_matrix[4] +
			params.view_matrix[5] * params.view_matrix[5] +
			params.view_matrix[6] * params.view_matrix[6]);
	float col2_mag = Math::sqrt(params.view_matrix[8] * params.view_matrix[8] +
			params.view_matrix[9] * params.view_matrix[9] +
			params.view_matrix[10] * params.view_matrix[10]);

	bool matrix_invalid = (Math::abs(col0_mag - 1.0f) > 0.01f) ||
			(Math::abs(col1_mag - 1.0f) > 0.01f) ||
			(Math::abs(col2_mag - 1.0f) > 0.01f);

	// PROD-1 (#626): Only log when matrix is actually invalid (error condition)
	// Removed periodic "everything is fine" logging that cluttered production output
#ifdef DEBUG_ENABLED
	if (matrix_invalid) {
		WARN_PRINT_ONCE(vformat("[VIEW-DEBUG] Invalid view matrix: col_magnitudes=(%.4f, %.4f, %.4f) - should all be 1.0",
				col0_mag, col1_mag, col2_mag));
	}
#endif

	// Extract camera world position for SH evaluation
	Vector3 cam_pos = camera_transform.origin;
	params.camera_position[0] = cam_pos.x;
	params.camera_position[1] = cam_pos.y;
	params.camera_position[2] = cam_pos.z;
	params.camera_position[3] = 0.0f; // std140 padding
	params.alpha_floor = p_params.alpha_floor;
	params.force_solid_coverage = p_params.force_solid_coverage ? 1u : 0u;
	params.overlap_record_count = p_overlap_record_count;
	params._pad_overlap = 0u;
	params.cull_far_tolerance = p_params.cull_far_tolerance;
	params.tiny_splat_screen_radius = p_params.tiny_splat_screen_radius;
	params.max_conic_aspect = p_params.max_conic_aspect;
	params.low_pass_filter = CLAMP(p_params.low_pass_filter, 0.05f, 2.0f);

	// Jacobian diagnostic toggles
	params.jacobian_diag_flags[0] = p_params.jacobian_bypass_radius_depth_floor ? 1.0f : 0.0f;
	params.jacobian_diag_flags[1] = p_params.jacobian_bypass_j_col2_clamp ? 1.0f : 0.0f;
	params.jacobian_diag_flags[2] = p_params.jacobian_invert_j_col2_sign ? 1.0f : 0.0f;
	params.jacobian_diag_flags[3] = 0.0f; // reserved

	const Projection &projection_gpu = p_params.render_projection;
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			params.projection_matrix[column * 4 + row] = projection_gpu.columns[column][row];
		}
	}
	const Projection inv_projection_gpu = projection_gpu.inverse();
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			params.inv_projection_matrix[column * 4 + row] = inv_projection_gpu.columns[column][row];
		}
	}

	// Get viewport size for shader params - use p_params.viewport_size if valid,
	// otherwise fall back to texture format or internal grid_state.viewport_size.
	RID viewport_texture = owner.get_output_texture();
	Vector2i param_view = _resolve_param_view(p_params, p_resource_device, viewport_texture);
	params.viewport_size[0] = float(MAX(param_view.x, 1));
	params.viewport_size[1] = float(MAX(param_view.y, 1));

	params.tile_count[0] = float(owner.grid_state.tiles_x);
	params.tile_count[1] = float(owner.grid_state.tiles_y);

	params.total_gaussians = p_resolved_total_gaussians;
	params.visible_gaussians = p_params.splat_count;

	// Use Godot's projection helpers; clip tolerance is handled in shader via cull_far_tolerance.
	params.near_plane = p_params.projection.get_z_near();
	params.far_plane = p_params.projection.get_z_far();
	bool near_far_fallback_used = false;
	if (params.near_plane < 0.0f || params.far_plane <= params.near_plane) {
		WARN_PRINT_ONCE(vformat("[TileRenderer] Invalid near/far extracted: near=%f, far=%f. Using fallback values.",
				params.near_plane, params.far_plane));
		params.near_plane = 0.1f;
		params.far_plane = 1000.0f;
		near_far_fallback_used = true;
	}

	// Diagnostic logging removed; use debug overlays instead.

	// Set debug overlay parameters
	params.debug_flags[0] = (p_params.debug_show_tile_bounds || p_params.debug_show_tile_grid) ? 1.0f : 0.0f;
	// Enable rasterizer stats collection for HUD display even without visual splat coverage overlay.
	// This allows the runtime overlay to show rasterizer stats (samples, iterations, contributions)
	// when only the performance HUD is enabled. The perf-capture harness can also
	// flip stats on without forcing visual overlays via perf_capture_raster_shader_counters.
	bool collect_raster_stats = p_params.debug_show_splat_coverage ||
			p_params.debug_show_performance_hud ||
			p_params.perf_capture_raster_shader_counters;
	params.debug_flags[1] = collect_raster_stats ? 1.0f : 0.0f;
	params.debug_flags[2] = p_params.debug_show_overflow_tiles ? 1.0f : 0.0f;
	params.debug_flags[3] = p_params.debug_show_projection_issues ? 1.0f : 0.0f;
	// debug_overlay_flags[0]: 0=off, 1=density heatmap, 2=shadow opacity debug
	params.debug_overlay_flags[0] = p_params.debug_show_shadow_opacity ? 2.0f : (p_params.debug_show_density_heatmap ? 1.0f : 0.0f);
	params.debug_overlay_flags[1] = p_params.debug_show_depth_visualization ? 1.0f : 0.0f;
	params.debug_overlay_flags[2] = p_params.debug_show_projection_issues ? 1.0f : 0.0f;
	params.debug_overlay_flags[3] = p_params.debug_show_white_albedo ? 1.0f : 0.0f;
	// Set SH band configuration from global config
	params.sh_config[0] = static_cast<float>(g_sh_config.sh_bands);
	uint32_t sh_divisor = owner.render_settings.enable_sh_amortization
			? owner.render_settings.sh_amortization_divisor
			: 1u;
	if (sh_divisor < 1u) {
		sh_divisor = 1u;
	}
	uint32_t sh_phase = (sh_divisor > 1u) ? (owner.frame_state.current_frame_serial % sh_divisor) : 0u;
	params.sh_config[1] = static_cast<float>(sh_divisor);
	params.sh_config[2] = static_cast<float>(sh_phase);
	params.sh_config[3] = owner.sh_cache_needs_full_update ? 1.0f : 0.0f;
	// SH decode configuration is reserved; decode now comes from per-gaussian metadata.
	params.sh_decode_config[0] = 0.0f;
	params.sh_decode_config[1] = 0.0f;
	params.sh_decode_config[2] = 0.0f;
	params.sh_decode_config[3] = 0.0f;
	// Opacity-aware culling configuration (FlashGS optimization)
	// When enabled, reduces tile-Gaussian pairs by ~94% using opacity-based radius calculation
	params.opacity_culling_config[0] = p_params.opacity_aware_culling ? 1.0f : 0.0f;
	params.opacity_culling_config[1] = p_params.visibility_threshold;
	// Optional per-splat hard cull at projection. Clamp to
	// [0, 0.99] so callers cannot accidentally cull every splat (1.0 would
	// reject all opacities since the test is opacity <= alpha_clip).
	params.opacity_culling_config[2] = CLAMP(p_params.alpha_clip, 0.0f, 0.99f);
	params.opacity_culling_config[3] = 0.0f; // reserved
	// LOD blending configuration (LODGE technique)
	params.lod_blend_config[0] = p_params.lod_blend_factor;
	params.lod_blend_config[1] = p_params.lod_blend_enabled ? 1.0f : 0.0f;
	params.lod_blend_config[2] = p_params.lod_blend_distance;
	params.lod_blend_config[3] = 0.0f; // reserved
	// Distance-based culling configuration
	params.distance_cull_config[0] = p_params.distance_cull_start;
	params.distance_cull_config[1] = p_params.distance_cull_max_rate;
	params.distance_cull_config[2] = p_params.distance_cull_enabled ? 1.0f : 0.0f;
	// Reuse .w as overlap keep ratio for overflow thinning (1.0 = keep all records).
	params.distance_cull_config[3] = CLAMP(p_overlap_keep_ratio, 0.0f, 1.0f);
	// Color grading configuration
	if (p_params.color_grading.is_valid() && p_params.color_grading->get_enabled()) {
		params.color_grading_primary[0] = 1.0f; // enabled = true
		params.color_grading_primary[1] = p_params.color_grading->get_exposure();
		params.color_grading_primary[2] = p_params.color_grading->get_contrast();
		params.color_grading_primary[3] = p_params.color_grading->get_saturation();
		params.color_grading_secondary[0] = p_params.color_grading->get_temperature();
		params.color_grading_secondary[1] = p_params.color_grading->get_tint();
		params.color_grading_secondary[2] = p_params.color_grading->get_hue_shift();
		params.color_grading_secondary[3] = 0.0f; // reserved
	} else {
		// Disabled or no color grading resource assigned
		params.color_grading_primary[0] = 0.0f; // enabled = false
		params.color_grading_primary[1] = 0.0f; // exposure = 0
		params.color_grading_primary[2] = 1.0f; // contrast = 1
		params.color_grading_primary[3] = 1.0f; // saturation = 1
		params.color_grading_secondary[0] = 0.0f; // temperature = 0
		params.color_grading_secondary[1] = 0.0f; // tint = 0
		params.color_grading_secondary[2] = 0.0f; // hue_shift = 0
		params.color_grading_secondary[3] = 0.0f; // reserved
	}
	params.lighting_config[0] = p_params.direct_light_scale;
	params.lighting_config[1] = p_params.indirect_sh_scale;
	params.lighting_config[2] = p_params.enable_direct_lighting ? 1.0f : 0.0f;
	params.lighting_config[3] = static_cast<float>(p_params.normal_mode);
	params.shadow_strength[0] = p_params.shadow_strength;
	params.shadow_strength[1] = 0.0f;
	params.shadow_strength[2] = 0.0f;
	params.shadow_strength[3] = 0.0f;
	params.shadow_bias_config[0] = p_params.shadow_receiver_bias_scale;
	params.shadow_bias_config[1] = p_params.shadow_receiver_bias_min;
	params.shadow_bias_config[2] = p_params.shadow_receiver_bias_max;
	params.shadow_bias_config[3] = 0.0f;
	params.lighting_mode[0] = static_cast<uint32_t>(p_params.direct_lighting_mode);
	params.lighting_mode[1] = 0u;
	params.lighting_mode[2] = 0u;
	params.lighting_mode[3] = 0u;

	const GaussianSplatting::TileLightingClusterABIConfig cluster_config =
			GaussianSplatting::TileLightingSetABI::compute_cluster_config(p_params.viewport_size,
					p_params.cluster_size, p_params.cluster_max_elements, p_params.cluster_buffer.is_valid());

	params.light_counts[0] = p_params.omni_light_count;
	params.light_counts[1] = p_params.spot_light_count;
	params.light_counts[2] = cluster_config.enabled ? 1u : 0u;
	params.light_counts[3] = p_params.light_mask;
	params.cluster_config[0] = cluster_config.cluster_shift;
	params.cluster_config[1] = cluster_config.cluster_width;
	params.cluster_config[2] = cluster_config.cluster_type_size;
	params.cluster_config[3] = cluster_config.max_cluster_element_count_div_32;
	params.debug_overlay_opacity = p_params.debug_overlay_opacity;
	params.opacity_multiplier = p_params.opacity_multiplier;
	// camera_position already populated above
	params.alpha_floor = p_params.alpha_floor;
	params.force_solid_coverage = p_params.force_solid_coverage ? 1u : 0u;
	params.overlap_record_count = p_overlap_record_count;
	params._pad_overlap = 0u;
	owner.diagnostics.capture_tile_density_snapshot = p_params.debug_show_tile_grid || p_params.debug_show_density_heatmap;

	// Set instance rotation inverse for SH view direction correction.
	// When the node has a rotation transform, SH coefficients (stored in original capture
	// coordinates) expect view directions in the same frame. This matrix transforms view
	// directions from the current local space back to the original coordinate frame.
	if (p_params.instance_rotation_valid) {
		// Store rotation inverse as column-major mat3 in 3 vec4s (std140)
		const Basis &rot_inv = p_params.instance_rotation_inverse;
		// Column 0
		params.instance_rotation_inv_col0[0] = rot_inv[0][0]; // Row 0, Col 0
		params.instance_rotation_inv_col0[1] = rot_inv[1][0]; // Row 1, Col 0
		params.instance_rotation_inv_col0[2] = rot_inv[2][0]; // Row 2, Col 0
		params.instance_rotation_inv_col0[3] = 0.0f;
		// Column 1
		params.instance_rotation_inv_col1[0] = rot_inv[0][1]; // Row 0, Col 1
		params.instance_rotation_inv_col1[1] = rot_inv[1][1]; // Row 1, Col 1
		params.instance_rotation_inv_col1[2] = rot_inv[2][1]; // Row 2, Col 1
		params.instance_rotation_inv_col1[3] = 0.0f;
		// Column 2
		params.instance_rotation_inv_col2[0] = rot_inv[0][2]; // Row 0, Col 2
		params.instance_rotation_inv_col2[1] = rot_inv[1][2]; // Row 1, Col 2
		params.instance_rotation_inv_col2[2] = rot_inv[2][2]; // Row 2, Col 2
		params.instance_rotation_inv_col2[3] = 0.0f;
	} else {
		// Identity matrix (no transformation needed)
		params.instance_rotation_inv_col0[0] = 1.0f;
		params.instance_rotation_inv_col0[1] = 0.0f;
		params.instance_rotation_inv_col0[2] = 0.0f;
		params.instance_rotation_inv_col0[3] = 0.0f;
		params.instance_rotation_inv_col1[0] = 0.0f;
		params.instance_rotation_inv_col1[1] = 1.0f;
		params.instance_rotation_inv_col1[2] = 0.0f;
		params.instance_rotation_inv_col1[3] = 0.0f;
		params.instance_rotation_inv_col2[0] = 0.0f;
		params.instance_rotation_inv_col2[1] = 0.0f;
		params.instance_rotation_inv_col2[2] = 1.0f;
		params.instance_rotation_inv_col2[3] = 0.0f;
	}

	params.wind_dir_strength[0] = p_params.wind_direction.x;
	params.wind_dir_strength[1] = p_params.wind_direction.y;
	params.wind_dir_strength[2] = p_params.wind_direction.z;
	params.wind_dir_strength[3] = MAX(0.0f, p_params.wind_strength);
	params.wind_time_config[0] = p_params.wind_time_seconds;
	params.wind_time_config[1] = MAX(0.0f, p_params.wind_frequency);
	params.wind_time_config[2] = p_params.wind_spatial_frequency;
	params.wind_time_config[3] = p_params.wind_enabled ? 1.0f : 0.0f;
	_build_effector_payload(p_params, params.effector_meta, params.effector_spheres, params.effector_configs,
			params.effector_opacity_configs);
	params.hotspot_cull_config[0] = float(p_params.hotspot_pressure_threshold);
	params.hotspot_cull_config[1] = MAX(0.0f, p_params.hotspot_min_radius_px);
	params.hotspot_cull_config[2] = 0.0f;
	params.hotspot_cull_config[3] = 0.0f;

	// Per-splat scene-depth clip (compositing slice D). eps_norm converts the shared
	// view-space epsilon contract into the payload's normalized linear-depth space using
	// the SAME near/far pair (and the same 1e-4 denominator guard) that defined
	// linear_depth at binning time. If the near/far FALLBACK above fired, that pair no
	// longer matches the scene camera's — the normalized comparison would misplace the
	// mesh depth (e.g. sky at scene-far 500 lands mid-range against fallback far 1000),
	// so the clip is disabled for such frames rather than clipping wrongly.
	bool scene_clip_enabled = p_params.scene_depth_clip_enabled;
	if (scene_clip_enabled && near_far_fallback_used) {
		WARN_PRINT_ONCE("[TileRenderer] Per-splat scene-depth clip disabled this session's fallback-projection frames: GS near/far fallback diverges from the scene camera pair.");
		scene_clip_enabled = false;
	}
	params.scene_depth_clip_config[0] = scene_clip_enabled ? 1.0f : 0.0f;
	params.scene_depth_clip_config[1] = GS_COMPOSITE_DEPTH_EPSILON_VIEW / MAX(params.far_plane - params.near_plane, 1e-4f);
	params.scene_depth_clip_config[2] = p_params.scene_depth_linearize_mul;
	params.scene_depth_clip_config[3] = p_params.scene_depth_linearize_add;
	params.scene_depth_clip_config2[0] = p_params.scene_depth_z_near;
	params.scene_depth_clip_config2[1] = p_params.scene_depth_z_far;
	params.scene_depth_clip_config2[2] = p_params.scene_depth_is_ortho ? 1.0f : 0.0f;
	params.scene_depth_clip_config2[3] = 0.0f;

	return params;
}
