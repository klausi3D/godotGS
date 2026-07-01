/**
 * @file gs_project_settings.h
 * @brief Shared ProjectSettings helper utilities for the Gaussian Splatting module.
 *
 * This header provides canonical, type-safe accessors for reading Godot
 * ProjectSettings values.  It replaces the many per-file static copies of
 * _get_bool_setting / _ps_get_uint / _ps_get_float that were duplicated
 * across the module.
 *
 * Usage:
 *   #include "core/gs_project_settings.h"
 *   bool val = gs::settings::get_bool(ps, "rendering/gaussian_splatting/debug/enable_all_debug", false);
 */

#ifndef GS_PROJECT_SETTINGS_H
#define GS_PROJECT_SETTINGS_H

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/variant/variant.h"

namespace gs {

static constexpr int GS_MIN_MAX_SPLAT_COUNT = 1000;  // Minimum allowed quality/max_splat_count floor.

namespace settings {

/**
 * @brief Read a boolean from ProjectSettings with type coercion.
 *
 * Handles BOOL, INT (non-zero == true) and FLOAT (non-zero-approx == true).
 * Returns @p p_fallback when @p p_ps is null, the setting does not exist, or the
 * stored type cannot be coerced.
 */
static inline bool get_bool(ProjectSettings *p_ps, const StringName &p_name, bool p_fallback) {
	ERR_FAIL_NULL_V(p_ps, p_fallback);

	if (!p_ps->has_setting(p_name)) {
		return p_fallback;
	}

	Variant value = p_ps->get_setting_with_override(p_name);
	if (value.get_type() == Variant::BOOL) {
		return (bool)value;
	}
	if (value.get_type() == Variant::INT) {
		return value.operator int64_t() != 0;
	}
	if (value.get_type() == Variant::FLOAT) {
		return !Math::is_zero_approx((float)value.operator double());
	}

	return p_fallback;
}

/**
 * @brief Read an unsigned 32-bit integer from ProjectSettings with type coercion.
 *
 * Handles INT and FLOAT types.  Negative values fall back to @p p_fallback.
 */
static inline uint32_t get_uint(ProjectSettings *p_ps, const StringName &p_name, uint32_t p_fallback) {
	if (!p_ps || !p_ps->has_setting(p_name)) {
		return p_fallback;
	}

	Variant value = p_ps->get_setting_with_override(p_name);
	if (value.get_type() == Variant::INT) {
		int64_t v = value.operator int64_t();
		return v < 0 ? p_fallback : uint32_t(v);
	}
	if (value.get_type() == Variant::FLOAT) {
		double v = value.operator double();
		return v < 0.0 ? p_fallback : uint32_t(Math::round(v));
	}

	return p_fallback;
}

/**
 * @brief Read a float from ProjectSettings with type coercion.
 *
 * Handles FLOAT and INT types.
 */
static inline float get_float(ProjectSettings *p_ps, const StringName &p_name, float p_fallback) {
	if (!p_ps || !p_ps->has_setting(p_name)) {
		return p_fallback;
	}

	Variant value = p_ps->get_setting_with_override(p_name);
	if (value.get_type() == Variant::FLOAT) {
		return (float)value.operator double();
	}
	if (value.get_type() == Variant::INT) {
		return (float)value.operator int64_t();
	}

	return p_fallback;
}

/**
 * @brief Read a signed integer from ProjectSettings with type coercion.
 *
 * Unlike get_uint, preserves negative values (needed for sentinel defaults).
 * Handles INT, FLOAT, and BOOL (for backward-compatible bool-to-int migration).
 */
static inline int get_int(ProjectSettings *p_ps, const StringName &p_name, int p_fallback) {
	if (!p_ps || !p_ps->has_setting(p_name)) {
		return p_fallback;
	}
	Variant value = p_ps->get_setting_with_override(p_name);
	if (value.get_type() == Variant::INT) {
		return static_cast<int>(value.operator int64_t());
	}
	if (value.get_type() == Variant::FLOAT) {
		return static_cast<int>(Math::round(value.operator double()));
	}
	if (value.get_type() == Variant::BOOL) {
		return value.operator bool() ? 1 : 0;
	}
	return p_fallback;
}

/**
 * @brief Convenience: check whether "all debug" is enabled for the GS module.
 */
static inline bool is_all_debug_enabled(ProjectSettings *p_ps) {
	return get_bool(p_ps, "rendering/gaussian_splatting/debug/enable_all_debug", false);
}

/**
 * @brief Check whether data-level debug logging is enabled.
 */
static inline bool is_data_log_enabled() {
#ifdef GS_SILENCE_LOGS
	return false;
#else
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return false;
	}
	if (is_all_debug_enabled(ps)) {
		return true;
	}
	return get_bool(ps, "rendering/gaussian_splatting/debug/enable_data_logging", false);
#endif
}

/**
 * @brief Check whether per-frame debug logging is enabled.
 */
static inline bool is_frame_log_enabled() {
#ifdef GS_SILENCE_LOGS
	return false;
#else
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return false;
	}
	if (is_all_debug_enabled(ps)) {
		return true;
	}
	return get_bool(ps, "rendering/gaussian_splatting/debug/enable_frame_logging", false);
#endif
}

// Streaming route policy constants.
enum GSStreamingRoutePolicy {
	GS_ROUTE_RESIDENT = 0,
	GS_ROUTE_STREAMING = 1,
};

struct GSSphereEffectorSettings {
	int max_effectors = 1;
	bool enabled = false;
	Vector3 center = Vector3();
	float radius = 0.0f;
	float strength = 0.0f;
	float falloff = 2.0f;
	float frequency = 2.0f;
	bool affect_position = true;
	bool affect_opacity = false;
	float opacity_strength = 1.0f;
	float target_opacity = 0.0f;
};

static inline const char *get_streaming_route_policy_token(int p_policy) {
	switch (p_policy) {
		case GS_ROUTE_RESIDENT:
			return "resident";
		case GS_ROUTE_STREAMING:
			return "streaming";
		default:
			return "unknown";
	}
}

static inline const char *get_streaming_route_policy_source(ProjectSettings *p_ps) {
	if (!p_ps) {
		return "default_fallback";
	}
	if (!p_ps->has_setting("rendering/gaussian_splatting/streaming/route_policy")) {
		return "default_fallback";
	}
	return "route_policy";
}

/**
 * @brief Resolve the effective streaming route policy.
 *
 * Scope: **world submissions only.** Consumed by GaussianSplatWorld3D when it
 * builds its WorldSubmission's residency hint (streaming vs resident backend).
 *
 * GaussianSplatNode3D (direct per-instance content) always publishes
 * SUBMISSION_RESIDENCY_HINT_RESIDENT regardless of this setting; do not treat
 * route_policy as a project-wide backend knob. If per-node backend steering is
 * ever needed, introduce a narrow per-world-group setting rather than
 * broadening this one.
 */
static inline int get_streaming_route_policy(ProjectSettings *p_ps) {
	if (!p_ps) {
		return GS_ROUTE_STREAMING; // safe fallback: preserve existing behavior
	}
	return (int)get_uint(p_ps, "rendering/gaussian_splatting/streaming/route_policy",
			(uint32_t)GS_ROUTE_STREAMING);
}

static inline GSSphereEffectorSettings get_sphere_effector_settings(ProjectSettings *p_ps, bool p_warn_on_clamp = false) {
	GSSphereEffectorSettings settings;
	if (!p_ps) {
		return settings;
	}

	const int requested_max_effectors = get_int(p_ps, "rendering/gaussian_splatting/effects/max_effectors", settings.max_effectors);
	settings.max_effectors = CLAMP(requested_max_effectors, 0, 1);
	if (p_warn_on_clamp && requested_max_effectors > 1) {
		WARN_PRINT_ONCE("[GaussianSplatting] Only one sphere effector is currently supported. "
				"'rendering/gaussian_splatting/effects/max_effectors' was clamped to 1.");
	}

	settings.enabled = get_bool(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_enabled", settings.enabled);
	settings.center.x = get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_center_x", settings.center.x);
	settings.center.y = get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_center_y", settings.center.y);
	settings.center.z = get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_center_z", settings.center.z);
	settings.radius = MAX(get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_radius", settings.radius), 0.0f);
	settings.strength = get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_strength", settings.strength);
	settings.falloff = MAX(get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_falloff", settings.falloff), 0.001f);
	settings.frequency = MAX(get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_frequency", settings.frequency), 0.1f);
	settings.affect_position = get_bool(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_affect_position", settings.affect_position);
	settings.affect_opacity = get_bool(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_affect_opacity", settings.affect_opacity);
	settings.opacity_strength = CLAMP(
			get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_opacity_strength", settings.opacity_strength),
			0.0f, 1.0f);
	settings.target_opacity = CLAMP(
			get_float(p_ps, "rendering/gaussian_splatting/effects/sphere_effector_target_opacity", settings.target_opacity),
			0.0f, 1.0f);
	settings.enabled = settings.enabled && settings.max_effectors > 0;
	return settings;
}

} // namespace settings
} // namespace gs

#endif // GS_PROJECT_SETTINGS_H
