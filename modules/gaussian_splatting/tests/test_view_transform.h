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
//      off-axis frustum. Since #1159 the FLIP beside it is the engine's own
//      whole-row flip too (GaussianSplatRenderer::apply_flip_y()), so the
//      off-axis subcase pins the WHOLE matrix to the engine's.
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
	// For this SYMMETRIC perspective the old single-entry flip and the engine's
	// whole-row flip (#1159) are the same matrix, so "bit-identical to the
	// pre-#929 matrix" still holds after #1159 changed the flip.
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

	SUBCASE("an off-axis frustum gets the engine's flip and jitter, whole matrix (#1159)") {
		// The symmetric perspective above is exactly the case where the old
		// single-entry flip and the engine's whole-row flip agree, so on its own
		// it cannot pin down either term for any other projection type.
		//
		// set_frustum() with a vertical offset writes a non-zero columns[2][1]
		// (core/math/projection.cpp), which the pre-#1159 flip left positive and
		// the engine negates. Until #1159 this subcase declined to assert the
		// divergence; it now asserts the agreement: the render projection is the
		// engine's RenderSceneDataRD::get_cam_projection() correction
		// (render_scene_data_rd.cpp:40-45) -- flip and jitter in ONE matrix --
		// applied to the camera projection, minus only its depth remap.
		Projection frustum;
		frustum.set_frustum(2.0f, 16.0f / 9.0f, Vector2(0.0f, 0.3f), 0.05f, 200.0f);
		REQUIRE(frustum.columns[2][1] != 0.0f); // the divergent entry really is set

		const Vector2 jitter(0.5f / 960.0f, -0.25f / 540.0f);
		Projection engine_correction;
		engine_correction.set_depth_correction(true, false, false);
		engine_correction.add_jitter_offset(jitter);
		const Projection expected = engine_correction * frustum;

		const Projection built = GaussianSplatRenderer::build_render_projection(frustum, true, jitter);
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				CHECK(built.columns[c][r] == doctest::Approx(expected.columns[c][r]));
			}
		}

		// Discrimination: the pre-#1159 construction (single-entry flip, then the
		// same jitter) differs from the engine in exactly the clip-Y entry the
		// vertical offset writes, by a margin no tolerance above could absorb.
		Projection old_flip = frustum;
		old_flip.columns[1][1] = -old_flip.columns[1][1];
		Projection jitter_only;
		jitter_only.add_jitter_offset(jitter);
		const Projection old_built = jitter_only * old_flip;
		CHECK(Math::abs(old_built.columns[2][1] - expected.columns[2][1]) > real_t(0.5));
		CHECK(Math::abs(built.columns[2][1] - expected.columns[2][1]) < real_t(1e-6));

		// Zero jitter: exactly the flip correction times the camera projection,
		// with no jitter multiply on top.
		Projection flip_correction;
		flip_correction.set_depth_correction(true, false, false);
		const Projection unjittered = GaussianSplatRenderer::build_render_projection(frustum, true, Vector2());
		CHECK(unjittered == flip_correction * frustum);

		// The focal-length entries the tile binning shader reads are the same
		// under both flips: #1159 does not move the conic or the Jacobian.
		CHECK(built.columns[0][0] == old_built.columns[0][0]);
		CHECK(built.columns[1][1] == old_built.columns[1][1]);
	}
}

// #1159: flip_y is the engine's whole-row clip-Y correction, not a negation of
// columns[1][1] alone.
//
// The engine renders every mesh with `correction * cam_projection`, where
// `correction.set_depth_correction(flip_y)` holds m[5] = -1
// (core/math/projection.cpp:787-801, render_scene_data_rd.cpp:40-45). That
// negates every entry of the clip-Y output row. GaussianSplatRenderer used to
// negate only columns[1][1] in the render, cull and shadow projections; the two
// agree only when columns[0][1], [2][1] and [3][1] are zero. For an off-axis
// frustum (Camera3D PROJECTION_FRUSTUM with frustum_offset.y != 0, XR eyes) or a
// shifted orthographic matrix they do not, and splats landed at a different NDC
// y than meshes while the culler extracted a vertically mirrored frustum.
//
// What a failure looks like for each subcase: the asymmetric subcases compare
// against the engine's own construction AND against the defining property of a
// Y flip (NDC y negated, x/z/w untouched) at probe points, and each one first
// proves its fixture can see the bug (the old flip lands >= 0.9 NDC away).
static Vector3 gs_test_ndc(const Projection &p_projection, const Vector3 &p_view_pos) {
	const Vector4 clip = p_projection.xform(Vector4(p_view_pos.x, p_view_pos.y, p_view_pos.z, 1.0f));
	return Vector3(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
}

static Projection gs_test_old_single_entry_flip(const Projection &p_projection) {
	Projection flipped = p_projection;
	flipped.columns[1][1] = -flipped.columns[1][1];
	return flipped;
}

static Projection gs_test_engine_flip(const Projection &p_projection) {
	Projection correction;
	correction.set_depth_correction(true, false, false);
	return correction * p_projection;
}

TEST_CASE("[GaussianSplatting][ViewTransform] flip_y is the engine's whole-row correction (#1159)") {
	SUBCASE("symmetric perspective is unchanged, bit for bit") {
		Projection cam;
		cam.set_perspective(75.0f, 16.0f / 9.0f, 0.05f, 200.0f);
		const Projection flipped = GaussianSplatRenderer::apply_flip_y(cam, true);
		CHECK(flipped == gs_test_old_single_entry_flip(cam));
		CHECK(flipped == gs_test_engine_flip(cam));
		CHECK(GaussianSplatRenderer::apply_flip_y(cam, false) == cam);
	}

	SUBCASE("off-axis set_frustum matches the engine and not the old single-entry flip") {
		// The audit's reproduction (record C06): l=-0.5 r=1.0 b=-0.3 t=0.8.
		Projection frustum;
		frustum.set_frustum(-0.5f, 1.0f, -0.3f, 0.8f, 0.1f, 100.0f);
		REQUIRE(frustum.columns[2][1] != 0.0f);

		const Projection flipped = GaussianSplatRenderer::apply_flip_y(frustum, true);
		CHECK(flipped == gs_test_engine_flip(frustum));
		CHECK(flipped != gs_test_old_single_entry_flip(frustum));
		CHECK(flipped.columns[2][1] == -frustum.columns[2][1]);
		CHECK(GaussianSplatRenderer::apply_flip_y(frustum, false) == frustum);

		const Vector3 probes[3] = {
			Vector3(0.0f, 0.0f, -1.0f),
			Vector3(0.2f, 0.15f, -3.0f),
			Vector3(-0.1f, -0.4f, -40.0f),
		};
		for (const Vector3 &probe : probes) {
			const Vector3 base = gs_test_ndc(frustum, probe);
			const Vector3 ndc = gs_test_ndc(flipped, probe);
			CHECK(ndc.x == doctest::Approx(base.x));
			CHECK(ndc.y == doctest::Approx(-base.y));
			CHECK(ndc.z == doctest::Approx(base.z));
		}
		// The fixture can see the bug: (0,0,-1) is at NDC y = -0.4545 unflipped;
		// the engine puts it at +0.4545, the old flip left it at -0.4545.
		const real_t engine_y = gs_test_ndc(flipped, probes[0]).y;
		const real_t old_y = gs_test_ndc(gs_test_old_single_entry_flip(frustum), probes[0]).y;
		CHECK(engine_y == doctest::Approx(0.5 / 1.1));
		CHECK(Math::abs(engine_y - old_y) > real_t(0.9));
	}

	SUBCASE("shifted orthographic matches the engine and not the old single-entry flip") {
		Projection ortho;
		ortho.set_orthogonal(-1.0f, 3.0f, -0.5f, 1.5f, 0.05f, 100.0f);
		REQUIRE(ortho.columns[3][1] != 0.0f);

		const Projection flipped = GaussianSplatRenderer::apply_flip_y(ortho, true);
		CHECK(flipped == gs_test_engine_flip(ortho));
		CHECK(flipped != gs_test_old_single_entry_flip(ortho));
		CHECK(flipped.columns[3][1] == -ortho.columns[3][1]);
		CHECK(flipped.is_orthogonal());

		const Vector3 probes[2] = {
			Vector3(0.0f, 0.0f, -1.0f),
			Vector3(1.5f, 1.2f, -20.0f),
		};
		for (const Vector3 &probe : probes) {
			const Vector3 base = gs_test_ndc(ortho, probe);
			const Vector3 ndc = gs_test_ndc(flipped, probe);
			CHECK(ndc.x == doctest::Approx(base.x));
			CHECK(ndc.y == doctest::Approx(-base.y));
			CHECK(ndc.z == doctest::Approx(base.z));
		}
		const real_t engine_y = gs_test_ndc(flipped, probes[0]).y;
		const real_t old_y = gs_test_ndc(gs_test_old_single_entry_flip(ortho), probes[0]).y;
		CHECK(Math::abs(engine_y - old_y) == doctest::Approx(1.0));
	}

	SUBCASE("render projection uses the same flip as apply_flip_y") {
		Projection frustum;
		frustum.set_frustum(-0.5f, 1.0f, -0.3f, 0.8f, 0.1f, 100.0f);
		CHECK(GaussianSplatRenderer::build_render_projection(frustum, true, Vector2()) ==
				GaussianSplatRenderer::apply_flip_y(frustum, true));
		CHECK(GaussianSplatRenderer::build_render_projection(frustum, false, Vector2()) == frustum);
	}
}

// #1156: the CPU half of the orthographic tile-binning contract.
//
// shaders/tile_binning.glsl classifies the uploaded projection as orthographic
// when abs(projection_matrix[2][3]) < 0.5 (the same test tile_resolve.glsl uses)
// and then builds the EWA Jacobian as diag(focal_x, focal_y), focal = P[i][i] *
// viewport / 2, with no 1/z terms. The shader itself is pinned by the
// `tile_binning_orthographic_jacobian` contract in shaders/compile_shaders.py;
// these cases pin what that branch assumes about the matrices the renderer
// actually uploads (build_render_projection(): flip_y and TAA jitter applied):
//   1. the [2][3] test separates every engine perspective/frustum matrix from
//      every engine orthographic one, after flip and jitter;
//   2. for an orthographic matrix the true screen-space derivative IS
//      diag(focal_x, focal_y) at every depth -- measured by finite differences
//      of the full projection -- whereas the perspective Jacobian the shader
//      used before scaled it by 1/depth;
//   3. a KEEP_HEIGHT ortho size above the viewport height gives |focal_y| < 1,
//      inside the perspective focal band's reject range, which is why the
//      orthographic branch needs its own range check.
// The rendered footprint itself needs the GPU oracle described in #1156.
static Vector2 gs_test_screen_px(const Projection &p_projection, const Vector3 &p_view_pos, const Vector2 &p_viewport) {
	const Vector3 ndc = gs_test_ndc(p_projection, p_view_pos);
	return Vector2((ndc.x * 0.5f + 0.5f) * p_viewport.x, (ndc.y * 0.5f + 0.5f) * p_viewport.y);
}

static bool gs_test_tile_binning_is_ortho(const Projection &p_projection) {
	// Mirrors `abs(params.projection_matrix[2][3]) < 0.5` in tile_binning.glsl.
	return Math::abs(p_projection.columns[2][3]) < real_t(0.5);
}

TEST_CASE("[GaussianSplatting][ViewTransform] Orthographic projections reach tile binning with a depth-free Jacobian (#1156)") {
	const Vector2 viewport(1920.0f, 1080.0f);
	const Vector2 jitter(0.5f / 1920.0f, -0.25f / 1080.0f);

	SUBCASE("the [2][3] test separates perspective from orthographic after flip and jitter") {
		Projection perspective;
		perspective.set_perspective(70.0f, viewport.x / viewport.y, 0.05f, 500.0f);
		Projection off_axis;
		off_axis.set_frustum(2.0f, viewport.x / viewport.y, Vector2(0.2f, 0.3f), 0.05f, 500.0f);
		Projection ortho;
		ortho.set_orthogonal(20.0f, viewport.x / viewport.y, 0.05f, 500.0f, false);
		Projection shifted_ortho;
		shifted_ortho.set_orthogonal(-1.0f, 3.0f, -0.5f, 1.5f, 0.05f, 500.0f);

		for (const bool flip : { false, true }) {
			for (const Vector2 &j : { Vector2(), jitter }) {
				CHECK_FALSE(gs_test_tile_binning_is_ortho(GaussianSplatRenderer::build_render_projection(perspective, flip, j)));
				CHECK_FALSE(gs_test_tile_binning_is_ortho(GaussianSplatRenderer::build_render_projection(off_axis, flip, j)));
				CHECK(gs_test_tile_binning_is_ortho(GaussianSplatRenderer::build_render_projection(ortho, flip, j)));
				CHECK(gs_test_tile_binning_is_ortho(GaussianSplatRenderer::build_render_projection(shifted_ortho, flip, j)));
			}
		}
	}

	SUBCASE("the orthographic screen derivative is diag(focal_x, focal_y) at every depth") {
		Projection ortho;
		ortho.set_orthogonal(20.0f, viewport.x / viewport.y, 0.05f, 500.0f, false);
		const Projection uploaded = GaussianSplatRenderer::build_render_projection(ortho, true, jitter);
		REQUIRE(gs_test_tile_binning_is_ortho(uploaded));

		const double focal_x = double(uploaded.columns[0][0]) * viewport.x * 0.5;
		const double focal_y = double(uploaded.columns[1][1]) * viewport.y * 0.5;
		// World units. The orthographic map is affine, so the difference quotient
		// is exact for any h; a full unit keeps float rounding of ~1000 px screen
		// coordinates well below the tolerances.
		const real_t h = 1.0f;
		for (const real_t depth : { real_t(2.0), real_t(20.0), real_t(200.0) }) {
			const Vector3 p(1.5f, -2.0f, -depth);
			const Vector2 s0 = gs_test_screen_px(uploaded, p, viewport);
			const Vector2 sx = gs_test_screen_px(uploaded, p + Vector3(h, 0.0f, 0.0f), viewport);
			const Vector2 sy = gs_test_screen_px(uploaded, p + Vector3(0.0f, h, 0.0f), viewport);
			const Vector2 sz = gs_test_screen_px(uploaded, p + Vector3(0.0f, 0.0f, -h), viewport);
			// d(screen)/d(view): row 0 = (focal_x, 0, 0), row 1 = (0, focal_y, 0).
			CHECK((sx.x - s0.x) / h == doctest::Approx(focal_x).epsilon(1e-3));
			CHECK((sy.y - s0.y) / h == doctest::Approx(focal_y).epsilon(1e-3));
			CHECK(Math::abs((sy.x - s0.x) / h) < 1e-2);
			CHECK(Math::abs((sx.y - s0.y) / h) < 1e-2);
			CHECK(Math::abs((sz.x - s0.x) / h) < 1e-2);
			CHECK(Math::abs((sz.y - s0.y) / h) < 1e-2);
			// The same derivative at all three depths is the point: the
			// pre-#1156 perspective Jacobian scaled this diagonal by 1/depth
			// (20x per axis, 400x in footprint area, at depth 20).
		}
	}

	SUBCASE("an ortho size above the viewport height leaves the perspective focal band") {
		// Camera3D KEEP_HEIGHT: set_orthogonal(size, aspect, near, far, false)
		// makes the vertical extent `size`, so focal_y = viewport.y / size.
		Projection large;
		large.set_orthogonal(2000.0f, viewport.x / viewport.y, 0.05f, 5000.0f, false);
		const Projection uploaded = GaussianSplatRenderer::build_render_projection(large, true, Vector2());
		REQUIRE(gs_test_tile_binning_is_ortho(uploaded));
		const double focal_y = double(uploaded.columns[1][1]) * viewport.y * 0.5;
		CHECK(Math::abs(focal_y) == doctest::Approx(viewport.y / 2000.0));
		// The perspective band rejects |focal| < 1; this valid camera is below it.
		CHECK(Math::abs(focal_y) < 1.0);
		CHECK(Math::abs(focal_y) > 0.0);
	}
}

} // namespace TestGaussianSplatting
