#ifndef GS_DEFORMATION_FRAME_H
#define GS_DEFORMATION_FRAME_H

#include "../core/gaussian_splat_scene_director.h"
#include "tile_render_types.h"

namespace gs_wind {
// Same director slot ordering and project fallback as the main colour/depth producers.
inline void fill_frame_params(const GaussianSplatRenderer *p_renderer, double p_animation_time,
		GaussianSplatting::TileRenderParams &r_params) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	GaussianSplatting::apply_wind_to_render_params(r_params, gs::settings::get_wind_settings(ps), p_animation_time);
	LocalVector<GaussianSplatSceneDirector::SphereEffectorSelection> payload;
	uint32_t total = 0;
	if (const auto *director = GaussianSplatSceneDirector::get_singleton()) {
		director->build_sphere_effector_payload_for_renderer(p_renderer, payload, &total);
	}
	r_params.sphere_effector_count = payload.size();
	if (!payload.is_empty()) {
		r_params.sphere_effector_max_active = payload.size();
		for (uint32_t i = 0; i < payload.size() && i < GS_MAX_SPHERE_EFFECTORS; ++i) {
			auto &dst = r_params.sphere_effectors[i];
			const auto &src = payload[i];
			dst.enabled = src.enabled;
			dst.center = src.center;
			dst.radius = src.radius;
			dst.strength = src.strength;
			dst.falloff = src.falloff;
			dst.frequency = src.frequency;
			dst.affect_position = src.affect_position;
			dst.affect_opacity = src.affect_opacity;
			dst.opacity_strength = src.opacity_strength;
			dst.target_opacity = src.target_opacity;
		}
	} else {
		const auto settings = gs::settings::get_sphere_effector_settings(ps);
		r_params.sphere_effector_max_active = MIN(uint32_t(MAX(settings.max_effectors, 0)), GS_MAX_SPHERE_EFFECTORS);
		r_params.sphere_effector_enabled = settings.enabled && r_params.sphere_effector_max_active > 0;
		r_params.sphere_effector_center = settings.center;
		r_params.sphere_effector_radius = settings.radius;
		r_params.sphere_effector_strength = settings.strength;
		r_params.sphere_effector_falloff = settings.falloff;
		r_params.sphere_effector_frequency = settings.frequency;
		r_params.sphere_effector_affect_position = settings.affect_position;
		r_params.sphere_effector_affect_opacity = settings.affect_opacity;
		r_params.sphere_effector_opacity_strength = settings.opacity_strength;
		r_params.sphere_effector_target_opacity = settings.target_opacity;
	}
}
}
#endif
