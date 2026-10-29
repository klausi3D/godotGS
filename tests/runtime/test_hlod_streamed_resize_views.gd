extends SceneTree

# Target-backed proof for the streamed HLOD route's tile grid (D1 resize fix). A streamed world never runs a canonical
# frame after startup, so the grid has to follow each view's size: window resizes, many distinct sizes (the ledgers must
# track live bytes), two views of different sizes in the same frame, a memory refusal, and recovery from it by a smaller view.
# Functional signal only: every frame that claims to be active must also light pixels.
const GsRuntimeReport := preload("gs_runtime_report.gd")
const SCENARIO := "Streamed HLOD Resize Views"
const TIMEOUT_MSEC := 150_000
const STARTUP_MSEC := 40_000
const MAIN := Vector2i(320, 180)
const SPLATS := 120000

var _report := GsRuntimeReport.new(SCENARIO)
var _scene: Node3D
var _node: GaussianSplatWorld3D
var _renderer: GaussianSplatRenderer
var _subvp: SubViewport
var _metrics: Dictionary = {"streamed_resize_status": "not_started"}
var _deadline := 0


func _init() -> void:
	call_deferred("_run")


func _emit_metrics() -> void:
	print("%s %s" % [GsRuntimeReport.METRICS_MARKER, JSON.stringify(_metrics, "", false, true)])


func _cleanup() -> void:
	if _renderer != null:
		_renderer.test_set_hlod_route_limit_override(0)
	_renderer = null
	_node = null
	if _scene != null:
		_scene.queue_free()
	_scene = null


func _fail(reason: String) -> void:
	_metrics["streamed_resize_status"] = "failed"
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


func _lit(image: Image) -> int:
	if image == null or image.is_empty():
		return 0
	var lit := 0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var pixel := image.get_pixel(x, y)
			if pixel.r > 0.0 or pixel.g > 0.0 or pixel.b > 0.0:
				lit += 1
	return lit


# One drawn frame of the renderer: its stats plus the lit pixels of the main window (and of the second view when present).
func _frame() -> Dictionary:
	await RenderingServer.frame_post_draw
	var stats := _renderer.get_render_stats()
	var sample := {
		"active": bool(stats.get("hlod_active", false)),
		"reason": str(stats.get("hlod_reason", "")),
		"streaming": bool(stats.get("hlod_streaming", false)),
		"refused_views": int(stats.get("hlod_refused_views", 0)),
		"last_refusal": str(stats.get("hlod_last_refusal_reason", "")),
		"has_owned": stats.has("hlod_owned_byte_upper_bound"),
		"owned": int(stats.get("hlod_owned_byte_upper_bound", 0)),
		"ready": int(stats.get("hlod_stream_ready_frames", 0)),
		"lit": _lit(get_root().get_texture().get_image()),
		"sub_lit": 0,
		"win": get_root().size,
	}
	if _subvp != null:
		sample["sub_lit"] = _lit(_subvp.get_texture().get_image())
	return sample


func _set_size(size: Vector2i) -> void:
	get_root().size = size


# Runs `count` frames at the current size; the first `settle` are not judged.
func _run_frames(count: int, settle: int) -> Array:
	var rows: Array = []
	for i in range(count):
		if Time.get_ticks_msec() > _deadline:
			_fail("deadline while running frames")
			return []
		var row: Dictionary = await _frame()
		rows.append(row)
	return rows.slice(settle)


func _all_drawn(rows: Array, label: String) -> bool:
	if not _check(not rows.is_empty(), "%s: no frames" % label):
		return false
	for row in rows:
		if not _check(bool(row["streaming"]), "%s: world is not streamed" % label):
			return false
		if not _check(bool(row["has_owned"]) and int(row["owned"]) > 0, "%s: route bytes stat missing or zero" % label):
			return false
		if not _check(bool(row["active"]) and int(row["lit"]) > 0, "%s: frame not active+lit at %s (reason %s, lit %d)" % [label, row["win"], row["reason"], row["lit"]]):
			return false
	return true


func _build_world() -> GaussianSplatWorld:
	var rng := RandomNumberGenerator.new()
	rng.seed = 7
	var data := GaussianData.new()
	data.resize(SPLATS)
	var positions := PackedVector3Array()
	var scales := PackedVector3Array()
	var rotations: Array[Quaternion] = []
	var opacities := PackedFloat32Array()
	var coefficients := PackedFloat32Array()
	for i in range(SPLATS):
		var p := Vector3(rng.randfn(0.0, 1.2), rng.randfn(0.0, 0.8), rng.randfn(0.0, 1.2))
		# Dyadic positions: the HLOD exact-reconstruction check (hlod_center_reconstruction_not_exact) needs values the
		# lattice represents exactly.
		p = Vector3(roundf(p.x * 32.0) / 32.0, roundf(p.y * 32.0) / 32.0, roundf(p.z * 32.0) / 32.0)
		positions.append(p + Vector3(0.0, 0.0, -6.0))
		scales.append(Vector3(0.0625, 0.0625, 0.0625))
		rotations.append(Quaternion())
		opacities.append(0.9)
		coefficients.append_array(PackedFloat32Array([0.75, 0.625, 0.5]))
	data.set_positions(positions)
	data.set_scales(scales)
	data.set_rotations(rotations)
	data.set_opacities(opacities)
	data.set_spherical_harmonics(coefficients)
	var world := GaussianSplatWorld.new()
	world.gaussian_data = data
	if world.bake_hlod() != OK or not world.has_hlod_tree():
		return null
	return world


func _run() -> void:
	_deadline = Time.get_ticks_msec() + TIMEOUT_MSEC
	var rd := RenderingServer.get_rendering_device()
	if not _check(rd != null and not OS.has_feature("headless"), "Native RD runtime required"):
		return
	ProjectSettings.set_setting("rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled", true)
	ProjectSettings.set_setting("rendering/gaussian_splatting/streaming/route_policy", 0)
	ProjectSettings.set_setting("rendering/gaussian_splatting/instance_pipeline/enabled", true)
	ProjectSettings.set_setting("rendering/gaussian_splatting/sorting/force_cpu_sort", false)
	ProjectSettings.set_setting("rendering/gaussian_splatting/compression/per_chunk_quantization", 0)
	# A small atlas budget makes the 60k-splat tree stream (the whole tree does not fit).
	ProjectSettings.set_setting("rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb", 16)
	ProjectSettings.emit_signal("settings_changed")
	_set_size(MAIN)
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED
	RenderingServer.set_default_clear_color(Color.BLACK)
	var world := _build_world()
	if not _check(world != null, "Synthetic HLOD bake failed"):
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
	_node.set_hlod_tau_px(8.0)
	_scene.add_child(_node)
	_node.apply_world()

	# Startup: wait until the streamed route draws at the startup size.
	var drawn := 0
	var startup_deadline := Time.get_ticks_msec() + STARTUP_MSEC
	while drawn < 5 and Time.get_ticks_msec() < startup_deadline:
		await RenderingServer.frame_post_draw
		_renderer = _node.get_renderer()
		if _renderer == null:
			continue
		var row: Dictionary = await _frame()
		drawn = drawn + 1 if (bool(row["active"]) and int(row["lit"]) > 0) else 0
		_metrics["startup_reason"] = row["reason"]
	if drawn < 5 and _renderer != null:
		var diag := {}
		for key in _renderer.get_render_stats():
			if str(key).begins_with("hlod_") and not str(key).begins_with("hlod_stream_payload"):
				diag[key] = _renderer.get_render_stats()[key]
		_metrics["startup_hlod_stats"] = diag
		var all_stats := _renderer.get_render_stats()
		for key in ["frame_count", "sorted_splats", "visible_splats", "rendered_splat_count", "overlap_records", "global_overlap_capacity", "stage_raster_status", "stage_raster_reason", "stage_sort_status", "stage_cull_status", "route_uid"]:
			diag[key] = all_stats.get(key, "missing")
		diag["rendered_content"] = _renderer.has_rendered_content()
		diag["engine_frames"] = Engine.get_frames_drawn()
	if not _check(drawn >= 5, "Streamed route did not draw at the startup size (last reason %s)" % _metrics.get("startup_reason", "")):
		return
	var rows: Array = await _run_frames(8, 0)
	if not _all_drawn(rows, "startup"):
		return
	var baseline_refused := int(rows[-1]["refused_views"])

	# 1. A held resize (the F1 repro): drawn at a size other than the startup size, and again at the startup size.
	_set_size(Vector2i(256, 144))
	rows = await _run_frames(14, 4)
	if not _all_drawn(rows, "held resize 256x144"):
		return
	_set_size(MAIN)
	rows = await _run_frames(14, 4)
	if not _all_drawn(rows, "return to startup size"):
		return

	# 2. Many distinct sizes: the texture ledgers (tile and compositor) follow live bytes, so the route is never refused
	# for good by a ledger that only grew.
	var owned_peak := 0
	var owned_first := int(rows[-1]["owned"])
	for i in range(30):
		var size := Vector2i(192 + 8 * (i % 9), 108 + 6 * ((i * 5) % 11))
		_set_size(size)
		rows = await _run_frames(6, 3)
		if not _all_drawn(rows, "size sweep %s" % size):
			return
		owned_peak = max(owned_peak, int(rows[-1]["owned"]))
	_set_size(MAIN)
	rows = await _run_frames(12, 4)
	if not _all_drawn(rows, "after the sweep"):
		return
	var owned_after := int(rows[-1]["owned"])
	_metrics["owned_first"] = owned_first
	_metrics["owned_peak_after_sweep"] = owned_peak
	_metrics["owned_after_sweep"] = owned_after
	# The ledgers follow LIVE bytes: back at the startup size the route holds what it held at the start (a ledger that only
	# adds discarded textures grows by about one scratch per distinct size, several MB over this sweep).
	if not _check(owned_peak <= owned_first + 2 * 1024 * 1024, "Route bytes grew across the size sweep: %d -> %d" % [owned_first, owned_peak]):
		return
	if not _check(abs(owned_after - owned_first) <= 1024 * 1024, "Route bytes at the startup size differ after the sweep: %d -> %d" % [owned_first, owned_after]):
		return

	# 3. Two views of different sizes in the same frame (the F4 repro): both draw, every frame, and none is refused.
	_subvp = SubViewport.new()
	_subvp.size = Vector2i(160, 90)
	_subvp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_subvp.msaa_3d = Viewport.MSAA_DISABLED
	_scene.add_child(_subvp)
	var subcam := Camera3D.new()
	subcam.fov = 60.0
	subcam.near = 0.1
	subcam.far = 100.0
	_subvp.add_child(subcam)
	subcam.make_current()
	rows = await _run_frames(16, 4)
	if not _all_drawn(rows, "two views, main"):
		return
	var sub_lit_frames := 0
	for row in rows:
		if int(row["sub_lit"]) > 0:
			sub_lit_frames += 1
	_metrics["two_view_sub_lit_frames"] = sub_lit_frames
	_metrics["two_view_frames"] = rows.size()
	if not _check(sub_lit_frames == rows.size(), "Second view blank in %d of %d frames" % [rows.size() - sub_lit_frames, rows.size()]):
		return
	if not _check(int(rows[-1]["refused_views"]) == baseline_refused, "A view was refused while both views fit (refused %d, baseline %d, last %s)" % [rows[-1]["refused_views"], baseline_refused, rows[-1]["last_refusal"]]):
		return
	_subvp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	_scene.remove_child(_subvp)
	_subvp.free()
	_subvp = null

	# 4. A refusal for memory is honest (reason, not active), and a smaller view recovers from a route pushed over its limit.
	_set_size(Vector2i(640, 360))
	rows = await _run_frames(10, 4)
	if not _all_drawn(rows, "large view"):
		return
	var large_owned := int(rows[-1]["owned"])
	# 4a. The limit leaves no room for a larger grid: the larger view is refused by name and not active.
	_renderer.test_set_hlod_route_limit_override(large_owned + 512 * 1024)
	_set_size(Vector2i(960, 540))
	rows = await _run_frames(10, 4)
	if not _check(not rows.is_empty(), "no frames after the larger view"):
		return
	for row in rows:
		if not _check(not bool(row["active"]) and str(row["reason"]) == "hlod_view_size_memory_limit", "larger view must be refused for memory (active %s, reason %s)" % [row["active"], row["reason"]]):
			return
		if not _check(int(row["lit"]) == 0, "a refused streamed view must draw nothing"):
			return
	_metrics["refusal_reason"] = rows[-1]["reason"]
	if not _check(int(rows[-1]["refused_views"]) > baseline_refused and str(rows[-1]["last_refusal"]) == "hlod_view_size_memory_limit", "the refusal is not counted or named"):
		return
	# 4b. A route that is over its limit (the limit drops below what the 640x360 grid holds) refuses its same-size view,
	# and a smaller view shrinks the grid and draws again instead of being refused for good.
	_set_size(Vector2i(640, 360))
	rows = await _run_frames(8, 4)
	_renderer.test_set_hlod_route_limit_override(max(1, large_owned - 2 * 1024 * 1024))
	rows = await _run_frames(8, 4)
	for row in rows:
		if not _check(not bool(row["active"]) and str(row["reason"]) == "route_memory_limit", "the over-limit same-size view must be refused (active %s, reason %s)" % [row["active"], row["reason"]]):
			return
	_set_size(Vector2i(160, 90))
	rows = await _run_frames(12, 4)
	if not _all_drawn(rows, "smaller view after an over-limit route"):
		return
	_renderer.test_set_hlod_route_limit_override(0)
	_set_size(MAIN)
	rows = await _run_frames(12, 4)
	if not _all_drawn(rows, "recovered at the startup size"):
		return

	if not _check(Time.get_ticks_msec() < _deadline, "Proof completed after its deadline"):
		return
	_metrics["streamed_resize_status"] = "passed"
	_emit_metrics()
	_report.emit_pass()
	_cleanup()
	quit(0)
