/**************************************************************************/
/*  test_gaussian_importance_prune.h                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

// Unit tests for GaussianData::prune_by_importance() (GS-PERF-PRUNE, issue #456,
// slice 2a). Exercises the pure CORE method that the PLY/SPZ importer (slice 2b)
// will call between full array materialization and the streaming chunk bake. The
// method reuses the shared #420 metric (ResidentAtlasBudget::gaussian_importance()
// + select_top_k_indices()) and forward-in-place compacts the Gaussian payload and
// its strided high-order SH block together.
//
// See docs/architecture/adr-import-importance-pruning.md (Decisions).

#include "../core/gaussian_data.h"
#include "tests/test_macros.h"

#include <cstring>

namespace GaussianPruneTests {

// One splat whose importance == clamp(opacity,0,1) * (max|scale| + 1e-4) is driven
// solely by p_importance_scale (opacity 1, other axes below it), so a test can lay
// out an exact importance ranking. position.x encodes the SOURCE index so a compacted
// payload can be traced back to the source splat it came from. Every field is set to a
// GPU-valid value so validate_gpu_payload() can double as the "storage is consistent"
// assertion (it enforces sh_high_order size == count * stride).
static Gaussian make_splat(uint32_t p_source_index, float p_importance_scale) {
	Gaussian g = {};
	g.position = Vector3(float(p_source_index), 0.0f, 0.0f);
	g.opacity = 1.0f;
	g.scale = Vector3(p_importance_scale, 0.01f, 0.01f); // max|scale| == p_importance_scale
	g.rotation = Quaternion(); // identity, unit length
	g.sh_dc = Color(1, 1, 1, 1);
	g.normal = Vector3(0, 1, 0);
	g.area = 1.0f;
	g.brush_axes = Vector2(1.0f, 1.0f);
	g.stroke_age = 0.0f;
	return g;
}

// Build a GaussianData with the given per-index importance scales and a high-order SH
// block of `p_stride` Vector3 per splat. SH coefficient c of source i is set to
// Vector3(i, c, 0) so compaction can be verified value-by-value. p_stride == 0 builds
// an asset with NO high-order SH.
static void build_dataset(Ref<::GaussianData> &p_data, const Vector<float> &p_scales, uint32_t p_stride) {
	LocalVector<Gaussian> gaussians;
	LocalVector<Vector3> sh_high_order;
	const uint32_t count = uint32_t(p_scales.size());
	gaussians.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		gaussians[i] = make_splat(i, p_scales[i]);
	}
	if (p_stride > 0u) {
		sh_high_order.resize(count * p_stride);
		for (uint32_t i = 0; i < count; i++) {
			for (uint32_t c = 0; c < p_stride; c++) {
				sh_high_order[i * p_stride + c] = Vector3(float(i), float(c), 0.0f);
			}
		}
	}
	p_data->set_gaussian_payload(gaussians, sh_high_order, /*first_order*/ 0u, p_stride, /*2d*/ false);
}

// Assert the kept payload matches exactly the expected ascending source indices, both
// in the Gaussian payload (position.x) and in the strided high-order SH block.
static void check_kept(const Ref<::GaussianData> &p_data, const Vector<int> &p_expected_sources, uint32_t p_stride) {
	REQUIRE(p_data->get_count() == p_expected_sources.size());
	const Gaussian *g = p_data->get_gaussians();
	REQUIRE(g != nullptr);
	const Vector3 *sh = p_data->get_sh_high_order_coefficients_ptr();
	if (p_stride > 0u) {
		CHECK(p_data->get_sh_high_order_count() == p_stride);
		REQUIRE(sh != nullptr);
	}
	for (int j = 0; j < p_expected_sources.size(); j++) {
		const int src = p_expected_sources[j];
		CHECK(int(g[j].position.x) == src);
		if (p_stride > 0u) {
			for (uint32_t c = 0; c < p_stride; c++) {
				const Vector3 &v = sh[uint32_t(j) * p_stride + c];
				CHECK(int(v.x) == src); // source index preserved
				CHECK(int(v.y) == int(c)); // coefficient slot preserved
			}
		}
	}
	// validate_gpu_payload() enforces sh_high_order_coefficients.size() == count * stride,
	// so an OK result proves the SH block was resized to exactly kept * stride (no stale tail).
	CHECK(p_data->validate_gpu_payload() == OK);
}

} // namespace GaussianPruneTests

TEST_CASE("[GaussianSplatting][GaussianPrune] No-op default leaves storage byte-identical") {
	using namespace GaussianPruneTests;
	Ref<::GaussianData> data;
	data.instantiate();
	Vector<float> scales;
	for (int i = 0; i < 6; i++) {
		scales.push_back(0.1f * float(i + 1));
	}
	const uint32_t stride = 2u;
	build_dataset(data, scales, stride);

	const uint64_t rev_before = data->get_content_revision();

	// Snapshot the raw bytes of both parallel arrays before pruning.
	LocalVector<Gaussian> gaussians_before;
	gaussians_before.resize(6);
	memcpy(gaussians_before.ptr(), data->get_gaussians(), sizeof(Gaussian) * 6);
	LocalVector<Vector3> sh_before;
	sh_before.resize(6 * stride);
	memcpy(sh_before.ptr(), data->get_sh_high_order_coefficients_ptr(), sizeof(Vector3) * 6 * stride);

	// Default no-op: keep_ratio 1.0, threshold 0.0 -> must NOT touch storage.
	const uint32_t kept = data->prune_by_importance(1.0, 0.0f);

	CHECK(kept == 6u);
	CHECK(data->get_count() == 6);
	CHECK(data->get_sh_high_order_count() == stride);
	// Content revision is untouched (proves the fast path skipped all mutation).
	CHECK(data->get_content_revision() == rev_before);
	// Byte-for-byte identical payload and SH.
	CHECK(memcmp(data->get_gaussians(), gaussians_before.ptr(), sizeof(Gaussian) * 6) == 0);
	CHECK(memcmp(data->get_sh_high_order_coefficients_ptr(), sh_before.ptr(), sizeof(Vector3) * 6 * stride) == 0);
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Ratio keeps the round(count*ratio) highest-importance splats") {
	using namespace GaussianPruneTests;
	const uint32_t stride = 3u;

	SUBCASE("Even count: ratio 0.5 of 8 keeps the top 4") {
		Ref<::GaussianData> data;
		data.instantiate();
		Vector<float> scales; // importance strictly increases with index
		for (int i = 0; i < 8; i++) {
			scales.push_back(0.1f * float(i + 1));
		}
		build_dataset(data, scales, stride);

		const uint32_t kept = data->prune_by_importance(0.5, 0.0f);
		CHECK(kept == 4u);
		// Top 4 by importance are the 4 highest indices, returned in ascending source order.
		Vector<int> expected;
		expected.push_back(4);
		expected.push_back(5);
		expected.push_back(6);
		expected.push_back(7);
		check_kept(data, expected, stride);
	}

	SUBCASE("Odd count: ratio 0.5 of 5 rounds half up to 3") {
		Ref<::GaussianData> data;
		data.instantiate();
		Vector<float> scales;
		for (int i = 0; i < 5; i++) {
			scales.push_back(0.1f * float(i + 1));
		}
		build_dataset(data, scales, stride);

		// round(5 * 0.5) == round(2.5) == 3 (half away from zero).
		const uint32_t kept = data->prune_by_importance(0.5, 0.0f);
		CHECK(kept == 3u);
		Vector<int> expected;
		expected.push_back(2);
		expected.push_back(3);
		expected.push_back(4);
		check_kept(data, expected, stride);
	}
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Threshold drops sub-threshold splats") {
	using namespace GaussianPruneTests;
	const uint32_t stride = 2u;
	Ref<::GaussianData> data;
	data.instantiate();
	Vector<float> scales; // importance ~= 0.1001 .. 0.6001 for indices 0..5
	for (int i = 0; i < 6; i++) {
		scales.push_back(0.1f * float(i + 1));
	}
	build_dataset(data, scales, stride);

	// Threshold 0.35 with no ratio limit: keep importance >= 0.35 -> indices 3,4,5.
	const uint32_t kept = data->prune_by_importance(1.0, 0.35f);
	CHECK(kept == 3u);
	Vector<int> expected;
	expected.push_back(3);
	expected.push_back(4);
	expected.push_back(5);
	check_kept(data, expected, stride);
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Ratio and threshold intersect (a splat must pass both)") {
	using namespace GaussianPruneTests;
	const uint32_t stride = 3u;
	Ref<::GaussianData> data;
	data.instantiate();
	// Non-monotonic importances so ratio and threshold each bind on different splats.
	// index:      0     1     2     3     4     5
	// importance: 0.9   0.2   0.7   0.3   0.8   0.1
	Vector<float> scales;
	scales.push_back(0.9f);
	scales.push_back(0.2f);
	scales.push_back(0.7f);
	scales.push_back(0.3f);
	scales.push_back(0.8f);
	scales.push_back(0.1f);
	build_dataset(data, scales, stride);

	// ratio 0.5 -> keep top 3 by importance = {0, 2, 4}.
	// threshold 0.25 -> globally keeps {0, 2, 3, 4} (index 3 @ 0.3 passes the threshold).
	// Intersection = {0, 2, 4}: index 3 passes the THRESHOLD but is outside the ratio top-3,
	// so the ratio constraint still drops it (proves AND, not threshold-only).
	const uint32_t kept = data->prune_by_importance(0.5, 0.25f);
	CHECK(kept == 3u);
	Vector<int> expected;
	expected.push_back(0);
	expected.push_back(2);
	expected.push_back(4);
	check_kept(data, expected, stride);
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Keep-top-1 clamp when threshold exceeds every splat") {
	using namespace GaussianPruneTests;
	const uint32_t stride = 2u;
	Ref<::GaussianData> data;
	data.instantiate();
	Vector<float> scales; // max importance ~= 0.6001 at index 5
	for (int i = 0; i < 6; i++) {
		scales.push_back(0.1f * float(i + 1));
	}
	build_dataset(data, scales, stride);

	// Threshold above every splat's importance would prune to zero; the clamp keeps the
	// single highest-importance splat instead (index 5) and emits WARN_PRINT_ONCE.
	const uint32_t kept = data->prune_by_importance(1.0, 10.0f);
	CHECK(kept == 1u);
	Vector<int> expected;
	expected.push_back(5); // the max-importance splat survives
	check_kept(data, expected, stride);
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Deterministic: same input + options -> identical output") {
	using namespace GaussianPruneTests;
	const uint32_t stride = 3u;
	Vector<float> scales;
	scales.push_back(0.5f);
	scales.push_back(0.9f);
	scales.push_back(0.1f);
	scales.push_back(0.7f);
	scales.push_back(0.3f);
	scales.push_back(0.8f);
	scales.push_back(0.2f);
	scales.push_back(0.6f);

	Ref<::GaussianData> a;
	a.instantiate();
	build_dataset(a, scales, stride);
	Ref<::GaussianData> b;
	b.instantiate();
	build_dataset(b, scales, stride);

	const uint32_t kept_a = a->prune_by_importance(0.5, 0.0f);
	const uint32_t kept_b = b->prune_by_importance(0.5, 0.0f);
	REQUIRE(kept_a == kept_b);
	REQUIRE(a->get_count() == b->get_count());

	const Gaussian *ga = a->get_gaussians();
	const Gaussian *gb = b->get_gaussians();
	const Vector3 *sha = a->get_sh_high_order_coefficients_ptr();
	const Vector3 *shb = b->get_sh_high_order_coefficients_ptr();
	for (int j = 0; j < a->get_count(); j++) {
		CHECK(ga[j].position.x == gb[j].position.x);
		for (uint32_t c = 0; c < stride; c++) {
			CHECK(sha[uint32_t(j) * stride + c].x == shb[uint32_t(j) * stride + c].x);
		}
	}
}

TEST_CASE("[GaussianSplatting][GaussianPrune] Asset with no high-order SH prunes correctly") {
	using namespace GaussianPruneTests;
	Ref<::GaussianData> data;
	data.instantiate();
	Vector<float> scales;
	for (int i = 0; i < 4; i++) {
		scales.push_back(0.1f * float(i + 1));
	}
	build_dataset(data, scales, /*stride*/ 0u); // no high-order SH

	CHECK(data->get_sh_high_order_count() == 0u);
	CHECK(data->get_sh_high_order_coefficients_ptr() == nullptr);

	const uint32_t kept = data->prune_by_importance(0.5, 0.0f);
	CHECK(kept == 2u);
	Vector<int> expected;
	expected.push_back(2);
	expected.push_back(3);
	check_kept(data, expected, /*stride*/ 0u);
	// Still no high-order SH after pruning.
	CHECK(data->get_sh_high_order_count() == 0u);
	CHECK(data->get_sh_high_order_coefficients_ptr() == nullptr);
}
