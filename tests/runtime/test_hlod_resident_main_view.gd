extends SceneTree

# Target-backed replacement for the positive C++ dummy-SceneTree case. All
# resources and framebuffers are owned by the normal native RD compositor.
const GsRuntimeReport := preload("gs_runtime_report.gd")
const SCENARIO := "Resident HLOD Main View"
const TIMEOUT_MSEC := 10_000
const CPU_ROUTE := "INSTANCE.CULL.HLOD_CPU"
const CPU_REASON := "hlod_cpu_view_admission_selection_upload"
const SAMPLE_KEYS := [
	"frame_count", "hlod_active", "hlod_reason", "hlod_selected_nodes",
	"hlod_selected_splats", "hlod_selected_parents", "hlod_source_generation",
	"hlod_content_generation", "hlod_view_epoch", "hlod_bound_source_generation",
	"hlod_bound_relative", "hlod_bound_atlas_valid", "hlod_canonical_atlas_valid",
	"hlod_bound_atlas_rid", "hlod_canonical_atlas_rid",
	"hlod_canonical_world_submission_active", "hlod_relative_world_submission_active",
	"renderer_device_object_id", "stage_metrics_valid", "stage_cull_route_uid",
	"cull_route_uid", "cull_route_reason", "hlod_cpu_cut_usec", "hlod_cpu_prepare_usec",
	"hlod_cpu_prepare_timing_scope", "stage_cull_time_ms", "culling_time_ms",
	"sort_route_uid", "stage_sort_sorted_count", "sorted_splats",
	"stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status",
]

var _report := GsRuntimeReport.new(SCENARIO)
var _scene: Node3D
var _node: GaussianSplatWorld3D
var _renderer: GaussianSplatRenderer
var _metrics: Dictionary = {"hlod_main_view_proof_status": "not_started", "samples": [], "orthographic_samples": []}
var _last_publication_engine_frame := -1

signal publication_observed(observation: Dictionary)


func _init() -> void:
	call_deferred("_run")


func _emit_metrics() -> void:
	# Full precision preserves the measured float32 stage times through JSON.
	print("%s %s" % [GsRuntimeReport.METRICS_MARKER, JSON.stringify(_metrics, "", false, true)])


func _cleanup() -> void:
	_renderer = null
	_node = null
	if _scene != null:
		_scene.queue_free()
	_scene = null


func _fail(reason: String) -> void:
	_metrics["hlod_main_view_proof_status"] = "failed"
	_metrics["reason"] = reason
	push_error("%s %s" % [GsRuntimeReport.FAIL_MARKER, reason])
	_emit_metrics()
	_cleanup()
	quit(1)


func _check(condition: bool, reason: String) -> bool:
	if not condition:
		_fail(reason)
		return false
	_report.ok()
	return true


func _f32(value: float) -> float:
	return PackedFloat32Array([value])[0]


func _check_sample(sample: Dictionary) -> bool:
	var exact := {
		"hlod_active": true, "hlod_reason": "active", "hlod_selected_nodes": 1,
		"hlod_selected_splats": 2, "hlod_selected_parents": 0,
		"hlod_bound_relative": false, "hlod_bound_atlas_valid": true,
		"hlod_canonical_atlas_valid": true, "hlod_canonical_world_submission_active": false,
		"hlod_relative_world_submission_active": false, "stage_metrics_valid": true,
		"stage_cull_route_uid": CPU_ROUTE, "cull_route_uid": CPU_ROUTE,
		"cull_route_reason": CPU_REASON,
		"hlod_cpu_prepare_timing_scope": "cpu_view_admission_selection_draw_upload",
		"sort_route_uid": "INSTANCE.SORT.GPU", "stage_sort_sorted_count": 2,
		"sorted_splats": 2, "rendered_content": true,
		"stage_cull_status": "success", "stage_sort_status": "success",
		"stage_raster_status": "success", "stage_composite_status": "success",
	}
	for key in exact:
		if not _check(sample[key] == exact[key], "%s: expected %s, got %s" % [key, exact[key], sample[key]]):
			return false
	# ObjectID/RID are signed Variant integers; negative values can be valid.
	for key in ["hlod_source_generation", "hlod_view_epoch", "hlod_bound_atlas_rid", "hlod_canonical_atlas_rid", "renderer_device_object_id", "server_device_object_id"]:
		if not _check(int(sample[key]) != 0, "%s must be nonzero" % key):
			return false
	if not _check(sample["hlod_bound_atlas_rid"] == sample["hlod_canonical_atlas_rid"], "Canonical atlas was not restored"):
		return false
	if not _check(sample["hlod_bound_source_generation"] == sample["hlod_source_generation"], "Restored bundle has a different source generation"):
		return false
	if not _check(sample["renderer_device_object_id"] == sample["server_device_object_id"], "Renderer and actual server RD differ"):
		return false
	var prepare_usec := int(sample["hlod_cpu_prepare_usec"])
	var cut_usec := int(sample["hlod_cpu_cut_usec"])
	if not _check(cut_usec >= 0 and prepare_usec >= cut_usec, "CPU preparation must contain selection duration"):
		return false
	# Match C++ float(usec) * 0.001f, including BOTH float32 rounding steps.
	# Zero ticks are legal for this tiny fixture; no artificial minimum.
	var expected_ms := _f32(_f32(float(prepare_usec)) * _f32(0.001))
	if not _check(float(sample["stage_cull_time_ms"]) == expected_ms, "Stage time differs from measured CPU preparation"):
		return false
	return _check(sample["culling_time_ms"] == sample["stage_cull_time_ms"], "Stage and performance CPU times differ")


func _check_orthographic_sample(sample: Dictionary, last_positive: Dictionary) -> bool:
	var exact := {
		"camera_projection": Camera3D.PROJECTION_ORTHOGONAL,
		"hlod_active": false, "hlod_reason": "unsupported_view_or_material",
		"hlod_selected_nodes": 0, "hlod_selected_splats": 0, "hlod_selected_parents": 0,
		"hlod_bound_relative": false, "hlod_bound_atlas_valid": true,
		"hlod_canonical_atlas_valid": true, "hlod_canonical_world_submission_active": false,
		"hlod_relative_world_submission_active": false, "stage_metrics_valid": true,
		"stage_cull_route_uid": "INSTANCE.CULL.GPU", "cull_route_uid": "INSTANCE.CULL.GPU",
		"sort_route_uid": "INSTANCE.SORT.GPU", "stage_sort_sorted_count": 2,
		"sorted_splats": 2, "rendered_content": true,
		"stage_cull_status": "success", "stage_sort_status": "success",
		"stage_raster_status": "success", "stage_composite_status": "success",
	}
	for key in exact:
		if not _check(sample[key] == exact[key], "Orthographic %s: expected %s, got %s" % [key, exact[key], sample[key]]):
			return false
	# The prepared source and valid restored atlas must survive this view-only
	# transition. Rejection precedes view-history acquisition; no new epoch or
	# HLOD CPU-route timing is expected on the ordinary canonical GPU route.
	for key in ["hlod_source_generation", "hlod_bound_source_generation", "hlod_bound_atlas_rid", "hlod_canonical_atlas_rid", "renderer_device_object_id", "server_device_object_id"]:
		if not _check(int(sample[key]) != 0 and sample[key] == last_positive[key], "Orthographic %s differs from the admitted perspective identity" % key):
			return false
	for key in ["engine_frame", "frame_count"]:
		if not _check(int(sample[key]) > int(last_positive[key]), "Orthographic %s is not newer than the perspective proof" % key):
			return false
	return true


func _check_orthographic_fallback(camera: Camera3D, rd: RenderingDevice, last_positive: Dictionary, deadline: int) -> bool:
	camera.set_orthogonal(2.0, 0.1, 100.0)
	var previous: Dictionary = {}
	while Time.get_ticks_msec() < deadline:
		await RenderingServer.frame_post_draw
		if Time.get_ticks_msec() >= deadline:
			break
		var stats := _renderer.get_render_stats()
		# A post-draw wake can still expose the previous renderer observation.
		# It cannot count as either of the two new fallback frames.
		if Engine.get_frames_drawn() <= int(last_positive["engine_frame"]) or int(stats.get("frame_count", -1)) <= int(last_positive["frame_count"]):
			continue
		var sample: Dictionary = {}
		for key in SAMPLE_KEYS:
			if not _check(stats.has(key), "Missing orthographic renderer field: %s" % key):
				return false
			sample[key] = stats[key]
		sample["engine_frame"] = Engine.get_frames_drawn()
		sample["server_device_object_id"] = rd.get_instance_id()
		sample["rendered_content"] = _renderer.has_rendered_content()
		sample["camera_projection"] = camera.projection
		_metrics["orthographic_samples"] = [sample] if previous.is_empty() else [previous, sample]
		if not _check_orthographic_sample(sample, last_positive):
			return false
		if previous.is_empty() or int(sample["engine_frame"]) != int(previous["engine_frame"]) + 1 or int(sample["frame_count"]) != int(previous["frame_count"]) + 1:
			previous = sample
			continue
		return _check(Time.get_ticks_msec() < deadline, "Orthographic proof completed after its deadline")
	_fail("No two consecutive fresh orthographic fallback frames within deadline")
	return false


func _deliver_publication_observation(observation: Dictionary) -> void:
	publication_observed.emit(observation)


func _read_publication_on_render_thread(foreign_device: bool) -> void:
	var observation: Dictionary = _renderer.test_hlod_foreign_device_publication() if foreign_device else _renderer.test_observe_hlod_publication()
	_deliver_publication_observation.call_deferred(observation)


func _observe_publication(foreign_device := false) -> Dictionary:
	RenderingServer.call_on_render_thread(_read_publication_on_render_thread.bind(foreign_device))
	return await publication_observed


func _check_current_publication(observation: Dictionary, source: GaussianData, count: int) -> bool:
	if not _check(not observation.has("error"), "Actual publication readback failed: %s" % observation.get("error", "")):
		return false
	for pair in [["atlas_hex", "source_leaf_hex"], ["instance_hex", "expected_instance_hex"], ["grading_hex", "expected_grading_hex"]]:
		if not _check(not str(observation.get(pair[0], "")).is_empty() and observation.get(pair[0]) == observation.get(pair[1]), "Current %s differs from actual source/instance/grading" % pair[0]):
			return false
	if not _check(observation["source_data_id"] == source.get_instance_id(), "Publication belongs to an obsolete source"):
		return false
	if not _check(observation["input_current"] and int(observation["input_source_generation"]) != 0, "Current HLOD input snapshot missing"):
		return false
	return _check(observation["bound_atlas_count"] == count and observation["source_leaf_count"] == count, "Current complete leaf atlas missing")


func _wait_publication(source: GaussianData, count: int, active: bool, reason: String, deadline: int, after_frame: int) -> Dictionary:
	while Time.get_ticks_msec() < deadline:
		await RenderingServer.frame_post_draw
		var observation := await _observe_publication()
		if Time.get_ticks_msec() >= deadline:
			break
		if observation.has("error"):
			continue # A genuinely absent current contract is warmup, never success.
		var stats: Dictionary = observation["stats"]
		if int(stats["frame_count"]) <= after_frame or int(observation["engine_frame"]) <= _last_publication_engine_frame:
			continue
		# As soon as the renderer has current B, ANY bound A payload is a failure,
		# including observations before the requested fallback/recovery completes.
		if observation["source_data_id"] == source.get_instance_id():
			if not _check_current_publication(observation, source, count):
				return {}
		else:
			continue
		if bool(stats["hlod_active"]) != active or stats["hlod_reason"] != reason:
			continue
		var stages_succeeded := bool(stats["stage_metrics_valid"])
		for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
			stages_succeeded = stages_succeeded and stats[key] == "success"
		if not stages_succeeded:
			continue
		if not _check(observation["rendered_content"] and stats["sort_route_uid"] == "INSTANCE.SORT.GPU" and not stats["hlod_bound_relative"], "Current route did not render and restore canonically"):
			return {}
		if not _check(bool(observation["prepared_current"]) == active or reason == "lod_disabled_canonical_leaves", "Prepared eligibility differs from publication phase"):
			return {}
		if not active and reason != "lod_disabled_canonical_leaves":
			if not _check(int(observation["prepared_source_generation"]) == 0 and int(observation["failure_hits"]) > 0, "Failure seam did not invalidate the previous prepared route"):
				return {}
		if active:
			if not _check(int(observation["prepared_source_generation"]) == int(observation["input_source_generation"]) and int(stats["hlod_selected_splats"]) > 0 and stats["stage_sort_sorted_count"] == stats["hlod_selected_splats"] and stats["sorted_splats"] == stats["hlod_selected_splats"], "Prepared cut did not reach the actual GPU sort"):
				return {}
		if not active:
			if not _check(stats["hlod_selected_nodes"] == 0 and stats["hlod_selected_splats"] == 0 and stats["hlod_selected_parents"] == 0 and stats["stage_sort_sorted_count"] == count and stats["sorted_splats"] == count, "Canonical fallback did not render current complete leaves"):
				return {}
		_last_publication_engine_frame = int(observation["engine_frame"])
		return observation
	_fail("Publication transition did not complete within deadline: %s" % reason)
	return {}


func _transition_data(serial: int, count := 2) -> GaussianData:
	var data := GaussianData.new()
	data.resize(count)
	var positions := PackedVector3Array()
	var scales := PackedVector3Array()
	var rotations: Array[Quaternion] = []
	var opacities := PackedFloat32Array()
	var coefficients := PackedFloat32Array()
	for i in range(count):
		positions.append(Vector3(0.125 + i * 0.125 + serial * 0.03125, 0.125 + (i % 2) * 0.25, -4.0))
		scales.append(Vector3(0.0625, 0.0625, 0.0625))
		rotations.append(Quaternion())
		opacities.append(0.75)
		# Genuine SH3: DC, first order and all twelve higher-order coefficients.
		for coefficient in range(16 * 3):
			coefficients.append((serial + 1) * 0.015625 + (coefficient % 7) * 0.001953125)
	data.set_positions(positions)
	data.set_scales(scales)
	data.set_rotations(rotations)
	data.set_opacities(opacities)
	data.set_spherical_harmonics(coefficients)
	return data


func _replace_transition_world(serial: int) -> GaussianSplatWorld:
	var world := GaussianSplatWorld.new()
	world.gaussian_data = _transition_data(serial)
	if not _check(world.bake_hlod() == OK and world.has_hlod_tree(), "Actual transition source bake failed"):
		return null
	_node.world = world
	_node.apply_world()
	return world


func _run_publication_and_lod_proof(camera: Camera3D, deadline: int) -> bool:
	for method in ["test_set_hlod_publication_failure", "test_observe_hlod_publication", "test_hlod_foreign_device_publication", "test_set_hlod_grading", "test_bake_hlod_small_leaf_fixture"]:
		if not _check(_renderer.has_method(method), "Required native test hook missing: %s" % method):
			return false
	camera.set_perspective(70.0, 0.1, 100.0)
	_last_publication_engine_frame = int(_metrics["orthographic_samples"][-1]["engine_frame"])
	_metrics["publication_transitions"] = []
	_metrics["identity_transitions"] = []
	var world := _replace_transition_world(1)
	if world == null:
		return false
	var before := await _wait_publication(world.gaussian_data, 2, true, "active", deadline, int(_metrics["orthographic_samples"][-1]["frame_count"]))
	if before.is_empty():
		return false
	var failure_reasons := ["canonical_sort_capacity_warmup", "resource_ledger_warmup_or_unavailable", "hlod_candidate_memory_limit", "hlod_candidate_allocation_failed", "hlod_candidate_readiness_or_memory_failed"]
	for failure in range(1, 6):
		_renderer.test_set_hlod_publication_failure(failure)
		world = _replace_transition_world(failure + 1)
		if world == null:
			return false
		var refused := await _wait_publication(world.gaussian_data, 2, false, failure_reasons[failure - 1], deadline, int(before["stats"]["frame_count"]))
		if refused.is_empty():
			return false
		if not _check(refused["atlas_hex"] != before["atlas_hex"] and refused["source_data_id"] != before["source_data_id"], "A-to-B fixture failed to discriminate payload identity"):
			return false
		if not _check(refused["source_first_order_count"] == 3 and refused["source_high_order_count"] == 12, "Transition must preserve complete SH3 payload"):
			return false
		_renderer.test_set_hlod_publication_failure(0)
		var recovered := await _wait_publication(world.gaussian_data, 2, true, "active", deadline, int(refused["stats"]["frame_count"]))
		if recovered.is_empty():
			return false
		if not _check(recovered["atlas_hex"] == refused["atlas_hex"] and recovered["input_source_generation"] == refused["input_source_generation"], "Recovery changed current B identity or payload"):
			return false
		_metrics["publication_transitions"].append({"failure": failure, "before": before, "refused": refused, "recovered": recovered})
		before = recovered
	# Instance, grading-default and draw-capacity changes with the same source
	# must also invalidate retained contracts before a failed rebuild.
	var previous_max := _renderer.get_max_splats()
	for change in ["instance", "grading", "capacity"]:
		_renderer.test_set_hlod_publication_failure(4)
		if change == "instance":
			# The world shim is identity even when world opacity changes. Exercise
			# the actual director generation by adding/removing a real instance,
			# then recover the exclusive world before another frame is rendered.
			var asset := GaussianSplatAsset.new()
			asset.set_splat_count(1)
			asset.set_positions(PackedFloat32Array([0.125, 0.125, -4.0]))
			asset.set_scales(PackedFloat32Array([0.0625, 0.0625, 0.0625]))
			asset.set_rotations(PackedFloat32Array([0.0, 0.0, 0.0, 1.0]))
			var transient := GaussianSplatNode3D.new()
			transient.set_splat_asset(asset)
			_scene.add_child(transient)
			transient.force_update()
			_scene.remove_child(transient)
			transient.free()
			_node.apply_world()
		elif change == "grading":
			var grading := ColorGradingResource.new()
			grading.exposure = 0.25
			_renderer.test_set_hlod_grading(grading)
		else:
			_renderer.set_max_splats(1)
		var refused := await _wait_publication(world.gaussian_data, 2, false, "hlod_candidate_allocation_failed", deadline, int(before["stats"]["frame_count"]))
		if refused.is_empty():
			return false
		if change == "instance" and not _check(int(refused["current_instance_generation"]) > int(before["current_instance_generation"]), "Actual director instance generation did not advance"):
			return false
		if change == "grading" and not _check(refused["grading_hex"] != before["grading_hex"], "Grading control did not change real GPU grading data"):
			return false
		if change == "capacity":
			if not _check(refused["relative_capacity"] == 1, "Capacity control did not take effect"):
				return false
			_renderer.set_max_splats(previous_max)
		_renderer.test_set_hlod_publication_failure(0)
		var recovered := await _wait_publication(world.gaussian_data, 2, true, "active", deadline, int(refused["stats"]["frame_count"]))
		if recovered.is_empty():
			return false
		_metrics["identity_transitions"].append({"change": change, "before": before, "refused": refused, "recovered": recovered})
		before = recovered
	var foreign := await _observe_publication(true)
	if not _check(not foreign.has("error") and int(foreign.get("original_device", 0)) != 0 and int(foreign.get("foreign_device", 0)) != 0 and foreign.get("original_device") != foreign.get("foreign_device"), "Actual second RenderingDevice probe missing"):
		return false
	if not _check(not foreign["publication_succeeded"] and not foreign["prepared_retained"], "Foreign device published or retained first-device HLOD resources"):
		return false
	var device_recovered := await _wait_publication(world.gaussian_data, 2, true, "active", deadline, int(before["stats"]["frame_count"]))
	if device_recovered.is_empty():
		return false
	_metrics["device_transition"] = {"before": before, "foreign": foreign, "recovered": device_recovered}
	# Existing real baker, only a smaller leaf size for a bounded multi-level fixture.
	var parent_world := _renderer.test_bake_hlod_small_leaf_fixture(_transition_data(0, 8)) as GaussianSplatWorld
	if not _check(parent_world != null and parent_world.has_hlod_tree(), "Actual multi-level fixture bake failed"):
		return false
	_node.world = parent_world
	_node.set_hlod_tau_px(10000.0)
	_node.apply_world()
	var lod_on := await _wait_publication(parent_world.gaussian_data, 8, true, "active", deadline, int(device_recovered["stats"]["frame_count"]))
	if lod_on.is_empty():
		return false
	if not _check(int(lod_on["stats"]["hlod_selected_parents"]) > 0 and int(lod_on["stats"]["hlod_selected_splats"]) < 8, "LOD-on fixture did not select a reduced parent representation"):
		return false
	_node.set_lod_enabled(false)
	var lod_off := await _wait_publication(parent_world.gaussian_data, 8, false, "lod_disabled_canonical_leaves", deadline, int(lod_on["stats"]["frame_count"]))
	if lod_off.is_empty():
		return false
	_node.set_lod_enabled(true)
	var lod_reenabled := await _wait_publication(parent_world.gaussian_data, 8, true, "active", deadline, int(lod_off["stats"]["frame_count"]))
	if lod_reenabled.is_empty():
		return false
	if not _check(int(lod_reenabled["stats"]["hlod_selected_parents"]) > 0 and int(lod_reenabled["stats"]["hlod_selected_splats"]) < 8, "LOD re-enable did not restore parent selection"):
		return false
	_metrics["lod_policy_transition"] = {"on": lod_on, "off": lod_off, "reenabled": lod_reenabled}
	return _check(Time.get_ticks_msec() < deadline, "Publication and LOD proof completed after its deadline")


func _run() -> void:
	# A bare --script can start the editor's project manager and still render
	# this scene behind its UI. Assert actual context before creating anything.
	_metrics["project_file_present"] = FileAccess.file_exists("res://project.godot")
	_metrics["project_path"] = ProjectSettings.globalize_path("res://")
	_metrics["editor_hint"] = Engine.is_editor_hint()
	_metrics["initial_root_children"] = get_root().get_child_count()
	if not _check(_metrics["project_file_present"] and not _metrics["editor_hint"], "A native game project, without editor or project manager, is required"):
		return
	if not _check(_metrics["initial_root_children"] == 0, "Foreign root children would contaminate the main-view proof"):
		return
	var rd := RenderingServer.get_rendering_device()
	if not _check(rd != null and not OS.has_feature("headless"), "Native RD main-view runtime required"):
		return
	# This process owns these settings; nothing is saved to the project.
	ProjectSettings.set_setting("rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled", true)
	ProjectSettings.set_setting("rendering/gaussian_splatting/streaming/route_policy", 0)
	ProjectSettings.set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true)
	ProjectSettings.set_setting("rendering/gaussian_splatting/sorting/force_cpu_sort", false)
	ProjectSettings.set_setting("rendering/gaussian_splatting/compression/per_chunk_quantization", 0)
	ProjectSettings.emit_signal("settings_changed")
	get_root().size = Vector2i(64, 64)
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED
	RenderingServer.set_default_clear_color(Color.BLACK)

	var data := GaussianData.new()
	data.resize(2)
	data.set_positions(PackedVector3Array([Vector3(0.125, 0.125, -4.0), Vector3(0.375, 0.125, -4.0)]))
	data.set_scales(PackedVector3Array([Vector3(0.125, 0.125, 0.125), Vector3(0.125, 0.125, 0.125)]))
	var rotations: Array[Quaternion] = [Quaternion(), Quaternion()]
	data.set_rotations(rotations)
	data.set_opacities(PackedFloat32Array([0.75, 0.75]))
	data.set_spherical_harmonics(PackedFloat32Array([0.0, 0.0, 0.0, 0.0, 0.0, 0.0]))
	var world := GaussianSplatWorld.new()
	world.gaussian_data = data
	if not _check(world.bake_hlod() == OK and world.has_hlod_tree(), "Actual two-splat HLOD bake failed"):
		return
	_scene = Node3D.new()
	get_root().add_child(_scene)
	var camera := Camera3D.new()
	camera.fov = 70.0
	camera.near = 0.1
	camera.far = 100.0
	_scene.add_child(camera)
	camera.make_current()
	_node = GaussianSplatWorld3D.new()
	_node.set_auto_apply_on_ready(false)
	_node.world = world
	_scene.add_child(_node)
	_node.apply_world()
	var deadline := Time.get_ticks_msec() + TIMEOUT_MSEC
	var previous: Dictionary = {}
	while Time.get_ticks_msec() < deadline:
		await RenderingServer.frame_post_draw
		if Time.get_ticks_msec() >= deadline:
			break
		_renderer = _node.get_renderer()
		if _renderer == null:
			continue
		var stats := _renderer.get_render_stats()
		_metrics["last_hlod_reason"] = stats.get("hlod_reason", "missing")
		if not bool(stats.get("hlod_active", false)):
			previous = {}
			continue
		var sample: Dictionary = {}
		for key in SAMPLE_KEYS:
			if not _check(stats.has(key), "Missing actual renderer field: %s" % key):
				return
			sample[key] = stats[key]
		sample["engine_frame"] = Engine.get_frames_drawn()
		sample["server_device_object_id"] = rd.get_instance_id()
		sample["rendered_content"] = _renderer.has_rendered_content()
		_metrics["samples"] = [sample] if previous.is_empty() else [previous, sample]
		if not _check_sample(sample):
			return
		if previous.is_empty() or int(sample["engine_frame"]) != int(previous["engine_frame"]) + 1 or int(sample["frame_count"]) != int(previous["frame_count"]) + 1:
			previous = sample
			continue
		for key in ["hlod_source_generation", "hlod_view_epoch", "hlod_canonical_atlas_rid", "server_device_object_id"]:
			if not _check(sample[key] == previous[key], "%s changed across consecutive static frames" % key):
				return
		if not _check(int(sample["hlod_content_generation"]) > int(previous["hlod_content_generation"]), "Cut publication did not advance"):
			return
		_metrics["capture_root_children"] = get_root().get_child_count()
		_metrics["capture_scene_is_only_root_child"] = get_root().get_child_count() == 1 and get_root().get_child(0) == _scene
		if not _check(_metrics["capture_scene_is_only_root_child"], "Capture root contains content outside the test scene"):
			return
		var image := get_root().get_texture().get_image()
		if not _check(image != null and not image.is_empty(), "Actual post-draw target capture missing"):
			return
		# Preserve a real image even if dimensions or visible signal fail below.
		var path := "user://resident_hlod_main_view_%d.png" % OS.get_process_id()
		if not _check(image.save_png(path) == OK, "Cannot preserve actual viewport capture"):
			return
		_metrics["image_path"] = ProjectSettings.globalize_path(path)
		_metrics["image_sha256"] = FileAccess.get_sha256(path)
		_metrics["image_width"] = image.get_width()
		_metrics["image_height"] = image.get_height()
		if not _check(image.get_size() == Vector2i(64, 64), "Actual target must be exactly 64x64"):
			return
		# Functional signal only: positive RGB from the scene, plus unchanged
		# black background. No luma tolerance or per-splat/SH/quality claim.
		var nonblack_rgb_pixels := 0
		for y in range(image.get_height()):
			for x in range(image.get_width()):
				var pixel := image.get_pixel(x, y)
				if pixel.r != 0.0 or pixel.g != 0.0 or pixel.b != 0.0:
					nonblack_rgb_pixels += 1
		_metrics["image_nonblack_rgb_pixels"] = nonblack_rgb_pixels
		if not _check(nonblack_rgb_pixels > 0 and nonblack_rgb_pixels < 64 * 64, "Actual target must contain scene RGB signal and black background"):
			return
		# Keep the complete perspective samples and image above. The same real
		# World3D then exercises the late view gate with its prepared source intact.
		var fallback_proved := await _check_orthographic_fallback(camera, rd, sample, deadline)
		if not fallback_proved:
			return
		var transitions_proved := await _run_publication_and_lod_proof(camera, deadline)
		if not transitions_proved:
			return
		if not _check(Time.get_ticks_msec() < deadline, "Proof completed after its deadline"):
			return
		_metrics["hlod_main_view_proof_status"] = "passed"
		_emit_metrics()
		_report.emit_pass()
		_cleanup()
		quit(0)
		return
	_fail("No two consecutive admitted main-view frames within deadline; last reason: %s" % _metrics.get("last_hlod_reason", "renderer unavailable"))
