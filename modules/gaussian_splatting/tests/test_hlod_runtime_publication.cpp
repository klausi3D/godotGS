extern "C" int test_hlod_runtime_publication_cpp_force_link() { return 0; }
#include "test_macros.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gs_vector_alloc.h"
#include "../lod/hlod_runtime_selection.h"
#include <cmath>
#include "../nodes/gaussian_splat_world_3d.h"
#include <limits>
#include "../core/gaussian_splat_scene_director.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

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

namespace TestHlodRuntimePublication {
TEST_CASE("[GaussianSplatting][HLOD][Node][SceneTree] tau changes preserve explicit world registration and scenario ownership") {
    SceneTree *tree = SceneTree::get_singleton();
    if (!tree || !tree->get_root()) { FAIL("actual SceneTree root required"); return; }
    GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
    const bool owns_director = !director;
    if (!director) { director = memnew(GaussianSplatSceneDirector); }
    struct Fixture {
        GaussianSplatSceneDirector *director;
        bool owns_director;
        GaussianSplatWorld3D *owner = nullptr;
        GaussianSplatWorld3D *peer = nullptr;
        ~Fixture() {
            for (auto *node : { peer, owner }) {
                if (node) {
                    if (node->get_parent()) { node->get_parent()->remove_child(node); }
                    memdelete(node);
                }
            }
            if (owns_director) { memdelete(director); }
        }
    } fixture{ director, owns_director };
    Ref<GaussianData> data;
    data.instantiate();
    LocalVector<Gaussian> splats;
    splats.resize(1); // Fixed one-element synthetic publication payload.
    splats[0].position = Vector3(0.125, 0.125, -4.0);
    splats[0].scale = Vector3(0.125, 0.125, 0.125);
    splats[0].opacity = 0.75;
    LocalVector<Vector3> sh;
    data->set_gaussian_payload(splats, sh, 0, 0, false, false);
    Ref<GaussianSplatWorld> world;
    world.instantiate();
    world->set_gaussian_data(data);
    fixture.owner = memnew(GaussianSplatWorld3D);
    fixture.owner->set_auto_apply_on_ready(false);
    fixture.owner->set_world(world);
    tree->get_root()->add_child(fixture.owner);
    tree->process(0.0);
    auto registered = [&](GaussianSplatWorld3D *node) {
        GaussianSplatSceneDirector::WorldSubmission current;
        return director->get_world_submission(node->get_instance_id(), &current);
    };
    if (registered(fixture.owner)) { FAIL("never-applied precondition missing"); return; }
    fixture.owner->set_hlod_tau_px(0.5f);
    CHECK(fixture.owner->get_hlod_tau_px() == 0.5f);
    CHECK_FALSE(registered(fixture.owner));
    fixture.owner->apply_world();
    if (!registered(fixture.owner)) { FAIL("explicit apply failed"); return; }
    fixture.owner->set_hlod_tau_px(0.75f);
    GaussianSplatSceneDirector::WorldSubmission applied;
    if (!director->get_world_submission(fixture.owner->get_instance_id(), &applied)) { FAIL("applied owner lost submission"); return; }
    CHECK(applied.hlod_tau_px == 0.75f);
    CHECK(applied.gaussian_data == data);
    ERR_PRINT_OFF;
    fixture.owner->set_hlod_tau_px(-1.0f);
    fixture.owner->set_hlod_tau_px(std::numeric_limits<float>::quiet_NaN());
    fixture.owner->set_hlod_tau_px(std::numeric_limits<float>::infinity());
    ERR_PRINT_ON;
    CHECK(fixture.owner->get_hlod_tau_px() == 0.75f);
    GaussianSplatSceneDirector::WorldSubmission unchanged;
    if (!director->get_world_submission(fixture.owner->get_instance_id(), &unchanged)) { FAIL("invalid tau changed registration"); return; }
    CHECK(unchanged.hlod_tau_px == applied.hlod_tau_px);
    CHECK(unchanged.gaussian_data == applied.gaussian_data);
    fixture.peer = memnew(GaussianSplatWorld3D);
    fixture.peer->set_auto_apply_on_ready(false);
    fixture.peer->set_world(world);
    tree->get_root()->add_child(fixture.peer);
    fixture.peer->apply_world(); // Competing owner loses the existing scenario arbitration.
    if (registered(fixture.peer)) { FAIL("competing owner unexpectedly acquired occupied scenario"); return; }
    fixture.peer->set_hlod_tau_px(1.5f);
    CHECK(fixture.peer->get_hlod_tau_px() == 1.5f);
    CHECK_FALSE(registered(fixture.peer));
    CHECK(registered(fixture.owner));
    fixture.owner->clear_world();
    if (registered(fixture.owner)) { FAIL("clear_world did not release submission"); return; }
    fixture.owner->set_hlod_tau_px(2.0f);
    CHECK(fixture.owner->get_hlod_tau_px() == 2.0f);
    CHECK_FALSE(registered(fixture.owner));
    CHECK_FALSE(registered(fixture.peer));
}
}
