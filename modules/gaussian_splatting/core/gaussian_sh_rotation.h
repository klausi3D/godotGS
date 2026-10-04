/**************************************************************************/
/*  gaussian_sh_rotation.h                                                */
/**************************************************************************/

#pragma once

// Rotation of real spherical-harmonic coefficients (bands 1-3) for baking a
// rigid rotation into splat colour (#1171).
//
// Convention. Coefficients are evaluated against EXACTLY the basis of
// compute_sh_basis() in shaders/includes/gs_sh_binning.glsl (real SH with the
// Condon-Shortley phase, band 1 = C1 * (-y, z, -x)); eval_basis() below is its
// CPU mirror and the only basis this file knows about. A colour field
//     f(d) = sum_i c_i * Y_i(d)
// rotated by R (a direction d_local maps to R * d_local) is
//     f'(d) = f(R^T d) = sum_i c'_i * Y_i(d),
// which is what the renderer evaluates for an unmerged node: it rotates the
// world view direction into the asset frame with the instance's inverse
// rotation before evaluating the asset's coefficients (tile_binning.glsl,
// view_dir_local). Baking R into the coefficients gives the merged, identity-
// transformed copy the same colour for every view direction.
//
// Construction. Rotations map each band onto itself, so for band l there is a
// (2l+1) x (2l+1) matrix M_l with y_l(R^T d) = M_l y_l(d), and c'_l = M_l^T c_l.
// M_l is recovered exactly (up to round-off) by least squares over a fixed,
// well-spread direction set: with A[k][i] = Y_i(d_k) and B[k][i] = Y_i(R^T d_k),
// M_l^T = (A^T A)^{-1} A^T B. Building it from the evaluator, rather than from
// hand-transcribed Wigner/Ivanic-Ruedenberg recurrences, makes it consistent
// with the shader's sign and ordering conventions by construction.
//
// Header-only and free of engine types so it can be unit-tested in isolation.

#include <cmath>
#include <cstdint>

namespace GaussianSHRotation {

static constexpr int MAX_TERMS = 16; // degree 3: 1 + 3 + 5 + 7

// Same constants as gs_sh_binning.glsl.
static constexpr double SH_C0 = 0.28209479177387814;
static constexpr double SH_C1 = 0.4886025119029199;
static constexpr double SH_C2_0 = 1.0925484305920792;
static constexpr double SH_C2_1 = 0.31539156525252005;
static constexpr double SH_C2_2 = 0.5462742152960396;
static constexpr double SH_C3_0 = 0.5900435899266435;
static constexpr double SH_C3_1 = 2.890611442640554;
static constexpr double SH_C3_2 = 0.4570457994644658;
static constexpr double SH_C3_3 = 0.3731763325901154;
static constexpr double SH_C3_5 = 1.445305721320277; // Inria SH_C3[5]; SH_C3_1 / 2 (#1157)

// CPU mirror of compute_sh_basis() (gs_sh_binning.glsl), all 16 terms, for a
// unit direction (x, y, z). Keep in lockstep with the shader: the rotation is
// derived from this function, so it stays consistent with whatever basis the
// renderer evaluates. Band 3 terms 11, 13 and 14 are in the Inria
// computeColorFromSH form since #1157 (SH_C3[2] = SH_C3[4] = -SH_C3_2,
// SH_C3[5] = SH_C3_5); the rotation is derived from this evaluator, so it
// follows the shader whenever the two are kept in lockstep.
inline void eval_basis(double x, double y, double z, double r_basis[MAX_TERMS]) {
	const double xx = x * x;
	const double yy = y * y;
	const double zz = z * z;
	r_basis[0] = SH_C0;
	r_basis[1] = -SH_C1 * y;
	r_basis[2] = SH_C1 * z;
	r_basis[3] = -SH_C1 * x;
	r_basis[4] = SH_C2_0 * x * y;
	r_basis[5] = -SH_C2_0 * y * z;
	r_basis[6] = SH_C2_1 * (3.0 * zz - 1.0);
	r_basis[7] = -SH_C2_0 * x * z;
	r_basis[8] = SH_C2_2 * (xx - yy);
	r_basis[9] = -SH_C3_0 * y * (3.0 * xx - yy);
	r_basis[10] = SH_C3_1 * x * y * z;
	r_basis[11] = -SH_C3_2 * y * (4.0 * zz - xx - yy);
	r_basis[12] = SH_C3_3 * z * (5.0 * zz - 3.0);
	r_basis[13] = -SH_C3_2 * x * (4.0 * zz - xx - yy);
	r_basis[14] = SH_C3_5 * z * (xx - yy);
	r_basis[15] = -SH_C3_0 * x * (xx - 3.0 * yy);
}

// Per-band coefficient transforms: c'_band = band_matrix * c_band.
struct Rotation {
	bool identity = true;
	double band1[3][3] = {};
	double band2[5][5] = {};
	double band3[7][7] = {};
};

namespace detail {

static constexpr int SAMPLE_COUNT = 64;

// Fibonacci-sphere direction k of SAMPLE_COUNT (deterministic, well spread, and
// generic enough that A^T A is well conditioned for every band up to 3).
inline void sample_direction(int k, double r_dir[3]) {
	const double golden_angle = 2.39996322972865332;
	const double z = 1.0 - (2.0 * double(k) + 1.0) / double(SAMPLE_COUNT);
	const double radius = std::sqrt(1.0 > z * z ? 1.0 - z * z : 0.0);
	const double phi = golden_angle * double(k);
	r_dir[0] = radius * std::cos(phi);
	r_dir[1] = radius * std::sin(phi);
	r_dir[2] = z;
}

// Solves G * X = H for X (n x n, n <= 7) by Gauss-Jordan with partial pivoting.
// G and H are consumed. Returns false on a (numerically) singular G.
inline bool solve(int n, double G[7][7], double H[7][7], double X[7][7]) {
	for (int col = 0; col < n; col++) {
		int pivot = col;
		for (int row = col + 1; row < n; row++) {
			if (std::fabs(G[row][col]) > std::fabs(G[pivot][col])) {
				pivot = row;
			}
		}
		if (std::fabs(G[pivot][col]) < 1e-12) {
			return false;
		}
		if (pivot != col) {
			for (int j = 0; j < n; j++) {
				const double tg = G[col][j];
				G[col][j] = G[pivot][j];
				G[pivot][j] = tg;
				const double th = H[col][j];
				H[col][j] = H[pivot][j];
				H[pivot][j] = th;
			}
		}
		const double inv = 1.0 / G[col][col];
		for (int j = 0; j < n; j++) {
			G[col][j] *= inv;
			H[col][j] *= inv;
		}
		for (int row = 0; row < n; row++) {
			if (row == col) {
				continue;
			}
			const double factor = G[row][col];
			if (factor == 0.0) {
				continue;
			}
			for (int j = 0; j < n; j++) {
				G[row][j] -= factor * G[col][j];
				H[row][j] -= factor * H[col][j];
			}
		}
	}
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			X[i][j] = H[i][j];
		}
	}
	return true;
}

// Builds the band matrix T (n x n, terms [offset, offset + n)) with
// c'_band = T * c_band, i.e. T = M^T = (A^T A)^{-1} A^T B.
inline bool build_band(const double p_r[3][3], int p_offset, int p_n, double r_t[7][7]) {
	double G[7][7] = {};
	double H[7][7] = {};
	for (int k = 0; k < SAMPLE_COUNT; k++) {
		double d[3];
		sample_direction(k, d);
		// R^T d: the local direction whose colour lands on world direction d.
		const double lx = p_r[0][0] * d[0] + p_r[1][0] * d[1] + p_r[2][0] * d[2];
		const double ly = p_r[0][1] * d[0] + p_r[1][1] * d[1] + p_r[2][1] * d[2];
		const double lz = p_r[0][2] * d[0] + p_r[1][2] * d[1] + p_r[2][2] * d[2];
		double a[MAX_TERMS];
		double b[MAX_TERMS];
		eval_basis(d[0], d[1], d[2], a);
		eval_basis(lx, ly, lz, b);
		for (int i = 0; i < p_n; i++) {
			for (int j = 0; j < p_n; j++) {
				G[i][j] += a[p_offset + i] * a[p_offset + j];
				H[i][j] += a[p_offset + i] * b[p_offset + j];
			}
		}
	}
	return solve(p_n, G, H, r_t);
}

} // namespace detail

// True when p_r is the identity to within p_epsilon per element.
inline bool is_identity(const double p_r[3][3], double p_epsilon = 1e-6) {
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			const double expected = (i == j) ? 1.0 : 0.0;
			if (std::fabs(p_r[i][j] - expected) > p_epsilon) {
				return false;
			}
		}
	}
	return true;
}

// Builds the coefficient rotation for the proper rotation matrix p_r
// (row-major, d_rotated = p_r * d). Returns false if p_r is not a proper
// rotation (non-orthonormal or a reflection), in which case r_out is identity.
inline bool make_rotation(const double p_r[3][3], Rotation &r_out) {
	r_out = Rotation();
	// Orthonormality and det = +1: SH rotation is only defined for SO(3).
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			double dot = 0.0;
			for (int k = 0; k < 3; k++) {
				dot += p_r[i][k] * p_r[j][k];
			}
			if (std::fabs(dot - (i == j ? 1.0 : 0.0)) > 1e-4) {
				return false;
			}
		}
	}
	const double det = p_r[0][0] * (p_r[1][1] * p_r[2][2] - p_r[1][2] * p_r[2][1]) -
			p_r[0][1] * (p_r[1][0] * p_r[2][2] - p_r[1][2] * p_r[2][0]) +
			p_r[0][2] * (p_r[1][0] * p_r[2][1] - p_r[1][1] * p_r[2][0]);
	if (det < 0.0) {
		return false;
	}
	if (is_identity(p_r)) {
		return true;
	}
	double t1[7][7] = {};
	double t2[7][7] = {};
	double t3[7][7] = {};
	if (!detail::build_band(p_r, 1, 3, t1) || !detail::build_band(p_r, 4, 5, t2) || !detail::build_band(p_r, 9, 7, t3)) {
		return false;
	}
	for (int i = 0; i < 7; i++) {
		for (int j = 0; j < 7; j++) {
			if (i < 3 && j < 3) {
				r_out.band1[i][j] = t1[i][j];
			}
			if (i < 5 && j < 5) {
				r_out.band2[i][j] = t2[i][j];
			}
			r_out.band3[i][j] = t3[i][j];
		}
	}
	r_out.identity = false;
	return true;
}

namespace detail {

template <int N>
inline void rotate_band(const double (&p_t)[N][N], float *p_terms_rgb, int p_offset) {
	for (int channel = 0; channel < 3; channel++) {
		double in[N];
		for (int j = 0; j < N; j++) {
			in[j] = double(p_terms_rgb[(p_offset + j) * 3 + channel]);
		}
		for (int i = 0; i < N; i++) {
			double sum = 0.0;
			for (int j = 0; j < N; j++) {
				sum += p_t[i][j] * in[j];
			}
			p_terms_rgb[(p_offset + i) * 3 + channel] = float(sum);
		}
	}
}

} // namespace detail

// Rotates one splat's coefficients in place. p_terms_rgb holds p_term_count
// RGB triplets in basis order (term 0 = DC, untouched; terms 1..15 = bands 1-3),
// the layout of GaussianSplatAsset::get_spherical_harmonics_buffer().
// Only complete bands are rotated. Returns false when a band is present only
// partially (term_count not of the form (L+1)^2); that band's terms are left
// unrotated because a rotation mixes all 2l+1 terms of a band.
inline bool rotate_terms(const Rotation &p_rotation, float *p_terms_rgb, uint32_t p_term_count) {
	if (p_rotation.identity || p_terms_rgb == nullptr) {
		return true;
	}
	const uint32_t terms = p_term_count > uint32_t(MAX_TERMS) ? uint32_t(MAX_TERMS) : p_term_count;
	bool complete = true;
	if (terms >= 4u) {
		detail::rotate_band<3>(p_rotation.band1, p_terms_rgb, 1);
	} else if (terms > 1u) {
		complete = false;
	}
	if (terms >= 9u) {
		detail::rotate_band<5>(p_rotation.band2, p_terms_rgb, 4);
	} else if (terms > 4u) {
		complete = false;
	}
	if (terms >= 16u) {
		detail::rotate_band<7>(p_rotation.band3, p_terms_rgb, 9);
	} else if (terms > 9u) {
		complete = false;
	}
	return complete;
}

// Evaluates sum_i c_i Y_i(d) for one channel of p_terms_rgb (test/diagnostic aid).
inline double evaluate(const float *p_terms_rgb, uint32_t p_term_count, int p_channel, double x, double y, double z) {
	double basis[MAX_TERMS];
	eval_basis(x, y, z, basis);
	const uint32_t terms = p_term_count > uint32_t(MAX_TERMS) ? uint32_t(MAX_TERMS) : p_term_count;
	double sum = 0.0;
	for (uint32_t i = 0; i < terms; i++) {
		sum += double(p_terms_rgb[i * 3 + uint32_t(p_channel)]) * basis[i];
	}
	return sum;
}

} // namespace GaussianSHRotation
