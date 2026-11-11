#include "gaussian_splat_hlod_bake_bounds.h"

#include "core/math/math_funcs.h"

#include <cmath>

namespace gs_hlod {
namespace bake_bounds {

void BoundsAccumulator::add(const double p_lo[3], const double p_hi[3]) {
	for (int a = 0; a < 3; a++) {
		lo[a] = MIN(lo[a], p_lo[a]);
		hi[a] = MAX(hi[a], p_hi[a]);
	}
}

// Largest float f with p_center + f <= p_target (or smallest with >=), evaluated in double
// exactly as the validator evaluates it, so stored bounds contain what they must.
float round_down_offset(double p_center, double p_target) {
	float f = float(p_target - p_center);
	while (p_center + double(f) > p_target) {
		f = std::nextafter(f, -FLT_MAX);
	}
	return f;
}

float round_up_offset(double p_center, double p_target) {
	float f = float(p_target - p_center);
	while (p_center + double(f) < p_target) {
		f = std::nextafter(f, FLT_MAX);
	}
	return f;
}

float round_up(double p_value) {
	float f = float(p_value);
	if (double(f) < p_value) {
		f = std::nextafter(f, FLT_MAX);
	}
	return f;
}

// Adds the 3-sigma axis-aligned extent of p_span in the accumulator's frame.
void add_payload_bounds(const SplatSpan &p_span, const double p_shift[3], BoundsAccumulator &r_bounds) {
	for (uint32_t i = 0; i < p_span.count; i++) {
		const Gaussian &g = p_span.gaussians[i];
		double cov[6];
		splat_covariance(g, cov);
		const double half[3] = { 3.0 * std::sqrt(MAX(cov[0], 0.0)), 3.0 * std::sqrt(MAX(cov[3], 0.0)), 3.0 * std::sqrt(MAX(cov[5], 0.0)) };
		const double p[3] = { double(g.position.x) + p_shift[0], double(g.position.y) + p_shift[1], double(g.position.z) + p_shift[2] };
		const double lo[3] = { p[0] - half[0], p[1] - half[1], p[2] - half[2] };
		const double hi[3] = { p[0] + half[0], p[1] + half[1], p[2] + half[2] };
		r_bounds.add(lo, hi);
	}
}

double payload_radius(const SplatSpan &p_span, const double p_shift[3], const double p_mid[3]) {
	double radius = 0.0;
	for (uint32_t i = 0; i < p_span.count; i++) {
		const Gaussian &g = p_span.gaussians[i];
		const double d[3] = { double(g.position.x) + p_shift[0] - p_mid[0], double(g.position.y) + p_shift[1] - p_mid[1],
			double(g.position.z) + p_shift[2] - p_mid[2] };
		const double s_max = MAX(Math::abs(g.scale.x), MAX(Math::abs(g.scale.y), Math::abs(g.scale.z)));
		radius = MAX(radius, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 3.0 * s_max);
	}
	return radius;
}

} // namespace bake_bounds
} // namespace gs_hlod
