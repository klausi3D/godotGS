#ifndef GS_WIND_BOUNDS_H
#define GS_WIND_BOUNDS_H

#include "../core/gs_project_settings.h"
#include "gaussian_gpu_layout.h"
#include "core/math/basis.h"
#include <cmath>
#include <cstring>
#include <limits>

// W-S global bound, in scene units. Effectors have independent additive displacement;
// their push is the SUM of absolute enabled positional strengths, including overlapping
// spheres. The effective base is at least one for effectors/forced rows, preserving the
// existing zero-project-strength override while bounding the additive term.
namespace gs_wind {
// RenderingServer grows the LOCAL AABB before transforming it. The transformed cube's support on world axis i is
// local_margin * sum(abs(basis[i])); choose the smallest support so every world axis gains at least D_max.
inline real_t local_visibility_margin(double p_scene_margin, const Basis &p_basis) {
	if (p_scene_margin <= 0.0) { return 0.0; }
	double support = std::numeric_limits<double>::infinity();
	for (int axis = 0; axis < 3; ++axis) {
		const Vector3 &row = p_basis[axis];
		support = MIN(support, std::abs(double(row.x)) + std::abs(double(row.y)) + std::abs(double(row.z)));
	}
	ERR_FAIL_COND_V_MSG(!std::isfinite(p_scene_margin) || !std::isfinite(support) || support <= 0.0, 0.0,
			"Wind visibility bounds require finite displacement and nonzero support on every instance axis.");
	const double local = p_scene_margin / support;
	ERR_FAIL_COND_V_MSG(local > std::numeric_limits<real_t>::max(), 0.0, "Wind visibility margin exceeds coordinate precision.");
	real_t rounded = real_t(local);
	if (double(rounded) < local) { rounded = std::nextafter(rounded, std::numeric_limits<real_t>::infinity()); }
	return rounded;
}

// B3 seam: legacy full envelope until the fix is applied.
inline double node_displacement_bound(double p_d_max, uint8_t p_weight, bool p_semantic_valid) {
	return p_d_max;
}

inline double displacement_bound(double p_strength, double p_intensity, double p_push) {
	return MAX(0.0, p_strength) * MAX(0.0, p_intensity) * (1.0 + MAX(0.0, p_push));
}

inline bool row_wind_enabled(const gs::settings::GSWindSettings &p_settings, const InstanceDataGPU &p_row) {
	const uint32_t mode = uint32_t(p_row.params[3]);
	return mode == GS_INSTANCE_WIND_MODE_FORCE_ENABLED ||
			(mode != GS_INSTANCE_WIND_MODE_FORCE_DISABLED && p_settings.enabled);
}

// Reusable shadow-refresh predicate. Pass the corresponding FrameRecord::row_casts bit.
// This means nonzero wind amplitude on a casting row; it does not assert changing time
// (time_scale/frequency may be zero), evaluate distance/level fade, or include effectors.
inline bool wind_active_on_casting_row(const gs::settings::GSWindSettings &p_settings,
		const InstanceDataGPU &p_row, bool p_casts) {
	if (!p_casts || !row_wind_enabled(p_settings, p_row) || !(p_row.params[2] > 0.0f)) { return false; }
	const bool forced = uint32_t(p_row.params[3]) == GS_INSTANCE_WIND_MODE_FORCE_ENABLED;
	if (!(p_settings.strength > 0.0f) && !forced) { return false; }
	const Vector3 override_direction(p_row.wind_params[0], p_row.wind_params[1], p_row.wind_params[2]);
	const Vector3 direction = override_direction.length_squared() > 1.0e-8f ? override_direction : p_settings.direction;
	return direction.length_squared() > 1.0e-12f;
}

inline bool row_has_effectors(const InstanceDataGPU &p_row) {
	uint32_t mask = 0;
	std::memcpy(&mask, &p_row.effect_params[2], sizeof(mask));
	return mask != 0 && p_row.effect_params[0] > 0.0f;
}

inline double frame_displacement_bound(const gs::settings::GSWindSettings &p_settings,
		const InstanceDataGPU *p_rows, uint32_t p_count, double p_effector_push, bool p_project_effector = false) {
	double strength = MAX(0.0, double(p_settings.strength));
	double intensity = 0.0;
	for (uint32_t i = 0; i < p_count; ++i) {
		const InstanceDataGPU &row = p_rows[i];
		if (row_wind_enabled(p_settings, row)) {
			intensity = MAX(intensity, MAX(0.0, double(row.params[2])));
			if (uint32_t(row.params[3]) == GS_INSTANCE_WIND_MODE_FORCE_ENABLED) { strength = MAX(strength, 1.0); }
		}
		if (p_effector_push > 0.0 && (row_has_effectors(row) || (p_project_effector && row.effect_params[0] > 0.0f))) {
			strength = MAX(strength, 1.0);
			intensity = MAX(intensity, double(row.effect_params[0]));
		}
	}
	return displacement_bound(strength, intensity, p_effector_push);
}
}
#endif
