#pragma once

#include "test_macros.h"
#include "gs_test_setting_guard.h"
#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_asset.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gs_project_settings.h"
#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/gaussian_splat_world_3d.h"
#include "../interfaces/gpu_sorting_pipeline.h" // #980: SortBufferHandles / get_buffer_handles()
#include "../renderer/quantization_config.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "servers/rendering_server.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/storage/render_scene_buffers.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace {

Ref<GaussianData> stage1a_make_submission_test_data(int p_count, float p_x_offset = 0.0f) {
	Ref<GaussianData> data;
	data.instantiate();
	data->resize(p_count);
	for (int i = 0; i < p_count; i++) {
		// Value-initialised: Gaussian leaves area / normal / stroke_age /
		// brush_axes / painterly_meta without default initialisers
		// (core/gaussian_data.h), and _validate_gpu_payload_locked rejects the
		// indeterminate values that `Gaussian g;` would leave there. Before #685
		// this helper hit exactly that -- every GPU-tagged case built on it logged
		// "Gaussian[0] has invalid stroke age" and create_gpu_buffer returned a
		// null RID, so the resident-route cases below rendered with no gaussian
		// buffer at all.
		Gaussian g = {};
		g.position = Vector3(p_x_offset + float(i), 0.0f, 0.0f);
		g.scale = Vector3(1.0f, 1.0f, 1.0f);
		g.rotation = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
		g.opacity = 1.0f;
		g.sh_dc = Color(1.0f, 1.0f, 1.0f, 1.0f);
		g.normal = Vector3(0.0f, 0.0f, 1.0f);
		data->set_gaussian(i, g);
	}
	return data;
}

Ref<GaussianSplatAsset> stage1a_make_submission_test_asset(float p_x_offset = 0.0f) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);

	PackedFloat32Array positions;
	positions.resize(3);
	{
		float *ptr = positions.ptrw();
		ptr[0] = p_x_offset;
		ptr[1] = 0.0f;
		ptr[2] = 0.0f;
	}
	asset->set_positions(positions);

	PackedFloat32Array scales;
	scales.resize(3);
	{
		float *ptr = scales.ptrw();
		ptr[0] = 1.0f;
		ptr[1] = 1.0f;
		ptr[2] = 1.0f;
	}
	asset->set_scales(scales);

	PackedFloat32Array rotations;
	rotations.resize(4);
	{
		float *ptr = rotations.ptrw();
		ptr[0] = 1.0f;
		ptr[1] = 0.0f;
		ptr[2] = 0.0f;
		ptr[3] = 0.0f;
	}
	asset->set_rotations(rotations);

	PackedFloat32Array sh_dc;
	sh_dc.resize(3);
	{
		float *ptr = sh_dc.ptrw();
		ptr[0] = 1.0f;
		ptr[1] = 1.0f;
		ptr[2] = 1.0f;
	}
	asset->set_sh_dc_coefficients(sh_dc);

	PackedFloat32Array opacity_logits;
	opacity_logits.resize(1);
	opacity_logits.set(0, 10.0f);
	asset->set_opacity_logits(opacity_logits);
	return asset;
}

StaticChunk stage1a_make_submission_test_chunk(uint32_t p_index) {
	StaticChunk chunk;
	chunk.bounds = AABB(Vector3(float(p_index), 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
	chunk.center = chunk.bounds.get_center();
	chunk.radius = 1.0f;
	chunk.indices.push_back(p_index);
	return chunk;
}

// Re-publish the submission a GaussianSplatWorld3D just applied, with the
// residency hint flipped to RESIDENT and everything else byte-identical.
//
// GaussianSplatWorld3D derives its hint solely from
// `rendering/gaussian_splatting/streaming/route_policy`
// (nodes/gaussian_splat_world_3d.cpp:511-514), so a STREAMING project cannot
// publish a resident-hinted world submission through the node API. That
// combination -- project asks for streaming, the world submission asks for
// resident, resident wins -- is exactly what
// GaussianSplatRenderer::should_prefer_resident_backend's
// `submission_hint_resident:%s` branch exists to express
// (renderer/gaussian_splat_renderer.cpp:2810-2823), and it is only reachable by
// submitting to the director directly. Re-submitting under the SAME owner_id
// replaces the record rather than being arbitrated away
// (`same_owner`, core/gaussian_splat_scene_director.cpp:2303).
//
// Reading the live record back and flipping one field (rather than hand-building
// a submission) keeps the renderer's data, chunks and overrides exactly what
// apply_world() produced, so only the hint varies. (#685)
bool stage1a_repin_world_submission_as_resident(GaussianSplatSceneDirector *p_director, Node *p_owner) {
	if (p_director == nullptr || p_owner == nullptr) {
		return false;
	}
	GaussianSplatSceneDirector::WorldSubmission submission;
	if (!p_director->get_world_submission(p_owner->get_instance_id(), &submission)) {
		return false;
	}
	if (!submission.has_desired_residency_hint ||
			submission.desired_residency_hint != GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING) {
		// The precondition this helper exists to invert is gone: the node already
		// published a resident hint, so the test would no longer be exercising
		// hint-beats-policy. Fail rather than silently pass a weaker case.
		return false;
	}
	submission.desired_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	return p_director->submit_world_submission(submission);
}

} // namespace

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] World submission entrypoints arbitrate ownership and release cleanly") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);
	const GaussianSplatSceneDirector::SubmissionCounts baseline_counts = director->get_submission_counts();

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner_a = memnew(Node);
	Node *owner_b = memnew(Node);
	REQUIRE(owner_a != nullptr);
	REQUIRE(owner_b != nullptr);
	root->add_child(owner_a);
	root->add_child(owner_b);
	tree->process(0.0);

	GaussianSplatSceneDirector::WorldSubmission submission_a;
	submission_a.owner_id = owner_a->get_instance_id();
	submission_a.scenario = scenario;
	submission_a.gaussian_data = stage1a_make_submission_test_data(3, 0.0f);
	submission_a.static_chunks.push_back(stage1a_make_submission_test_chunk(0));
	submission_a.metadata[StringName("label")] = String("owner_a");
	submission_a.desired_renderer_overrides[StringName("max_splats")] = int64_t(2048);

	GaussianSplatSceneDirector::WorldSubmission submission_b;
	submission_b.owner_id = owner_b->get_instance_id();
	submission_b.scenario = scenario;
	submission_b.gaussian_data = stage1a_make_submission_test_data(2, 20.0f);
	submission_b.static_chunks.push_back(stage1a_make_submission_test_chunk(1));
	submission_b.metadata[StringName("label")] = String("owner_b");
	submission_b.desired_renderer_overrides[StringName("max_splats")] = int64_t(1024);

	CHECK(director->submit_world_submission(submission_a));

	GaussianSplatSceneDirector::WorldSubmission queried_submission;
	CHECK(director->get_world_submission_for_scenario(scenario, &queried_submission));
	CHECK(queried_submission.owner_id == submission_a.owner_id);
	CHECK(queried_submission.gaussian_data == submission_a.gaussian_data);

	CHECK_FALSE(director->submit_world_submission(submission_b));
	CHECK(director->get_world_submission_for_scenario(scenario, &queried_submission));
	CHECK(queried_submission.owner_id == submission_a.owner_id);

	GaussianSplatSceneDirector::SubmissionCounts counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions);
	CHECK(counts.world_submissions == baseline_counts.world_submissions + 1);

	director->release_world_submission(submission_a.owner_id);
	CHECK_FALSE(director->get_world_submission(submission_a.owner_id, &queried_submission));
	CHECK_FALSE(director->get_world_submission_for_scenario(scenario, &queried_submission));

	CHECK(director->submit_world_submission(submission_b));
	CHECK(director->get_world_submission_for_scenario(scenario, &queried_submission));
	CHECK(queried_submission.owner_id == submission_b.owner_id);

	director->release_world_submission(submission_b.owner_id);
	CHECK_FALSE(director->get_world_submission(submission_b.owner_id, &queried_submission));
	counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions);
	CHECK(counts.world_submissions == baseline_counts.world_submissions);

	root->remove_child(owner_b);
	root->remove_child(owner_a);
	memdelete(owner_b);
	memdelete(owner_a);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Same-owner resubmit preserves the original renderer restore point") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the same-owner restore-point test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(owner);
		memdelete(owner);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot baseline_renderer_state =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(baseline_renderer_state.valid);
	CHECK_FALSE(baseline_renderer_state.has_active_world_submission);

	GaussianSplatSceneDirector::WorldSubmission submission_a;
	submission_a.owner_id = owner->get_instance_id();
	submission_a.scenario = scenario;
	submission_a.gaussian_data = stage1a_make_submission_test_data(4, 0.0f);
	submission_a.static_chunks.push_back(stage1a_make_submission_test_chunk(0));
	submission_a.desired_renderer_overrides[StringName("lod_enabled")] = false;
	submission_a.desired_renderer_overrides[StringName("lod_bias")] = 1.5;
	submission_a.desired_renderer_overrides[StringName("max_splats")] = int64_t(2048);

	GaussianSplatSceneDirector::WorldSubmission submission_b = submission_a;
	submission_b.gaussian_data = stage1a_make_submission_test_data(2, 50.0f);
	submission_b.static_chunks.clear();
	submission_b.static_chunks.push_back(stage1a_make_submission_test_chunk(1));
	submission_b.desired_renderer_overrides[StringName("lod_enabled")] = true;
	submission_b.desired_renderer_overrides[StringName("lod_bias")] = 0.5;
	submission_b.desired_renderer_overrides[StringName("max_splats")] = int64_t(1024);

	CHECK(director->submit_world_submission(submission_a));
	CHECK(director->submit_world_submission(submission_b));

	director->release_world_submission(owner->get_instance_id());

	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot restored_renderer_state =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(restored_renderer_state.valid);
	CHECK(restored_renderer_state.gaussian_data == baseline_renderer_state.gaussian_data);
	CHECK(restored_renderer_state.static_chunks.size() == baseline_renderer_state.static_chunks.size());
	CHECK(restored_renderer_state.lod_enabled == baseline_renderer_state.lod_enabled);
	CHECK(restored_renderer_state.lod_bias == doctest::Approx(baseline_renderer_state.lod_bias));
	CHECK(restored_renderer_state.lod_max_distance == doctest::Approx(baseline_renderer_state.lod_max_distance));
	CHECK(restored_renderer_state.frustum_culling == baseline_renderer_state.frustum_culling);
	CHECK(restored_renderer_state.async_upload_enabled == baseline_renderer_state.async_upload_enabled);
	CHECK(restored_renderer_state.opacity_multiplier == doctest::Approx(baseline_renderer_state.opacity_multiplier));
	CHECK(restored_renderer_state.max_splats == baseline_renderer_state.max_splats);
	CHECK(restored_renderer_state.streaming_overrides.override_prefetch == baseline_renderer_state.streaming_overrides.override_prefetch);
	CHECK(restored_renderer_state.streaming_overrides.predictive_prefetch_enabled ==
			baseline_renderer_state.streaming_overrides.predictive_prefetch_enabled);
	CHECK(restored_renderer_state.streaming_overrides.prefetch_lookahead_distance ==
			doctest::Approx(baseline_renderer_state.streaming_overrides.prefetch_lookahead_distance));
	CHECK(restored_renderer_state.streaming_overrides.override_vram_budget ==
			baseline_renderer_state.streaming_overrides.override_vram_budget);
	CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.budget_mb ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.budget_mb);
	CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.min_chunks ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.min_chunks);
	CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.max_chunks ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.max_chunks);
	CHECK(restored_renderer_state.streaming_overrides.override_io_source ==
			baseline_renderer_state.streaming_overrides.override_io_source);
	CHECK(restored_renderer_state.has_active_world_submission == baseline_renderer_state.has_active_world_submission);
	CHECK(restored_renderer_state.has_desired_residency_hint == baseline_renderer_state.has_desired_residency_hint);
	CHECK(restored_renderer_state.desired_residency_hint == baseline_renderer_state.desired_residency_hint);

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Same-owner resubmit defaults omitted overrides from the preserved baseline") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the same-owner baseline-default test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(owner);
		memdelete(owner);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot baseline_renderer_state =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(baseline_renderer_state.valid);

	GaussianSplatSceneDirector::WorldSubmission submission_a;
	submission_a.owner_id = owner->get_instance_id();
	submission_a.scenario = scenario;
	submission_a.gaussian_data = stage1a_make_submission_test_data(6, 10.0f);
	submission_a.static_chunks.push_back(stage1a_make_submission_test_chunk(0));
	submission_a.desired_renderer_overrides[StringName("lod_enabled")] = false;
	submission_a.desired_renderer_overrides[StringName("lod_bias")] = 1.5;
	submission_a.desired_renderer_overrides[StringName("frustum_culling")] = false;
	submission_a.desired_renderer_overrides[StringName("async_upload_enabled")] = false;
	submission_a.desired_renderer_overrides[StringName("opacity_multiplier")] = 0.4f;
	submission_a.desired_renderer_overrides[StringName("max_splats")] = int64_t(4);
	Dictionary submission_a_streaming;
	submission_a_streaming[StringName("override_prefetch")] = true;
	submission_a_streaming[StringName("predictive_prefetch_enabled")] = false;
	submission_a_streaming[StringName("prefetch_lookahead_distance")] = 24.0f;
	submission_a_streaming[StringName("override_vram_budget")] = true;
	submission_a_streaming[StringName("vram_budget_mb")] = int64_t(64);
	submission_a_streaming[StringName("vram_min_chunks")] = int64_t(1);
	submission_a_streaming[StringName("vram_max_chunks")] = int64_t(4);
	submission_a.desired_renderer_overrides[StringName("streaming")] = submission_a_streaming;

	GaussianSplatSceneDirector::WorldSubmission submission_b;
	submission_b.owner_id = owner->get_instance_id();
	submission_b.scenario = scenario;
	submission_b.gaussian_data = stage1a_make_submission_test_data(3, 30.0f);
	submission_b.static_chunks.push_back(stage1a_make_submission_test_chunk(1));
	submission_b.desired_renderer_overrides[StringName("lod_bias")] = 0.25f;
	Dictionary submission_b_streaming;
	submission_b_streaming[StringName("prefetch_lookahead_distance")] = 18.0f;
	submission_b.desired_renderer_overrides[StringName("streaming")] = submission_b_streaming;

	CHECK(director->submit_world_submission(submission_a));
	CHECK(director->submit_world_submission(submission_b));

	CHECK(renderer->get_lod_enabled() == baseline_renderer_state.lod_enabled);
	CHECK(renderer->get_lod_bias() == doctest::Approx(0.25f));
	CHECK(renderer->get_frustum_culling() == baseline_renderer_state.frustum_culling);
	CHECK(renderer->get_async_upload_enabled() == baseline_renderer_state.async_upload_enabled);
	CHECK(renderer->get_opacity_multiplier() == doctest::Approx(baseline_renderer_state.opacity_multiplier));
	CHECK(renderer->get_max_splats() == 3);

	const GaussianStreamingSystem::ConfigOverrides streaming_overrides = renderer->get_streaming_config_overrides();
	CHECK(streaming_overrides.override_prefetch == baseline_renderer_state.streaming_overrides.override_prefetch);
	CHECK(streaming_overrides.predictive_prefetch_enabled ==
			baseline_renderer_state.streaming_overrides.predictive_prefetch_enabled);
	CHECK(streaming_overrides.prefetch_lookahead_distance == doctest::Approx(18.0f));
	CHECK(streaming_overrides.override_vram_budget == baseline_renderer_state.streaming_overrides.override_vram_budget);
	CHECK(streaming_overrides.vram_budget_config.budget_mb ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.budget_mb);
	CHECK(streaming_overrides.vram_budget_config.min_chunks ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.min_chunks);
	CHECK(streaming_overrides.vram_budget_config.max_chunks ==
			baseline_renderer_state.streaming_overrides.vram_budget_config.max_chunks);

	director->release_world_submission(owner->get_instance_id());

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][World][SceneTree] World node forwards desired overrides through director submission") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard tier_preset_guard(project_settings, "rendering/gaussian_splatting/quality/tier_preset");
	ProjectSettingGuard tier_apply_guard(project_settings, "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets");
	ProjectSettingGuard predictive_guard(project_settings, "rendering/gaussian_splatting/streaming/predictive_prefetch_enabled");
	ProjectSettingGuard prefetch_guard(project_settings, "rendering/gaussian_splatting/streaming/prefetch_lookahead_distance");
	project_settings->set_setting("rendering/gaussian_splatting/quality/tier_preset", "low");
	project_settings->set_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", true);
	project_settings->set_setting("rendering/gaussian_splatting/streaming/predictive_prefetch_enabled", false);
	project_settings->set_setting("rendering/gaussian_splatting/streaming/prefetch_lookahead_distance", 6.0f);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	Ref<GaussianData> data = stage1a_make_submission_test_data(5, 5.0f);
	world_resource->set_gaussian_data(data);
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);
	Dictionary metadata;
	metadata[StringName("label")] = String("stage1b_world");
	world_resource->set_metadata(metadata);
	world_resource->set_path("res://stage1b_world.gsplatworld");

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	node->set_lod_enabled(false);
	node->set_lod_bias(1.75f);
	node->set_max_render_distance(80.0f);
	node->set_max_splat_count(900000);
	node->set_use_frustum_culling(false);
	node->set_async_upload_enabled(false);
	node->set_opacity(0.35f);

	root->add_child(node);
	tree->process(0.0);
	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot baseline_renderer_state;
	if (renderer.is_valid()) {
		baseline_renderer_state = renderer->snapshot_world_submission_runtime_state();
		CHECK(baseline_renderer_state.valid);
		CHECK_FALSE(baseline_renderer_state.has_active_world_submission);
	}
	node->apply_world();

	GaussianSplatSceneDirector::WorldSubmission submission;
	CHECK(director->get_world_submission(node->get_instance_id(), &submission));
	CHECK(submission.owner_id == node->get_instance_id());
	CHECK(submission.gaussian_data == data);
	CHECK(submission.static_chunks.size() == 1);
	CHECK(submission.metadata[StringName("label")] == String("stage1b_world"));
	CHECK(submission.metadata[StringName("world_path")] == String("res://stage1b_world.gsplatworld"));
	CHECK(submission.has_desired_residency_hint);
	CHECK(submission.desired_residency_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
	CHECK_FALSE((bool)submission.desired_renderer_overrides[StringName("lod_enabled")]);
	CHECK(float(submission.desired_renderer_overrides[StringName("lod_bias")]) == doctest::Approx(1.75f));
	CHECK(float(submission.desired_renderer_overrides[StringName("lod_max_distance")]) == doctest::Approx(80.0f));
	CHECK(int64_t(submission.desired_renderer_overrides[StringName("max_splats")]) == 300000);
	CHECK_FALSE((bool)submission.desired_renderer_overrides[StringName("frustum_culling")]);
	CHECK_FALSE((bool)submission.desired_renderer_overrides[StringName("async_upload_enabled")]);
	CHECK(float(submission.desired_renderer_overrides[StringName("opacity_multiplier")]) == doctest::Approx(0.35f));

	const Dictionary streaming_overrides = submission.desired_renderer_overrides[StringName("streaming")];
	CHECK((bool)streaming_overrides[StringName("override_prefetch")]);
	CHECK_FALSE((bool)streaming_overrides[StringName("predictive_prefetch_enabled")]);
	CHECK(float(streaming_overrides[StringName("prefetch_lookahead_distance")]) == doctest::Approx(12.0f));
	CHECK((bool)streaming_overrides[StringName("override_vram_budget")]);
	CHECK(int64_t(streaming_overrides[StringName("vram_budget_mb")]) == 256);
	CHECK(int64_t(streaming_overrides[StringName("vram_min_chunks")]) == 2);
	CHECK(int64_t(streaming_overrides[StringName("vram_max_chunks")]) == 32);
	CHECK((bool)streaming_overrides[StringName("override_io_source")]);
	// io_source_path was removed: only the override_io_source flag is published now. Any string
	// payload is dropped by the director because no runtime consumer reads it.
	CHECK(!streaming_overrides.has(StringName("io_source_path")));

	if (renderer.is_valid()) {
		int32_t residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
		String residency_source;
		CHECK(director->get_submission_residency_hint_for_renderer(renderer.ptr(), &residency_hint, &residency_source));
		CHECK(residency_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
		CHECK(residency_source == String("world_submission"));
		int32_t renderer_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
		String renderer_residency_source;
		CHECK(renderer->get_submission_residency_hint(&renderer_residency_hint, &renderer_residency_source));
		CHECK(renderer_residency_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
		CHECK(renderer_residency_source == String("world_submission"));
		String backend_reason;
		CHECK_FALSE(renderer->should_prefer_resident_backend(gs::settings::GS_ROUTE_STREAMING, &backend_reason));
		CHECK(backend_reason == String("submission_hint_streaming:world_submission"));
		CHECK(renderer->get_gaussian_data() == data);
		CHECK(renderer->get_static_chunks().size() == 1);
		CHECK_FALSE(renderer->get_lod_enabled());
		CHECK(renderer->get_lod_bias() == doctest::Approx(1.75f));
		CHECK(renderer->get_lod_max_distance() == doctest::Approx(80.0f));
		CHECK(renderer->get_max_splats() == 5);
		CHECK_FALSE(renderer->get_frustum_culling());
		CHECK_FALSE(renderer->get_async_upload_enabled());
		CHECK(renderer->get_opacity_multiplier() == doctest::Approx(0.35f));
	}

	node->clear_world();
	CHECK_FALSE(director->get_world_submission(node->get_instance_id(), &submission));
	if (renderer.is_valid()) {
		const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot restored_renderer_state =
				renderer->snapshot_world_submission_runtime_state();
		CHECK(restored_renderer_state.valid);
		CHECK(restored_renderer_state.gaussian_data == baseline_renderer_state.gaussian_data);
		CHECK(restored_renderer_state.static_chunks.size() == baseline_renderer_state.static_chunks.size());
		CHECK(restored_renderer_state.lod_enabled == baseline_renderer_state.lod_enabled);
		CHECK(restored_renderer_state.lod_bias == doctest::Approx(baseline_renderer_state.lod_bias));
		CHECK(restored_renderer_state.lod_max_distance == doctest::Approx(baseline_renderer_state.lod_max_distance));
		CHECK(restored_renderer_state.frustum_culling == baseline_renderer_state.frustum_culling);
		CHECK(restored_renderer_state.async_upload_enabled == baseline_renderer_state.async_upload_enabled);
		CHECK(restored_renderer_state.opacity_multiplier == doctest::Approx(baseline_renderer_state.opacity_multiplier));
		CHECK(restored_renderer_state.max_splats == baseline_renderer_state.max_splats);
		CHECK(restored_renderer_state.streaming_overrides.override_prefetch == baseline_renderer_state.streaming_overrides.override_prefetch);
		CHECK(restored_renderer_state.streaming_overrides.predictive_prefetch_enabled ==
				baseline_renderer_state.streaming_overrides.predictive_prefetch_enabled);
		CHECK(restored_renderer_state.streaming_overrides.prefetch_lookahead_distance ==
				doctest::Approx(baseline_renderer_state.streaming_overrides.prefetch_lookahead_distance));
		CHECK(restored_renderer_state.streaming_overrides.override_vram_budget ==
				baseline_renderer_state.streaming_overrides.override_vram_budget);
		CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.budget_mb ==
				baseline_renderer_state.streaming_overrides.vram_budget_config.budget_mb);
		CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.min_chunks ==
				baseline_renderer_state.streaming_overrides.vram_budget_config.min_chunks);
		CHECK(restored_renderer_state.streaming_overrides.vram_budget_config.max_chunks ==
				baseline_renderer_state.streaming_overrides.vram_budget_config.max_chunks);
		CHECK(restored_renderer_state.streaming_overrides.override_io_source ==
				baseline_renderer_state.streaming_overrides.override_io_source);
		CHECK(restored_renderer_state.has_active_world_submission == baseline_renderer_state.has_active_world_submission);
		CHECK(restored_renderer_state.has_desired_residency_hint == baseline_renderer_state.has_desired_residency_hint);
		CHECK(restored_renderer_state.desired_residency_hint == baseline_renderer_state.desired_residency_hint);
	}
	if (renderer.is_valid()) {
		int32_t renderer_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
		String renderer_residency_source;
		CHECK_FALSE(renderer->get_submission_residency_hint(&renderer_residency_hint, &renderer_residency_source));
		CHECK(renderer_residency_source == String("none"));
	}

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][World][SceneTree] strict identity transform rejects non-identity world submissions") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard strict_identity_guard(project_settings,
			"rendering/gaussian_splatting/world/strict_identity_transform");
	project_settings->set_setting("rendering/gaussian_splatting/world/strict_identity_transform", true);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(2, 3.0f));

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	node->set_transform(Transform3D(Basis(), Vector3(1.0f, 0.0f, 0.0f)));

	root->add_child(node);
	tree->process(0.0);

	GaussianSplatSceneDirector::WorldSubmission submission;
	node->apply_world();
	CHECK_FALSE(director->get_world_submission(node->get_instance_id(), &submission));

	node->set_transform(Transform3D());
	node->apply_world();
	CHECK(director->get_world_submission(node->get_instance_id(), &submission));

	node->clear_world();
	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

// Regression test for #517: GaussianSplatWorld3D::NOTIFICATION_EXIT_TREE
// unregisters the world submission and frees render_instance/gaussian_base,
// but NOTIFICATION_READY only fires once per node lifetime. Reparenting a
// node is exit+enter, so before the fix a re-added world node stayed dark
// until something explicitly called apply_world() or a property setter.
// Mirrors the GaussianSplatNode3D analog above ("Shared renderer survives
// temporary last-instance unregister") but asserts on the world-submission
// record directly, since world submissions are director bookkeeping that
// does not require a real GPU renderer to exist (see the ownership-gate
// tests above).
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] World submission survives tree re-entry after reparent") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(6, 2.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	REQUIRE(world_node != nullptr);
	// Default auto_apply_on_ready (true): the first tree-entry applies the
	// world via NOTIFICATION_READY, matching the common configuration and
	// keeping this test independent of the auto_apply_on_ready gate, which
	// only governs first-entry behavior (see gaussian_splat_world_3d.cpp's
	// NOTIFICATION_ENTER_TREE comment).
	world_node->set_world(world_resource);

	root->add_child(world_node);
	tree->process(0.0);

	const ObjectID owner_id = world_node->get_instance_id();
	GaussianSplatSceneDirector::WorldSubmission submission;
	REQUIRE_MESSAGE(director->get_world_submission(owner_id, &submission),
			"auto_apply_on_ready should have registered the world submission on first READY.");

	// Reparenting (or any tree removal + re-add) is exit+enter. Confirm
	// EXIT_TREE actually tears the submission down, so the re-entry check
	// below exercises the fix rather than a no-op.
	root->remove_child(world_node);
	tree->process(0.0);
	CHECK_FALSE_MESSAGE(director->get_world_submission(owner_id, &submission),
			"NOTIFICATION_EXIT_TREE must unregister the world submission.");

	// Re-entry: NOTIFICATION_READY does NOT fire again. Before the #517 fix
	// the node stayed dark here until apply_world() or a setter ran.
	root->add_child(world_node);
	tree->process(0.0);
	CHECK_MESSAGE(director->get_world_submission(owner_id, &submission),
			"NOTIFICATION_ENTER_TREE must restore a previously-active world submission on re-entry.");
	CHECK(submission.owner_id == owner_id);
	CHECK(submission.gaussian_data == world_resource->get_gaussian_data());

	world_node->clear_world();
	root->remove_child(world_node);
	memdelete(world_node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Zero-splat submissions do not surface residency authority") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the zero-splat residency-authority test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(owner);
		memdelete(owner);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = owner->get_instance_id();
	submission.scenario = world->get_scenario();
	submission.gaussian_data = stage1a_make_submission_test_data(0);
	submission.has_desired_residency_hint = true;
	submission.desired_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;

	CHECK(director->submit_world_submission(submission));
	CHECK_FALSE(director->has_world_submission_for_renderer(renderer.ptr()));

	int32_t renderer_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	String renderer_hint_source;
	CHECK_FALSE(renderer->get_submission_residency_hint(&renderer_hint, &renderer_hint_source));
	CHECK(renderer_hint_source == String("none"));

	int32_t director_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	String director_hint_source;
	CHECK_FALSE(director->get_submission_residency_hint_for_renderer(renderer.ptr(), &director_hint, &director_hint_source));
	CHECK(director_hint_source == String("none"));

	String backend_reason;
	CHECK_FALSE(renderer->should_prefer_resident_backend(gs::settings::GS_ROUTE_STREAMING, &backend_reason));
	CHECK(backend_reason == String("requested_streaming_policy"));

	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot runtime_state =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(runtime_state.valid);
	CHECK_FALSE(runtime_state.has_active_world_submission);
	CHECK_FALSE(runtime_state.has_desired_residency_hint);

	director->release_world_submission(owner->get_instance_id());

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Staged world submissions mark streaming path ownership in the backend plan") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	const String route_policy_setting = "rendering/gaussian_splatting/streaming/route_policy";
	const bool had_route_policy = project_settings->has_setting(route_policy_setting);
	const Variant previous_route_policy = had_route_policy ? project_settings->get_setting(route_policy_setting) : Variant();
	project_settings->set_setting(route_policy_setting, int64_t(gs::settings::GS_ROUTE_STREAMING));
	project_settings->emit_signal("settings_changed");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the staged-world backend-plan test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(owner);
		memdelete(owner);
		tree->process(0.0);
		if (had_route_policy) {
			project_settings->set_setting(route_policy_setting, previous_route_policy);
		} else {
			project_settings->clear(route_policy_setting);
		}
		project_settings->emit_signal("settings_changed");
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = owner->get_instance_id();
	submission.scenario = world->get_scenario();
	submission.gaussian_data = stage1a_make_submission_test_data(32, 16.0f);
	submission.static_chunks = Vector<GaussianSplatRenderer::StaticChunk>();
	submission.has_desired_residency_hint = true;
	submission.desired_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;

	CHECK(director->submit_world_submission(submission));
	CHECK(director->has_world_submission_for_renderer(renderer.ptr()));

	const GaussianSplatRenderer::FrameBackendPlan backend_plan = renderer->build_frame_backend_plan(false);
	CHECK(backend_plan.streaming_requested);
	CHECK(backend_plan.has_active_world_submission);

	director->release_world_submission(owner->get_instance_id());

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (had_route_policy) {
		project_settings->set_setting(route_policy_setting, previous_route_policy);
	} else {
		project_settings->clear(route_policy_setting);
	}
	project_settings->emit_signal("settings_changed");

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] World submission renders through the resident instanced route without a streaming system") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		MESSAGE("Skipping test - Rendering server unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard route_guard(project_settings, "rendering/gaussian_splatting/streaming/route_policy");
	ProjectSettingGuard instance_guard(project_settings, "rendering/gaussian_splatting/instance_pipeline/enabled");
	project_settings->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			int64_t(gs::settings::GS_ROUTE_STREAMING));
	project_settings->set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true);
	project_settings->emit_signal("settings_changed");

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	Ref<GaussianData> data = stage1a_make_submission_test_data(32, 15.0f);
	world_resource->set_gaussian_data(data);
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		MESSAGE("Skipping test - renderer unavailable");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		return;
	}

	// The route policy above says STREAMING. GaussianSplatWorld3D mirrors that into
	// its residency hint, so as written this case could never reach the resident
	// backend its name and its assertions describe -- should_prefer_resident_backend
	// would return false with `submission_hint_streaming:world_submission`. Re-pin
	// the submission's hint to RESIDENT so the case tests what it claims: a world
	// submission whose hint overrides a streaming project policy.
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	if (director == nullptr) {
		FAIL("SceneDirector singleton required to re-pin the world submission's residency hint.");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		return;
	}
	if (!stage1a_repin_world_submission_as_resident(director, node)) {
		FAIL("Failed to re-publish the world submission with a RESIDENT residency hint.");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		return;
	}
	{
		String backend_reason;
		CHECK(renderer->should_prefer_resident_backend(gs::settings::GS_ROUTE_STREAMING, &backend_reason));
		CHECK(backend_reason == String("submission_hint_resident:world_submission"));
	}

	renderer->get_debug_state().show_performance_hud = true;
	renderer->test_release_current_streaming_system();
	CHECK_FALSE(renderer->test_has_current_streaming_system());

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	renderer->render_scene_instance(&render_data);

	CHECK_FALSE(renderer->test_has_current_streaming_system());
	CHECK(renderer->has_instance_pipeline_buffers());
	CHECK(renderer->has_instance_asset_remap());

	const Dictionary stats = renderer->get_render_stats();
	CHECK(stats.get("route_uid", String()) == String("INSTANCE.RESIDENT"));
	CHECK(stats.get("requested_route_policy", String()) == String("streaming"));
	CHECK(stats.get("instance_backend_policy", String()) == String("resident"));
	CHECK(stats.get("backend_selection_reason", String()) == String("submission_hint_resident:world_submission"));
	CHECK(bool(stats.get("instance_contract_ready", false)));
	CHECK(stats.get("data_source", String()) == String("ResidentInstanceAtlas"));

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);
}

TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] Resident rejection preserves resident diagnostics and skips streaming pivot") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		MESSAGE("Skipping test - Rendering server unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard route_guard(project_settings, "rendering/gaussian_splatting/streaming/route_policy");
	ProjectSettingGuard instance_guard(project_settings, "rendering/gaussian_splatting/instance_pipeline/enabled");
	project_settings->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			int64_t(gs::settings::GS_ROUTE_STREAMING));
	project_settings->set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true);
	project_settings->emit_signal("settings_changed");

	const QuantizationConfig saved_quantization_config = g_quantization_config;
	g_quantization_config.per_chunk_quantization = true;
	g_quantization_config.position_bits = 16;
	g_quantization_config.scale_bits = 12;
	g_quantization_config.quantize_scales = false;

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	Ref<GaussianData> data = stage1a_make_submission_test_data(32, 20.0f);
	world_resource->set_gaussian_data(data);
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		MESSAGE("Skipping test - renderer unavailable");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		g_quantization_config = saved_quantization_config;
		return;
	}

	// Same correction as the resident-instanced-route case above: the STREAMING
	// route policy this test sets makes GaussianSplatWorld3D publish a STREAMING
	// residency hint, which should_prefer_resident_backend can only answer with
	// `submission_hint_streaming:world_submission`. Re-pin the submission's hint so
	// the resident backend is actually selected and the resident-diagnostics
	// assertions below have a resident decision to observe. (#685)
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	if (director == nullptr) {
		FAIL("SceneDirector singleton required to re-pin the world submission's residency hint.");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		g_quantization_config = saved_quantization_config;
		return;
	}
	if (!stage1a_repin_world_submission_as_resident(director, node)) {
		FAIL("Failed to re-publish the world submission with a RESIDENT residency hint.");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		g_quantization_config = saved_quantization_config;
		return;
	}
	{
		String backend_reason;
		CHECK(renderer->should_prefer_resident_backend(gs::settings::GS_ROUTE_STREAMING, &backend_reason));
		CHECK(backend_reason == String("submission_hint_resident:world_submission"));
	}

	renderer->get_debug_state().show_performance_hud = true;
	renderer->test_release_current_streaming_system();
	CHECK_FALSE(renderer->test_has_current_streaming_system());

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	renderer->render_scene_instance(&render_data);

		// GS-PERF-Q80B: the resident atlas now publishes the quantized contract instead of
		// rejecting it, so the resident-hinted world submission must succeed on the resident
		// backend (no COMMON_SKIP_RESIDENT_NOT_FEASIBLE, no resident_quantization_unsupported).
		const Dictionary stats = renderer->get_render_stats();
		const String route_uid = stats.get("route_uid", String());
		CHECK_FALSE(route_uid.begins_with(String(RenderRouteUID::COMMON_SKIP_RESIDENT_NOT_FEASIBLE)));
		CHECK(stats.get("requested_route_policy", String()) == String("streaming"));
		CHECK(stats.get("instance_backend_policy", String()) == String("resident"));
		CHECK(String(stats.get("backend_selection_reason", String())).find(
				"resident_quantization_unsupported") == -1);
		CHECK(renderer->has_instance_pipeline_buffers());
		CHECK(renderer->is_instance_contract_ready());

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);
	g_quantization_config = saved_quantization_config;
}

TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] Explicit resident quantization rejection falls back to the legacy resident path") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		MESSAGE("Skipping test - Rendering server unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard route_guard(project_settings, "rendering/gaussian_splatting/streaming/route_policy");
	ProjectSettingGuard instance_guard(project_settings, "rendering/gaussian_splatting/instance_pipeline/enabled");
	project_settings->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			int64_t(gs::settings::GS_ROUTE_RESIDENT));
	project_settings->set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true);
	project_settings->emit_signal("settings_changed");

	const QuantizationConfig saved_quantization_config = g_quantization_config;
	g_quantization_config.per_chunk_quantization = true;
	g_quantization_config.position_bits = 16;
	g_quantization_config.scale_bits = 12;
	g_quantization_config.quantize_scales = false;

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	// #719: the static chunk below indexes splat 0, whose chunk bounds are the origin AABB
	// (stage1a_make_submission_test_chunk). The camera sits at (0,0,5) looking down -Z, so the
	// atlas splat must be inside that frustum to produce rendered output. The previous 20.0f
	// x-offset placed splat 0 at x=20 -- consistent with the origin chunk bounds for coarse cull,
	// but far outside the camera frustum, so depth_compute per-splat-culled it and the resident
	// quantized atlas rendered nothing (element_count=0). That was the sole reason
	// has_rendered_content() and get_visible_splat_count() > 0 failed here (measured on an
	// RTX 3090); the published contract itself is correct. Offset 0.0f keeps splat 0 at the origin,
	// consistent with the chunk bounds and in-frustum, so the resident quantized path renders.
	Ref<GaussianData> data = stage1a_make_submission_test_data(32, 0.0f);
	world_resource->set_gaussian_data(data);
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		MESSAGE("Skipping test - renderer unavailable");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		g_quantization_config = saved_quantization_config;
		return;
	}

	renderer->get_debug_state().show_performance_hud = true;
	renderer->test_release_current_streaming_system();
	CHECK_FALSE(renderer->test_has_current_streaming_system());

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	renderer->render_scene_instance(&render_data);

	CHECK_FALSE(renderer->test_has_current_streaming_system());
	// GS-PERF-Q80B: under a resident route policy, quantized resident data now publishes the
	// resident instance contract (no legacy-path fallback) and renders.
	CHECK(renderer->has_instance_pipeline_buffers());
	CHECK(renderer->get_instance_backend_policy() == GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT);
	CHECK(renderer->is_instance_contract_ready());
	CHECK(renderer->has_rendered_content());
	CHECK(renderer->get_visible_splat_count() > 0);

	const Dictionary stats = renderer->get_render_stats();
	CHECK(stats.get("requested_route_policy", String()) == String("resident"));
	CHECK(stats.get("instance_backend_policy", String()) == String("resident"));
	CHECK(stats.get("backend_selection_reason", String()) == String("requested_resident_policy"));
	CHECK(String(stats.get("backend_selection_reason", String())).find("resident_quantization_unsupported") == -1);

	// Direct assertions stop at "resident was requested, no streaming system was used, and no
	// resident instance contract/remap survived publication." The current renderer diagnostics do
	// not expose a dedicated legacy-resident route token, so the final legacy-resident path is
	// proven indirectly by the successful render under those conditions.

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);
	g_quantization_config = saved_quantization_config;
}

// #980 -- a runtime sorting-config reload (or any other sorter rebuild) must keep the resident
// instance route rendering, and the published instance contract must be told about it.
//
// DEFECT (reproduced on an RTX 3090 / Vulkan at 7dd6f9e6606 with a real-scan asset under a
// GaussianSplatNode3D: 201 identical failures in the 200 frames after the call, visible_splats
// 0 for the rest of the session). GaussianSplatRenderer::reload_gpu_sorting_config_from_
// project_settings() marks the sorter dirty and calls refresh_gpu_sorter(); GPUSortingPipeline::
// rebuild_sorter() then frees and reallocates the pipeline-owned sort buffers (manage_buffers),
// but the resident instance contract keeps COPIES of the old RIDs (the resident publisher takes
// them from get_buffer_handles() at publish time) and the per-frame sort stage copies the
// contract into the pipeline's instance inputs. The instance depth pass then binds a freed
// buffer at binding 6 on every frame ("Storage buffer supplied (binding: 6) is invalid"), the
// sort route reports COMMON.SKIP.NO_VISIBLE and nothing renders. The resident publisher never
// republishes: its skip condition sees an unchanged source generation and freed-but-nonzero
// RIDs. The same funnel serves set_max_splats(), set_quality_preset() (refresh deferred to the
// next sort), the forced-algorithm override and the sort benchmarks, so reload is only the
// cheapest trigger.
//
// WHAT IT ASSERTS, on the same resident fixture as the quantization case above (proven to render
// on a real device):
//   1. a healthy control frame, and the contract's sort buffers equal the pipeline's handles;
//   2. after reload with NO setting changed, FIVE consecutive frames each render with visible
//      splats and a non-skipped sort route -- several frames, so a fix that survives one frame
//      and decays still fails;
//   3. the pipeline's sort buffers differ from the pre-reload ones (the reload really
//      reallocated: it moves the pipeline from the publisher's owned buffers to the GPU buffer
//      manager's), the contract's sort RIDs equal the pipeline's CURRENT handles, and the
//      instance-pipeline content
//      generation MOVED. Visible splats prove the depth pass got good RIDs; the generation
//      proves the record became honest. A fix that patches the RIDs but leaves the
//      change-detector stable passes the first and fails the second;
//   4. the same for the DEFERRED trigger set_quality_preset(): its rebuild is serviced before the
//      instance route is gated on it (render_sorting_orchestrator.cpp, sort_gaussians_for_view),
//      observable as the pipeline's sorter capacity reaching the preset's max_splats. Before this
//      fix the flag was serviced only below the available_splats == 0 exit, which the resident
//      route always takes, so the instance route stayed gated and the sort reported
//      COMMON.SKIP.NO_VISIBLE for the rest of the session -- a second, pre-existing hole;
//   5. (#984 review round 1) the fixture publishes a requirement of 250,001, ONE above the
//      performance preset's budget; after every trigger (and after set_max_splats(8)) the sorter,
//      the buffers and the contract must still be sized for the PUBLISHED requirement and the
//      frame must show the same splats as the control -- the refresh sizes from the requirement,
//      not from the single-instance budget, so the republish never clamps the contract.
// Pre-fix: phase 2 fails on the first post-reload frame (visible_splats=0,
// sort_route_uid=COMMON.SKIP.NO_VISIBLE). With the republish deleted the same, plus the named
// stale_sort_buffer_handles diagnostic in the log; with the generation move deleted, phase 3's
// generation assertion fails alone.
TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] Runtime sorting-config reload keeps the resident instance route rendering and republishes the sort buffers (#980)") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (tree == nullptr || tree->get_root() == nullptr) {
		FAIL("SceneTree unavailable in a [SceneTree] case - the harness is required to provide one");
		return;
	}
	Window *root = tree->get_root();

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings == nullptr) {
		FAIL("ProjectSettings unavailable - the engine bootstrap always provides one");
		return;
	}
	ProjectSettingGuard route_guard(project_settings, "rendering/gaussian_splatting/streaming/route_policy");
	ProjectSettingGuard instance_guard(project_settings, "rendering/gaussian_splatting/instance_pipeline/enabled");
	project_settings->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			int64_t(gs::settings::GS_ROUTE_RESIDENT));
	project_settings->set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true);
	project_settings->emit_signal("settings_changed");

	// Same fixture as the quantization case above: it is the resident configuration proven to
	// produce rendered output under --gs-gpu-test (#719/#745).
	const QuantizationConfig saved_quantization_config = g_quantization_config;
	g_quantization_config.per_chunk_quantization = true;
	g_quantization_config.position_bits = 16;
	g_quantization_config.scale_bits = 12;
	g_quantization_config.quantize_scales = false;

	constexpr uint32_t FIXTURE_SPLATS = 250001u;
	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	Ref<GaussianData> data = stage1a_make_submission_test_data(int(FIXTURE_SPLATS), 0.0f);
	world_resource->set_gaussian_data(data);
	// One chunk indexing ALL splats (x = 0..FIXTURE_SPLATS-1): the published sort requirement
	// becomes FIXTURE_SPLATS (1 instance x 1 chunk x that many chunk splats) -- ONE above the
	// 250,000 budget set_quality_preset("performance") applies, which is exactly the review's
	// scenario: the publisher records its generation first, the deferred refresh must not then
	// rebuild the sorter below the requirement. Only the splats near the origin are in the
	// frustum (camera at z=5, 70 degrees).
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	GaussianSplatRenderer::StaticChunk full_chunk;
	full_chunk.bounds = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(float(FIXTURE_SPLATS), 1.0f, 1.0f));
	full_chunk.center = full_chunk.bounds.get_center();
	full_chunk.radius = full_chunk.bounds.size.length() * 0.5f;
	for (uint32_t i = 0; i < FIXTURE_SPLATS; i++) {
		full_chunk.indices.push_back(i);
	}
	chunks.push_back(full_chunk);
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	auto teardown = [&]() {
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		g_quantization_config = saved_quantization_config;
	};

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		FAIL("Premise failed: the world node has no renderer under a real RenderingDevice");
		teardown();
		return;
	}
	renderer->test_release_current_streaming_system();

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	// ---- Phase 1: healthy control. ----
	renderer->render_scene_instance(&render_data);
	if (!renderer->has_instance_pipeline_buffers() ||
			renderer->get_instance_backend_policy() != GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT ||
			!renderer->has_rendered_content() || renderer->get_visible_splat_count() == 0) {
		FAIL("Premise failed: the resident fixture did not render before any trigger (has_buffers=",
				renderer->has_instance_pipeline_buffers(), " resident=",
				renderer->get_instance_backend_policy() == GaussianRenderPipeline::InstanceBackendPolicy::RESIDENT,
				" rendered=", renderer->has_rendered_content(), " visible=", renderer->get_visible_splat_count(),
				"); the fixture, not the reload path, is at fault");
		teardown();
		return;
	}
	Ref<GPUSortingPipeline> sorting_pipeline = renderer->get_subsystem_state().sorting_pipeline;
	if (sorting_pipeline.is_null()) {
		FAIL("Premise failed: no sorting pipeline on the resident renderer");
		teardown();
		return;
	}
	auto contract_matches_pipeline = [&]() {
		const GaussianRenderPipeline::InstancePipelineBuffers &published = renderer->get_instance_pipeline_buffers();
		const SortBufferHandles handles = sorting_pipeline->get_buffer_handles();
		return handles.valid && published.sort_key_buffer == handles.keys_buffer &&
				published.sort_value_buffer == handles.indices_buffer;
	};
	CHECK_MESSAGE(contract_matches_pipeline(), "Premise: the published contract and the pipeline agree on the sort buffers before any trigger");
	auto republish_count = [&]() -> int64_t {
		return int64_t(renderer->get_render_stats().get("instance_sort_buffer_republishes", int64_t(-1)));
	};
	auto dump_state = [&](const String &p_label) {
		const GaussianRenderPipeline::InstancePipelineBuffers &published = renderer->get_instance_pipeline_buffers();
		const SortBufferHandles handles = sorting_pipeline->get_buffer_handles();
		MESSAGE("[#980 state] ", p_label, ": pipeline keys=", handles.keys_buffer.get_id(), " values=", handles.indices_buffer.get_id(),
				" capacity=", handles.capacity, " | contract keys=", published.sort_key_buffer.get_id(), " values=",
				published.sort_value_buffer.get_id(), " max_visible_splats=", published.max_visible_splats,
				" | content_generation=", renderer->get_instance_pipeline_content_generation(), " republishes=", republish_count(),
				" sorter_capacity=", sorting_pipeline->get_max_elements(), " manager_capacity=",
				(renderer->get_resource_state().buffer_manager.is_valid() ? renderer->get_resource_state().buffer_manager->get_buffer_capacity() : 0u),
				" buffer_manager_initialized=", renderer->get_resource_state().buffer_manager_initialized,
				" visible=", renderer->get_visible_splat_count(), " max_splats=", renderer->get_max_splats());
	};
	dump_state("after the healthy control frame");
	const uint32_t visible_control = renderer->get_visible_splat_count();
	const uint32_t published_requirement = renderer->get_instance_pipeline_buffers().max_visible_splats;
	if (published_requirement <= 250000u) {
		FAIL("Premise failed: the fixture did not publish a sort requirement above the performance preset's budget of 250,000 (got ",
				published_requirement, "); the review scenario could not be provoked");
		teardown();
		return;
	}

	// Drives p_frames frames after a trigger and checks every one of them, then the record.
	// p_expect_reallocation: the trigger must leave the pipeline on DIFFERENT sort buffers (the
	// reload does: it moves the pipeline from the publisher's owned buffers to the GPU buffer
	// manager's). p_min_sorter_capacity_after: the trigger must leave the pipeline's sorter at
	// least this large (the deferred preset does: max_splats 250000). Two different observables
	// because the resident publisher may itself republish within a frame and the buffer
	// manager's external buffers are re-adopted after a rebuild, so end-state RIDs alone cannot
	// tell a serviced rebuild from an unserviced one; the sorter capacity can.
	// #984 review round 1: after ANY trigger the instance route must still be sized for the
	// PUBLISHED requirement -- sorter, buffers and the contract itself -- and show the same
	// splats as the control. The refresh used to size the sorter from the single-instance
	// max_splats budget; when that is below the requirement the republish clamped the contract
	// and the resident publisher's fast path kept it there.
	auto check_requirement_held = [&](const String &p_trigger) {
		const GaussianRenderPipeline::InstancePipelineBuffers &published = renderer->get_instance_pipeline_buffers();
		const SortBufferHandles handles = sorting_pipeline->get_buffer_handles();
		INFO("#984 REVIEW REGRESSION: after ", p_trigger, " the instance route dropped below its published requirement (sorter capacity ",
				sorting_pipeline->get_max_elements(), ", buffers ", handles.capacity, ", contract max_visible_splats ",
				published.max_visible_splats, ", requirement ", published_requirement, ", visible ", renderer->get_visible_splat_count(),
				" vs control ", visible_control, "): the refresh sized the sorter from the single-instance budget and the republish clamped the contract");
		CHECK(sorting_pipeline->get_max_elements() >= published_requirement);
		CHECK(handles.capacity >= published_requirement);
		CHECK(published.max_visible_splats == published_requirement);
		CHECK(renderer->get_visible_splat_count() == visible_control);
	};
	auto drive_and_check = [&](const String &p_trigger, int p_frames, const RID &p_pipeline_keys_before, uint64_t p_generation_before,
									int64_t p_republishes_before, bool p_expect_reallocation, uint32_t p_min_sorter_capacity_after) {
		for (int frame = 1; frame <= p_frames; frame++) {
			renderer->render_scene_instance(&render_data);
			const uint32_t visible = renderer->get_visible_splat_count();
			const Dictionary stats = renderer->get_render_stats();
			const String sort_route = stats.get("sort_route_uid", String());
			const bool rendered = renderer->has_rendered_content() && visible > 0 &&
					sort_route != String("COMMON.SKIP.NO_VISIBLE");
			INFO("#980 REGRESSION: frame ", frame, " after ", p_trigger, " did not render (visible_splats=", visible,
					" sort_route_uid=", sort_route, " has_rendered_content=", renderer->has_rendered_content(),
					" visible_after_culling=", int64_t(stats.get("visible_after_culling", int64_t(-1))),
					" cull_route_uid=", String(stats.get("cull_route_uid", String())),
					" stage_cull_reason=", String(stats.get("stage_cull_reason", String())),
					"): with a stale contract the instance depth pass binds sort buffers the sorter rebuild freed");
			CHECK(rendered);
		}
		const GaussianRenderPipeline::InstancePipelineBuffers &published = renderer->get_instance_pipeline_buffers();
		const SortBufferHandles handles = sorting_pipeline->get_buffer_handles();
		{
			INFO("After ", p_trigger, " the published contract's sort buffers are not the pipeline's current buffers (contract keys=",
					published.sort_key_buffer.get_id(), " pipeline keys=", handles.keys_buffer.get_id(),
					" contract values=", published.sort_value_buffer.get_id(), " pipeline values=", handles.indices_buffer.get_id(),
					"): the record is stale");
			CHECK(handles.valid);
			CHECK(published.sort_key_buffer == handles.keys_buffer);
			CHECK(published.sort_value_buffer == handles.indices_buffer);
		}
		if (p_expect_reallocation) {
			INFO("Premise: ", p_trigger, " did not reallocate the pipeline's sort buffers (keys ", p_pipeline_keys_before.get_id(),
					" -> ", handles.keys_buffer.get_id(), "), so this phase exercised nothing");
			CHECK(handles.keys_buffer != p_pipeline_keys_before);
		}
		if (p_min_sorter_capacity_after > 0) {
			INFO("Premise: ", p_trigger, " did not get its sorter rebuild serviced (pipeline sorter capacity ",
					sorting_pipeline->get_max_elements(), " < ", p_min_sorter_capacity_after,
					"): the deferred rebuild went around the funnel instead of through it");
			CHECK(sorting_pipeline->get_max_elements() >= p_min_sorter_capacity_after);
		}
		{
			// The record's change-detector must move exactly when the record was republished:
			// the reload republishes exactly once (its RIDs change); the deferred trigger
			// republishes whenever the rebuild left the pipeline on different buffers than the
			// contract held at that moment (the resident publisher can itself republish within
			// the frame, so the count is what pairs with the generation, not end-state RIDs).
			const uint64_t generation_after = renderer->get_instance_pipeline_content_generation();
			const int64_t republishes_after = republish_count();
			INFO("After ", p_trigger, " the instance-pipeline content generation did not follow the record (republishes ",
					p_republishes_before, " -> ", republishes_after, ", generation ", p_generation_before, " -> ", generation_after,
					"): the contract's sort buffers were republished but its change-detector says nothing changed, or the reverse");
			// One direction only: the resident publisher writes the same generation on its own
			// republishes, so "generation moved" does not imply "this fix republished".
			if (republishes_after > p_republishes_before) {
				CHECK(generation_after != p_generation_before);
			}
			if (p_expect_reallocation) {
				INFO("The reload must republish the contract exactly once (republishes ", p_republishes_before, " -> ", republishes_after, ")");
				CHECK(republishes_after == p_republishes_before + 1);
			}
		}
	};

	// ---- Phase 2: the reload, with NO setting changed. ----
	const RID keys_before_reload = sorting_pipeline->get_buffer_handles().keys_buffer;
	const uint64_t generation_before_reload = renderer->get_instance_pipeline_content_generation();
	const int64_t republishes_before_reload = republish_count();
	CHECK_MESSAGE(republishes_before_reload == 0, "Premise: no contract republish happened before the first trigger");
	renderer->reload_gpu_sorting_config_from_project_settings();
	dump_state("right after reload (before any frame)");
	drive_and_check(String("reload_gpu_sorting_config_from_project_settings()"), 5, keys_before_reload, generation_before_reload,
			republishes_before_reload, true, 0u);
	check_requirement_held(String("reload_gpu_sorting_config_from_project_settings()"));
	dump_state("after 5 frames post-reload");

	// ---- Phase 3: the DEFERRED trigger; its rebuild runs on the next sort. ----
	// set_quality_preset("performance") also raises the LOD bias and switches culling
	// knobs, which on this 32-splat fixture cull everything by themselves. Those knobs are
	// restored right after the call so the only effect left is the one under test: the
	// sorter rebuild it armed (sorter_needs_rebuild), which lands in refresh_gpu_sorter()
	// from sort_gaussians_for_view() on the next frame -- inside the funnel, after this
	// frame's stage copy of the contract.
	const RID keys_before_preset = sorting_pipeline->get_buffer_handles().keys_buffer;
	const uint64_t generation_before_preset = renderer->get_instance_pipeline_content_generation();
	const int64_t republishes_before_preset = republish_count();
	const bool lod_enabled_before = renderer->get_lod_enabled();
	const float lod_bias_before = renderer->get_lod_bias();
	const bool frustum_culling_before = renderer->get_frustum_culling();
	renderer->set_quality_preset("performance");
	renderer->set_lod_enabled(lod_enabled_before);
	renderer->set_lod_bias(lod_bias_before);
	renderer->set_frustum_culling(frustum_culling_before);
	dump_state("right after set_quality_preset (before any frame)");
	drive_and_check(String("set_quality_preset(\"performance\") (refresh deferred to the next sort)"), 5, keys_before_preset,
			generation_before_preset, republishes_before_preset, false, 250000u);
	check_requirement_held(String("set_quality_preset(\"performance\") (budget 250,000 below the published requirement)"));
	dump_state("after 5 frames post-preset");

	// ---- Phase 4 (#984 review round 1): a refresh must never rebuild below the PUBLISHED requirement. ----
	// set_max_splats() refreshes immediately with the single-instance budget, here 8, while the
	// published requirement is 32. The sorter and the buffers must stay at >= 32, the contract
	// must not be clamped, and the frame must show the same splats as the control. Round-1
	// head: the sorter was rebuilt at 8 and, once the GPU buffer manager followed the budget,
	// the republish clamped the contract to the manager's capacity -- permanently, because the
	// resident publisher's fast path sees an unchanged source generation and valid RIDs.
	const RID keys_before_budget = sorting_pipeline->get_buffer_handles().keys_buffer;
	const uint64_t generation_before_budget = renderer->get_instance_pipeline_content_generation();
	const int64_t republishes_before_budget = republish_count();
	renderer->set_max_splats(8);
	dump_state("right after set_max_splats(8)");
	drive_and_check(String("set_max_splats(8) (single-instance budget far below the published requirement)"), 5, keys_before_budget,
			generation_before_budget, republishes_before_budget, false, 0u);
	check_requirement_held(String("set_max_splats(8)"));
	dump_state("after 5 frames post-budget");

	teardown();
}

// #702 -- the local-device instance cull must publish THIS frame's counter
// readback, not the pre-submit snapshot.
//
// WHY THIS IS A [RequiresGPU] CASE AND NOT A UNIT TEST. The defect only exists
// on a device where `gs_device_utils::safe_submit` actually submits and syncs,
// i.e. a LOCAL RenderingDevice (`gs_device_utils.h`; a no-op on the main
// device). `RenderingDevice::sync()` -> `_begin_frame()` -> `_stall_for_frame()`
// is what drains `buffer_get_data_async` callbacks
// (servers/rendering/rendering_device.cpp), so the fresh count lands *inside*
// the `safe_submit()` call and the old post-submit write-back overwrote it.
// Nothing short of a real device reproduces that ordering.
//
// WHAT IT ASSERTS, AND WHAT IT DELIBERATELY DOES NOT. It asserts the cull
// publishes a visible chunk count on the FIRST frame and that the sort stage
// therefore runs. It does NOT assert that the resident route produces rendered
// output: it does not, for a SEPARATE reason downstream of this fix (the
// instance contract publishes `max_chunk_splats=1` and the sort's own
// instance-count buffer resolves to 0), which is why the resident-quantization
// waiver above still stands. Asserting rendered output here would fail for a
// cause this fix does not address and make the case unable to discriminate.
//
// PRE-FIX / POST-FIX, measured on an RTX 3090 (dev build, Vulkan):
//   pre-fix   visible_after_culling=0  sort_route_uid=COMMON.UNSET.SORT_ROUTE  stage_sort_status=skipped
//   post-fix  visible_after_culling=1  sort_route_uid=INSTANCE.SORT.GPU        stage_sort_status=success
TEST_CASE("[GaussianSplatting][World][SceneTree][RequiresGPU] Local-device instance cull publishes the current frame's visible count and unblocks the sort stage") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		MESSAGE("Skipping test - Rendering server unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (tree == nullptr) {
		FAIL("SceneTree singleton required; the world node below cannot be entered into a tree");
		return;
	}
	Window *root = tree->get_root();
	if (root == nullptr) {
		FAIL("SceneTree root window required; the world node below cannot be entered into a tree");
		return;
	}
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings == nullptr) {
		FAIL("ProjectSettings singleton required to select the resident instance route");
		return;
	}

	ProjectSettingGuard route_guard(project_settings, "rendering/gaussian_splatting/streaming/route_policy");
	ProjectSettingGuard instance_guard(project_settings, "rendering/gaussian_splatting/instance_pipeline/enabled");
	project_settings->set_setting("rendering/gaussian_splatting/streaming/route_policy",
			int64_t(gs::settings::GS_ROUTE_RESIDENT));
	project_settings->set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true);
	project_settings->emit_signal("settings_changed");

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(32, 20.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	if (node == nullptr) {
		FAIL("could not allocate a GaussianSplatWorld3D");
		return;
	}
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (renderer.is_null()) {
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		FAIL("renderer unavailable; the instance cull path under test never runs");
		return;
	}

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);
	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	// ONE frame, deliberately. The whole point is that the counter readback is
	// consumed on the frame that issued it; a warm-up loop would let a later
	// frame's snapshot mask the defect.
	renderer->render_scene_instance(&render_data);

	const Dictionary stats = renderer->get_render_stats();
	CHECK_MESSAGE(int64_t(stats.get("visible_after_culling", int64_t(0))) > 0,
			"the instance cull must publish the counter value read back during its own submit, "
			"not the pre-submit snapshot (which is 0 on the first frame)");
	CHECK_MESSAGE(String(stats.get("cull_route_uid", String())) == String("INSTANCE.CULL.GPU"),
			"this case must exercise the GPU instance-cull path, or it cannot discriminate");
	CHECK_MESSAGE(String(stats.get("sort_route_uid", String())) == String("INSTANCE.SORT.GPU"),
			"a visible cull count must let the sort stage run; an unset sort route means the "
			"cull published 0 and render_instancing_orchestrator took the no-visible-splats skip");
	CHECK_MESSAGE(String(stats.get("stage_sort_status", String())) == String("success"),
			"the sort stage must report success once the cull publishes visible chunks");

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);
}

TEST_CASE("[GaussianSplatting][World][SceneTree] World node preserves prior renderer streaming overrides when tier budgets are disabled") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	REQUIRE(project_settings != nullptr);
	ProjectSettingGuard tier_preset_guard(project_settings, "rendering/gaussian_splatting/quality/tier_preset");
	ProjectSettingGuard tier_apply_guard(project_settings, "rendering/gaussian_splatting/quality/tier_apply_streaming_budgets");
	ProjectSettingGuard predictive_guard(project_settings, "rendering/gaussian_splatting/streaming/predictive_prefetch_enabled");
	ProjectSettingGuard prefetch_guard(project_settings, "rendering/gaussian_splatting/streaming/prefetch_lookahead_distance");
	project_settings->set_setting("rendering/gaussian_splatting/quality/tier_preset", "low");
	project_settings->set_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", true);
	project_settings->set_setting("rendering/gaussian_splatting/streaming/predictive_prefetch_enabled", false);
	project_settings->set_setting("rendering/gaussian_splatting/streaming/prefetch_lookahead_distance", 6.0f);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(5, 50.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *node = memnew(GaussianSplatWorld3D);
	REQUIRE(node != nullptr);
	node->set_auto_apply_on_ready(false);
	node->set_world(world_resource);
	node->set_max_render_distance(80.0f);
	root->add_child(node);
	tree->process(0.0);
	node->apply_world();

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		MESSAGE("Skipping test - renderer unavailable");
		root->remove_child(node);
		memdelete(node);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	const GaussianStreamingSystem::ConfigOverrides before_overrides = renderer->get_streaming_config_overrides();
	CHECK(before_overrides.override_prefetch);
	CHECK_FALSE(before_overrides.predictive_prefetch_enabled);
	CHECK(before_overrides.prefetch_lookahead_distance == doctest::Approx(12.0f));
	CHECK(before_overrides.override_vram_budget);

	project_settings->set_setting("rendering/gaussian_splatting/quality/tier_apply_streaming_budgets", false);
	node->set_max_render_distance(120.0f);

	GaussianSplatSceneDirector::WorldSubmission submission;
	CHECK(director->get_world_submission(node->get_instance_id(), &submission));
	CHECK_FALSE(submission.desired_renderer_overrides.has(StringName("streaming")));

	const GaussianStreamingSystem::ConfigOverrides after_overrides = renderer->get_streaming_config_overrides();
	CHECK(after_overrides.override_prefetch == before_overrides.override_prefetch);
	CHECK(after_overrides.predictive_prefetch_enabled == before_overrides.predictive_prefetch_enabled);
	CHECK(after_overrides.prefetch_lookahead_distance == doctest::Approx(before_overrides.prefetch_lookahead_distance));
	CHECK(after_overrides.override_vram_budget == before_overrides.override_vram_budget);
	CHECK(after_overrides.vram_budget_config.budget_mb == before_overrides.vram_budget_config.budget_mb);
	CHECK(after_overrides.vram_budget_config.min_chunks == before_overrides.vram_budget_config.min_chunks);
	CHECK(after_overrides.vram_budget_config.max_chunks == before_overrides.vram_budget_config.max_chunks);

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] Instance submission query mirrors live node registration") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);
	const GaussianSplatSceneDirector::SubmissionCounts baseline_counts = director->get_submission_counts();

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);
	node->set_splat_asset(stage1a_make_submission_test_asset(42.0f));
	node->set_opacity(0.5f);
	node->set_quality_preset(GaussianSplatNode3D::QUALITY_CUSTOM);
	node->set_lod_bias(1.25f);
	node->set_cast_shadow(true);

	root->add_child(node);
	tree->process(0.0);

	GaussianSplatSceneDirector::InstanceSubmission submission;
	CHECK(director->get_instance_submission(node->get_instance_id(), &submission));
	CHECK(submission.node_id == node->get_instance_id());
	CHECK(submission.asset.is_valid());
	CHECK(submission.opacity == doctest::Approx(0.5f));
	CHECK(submission.lod_bias == doctest::Approx(1.25f));
	CHECK(submission.casts_shadow);
	CHECK(submission.visible);

	GaussianSplatSceneDirector::SubmissionCounts counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions + 1);
	CHECK(counts.world_submissions == baseline_counts.world_submissions);

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	CHECK_FALSE(director->get_instance_submission(submission.node_id, &submission));
	counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions);
	CHECK(counts.world_submissions == baseline_counts.world_submissions);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] Explicit instance submission entrypoints round-trip") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);
	const GaussianSplatSceneDirector::SubmissionCounts baseline_counts = director->get_submission_counts();

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);
	root->add_child(node);
	tree->process(0.0);

	Ref<GaussianSplatAsset> asset = stage1a_make_submission_test_asset(8.0f);
	const Transform3D initial_transform(Basis(), Vector3(1.0f, 2.0f, 3.0f));
	const Transform3D updated_transform(Basis(), Vector3(4.0f, 5.0f, 6.0f));
	const Vector3 updated_wind_direction(0.0f, 1.0f, 0.0f);

	director->register_instance_submission(node->get_instance_id(), asset, initial_transform,
			0.25f, 1.5f, 0u, true, 0.8f,
			GaussianSplatSceneDirector::INSTANCE_WIND_FORCE_ENABLED,
			Vector3(1.0f, 0.0f, 0.0f), 2.0f, true,
			true, GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING,
			0.4f, 0.1f);

	GaussianSplatSceneDirector::InstanceSubmission submission;
	CHECK(director->get_instance_submission(node->get_instance_id(), &submission));
	CHECK(submission.node_id == node->get_instance_id());
	CHECK(submission.asset == asset);
	CHECK(submission.transform.origin.is_equal_approx(initial_transform.origin));
	CHECK(submission.opacity == doctest::Approx(0.25f));
	CHECK(submission.lod_bias == doctest::Approx(1.5f));
	CHECK(submission.casts_shadow);
	CHECK(submission.visible);
	CHECK(submission.wind_intensity == doctest::Approx(0.8f));
	CHECK(submission.wind_mode == GaussianSplatSceneDirector::INSTANCE_WIND_FORCE_ENABLED);
	CHECK(submission.wind_direction.is_equal_approx(Vector3(1.0f, 0.0f, 0.0f)));
	CHECK(submission.wind_frequency == doctest::Approx(2.0f));
	CHECK(submission.effect_position_scale == doctest::Approx(0.4f));
	CHECK(submission.effect_opacity_scale == doctest::Approx(0.1f));
	CHECK(submission.has_desired_residency_hint);
	CHECK(submission.desired_residency_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
	int32_t renderer_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	String renderer_hint_source;
	if (submission.renderer.is_valid()) {
		CHECK(director->get_submission_residency_hint_for_renderer(submission.renderer.ptr(), &renderer_hint, &renderer_hint_source));
		CHECK(renderer_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
		CHECK(renderer_hint_source == String("instance_submission"));
		LocalVector<InstanceDataGPU> initial_buffer;
		director->build_instance_buffer_for_renderer(submission.renderer.ptr(), initial_buffer, false);
		REQUIRE(initial_buffer.size() == 1);
		CHECK(initial_buffer[0].effect_params[0] == doctest::Approx(0.4f));
		CHECK(initial_buffer[0].effect_params[1] == doctest::Approx(0.1f));
	}

	director->update_instance_submission_transform(node->get_instance_id(), updated_transform);
	director->update_instance_submission_params(node->get_instance_id(), 0.6f, 0.9f, 0u, false, 1.2f,
			GaussianSplatSceneDirector::INSTANCE_WIND_FORCE_DISABLED,
			updated_wind_direction, 3.5f, false,
			true, GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT,
			1.8f, 0.65f);

	CHECK(director->get_instance_submission(node->get_instance_id(), &submission));
	CHECK(submission.transform.origin.is_equal_approx(updated_transform.origin));
	CHECK(submission.opacity == doctest::Approx(0.6f));
	CHECK(submission.lod_bias == doctest::Approx(0.9f));
	CHECK_FALSE(submission.casts_shadow);
	CHECK_FALSE(submission.visible);
	CHECK(submission.wind_intensity == doctest::Approx(1.2f));
	CHECK(submission.wind_mode == GaussianSplatSceneDirector::INSTANCE_WIND_FORCE_DISABLED);
	CHECK(submission.wind_direction.is_equal_approx(updated_wind_direction));
	CHECK(submission.wind_frequency == doctest::Approx(3.5f));
	CHECK(submission.effect_position_scale == doctest::Approx(1.8f));
	CHECK(submission.effect_opacity_scale == doctest::Approx(0.65f));
	CHECK(submission.has_desired_residency_hint);
	CHECK(submission.desired_residency_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT);
	if (submission.renderer.is_valid()) {
		CHECK(director->get_submission_residency_hint_for_renderer(submission.renderer.ptr(), &renderer_hint, &renderer_hint_source));
		CHECK(renderer_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT);
		CHECK(renderer_hint_source == String("instance_submission"));
		LocalVector<InstanceDataGPU> updated_buffer;
		director->build_instance_buffer_for_renderer(submission.renderer.ptr(), updated_buffer, false);
		REQUIRE(updated_buffer.size() == 1);
		CHECK(updated_buffer[0].effect_params[0] == doctest::Approx(1.8f));
		CHECK(updated_buffer[0].effect_params[1] == doctest::Approx(0.65f));
	}

	GaussianSplatSceneDirector::SubmissionCounts counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions + 1);
	CHECK(counts.world_submissions == baseline_counts.world_submissions);

	director->unregister_instance_submission(node->get_instance_id());
	CHECK_FALSE(director->get_instance_submission(node->get_instance_id(), &submission));

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	counts = director->get_submission_counts();
	CHECK(counts.instance_submissions == baseline_counts.instance_submissions);
	CHECK(counts.world_submissions == baseline_counts.world_submissions);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Mixed instance residency hints collapse to no effective renderer hint") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
	GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
	REQUIRE(node_a != nullptr);
	REQUIRE(node_b != nullptr);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);

	director->register_instance_submission(node_a->get_instance_id(), stage1a_make_submission_test_asset(2.0f),
			Transform3D(), 1.0f, 0.0f, 0u, false, 1.0f,
			GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT, Vector3(), 1.0f, true,
			true, GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT);
	director->register_instance_submission(node_b->get_instance_id(), stage1a_make_submission_test_asset(12.0f),
			Transform3D(), 1.0f, 0.0f, 0u, false, 1.0f,
			GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT, Vector3(), 1.0f, true,
			true, GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);

	GaussianSplatSceneDirector::InstanceSubmission submission_a;
	GaussianSplatSceneDirector::InstanceSubmission submission_b;
	CHECK(director->get_instance_submission(node_a->get_instance_id(), &submission_a));
	CHECK(director->get_instance_submission(node_b->get_instance_id(), &submission_b));
	if (!submission_a.renderer.is_valid() || !submission_b.renderer.is_valid()) {
		FAIL("Instance submissions carry no renderer for the mixed-hint test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		director->unregister_instance_submission(node_a->get_instance_id());
		director->unregister_instance_submission(node_b->get_instance_id());
		root->remove_child(node_a);
		root->remove_child(node_b);
		memdelete(node_a);
		memdelete(node_b);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}
	CHECK(submission_a.renderer == submission_b.renderer);
	CHECK_FALSE(director->has_world_submission_for_renderer(submission_a.renderer.ptr()));

	int32_t renderer_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
	String renderer_hint_source;
	CHECK_FALSE(director->get_submission_residency_hint_for_renderer(submission_a.renderer.ptr(),
			&renderer_hint, &renderer_hint_source));
	CHECK(renderer_hint_source == String("mixed_instance_submissions"));

	director->unregister_instance_submission(node_a->get_instance_id());
	director->unregister_instance_submission(node_b->get_instance_id());

	root->remove_child(node_a);
	root->remove_child(node_b);
	memdelete(node_a);
	memdelete(node_b);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Active world residency hint takes precedence over instance hints") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(8, 4.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	GaussianSplatNode3D *instance_node = memnew(GaussianSplatNode3D);
	REQUIRE(world_node != nullptr);
	REQUIRE(instance_node != nullptr);
	world_node->set_auto_apply_on_ready(false);
	world_node->set_world(world_resource);
	root->add_child(world_node);
	root->add_child(instance_node);
	tree->process(0.0);
	world_node->apply_world();

	Ref<GaussianSplatRenderer> renderer = world_node->get_renderer();
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the active-world residency-precedence test. "
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case could report green having executed nothing.)");
		root->remove_child(world_node);
		root->remove_child(instance_node);
		memdelete(world_node);
		memdelete(instance_node);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	director->register_instance_submission(instance_node->get_instance_id(), stage1a_make_submission_test_asset(18.0f),
			Transform3D(), 1.0f, 0.0f, 0u, false, 1.0f,
			GaussianSplatSceneDirector::INSTANCE_WIND_INHERIT, Vector3(), 1.0f, true,
			true, GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);

	int32_t renderer_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;
	String renderer_hint_source;
	CHECK(director->get_submission_residency_hint_for_renderer(renderer.ptr(), &renderer_hint, &renderer_hint_source));
	CHECK(renderer_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
	CHECK(renderer_hint_source == String("world_submission"));

	director->unregister_instance_submission(instance_node->get_instance_id());
	world_node->clear_world();

	root->remove_child(world_node);
	root->remove_child(instance_node);
	memdelete(world_node);
	memdelete(instance_node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Shared renderer survives temporary last-instance unregister") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);
	const GaussianSplatSceneDirector::SubmissionCounts baseline_counts = director->get_submission_counts();

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);
	node->set_splat_asset(stage1a_make_submission_test_asset(6.0f));
	root->add_child(node);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> retained_renderer = node->get_renderer();
	if (!retained_renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the renderer-retention test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	CHECK(director->get_submission_counts().instance_submissions == baseline_counts.instance_submissions + 1);

	root->remove_child(node);
	tree->process(0.0);

	CHECK(director->get_submission_counts().instance_submissions == baseline_counts.instance_submissions);

	Ref<GaussianSplatRenderer> shared_renderer = director->get_shared_renderer(world.ptr());
	CHECK(shared_renderer == retained_renderer);

	root->add_child(node);
	tree->process(0.0);

	CHECK(node->get_renderer() == retained_renderer);
	CHECK(director->get_submission_counts().instance_submissions == baseline_counts.instance_submissions + 1);

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Active world submission survives last-instance unregister") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(8, 4.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	GaussianSplatNode3D *instance_node = memnew(GaussianSplatNode3D);
	REQUIRE(world_node != nullptr);
	REQUIRE(instance_node != nullptr);
	world_node->set_auto_apply_on_ready(false);
	world_node->set_world(world_resource);
	instance_node->set_splat_asset(stage1a_make_submission_test_asset(18.0f));
	root->add_child(world_node);
	root->add_child(instance_node);
	tree->process(0.0);
	world_node->apply_world();

	Ref<GaussianSplatRenderer> renderer = world_node->get_renderer();
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the active-world retention test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
		root->remove_child(world_node);
		root->remove_child(instance_node);
		memdelete(world_node);
		memdelete(instance_node);
		tree->process(0.0);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}

	GaussianSplatSceneDirector::WorldSubmission queried_submission;
	CHECK(director->has_world_submission_for_renderer(renderer.ptr()));
	CHECK(director->get_world_submission_for_scenario(world->get_scenario(), &queried_submission));

	root->remove_child(instance_node);
	tree->process(0.0);

	CHECK(director->has_world_submission_for_renderer(renderer.ptr()));
	CHECK(director->get_world_submission_for_scenario(world->get_scenario(), &queried_submission));
	CHECK(director->get_shared_renderer(world.ptr()) == renderer);

	world_node->clear_world();
	root->remove_child(world_node);
	memdelete(world_node);
	memdelete(instance_node);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] World submission produces identity instance in build_instance_buffer_for_renderer") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = owner->get_instance_id();
	submission.scenario = scenario;
	submission.gaussian_data = stage1a_make_submission_test_data(8, 0.0f);
	submission.static_chunks.push_back(stage1a_make_submission_test_chunk(0));

	CHECK(director->submit_world_submission(submission));

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (renderer.is_valid()) {
		LocalVector<InstanceDataGPU> instance_buffer;
		director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer, false);

		CHECK_MESSAGE(instance_buffer.size() == 1,
				"World submission should produce exactly one instance entry");
		if (!instance_buffer.is_empty()) {
			const InstanceDataGPU &entry = instance_buffer[0];
			CHECK_MESSAGE(entry.ids[0] == 0u,
					"World submission instance should reference primary asset (id=0)");
			CHECK_MESSAGE((entry.ids[1] & GS_INSTANCE_FLAG_ROTATION_IDENTITY) != 0,
					"World submission instance should have identity rotation flag");
			CHECK_MESSAGE((entry.ids[1] & GS_INSTANCE_FLAG_SCALE_IDENTITY) != 0,
					"World submission instance should have identity scale flag");
			CHECK_MESSAGE((entry.ids[1] & GS_INSTANCE_FLAG_TRANSLATION_ZERO) != 0,
					"World submission instance should have zero translation flag");
			CHECK(entry.rotation[3] == doctest::Approx(1.0f));
			CHECK(entry.inv_rotation[3] == doctest::Approx(1.0f));
			CHECK(entry.translation_scale[3] == doctest::Approx(1.0f));
			CHECK(entry.params[0] == doctest::Approx(1.0f));
		}
	} else {
		FAIL("Shared renderer unavailable for the identity-instance buffer test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
	}

	director->release_world_submission(submission.owner_id);

	if (renderer.is_valid()) {
		LocalVector<InstanceDataGPU> instance_buffer_after;
		director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer_after, false);
		CHECK_MESSAGE(instance_buffer_after.is_empty(),
				"After release, world submission should produce no instances");
	}

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Source-backed world submission remains renderable without resident data") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianData> source_data = stage1a_make_submission_test_data(8, 0.0f);
	Ref<InMemoryChunkPayloadSource> payload_source;
	payload_source.instantiate();
	payload_source->set_data(source_data);

	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = owner->get_instance_id();
	submission.scenario = scenario;
	submission.payload_source = payload_source;
	submission.static_chunks.push_back(stage1a_make_submission_test_chunk(0));
	submission.has_desired_residency_hint = true;
	submission.desired_residency_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING;

	CHECK(director->submit_world_submission(submission));

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (renderer.is_valid()) {
		CHECK(director->has_world_submission_for_renderer(renderer.ptr()));

		int32_t renderer_hint = GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;
		String renderer_hint_source;
		CHECK(director->get_submission_residency_hint_for_renderer(renderer.ptr(),
				&renderer_hint, &renderer_hint_source));
		CHECK(renderer_hint == GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_STREAMING);
		CHECK(renderer_hint_source == String("world_submission"));

		LocalVector<InstanceDataGPU> instance_buffer;
		director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer, false);
		CHECK_MESSAGE(instance_buffer.size() == 1,
				"Source-backed world submission should produce the same identity instance as resident data.");

		LocalVector<InstanceGradingGPU> grading_buffer;
		director->build_instance_grading_buffer_for_renderer(renderer.ptr(), grading_buffer, false);
		CHECK_MESSAGE(grading_buffer.size() == 1,
				"Source-backed world submission should produce a matching grading row.");
	} else {
		FAIL("Shared renderer unavailable for the source-backed world-submission test. " 
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null shared renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case reported green having executed almost nothing.)");
	}

	director->release_world_submission(submission.owner_id);
	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] World submission with zero splats produces no instance") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	Node *owner = memnew(Node);
	REQUIRE(owner != nullptr);
	root->add_child(owner);
	tree->process(0.0);

	Ref<GaussianData> empty_data;
	empty_data.instantiate();
	empty_data->resize(0);

	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = owner->get_instance_id();
	submission.scenario = scenario;
	submission.gaussian_data = empty_data;

	CHECK(director->submit_world_submission(submission));

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (renderer.is_valid()) {
		LocalVector<InstanceDataGPU> instance_buffer;
		director->build_instance_buffer_for_renderer(renderer.ptr(), instance_buffer, false);
		CHECK_MESSAGE(instance_buffer.is_empty(),
				"Zero-splat world submission should produce no instances");
	}

	director->release_world_submission(submission.owner_id);
	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);

	if (owns_director) {
		memdelete(director);
	}
}

// Regression: PR #280 ("Wave 1 fallback cleanup") removed the legacy resident fallback inside
// _try_render_resident_frame(), making the early "no_render_data" gate fatal whenever
// scene_state.gaussian_data was null. Asset-backed GaussianSplatNode3D scenes hit that gate
// because they hand their data to the director (instance assets) but never call
// renderer->set_gaussian_data(). The frame skipped before the resident contract publisher could
// pick the asset up from the director, so node_visible_splats_max stayed at 0 even though every
// splat was registered and ready to render. This test pins the canonical asset-backed path.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Asset-backed GaussianSplatNode3D renders without direct set_gaussian_data") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		FAIL("RenderingServer singleton unavailable for the asset-backed render test. "
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null RenderingServer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case could report green having executed nothing.)");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<GaussianSplatAsset> asset = stage1a_make_submission_test_asset(0.0f);
	REQUIRE(asset.is_valid());

	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	REQUIRE(node != nullptr);
	node->set_splat_asset(asset);
	root->add_child(node);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = node->get_renderer();
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the asset-backed render test. "
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case could report green having executed nothing.)");
		root->remove_child(node);
		memdelete(node);
		tree->process(0.0);
		return;
	}

	// Sanity: the canonical asset-backed path must NOT have populated scene_state.gaussian_data
	// directly. If this ever flips true, the regression we're guarding here has been masked by
	// some other pathway and this test is no longer testing what it claims to test.
	CHECK_FALSE(renderer->get_scene_state().gaussian_data.is_valid());

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	renderer->render_scene_instance(&render_data);

	// Pre-fix behavior was: route_uid begins with COMMON_SKIP_RESIDENT_NOT_FEASIBLE,
	// instance_contract_ready=false, has_instance_pipeline_buffers()=false.
	// With the fix the publisher runs, builds the atlas from director-stored asset records, and
	// the resident contract becomes ready.
	const Dictionary stats = renderer->get_render_stats();
	const String route_uid = stats.get("route_uid", String());
	CHECK_FALSE_MESSAGE(route_uid.begins_with(String(RenderRouteUID::COMMON_SKIP_RESIDENT_NOT_FEASIBLE)),
			vformat("Asset-backed node was rejected with no_render_data; route_uid=%s", route_uid));
	CHECK(stats.get("instance_backend_policy", String()) == String("resident"));
	CHECK(bool(stats.get("instance_contract_ready", false)));
	CHECK(renderer->has_instance_pipeline_buffers());

	root->remove_child(node);
	memdelete(node);
	tree->process(0.0);
}

// Regression: the resident contract publisher used to mix per-instance state (visibility,
// transform, opacity, wind, color grading) into the same generation hash that gated the
// atlas pack. Any per-frame mutation -- common in gameplay (player walking, interactable
// rotation tweens, wind animation) -- forced a full repack of every registered asset's
// gaussian arrays, costing hundreds of ms of CPU time on dense scenes.
//
// The fix splits the publisher's source_generation into atlas_generation (asset list +
// per-asset content_revision) and instance_generation (everything else), and uses
// collect_registered_assets_for_renderer for atlas membership so visibility flips never
// mutate the asset set. This test pins the new contract: per-instance churn must NOT
// cause the publisher to re-run the atlas pack loop.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree][RequiresGPU] Resident publisher does not repack atlas on per-instance state changes") {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		FAIL("RenderingServer singleton unavailable for the resident-publisher repack test. "
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null RenderingServer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case could report green having executed nothing.)");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<GaussianSplatAsset> asset_a = stage1a_make_submission_test_asset(0.0f);
	Ref<GaussianSplatAsset> asset_b = stage1a_make_submission_test_asset(5.0f);
	REQUIRE(asset_a.is_valid());
	REQUIRE(asset_b.is_valid());

	GaussianSplatNode3D *node_a = memnew(GaussianSplatNode3D);
	GaussianSplatNode3D *node_b = memnew(GaussianSplatNode3D);
	node_a->set_splat_asset(asset_a);
	node_b->set_splat_asset(asset_b);
	root->add_child(node_a);
	root->add_child(node_b);
	tree->process(0.0);

	Ref<GaussianSplatRenderer> renderer = node_a->get_renderer();
	if (!renderer.is_valid()) {
		FAIL("Shared renderer unavailable for the resident-publisher repack test. "
				"This case is [RequiresGPU] and executes only under the --gs-gpu-test harness, which brings up a real RenderingDevice. A null renderer here means the harness failed to provide one -- that is a harness/product failure, not a reason to skip. (Previously this branch silently returned, so the case could report green having executed nothing.)");
		root->remove_child(node_b);
		root->remove_child(node_a);
		memdelete(node_b);
		memdelete(node_a);
		tree->process(0.0);
		return;
	}
	REQUIRE(node_a->get_renderer() == node_b->get_renderer());

	RenderSceneDataRD scene_data;
	scene_data.cam_transform = Transform3D(Basis(), Vector3(0.0f, 0.0f, 5.0f));
	scene_data.cam_projection.set_perspective(70.0f, 1.0f, 0.1f, 100.0f);

	RenderDataRD render_data;
	render_data.scene_data = &scene_data;
	render_data.render_buffers = Ref<RenderSceneBuffersRD>();

	// First render: this is the slow path, should pack the atlas exactly once.
	renderer->render_scene_instance(&render_data);
	const uint64_t pack_count_after_first = renderer->get_resource_state().resident_atlas_pack_count;
	CHECK_GE(pack_count_after_first, uint64_t(1));

	// Second render with no state change: full early-out, no repack.
	renderer->render_scene_instance(&render_data);
	CHECK_EQ(renderer->get_resource_state().resident_atlas_pack_count, pack_count_after_first);

	// Hammer per-instance state — none of these touch atlas content (asset list, content
	// revisions are stable), so the publisher must take the fast path on every frame.
	node_a->set_opacity(0.5f);
	node_a->set_lod_bias(1.5f);
	node_a->set_visible(false);
	node_a->set_transform(Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), 0.5f), Vector3(1.0f, 0.0f, 0.0f)));
	tree->process(0.0);
	renderer->render_scene_instance(&render_data);
	node_a->set_visible(true);
	node_a->set_transform(Transform3D(Basis(Vector3(0.0f, 1.0f, 0.0f), 1.0f), Vector3(2.0f, 0.0f, 0.0f)));
	tree->process(0.0);
	renderer->render_scene_instance(&render_data);
	node_b->set_cast_shadow(true);
	tree->process(0.0);
	renderer->render_scene_instance(&render_data);

	CHECK_MESSAGE(renderer->get_resource_state().resident_atlas_pack_count == pack_count_after_first,
			"Per-instance state changes (opacity, lod_bias, visibility, transform, cast_shadow) "
			"must NOT trigger a resident atlas repack — that was the dream_memory stutter regression.");

	root->remove_child(node_b);
	root->remove_child(node_a);
	memdelete(node_b);
	memdelete(node_a);
	tree->process(0.0);
}

// Regression test for Codex review comment #3294053937 on PR #387.
//
// Before the fix, GaussianSplatWorld3D::NOTIFICATION_PREDELETE
// unconditionally called teardown_world_for_scenario(last_known_scenario).
// When a non-owner duplicate world node (whose submit_world_submission
// was rejected because another live owner held the scenario) was deleted,
// that scenario-wide teardown wiped the SharedWorld entry for the active
// owner, dropping renderer/submission state for the still-live peer.
//
// The fix routes per-instance world node PREDELETE through the existing
// ownership-aware release_world_submission(owner_id) path (mirroring
// _unregister_shared_renderer / EXIT_TREE). Non-owners become a no-op
// and the active owner's SharedWorld is preserved.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] Deleting a non-owner world node preserves the active owner's SharedWorld") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	Ref<GaussianSplatWorld> world_resource_a;
	world_resource_a.instantiate();
	world_resource_a->set_gaussian_data(stage1a_make_submission_test_data(8, 1.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks_a;
	chunks_a.push_back(stage1a_make_submission_test_chunk(0));
	world_resource_a->set_static_chunks(chunks_a);

	Ref<GaussianSplatWorld> world_resource_b;
	world_resource_b.instantiate();
	world_resource_b->set_gaussian_data(stage1a_make_submission_test_data(4, 50.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks_b;
	chunks_b.push_back(stage1a_make_submission_test_chunk(1));
	world_resource_b->set_static_chunks(chunks_b);

	// First world node: registers, wins ownership of the scenario's
	// world-submission slot. submit_world_submission's ownership
	// arbitration runs without needing a real GPU renderer (the contract
	// apply is a no-op when the shared renderer is null), so this test
	// validates the PREDELETE ownership gate even on headless / no-GPU
	// environments.
	GaussianSplatWorld3D *world_node_a = memnew(GaussianSplatWorld3D);
	REQUIRE(world_node_a != nullptr);
	world_node_a->set_auto_apply_on_ready(false);
	world_node_a->set_world(world_resource_a);
	root->add_child(world_node_a);
	tree->process(0.0);
	world_node_a->apply_world();

	// Confirm A is the active world-submission owner of the scenario.
	// Query via the director (not via world_node_a->get_renderer(), which
	// may be null when no GPU is available -- ownership is independent
	// of renderer availability).
	GaussianSplatSceneDirector::WorldSubmission queried_submission;
	const ObjectID owner_a_id = world_node_a->get_instance_id();
	REQUIRE_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"world_node_a should have submitted to the director on apply_world().");
	REQUIRE(queried_submission.owner_id == owner_a_id);
	REQUIRE_MESSAGE(director->get_world_submission(owner_a_id, &queried_submission),
			"Director should have an active world-submission keyed by owner_a_id.");

	// Snapshot the renderer Ref if one exists; pure-CPU environments may
	// return null but that's fine for the ownership-gate assertion below.
	Ref<GaussianSplatRenderer> renderer_a_before = director->get_shared_renderer(world.ptr());

	// Second world node bound to the SAME scenario: its submit must be
	// rejected by ownership arbitration since A is still live (see
	// gaussian_splat_scene_director.cpp::submit_world_submission and
	// _is_world_submission_owner_live).
	GaussianSplatWorld3D *world_node_b = memnew(GaussianSplatWorld3D);
	REQUIRE(world_node_b != nullptr);
	world_node_b->set_auto_apply_on_ready(false);
	world_node_b->set_world(world_resource_b);
	root->add_child(world_node_b);
	tree->process(0.0);
	world_node_b->apply_world();

	// B is a non-owner: the scenario's world-submission must still point at A.
	CHECK_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"Active owner's world-submission must still exist after a non-owner peer applies.");
	CHECK_MESSAGE(queried_submission.owner_id == owner_a_id,
			"Expected the second GaussianSplatWorld3D to be rejected by ownership arbitration; "
			"active owner of the scenario must still be world_node_a.");
	// B's own world-submission must NOT exist (it was rejected).
	GaussianSplatSceneDirector::WorldSubmission rejected_query;
	CHECK_FALSE_MESSAGE(director->get_world_submission(world_node_b->get_instance_id(), &rejected_query),
			"Non-owner world_node_b should not have an active world-submission record.");

	// The bug under test: memdelete(B) triggers NOTIFICATION_PREDELETE on a
	// non-owner. Pre-fix, this called teardown_world_for_scenario(scenario),
	// erasing the SharedWorld for A.
	root->remove_child(world_node_b);
	memdelete(world_node_b);
	tree->process(0.0);

	// Post-fix expectations: A's SharedWorld entry and active world-submission
	// must be intact. The director's world-submission record for A must still
	// be queryable both by scenario and by owner id.
	CHECK_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"Scenario's world-submission record must survive a non-owner peer's PREDELETE.");
	CHECK_MESSAGE(queried_submission.owner_id == owner_a_id,
			"Active owner of the scenario must still be world_node_a after non-owner PREDELETE.");
	CHECK_MESSAGE(director->get_world_submission(owner_a_id, &queried_submission),
			"Active owner's world-submission must survive a non-owner peer's PREDELETE.");
	// If a renderer was created, it must be the same Ref (the SharedWorld
	// entry must not have been torn down and recreated). Pre-fix this also
	// became null because the entry was erased.
	if (renderer_a_before.is_valid()) {
		Ref<GaussianSplatRenderer> renderer_a_after = director->get_shared_renderer(world.ptr());
		CHECK_MESSAGE(renderer_a_after == renderer_a_before,
				"Active owner's shared renderer must be the same instance after a non-owner peer's PREDELETE.");
		CHECK_MESSAGE(director->has_world_submission_for_renderer(renderer_a_before.ptr()),
				"Active owner's world-submission must still resolve from its renderer.");
	}

	// Cleanup: removing A IS the owner-path, this exercises the
	// release_world_submission + _prune_world_if_unused tail.
	world_node_a->clear_world();
	root->remove_child(world_node_a);
	memdelete(world_node_a);
	tree->process(0.0);

	// After the owner leaves, the world-submission must be gone.
	CHECK_FALSE_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"Owner's PREDELETE must release the world-submission record.");

	if (owns_director) {
		memdelete(director);
	}
}

// Regression test for Codex review comment #3294797697 on PR #387 (world-node analog).
//
// Before the fix, GaussianSplatWorld3D::NOTIFICATION_PREDELETE relied on the
// second `release_world_submission()` call to trigger _prune_world_if_unused
// AFTER the per-instance `renderer.unref()`. But NOTIFICATION_EXIT_TREE
// already ran release_world_submission, so the second call's
// `_find_world_for_world_submission(owner_id)` returned null and never
// reached the prune helper. With the refcount drop happening only at
// PREDELETE, the SharedWorld entry persisted across reload cycles holding
// the renderer/data lifetime anchor.
//
// This test exercises the EXIT_TREE-then-PREDELETE ordering with an
// external Ref on the renderer that keeps the refcount above 1 across
// EXIT_TREE (so the EXIT_TREE prune correctly observes refcount>1 and
// skips the prune). Then we drop the external Ref and trigger PREDELETE
// via memdelete; the fix's explicit `try_prune_world_if_unused` after
// `renderer.unref()` must garbage-collect the SharedWorld.
//
// The pre-existing non-owner test (above) does NOT cover this case --
// it exercises a DIFFERENT no-op reason (ownership-aware path correctly
// does nothing for non-owners while the owner's entry is preserved).
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] World node PREDELETE prunes SharedWorld after renderer ref drop") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	Ref<GaussianSplatWorld> world_resource;
	world_resource.instantiate();
	world_resource->set_gaussian_data(stage1a_make_submission_test_data(8, 1.0f));
	Vector<GaussianSplatRenderer::StaticChunk> chunks;
	chunks.push_back(stage1a_make_submission_test_chunk(0));
	world_resource->set_static_chunks(chunks);

	GaussianSplatWorld3D *world_node = memnew(GaussianSplatWorld3D);
	REQUIRE(world_node != nullptr);
	world_node->set_auto_apply_on_ready(false);
	world_node->set_world(world_resource);
	root->add_child(world_node);
	tree->process(0.0);
	world_node->apply_world();

	// Snapshot a renderer Ref. On headless / no-GPU runners this may be null;
	// in that case the SharedWorld is created without a renderer and prune
	// reduces to the refcount==null branch of _should_prune_world, which is
	// still the correct gate.
	Ref<GaussianSplatRenderer> external_renderer_ref = director->get_shared_renderer(world.ptr());

	// The director should have created an entry for this scenario.
	REQUIRE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"Director should have a SharedWorld entry for the scenario after world_node->apply_world().");

	// EXIT_TREE: remove from tree. This triggers
	// _unregister_shared_renderer() -> release_world_submission() which calls
	// _prune_world_if_unused. Because world_node still holds its renderer
	// member Ref AND we hold an external Ref above, refcount must be >1 and
	// the SharedWorld entry must be preserved.
	root->remove_child(world_node);
	tree->process(0.0);

	if (external_renderer_ref.is_valid()) {
		// The renderer Ref keeps refcount > 1 -- entry must survive EXIT_TREE.
		CHECK_MESSAGE(director->has_shared_world_for_scenario(scenario),
				"SharedWorld must survive EXIT_TREE while an external renderer Ref still pins refcount > 1.");
	}

	// Drop the external Ref. Now the only remaining Ref is whatever
	// world_node->renderer holds. PREDELETE's renderer.unref() will drop
	// the last meaningful Ref, and the explicit try_prune_world_if_unused()
	// must garbage-collect the SharedWorld entry. Pre-fix this never
	// happened because the second release_world_submission() in PREDELETE
	// was a no-op (the owner record was cleared in EXIT_TREE).
	external_renderer_ref.unref();

	// memdelete triggers NOTIFICATION_PREDELETE.
	memdelete(world_node);
	tree->process(0.0);

	CHECK_FALSE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"SharedWorld must be pruned after PREDELETE's renderer.unref() drops the last reference. "
			"Pre-fix this entry persisted because the second release_world_submission() in PREDELETE "
			"was a no-op (owner record already cleared in EXIT_TREE), so _prune_world_if_unused was "
			"never re-run with the reduced refcount. Defeated the F6-reload-leak fix in PR #387.");
	GaussianSplatSceneDirector::WorldSubmission queried_submission;
	CHECK_FALSE_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"No world-submission record should remain for the scenario after PREDELETE.");

	if (owns_director) {
		memdelete(director);
	}
}

// Regression test for Codex review comment #3294797692 on PR #387 (node-side analog).
//
// Mirrors the world-node case above for GaussianSplatNode3D. The bug shape
// is identical: NOTIFICATION_EXIT_TREE runs _unregister_shared_renderer() ->
// unregister_instance() -> _prune_world_if_unused; the prune correctly
// observes refcount>1 (this node still holds renderer Ref) and skips. On
// PREDELETE the second _unregister_shared_renderer() call's
// unregister_instance() finds no instance record (already removed in
// EXIT_TREE) and returns early WITHOUT calling _prune_world_if_unused. With
// renderer.unref() happening before the second call, the refcount drop
// never reaches the prune helper, so the SharedWorld entry persists.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] Node PREDELETE prunes SharedWorld after renderer ref drop") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	GaussianSplatNode3D *instance_node = memnew(GaussianSplatNode3D);
	REQUIRE(instance_node != nullptr);
	instance_node->set_splat_asset(stage1a_make_submission_test_asset(2.0f));
	root->add_child(instance_node);
	tree->process(0.0);

	// Snapshot an external renderer Ref to keep refcount > 1 across EXIT_TREE.
	Ref<GaussianSplatRenderer> external_renderer_ref = director->get_shared_renderer(world.ptr());

	REQUIRE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"Director should have a SharedWorld entry after instance_node registers.");

	// EXIT_TREE: triggers unregister_instance -> _prune_world_if_unused, but
	// the prune is gated by refcount>1 (node + external Ref both alive).
	root->remove_child(instance_node);
	tree->process(0.0);

	if (external_renderer_ref.is_valid()) {
		CHECK_MESSAGE(director->has_shared_world_for_scenario(scenario),
				"SharedWorld must survive EXIT_TREE while an external renderer Ref still pins refcount > 1.");
	}

	// Drop the external Ref so the node's `renderer` member is the only
	// remaining Ref. PREDELETE's renderer.unref() must drop the last
	// meaningful Ref and the explicit try_prune_world_if_unused() must
	// garbage-collect the SharedWorld.
	external_renderer_ref.unref();

	memdelete(instance_node);
	tree->process(0.0);

	CHECK_FALSE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"SharedWorld must be pruned after PREDELETE's renderer.unref() drops the last reference. "
			"Pre-fix this entry persisted because the second _unregister_shared_renderer() in PREDELETE "
			"was a no-op (instance record already removed in EXIT_TREE), so _prune_world_if_unused was "
			"never re-run with the reduced refcount. Defeated the F6-reload-leak fix in PR #387.");

	if (owns_director) {
		memdelete(director);
	}
}

// Regression guard for #611 (world_mutex ↔ render-thread lock-order inversion).
//
// The real deadlock — the render thread blocked acquiring world_mutex inside a
// *_for_renderer builder while the main thread holds world_mutex and blocks on a
// render-thread dispatch (renderer teardown / initialize) — cannot be reproduced
// in this headless harness because there is no live render thread and the
// dispatcher short-circuits when the render loop is disabled. What this test DOES
// pin is the structural half of the fix: teardown_world_for_scenario moves the
// renderer Ref out of the map under the lock and erases the SharedWorld, then
// releases the Ref only after world_mutex is dropped. It asserts the observable
// contract (world created, then fully erased, and idempotent on re-entry) so a
// regression that reintroduces a synchronous renderer drop under the lock — or
// leaks the deferred Ref — is caught. On no-GPU runners the renderer is null, so
// this exercises the deferred-release control flow with an empty sink.
TEST_CASE("[GaussianSplatting][SceneDirector][SceneTree] teardown_world_for_scenario erases world outside world_mutex and is idempotent (#611)") {
	SceneTree *tree = SceneTree::get_singleton();
	REQUIRE_MESSAGE(tree != nullptr, "SceneTree singleton required");

	Window *root = tree->get_root();
	REQUIRE_MESSAGE(root != nullptr, "SceneTree root window required");

	Ref<World3D> world = root->get_world_3d();
	REQUIRE(world.is_valid());
	const RID scenario = world->get_scenario();
	REQUIRE(scenario.is_valid());

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	// Create a SharedWorld entry for the scenario via a world-submission. On
	// no-GPU runners the per-scenario renderer is null, but the SharedWorld entry
	// (and its active submission) is still created.
	GaussianSplatSceneDirector::WorldSubmission submission;
	submission.owner_id = root->get_instance_id();
	submission.scenario = scenario;
	submission.gaussian_data = stage1a_make_submission_test_data(8, 1.0f);
	submission.bounds = submission.gaussian_data->get_aabb();
	const bool submitted = director->submit_world_submission(submission);
	REQUIRE_MESSAGE(submitted, "submit_world_submission should create the SharedWorld entry.");
	REQUIRE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"Director should have a SharedWorld entry after submit_world_submission.");

	// Teardown must erase the SharedWorld entry (moving any renderer Ref out for a
	// post-lock release) and leave no world-submission record behind.
	director->teardown_world_for_scenario(scenario);
	CHECK_FALSE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"teardown_world_for_scenario must erase the SharedWorld entry.");
	GaussianSplatSceneDirector::WorldSubmission queried_submission;
	CHECK_FALSE_MESSAGE(director->get_world_submission_for_scenario(scenario, &queried_submission),
			"No world-submission record should remain after teardown_world_for_scenario.");

	// Idempotent: a second teardown on the now-absent entry is a safe no-op.
	director->teardown_world_for_scenario(scenario);
	CHECK_FALSE_MESSAGE(director->has_shared_world_for_scenario(scenario),
			"teardown_world_for_scenario must remain a no-op when the entry is already gone.");

	if (owns_director) {
		memdelete(director);
	}
}

// #611 PR B2 — the previous-world eviction branch in submit_world_submission's
// commit phase.
//
// WHY THIS TEST EXISTS. Mutation-checking PR B2 found that branch was reachable
// by no test in this repository, for a reason that turned out to be a corpus gap
// rather than a structural limit: every other world-submission test here binds
// both of its submissions to the SAME scenario (`root->get_world_3d()`), so
// `previous_world != world` was never true. Two mutations of production code —
// deleting the eviction entirely, and reintroducing a cancel()-ordering bug that
// drops the queued restore — both passed the full 639-case suite.
//
// This test constructs a SECOND scenario, which is all that was missing.
//
// WHAT IT PINS, PRECISELY. The branch has two effects, and they do not have the
// same testability:
//
//   1. `previous_world->world_submission` is cleared — pure bookkeeping, needs no
//      RenderingDevice. That is what this test asserts, and it is what makes the
//      eviction pinned rather than merely reviewed.
//   2. a restore is queued for the previous world's renderer — headless that
//      renderer is always null and `queue_restore` early-outs, so nothing is
//      enqueued and nothing observable happens. This test does NOT pin it, and no
//      headless test can; that half needs a live device.
//
// So a reviewer should read this as covering the eviction's *record* half only.
//
// #675: half 2 — and with it the cancel()-ordering bug — is now pinned by the
// [RequiresGPU] twin of this case at the bottom of this file, which runs in
// run_gpu_harness.py's SceneDirectorSceneTree batch. Mutation-checked: moving
// `cancel()` after the queued restore fails that case on 5 renderer-side
// assertions while THIS case still passes, which is exactly the split described
// above.
TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree] Re-submitting one owner to a second scenario evicts its previous world's record") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (!director) {
		FAIL("GaussianSplatSceneDirector unavailable");
		return;
	}
	const GaussianSplatSceneDirector::SubmissionCounts baseline_counts = director->get_submission_counts();

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		FAIL("SceneTree singleton required");
		return;
	}
	Window *root = tree->get_root();
	if (!root) {
		FAIL("SceneTree root window required");
		return;
	}

	Ref<World3D> world_a = root->get_world_3d();
	if (world_a.is_null()) {
		FAIL("root World3D required");
		return;
	}
	// The second scenario — the thing no other test in this file constructs, and
	// the sole reason the eviction branch was unreachable.
	Ref<World3D> world_b;
	world_b.instantiate();
	if (world_b.is_null()) {
		FAIL("could not instantiate a second World3D");
		return;
	}
	const RID scenario_a = world_a->get_scenario();
	const RID scenario_b = world_b->get_scenario();
	if (!scenario_a.is_valid() || !scenario_b.is_valid() || scenario_a == scenario_b) {
		// Without two DISTINCT scenarios `previous_world != world` cannot hold and
		// every assertion below would pass vacuously against the very mutation this
		// test exists to catch.
		FAIL("two distinct valid scenarios are required to reach the eviction branch");
		return;
	}

	Node *owner = memnew(Node);
	if (!owner) {
		FAIL("could not allocate the submission owner");
		return;
	}
	root->add_child(owner);
	tree->process(0.0);
	const ObjectID owner_id = owner->get_instance_id();

	// SAME owner, DIFFERENT scenarios. That pairing is what makes
	// _find_world_for_world_submission return world A while the commit targets
	// world B.
	GaussianSplatSceneDirector::WorldSubmission submission_a;
	submission_a.owner_id = owner_id;
	submission_a.scenario = scenario_a;
	submission_a.gaussian_data = stage1a_make_submission_test_data(3, 0.0f);
	submission_a.static_chunks.push_back(stage1a_make_submission_test_chunk(0));

	GaussianSplatSceneDirector::WorldSubmission submission_b;
	submission_b.owner_id = owner_id;
	submission_b.scenario = scenario_b;
	submission_b.gaussian_data = stage1a_make_submission_test_data(2, 20.0f);
	submission_b.static_chunks.push_back(stage1a_make_submission_test_chunk(1));

	CHECK(director->submit_world_submission(submission_a));
	GaussianSplatSceneDirector::WorldSubmission queried;
	CHECK(director->get_world_submission_for_scenario(scenario_a, &queried));
	CHECK(queried.owner_id == owner_id);

	CHECK(director->submit_world_submission(submission_b));

	// THE LOAD-BEARING ASSERTION. Scenario A's record must be gone: the owner has
	// moved to B, and leaving A active would mean one owner holding two world
	// submissions, with A's renderer still bound to a contract nobody owns.
	CHECK_FALSE(director->get_world_submission_for_scenario(scenario_a, &queried));

	CHECK(director->get_world_submission_for_scenario(scenario_b, &queried));
	CHECK(queried.owner_id == owner_id);
	CHECK(queried.gaussian_data == submission_b.gaussian_data);

	// Same claim counted rather than queried, so a regression that leaves A active
	// is caught even if the per-scenario getter changes shape.
	const GaussianSplatSceneDirector::SubmissionCounts after_move = director->get_submission_counts();
	CHECK(after_move.world_submissions == baseline_counts.world_submissions + 1);

	// And the owner resolves to B, not to a stale A.
	CHECK(director->get_world_submission(owner_id, &queried));
	CHECK(queried.scenario == scenario_b);

	director->release_world_submission(owner_id);
	CHECK_FALSE(director->get_world_submission(owner_id, &queried));
	const GaussianSplatSceneDirector::SubmissionCounts after_release = director->get_submission_counts();
	CHECK(after_release.world_submissions == baseline_counts.world_submissions);

	// Leave no SharedWorld behind for the tests that share this director.
	director->try_prune_world_if_unused(scenario_a);
	director->try_prune_world_if_unused(scenario_b);

	root->remove_child(owner);
	memdelete(owner);
	if (owns_director) {
		memdelete(director);
	}
}

// #675 — the RESTORE half of previous-world eviction, on a live RenderingDevice.
//
// WHAT THE HEADLESS TWIN ABOVE CANNOT REACH. The `[SceneTree]` test directly
// above pins the eviction's *record* half (`previous_world->world_submission` is
// cleared) — pure bookkeeping that needs no device. Its other half is
// `queue_restore(previous_world->renderer, ...)`, and headless
// `previous_world->renderer` is always null, so `queue_restore` early-outs, the
// queue stays empty, and nothing observable happens. That is the half three
// ordering defects in #611 PR B2 lived in, and it is the half this case covers.
//
// This is the same fixture with `[RequiresGPU]` and renderer-side assertions
// added, so it lands in run_gpu_harness.py's `SceneDirectorSceneTree` batch
// (filter `*[SceneDirector][WorldSubmission][SceneTree][RequiresGPU]*`) where a
// real device exists and both worlds get real renderers.
//
// THE ASSERTION SHAPE IS THE POINT. Every one of #611 PR B2's three ordering
// defects was invisible to assertions that checked only one side: the director's
// record and the renderer's installed contract must be checked TOGETHER, because
// each defect left exactly one of them right and the other wrong.
//
// MUTATION-CHECKED against the `cancel()` ordering in
// gaussian_splat_scene_director.cpp's commit branch. `cancel()` clears the ENTIRE
// queue, so moving it after the `queue_restore` below drops the restore: the
// evicted world's renderer keeps a contract nobody owns while the director's
// record for it is gone. With that mutation applied this case FAILS on
// `evicted_renderer_state.has_active_world_submission` (and on the gaussian_data
// check); the headless twin above still passes, which is precisely the gap.
TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Cross-scenario eviction restores the evicted world's renderer and clears its record") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (!director) {
		FAIL("GaussianSplatSceneDirector unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		FAIL("SceneTree singleton required");
		return;
	}
	Window *root = tree->get_root();
	if (!root) {
		FAIL("SceneTree root window required");
		return;
	}

	Ref<World3D> world_a = root->get_world_3d();
	if (world_a.is_null()) {
		FAIL("root World3D required");
		return;
	}
	// The second scenario. Without two DISTINCT scenarios `previous_world != world`
	// cannot hold and every assertion below would pass vacuously against the very
	// mutations this test exists to catch.
	Ref<World3D> world_b;
	world_b.instantiate();
	if (world_b.is_null()) {
		FAIL("could not instantiate a second World3D");
		return;
	}
	const RID scenario_a = world_a->get_scenario();
	const RID scenario_b = world_b->get_scenario();
	if (!scenario_a.is_valid() || !scenario_b.is_valid() || scenario_a == scenario_b) {
		FAIL("two distinct valid scenarios are required to reach the eviction branch");
		return;
	}

	// Renderer A up-front, so the pre-submission baseline the eviction must restore
	// TO is captured rather than assumed. World B is deliberately left alone: its
	// renderer is lazily created by submit_world_submission's phase 1, which is the
	// lazy-creation path #675 names.
	Ref<GaussianSplatRenderer> renderer_a = director->get_shared_renderer(world_a.ptr());
	if (renderer_a.is_null()) {
		FAIL("Shared renderer unavailable for scenario A. This case is [RequiresGPU] and "
			 "runs only under the --gs-gpu-test harness, which brings up a real "
			 "RenderingDevice. A null renderer here is a harness/product failure, not a "
			 "reason to skip -- the eviction's restore half cannot be observed without it.");
		return;
	}
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot baseline_a =
			renderer_a->snapshot_world_submission_runtime_state();
	CHECK(baseline_a.valid);
	// If a prior case in this batch left a contract installed on the shared root
	// renderer, the post-eviction comparison below would be against the wrong
	// baseline, so assert the starting point rather than trusting it.
	CHECK_FALSE(baseline_a.has_active_world_submission);

	Node *owner = memnew(Node);
	if (!owner) {
		FAIL("could not allocate the submission owner");
		return;
	}
	root->add_child(owner);
	tree->process(0.0);
	const ObjectID owner_id = owner->get_instance_id();

	// SAME owner, DIFFERENT scenarios -- the pairing that makes
	// _find_world_for_world_submission return world A while the commit targets B.
	GaussianSplatSceneDirector::WorldSubmission submission_a;
	submission_a.owner_id = owner_id;
	submission_a.scenario = scenario_a;
	submission_a.gaussian_data = stage1a_make_submission_test_data(3, 0.0f);
	submission_a.static_chunks.push_back(stage1a_make_submission_test_chunk(0));

	GaussianSplatSceneDirector::WorldSubmission submission_b;
	submission_b.owner_id = owner_id;
	submission_b.scenario = scenario_b;
	submission_b.gaussian_data = stage1a_make_submission_test_data(2, 20.0f);
	submission_b.static_chunks.push_back(stage1a_make_submission_test_chunk(1));

	CHECK(director->submit_world_submission(submission_a));

	// Both sides after the first commit, so the eviction below is known to start
	// from a state where the renderer and the record actually agree.
	GaussianSplatSceneDirector::WorldSubmission queried;
	CHECK(director->get_world_submission_for_scenario(scenario_a, &queried));
	CHECK(queried.owner_id == owner_id);
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot installed_a =
			renderer_a->snapshot_world_submission_runtime_state();
	CHECK(installed_a.has_active_world_submission);
	CHECK(installed_a.gaussian_data == submission_a.gaussian_data);

	// THE EVICTION. Phase 1 lazily creates world B's renderer; phase 3 commits B,
	// clears A's record, and queues the restore of A's renderer.
	CHECK(director->submit_world_submission(submission_b));

	Ref<GaussianSplatRenderer> renderer_b = director->get_shared_renderer(world_b.ptr());
	if (renderer_b.is_null()) {
		FAIL("Shared renderer unavailable for scenario B; the lazily-created-renderer path "
			 "under test did not produce a renderer.");
		root->remove_child(owner);
		memdelete(owner);
		if (owns_director) {
			memdelete(director);
		}
		return;
	}
	// Distinct renderers, or "the evicted world's renderer" and "the committing
	// world's renderer" would be the same object and the restore assertions below
	// would be asserting against the wrong renderer.
	CHECK(renderer_a.ptr() != renderer_b.ptr());

	// ---- LOAD-BEARING: the renderer side of the eviction. ----
	// This is what no headless lane can observe. A's renderer must be back at its
	// pre-submission baseline; if the queued restore was dropped it still holds A's
	// contract while A's record is gone.
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot evicted_renderer_state =
			renderer_a->snapshot_world_submission_runtime_state();
	CHECK(evicted_renderer_state.valid);
	CHECK_FALSE(evicted_renderer_state.has_active_world_submission);
	CHECK(evicted_renderer_state.gaussian_data.is_null());
	CHECK(evicted_renderer_state.gaussian_data == baseline_a.gaussian_data);
	CHECK(evicted_renderer_state.has_active_world_submission == baseline_a.has_active_world_submission);
	CHECK(evicted_renderer_state.has_desired_residency_hint == baseline_a.has_desired_residency_hint);
	CHECK(evicted_renderer_state.max_splats == baseline_a.max_splats);

	// ---- and the director side, which must agree with it. ----
	CHECK_FALSE(director->get_world_submission_for_scenario(scenario_a, &queried));

	// The committing world: record AND renderer both hold B.
	CHECK(director->get_world_submission_for_scenario(scenario_b, &queried));
	CHECK(queried.owner_id == owner_id);
	CHECK(queried.gaussian_data == submission_b.gaussian_data);
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot installed_b =
			renderer_b->snapshot_world_submission_runtime_state();
	CHECK(installed_b.has_active_world_submission);
	CHECK(installed_b.gaussian_data == submission_b.gaussian_data);

	// And the owner resolves to B, not to a stale A.
	CHECK(director->get_world_submission(owner_id, &queried));
	CHECK(queried.scenario == scenario_b);

	director->release_world_submission(owner_id);
	CHECK_FALSE(director->get_world_submission(owner_id, &queried));

	// Drop our renderer Refs BEFORE pruning, or _should_prune_world's
	// `get_reference_count() <= 1` test sees an inflated count and skips, leaving a
	// SharedWorld behind for the rest of the batch.
	renderer_a.unref();
	renderer_b.unref();
	// Scenario B's RID dies with the local `world_b` Ref, so its SharedWorld must go
	// first or the director keeps an entry keyed by a freed RID.
	director->teardown_world_for_scenario(scenario_b);
	director->try_prune_world_if_unused(scenario_a);

	root->remove_child(owner);
	memdelete(owner);
	tree->process(0.0);
	if (owns_director) {
		memdelete(director);
	}
}

// #675 — the REJECTION path's both-sides end state, on a live RenderingDevice.
//
// The reachable rejection is phase 1's arbitration reject: a DIFFERENT, still-live
// owner submitting to a scenario whose record is already held. It returns false
// before any renderer mutation, and its contract is that BOTH sides are left
// exactly as they were -- the incumbent's record stays active and the renderer
// keeps the incumbent's contract.
//
// That pairing is the point. A rejection that cleared the renderer while leaving
// the record active is the exact shape of #611 PR B2's third defect (found by
// review, not by any lane); headless it is invisible, because a device-less
// renderer has no contract to keep or lose.
//
// SCOPE, HONESTLY. Phase 3's rollback (`queue_restore_first`) is NOT exercised
// here and is not reachable from this or any other single-threaded test -- see the
// note in test_scene_director_renderer_contract_lock.h, which pins that ordering at
// the DeferredRendererWork level instead.
TEST_CASE("[GaussianSplatting][SceneDirector][WorldSubmission][SceneTree][RequiresGPU] Arbitration rejection leaves the incumbent's record and renderer contract intact") {
	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	if (!director) {
		FAIL("GaussianSplatSceneDirector unavailable");
		return;
	}

	SceneTree *tree = SceneTree::get_singleton();
	if (!tree) {
		FAIL("SceneTree singleton required");
		return;
	}
	Window *root = tree->get_root();
	if (!root) {
		FAIL("SceneTree root window required");
		return;
	}
	Ref<World3D> world = root->get_world_3d();
	if (world.is_null()) {
		FAIL("root World3D required");
		return;
	}
	const RID scenario = world->get_scenario();
	if (!scenario.is_valid()) {
		FAIL("valid scenario required");
		return;
	}

	Ref<GaussianSplatRenderer> renderer = director->get_shared_renderer(world.ptr());
	if (renderer.is_null()) {
		FAIL("Shared renderer unavailable. This case is [RequiresGPU] and runs only under "
			 "the --gs-gpu-test harness, which brings up a real RenderingDevice. Without a "
			 "renderer the 'renderer contract survives the rejection' half asserts nothing.");
		return;
	}
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot baseline =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(baseline.valid);
	CHECK_FALSE(baseline.has_active_world_submission);

	// Both owners stay in the tree, so _is_world_submission_owner_live() reports the
	// incumbent as live -- which is what makes phase 1 reject rather than evict.
	Node *incumbent = memnew(Node);
	Node *challenger = memnew(Node);
	if (!incumbent || !challenger) {
		FAIL("could not allocate the submission owners");
		if (incumbent) {
			memdelete(incumbent);
		}
		if (challenger) {
			memdelete(challenger);
		}
		return;
	}
	root->add_child(incumbent);
	root->add_child(challenger);
	tree->process(0.0);

	GaussianSplatSceneDirector::WorldSubmission incumbent_submission;
	incumbent_submission.owner_id = incumbent->get_instance_id();
	incumbent_submission.scenario = scenario;
	incumbent_submission.gaussian_data = stage1a_make_submission_test_data(4, 0.0f);
	incumbent_submission.static_chunks.push_back(stage1a_make_submission_test_chunk(0));

	GaussianSplatSceneDirector::WorldSubmission challenger_submission;
	challenger_submission.owner_id = challenger->get_instance_id();
	challenger_submission.scenario = scenario;
	challenger_submission.gaussian_data = stage1a_make_submission_test_data(2, 40.0f);
	challenger_submission.static_chunks.push_back(stage1a_make_submission_test_chunk(1));

	CHECK(director->submit_world_submission(incumbent_submission));
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot installed =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(installed.has_active_world_submission);
	CHECK(installed.gaussian_data == incumbent_submission.gaussian_data);

	// THE REJECTION.
	CHECK_FALSE(director->submit_world_submission(challenger_submission));

	// ---- Renderer side: still the incumbent's contract, untouched. ----
	const GaussianSplatRenderer::WorldSubmissionRuntimeStateSnapshot after_reject =
			renderer->snapshot_world_submission_runtime_state();
	CHECK(after_reject.valid);
	CHECK(after_reject.has_active_world_submission);
	CHECK(after_reject.gaussian_data == incumbent_submission.gaussian_data);
	// Specifically NOT the challenger's data: a rejection that applied first and
	// failed to roll back would show this.
	CHECK(after_reject.gaussian_data != challenger_submission.gaussian_data);
	CHECK(after_reject.max_splats == installed.max_splats);

	// ---- Director side: still the incumbent's record, and only one of them. ----
	GaussianSplatSceneDirector::WorldSubmission queried;
	CHECK(director->get_world_submission_for_scenario(scenario, &queried));
	CHECK(queried.owner_id == incumbent->get_instance_id());
	CHECK(queried.gaussian_data == incumbent_submission.gaussian_data);
	CHECK_FALSE(director->get_world_submission(challenger->get_instance_id(), &queried));

	director->release_world_submission(incumbent->get_instance_id());
	renderer.unref();
	director->try_prune_world_if_unused(scenario);

	root->remove_child(incumbent);
	root->remove_child(challenger);
	memdelete(incumbent);
	memdelete(challenger);
	tree->process(0.0);
	if (owns_director) {
		memdelete(director);
	}
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
