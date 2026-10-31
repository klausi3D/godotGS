extends SceneTree

# S1b matched-arm render (one arm, one process).
#
# Renders ONE .gsplatworld (the full-detail reference or a materialized cut arm)
# at a frozen view through the canonical resident route, and writes a PNG plus a
# telemetry JSON. tests/runtime/hlod_s1b_quality/s1b_quality.py launches this once
# per arm with an identical job so every arm gets a fresh process, identical
# settings and identical camera; it then scores the PNGs.
#
# Usage (non-headless; needs a real RenderingDevice):
#   godot --path tests/runtime/hlod_s1b_quality --script s1b_render_arm.gd -- --job=<job.json>
#
# This script never decides pass/fail on image quality. It fails closed only on the
# conditions that make an image meaningless: wrong/missing/thinned fixture, no
# renderer, no fresh success-status frames, any loss counter, unstable frames or a
# black frame. Every outcome (including failure) writes the telemetry file.

const TELEMETRY_SCHEMA := 1
const READY_TIMEOUT_MSEC := 180_000
const STABLE_FRAMES := 4
const MIN_NONBLACK_PIXELS := 64
const SETTINGS_PREFIX := "rendering/gaussian_splatting/"
# Camera margin (metres) grown around the AABB for the near/far clipping proof, to
# cover splat extent beyond the centre-point AABB.
const CLIP_PROOF_MARGIN := 5.0
# Live loss telemetry: renderer.get_overflow_stats() (always-on async readback of the resident
# OverflowStats SSBO, #1137). The get_render_stats() keys raster_overflow_tiles /
# raster_overlap_records / raster_max_splats_per_tile read 0 in every frame even at 230k visible
# splats, so they are recorded as context only and never counted as evidence of zero loss.
# Liveness: sampled_drop_frame_serial must be > 0 and strictly advance across the stable window;
# a counter that does not advance proves nothing. Loss: every key below must be an int == 0.
const LIVE_SERIAL_KEY := "sampled_drop_frame_serial"
const LIVE_ZERO_COUNTERS := ["sampled_dropped_records", "sampled_raster_truncated_tiles", "overflow_drop_events", "raster_tile_cap_drop_events", "clamped_records", "overflow_tile_count"]
# aggregated_records is the total of attempted tile-record inserts: it must be > 0 (populated).
const LIVE_CONTEXT_KEYS := ["raster_tile_cap", "aggregated_records"]
const RECORDED_STAT_KEYS := [
	"frame_count", "visible_splats", "total_splats", "sorted_splats", "stage_sort_sorted_count",
	"stage_metrics_valid", "stage_cull_status", "stage_sort_status", "stage_raster_status",
	"stage_composite_status", "raster_overflow_tiles", "raster_overflow_ratio", "raster_overlap_records",
	"raster_overlap_record_budget_effective", "raster_overlap_thinning_keep_ratio",
	"raster_max_splats_per_tile", "raster_max_raster_splats_per_tile", "raster_tile_splat_capacity",
	"raster_path", "hlod_active", "hlod_reason", "hlod_selected_splats", "cull_route_uid", "sort_route_uid",
	"selected_route_uid", "render_mode",
]

var _job: Dictionary = {}
var _telemetry: Dictionary = {"schema_version": TELEMETRY_SCHEMA, "status": "not_started", "reason": ""}
var _scene: Node3D
var _node: GaussianSplatWorld3D


func _init() -> void:
	call_deferred("_run")


func _finish(status: String, reason: String) -> void:
	_telemetry["status"] = status
	_telemetry["reason"] = reason
	var path := str(_job.get("out_json", ""))
	if not path.is_empty():
		var f := FileAccess.open(path, FileAccess.WRITE)
		if f != null:
			f.store_string(JSON.stringify(_telemetry, "\t", false, true))
			f.close()
	if status != "ok":
		push_error("[S1B_ARM_FAIL] %s" % reason)
	print("[S1B_ARM] %s %s" % [status, reason])
	if _scene != null:
		_scene.queue_free()
	_scene = null
	_node = null
	quit(0 if status == "ok" else 1)


func _fail(reason: String) -> void:
	_finish("failed", reason)


func _parse_job() -> bool:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--job="):
			var text := FileAccess.get_file_as_string(arg.substr(6))
			var parsed = JSON.parse_string(text)
			if typeof(parsed) == TYPE_DICTIONARY:
				_job = parsed
				return true
	return false


func _vec3(a) -> Vector3:
	return Vector3(float(a[0]), float(a[1]), float(a[2]))


func _camera_basis(rows) -> Basis:
	# basis_rows are matrix rows; Godot's Basis constructor takes columns.
	return Basis(
		Vector3(float(rows[0][0]), float(rows[1][0]), float(rows[2][0])),
		Vector3(float(rows[0][1]), float(rows[1][1]), float(rows[2][1])),
		Vector3(float(rows[0][2]), float(rows[1][2]), float(rows[2][2])))


func _clip_proof(aabb: AABB, cam_pos: Vector3, forward: Vector3, near: float, far: float) -> Dictionary:
	var grown := aabb.grow(CLIP_PROOF_MARGIN)
	var min_depth := INF
	var max_depth := -INF
	var max_dist := 0.0
	for i in range(8):
		var corner := grown.get_endpoint(i)
		var rel := corner - cam_pos
		var depth := rel.dot(forward)
		min_depth = min(min_depth, depth)
		max_depth = max(max_depth, depth)
		max_dist = max(max_dist, rel.length())
	return {
		"aabb_position": [aabb.position.x, aabb.position.y, aabb.position.z],
		"aabb_size": [aabb.size.x, aabb.size.y, aabb.size.z],
		"margin_m": CLIP_PROOF_MARGIN,
		"min_depth": min_depth, "max_depth": max_depth, "max_corner_distance": max_dist,
		"near": near, "far": far,
		# The proof concerns the far CLAMP (1000 instead of the frozen 1e6): every point of the grown
		# AABB lies closer than far. The near plane is the frozen intrinsic and is not changed; a
		# camera inside the bounds (inside-root views, required by the ADR) has min_depth <= near
		# and clips nearby splats, identically for the reference and every arm (same camera), so
		# that is recorded, not rejected.
		"camera_inside_grown_bounds": grown.has_point(cam_pos),
		"clips_nothing": near > 0.0 and max_depth < far and max_dist < far,
	}


func _sample_stats(renderer) -> Dictionary:
	var stats: Dictionary = renderer.get_render_stats()
	var sample: Dictionary = {}
	for key in RECORDED_STAT_KEYS:
		sample[key] = stats.get(key, null)
	var live: Dictionary = renderer.get_overflow_stats()
	for key in [LIVE_SERIAL_KEY] + LIVE_ZERO_COUNTERS + LIVE_CONTEXT_KEYS:
		sample["live_" + key] = live.get(key, null)
	sample["rendered_content"] = renderer.has_rendered_content()
	return sample


func _live_advanced(window: Array) -> bool:
	if window.size() < 2:
		return false
	var first = window[0].get("live_" + LIVE_SERIAL_KEY)
	var last = window[window.size() - 1].get("live_" + LIVE_SERIAL_KEY)
	var inserts = window[window.size() - 1].get("live_aggregated_records")
	return typeof(first) == TYPE_INT and typeof(last) == TYPE_INT and int(first) > 0 and int(last) > int(first) and typeof(inserts) == TYPE_INT and int(inserts) > 0


func _stages_ok(sample: Dictionary) -> bool:
	if sample.get("stage_metrics_valid") != true:
		return false
	for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
		if sample.get(key) != "success":
			return false
	return sample.get("rendered_content") == true


func _capture() -> Image:
	var image := get_root().get_texture().get_image()
	if image == null or image.is_empty():
		return null
	image.convert(Image.FORMAT_RGB8)
	return image


func _nonblack(image: Image) -> int:
	var data := image.get_data()
	var count := 0
	for i in range(0, data.size(), 3):
		if data[i] != 0 or data[i + 1] != 0 or data[i + 2] != 0:
			count += 1
	return count


func _run() -> void:
	if not _parse_job():
		_fail("missing or unreadable --job=<json>")
		return
	var world_path := str(_job["world"])
	var expected_count := int(_job["expected_splats"])
	var cam: Dictionary = _job["camera"]
	var viewport_size := Vector2i(int(cam["viewport"][0]), int(cam["viewport"][1]))
	_telemetry["world"] = world_path
	_telemetry["expected_splats"] = expected_count
	_telemetry["camera_job"] = cam
	_telemetry["engine_version"] = Engine.get_version_info()

	if OS.has_feature("headless") or DisplayServer.get_name() == "headless":
		_fail("render requires a non-headless window")
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail("no RenderingDevice")
		return
	_telemetry["adapter"] = rd.get_device_name()
	_telemetry["api_version"] = rd.get_device_api_version() if rd.has_method("get_device_api_version") else ""
	_telemetry["driver_version"] = rd.get_device_vendor_name() if rd.has_method("get_device_vendor_name") else ""
	if get_root().get_child_count() != 0:
		_fail("foreign root children would contaminate the capture")
		return

	# Fixture guards (missing file, byte drift, thinned import).
	if not FileAccess.file_exists(world_path):
		_fail("arm world missing: %s" % world_path)
		return
	var actual_sha := FileAccess.get_sha256(world_path)
	_telemetry["world_sha256"] = actual_sha
	if actual_sha != str(_job["expected_sha256"]):
		_fail("world sha256 %s != expected %s (stale or substituted fixture)" % [actual_sha, _job["expected_sha256"]])
		return
	var world = ResourceLoader.load(world_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	if world == null or not (world is GaussianSplatWorld):
		_fail("ResourceLoader did not return a GaussianSplatWorld for %s" % world_path)
		return
	if world.get_gaussian_data() == null and world.materialize_resident_gaussian_data() != OK:
		_fail("world has no resident GaussianData and materialization failed")
		return
	var data: GaussianData = world.get_gaussian_data()
	_telemetry["world_splat_count"] = world.get_splat_count()
	_telemetry["data_count"] = data.get_count()
	_telemetry["sh_degree"] = world.get_sh_degree()
	_telemetry["sh_first_order_count"] = world.get_sh_first_order_count()
	_telemetry["sh_high_order_count"] = world.get_sh_high_order_count()
	_telemetry["world_has_hlod_tree"] = world.has_hlod_tree()
	_telemetry["payload_mode"] = world.get_payload_mode()
	if expected_count <= 0 or world.get_splat_count() != expected_count or data.get_count() != expected_count:
		_fail("splat count %d/%d != expected %d (zero-splat or thinned fixture)" % [world.get_splat_count(), data.get_count(), expected_count])
		return

	# This process owns these settings; nothing is saved. Canonical resident route:
	# arms are materialized cuts, so the HLOD runtime cut is off for every arm.
	ProjectSettings.set_setting(SETTINGS_PREFIX + "lod/hlod_runtime_cut_enabled", false)
	ProjectSettings.set_setting(SETTINGS_PREFIX + "streaming/route_policy", 0)
	ProjectSettings.set_setting(SETTINGS_PREFIX + "instance_pipeline/enabled", true)
	ProjectSettings.set_setting(SETTINGS_PREFIX + "sorting/force_cpu_sort", false)
	ProjectSettings.set_setting(SETTINGS_PREFIX + "compression/per_chunk_quantization", 0)
	for key in _job.get("project_settings", {}):
		ProjectSettings.set_setting(SETTINGS_PREFIX + key, _job["project_settings"][key])
	ProjectSettings.emit_signal("settings_changed")
	var settings_dump: Dictionary = {}
	for prop in ProjectSettings.get_property_list():
		var name := str(prop["name"])
		if name.begins_with(SETTINGS_PREFIX):
			settings_dump[name] = str(ProjectSettings.get_setting(name))
	_telemetry["project_settings"] = settings_dump

	get_root().size = viewport_size
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED
	RenderingServer.set_default_clear_color(Color.BLACK)

	_scene = Node3D.new()
	get_root().add_child(_scene)
	var env_node := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0, 0, 0, 1)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.tonemap_exposure = 1.0
	env_node.environment = env
	_scene.add_child(env_node)

	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_PERSPECTIVE
	camera.keep_aspect = Camera3D.KEEP_HEIGHT
	camera.fov = float(cam["vertical_fov_degrees"])
	camera.near = float(cam["near"])
	camera.far = float(cam["far"])
	_scene.add_child(camera)
	camera.transform = Transform3D(_camera_basis(cam["basis_rows"]), _vec3(cam["position"]))
	camera.make_current()
	var forward := -camera.global_transform.basis.z
	var to_target := (_vec3(cam["target"]) - camera.global_position).normalized()
	_telemetry["camera"] = {
		"fov": camera.fov, "near": camera.near, "far": camera.far, "position": cam["position"],
		"forward": [forward.x, forward.y, forward.z], "aim_dot": forward.dot(to_target),
		"viewport": [viewport_size.x, viewport_size.y],
	}
	if forward.dot(to_target) < 0.999999:
		_fail("camera basis does not look at the frozen target (dot=%f)" % forward.dot(to_target))
		return
	var clip := _clip_proof(data.get_aabb(), camera.global_position, forward, camera.near, camera.far)
	_telemetry["clip_proof"] = clip
	if not clip["clips_nothing"]:
		_fail("far plane may clip the asset: %s" % JSON.stringify(clip))
		return

	_node = GaussianSplatWorld3D.new()
	_node.set_auto_apply_on_ready(false)
	_node.world = world
	_scene.add_child(_node)
	_node.apply_world()

	var deadline := Time.get_ticks_msec() + READY_TIMEOUT_MSEC
	var frames_waited := 0
	var window: Array = []
	var last_frame_count := -1
	var renderer = null
	while Time.get_ticks_msec() < deadline:
		await RenderingServer.frame_post_draw
		frames_waited += 1
		renderer = _node.get_renderer()
		if renderer == null:
			continue
		var sample := _sample_stats(renderer)
		var frame_count := int(sample.get("frame_count", -1))
		# A post-draw wake can still expose the previous renderer observation.
		if frame_count <= last_frame_count:
			continue
		last_frame_count = frame_count
		if not _stages_ok(sample) or int(sample.get("total_splats", -1)) != expected_count or int(sample.get("visible_splats", 0)) <= 0:
			window.clear()
			_telemetry["last_unready_sample"] = sample
			continue
		var image := _capture()
		if image == null:
			window.clear()
			continue
		sample["image_sha256"] = _sha256_bytes(image.get_data())
		sample["image_size"] = [image.get_width(), image.get_height()]
		sample["_image"] = image
		window.append(sample)
		if window.size() > STABLE_FRAMES:
			window.pop_front()
		# The stable window must also span an advance of the live overflow readback, so the loss
		# counters were sampled while this exact static scene was on screen.
		if window.size() == STABLE_FRAMES and _live_advanced(window):
			break
	_telemetry["frames_waited"] = frames_waited
	if window.size() != STABLE_FRAMES:
		_fail("no %d consecutive fresh success-status frames with total_splats == %d before the deadline (renderer=%s); last unready: %s" % [STABLE_FRAMES, expected_count, str(renderer != null), JSON.stringify(_telemetry.get("last_unready_sample", {}))])
		return

	var final_sample: Dictionary = window[window.size() - 1]
	var image: Image = final_sample["_image"]
	var distinct_hashes := {}
	var frames_out: Array = []
	for s in window:
		distinct_hashes[s["image_sha256"]] = true
		var copy: Dictionary = s.duplicate()
		copy.erase("_image")
		frames_out.append(copy)
	_telemetry["frames"] = frames_out
	_telemetry["distinct_frame_hashes"] = distinct_hashes.size()

	# Loss counters: typed, present, ADVANCING (live) and zero. Absence is never a pass.
	if not _live_advanced(window):
		_fail("live overflow readback did not advance across the stable window (%s): zero-loss is unproven" % JSON.stringify(window.map(func(w): return w.get("live_" + LIVE_SERIAL_KEY))))
		return
	for s in window:
		for key in LIVE_ZERO_COUNTERS:
			if typeof(s.get("live_" + key)) != TYPE_INT:
				_fail("required live loss counter '%s' missing or not an int" % key)
				return
			if int(s["live_" + key]) != 0:
				_fail("loss counter %s = %d (cap/drop/overflow observed)" % [key, int(s["live_" + key])])
				return
	for s in window:
		if s.get("hlod_active") != false:
			_fail("hlod_active is not false: arm is not on the canonical route (%s)" % str(s.get("hlod_reason")))
			return
		for key in ["visible_splats", "sorted_splats", "total_splats"]:
			if s.get(key) != final_sample.get(key):
				_fail("%s changed across the stable window" % key)
				return
		if s.get("raster_overlap_thinning_keep_ratio") != null and float(s["raster_overlap_thinning_keep_ratio"]) < 1.0:
			_fail("overlap-record thinning active (keep_ratio=%s)" % str(s["raster_overlap_thinning_keep_ratio"]))
			return
	if distinct_hashes.size() != 1:
		_fail("static scene produced %d distinct frame images in the stable window (flicker)" % distinct_hashes.size())
		return

	if image.get_size() != viewport_size:
		_fail("capture size %s != requested %s" % [str(image.get_size()), str(viewport_size)])
		return
	var nonblack := _nonblack(image)
	_telemetry["nonblack_pixels"] = nonblack
	if nonblack < MIN_NONBLACK_PIXELS or nonblack >= image.get_width() * image.get_height():
		_fail("black or saturated frame: %d non-black pixels" % nonblack)
		return

	var png_path := str(_job["out_png"])
	if image.save_png(png_path) != OK:
		_fail("cannot write %s" % png_path)
		return
	_telemetry["png"] = png_path
	_telemetry["png_sha256"] = FileAccess.get_sha256(png_path)
	_telemetry["drawn"] = {
		"visible_splats": int(final_sample["visible_splats"]),
		"sorted_splats": int(final_sample["sorted_splats"]),
		"total_splats": int(final_sample["total_splats"]),
	}
	_finish("ok", "rendered")


func _sha256_bytes(bytes: PackedByteArray) -> String:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(bytes)
	return ctx.finish().hex_encode()
