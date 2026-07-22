#pragma once

// Regression coverage for #740: the InstanceStore cache-invalidation bump.
//
// GaussianSplatSceneDirector keeps two monotonic counters per SharedWorld --
// InstanceStore::instance_generation and instance_asset_generation. The renderer
// re-uploads instance / asset buffers only when the value it last observed
// (get_instance_generation_for_renderer / get_instance_asset_generation_for_renderer)
// has advanced. If a mutation entry point forgot to bump, or the bump helper were
// neutered, every instance edit would silently stop re-uploading -- and, as #740
// documents, NOTHING in the suite noticed: neutering InstanceStore::bump_generation()
// left the CPU scene-director, SceneDirectorSceneTree and NodeSceneTree batches all
// green, because the only nearby test (test_scene_director_lod_walk_cache.h) checks
// LODConfig::operator== and never reads a generation counter.
//
// These cases close that blind spot. They register / mutate / unregister instances
// through the public director API and assert the generation counters STRICTLY
// ADVANCE (new > old, not merely non-zero). They run headless: the counters are
// read by scenario via test_instance_generation_for_scenario(), which returns the
// exact InstanceStore value get_instance_generation_for_renderer() returns once a
// renderer is attached -- so no RenderingDevice is required and the coverage lands
// in the fast [SceneDirector] lane instead of a deferred [RequiresGPU] entry.
//
// MUTATION-PROVEN: neutering InstanceStore::bump_generation() reddens the
// instance_generation case; neutering InstanceStore::bump_asset_generation()
// reddens the asset-selection assertion of the asset case. See PR #740 evidence.

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace {

Ref<GaussianSplatAsset> _make_generation_test_asset(float p_x_offset) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);

	PackedFloat32Array positions;
	positions.resize(3);
	positions.set(0, p_x_offset);
	positions.set(1, 0.0f);
	positions.set(2, 0.0f);
	asset->set_positions(positions);

	PackedFloat32Array scales;
	scales.resize(3);
	scales.set(0, 1.0f);
	scales.set(1, 1.0f);
	scales.set(2, 1.0f);
	asset->set_scales(scales);

	PackedFloat32Array rotations;
	rotations.resize(4);
	rotations.set(0, 1.0f);
	rotations.set(1, 0.0f);
	rotations.set(2, 0.0f);
	rotations.set(3, 0.0f);
	asset->set_rotations(rotations);

	PackedFloat32Array sh_dc;
	sh_dc.resize(3);
	sh_dc.set(0, 1.0f);
	sh_dc.set(1, 1.0f);
	sh_dc.set(2, 1.0f);
	asset->set_sh_dc_coefficients(sh_dc);

	PackedFloat32Array opacity_logits;
	opacity_logits.resize(1);
	opacity_logits.set(0, 10.0f);
	asset->set_opacity_logits(opacity_logits);

	return asset;
}

} // namespace

// The instance_generation counter must advance on EVERY instance-set mutation:
// register, transform edit, param edit, add-a-second-instance, unregister. Every
// one of these bumps routes exclusively through InstanceStore::bump_generation(),
// so neutering that helper freezes the counter and reddens the strict-advance
// assertions below (that is the #740 mutation-proof).
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] instance_generation strictly advances on every instance mutation") {
	SceneTree *tree = SceneTree::get_singleton();
	// #656: REQUIRE does not abort under DOCTEST_CONFIG_NO_EXCEPTIONS, so a null-ish
	// REQUIRE followed by a dereference would crash the whole binary on failure.
	// Guard with an explicit early return before every deref.
	if (tree == nullptr) {
		FAIL("SceneTree must exist (provided by [SceneTree] tag)");
		return;
	}

	Window *root = tree->get_root();
	if (root == nullptr) {
		FAIL("SceneTree root window must exist");
		return;
	}

	Ref<World3D> world = root->get_world_3d();
	if (!world.is_valid()) {
		FAIL("SceneTree root must have a valid World3D");
		return;
	}
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (director == nullptr) {
		FAIL("GaussianSplatSceneDirector must be available");
		return;
	}

	Node3D *node_a = memnew(Node3D);
	Node3D *node_b = memnew(Node3D);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);

	Ref<GaussianSplatAsset> asset = _make_generation_test_asset(0.0f);
	REQUIRE(asset.is_valid());

	// Register the first instance -> the world is created and the counter is
	// bumped past its unset (0) / initial value.
	director->register_instance(node_a->get_instance_id(), asset, Transform3D(), 1.0f, 0.0f, 0u);
	const uint64_t gen_after_register = director->test_instance_generation_for_scenario(scenario);
	REQUIRE(gen_after_register > 0);

	// A transform edit is a re-upload trigger; the counter must move.
	Transform3D moved;
	moved.origin = Vector3(5.0f, 0.0f, 0.0f);
	director->update_instance_transform(node_a->get_instance_id(), moved);
	const uint64_t gen_after_transform = director->test_instance_generation_for_scenario(scenario);
	CHECK_MESSAGE(gen_after_transform > gen_after_register,
			"update_instance_transform must strictly advance instance_generation");

	// A param edit (opacity) is likewise a re-upload trigger.
	director->update_instance_params(node_a->get_instance_id(), 0.5f, 0.0f, 0u);
	const uint64_t gen_after_params = director->test_instance_generation_for_scenario(scenario);
	CHECK_MESSAGE(gen_after_params > gen_after_transform,
			"update_instance_params must strictly advance instance_generation");

	// Adding a second instance changes the instance set -> counter advances.
	director->register_instance(node_b->get_instance_id(), asset, Transform3D(), 1.0f, 0.0f, 0u);
	const uint64_t gen_after_add = director->test_instance_generation_for_scenario(scenario);
	CHECK_MESSAGE(gen_after_add > gen_after_params,
			"registering a second instance must strictly advance instance_generation");

	// Removing an instance changes the instance set -> counter advances.
	director->unregister_instance(node_b->get_instance_id());
	const uint64_t gen_after_remove = director->test_instance_generation_for_scenario(scenario);
	CHECK_MESSAGE(gen_after_remove > gen_after_add,
			"unregister_instance must strictly advance instance_generation");

	director->unregister_instance(node_a->get_instance_id());

	root->remove_child(node_b);
	root->remove_child(node_a);
	memdelete(node_b);
	memdelete(node_a);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

// The instance_asset_generation counter gates the ASSET-selection re-upload
// (which assets are resident, which are shadow casters). It must advance when a
// new asset is bound and when an asset-selection input (casts_shadow) flips. The
// casts_shadow flip routes ONLY through InstanceStore::bump_asset_generation()
// (no asset retain/release on a bare param edit), so neutering that helper freezes
// the counter across the flip and reddens the strict-advance assertion -- the
// asset-path mutation-proof for #740.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] instance_asset_generation strictly advances on asset bind and selection change") {
	SceneTree *tree = SceneTree::get_singleton();
	// #656: REQUIRE does not abort under DOCTEST_CONFIG_NO_EXCEPTIONS, so a null-ish
	// REQUIRE followed by a dereference would crash the whole binary on failure.
	// Guard with an explicit early return before every deref.
	if (tree == nullptr) {
		FAIL("SceneTree must exist (provided by [SceneTree] tag)");
		return;
	}

	Window *root = tree->get_root();
	if (root == nullptr) {
		FAIL("SceneTree root window must exist");
		return;
	}

	Ref<World3D> world = root->get_world_3d();
	if (!world.is_valid()) {
		FAIL("SceneTree root must have a valid World3D");
		return;
	}
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (director == nullptr) {
		FAIL("GaussianSplatSceneDirector must be available");
		return;
	}

	Node3D *node_a = memnew(Node3D);
	Node3D *node_b = memnew(Node3D);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);

	Ref<GaussianSplatAsset> asset_a = _make_generation_test_asset(0.0f);
	Ref<GaussianSplatAsset> asset_b = _make_generation_test_asset(10.0f);
	if (!asset_a.is_valid() || !asset_b.is_valid()) {
		FAIL("both test assets must be valid");
		return;
	}
	REQUIRE(asset_a->get_instance_id() != asset_b->get_instance_id());

	// Binding the first asset creates the world and its asset record.
	director->register_instance(node_a->get_instance_id(), asset_a, Transform3D(), 1.0f, 0.0f, 0u);
	const uint64_t asset_gen_after_first = director->test_instance_asset_generation_for_scenario(scenario);
	REQUIRE(asset_gen_after_first > 0);

	// Binding a DISTINCT second asset retains a new asset record -> asset gen advances.
	director->register_instance(node_b->get_instance_id(), asset_b, Transform3D(), 1.0f, 0.0f, 0u);
	const uint64_t asset_gen_after_second = director->test_instance_asset_generation_for_scenario(scenario);
	CHECK_MESSAGE(asset_gen_after_second > asset_gen_after_first,
			"binding a distinct asset must strictly advance instance_asset_generation");

	// Flip casts_shadow on node_a. This changes asset SELECTION without retaining
	// or releasing any asset record, so the ONLY thing that can advance the asset
	// generation here is InstanceStore::bump_asset_generation(). (opacity/lod/flags
	// unchanged from the register call above so instance_generation's non-asset
	// bump is not what we are measuring.)
	director->update_instance_params(node_a->get_instance_id(), 1.0f, 0.0f, 0u, /*casts_shadow=*/true);
	const uint64_t asset_gen_after_shadow_flip = director->test_instance_asset_generation_for_scenario(scenario);
	CHECK_MESSAGE(asset_gen_after_shadow_flip > asset_gen_after_second,
			"flipping casts_shadow must strictly advance instance_asset_generation");

	director->unregister_instance(node_a->get_instance_id());
	director->unregister_instance(node_b->get_instance_id());

	root->remove_child(node_b);
	root->remove_child(node_a);
	memdelete(node_b);
	memdelete(node_a);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
