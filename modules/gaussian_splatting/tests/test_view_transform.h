#pragma once

#include "test_macros.h"

#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/basis.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"

#include "../renderer/gaussian_splat_renderer.h"

namespace TestGaussianSplatting {

/**
 * Test suite for validating the view transform pipeline.
 *
 * These tests verify that:
 * 1. Camera transform -> View transform conversion is correct
 * 2. Matrix packing for GPU (column-major) is correct
 * 3. World-space points transform correctly to view space
 * 4. The inverse relationship holds: cam_transform * view_transform ≈ identity
 */

// Tolerance for floating point comparisons
constexpr float TRANSFORM_EPSILON = 1e-5f;

static bool is_near(float a, float b, float epsilon = TRANSFORM_EPSILON) {
	return Math::abs(a - b) < epsilon;
}

static bool is_near_vector(const Vector3 &a, const Vector3 &b, float epsilon = TRANSFORM_EPSILON) {
	return is_near(a.x, b.x, epsilon) && is_near(a.y, b.y, epsilon) && is_near(a.z, b.z, epsilon);
}

static bool is_identity_basis(const Basis &b, float epsilon = TRANSFORM_EPSILON) {
	return is_near(b[0][0], 1.0f, epsilon) && is_near(b[0][1], 0.0f, epsilon) && is_near(b[0][2], 0.0f, epsilon) &&
		   is_near(b[1][0], 0.0f, epsilon) && is_near(b[1][1], 1.0f, epsilon) && is_near(b[1][2], 0.0f, epsilon) &&
		   is_near(b[2][0], 0.0f, epsilon) && is_near(b[2][1], 0.0f, epsilon) && is_near(b[2][2], 1.0f, epsilon);
}

/**
 * Result structure for transform validation
 */
struct TransformValidationResult {
	bool passed = true;
	String error_message;

	// Detailed data for debugging
	Transform3D cam_transform;
	Transform3D view_transform;
	Transform3D product; // cam * view - should be identity
	Vector3 test_world_point;
	Vector3 test_view_point;
	Vector3 expected_view_point;

	void fail(const String &msg) {
		passed = false;
		error_message = msg;
	}
};

/**
 * Test 1: Verify that cam_transform.affine_inverse() produces correct view matrix
 *
 * For a camera at position C with rotation R:
 * - cam_transform.origin = C
 * - cam_transform.basis = R
 * - view_transform = cam_transform.affine_inverse()
 * - cam_transform * view_transform should equal identity
 */
static TransformValidationResult test_affine_inverse_identity() {
	TransformValidationResult result;

	// Create a camera transform: position (5, 3, -10), rotated 45 degrees around Y
	Quaternion rotation = Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(45.0f));
	result.cam_transform = Transform3D(Basis(rotation), Vector3(5.0f, 3.0f, -10.0f));

	// Compute view transform
	result.view_transform = result.cam_transform.affine_inverse();

	// Product should be identity
	result.product = result.cam_transform * result.view_transform;

	// Check if product is identity
	if (!is_near_vector(result.product.origin, Vector3(0, 0, 0), 1e-4f)) {
		result.fail(vformat("cam * view origin != (0,0,0), got (%.6f, %.6f, %.6f)",
			result.product.origin.x, result.product.origin.y, result.product.origin.z));
		return result;
	}

	if (!is_identity_basis(result.product.basis, 1e-4f)) {
		result.fail("cam * view basis != identity");
		return result;
	}

	return result;
}

/**
 * Test 2: Verify world-to-view transformation for a known point
 *
 * A point at the camera's position should transform to origin in view space.
 * A point in front of the camera should have negative Z in view space (Godot convention).
 */
static TransformValidationResult test_world_to_view_transform() {
	TransformValidationResult result;

	// Camera at (0, 2, 5) looking along -Z (default orientation)
	result.cam_transform = Transform3D(Basis(), Vector3(0.0f, 2.0f, 5.0f));
	result.view_transform = result.cam_transform.affine_inverse();

	// Test point at camera position - should become origin
	result.test_world_point = Vector3(0.0f, 2.0f, 5.0f);
	result.test_view_point = result.view_transform.xform(result.test_world_point);
	result.expected_view_point = Vector3(0.0f, 0.0f, 0.0f);

	if (!is_near_vector(result.test_view_point, result.expected_view_point, 1e-4f)) {
		result.fail(vformat("Point at camera position should be origin in view space. Got (%.4f, %.4f, %.4f)",
			result.test_view_point.x, result.test_view_point.y, result.test_view_point.z));
		return result;
	}

	// Test point in front of camera - should have negative Z
	result.test_world_point = Vector3(0.0f, 2.0f, 0.0f); // 5 units in front
	result.test_view_point = result.view_transform.xform(result.test_world_point);

	if (result.test_view_point.z >= 0.0f) {
		result.fail(vformat("Point in front of camera should have negative Z in view space. Got Z=%.4f",
			result.test_view_point.z));
		return result;
	}

	return result;
}

/**
 * Test 3: Verify that rotating camera rotates view space correctly
 *
 * When camera rotates right (positive Y rotation), points that were in front
 * should now appear to the left in view space.
 */
static TransformValidationResult test_camera_rotation_effect() {
	TransformValidationResult result;

	// Camera at origin, rotated 90 degrees to the right (around Y)
	Quaternion rotation = Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(90.0f));
	result.cam_transform = Transform3D(Basis(rotation), Vector3(0.0f, 0.0f, 0.0f));
	result.view_transform = result.cam_transform.affine_inverse();

	// A point at (0, 0, -5) in world space (originally in front of unrotated camera)
	// After camera rotates 90 degrees right, this point should be to the LEFT in view space
	result.test_world_point = Vector3(0.0f, 0.0f, -5.0f);
	result.test_view_point = result.view_transform.xform(result.test_world_point);

	// In view space, X should be positive (point is to our left after we rotated right)
	// Z should be near zero (point is to the side, not in front/behind)
	if (result.test_view_point.x < 0.0f) {
		result.fail(vformat("Point should be to the LEFT (positive X) in view space after camera rotates right. Got X=%.4f",
			result.test_view_point.x));
		return result;
	}

	return result;
}

/**
 * Test 4: Verify GPU matrix packing produces correct column-major layout
 *
 * GLSL mat4 expects column-major storage:
 * float[0-3] = column 0, float[4-7] = column 1, etc.
 */
struct GPUMatrixPackResult {
	bool passed = true;
	String error_message;
	float gpu_matrix[16];
	Transform3D source_transform;

	void fail(const String &msg) {
		passed = false;
		error_message = msg;
	}
};

static void pack_transform_to_gpu_matrix(const Transform3D &t, float out_matrix[16]) {
	// Pack exactly as tile_renderer.cpp does
	for (int column = 0; column < 3; column++) {
		Vector3 column_vec = t.basis.get_column(column);
		out_matrix[column * 4 + 0] = column_vec.x;
		out_matrix[column * 4 + 1] = column_vec.y;
		out_matrix[column * 4 + 2] = column_vec.z;
		out_matrix[column * 4 + 3] = 0.0f;
	}
	out_matrix[12] = t.origin.x;
	out_matrix[13] = t.origin.y;
	out_matrix[14] = t.origin.z;
	out_matrix[15] = 1.0f;
}

static GPUMatrixPackResult test_gpu_matrix_packing() {
	GPUMatrixPackResult result;

	// Create a known transform
	Quaternion rotation = Quaternion(Vector3(0, 1, 0), Math::deg_to_rad(30.0f));
	result.source_transform = Transform3D(Basis(rotation), Vector3(1.0f, 2.0f, 3.0f));

	pack_transform_to_gpu_matrix(result.source_transform, result.gpu_matrix);

	// Verify column 0 matches basis.get_column(0)
	Vector3 col0 = result.source_transform.basis.get_column(0);
	if (!is_near(result.gpu_matrix[0], col0.x) ||
		!is_near(result.gpu_matrix[1], col0.y) ||
		!is_near(result.gpu_matrix[2], col0.z)) {
		result.fail("GPU matrix column 0 doesn't match basis.get_column(0)");
		return result;
	}

	// Verify column 3 (translation) is origin
	if (!is_near(result.gpu_matrix[12], result.source_transform.origin.x) ||
		!is_near(result.gpu_matrix[13], result.source_transform.origin.y) ||
		!is_near(result.gpu_matrix[14], result.source_transform.origin.z)) {
		result.fail("GPU matrix column 3 doesn't match origin");
		return result;
	}

	// Verify M[15] = 1.0 for proper homogeneous coordinates
	if (!is_near(result.gpu_matrix[15], 1.0f)) {
		result.fail("GPU matrix [15] should be 1.0");
		return result;
	}

	return result;
}

/**
 * Test 5: Simulate full transform pipeline and verify result
 *
 * This test simulates what happens in the shader:
 * view_pos = view_matrix * vec4(world_pos, 1.0)
 */
static TransformValidationResult test_full_pipeline_simulation() {
	TransformValidationResult result;

	// Camera setup: at (3, 2, 8) looking toward origin
	Vector3 cam_pos = Vector3(3.0f, 2.0f, 8.0f);
	Vector3 look_at = Vector3(0.0f, 0.0f, 0.0f);
	Vector3 up = Vector3(0.0f, 1.0f, 0.0f);

	// Build camera transform (look_at style)
	Vector3 forward = (look_at - cam_pos).normalized();
	Vector3 right = up.cross(forward).normalized();
	Vector3 cam_up = forward.cross(right);

	Basis cam_basis;
	cam_basis.set_column(0, right);
	cam_basis.set_column(1, cam_up);
	cam_basis.set_column(2, -forward); // Camera looks down -Z

	result.cam_transform = Transform3D(cam_basis, cam_pos);
	result.view_transform = result.cam_transform.affine_inverse();

	// World point at origin
	result.test_world_point = Vector3(0.0f, 0.0f, 0.0f);

	// Pack for GPU
	float gpu_matrix[16];
	pack_transform_to_gpu_matrix(result.view_transform, gpu_matrix);

	// Simulate GPU transform: result = M * v (column-major)
	float vx = result.test_world_point.x;
	float vy = result.test_world_point.y;
	float vz = result.test_world_point.z;
	float vw = 1.0f;

	result.test_view_point.x = gpu_matrix[0]*vx + gpu_matrix[4]*vy + gpu_matrix[8]*vz + gpu_matrix[12]*vw;
	result.test_view_point.y = gpu_matrix[1]*vx + gpu_matrix[5]*vy + gpu_matrix[9]*vz + gpu_matrix[13]*vw;
	result.test_view_point.z = gpu_matrix[2]*vx + gpu_matrix[6]*vy + gpu_matrix[10]*vz + gpu_matrix[14]*vw;

	// Origin should be in front of camera (negative Z in view space)
	if (result.test_view_point.z >= 0.0f) {
		result.fail(vformat("Origin should be in front of camera (negative Z). Got Z=%.4f", result.test_view_point.z));
		return result;
	}

	// Verify using Transform3D.xform gives same result
	Vector3 cpu_view_point = result.view_transform.xform(result.test_world_point);
	if (!is_near_vector(result.test_view_point, cpu_view_point, 1e-4f)) {
		result.fail(vformat("GPU simulation (%.4f,%.4f,%.4f) != CPU xform (%.4f,%.4f,%.4f)",
			result.test_view_point.x, result.test_view_point.y, result.test_view_point.z,
			cpu_view_point.x, cpu_view_point.y, cpu_view_point.z));
		return result;
	}

	return result;
}

// Doctest TEST_CASE wrappers for automatic discovery
TEST_CASE("[GaussianSplatting][ViewTransform] Affine inverse identity property") {
	auto result = test_affine_inverse_identity();
	CHECK_MESSAGE(result.passed, result.error_message.utf8().get_data());
}

TEST_CASE("[GaussianSplatting][ViewTransform] World to view transform") {
	auto result = test_world_to_view_transform();
	CHECK_MESSAGE(result.passed, result.error_message.utf8().get_data());
}

TEST_CASE("[GaussianSplatting][ViewTransform] Camera rotation effect") {
	auto result = test_camera_rotation_effect();
	CHECK_MESSAGE(result.passed, result.error_message.utf8().get_data());
}

TEST_CASE("[GaussianSplatting][ViewTransform] GPU matrix packing") {
	auto result = test_gpu_matrix_packing();
	CHECK_MESSAGE(result.passed, result.error_message.utf8().get_data());
}

TEST_CASE("[GaussianSplatting][ViewTransform] Full pipeline simulation") {
	auto result = test_full_pipeline_simulation();
	CHECK_MESSAGE(result.passed, result.error_message.utf8().get_data());
}

// #929: the projection uploaded to the splat GPU pipeline must carry the same
// temporal jitter every other piece of geometry in the frame is rendered with.
//
// Until this landed, `render_projection` was `cam_projection` with `flip_y`
// applied and nothing else, while FSR2 -- handed `taa_jitter` immediately below
// the splat composite hook -- un-jitters the whole internal buffer by that
// vector (`thirdparty/amd-fsr2/shaders/ffx_fsr2_common.h:431-437`). The splat
// layer was therefore reconstructed displaced by +jitter, changing every frame
// along the Halton sequence: measured as splat-region swimming locked to the
// engine's jitter phase (autocorrelation +1.00 at lag 8 at scale 1.0).
//
// The three properties asserted here are exactly the three the rest of the
// pipeline depends on:
//
//   1. Zero jitter is BIT-IDENTICAL to the old matrix. Every non-temporal
//      configuration -- which is the Godot default -- must be untouched, and
//      "untouched" has to be exact, not approximate.
//   2. A non-zero jitter enters as `J * (flip * P)` with
//      `J = identity + add_jitter_offset`, the same left-multiplied translation
//      `RenderSceneDataRD::get_cam_projection()` applies
//      (`render_scene_data_rd.cpp:40-45`). The JITTER TERM is the engine's
//      exactly, for every projection type -- the last subcase pins that on an
//      off-axis frustum. The FLIP beside it is the module's own single-entry
//      one and is NOT the engine's; see build_render_projection()'s docblock.
//      Taking the jitter value verbatim is what
//      makes the composite depth test compare GS depth against the engine's
//      scene depth in the SAME subpixel frame; a rescaled or negated jitter
//      would misregister silhouettes instead of fixing them.
//   3. It is a pure NDC translation of exactly +jitter at every depth, and it
//      leaves `columns[0][0]` / `columns[1][1]` alone -- the two entries
//      `shaders/tile_binning.glsl:567-568` derives focal_x/focal_y from, and
//      hence the conic, the Jacobian and the subpixel-cull hysteresis.
TEST_CASE("[GaussianSplatting][ViewTransform] Render projection carries the engine TAA jitter") {
	Projection cam;
	cam.set_perspective(75.0f, 16.0f / 9.0f, 0.05f, 200.0f);

	// The pre-#929 construction, written out rather than referenced, so this case
	// keeps meaning what it says if build_render_projection() is refactored.
	Projection flipped_only = cam;
	flipped_only.columns[1][1] = -flipped_only.columns[1][1];

	SUBCASE("zero jitter is bit-identical to the flip-only matrix") {
		const Projection built = GaussianSplatRenderer::build_render_projection(cam, true, Vector2());
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				CHECK(built.columns[c][r] == flipped_only.columns[c][r]);
			}
		}
		const Projection unflipped = GaussianSplatRenderer::build_render_projection(cam, false, Vector2());
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				CHECK(unflipped.columns[c][r] == cam.columns[c][r]);
			}
		}
	}

	SUBCASE("a non-zero jitter is the engine's own left-multiplied translation") {
		// A real Halton sample at 960x540: camera_jitter_array[i] / viewport_size
		// (renderer_scene_cull.cpp:2695).
		const Vector2 jitter(0.5f / 960.0f, -0.25f / 540.0f);

		Projection correction;
		correction.add_jitter_offset(jitter);
		const Projection expected = correction * flipped_only;
		const Projection built = GaussianSplatRenderer::build_render_projection(cam, true, jitter);

		// Exact, not approximate: the two sides perform the identical sequence of
		// operations on identical inputs, so anything but bit equality means the
		// implementation composed something else.
		CHECK(built == expected);

		// Discrimination: the jitter must actually move the matrix, or every
		// equality above would hold for a no-op implementation too.
		bool differs = false;
		for (int c = 0; c < 4 && !differs; c++) {
			for (int r = 0; r < 4 && !differs; r++) {
				differs = built.columns[c][r] != flipped_only.columns[c][r];
			}
		}
		CHECK(differs);

		// The focal-length entries the tile binning shader reads are untouched.
		CHECK(built.columns[0][0] == flipped_only.columns[0][0]);
		CHECK(built.columns[1][1] == flipped_only.columns[1][1]);

		// Depth-independent NDC translation of exactly +jitter.
		const Vector3 probes[3] = {
			Vector3(0.3f, -0.2f, -0.5f),
			Vector3(0.3f, -0.2f, -5.0f),
			Vector3(0.3f, -0.2f, -100.0f),
		};
		//
		// ABSOLUTE tolerance, not doctest::Approx's relative one: the jitter is
		// ~5e-4 NDC and is recovered by subtracting two nearly equal quotients,
		// so the relative error of the result is far larger than the relative
		// error of its inputs. 2e-5 is 4% of the signal -- still two orders of
		// magnitude below "the jitter was not applied at all", which is what
		// this has to separate.
		const double kNdcTolerance = 2.0e-5;
		for (const Vector3 &view_pos : probes) {
			const Vector4 clip_base = flipped_only.xform(Vector4(view_pos.x, view_pos.y, view_pos.z, 1.0f));
			const Vector4 clip_jit = built.xform(Vector4(view_pos.x, view_pos.y, view_pos.z, 1.0f));
			if (Math::abs(clip_base.w) < 1e-6f || Math::abs(clip_jit.w) < 1e-6f) {
				FAIL("degenerate w in the probe projection; the test fixture is wrong");
				continue;
			}
			// w is untouched by a translation in columns[2][0]/[2][1], so the two
			// perspective divides use the same denominator.
			CHECK(clip_jit.w == doctest::Approx(clip_base.w).epsilon(1e-6));
			const double ndc_dx = double(clip_jit.x) / double(clip_jit.w) - double(clip_base.x) / double(clip_base.w);
			const double ndc_dy = double(clip_jit.y) / double(clip_jit.w) - double(clip_base.y) / double(clip_base.w);
			CHECK(Math::abs(ndc_dx - double(jitter.x)) < kNdcTolerance);
			CHECK(Math::abs(ndc_dy - double(jitter.y)) < kNdcTolerance);
		}
	}

	SUBCASE("different jitter phases produce different matrices") {
		// The property the render-cache reuse key depends on (#929): two
		// consecutive frames of a STATIC camera differ only here.
		const Projection a = GaussianSplatRenderer::build_render_projection(cam, true, Vector2(0.5f / 960.0f, 0.0f));
		const Projection b = GaussianSplatRenderer::build_render_projection(cam, true, Vector2(-0.25f / 960.0f, 0.0f));
		CHECK(a != b);
	}

	SUBCASE("the jitter term is unchanged by an off-axis frustum") {
		// The symmetric perspective above is exactly the case where the module's
		// single-entry flip and the engine's whole-row flip agree, so on its own
		// it cannot pin down what the jitter does for any other projection type.
		//
		// This is the case where they DIVERGE: set_frustum() with a vertical
		// offset writes a non-zero columns[2][1] (core/math/projection.cpp),
		// which the module's flip leaves positive and the engine's negates. That
		// divergence predates #929 and this PR does not change it -- it is
		// recorded in build_render_projection()'s docblock and is not asserted
		// here, because asserting it would pin a behaviour nobody has decided on.
		//
		// What IS asserted, and what the fix actually claims, is that the JITTER
		// contribution is the engine's own translation regardless of projection
		// type: it depends only on the w row, which neither flip touches.
		Projection frustum;
		frustum.set_frustum(2.0f, 16.0f / 9.0f, Vector2(0.0f, 0.3f), 0.05f, 200.0f);
		REQUIRE(frustum.columns[2][1] != 0.0f); // the divergent entry really is set

		Projection frustum_flipped = frustum;
		frustum_flipped.columns[1][1] = -frustum_flipped.columns[1][1];

		const Vector2 jitter(0.5f / 960.0f, -0.25f / 540.0f);
		const Projection built = GaussianSplatRenderer::build_render_projection(frustum, true, jitter);

		// The per-entry delta the jitter introduces is exactly what
		// add_jitter_offset() contributes through the w row, on every column.
		for (int c = 0; c < 4; c++) {
			const real_t w_row = frustum_flipped.columns[c][3];
			CHECK(built.columns[c][0] == doctest::Approx(frustum_flipped.columns[c][0] + jitter.x * w_row));
			CHECK(built.columns[c][1] == doctest::Approx(frustum_flipped.columns[c][1] + jitter.y * w_row));
			CHECK(built.columns[c][2] == frustum_flipped.columns[c][2]);
			CHECK(built.columns[c][3] == frustum_flipped.columns[c][3]);
		}
		// And zero jitter is still bit-identical for this projection type too.
		const Projection unjittered = GaussianSplatRenderer::build_render_projection(frustum, true, Vector2());
		CHECK(unjittered == frustum_flipped);
	}
}

} // namespace TestGaussianSplatting
