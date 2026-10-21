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
	CHECK(dropped.status == gs_hlod_runtime::CutHistory::ROOT_DROPPED);
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
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::ROOT_DROPPED);
	r.record_budget = 2.5; // Exactly one 80% admission factor.
	r.splat_budget = 2;
	CHECK(history.select(data, r).status == gs_hlod_runtime::CutHistory::COMPLETE);
	uint64_t bytes = 99;
	CHECK_FALSE(gs_hlod_runtime::allocation_bytes(UINT64_MAX, 8, bytes));
	CHECK_FALSE(gs_hlod_runtime::allocation_bytes(8, 8, bytes, 64));
	CHECK(gs_hlod_runtime::allocation_bytes(8, 8, bytes, 80));
	CHECK(bytes == 80);
}
}
