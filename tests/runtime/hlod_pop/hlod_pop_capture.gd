extends SceneTree

# Local HLOD pop capture driver (not a CI lane). Loads a baked HLOD world, runs a dolly and records,
# per step, the camera pose, the drawn node ids with their cross-fade factors, and a screenshot.
# hlod_pop_metric.py turns that into per-transition pop scores. Optionally parks the camera at one
# step's pose afterwards and captures consecutive frames (static-frame determinism).
#
#   godot --path <proj> --rendering-driver vulkan --resolution WxH --windowed \
#         --script tests/runtime/hlod_pop/hlod_pop_capture.gd -- --plan plan.json
#
# Plan keys: world, out_dir, resolution [w, h], settings {project setting: value}, node {max_splat_count,
# max_render_distance_m, hlod_tau_px}, camera {fov_deg, near_m, far_m}, settle_deadline_s,
# dolly {from, to, look_at, steps, t_from, t_to, shot_scale, settle_each_step, step_deadline_s},
# static {step, frames} (optional).
#
# Waits follow tests/AGENTS.md: frames are pumped until the condition holds or a wall-clock deadline
# expires; an expired deadline is recorded as "settled": false, never as settled.

var _plan: Dictionary
var _node: Node3D
var _camera: Camera3D
var _out := ""


func _initialize() -> void:
	_run.call_deferred()


func _fail(msg: String) -> void:
	printerr("hlod_pop_capture: " + msg)
	_write_json(_out.path_join("error.json") if not _out.is_empty() else "user://hlod_pop_error.json", {"error": msg})
	quit(1)


func _write_json(path: String, value) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(value, " ", false))


func _v3(a) -> Vector3:
	return Vector3(float(a[0]), float(a[1]), float(a[2]))


func _stats() -> Dictionary:
	var r = _node.get_renderer() if _node != null else null
	return r.get_render_stats() if r != null else {}


func _set_pose(pos: Vector3, look_at: Vector3) -> void:
	_camera.global_transform = Transform3D(Basis(), pos).looking_at(look_at, Vector3.UP)


# Pump frames until the HLOD cut is resident and its drawn set (ids and fades) is stable for
# `stable_frames`, or the deadline.
func _settle(deadline_s: float, stable_frames := 8) -> Dictionary:
	var t0 := Time.get_ticks_usec()
	var deadline := t0 + int(deadline_s * 1e6)
	var last_key = null
	var stable := 0
	var frames := 0
	while true:
		await process_frame
		frames += 1
		var s := _stats()
		var ok: bool = s.get("hlod_active") == true and int(s.get("hlod_stream_loading_nodes", 1)) == 0 \
				and int(s.get("hlod_stream_nonresident_drawn", 1)) == 0 and s.get("hlod_drawn_node_digest") != null
		var key = [s.get("hlod_drawn_node_digest"), s.get("hlod_drawn_node_fades")]
		if ok and key == last_key:
			stable += 1
		else:
			stable = 0
		last_key = key
		var now := Time.get_ticks_usec()
		if stable >= stable_frames:
			return {"settled": now <= deadline, "frames": frames, "wall_s": (now - t0) / 1e6}
		if now > deadline:
			return {"settled": false, "frames": frames, "wall_s": (now - t0) / 1e6}
	return {}


func _shot(path: String, scale := 1.0) -> String:
	var img: Image = get_root().get_texture().get_image()
	if img == null or img.is_empty():
		return "no image"
	if scale != 1.0:
		img.resize(int(img.get_width() * scale), int(img.get_height() * scale), Image.INTERPOLATE_BILINEAR)
	return "" if img.save_png(path) == OK else "save failed"


func _row(stats: Dictionary) -> Dictionary:
	var ids = stats.get("hlod_drawn_node_ids")
	var fades = stats.get("hlod_drawn_node_fades")  # null on a binary without the cross-fade
	return {
		"frame": Engine.get_frames_drawn(),
		"hlod_active": stats.get("hlod_active"),
		"rendered_splat_count": stats.get("rendered_splat_count"),
		"hlod_selected_splats": stats.get("hlod_selected_splats"),
		"hlod_cut_budget_bound": stats.get("hlod_cut_budget_bound"),
		"hlod_stream_loading_nodes": stats.get("hlod_stream_loading_nodes"),
		"hlod_stream_nonresident_drawn": stats.get("hlod_stream_nonresident_drawn"),
		"hlod_fade_pairs": stats.get("hlod_fade_pairs"),
		"hlod_fade_groups": stats.get("hlod_fade_groups"),
		"hlod_fade_snapped": stats.get("hlod_fade_snapped"),
		"hlod_fade_snapped_total": stats.get("hlod_fade_snapped_total"),
		"hlod_fade_extra_splats": stats.get("hlod_fade_extra_splats"),
		"hlod_stream_uploaded_this_frame": stats.get("hlod_stream_uploaded_this_frame"),
		"hlod_stream_evicted_total": stats.get("hlod_stream_evicted_total"),
		"hlod_stream_requested_this_frame": stats.get("hlod_stream_requested_this_frame"),
		"gpu_pass_coherent_total_ms": (stats.get("gpu_pass_timing", {}) as Dictionary).get("coherent_total_ms") if stats.get("gpu_pass_timing") is Dictionary else null,
		"gpu_frame_time_ms": stats.get("gpu_frame_time_ms"),
		"gpu_frame_time_valid": stats.get("gpu_frame_time_valid"),
		"drawn_node_ids": (Array(ids) if ids is PackedInt32Array else null),
		"drawn_node_fades": (Array(fades) if fades is PackedFloat32Array else null),
		"drawn_node_digest": stats.get("hlod_drawn_node_digest"),
	}


func _pose_at(d: Dictionary, i: int) -> Array:
	var a := _v3(d["from"])
	var b := _v3(d["to"])
	var look := _v3(d["look_at"])
	var steps := int(d.get("steps", 300))
	# t_from/t_to restrict the dolly to a segment of the same path (finer steps over a shorter range).
	var t := lerpf(float(d.get("t_from", 0.0)), float(d.get("t_to", 1.0)), float(i) / float(steps))
	# Geometric spacing: equal relative distance change per step (LOD error scales with 1/distance).
	var da := a.distance_to(look)
	var db := b.distance_to(look)
	var dist := da * pow(db / da, t)
	var dirv := (a.lerp(b, t) - look).normalized()
	return [look + dirv * dist, look, dist]


func _run_dolly(d: Dictionary) -> Dictionary:
	var dir := _out.path_join("frames")
	DirAccess.make_dir_recursive_absolute(dir)
	var steps := int(d.get("steps", 300))
	var scale := float(d.get("shot_scale", 0.5))
	var settle_each := bool(d.get("settle_each_step", true))
	var p0 := _pose_at(d, 0)
	_set_pose(p0[0], p0[1])
	var first_settle := await _settle(float(_plan.get("settle_deadline_s", 60.0)))
	var rows := []
	for i in range(steps + 1):
		var p := _pose_at(d, i)
		_set_pose(p[0], p[1])
		var settle := {}
		if settle_each:
			settle = await _settle(float(d.get("step_deadline_s", 10.0)), 3)
		else:
			await process_frame
		await process_frame
		var row := _row(_stats())
		row["step"] = i
		row["distance_m"] = p[2]
		row["cam_pos"] = [p[0].x, p[0].y, p[0].z]
		row["look_at"] = [p[1].x, p[1].y, p[1].z]
		row["settle"] = settle
		row["shot"] = "frames/%04d.png" % i
		row["shot_error"] = _shot(_out.path_join(row["shot"]), scale)
		rows.append(row)
	return {"first_settle": first_settle, "rows": rows}


func _run_static(d: Dictionary, s: Dictionary) -> Dictionary:
	var dir := _out.path_join("static")
	DirAccess.make_dir_recursive_absolute(dir)
	var p := _pose_at(d, int(s.get("step", 0)))
	_set_pose(p[0], p[1])
	var settle := await _settle(float(_plan.get("settle_deadline_s", 60.0)))
	var rows := []
	for i in range(int(s.get("frames", 30))):
		await process_frame
		var row := _row(_stats())
		row["shot"] = "static/%03d.png" % i
		row["shot_error"] = _shot(_out.path_join(row["shot"]), 1.0)
		rows.append(row)
	return {"step": int(s.get("step", 0)), "settle": settle, "rows": rows}


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var plan_path := ""
	for i in range(args.size()):
		if args[i] == "--plan" and i + 1 < args.size():
			plan_path = args[i + 1]
	_plan = JSON.parse_string(FileAccess.get_file_as_string(plan_path))
	if typeof(_plan) != TYPE_DICTIONARY:
		_fail("bad plan " + plan_path)
		return
	_out = str(_plan["out_dir"])
	DirAccess.make_dir_recursive_absolute(_out)
	if DisplayServer.get_name() == "headless":
		_fail("needs a GPU-backed window")
		return
	for key in _plan.get("settings", {}).keys():
		ProjectSettings.set_setting(key, _plan["settings"][key])
	ProjectSettings.emit_signal("settings_changed")
	var res: Array = _plan.get("resolution", [1920, 1080])
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	DisplayServer.window_set_size(Vector2i(int(res[0]), int(res[1])))
	get_root().size = Vector2i(int(res[0]), int(res[1]))
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED

	var cam_cfg: Dictionary = _plan.get("camera", {})
	var scene := Node3D.new()
	get_root().add_child(scene)
	var env := WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.05, 0.05, 0.07)
	scene.add_child(env)
	_camera = Camera3D.new()
	_camera.fov = float(cam_cfg.get("fov_deg", 75.0))
	_camera.near = float(cam_cfg.get("near_m", 0.05))
	_camera.far = float(cam_cfg.get("far_m", 400.0))
	scene.add_child(_camera)
	_camera.make_current()

	var world_res = ResourceLoader.load(str(_plan["world"]))
	if world_res == null or not (world_res is GaussianSplatWorld):
		_fail("cannot load world " + str(_plan["world"]))
		return
	_node = GaussianSplatWorld3D.new()
	_node.set_auto_apply_on_ready(false)
	_node.world = world_res
	var ncfg: Dictionary = _plan.get("node", {})
	if ncfg.has("max_splat_count"): _node.set_max_splat_count(int(ncfg["max_splat_count"]))
	if ncfg.has("max_render_distance_m"): _node.set_max_render_distance(float(ncfg["max_render_distance_m"]))
	if ncfg.has("hlod_tau_px"): _node.set_hlod_tau_px(float(ncfg["hlod_tau_px"]))
	scene.add_child(_node)
	var d: Dictionary = _plan["dolly"]
	var p0 := _pose_at(d, 0)
	_set_pose(p0[0], p0[1])
	_node.apply_world()
	var meta := {"adapter": RenderingServer.get_video_adapter_name(), "is_debug_build": OS.is_debug_build(),
			"engine": Engine.get_version_info(), "world": _plan["world"], "camera": cam_cfg,
			"resolution": res, "shot_scale": float(d.get("shot_scale", 0.5)), "settings_readback": {}}
	for key in _plan.get("settings", {}).keys():
		meta["settings_readback"][key] = ProjectSettings.get_setting(key)
	_write_json(_out.path_join("meta.json"), meta)
	_write_json(_out.path_join("dolly.json"), await _run_dolly(d))
	if _plan.get("static") is Dictionary:
		_write_json(_out.path_join("static.json"), await _run_static(d, _plan["static"]))
	print("hlod_pop_capture: done")
	quit(0)
