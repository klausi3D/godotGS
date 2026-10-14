/**************************************************************************/
/*  test_sh_rotation.h                                                    */
/**************************************************************************/

#pragma once

// #1171: gaussian_splat_merge_sources() baked each source's rotation into the
// splat quaternions and normals but copied SH bands 1-3 unrotated, so a merged
// world with a rotated source answered view-dependent colour to the wrong
// direction. These cases pin the SH rotation math (core/gaussian_sh_rotation.h)
// and the merge's use of it.

#include "test_macros.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_sh_rotation.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_merge_utils.h"
#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/math/quaternion.h"

namespace TestSHRotation {

// Deterministic generator (no engine RNG state shared with other cases).
struct Lcg {
	uint64_t state;
	explicit Lcg(uint64_t p_seed) :
			state(p_seed) {}
	double next_signed() {
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return double((state >> 11) & ((1ull << 53) - 1)) / double(1ull << 53) * 2.0 - 1.0;
	}
	void next_unit(double r_dir[3]) {
		double n = 0.0;
		do {
			r_dir[0] = next_signed();
			r_dir[1] = next_signed();
			r_dir[2] = next_signed();
			n = r_dir[0] * r_dir[0] + r_dir[1] * r_dir[1] + r_dir[2] * r_dir[2];
		} while (n < 1e-3 || n > 1.0);
		n = Math::sqrt(n);
		for (int i = 0; i < 3; i++) {
			r_dir[i] /= n;
		}
	}
};

static void _basis_to_rows(const Basis &p_basis, double r_rows[3][3]) {
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r_rows[i][j] = double(p_basis.rows[i][j]);
		}
	}
}

// R^T d for row-major R.
static void _inverse_rotate(const double p_r[3][3], const double p_d[3], double r_out[3]) {
	for (int i = 0; i < 3; i++) {
		r_out[i] = p_r[0][i] * p_d[0] + p_r[1][i] * p_d[1] + p_r[2][i] * p_d[2];
	}
}

// Degree-3 splat payload: band 1 in Gaussian::sh_1, bands 2-3 in the sidecar.
static Ref<GaussianSplatAsset> _make_degree3_asset(uint32_t p_splats, uint64_t p_seed) {
	Lcg rng(p_seed);
	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_splats);
	LocalVector<Vector3> high;
	high.resize(p_splats * 12u);
	for (uint32_t i = 0; i < p_splats; i++) {
		Gaussian &g = gaussians[i];
		g.position = Vector3(float(i), 0.5f * float(i), -0.25f * float(i));
		g.scale = Vector3(0.1f, 0.2f, 0.3f);
		g.rotation = Quaternion();
		g.opacity = 0.8f;
		g.area = 1.0f;
		g.normal = Vector3(0, 1, 0);
		g.brush_axes = Vector2(1, 1);
		g.sh_dc = Color(float(rng.next_signed()), float(rng.next_signed()), float(rng.next_signed()), 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		for (int t = 0; t < 3; t++) {
			g.sh_1[t] = Vector3(float(rng.next_signed()), float(rng.next_signed()), float(rng.next_signed()));
		}
		for (int t = 0; t < 12; t++) {
			high[i * 12u + uint32_t(t)] = Vector3(float(rng.next_signed()), float(rng.next_signed()), float(rng.next_signed()));
		}
	}
	Ref<::GaussianData> data;
	data.instantiate();
	data->set_gaussian_payload(gaussians, high, 3u, 12u, false);
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	if (asset->populate_from_gaussian_data(data) != OK) {
		return Ref<GaussianSplatAsset>();
	}
	return asset;
}

} // namespace TestSHRotation

TEST_CASE("[GaussianSplatting][SHEncoding] SH rotation by identity is a no-op (#1171)") {
	using namespace GaussianSHRotation;
	const double identity[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	Rotation rotation;
	CHECK(make_rotation(identity, rotation));
	CHECK(rotation.identity);

	TestSHRotation::Lcg rng(7);
	float coeffs[MAX_TERMS * 3];
	float original[MAX_TERMS * 3];
	for (int i = 0; i < MAX_TERMS * 3; i++) {
		coeffs[i] = original[i] = float(rng.next_signed());
	}
	CHECK(rotate_terms(rotation, coeffs, MAX_TERMS));
	for (int i = 0; i < MAX_TERMS * 3; i++) {
		CHECK_EQ(coeffs[i], original[i]);
	}
}

TEST_CASE("[GaussianSplatting][SHEncoding] Rotated SH evaluated at d equals the original at R^-1 d (#1171)") {
	using namespace GaussianSHRotation;
	TestSHRotation::Lcg rng(0x5eedULL);
	double worst = 0.0;
	for (int trial = 0; trial < 24; trial++) {
		double axis[3];
		rng.next_unit(axis);
		const real_t angle = real_t(Math::PI * rng.next_signed());
		const Vector3 axis_vector = Vector3(real_t(axis[0]), real_t(axis[1]), real_t(axis[2])).normalized();
		const Basis basis = Basis(Quaternion(axis_vector, angle));
		double r[3][3];
		TestSHRotation::_basis_to_rows(basis, r);
		Rotation rotation;
		if (!make_rotation(r, rotation)) {
			FAIL("make_rotation rejected a proper rotation");
			return;
		}

		float original[MAX_TERMS * 3];
		float rotated[MAX_TERMS * 3];
		for (int i = 0; i < MAX_TERMS * 3; i++) {
			original[i] = rotated[i] = float(rng.next_signed());
		}
		CHECK(rotate_terms(rotation, rotated, MAX_TERMS));
		// DC is rotation-invariant and must not be touched.
		CHECK_EQ(rotated[0], original[0]);
		CHECK_EQ(rotated[1], original[1]);
		CHECK_EQ(rotated[2], original[2]);

		for (int sample = 0; sample < 32; sample++) {
			double d[3];
			rng.next_unit(d);
			double local[3];
			TestSHRotation::_inverse_rotate(r, d, local);
			for (int channel = 0; channel < 3; channel++) {
				const double got = evaluate(rotated, MAX_TERMS, channel, d[0], d[1], d[2]);
				const double expected = evaluate(original, MAX_TERMS, channel, local[0], local[1], local[2]);
				worst = MAX(worst, Math::abs(got - expected));
			}
		}
	}
	// Coefficients are O(1); the only error is fp32 storage of the result.
	CHECK_MESSAGE(worst < 1e-5, vformat("max |f'(d) - f(R^-1 d)| = %f", worst));
}

TEST_CASE("[GaussianSplatting][SHEncoding] The CPU SH basis mirrors the shader band-1 convention (#1171)") {
	// gs_sh_binning.glsl: basis[1..3] = C1 * (-y, z, -x). Pin it so the CPU mirror
	// the rotation is built from cannot drift from the renderer silently.
	double basis[GaussianSHRotation::MAX_TERMS];
	GaussianSHRotation::eval_basis(0.0, 0.0, 1.0, basis);
	CHECK(Math::is_equal_approx(basis[0], 0.28209479177387814));
	CHECK(Math::is_equal_approx(basis[2], 0.4886025119029199));
	CHECK(Math::is_zero_approx(basis[1]));
	CHECK(Math::is_zero_approx(basis[3]));
	GaussianSHRotation::eval_basis(1.0, 0.0, 0.0, basis);
	CHECK(Math::is_equal_approx(basis[3], -0.4886025119029199));
	GaussianSHRotation::eval_basis(0.0, 1.0, 0.0, basis);
	CHECK(Math::is_equal_approx(basis[1], -0.4886025119029199));
}

TEST_CASE("[GaussianSplatting][SHEncoding] A merged world rotates each source's SH bands with the source (#1171)") {
	using namespace GaussianSHRotation;
	const uint32_t splats = 4;
	Ref<GaussianSplatAsset> still = TestSHRotation::_make_degree3_asset(splats, 11);
	Ref<GaussianSplatAsset> turned = TestSHRotation::_make_degree3_asset(splats, 23);
	if (still.is_null() || turned.is_null()) {
		FAIL("could not build the degree-3 source assets");
		return;
	}
	const PackedFloat32Array still_sh = still->get_spherical_harmonics_buffer();
	const PackedFloat32Array turned_sh = turned->get_spherical_harmonics_buffer();
	if (still_sh.size() != int(splats) * 48 || turned_sh.size() != int(splats) * 48) {
		FAIL("source assets must carry 16 SH terms per splat, got ", still_sh.size(), " and ", turned_sh.size());
		return;
	}

	// 90 degrees about +Y, plus a translation (which must not affect SH).
	const Quaternion quarter_turn(Vector3(0, 1, 0), real_t(Math::PI * 0.5));
	Vector<GaussianSplatMergeSource> sources;
	GaussianSplatMergeSource a;
	a.asset = still;
	a.transform = Transform3D();
	sources.push_back(a);
	GaussianSplatMergeSource b;
	b.asset = turned;
	b.transform = Transform3D(Basis(quarter_turn), Vector3(5, 0, 0));
	sources.push_back(b);

	GaussianSplatMergeResult out;
	if (!gaussian_splat_merge_sources(sources, 10.0f, out) || out.data.is_null() ||
			out.data->get_count() != int(splats * 2)) {
		FAIL("merge of two degree-3 sources must succeed with 8 splats");
		return;
	}

	double r[3][3];
	TestSHRotation::_basis_to_rows(Basis(quarter_turn), r);
	Rotation rotation;
	if (!make_rotation(r, rotation) || rotation.identity) {
		FAIL("the quarter turn must build a non-identity SH rotation");
		return;
	}

	TestSHRotation::Lcg rng(99);
	for (uint32_t i = 0; i < splats; i++) {
		// Identity source: copied unchanged.
		const PackedFloat32Array merged_still = out.data->get_spherical_harmonics(int(i));
		// Rotated source: equals the per-splat rotation of the same coefficients.
		const PackedFloat32Array merged_turned = out.data->get_spherical_harmonics(int(splats + i));
		if (merged_still.size() != 48 || merged_turned.size() != 48) {
			FAIL("merged splat ", i, " lost SH terms: ", merged_still.size(), " / ", merged_turned.size());
			return;
		}
		float expected[48];
		float source[48];
		for (int k = 0; k < 48; k++) {
			source[k] = turned_sh[int(i) * 48 + k];
			expected[k] = source[k];
			CHECK_EQ(merged_still[k], still_sh[int(i) * 48 + k]);
		}
		rotate_terms(rotation, expected, 16);
		for (int k = 0; k < 48; k++) {
			CHECK_MESSAGE(Math::abs(merged_turned[k] - expected[k]) <= 1e-5f,
					vformat("splat %d float %d: merged %f expected %f", i, k, merged_turned[k], expected[k]));
		}
		// Independent of rotate_terms: the merged colour seen from world
		// direction d is the source colour seen from R^-1 d, which is what the
		// unmerged rotated node renders.
		float merged_terms[48];
		for (int k = 0; k < 48; k++) {
			merged_terms[k] = merged_turned[k];
		}
		for (int sample = 0; sample < 8; sample++) {
			double d[3];
			rng.next_unit(d);
			double local[3];
			TestSHRotation::_inverse_rotate(r, d, local);
			for (int channel = 0; channel < 3; channel++) {
				const double got = evaluate(merged_terms, 16, channel, d[0], d[1], d[2]);
				const double want = evaluate(source, 16, channel, local[0], local[1], local[2]);
				CHECK_MESSAGE(Math::abs(got - want) < 1e-4,
						vformat("splat %d channel %d: merged %f, unmerged %f", i, channel, got, want));
			}
		}
	}
}
