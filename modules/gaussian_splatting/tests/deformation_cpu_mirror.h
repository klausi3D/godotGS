#pragma once

// CPU-side mirror of the sphere-effector / wind deformation math in
// `modules/gaussian_splatting/shaders/includes/gs_deformation.glsl`. The
// shader is the production code path; this header exists so doctests can
// exercise the same math without a GPU. Any edit to the shader must be
// reflected here (and vice-versa), or the tests in
// `test_deformation_math.h` will drift from the shader and become
// misleading.
//
// Keep this header free of Godot dependencies beyond what the shader
// semantics require — the point is a tight, reviewable reference.

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"

#include <cstdint>

namespace GaussianSplatting {
namespace DeformationCPUMirror {

// Mirror of GS_INSTANCE_WIND_MODE_* from gs_instance_layout.glsl.
enum WindMode : uint32_t {
	WIND_MODE_INHERIT = 0u,
	WIND_MODE_FORCE_DISABLED = 1u,
	WIND_MODE_FORCE_ENABLED = 2u,
	WIND_MODE_SEMANTIC_INHERIT = 3u,
};

inline uint32_t wind_hash_u32(uint32_t v) {
	v ^= v >> 16u;
	v *= 0x7feb352du;
	v ^= v >> 15u;
	v *= 0x846ca68bu;
	v ^= v >> 16u;
	return v;
}

inline float wind_lattice_value(int32_t x, int32_t z) {
	const uint32_t value = wind_hash_u32(uint32_t(x) ^ (wind_hash_u32(uint32_t(z)) * 0x9e3779b9u));
	return float(value & 0x00FFFFFFu) / 16777215.0f;
}

inline float wind_phase_noise(const Vector3 &scene_rest_position) {
	const float lattice_x = float(scene_rest_position.x) / 32.0f;
	const float lattice_z = float(scene_rest_position.z) / 32.0f;
	const int32_t x = int32_t(Math::floor(lattice_x));
	const int32_t z = int32_t(Math::floor(lattice_z));
	float fx = lattice_x - float(x);
	float fz = lattice_z - float(z);
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float lower = wind_lattice_value(x, z) * (1.0f - fx) + wind_lattice_value(x + 1, z) * fx;
	const float upper = wind_lattice_value(x, z + 1) * (1.0f - fx) + wind_lattice_value(x + 1, z + 1) * fx;
	return (lower * (1.0f - fz) + upper * fz) * 6.28318530718f;
}

inline float wind_distance_level_fade(const Vector3 &rest_from_camera, uint32_t level) {
	return CLAMP((128.0f - float(rest_from_camera.length())) / 64.0f, 0.0f, 1.0f) *
			CLAMP(1.0f - float(level) / 4.0f, 0.0f, 1.0f);
}

inline float splat_wind_weight(uint32_t semantic, float opacity, WindMode mode, bool force_legacy = false) {
	return 0.2f + 0.8f * CLAMP(1.0f - opacity, 0.0f, 1.0f);
}

inline float wind_displacement(const Vector3 &scene_rest_position, float time_seconds, float temporal_frequency,
		float spatial_frequency, float strength, float opacity, float row_intensity, float deformation_fade,
		uint32_t semantic = 0, WindMode mode = WIND_MODE_INHERIT, bool force_legacy = false) {
	const float resistance = 0.2f + 0.8f * CLAMP(1.0f - opacity, 0.0f, 1.0f);
	const float phase = float(scene_rest_position.x + scene_rest_position.z) * spatial_frequency +
			time_seconds * temporal_frequency + wind_phase_noise(scene_rest_position);
	return Math::sin(phase) * strength * resistance * MAX(row_intensity, 0.0f) * CLAMP(deformation_fade, 0.0f, 1.0f);
}

// Mirror of the per-instance wind-strength fallback path in
// gs_apply_wind_deformation() after the #3 fix (wind fallback). When the
// project-level wind_strength is zero but an instance forces wind on via
// its own override, the shader substitutes a unit base so the instance's
// multiplicative contributions (intensity, frequency) are effective.
inline float effective_wind_strength(float global_strength, WindMode mode) {
	const float base = MAX(global_strength, 0.0f);
	if (mode == WIND_MODE_FORCE_ENABLED && base <= 0.0f) {
		return 1.0f;
	}
	return base;
}

inline float effective_wind_temporal_frequency(float global_frequency, WindMode mode) {
	const float base = MAX(global_frequency, 0.0f);
	if (mode == WIND_MODE_FORCE_ENABLED && base <= 0.0f) {
		return 1.0f;
	}
	return base;
}

// Mirror of gs_compute_sphere_effector_weight() after #1 (sphere position
// sway). Returns a signed temporal factor in [-1, 1] so splats oscillate
// through rest, and couples the phase to a low-frequency spatial term so
// neighbouring splats stay in phase — no per-splat random jitter. Callers
// use the returned factor multiplied by the spatial influence (returned as
// `spatial_influence`) to drive position deformation.
struct SphereAnimSample {
	float spatial_influence; // 0..1, steady spatial falloff envelope
	float anim_factor; // -1..1, signed temporal pulse used by position
};

inline SphereAnimSample sphere_effector_anim_sample(
		const Vector3 &world_position,
		const Vector3 &effector_center,
		float radius,
		float falloff,
		float frequency,
		float time_seconds) {
	SphereAnimSample out = { 0.0f, 0.0f };
	if (radius <= 1e-6f) {
		return out;
	}
	const Vector3 delta = world_position - effector_center;
	const float distance_to_center = delta.length();
	if (distance_to_center >= radius) {
		return out;
	}
	const float normalized = CLAMP(1.0f - (distance_to_center / radius), 0.0f, 1.0f);
	const float falloff_safe = MAX(falloff, 0.001f);
	out.spatial_influence = Math::pow(normalized, falloff_safe);
	const float freq_safe = frequency > 0.0f ? frequency : 2.0f;
	const float spatial_coeff = 1.0f / MAX(radius, 1e-3f);
	const float spatial_phase = delta.dot(Vector3(spatial_coeff, spatial_coeff, spatial_coeff));
	const float anim_phase = time_seconds * freq_safe * 6.28318530718f + spatial_phase + wind_phase_noise(world_position);
	out.anim_factor = Math::sin(anim_phase);
	return out;
}

} // namespace DeformationCPUMirror
} // namespace GaussianSplatting
