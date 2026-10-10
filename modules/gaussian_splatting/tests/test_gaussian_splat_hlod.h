#pragma once

// HLOD slice S1a (ADR docs/architecture/adr-hlod-streaming.md at 74bf3699702): the `sat` merge,
// the world-aligned octree bake, the .gsplatworld v2 format and the importer bake (tests 3 and 4
// of the ADR's §8).
//
// The merge golden numbers come from the ADR's Python prototype (`hlod.merge_rep(rep, bmin, eps,
// 'sat')`, with the single-child identity fix) run on the same fixture, which this file
// regenerates bit-exactly: positions, scales and opacities are built from a 32-bit hash with float
// operations that are exact (power-of-two scaling) or single IEEE roundings, so Python (numpy
// float32) and C++ produce identical inputs.

#include "test_macros.h"

#include "../core/gaussian_splat_hlod_bake.h"
#include "../core/gaussian_splat_hlod_merge.h"
#include "../core/gaussian_splat_hlod_tree.h"
#include "../core/gaussian_splat_world.h"
#include "../io/gaussian_splat_world_io.h"
#include "../io/resource_importer_gsplatworld.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/compression.h"
#include "core/io/resource_loader.h"
#include "core/math/geometry_3d.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/os/os.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <string>

namespace TestGaussianSplatHlod {

inline uint32_t hlod_fixture_hash(uint32_t p_x) {
	p_x ^= p_x >> 16u;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15u;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16u;
	return p_x;
}

inline float hlod_fixture_unit(uint32_t p_i, uint32_t p_k, uint32_t p_seed) {
	return float(hlod_fixture_hash(p_i * 8u + p_k + p_seed) >> 8u) * (1.0f / 16777216.0f);
}

// p_ground flat splats on a 32 m x 32 m patch centred on the origin (quadtree-like content) and
// p_crown volumetric splats in a 4 m cube (octree-like content). Mirrors golden_merge.py fixture().
inline void hlod_make_fixture(uint32_t p_ground, uint32_t p_crown, LocalVector<Gaussian> &r_out) {
	static const Quaternion quats[5] = {
		Quaternion(0.0f, 0.0f, 0.0f, 1.0f),
		Quaternion(0.6f, 0.0f, 0.0f, 0.8f),
		Quaternion(0.0f, 0.6f, 0.0f, 0.8f),
		Quaternion(0.0f, 0.0f, 0.6f, 0.8f),
		Quaternion(0.5f, 0.5f, 0.5f, 0.5f),
	};
	r_out.clear();
	r_out.resize(p_ground + p_crown);
	for (uint32_t i = 0; i < p_ground; i++) {
		float u[8];
		for (uint32_t k = 0; k < 8u; k++) {
			u[k] = hlod_fixture_unit(i, k, 0x1000u);
		}
		Gaussian g;
		g.position = Vector3((u[0] - 0.5f) * 32.0f, (u[1] - 0.5f) * 0.03125f, (u[2] - 0.5f) * 32.0f);
		g.scale = Vector3((1.0f + u[3]) * 0.0625f, 0.00390625f, (1.0f + u[4]) * 0.0625f);
		g.rotation = quats[0];
		g.opacity = 0.25f + u[5] * 0.5f;
		g.sh_dc = Color((u[6] - 0.5f) * 2.0f, (u[7] - 0.5f) * 2.0f, (u[5] - 0.5f) * 2.0f, 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		r_out[i] = g;
	}
	for (uint32_t i = 0; i < p_crown; i++) {
		float u[8];
		for (uint32_t k = 0; k < 8u; k++) {
			u[k] = hlod_fixture_unit(i, k, 0x2000u);
		}
		Gaussian g;
		g.position = Vector3(8.0f + (u[0] - 0.5f) * 4.0f, 3.0f + (u[1] - 0.5f) * 4.0f, 8.0f + (u[2] - 0.5f) * 4.0f);
		g.scale = Vector3((1.0f + u[3]) * 0.015625f, (1.0f + u[4]) * 0.015625f, (1.0f + u[5]) * 0.015625f);
		g.rotation = quats[hlod_fixture_hash(i + 0x3000u) % 5u];
		g.opacity = 0.25f + u[6] * 0.5f;
		g.sh_dc = Color((u[7] - 0.5f) * 2.0f, (u[3] - 0.5f) * 2.0f, (u[4] - 0.5f) * 2.0f, 1.0f);
		g.render_meta = gaussian_set_dc_encoding(0u, GAUSSIAN_DC_ENCODING_LINEAR_RGB);
		r_out[p_ground + i] = g;
	}
}

inline gs_hlod::SplatSpan hlod_span(const LocalVector<Gaussian> &p_g) {
	gs_hlod::SplatSpan s;
	s.gaussians = p_g.ptr();
	s.count = p_g.size();
	return s;
}

inline void hlod_bounds(const LocalVector<Gaussian> &p_g, Vector3 &r_min, Vector3 &r_max) {
	r_min = p_g[0].position;
	r_max = p_g[0].position;
	for (uint32_t i = 1; i < p_g.size(); i++) {
		r_min = r_min.min(p_g[i].position);
		r_max = r_max.max(p_g[i].position);
	}
}

// Covariance of a splat, packed xx, xy, xz, yy, yz, zz, computed independently of the merge code
// (Godot's Basis from the quaternion).
inline void hlod_covariance(const Gaussian &p_g, double r_cov[6]) {
	const Basis r(p_g.rotation.normalized());
	const double s2[3] = { double(p_g.scale.x) * p_g.scale.x, double(p_g.scale.y) * p_g.scale.y, double(p_g.scale.z) * p_g.scale.z };
	auto entry = [&](int i, int j) {
		return double(r.rows[i][0]) * s2[0] * r.rows[j][0] + double(r.rows[i][1]) * s2[1] * r.rows[j][1] + double(r.rows[i][2]) * s2[2] * r.rows[j][2];
	};
	r_cov[0] = entry(0, 0);
	r_cov[1] = entry(0, 1);
	r_cov[2] = entry(0, 2);
	r_cov[3] = entry(1, 1);
	r_cov[4] = entry(1, 2);
	r_cov[5] = entry(2, 2);
}

inline bool hlod_close(double p_got, double p_want, double p_rel, double p_abs = 1e-9) {
	return std::fabs(p_got - p_want) <= MAX(p_abs, p_rel * std::fabs(p_want));
}

struct HlodMergeGolden {
	double divisor; // eps = extent / divisor
	uint32_t count;
	double sum_alpha;
	double sum_w;
	double wmean[3];
	double sum_trace;
	double sum_dc[3];
};

// golden_merge.py on hlod_make_fixture(6000, 4000): prototype merge_rep(..., 'sat') (with the
// single-child identity) at three cell sizes, anchored at the fixture's centre bbox min.
static const HlodMergeGolden kHlodMergeGolden[] = {
	{ 64.0, 3708, 1615.26034, 21.9672624, { 0.573375376, 0.16238762, 0.386470452 }, 126.971684, { -9.44053141, -19.2536744, 247.353317 } },
	{ 16.0, 267, 76.5241084, 23.3560311, { 0.506205876, 0.136166493, 0.269724941 }, 175.419129, { -2.46409815, -0.609870235, 44.5649421 } },
	{ 4.0, 17, 4.91692078, 23.7466024, { 0.477552169, 0.135863025, 0.265426655 }, 181.354352, { -0.0131785873, 0.657041627, 3.10474995 } },
};

inline bool hlod_bake(const LocalVector<Gaussian> &p_g, uint32_t p_leaf_max, gs_hlod::BakeResult &r_result, String *r_error = nullptr) {
	gs_hlod::BakeInput input;
	input.gaussians = p_g.ptr();
	input.count = p_g.size();
	gs_hlod::BakeParams params;
	params.leaf_max_splats = p_leaf_max;
	return gs_hlod::bake_world(input, params, r_result, r_error);
}

// World-space bounds of a node's cell region.
inline void hlod_cell_region(const GaussianSplatHlodTree &p_tree, const GaussianSplatHlodNode &p_node, double r_lo[3], double r_hi[3]) {
	double c[3];
	p_tree.node_cell_center(p_node, c);
	const double half = 0.5 * p_tree.node_cell_edge(p_node);
	for (int a = 0; a < 3; a++) {
		r_lo[a] = c[a] - half;
		r_hi[a] = c[a] + half;
	}
}

// World-space extent of the root node, in double (the tree validator's containment frame).
inline void hlod_root_extent(const GaussianSplatHlodTree &p_tree, double r_lo[3], double r_hi[3]) {
	double c[3];
	p_tree.node_cell_center(p_tree.nodes[0], c);
	for (int a = 0; a < 3; a++) {
		r_lo[a] = c[a] + double(p_tree.nodes[0].aabb_min[a]);
		r_hi[a] = c[a] + double(p_tree.nodes[0].aabb_max[a]);
	}
}

inline bool hlod_aabb_encloses(const AABB &p_bounds, const double p_lo[3], const double p_hi[3]) {
	for (int a = 0; a < 3; a++) {
		const double begin = double(p_bounds.position[a]);
		if (!(p_bounds.size[a] >= 0.0f) || begin > p_lo[a] || begin + double(p_bounds.size[a]) < p_hi[a]) {
			return false;
		}
	}
	return true;
}

// Long splats rotated so their long (local y) axis lies along world x: three sigma reaches
// x = +-6 m while GaussianData::get_aabb(), which ignores rotation, spans only x = +-0.03 m.
inline void hlod_make_needle_fixture(LocalVector<Gaussian> &r_out) {
	hlod_make_fixture(0u, 2u, r_out);
	for (uint32_t i = 0; i < r_out.size(); i++) {
		r_out[i].position = Vector3(0.0f, 0.25f * float(i), 0.0f);
		r_out[i].scale = Vector3(0.01f, 2.0f, 0.01f);
		r_out[i].rotation = Quaternion(Vector3(0.0f, 0.0f, 1.0f), Math::PI * 0.5f);
	}
}

inline String hlod_temp_path(const String &p_prefix) {
	return OS::get_singleton()->get_temp_path().path_join("godotgs_hlod_" + p_prefix + "_" + itos(OS::get_singleton()->get_ticks_usec()) + ".gsplatworld");
}

inline PackedByteArray hlod_read_file(const String &p_path) {
	return FileAccess::get_file_as_bytes(p_path);
}

inline bool hlod_write_file(const String &p_path, const PackedByteArray &p_bytes) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_buffer(p_bytes.ptr(), p_bytes.size());
	return f->get_error() == OK;
}

inline Ref<GaussianSplatWorld> hlod_make_world(const LocalVector<Gaussian> &p_g) {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Vector3> no_sh;
	data->set_gaussian_payload(p_g, no_sh, 0u, 0u, false);
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_gaussian_data(data);
	return world;
}

inline bool hlod_nodes_equal(const GaussianSplatHlodNode &p_a, const GaussianSplatHlodNode &p_b) {
	return p_a.cell == p_b.cell && p_a.aabb_min == p_b.aabb_min && p_a.aabb_max == p_b.aabb_max && p_a.radius == p_b.radius &&
			p_a.geometric_error == p_b.geometric_error && p_a.error_chosen == p_b.error_chosen &&
			p_a.error_rejected == p_b.error_rejected && p_a.parent == p_b.parent && p_a.first_child == p_b.first_child &&
			p_a.child_count == p_b.child_count && p_a.height == p_b.height && p_a.payload_first == p_b.payload_first &&
			p_a.payload_count == p_b.payload_count && p_a.flags == p_b.flags;
}

} // namespace TestGaussianSplatHlod

// ------------------------------------------------------------------------------------------------
// Merge math (§6.3)
// ------------------------------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] fixture matches the golden generator") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(6000u, 4000u, g);
	Vector3 lo, hi;
	hlod_bounds(g, lo, hi);
	// golden_merge.py prints these from its own float32 fixture; equality proves the inputs match.
	CHECK(lo.x == doctest::Approx(-15.985193252563477).epsilon(1e-9));
	CHECK(lo.y == doctest::Approx(-0.015615437179803848).epsilon(1e-9));
	CHECK(lo.z == doctest::Approx(-15.984203338623047).epsilon(1e-9));
	CHECK(hi.x == doctest::Approx(15.996135711669922).epsilon(1e-9));
	CHECK(hi.y == doctest::Approx(4.999905586242676).epsilon(1e-9));
	CHECK(hi.z == doctest::Approx(15.992788314819336).epsilon(1e-9));
	double sum_alpha = 0.0;
	double sum_pos[3] = { 0.0, 0.0, 0.0 };
	for (uint32_t i = 0; i < g.size(); i++) {
		sum_alpha += g[i].opacity;
		sum_pos[0] += g[i].position.x;
		sum_pos[1] += g[i].position.y;
		sum_pos[2] += g[i].position.z;
	}
	CHECK(hlod_close(sum_alpha, 5007.777240276337, 1e-12));
	CHECK(hlod_close(sum_pos[0], 32489.254411697388, 1e-12));
	CHECK(hlod_close(sum_pos[1], 11831.794323716313, 1e-12));
	CHECK(hlod_close(sum_pos[2], 31583.17022752762, 1e-12));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] sat merge matches the ADR prototype at three cell sizes") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(6000u, 4000u, g);
	Vector3 lo, hi;
	hlod_bounds(g, lo, hi);
	const double extent = double(MAX(hi.x - lo.x, MAX(hi.y - lo.y, hi.z - lo.z)));
	const gs_hlod::SplatSpan rep = hlod_span(g);
	gs_hlod::MergeScratch scratch;

	for (const HlodMergeGolden &golden : kHlodMergeGolden) {
		const double eps = extent / golden.divisor;
		CAPTURE(golden.divisor);
		// The count is an exact integer function of the same float inputs on both sides.
		CHECK_EQ(gs_hlod::merged_count_at_eps(rep, lo, eps, scratch), golden.count);

		gs_hlod::SplatList merged;
		gs_hlod::merge_at_eps(rep, lo, eps, scratch, merged);
		if ((merged.size()) != (golden.count)) {
			FAIL("merged.size() == golden.count");
			return;
		}

		double sum_alpha = 0.0, sum_w = 0.0, sum_trace = 0.0;
		double wmean[3] = { 0.0, 0.0, 0.0 };
		double sum_dc[3] = { 0.0, 0.0, 0.0 };
		for (uint32_t i = 0; i < merged.size(); i++) {
			const Gaussian &m = merged.gaussians[i];
			const double w = double(gs_hlod::importance_area(m));
			double cov[6];
			hlod_covariance(m, cov);
			sum_alpha += m.opacity;
			sum_w += w;
			sum_trace += cov[0] + cov[3] + cov[5];
			wmean[0] += w * m.position.x;
			wmean[1] += w * m.position.y;
			wmean[2] += w * m.position.z;
			sum_dc[0] += m.sh_dc.r;
			sum_dc[1] += m.sh_dc.g;
			sum_dc[2] += m.sh_dc.b;
		}
		// Tolerances: the prototype keeps float64 moments, the payload stores float32 splats.
		CHECK(hlod_close(sum_alpha, golden.sum_alpha, 1e-5));
		CHECK(hlod_close(sum_w, golden.sum_w, 1e-4));
		CHECK(hlod_close(sum_trace, golden.sum_trace, 1e-4));
		for (int k = 0; k < 3; k++) {
			CHECK(hlod_close(wmean[k] / sum_w, golden.wmean[k], 1e-4, 1e-5));
			CHECK(hlod_close(sum_dc[k], golden.sum_dc[k], 1e-4, 1e-4));
		}
	}

	// Element-wise spot checks at eps = extent / 64 (prototype outputs 0 and 1).
	gs_hlod::SplatList merged;
	gs_hlod::merge_at_eps(rep, lo, extent / 64.0, scratch, merged);
	if (!(merged.size() > 1u)) {
		FAIL("merged.size() > 1u");
		return;
	}
	const Gaussian &m0 = merged.gaussians[0];
	CHECK(hlod_close(m0.position.x, -15.9293487, 1e-6));
	CHECK(hlod_close(m0.position.y, -4.751232668e-03, 1e-5));
	CHECK(hlod_close(m0.position.z, -15.7893009, 1e-6));
	CHECK(hlod_close(m0.opacity, 0.559557923, 1e-6));
	double cov0[6];
	hlod_covariance(m0, cov0);
	const double want0[6] = { 0.0124825005, 0.000189462875, -0.000553487887, 5.44239143e-05, -0.000114415145, 0.0068942961 };
	for (int k = 0; k < 6; k++) {
		CHECK(hlod_close(cov0[k], want0[k], 1e-4, 1e-9));
	}
	CHECK(hlod_close(m0.sh_dc.r, -0.258357171, 1e-5));
	CHECK(hlod_close(m0.sh_dc.g, 0.228524302, 1e-5));
	CHECK(hlod_close(m0.sh_dc.b, 0.018841644, 1e-5));
	// Prototype output 1 is a single-splat cell: copied unchanged (identity, decision 8d), so it
	// is bit-identical to one input splat, opacity 0.632249177 included.
	const Gaussian &m1 = merged.gaussians[1];
	CHECK(hlod_close(m1.opacity, 0.632249177, 1e-6));
	bool found = false;
	for (uint32_t i = 0; i < g.size() && !found; i++) {
		found = memcmp(&g[i], &m1, sizeof(Gaussian)) == 0;
	}
	CHECK_MESSAGE(found, "A single-child cell must be a bit-exact copy of its child.");
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] merge preserves every pass-through splat and SH when there are no merge cells") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(0u, 3u, g);
	LocalVector<Vector3> sh;
	sh.resize(g.size() * 12u);
	for (uint32_t i = 0; i < g.size(); i++) {
		g[i].scale = Vector3(2.0f, 2.0f, 2.0f);
	}
	for (uint32_t i = 0; i < sh.size(); i++) {
		sh[i] = Vector3(float(i), float(i + 1u), float(i + 2u));
	}
	gs_hlod::SplatSpan span = hlod_span(g);
	span.sh_high_order = sh.ptr();
	span.sh_high_order_count = 12u;
	gs_hlod::MergeScratch scratch;
	gs_hlod::SplatList merged;
	CHECK_EQ(gs_hlod::merged_count_at_eps(span, Vector3(), 0.125, scratch), g.size());
	gs_hlod::merge_at_eps(span, Vector3(), 0.125, scratch, merged);
	CHECK(scratch.entries.is_empty());
	if (merged.size() != g.size() || merged.sh_high_order.size() != sh.size()) {
		FAIL("all pass-through payload and SH must be retained");
		return;
	}
	CHECK(memcmp(merged.gaussians.ptr(), g.ptr(), g.size() * sizeof(Gaussian)) == 0);
	CHECK(memcmp(merged.sh_high_order.ptr(), sh.ptr(), sh.size() * sizeof(Vector3)) == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] moment matching preserves the weighted moments before inflation") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(0u, 64u, g);
	LocalVector<uint32_t> members;
	for (uint32_t i = 0; i < g.size(); i++) {
		members.push_back(i);
	}

	// Reference moments computed independently here (double, Basis-based covariance).
	double want_mean[3] = { 0.0, 0.0, 0.0 };
	double want_cov[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
	double want_mass = 0.0;
	double ws = 0.0;
	auto weight = [&](const Gaussian &p_s, double &r_area) {
		float s[3] = { Math::abs(p_s.scale.x), Math::abs(p_s.scale.y), Math::abs(p_s.scale.z) };
		std::sort(s, s + 3);
		r_area = double(s[2]) * s[1];
		return double(p_s.opacity) * r_area + 1e-20;
	};
	for (uint32_t i = 0; i < g.size(); i++) {
		double area;
		const double w = weight(g[i], area);
		ws += w;
		want_mean[0] += w * g[i].position.x;
		want_mean[1] += w * g[i].position.y;
		want_mean[2] += w * g[i].position.z;
	}
	for (int k = 0; k < 3; k++) {
		want_mean[k] /= ws;
	}
	for (uint32_t i = 0; i < g.size(); i++) {
		double area;
		const double w = weight(g[i], area);
		double c[6];
		hlod_covariance(g[i], c);
		const double d[3] = { g[i].position.x - want_mean[0], g[i].position.y - want_mean[1], g[i].position.z - want_mean[2] };
		want_cov[0] += w * (c[0] + d[0] * d[0]);
		want_cov[1] += w * (c[1] + d[0] * d[1]);
		want_cov[2] += w * (c[2] + d[0] * d[2]);
		want_cov[3] += w * (c[3] + d[1] * d[1]);
		want_cov[4] += w * (c[4] + d[1] * d[2]);
		want_cov[5] += w * (c[5] + d[2] * d[2]);
		want_mass += double(g[i].opacity) * 2.0 * 3.14159265358979323846 * area;
	}
	for (int k = 0; k < 6; k++) {
		want_cov[k] /= ws;
	}

	Gaussian out;
	gs_hlod::CellMergeMoments moments;
	gs_hlod::merge_cell_sat(hlod_span(g), members.ptr(), members.size(), out, nullptr, &moments);
	for (int k = 0; k < 3; k++) {
		CHECK(hlod_close(moments.mean[k], want_mean[k], 1e-12, 1e-12));
		CHECK(hlod_close(double(out.position[k]), want_mean[k], 1e-6, 1e-6));
	}
	for (int k = 0; k < 6; k++) {
		CHECK(hlod_close(moments.covariance[k], want_cov[k], 1e-9, 1e-12));
	}
	CHECK(hlod_close(moments.alpha_mass, want_mass, 1e-9));
	// No inflation here (a 4 m cube of 2-4 cm splats covers far less than the merged footprint):
	// the emitted covariance IS the second moment, to float precision.
	CHECK(moments.inflation == 1.0);
	double out_cov[6];
	hlod_covariance(out, out_cov);
	for (int k = 0; k < 6; k++) {
		CHECK(hlod_close(out_cov[k], want_cov[k], 1e-5, 1e-7));
	}
	CHECK(hlod_close(out.opacity, MIN(1.0 - std::exp(-moments.alpha_mass / moments.capacity), 0.99), 1e-6));

	// Sixteen coincident unit splats at alpha 0.5: M / C = 8, so the two largest axes inflate by
	// exactly the 1.25 cap per axis, the smallest stays, and alpha saturates at 0.99.
	LocalVector<Gaussian> stacked;
	for (uint32_t i = 0; i < 16u; i++) {
		Gaussian s;
		s.position = Vector3(1.0f, 2.0f, 3.0f);
		s.scale = Vector3(1.0f, 1.0f, 1.0f);
		s.opacity = 0.5f;
		stacked.push_back(s);
	}
	uint32_t stacked_members[16];
	for (uint32_t i = 0; i < 16u; i++) {
		stacked_members[i] = i;
	}
	Gaussian stacked_out;
	gs_hlod::CellMergeMoments stacked_moments;
	gs_hlod::merge_cell_sat(hlod_span(stacked), stacked_members, 16u, stacked_out, nullptr, &stacked_moments);
	CHECK(stacked_moments.inflation == gs_hlod::kSatMaxInflation);
	// Before inflation the second moment is the children's (identity covariance).
	CHECK(hlod_close(stacked_moments.covariance[0], 1.0, 1e-9));
	CHECK(hlod_close(stacked_moments.covariance[3], 1.0, 1e-9));
	CHECK(hlod_close(stacked_moments.covariance[5], 1.0, 1e-9));
	float axes[3] = { stacked_out.scale.x, stacked_out.scale.y, stacked_out.scale.z };
	std::sort(axes, axes + 3);
	CHECK(hlod_close(axes[0], 1.0, 1e-6));
	CHECK(hlod_close(axes[1], 1.25, 1e-6));
	CHECK(hlod_close(axes[2], 1.25, 1e-6));
	CHECK(hlod_close(stacked_out.opacity, 0.99, 1e-6));
	CHECK(stacked_out.position.is_equal_approx(Vector3(1.0f, 2.0f, 3.0f)));

	// A single member is copied bit-exactly (identity), SH included.
	LocalVector<Gaussian> one;
	one.push_back(g[7]);
	LocalVector<Vector3> one_sh;
	one_sh.push_back(Vector3(0.1f, 0.2f, 0.3f));
	one_sh.push_back(Vector3(-0.4f, 0.5f, -0.6f));
	gs_hlod::SplatSpan one_span = hlod_span(one);
	one_span.sh_high_order = one_sh.ptr();
	one_span.sh_high_order_count = 2u;
	uint32_t only = 0u;
	Gaussian one_out;
	Vector3 one_out_sh[2];
	gs_hlod::merge_cell_sat(one_span, &only, 1u, one_out, one_out_sh);
	CHECK(memcmp(&one_out, &one[0], sizeof(Gaussian)) == 0);
	CHECK(one_out_sh[0] == one_sh[0]);
	CHECK(one_out_sh[1] == one_sh[1]);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] merge never mixes DC encodings in one cell") {
	using namespace TestGaussianSplatHlod;
	// Four small splats in one 1 m cell: two LINEAR_RGB, two LEGACY_BIAS (worlds merged from
	// several assets carry both, core/gaussian_splat_merge_utils.cpp).
	LocalVector<Gaussian> g;
	for (uint32_t i = 0; i < 4u; i++) {
		Gaussian s;
		s.position = Vector3(0.1f + 0.2f * float(i), 0.5f, 0.5f);
		s.scale = Vector3(0.01f, 0.01f, 0.01f);
		s.opacity = 0.5f;
		const bool linear = i < 2u;
		s.sh_dc = linear ? Color(0.4f, 0.4f, 0.4f, 1.0f) : Color(-2.0f, -2.0f, -2.0f, 1.0f);
		s.render_meta = gaussian_set_dc_encoding(0u, linear ? GAUSSIAN_DC_ENCODING_LINEAR_RGB : GAUSSIAN_DC_ENCODING_LEGACY_BIAS);
		g.push_back(s);
	}
	gs_hlod::MergeScratch scratch;
	CHECK_EQ(gs_hlod::merged_count_at_eps(hlod_span(g), Vector3(0, 0, 0), 1.0, scratch), 2u);
	gs_hlod::SplatList merged;
	gs_hlod::merge_at_eps(hlod_span(g), Vector3(0, 0, 0), 1.0, scratch, merged);
	if (merged.size() != 2u) {
		FAIL("expected one merged splat per encoding");
		return;
	}
	for (uint32_t i = 0; i < 2u; i++) {
		const Gaussian &m = merged.gaussians[i];
		const bool linear = gaussian_get_dc_encoding(m.render_meta) == GAUSSIAN_DC_ENCODING_LINEAR_RGB;
		// Each output averages only its own colour space.
		CHECK(hlod_close(m.sh_dc.r, linear ? 0.4 : -2.0, 1e-6));
	}
	CHECK(gaussian_get_dc_encoding(merged.gaussians[0].render_meta) != gaussian_get_dc_encoding(merged.gaussians[1].render_meta));
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] importance order is opacity x area, ties by index") {
	LocalVector<Gaussian> g;
	g.resize(4);
	g[0].opacity = 0.5f;
	g[0].scale = Vector3(1.0f, 1.0f, 0.1f); // 0.5
	g[1].opacity = 1.0f;
	g[1].scale = Vector3(0.1f, 2.0f, 1.0f); // 2.0 (largest two axes 2 x 1)
	g[2].opacity = 0.25f;
	g[2].scale = Vector3(2.0f, 1.0f, 1.0f); // 0.5, tie with 0 -> after it
	g[3].opacity = 1.0f;
	g[3].scale = Vector3(4.0f, 0.1f, 0.1f); // 0.4 (largest axis is long, area small)
	LocalVector<uint32_t> order;
	gs_hlod::importance_order(TestGaussianSplatHlod::hlod_span(g), order);
	if ((order.size()) != (4u)) {
		FAIL("order.size() == 4u");
		return;
	}
	CHECK_EQ(order[0], 1u);
	CHECK_EQ(order[1], 0u);
	CHECK_EQ(order[2], 2u);
	CHECK_EQ(order[3], 3u);
}

// ------------------------------------------------------------------------------------------------
// Bake (§6.1, §6.2): tree invariants, coincident centres, lattice stability
// ------------------------------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: every splat in exactly one leaf, bounds nest, error is monotone, deterministic") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	gs_hlod::BakeResult result;
	String error;
	// Small leaves so the tree is several levels deep on a 40k fixture.
	if (!(hlod_bake(g, 2048u, result, &error))) {
		FAIL(error.utf8().get_data());
		return;
	}
	const GaussianSplatHlodTree &tree = result.tree;
	String reason;
	CHECK_MESSAGE(gs_hlod_validate_tree(tree, &reason), reason.utf8().get_data());
	if (tree.nodes.size() <= 10u) {
		FAIL("the 40k fixture must bake to more than 10 nodes");
		return;
	}
	CHECK(tree.nodes[0].height >= 3u);

	// Coverage: the leaf section is a permutation of the input, bit-exact.
	if ((result.leaf_gaussians.size()) != (g.size())) {
		FAIL("result.leaf_gaussians.size() == g.size()");
		return;
	}
	if ((result.leaf_source_index.size()) != (g.size())) {
		FAIL("result.leaf_source_index.size() == g.size()");
		return;
	}
	LocalVector<uint8_t> seen;
	seen.resize(g.size());
	memset(seen.ptr(), 0, seen.size());
	bool exact = true;
	for (uint32_t i = 0; i < g.size(); i++) {
		const uint32_t src = result.leaf_source_index[i];
		if (!(src < g.size())) {
			FAIL("src < g.size()");
			return;
		}
		CHECK_EQ(seen[src], 0u);
		seen[src] = 1u;
		exact = exact && memcmp(&result.leaf_gaussians[i], &g[src], sizeof(Gaussian)) == 0;
	}
	CHECK(exact);

	// The origin-centred root: the fixture straddles the planes through the origin.
	CHECK((tree.nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u);

	uint64_t leaf_splats = 0u;
	uint32_t grouped = 0u;
	for (uint32_t i = 0; i < tree.nodes.size(); i++) {
		const GaussianSplatHlodNode &n = tree.nodes[i];
		CAPTURE(i);
		CHECK(n.payload_count <= gs_hlod::kMaxNodeSplats);
		double center[3];
		tree.node_cell_center(n, center);
		const Gaussian *payload = n.child_count == 0u ? result.leaf_gaussians.ptr() + n.payload_first
													  : tree.interior_gaussians.ptr() + (n.payload_first - tree.leaf_splat_count);
		// Leaves hold absolute positions here (the runtime frame); interior payloads are node-relative.
		const double shift[3] = { n.child_count == 0u ? 0.0 : center[0], n.child_count == 0u ? 0.0 : center[1], n.child_count == 0u ? 0.0 : center[2] };
		for (uint32_t k = 0; k < n.payload_count; k++) {
			const Gaussian &s = payload[k];
			// Importance order inside the node.
			if (k > 0u) {
				CHECK(gs_hlod::importance_area(payload[k - 1u]) >= gs_hlod::importance_area(s));
			}
			// Node bounds contain the node's own payload at three sigma (§6.1, test 3).
			double cov[6];
			hlod_covariance(s, cov);
			const double half[3] = { 3.0 * std::sqrt(cov[0]), 3.0 * std::sqrt(cov[3]), 3.0 * std::sqrt(cov[5]) };
			const double p[3] = { s.position.x + shift[0], s.position.y + shift[1], s.position.z + shift[2] };
			for (int a = 0; a < 3; a++) {
				const double lo = center[a] + n.aabb_min[a];
				const double hi = center[a] + n.aabb_max[a];
				CHECK(p[a] - half[a] >= lo - 1e-5 * MAX(1.0, std::fabs(lo)));
				CHECK(p[a] + half[a] <= hi + 1e-5 * MAX(1.0, std::fabs(hi)));
			}
		}
		if (n.child_count == 0u) {
			CHECK(n.payload_count <= 2048u);
			CHECK(n.geometric_error == 0.0f);
			leaf_splats += n.payload_count;
			grouped += (n.flags & gs_hlod::kNodeFlagGroupedLeaf) != 0u ? 1u : 0u;
			// Every leaf centre lies inside its cell region (grouped leaves: the parent's cell).
			double lo[3], hi[3];
			hlod_cell_region(tree, n, lo, hi);
			for (uint32_t k = 0; k < n.payload_count; k++) {
				const Vector3 &p = payload[k].position;
				CHECK((p.x >= lo[0] && p.x < hi[0] && p.y >= lo[1] && p.y < hi[1] && p.z >= lo[2] && p.z < hi[2]));
			}
			continue;
		}
		// Interior: at most a quarter of the children, eps >= twice the children's and >= edge / 64.
		uint64_t child_total = 0u;
		for (uint32_t c = n.first_child; c < n.first_child + n.child_count; c++) {
			child_total += tree.nodes[c].payload_count;
			CHECK(n.geometric_error >= 2.0f * tree.nodes[c].geometric_error);
			CHECK(tree.nodes[c].parent == i);
		}
		CHECK(uint64_t(n.payload_count) <= (child_total + 3u) / 4u);
		CHECK(double(n.geometric_error) >= tree.node_cell_edge(n) / 64.0 * (1.0 - 1e-6));
	}
	CHECK_EQ(leaf_splats, uint64_t(g.size()));
	CHECK(grouped > 0u);
	// Interior overhead within the ADR's 35% bound (test 3).
	CHECK(tree.interior_splat_count <= uint32_t(0.35 * g.size()));
	MESSAGE(vformat("fixture 40k: %d nodes, height %d, interior overhead %.1f%%", tree.nodes.size(), tree.nodes[0].height,
			100.0 * tree.interior_splat_count / g.size()));

	// Deterministic: a second bake is byte-identical in table and payload.
	gs_hlod::BakeResult again;
	if (!(hlod_bake(g, 2048u, again))) {
		FAIL("hlod_bake(g, 2048u, again)");
		return;
	}
	if ((again.tree.nodes.size()) != (tree.nodes.size())) {
		FAIL("again.tree.nodes.size() == tree.nodes.size()");
		return;
	}
	bool same = true;
	for (uint32_t i = 0; i < tree.nodes.size(); i++) {
		same = same && hlod_nodes_equal(tree.nodes[i], again.tree.nodes[i]);
	}
	CHECK(same);
	if ((again.tree.interior_gaussians.size()) != (tree.interior_gaussians.size())) {
		FAIL("again.tree.interior_gaussians.size() == tree.interior_gaussians.size()");
		return;
	}
	CHECK(memcmp(again.tree.interior_gaussians.ptr(), tree.interior_gaussians.ptr(), sizeof(Gaussian) * tree.interior_gaussians.size()) == 0);
	CHECK(memcmp(again.leaf_gaussians.ptr(), result.leaf_gaussians.ptr(), sizeof(Gaussian) * g.size()) == 0);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: flat content subdivides as a quadtree, volume as an octree") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> ground, crown;
	hlod_make_fixture(30000u, 0u, ground);
	hlod_make_fixture(0u, 30000u, crown);
	// Centre the 4 m crown on (8, 8, 8), a corner shared by eight lattice cells, so the volume
	// straddles midplanes in all three axes.
	for (uint32_t i = 0; i < crown.size(); i++) {
		crown[i].position.y += 5.0f;
	}
	gs_hlod::BakeResult flat, volume;
	if (!(hlod_bake(ground, 1024u, flat))) {
		FAIL("hlod_bake(ground, 1024u, flat)");
		return;
	}
	if (!(hlod_bake(crown, 1024u, volume))) {
		FAIL("hlod_bake(crown, 1024u, volume)");
		return;
	}
	// Ground lies in |y| < 2^-6 m: below the root, every octant split puts it on one side of the
	// y midplane, so no cell (other than the origin-centred root, which straddles y = 0) has
	// more than four occupied octants.
	uint32_t flat_max_children = 0u, volume_max_children = 0u;
	for (uint32_t i = 1; i < flat.tree.nodes.size(); i++) {
		const GaussianSplatHlodNode &n = flat.tree.nodes[i];
		if ((n.flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf)) == 0u && n.child_count > 0u) {
			uint32_t octant_children = 0u;
			for (uint32_t c = n.first_child; c < n.first_child + n.child_count; c++) {
				octant_children += (flat.tree.nodes[c].flags & gs_hlod::kNodeFlagGroupedLeaf) == 0u ? 1u : 0u;
			}
			flat_max_children = MAX(flat_max_children, octant_children);
		}
	}
	for (uint32_t i = 0; i < volume.tree.nodes.size(); i++) {
		volume_max_children = MAX(volume_max_children, volume.tree.nodes[i].child_count);
	}
	CHECK(flat_max_children <= 4u);
	CHECK(flat_max_children >= 2u);
	CHECK(volume_max_children > 4u);
	// The crown (centred at (8, 8, 8)) does not straddle the origin planes: a plain lattice root,
	// the smallest lattice cell holding [6, 10]^3, which is [0, 16)^3.
	CHECK((volume.tree.nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) == 0u);
	CHECK(volume.tree.nodes[0].cell.e == 4);
	CHECK(volume.tree.nodes[0].child_count == 8u);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: 40,000 coincident centres become an index-split group, no node over 16,384") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(0u, 40000u, g);
	for (uint32_t i = 0; i < g.size(); i++) {
		g[i].position = Vector3(5.25f, 1.5f, -2.0f); // bit-identical centres
	}
	gs_hlod::BakeResult result;
	String error;
	if (!(hlod_bake(g, gs_hlod::kMaxNodeSplats, result, &error))) {
		FAIL(error.utf8().get_data());
		return;
	}
	const GaussianSplatHlodTree &tree = result.tree;
	String reason;
	CHECK_MESSAGE(gs_hlod_validate_tree(tree, &reason), reason.utf8().get_data());
	uint32_t split_leaves = 0u;
	for (uint32_t i = 0; i < tree.nodes.size(); i++) {
		CHECK(tree.nodes[i].payload_count <= gs_hlod::kMaxNodeSplats);
		if (tree.nodes[i].child_count == 0u) {
			CHECK((tree.nodes[i].flags & gs_hlod::kNodeFlagSplitByIndex) != 0u);
			CHECK(tree.nodes[i].cell == tree.nodes[0].cell);
			split_leaves++;
		}
	}
	CHECK_EQ(split_leaves, 3u); // ceil(40,000 / 16,384)
	CHECK_EQ(tree.nodes[0].child_count, 3u);
	// Index order inside the group: (Morton code, source index) -> source index for identical centres.
	CHECK_EQ(result.leaf_source_index.size(), 40000u);

	// More chunks than eight children: intermediate nodes on the same cell keep fan-out <= 8.
	LocalVector<Gaussian> many;
	hlod_make_fixture(0u, 12000u, many);
	for (uint32_t i = 0; i < many.size(); i++) {
		many[i].position = Vector3(-3.0f, 0.5f, 7.0f);
	}
	gs_hlod::BakeResult grouped;
	if (!(hlod_bake(many, 1000u, grouped))) {
		FAIL("hlod_bake(many, 1000u, grouped)");
		return;
	}
	CHECK_MESSAGE(gs_hlod_validate_tree(grouped.tree, &reason), reason.utf8().get_data());
	uint32_t leaves = 0u;
	for (uint32_t i = 0; i < grouped.tree.nodes.size(); i++) {
		CHECK(grouped.tree.nodes[i].child_count <= gs_hlod::kMaxChildren);
		leaves += grouped.tree.nodes[i].child_count == 0u ? 1u : 0u;
	}
	CHECK_EQ(leaves, 12u);
	CHECK(grouped.tree.nodes[0].height >= 2u);
}

namespace TestGaussianSplatHlod {

// Depth of each node (root = 0).
inline void hlod_depths(const GaussianSplatHlodTree &p_tree, LocalVector<uint32_t> &r_depth) {
	r_depth.resize(p_tree.nodes.size());
	for (uint32_t i = 0; i < p_tree.nodes.size(); i++) {
		r_depth[i] = i == 0u ? 0u : r_depth[p_tree.nodes[i].parent] + 1u;
	}
}

// True when p_node of tree A has an identical counterpart in tree B: same cell, flags, size,
// error, bounds and payload bytes (leaf payloads compared as the source splats they hold).
inline bool hlod_node_survives(const gs_hlod::BakeResult &p_a, const GaussianSplatHlodNode &p_node, const gs_hlod::BakeResult &p_b) {
	for (uint32_t j = 0; j < p_b.tree.nodes.size(); j++) {
		const GaussianSplatHlodNode &nb = p_b.tree.nodes[j];
		if (!(nb.cell == p_node.cell) || nb.flags != p_node.flags || nb.payload_count != p_node.payload_count ||
				nb.geometric_error != p_node.geometric_error || !(nb.aabb_min == p_node.aabb_min) || !(nb.aabb_max == p_node.aabb_max)) {
			continue;
		}
		if (p_node.child_count == 0u) {
			if (memcmp(p_a.leaf_gaussians.ptr() + p_node.payload_first, p_b.leaf_gaussians.ptr() + nb.payload_first,
						sizeof(Gaussian) * p_node.payload_count) == 0) {
				return true;
			}
		} else if (memcmp(p_a.tree.interior_gaussians.ptr() + (p_node.payload_first - p_a.tree.leaf_splat_count),
						   p_b.tree.interior_gaussians.ptr() + (nb.payload_first - p_b.tree.leaf_splat_count),
						   sizeof(Gaussian) * p_node.payload_count) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace TestGaussianSplatHlod

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: the lattice is stable, adding a splat changes only the cells on its path") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> base;
	hlod_make_fixture(24000u, 16000u, base);
	gs_hlod::BakeResult a;
	if (!hlod_bake(base, 2048u, a)) {
		FAIL("base bake");
		return;
	}
	// The fixture spans [-16, 16) x [-0.016, 5] x [-16, 16): origin-centred root cube [-16, 16)^3.
	CHECK((a.tree.nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u);

	// Case 1: the new splat lies OUTSIDE the data bounds (y = 7 > 5) but inside the root cube. A
	// split at the data's bounding-box midpoint (the withdrawn prototype rule) moves split planes
	// everywhere; the lattice moves none. Case 2: inside the data bounds.
	const Vector3 additions[2] = { Vector3(-11.3f, 7.0f, 6.7f), Vector3(-11.3f, 0.002f, 6.7f) };
	for (int which = 0; which < 2; which++) {
		CAPTURE(which);
		LocalVector<Gaussian> edited = base;
		Gaussian extra = base[123];
		extra.position = additions[which];
		edited.push_back(extra);
		gs_hlod::BakeResult b;
		if (!hlod_bake(edited, 2048u, b)) {
			FAIL("edited bake");
			return;
		}
		CHECK(a.tree.nodes[0].cell == b.tree.nodes[0].cell);
		CHECK(a.tree.nodes[0].flags == b.tree.nodes[0].flags);
		// Every node off the new splat's path must exist unchanged in the edited bake.
		uint32_t compared = 0u, skipped = 0u, survived = 0u;
		for (uint32_t i = 0; i < a.tree.nodes.size(); i++) {
			const GaussianSplatHlodNode &na = a.tree.nodes[i];
			double lo[3], hi[3];
			hlod_cell_region(a.tree, na, lo, hi);
			const Vector3 &p = extra.position;
			if (p.x >= lo[0] && p.x < hi[0] && p.y >= lo[1] && p.y < hi[1] && p.z >= lo[2] && p.z < hi[2]) {
				skipped++;
				continue;
			}
			compared++;
			survived += hlod_node_survives(a, na, b) ? 1u : 0u;
		}
		CHECK_EQ(survived, compared);
		CHECK(compared > 10u);
		CHECK(skipped >= 2u); // the root and at least one cell under it
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: growing the root re-addresses only the root's direct leaves") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> base;
	hlod_make_fixture(24000u, 16000u, base);
	gs_hlod::BakeResult a;
	if (!hlod_bake(base, 2048u, a)) {
		FAIL("base bake");
		return;
	}
	// A splat outside the root cube [-16, 16)^3: the origin-centred root grows to [-32, 32)^3.
	LocalVector<Gaussian> grown = base;
	Gaussian extra = base[7];
	extra.position = Vector3(3.0f, 20.0f, -2.0f);
	grown.push_back(extra);
	gs_hlod::BakeResult b;
	if (!hlod_bake(grown, 2048u, b)) {
		FAIL("grown bake");
		return;
	}
	CHECK(b.tree.nodes[0].cell.e == a.tree.nodes[0].cell.e + 1);
	// Every node at depth >= 2 is untouched (its cell, bounds and payload bytes). Depth-1 interior
	// nodes keep their address as lattice cells too (floor_half(-1) = -1); only the old root's
	// direct LEAVES (and its grouped / split leaves, addressed by the cube) may change.
	LocalVector<uint32_t> depth;
	hlod_depths(a.tree, depth);
	uint32_t deep = 0u, deep_survived = 0u, interior1 = 0u, interior1_survived = 0u;
	for (uint32_t i = 0; i < a.tree.nodes.size(); i++) {
		const GaussianSplatHlodNode &na = a.tree.nodes[i];
		if (depth[i] >= 2u) {
			deep++;
			deep_survived += hlod_node_survives(a, na, b) ? 1u : 0u;
		} else if (depth[i] == 1u && na.child_count > 0u) {
			interior1++;
			interior1_survived += hlod_node_survives(a, na, b) ? 1u : 0u;
		}
	}
	CHECK(deep > 10u);
	CHECK_EQ(deep_survived, deep);
	CHECK(interior1 > 0u);
	CHECK_EQ(interior1_survived, interior1);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake: lattice roots, node-relative precision far from the origin, fail-closed input") {
	using namespace TestGaussianSplatHlod;
	// Content in one lattice cell: the root is the smallest such cell.
	double origin[3] = { 0.0, 0.0, 0.0 };
	const double lo[3] = { 8.5, 3.25, 8.5 };
	const double hi[3] = { 11.0, 3.75, 9.0 };
	GaussianSplatHlodCell cell;
	bool centred = true;
	if (!(gs_hlod::choose_root_cell(origin, lo, hi, cell, centred))) {
		FAIL("gs_hlod::choose_root_cell(origin, lo, hi, cell, centred)");
		return;
	}
	CHECK_FALSE(centred);
	CHECK(cell.e == 2); // [8, 12) x [0, 4) x [8, 12)
	CHECK(cell.ix == 2);
	CHECK(cell.iy == 0);
	CHECK(cell.iz == 2);
	// Straddling the origin plane x = 0: no lattice cell holds it; origin-centred cube instead.
	const double lo2[3] = { -0.5, 0.25, 0.25 };
	const double hi2[3] = { 0.5, 0.75, 0.75 };
	if (!(gs_hlod::choose_root_cell(origin, lo2, hi2, cell, centred))) {
		FAIL("gs_hlod::choose_root_cell(origin, lo2, hi2, cell, centred)");
		return;
	}
	CHECK(centred);
	CHECK(cell.e == 0); // [-1, 1)^3

	// 1,000 km from the origin: node-relative storage keeps the payload at cell resolution.
	LocalVector<Gaussian> far;
	hlod_make_fixture(4000u, 4000u, far);
	for (uint32_t i = 0; i < far.size(); i++) {
		far[i].position += Vector3(1.0e6f, 0.0f, 1.0e6f);
	}
	gs_hlod::BakeResult result;
	if (!(hlod_bake(far, 1024u, result))) {
		FAIL("hlod_bake(far, 1024u, result)");
		return;
	}
	for (uint32_t i = 0; i < result.tree.nodes.size(); i++) {
		const GaussianSplatHlodNode &n = result.tree.nodes[i];
		if (n.child_count == 0u) {
			continue;
		}
		// Interior payloads are relative to the cell centre: their magnitude is bounded by the
		// cell, not by the 10^6 m offset.
		const double reach = 0.5 * result.tree.node_cell_edge(n) * 1.8 + 1.0;
		for (uint32_t k = 0; k < n.payload_count; k++) {
			const Vector3 &p = result.tree.interior_gaussians[uint32_t(n.payload_first - result.tree.leaf_splat_count) + k].position;
			CHECK(std::fabs(p.x) < reach);
			CHECK(std::fabs(p.z) < reach);
		}
	}

	// Fail closed on non-finite input and on an empty input.
	LocalVector<Gaussian> bad;
	hlod_make_fixture(100u, 0u, bad);
	bad[50].scale.y = NAN;
	gs_hlod::BakeResult bad_result;
	String error;
	CHECK_FALSE(hlod_bake(bad, 64u, bad_result, &error));
	CHECK(error.contains("not finite"));
	LocalVector<Gaussian> none;
	CHECK_FALSE(hlod_bake(none, 64u, bad_result, &error));
	CHECK_FALSE(hlod_bake(bad, 0u, bad_result, &error));
	CHECK_FALSE(hlod_bake(bad, gs_hlod::kMaxNodeSplats + 1u, bad_result, &error));
}

// ------------------------------------------------------------------------------------------------
// Node-table validator (§7), on a hand-built tree
// ------------------------------------------------------------------------------------------------

namespace TestGaussianSplatHlod {

// Lattice cell (e = 2, 1, 0, 1): [4, 8) x [0, 4) x [4, 8), centre (6, 2, 6).
//   node 0 root      merged    payload [10, 12)  children 1..2
//   node 1 leaf A    octant (1, 2, 0, 2)          payload [0, 4)
//   node 2 merged B  octant (1, 3, 0, 2)          payload [12, 15)  children 3..4
//   node 3 leaf C    octant (0, 6, 0, 4)          payload [4, 7)
//   node 4 leaf D    grouped, cell of B           payload [7, 10)
inline void hlod_make_valid_tree(GaussianSplatHlodTree &r_tree) {
	r_tree = GaussianSplatHlodTree();
	r_tree.leaf_splat_count = 10u;
	r_tree.interior_splat_count = 5u;
	r_tree.nodes.resize(5);
	LocalVector<GaussianSplatHlodNode> &n = r_tree.nodes;
	auto set = [&](uint32_t i, int32_t e, int64_t x, int64_t y, int64_t z) {
		n[i].cell.e = e;
		n[i].cell.ix = x;
		n[i].cell.iy = y;
		n[i].cell.iz = z;
	};
	// Bounds are node-relative; give every node the same world box [6.2, 6.8]^3 nested
	// generously inside its parent's.
	auto box = [&](uint32_t i, double p_lo, double p_hi) {
		double c[3];
		r_tree.node_cell_center(n[i], c);
		for (int a = 0; a < 3; a++) {
			n[i].aabb_min[a] = float(p_lo - c[a]);
			n[i].aabb_max[a] = float(p_hi - c[a]);
		}
		n[i].radius = float((p_hi - p_lo) * 0.9);
	};
	set(0, 2, 1, 0, 1);
	n[0].first_child = 1u;
	n[0].child_count = 2u;
	n[0].payload_first = 10u;
	n[0].payload_count = 2u;
	n[0].geometric_error = 1.0f;
	n[0].height = 2u;
	n[0].flags = gs_hlod::NODE_KIND_MERGED;
	box(0, 4.0, 8.0);

	set(1, 1, 2, 0, 2);
	n[1].parent = 0u;
	n[1].payload_first = 0u;
	n[1].payload_count = 4u;
	box(1, 4.5, 5.5);

	set(2, 1, 3, 0, 2);
	n[2].parent = 0u;
	n[2].first_child = 3u;
	n[2].child_count = 2u;
	n[2].payload_first = 12u;
	n[2].payload_count = 3u;
	n[2].geometric_error = 0.5f;
	n[2].height = 1u;
	n[2].flags = gs_hlod::NODE_KIND_SELECTED;
	box(2, 6.0, 7.5);

	set(3, 0, 6, 0, 4);
	n[3].parent = 2u;
	n[3].payload_first = 4u;
	n[3].payload_count = 3u;
	box(3, 6.2, 6.8);

	set(4, 1, 3, 0, 2);
	n[4].parent = 2u;
	n[4].payload_first = 7u;
	n[4].payload_count = 3u;
	n[4].flags = gs_hlod::kNodeFlagGroupedLeaf;
	box(4, 6.1, 7.0);
}

} // namespace TestGaussianSplatHlod

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] node-table validator accepts a valid tree and rejects every broken invariant") {
	using namespace TestGaussianSplatHlod;
	GaussianSplatHlodTree valid;
	hlod_make_valid_tree(valid);
	String reason;
	if (!(gs_hlod_validate_tree(valid, &reason))) {
		FAIL(reason.utf8().get_data());
		return;
	}

	struct Mutation {
		const char *name;
		void (*apply)(GaussianSplatHlodTree &);
	};
	static const Mutation mutations[] = {
		{ "root has a parent", [](GaussianSplatHlodTree &t) { t.nodes[0].parent = 1u; } },
		{ "parent after child (cycle)", [](GaussianSplatHlodTree &t) { t.nodes[1].parent = 3u; } },
		{ "child points to another parent", [](GaussianSplatHlodTree &t) { t.nodes[3].parent = 0u; } },
		{ "child range past the table", [](GaussianSplatHlodTree &t) { t.nodes[2].child_count = 3u; } },
		{ "child range before the parent", [](GaussianSplatHlodTree &t) { t.nodes[2].first_child = 1u; } },
		{ "orphan (child count too small)", [](GaussianSplatHlodTree &t) { t.nodes[0].child_count = 1u; } },
		{ "too many children", [](GaussianSplatHlodTree &t) { t.nodes[0].child_count = 9u; } },
		{ "child cell not an octant", [](GaussianSplatHlodTree &t) { t.nodes[1].cell.ix = 4; } },
		{ "child cell two levels down", [](GaussianSplatHlodTree &t) { t.nodes[3].cell.e = -1; } },
		{ "grouped leaf not on the parent cell", [](GaussianSplatHlodTree &t) { t.nodes[4].cell.ix = 2; } },
		{ "grouped node with children", [](GaussianSplatHlodTree &t) { t.nodes[2].flags |= gs_hlod::kNodeFlagGroupedLeaf; } },
		{ "origin-centred flag below the root", [](GaussianSplatHlodTree &t) { t.nodes[3].flags |= gs_hlod::kNodeFlagOriginCentredRoot; } },
		{ "origin-centred root with an index", [](GaussianSplatHlodTree &t) { t.nodes[0].flags |= gs_hlod::kNodeFlagOriginCentredRoot; } },
		{ "negative overlap footprint", [](GaussianSplatHlodTree &t) { t.nodes[1].overlap_footprint = -1.0f; } },
		{ "NaN overlap footprint", [](GaussianSplatHlodTree &t) { t.nodes[1].overlap_footprint = NAN; } },
		{ "cell exponent below the 2^-16 m floor", [](GaussianSplatHlodTree &t) { t.nodes[3].cell.e = -17; } },
		{ "cell index beyond 2^52", [](GaussianSplatHlodTree &t) { t.nodes[1].cell.iz = int64_t(1) << 53; } },
		{ "empty payload", [](GaussianSplatHlodTree &t) { t.nodes[1].payload_count = 0u; } },
		{ "oversized payload", [](GaussianSplatHlodTree &t) { t.nodes[1].payload_count = gs_hlod::kMaxNodeSplats + 1u; } },
		{ "leaf payload overlap", [](GaussianSplatHlodTree &t) { t.nodes[3].payload_first = 3u; } },
		{ "leaf payload gap", [](GaussianSplatHlodTree &t) { t.nodes[4].payload_count = 2u; } },
		{ "leaf payload in the interior range", [](GaussianSplatHlodTree &t) { t.nodes[1].payload_first = 10u; } },
		{ "interior payload in the leaf range", [](GaussianSplatHlodTree &t) { t.nodes[0].payload_first = 8u; } },
		{ "payload past the end", [](GaussianSplatHlodTree &t) { t.nodes[2].payload_first = 14u; } },
		{ "payload offset overflow", [](GaussianSplatHlodTree &t) { t.nodes[2].payload_first = UINT64_MAX - 1u; } },
		{ "interior count does not match", [](GaussianSplatHlodTree &t) { t.interior_splat_count = 6u; } },
		{ "leaf count does not match", [](GaussianSplatHlodTree &t) { t.leaf_splat_count = 11u; } },
		{ "more nodes than splats", [](GaussianSplatHlodTree &t) { t.leaf_splat_count = 2u; t.interior_splat_count = 2u; } },
		{ "leaf with interior kind", [](GaussianSplatHlodTree &t) { t.nodes[1].flags = gs_hlod::NODE_KIND_MERGED; } },
		{ "interior with leaf kind", [](GaussianSplatHlodTree &t) { t.nodes[2].flags = gs_hlod::NODE_KIND_LEAF; } },
		{ "reserved kind", [](GaussianSplatHlodTree &t) { t.nodes[0].flags = gs_hlod::NODE_KIND_RESERVED; } },
		{ "unknown flag bit", [](GaussianSplatHlodTree &t) { t.nodes[0].flags |= 0x100u; } },
		{ "leaf with geometric error", [](GaussianSplatHlodTree &t) { t.nodes[1].geometric_error = 0.1f; } },
		{ "interior without geometric error", [](GaussianSplatHlodTree &t) { t.nodes[2].geometric_error = 0.0f; } },
		{ "non-monotone error", [](GaussianSplatHlodTree &t) { t.nodes[2].geometric_error = 2.0f; } },
		{ "NaN bound", [](GaussianSplatHlodTree &t) { t.nodes[3].aabb_max.y = NAN; } },
		{ "infinite radius", [](GaussianSplatHlodTree &t) { t.nodes[0].radius = INFINITY; } },
		{ "negative radius", [](GaussianSplatHlodTree &t) { t.nodes[0].radius = -1.0f; } },
		{ "tiny positive leaf radius", [](GaussianSplatHlodTree &t) { t.nodes[1].radius = 0.001f; } },
		{ "zero radius for nonzero bounds", [](GaussianSplatHlodTree &t) { t.nodes[1].radius = 0.0f; } },
		{ "parent sphere excludes a child sphere", [](GaussianSplatHlodTree &t) { t.nodes[0].radius = 2.0f; } },
		{ "inverted AABB", [](GaussianSplatHlodTree &t) { t.nodes[4].aabb_min.x = t.nodes[4].aabb_max.x + 1.0f; } },
		{ "child outside parent", [](GaussianSplatHlodTree &t) { t.nodes[3].aabb_max.x += 2.0f; } },
		{ "wrong height", [](GaussianSplatHlodTree &t) { t.nodes[0].height = 1u; } },
		{ "leaf with height", [](GaussianSplatHlodTree &t) { t.nodes[1].height = 1u; } },
		{ "negative error", [](GaussianSplatHlodTree &t) { t.nodes[0].error_chosen = -0.5f; } },
		{ "non-finite origin", [](GaussianSplatHlodTree &t) { t.origin[1] = NAN; } },
		{ "bake rule version 0", [](GaussianSplatHlodTree &t) { t.bake_rule_version = 0u; } },
		{ "instance without a top-level tree", [](GaussianSplatHlodTree &t) { t.instances.push_back(GaussianSplatHlodInstance()); } },
	};
	for (const Mutation &m : mutations) {
		GaussianSplatHlodTree tree;
		hlod_make_valid_tree(tree);
		m.apply(tree);
		String why;
		CHECK_MESSAGE(!gs_hlod_validate_tree(tree, &why), m.name);
		CHECK_MESSAGE(!why.is_empty(), m.name);
	}

	// Instances: a valid top-level tree (one leaf) with one instance is accepted; a reference to a
	// missing node or a non-finite transform is not.
	GaussianSplatHlodTree with_instance;
	hlod_make_valid_tree(with_instance);
	GaussianSplatHlodNode top;
	top.cell = with_instance.nodes[0].cell;
	with_instance.top_level_nodes.push_back(top);
	GaussianSplatHlodInstance inst;
	inst.instance_key = 42u;
	inst.asset_uid = 7;
	with_instance.instances.push_back(inst);
	CHECK_MESSAGE(gs_hlod_validate_tree(with_instance, &reason), reason.utf8().get_data());
	with_instance.instances[0].top_level_node = 1u;
	CHECK_FALSE(gs_hlod_validate_tree(with_instance, &reason));
	with_instance.instances[0].top_level_node = 0u;
	with_instance.instances[0].origin[2] = INFINITY;
	CHECK_FALSE(gs_hlod_validate_tree(with_instance, &reason));

	GaussianSplatHlodTree empty;
	CHECK_FALSE(gs_hlod_validate_tree(empty, &reason));

	// One encoding per tree: a shared-cell child of an origin-centred root must carry the flag
	// (it names the root's cube), and a shared-cell child of a lattice cell must not.
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	gs_hlod::BakeResult baked;
	if (!hlod_bake(g, gs_hlod::kMaxNodeSplats, baked)) { // default leaves: small root octants group at the root
		FAIL("bake for the canonical-flag check");
		return;
	}
	CHECK_MESSAGE(gs_hlod_validate_tree(baked.tree, &reason), reason.utf8().get_data());
	// Widen the root's bounds (still valid) so the float rounding of the shifted child bounds
	// below cannot trip the containment check instead of the encoding rule.
	baked.tree.nodes[0].aabb_min -= Vector3(1.0f, 1.0f, 1.0f);
	baked.tree.nodes[0].aabb_max += Vector3(1.0f, 1.0f, 1.0f);
	CHECK_MESSAGE(gs_hlod_validate_tree(baked.tree, &reason), reason.utf8().get_data());
	bool found_shared = false;
	for (uint32_t i = 1; i < baked.tree.nodes.size(); i++) {
		GaussianSplatHlodNode &n = baked.tree.nodes[i];
		const bool shared = (n.flags & (gs_hlod::kNodeFlagSplitByIndex | gs_hlod::kNodeFlagGroupedLeaf)) != 0u;
		if (shared && n.parent == 0u) {
			found_shared = true;
			CHECK((n.flags & gs_hlod::kNodeFlagOriginCentredRoot) != 0u);
			// The other encoding: same address, no flag. Without the flag the address names the
			// lattice cell [O, O + 2^e), whose centre is half a cell away, so shift the
			// node-relative bounds to keep the world bounds identical: only the canonical-encoding
			// rule can reject this.
			const Vector3 keep_min = n.aabb_min;
			const Vector3 keep_max = n.aabb_max;
			const float half = float(std::ldexp(1.0, n.cell.e - 1));
			n.flags &= ~gs_hlod::kNodeFlagOriginCentredRoot;
			n.aabb_min -= Vector3(half, half, half);
			n.aabb_max -= Vector3(half, half, half);
			CHECK_FALSE(gs_hlod_validate_tree(baked.tree, &reason));
			n.flags |= gs_hlod::kNodeFlagOriginCentredRoot;
			n.aabb_min = keep_min;
			n.aabb_max = keep_max;
			break;
		}
	}
	CHECK(found_shared);
}

// ------------------------------------------------------------------------------------------------
// .gsplatworld v2 (§7): round trip, v1 compatibility, malformed files, importer
// ------------------------------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] v2 round trip: streamable, resident and compressed; tree-less worlds stay v1") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	// More than one default leaf, placed ~1 km from the origin: leaves sit in lattice cells whose
	// centres are far from (0, 0, 0), so node-relative storage differs from absolute positions
	// and a reader that forgot a leaf's frame would be off by hundreds of metres.
	hlod_make_fixture(24000u, 16000u, g);
	for (uint32_t i = 0; i < g.size(); i++) {
		g[i].position += Vector3(1000.0f, 2.0f, -500.0f);
	}
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	const AABB user_bounds(Vector3(984, 1, -516), Vector3(32, 7, 32));
	world->set_bounds(user_bounds);
	Dictionary meta;
	meta["note"] = "hlod round trip";
	world->set_metadata(meta);

	// A tree-less world keeps the v1 layout (version 1 on disk).
	const String v1_path = hlod_temp_path("v1");
	ResourceFormatSaverGaussianSplatWorld saver;
	if (!(saver.save(world, v1_path) == OK)) {
		FAIL("saver.save(world, v1_path) == OK");
		return;
	}
	{
		Ref<FileAccess> f = FileAccess::open(v1_path, FileAccess::READ);
		if (!(f.is_valid())) {
			FAIL("f.is_valid()");
			return;
		}
		f->seek(4);
		CHECK_EQ(f->get_32(), 1u);
	}

	if (!(world->bake_hlod() == OK)) {
		FAIL("world->bake_hlod() == OK");
		return;
	}
	if (!(world->has_hlod_tree())) {
		FAIL("world->has_hlod_tree()");
		return;
	}
	const GaussianSplatHlodTree &tree = world->get_hlod_tree();
	uint32_t leaf_nodes = 0u;
	for (uint32_t i = 0; i < tree.nodes.size(); i++) {
		leaf_nodes += tree.nodes[i].child_count == 0u ? 1u : 0u;
	}
	CHECK_EQ(uint32_t(world->get_chunk_count()), leaf_nodes);
	const Dictionary info = world->get_hlod_info();
	CHECK(bool(info["has_hlod"]));
	CHECK(int64_t(info["leaf_splat_count"]) == int64_t(g.size()));

	CHECK((tree.nodes[0].flags & gs_hlod::kNodeFlagOriginCentredRoot) == 0u);
	if (tree.nodes.size() < 3u) {
		FAIL("the round-trip world must have more than one leaf");
		return;
	}
	LocalVector<Gaussian> baked_leaf;
	LocalVector<Vector3> sh_unused;
	uint32_t f1 = 0, f2 = 0;
	if (!(world->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), baked_leaf, sh_unused, f1, f2))) {
		FAIL("world->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), baked_leaf, sh_unused, f1, f2)");
		return;
	}

	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 0; mode < 3; mode++) {
		CAPTURE(mode);
		const String path = hlod_temp_path(vformat("rt%d", mode));
		Error save_err = ERR_BUG;
		if (mode == 0) {
			save_err = saver.save(world, path);
		} else if (mode == 1) {
			save_err = saver.save_resident_uncompressed(world, path);
		} else {
			save_err = saver.save_resident_compressed(world, path);
		}
		if (!(save_err == OK)) {
			FAIL("save_err == OK");
			return;
		}
		{
			Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
			if (!(f.is_valid())) {
				FAIL("f.is_valid()");
				return;
			}
			f->seek(4);
			CHECK_EQ(f->get_32(), 2u);
		}
		Error load_err = ERR_BUG;
		Ref<GaussianSplatWorld> loaded = loader.load(path, "", &load_err);
		if (!(load_err == OK)) {
			FAIL("load_err == OK");
			return;
		}
		if (!(loaded.is_valid())) {
			FAIL("loaded.is_valid()");
			return;
		}
		if (!(loaded->has_hlod_tree())) {
			FAIL("loaded->has_hlod_tree()");
			return;
		}
		CHECK(loaded->get_metadata()["note"] == Variant("hlod round trip"));
		{
			// The header keeps the user's bounds, widened just enough to enclose the validated root.
			// These user bounds start at x = 984 m, inside the splats' three-sigma support.
			double lo[3], hi[3];
			hlod_root_extent(tree, lo, hi);
			const AABB user = user_bounds;
			const AABB got = loaded->get_bounds();
			CHECK_FALSE(hlod_aabb_encloses(user, lo, hi));
			CHECK(hlod_aabb_encloses(got, lo, hi));
			// bake_hlod() already published these bounds on the live world; the saver adds nothing.
			CHECK(got == world->get_bounds());
			for (int a = 0; a < 3; a++) {
				CAPTURE(a);
				const double want_lo = MIN(lo[a], double(user.position[a]));
				const double want_hi = MAX(hi[a], double(user.position[a]) + double(user.size[a]));
				const double begin = double(got.position[a]);
				const double end = begin + double(got.size[a]);
				CHECK(begin <= want_lo);
				CHECK(end >= want_hi);
				CHECK(begin >= want_lo - 4.0 * FLT_EPSILON * MAX(1.0, std::abs(want_lo)));
				CHECK(end <= want_hi + 4.0 * FLT_EPSILON * MAX(1.0, std::abs(want_hi)));
			}
		}
		const GaussianSplatHlodTree &t2 = loaded->get_hlod_tree();
		if ((t2.nodes.size()) != (tree.nodes.size())) {
			FAIL("t2.nodes.size() == tree.nodes.size()");
			return;
		}
		bool nodes_same = true;
		for (uint32_t i = 0; i < tree.nodes.size(); i++) {
			nodes_same = nodes_same && hlod_nodes_equal(tree.nodes[i], t2.nodes[i]);
		}
		CHECK(nodes_same);
		for (int a = 0; a < 3; a++) {
			CHECK(t2.origin[a] == tree.origin[a]);
		}
		// Interior payload: bit-exact (it is node-relative in memory and on disk).
		LocalVector<Gaussian> interior;
		LocalVector<Vector3> interior_sh;
		if (!(t2.read_interior_payload(interior, interior_sh) == OK)) {
			FAIL("t2.read_interior_payload(interior, interior_sh) == OK");
			return;
		}
		if ((interior.size()) != (tree.interior_gaussians.size())) {
			FAIL("interior.size() == tree.interior_gaussians.size()");
			return;
		}
		CHECK(memcmp(interior.ptr(), tree.interior_gaussians.ptr(), sizeof(Gaussian) * interior.size()) == 0);
		CHECK_EQ(mode == 0, loaded->is_streamable_payload());
		// Leaf payload: through the file-backed source (mode 0) or resident data, absolute
		// positions within float32 resolution of the cell; every other field bit-exact.
		LocalVector<Gaussian> leaf;
		LocalVector<Vector3> leaf_sh;
		uint32_t a1 = 0, a2 = 0;
		if (mode == 0) {
			if (!(loaded->get_chunk_payload_source()->capture_chunk_snapshot(0, g.size(), leaf, leaf_sh, a1, a2))) {
				FAIL("loaded->get_chunk_payload_source()->capture_chunk_snapshot(0, g.size(), leaf, leaf_sh, a1, a2)");
				return;
			}
		} else {
			if (!(loaded->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), leaf, leaf_sh, a1, a2))) {
				FAIL("loaded->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), leaf, leaf_sh, a1, a2)");
				return;
			}
		}
		if ((leaf.size()) != (baked_leaf.size())) {
			FAIL("leaf.size() == baked_leaf.size()");
			return;
		}
		// Absolute float32 -> node-relative float32 -> absolute float32: the stored value is exact
		// to the float32 resolution of the cell (edge x 2^-24, ADR §6.2) and the conversion back
		// adds one rounding of the absolute coordinate.
		uint32_t off_by_more = 0u;
		uint32_t moved = 0u;
		double worst_over_edge = 0.0;
		bool rest_same = true;
		for (uint32_t ni = 0; ni < t2.nodes.size(); ni++) {
			const GaussianSplatHlodNode &n = t2.nodes[ni];
			if (n.child_count != 0u) {
				continue;
			}
			const double edge = t2.node_cell_edge(n);
			for (uint32_t i = uint32_t(n.payload_first); i < uint32_t(n.payload_first) + n.payload_count && i < leaf.size(); i++) {
				for (int a = 0; a < 3; a++) {
					const double want = baked_leaf[i].position[a];
					const double tol = edge * std::ldexp(1.0, -23) + MAX(1.0, std::fabs(want)) * std::ldexp(1.0, -23);
					const double diff = std::fabs(double(leaf[i].position[a]) - want);
					off_by_more += diff > tol ? 1u : 0u;
					moved += diff > 0.0 ? 1u : 0u;
					worst_over_edge = MAX(worst_over_edge, diff / edge);
				}
				Gaussian x = leaf[i];
				x.position = baked_leaf[i].position;
				rest_same = rest_same && memcmp(&x, &baked_leaf[i], sizeof(Gaussian)) == 0;
			}
		}
		CHECK_EQ(off_by_more, 0u);
		const String moved_note = vformat("mode %d: %d of %d coordinates moved by the node-relative round trip; worst = ", mode, moved, leaf.size() * 3u) +
				String::num_scientific(worst_over_edge) + " x cell edge (2^-23 = 1.19e-07)";
		MESSAGE(moved_note);
		CHECK(rest_same);
		// The indexed read path applies the frames too.
		if (mode == 0) {
			const uint32_t picks[4] = { 0u, 17u, uint32_t(g.size() / 2u), uint32_t(g.size() - 1u) };
			LocalVector<Gaussian> picked;
			if (!(loaded->get_chunk_payload_source()->capture_indexed_chunk_snapshot(picks, 4u, picked, leaf_sh, a1, a2))) {
				FAIL("loaded->get_chunk_payload_source()->capture_indexed_chunk_snapshot(picks, 4u, picked, leaf_sh, a1, a2)");
				return;
			}
			for (int k = 0; k < 4; k++) {
				CHECK(picked[k].position.distance_to(baked_leaf[picks[k]].position) <= 1.0e-3f);
			}
		}
		// Re-saving a loaded world (file-backed interior included) gives the same tables.
		const String path2 = hlod_temp_path(vformat("rt%d_again", mode));
		if (!(saver.save(loaded, path2) == OK)) {
			FAIL("saver.save(loaded, path2) == OK");
			return;
		}
		Ref<GaussianSplatWorld> again = loader.load(path2, "", &load_err);
		if (!(again.is_valid())) {
			FAIL("again.is_valid()");
			return;
		}
		if ((again->get_hlod_tree().nodes.size()) != (tree.nodes.size())) {
			FAIL("again->get_hlod_tree().nodes.size() == tree.nodes.size()");
			return;
		}
		bool again_same = true;
		for (uint32_t i = 0; i < tree.nodes.size(); i++) {
			again_same = again_same && hlod_nodes_equal(tree.nodes[i], again->get_hlod_tree().nodes[i]);
		}
		CHECK(again_same);
		again.unref();
		loaded.unref();
		DirAccess::remove_absolute(path2);
		DirAccess::remove_absolute(path);
	}

	// v1 still loads (tree-less).
	Error v1_err = ERR_BUG;
	Ref<GaussianSplatWorld> v1 = loader.load(v1_path, "", &v1_err);
	CHECK(v1_err == OK);
	if (!(v1.is_valid())) {
		FAIL("v1.is_valid()");
		return;
	}
	CHECK_FALSE(v1->has_hlod_tree());
	v1.unref();
	DirAccess::remove_absolute(v1_path);

	// A new payload drops the tree (its leaves would index the old one), and saving then writes v1.
	world->set_gaussian_data(world->get_gaussian_data());
	CHECK_FALSE(world->has_hlod_tree());
	CHECK_EQ(world->get_chunk_count(), 0);

	// The 2D flag (set by the PLY loader for any PLY with normals) bakes and round-trips.
	LocalVector<Gaussian> small;
	hlod_make_fixture(2000u, 1000u, small);
	Ref<GaussianSplatWorld> flat_world = hlod_make_world(small);
	flat_world->get_gaussian_data()->set_2d_mode(true);
	if (flat_world->bake_hlod() != OK) {
		FAIL("a 2D-flagged world must bake");
		return;
	}
	CHECK(flat_world->get_2d_mode());
	const String flat_path = hlod_temp_path("flag2d");
	if (saver.save(flat_world, flat_path) != OK) {
		FAIL("saving a baked 2D-flagged world");
		return;
	}
	Error flat_err = ERR_BUG;
	Ref<GaussianSplatWorld> flat_loaded = loader.load_resident(flat_path, &flat_err);
	if (flat_loaded.is_null()) {
		FAIL("loading a baked 2D-flagged world");
		return;
	}
	CHECK(flat_loaded->has_hlod_tree());
	CHECK(flat_loaded->get_2d_mode());
	flat_loaded.unref();
	DirAccess::remove_absolute(flat_path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] v2 loader rejects malformed or oversized files") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	// More than one default (16,384-splat) leaf, so the table has a root and children.
	hlod_make_fixture(24000u, 16000u, g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (!(world->bake_hlod() == OK)) {
		FAIL("world->bake_hlod() == OK");
		return;
	}
	const String valid_path = hlod_temp_path("malformed_valid");
	ResourceFormatSaverGaussianSplatWorld saver;
	if (!(saver.save(world, valid_path) == OK)) {
		FAIL("saver.save(world, valid_path) == OK");
		return;
	}
	const PackedByteArray valid = hlod_read_file(valid_path);
	if (!(valid.size() > 184)) {
		FAIL("valid.size() > 184");
		return;
	}

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<Resource> sanity = loader.load(valid_path, "", &err);
	if (!(err == OK)) {
		FAIL("err == OK");
		return;
	}
	if (!(sanity.is_valid())) {
		FAIL("sanity.is_valid()");
		return;
	}
	sanity.unref();

	auto u32_at = [](const PackedByteArray &p_b, int64_t p_off) {
		uint32_t v;
		memcpy(&v, p_b.ptr() + p_off, 4);
		return v;
	};
	auto u64_at = [](const PackedByteArray &p_b, int64_t p_off) {
		uint64_t v;
		memcpy(&v, p_b.ptr() + p_off, 8);
		return v;
	};
	const uint64_t node_table = u64_at(valid, 136);
	const uint32_t node_count = u32_at(valid, 144);
	if (node_count < 3u || node_table + uint64_t(node_count) * 128u > uint64_t(valid.size())) {
		FAIL("the fixture must bake to a root with children");
		return;
	}
	const int64_t node1 = int64_t(node_table) + 128; // a child of the root

	struct Patch {
		const char *name;
		int64_t offset;
		int width; // bytes
		uint64_t value;
	};
	const Patch patches[] = {
		{ "HLOD flag cleared", 8, 4, u32_at(valid, 8) & ~(1u << 6u) },
		{ "unknown header flag", 8, 4, u32_at(valid, 8) | (1u << 20u) },
		{ "v1 version with the HLOD flag", 4, 4, 1u },
		{ "chunk count in a v2 file", 52, 4, 3u },
		{ "chunk table offset in a v2 file", 72, 8, 200u },
		{ "leaf count does not match the nodes", 12, 4, u32_at(valid, 12) - 1u },
		{ "interior count overflows 32 bits", 132, 4, 0xFFFFFFF0u },
		{ "interior count does not match", 132, 4, u32_at(valid, 132) + 1u },
		{ "bake rule version 0", 128, 4, 0u },
		{ "reserved header word", 172, 4, 1u },
		{ "reserved header qword", 176, 8, 1u },
		{ "node count 0", 144, 4, 0u },
		{ "node count larger than the payload", 144, 4, 0x7FFFFFFFu },
		{ "node table past the end", 136, 8, uint64_t(valid.size()) - 64u },
		{ "node table inside the header", 136, 8, 16u },
		{ "instance count without a table", 168, 4, 1u },
		{ "top-level nodes without a table", 148, 4, 2u },
		{ "gaussian offset inside the header", 56, 8, 120u },
		{ "origin is NaN", 104, 8, 0x7FF8000000000000ull },
		{ "node: cell exponent below 2^-16", node1 + 0, 1, uint64_t(uint8_t(int8_t(-17))) },
		{ "node: nine children", int64_t(node_table) + 1, 1, 9u },
		{ "node: unknown flag", node1 + 4, 4, 0x80u },
		{ "node: parent after itself", node1 + 72, 4, node_count - 1u },
		{ "node: payload count 0", node1 + 88, 4, 0u },
		{ "node: payload count over 16,384", node1 + 88, 4, 16385u },
		{ "node: payload start past the end", node1 + 80, 8, 0xFFFFFFFFFFull },
		{ "node: NaN bound", node1 + 32, 4, 0x7FC00000u },
		{ "node: tiny positive radius", node1 + 56, 4, 0x3A83126Fu },
		{ "node: zero radius", node1 + 56, 4, 0u },
		{ "node: root error negative", int64_t(node_table) + 60, 4, 0xBF800000u },
	};
	int index = 0;
	for (const Patch &patch : patches) {
		PackedByteArray bytes = valid;
		if (patch.offset + patch.width > bytes.size()) {
			FAIL(patch.name);
			continue;
		}
		memcpy(bytes.ptrw() + patch.offset, &patch.value, patch.width);
		const String path = hlod_temp_path(vformat("malformed_%d", index++));
		if (!(hlod_write_file(path, bytes))) {
			FAIL("hlod_write_file(path, bytes)");
			return;
		}
		Error load_err = OK;
		Ref<Resource> result = loader.load(path, "", &load_err);
		CHECK_MESSAGE(!result.is_valid(), patch.name);
		CHECK_MESSAGE((load_err == ERR_FILE_CORRUPT || load_err == ERR_OUT_OF_MEMORY), patch.name);
		DirAccess::remove_absolute(path);
	}
	// Truncations anywhere in the tables or payload.
	const int64_t cuts[] = { 150, 184 + 100, int64_t(node_table) + 50, int64_t(valid.size()) - 1 };
	for (int64_t cut : cuts) {
		PackedByteArray bytes = valid;
		bytes.resize(cut);
		const String path = hlod_temp_path(vformat("truncated_%d", int(cut)));
		if (!(hlod_write_file(path, bytes))) {
			FAIL("hlod_write_file(path, bytes)");
			return;
		}
		Error load_err = OK;
		CAPTURE(cut);
		CHECK_FALSE(loader.load(path, "", &load_err).is_valid());
		CHECK(load_err != OK);
		DirAccess::remove_absolute(path);
	}
	// Forced-resident loads go through the same validation.
	{
		PackedByteArray bytes = valid;
		const uint32_t zero = 0u;
		memcpy(bytes.ptrw() + node1 + 88, &zero, 4);
		const String path = hlod_temp_path("malformed_resident");
		if (!(hlod_write_file(path, bytes))) {
			FAIL("hlod_write_file(path, bytes)");
			return;
		}
		Error load_err = OK;
		CHECK_FALSE(loader.load_resident(path, &load_err).is_valid());
		CHECK(load_err == ERR_FILE_CORRUPT);
		DirAccess::remove_absolute(path);
	}
	DirAccess::remove_absolute(valid_path);
}

// The v2 header AABB becomes the world's bounds and, through GaussianSplatWorld3D, the engine's
// instance AABB. A finite header that is smaller than or disjoint from the validated root would
// let frustum culling hide valid splats, so the saver widens it and the loader refuses it.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] review-r3 header world bounds must enclose the validated root") {
	using namespace TestGaussianSplatHlod;
	auto encloses_aabb = [](const AABB &p_outer, const AABB &p_inner) {
		double lo[3], hi[3];
		for (int a = 0; a < 3; a++) {
			lo[a] = double(p_inner.position[a]);
			hi[a] = lo[a] + double(p_inner.size[a]);
		}
		return hlod_aabb_encloses(p_outer, lo, hi);
	};
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	LocalVector<Gaussian> g;
	hlod_make_needle_fixture(g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK) {
		FAIL("bake the needle producer");
		return;
	}
	double lo[3], hi[3];
	hlod_root_extent(world->get_hlod_tree(), lo, hi);
	// bake_hlod() publishes bounds that enclose the root on the live world, too.
	CHECK(hlod_aabb_encloses(world->get_bounds(), lo, hi));
	// Control: on this valid content GaussianData::get_aabb() misses the rotated three-sigma
	// support, so writing it verbatim reproduces the defect. Put it back as the world's bounds
	// (as a caller's set_bounds() can) so the saver below must widen it.
	world->set_bounds(world->get_gaussian_data()->get_aabb());
	CHECK_FALSE(hlod_aabb_encloses(world->get_bounds(), lo, hi));
	CHECK(hi[0] - lo[0] > 10.0);

	// Producer: every payload mode writes header bounds that enclose the root and the world's bounds.
	for (int mode = 1; mode <= 3; mode++) {
		const String path = hlod_temp_path("bounds_producer_r3");
		if (saver.save_with_payload_mode(world, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
			FAIL("save the needle producer");
			return;
		}
		for (bool resident : { false, true }) {
			CAPTURE(mode);
			CAPTURE(resident);
			Error err = ERR_BUG;
			Ref<GaussianSplatWorld> loaded;
			if (resident) {
				loaded = loader.load_resident(path, &err);
			} else {
				loaded = loader.load(path, "", &err);
			}
			if (loaded.is_null() || err != OK) {
				FAIL("the valid needle producer must load");
				DirAccess::remove_absolute(path);
				return;
			}
			CHECK(hlod_aabb_encloses(loaded->get_bounds(), lo, hi));
			CHECK(encloses_aabb(loaded->get_bounds(), world->get_bounds()));
		}
		DirAccess::remove_absolute(path);
	}

	// Malformed: only the header bounds change; the node table and payload stay valid.
	const String valid_path = hlod_temp_path("bounds_valid_r3");
	if (saver.save(world, valid_path) != OK) {
		FAIL("save the needle mutation base");
		return;
	}
	const PackedByteArray valid = hlod_read_file(valid_path);
	if (valid.size() < 184) {
		FAIL("the needle mutation base is shorter than the v2 header");
		return;
	}
	float pos[3], size[3];
	memcpy(pos, valid.ptr() + 28, sizeof(pos));
	memcpy(size, valid.ptr() + 40, sizeof(size));
	// The writer rounds the root's low x edge outward by at most one float step.
	CHECK(double(pos[0]) <= lo[0]);
	CHECK(double(std::nextafter(pos[0], INFINITY)) > lo[0]);
	struct BoundsCase {
		const char *name;
		int offset; // 28 + 4 * axis for position, 40 + 4 * axis for size
		float value;
		bool loads;
	};
	const BoundsCase cases[] = {
		{ "control: header widened beyond the root", 28, pos[0] - 100.0f, true },
		{ "NaN position", 28, NAN, false },
		{ "infinite position", 32, -INFINITY, false },
		{ "infinite size", 40, INFINITY, false },
		{ "negative size", 48, -1.0f, false },
		{ "zero size on the root's long axis", 40, 0.0f, false },
		{ "half the root's extent", 40, size[0] * 0.5f, false },
		{ "finite but disjoint from the root", 28, pos[0] + 1000.0f, false },
		// x is the axis where the header edge is the root's own edge (the world bounds are inside it).
		{ "low edge one float ULP inside the root", 28, std::nextafter(pos[0], INFINITY), false },
	};
#ifdef TOOLS_ENABLED
	Ref<ResourceImporterGSplatWorld> importer;
	importer.instantiate();
	HashMap<StringName, Variant> options;
	const String import_base = hlod_temp_path("bounds_import_r3");
#endif
	int index = 0;
	for (const BoundsCase &c : cases) {
		CAPTURE(c.name);
		PackedByteArray bytes = valid;
		memcpy(bytes.ptrw() + c.offset, &c.value, sizeof(float));
		if (c.offset == 28 && c.loads) {
			const float widened = size[0] + 200.0f;
			memcpy(bytes.ptrw() + 40, &widened, sizeof(float));
		}
		const String path = hlod_temp_path(vformat("bounds_r3_%d", index++));
		if (!hlod_write_file(path, bytes)) {
			FAIL("write the header-bounds mutation");
			return;
		}
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> streamed = loader.load(path, "", &err);
		CHECK(streamed.is_valid() == c.loads);
		CHECK(err == (c.loads ? OK : ERR_FILE_CORRUPT));
		streamed.unref();
		Ref<GaussianSplatWorld> resident = loader.load_resident(path, &err);
		CHECK(resident.is_valid() == c.loads);
		CHECK(err == (c.loads ? OK : ERR_FILE_CORRUPT));
		resident.unref();
#ifdef TOOLS_ENABLED
		CHECK((importer->import(ResourceUID::INVALID_ID, path, import_base, options, nullptr, nullptr, nullptr) == OK) == c.loads);
		DirAccess::remove_absolute(import_base + ".gsplatworld");
#endif
		DirAccess::remove_absolute(path);
	}
	DirAccess::remove_absolute(valid_path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] review-r2 rejects payloads inconsistent with node bounds in every storage mode") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(18000u, 2000u, g);
	LocalVector<Vector3> sh;
	sh.resize(g.size() * 12u);
	for (Vector3 &v : sh) {
		v = Vector3(0.125f, -0.25f, 0.5f);
	}
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	world->get_gaussian_data()->set_gaussian_payload(g, sh, 3u, 12u, false);
	if (world->bake_hlod() != OK || world->get_hlod_tree().interior_splat_count == 0u) {
		FAIL("producer must bake both leaf and interior payloads");
		return;
	}
	auto u64_at = [](const PackedByteArray &b, int64_t off) {
		uint64_t value;
		memcpy(&value, b.ptr() + off, sizeof(value));
		return value;
	};
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 1; mode <= 3; mode++) {
		const String path = hlod_temp_path("payload_review_r2");
		if (saver.save_with_payload_mode(world, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
			FAIL("save producer control");
			return;
		}
		const PackedByteArray valid = hlod_read_file(path);
		Error err = ERR_BUG;
		if (loader.load(path, "", &err).is_null() || err != OK) {
			FAIL("unmodified producer must load");
			return;
		}
		uint32_t flags;
		memcpy(&flags, valid.ptr() + 8, 4);
		const bool compressed = (flags & (1u << 4u)) != 0u;
		const uint64_t offset = u64_at(valid, 56);
		const uint64_t total = uint64_t(g.size()) + world->get_hlod_tree().interior_splat_count;
		PackedByteArray raw;
		raw.resize(total * sizeof(Gaussian));
		const uint64_t old_blob_size = compressed ? u64_at(valid, offset) : raw.size();
		if (compressed) {
			if (Compression::decompress(raw.ptrw(), raw.size(), valid.ptr() + offset + 8u, old_blob_size, Compression::MODE_GZIP) != raw.size()) {
				FAIL("decompress producer payload for mutation");
				return;
			}
		} else {
			memcpy(raw.ptrw(), valid.ptr() + offset, raw.size());
		}
#ifdef TOOLS_ENABLED
		Ref<ResourceImporterGSplatWorld> importer;
		importer.instantiate();
		HashMap<StringName, Variant> options;
		const String import_base = hlod_temp_path("payload_previous_r2");
		if (importer->import(ResourceUID::INVALID_ID, path, import_base, options, nullptr, nullptr, nullptr) != OK) {
			FAIL("establish previous valid import");
			return;
		}
		const PackedByteArray previous = hlod_read_file(import_base + ".gsplatworld");
#endif
		const size_t fields[] = { offsetof(Gaussian, position), offsetof(Gaussian, scale), offsetof(Gaussian, opacity),
			offsetof(Gaussian, sh_dc), offsetof(Gaussian, sh_1), offsetof(Gaussian, normal), offsetof(Gaussian, brush_axes) };
		for (int interior = 0; interior < 2; interior++) {
			for (uint32_t field = 0; field < std::size(fields) + 2u; field++) {
				if (interior && field == std::size(fields) + 1u) {
					continue; // Parent spheres also have independently checked child containment.
				}
				CAPTURE(mode);
				CAPTURE(interior);
				CAPTURE(field);
				PackedByteArray bytes = valid;
				PackedByteArray mutated = raw;
				const uint64_t first = interior ? g.size() : 0u;
				const float bad = field < 2u ? 1e20f : NAN;
				if (field < std::size(fields)) {
					memcpy(mutated.ptrw() + first * sizeof(Gaussian) + fields[field], &bad, sizeof(bad));
				}
				if (compressed) {
					PackedByteArray blob;
					blob.resize(Compression::get_max_compressed_buffer_size(mutated.size(), Compression::MODE_GZIP));
					const int64_t blob_size = Compression::compress(blob.ptrw(), mutated.ptr(), mutated.size(), Compression::MODE_GZIP);
					if (blob_size <= 0) {
						FAIL("recompress mutated payload");
						return;
					}
					const int64_t delta = blob_size - int64_t(old_blob_size);
					bytes.resize(valid.size() + delta);
					memcpy(bytes.ptrw(), valid.ptr(), offset);
					memcpy(bytes.ptrw() + offset, &blob_size, 8);
					memcpy(bytes.ptrw() + offset + 8u, blob.ptr(), blob_size);
					const uint64_t suffix = offset + 8u + old_blob_size;
					memcpy(bytes.ptrw() + suffix + delta, valid.ptr() + suffix, valid.size() - suffix);
					const int section_offsets[] = { 64, 72, 88, 136, 152, 160 };
					for (int header_offset : section_offsets) {
						const uint64_t old_offset = u64_at(valid, header_offset);
						if (old_offset >= suffix) {
							const uint64_t new_offset = old_offset + delta;
							memcpy(bytes.ptrw() + header_offset, &new_offset, 8);
						}
					}
				} else {
					memcpy(bytes.ptrw() + offset, mutated.ptr(), mutated.size());
				}
				if (field == std::size(fields)) {
					memcpy(bytes.ptrw() + u64_at(bytes, 64) + first * 12u * sizeof(Vector3), &bad, sizeof(bad));
				}
				if (field == std::size(fields) + 1u) {
					// Keep a structurally valid table, but shrink one sphere below its own
					// payload support. AABB-only validation must not pass this mutation.
					GaussianSplatHlodTree table = world->get_hlod_tree();
					uint32_t node_index = 0u;
					while (node_index < table.nodes.size() && table.nodes[node_index].payload_first != first) {
						node_index++;
					}
					if (node_index == table.nodes.size()) {
						FAIL("find referenced payload node");
						return;
					}
					GaussianSplatHlodNode &node = table.nodes[node_index];
					double half_extent = 0.0;
					for (int a = 0; a < 3; a++) {
						half_extent = MAX(half_extent, 0.5 * (double(node.aabb_max[a]) - double(node.aabb_min[a])));
					}
					node.radius = std::nextafter(float(half_extent), INFINITY);
					String reason;
					if (!gs_hlod_validate_tree(table, &reason)) {
						FAIL("sphere mutation must retain a valid node table");
						return;
					}
					memcpy(bytes.ptrw() + u64_at(bytes, 136) + uint64_t(node_index) * 128u + 56u, &node.radius, 4);
				}
				if (!hlod_write_file(path, bytes)) {
					FAIL("write mutated payload");
					return;
				}
				CHECK_FALSE(loader.load(path, "", &err).is_valid());
				CHECK(err == ERR_FILE_CORRUPT);
				CHECK_FALSE(loader.load_resident(path, &err).is_valid());
				CHECK(err == ERR_FILE_CORRUPT);
#ifdef TOOLS_ENABLED
				CHECK(importer->import(ResourceUID::INVALID_ID, path, import_base, options, nullptr, nullptr, nullptr) == ERR_FILE_CORRUPT);
				CHECK(bool(hlod_read_file(import_base + ".gsplatworld") == previous));
#endif
			}
		}
#ifdef TOOLS_ENABLED
		DirAccess::remove_absolute(import_base + ".gsplatworld");
#endif
		DirAccess::remove_absolute(path);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] review-r2 rejects invalid metadata rather than silently discarding it") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(30u, 10u, g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	Dictionary metadata;
	metadata["note"] = "valid dictionary control";
	world->set_metadata(metadata);
	if (world->bake_hlod() != OK) {
		FAIL("bake metadata control");
		return;
	}
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 1; mode <= 3; mode++) {
		const String path = hlod_temp_path("metadata_review_r2");
		if (saver.save_with_payload_mode(world, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
			FAIL("save metadata control");
			return;
		}
		const PackedByteArray valid = hlod_read_file(path);
		uint64_t offset, size;
		memcpy(&offset, valid.ptr() + 88, 8);
		memcpy(&size, valid.ptr() + 96, 8);
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> control = loader.load(path, "", &err);
		if (size < 4u || control.is_null() || err != OK || control->get_metadata() != metadata) {
			FAIL("producer dictionary must round-trip");
			return;
		}
		const char *invalid[] = { "{bad", "null", "[]", "42" };
		for (const char *json : invalid) {
			CAPTURE(mode);
			CAPTURE(json);
			PackedByteArray bytes = valid;
			memset(bytes.ptrw() + offset, ' ', size);
			memcpy(bytes.ptrw() + offset, json, strlen(json));
			if (!hlod_write_file(path, bytes)) {
				FAIL("write invalid metadata");
				return;
			}
			CHECK_FALSE(loader.load(path, "", &err).is_valid());
			CHECK(err == ERR_FILE_CORRUPT);
			CHECK_FALSE(loader.load_resident(path, &err).is_valid());
			CHECK(err == ERR_FILE_CORRUPT);
		}
		// Invalid UTF-8 inside a quoted value remains syntactically valid JSON if
		// a permissive decoder silently replaces it, so parsing alone is not enough.
		const int marker = String::utf8(reinterpret_cast<const char *>(valid.ptr() + offset), size).find("valid dictionary control");
		if (marker < 0) {
			FAIL("find producer metadata value");
			return;
		}
		PackedByteArray bytes = valid;
		bytes.ptrw()[offset + marker] = 0xffu;
		if (!hlod_write_file(path, bytes)) {
			FAIL("write invalid metadata UTF-8");
			return;
		}
		CHECK_FALSE(loader.load(path, "", &err).is_valid());
		CHECK(err == ERR_FILE_CORRUPT);
		CHECK_FALSE(loader.load_resident(path, &err).is_valid());
		CHECK(err == ERR_FILE_CORRUPT);
		DirAccess::remove_absolute(path);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] review-r3 save refuses metadata that cannot round-trip as JSON") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(30u, 10u, g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK) {
		FAIL("bake metadata round-trip control");
		return;
	}
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	const String path = hlod_temp_path("metadata_review_r3");
	// Control: finite floats (top level and nested) save and reload.
	Dictionary finite;
	finite["scale"] = 0.5;
	Array nested;
	nested.push_back(-2.25);
	finite["nested"] = nested;
	world->set_metadata(finite);
	if (saver.save(world, path) != OK) {
		FAIL("save finite metadata control");
		return;
	}
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> control = loader.load(path, "", &err);
	CHECK(err == OK);
	CHECK(control.is_valid());
	const PackedByteArray previous = hlod_read_file(path);
	control.unref();

	const double non_finite[] = { NAN, INFINITY, -INFINITY };
	for (double value : non_finite) {
		for (bool in_array : { false, true }) {
			CAPTURE(value);
			CAPTURE(in_array);
			Dictionary bad;
			if (in_array) {
				Array arr;
				arr.push_back(value);
				bad["nested"] = arr;
			} else {
				bad["scale"] = value;
			}
			world->set_metadata(bad);
			for (int mode = 1; mode <= 3; mode++) {
				CAPTURE(mode);
				ERR_PRINT_OFF;
				const Error save_err = saver.save_with_payload_mode(world, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode));
				ERR_PRINT_ON;
				CHECK(save_err == ERR_INVALID_DATA);
				// The refused save must not replace the previous, loadable file.
				CHECK(bool(hlod_read_file(path) == previous));
			}
		}
	}
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] review-r2 rejects finite doubles that overflow runtime node coordinates") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(30u, 10u, g);
	gs_hlod::BakeResult baked;
	if (!hlod_bake(g, gs_hlod::kMaxNodeSplats, baked)) {
		FAIL("bake runtime coordinate control");
		return;
	}
	String reason;
	const double origins[] = { DBL_MAX, double(FLT_MAX) * 2.0, -DBL_MAX };
	for (double origin : origins) {
		GaussianSplatHlodTree tree = baked.tree;
		tree.origin[0] = origin;
		CHECK_FALSE(gs_hlod_validate_tree(tree, &reason));
	}
	GaussianSplatHlodTree tree = baked.tree;
	tree.origin[0] = 1e6;
	CHECK(gs_hlod_validate_tree(tree, &reason));
	// Both endpoints can be finite while the real_t AABB size overflows.
	tree = baked.tree;
	tree.nodes[0].aabb_min.x = -FLT_MAX;
	tree.nodes[0].aabb_max.x = FLT_MAX;
	tree.nodes[0].radius = FLT_MAX;
	CHECK_FALSE(gs_hlod_validate_tree(tree, &reason));
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK) {
		FAIL("bake serialized runtime coordinate control");
		return;
	}
	const String path = hlod_temp_path("coordinates_review_r2");
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	if (saver.save(world, path) != OK) {
		FAIL("save runtime coordinate control");
		return;
	}
	PackedByteArray bytes = hlod_read_file(path);
	const double bad = DBL_MAX;
	memcpy(bytes.ptrw() + 104, &bad, sizeof(bad));
	if (!hlod_write_file(path, bytes)) {
		FAIL("write runtime coordinate mutation");
		return;
	}
	Error err = ERR_BUG;
	CHECK_FALSE(loader.load(path, "", &err).is_valid());
	CHECK(err == ERR_FILE_CORRUPT);
	CHECK_FALSE(loader.load_resident(path, &err).is_valid());
	CHECK(err == ERR_FILE_CORRUPT);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] review-r2 valid tight isotropic rotated and far-offset payloads round-trip") {
	using namespace TestGaussianSplatHlod;
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int fixture = 0; fixture < 3; fixture++) {
		LocalVector<Gaussian> g;
		hlod_make_fixture(0u, fixture == 2 ? 2u : 1u, g);
		for (uint32_t i = 0; i < g.size(); i++) {
			g[i].position = fixture == 2 ? Vector3(1000000.0f + float(i) * 0.0625f, 0, 0) : Vector3();
			g[i].scale = fixture == 1 ? Vector3(0.01f, 0.25f, 0.0625f) : Vector3(0.01f, 0.01f, 0.01f);
			g[i].rotation = fixture == 1 ? Quaternion(Vector3(1, 2, 3).normalized(), 0.37f) : Quaternion();
		}
		Ref<GaussianSplatWorld> world = hlod_make_world(g);
		if (world->bake_hlod() != OK) {
			FAIL("bake tight payload producer");
			return;
		}
		LocalVector<Gaussian> expected;
		LocalVector<Vector3> expected_sh;
		uint32_t first_order = 0, high_order = 0;
		if (!world->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), expected, expected_sh, first_order, high_order)) {
			FAIL("capture producer payload order");
			return;
		}
		for (int mode = 1; mode <= 3; mode++) {
			CAPTURE(fixture);
			CAPTURE(mode);
			const String path = hlod_temp_path("tight_control_review_r2");
			if (saver.save_with_payload_mode(world, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
				FAIL("save tight producer control");
				return;
			}
			for (bool resident : { false, true }) {
				Error err = ERR_BUG;
				Ref<GaussianSplatWorld> loaded;
				if (resident) {
					loaded = loader.load_resident(path, &err);
				} else {
					loaded = loader.load(path, "", &err);
				}
				if (loaded.is_null() || err != OK) {
					FAIL("valid tight producer must load in every payload mode");
					return;
				}
				Ref<ChunkPayloadSource> source = loaded->get_chunk_payload_source();
				LocalVector<Gaussian> actual;
				LocalVector<Vector3> actual_sh;
				bool captured = false;
				if (loaded->has_resident_gaussian_data()) {
					captured = loaded->get_gaussian_data()->capture_chunk_snapshot(0, g.size(), actual, actual_sh, first_order, high_order);
				} else if (source.is_valid()) {
					captured = source->capture_chunk_snapshot(0, g.size(), actual, actual_sh, first_order, high_order);
				}
				if (!captured) {
					FAIL("loaded tight payload must remain readable");
					return;
				}
				if (actual.size() != expected.size()) {
					FAIL("loaded tight payload cardinality differs from the producer");
					return;
				}
				for (uint32_t i = 0; i < actual.size(); i++) {
					CHECK(actual[i].position == expected[i].position);
					CHECK(actual[i].scale == expected[i].scale);
					CHECK(actual[i].rotation == expected[i].rotation);
				}
			}
			DirAccess::remove_absolute(path);
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD][MalformedCorpus] v2 rejects overlapping sections and preserves a prior import") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(3000u, 1000u, g);
	LocalVector<Vector3> sh;
	sh.resize(g.size() * 12u);
	for (uint32_t i = 0; i < sh.size(); i++) {
		sh[i] = Vector3(0.125f, -0.25f, 0.5f);
	}
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	world->get_gaussian_data()->set_gaussian_payload(g, sh, 3u, 12u, false);
	Dictionary metadata;
	metadata["note"] = "section extent control";
	world->set_metadata(metadata);
	if (world->bake_hlod() != OK) {
		FAIL("bake SH3 section fixture");
		return;
	}
	auto u64_at = [](const PackedByteArray &p_bytes, int p_offset) {
		uint64_t value;
		memcpy(&value, p_bytes.ptr() + p_offset, sizeof(value));
		return value;
	};
	auto patch_u64 = [](PackedByteArray &r_bytes, int p_offset, uint64_t p_value) {
		memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
	};
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 0; mode < 3; mode++) {
		const String valid_path = hlod_temp_path("section_control");
		if (saver.save_with_payload_mode(world, valid_path,
				ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode + 1)) != OK) {
			FAIL("save section control");
			return;
		}
		const PackedByteArray valid = hlod_read_file(valid_path);
		if (valid.size() <= 184) {
			FAIL("complete producer-generated v2 header required");
			return;
		}
		const uint64_t gaussian_offset = u64_at(valid, 56);
		const uint64_t sh_offset = u64_at(valid, 64);
		const uint64_t node_offset = u64_at(valid, 136);
		const uint64_t metadata_offset = u64_at(valid, 88);
		const uint64_t metadata_size = u64_at(valid, 96);
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> control = loader.load(valid_path, "", &err);
		if (control.is_null() || err != OK) {
			FAIL("unpatched producer control must load");
			return;
		}
		CHECK(control->has_hlod_tree());
		control.unref();
#ifdef TOOLS_ENABLED
		Ref<ResourceImporterGSplatWorld> importer;
		importer.instantiate();
		HashMap<StringName, Variant> options;
		const String import_base = hlod_temp_path("section_previous");
		if (importer->import(ResourceUID::INVALID_ID, valid_path, import_base, options, nullptr, nullptr, nullptr) != OK) {
			FAIL("establish previous good import");
			return;
		}
		const PackedByteArray previous = hlod_read_file(import_base + ".gsplatworld");
		if (previous != valid) {
			FAIL("the positive importer control must publish the complete producer file");
			return;
		}
#endif
		struct Patch {
			const char *name;
			int offset;
			uint64_t value;
		};
		const Patch patches[] = {
			{ "SH in header", 64, 0u },
			{ "SH overlaps last header byte", 64, 183u },
			{ "SH in Gaussian section", 64, gaussian_offset },
			{ "SH overlaps last Gaussian byte", 64, sh_offset - 1u },
			{ "SH overlaps first node byte", 64, sh_offset + 1u },
			{ "Gaussian in SH section", 56, sh_offset },
			{ "metadata in header", 88, 0u },
			{ "metadata in Gaussian section", 88, gaussian_offset },
			{ "metadata in SH section", 88, sh_offset },
			{ "metadata in node table", 88, node_offset },
		};
		for (const Patch &patch : patches) {
			INFO("mode ", mode, ", mutation ", std::string(patch.name));
			PackedByteArray corrupted = valid;
			patch_u64(corrupted, patch.offset, patch.value);
			const String bad_path = hlod_temp_path("section_overlap");
			if (!hlod_write_file(bad_path, corrupted)) {
				FAIL("write section mutation");
				return;
			}
			err = ERR_BUG;
			Ref<GaussianSplatWorld> rejected = loader.load(bad_path, "", &err);
			CHECK(rejected.is_null());
			CHECK(err == ERR_FILE_CORRUPT);
			rejected.unref();
			err = ERR_BUG;
			rejected = loader.load_resident(bad_path, &err);
			CHECK(rejected.is_null());
			CHECK(err == ERR_FILE_CORRUPT);
			rejected.unref();
#ifdef TOOLS_ENABLED
			CHECK(importer->import(ResourceUID::INVALID_ID, bad_path, import_base, options, nullptr, nullptr, nullptr) != OK);
			const bool previous_unchanged = hlod_read_file(import_base + ".gsplatworld") == previous;
			CHECK(previous_unchanged);
#endif
			DirAccess::remove_absolute(bad_path);
		}
		// Ordering is not the contract: move metadata before the Gaussian section, disjointly.
		PackedByteArray reordered = valid.slice(0, 184);
		reordered.append_array(valid.slice(metadata_offset, metadata_offset + metadata_size));
		reordered.append_array(valid.slice(184, metadata_offset));
		patch_u64(reordered, 56, gaussian_offset + metadata_size);
		patch_u64(reordered, 64, sh_offset + metadata_size);
		patch_u64(reordered, 136, node_offset + metadata_size);
		patch_u64(reordered, 88, 184u);
		const String reordered_path = hlod_temp_path("section_reordered");
		if (!hlod_write_file(reordered_path, reordered)) {
			FAIL("write reordered control");
			return;
		}
		err = ERR_BUG;
		Ref<GaussianSplatWorld> reordered_world = loader.load(reordered_path, "", &err);
		CHECK(err == OK);
		if (reordered_world.is_valid()) {
			CHECK(reordered_world->get_metadata() == metadata);
		} else {
			FAIL("disjoint reordered sections must load");
		}
		reordered_world.unref();
		DirAccess::remove_absolute(reordered_path);
#ifdef TOOLS_ENABLED
		DirAccess::remove_absolute(import_base + ".gsplatworld");
#endif
		DirAccess::remove_absolute(valid_path);
	}
}

namespace TestGaussianSplatHlod {

struct HlodSnapshotEdit {
	bool after_capture = false;
	bool fired = false;

	static void edit(void *p_userdata, const Ref<GaussianData> &p_data, bool p_after_capture) {
		HlodSnapshotEdit *probe = static_cast<HlodSnapshotEdit *>(p_userdata);
		if (probe->fired || probe->after_capture != p_after_capture) {
			return;
		}
		probe->fired = true;
		if (p_after_capture) {
			LocalVector<Gaussian> leaf;
			LocalVector<Vector3> sh;
			uint32_t first = 0u, high = 0u;
			if (p_data->capture_chunk_snapshot(0, p_data->get_count(), leaf, sh, first, high)) {
				p_data->set_gaussian_payload(leaf, sh, 3u, 0u, true);
			}
		} else {
			Gaussian edited = p_data->get_gaussian(0);
			edited.opacity = 0.01f;
			p_data->set_gaussian(0, edited);
		}
	}

	explicit HlodSnapshotEdit(bool p_after_capture) : after_capture(p_after_capture) {
		ResourceFormatSaverGaussianSplatWorld::hlod_snapshot_test_userdata = this;
		ResourceFormatSaverGaussianSplatWorld::hlod_snapshot_test_hook = edit;
	}
	~HlodSnapshotEdit() {
		ResourceFormatSaverGaussianSplatWorld::hlod_snapshot_test_hook = nullptr;
		ResourceFormatSaverGaussianSplatWorld::hlod_snapshot_test_userdata = nullptr;
	}
};

} // namespace TestGaussianSplatHlod

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] radius validation accepts isotropic spheres and resident chunks enclose their AABB") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(0u, 1u, g);
	g[0].position = Vector3();
	g[0].scale = Vector3(0.125f, 0.125f, 0.125f);
	g[0].rotation = Quaternion();
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK || world->get_hlod_tree().nodes.size() != 1u) {
		FAIL("single isotropic splat must bake to one leaf");
		return;
	}
	const GaussianSplatHlodTree &tree = world->get_hlod_tree();
	const GaussianSplatHlodNode &leaf = tree.nodes[0];
	String reason;
	CHECK_MESSAGE(gs_hlod_validate_tree(tree, &reason), reason.utf8().get_data());
	CHECK(leaf.radius < tree.node_world_aabb(leaf).size.length() * 0.5f);
	for (int axis = 0; axis < 3; axis++) {
		CHECK(double(leaf.radius) >= 0.5 * (double(leaf.aabb_max[axis]) - double(leaf.aabb_min[axis])));
	}
	const Vector<GaussianSplatRenderer::StaticChunk> &chunks = world->get_static_chunks();
	if (chunks.size() != 1) {
		FAIL("resident world must have one HLOD leaf chunk");
		return;
	}
	CHECK(chunks[0].radius >= chunks[0].bounds.size.length() * 0.5f);
	// A diagonal-plane overlap cannot disappear just because the serialized sphere is tight.
	const Vector3 normal = Vector3(1, 1, 1).normalized();
	const float distance = chunks[0].bounds.size.length() * 0.5f;
	const Plane boundary(normal, normal.dot(chunks[0].center) - distance);
	CHECK(boundary.distance_to(chunks[0].center) <= chunks[0].radius);
	CHECK(boundary.distance_to(chunks[0].center) > leaf.radius);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] resident chunk radius survives far-offset center rounding") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(0u, 2u, g);
	for (uint32_t i = 0; i < g.size(); i++) {
		g[i].position = Vector3(1000000.0f + float(i) * 0.0625f, 0, 0);
		g[i].scale = Vector3(0.01f, 0.01f, 0.01f);
		g[i].rotation = Quaternion();
	}
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK || world->get_static_chunks().size() != 1) {
		FAIL("far-offset pair must bake to one leaf chunk");
		return;
	}
	const GaussianSplatRenderer::StaticChunk &chunk = world->get_static_chunks()[0];
	const Plane boundary(Vector3(-1, 0, 0), -g[1].position.x);
	CHECK(boundary.distance_to(g[1].position) == 0.0f);
	CHECK(boundary.distance_to(chunk.center) <= chunk.radius);
	// Bound every exact stored corner about the emitted float center, not a rounded midpoint.
	const GaussianSplatHlodTree &tree = world->get_hlod_tree();
	const GaussianSplatHlodNode &leaf = tree.nodes[0];
	double cell_center[3];
	tree.node_cell_center(leaf, cell_center);
	for (uint32_t corner = 0; corner < 8u; corner++) {
		double distance_squared = 0.0;
		for (int a = 0; a < 3; a++) {
			const double endpoint = cell_center[a] + double((corner & (1u << a)) != 0u ? leaf.aabb_max[a] : leaf.aabb_min[a]);
			const double delta = endpoint - double(chunk.center[a]);
			distance_squared += delta * delta;
		}
		CHECK(double(chunk.radius) >= std::sqrt(distance_squared));
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] save rejects edits before snapshot and freezes metadata after snapshot") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(3000u, 1000u, g);
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (bool after_capture : { false, true }) {
		Ref<GaussianSplatWorld> world = hlod_make_world(g);
		if (world->bake_hlod() != OK) {
			FAIL("bake snapshot control");
			return;
		}
		const String path = hlod_temp_path(after_capture ? "snapshot_metadata" : "snapshot_stale");
		if (saver.save(world, path) != OK) {
			FAIL("save snapshot control");
			return;
		}
		const PackedByteArray before = hlod_read_file(path);
		{
			HlodSnapshotEdit edit(after_capture);
			CHECK(saver.save(world, path) == (after_capture ? OK : ERR_INVALID_DATA));
			CHECK(edit.fired);
		}
		// An edit after capture may not change the coherent old snapshot on disk.
		const bool unchanged = hlod_read_file(path) == before;
		CHECK(unchanged);
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &err);
		CHECK(err == OK);
		if (loaded.is_null() || loaded->get_gaussian_data().is_null()) {
			FAIL("resident snapshot control load");
			DirAccess::remove_absolute(path);
			return;
		}
		CHECK_FALSE(loaded->get_gaussian_data()->get_2d_mode());
		CHECK_EQ(loaded->get_gaussian_data()->get_sh_degree(), 0u);
		loaded.unref();
		DirAccess::remove_absolute(path);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] a failed bake leaves the world untouched; an edited payload is not saved with a stale tree") {
	using namespace TestGaussianSplatHlod;
	ResourceFormatSaverGaussianSplatWorld saver;
	// Failure: non-finite input. The world keeps its payload, chunks and (absent) tree.
	LocalVector<Gaussian> bad;
	hlod_make_fixture(3000u, 1000u, bad);
	bad[10].opacity = NAN;
	Ref<GaussianSplatWorld> broken = hlod_make_world(bad);
	const Ref<GaussianData> before = broken->get_gaussian_data();
	const int chunks_before = broken->get_chunk_count();
	CHECK(broken->bake_hlod() == ERR_INVALID_DATA);
	CHECK(broken->get_gaussian_data() == before);
	CHECK_EQ(broken->get_chunk_count(), chunks_before);
	CHECK_FALSE(broken->has_hlod_tree());

	// Staleness: an in-place edit of the baked payload must not be saved with the old tree.
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK) {
		FAIL("bake");
		return;
	}
	CHECK(world->is_hlod_payload_current());
	const String path = hlod_temp_path("stale");
	CHECK(saver.save(world, path) == OK);
	Gaussian edited = g[0];
	edited.opacity = 0.01f;
	world->get_gaussian_data()->set_gaussian(5, edited);
	CHECK_FALSE(world->is_hlod_payload_current());
	CHECK(saver.save(world, path) == ERR_INVALID_DATA);
	// Re-baking makes it current again.
	CHECK(world->bake_hlod() == OK);
	CHECK(world->is_hlod_payload_current());
	CHECK(saver.save(world, path) == OK);
	// A resident load records the loaded payload as current.
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &err);
	if (loaded.is_null()) {
		FAIL("resident load");
		return;
	}
	CHECK(loaded->is_hlod_payload_current());
	loaded.unref();
	DirAccess::remove_absolute(path);
}

namespace TestGaussianSplatHlod {

class HlodBakeObserver : public Object {
public:
	GaussianSplatWorld *world = nullptr;
	int notifications = 0;
	bool edit_payload = false;
	bool saw_complete_bake = false;

	void on_changed() {
		if (++notifications != 1 || !edit_payload) {
			return;
		}
		saw_complete_bake = world->has_hlod_tree() && world->is_hlod_payload_current() && world->get_chunk_count() > 0;
		Ref<GaussianData> data = world->get_gaussian_data();
		if (data.is_valid() && data->get_count() > 0) {
			Gaussian edited = data->get_gaussian(0);
			edited.opacity = 0.01f;
			data->set_gaussian(0, edited);
		}
	}
};

struct HlodBakeEdit {
	bool change_2d;
	bool fired = false;
	explicit HlodBakeEdit(bool p_change_2d) : change_2d(p_change_2d) {
		GaussianSplatWorld::hlod_bake_test_userdata = this;
		GaussianSplatWorld::hlod_bake_test_hook = [](void *p_userdata, GaussianSplatWorld *p_world) {
			HlodBakeEdit *edit = static_cast<HlodBakeEdit *>(p_userdata);
			edit->fired = true;
			Ref<GaussianData> data = p_world->get_gaussian_data();
			if (edit->change_2d) {
				data->set_2d_mode(true);
			} else {
				Gaussian g = data->get_gaussian(0);
				g.opacity = 0.01f;
				data->set_gaussian(0, g);
			}
		};
	}
	~HlodBakeEdit() {
		GaussianSplatWorld::hlod_bake_test_hook = nullptr;
		GaussianSplatWorld::hlod_bake_test_userdata = nullptr;
	}
};

} // namespace TestGaussianSplatHlod

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake rejects payload and 2D edits after its snapshot without losing the edit") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(3000u, 1000u, g);
	for (bool change_2d : { false, true }) {
		Ref<GaussianSplatWorld> world = hlod_make_world(g);
		if (world->bake_hlod() != OK) {
			FAIL("bake race control");
			return;
		}
		const Ref<GaussianData> original = world->get_gaussian_data();
		const PackedInt32Array chunks = world->get_chunk_sizes();
		const Dictionary tree_info = world->get_hlod_info();
		HlodBakeObserver *observer = memnew(HlodBakeObserver);
		world->connect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
		{
			HlodBakeEdit edit(change_2d);
			CHECK(world->bake_hlod() == ERR_INVALID_DATA);
			CHECK(edit.fired);
		}
		CHECK(world->get_gaussian_data() == original);
		CHECK(world->get_chunk_sizes() == chunks);
		CHECK(world->get_hlod_info() == tree_info);
		CHECK_FALSE(world->is_hlod_payload_current());
		if (change_2d) {
			CHECK(world->get_2d_mode());
		} else {
			CHECK(world->get_gaussian_data()->get_gaussian(0).opacity == 0.01f);
		}
		CHECK_EQ(observer->notifications, 0);
		world->disconnect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
		memdelete(observer);
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] replacing a file-backed payload invalidates its tree and leaf chunks") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(3000u, 1000u, g);
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	const String old_path = hlod_temp_path("source_replace_old");
	const String new_path = hlod_temp_path("source_replace_new");
	const String saved_path = hlod_temp_path("source_replace_saved");
	Ref<GaussianSplatWorld> original = hlod_make_world(g);
	if (original->bake_hlod() != OK || saver.save(original, old_path) != OK) {
		FAIL("original source replacement fixture");
		return;
	}
	for (Gaussian &gaussian : g) {
		gaussian.opacity = 0.01f;
	}
	Ref<GaussianSplatWorld> replacement = hlod_make_world(g);
	if (replacement->bake_hlod() != OK || saver.save(replacement, new_path) != OK) {
		FAIL("replacement source fixture");
		return;
	}
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> world = loader.load(old_path, old_path, &err);
	if (err != OK || world.is_null()) {
		FAIL("load original file-backed source");
		return;
	}
	Ref<GaussianSplatWorld> source_world = loader.load(new_path, new_path, &err);
	if (err != OK || source_world.is_null()) {
		FAIL("load replacement file-backed source");
		return;
	}
	CHECK(world->get_gaussian_data().is_null());
	CHECK(world->has_hlod_tree());
	world->set_chunk_payload_source(world->get_chunk_payload_source());
	CHECK(world->has_hlod_tree());
	world->set_chunk_payload_source(source_world->get_chunk_payload_source());
	CHECK_FALSE(world->has_hlod_tree());
	CHECK_EQ(world->get_chunk_count(), 0);
	world->set_static_chunks(source_world->get_static_chunks());
	if (saver.save(world, saved_path) != OK) {
		FAIL("save replaced source without an obsolete tree");
		return;
	}
	Ref<GaussianSplatWorld> loaded = loader.load_resident(saved_path, &err);
	if (err != OK || loaded.is_null() || loaded->get_gaussian_data().is_null()) {
		FAIL("reload replacement leaf payload");
		return;
	}
	CHECK_FALSE(loaded->has_hlod_tree());
	CHECK(loaded->get_gaussian_data()->get_gaussian(0).opacity == 0.01f);
	loaded.unref();
	world.unref();
	source_world.unref();
	DirAccess::remove_absolute(old_path);
	DirAccess::remove_absolute(new_path);
	DirAccess::remove_absolute(saved_path);
}

// Resident counterpart of the file-backed case above: set_gaussian_data() with another payload
// must drop the leaf chunks with the tree, or the next submission pairs the new payload with the
// old leaf index ranges and bounds.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD] replacing a resident payload invalidates its tree and leaf chunks") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	LocalVector<Gaussian> small;
	hlod_make_fixture(200u, 100u, small);
	Ref<GaussianData> replacement;
	replacement.instantiate();
	LocalVector<Vector3> no_sh;
	replacement->set_gaussian_payload(small, no_sh, 0u, 0u, false);

	// Baked in memory.
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	if (world->bake_hlod() != OK || world->get_chunk_count() < 2) {
		FAIL("bake a multi-leaf resident world");
		return;
	}
	world->set_gaussian_data(replacement);
	CHECK_FALSE(world->has_hlod_tree());
	CHECK_EQ(world->get_chunk_count(), 0);
	CHECK(world->get_gaussian_data() == replacement);

	// Loaded resident from a v2 file.
	Ref<GaussianSplatWorld> baked = hlod_make_world(g);
	ResourceFormatSaverGaussianSplatWorld saver;
	const String path = hlod_temp_path("resident_replace");
	if (baked->bake_hlod() != OK || saver.save_resident_uncompressed(baked, path) != OK) {
		FAIL("save a baked resident world");
		return;
	}
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> loaded = loader.load_resident(path, &err);
	DirAccess::remove_absolute(path);
	if (err != OK || loaded.is_null() || !loaded->has_hlod_tree() || loaded->get_chunk_count() < 2) {
		FAIL("load the baked resident world");
		return;
	}
	loaded->set_gaussian_data(replacement);
	CHECK_FALSE(loaded->has_hlod_tree());
	CHECK_EQ(loaded->get_chunk_count(), 0);

	// Control: a tree-less world keeps the chunks its producer set (behaviour unchanged).
	Ref<GaussianSplatWorld> plain = hlod_make_world(g);
	plain->set_static_chunks(baked->get_static_chunks());
	const int plain_chunks = plain->get_chunk_count();
	CHECK(plain_chunks >= 2);
	plain->set_gaussian_data(plain->get_gaussian_data());
	CHECK_EQ(plain->get_chunk_count(), plain_chunks);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] a rejected file-backed bake preserves streaming and emits no changes") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> bad;
	hlod_make_fixture(3000u, 1000u, bad);
	bad[10].opacity = NAN;
	ResourceFormatSaverGaussianSplatWorld saver;
	const String path = hlod_temp_path("failed_streamable_bake");
	if (saver.save_with_payload_mode(hlod_make_world(bad), path,
			ResourceFormatSaverGaussianSplatWorld::SAVE_PAYLOAD_STREAMABLE_UNCOMPRESSED) != OK) {
		FAIL("save streamable failure fixture");
		return;
	}
	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> world = loader.load(path, path, &err);
	if (world.is_null() || err != OK || !world->is_streamable_payload()) {
		FAIL("load a file-backed failure fixture");
		world.unref();
		DirAccess::remove_absolute(path);
		return;
	}
	Ref<ChunkPayloadSource> source = world->get_chunk_payload_source();
	const PackedInt32Array chunks = world->get_chunk_sizes();
	const Array chunk_bounds = world->get_chunk_aabbs();
	const AABB bounds = world->get_bounds();
	HlodBakeObserver *observer = memnew(HlodBakeObserver);
	world->connect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
	CHECK(world->bake_hlod() == ERR_INVALID_DATA);
	CHECK(world->is_streamable_payload());
	CHECK_FALSE(world->has_resident_gaussian_data());
	CHECK(world->get_chunk_payload_source() == source);
	CHECK(world->get_chunk_sizes() == chunks);
	CHECK(world->get_chunk_aabbs() == chunk_bounds);
	CHECK(world->get_bounds() == bounds);
	CHECK_FALSE(world->has_hlod_tree());
	CHECK_EQ(observer->notifications, 0);
	world->disconnect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
	memdelete(observer);
	world.unref();
	source.unref();
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] bake notifications publish complete state and cannot bless callback edits") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	Ref<GaussianSplatWorld> world = hlod_make_world(g);
	HlodBakeObserver *observer = memnew(HlodBakeObserver);
	observer->world = world.ptr();
	observer->edit_payload = true;
	world->connect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
	CHECK(world->bake_hlod() == OK);
	CHECK_EQ(observer->notifications, 1);
	CHECK(observer->saw_complete_bake);
	CHECK_FALSE(world->is_hlod_payload_current());
	world->disconnect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
	memdelete(observer);
	ResourceFormatSaverGaussianSplatWorld saver;
	const String path = hlod_temp_path("callback_stale_bake");
	CHECK(saver.save(world, path) == ERR_INVALID_DATA);
	DirAccess::remove_absolute(path);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] review-r4 editor reimport reloads the complete world state") {
	using namespace TestGaussianSplatHlod;
	// A is loaded and cached; B then overwrites A's file, as an editor reimport does.
	LocalVector<Gaussian> ga;
	hlod_make_fixture(24000u, 16000u, ga);
	LocalVector<Gaussian> gb;
	hlod_make_fixture(14000u, 8000u, gb);
	for (uint32_t i = 0; i < gb.size(); i++) {
		gb[i].position += Vector3(40.0f, 1.0f, -25.0f);
	}
	Ref<GaussianSplatWorld> world_a = hlod_make_world(ga);
	Ref<GaussianSplatWorld> world_b = hlod_make_world(gb);
	Dictionary meta_b;
	meta_b["note"] = "reimported";
	world_b->set_metadata(meta_b);
	if (world_a->bake_hlod() != OK || world_b->bake_hlod() != OK) {
		FAIL("bake the reload fixtures");
		return;
	}
	using Saver = ResourceFormatSaverGaussianSplatWorld;
	Saver saver;
	// Cached file-backed world reimported as resident, and cached resident world reimported as
	// file-backed (the file source must be replaced, not kept or dropped).
	for (int cached_streamable = 0; cached_streamable < 2; cached_streamable++) {
		CAPTURE(cached_streamable);
		const Saver::PayloadSaveMode mode_a = cached_streamable ? Saver::SAVE_PAYLOAD_STREAMABLE_UNCOMPRESSED : Saver::SAVE_PAYLOAD_RESIDENT_UNCOMPRESSED;
		const Saver::PayloadSaveMode mode_b = cached_streamable ? Saver::SAVE_PAYLOAD_RESIDENT_UNCOMPRESSED : Saver::SAVE_PAYLOAD_STREAMABLE_UNCOMPRESSED;
		const String path = hlod_temp_path(vformat("reload%d", cached_streamable));
		const String resave_cached = hlod_temp_path(vformat("reload%d_cached", cached_streamable));
		const String resave_fresh = hlod_temp_path(vformat("reload%d_fresh", cached_streamable));
		if (saver.save_with_payload_mode(world_a, path, mode_a) != OK) {
			FAIL("save the cached world");
			return;
		}
		Ref<GaussianSplatWorld> cached = ResourceLoader::load(path, "GaussianSplatWorld", ResourceFormatLoader::CACHE_MODE_IGNORE);
		if (cached.is_null() || !cached->has_hlod_tree() || cached->is_streamable_payload() != bool(cached_streamable)) {
			FAIL("load the cached world");
			DirAccess::remove_absolute(path);
			return;
		}
		if (saver.save_with_payload_mode(world_b, path, mode_b) != OK) {
			FAIL("overwrite the cached world's file");
			DirAccess::remove_absolute(path);
			return;
		}
		// The body of Resource::reload_from_file(), which returns early for paths outside res://.
		Ref<Resource> reimported = ResourceLoader::load(path, "GaussianSplatWorld", ResourceFormatLoader::CACHE_MODE_IGNORE);
		Ref<GaussianSplatWorld> fresh = ResourceLoader::load(path, "GaussianSplatWorld", ResourceFormatLoader::CACHE_MODE_IGNORE);
		if (reimported.is_null() || fresh.is_null() || !fresh->has_hlod_tree()) {
			FAIL("load the reimported world");
			DirAccess::remove_absolute(path);
			return;
		}
		HlodBakeObserver *observer = memnew(HlodBakeObserver);
		cached->connect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
		CHECK(cached->copy_from(reimported) == OK);
		reimported.unref();
		CHECK_EQ(observer->notifications, 1);
		cached->disconnect(SNAME("changed"), callable_mp(observer, &HlodBakeObserver::on_changed));
		memdelete(observer);

		// Payload mode and payload metadata.
		CHECK_EQ(cached->get_payload_mode(), fresh->get_payload_mode());
		CHECK_EQ(cached->is_streamable_payload(), fresh->is_streamable_payload());
		CHECK_EQ(cached->is_streamable_payload(), !bool(cached_streamable));
		CHECK_EQ(cached->has_resident_gaussian_data(), fresh->has_resident_gaussian_data());
		CHECK_EQ(cached->has_chunk_payload_source(), fresh->has_chunk_payload_source());
		CHECK_EQ(cached->get_resident_only_reason(), fresh->get_resident_only_reason());
		CHECK_EQ(cached->get_splat_count(), fresh->get_splat_count());
		CHECK_EQ(cached->get_splat_count(), gb.size());
		CHECK_EQ(cached->get_sh_degree(), fresh->get_sh_degree());
		CHECK_EQ(cached->get_sh_first_order_count(), fresh->get_sh_first_order_count());
		CHECK_EQ(cached->get_sh_high_order_count(), fresh->get_sh_high_order_count());
		CHECK_EQ(cached->get_2d_mode(), fresh->get_2d_mode());
		CHECK(cached->get_bounds() == fresh->get_bounds());
		CHECK(cached->get_metadata() == fresh->get_metadata());
		// Leaf chunks.
		const Vector<GaussianSplatRenderer::StaticChunk> &cached_chunks = cached->get_static_chunks();
		const Vector<GaussianSplatRenderer::StaticChunk> &fresh_chunks = fresh->get_static_chunks();
		CHECK_GT(fresh_chunks.size(), 1);
		if (cached_chunks.size() != fresh_chunks.size()) {
			FAIL_CHECK("reloaded leaf chunk count ", cached_chunks.size(), " != fresh ", fresh_chunks.size());
		} else {
			bool chunks_equal = true;
			for (int i = 0; i < fresh_chunks.size(); i++) {
				chunks_equal = chunks_equal && cached_chunks[i].bounds == fresh_chunks[i].bounds &&
						cached_chunks[i].center == fresh_chunks[i].center && cached_chunks[i].radius == fresh_chunks[i].radius &&
						cached_chunks[i].indices == fresh_chunks[i].indices;
			}
			CHECK(chunks_equal);
		}
		// Tree: node table, counts, interior payload location.
		CHECK(cached->has_hlod_tree());
		CHECK(cached->is_hlod_payload_current());
		CHECK(cached->get_hlod_info() == fresh->get_hlod_info());
		const GaussianSplatHlodTree &ct = cached->get_hlod_tree();
		const GaussianSplatHlodTree &ft = fresh->get_hlod_tree();
		bool nodes_equal = ct.nodes.size() == ft.nodes.size();
		for (uint32_t i = 0; nodes_equal && i < ft.nodes.size(); i++) {
			nodes_equal = hlod_nodes_equal(ct.nodes[i], ft.nodes[i]);
		}
		CHECK(nodes_equal);
		CHECK_EQ(ct.leaf_splat_count, ft.leaf_splat_count);
		CHECK_EQ(ct.interior_splat_count, ft.interior_splat_count);
		CHECK_EQ(ct.interior_resident, ft.interior_resident);
		CHECK_EQ(ct.interior_file_path, ft.interior_file_path);
		CHECK_EQ(ct.interior_gaussian_offset, ft.interior_gaussian_offset);
		CHECK_EQ(ct.interior_sh_offset, ft.interior_sh_offset);
		CHECK(std::memcmp(ct.origin, ft.origin, sizeof(ct.origin)) == 0);
		// Everything the format round-trips: re-saving the reloaded and a fresh load gives one file.
		CHECK(saver.save(cached, resave_cached) == OK);
		CHECK(saver.save(fresh, resave_fresh) == OK);
		const PackedByteArray cached_bytes = hlod_read_file(resave_cached);
		CHECK_FALSE(cached_bytes.is_empty());
		CHECK(cached_bytes == hlod_read_file(resave_fresh));

		cached.unref();
		fresh.unref();
		DirAccess::remove_absolute(path);
		DirAccess::remove_absolute(resave_cached);
		DirAccess::remove_absolute(resave_fresh);
	}
}

#ifdef TOOLS_ENABLED
TEST_CASE("[GaussianSplatting][WorldIO][HLOD] review-r2 producer rejects non-finite render extensions and preserves valid v1 fallback") {
	using namespace TestGaussianSplatHlod;
	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	Ref<ResourceImporterGSplatWorld> importer;
	importer.instantiate();
	HashMap<StringName, Variant> options;
	for (int field = 0; field < 4; field++) {
		CAPTURE(field);
		LocalVector<Gaussian> bad;
		hlod_make_fixture(0u, 1u, bad);
		switch (field) {
			case 0: bad[0].normal.x = NAN; break;
			case 1: bad[0].area = NAN; break;
			case 2: bad[0].stroke_age = NAN; break;
			case 3: bad[0].brush_axes.x = NAN; break;
		}
		Ref<GaussianSplatWorld> world = hlod_make_world(bad);
		const Ref<GaussianData> before = world->get_gaussian_data();
		const int chunks_before = world->get_chunk_count();
		const AABB bounds_before = world->get_bounds();
		CHECK(world->bake_hlod() == ERR_INVALID_DATA);
		CHECK(world->get_gaussian_data() == before);
		CHECK(world->get_chunk_count() == chunks_before);
		CHECK(world->get_bounds() == bounds_before);
		CHECK_FALSE(world->has_hlod_tree());
		for (int mode = 1; mode <= 3; mode++) {
			CAPTURE(mode);
			const String source_path = hlod_temp_path("extension_fallback_source");
			const String base = hlod_temp_path("extension_fallback_previous");
			LocalVector<Gaussian> good;
			hlod_make_fixture(0u, 1u, good);
			if (saver.save(hlod_make_world(good), source_path) != OK ||
					importer->import(ResourceUID::INVALID_ID, source_path, base, options, nullptr, nullptr, nullptr) != OK) {
				FAIL("establish a previous good import");
				return;
			}
			if (saver.save_with_payload_mode(hlod_make_world(bad), source_path,
						ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
				FAIL("save legal tree-less v1 source");
				return;
			}
			const PackedByteArray source_bytes = hlod_read_file(source_path);
			Error err = ERR_BUG;
			if (loader.load(source_path, "", &err).is_null() || err != OK) {
				FAIL("v1 remains legal input for the existing plain-copy fallback");
				return;
			}
			Variant metadata;
			CHECK(importer->import(ResourceUID::INVALID_ID, source_path, base, options, nullptr, nullptr, &metadata) == OK);
			CHECK(String(Dictionary(metadata).get("hlod_skip_reason", "")) == "bake_failed");
			CHECK(FileAccess::exists(base + ".gsplatworld"));
			CHECK(bool(hlod_read_file(base + ".gsplatworld") == source_bytes));
			CHECK(bool(hlod_read_file(source_path) == source_bytes));
			Ref<GaussianSplatWorld> fallback = loader.load(base + ".gsplatworld", "", &err);
			CHECK(fallback.is_valid());
			CHECK(err == OK);
			if (fallback.is_valid()) {
				CHECK_FALSE(fallback->has_hlod_tree());
			}
			DirAccess::remove_absolute(base + ".gsplatworld");
			DirAccess::remove_absolute(source_path);
		}
	}
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] importer falls back to the plain copy when the bake fails, and never replaces a good import with a corrupt v2 source") {
	using namespace TestGaussianSplatHlod;
	ResourceFormatSaverGaussianSplatWorld saver;
	Ref<ResourceImporterGSplatWorld> importer;
	importer.instantiate();
	HashMap<StringName, Variant> options;

	// A v1 source the bake refuses (non-finite opacity) still imports, tree-less, with a reason.
	LocalVector<Gaussian> bad;
	hlod_make_fixture(3000u, 1000u, bad);
	bad[10].opacity = NAN;
	const String bad_source = hlod_temp_path("import_bad_source");
	if (saver.save(hlod_make_world(bad), bad_source) != OK) {
		FAIL("save bad source");
		return;
	}
	const String bad_base = OS::get_singleton()->get_temp_path().path_join("godotgs_hlod_import_bad_" + itos(OS::get_singleton()->get_ticks_usec()));
	Variant bad_md;
	CHECK(importer->import(ResourceUID::INVALID_ID, bad_source, bad_base, options, nullptr, nullptr, &bad_md) == OK);
	CHECK(String(Dictionary(bad_md)["hlod_skip_reason"]) == String("bake_failed"));
	CHECK(hlod_read_file(bad_base + ".gsplatworld") == hlod_read_file(bad_source));

	// A good import exists; re-importing a corrupt v2 source over it fails and leaves it intact.
	LocalVector<Gaussian> g;
	hlod_make_fixture(24000u, 16000u, g);
	const String good_source = hlod_temp_path("import_good_source");
	if (saver.save(hlod_make_world(g), good_source) != OK) {
		FAIL("save good source");
		return;
	}
	const String base = OS::get_singleton()->get_temp_path().path_join("godotgs_hlod_import_keep_" + itos(OS::get_singleton()->get_ticks_usec()));
	if (importer->import(ResourceUID::INVALID_ID, good_source, base, options, nullptr, nullptr, nullptr) != OK) {
		FAIL("good import");
		return;
	}
	const PackedByteArray good_import = hlod_read_file(base + ".gsplatworld");
	PackedByteArray corrupt = good_import;
	const uint32_t zero = 0u;
	memcpy(corrupt.ptrw() + 144, &zero, 4); // node_count = 0
	const String corrupt_source = hlod_temp_path("import_corrupt_v2");
	if (!hlod_write_file(corrupt_source, corrupt)) {
		FAIL("write corrupt source");
		return;
	}
	CHECK(importer->import(ResourceUID::INVALID_ID, corrupt_source, base, options, nullptr, nullptr, nullptr) != OK);
	CHECK(hlod_read_file(base + ".gsplatworld") == good_import);

	// A v1 header can pass the importer probe yet fail full SH validation.
	// Neither the fallback nor an in-place import may publish/delete that source.
	PackedByteArray corrupt_v1 = hlod_read_file(good_source);
	if (corrupt_v1.size() < 104) {
		FAIL("producer-generated v1 header required");
		return;
	}
	const uint32_t high_order_without_flag = 1u;
	memcpy(corrupt_v1.ptrw() + 24, &high_order_without_flag, 4);
	const String corrupt_v1_source = hlod_temp_path("import_corrupt_v1");
	if (!hlod_write_file(corrupt_v1_source, corrupt_v1)) {
		FAIL("write corrupt v1 source");
		return;
	}
	CHECK(importer->import(ResourceUID::INVALID_ID, corrupt_v1_source, base, options, nullptr, nullptr, nullptr) != OK);
	const bool retained_previous_import = hlod_read_file(base + ".gsplatworld") == good_import;
	CHECK(retained_previous_import);
	CHECK(importer->import(ResourceUID::INVALID_ID, corrupt_v1_source,
			corrupt_v1_source.get_basename(), options, nullptr, nullptr, nullptr) != OK);
	const bool retained_source = hlod_read_file(corrupt_v1_source) == corrupt_v1;
	CHECK(retained_source);

	DirAccess::remove_absolute(corrupt_v1_source);
	DirAccess::remove_absolute(corrupt_source);
	DirAccess::remove_absolute(base + ".gsplatworld");
	DirAccess::remove_absolute(good_source);
	DirAccess::remove_absolute(bad_base + ".gsplatworld");
	DirAccess::remove_absolute(bad_source);
}

TEST_CASE("[GaussianSplatting][WorldIO][HLOD] importer bakes a v1 source into a v2 copy; source untouched; same tree as bake_hlod()") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_fixture(9000u, 6000u, g);
	Ref<GaussianSplatWorld> source = hlod_make_world(g);
	source->set_bounds(AABB(Vector3(-16, -1, -16), Vector3(32, 7, 32)));
	const String source_path = hlod_temp_path("import_source");
	ResourceFormatSaverGaussianSplatWorld saver;
	if (!(saver.save(source, source_path) == OK)) {
		FAIL("saver.save(source, source_path) == OK");
		return;
	}
	const PackedByteArray source_before = hlod_read_file(source_path);
	{
		Ref<FileAccess> f = FileAccess::open(source_path, FileAccess::READ);
		if (!(f.is_valid())) {
			FAIL("f.is_valid()");
			return;
		}
		f->seek(4);
		CHECK_EQ(f->get_32(), 1u);
	}

	Ref<ResourceImporterGSplatWorld> importer;
	importer.instantiate();
	CHECK_EQ(importer->get_format_version(), 4);
	const String save_base = OS::get_singleton()->get_temp_path().path_join("godotgs_hlod_import_" + itos(OS::get_singleton()->get_ticks_usec()));
	HashMap<StringName, Variant> options;
	Variant metadata;
	if (!(importer->import(ResourceUID::INVALID_ID, source_path, save_base, options, nullptr, nullptr, &metadata) == OK)) {
		FAIL("importer->import(ResourceUID::INVALID_ID, source_path, save_base, options, nullptr, nullptr, &metadata) == OK");
		return;
	}
	const String imported_path = save_base + ".gsplatworld";

	// The source is only read.
	CHECK(hlod_read_file(source_path) == source_before);
	// The imported copy is v2 with a tree.
	{
		Ref<FileAccess> f = FileAccess::open(imported_path, FileAccess::READ);
		if (!(f.is_valid())) {
			FAIL("f.is_valid()");
			return;
		}
		f->seek(4);
		CHECK_EQ(f->get_32(), 2u);
	}
	if (!(metadata.get_type() == Variant::DICTIONARY)) {
		FAIL("metadata.get_type() == Variant::DICTIONARY");
		return;
	}
	const Dictionary md = metadata;
	CHECK(bool(Dictionary(md["hlod"])["has_hlod"]));
	CHECK(String(md["hlod_skip_reason"]).is_empty());
	CHECK(bool(md["streamable"]));

	ResourceFormatLoaderGaussianSplatWorld loader;
	Error err = ERR_BUG;
	Ref<GaussianSplatWorld> imported = loader.load(imported_path, "", &err);
	if (!(imported.is_valid())) {
		FAIL("imported.is_valid()");
		return;
	}
	if (!(imported->has_hlod_tree())) {
		FAIL("imported->has_hlod_tree()");
		return;
	}

	// bake_hlod() on the runtime-built world gives the same tree as the importer (test 4).
	Ref<GaussianSplatWorld> runtime = hlod_make_world(g);
	if (!(runtime->bake_hlod() == OK)) {
		FAIL("runtime->bake_hlod() == OK");
		return;
	}
	const GaussianSplatHlodTree &a = imported->get_hlod_tree();
	const GaussianSplatHlodTree &b = runtime->get_hlod_tree();
	if ((a.nodes.size()) != (b.nodes.size())) {
		FAIL("a.nodes.size() == b.nodes.size()");
		return;
	}
	bool same = true;
	for (uint32_t i = 0; i < a.nodes.size(); i++) {
		same = same && hlod_nodes_equal(a.nodes[i], b.nodes[i]);
	}
	CHECK(same);

	// Re-importing a v2 file copies it unchanged (no second bake).
	Variant metadata2;
	const String save_base2 = save_base + "_again";
	if (!(importer->import(ResourceUID::INVALID_ID, imported_path, save_base2, options, nullptr, nullptr, &metadata2) == OK)) {
		FAIL("importer->import(ResourceUID::INVALID_ID, imported_path, save_base2, options, nullptr, nullptr, &metadata2) == OK");
		return;
	}
	CHECK(hlod_read_file(save_base2 + ".gsplatworld") == hlod_read_file(imported_path));
	CHECK(String(Dictionary(metadata2)["hlod_skip_reason"]) == String("source_already_v2"));

	imported.unref();
	DirAccess::remove_absolute(save_base2 + ".gsplatworld");
	DirAccess::remove_absolute(imported_path);
	DirAccess::remove_absolute(source_path);
}
#endif // TOOLS_ENABLED

// Camera-frustum property on what the engine is handed. GaussianSplatWorld3D::_update_bounds()
// publishes world->get_bounds() (when it has volume) as the instance AABB, and RendererSceneCull
// keeps an instance when that AABB intersects the camera's convex frustum. The camera below sees
// only needle support that GaussianData::get_aabb(), the bounds a verbatim header would carry,
// misses. RendererSceneCull itself is not reachable here: under --gs-gpu-test the [SceneTree]
// listener uses the dummy rasterizer, which creates no GaussianSplatStorage, so a world node's
// instance never enters the scenario indexer.
TEST_CASE("[GaussianSplatting][WorldIO][HLOD] review-r3 loaded v2 world bounds keep valid splats inside a camera frustum") {
	using namespace TestGaussianSplatHlod;
	LocalVector<Gaussian> g;
	hlod_make_needle_fixture(g);
	Ref<GaussianSplatWorld> source = hlod_make_world(g);
	if (source->bake_hlod() != OK) {
		FAIL("bake the needle producer");
		return;
	}
	double lo[3], hi[3];
	hlod_root_extent(source->get_hlod_tree(), lo, hi);
	// static_cast, not real_t(lo[0]): GCC parses the functional casts as a function declaration.
	const AABB root(Vector3(static_cast<real_t>(lo[0]), static_cast<real_t>(lo[1]), static_cast<real_t>(lo[2])),
			Vector3(real_t(hi[0] - lo[0]), real_t(hi[1] - lo[1]), real_t(hi[2] - lo[2])));

	Projection projection;
	projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	// Looking down -z from (4, 0, 5): sees x in [0.5, 7.5] m at z = 0.
	const Vector<Plane> near_planes = projection.get_projection_planes(Transform3D(Basis(), Vector3(4.0f, 0.0f, 5.0f)));
	const Vector<Plane> far_planes = projection.get_projection_planes(Transform3D(Basis(), Vector3(50.0f, 0.0f, 5.0f)));
	const Vector<Vector3> near_points = Geometry3D::compute_convex_mesh_points(near_planes.ptr(), near_planes.size());
	const Vector<Vector3> far_points = Geometry3D::compute_convex_mesh_points(far_planes.ptr(), far_planes.size());
	auto in_near = [&](const AABB &p_box) {
		return p_box.intersects_convex_shape(near_planes.ptr(), near_planes.size(), near_points.ptr(), near_points.size());
	};
	auto in_far = [&](const AABB &p_box) {
		return p_box.intersects_convex_shape(far_planes.ptr(), far_planes.size(), far_points.ptr(), far_points.size());
	};
	// Controls: the validated splat support is in view; the verbatim-header bounds are not.
	CHECK(in_near(root));
	const AABB payload_aabb = source->get_gaussian_data()->get_aabb();
	CHECK_FALSE(in_near(payload_aabb));
	// The live world bake_hlod() returns, rendered without a save and reload.
	CHECK(source->get_bounds().has_volume());
	CHECK(in_near(source->get_bounds()));
	CHECK_FALSE(in_far(source->get_bounds()));
	// Loaded worlds: the saver must widen bounds that miss the root (e.g. set by a caller).
	source->set_bounds(payload_aabb);

	ResourceFormatSaverGaussianSplatWorld saver;
	ResourceFormatLoaderGaussianSplatWorld loader;
	for (int mode = 1; mode <= 3; mode++) {
		const String path = hlod_temp_path("frustum_r3");
		if (saver.save_with_payload_mode(source, path, ResourceFormatSaverGaussianSplatWorld::PayloadSaveMode(mode)) != OK) {
			FAIL("save the needle producer");
			return;
		}
		CAPTURE(mode);
		Error err = ERR_BUG;
		Ref<GaussianSplatWorld> loaded = loader.load(path, "", &err);
		if (loaded.is_null() || err != OK) {
			FAIL("the needle producer must load");
			DirAccess::remove_absolute(path);
			return;
		}
		const AABB published = loaded->get_bounds();
		// _update_bounds() falls back to GaussianData::get_aabb() for bounds without volume.
		CHECK(published.has_volume());
		CHECK(in_near(published));
		// A view 50 m away misses it, so the frustum test discriminates.
		CHECK_FALSE(in_far(published));
		loaded.unref();
		DirAccess::remove_absolute(path);
	}
}
