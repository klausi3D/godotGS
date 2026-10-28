extern "C" int test_hlod_runtime_selection_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_runtime_selection.h"
#include "../core/gs_vector_alloc.h"
#include <limits>

namespace TestHlodRuntimeSelection {
static void tree(GaussianSplatHlodRuntimeData &r_data) {
	r_data.nodes.resize(7); // Fixed synthetic topology, no payload allocation.
	for (int i = 0; i < 7; ++i) {
		auto &n = r_data.nodes.write[i];
		n.bounds_center = Vector3(0, 0, -10);
		n.topology.payload_count = i < 3 ? 2 : 4;
		n.topology.geometric_error = i == 0 ? 4.0f : (i < 3 ? 2.0f : 0.0f);
		if (i < 3) { n.topology.first_child = i * 2 + 1; n.topology.child_count = 2; }
	}
}
static gs_hlod_runtime::CutRequest request() {
	gs_hlod_runtime::CutRequest r;
	r.frustum_enabled = false;
	r.focal_pixels = 100;
	r.tau_pixels = 1;
	r.record_budget = 100000;
	r.splat_budget = 100;
	r.node_budget = 7;
	return r;
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut covers leaves with deterministic first-refusal and fixed-point history") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	for (int repeat = 0; repeat < 3; ++repeat) {
		auto full = history.select(data, r);
		CHECK(full.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(full.selected_count == 4);
		CHECK(full.selected_splats == 16);
		if (full.selected_count != 4) { return; }
		for (uint32_t i = 0; i < 4; ++i) { CHECK(history.selected_node(i) == i + 3); }
	}
	history.invalidate();
	r.splat_budget = 10;
	auto capped = history.select(data, r);
	CHECK(capped.budget_bound);
	CHECK(capped.selected_splats == 10);
	CHECK(capped.selected_count == 3);
	if (capped.selected_count != 3) { return; }
	CHECK(history.selected_node(0) == 2); // Stable tie chooses node 1 before node 2.
	CHECK(history.selected_node(1) == 3);
	CHECK(history.selected_node(2) == 4);
	const uint64_t bytes = history.allocated_bytes();
	auto again = history.select(data, r);
	CHECK(again.selected_splats == capped.selected_splats);
	CHECK(again.selected_count == capped.selected_count);
	CHECK(history.allocated_bytes() == bytes);
	r.splat_budget = 1;
	auto dropped = history.select(data, r);
	CHECK(dropped.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE); // Every frontier exceeds one splat.
	CHECK(dropped.selected_count == 0);
	CHECK(dropped.selected_splats == 0);
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut retains complete parent on unavailable child and rejects invalid arithmetic") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t resident[7] = {1, 0, 1, 1, 1, 1, 1};
	r.resident = resident; r.resident_count = 7;
	auto cut = history.select(data, r);
	CHECK(cut.selected_count == 1);
	if (cut.selected_count != 1) { return; }
	CHECK(history.selected_node(0) == 0);
	r.resident = nullptr;
	r.tau_pixels = std::numeric_limits<double>::quiet_NaN();
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::INVALID);
	r.tau_pixels = 1;
	r.record_budget = 2; // 80% is below the root's two complete records.
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	r.record_budget = 2.5; // Exactly one 80% admission factor.
	r.splat_budget = 2;
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::COMPLETE);
	uint64_t bytes = 99;
	CHECK_FALSE(gs_hlod_runtime::allocation_bytes(UINT64_MAX, 8, bytes));
	CHECK_FALSE(gs_hlod_runtime::allocation_bytes(8, 8, bytes, 64));
	CHECK(gs_hlod_runtime::allocation_bytes(8, 8, bytes, 80));
	CHECK(bytes == 80);
}
// Binary tree of 7 nodes: a huge root payload that never fits, interiors 40 and leaves 4 splats.
static void heavy_root_tree(GaussianSplatHlodRuntimeData &r_data) {
	tree(r_data);
	for (int i = 0; i < 7; ++i) {
		auto &n = r_data.nodes.write[i];
		n.topology.payload_count = i == 0 ? 100 : (i < 3 ? 40 : 4);
		n.topology.radius = i == 0 ? 50.0f : 5.0f; // Camera at (0,0,0) is inside the root bounds.
	}
}
static bool covers_without_overlap(const gs_hlod_runtime::CutHistory &p_history, uint32_t p_count, uint32_t p_nodes) {
	// Complete binary heap layout: every leaf must have exactly one selected ancestor-or-self.
	for (uint32_t leaf = p_nodes / 2; leaf < p_nodes; ++leaf) {
		uint32_t hits = 0;
		for (uint32_t i = 0; i < p_count; ++i) {
			for (uint32_t a = leaf;; a = (a - 1) / 2) {
				if (a == p_history.selected_node(i)) { ++hits; break; }
				if (a == 0) { break; }
			}
		}
		if (hits != 1) { return false; }
	}
	return true;
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut bootstraps a complete child frontier inside a root that exceeds a cap") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3(0, 0, 0);
	r.tau_pixels = 1e9; // No refinement: isolate the bootstrap frontier.
	r.splat_budget = 90; // Root (100) is over the splat cap; the two interiors (80) fit.
	const uint64_t bytes = history.allocated_bytes();
	for (int repeat = 0; repeat < 3; ++repeat) {
		auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(cut.selected_count == 2);
		CHECK(cut.selected_splats == 80);
		CHECK(cut.bootstrapped);
		if (cut.selected_count != 2) { return; }
		CHECK(history.selected_node(0) == 1);
		CHECK(history.selected_node(1) == 2);
		CHECK(covers_without_overlap(history, cut.selected_count, 7));
		CHECK(cut.visited_nodes <= 1 + 7 * gs_hlod::kMaxChildren);
	}
	CHECK(history.allocated_bytes() == bytes);
	// Record cap alone (root cost 100 > 80 percent of 100): same frontier.
	r.splat_budget = 1000;
	r.record_budget = 100;
	auto by_records = history.select(data, r);
	CHECK(by_records.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(by_records.selected_count == 2);
	// Once the frontier fits, the first-refusal refinement walk continues from it.
	r.record_budget = 100000;
	r.tau_pixels = 1;
	r.splat_budget = 90;
	history.invalidate();
	auto refined = history.select(data, r);
	CHECK(refined.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(refined.selected_count == 4);
	CHECK(refined.selected_splats == 16);
	CHECK(covers_without_overlap(history, refined.selected_count, 7));
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut bootstrap reports distinct explicit failures and never a partial cut") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3(0, 0, 0);
	r.tau_pixels = 1e9;
	r.splat_budget = 90;
	// Missing residency: a visible child needed by the only expansion is not resident.
	uint8_t resident[7] = {1, 1, 0, 1, 1, 1, 1};
	r.resident = resident; r.resident_count = 7;
	auto missing = history.select(data, r);
	CHECK(missing.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(missing.selected_count == 0);
	CHECK(missing.selected_splats == 0);
	r.resident = nullptr;
	r.resident_count = 0;
	// Genuinely infeasible: even the leaf frontier (16 splats) exceeds the cap.
	r.splat_budget = 10;
	auto infeasible = history.select(data, r);
	CHECK(infeasible.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(infeasible.selected_count == 0);
	// The node cap refuses the only frontier that fits the splat cap.
	r.splat_budget = 90;
	r.node_budget = 1;
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	// Arithmetic invalidity: a child cost is not finite.
	r.node_budget = 7;
	data.nodes.write[1].footprint = std::numeric_limits<float>::infinity();
	auto arithmetic = history.select(data, r);
	CHECK(arithmetic.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_ARITHMETIC);
	CHECK(arithmetic.selected_count == 0);
	data.nodes.write[1].footprint = 0.0f;
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::COMPLETE);
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut bootstrap search work is bounded and exhaustion is distinct from infeasibility") {
	const uint32_t node_budget = 2;
	const uint32_t depth = 1 + node_budget * gs_hlod::kMaxChildren + 8; // One child per expansion: one visit each.
	GaussianSplatHlodRuntimeData data;
	data.nodes.resize(depth);
	for (uint32_t i = 0; i < depth; ++i) {
		auto &n = data.nodes.write[i];
		n.bounds_center = Vector3(0, 0, -10);
		n.topology.payload_count = i + 1 == depth ? 1 : 100;
		if (i + 1 < depth) { n.topology.first_child = i + 1; n.topology.child_count = 1; }
	}
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(depth)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.tau_pixels = 1e9;
	r.splat_budget = 50;
	r.node_budget = node_budget;
	auto exhausted = history.select(data, r);
	CHECK(exhausted.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_EXHAUSTED);
	CHECK(exhausted.selected_count == 0);
	CHECK(exhausted.visited_nodes <= 2 + node_budget * gs_hlod::kMaxChildren);
	r.node_budget = depth; // A larger configured bound reaches the fitting leaf.
	auto found = history.select(data, r);
	CHECK(found.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(found.selected_count == 1);
	if (found.selected_count == 1) { CHECK(history.selected_node(0) == depth - 1); }
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut budget sweep stays monotone across bootstrap transitions") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3(0, 0, 0);
	// tau 1: the LOD wants full refinement, so more budget never coarsens any leaf's covering node.
	// tau 1e9: the LOD wants the root; the forced bootstrap frontier only gets coarser (stops earlier) with budget.
	for (const double tau : { 1.0, 1e9 }) {
		r.tau_pixels = tau;
		const bool wants_refinement = tau < 1000.0;
		uint32_t previous_depth[4] = { 0, 0, 0, 0 };
		bool have_previous = false;
		uint32_t distinct_depth_vectors = 0;
		for (uint64_t budget = 1; budget <= 200; ++budget) {
			history.invalidate(); // Compare cold selections per budget.
			r.splat_budget = budget;
			auto cut = history.select(data, r);
			if (cut.status != gs_hlod_runtime::CutHistory::COMPLETE) {
				CHECK(cut.selected_count == 0);
				CHECK_FALSE(have_previous); // Once feasible, raising a budget never loses the cut.
				continue;
			}
			CHECK(covers_without_overlap(history, cut.selected_count, 7));
			uint32_t depth_of_leaf[4] = { 0, 0, 0, 0 };
			for (uint32_t i = 0; i < cut.selected_count; ++i) {
				const uint32_t node = history.selected_node(i);
				const uint32_t d = node == 0 ? 0 : (node < 3 ? 1 : 2);
				for (uint32_t leaf = 0; leaf < 4; ++leaf) {
					uint32_t a = leaf + 3;
					while (a > node) { a = (a - 1) / 2; }
					if (a == node) { depth_of_leaf[leaf] = d; }
				}
			}
			if (have_previous) {
				for (uint32_t leaf = 0; leaf < 4; ++leaf) {
					CHECK(wants_refinement ? depth_of_leaf[leaf] >= previous_depth[leaf] : depth_of_leaf[leaf] <= previous_depth[leaf]);
				}
				distinct_depth_vectors += memcmp(previous_depth, depth_of_leaf, sizeof(depth_of_leaf)) != 0 ? 1u : 0u;
			}
			memcpy(previous_depth, depth_of_leaf, sizeof(depth_of_leaf));
			have_previous = true;
		}
		CHECK(have_previous);
		CHECK(distinct_depth_vectors == (wants_refinement ? 0u : 3u)); // Bootstrap transitions leaves -> {2,3,4} -> {1,2} -> root.
	}
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut keeps refinement hysteresis for a bootstrap-expanded parent") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	for (int i = 0; i < 7; ++i) { data.nodes.write[i].topology.radius = 1.0f; }
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3(0, 0, 0);
	r.tau_pixels = 40; // Root error 44.4 lies in the (tau, 1.25 tau] hysteresis band; interiors (22.2) stay put.
	r.splat_budget = 90; // Root payload 100 is over the cap: bootstrap expands it.
	auto first = history.select(data, r);
	CHECK(first.bootstrapped);
	CHECK(first.selected_count == 2);
	r.splat_budget = 1000; // Root now fits; only the previous-frame stamp keeps it refined.
	auto second = history.select(data, r);
	CHECK(second.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK_FALSE(second.bootstrapped);
	CHECK(second.selected_count == 2);
	history.invalidate();
	CHECK(history.select(data, r).selected_count == 1); // Cold start in the band: no hysteresis, root stays.
}
}
