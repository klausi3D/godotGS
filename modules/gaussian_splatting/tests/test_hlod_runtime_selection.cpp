extern "C" int test_hlod_runtime_selection_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_runtime_selection.h"
#include "../core/gs_vector_alloc.h"
#include <algorithm>
#include <limits>
#include <vector>

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
// The record estimate's distance is the distance to the near surface of the node's bounding sphere, floored at
// kCostInsideDistanceFraction (0.25) x radius. Without the floor a camera inside the sphere gives an unbounded estimate
// and no frontier is ever admitted, so a world with no canonical route (V4 paged, streamed) draws nothing for good (camera 2 m
// inside the holzbank: estimate 1e17, measured demand 5.5e5). The floor is a heuristic, not an upper bound: it also lowers
// the estimate in the shell between radius and 1.25 x radius outside the sphere. Expected values below are hard-coded
// (not derived from the constant) so a changed constant is caught.
// Fixture: footprint 1 m2, focal 100 px, tile area 256 -> term = 1e4 / (d^2 x 256); payload 2 (interior) or 4 (leaf).
static void sphere_tree(GaussianSplatHlodRuntimeData &r_data, float p_root, float p_interior, float p_leaf) {
	tree(r_data);
	for (int i = 0; i < 7; ++i) {
		auto &n = r_data.nodes.write[i];
		n.topology.radius = i == 0 ? p_root : (i < 3 ? p_interior : p_leaf);
		n.footprint = 1.0;
	}
}
static gs_hlod_runtime::CutRequest sphere_request() {
	auto r = request();
	r.tile_area = 256.0;
	return r;
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut admits a frontier when the camera is inside every node sphere") {
	GaussianSplatHlodRuntimeData data;
	sphere_tree(data, 5.0f, 5.0f, 5.0f);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = sphere_request();
	r.camera = Vector3(0, 0, -10); // The node centre.
	r.record_budget = 100000; // 80 percent = 80000 records.
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_count == 4); // The leaves: the quality rule refines everything at distance 0.
	CHECK(cut.estimated_records == doctest::Approx(116.0)); // Floor distance 1.25: each leaf is 4 + 25 = 29.
	// The floor does not hide a genuinely over-budget view: 80 percent of 30 = 24 is below every node (27, 27, 29).
	r.record_budget = 30;
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut record estimate is finite, monotone towards a node, and exact outside, in the shell and inside") {
	GaussianSplatHlodRuntimeData data;
	sphere_tree(data, 5.0f, 5.0f, 5.0f);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = sphere_request();
	r.tau_pixels = 1e30; // No refinement: the estimate is the root node's own cost (payload 2).
	r.record_budget = 1e30;
	r.splat_budget = 1000000;
	auto root_estimate = [&](float p_distance) {
		r.camera = Vector3(0, 0, -10.0f + p_distance); // p_distance from the node centre; the radius is 5.
		auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		return cut.estimated_records;
	};
	double previous = 0.0;
	for (int i = 0; i <= 80; ++i) { // From 40 m away to the centre.
		const double e = root_estimate(40.0f - 0.5f * i);
		CHECK(std::isfinite(e));
		CHECK(e >= previous);
		previous = e;
	}
	CHECK(root_estimate(30.0f) == doctest::Approx(2.0625)); // Outside the shell: near-surface d = 25, unchanged by the floor.
	CHECK(root_estimate(6.5f) == doctest::Approx(19.361111)); // Just outside the shell: d = 1.5, unchanged.
	CHECK(root_estimate(6.25f) == doctest::Approx(27.0)); // Shell edge: d = 1.25 either way.
	// The shell (radius < distance < 1.25 x radius) IS floored: the unfloored near-surface distance would give 41.0625 and 158.25.
	CHECK(root_estimate(6.0f) == doctest::Approx(27.0));
	CHECK(root_estimate(5.5f) == doctest::Approx(27.0));
	CHECK(root_estimate(4.0f) == doctest::Approx(27.0)); // Strictly inside.
	CHECK(root_estimate(0.0f) == doctest::Approx(27.0)); // Centre.
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut record floor follows each node's own radius when children are smaller than their parent") {
	GaussianSplatHlodRuntimeData data;
	sphere_tree(data, 8.0f, 4.0f, 2.0f); // Floors 2.0, 1.0 and 0.5.
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = sphere_request();
	r.camera = Vector3(0, 0, -10);
	r.record_budget = 1000; // 80 percent = 800.
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_count == 4);
	// Leaf: 4 + 1e4 / (0.25 x 256) = 160.25; four leaves 641. (Interior 2 + 39.0625, root 2 + 9.765625.)
	CHECK(cut.estimated_records == doctest::Approx(641.0));
	r.record_budget = 100; // 80 percent = 80: only the root (11.765625) fits; the interiors (82.125 together) are refused.
	auto root_only = history.select(data, r);
	CHECK(root_only.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(root_only.selected_count == 1);
	CHECK(root_only.estimated_records == doctest::Approx(11.765625));
	r.record_budget = 110; // 80 percent = 88: the two interiors (82.125) fit; the leaves (641) do not.
	auto coarse = history.select(data, r);
	CHECK(coarse.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(coarse.selected_count == 2);
	CHECK(coarse.estimated_records == doctest::Approx(82.125));
}

static std::vector<uint32_t> demand_of(const gs_hlod_runtime::CutHistory &p_history) {
	std::vector<uint32_t> out;
	for (uint32_t i = 0; i < p_history.demand_count(); ++i) { out.push_back(p_history.demand_node(i)); }
	return out;
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut reports Desired demand for missing nodes, as whole child groups only when the transition fits") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	const uint64_t bytes = history.allocated_bytes();
	auto r = request();
	// Root not resident: the root is the demand, nothing is selected.
	uint8_t none[7] = { 0, 0, 0, 0, 0, 0, 0 };
	r.resident = none; r.resident_count = 7;
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::UNAVAILABLE);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 0 });
	// Root only: the whole child group is demanded, parent stays selected.
	uint8_t root[7] = { 1, 0, 0, 0, 0, 0, 0 };
	r.resident = root;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_count == 1);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 1, 2 });
	// One child resident: only the missing child is demanded and the cut still does not move.
	uint8_t half[7] = { 1, 1, 0, 0, 0, 0, 0 };
	r.resident = half;
	cut = history.select(data, r);
	CHECK(cut.selected_count == 1);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 2 });
	// Demand is bounded by the budget: the children (4 splats) would be refused under a 3 splat cap, so no
	// bandwidth is spent on them.
	r.resident = root;
	r.splat_budget = 3;
	cut = history.select(data, r);
	CHECK(cut.selected_count == 1);
	CHECK(demand_of(history).empty());
	r.splat_budget = 100;
	// Everything resident: nothing to ask for, and a later call clears the previous call's demand.
	r.resident = nullptr;
	cut = history.select(data, r);
	CHECK(cut.selected_count == 4);
	CHECK(demand_of(history).empty());
	// The demand list is preallocated: selecting never grows the workspace.
	CHECK(history.allocated_bytes() == bytes);
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut demand is planned against the budget together with the higher-priority demand") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t interiors[7] = { 1, 1, 1, 0, 0, 0, 0 };
	r.resident = interiors; r.resident_count = 7;
	// Cut {1, 2} = 4 splats. Refining node 1 adds 6 (10, fits a 10 splat cap); refining node 2 as well would
	// reach 16. Once node 1's children are resident the walk stops at node 2, so asking for node 2's children
	// now would only spend bandwidth and atlas pages on content the budget will refuse.
	r.splat_budget = 10;
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_count == 2);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	// With room for both transitions, both groups are demanded in priority order.
	r.splat_budget = 16;
	cut = history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut bootstrap demands every missing child of the expansion it needs") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3(0, 0, 0);
	r.tau_pixels = 1e9;
	r.splat_budget = 90;
	uint8_t root[7] = { 1, 0, 0, 1, 1, 1, 1 };
	r.resident = root; r.resident_count = 7;
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(cut.selected_count == 0);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 1, 2 });
	uint8_t frontier[7] = { 1, 1, 1, 1, 1, 1, 1 };
	r.resident = frontier;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(demand_of(history).empty());
	CHECK(history.partial_frontier_count() == 0);
	// A frontier that needs a second expansion: 45 splats fits neither the root (100), {1, 2} (80) nor {2, 3, 4}
	// (48), so node 1 is expanded into {3, 4} and node 2's children {5, 6} are missing. The reached frontier {2, 3, 4} is
	// resident work the residency layer must keep while {5, 6} load; it is reported apart from the demand.
	r.splat_budget = 45;
	uint8_t partial[7] = { 1, 1, 1, 1, 1, 0, 0 };
	r.resident = partial;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(cut.selected_count == 0);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 5, 6 });
	std::vector<uint32_t> kept;
	for (uint32_t i = 0; i < history.partial_frontier_count(); ++i) { kept.push_back(history.partial_frontier_node(i)); }
	std::sort(kept.begin(), kept.end());
	CHECK(kept == std::vector<uint32_t>{ 2, 3, 4 });
	// Every other status clears it.
	r.resident = frontier;
	r.splat_budget = 90;
	history.select(data, r);
	CHECK(history.partial_frontier_count() == 0);
}
TEST_CASE("[GaussianSplatting][HLOD] runtime cut plans demand in atlas pages, ancestors of the cut included") {
	GaussianSplatHlodRuntimeData data;
	tree(data); // Every node holds fewer splats than a page, so each occupies exactly one page.
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t interiors[7] = { 1, 1, 1, 0, 0, 0, 0 };
	r.resident = interiors; r.resident_count = 7;
	// Splats and records would admit every transition (cap 100 splats, 16 needed).
	r.page_splats = 1024;
	// Cut {1, 2} keeps three pages resident (root and both interiors). Each child group adds two pages.
	r.page_budget = 5;
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 }); // 3 + 2 = 5 fits, the second group (7) does not.
	r.page_budget = 7;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
	r.page_budget = 4; // Not even one group fits next to the ancestors.
	history.select(data, r);
	CHECK(demand_of(history).empty());
	// Pages round up: 2000 splats occupy two pages, so each leaf pair costs four pages (3 + 4 = 7 > 6).
	for (int i = 3; i < 7; ++i) { data.nodes.write[i].topology.payload_count = 2000; }
	r.splat_budget = 100000;
	r.page_budget = 6;
	history.select(data, r);
	CHECK(demand_of(history).empty());
	r.page_budget = 7;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	// Without a page plan the selector behaves as before.
	r.page_splats = 0;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
}
}
