extern "C" int test_hlod_runtime_selection_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../lod/hlod_runtime_selection.h"
#include "../renderer/hlod_cut_telemetry.h"
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
	r_data.prepare_placement_reserve();
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

TEST_CASE("[GaussianSplatting][HLOD] cut diagnostics distinguish history holds cap refusals and demanded residency misses") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	// Historical workspace plus the explicitly admitted parallel transition descriptors.
	uint64_t transitions = 0;
	CHECK(gs_hlod_runtime::allocation_bytes(7, sizeof(gs_hlod_runtime::TransitionGroup), transitions));
	CHECK(gs_hlod_runtime::CutHistory::workspace_bytes(7) == 368 + transitions);
	CHECK(history.allocated_bytes() == 392 + transitions); // Includes the 24-byte diagnostic allocation.
	auto r = request();
	uint8_t resident[7] = {1, 0, 1, 1, 1, 1, 1};
	r.resident = resident; r.resident_count = 7;
	auto missing = history.select(data, r);
	CHECK(missing.residency_missed_count == 1);
	CHECK(history.diagnostic_flags(0) == (gs_hlod_runtime::CutHistory::RESIDENCY_MISSED | gs_hlod_runtime::CutHistory::MISS_DEMANDED));
	CHECK(missing.first_cap_refused_node == -1);
	CHECK(history.demand_count() == 1);
	r.page_splats = 1; r.page_budget = 2; // Root fits; the missing-child closure does not.
	auto undemanded = history.select(data, r);
	CHECK(undemanded.residency_missed_count == 1);
	CHECK(history.diagnostic_flags(0) == gs_hlod_runtime::CutHistory::RESIDENCY_MISSED);
	CHECK(undemanded.first_cap_refused_node == 0);
	CHECK(history.demand_count() == 0);
	r.resident = nullptr; r.page_splats = 0;
	r.splat_budget = 10;
	auto capped = history.select(data, r);
	CHECK(capped.budget_bound);
	CHECK(capped.first_cap_refused_node == 2);
	CHECK(capped.residency_missed_count == 0);
	CHECK(history.diagnostic_flags(0) == 0);
	r.splat_budget = 100;
	history.select(data, r); // Seed both interior refinements above the upper threshold.
	r.focal_pixels = 6.25; // e(interior) == 1.25 tau: retained only because of history.
	auto held = history.select(data, r);
	CHECK(held.hysteresis_held_count == 2);
	CHECK(history.diagnostic_flags(1) == gs_hlod_runtime::CutHistory::HYSTERESIS_HELD);
	CHECK(history.diagnostic_flags(2) == gs_hlod_runtime::CutHistory::HYSTERESIS_HELD);
	CHECK(held.selected_count == 4);
	const uint64_t bytes = history.allocated_bytes();
	r.focal_pixels = 5; // e == tau: history must no longer hold either refinement.
	auto released = history.select(data, r);
	CHECK(released.hysteresis_held_count == 0);
	CHECK(released.selected_count == 2);
	CHECK(history.diagnostic_flags(1) == 0);
	CHECK(history.allocated_bytes() == bytes);
	history.invalidate();
	r.focal_pixels = 6.25;
	CHECK(history.select(data, r).hysteresis_held_count == 0);
	r.tau_pixels = 0; // Invalid invocations must not leak previous diagnostic flags.
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::INVALID);
	CHECK(history.diagnostic_flags(2) == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] identical final pose and residency admit different history fixed points (#1455)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	data.nodes.write[3].topology.payload_count = 3;
	data.nodes.write[4].topology.payload_count = 4;
	data.nodes.write[5].topology.payload_count = 4;
	data.nodes.write[6].topology.payload_count = 5;
	gs_hlod_runtime::CutHistory left, right;
	if (!left.reserve(7) || !right.reserve(7)) { FAIL("cut workspaces unavailable"); return; }
	auto r = request();
	r.splat_budget = 11; // Either child refinement fits; both together (16) do not.
	left.select(data, r); // Stable ties refine node 1; node 2 then hits the cap.
	uint8_t resident[7] = {1, 1, 1, 0, 1, 1, 1};
	r.resident = resident; r.resident_count = 7;
	right.select(data, r); // A miss at node 1 lets resident node 2 refine instead.
	r.resident = nullptr; // Both histories now see the exact same all-resident input.
	r.focal_pixels = 5.5; // Both interiors have tau < e=1.1 < 1.25 tau.
	for (int repeat = 0; repeat < 4; ++repeat) {
		const auto a = left.select(data, r);
		const auto b = right.select(data, r);
		CHECK(a.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(b.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(a.selected_count == 3);
		CHECK(b.selected_count == 3);
		CHECK(a.selected_splats == 9);
		CHECK(b.selected_splats == 11);
		CHECK(a.hysteresis_held_count == 1);
		CHECK(b.hysteresis_held_count == 1);
		CHECK(left.diagnostic_flags(1) == gs_hlod_runtime::CutHistory::HYSTERESIS_HELD);
		CHECK(right.diagnostic_flags(2) == gs_hlod_runtime::CutHistory::HYSTERESIS_HELD);
		CHECK(left.demand_count() == 0);
		CHECK(right.demand_count() == 0);
	}
}

TEST_CASE("[GaussianSplatting][HLOD] cut telemetry hashes sorted IDs and replaces diagnostics between frames") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.splat_budget = 10;
	const auto cut = history.select(data, r);
	gs_hlod_runtime::DrawRecord records[3];
	records[0].node = 4; records[1].node = 2; records[2].node = 3;
	HlodCutTelemetry telemetry;
	telemetry.capture(history, cut, 7, records, 3);
	CHECK(telemetry.valid);
	CHECK(telemetry.budget_bound);
	CHECK(telemetry.first_cap_refused_node == 2);
	if (telemetry.drawn_ids.size() != 3) { FAIL("expected three drawn IDs"); return; }
	CHECK(telemetry.drawn_ids[0] == 2);
	CHECK(telemetry.drawn_ids[1] == 3);
	CHECK(telemetry.drawn_ids[2] == 4);
	const String digest = telemetry.drawn_digest;
	std::swap(records[0], records[1]);
	telemetry.capture(history, cut, 7, records, 3);
	CHECK(telemetry.drawn_digest == digest);
	records[0].node = 1;
	telemetry.capture(history, cut, 7, records, 3);
	CHECK(telemetry.drawn_digest != digest);
	uint8_t resident[7] = {1, 0, 1, 1, 1, 1, 1};
	r.resident = resident; r.resident_count = 7;
	const auto missing = history.select(data, r);
	telemetry.capture(history, missing, 7, nullptr, 0);
	CHECK(telemetry.drawn_ids.is_empty());
	CHECK(telemetry.drawn_digest == "cbf29ce484222325"); // FNV-1a empty input, never an absence sentinel.
	CHECK(telemetry.first_cap_refused_node == -1);
	if (telemetry.residency_missed.size() != 1 || telemetry.residency_missed_demanded.size() != 1) {
		FAIL("expected one demanded parent"); return;
	}
	CHECK(telemetry.residency_missed[0] == 0);
	CHECK(bool(telemetry.residency_missed_demanded[0]));
	Dictionary saved;
	telemetry.append_stats(saved, true);
	r.page_splats = 1; r.page_budget = 2;
	const auto undemanded = history.select(data, r);
	telemetry.capture(history, undemanded, 7, records, 3);
	Dictionary next;
	telemetry.append_stats(next, true);
	const Array saved_demanded = saved["hlod_cut_residency_missed_demanded"];
	const Array next_demanded = next["hlod_cut_residency_missed_demanded"];
	if (saved_demanded.size() != 1 || next_demanded.size() != 1) { FAIL("missing snapshot demand status"); return; }
	CHECK(bool(saved_demanded[0]));
	CHECK_FALSE(bool(next_demanded[0]));
	const PackedInt32Array saved_ids = saved["hlod_drawn_node_ids"];
	CHECK(saved_ids.is_empty());
	telemetry.valid = false;
	telemetry.append_stats(next, true);
	CHECK(next["hlod_drawn_node_ids"].get_type() == Variant::NIL);
	CHECK(next["hlod_cut_residency_missed"].get_type() == Variant::NIL);
}
// Binary tree of 7 nodes: a huge root payload that never fits, interiors 40 and leaves 4 splats.
static void heavy_root_tree(GaussianSplatHlodRuntimeData &r_data) {
	tree(r_data);
	for (int i = 0; i < 7; ++i) {
		auto &n = r_data.nodes.write[i];
		n.topology.payload_count = i == 0 ? 100 : (i < 3 ? 40 : 4);
		n.topology.radius = i == 0 ? 50.0f : 5.0f; // Camera at (0,0,0) is inside the root bounds.
	}
	r_data.prepare_placement_reserve();
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
static std::vector<uint32_t> selected_of(const gs_hlod_runtime::CutHistory &p_history, const gs_hlod_runtime::CutHistory::Result &p_cut) {
	std::vector<uint32_t> out;
	for (uint32_t i = 0; i < p_cut.selected_count; ++i) { out.push_back(p_history.selected_node(i)); }
	return out;
}
static uint64_t binary_cut_and_demand_pages(const GaussianSplatHlodRuntimeData &p_data,
		const gs_hlod_runtime::CutHistory &p_history, const gs_hlod_runtime::CutHistory::Result &p_cut, uint32_t p_page_splats) {
	// Independent oracle for these seven-node binary fixtures: union the cut and ancestors, then ALL siblings
	// of each demanded child. This includes resident siblings and parent/child overlap, never telemetry totals.
	if (p_data.nodes.size() != 7 || p_page_splats == 0 || p_cut.selected_count > 7 || p_history.demand_count() > 7) {
		FAIL("page oracle requires the admitted seven-node binary fixture"); return UINT64_MAX;
	}
	bool charged[7] = {};
	auto add_ancestors = [&](uint32_t p_node) {
		for (uint32_t node = p_node;; node = (node - 1) / 2) {
			charged[node] = true;
			if (node == 0) { break; }
		}
	};
	for (uint32_t i = 0; i < p_cut.selected_count; ++i) {
		const uint32_t node = p_history.selected_node(i);
		if (node >= 7) { FAIL("selected node outside page fixture"); return UINT64_MAX; }
		add_ancestors(node);
	}
	for (uint32_t i = 0; i < p_history.demand_count(); ++i) {
		const uint32_t node = p_history.demand_node(i);
		if (node >= 7) { FAIL("demanded node outside page fixture"); return UINT64_MAX; }
		if (node == 0) { add_ancestors(node); continue; }
		const uint32_t first_sibling = ((node - 1) / 2) * 2 + 1;
		add_ancestors(first_sibling);
		add_ancestors(first_sibling + 1);
	}
	uint64_t pages = 0;
	for (uint32_t node = 0; node < 7; ++node) {
		if (charged[node]) { pages += (uint64_t(p_data.nodes[node].topology.payload_count) + p_page_splats - 1) / p_page_splats; }
	}
	return pages;
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
	r.page_budget = 5 + 1; // Five closure pages plus the largest node's one-page reserve.
	auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 }); // 3 + 2 = 5 fits, the second group (7) does not.
	r.page_budget = 7 + 1;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
	r.page_budget = 4 + 1; // Not even one group fits next to the ancestors and reserve.
	history.select(data, r);
	CHECK(demand_of(history).empty());
	// Pages round up: 2000 splats occupy two pages, so each leaf pair costs four pages (3 + 4 = 7 > 6).
	for (int i = 3; i < 7; ++i) { data.nodes.write[i].topology.payload_count = 2000; }
	data.prepare_placement_reserve();
	r.splat_budget = 100000;
	r.page_budget = 6 + 2; // The largest node now reserves two pages.
	history.select(data, r);
	CHECK(demand_of(history).empty());
	r.page_budget = 7 + 2;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	// Without a page plan the selector behaves as before.
	r.page_splats = 0;
	history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
}
TEST_CASE("[GaussianSplatting][HLOD] page-feasible resident refinement keeps a complete parent when children cannot fit (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request(); // Every node resident; splat, node and record caps are loose.
	r.page_splats = 1024;
	r.page_budget = 2; // Root costs one page; retaining it while adding its children would cost three.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 0 });
	CHECK(covers_without_overlap(history, cut.selected_count, 7));
	CHECK(cut.closure_measured);
	CHECK(cut.demanded_closure_pages == 1);
	CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) <= r.page_budget);
	CHECK(cut.first_cap_refused_node == 0);
	CHECK(cut.page_refused_groups > 0); // Page-specific observation, separate from the renderer's budget-frame counter.
	CHECK_FALSE(cut.budget_bound); // Page-only binds do not broaden the comparator's existing counter.
	CHECK_FALSE(cut.resident_refinement_bypass_over_budget);
	CHECK(demand_of(history).empty());
}

TEST_CASE("[GaussianSplatting][HLOD] page-feasible bootstrap retains a complete affordable frontier for ready and missing children (#1483)") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	data.nodes.write[3].topology.payload_count = 24;
	data.nodes.write[4].topology.payload_count = 24;
	data.prepare_placement_reserve();
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3();
	r.tau_pixels = 1e9; // Isolate forced bootstrap; no ordinary quality refinement follows it.
	r.splat_budget = 60;
	r.page_splats = 16;
	r.page_budget = 15 + 7; // Preserve a 15-page closure ceiling plus the 100-splat root's reserve.
	uint8_t resident[7] = { 1, 1, 1, 1, 1, 1, 1 };
	SUBCASE("ready children cannot push the bootstrap closure over the cap") {}
	SUBCASE("missing children cannot reserve an infeasible bootstrap transition") { resident[3] = resident[4] = 0; }
	r.resident = resident; r.resident_count = 7;
	// Root (100 splats) and {1,2} (80) fail the 60-splat cap. The base closure costs 7+3+3=13 pages.
	// Node 1's children need four more pages: 17 > 15. Node 2's children need only two: {1,5,6}
	// is a COMPLETE 48-splat frontier with a 15-page closure. Keeping root would violate the splat cap.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 5, 6 });
	CHECK(cut.selected_splats == 48);
	CHECK(cut.selected_splats <= r.splat_budget);
	CHECK(cut.estimated_records <= gs_hlod_runtime::kRecordAdmissionFactor * r.record_budget);
	CHECK(covers_without_overlap(history, cut.selected_count, 7));
	CHECK(cut.bootstrapped);
	CHECK(cut.budget_bound); // Bootstrap already binds the existing splat cap.
	CHECK(cut.first_cap_refused_node == 1);
	CHECK(cut.page_refused_groups > 0);
	CHECK(cut.demanded_closure_pages == 15);
	CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) == 15);
	CHECK_FALSE(cut.bootstrap_bypass_over_budget);
	CHECK(demand_of(history).empty());
}

TEST_CASE("[GaussianSplatting][HLOD] page-bound cuts stay deterministic inside the refinement hysteresis band (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.page_splats = 1024;
	r.page_budget = 7 + 1;
	const auto seeded = history.select(data, r);
	if (seeded.status != gs_hlod_runtime::CutHistory::COMPLETE || seeded.selected_count != 4) {
		FAIL("hysteresis precondition requires both interiors previously refined"); return;
	}
	r.page_budget = 5 + 1; // Root + interiors + ONE leaf pair + reserve; never enough for both pairs.
	r.focal_pixels = 6.25; // e(interior) == 1.25 tau: an already refined interior is held by history.
	const uint64_t bytes = history.allocated_bytes();
	for (int repeat = 0; repeat < 16; ++repeat) { // Pure synchronous selection, not an asynchronous frame wait.
		const auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 2, 3, 4 });
		CHECK(covers_without_overlap(history, cut.selected_count, 7));
		CHECK(cut.hysteresis_held_count == 1);
		CHECK(cut.demanded_closure_pages == 5);
		CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) == 5);
		CHECK(demand_of(history).empty());
	}
	CHECK(history.allocated_bytes() == bytes);
}

TEST_CASE("[GaussianSplatting][HLOD] a generous page budget preserves the legal unbounded cut and demand (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory unbounded, bounded;
	if (!unbounded.reserve(7) || !bounded.reserve(7)) { FAIL("cut workspaces unavailable"); return; }
	auto r = request();
	uint8_t resident[7] = { 1, 1, 1, 1, 1, 1, 1 };
	SUBCASE("all children resident") {}
	SUBCASE("both refinement groups missing") { resident[3] = resident[4] = resident[5] = resident[6] = 0; }
	r.resident = resident; r.resident_count = 7;
	const auto before = unbounded.select(data, r);
	if (before.status != gs_hlod_runtime::CutHistory::COMPLETE) { FAIL("legal reference cut must be complete"); return; }
	r.page_splats = 3;
	r.page_budget = 11 + 2; // Full 11-page closure plus the largest node's two-page reserve.
	const auto after = bounded.select(data, r);
	CHECK(after.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(bounded, after) == selected_of(unbounded, before));
	CHECK(after.selected_splats == before.selected_splats);
	CHECK(after.estimated_records == before.estimated_records);
	CHECK(after.budget_bound == before.budget_bound);
	CHECK(demand_of(bounded) == demand_of(unbounded));
	CHECK(covers_without_overlap(bounded, after.selected_count, 7));
	CHECK(after.demanded_closure_pages == 11);
	CHECK(binary_cut_and_demand_pages(data, bounded, after, r.page_splats) == 11);
	CHECK(after.page_refused_groups == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] resident refinement cannot spend page headroom promised to higher-priority demand (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t resident[7] = { 1, 1, 1, 0, 0, 1, 1 };
	r.resident = resident; r.resident_count = 7;
	r.page_splats = 1024;
	r.page_budget = 5 + 1;
	// Root+interiors cost 3. Higher-priority node 1 first promises 2 pages to missing {3,4}.
	// Resident {5,6} must then wait: accepting them would make cut+ancestors+promised children cost 7.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 2 });
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	CHECK(covers_without_overlap(history, cut.selected_count, 7));
	CHECK(cut.demanded_closure_pages == 5);
	CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) <= r.page_budget);
	CHECK(cut.first_cap_refused_node == 2);
	CHECK(cut.page_refused_groups > 0);
	CHECK_FALSE(cut.budget_bound);
}

TEST_CASE("[GaussianSplatting][HLOD] bootstrap demand reserves page headroom and is charged once when revisited (#1483)") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.splat_budget = 60;
	r.page_splats = 16;
	r.page_budget = 15 + 7; // 15-page closure ceiling plus the largest node's seven-page reserve.
	uint8_t resident[7] = { 1, 1, 1, 0, 0, 1, 1 };
	r.resident = resident; r.resident_count = 7;
	SUBCASE("pending bootstrap demand leaves no room for the other resident branch") {
		// Root+interiors cost 13; missing {3,4} reserve two. Resident {5,6} would need two more.
		const auto waiting = history.select(data, r);
		CHECK(waiting.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
		CHECK(waiting.selected_count == 0); // No frontier satisfies both the page and splat caps yet.
		CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
		CHECK(waiting.demanded_closure_pages == 15);
		CHECK(waiting.page_refused_groups == 1);
		std::vector<uint32_t> kept;
		for (uint32_t i = 0; i < history.partial_frontier_count(); ++i) { kept.push_back(history.partial_frontier_node(i)); }
		std::sort(kept.begin(), kept.end());
		CHECK(kept == std::vector<uint32_t>{ 1, 2 });
		resident[3] = resident[4] = 1;
		const auto ready = history.select(data, r);
		CHECK(ready.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(selected_of(history, ready) == std::vector<uint32_t>{ 2, 3, 4 });
		CHECK(covers_without_overlap(history, ready.selected_count, 7));
		CHECK(binary_cut_and_demand_pages(data, history, ready, r.page_splats) == 15);
		CHECK(ready.budget_bound); // The page-only refusal must not clear the earlier bootstrap splat bind.
	}
	SUBCASE("ordinary refinement revisits an already reserved bootstrap group") {
		r.page_budget = 17 + 7;
		const auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 5, 6 });
		CHECK(covers_without_overlap(history, cut.selected_count, 7));
		CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 3, 4 }); // Existing demand-list duplicate semantics.
		CHECK(cut.demanded_closure_pages == 17);
		CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) == 17);
		CHECK(cut.page_refused_groups == 0); // Revisited group needs zero additional pages, not another two.
	}
}

TEST_CASE("[GaussianSplatting][HLOD] an impossible root page closure is explicit and emits no unfulfillable demand (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.page_splats = 1;
	r.page_budget = 1; // Root alone needs two pages, retained in every possible frontier's closure.
	uint8_t resident[7] = { 1, 1, 1, 1, 1, 1, 1 };
	SUBCASE("resident root") {}
	SUBCASE("missing root") { resident[0] = 0; }
	r.resident = resident; r.resident_count = 7;
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(cut.selected_count == 0);
	CHECK(cut.first_cap_refused_node == 0);
	CHECK(cut.page_refused_groups == 1);
	CHECK_FALSE(cut.budget_bound);
	CHECK(demand_of(history).empty());
}

TEST_CASE("[GaussianSplatting][HLOD] pressure closure counts ancestors and complete transition overlap without changing demand") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t resident[7] = { 1, 1, 1, 0, 0, 0, 0 };
	r.resident = resident; r.resident_count = 7;
	r.page_splats = 3;
	r.page_budget = 7 + 2; // Seven-page closure ceiling plus the two-page reserve.
	const uint64_t bytes = history.allocated_bytes();
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.closure_measured);
	// Root 1 + interiors 1+1 + demanded leaf pair 2+2 = 7, including the retained parent.
	CHECK(cut.demanded_closure_pages == 7);
	CHECK(cut.page_budget == 9);
	CHECK(cut.resident_refinement_bypass_pages == 2);
	CHECK_FALSE(cut.resident_refinement_bypass_over_budget);
	CHECK(cut.bootstrap_bypass_pages == 0);
	CHECK(cut.page_refused_groups == 1);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	CHECK(cut.selected_count == 2);
	HlodCutTelemetry telemetry;
	telemetry.capture(history, cut, 7, nullptr, 0);
	Dictionary first;
	telemetry.append_stats(first, true);
	CHECK(bool(first["hlod_stream_closure_measured"]));
	CHECK(int64_t(first["hlod_stream_demanded_closure_pages"]) == 7);
	CHECK(int64_t(first["hlod_stream_page_refused_groups_total"]) == 1);
	r.page_budget = 11 + 2;
	const auto both = history.select(data, r);
	CHECK(both.demanded_closure_pages == 11);
	CHECK(both.page_refused_groups == 0);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4, 5, 6 });
	telemetry.capture(history, both, 7, nullptr, 0);
	Dictionary second;
	telemetry.append_stats(second, true);
	CHECK(int64_t(second["hlod_stream_demanded_closure_pages"]) == 11);
	CHECK(int64_t(second["hlod_stream_page_refused_groups_total"]) == 1);
	CHECK(int64_t(first["hlod_stream_demanded_closure_pages"]) == 7); // Retained snapshot is immutable.
	CHECK(history.allocated_bytes() == bytes);

	// #1483: resident children obey the same page cap; only the root-to-interiors transition fits.
	for (uint8_t &value : resident) { value = 1; }
	r.page_budget = 3 + 2;
	const auto bypass = history.select(data, r);
	CHECK(selected_of(history, bypass) == std::vector<uint32_t>{ 1, 2 });
	CHECK(history.demand_count() == 0);
	CHECK(bypass.demanded_closure_pages == 3);
	CHECK(bypass.resident_refinement_bypass_pages == 2);
	CHECK_FALSE(bypass.resident_refinement_bypass_over_budget);
	CHECK(bypass.page_refused_groups > 0);
}

TEST_CASE("[GaussianSplatting][HLOD] pressure bootstrap charges resident siblings and missing children at the page cap") {
	GaussianSplatHlodRuntimeData data;
	heavy_root_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.camera = Vector3();
	r.tau_pixels = 1e9;
	r.splat_budget = 90;
	r.page_splats = 16;
	r.page_budget = 13 + 7; // Root (7) and both interiors (3+3), plus the seven-page reserve.
	uint8_t resident[7] = { 1, 1, 0, 1, 1, 1, 1 };
	r.resident = resident; r.resident_count = 7;
	const auto missing = history.select(data, r);
	CHECK(missing.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 2 });
	// ceil(100/16) + ceil(40/16) + ceil(40/16) = 7+3+3, despite only node 2 being missing.
	CHECK(missing.demanded_closure_pages == 13);
	CHECK(missing.bootstrap_bypass_pages == 6);
	CHECK_FALSE(missing.bootstrap_bypass_over_budget);
	CHECK(missing.resident_refinement_bypass_pages == 0);
	resident[2] = 1;
	const auto ready = history.select(data, r);
	CHECK(ready.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(ready.selected_count == 2);
	CHECK(ready.demanded_closure_pages == 13);
	CHECK(ready.bootstrap_bypass_pages == 6);
	CHECK_FALSE(ready.bootstrap_bypass_over_budget);
	CHECK(history.demand_count() == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] pressure page telemetry distinguishes missing measurement from measured zero") {
	GaussianSplatHlodRuntimeData data;
	tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	HlodCutTelemetry telemetry;
	telemetry.capture(history, history.select(data, r), 7, nullptr, 0);
	Dictionary stats;
	telemetry.append_stats(stats, true);
	CHECK_FALSE(bool(stats["hlod_stream_closure_measured"]));
	CHECK(stats["hlod_stream_demanded_closure_pages"].get_type() == Variant::NIL);
	CHECK(stats["hlod_stream_page_refused_groups_total"].get_type() == Variant::NIL);
	r.page_splats = 1024; r.page_budget = 7 + 1;
	const auto measured = history.select(data, r);
	telemetry.capture(history, measured, 7, nullptr, 0);
	telemetry.append_stats(stats, true);
	CHECK(bool(stats["hlod_stream_closure_measured"]));
	CHECK(int64_t(stats["hlod_stream_page_refused_groups"]) == 0);
	telemetry.valid = false;
	telemetry.append_stats(stats, true);
	CHECK_FALSE(bool(stats["hlod_stream_closure_measured"]));
	CHECK(stats["hlod_stream_demanded_closure_pages"].get_type() == Variant::NIL);
}

// #1472: the record budget is the allocated overlap storage. A storage below the view's demand does not overflow, it
// holds the cut coarse, so the selector names the step the record cap ALONE refused and the route can grow the storage.
TEST_CASE("[GaussianSplatting][HLOD] runtime cut reports the record demand only the record cap refused (#1472)") {
	GaussianSplatHlodRuntimeData data;
	tree(data); // No footprint: a cut's record estimate is its splat count (2, 4, 10, 16 along the refinement).
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	auto cut = history.select(data, r);
	CHECK(cut.selected_splats == 16);
	CHECK_FALSE(cut.budget_bound);
	CHECK(cut.record_demand == 0.0); // Nothing was refused.
	// 80 percent of 12.5 admits 10 records: the cut {3, 4, 2}. Refining node 2 (16 records, 16 splats) is refused by
	// the record cap alone.
	history.invalidate();
	r.record_budget = 12.5;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_splats == 10);
	CHECK(cut.budget_bound);
	CHECK(cut.record_demand == doctest::Approx(16.0));
	// The splat cap refuses the same step: more records would not admit it, so no record demand is reported.
	history.invalidate();
	r.record_budget = 100000;
	r.splat_budget = 10;
	cut = history.select(data, r);
	CHECK(cut.selected_splats == 10);
	CHECK(cut.budget_bound);
	CHECK(cut.record_demand == 0.0);
	r.record_budget = 12.5; // Both caps refuse it: still nothing more records alone would admit.
	history.invalidate();
	cut = history.select(data, r);
	CHECK(cut.selected_splats == 10);
	CHECK(cut.record_demand == 0.0);
	// A streamed world: only the interiors are resident. The cut {1, 2} cannot move, and the record cap keeps the second
	// child group from even being requested, which never shows as an overflow or as a refused refinement.
	history.invalidate();
	r.splat_budget = 100;
	uint8_t interiors[7] = { 1, 1, 1, 0, 0, 0, 0 };
	r.resident = interiors; r.resident_count = 7;
	cut = history.select(data, r);
	CHECK(cut.selected_count == 2);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	CHECK(cut.record_demand == doctest::Approx(16.0));
	// The page budget refuses that group instead: no record demand.
	history.invalidate();
	r.record_budget = 100000;
	r.page_splats = 1024;
	r.page_budget = 5 + 1; // Page admission ceiling remains five after reserving one page.
	cut = history.select(data, r);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 3, 4 });
	CHECK(cut.record_demand == 0.0);
	// A root that needs the bootstrap for its records alone reports its own estimate, also when no frontier fits.
	history.invalidate();
	r.resident = nullptr; r.resident_count = 0;
	r.page_splats = 0; r.page_budget = 0;
	r.record_budget = 2; // 80 percent is below the root's two records.
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(cut.record_demand == doctest::Approx(2.0));
	// A root over the splat cap, and below it frontiers whose splats and nodes fit but whose records do not ({1, 2} = 4,
	// then 10, then 16 against a cap of 3.2): no frontier fits, and only records are missing. The demand is the cheapest
	// such frontier, or a streamed world with a small storage would stay blank with nothing asking for growth.
	history.invalidate();
	data.nodes.write[0].topology.payload_count = 100;
	r.record_budget = 4;
	r.splat_budget = 16;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(cut.record_demand == doctest::Approx(4.0));
	// With the storage that demand asks for (80 percent of 6.5 admits 4, then stops at 10) the same view draws.
	history.invalidate();
	r.record_budget = 6.5;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.selected_splats == 4);
	CHECK(cut.record_demand == doctest::Approx(10.0)); // Now the ordinary refinement names the next step.
	data.nodes.write[0].topology.payload_count = 2;
	// A root over the splat cap is not a record demand.
	history.invalidate();
	r.record_budget = 100000;
	r.splat_budget = 1;
	cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(cut.record_demand == 0.0);
}

TEST_CASE("[GaussianSplatting][HLOD] whole-tree atlas capacity admits the main-view wall closure without placement headroom (#1483)") {
	GaussianSplatHlodRuntimeData data;
	data.nodes.resize(3); // Main-view wall: one coarse payload and two leaves, no payload allocation here.
	const uint32_t payloads[3] = { 256, 12800, 12800 };
	for (uint32_t i = 0; i < 3; ++i) {
		data.nodes.write[i].bounds_center = Vector3(0, 0, -10);
		data.nodes.write[i].topology.payload_count = payloads[i];
	}
	data.nodes.write[0].topology.first_child = 1;
	data.nodes.write[0].topology.child_count = 2;
	data.nodes.write[0].topology.geometric_error = 4.0f;
	data.prepare_placement_reserve();
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(3)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.splat_budget = 40000;
	r.node_budget = 3;
	r.page_splats = 1024;
	uint8_t resident[3] = { 1, 1, 1 };
	SUBCASE("ready leaves") {}
	SUBCASE("missing leaves") { resident[1] = resident[2] = 0; }
	r.resident = resident; r.resident_count = 3;
	r.page_budget = 27; // 1 + 13 + 13: the atlas can retain every node, so no placement reserve is needed.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_MEASURED);
	CHECK(cut.placement_reserve_measured);
	CHECK(cut.placement_reserve_pages == 0);
	CHECK(cut.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
	CHECK(cut.demanded_closure_pages == 27);
	CHECK(cut.page_budget == 27);
	CHECK(cut.page_refused_groups == 0);
	CHECK_FALSE(cut.budget_bound);
	if (resident[1]) {
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 2 });
		CHECK(cut.selected_splats == 25600);
		CHECK(demand_of(history).empty());
	} else {
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 0 });
		CHECK(demand_of(history) == std::vector<uint32_t>{ 1, 2 });
	}
	CHECK(covers_without_overlap(history, cut.selected_count, 3));
	r.page_budget = 26; // One physical page short: the exception must stop here.
	const auto refused = history.select(data, r);
	CHECK(refused.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(refused.placement_reserve_pages == 13);
	CHECK(selected_of(history, refused) == std::vector<uint32_t>{ 0 });
	CHECK(demand_of(history).empty());
	CHECK(refused.page_refused_groups == 1);
	r.page_splats = 512;
	r.page_budget = 51; // 1 + 25 + 25, but the cached total was prepared for 1024/page.
	const auto different_geometry = history.select(data, r);
	CHECK(different_geometry.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(different_geometry.placement_reserve_pages == 25);
	CHECK(selected_of(history, different_geometry) == std::vector<uint32_t>{ 0 });
	CHECK(demand_of(history).empty());
	data.prepare_placement_reserve(512);
	CHECK(data.max_node_payload_count == 12800);
	CHECK(data.placement_page_splats == 512);
	CHECK(data.placement_node_pages == 51);
	const auto prepared_geometry = history.select(data, r);
	CHECK(prepared_geometry.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(prepared_geometry.placement_reserve_pages == 0);
	CHECK(prepared_geometry.demanded_closure_pages == 51);
	CHECK(prepared_geometry.page_refused_groups == 0);
	CHECK(selected_of(history, prepared_geometry) == (resident[1] ? std::vector<uint32_t>{ 1, 2 } : std::vector<uint32_t>{ 0 }));
	CHECK(demand_of(history) == (resident[1] ? std::vector<uint32_t>{} : std::vector<uint32_t>{ 1, 2 }));
}

// Placement reserve fixtures allocate no payload. A node, rather than a sibling group, is one contiguous run.
// 500/1500/2500 splats at 1024/page cost 1/2/3 pages: transition closure 6, reserve ceil(2500/1024) = 3.
// Four unrefined grandchildren keep the whole tree at 14 pages, above the budgets testing reserve pressure.
static void placement_reserve_tree(GaussianSplatHlodRuntimeData &r_data) {
	tree(r_data);
	const uint32_t payloads[7] = { 500, 1500, 2500, 2000, 2000, 2000, 2000 };
	for (uint32_t i = 0; i < 7; ++i) {
		auto &n = r_data.nodes.write[i];
		n.topology.payload_count = payloads[i];
		n.topology.geometric_error = i == 0 ? 4.0f : 0.0f;
	}
	r_data.prepare_placement_reserve();
}
static gs_hlod_runtime::CutRequest placement_reserve_request() {
	auto r = request();
	r.splat_budget = 10000;
	r.node_budget = 3;
	r.page_splats = 1024;
	return r;
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve refuses a physically exact-fit transition and keeps its complete parent (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.page_budget = 6; // Closure fits the physical atlas exactly, but exceeds the admission ceiling of 3.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 0 });
	CHECK(cut.selected_splats == 500);
	CHECK(cut.placement_reason == gs_hlod_runtime::CutHistory::RESERVE_ROOT_ONLY);
	CHECK(cut.placement_reserve_measured);
	CHECK(cut.placement_reserve_pages == 3);
	CHECK(covers_without_overlap(history, cut.selected_count, 7));
	CHECK(cut.demanded_closure_pages == 1);
	CHECK(cut.page_budget == 6); // Reserve is headroom; it does not rewrite the physical budget.
	CHECK(cut.page_refused_groups == 1);
	CHECK(cut.first_cap_refused_node == 0);
	CHECK_FALSE(cut.budget_bound);
	CHECK(demand_of(history).empty());
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve equals the rounded largest node run rather than the sibling sum (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	uint64_t first_admitted_budget = 0;
	for (uint64_t pages = 6; pages <= 12; ++pages) {
		history.invalidate();
		r.page_budget = pages;
		const auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		if (selected_of(history, cut) == std::vector<uint32_t>{ 1, 2 }) {
			first_admitted_budget = pages;
			break;
		}
	}
	if (first_admitted_budget < 6) { FAIL("no complete transition admitted in the legal budget range"); return; }
	// Infer the actual headroom from existing public selection results, so this red test needs no new API.
	CHECK(first_admitted_budget == 9);
	CHECK(first_admitted_budget - 6 == 3); // M=2500; ceil(M/1024)=3, not child-group pages 2+3=5.
	CHECK(data.placement_reserve_prepared);
	CHECK(data.max_node_payload_count == 2500);
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve preserves the cut and demand with a generous page budget (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	gs_hlod_runtime::CutHistory bounded, unbounded;
	if (!bounded.reserve(7) || !unbounded.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	uint8_t resident[7] = { 1, 1, 1 };
	SUBCASE("ready children") {}
	SUBCASE("missing children") { resident[1] = resident[2] = 0; }
	r.resident = resident; r.resident_count = 7;
	r.page_splats = 0;
	const auto before = unbounded.select(data, r);
	r.page_splats = 1024;
	r.page_budget = 64;
	const auto after = bounded.select(data, r);
	CHECK(before.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(after.status == before.status);
	CHECK(selected_of(bounded, after) == selected_of(unbounded, before));
	CHECK(demand_of(bounded) == demand_of(unbounded));
	CHECK(after.selected_splats == before.selected_splats);
	CHECK(after.estimated_records == before.estimated_records);
	CHECK(after.budget_bound == before.budget_bound);
	CHECK(after.demanded_closure_pages == 6);
	CHECK(after.page_refused_groups == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve boundary is deterministic and preserves admissible hysteresis (#1483)") {
	GaussianSplatHlodRuntimeData data;
	tree(data); // All seven nodes cost one page; reserve is one page.
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.page_splats = 1024;
	r.page_budget = 8; // Seven-page full closure plus the reserve.
	const auto seeded = history.select(data, r);
	if (seeded.status != gs_hlod_runtime::CutHistory::COMPLETE || seeded.selected_count != 4) {
		FAIL("hysteresis precondition requires both interiors previously refined"); return;
	}
	bool hold_one = false;
	SUBCASE("one page below the reserved transition boundary") { r.page_budget = 5; }
	SUBCASE("exact reserved transition boundary holds one historical refinement") { r.page_budget = 6; hold_one = true; }
	r.focal_pixels = 6.25; // Interior error == 1.25 tau: only history permits refinement here.
	const std::vector<uint32_t> expected = hold_one ? std::vector<uint32_t>{ 2, 3, 4 } : std::vector<uint32_t>{ 1, 2 };
	const uint64_t bytes = history.allocated_bytes();
	for (int repeat = 0; repeat < 8; ++repeat) { // Synchronous selection, never an async frame wait.
		const auto cut = history.select(data, r);
		CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(selected_of(history, cut) == expected);
		CHECK(cut.hysteresis_held_count == (hold_one ? 1u : 0u));
		CHECK(covers_without_overlap(history, cut.selected_count, 7));
		CHECK(binary_cut_and_demand_pages(data, history, cut, r.page_splats) <= r.page_budget - 1);
		CHECK_FALSE(cut.budget_bound);
		CHECK(demand_of(history).empty());
	}
	CHECK(history.allocated_bytes() == bytes);
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve larger than the atlas retains a physically admissible complete root (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data); // Reserve 3 exceeds physical budget 2; the mandatory root needs only 1.
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.page_budget = 2;
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 0 });
	CHECK(covers_without_overlap(history, cut.selected_count, 7));
	CHECK(cut.placement_reason == gs_hlod_runtime::CutHistory::RESERVE_ROOT_ONLY);
	CHECK(cut.demanded_closure_pages == 1);
	CHECK(cut.page_refused_groups > 0);
	CHECK_FALSE(cut.budget_bound);
	CHECK(demand_of(history).empty());
	// The mandatory root must still obey physical capacity and the existing selection caps.
	r.page_budget = 0;
	const auto no_space = history.select(data, r);
	CHECK(no_space.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(no_space.selected_count == 0);
	CHECK(no_space.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
	r.page_budget = 2;
	r.splat_budget = 499;
	const auto over_splats = history.select(data, r);
	CHECK(over_splats.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(over_splats.selected_count == 0);
	r.splat_budget = 10000;
	uint8_t absent[7] = { 0, 0, 0 };
	r.resident = absent; r.resident_count = 7;
	const auto loading = history.select(data, r);
	CHECK(loading.status == gs_hlod_runtime::CutHistory::UNAVAILABLE);
	CHECK(loading.selected_count == 0);
	CHECK(demand_of(history) == std::vector<uint32_t>{ 0 }); // Mandatory root demand uses physical capacity too.
	CHECK(loading.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve admits ready and demanded transitions at the same ceiling (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	uint8_t resident[7] = { 1, 1, 1 };
	SUBCASE("ready group") {}
	SUBCASE("one missing sibling") { resident[2] = 0; }
	SUBCASE("both missing siblings") { resident[1] = resident[2] = 0; }
	r.resident = resident; r.resident_count = 7;
	r.page_budget = 8; // Six-page transition exceeds L=5 even though physical capacity is sufficient.
	const auto refused = history.select(data, r);
	CHECK(refused.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, refused) == std::vector<uint32_t>{ 0 });
	CHECK(demand_of(history).empty());
	CHECK(refused.demanded_closure_pages == 1);
	CHECK(refused.page_refused_groups == 1);
	CHECK_FALSE(refused.budget_bound);
	r.page_budget = 9; // Six-page transition exactly meets L=6.
	const auto admitted = history.select(data, r);
	CHECK(admitted.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(admitted.demanded_closure_pages == 6);
	CHECK(admitted.page_refused_groups == 0);
	if (resident[1] && resident[2]) {
		CHECK(selected_of(history, admitted) == std::vector<uint32_t>{ 1, 2 });
		CHECK(demand_of(history).empty());
	} else {
		CHECK(selected_of(history, admitted) == std::vector<uint32_t>{ 0 });
		const std::vector<uint32_t> expected = resident[1] ? std::vector<uint32_t>{ 2 } : std::vector<uint32_t>{ 1, 2 };
		CHECK(demand_of(history) == expected);
	}
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve applies equally to ready and missing bootstrap expansions (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	data.nodes.write[0].topology.payload_count = 2500;
	data.nodes.write[1].topology.payload_count = 500;
	data.nodes.write[2].topology.payload_count = 1500;
	data.prepare_placement_reserve();
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.splat_budget = 2000; // Root fails the splat cap; its complete child frontier fits it exactly.
	r.tau_pixels = 1e9; // Isolate forced bootstrap from ordinary quality refinement.
	uint8_t resident[7] = { 1, 1, 1 };
	SUBCASE("ready bootstrap children") {}
	SUBCASE("missing bootstrap children") { resident[1] = resident[2] = 0; }
	r.resident = resident; r.resident_count = 7;
	r.page_budget = 6; // Closure 3+1+2=6, reserve 3: transition must be refused.
	const auto refused = history.select(data, r);
	CHECK(refused.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_INFEASIBLE);
	CHECK(refused.selected_count == 0); // Never retain a root that violates an existing hard cap.
	CHECK(demand_of(history).empty());
	CHECK(refused.page_refused_groups > 0);
	CHECK(refused.budget_bound);
	r.page_budget = 9;
	const auto admitted = history.select(data, r);
	if (resident[1] && resident[2]) {
		CHECK(admitted.status == gs_hlod_runtime::CutHistory::COMPLETE);
		CHECK(selected_of(history, admitted) == std::vector<uint32_t>{ 1, 2 });
		CHECK(admitted.selected_splats == 2000);
		CHECK(covers_without_overlap(history, admitted.selected_count, 7));
		CHECK(demand_of(history).empty());
	} else {
		CHECK(admitted.status == gs_hlod_runtime::CutHistory::BOOTSTRAP_MISSING_RESIDENCY);
		CHECK(admitted.selected_count == 0);
		CHECK(demand_of(history) == std::vector<uint32_t>{ 1, 2 });
	}
	CHECK(admitted.demanded_closure_pages == 6);
}

TEST_CASE("[GaussianSplatting][HLOD] direct cut telemetry preserves the streamer's placement reserve observation (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.page_budget = 64;
	const auto cut = history.select(data, r);
	HlodCutTelemetry telemetry;
	telemetry.capture(history, cut, 7, nullptr, 0);
	Dictionary stats;
	const String key = "hlod_stream_placement_reserve_pages";
	telemetry.append_stats(stats, false);
	CHECK_FALSE(stats.has(key)); // Only streamer render-stats export may own this external key.
	for (const Variant &observation : { Variant(), Variant(int64_t(0)), Variant(int64_t(3)) }) {
		stats[key] = observation;
		telemetry.append_stats(stats, false);
		CHECK(stats[key] == observation);
		HlodCutTelemetry absent;
		absent.append_stats(stats, false);
		CHECK(stats[key] == observation); // An absent direct cut must not erase a streamer observation either.
	}
}

TEST_CASE("[GaussianSplatting][HLOD] placement reserve cache excludes stale nodes and unprepared snapshots still draw (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	data.nodes.write[0].topology.payload_count = 5000; // Larger than every live node, but stale after an edit.
	data.edit_revision = 1;
	data.edit_state.resize(7);
	data.edit_state.fill(GaussianSplatHlodRuntimeData::EDIT_CLEAN);
	data.edit_state.write[0] = GaussianSplatHlodRuntimeData::EDIT_STALE;
	data.prepare_placement_reserve();
	CHECK(data.max_node_payload_count == 2500);
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.page_budget = 8; // Live closure 2+3, plus three pages of reserve; no stale-root pages.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 2 });
	CHECK(cut.placement_reserve_measured);
	CHECK(cut.placement_reserve_pages == 3);
	CHECK(cut.demanded_closure_pages == 5);
	CHECK(cut.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
	data.placement_reserve_prepared = false;
	const auto unprepared = history.select(data, r);
	CHECK(unprepared.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, unprepared) == std::vector<uint32_t>{ 1, 2 });
	CHECK(unprepared.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED);
	CHECK_FALSE(unprepared.placement_reserve_measured);
	r.page_splats = 0;
	const auto disabled = history.select(data, r);
	CHECK(disabled.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK_FALSE(disabled.placement_reserve_measured);
	CHECK(disabled.placement_reserve_pages == 0);
	CHECK(disabled.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
}

TEST_CASE("[GaussianSplatting][HLOD] unprepared reserve preserves option-1 cut demand and physical page admission (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	data.placement_reserve_prepared = false;
	data.max_node_payload_count = UINT32_MAX; // An unprepared cache must never influence admission.
	gs_hlod_runtime::CutHistory history, reference;
	if (!history.reserve(7) || !reference.reserve(7)) { FAIL("cut workspaces unavailable"); return; }
	auto r = placement_reserve_request();
	uint8_t resident[7] = { 1, 1, 1 };
	SUBCASE("ready children") {}
	SUBCASE("missing children") { resident[1] = resident[2] = 0; }
	r.resident = resident; r.resident_count = 7;
	r.page_budget = 6; // Option 1 admits this exact physical fit: root 1 + children 2+3 pages.
	const auto cut = history.select(data, r);
	CHECK(cut.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(cut.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED);
	CHECK_FALSE(cut.placement_reserve_measured);
	CHECK(cut.placement_reserve_pages == 0);
	CHECK(cut.page_budget == 6);
	CHECK(cut.demanded_closure_pages == 6);
	CHECK(cut.page_refused_groups == 0);
	CHECK_FALSE(cut.budget_bound);
	CHECK(cut.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
	if (resident[1]) {
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 1, 2 });
		CHECK(demand_of(history).empty());
	} else {
		CHECK(selected_of(history, cut) == std::vector<uint32_t>{ 0 });
		CHECK(demand_of(history) == std::vector<uint32_t>{ 1, 2 });
	}
	r.page_splats = 0; // The physical-fit option-1 frontier also equals the page-disabled reference here.
	const auto expected = reference.select(data, r);
	CHECK(selected_of(history, cut) == selected_of(reference, expected));
	CHECK(demand_of(history) == demand_of(reference));
	r.page_splats = 1024;
	r.page_budget = 5; // Fallback must still enforce pages; it is not disabled page admission.
	const auto refused = history.select(data, r);
	CHECK(refused.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(selected_of(history, refused) == std::vector<uint32_t>{ 0 });
	CHECK(demand_of(history).empty());
	CHECK(refused.demanded_closure_pages == 1);
	CHECK(refused.page_refused_groups == 1);
	CHECK(refused.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED);
	CHECK(refused.placement_reason == gs_hlod_runtime::CutHistory::PLACEMENT_NONE);
}

TEST_CASE("[GaussianSplatting][HLOD] prepared zero placement reserve is measured and distinct from unprepared and absent (#1483)") {
	GaussianSplatHlodRuntimeData data;
	placement_reserve_tree(data);
	// Zero-payload synthetic nodes exercise the zero encoding without claiming a valid on-disk world.
	for (int64_t i = 0; i < data.nodes.size(); ++i) { data.nodes.write[i].topology.payload_count = 0; }
	data.prepare_placement_reserve();
	gs_hlod_runtime::CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = placement_reserve_request();
	r.page_budget = 0;
	const auto measured = history.select(data, r);
	CHECK(measured.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(measured.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_MEASURED);
	CHECK(measured.placement_reserve_measured);
	CHECK(measured.placement_reserve_pages == 0);
	CHECK(selected_of(history, measured) == std::vector<uint32_t>{ 1, 2 });
	data.placement_reserve_prepared = false;
	const auto unprepared = history.select(data, r);
	CHECK(unprepared.status == gs_hlod_runtime::CutHistory::COMPLETE);
	CHECK(unprepared.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_UNPREPARED);
	CHECK_FALSE(unprepared.placement_reserve_measured);
	CHECK(unprepared.placement_reserve_pages == 0);
	CHECK(selected_of(history, unprepared) == std::vector<uint32_t>{ 1, 2 });
	r.page_splats = 0;
	const auto absent = history.select(data, r);
	CHECK(absent.placement_reserve_state == gs_hlod_runtime::CutHistory::RESERVE_ABSENT);
	CHECK_FALSE(absent.placement_reserve_measured);
}

TEST_CASE("[GaussianSplatting][HLOD] transition descriptors retain visible resident siblings and exclude invisible children (#1483 B)") {
	using namespace gs_hlod_runtime;
	GaussianSplatHlodRuntimeData data;
	data.nodes.resize(4);
	for (uint32_t i = 0; i < 4; ++i) {
		auto &node = data.nodes.write[i];
		node.bounds_center = Vector3(i == 3 ? 100 : 0, 0, -10);
		node.topology.payload_count = 1;
		if (i != 0) { node.topology.parent = 0; }
	}
	data.nodes.write[0].topology.first_child = 1;
	data.nodes.write[0].topology.child_count = 3;
	data.nodes.write[0].topology.geometric_error = 4;
	CutHistory history;
	if (!history.reserve(4)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	r.node_budget = 4;
	r.frustum_enabled = true;
	for (Plane &plane : r.frustum) { plane = Plane(Vector3(1, 0, 0), 10); }
	uint8_t resident[4] = { 1, 1, 0, 1 };
	r.resident = resident;
	r.resident_count = 4;
	TransitionKind expected = TRANSITION_REFINEMENT;
	SUBCASE("ordinary refinement") {}
	SUBCASE("forced bootstrap captures visibility before skipping missing children") {
		data.nodes.write[0].topology.payload_count = 100;
		r.splat_budget = 2;
		expected = TRANSITION_BOOTSTRAP;
	}
	const auto cut = history.select(data, r);
	CHECK(cut.status == (expected == TRANSITION_BOOTSTRAP ? CutHistory::BOOTSTRAP_MISSING_RESIDENCY : CutHistory::COMPLETE));
	if (history.demand_count() != 1) { FAIL("expected one missing child, got ", history.demand_count()); return; }
	CHECK(history.demand_node(0) == 2);
	const auto &group = history.demand_transition(0);
	CHECK(group.parent == 0);
	CHECK(group.first_child == 1);
	CHECK(group.visible_mask == 3); // Child 1 is resident, child 2 is missing, child 3 is invisible.
	CHECK(group.kind == expected);
	CHECK(group.ordinal == 0);
}

TEST_CASE("[GaussianSplatting][HLOD] transition kinds and emission ordinals preserve root priority and duplicate ordering (#1483 B)") {
	using namespace gs_hlod_runtime;
	GaussianSplatHlodRuntimeData data;
	tree(data);
	CutHistory history;
	if (!history.reserve(7)) { FAIL("cut workspace unavailable"); return; }
	auto r = request();
	uint8_t resident[7] = { 0, 0, 0, 0, 0, 0, 0 };
	r.resident = resident;
	r.resident_count = 7;
	CHECK(history.select(data, r).status == CutHistory::UNAVAILABLE);
	if (history.demand_count() != 1) { FAIL("root demand missing"); return; }
	CHECK(history.demand_node(0) == 0);
	CHECK(history.demand_transition(0).kind == TRANSITION_ROOT);
	CHECK(history.demand_transition(0).parent == UINT32_MAX);
	CHECK(history.demand_transition(0).ordinal == 0);
	resident[0] = resident[1] = resident[2] = 1;
	CHECK(history.select(data, r).status == CutHistory::COMPLETE);
	if (history.demand_count() != 4) { FAIL("expected two complete leaf demands"); return; }
	for (uint32_t i = 0; i < 4; ++i) {
		CHECK(history.demand_node(i) == i + 3);
		CHECK(history.demand_transition(i).kind == TRANSITION_REFINEMENT);
		CHECK(history.demand_transition(i).parent == i / 2 + 1);
		CHECK(history.demand_transition(i).ordinal == (i / 2) * 2);
		CHECK(history.demand_transition(i).visible_mask == 3);
	}
	heavy_root_tree(data);
	history.invalidate();
	resident[5] = resident[6] = 1;
	r.splat_budget = 60;
	r.page_splats = 16;
	r.page_budget = 24;
	CHECK(history.select(data, r).status == CutHistory::COMPLETE);
	if (history.demand_count() != 4) { FAIL("expected bootstrap demand revisited by refinement"); return; }
	for (uint32_t i = 0; i < 4; ++i) {
		CHECK(history.demand_node(i) == 3 + i % 2);
		CHECK(history.demand_transition(i).kind == (i < 2 ? TRANSITION_BOOTSTRAP : TRANSITION_REFINEMENT));
		CHECK(history.demand_transition(i).ordinal == (i < 2 ? 0 : 2));
		CHECK(history.demand_transition(i).parent == 1);
	}
}
}
