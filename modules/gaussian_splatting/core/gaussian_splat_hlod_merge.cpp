#include "gaussian_splat_hlod_merge.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace gs_hlod {

namespace {

static constexpr double kTwoPi = 6.283185307179586;
static constexpr double kMinEigen = 1e-14; // floor of covariance eigenvalues (as the prototype)
static constexpr double kWeightEpsilon = 1e-20;
static constexpr uint32_t kCellCoordBits = 21u;
static constexpr int64_t kCellCoordLimit = int64_t(1) << kCellCoordBits;

inline float clamp01(float p_v) {
	return p_v < 0.0f ? 0.0f : (p_v > 1.0f ? 1.0f : p_v);
}

inline void sorted_sigmas(const Gaussian &p_g, float &r_max, float &r_mid) {
	float a = Math::abs(p_g.scale.x);
	float b = Math::abs(p_g.scale.y);
	float c = Math::abs(p_g.scale.z);
	if (a < b) {
		SWAP(a, b);
	}
	if (b < c) {
		SWAP(b, c);
	}
	if (a < b) {
		SWAP(a, b);
	}
	r_max = a;
	r_mid = b;
}

// Rotation matrix of a quaternion (normalized first), row-major R[row][col]; the standard
// 3DGS convention Sigma = R diag(s^2) R^T, as gsply.quat_to_mat of the ADR prototype.
inline void quat_to_matrix(const Quaternion &p_q, double r_m[3][3]) {
	double x = p_q.x, y = p_q.y, z = p_q.z, w = p_q.w;
	const double n = std::sqrt(x * x + y * y + z * z + w * w);
	if (n > 0.0) {
		x /= n;
		y /= n;
		z /= n;
		w /= n;
	} else {
		x = 0.0;
		y = 0.0;
		z = 0.0;
		w = 1.0;
	}
	r_m[0][0] = 1.0 - 2.0 * (y * y + z * z);
	r_m[0][1] = 2.0 * (x * y - w * z);
	r_m[0][2] = 2.0 * (x * z + w * y);
	r_m[1][0] = 2.0 * (x * y + w * z);
	r_m[1][1] = 1.0 - 2.0 * (x * x + z * z);
	r_m[1][2] = 2.0 * (y * z - w * x);
	r_m[2][0] = 2.0 * (x * z - w * y);
	r_m[2][1] = 2.0 * (y * z + w * x);
	r_m[2][2] = 1.0 - 2.0 * (x * x + y * y);
}

// Cyclic Jacobi eigen-decomposition of a symmetric 3x3 matrix. Eigenvalues ascending; eigenvector
// k is column k of r_vec. Deterministic (fixed sweep order).
void eigen_symmetric3(const double p_packed[6], double r_val[3], double r_vec[3][3]) {
	double a[3][3] = {
		{ p_packed[0], p_packed[1], p_packed[2] },
		{ p_packed[1], p_packed[3], p_packed[4] },
		{ p_packed[2], p_packed[4], p_packed[5] },
	};
	double v[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
	static const int pairs[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };
	for (int sweep = 0; sweep < 64; sweep++) {
		const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
		const double diag = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
		if (off == 0.0 || off <= 1e-30 * diag) {
			break;
		}
		for (int k = 0; k < 3; k++) {
			const int p = pairs[k][0];
			const int q = pairs[k][1];
			const double apq = a[p][q];
			if (apq == 0.0) {
				continue;
			}
			const double theta = (a[q][q] - a[p][p]) / (2.0 * apq);
			const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
			const double c = 1.0 / std::sqrt(t * t + 1.0);
			const double s = t * c;
			// A' = J^T A J with J the Givens rotation in the (p, q) plane.
			for (int r = 0; r < 3; r++) {
				const double arp = a[r][p];
				const double arq = a[r][q];
				a[r][p] = c * arp - s * arq;
				a[r][q] = s * arp + c * arq;
			}
			for (int r = 0; r < 3; r++) {
				const double apr = a[p][r];
				const double aqr = a[q][r];
				a[p][r] = c * apr - s * aqr;
				a[q][r] = s * apr + c * aqr;
			}
			for (int r = 0; r < 3; r++) {
				const double vrp = v[r][p];
				const double vrq = v[r][q];
				v[r][p] = c * vrp - s * vrq;
				v[r][q] = s * vrp + c * vrq;
			}
		}
	}
	int order[3] = { 0, 1, 2 };
	const double vals[3] = { a[0][0], a[1][1], a[2][2] };
	// Ascending, ties by index (stable).
	for (int i = 1; i < 3; i++) {
		for (int j = i; j > 0 && vals[order[j]] < vals[order[j - 1]]; j--) {
			SWAP(order[j], order[j - 1]);
		}
	}
	for (int k = 0; k < 3; k++) {
		r_val[k] = vals[order[k]];
		for (int r = 0; r < 3; r++) {
			r_vec[r][k] = v[r][order[k]];
		}
	}
}

// Shepperd's method, as gsply.mat_to_quat; p_m is a proper rotation (columns = axes).
Quaternion matrix_to_quat(const double p_m[3][3]) {
	double w, x, y, z;
	const double tr = p_m[0][0] + p_m[1][1] + p_m[2][2];
	if (tr > 0.0) {
		const double s = std::sqrt(MAX(tr + 1.0, 1e-12)) * 2.0;
		w = 0.25 * s;
		x = (p_m[2][1] - p_m[1][2]) / s;
		y = (p_m[0][2] - p_m[2][0]) / s;
		z = (p_m[1][0] - p_m[0][1]) / s;
	} else if (p_m[0][0] > p_m[1][1] && p_m[0][0] > p_m[2][2]) {
		const double s = std::sqrt(MAX(1.0 + p_m[0][0] - p_m[1][1] - p_m[2][2], 1e-12)) * 2.0;
		w = (p_m[2][1] - p_m[1][2]) / s;
		x = 0.25 * s;
		y = (p_m[0][1] + p_m[1][0]) / s;
		z = (p_m[0][2] + p_m[2][0]) / s;
	} else if (p_m[1][1] > p_m[2][2]) {
		const double s = std::sqrt(MAX(1.0 + p_m[1][1] - p_m[0][0] - p_m[2][2], 1e-12)) * 2.0;
		w = (p_m[0][2] - p_m[2][0]) / s;
		x = (p_m[0][1] + p_m[1][0]) / s;
		y = 0.25 * s;
		z = (p_m[1][2] + p_m[2][1]) / s;
	} else {
		const double s = std::sqrt(MAX(1.0 + p_m[2][2] - p_m[0][0] - p_m[1][1], 1e-12)) * 2.0;
		w = (p_m[1][0] - p_m[0][1]) / s;
		x = (p_m[0][2] + p_m[2][0]) / s;
		y = (p_m[1][2] + p_m[2][1]) / s;
		z = 0.25 * s;
	}
	const double n = std::sqrt(w * w + x * x + y * y + z * z);
	return Quaternion(real_t(x / n), real_t(y / n), real_t(z / n), real_t(w / n));
}


// Buckets the non-pass-through splats of p_rep on the eps grid. Fills r_entries sorted by
// (cell, index) and r_pass_through (1 = passes through). Returns ERR_PARAMETER_RANGE_ERROR when the
// grid is too fine for the 21-bit-per-axis cell key (the caller's eps is far below the content
// extent) and ERR_OUT_OF_MEMORY when the scratch cannot grow (entries then empty).
Error bucket_cells(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch) {
	LocalVector<MergeCellEntry> &r_entries = r_scratch.entries;
	LocalVector<uint8_t> &r_pass_through = r_scratch.pass_through;
	LocalVector<int64_t> &cells = r_scratch.cells;
	r_entries.clear();
	if (!fallible_resize(r_pass_through, p_rep.count, "gs_hlod::bucket_cells pass_through")) {
		return ERR_OUT_OF_MEMORY;
	}
	if (p_rep.count == 0u) {
		return OK;
	}
	int64_t lo[3] = { INT64_MAX, INT64_MAX, INT64_MAX };
	int64_t hi[3] = { INT64_MIN, INT64_MIN, INT64_MIN };
	if (!fallible_resize(cells, uint64_t(p_rep.count) * 3u, "gs_hlod::bucket_cells cells")) {
		return ERR_OUT_OF_MEMORY;
	}
	uint32_t merged = 0u;
	for (uint32_t i = 0; i < p_rep.count; i++) {
		const Gaussian &g = p_rep.gaussians[i];
		float s_max, s_mid;
		sorted_sigmas(g, s_max, s_mid);
		const bool pass = 2.0 * double(s_max) >= p_eps;
		r_pass_through[i] = pass ? 1u : 0u;
		if (pass) {
			continue;
		}
		merged++;
		const double p[3] = { double(g.position.x), double(g.position.y), double(g.position.z) };
		const double o[3] = { double(p_origin.x), double(p_origin.y), double(p_origin.z) };
		for (int a = 0; a < 3; a++) {
			const double f = std::floor((p[a] - o[a]) / p_eps);
			if (!(f > -9.0e15 && f < 9.0e15)) {
				return ERR_PARAMETER_RANGE_ERROR;
			}
			const int64_t c = int64_t(f);
			cells[i * 3u + uint32_t(a)] = c;
			lo[a] = MIN(lo[a], c);
			hi[a] = MAX(hi[a], c);
		}
	}
	if (merged == 0u) {
		return OK;
	}
	for (int a = 0; a < 3; a++) {
		if (hi[a] - lo[a] >= kCellCoordLimit) {
			return ERR_PARAMETER_RANGE_ERROR;
		}
	}
	if (!fallible_resize(r_entries, merged, "gs_hlod::bucket_cells entries")) {
		return ERR_OUT_OF_MEMORY;
	}
	uint32_t w = 0u;
	for (uint32_t i = 0; i < p_rep.count; i++) {
		if (r_pass_through[i]) {
			continue;
		}
		// Lexicographic (x, y, z) order of the relative cell = the prototype's
		// key (cx * dy + cy) * dz + cz order. Bit 63 is the splat's DC encoding: sh_dc is decoded
		// as dc + 0.5 (LINEAR_RGB) or 1.5 sigmoid(dc) - 0.25 (LEGACY_BIAS, gs_sh_binning.glsl),
		// so averaging across encodings would mix colour spaces; worlds merged from several
		// assets carry both (core/gaussian_splat_merge_utils.cpp). Cells never mix them. With a
		// single encoding the bit is constant and the order is the prototype's.
		const uint64_t cx = uint64_t(cells[i * 3u + 0u] - lo[0]);
		const uint64_t cy = uint64_t(cells[i * 3u + 1u] - lo[1]);
		const uint64_t cz = uint64_t(cells[i * 3u + 2u] - lo[2]);
		const uint64_t encoding = gaussian_get_dc_encoding(p_rep.gaussians[i].render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB ? 1u : 0u;
		r_entries[w].key = (encoding << 63u) | (cx << (2u * kCellCoordBits)) | (cy << kCellCoordBits) | cz;
		r_entries[w].index = i;
		w++;
	}
	std::sort(r_entries.ptr(), r_entries.ptr() + r_entries.size());
	return OK;
}

} // namespace

void splat_covariance(const Gaussian &p_g, double r_cov[6]) {
	double m[3][3];
	quat_to_matrix(p_g.rotation, m);
	const double s2[3] = {
		double(p_g.scale.x) * double(p_g.scale.x),
		double(p_g.scale.y) * double(p_g.scale.y),
		double(p_g.scale.z) * double(p_g.scale.z),
	};
	auto entry = [&](int i, int j) {
		return m[i][0] * s2[0] * m[j][0] + m[i][1] * s2[1] * m[j][1] + m[i][2] * s2[2] * m[j][2];
	};
	r_cov[0] = entry(0, 0);
	r_cov[1] = entry(0, 1);
	r_cov[2] = entry(0, 2);
	r_cov[3] = entry(1, 1);
	r_cov[4] = entry(1, 2);
	r_cov[5] = entry(2, 2);
}

SplatSpan SplatList::span() const {
	SplatSpan s;
	s.gaussians = gaussians.ptr();
	s.sh_high_order = sh_high_order.is_empty() ? nullptr : sh_high_order.ptr();
	s.count = gaussians.size();
	s.sh_high_order_count = sh_high_order_count;
	return s;
}

Error SplatList::append(const SplatSpan &p_src, uint32_t p_first, uint32_t p_count,
		const char *p_where_gaussians, const char *p_where_sh) {
	ERR_FAIL_COND_V(p_src.sh_high_order_count != sh_high_order_count, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(uint64_t(p_first) + uint64_t(p_count) > uint64_t(p_src.count), ERR_INVALID_PARAMETER);
	const uint32_t base = gaussians.size();
	const uint32_t sh_base = sh_high_order.size();
	// A failed SH grow shrinks the Gaussians back, so a failure leaves the list's contents unchanged.
	if (!fallible_resize(gaussians, uint64_t(base) + p_count, p_where_gaussians)) {
		return ERR_OUT_OF_MEMORY;
	}
	if (sh_high_order_count > 0u &&
			!fallible_resize(sh_high_order, uint64_t(sh_base) + uint64_t(p_count) * sh_high_order_count, p_where_sh)) {
		gaussians.resize(base);
		return ERR_OUT_OF_MEMORY;
	}
	if (p_count > 0u) {
		memcpy(gaussians.ptr() + base, p_src.gaussians + p_first, sizeof(Gaussian) * p_count);
	}
	if (sh_high_order_count > 0u) {
		if (p_count > 0u) {
			memcpy(sh_high_order.ptr() + sh_base, p_src.sh_high_order + uint64_t(p_first) * sh_high_order_count,
					sizeof(Vector3) * uint64_t(p_count) * sh_high_order_count);
		}
	}
	return OK;
}

float importance_area(const Gaussian &p_g) {
	float s_max, s_mid;
	sorted_sigmas(p_g, s_max, s_mid);
	return clamp01(p_g.opacity) * s_max * s_mid;
}

bool importance_order(const SplatSpan &p_span, LocalVector<uint32_t> &r_order) {
	LocalVector<float> keys;
	if (!fallible_resize(r_order, p_span.count, "gs_hlod::importance_order order") ||
			!fallible_resize(keys, p_span.count, "gs_hlod::importance_order keys")) {
		r_order.clear();
		return false;
	}
	for (uint32_t i = 0; i < p_span.count; i++) {
		r_order[i] = i;
		keys[i] = importance_area(p_span.gaussians[i]);
	}
	const float *k = keys.ptr();
	std::sort(r_order.ptr(), r_order.ptr() + r_order.size(), [k](uint32_t p_a, uint32_t p_b) {
		return k[p_a] > k[p_b] || (k[p_a] == k[p_b] && p_a < p_b);
	});
	return true;
}

void merge_cell_sat(const SplatSpan &p_rep, const uint32_t *p_members, uint32_t p_member_count,
		Gaussian &r_out, Vector3 *p_out_sh, CellMergeMoments *r_moments) {
	ERR_FAIL_COND(p_member_count == 0u);
	const uint32_t sh_count = p_rep.sh_high_order_count;

	if (p_member_count == 1u) {
		// A cell with a single child is copied unchanged (ADR §6.3, decision 8d): the `sat` formula
		// would map a lone splat's opacity a to 1 - exp(-a), which was a prototype bug.
		const uint32_t idx = p_members[0];
		r_out = p_rep.gaussians[idx];
		if (p_out_sh && sh_count > 0u && p_rep.sh_high_order) {
			memcpy(p_out_sh, p_rep.sh_high_order + uint64_t(idx) * sh_count, sizeof(Vector3) * sh_count);
		}
		if (r_moments) {
			const Gaussian &g = p_rep.gaussians[idx];
			float s_max, s_mid;
			sorted_sigmas(g, s_max, s_mid);
			*r_moments = CellMergeMoments();
			r_moments->weight_sum = double(clamp01(g.opacity)) * s_max * s_mid + kWeightEpsilon;
			r_moments->mean[0] = g.position.x;
			r_moments->mean[1] = g.position.y;
			r_moments->mean[2] = g.position.z;
			splat_covariance(g, r_moments->covariance);
			r_moments->alpha_mass = double(clamp01(g.opacity)) * kTwoPi * s_max * s_mid;
			r_moments->capacity = kTwoPi * double(s_max) * double(s_mid);
			r_moments->inflation = 1.0;
			r_moments->alpha = g.opacity;
		}
		return;
	}

	// Pass 1: weights, weighted mean, heaviest member.
	double weight_sum = 0.0;
	double mean[3] = { 0.0, 0.0, 0.0 };
	double best_w = -1.0;
	uint32_t best = p_members[0];
	for (uint32_t m = 0; m < p_member_count; m++) {
		const Gaussian &g = p_rep.gaussians[p_members[m]];
		float s_max, s_mid;
		sorted_sigmas(g, s_max, s_mid);
		const double w = double(clamp01(g.opacity)) * double(s_max) * double(s_mid) + kWeightEpsilon;
		weight_sum += w;
		mean[0] += w * double(g.position.x);
		mean[1] += w * double(g.position.y);
		mean[2] += w * double(g.position.z);
		if (w > best_w) {
			best_w = w;
			best = p_members[m];
		}
	}
	mean[0] /= weight_sum;
	mean[1] /= weight_sum;
	mean[2] /= weight_sum;

	// Pass 2: covariance, SH and alpha mass.
	double cov[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	double dc[4] = { 0.0, 0.0, 0.0, 0.0 };
	double sh1[3][3] = { { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0 } };
	double alpha_mass = 0.0;
	if (p_out_sh && sh_count > 0u) {
		for (uint32_t c = 0; c < sh_count; c++) {
			p_out_sh[c] = Vector3();
		}
	}
	static constexpr uint32_t kStackShVectors = 16u;
	double sh_stack[kStackShVectors * 3u];
	LocalVector<double> sh_heap;
	double *sh_acc = sh_stack;
	if (sh_count > kStackShVectors) {
		sh_heap.resize(sh_count * 3u);
		sh_acc = sh_heap.ptr();
	}
	for (uint32_t c = 0; c < sh_count * 3u; c++) {
		sh_acc[c] = 0.0;
	}
	for (uint32_t m = 0; m < p_member_count; m++) {
		const uint32_t idx = p_members[m];
		const Gaussian &g = p_rep.gaussians[idx];
		float s_max, s_mid;
		sorted_sigmas(g, s_max, s_mid);
		const double a = double(clamp01(g.opacity));
		const double sa = double(s_max) * double(s_mid);
		const double w = a * sa + kWeightEpsilon;
		double ci[6];
		splat_covariance(g, ci);
		const double d[3] = { double(g.position.x) - mean[0], double(g.position.y) - mean[1], double(g.position.z) - mean[2] };
		cov[0] += w * (ci[0] + d[0] * d[0]);
		cov[1] += w * (ci[1] + d[0] * d[1]);
		cov[2] += w * (ci[2] + d[0] * d[2]);
		cov[3] += w * (ci[3] + d[1] * d[1]);
		cov[4] += w * (ci[4] + d[1] * d[2]);
		cov[5] += w * (ci[5] + d[2] * d[2]);
		dc[0] += w * double(g.sh_dc.r);
		dc[1] += w * double(g.sh_dc.g);
		dc[2] += w * double(g.sh_dc.b);
		dc[3] += w * double(g.sh_dc.a);
		for (int k = 0; k < 3; k++) {
			sh1[k][0] += w * double(g.sh_1[k].x);
			sh1[k][1] += w * double(g.sh_1[k].y);
			sh1[k][2] += w * double(g.sh_1[k].z);
		}
		if (sh_count > 0u && p_rep.sh_high_order) {
			const Vector3 *src = p_rep.sh_high_order + uint64_t(idx) * sh_count;
			for (uint32_t c = 0; c < sh_count; c++) {
				sh_acc[c * 3u + 0u] += w * double(src[c].x);
				sh_acc[c * 3u + 1u] += w * double(src[c].y);
				sh_acc[c * 3u + 2u] += w * double(src[c].z);
			}
		}
		alpha_mass += a * kTwoPi * sa;
	}
	for (int k = 0; k < 6; k++) {
		cov[k] /= weight_sum;
	}

	// `sat` opacity: inflate the two largest axes by k <= 1.25, alpha = 1 - exp(-M / C).
	double lam[3];
	double vec[3][3];
	eigen_symmetric3(cov, lam, vec);
	for (int k = 0; k < 3; k++) {
		lam[k] = MAX(lam[k], kMinEigen);
	}
	double capacity = kTwoPi * std::sqrt(lam[2] * lam[1]);
	const double k_inflate = CLAMP(std::sqrt(alpha_mass / capacity), 1.0, kSatMaxInflation);
	lam[1] *= k_inflate * k_inflate;
	lam[2] *= k_inflate * k_inflate;
	capacity *= k_inflate * k_inflate;
	const double alpha = MIN(1.0 - std::exp(-alpha_mass / capacity), kMaxMergedAlpha);

	// Proper rotation: flip eigenvector 0 when the basis is left-handed (as the prototype).
	const double det = vec[0][0] * (vec[1][1] * vec[2][2] - vec[1][2] * vec[2][1]) -
			vec[0][1] * (vec[1][0] * vec[2][2] - vec[1][2] * vec[2][0]) +
			vec[0][2] * (vec[1][0] * vec[2][1] - vec[1][1] * vec[2][0]);
	if (det < 0.0) {
		for (int r = 0; r < 3; r++) {
			vec[r][0] = -vec[r][0];
		}
	}

	r_out = p_rep.gaussians[best];
	r_out.position = Vector3(real_t(mean[0]), real_t(mean[1]), real_t(mean[2]));
	r_out.scale = Vector3(real_t(std::sqrt(lam[0])), real_t(std::sqrt(lam[1])), real_t(std::sqrt(lam[2])));
	r_out.rotation = matrix_to_quat(vec);
	r_out.opacity = float(alpha);
	r_out.sh_dc = Color(float(dc[0] / weight_sum), float(dc[1] / weight_sum), float(dc[2] / weight_sum), float(dc[3] / weight_sum));
	for (int k = 0; k < 3; k++) {
		r_out.sh_1[k] = Vector3(real_t(sh1[k][0] / weight_sum), real_t(sh1[k][1] / weight_sum), real_t(sh1[k][2] / weight_sum));
	}
	if (p_out_sh && sh_count > 0u) {
		for (uint32_t c = 0; c < sh_count; c++) {
			p_out_sh[c] = Vector3(real_t(sh_acc[c * 3u + 0u] / weight_sum), real_t(sh_acc[c * 3u + 1u] / weight_sum),
					real_t(sh_acc[c * 3u + 2u] / weight_sum));
		}
	}

	if (r_moments) {
		r_moments->weight_sum = weight_sum;
		for (int k = 0; k < 3; k++) {
			r_moments->mean[k] = mean[k];
		}
		for (int k = 0; k < 6; k++) {
			r_moments->covariance[k] = cov[k];
		}
		r_moments->alpha_mass = alpha_mass;
		r_moments->capacity = capacity;
		r_moments->inflation = k_inflate;
		r_moments->alpha = alpha;
	}
}

uint32_t merged_count_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch,
		Error *r_code) {
	if (r_code) {
		*r_code = OK;
	}
	ERR_FAIL_COND_V(!(p_eps > 0.0), UINT32_MAX);
	const Error bucketed = bucket_cells(p_rep, p_origin, p_eps, r_scratch);
	if (bucketed != OK) {
		if (r_code) {
			*r_code = bucketed;
		}
		return UINT32_MAX;
	}
	const LocalVector<MergeCellEntry> &entries = r_scratch.entries;
	uint32_t count = 0u;
	for (uint32_t i = 0; i < p_rep.count; i++) {
		count += r_scratch.pass_through[i];
	}
	for (uint32_t i = 0; i < entries.size(); i++) {
		if (i == 0u || entries[i].key != entries[i - 1u].key) {
			count++;
		}
	}
	return count;
}

Error merge_at_eps(const SplatSpan &p_rep, const Vector3 &p_origin, double p_eps, MergeScratch &r_scratch, SplatList &r_out) {
	r_out.clear();
	r_out.sh_high_order_count = p_rep.sh_high_order_count;
	ERR_FAIL_COND_V(!(p_eps > 0.0), ERR_INVALID_PARAMETER);
	const Error bucketed = bucket_cells(p_rep, p_origin, p_eps, r_scratch);
	if (bucketed == ERR_OUT_OF_MEMORY) {
		return ERR_OUT_OF_MEMORY;
	}
	ERR_FAIL_COND_V_MSG(bucketed != OK, bucketed, "HLOD merge cell size is too small for the content extent.");
	const LocalVector<MergeCellEntry> &entries = r_scratch.entries;
	const uint32_t sh_count = p_rep.sh_high_order_count;

	uint32_t cell_count = 0u;
	for (uint32_t i = 0; i < entries.size(); i++) {
		if (i == 0u || entries[i].key != entries[i - 1u].key) {
			cell_count++;
		}
	}
	uint32_t pass_count = 0u;
	for (uint32_t i = 0; i < p_rep.count; i++) {
		pass_count += r_scratch.pass_through[i];
	}
	if (!fallible_resize(r_out.gaussians, uint64_t(pass_count) + cell_count, "gs_hlod::merge_at_eps merged_gaussians") ||
			(sh_count > 0u && !fallible_resize(r_out.sh_high_order, (uint64_t(pass_count) + cell_count) * sh_count,
									   "gs_hlod::merge_at_eps merged_sh_high_order"))) {
		r_out.clear();
		return ERR_OUT_OF_MEMORY;
	}

	uint32_t w = 0u;
	for (uint32_t i = 0; i < p_rep.count; i++) {
		if (!r_scratch.pass_through[i]) {
			continue;
		}
		r_out.gaussians[w] = p_rep.gaussians[i];
		if (sh_count > 0u) {
			memcpy(r_out.sh_high_order.ptr() + uint64_t(w) * sh_count, p_rep.sh_high_order + uint64_t(i) * sh_count,
					sizeof(Vector3) * sh_count);
		}
		w++;
	}
	if (entries.is_empty()) {
		return OK; // All payload and SH entries have already been copied unchanged.
	}
	LocalVector<uint32_t> &members = r_scratch.members;
	uint32_t run_start = 0u;
	for (uint32_t i = 1; i <= entries.size(); i++) {
		if (i < entries.size() && entries[i].key == entries[run_start].key) {
			continue;
		}
		if (!fallible_resize(members, i - run_start, "gs_hlod::merge_at_eps members")) {
			r_out.clear();
			return ERR_OUT_OF_MEMORY;
		}
		for (uint32_t m = run_start; m < i; m++) {
			members[m - run_start] = entries[m].index;
		}
		merge_cell_sat(p_rep, members.ptr(), members.size(), r_out.gaussians[w],
				sh_count > 0u ? r_out.sh_high_order.ptr() + uint64_t(w) * sh_count : nullptr);
		w++;
		run_start = i;
	}
	return OK;
}

bool bake_interior_node(const SplatSpan *p_children, uint32_t p_child_count,
		const Vector3 &p_grid_origin, double p_cell_edge, double p_max_child_error,
		const InteriorBakeParams &p_params, InteriorBakeScratch &r_scratch, InteriorBakeResult &r_result,
		String *r_error, Error *r_code) {
	const uint64_t t_start = OS::get_singleton()->get_ticks_usec();
	auto out_of_memory = [&](const char *p_what) {
		if (r_error) {
			*r_error = vformat("HLOD merge cannot allocate %s.", String(p_what));
		}
		if (r_code) {
			*r_code = ERR_OUT_OF_MEMORY;
		}
		return false;
	};
	ERR_FAIL_COND_V(p_child_count == 0u, false);
	const uint32_t sh_count = p_children[0].sh_high_order_count;

	SplatList &rep = r_scratch.reference;
	rep.clear();
	rep.sh_high_order_count = sh_count;
	for (uint32_t c = 0; c < p_child_count; c++) {
		ERR_FAIL_COND_V(p_children[c].sh_high_order_count != sh_count, false);
		const Error appended = rep.append(p_children[c], 0u, p_children[c].count,
				"gs_hlod::bake_interior_node reference_gaussians", "gs_hlod::bake_interior_node reference_sh_high_order");
		if (appended == ERR_OUT_OF_MEMORY) {
			return out_of_memory("the merge reference");
		}
		ERR_FAIL_COND_V(appended != OK, false);
	}
	const SplatSpan rep_span = rep.span();
	ERR_FAIL_COND_V(rep_span.count == 0u, false);

	// eps search (ADR §6.3): start at cell edge / 64 and at least twice the children's eps, grow
	// by 1.25 until the merge is at most a quarter of the children and at most 16,384 splats.
	double eps = MAX(p_cell_edge / double(MAX(1u, p_params.merge_grid)), 2.0 * p_max_child_error);
	if (!(eps > 0.0)) {
		// Degenerate cell with leaf children (only reachable with a zero cell edge, which the tree
		// never produces): start from a tiny cell and let the search grow it.
		eps = 1e-9;
	}
	const uint32_t target = uint32_t(MIN<uint64_t>(p_params.node_max_splats,
			MAX<uint64_t>(1u, uint64_t(std::ceil(double(rep_span.count) / p_params.reduction)))));
	uint32_t steps = 0u;
	while (true) {
		Error count_code = OK;
		const uint32_t count = merged_count_at_eps(rep_span, p_grid_origin, eps, r_scratch.merge, &count_code);
		if (count_code == ERR_OUT_OF_MEMORY) {
			return out_of_memory("the merge cell scratch");
		}
		if (count <= target) {
			break;
		}
		if (++steps > p_params.max_eps_steps || !std::isfinite(eps * p_params.eps_growth)) {
			if (r_error) {
				*r_error = vformat("HLOD merge did not reach %d splats from %d within %d cell-size steps.",
						target, rep_span.count, p_params.max_eps_steps);
			}
			if (r_code) {
				*r_code = ERR_INVALID_DATA;
			}
			return false;
		}
		eps *= p_params.eps_growth;
	}
	const Error merged = merge_at_eps(rep_span, p_grid_origin, eps, r_scratch.merge, r_scratch.merged);
	if (merged == ERR_OUT_OF_MEMORY) {
		return out_of_memory("the merged candidate");
	}
	ERR_FAIL_COND_V(merged != OK, false);
	const uint64_t t_merged = OS::get_singleton()->get_ticks_usec();

	const SplatSpan merged_span = r_scratch.merged.span();
	if (!importance_order(merged_span, r_scratch.order)) {
		return out_of_memory("the importance order");
	}
	r_result.payload.clear();
	r_result.payload.sh_high_order_count = sh_count;
	if (!fallible_resize(r_result.payload.gaussians, merged_span.count, "gs_hlod::bake_interior_node payload_gaussians") ||
			(sh_count > 0u && !fallible_resize(r_result.payload.sh_high_order, uint64_t(merged_span.count) * sh_count,
									   "gs_hlod::bake_interior_node payload_sh_high_order"))) {
		r_result.payload.clear();
		return out_of_memory("the node payload");
	}
	for (uint32_t i = 0; i < merged_span.count; i++) {
		const uint32_t src = r_scratch.order[i];
		r_result.payload.gaussians[i] = merged_span.gaussians[src];
		if (sh_count > 0u) {
			memcpy(r_result.payload.sh_high_order.ptr() + uint64_t(i) * sh_count,
					merged_span.sh_high_order + uint64_t(src) * sh_count, sizeof(Vector3) * sh_count);
		}
	}
	const uint64_t t_end = OS::get_singleton()->get_ticks_usec();

	r_result.geometric_error = eps;
	r_result.eps_steps = steps;
	r_result.merge_usec = t_merged - t_start;
	r_result.order_usec = t_end - t_merged;
	return true;
}

} // namespace gs_hlod
