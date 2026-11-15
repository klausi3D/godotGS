#ifndef TEST_CAMERA_CUT_POLICY_H
#define TEST_CAMERA_CUT_POLICY_H

#include "test_macros.h"
#include "../renderer/camera_cut_policy.h"

#include <initializer_list>

namespace TestCameraCutPolicy {

inline Projection projection(real_t p_far = 400.0) {
	Projection result;
	result.set_perspective(107.5, 16.0 / 9.0, 0.05, p_far);
	return result;
}

TEST_CASE("[GaussianSplatting][D3Telemetry] camera cuts bootstrap once and coalesce explicit hints") {
	GaussianSplatting::CameraCutDetector detector;
	const Projection lens = projection();
	const Vector2i viewport(1920, 1080);
	CHECK_FALSE(detector.update(Transform3D(), lens, viewport, 0));
	CHECK(detector.update(Transform3D(), lens, viewport, 2));
	detector.measurement_completed();
	CHECK_FALSE(detector.update(Transform3D(), lens, viewport, 2));
	CHECK(detector.update(Transform3D(), lens, viewport, 3));
	detector.measurement_completed();
	CHECK_FALSE(detector.update(Transform3D(), lens, viewport, 3));
	// A hint before the first frame is served by the existing bootstrap.
	GaussianSplatting::CameraCutDetector first;
	CHECK(first.update(Transform3D(), lens, viewport, 1));
	const auto decision = GaussianSplatting::overlap_measurement_decision(false, true, false, true, false, true);
	CHECK(decision.bootstrap);
	CHECK_FALSE(decision.cut);
	CHECK(decision.sync);
}

TEST_CASE("[GaussianSplatting][D3Telemetry] T0 speed and fast continuous rotation stay asynchronous") {
	for (int fps : { 10, 30, 60, 144 }) {
		GaussianSplatting::CameraCutDetector detector;
		const Projection lens = projection();
		Transform3D camera;
		CHECK_FALSE(detector.update(camera, lens, Vector2i(1920, 1080), 0));
		for (int frame = 1; frame <= fps * 2; ++frame) {
			camera.origin.x = 120.0 * frame / fps;
			// 720 degrees/s is much faster than T0's 24 degrees/s town orbit.
			camera.basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(real_t(720.0 * frame / fps)));
			const bool cut = detector.update(camera, lens, Vector2i(1920, 1080), 0);
			CHECK_FALSE(cut);
			CHECK_FALSE(GaussianSplatting::overlap_measurement_decision(false, true, false, true, true, cut).sync);
		}
	}
}

TEST_CASE("[GaussianSplatting][D3Telemetry] teleport demand is synchronous on the chunk route only") {
	for (real_t scale : { real_t(0.01), real_t(1), real_t(100) }) {
		GaussianSplatting::CameraCutDetector detector;
		const Projection lens = projection(400 * scale);
		Transform3D camera;
		detector.update(camera, lens, Vector2i(1920, 1080), 0);
		camera.origin = Vector3(202.196 * scale, 0, 0); // Smallest T0 teleport (C7).
		const bool cut = detector.update(camera, lens, Vector2i(1920, 1080), 0);
		CHECK(cut);
		CHECK(GaussianSplatting::overlap_measurement_decision(false, true, false, true, true, cut).sync);
		CHECK_FALSE(GaussianSplatting::overlap_measurement_decision(true, true, false, true, true, cut).sync);
		CHECK_FALSE(GaussianSplatting::overlap_measurement_decision(false, false, false, true, true, cut).sync);
		detector.measurement_completed();
		CHECK_FALSE(detector.update(camera, lens, Vector2i(1920, 1080), 0));
	}
	// Owner-approved cuts also override timestamp-preserving bootstrap policy.
	CHECK(GaussianSplatting::overlap_measurement_decision(false, true, false, false, true, true).sync);
	CHECK(GaussianSplatting::overlap_measurement_decision(false, true, false, false, false, true).sync);
}

TEST_CASE("[GaussianSplatting][D3Telemetry] camera lens viewport and direction discontinuities") {
	GaussianSplatting::CameraCutDetector detector;
	Transform3D camera;
	Projection lens = projection();
	detector.update(camera, lens, Vector2i(1920, 1080), 0);
	camera.basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(real_t(139.4)));
	CHECK_FALSE(detector.update(camera, lens, Vector2i(1920, 1080), 0));
	camera.basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(real_t(319.4)));
	CHECK(detector.update(camera, lens, Vector2i(1920, 1080), 0));
	detector.measurement_completed();
	CHECK(detector.update(camera, lens, Vector2i(1280, 720), 0));
	detector.measurement_completed();
	// Incremental zoom does not classify as a cut.
	lens.set_perspective(107.0, 16.0 / 9.0, 0.05, 400.0);
	CHECK_FALSE(detector.update(camera, lens, Vector2i(1280, 720), 0));
	lens.set_perspective(50.0, 16.0 / 9.0, 0.05, 400.0);
	CHECK(detector.update(camera, lens, Vector2i(1280, 720), 0));
	// A cut remains pending until a measurement actually completes.
	CHECK(detector.update(camera, lens, Vector2i(1280, 720), 0));
	detector.measurement_completed();
	CHECK_FALSE(detector.update(camera, lens, Vector2i(1280, 720), 0));
}

} // namespace TestCameraCutPolicy

#endif
