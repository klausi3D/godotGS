/**************************************************************************/
/*  test_gaussian_scene_cull_bounds.h                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

// #1548: the scene cull must transform a Gaussian instance's bounds exactly
// once. The producers (GaussianSplatNode3D, GaussianSplatWorld3D) give the
// render instance the node's global transform and its LOCAL bounds as the
// custom AABB. Before the fix the GS case of
// RendererSceneCull::_update_instance_aabb ignored custom_aabb and read the
// storage AABB, which the producers had already transformed to world space,
// and _update_instance then applied the instance transform a second time.
//
// The cases run the real producer nodes in a [SceneTree] with the headless
// dummy rasterizer, install a GaussianSplatStorage (the fixture from
// test_gaussian_storage_render_thread_routing.h), and then call the real
// frame cull (RendererSceneCull::_scene_cull, the code that fills
// cull_result.gaussian_splats) with a camera frustum. Each case uses two
// cameras: one that sees only the correctly transformed box and one that sees
// only the double-transformed box. Each camera's discrimination is asserted
// geometrically first, so a camera that saw both boxes (or neither) fails the
// case instead of passing it.
//
// Shadow casters: the scene cull has no light-view cull for Gaussian
// instances. The RD scene renderer builds its Gaussian shadow-caster list from
// the camera cull list (renderer_scene_render_rd.cpp, gaussian_shadow_renderers)
// and the HLOD shadow refresh does too (renderer_scene_cull.cpp,
// collect_hlod_shadow_refresh_owners). The shadow case therefore asserts that a
// shadow-casting node on screen reaches that list with casts_shadow set.

#ifndef TEST_GAUSSIAN_SCENE_CULL_BOUNDS_H
#define TEST_GAUSSIAN_SCENE_CULL_BOUNDS_H

#include "tests/test_macros.h"

#include "core/math/geometry_3d.h"
#include "core/math/projection.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering/renderer_rd/storage_rd/gaussian_splat_storage.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server_globals.h"

#include "../core/gaussian_splat_world.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "test_gaussian_storage_render_thread_routing.h"

namespace TestGaussianSceneCullBounds {

using TestGaussianStorageRenderThreadRouting::StorageRoutingFixture;

static RendererSceneCull::Instance *find_gaussian_instance(ObjectID p_owner) {
	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	for (const RID &instance_rid : scene->instance_owner.get_owned_list()) {
		RendererSceneCull::Instance *instance = scene->instance_owner.get_or_null(instance_rid);
		if (instance && instance->base_type == RS::INSTANCE_GAUSSIAN_SPLAT && instance->object_id == p_owner) {
			return instance;
		}
	}
	return nullptr;
}

// A narrow perspective camera at p_eye looking down -Z.
static Vector<Plane> camera_planes(const Vector3 &p_eye) {
	Projection projection;
	projection.set_perspective(20.0, 1.0, 0.05, 200.0);
	return projection.get_projection_planes(Transform3D(Basis(), p_eye));
}

static bool box_in_planes(const AABB &p_box, const Vector<Plane> &p_planes) {
	const Vector<Vector3> points = Geometry3D::compute_convex_mesh_points(p_planes.ptr(), p_planes.size());
	return p_box.intersects_convex_shape(p_planes.ptr(), p_planes.size(), points.ptr(), points.size());
}

// Runs the real frame cull over the instance's scenario. Returns whether the
// instance's Gaussian slot is in cull_result.gaussian_splats.
static bool frame_cull_collects(RendererSceneCull::Instance *p_instance, const Vector3 &p_eye, String &r_error) {
	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	if (!p_instance->scenario) {
		r_error = "the Gaussian render instance has no scenario";
		return false;
	}
	if (p_instance->array_index < 0) {
		r_error = "the Gaussian render instance is not in the scenario's cull arrays (hidden or empty AABB)";
		return false;
	}
	const Vector<Plane> planes = camera_planes(p_eye);
	Projection projection;
	projection.set_perspective(20.0, 1.0, 0.05, 200.0);
	// Per-frame Cull state, set the way RendererSceneCull::_render_scene sets it
	// before _scene_cull for a camera with no directional shadow and no SDFGI
	// (renderer_scene_cull.cpp: cull.frustum, cull.shadow_count = 0,
	// cull.sdfgi.region_count = 0). Cull::shadow_count has no initializer
	// (renderer_scene_cull.h), so leaving it unset makes _scene_cull walk
	// garbage directional shadows: the Linux CI crash on #1566.
	scene->cull.frustum = RendererSceneCull::Frustum(planes);
	scene->cull.shadow_count = 0;
	scene->cull.sdfgi.region_count = 0;
	scene->cull.sdfgi.cascade_light_count = 0;

	RendererSceneCull::CullData cull_data;
	cull_data.cull = &scene->cull;
	cull_data.scenario = p_instance->scenario;
	cull_data.cam_transform = Transform3D(Basis(), p_eye);
	cull_data.visible_layers = 0xFFFFFFFF;
	cull_data.render_reflection_probe = nullptr;
	cull_data.occlusion_buffer = nullptr;
	cull_data.camera_matrix = &projection;
	cull_data.visibility_viewport_mask = 0;

	RendererSceneCull::InstanceCullResult result;
	result.init(&scene->rid_cull_page_pool, &scene->geometry_instance_cull_page_pool, &scene->instance_cull_page_pool);
	scene->_scene_cull(cull_data, result, 0, p_instance->scenario->instance_data.size());
	const RID base = p_instance->base;
	bool found = false;
	for (uint64_t i = 0; i < result.gaussian_splats.size(); i++) {
		if (result.gaussian_splats[i] == base) {
			found = true;
		}
	}
	result.reset();
	return found;
}

struct CullScenario {
	const char *name;
	Transform3D xform;
	// The camera that sees only the correctly transformed box, and the one
	// that sees only the double-transformed box.
	Vector3 eye_correct;
	Vector3 eye_double;
};

static void run_node_scenario(const CullScenario &p_scenario, bool p_cast_shadow) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required (provided by the [SceneTree] tag)");
		return;
	}
	StorageRoutingFixture fixture;
	const String install_error = fixture.install();
	if (!install_error.is_empty()) {
		FAIL(install_error.utf8().get_data());
		return;
	}
	const String scenario_name = p_scenario.name;
	CAPTURE(scenario_name);

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	node->set_transform(p_scenario.xform);
	node->set_cast_shadow(p_cast_shadow);
	tree->get_root()->add_child(node);
	tree->process(0.0);

	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	scene->update_dirty_instances();
	RendererSceneCull::Instance *instance = find_gaussian_instance(node->get_instance_id());
	if (!instance) {
		FAIL("no Gaussian render instance is attached to the node");
		tree->get_root()->remove_child(node);
		memdelete(node);
		return;
	}

	const AABB local = node->get_aabb();
	if (!local.has_volume()) {
		FAIL("the node published no local bounds; the case cannot discriminate");
		tree->get_root()->remove_child(node);
		memdelete(node);
		return;
	}
	const Transform3D global = node->get_global_transform();
	AABB local_with_margin = local;
	local_with_margin.grow_by(instance->extra_margin);
	const AABB expected = global.xform(local_with_margin);
	const AABB doubled = global.xform(global.xform(local_with_margin));

	// The scenario must be able to tell the two boxes apart.
	if (expected.intersects(doubled)) {
		FAIL("the correct and double-transformed boxes overlap; choose a scenario that separates them");
	}
	const Vector<Plane> sees_correct = camera_planes(p_scenario.eye_correct);
	const Vector<Plane> sees_double = camera_planes(p_scenario.eye_double);
	CHECK_MESSAGE(box_in_planes(expected, sees_correct), "camera A must see the correctly transformed box");
	CHECK_MESSAGE(!box_in_planes(doubled, sees_correct), "camera A must not see the double-transformed box");
	CHECK_MESSAGE(box_in_planes(doubled, sees_double), "camera B must see the double-transformed box");
	CHECK_MESSAGE(!box_in_planes(expected, sees_double), "camera B must not see the correctly transformed box");

	CAPTURE(expected);
	CAPTURE(instance->transformed_aabb);
	CHECK_MESSAGE(instance->transformed_aabb.position.is_equal_approx(expected.position),
			"the cull AABB must be the local bounds transformed exactly once");
	CHECK_MESSAGE(instance->transformed_aabb.size.is_equal_approx(expected.size),
			"the cull AABB must be the local bounds transformed exactly once");

	// The slot holds both spaces: local for the scene cull, world for the HLOD
	// shadow refresh (collect_hlod_shadow_refresh_owners reads it as owner bounds).
	const AABB slot_local = fixture.storage->gaussian_get_aabb(instance->base);
	const AABB slot_world = fixture.storage->gaussian_get_world_aabb(instance->base);
	const AABB world_expected = global.xform(local);
	CHECK_MESSAGE((slot_local.position.is_equal_approx(local.position) && slot_local.size.is_equal_approx(local.size)),
			"the storage slot's cull AABB must be instance-local");
	CHECK_MESSAGE((slot_world.position.is_equal_approx(world_expected.position) && slot_world.size.is_equal_approx(world_expected.size)),
			"the storage slot's world AABB must be the local bounds transformed once");

	// Harness control, independent of the fix: a camera aimed at whatever box
	// the cull actually holds must collect the instance. Without it, a harness
	// that collects nothing would pass the off-screen assertion below.
	String error;
	const Vector3 control_eye = instance->transformed_aabb.get_center() + Vector3(0, 0, 15);
	const bool collected_control = frame_cull_collects(instance, control_eye, error);
	if (!error.is_empty()) {
		FAIL(error.utf8().get_data());
	} else if (!collected_control) {
		FAIL("harness control: the frame cull did not collect the instance with its own cull box in view");
	} else {
		const bool collected_correct = frame_cull_collects(instance, p_scenario.eye_correct, error);
		CHECK_MESSAGE(collected_correct, "a node whose real bounds are on screen must reach cull_result.gaussian_splats");
		const bool collected_double = frame_cull_collects(instance, p_scenario.eye_double, error);
		CHECK_MESSAGE(!collected_double, "a node whose real bounds are off screen must not reach cull_result.gaussian_splats");
		if (p_cast_shadow) {
			// The RD scene renderer's Gaussian shadow-caster list is the cull
			// list filtered by the slot's casts_shadow flag.
			const bool reaches_shadow_list = collected_correct && fixture.storage->gaussian_get_casts_shadow(instance->base);
			CHECK(fixture.storage->gaussian_get_casts_shadow(instance->base));
			CHECK_MESSAGE(reaches_shadow_list, "an on-screen shadow-casting node must reach the Gaussian shadow-caster list");
		}
	}

	tree->get_root()->remove_child(node);
	memdelete(node);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] GaussianSplatNode3D bounds are transformed once by the scene cull (translated)") {
	run_node_scenario({ "translated", Transform3D(Basis(), Vector3(10, 0, 0)), Vector3(10, 0, 15), Vector3(20, 0, 15) }, false);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] GaussianSplatNode3D bounds are transformed once by the scene cull (rotated)") {
	// 90 degrees about Z, placed at x=10: the doubled box lands at (10, 10, 0).
	run_node_scenario({ "rotated", Transform3D(Basis(Vector3(0, 0, 1), Math::PI * 0.5), Vector3(10, 0, 0)), Vector3(10, 0, 15), Vector3(10, 10, 15) }, false);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] GaussianSplatNode3D bounds are transformed once by the scene cull (scaled)") {
	// Scale 3 at x=10: correct box [7, 13] on x, doubled box [31, 49].
	run_node_scenario({ "scaled", Transform3D(Basis().scaled(Vector3(3, 3, 3)), Vector3(10, 0, 0)), Vector3(10, 0, 15), Vector3(40, 0, 15) }, false);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] A shadow-casting GaussianSplatNode3D on screen reaches the Gaussian shadow-caster list") {
	run_node_scenario({ "translated shadow caster", Transform3D(Basis(), Vector3(10, 0, 0)), Vector3(10, 0, 15), Vector3(20, 0, 15) }, true);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] The scene cull honours a Gaussian instance's custom AABB like other geometry") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required (provided by the [SceneTree] tag)");
		return;
	}
	StorageRoutingFixture fixture;
	const String install_error = fixture.install();
	if (!install_error.is_empty()) {
		FAIL(install_error.utf8().get_data());
		return;
	}
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	node->set_transform(Transform3D(Basis(), Vector3(10, 0, 0)));
	tree->get_root()->add_child(node);
	tree->process(0.0);

	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	scene->update_dirty_instances();
	RendererSceneCull::Instance *instance = find_gaussian_instance(node->get_instance_id());
	if (!instance) {
		FAIL("no Gaussian render instance is attached to the node");
	} else {
		// A custom box that differs from the slot's bounds in both spaces, so
		// the result tells which one the cull read.
		const AABB custom(Vector3(2, 3, 4), Vector3(1, 1, 1));
		CHECK(custom != fixture.storage->gaussian_get_aabb(instance->base));
		RS::get_singleton()->instance_set_custom_aabb(instance->self, custom);
		scene->update_dirty_instances();
		AABB custom_with_margin = custom;
		custom_with_margin.grow_by(instance->extra_margin);
		const AABB expected = node->get_global_transform().xform(custom_with_margin);
		CAPTURE(expected);
		CAPTURE(instance->transformed_aabb);
		CHECK(instance->transformed_aabb.position.is_equal_approx(expected.position));
		CHECK(instance->transformed_aabb.size.is_equal_approx(expected.size));
	}
	tree->get_root()->remove_child(node);
	memdelete(node);
}

static void run_world_scenario(const Transform3D &p_xform, bool p_expect_doubled_differs) {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required (provided by the [SceneTree] tag)");
		return;
	}
	StorageRoutingFixture fixture;
	const String install_error = fixture.install();
	if (!install_error.is_empty()) {
		FAIL(install_error.utf8().get_data());
		return;
	}

	const AABB local(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	Ref<GaussianSplatWorld> world;
	world.instantiate();
	world->set_bounds(local);

	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	world_node->set_auto_apply_on_ready(false);
	world_node->set_transform(p_xform);
	tree->get_root()->add_child(world_node);
	ERR_PRINT_OFF;
	world_node->set_world(world);
	world_node->apply_world();
	ERR_PRINT_ON;
	tree->process(0.0);

	RendererSceneCull *scene = static_cast<RendererSceneCull *>(RSG::scene);
	scene->update_dirty_instances();
	RendererSceneCull::Instance *instance = find_gaussian_instance(world_node->get_instance_id());
	if (!instance) {
		FAIL("no Gaussian render instance is attached to the world node");
	} else {
		const Transform3D global = world_node->get_global_transform();
		AABB local_with_margin = local;
		local_with_margin.grow_by(instance->extra_margin);
		const AABB expected = global.xform(local_with_margin);
		if (p_expect_doubled_differs && expected.intersects(global.xform(expected))) {
			FAIL("the correct and double-transformed boxes overlap; choose a transform that separates them");
		}
		CAPTURE(expected);
		CAPTURE(instance->transformed_aabb);
		CHECK_MESSAGE(instance->transformed_aabb.position.is_equal_approx(expected.position),
				"the world instance's cull AABB must be its local bounds transformed exactly once");
		CHECK_MESSAGE(instance->transformed_aabb.size.is_equal_approx(expected.size),
				"the world instance's cull AABB must be its local bounds transformed exactly once");

		String error;
		const bool collected_control = frame_cull_collects(instance, instance->transformed_aabb.get_center() + Vector3(0, 0, 15), error);
		const bool collected = error.is_empty() && frame_cull_collects(instance, global.origin + Vector3(0, 0, 15), error);
		if (!error.is_empty()) {
			FAIL(error.utf8().get_data());
		} else if (!collected_control) {
			FAIL("harness control: the frame cull did not collect the instance with its own cull box in view");
		} else {
			CHECK_MESSAGE(collected, "a world whose bounds are on screen must reach cull_result.gaussian_splats");
		}
	}

	tree->get_root()->remove_child(world_node);
	memdelete(world_node);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] GaussianSplatWorld3D at identity is culled by its own bounds") {
	run_world_scenario(Transform3D(), false);
}

TEST_CASE("[GaussianSplatting][SceneCull][SceneTree] GaussianSplatWorld3D bounds are transformed once by the scene cull (non-identity, compat-warned)") {
	run_world_scenario(Transform3D(Basis(), Vector3(10, 0, 0)), true);
}

} // namespace TestGaussianSceneCullBounds

#endif // TEST_GAUSSIAN_SCENE_CULL_BOUNDS_H
