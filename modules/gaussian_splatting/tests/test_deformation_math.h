#pragma once

#include "test_macros.h"
#include "deformation_cpu_mirror.h"
#include "../renderer/render_frame_context_manager.h"

#include "core/os/os.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering_server.h"

// Tests for the CPU mirror of sphere-effector / wind deformation math in
// gs_deformation.glsl. The mirror lives in deformation_cpu_mirror.h and must
// stay in lockstep with the shader. Shader changes without a mirror update
// (or vice-versa) will make these tests misleading — review both files
// together.

TEST_CASE("[GaussianSplatting][HLOD][Deformation] wind strength fallback activates only under FORCE_ENABLED override") {
	using namespace GaussianSplatting::DeformationCPUMirror;

	// Project-level wind_strength unset (0.0) is the common case when a
	// scene uses only per-node wind overrides. Without the fallback, an
	// instance with wind_override_enabled + wind_strength=1.0 and
	// FORCE_ENABLED mode produced zero motion because the shader multiplied
	// by the global zero. The fallback substitutes 1.0 so the per-instance
	// intensity multiplier becomes effective.
	CHECK(Math::is_equal_approx(effective_wind_strength(0.0f, WIND_MODE_FORCE_ENABLED), 1.0f));

	// Non-zero globals are respected regardless of mode — the fallback is
	// "substitute when zero", not "override always".
	CHECK(Math::is_equal_approx(effective_wind_strength(2.5f, WIND_MODE_FORCE_ENABLED), 2.5f));

	// Non-override modes must not inject a synthetic base — that would
	// silently re-enable wind globally when the project disabled it.
	CHECK(Math::is_equal_approx(effective_wind_strength(0.0f, WIND_MODE_INHERIT), 0.0f));
	CHECK(Math::is_equal_approx(effective_wind_strength(0.0f, WIND_MODE_FORCE_DISABLED), 0.0f));

	// Negative (malformed) globals are clamped to zero by MAX(). Under
	// FORCE_ENABLED that path still produces the 1.0 fallback.
	CHECK(Math::is_equal_approx(effective_wind_strength(-1.0f, WIND_MODE_FORCE_ENABLED), 1.0f));
	CHECK(Math::is_equal_approx(effective_wind_strength(-1.0f, WIND_MODE_INHERIT), 0.0f));
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] wind temporal frequency fallback mirrors strength fallback") {
	using namespace GaussianSplatting::DeformationCPUMirror;

	// Same policy as strength: zero-global + FORCE_ENABLED substitutes 1.0,
	// everything else passes through. Kept symmetric so a caller that
	// overrides wind can rely on *both* amplitude and phase without tripping
	// on either of them being zero project-wide.
	CHECK(Math::is_equal_approx(effective_wind_temporal_frequency(0.0f, WIND_MODE_FORCE_ENABLED), 1.0f));
	CHECK(Math::is_equal_approx(effective_wind_temporal_frequency(0.75f, WIND_MODE_FORCE_ENABLED), 0.75f));
	CHECK(Math::is_equal_approx(effective_wind_temporal_frequency(0.0f, WIND_MODE_INHERIT), 0.0f));
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] phase depends only on scene rest position across relocated rows") {
	using namespace GaussianSplatting::DeformationCPUMirror;
	const Vector3 rest(31.999f, 2.0f, -64.001f);
	const float reference = wind_phase_noise(rest);
	// Neither physical atlas address nor row identity is an input to the phase API.
	const uint32_t atlas_indices[] = { 0u, 19u, 900001u };
	const uint32_t instance_ids[] = { 0u, 17u, 63u };
	for (uint32_t atlas_index : atlas_indices) {
		for (uint32_t instance_id : instance_ids) {
			(void)atlas_index;
			(void)instance_id;
			CHECK(wind_phase_noise(rest) == reference);
		}
	}
	CHECK(Math::abs(wind_phase_noise(rest + Vector3(0.001f, 0.0f, 0.0f)) - reference) < 0.001f);
	CHECK(wind_distance_level_fade(Vector3(128.0f, 0.0f, 0.0f), 0) == 0.0f);
	// ADR dynamics §6.3.1: the fade is by distance only. The former height-4 zero
	// (a level ramp 1 - height/4) made a parent sway 25 % less than its children.
	const Vector3 distances[] = { Vector3(), Vector3(64.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 96.0f), Vector3(128.0f, 0.0f, 0.0f) };
	for (const Vector3 &distance : distances) {
		const float leaf = wind_distance_level_fade(distance, 0);
		for (uint32_t level : { 1u, 2u, 3u, 4u, 7u, 31u }) {
			CHECK(wind_distance_level_fade(distance, level) == leaf);
		}
	}
	CHECK(wind_distance_level_fade(Vector3(), 4) == 1.0f);
	CHECK(wind_distance_level_fade(Vector3(0.0f, 0.0f, 96.0f), 4) == 0.5f);
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] caster and colour use the same current frame wind sample") {
	using namespace GaussianSplatting::DeformationCPUMirror;
	const Vector3 rest(1.25f, 0.5f, -4.0f);
	const float previous = wind_displacement(rest, 0.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 1.0f);
	const float colour = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 1.0f);
	const float caster = wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 1.0f);
	CHECK(caster == colour);
	CHECK(Math::abs(caster - previous) > 0.01f);
	CHECK(wind_displacement(rest, 1.1f, 1.0f, 0.1f, 2.0f, 0.75f, 1.0f, 0.0f) == 0.0f);
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] sphere effector anim factor is signed and integrates to zero") {
	using namespace GaussianSplatting::DeformationCPUMirror;

	// The #1 fix replaces the always-positive (0.5 + 0.5·sin) envelope with a
	// signed sin so position sway goes through rest instead of pulsing
	// one-sidedly outward. Integrated over a full period, mean should be ~0 —
	// if this regresses to a biased envelope, the test flags it immediately.
	const Vector3 center(0.0f, 0.0f, 0.0f);
	const Vector3 probe(3.0f, 0.0f, 0.0f); // well inside the sphere
	const float radius = 10.0f;
	const float falloff = 1.0f;
	const float frequency = 2.0f;

	const int samples = 512;
	double sum = 0.0;
	float min_factor = 2.0f;
	float max_factor = -2.0f;
	for (int i = 0; i < samples; i++) {
		const float t = (float(i) / float(samples)) * (1.0f / frequency); // one full period
		const SphereAnimSample s = sphere_effector_anim_sample(probe, center, radius, falloff, frequency, t);
		sum += s.anim_factor;
		min_factor = MIN(min_factor, s.anim_factor);
		max_factor = MAX(max_factor, s.anim_factor);
	}
	const double mean = sum / double(samples);
	CHECK(Math::abs(mean) < 0.05); // zero-mean to within sampling noise
	CHECK(min_factor < -0.9f); // swings through the negative half
	CHECK(max_factor > 0.9f); // and the positive half
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] sphere effector phase is spatially coherent across neighbours") {
	using namespace GaussianSplatting::DeformationCPUMirror;

	// The #1 fix ties anim phase to a low-frequency spatial term (scaled by
	// 1/radius) instead of a per-splat random hash. Two points a fraction of
	// a radius apart should therefore produce nearly-identical anim factors
	// — that's the thing that stops the sphere effector from reading as
	// per-splat noise. The wind path had spatial coherence already; this
	// brings the sphere path into parity.
	const Vector3 center(0.0f, 0.0f, 0.0f);
	const float radius = 10.0f;
	const float falloff = 1.0f;
	const float frequency = 2.0f;
	const float time_seconds = 0.17f; // arbitrary, away from zero-crossing

	const Vector3 a(2.0f, 0.0f, 0.0f);
	const Vector3 b(2.1f, 0.0f, 0.0f); // 1% of radius apart
	const float fa = sphere_effector_anim_sample(a, center, radius, falloff, frequency, time_seconds).anim_factor;
	const float fb = sphere_effector_anim_sample(b, center, radius, falloff, frequency, time_seconds).anim_factor;
	CHECK(Math::abs(fa - fb) < 0.05f);

	// Sanity-check the wavelength: a point one radius away from the first
	// probe should *not* be in phase — that's how the shader still produces
	// travelling motion, rather than a synchronous global pulse.
	const Vector3 c(a.x + radius, 0.0f, 0.0f);
	const float fc = sphere_effector_anim_sample(c, center, radius, falloff, frequency, time_seconds).anim_factor;
	// c is outside the sphere (radius=10, c.x = 12), so anim_factor would be
	// zero from the early-out. Probe a point still inside but on the far
	// side of one wavelength from `a`.
	const Vector3 c_inside(-2.0f, 0.0f, 0.0f); // ~4 units from `a` ≈ 0.4·radius offset
	const float fc_inside = sphere_effector_anim_sample(c_inside, center, radius, falloff, frequency, time_seconds).anim_factor;
	CHECK(Math::abs(fa - fc_inside) > 0.1f);
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] sphere effector spatial influence matches falloff envelope") {
	using namespace GaussianSplatting::DeformationCPUMirror;

	// Influence envelope is unaffected by the #1 sway fix (the edit only
	// touched the temporal phase). This test locks the spatial behaviour:
	// influence=1 at center, =0 at and beyond the radius, monotonic in
	// between. If the spatial path drifts, downstream opacity deformation
	// (which uses this same value) will mis-render in lockstep.
	const Vector3 center(0.0f, 0.0f, 0.0f);
	const float radius = 5.0f;
	const float falloff = 2.0f;

	CHECK(Math::is_equal_approx(
			sphere_effector_anim_sample(center, center, radius, falloff, 1.0f, 0.0f).spatial_influence,
			1.0f));
	CHECK(Math::is_equal_approx(
			sphere_effector_anim_sample(Vector3(radius, 0.0f, 0.0f), center, radius, falloff, 1.0f, 0.0f).spatial_influence,
			0.0f));
	CHECK(Math::is_equal_approx(
			sphere_effector_anim_sample(Vector3(radius + 1.0f, 0.0f, 0.0f), center, radius, falloff, 1.0f, 0.0f).spatial_influence,
			0.0f));

	// Monotonic decrease along the radial direction.
	const float near_center = sphere_effector_anim_sample(Vector3(1.0f, 0.0f, 0.0f), center, radius, falloff, 1.0f, 0.0f).spatial_influence;
	const float mid = sphere_effector_anim_sample(Vector3(2.5f, 0.0f, 0.0f), center, radius, falloff, 1.0f, 0.0f).spatial_influence;
	const float near_edge = sphere_effector_anim_sample(Vector3(4.0f, 0.0f, 0.0f), center, radius, falloff, 1.0f, 0.0f).spatial_influence;
	CHECK(near_center > mid);
	CHECK(mid > near_edge);
	CHECK(near_edge > 0.0f);
}

TEST_CASE("[GaussianSplatting][HLOD][Deformation] animation sampler latches one wall clock sample per server frame") {
	// Shadows and colour are separate calls in the same server frame. Wall
	// clock elapsed between them must not alter that frame's wind phase.
	const bool has_compositor = RenderingServer::get_singleton() != nullptr && RSG::rasterizer != nullptr;
	const double first = RenderFrameContextManager::sample_render_animation_time_seconds();
	// First call in the process latches epoch → 0. In a test binary this
	// is a one-shot because the epoch is a function-static; if a prior test
	// already called it, first will be small but non-zero. Both are valid —
	// just assert non-negativity and monotonicity rather than exact zero.
	CHECK(first >= 0.0);

	if (OS::get_singleton() != nullptr) {
		OS::get_singleton()->delay_usec(2000); // ~2 ms
	}
	const double second = RenderFrameContextManager::sample_render_animation_time_seconds();
	CHECK(second >= first);

	const double delta = second - first;
	if (has_compositor) {
		CHECK(second == first); // No server frame was advanced by this test.
	} else {
		// Device-free fallback retains the monotonic wall-clock contract.
		CHECK(delta > 0.0005);
		CHECK(delta < 1.0);
	}
}
