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
var _metrics: Dictionary = {"hlod_main_view_proof_status": "not_started", "samples": []}


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


func _run() -> void:
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
		var image := get_root().get_texture().get_image()
		if not _check(image != null and not image.is_empty(), "Actual post-draw target capture missing"):
			return
		var path := "user://resident_hlod_main_view_%d.png" % OS.get_process_id()
		if not _check(image.save_png(path) == OK, "Cannot preserve actual viewport capture"):
			return
		_metrics["image_path"] = ProjectSettings.globalize_path(path)
		_metrics["image_sha256"] = FileAccess.get_sha256(path)
		_metrics["image_width"] = image.get_width()
		_metrics["image_height"] = image.get_height()
		if not _check(Time.get_ticks_msec() < deadline, "Proof completed after its deadline"):
			return
		_metrics["hlod_main_view_proof_status"] = "passed"
		_emit_metrics()
		_report.emit_pass()
		_cleanup()
		quit(0)
		return
	_fail("No two consecutive admitted main-view frames within deadline; last reason: %s" % _metrics.get("last_hlod_reason", "renderer unavailable"))
