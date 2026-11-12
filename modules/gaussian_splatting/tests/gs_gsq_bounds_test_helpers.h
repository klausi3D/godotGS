#pragma once

#include "test_macros.h"
#include "../core/gaussian_splat_hlod_bake_bounds.h"
#include "../core/gs_vector_alloc.h"
#include "../io/gaussian_splat_world_paged.h"
#include "../io/gs_world_writer.h"
#include "../io/gsq/gsq_codec.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace TestGSQBounds {

struct Budget {
	double axis[3] = {};
	double radial = 0;
};

inline double float_spacing(double p_value) {
	const float f = float(std::abs(p_value));
	return double(std::nextafter(f, std::numeric_limits<float>::infinity())) - double(f);
}

// Independent spatial error envelope for gsq_codec.cpp's quantisers:
// - pos_step_m selects 16/21 bits using Q24 page ranges; it is NOT a bound
//   on every 21-bit page's step. R/(2*M) covers rounding to the nearest code;
//   (1/2 + 1 + M/2^23)/2^24 covers input Q24 rounding, dequantisation floor,
//   and rounding the Q22 step. Add float/frame rounding separately below.
// - Scale uses 1023 intervals in Q16 log2. Half an interval plus 3 Q16 units
//   covers log truncation/repeated-squaring drift and Q22 dequantisation.
//   pow2_q16 uses at most 16 rounded Q30 factors and 16 rounded products;
//   (1+2^-31)^32 and final float rounding bound their relative error.
// - Smallest-three rotation uses 511 intervals over [0,sqrt(1/2)] in Q30.
//   The stored-component error e includes half a step and Q30 normalisation
//   (4 units), integer norm (2), division (2), and dequantisation (2).
//   The omitted original component is >=1/2. Squared-norm error is at most
//   h=3e+3e^2, hence its sqrt error is h/(1/2+sqrt(1/4-h)) plus one Q30 unit.
//   Normalising the decoded quaternion doubles its error; the rotation-matrix
//   operator error is at most twice that, giving 4*delta.
// Reverse triangle on each row of R*diag(scale) therefore bounds three-sigma
// AABB support change by 3*(scale_error + max_scale*rotation_error). Sphere
// support uses 3*max_scale, so rotation contributes nothing to its radius.
// CodecParams sh_bits/opacity_bits/zstd_level do not change spatial support.
// These budgets use the ORIGINAL payload and its independently encoded page,
// never the writer's bounds or its decoded payload as the error allowance.
inline bool quantisation_budget(const Gaussian *p_original, uint32_t p_count,
		const double p_center[3], bool p_leaf, const gs_gsq::PageInfo &p_page,
		const gs_world_writer::CodecParams &p_params, Budget &r_budget) {
	if (!p_original || !p_count || p_page.splat_count != p_count ||
			(p_page.position_bits != 16 && p_page.position_bits != 21)) {
		FAIL("GSQ bounds oracle needs a complete independently encoded node");
		return false;
	}
	CHECK(p_params.codec_id == gs_world_writer::kCodecGsqV1);
	const double q24 = std::ldexp(1.0, 24);
	double ranges[3];
	double longest = 0;
	for (int a = 0; a < 3; ++a) {
		ranges[a] = std::round(double(p_page.position_max[a]) * q24) - std::round(double(p_page.position_min[a]) * q24);
		longest = MAX(longest, ranges[a]);
	}
	const uint32_t bits = longest > std::round(double(p_params.pos_step_m) * q24) * 65535.0 ? 21 : 16;
	CHECK(p_page.position_bits == bits);
	const double codes = double((1u << bits) - 1);
	const double q30 = std::ldexp(1.0, -30);
	const double e = std::sqrt(0.5) / (2.0 * 511.0) + (4 + 2 + 2 + 2) * q30;
	const double h = 3 * e + 3 * e * e;
	const double omitted = h / (0.5 + std::sqrt(0.25 - h)) + q30;
	const double rotation_error = 4 * (std::sqrt(3 * e * e + omitted * omitted) + std::ldexp(1.0, -24));
	const double pow_rounding = std::pow(1 + std::ldexp(1.0, -31), 32) * (1 + std::ldexp(1.0, -24));
	double scale_error = 0, max_scale = 0;
	double position_error[3] = {};
	for (int a = 0; a < 3; ++a) {
		const double log_error = (double(p_page.log_scale_max[a]) - p_page.log_scale_min[a]) / (2 * 1023 * 65536.0) + 3.0 / 65536;
		const double factor = std::exp2(log_error) * pow_rounding - 1;
		const double max_relative = MAX(std::abs(double(p_page.position_min[a])), std::abs(double(p_page.position_max[a]))) + 1 / q24;
		double frame_error = 0;
		for (uint32_t i = 0; i < p_count; ++i) {
			const double scale = p_original[i].scale[a];
			const double floor_scale = std::ldexp(1.0, -32);
			max_scale = MAX(max_scale, scale);
			scale_error = MAX(scale_error, MAX(scale, floor_scale) * factor + MAX(0.0, floor_scale - scale));
			if (p_leaf) {
				const double relative = double(p_original[i].position[a]) - p_center[a];
				frame_error = MAX(frame_error, std::abs(double(real_t(relative)) - relative));
			}
		}
		position_error[a] = ranges[a] / (q24 * 2 * codes) + (0.5 + 1 + codes / std::ldexp(1.0, 23)) / q24 +
				0.5 * float_spacing(max_relative) + frame_error;
		if (p_leaf) {
			position_error[a] += 0.5 * float_spacing(std::abs(p_center[a]) + max_relative) +
					2 * std::numeric_limits<double>::epsilon() * (std::abs(p_center[a]) + max_relative);
		}
	}
	double position_squared = 0;
	for (int a = 0; a < 3; ++a) {
		r_budget.axis[a] = position_error[a] + 3 * (scale_error + max_scale * rotation_error);
		position_squared += position_error[a] * position_error[a];
	}
	r_budget.radial = std::sqrt(position_squared) + 3 * scale_error;
	return true;
}

inline void check_unchanged_fields(const GaussianSplatHlodNode &a, const GaussianSplatHlodNode &b) {
	CHECK(a.cell.e == b.cell.e);
	CHECK(a.cell.ix == b.cell.ix);
	CHECK(a.cell.iy == b.cell.iy);
	CHECK(a.cell.iz == b.cell.iz);
	CHECK(a.geometric_error == b.geometric_error);
	CHECK(a.error_chosen == b.error_chosen);
	CHECK(a.error_rejected == b.error_rejected);
	CHECK(a.parent == b.parent);
	CHECK(a.first_child == b.first_child);
	CHECK(a.child_count == b.child_count);
	CHECK(a.height == b.height);
	CHECK(a.payload_first == b.payload_first);
	CHECK(a.payload_count == b.payload_count);
	CHECK(a.flags == b.flags);
	CHECK(a.overlap_footprint == b.overlap_footprint);
}

inline bool check_decoded_support(const GaussianSplatHlodTree &p_tree, uint32_t p_node,
		const Vector<Gaussian> &p_runtime, const Vector<Vector3> &p_sh) {
	if (p_node >= p_tree.nodes.size() || p_runtime.size() != int(p_tree.nodes[p_node].payload_count) ||
			uint64_t(p_sh.size()) != uint64_t(p_runtime.size()) * p_tree.sh_high_order_count) {
		FAIL("GSQ support oracle needs the complete reopened payload");
		return false;
	}
	const auto &node = p_tree.nodes[p_node];
	double center[3];
	p_tree.node_cell_center(node, center);
	Vector<Gaussian> relative;
	if (!gs_resize_or_fail(relative, p_runtime.size(), "GSQ bounds test relative payload")) {
		FAIL("cannot allocate GSQ bounds test payload");
		return false;
	}
	std::memcpy(relative.ptrw(), p_runtime.ptr(), p_runtime.size() * sizeof(Gaussian));
	if (node.is_leaf()) {
		for (int i = 0; i < relative.size(); ++i) {
			for (int a = 0; a < 3; ++a) {
				relative.write[i].position[a] = real_t(double(p_runtime[i].position[a]) - center[a]);
			}
		}
	}
	String reason;
	const bool contained = gs_world_paged::validate_node_payload(center, p_tree.sh_high_order_count, node, relative.ptr(), p_sh.ptr(), reason);
	INFO("decoded support: ", doctest::String(reason.utf8().get_data()));
	CHECK(contained); // Includes AABB AND sphere, with the unchanged half-ULP tolerance.
	return contained;
}

inline void check_tree_bounds(const GaussianSplatHlodTree &p_bake, const GaussianSplatHlodTree &p_written,
		std::vector<Budget> p_budget) {
	if (p_bake.nodes.is_empty() || p_bake.nodes.size() != p_written.nodes.size() || p_budget.size() != p_bake.nodes.size()) {
		FAIL("GSQ bounds comparison needs all original/written nodes and budgets");
		return;
	}
	std::vector<double> midpoint_motion(p_budget.size());
	// BFS means children are already bounded during this reverse traversal.
	for (int64_t i = int64_t(p_bake.nodes.size()) - 1; i >= 0; --i) {
		INFO("bounds node=", i);
		const auto &old = p_bake.nodes[i];
		const auto &node = p_written.nodes[i];
		check_unchanged_fields(node, old);
		if (old.child_count && (old.first_child <= uint64_t(i) || uint64_t(old.first_child) + old.child_count > p_budget.size())) {
			FAIL("bounds oracle requires canonical BFS children");
			return;
		}
		double center[3];
		p_bake.node_cell_center(old, center);
		double motion_squared = 0;
		double radial = p_budget[i].radial;
		for (uint32_t c = old.first_child; c < old.first_child + old.child_count; ++c) {
			radial = MAX(radial, p_budget[c].radial + midpoint_motion[c]);
		}
		for (int a = 0; a < 3; ++a) {
			double pad = p_budget[i].axis[a];
			for (uint32_t c = old.first_child; c < old.first_child + old.child_count; ++c) {
				pad = MAX(pad, p_budget[c].axis[a]);
			}
			// Outward float rounding is allowed once per stored ancestor record.
			const double lo = gs_hlod::bake_bounds::round_down_offset(center[a], center[a] + double(old.aabb_min[a]) - pad);
			const double hi = gs_hlod::bake_bounds::round_up_offset(center[a], center[a] + double(old.aabb_max[a]) + pad);
			CHECK(node.aabb_min[a] <= old.aabb_min[a]);
			CHECK(node.aabb_max[a] >= old.aabb_max[a]);
			CHECK(double(node.aabb_min[a]) >= lo);
			CHECK(double(node.aabb_max[a]) <= hi);
			p_budget[i].axis[a] = MAX(double(old.aabb_min[a]) - lo, hi - double(old.aabb_max[a]));
			const double old_mid = 0.5 * ((center[a] + double(old.aabb_min[a])) + (center[a] + double(old.aabb_max[a])));
			const double new_mid = 0.5 * ((center[a] + double(node.aabb_min[a])) + (center[a] + double(node.aabb_max[a])));
			motion_squared += (new_mid - old_mid) * (new_mid - old_mid);
		}
		midpoint_motion[i] = std::sqrt(motion_squared);
		// A shifted sphere must contain the ENTIRE original sphere, not merely
		// keep its numeric radius. Ancestors additionally absorb child motion.
		CHECK(double(node.radius) >= double(old.radius) + midpoint_motion[i]);
		const double upper_radius = gs_hlod::bake_bounds::round_up(double(old.radius) + midpoint_motion[i] + radial);
		CHECK(double(node.radius) <= upper_radius);
		p_budget[i].radial = upper_radius - double(old.radius);
		for (uint32_t c = old.first_child; c < old.first_child + old.child_count; ++c) {
			const auto &child = p_written.nodes[c];
			double child_center[3];
			p_written.node_cell_center(child, child_center);
			double distance_squared = 0;
			for (int a = 0; a < 3; ++a) {
				CHECK(center[a] + double(node.aabb_min[a]) <= child_center[a] + double(child.aabb_min[a]));
				CHECK(center[a] + double(node.aabb_max[a]) >= child_center[a] + double(child.aabb_max[a]));
				const double mid = 0.5 * ((center[a] + double(node.aabb_min[a])) + (center[a] + double(node.aabb_max[a])));
				const double child_mid = 0.5 * ((child_center[a] + double(child.aabb_min[a])) + (child_center[a] + double(child.aabb_max[a])));
				distance_squared += (mid - child_mid) * (mid - child_mid);
			}
			CHECK(double(node.radius) >= std::sqrt(distance_squared) + double(child.radius));
		}
	}
}

} // namespace TestGSQBounds
