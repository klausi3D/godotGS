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


func _read_publication_on_render_thread(foreign_device: bool, request_id: int) -> void:
	# Only the initial render-thread hook submits copies. Bracket that call here;
	# native metadata separately freezes its actual renderer-frame identity.
	var copy_engine_frame := Engine.get_frames_drawn() if not foreign_device and request_id == 0 else -1
	var observation: Dictionary = _renderer.test_hlod_foreign_device_publication() if foreign_device else _renderer.test_observe_hlod_publication(request_id)
	if not foreign_device and request_id == 0 and bool(observation.get("pending", false)):
		if Engine.get_frames_drawn() != copy_engine_frame:
			observation = {"error": "engine_frame_changed_during_publication_copy", "fatal": true}
		else:
			observation["copy_engine_frame"] = copy_engine_frame
	_deliver_publication_observation.call_deferred(observation)


func _observe_publication(deadline: int, foreign_device := false) -> Dictionary:
	var request_id := 0
	var copy_engine_frame := -1
	while Time.get_ticks_msec() < deadline:
		RenderingServer.call_on_render_thread(_read_publication_on_render_thread.bind(foreign_device, request_id))
		var observation: Dictionary = await publication_observed
		if Time.get_ticks_msec() >= deadline:
			return {"error": "publication_observation_deadline"}
		if not bool(observation.get("pending", false)):
			if not foreign_device and not observation.has("error"):
				if request_id <= 0 or int(observation.get("observation_request_id", 0)) != request_id or copy_engine_frame <= 0:
					return {"error": "publication_observation_request_mismatch", "fatal": true}
				# Preserve the initial copy interval across all callbacks/polls.
				observation["engine_frame"] = copy_engine_frame
			return observation
		var observed_request := int(observation.get("observation_request_id", 0))
		if observed_request <= 0 or (request_id != 0 and request_id != observed_request):
			return {"error": "publication_observation_request_mismatch", "fatal": true}
		if request_id == 0:
			copy_engine_frame = int(observation.get("copy_engine_frame", -1))
			if copy_engine_frame <= 0:
				return {"error": "publication_copy_engine_frame_missing", "fatal": true}
		request_id = observed_request
		# Keep the actual renderer/GPU moving; no blocking readback or fresh timeout.
		await RenderingServer.frame_post_draw
	return {"error": "publication_observation_deadline"}


func _check_current_publication(observation: Dictionary, source: GaussianData, count: int) -> bool:
	if not _check(not observation.has("error"), "Actual publication readback failed: %s" % observation.get("error", "")):
		return false
	if not _check(observation.get("readback_complete", false) and int(observation.get("readback_completed_mask", 0)) == 15 and int(observation.get("observation_request_id", 0)) > 0 and int(observation.get("copy_renderer_frame", -1)) == int(observation["stats"]["frame_count"]) and int(observation.get("readback_poll_renderer_frame", -1)) >= int(observation.get("copy_renderer_frame", 0)), "Complete matching asynchronous GPU snapshot missing"):
		return false
	for pair in [["atlas_hex", "source_leaf_hex"], ["instance_hex", "expected_instance_hex"], ["grading_hex", "expected_grading_hex"]]:
		if not _check(not str(observation.get(pair[0], "")).is_empty() and observation.get(pair[0]) == observation.get(pair[1]), "Current %s differs from actual source/instance/grading" % pair[0]):
			return false
	if not _check(observation["source_data_id"] == source.get_instance_id(), "Publication belongs to an obsolete source"):
		return false
	if not _check(observation["input_current"] and int(observation["input_source_generation"]) != 0, "Current HLOD input snapshot missing"):
		return false
	return _check(observation["bound_atlas_count"] == count and observation["source_leaf_count"] == count, "Current complete leaf atlas missing")


func _check_current_gpu_count(observation: Dictionary, expected_count: int, active: bool) -> bool:
	var gpu: Dictionary = observation.get("gpu_sort_count", {})
	var stats: Dictionary = observation["stats"]
	var header := str(gpu.get("header_hex", "")).hex_decode()
	if not _check(gpu.get("header_bytes", 0) == 24 and header.size() == 24, "Complete actual IndirectDispatchLayout readback missing"):
		return false
	var fields := ["dispatch_x", "dispatch_y", "dispatch_z", "element_count", "overflow_flag", "unclamped_total"]
	var expected := [ceili(float(expected_count) / 256.0), 1, 1, expected_count, 0, expected_count]
	for i in range(fields.size()):
		if not _check(gpu.get(fields[i], -1) == expected[i] and header.decode_u32(i * 4) == expected[i], "Actual GPU %s does not match this complete visible fixture" % fields[i]):
			return false
	for key in ["source_data_id", "input_source_generation", "device_instance_id"]:
		if not _check(int(gpu.get(key, 0)) != 0 and gpu[key] == observation[key], "Actual GPU count belongs to another %s" % key):
			return false
	if not _check(int(gpu.get("count_buffer_rid", 0)) != 0 and gpu.get("relative") == active and gpu.get("renderer_frame") == stats["frame_count"] and int(gpu.get("pass_frame", -2)) + 1 == int(stats["frame_count"]), "Actual GPU count route/frame is stale"):
		return false
	var async_count: Dictionary = observation.get("async_sort_count", {})
	return _check(async_count.has("valid") and int(async_count.get("sample_pass_frame", -1)) >= 0 and int(async_count["sample_pass_frame"]) <= int(gpu["pass_frame"]) and int(async_count.get("element_count", -1)) >= 0, "Separate async count provenance missing")


func _wait_publication(source: GaussianData, count: int, active: bool, reason: String, deadline: int, after_frame: int, instance_before: Dictionary = {}) -> Dictionary:
	while Time.get_ticks_msec() < deadline:
		await RenderingServer.frame_post_draw
		var observation := await _observe_publication(deadline)
		if Time.get_ticks_msec() >= deadline:
			break
		if observation.has("error"):
			if bool(observation.get("fatal", false)):
				_fail("Admitted publication readback failed: %s" % observation["error"])
				return {}
			continue # Only a genuinely absent or obsolete contract is warmup.
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
		if not instance_before.is_empty():
			for key in ["source_data_id", "source_revision", "source_leaf_hex", "input_source_generation", "device_instance_id", "current_grading_generation", "grading_hex", "relative_capacity"]:
				if not _check(observation[key] == instance_before[key], "Instance-generation probe changed another identity: %s" % key):
					return {}
			if not _check(int(observation["current_instance_generation"]) > int(instance_before["current_instance_generation"]), "Actual director instance generation did not advance"):
				return {}
			if not _check(not observation["prepared_current"], "Changed instance generation retained the prepared HLOD route under an armed rebuild failure"):
				return {}
		if bool(stats["hlod_active"]) != active or stats["hlod_reason"] != reason:
			continue
		var stages_succeeded := bool(stats["stage_metrics_valid"])
		for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
			stages_succeeded = stages_succeeded and stats[key] == "success"
		if not stages_succeeded:
			continue
		_metrics["last_publication_observation"] = observation
		if not _check(observation["rendered_content"] and stats["sort_route_uid"] == "INSTANCE.SORT.GPU" and not stats["hlod_bound_relative"], "Current route did not render and restore canonically"):
			return {}
		if not _check(bool(observation["prepared_current"]) == active or reason == "lod_disabled_canonical_leaves", "Prepared eligibility differs from publication phase"):
			return {}
		if not active and reason != "lod_disabled_canonical_leaves":
			if not _check(int(observation["prepared_source_generation"]) == 0 and int(observation["failure_hits"]) > 0, "Failure seam did not invalidate the previous prepared route"):
				return {}
		if active:
			if not _check(int(observation["prepared_source_generation"]) == int(observation["input_source_generation"]) and int(stats["hlod_selected_splats"]) > 0, "Prepared cut identity or selected payload missing"):
				return {}
		if not active:
			if not _check(stats["hlod_selected_nodes"] == 0 and stats["hlod_selected_splats"] == 0 and stats["hlod_selected_parents"] == 0, "Canonical fallback retained a selected cut"):
				return {}
		# These statistics retain their asynchronous CPU-sample semantics. Require
		# exact current GPU coverage from the actual route's count buffer instead.
		if not _check_current_gpu_count(observation, int(stats["hlod_selected_splats"]) if active else count, active):
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
			# The world row is identity even when world opacity changes. Exercise
			# the actual director generation by adding/removing a real instance.
			# Since PR1b a node instance no longer takes the world off the HLOD
			# route (it is a row of the same bundle); adding one still changes the
			# asset set, which is what forces this rebuild (and so the armed
			# failure). The node is gone again before another frame is rendered,
			# so the recovered route is the world alone.
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
			# Keep the admitted world/snapshot unchanged: reapplying the world
			# would independently invalidate it and mask a missing instance guard.
		elif change == "grading":
			var grading := ColorGradingResource.new()
			grading.exposure = 0.25
			_renderer.test_set_hlod_grading(grading)
		else:
			_renderer.set_max_splats(1)
		var refused := await _wait_publication(world.gaussian_data, 2, false, "hlod_candidate_allocation_failed", deadline, int(before["stats"]["frame_count"]), before if change == "instance" else {})
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
	var foreign := await _observe_publication(deadline, true)
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
		# PR1b: the world and 8 moving node instances in one frame (own wall-clock bound, WI_TIMEOUT_MSEC per mode).
		var world_instances_proved := await _run_world_plus_instances_proof()
		if not world_instances_proved:
			return
		_metrics["hlod_main_view_proof_status"] = "passed"
		_emit_metrics()
		_report.emit_pass()
		_cleanup()
		quit(0)
		return
	_fail("No two consecutive admitted main-view frames within deadline; last reason: %s" % _metrics.get("last_hlod_reason", "renderer unavailable"))


# ---------------------------------------------------------------------------------------------------------------------
# PR1b (docs/architecture/adr-one-route-many-producers.md): the world and 8 moving GaussianSplatNode3D instances draw in
# one frame, one draw list and one depth sort, on a V4 paged world and on a whole-tree resident world. Fail-closed: every
# stat it reads must exist (a missing key is a failure), every frame is checked, and the first failure ends the scenario.
# A red wall at z = -10; instances 0-3 in front (z = -7), 4-7 behind (z = -13), then swapped. A front instance's centre
# pixel shows its own colour, a behind instance's centre pixel the wall's red: both verdicts flip on the swap. Moving
# nodes take the row re-upload path with 0 bundle rebuilds. A budget phase (the view's splat cap lowered) refuses whole
# objects with the counters moving, while the world keeps drawing.
const WI_WALL_Z := -10.0
const WI_FRONT_Z := -7.0
const WI_BEHIND_Z := -13.0
const WI_FRAMES := 60
const WI_INSTANCE_SPLATS := 1500
const WI_TIMEOUT_MSEC := 150_000
const WI_SIZE := Vector2i(256, 256)
const WI_COLORS := [Color(0, 1, 0), Color(0, 0.3, 1), Color(0, 1, 1), Color(0.1, 0.6, 1),
		Color(0, 1, 0.5), Color(0.2, 0.9, 0.2), Color(0.2, 0.2, 1), Color(0, 0.8, 0.8)]
const WI_FRAME_KEYS := ["hlod_reason", "hlod_world_draw_records", "hlod_instance_draw_records", "sort_dispatches_frame",
		"sort_dispatch_frame", "frame_count", "hlod_instance_refused_objects", "hlod_instance_admitted_objects",
		"hlod_instance_visible_objects", "hlod_bundle_builds", "hlod_instance_row_reuploads", "hlod_selected_splats",
		"hlod_instance_reserve_splats", "hlod_instance_reserve_refusal_frames", "hlod_last_refusal_reason",
		"hlod_overlap_capacity_records", "hlod_instance_overlap_growths"]
# Stationary frames after warm-up over which the overlap storage must not grow (Codex P1 on #1308: the objects' estimate
# was added on top of a storage already grown for it, so every stationary frame regrew it and rebuilt the sorter).
const WI_STATIONARY_FRAMES := 30


func _wi_dc(c: Color) -> PackedFloat32Array:
	var k := 0.28209479177387814
	return PackedFloat32Array([(c.r - 0.5) / k, (c.g - 0.5) / k, (c.b - 0.5) / k])


func _wi_wall(paged: bool) -> GaussianSplatWorld:
	var n := 160
	var data := GaussianData.new()
	data.resize(n * n)
	var pos := PackedVector3Array()
	var sc := PackedVector3Array()
	var rot: Array[Quaternion] = []
	var op := PackedFloat32Array()
	var sh := PackedFloat32Array()
	var red := _wi_dc(Color(1, 0, 0))
	for iy in range(n):
		for ix in range(n):
			pos.append(Vector3(-4.0 + 8.0 * (ix + 0.5) / n, -4.0 + 8.0 * (iy + 0.5) / n, WI_WALL_Z))
			sc.append(Vector3(0.06, 0.06, 0.02))
			rot.append(Quaternion())
			op.append(0.97)
			sh.append_array(red)
	data.set_positions(pos)
	data.set_scales(sc)
	data.set_rotations(rot)
	data.set_opacities(op)
	data.set_spherical_harmonics(sh)
	var w := GaussianSplatWorld.new()
	w.gaussian_data = data
	if w.bake_hlod() != OK or not w.has_hlod_tree():
		return null
	if not paged:
		return w
	var path := "user://hlod_world_instances_%d_%d.gsplatworld" % [OS.get_process_id(), Time.get_ticks_usec()]
	if w.save_hlod_paged(path) != OK:
		return null
	var loaded: GaussianSplatWorld = ResourceLoader.load(path, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
	return loaded if loaded != null and loaded.has_hlod_paged_source() else null


func _wi_ball(c: Color, p_seed: int) -> GaussianSplatAsset:
	var rng := RandomNumberGenerator.new()
	rng.seed = p_seed
	var a := GaussianSplatAsset.new()
	a.set_splat_count(WI_INSTANCE_SPLATS)
	var p := PackedFloat32Array()
	var s := PackedFloat32Array()
	var r := PackedFloat32Array()
	var dc := PackedFloat32Array()
	var o := PackedFloat32Array()
	var d := _wi_dc(c)
	for i in range(WI_INSTANCE_SPLATS):
		var v := Vector3(rng.randf_range(-1, 1), rng.randf_range(-1, 1), rng.randf_range(-1, 1))
		if v.length() > 1.0:
			v = v.normalized()
		v *= 0.35
		p.append_array(PackedFloat32Array([v.x, v.y, v.z]))
		s.append_array(PackedFloat32Array([0.09, 0.09, 0.09]))
		r.append_array(PackedFloat32Array([0.0, 0.0, 0.0, 1.0]))
		dc.append_array(d)
		o.append(6.0)
	a.set_positions(p)
	a.set_scales(s)
	a.set_rotations(r)
	a.set_sh_dc_coefficients(dc)
	a.set_opacity_logits(o)
	return a


func _wi_classify(c: Color) -> String:
	if c.r > 0.45 and c.g < 0.3 and c.b < 0.3:
		return "wall"
	if c.r < 0.35 and max(c.g, c.b) > 0.35:
		return "instance"
	if c.r < 0.08 and c.g < 0.08 and c.b < 0.08:
		return "background"
	return "other"


func _wi_stats(renderer: GaussianSplatRenderer, what: String) -> Dictionary:
	var stats := renderer.get_render_stats()
	for key in WI_FRAME_KEYS:
		if not _check(stats.has(key), "%s: missing renderer field %s" % [what, key]):
			return {}
	return stats


func _wi_place(nodes: Array, base: Array, t: float, swapped: bool) -> void:
	for i in range(nodes.size()):
		var front := (i < 4) != swapped
		var bp: Vector3 = base[i]
		nodes[i].position = Vector3(bp.x + 0.25 * sin(t * 2.0 + i), bp.y + 0.15 * cos(t * 1.7 + i), WI_FRONT_Z if front else WI_BEHIND_Z)


func _wi_run_mode(mode: String) -> bool:
	var deadline := Time.get_ticks_msec() + WI_TIMEOUT_MSEC
	var world := _wi_wall(mode == "paged")
	if not _check(world != null, "%s: wall world bake/load failed" % mode):
		return false
	var scene := Node3D.new()
	get_root().add_child(scene)
	_scene = scene
	var camera := Camera3D.new()
	camera.fov = 60.0
	camera.near = 0.1
	camera.far = 100.0
	scene.add_child(camera)
	camera.make_current()
	var world_node := GaussianSplatWorld3D.new()
	world_node.set_auto_apply_on_ready(false)
	world_node.world = world
	world_node.set_hlod_tau_px(0.5)
	scene.add_child(world_node)
	world_node.apply_world()
	var nodes: Array = []
	var base: Array = []
	var xs := [-2.6, -0.9, 0.9, 2.6]
	for i in range(8):
		var n := GaussianSplatNode3D.new()
		n.set_splat_asset(_wi_ball(WI_COLORS[i], 100 + i))
		var bp := Vector3(xs[i % 4], 1.4 if i < 4 else -1.4, WI_FRONT_Z if i < 4 else WI_BEHIND_Z)
		n.position = bp
		base.append(bp)
		scene.add_child(n)
		nodes.append(n)
	# Warm-up: pump frames (wall clock bound) until the route is active with both producers drawing.
	var renderer: GaussianSplatRenderer = null
	var stable := 0
	var last := {}
	while Time.get_ticks_msec() < deadline and stable < 10:
		await RenderingServer.frame_post_draw
		# The deadline binds the pass: a frame that ended past it is not an observation (tests/AGENTS.md).
		if not _check(Time.get_ticks_msec() < deadline, "%s: warm-up exceeded its deadline (stable=%d)" % [mode, stable]):
			return false
		renderer = world_node.get_renderer()
		if renderer == null:
			continue
		last = renderer.get_render_stats()
		if str(last.get("hlod_reason", "")) == "active" and int(last.get("hlod_world_draw_records", 0)) > 0 and int(last.get("hlod_instance_draw_records", 0)) > 0:
			stable += 1
		else:
			stable = 0
	if not _check(stable >= 10, "%s: world + instances never became active together (reason=%s, world_records=%s, instance_records=%s)" % [mode, last.get("hlod_reason", "MISSING"), last.get("hlod_world_draw_records", "MISSING"), last.get("hlod_instance_draw_records", "MISSING")]):
		return false
	var start := _wi_stats(renderer, mode)
	if start.is_empty():
		return false
	var builds_before := int(start["hlod_bundle_builds"])
	var summary := {"mode": mode, "start": start}
	# Stationary phase: nothing moves, so the shared overlap storage must hold still. The objects' record estimate is
	# held next to the world's need once; it is never added again on top of the storage already grown for it.
	var growths_before := int(start["hlod_instance_overlap_growths"])
	var capacity_before := int(start["hlod_overlap_capacity_records"])
	if not _check(growths_before >= 1 and capacity_before > 0, "%s: the objects' estimate never grew the overlap storage (growths=%d capacity=%d)" % [mode, growths_before, capacity_before]):
		return false
	for f in range(WI_STATIONARY_FRAMES):
		await RenderingServer.frame_post_draw
		if not _check(Time.get_ticks_msec() < deadline, "%s: stationary phase exceeded its deadline" % mode):
			return false
		var what := "%s stationary frame %d" % [mode, f]
		var st := _wi_stats(renderer, what)
		if st.is_empty():
			return false
		if not _check(str(st["hlod_reason"]) == "active" and int(st["hlod_instance_draw_records"]) == 8 and int(st["hlod_world_draw_records"]) > 0, "%s: reason=%s world=%s instance=%s" % [what, st["hlod_reason"], st["hlod_world_draw_records"], st["hlod_instance_draw_records"]]):
			return false
		if not _check(int(st["hlod_instance_overlap_growths"]) == growths_before and int(st["hlod_overlap_capacity_records"]) == capacity_before, "%s: the overlap storage grew with unchanged content (growths %d -> %s, capacity %d -> %s)" % [what, growths_before, st["hlod_instance_overlap_growths"], capacity_before, st["hlod_overlap_capacity_records"]]):
			return false
	var stationary := _wi_stats(renderer, mode)
	if stationary.is_empty():
		return false
	summary["after_stationary"] = stationary
	builds_before = int(stationary["hlod_bundle_builds"])
	var reuploads_before := int(stationary["hlod_instance_row_reuploads"])
	# Phases A (0-3 in front) and B (swapped): transform-only motion every frame.
	for phase in ["A", "B"]:
		var swapped: bool = phase == "B"
		var t := 0.0
		for f in range(WI_FRAMES):
			t += 1.0 / 60.0
			_wi_place(nodes, base, t, swapped)
			await RenderingServer.frame_post_draw
			await RenderingServer.frame_post_draw
			if not _check(Time.get_ticks_msec() < deadline, "%s: proof exceeded its deadline" % mode):
				return false
			var what := "%s %s frame %d" % [mode, phase, f]
			var st := _wi_stats(renderer, what)
			if st.is_empty():
				return false
			if not _check(str(st["hlod_reason"]) == "active", "%s: hlod_reason=%s" % [what, st["hlod_reason"]]):
				return false
			if not _check(int(st["hlod_world_draw_records"]) > 0 and int(st["hlod_instance_draw_records"]) == 8, "%s: world=%s instance=%s draw records" % [what, st["hlod_world_draw_records"], st["hlod_instance_draw_records"]]):
				return false
			if not _check(int(st["hlod_instance_admitted_objects"]) == 8 and int(st["hlod_instance_refused_objects"]) == 0, "%s: admitted=%s refused=%s" % [what, st["hlod_instance_admitted_objects"], st["hlod_instance_refused_objects"]]):
				return false
			if not _check(int(st["sort_dispatches_frame"]) == 1 and int(st["sort_dispatch_frame"]) == int(st["frame_count"]) - 1, "%s: %s sorts in frame %s (frame_count %s)" % [what, st["sort_dispatches_frame"], st["sort_dispatch_frame"], st["frame_count"]]):
				return false
			for key in st:
				var v := str(st[key])
				if not _check(not ("not_exclusive" in v or "not_exclusive" in str(key) or "identity_shim" in v), "%s: %s=%s" % [what, key, v]):
					return false
			if not _check(int(st["hlod_bundle_builds"]) == builds_before, "%s: moving nodes rebuilt the bundle (%d -> %s)" % [what, builds_before, st["hlod_bundle_builds"]]):
				return false
			var img := get_root().get_texture().get_image()
			if not _check(img != null and img.get_size() == WI_SIZE, "%s: viewport capture missing" % what):
				return false
			for i in range(8):
				var front := (i < 4) != swapped
				var px := camera.unproject_position(nodes[i].global_position)
				var c := img.get_pixelv(Vector2i(int(px.x), int(px.y)))
				var cls := _wi_classify(c)
				if not _check(cls == ("instance" if front else "wall"), "%s: node %d (%s) pixel %s is %s %s" % [what, i, "front" if front else "behind", px, cls, c]):
					return false
			if not _check(_wi_classify(img.get_pixel(3, 3)) == "background", "%s: background control is %s" % [what, img.get_pixel(3, 3)]):
				return false
			if not _check(_wi_classify(img.get_pixel(WI_SIZE.x / 2, WI_SIZE.y / 2)) == "wall", "%s: wall control is %s" % [what, img.get_pixel(WI_SIZE.x / 2, WI_SIZE.y / 2)]):
				return false
		var path := "user://hlod_world_instances_%s_%s_%d.png" % [mode, phase, OS.get_process_id()]
		if not _check(get_root().get_texture().get_image().save_png(path) == OK, "%s: cannot preserve the %s capture" % [mode, phase]):
			return false
		summary["image_" + phase] = ProjectSettings.globalize_path(path)
	var moved := _wi_stats(renderer, mode)
	if moved.is_empty():
		return false
	if not _check(int(moved["hlod_instance_row_reuploads"]) >= reuploads_before + 2 * WI_FRAMES, "%s: moving nodes did not take the row re-upload path (%d -> %s)" % [mode, reuploads_before, moved["hlod_instance_row_reuploads"]]):
		return false
	summary["after_motion"] = moved
	# Budget phase: the view's splat cap lowered so half of it holds 3 of the 8 objects. The rest are refused whole, the
	# counters move, the world keeps drawing within the caps minus the reserve.
	var cap := WI_INSTANCE_SPLATS * 6 + 10
	var refusal_frames_before := int(moved["hlod_instance_reserve_refusal_frames"])
	for n in nodes:
		n.set("quality/preset", 3) # Custom: otherwise the preset re-derives max_splat_count.
		n.set_max_splat_count(cap)
	world_node.set_max_splat_count(cap)
	var refused_seen := 0
	var budget := {}
	while Time.get_ticks_msec() < deadline and refused_seen < 20:
		_wi_place(nodes, base, 0.0, false)
		await RenderingServer.frame_post_draw
		# The deadline binds the pass: an observation from a frame that ended past it is a failure, not a count.
		if not _check(Time.get_ticks_msec() < deadline, "%s: budget phase exceeded its deadline (refused_seen=%d)" % [mode, refused_seen]):
			return false
		var st := renderer.get_render_stats()
		if str(st.get("hlod_reason", "")) == "active" and int(st.get("hlod_instance_refused_objects", 0)) > 0:
			refused_seen += 1
			budget = st
	if not _check(refused_seen >= 20 and not budget.is_empty(), "%s: the budget phase never refused an object" % mode):
		return false
	for key in WI_FRAME_KEYS:
		if not _check(budget.has(key), "%s budget: missing renderer field %s" % [mode, key]):
			return false
	if not _check(int(budget["hlod_instance_admitted_objects"]) == 3 and int(budget["hlod_instance_refused_objects"]) == 5 and int(budget["hlod_instance_draw_records"]) == 3, "%s budget: admitted=%s refused=%s instance records=%s (expected 3/5/3)" % [mode, budget["hlod_instance_admitted_objects"], budget["hlod_instance_refused_objects"], budget["hlod_instance_draw_records"]]):
		return false
	if not _check(int(budget["hlod_instance_reserve_refusal_frames"]) >= refusal_frames_before + 20, "%s budget: refusal-frame counter did not count" % mode):
		return false
	if not _check(int(budget["hlod_world_draw_records"]) > 0 and int(budget["hlod_selected_splats"]) > 0, "%s budget: the world stopped drawing" % mode):
		return false
	if not _check(int(budget["hlod_instance_reserve_splats"]) <= cap / 2 and int(budget["hlod_selected_splats"]) <= cap - int(budget["hlod_instance_reserve_splats"]), "%s budget: reserve %s / world %s exceed the caps" % [mode, budget["hlod_instance_reserve_splats"], budget["hlod_selected_splats"]]):
		return false
	if not _check(int(budget["sort_dispatches_frame"]) == 1, "%s budget: %s sorts" % [mode, budget["sort_dispatches_frame"]]):
		return false
	summary["budget"] = budget
	_metrics["world_instances_" + mode] = summary
	_scene = null
	scene.queue_free()
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	return true


func _run_world_plus_instances_proof() -> bool:
	# The earlier proof's scene is done: replace it (its renderer and route are released with it).
	if _scene != null:
		_scene.queue_free()
	_scene = null
	_node = null
	_renderer = null
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	get_root().size = WI_SIZE
	for mode in ["paged", "whole"]:
		var proved := await _wi_run_mode(mode)
		if not proved:
			return false
	return true
