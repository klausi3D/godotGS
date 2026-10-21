void gs_hlod_runtime_publication_tests_link() {}
#include "test_macros.h"
#include "../core/gaussian_splat_world.h"
#include "../core/gaussian_splat_hlod_runtime_data.h"
#include "../core/gs_vector_alloc.h"
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
