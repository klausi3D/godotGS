extern "C" int test_hlod_runtime_publication_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gs_vector_alloc.h"
#include "../lod/hlod_runtime_selection.h"
#include <cmath>
#include "../nodes/gaussian_splat_world_3d.h"
#include <limits>

namespace TestHlodRuntimePublication {
TEST_CASE("[GaussianSplatting][WorldIO][HLOD] detached runtime snapshot preserves leaves and rejects stale or failed publication") {
	Ref<GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(4); // Fixed exact dyadic positions and scales.
	for (uint32_t i = 0; i < 4; ++i) {
		gaussians[i].position = Vector3(16.0f + i * 0.125f, 0.125f, 0.125f);
		gaussians[i].scale = Vector3(0.03125f, 0.0625f, 0.125f);
		gaussians[i].opacity = 0.5f;
	}
	LocalVector<Vector3> sh;
	data->set_gaussian_payload(gaussians, sh, 0u, 0u, false, false);
	Ref<GaussianSplatWorld> world;
	world.instantiate(); world->set_gaussian_data(data);
	if (world->bake_hlod() != OK) { FAIL("synthetic world bake failed"); return; }
	Ref<GaussianSplatHlodRuntimeData> snapshot;
	String reason;
	if (world->capture_hlod_runtime_snapshot(snapshot, reason) != OK || snapshot.is_null()) {
		FAIL("snapshot failed: ", reason.utf8().get_data()); return;
	}
	CHECK(snapshot->is_current());
	CHECK(snapshot->leaf_count == 4);
	CHECK(snapshot->owned_cpu_bytes > 0);
	if (snapshot->leaf_count != 4 || snapshot->nodes.is_empty()) { return; }
	for (uint32_t i = 0; i < 4; ++i) {
		CHECK(snapshot->gaussian(i).position == world->get_gaussian_data()->get_gaussian(i).position);
	}
	CHECK(snapshot->nodes[0].footprint == doctest::Approx(4.0 * 36.0 * 0.0625 * 0.125));
	Ref<GaussianSplatHlodRuntimeData> preserved = snapshot;
	gs_vector_alloc_force_failure_at("HLOD runtime nodes");
	ERR_PRINT_OFF;
	const Error failed = world->capture_hlod_runtime_snapshot(snapshot, reason);
	ERR_PRINT_ON;
	CHECK_FALSE(gs_vector_alloc_forced_failure_is_armed());
	gs_vector_alloc_clear_forced_failure();
	CHECK(failed == ERR_OUT_OF_MEMORY);
	CHECK(snapshot == preserved);
	CHECK(preserved->is_current());
	Ref<GaussianData> replacement;
	replacement.instantiate();
	world->set_gaussian_data(replacement);
	CHECK_FALSE(preserved->is_current());
	CHECK(preserved->leaf_count == 4);
	CHECK(preserved->gaussian(0).position == gaussians[0].position);
}
TEST_CASE("[GaussianSplatting][HLOD] pixel threshold rejects invalid updates atomically") {
	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	CHECK(node->get_hlod_tau_px() == 0.0f);
	node->set_hlod_tau_px(0.5f);
	ERR_PRINT_OFF;
	node->set_hlod_tau_px(-1.0f);
	node->set_hlod_tau_px(std::numeric_limits<float>::quiet_NaN());
	ERR_PRINT_ON;
	CHECK(node->get_hlod_tau_px() == 0.5f);
	node->set_hlod_tau_px(0.0f);
	CHECK(node->get_hlod_tau_px() == 0.0f);
	memdelete(node);
}
}

namespace TestHlodRuntimePublication {
TEST_CASE("[GaussianSplatting][WorldIO][HLOD] rounded runtime sphere retains a tiny leaf at the frustum boundary") {
    Ref<GaussianData> data;
    data.instantiate();
    LocalVector<Gaussian> gaussians;
    gaussians.resize(2);
    for (uint32_t i = 0; i < 2; ++i) {
        gaussians[i].position = Vector3(16.0f + float(i + 2) * std::ldexp(1.0f, -19), 1.0f, -10.0f);
        const float scale = std::ldexp(1.0f, -30);
        gaussians[i].scale = Vector3(scale, scale, scale);
        gaussians[i].rotation = Quaternion();
        gaussians[i].opacity = 0.5f;
    }
    LocalVector<Vector3> sh;
    data->set_gaussian_payload(gaussians, sh, 0u, 0u, false, false);
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(data);
    if (world->bake_hlod() != OK) { FAIL("tiny exact-coordinate world bake failed"); return; }
    const float baked_radius = world->get_hlod_tree().nodes[0].radius;
    Ref<GaussianSplatHlodRuntimeData> snapshot;
    String reason;
    if (world->capture_hlod_runtime_snapshot(snapshot, reason) != OK || snapshot.is_null()) {
        FAIL("snapshot failed: ", reason.utf8().get_data()); return;
    }
    if (snapshot->nodes.size() != 1) { FAIL("expected one complete leaf"); return; }
    const Plane boundary(Vector3(-1, 0, 0), -gaussians[1].position.x);
    const auto &node = snapshot->nodes[0];
    // This fixture discriminates: the old unchanged radius rejected the node,
    // although the second canonical splat center lies on the inclusive plane.
    CHECK(boundary.distance_to(node.bounds_center) > baked_radius);
    CHECK(boundary.distance_to(gaussians[1].position) == 0.0f);
    CHECK(boundary.distance_to(node.bounds_center) <= node.topology.radius);
    CHECK(world->get_hlod_tree().nodes[0].radius == baked_radius);
    gs_hlod_runtime::CutHistory history;
    if (!history.reserve(1)) { FAIL("cut workspace unavailable"); return; }
    gs_hlod_runtime::CutRequest request;
    request.frustum_enabled = true;
    for (Plane &plane : request.frustum) { plane = Plane(Vector3(0, 1, 0), 1000.0f); }
    request.frustum[0] = boundary;
    request.focal_pixels = 100.0;
    request.tau_pixels = 1.0;
    request.record_budget = 1000.0;
    request.splat_budget = 2;
    request.node_budget = 1;
    const auto selected = history.select(*snapshot.ptr(), request);
    CHECK(selected.status == gs_hlod_runtime::CutHistory::COMPLETE);
    CHECK(selected.selected_count == 1);
    CHECK(selected.selected_splats == 2);
}
}
