extends SceneTree

# ADR dynamics §7. One producer for the subject and separate control processes.
# No row is filled with default zeros. Missing readbacks remain absent.
const Content = preload("gs_combined_1_content.gd")
const WIND = "rendering/gaussian_splatting/animation/"
const PIPE = "rendering/gaussian_splatting/pipeline/"
const DEADLINE_MS = 240000
const MAX_CALIBRATION_ATTEMPTS = 8
const CAPTURE_STRIDE = 224 # GSGame1Sample, existing layout guard + contract test.
const VIEW = Vector2i(1920, 1080)
const RAW_STRIDE = 176 # PackedGaussian; checked against the bridge's raw_stride.
const PAGE_SPLATS = 1024 # GS_HLOD_PAGE_SPLATS / HlodNodeStreamer DEFAULT_PAGE_SPLATS.

var _plan: Dictionary
var _raw: Dictionary = {"rows": {}, "frames": [], "completed": false}
var _scene: Node3D
var _camera: Camera3D
var _world: GaussianSplatWorld3D
var _renderer: GaussianSplatRenderer
var _objects: Array = []
var _assets: Array = []
var _character: MeshInstance3D
var _sun: DirectionalLight3D
var _omnis: Array = []
var _spot: SpotLight3D
var _overrides: Dictionary = {}
var _regions: Array = []
var _spawned = false
var _mail_mutex = Mutex.new()
var _mail: Dictionary = {}
var _mail_id = 0
var _stall_shader: RID
var _stall_pipeline: RID
var _stall_buffer: RID
var _stall_set: RID
var _stall_serial = 0
var _stall_results: Dictionary = {}
var _last_timestamp_frame = -1
var _last_image_proof: Dictionary = {}


func _init() -> void:
	call_deferred("_run")


func _json_value(value: Variant) -> Variant:
	if value is Dictionary:
		var result: Dictionary = {}
		for key in value:
			result[str(key)] = _json_value(value[key])
		return result
	if value is Array:
		var result: Array = []
		for item in value:
			result.append(_json_value(item))
		return result
	if value is Vector2 or value is Vector2i:
		return [value.x, value.y]
	if value is Vector3 or value is Vector3i:
		return [value.x, value.y, value.z]
	if value is Vector4:
		return [value.x, value.y, value.z, value.w]
	if value is RID:
		return value.get_id()
	return value


func _sha(bytes: PackedByteArray) -> String:
	var context = HashingContext.new()
	if context.start(HashingContext.HASH_SHA256) != OK or context.update(bytes) != OK:
		return ""
	return context.finish().hex_encode()


func _save() -> void:
	var f = FileAccess.open(str(_plan["output"]), FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(_json_value(_raw), "  ", true, true))
		f.close()


func _render(call: Callable) -> Variant:
	_mail_id += 1
	var ticket = _mail_id
	RenderingServer.call_on_render_thread(func():
		var value = call.call()
		_mail_mutex.lock()
		_mail[ticket] = value
		_mail_mutex.unlock())
	var deadline = Time.get_ticks_msec() + DEADLINE_MS
	while Time.get_ticks_msec() <= deadline:
		_mail_mutex.lock()
		var ready = _mail.has(ticket)
		var value = _mail.get(ticket)
		if ready:
			_mail.erase(ticket)
		_mail_mutex.unlock()
		if ready:
			return value
		await process_frame
	return {"error": "render-thread call deadline"}


func _native(method: String, args: Array = []) -> Variant:
	return await _render(func(): return _renderer.callv(method, args))


func _setting(key: String, value: Variant) -> void:
	ProjectSettings.set_setting(key, value)
	ProjectSettings.emit_signal("settings_changed")


func _pose_a() -> void:
	_camera.look_at_from_position(Vector3(0, 3, 24), Vector3(0, 1, -16), Vector3.UP)


func _build() -> String:
	# A refused world ends the arm here: nothing can settle without it, and the
	# loader's refusal text is in this process's log (godot.log).
	var loaded = ResourceLoader.load(str(_plan["world"]))
	if not loaded is GaussianSplatWorld:
		return "NOT_RUN: world failed to load (ResourceLoader refused %s; see godot.log)" % str(_plan["world"])
	_scene = Node3D.new()
	root.add_child(_scene)
	var environment = Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color.BLACK
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.15, 0.15, 0.15)
	environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var env = WorldEnvironment.new()
	env.environment = environment
	_scene.add_child(env)
	_camera = Camera3D.new()
	_camera.fov = 75
	_camera.far = 2000
	_scene.add_child(_camera)
	_camera.make_current()
	_pose_a()
	_world = GaussianSplatWorld3D.new()
	_world.set_auto_apply_on_ready(false)
	_world.world = loaded
	_world.set_max_splat_count(500000)
	_world.set_max_render_distance(1800)
	_world.set_lod_enabled(true)
	_world.set_cast_shadow(true)
	_world.wind_mode = GaussianSplatWorld3D.WIND_MODE_AUTO
	_scene.add_child(_world)
	_world.apply_world()
	for name in ["near", "mid", "far"]:
		var id = _world.get_region_id(name)
		_regions.append(id)
		var override = GaussianSplatRegionOverride.new()
		_world.set_region_override(id, override)
		_overrides[id] = override
	for kind in range(4):
		_assets.append(Content.build_asset(kind))
	for i in range(32):
		var node = GaussianSplatNode3D.new()
		node.set_splat_asset(_assets[i / 8]["asset"])
		node.set_cast_shadow(true)
		node.set_wind_override_enabled(true)
		node.set_wind_enabled(false)
		node.transform = Content.object_transform(i, 0)
		_scene.add_child(node)
		_objects.append(node)
	_character = MeshInstance3D.new()
	_character.mesh = CapsuleMesh.new()
	_character.position = Vector3(2, 1, -8)
	_scene.add_child(_character)
	_sun = DirectionalLight3D.new()
	_sun.shadow_enabled = true
	_scene.add_child(_sun)
	_sun.look_at_from_position(Vector3.ZERO, Content.SUN_DIRECTION, Vector3.UP)
	for i in range(4):
		var light = OmniLight3D.new()
		light.shadow_enabled = true
		light.omni_range = 32
		light.position = Content.OMNI_POSITIONS[i] + Vector3(0, 4, 0)
		_scene.add_child(light)
		_omnis.append(light)
	_spot = SpotLight3D.new()
	_spot.shadow_enabled = true
	_spot.spot_range = 40
	_spot.position = Vector3(0, 8, -6)
	_scene.add_child(_spot)
	# L2 reads the spot at pose A. Aim it at the pose-A eye, as omni[0] lights the
	# splats facing the camera: the ground at (0, 0, -10) is hidden from pose A.
	_spot.look_at(Vector3(0, 3, 24), Vector3.UP)
	_spot.light_energy = 0 # L2 isolates it in its diagnostic pass.
	return ""


func _inputs(sample: int) -> String:
	var t = float(sample) / 60.0
	if t < 6:
		_camera.look_at_from_position(Vector3(0, 3, 24 - t * 2), Vector3(0, 1, -16), Vector3.UP)
	elif t < 12:
		_camera.look_at_from_position(Vector3(-180, 12, 180), Vector3(-140, 1, 140), Vector3.UP)
	else:
		_pose_a() # Return to exactly pose A, then hold it.
	for i in range(32):
		_objects[i].transform = Content.object_transform(i, t)
	_character.position.y = 1 + 0.15 * sin(t * 3)
	_character.rotation.y = 0.25 * sin(t * 2)
	if bool(_plan["dynamics"]):
		for k in range(_regions.size()):
			_overrides[_regions[k]].tint = Color.from_hsv(fmod(floor(t / 2) * 0.17 + k * 0.3, 1), 0.8, 1)
			_overrides[_regions[k]].tint_strength = 0.7
		_overrides[_regions[0]].opacity = 0.0 if int(t / 4) % 2 else 1.0
	if sample == 600 and not _spawned:
		var asset = Content.build_asset(0)["asset"]
		var positions = asset.get_positions()
		positions[0] += 0.25 # A genuinely new payload/asset set at t = 10 s.
		asset.set_positions(positions)
		var node = GaussianSplatNode3D.new()
		node.set_splat_asset(asset)
		node.position = Vector3(1, 1, -6)
		node.set_cast_shadow(true)
		node.set_wind_override_enabled(true)
		node.set_wind_enabled(false)
		_scene.add_child(node)
		_objects.append(node)
		_spawned = true
	var digest_input: Array = [sample, _camera.global_transform, _character.transform, _spawned]
	for i in range(32):
		digest_input.append(_objects[i].transform)
	return JSON.stringify(digest_input).sha256_text()


func _stats_at_pose(stats: Dictionary) -> bool:
	# Existing flythrough probe's camera synchronization tolerance; this only
	# rejects stale telemetry, and is not a new ADR acceptance threshold.
	for part in ["origin", "forward", "up"]:
		for axis in ["x", "y", "z"]:
			if not stats.has("debug_cam_" + part + "_" + axis):
				return false
	var origin = Vector3(stats["debug_cam_origin_x"], stats["debug_cam_origin_y"], stats["debug_cam_origin_z"])
	var forward = Vector3(stats["debug_cam_forward_x"], stats["debug_cam_forward_y"], stats["debug_cam_forward_z"])
	var up = Vector3(stats["debug_cam_up_x"], stats["debug_cam_up_y"], stats["debug_cam_up_z"])
	return origin.distance_to(_camera.global_position) <= 0.001 and forward.distance_to(-_camera.global_transform.basis.z) <= 0.001 and up.distance_to(_camera.global_transform.basis.y) <= 0.001


func _settle() -> Dictionary:
	var start = Time.get_ticks_msec()
	var streak = 0
	var digest = null
	while Time.get_ticks_msec() - start <= DEADLINE_MS:
		await RenderingServer.frame_post_draw
		var elapsed = Time.get_ticks_msec() - start
		var stats = _renderer.get_render_stats()
		var ready = stats.get("hlod_active") == true and int(stats.get("hlod_stream_drawn_nodes", 0)) > 0
		for key in ["hlod_stream_desired_nodes", "hlod_stream_loading_nodes", "hlod_stream_failed_nodes", "hlod_stream_blocked_groups"]:
			ready = ready and stats.has(key) and int(stats[key]) == 0
		ready = ready and _stats_at_pose(stats)
		ready = ready and stats.get("hlod_drawn_node_digest") != null and not stats.get("hlod_drawn_node_ids", []).is_empty()
		if ready and stats.get("hlod_drawn_node_digest") == digest:
			streak += 1
		else:
			streak = 0
			digest = stats.get("hlod_drawn_node_digest")
		if elapsed <= DEADLINE_MS and streak >= 3:
			return {"ready": true, "elapsed_ms": elapsed, "stats": stats}
	return {"ready": false, "reason": "HLOD settle deadline", "elapsed_ms": Time.get_ticks_msec() - start}


func _buffer_bytes(buffer: RID, p_offset: int = 0, p_size: int = 0) -> PackedByteArray:
	_mail_id += 1
	var ticket = _mail_id
	var submission = await _render(func():
		var rd = RenderingServer.get_rendering_device()
		return rd.buffer_get_data_async(buffer, func(bytes: PackedByteArray):
			_mail_mutex.lock()
			_mail[ticket] = bytes
			_mail_mutex.unlock(), p_offset, p_size))
	if submission != OK:
		return PackedByteArray()
	var deadline = Time.get_ticks_msec() + DEADLINE_MS
	while Time.get_ticks_msec() <= deadline:
		_mail_mutex.lock()
		var ready = _mail.has(ticket)
		var bytes = _mail.get(ticket)
		if ready:
			_mail.erase(ticket)
		_mail_mutex.unlock()
		if ready:
			return bytes
		await RenderingServer.frame_post_draw
	return PackedByteArray()


func _vec(bytes: PackedByteArray, offset: int) -> Vector3:
	return Vector3(bytes.decode_float(offset), bytes.decode_float(offset + 4), bytes.decode_float(offset + 8))


func _capture(keys: Array, region: int = 0, level: int = -1, author: Callable = Callable()) -> Dictionary:
	var request = await _native("_test_gs_game_1_capture", [keys, region, level])
	if request.has("error"):
		return request
	if author.is_valid():
		author.call()
	var armed_frame = await _native("_test_gs_game_1_arm_capture")
	await RenderingServer.frame_post_draw
	var bytes = await _buffer_bytes(request["buffer"])
	if bytes.size() != int(request["bytes"]):
		return {"error": "capture readback absent/truncated"}
	var samples: Array = []
	for i in range(keys.size()):
		var at = 16 + i * CAPTURE_STRIDE
		var sample: Dictionary = {"key": keys[i], "colour_valid": bytes.decode_u32(at + 16) == int(request["epoch"]),
			"shadow_valid": bytes.decode_u32(at + 20) == int(request["epoch"]), "semantic": bytes.decode_u32(at + 24),
			"local_rest": _vec(bytes, at + 208), "level": bytes.decode_u32(at + 28), "colour_frame": bytes.decode_u32(at + 60), "shadow_frame": bytes.decode_u32(at + 124), "rest": _vec(bytes, at + 32), "colour": _vec(bytes, at + 48),
			"screen": _vec(bytes, at + 64), "tint": _vec(bytes, at + 80), "shadow": _vec(bytes, at + 96),
			"shadow_anchor": _vec(bytes, at + 112), "viewport": Vector2(bytes.decode_float(at + 192), bytes.decode_float(at + 196))}
		var matrix: Array = []
		for column in range(4):
			var c = at + 128 + column * 16
			matrix.append(Vector4(bytes.decode_float(c), bytes.decode_float(c + 4), bytes.decode_float(c + 8), bytes.decode_float(c + 12)))
		sample["light_from_frame"] = matrix
		samples.append(sample)
	return {"epoch": request["epoch"], "armed_frame": armed_frame, "sha256": _sha(bytes), "samples": samples,
		"target_region_sort_records": bytes.decode_u32(8), "observation": await _native("_test_gs_game_1_observe")}


func _world_keys(obs: Dictionary, count: int = 16) -> Array:
	var keys: Array = []
	var drawn = obs.get("stats", {}).get("hlod_drawn_node_ids", [])
	for node in drawn:
		if keys.size() >= count:
			break
		keys.append({"node": int(node), "local": 0, "row": 0})
	return keys


func _project(matrix: Array, point: Vector3, viewport: Vector2) -> Vector2:
	var p: Vector4 = matrix[0] * point.x + matrix[1] * point.y + matrix[2] * point.z + matrix[3]
	return Vector2(p.x, p.y) / p.w * viewport * 0.5


func _count_diff(a: Image, b: Image) -> int:
	if a == null or b == null or a.get_size() != b.get_size() or a.get_format() != b.get_format():
		return -1
	var aa = a.get_data()
	var bb = b.get_data()
	var stride = aa.size() / (a.get_width() * a.get_height())
	var changed = 0
	for i in range(a.get_width() * a.get_height()):
		var same = true
		for j in range(stride):
			same = same and aa[i * stride + j] == bb[i * stride + j]
		changed += 0 if same else 1
	return changed


func _count_lit(dark: Image, lit: Image) -> int:
	if dark == null or lit == null or dark.get_size() != lit.get_size():
		return -1
	var count = 0
	for y in range(lit.get_height()):
		for x in range(lit.get_width()):
			var a = dark.get_pixel(x, y)
			var b = lit.get_pixel(x, y)
			count += 1 if b.r + b.g + b.b > a.r + a.g + a.b else 0
	return count


func _wait_image() -> Image:
	var deadline = Time.get_ticks_msec() + DEADLINE_MS
	while Time.get_ticks_msec() <= deadline:
		var result = await _native("_test_gs_game_1_image_poll")
		if Time.get_ticks_msec() > deadline:
			return null
		if result.has("error"):
			return null
		if result.get("complete") == true:
			var format = -1
			match int(result["format"]):
				RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT: format = Image.FORMAT_RGBAH
				RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT: format = Image.FORMAT_RGBAF
				RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM: format = Image.FORMAT_RGBA8
			if format < 0:
				return null
			var bytes: PackedByteArray = result["bytes"]
			result.erase("bytes")
			result["sha256"] = _sha(bytes)
			_last_image_proof = result
			return Image.create_from_data(int(result["width"]), int(result["height"]), false, format, bytes)
		await RenderingServer.frame_post_draw
	return null


func _image() -> Image:
	var request = await _native("_test_gs_game_1_image_request", [[], _camera.global_transform])
	if request.get("armed") != true:
		return null
	return await _wait_image()


func _image_sha(image: Image) -> String:
	return _sha(image.get_data()) if image != null else ""


func _pages(obs: Dictionary, ids: Array = []) -> Array:
	var pages: Array = []
	for node in obs.get("nodes", []):
		if ids.is_empty() or int(node["node"]) in ids:
			for page in node["pages"]:
				pages.append(int(page))
	pages.sort()
	return pages


# ADR §7 W1 step 3: "Force eviction of the drawn set (page-budget churn). Pre-occupy
# the freed runs so readmission lands on different pages."
#
# Why page pressure alone did not evict it (run 45231549548: 16/62), measured locally
# with the per-pose counters below: the atlas was full (0 free pages, refusals only
# "pending retire"), no pose-A node held a lease or an edit hold, and every pose-A
# node that survived had been drawn again by the churn. The LRU kept what was still
# drawn, correctly; nothing was pinned. Churn views that avoid pose A entirely (the
# arcs below) stopped redrawing it, but their new demand ran out before the LRU
# reached the last pose-A nodes (43-60 of 62).
#
# So the churn keeps the page pressure (the arcs keep the atlas full and pre-occupy
# every freed page) and forces the eviction itself through the residency's own
# evict(), the call the LRU makes: _test_gs_game_1_evict, TESTS_ENABLED only. It
# refuses protected nodes, so a node still drawn or leased stays resident and the
# rows stay NOT_RUN. The atlas budget is unchanged.
#
# Pose A stands at (0, 3, 24) looking -z. Every churn camera stands on an arc in the
# +z half and looks outward, at most 60 degrees from +z (45 on arcs nearer than
# 130 m): a 60-degree view from 60 m redrew pose-A nodes locally.
func _churn_poses() -> Array:
	var poses: Array = []
	for radius in [230.0, 180.0, 120.0, 60.0]:
		for step in range(-4, 5):
			if radius < 130 and absi(step) > 3:
				continue
			var angle = deg_to_rad(15.0 * step)
			var pose = Vector3(radius * sin(angle), 8, radius * cos(angle))
			if absf(pose.x) <= 250 and absf(pose.z) <= 250:
				poses.append(pose)
	return poses


func _churn(before: Dictionary) -> Dictionary:
	var ids: Array = []
	for id in before.get("stats", {}).get("hlod_drawn_node_ids", []):
		ids.append(int(id))
	var old_pages = _pages(before, ids)
	var evidence: Array = []
	var deadline = Time.get_ticks_msec() + DEADLINE_MS
	var last: Dictionary = {}
	for pose in _churn_poses():
		var outward = Vector3(pose.x, 0, pose.z).normalized()
		_camera.look_at_from_position(pose, Vector3(pose.x, 1, pose.z) + outward * 100, Vector3.UP)
		var settled = await _settle()
		var obs = await _native("_test_gs_game_1_observe")
		# Page pressure first; then force out what it left, never a protected node.
		var lru_evicted: Array = []
		var still: Array = []
		for node in obs.get("nodes", []):
			if int(node["node"]) in ids:
				if node["resident"]:
					still.append(int(node["node"]))
				else:
					lru_evicted.append(int(node["node"]))
		var forced: Dictionary = {}
		if not still.is_empty():
			forced = await _native("_test_gs_game_1_evict", [still])
			obs = await _native("_test_gs_game_1_observe")
		last = obs
		var evicted: Array = []
		var occupied: Array = []
		for node in obs.get("nodes", []):
			if int(node["node"]) in ids and not node["resident"]:
				evicted.append(int(node["node"]))
			if int(node["node"]) not in ids:
				for page in node["pages"]:
					if int(page) in old_pages and int(page) not in occupied:
						occupied.append(int(page))
		evidence.append({"pose": [int(pose.x), int(pose.z)], "settled": settled.get("ready"), "evicted": evicted,
			"absent_before_forcing": lru_evicted, "forced": forced, "preoccupied": occupied,
			"residency": _churn_counters(obs, ids)})
		if not ids.is_empty() and evicted.size() == ids.size() and not old_pages.is_empty() and occupied.size() == old_pages.size():
			return {"evicted_nodes": evicted, "preoccupied_pages": occupied, "evidence": evidence}
		if Time.get_ticks_msec() > deadline:
			break
	return {"error": "full drawn-set eviction and preoccupation not observed", "evidence": evidence,
		"not_run_reason": _churn_reason(ids, evidence, last)}


const CHURN_COUNTERS = ["hlod_stream_pages_capacity", "hlod_stream_pages_free", "hlod_stream_pages_resident",
	"hlod_stream_pages_retiring", "hlod_stream_evicted_total", "hlod_stream_allocation_refused_total",
	"hlod_stream_allocation_protected_capacity_total", "hlod_stream_allocation_eviction_limit_total",
	"hlod_stream_allocation_pending_retire_total", "hlod_stream_allocation_over_page_budget_total",
	"hlod_stream_page_refused_groups_total", "hlod_stream_skipped_groups_total"]


# Distinguishes an LRU whose budget never filled (free pages remain, no refusal)
# from nodes pinned or leaked (held by leases, edit holds or stamps while the
# allocator refuses). Reported per churn pose; changes nothing.
func _churn_counters(obs: Dictionary, ids: Array) -> Dictionary:
	var stats: Dictionary = obs.get("stats", {})
	var counters: Dictionary = {}
	for key in CHURN_COUNTERS:
		counters[key] = stats.get(key)
	var held: Array = []
	var nodes: Array = obs.get("nodes", [])
	for id in ids:
		if id < nodes.size() and nodes[id]["resident"]:
			var hold: Dictionary = nodes[id].get("hold", {})
			held.append({"node": id, "level": nodes[id]["level"], "pages": nodes[id]["pages"].size(),
				"leases": hold.get("leases"), "sibling_leases": hold.get("sibling_leases"),
				"drawn_this_frame": hold.get("drawn_this_frame"), "edit_hold": hold.get("edit_hold"),
				"pinned_root": hold.get("pinned_root"), "last_drawn_frame": hold.get("last_drawn_frame")})
	counters["residency_frame"] = obs.get("residency_frame")
	counters["pose_a_resident"] = held
	return counters


# Names why W1/C1/R2 cannot run: the churn never evicted the whole pose-A set.
# Survivors are split into drawn, ancestors of drawn (protected by design) and
# the rest, which page pressure alone did not evict.
func _churn_reason(ids: Array, evidence: Array, last: Dictionary) -> String:
	var best = 0
	for item in evidence:
		best = maxi(best, item["evicted"].size())
	var drawn: Array = []
	for id in last.get("stats", {}).get("hlod_drawn_node_ids", []):
		drawn.append(int(id))
	var ancestors: Dictionary = {}
	var nodes: Array = last.get("nodes", [])
	for id in drawn:
		var n = int(nodes[id]["parent"]) if id < nodes.size() else -1
		while n >= 0 and n < nodes.size() and not ancestors.has(n):
			ancestors[n] = true
			n = int(nodes[n]["parent"])
	var stuck = 0
	for id in ids:
		if id < nodes.size() and nodes[id]["resident"] and id not in drawn and not ancestors.has(id):
			stuck += 1
	return "churn cannot reach full eviction: %d/%d; %d non-drawn non-ancestor nodes stay resident" % [best, ids.size(), stuck]


func _identity() -> void:
	_setting(WIND + "wind_time_scale", 0.0)
	_inputs(0)
	for override in _overrides.values():
		override.opacity = 1
		override.tint_strength = 0
	var arms: Array = []
	for divisor in [8, 0]:
		_setting(PIPE + "enable_sh_amortization", divisor != 0)
		_setting(PIPE + "sh_amortization_divisor", maxi(divisor, 1))
		_pose_a()
		var settled = await _settle()
		var before = await _native("_test_gs_game_1_observe")
		var probe_pages = await _atlas_pages(before, _drawn_nodes(before))
		# W1's wind_nonzero oracle selects its probes from these pages.
		var missing = _atlas_missing(before, _drawn_nodes(before), probe_pages)
		var keys = _wind_keys(before, probe_pages)
		var capture = await _capture(keys, _regions[1]) if not keys.is_empty() else {}
		var wind_nonzero = false
		for sample in capture.get("samples", []):
			wind_nonzero = wind_nonzero or (sample["colour_valid"] and sample["rest"] != sample["colour"])
		var image_a = await _image()
		var churn = await _churn(before)
		var first: Image = null
		var first_proof: Dictionary = {}
		var reference: Image = null
		var reference_proof: Dictionary = {}
		if not churn.has("error"):
			# Arm while every original node is still absent, before returning the camera.
			var away = _camera.global_transform
			_pose_a()
			var target_pose = _camera.global_transform
			_camera.global_transform = away
			var armed = await _native("_test_gs_game_1_image_request", [churn["evicted_nodes"], target_pose])
			_pose_a()
			if armed.get("armed") == true:
				first = await _wait_image()
				first_proof = _last_image_proof.duplicate(true) if first != null else {}
				if divisor == 8 and first != null:
					_setting(PIPE + "sh_amortization_divisor", 1)
					reference = await _image()
					reference_proof = _last_image_proof.duplicate(true) if reference != null else {}
					_setting(PIPE + "sh_amortization_divisor", 8)
		else:
			_pose_a()

		var returned = await _settle()
		# Oracle §7 step 7: after >= 2 frames; the settle streak is >=3.
		var changed = 0
		var comparisons: Array = []
		for i in range(3):
			var image = await _image()
			var difference = _count_diff(image_a, image)
			comparisons.append({"changed_pixels": difference, "image_sha256": _image_sha(image)})
			changed += 1 if difference > 0 else 0
		var after = await _native("_test_gs_game_1_observe")
		var ids: Array = []
		for id in before.get("stats", {}).get("hlod_drawn_node_ids", []):
			ids.append(int(id))
		arms.append({"divisor": divisor, "wind_time_scale": ProjectSettings.get_setting(WIND + "wind_time_scale"),
			"wind_nonzero": wind_nonzero, "hysteresis": "after >= 2 frames", "frames_after_readmission": 3,
			"settled": settled.get("ready") == true and returned.get("ready") == true,
			"drawn_nodes_before": before.get("stats", {}).get("hlod_drawn_node_ids", []),
			"evicted_nodes": churn.get("evicted_nodes", []), "preoccupied_pages": churn.get("preoccupied_pages", []),
			"pages_before": _pages(before, ids), "pages_after": _pages(after, ids),
			"drawn_digest_before": before.get("stats", {}).get("hlod_drawn_node_digest"),
			"drawn_digest_after": after.get("stats", {}).get("hlod_drawn_node_digest"),
			"frames_compared": comparisons.size(), "changed_frames": changed, "comparisons": comparisons,
			"baseline_sha256": _image_sha(image_a), "churn": churn, "atlas_missing": missing})
		if divisor == 8 and first != null and reference != null:
			_raw["rows"]["C1"] = {"changed_pixels": [_count_diff(first, reference)],
				"first_frame_after_admission": not first_proof.get("first_readmitted_nodes", []).is_empty(),
				"native_first_frame": first_proof, "native_reference_frame": reference_proof,
				"divisor": 8, "reference_divisor": 1, "same_pose": true,
				"same_drawn_digest": first_proof.get("drawn_digest") != null and first_proof.get("drawn_digest") == reference_proof.get("drawn_digest"),
				"first_sha256": _image_sha(first), "reference_sha256": _image_sha(reference)}
	_raw["rows"]["W1"] = {"arms": arms}
	for arm in arms:
		if not arm["atlas_missing"].is_empty():
			_raw["rows"]["W1"]["not_run_reason"] = "W1 atlas pages not read completely: %s" % _missing_summary(arm["atlas_missing"])
			break
	# Without full eviction W1 and C1 have no oracle (ADR §7 W1 step 3); say why.
	for arm in arms:
		if arm["churn"].has("error"):
			var reason = arm["churn"].get("not_run_reason", arm["churn"]["error"])
			if not _raw["rows"]["W1"].has("not_run_reason"):
				_raw["rows"]["W1"]["not_run_reason"] = reason
			if not _raw["rows"].has("C1"):
				_raw["rows"]["C1"] = {"not_run_reason": reason}
			break


# Reads only the atlas pages that hold the first `max_local` splats of each node
# (0: all). A whole-buffer async readback of the ~320 MB atlas came back
# zero-filled, so every decode saw semantic 0; a ranged read returns the record.
func _atlas_pages(obs: Dictionary, nodes: Array, max_local: int = 64) -> Dictionary:
	var pages: Dictionary = {}
	if int(obs.get("raw_stride", 0)) != RAW_STRIDE or obs.get("raw_payload") != true:
		return pages
	var buffer: RID = obs["atlas_buffer"]
	for node in nodes:
		var count = int(node["splats"]) if max_local <= 0 else mini(int(node["splats"]), max_local)
		for slot in range(mini((count + PAGE_SPLATS - 1) / PAGE_SPLATS, node["pages"].size())):
			var page = int(node["pages"][slot])
			if not pages.has(page):
				var bytes = await _buffer_bytes(buffer, page * PAGE_SPLATS * RAW_STRIDE, PAGE_SPLATS * RAW_STRIDE)
				if bytes.size() == PAGE_SPLATS * RAW_STRIDE:
					pages[page] = bytes
	return pages


# What a complete `_atlas_pages(obs, nodes, max_local)` read lacks: [] when every
# page holding the requested splats arrived at full length. A short or failed read,
# a node with fewer allocated pages than its splats need, or no raw atlas at all is
# missing coverage. The row that reads the pages is then NOT_RUN, never judged on
# whatever pages remained.
func _atlas_missing(obs: Dictionary, nodes: Array, pages: Dictionary, max_local: int = 64) -> Array:
	if int(obs.get("raw_stride", 0)) != RAW_STRIDE or obs.get("raw_payload") != true:
		return ["no raw atlas payload"]
	var missing: Array = []
	for node in nodes:
		var count = int(node["splats"]) if max_local <= 0 else mini(int(node["splats"]), max_local)
		var need = (count + PAGE_SPLATS - 1) / PAGE_SPLATS
		if node["pages"].size() < need:
			missing.append("node %d: %d/%d pages allocated" % [int(node["node"]), node["pages"].size(), need])
		for slot in range(mini(need, node["pages"].size())):
			var page = int(node["pages"][slot])
			if not pages.has(page):
				missing.append("node %d page %d" % [int(node["node"]), page])
	return missing


func _missing_summary(missing: Array) -> String:
	return "%d missing (%s%s)" % [missing.size(), ", ".join(PackedStringArray(missing.slice(0, 8))), ", ..." if missing.size() > 8 else ""]


# The drawn ids are published as null when the cut telemetry is not valid (seen
# under the O2 forcing control); a null list is treated as no drawn nodes.
func _drawn_ids(obs: Dictionary) -> Array:
	var ids = obs.get("stats", {}).get("hlod_drawn_node_ids")
	# Published as a packed array or an Array; anything else (null) is none.
	if ids is Array or ids is PackedInt32Array or ids is PackedInt64Array:
		return Array(ids)
	return []


func _drawn_nodes(obs: Dictionary) -> Array:
	var nodes: Array = []
	for id in _drawn_ids(obs):
		if int(id) < obs.get("nodes", []).size():
			nodes.append(obs["nodes"][int(id)])
	return nodes


# R1's expected LOD levels, from the tree and the asset alone (never the cut): the
# levels of every node with a payload whose cell box meets the named region volume
# recorded in the tree's own manifest. A box overlap can over-claim a level, which
# leaves R1 NOT_RUN, never PASS.
func _region_levels(obs: Dictionary, name: String) -> Dictionary:
	var manifest = JSON.parse_string(FileAccess.get_file_as_string(str(_plan["world"]).get_basename() + ".manifest.json"))
	if not manifest is Dictionary:
		return {"error": "tree manifest unreadable"}
	var volume = null
	for v in manifest.get("build_params", {}).get("volumes", []):
		if v.get("kind") == "region" and v.get("name") == name:
			volume = v
	if volume == null:
		return {"error": "no region volume named " + name}
	var t: Array = volume["transform"]
	if t.slice(0, 9) != [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]:
		return {"error": "region volume is not axis-aligned"}
	var half = Vector3(volume["size"][0], volume["size"][1], volume["size"][2]) * 0.5
	var low = Vector3(t[9], t[10], t[11]) - half
	var high = Vector3(t[9], t[10], t[11]) + half
	var levels: Array = []
	var nodes: Array = obs.get("nodes", [])
	for node in nodes:
		var level = int(node["level"])
		if int(node["splats"]) <= 0 or level in levels:
			continue
		var center = Vector3(node["center"][0], node["center"][1], node["center"][2])
		var reach = Vector3.ONE * float(node["edge"]) * 0.5
		var a = center - reach
		var b = center + reach
		if a.x <= high.x and b.x >= low.x and a.y <= high.y and b.y >= low.y and a.z <= high.z and b.z >= low.z:
			levels.append(level)
	levels.sort()
	if nodes.is_empty() or levels.is_empty():
		return {"error": "no tree node meets region " + name, "nodes": nodes.size()}
	return {"levels": levels, "nodes": nodes.size(), "region": name, "low": [low.x, low.y, low.z], "high": [high.x, high.y, high.z]}



# W1/W3 probes: leaf splats with a non-zero wind weight inside the wind fade
# (gs_wind_distance_level_fade: full to 64 m, zero at 128 m and at height >= 4).
# Frozen wind time leaves such splats statically displaced (ADR §7 W1 step 1).
func _wind_keys(obs: Dictionary, pages: Dictionary, count: int = 16) -> Array:
	var keys: Array = []
	for id in _drawn_ids(obs):
		var node = obs["nodes"][int(id)]
		if int(node["level"]) != 0:
			continue
		for local in range(mini(int(node["splats"]), 64)):
			var splat = _raw_splat(pages, node, local)
			if splat.is_empty():
				continue
			var wind = (int(splat["semantic"]) >> 16) & 255
			if wind > 0 and splat["position"].distance_to(_camera.global_position) < 64.0:
				keys.append({"node": int(id), "local": local, "row": 0})
				break
		if keys.size() >= count:
			break
	return keys


func _raw_splat(pages: Dictionary, node: Dictionary, local_index: int) -> Dictionary:
	if local_index / PAGE_SPLATS >= node["pages"].size():
		return {}
	var page = int(node["pages"][local_index / PAGE_SPLATS])
	if not pages.has(page):
		return {}
	var bytes: PackedByteArray = pages[page]
	var at = (local_index % PAGE_SPLATS) * RAW_STRIDE
	if at < 0 or at + RAW_STRIDE > bytes.size():
		return {}
	return {"position": _vec(bytes, at) + Vector3(node["center"][0], node["center"][1], node["center"][2]),
		"opacity": bytes.decode_float(at + 12), "scale": _vec(bytes, at + 16),
		"semantic": bytes.decode_u32(at + 28), "encoding": bytes.decode_u32(at + 172)}


func _cell(point: Vector3, center: Vector3, edge: float, eps: float) -> Vector3i:
	return Vector3i(((point - center + Vector3.ONE * edge * 0.5) / eps).floor())


# W2 probe: a height-1 parent whose leaf children are drawn at pose A, inside the
# wind distance fade (gs_wind_distance_level_fade: zero at 128 m), nearest first.
# Run 45231549548 picked a parent ~270 m away: every displacement was 0, so the
# row "passed" 0 px against 0 px. A probe whose children move less than
# W2_MIN_CHILD_DISPLACEMENT_PX on screen is no evidence; the row is then NOT_RUN.
const W2_MIN_CHILD_DISPLACEMENT_PX = 1.0
const WIND_FADE_ZERO_M = 128.0


func _w2_parents(fine: Dictionary) -> Array:
	var drawn = _drawn_ids(fine)
	var parents: Array = []
	for parent in fine.get("nodes", []):
		# Height 1: the children are leaves, so the W2 control (interiors held
		# still) breaks the parent and not the children it is compared with.
		if not parent["resident"] or int(parent["child_count"]) == 0 or float(parent["eps"]) <= 0 or int(parent["level"]) != 1:
			continue
		var center = Vector3(parent["center"][0], parent["center"][1], parent["center"][2])
		var distance = center.distance_to(_camera.global_position)
		if distance >= WIND_FADE_ZERO_M + float(parent["edge"]):
			continue
		var all_drawn = true
		for id in range(int(parent["first_child"]), int(parent["first_child"]) + int(parent["child_count"])):
			all_drawn = all_drawn and id in drawn
		if all_drawn:
			parents.append({"distance": distance, "node": parent})
	parents.sort_custom(func(a, b): return a["distance"] < b["distance"])
	return parents.map(func(p): return p["node"])


func _wind_lod() -> void:
	_pose_a()
	_world.set_hlod_tau_px(0.1)
	await _settle()
	var fine = await _native("_test_gs_game_1_observe")
	if fine.get("raw_payload") != true:
		return
	var search_deadline = Time.get_ticks_msec() + DEADLINE_MS
	var vacuous: Array = []
	for parent in _w2_parents(fine):
		if Time.get_ticks_msec() > search_deadline:
			break
		var children: Array = []
		for id in range(int(parent["first_child"]), int(parent["first_child"]) + int(parent["child_count"])):
			children.append(fine["nodes"][id])
		# Parent: its first 64 splats; children: every splat the merge may have taken.
		var bytes = await _atlas_pages(fine, [parent])
		var child_bytes = await _atlas_pages(fine, children, 0)
		# The merge oracle needs the whole child population: no truncated reads.
		var missing = _atlas_missing(fine, [parent], bytes) + _atlas_missing(fine, children, child_bytes, 0)
		if not missing.is_empty():
			_raw["rows"]["W2"] = {"not_run_reason": "W2 atlas pages not read completely: %s" % _missing_summary(missing), "parent": int(parent["node"])}
			_world.set_hlod_tau_px(0)
			return
		bytes.merge(child_bytes)
		var center = Vector3(parent["center"][0], parent["center"][1], parent["center"][2])
		for local in range(mini(int(parent["splats"]), 64)):
			if Time.get_ticks_msec() > search_deadline:
				break
			var p = _raw_splat(bytes, parent, local)
			# No wind weight or outside the distance fade: no displacement to compare.
			if p.is_empty() or (int(p["semantic"]) >> 16) & 255 == 0 or p["position"].distance_to(_camera.global_position) >= WIND_FADE_ZERO_M:
				continue
			var cell = _cell(p["position"], center, parent["edge"], parent["eps"])
			var keys: Array = []
			var weights: Array = []
			for child in children:
				for ci in range(int(child["splats"])):
					var c = _raw_splat(bytes, child, ci)
					if c.is_empty() or (int(c["semantic"]) & 65535) != (int(p["semantic"]) & 65535) or c["encoding"] != p["encoding"]:
						continue
					var sigmas: Array = [c["scale"].x, c["scale"].y, c["scale"].z]
					sigmas.sort()
					# Exact merge predicate and weights, gaussian_splat_hlod_merge.cpp.
					if 2 * sigmas[2] >= float(parent["eps"]) or _cell(c["position"], center, parent["edge"], parent["eps"]) != cell:
						continue
					keys.append({"node": int(child["node"]), "local": ci, "row": 0})
					weights.append(clampf(c["opacity"], 0, 1) * sigmas[2] * sigmas[1] + 1e-20)
			if keys.is_empty() or keys.size() > 64:
				continue
			var child_capture = await _capture(keys)
			var sum = Vector2.ZERO
			var total = 0.0
			for k in range(child_capture.get("samples", []).size()):
				var sample = child_capture["samples"][k]
				if sample["colour_valid"]:
					sum += (_camera.unproject_position(sample["colour"]) - _camera.unproject_position(sample["rest"])) * weights[k]
					total += weights[k]
			if total == 0 or child_capture.get("samples", []).size() != keys.size() or child_capture["samples"].any(func(v): return not v["colour_valid"]):
				continue
			var children_px = (sum / total).length()
			if children_px < W2_MIN_CHILD_DISPLACEMENT_PX:
				if vacuous.size() < 16:
					vacuous.append({"parent": int(parent["node"]), "local": local, "children_displacement_px": children_px})
				continue
			var selected = false
			var tau = 0.2
			var deadline = Time.get_ticks_msec() + DEADLINE_MS
			while tau <= 1024 and Time.get_ticks_msec() <= deadline:
				_world.set_hlod_tau_px(tau)
				await _settle()
				var obs = await _native("_test_gs_game_1_observe")
				if int(parent["node"]) in obs.get("stats", {}).get("hlod_drawn_node_ids", []):
					selected = true
					break
				tau *= 2
			if not selected:
				_world.set_hlod_tau_px(0.1)
				await _settle()
				continue
			var parent_capture = await _capture([{"node": int(parent["node"]), "local": local, "row": 0}])
			for sample in parent_capture.get("samples", []):
				if sample["colour_valid"]:
					var delta = _camera.unproject_position(sample["colour"]) - _camera.unproject_position(sample["rest"])
					var distance = p["position"].distance_to(_camera.global_position)
					_raw["rows"]["W2"] = {"errors_px": [delta.distance_to(sum / total)], "samples": keys.size(),
						"weights": weights, "parent": parent_capture, "children": child_capture,
						"merge_eps": parent["eps"], "merge_cell": cell, "level": parent["level"],
						"parent_node": int(parent["node"]), "parent_local": local, "probe_distance_m": distance,
						# Host mirror of the distance term of gs_wind_distance_level_fade.
						"distance_fade": clampf((WIND_FADE_ZERO_M - distance) / 64.0, 0, 1),
						"wind_weight": (int(p["semantic"]) >> 16) & 255,
						"children_displacement_px": children_px, "parent_displacement_px": delta.length(),
						"min_children_displacement_px": W2_MIN_CHILD_DISPLACEMENT_PX, "tau_px": tau,
						"rejected_vacuous": vacuous}
					_world.set_hlod_tau_px(0)
					return
	_world.set_hlod_tau_px(0)
	_raw["rows"]["W2"] = {"not_run_reason": "W2 no non-vacuous probe: no height-1 parent inside the wind distance fade whose leaf children move >= %s px (ADR §7 W2)" % W2_MIN_CHILD_DISPLACEMENT_PX,
		"rejected_vacuous": vacuous}


func _deformation_rows() -> void:
	_pose_a()
	await _settle()
	var obs = await _native("_test_gs_game_1_observe")
	if _drawn_ids(obs).is_empty():
		# Seen under the O2 forcing control: the cut telemetry publishes no drawn
		# ids, settle never completes and no probe can be selected. Name it.
		var reason = "no drawn node ids published at pose A (cut telemetry not valid); probes cannot be selected"
		for row in ["W3", "O2", "W4"]:
			if not _raw["rows"].has(row):
				_raw["rows"][row] = {"not_run_reason": reason}
		return
	var probe_pages = await _atlas_pages(obs, _drawn_nodes(obs))
	# W3's world probes come from these pages; O2 uses instance probes only.
	var w3_missing = _atlas_missing(obs, _drawn_nodes(obs), probe_pages)
	var keys = _wind_keys(obs, probe_pages)
	for key in obs.get("instance_probes", []):
		if keys.size() < 64:
			keys.append(key)
	if keys.is_empty():
		if not w3_missing.is_empty():
			_raw["rows"]["W3"] = {"not_run_reason": "W3 atlas pages not read completely: %s" % _missing_summary(w3_missing)}
		return
	var moving = false
	var capture = await _capture(keys, 0, -1, func():
		for i in range(32):
			_objects[i].transform = Content.object_transform(i, 1.0))
	for i in range(32):
		moving = moving or Content.object_transform(i, 0) != _objects[i].transform
	var shadow_errors: Array = []
	var offsets: Array = []
	var object_shadow: Array = []
	for sample in capture.get("samples", []):
		if not sample["colour_valid"]:
			continue
		if sample["shadow_valid"] and int(sample["colour_frame"]) > 0 and sample["colour_frame"] == sample["shadow_frame"]:
			var matrix: Array = sample["light_from_frame"]
			var anchor: Vector3 = sample["shadow_anchor"]
			var error = _project(matrix, sample["colour"] - anchor, sample["viewport"]).distance_to(_project(matrix, sample["shadow"] - anchor, sample["viewport"]))
			if sample["key"]["row"] == 0 and sample["rest"] != sample["colour"]:
				shadow_errors.append(error)
			elif int(sample["key"]["row"]) > 0 and sample["colour_frame"] == capture.get("armed_frame"):
				object_shadow.append(error)
		if int(sample["key"]["row"]) > 0 and sample["colour_frame"] == capture.get("armed_frame"):
			var row = int(sample["key"]["row"])
			var row_keys: Array = obs.get("row_keys", [])
			if row < row_keys.size():
				for i in range(32):
					if _objects[i].get_instance_id() == int(row_keys[row]):
						var expected = _objects[i].global_transform * sample["local_rest"]
						offsets.append(_camera.unproject_position(sample["colour"]).distance_to(_camera.unproject_position(expected)))
	if not w3_missing.is_empty():
		_raw["rows"]["W3"] = {"not_run_reason": "W3 atlas pages not read completely: %s" % _missing_summary(w3_missing)}
	elif not shadow_errors.is_empty():
		_raw["rows"]["W3"] = {"errors_shadow_texels": shadow_errors, "samples": shadow_errors.size(), "capture": capture}
	if moving and not offsets.is_empty() and not object_shadow.is_empty():
		_raw["rows"]["O2"] = {"moving_opportunity": moving, "object_offsets_px": offsets, "caster_colour_offsets_texels": object_shadow, "capture": capture}
	# Rock region: inspect actual semantic bytes before selecting zero-weight probes.
	_camera.look_at_from_position(Vector3(0, 4, 180), Vector3(0, 1, 220), Vector3.UP)
	await _settle()
	obs = await _native("_test_gs_game_1_observe")
	var bytes = await _atlas_pages(obs, _drawn_nodes(obs))
	var w4_missing = _atlas_missing(obs, _drawn_nodes(obs), bytes)
	if not w4_missing.is_empty():
		_raw["rows"]["W4"] = {"not_run_reason": "W4 atlas pages not read completely: %s" % _missing_summary(w4_missing)}
		return
	keys = []
	for id in _drawn_ids(obs):
		var node = obs["nodes"][int(id)]
		# One in-frustum probe per drawn node: 64 probes from the first drawn node
		# all fell outside the view and none was colour-valid (local run, 0/64).
		for local in range(mini(int(node["splats"]), 64)):
			var splat = _raw_splat(bytes, node, local)
			if not splat.is_empty() and ((int(splat["semantic"]) >> 16) & 255) == 0 and (int(splat["semantic"]) & 65535) == _regions[2] \
					and _camera.is_position_in_frustum(splat["position"]):
				keys.append({"node": int(id), "local": local, "row": 0})
				break
		if keys.size() == 64:
			break
	if keys.is_empty():
		_raw["rows"]["W4"] = {"not_run_reason": "W4 found no in-frustum zero-weight rock-region probe"}
	else:
		capture = await _capture(keys)
		var movement: Array = []
		for sample in capture.get("samples", []):
			if sample["colour_valid"]:
				movement.append(_camera.unproject_position(sample["colour"]).distance_to(_camera.unproject_position(sample["rest"])))
		if not movement.is_empty():
			_raw["rows"]["W4"] = {"weight_zero_displacement_px": movement, "samples": movement.size(), "capture": capture}
		else:
			_raw["rows"]["W4"] = {"not_run_reason": "W4 probes not colour-valid: 0/%d" % keys.size(), "capture": capture}


func _shadow_pixels(light: Light3D) -> Dictionary:
	_sun.light_energy = 0
	for omni in _omnis:
		omni.light_energy = 0
	_spot.light_energy = 0
	light.light_energy = 4
	_character.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	for node in _objects:
		node.set_cast_shadow(false)
	light.shadow_enabled = true
	var shadowed = await _image()
	var shadow_proof = _last_image_proof.duplicate(true) if shadowed != null else {}
	light.shadow_enabled = false
	var unshadowed = await _image()
	var unshadow_proof = _last_image_proof.duplicate(true) if unshadowed != null else {}
	light.shadow_enabled = true
	return {"pixels": _count_diff(shadowed, unshadowed) if shadow_proof.get("drawn_digest") != null and shadow_proof.get("drawn_digest") == unshadow_proof.get("drawn_digest") else null,
		"shadow_frame": shadow_proof, "unshadow_frame": unshadow_proof, "shadowed_sha256": _image_sha(shadowed),
		"unshadowed_sha256": _image_sha(unshadowed), "hlod": _renderer.get_render_stats().get("hlod_active") == true}


func _lighting_rows() -> void:
	_pose_a()
	await _settle()
	_sun.light_energy = 0
	for omni in _omnis:
		omni.light_energy = 0
	_spot.light_energy = 0
	var dark = await _image()
	var dark_proof = _last_image_proof.duplicate(true) if dark != null else {}
	_sun.light_energy = 1
	var lit = await _image()
	var lit_proof = _last_image_proof.duplicate(true) if lit != null else {}
	var pixels = _count_lit(dark, lit)
	_raw["rows"]["L1"] = {"lit_pixels": pixels, "lighting_verified": pixels > 0 and dark_proof.get("drawn_digest") != null and dark_proof.get("drawn_digest") == lit_proof.get("drawn_digest"),
		"dark_frame": dark_proof, "lit_frame": lit_proof,
		"dark_sha256": _image_sha(dark), "lit_sha256": _image_sha(lit)}
	var omni = await _shadow_pixels(_omnis[0])
	var spot = await _shadow_pixels(_spot)
	_raw["rows"]["L2"] = {"hlod": omni["hlod"] and spot["hlod"],
		"omni_splat_shadow_pixels": omni["pixels"], "spot_splat_shadow_pixels": spot["pixels"], "omni": omni, "spot": spot}
	_sun.light_energy = 1
	for light in _omnis:
		light.light_energy = 1
	_spot.light_energy = 0


func _green_capture(capture: Dictionary, first: bool = true) -> Variant:
	var valid = 0
	var green = true
	for sample in capture.get("samples", []):
		if sample["colour_valid"]:
			valid += 1
			if first and sample["colour_frame"] != capture.get("armed_frame"):
				return null # Missing the authored frame cannot make a control red.
			green = green and sample["tint"].y > 0 and sample["tint"].x == 0 and sample["tint"].z == 0
	return green if valid > 0 else null


func _regions_rows() -> void:
	_pose_a()
	_setting(PIPE + "enable_sh_amortization", false)
	for override in _overrides.values():
		override.opacity = 1
		override.tint_strength = 0
	var target = int(_regions[1])
	var lods: Array = []
	# ADR §7 R1 "at every LOD level": the levels are fixed from the tree before the
	# sweep. A level the sweep cannot reach stays expected and is named below.
	var tree = _region_levels(await _native("_test_gs_game_1_observe"), "mid")
	var expected_lods: Array = tree.get("levels", [])
	var observed_lods: Array = []
	var obs: Dictionary = {}
	for node in _objects:
		node.visible = false
	for tau in [0.05, 0.25, 1.0, 4.0, 16.0, 64.0, 256.0, 1024.0]:
		if not expected_lods.is_empty() and expected_lods.all(func(l): return l in observed_lods):
			break
		_world.set_hlod_tau_px(tau)
		await _settle()
		obs = await _native("_test_gs_game_1_observe")
		var drawn_levels: Array = []
		for id in obs.get("stats", {}).get("hlod_drawn_node_ids", []):
			var level = int(obs["nodes"][int(id)]["level"])
			if level not in drawn_levels and level not in observed_lods:
				drawn_levels.append(level)
		var keys = _world_keys(obs, 1)
		if keys.is_empty():
			continue
		for level in drawn_levels:
			# Test-only selection in all three shader consumers isolates the LOD
			# whose pixels are counted. A whole-image mask is never reused for other LODs.
			await _capture(keys, target, level)
			await _native("_test_gs_game_1_expected_regions", [true])
			_overrides[target].tint_strength = 0
			var neutral = await _image()
			_overrides[target].tint = Color(1, 0, 0)
			_overrides[target].tint_strength = 1
			var expected = await _image()
			await _native("_test_gs_game_1_expected_regions", [false])
			var actual = await _image()
			if neutral == null or expected == null or actual == null:
				continue
			var n = neutral.get_data()
			var e = expected.get_data()
			var aa = actual.get_data()
			if n.size() != e.size() or e.size() != aa.size():
				continue
			var stride = e.size() / (expected.get_width() * expected.get_height())
			var region_pixels = 0
			var tinted = 0
			var foreign = 0
			for pixel in range(expected.get_width() * expected.get_height()):
				var mask = false
				var matches = true
				for c in range(stride):
					mask = mask or n[pixel * stride + c] != e[pixel * stride + c]
					matches = matches and e[pixel * stride + c] == aa[pixel * stride + c]
				if mask:
					region_pixels += 1
					tinted += 1 if matches else 0
				elif not matches:
					foreign += 1
			if region_pixels > 0:
				observed_lods.append(level)
				lods.append({"level": level, "region_pixels": region_pixels, "tinted_pixels": tinted, "foreign_pixels": foreign,
					"reference_sha256": _sha(e), "actual_sha256": _sha(aa), "tau": tau, "isolated_level": level})

	var unreached: Array = []
	for level in expected_lods:
		if level not in observed_lods:
			unreached.append(level)
	_raw["rows"]["R1"] = {"lods": lods, "expected_lods": expected_lods, "expected_source": "tree_region_volume",
		"unreached_lods": unreached, "tree": tree}
	if tree.has("error"):
		_raw["rows"]["R1"]["not_run_reason"] = "R1 tree levels unknown: " + str(tree["error"])
	elif not unreached.is_empty():
		_raw["rows"]["R1"]["not_run_reason"] = "R1 tau sweep never drew target-region pixels at tree levels %s" % str(unreached)
	_world.set_hlod_tau_px(0)
	await _settle()
	# Select actual target-region splats from resident raw pages.
	obs = await _native("_test_gs_game_1_observe")
	var bytes = await _atlas_pages(obs, _drawn_nodes(obs))
	var missing = _atlas_missing(obs, _drawn_nodes(obs), bytes)
	var keys: Array = []
	if not missing.is_empty():
		# R2's and R3's target-region probes come from these pages.
		var reason = "atlas pages not read completely: %s" % _missing_summary(missing)
		for row in ["R2", "R3"]:
			_raw["rows"][row] = {"not_run_reason": row + " " + reason}
		obs = {}
	for id in obs.get("stats", {}).get("hlod_drawn_node_ids", []):
		var node = obs["nodes"][int(id)]
		for local in range(mini(int(node["splats"]), 64)):
			var splat = _raw_splat(bytes, node, local)
			if not splat.is_empty() and (int(splat["semantic"]) & 65535) == target:
				keys.append({"node": int(id), "local": local, "row": 0})
			if keys.size() >= 32:
				break
		if keys.size() >= 32:
			break
	if not keys.is_empty():
		var capture = await _capture(keys, target, -1, func(): _overrides[target].tint = Color(0, 1, 0))
		var visible_count = int(capture.get("target_region_sort_records", 0))
		var first_effect = _green_capture(capture)
		var before = await _native("_test_gs_game_1_observe")
		var churn = await _churn(before)
		_pose_a()
		await _settle()
		var returned = await _capture(keys, target)
		if not churn.has("error"):
			_raw["rows"]["R2"] = {"set_frame": capture.get("armed_frame"),
				"same_frame_effect": first_effect, "survives_far_evict_near": _green_capture(returned, false),
				"first_capture": capture, "return_capture": returned, "churn": churn}
		else:
			_raw["rows"]["R2"] = {"not_run_reason": churn.get("not_run_reason", churn["error"]), "churn": churn}
		for region in _regions:
			_overrides[region].cast_shadow = int(region) == target
		_overrides[target].opacity = 0
		capture = await _capture(keys, target)
		var shadow = await _shadow_pixels(_omnis[0])
		_raw["rows"]["R3"] = {"visible_region_records": visible_count,
			"hidden_region_sort_records": capture.get("target_region_sort_records"),
			"hidden_region_shadow_pixels": shadow["pixels"], "capture": capture, "shadow": shadow}

	for override in _overrides.values():
		override.opacity = 1
		override.cast_shadow = true


func _stall_on_rd(action: String, iterations: int = 0, serial: int = 0) -> Dictionary:
	var rd = RenderingServer.get_rendering_device()
	if action == "clear":
		for rid in [_stall_set, _stall_pipeline, _stall_shader, _stall_buffer]:
			if rid.is_valid():
				rd.free_rid(rid)
		_stall_set = RID()
		_stall_pipeline = RID()
		_stall_shader = RID()
		_stall_buffer = RID()
		return {}
	if action == "create":
		var source = RDShaderSource.new()
		source.source_compute = """#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) buffer Result { uint words[]; } output_data;
layout(push_constant,std430) uniform Params { uint iterations; } params;
void main() {
    uint value=gl_GlobalInvocationID.x+1u;
    for (uint i=0u;i<params.iterations;i++) { value=(value^(value>>13u))*1664525u+1013904223u; }
    output_data.words[gl_GlobalInvocationID.x]=value;
}
"""
		var spirv = rd.shader_compile_spirv_from_source(source)
		if not spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE).is_empty():
			return {"error": spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)}
		_stall_shader = rd.shader_create_from_spirv(spirv)
		_stall_buffer = rd.storage_buffer_create(256)
		_stall_pipeline = rd.compute_pipeline_create(_stall_shader)
		var uniform = RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		uniform.binding = 0
		uniform.add_id(_stall_buffer)
		_stall_set = rd.uniform_set_create([uniform], _stall_shader, 0)
		return {"ready": _stall_set.is_valid() and _stall_pipeline.is_valid()}
	if action == "dispatch":
		# The pipeline's push-constant block is 16 bytes (std430 rounding); a
		# 4-byte push is refused and the dispatch never runs.
		var push = PackedByteArray()
		push.resize(16)
		push.encode_u32(0, iterations)
		rd.capture_timestamp("GS_GAME_1_STALL_BEGIN_%d" % serial)
		var list = rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list, _stall_pipeline)
		rd.compute_list_bind_uniform_set(list, _stall_set, 0)
		rd.compute_list_set_push_constant(list, push, 16)
		rd.compute_list_dispatch(list, 1, 1, 1)
		rd.compute_list_end()
		rd.capture_timestamp("GS_GAME_1_STALL_END_%d" % serial)
		return {"serial": serial}
	var found: Dictionary = {}
	var frame = rd.get_captured_timestamps_frame()
	if frame != _last_timestamp_frame:
		_last_timestamp_frame = frame
		var begins: Dictionary = {}
		var rendered_serials: Array = []
		for i in range(rd.get_captured_timestamps_count()):
			var name = rd.get_captured_timestamp_name(i)
			if name.begins_with("TileBinning_") and name.ends_with("_Begin"):
				var rendered = int(name.trim_prefix("TileBinning_").trim_suffix("_Begin"))
				if rendered not in rendered_serials:
					rendered_serials.append(rendered)
			if name.begins_with("GS_GAME_1_STALL_BEGIN_"):
				begins[int(name.trim_prefix("GS_GAME_1_STALL_BEGIN_"))] = rd.get_captured_timestamp_gpu_time(i)
			if name.begins_with("GS_GAME_1_STALL_END_"):
				var key = int(name.trim_prefix("GS_GAME_1_STALL_END_"))
				if begins.has(key):
					var end = rd.get_captured_timestamp_gpu_time(i)
					found[key] = {"stall_ms": float(end - begins[key]) / 1000000.0,
						"stall_valid": end > begins[key] and begins[key] > 0, "timestamp_frame": frame}
		for key in found:
			if rendered_serials.size() == 1:
				found[key]["frame"] = rendered_serials[0]
	return found


func _calibrate_stall() -> Dictionary:
	# Reaching this function requires the TESTS_ENABLED-only native bridge.
	var created = await _render(func(): return _stall_on_rd("create"))
	var result = {"calibrated": false, "attempts": 0, "bounded_attempts": MAX_CALIBRATION_ATTEMPTS, "samples": []}
	if created.get("ready") != true:
		return result
	var iterations = 1024
	for attempt in range(MAX_CALIBRATION_ATTEMPTS):
		_stall_serial += 1
		var serial = _stall_serial
		await _render(func(): return _stall_on_rd("dispatch", iterations, serial))
		var deadline = Time.get_ticks_msec() + 30000
		var measurement: Dictionary = {}
		while Time.get_ticks_msec() <= deadline:
			var found = await _render(func(): return _stall_on_rd("poll"))
			if found.has(serial):
				measurement = found[serial]
				break
			await RenderingServer.frame_post_draw
		result["attempts"] = attempt + 1
		result["samples"].append({"iterations": iterations, "measurement": measurement})
		if measurement.get("stall_valid") == true and float(measurement.get("stall_ms", 0)) >= 30:
			result["calibrated"] = true
			result["iterations"] = iterations
			return result
		iterations *= 4
	return result


func _sample_frames() -> void:
	var frames: Array = []
	var by_frame: Dictionary = {}
	var stall_frames: Dictionary = {}
	var calibration: Dictionary = {}
	if _plan["control"] == "P1":
		calibration = await _calibrate_stall()
	var spawn_counts: Array = []
	var before_spawn = null
	var upload_first = null
	var upload_last = null
	var upload_first_usec = null
	var upload_last_usec = null
	var ledger_peak = null
	var ledger_verified = 0
	var ledger_limits: Array = []
	# The GS-pass GPU time (gpu_frame_ms) excludes Godot's own passes, shadow maps
	# included. The viewport's whole measured GPU and CPU times are reported beside
	# it as separate fields; P1's state does not read them.
	RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
	# Test-only forcing control (#1552): extra harness work after each boundary,
	# where the old loop-top-to-post_draw window never reached. The lane never sets
	# it; a local run does, to show the delay now worsens P1.
	var forcing_delay_usec = int(float(_plan.get("p1_forcing_out_of_window_delay_ms", 0)) * 1000.0)
	# #1552: a sample is the interval between two consecutive post_draw boundaries,
	# so everything one game frame costs -- the scripted inputs, the engine's
	# process/physics/draw/present, and this harness's own bookkeeping after the
	# boundary -- lands in exactly one sample, and the samples tile the window from
	# this anchor boundary to the last one.
	await RenderingServer.frame_post_draw
	var start = Time.get_ticks_usec()
	var previous_boundary = start
	var previous_drawn = Engine.get_frames_drawn()
	var harness_usec = 0
	for sample in range(int(_plan["samples"])):
		var script_begin = Time.get_ticks_usec()
		var digest = _inputs(sample)
		if _plan["control"] == "P1" and calibration.get("calibrated") == true and sample % 10 == 0:
			_stall_serial += 1
			var serial = _stall_serial
			RenderingServer.call_on_render_thread(func(): _stall_on_rd("dispatch", int(calibration["iterations"]), serial))
			stall_frames[serial] = sample
		var script_usec = Time.get_ticks_usec() - script_begin
		await RenderingServer.frame_post_draw
		var end = Time.get_ticks_usec()
		var drawn = Engine.get_frames_drawn()
		var stats = _renderer.get_render_stats()
		var frame = stats.get("production_metrics", {}).get("gs_game_1_submission_frame")
		var rec = {"sample": sample, "input_digest": digest, "frame": frame,
			"frame_ms": float(end - previous_boundary) / 1000.0, "spawn": sample >= 600,
			"boundary_usec": end, "drawn_frames": drawn - previous_drawn,
			"harness_ms": float(harness_usec) / 1000.0, "script_ms": float(script_usec) / 1000.0,
			"engine_ms": float(end - previous_boundary - harness_usec - script_usec) / 1000.0,
			"gpu_frame_valid": false}
		previous_boundary = end
		previous_drawn = drawn
		# Latest viewport measurement when this sample ended; it can lag the sample
		# by the timestamp readback latency, so it is not frame-matched.
		rec["viewport_gpu_ms"] = RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid())
		rec["viewport_cpu_ms"] = RenderingServer.viewport_get_measured_render_time_cpu(root.get_viewport_rid())
		rec["frame_setup_cpu_ms"] = RenderingServer.get_frame_setup_time_cpu()
		rec["process_ms"] = Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
		rec["physics_ms"] = Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0
		frames.append(rec)
		if frame != null:
			by_frame[int(frame)] = rec
		var timing = stats.get("production_metrics", {})
		var timing_frame = timing.get("gpu_coherent_frame_serial")
		if timing_frame != null and by_frame.has(int(timing_frame)):
			var target: Dictionary = by_frame[int(timing_frame)]
			target["gpu_frame"] = int(timing_frame)
			target["gpu_frame_ms"] = timing.get("gpu_coherent_total_ms")
			target["gpu_frame_valid"] = timing.get("gs_game_1_coherent_valid") == true
		var ledger = stats.get("device_vram_ledger", {})
		if ledger.has("limit_bytes"):
			ledger_limits.append(ledger["limit_bytes"])
			if int(ledger["limit_bytes"]) > 0 and int(ledger["limit_bytes"]) <= int(_plan["ledger_cap_bytes"]):
				ledger_verified += 1
		if ledger.get("device_observed") == true and ledger.has("device_bytes"):
			ledger_peak = maxi(int(ledger_peak) if ledger_peak != null else 0, int(ledger["device_bytes"]))
		if sample == 599:
			before_spawn = stats.get("hlod_stream_drawn_nodes")
		if sample >= 600 and stats.has("hlod_stream_drawn_nodes"):
			spawn_counts.append(stats["hlod_stream_drawn_nodes"])
		if stats.has("hlod_stream_uploaded_bytes_total"):
			if upload_first == null:
				upload_first = stats["hlod_stream_uploaded_bytes_total"]
				upload_first_usec = end
			upload_last = stats["hlod_stream_uploaded_bytes_total"]
			upload_last_usec = end
		if _plan["control"] == "P1":
			var found = await _render(func(): return _stall_on_rd("poll"))
			_stall_results.merge(found, true)
		if forcing_delay_usec > 0:
			OS.delay_usec(forcing_delay_usec)
		# Everything since the boundary is the harness's own; it falls inside the
		# next sample's interval and is reported there as harness_ms.
		harness_usec = Time.get_ticks_usec() - end
	var sampled_end = Time.get_ticks_usec()
	# Drain timing/stall observations by a wall-clock bound; these frames never
	# enter the subject or the P1 control percentile population.
	var drain = Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() <= drain:
		await RenderingServer.frame_post_draw
		var timing = _renderer.get_render_stats().get("production_metrics", {})
		var id = timing.get("gpu_coherent_frame_serial")
		if id != null and by_frame.has(int(id)):
			by_frame[int(id)]["gpu_frame"] = int(id)
			by_frame[int(id)]["gpu_frame_ms"] = timing.get("gpu_coherent_total_ms")
			by_frame[int(id)]["gpu_frame_valid"] = timing.get("gs_game_1_coherent_valid") == true
		if _plan["control"] == "P1":
			_stall_results.merge(await _render(func(): return _stall_on_rd("poll")), true)
	_raw["ledger_cap_verified"] = ledger_verified == frames.size()
	_raw["observed_ledger_limits"] = ledger_limits
	_raw["frames"] = frames
	_raw["rows"]["P1"] = {"frames": frames, "spawn_window_basis": "from scripted t=10 spawn through end of sampled pass; includes delayed admissions", "frame_basis": "post_draw_to_post_draw", "capture_start_usec": start, "capture_end_usec": previous_boundary, "forcing_out_of_window_delay_ms": float(forcing_delay_usec) / 1000.0}
	if _plan["control"] == "P1":
		var stalls: Array = []
		for serial in stall_frames:
			if _stall_results.has(serial):
				var stall: Dictionary = _stall_results[serial].duplicate()
				stalls.append(stall)
		_raw["rows"]["P1"].merge({"calibration": calibration, "stalls": stalls, "stalled_frame_count": stalls.size()})
		await _render(func(): return _stall_on_rd("clear"))
	_raw["rows"]["S1"] = {"before_drawn_nodes": before_spawn, "spawn_drawn_nodes": spawn_counts, "root_nodes": 1, "new_asset": _spawned}
	var memory: Dictionary = {}
	if ledger_peak != null:
		memory["ledger_bytes"] = ledger_peak
	if upload_first != null and upload_last != null and upload_last_usec > upload_first_usec:
		memory["upload_bytes_per_second"] = float(upload_last - upload_first) / (float(upload_last_usec - upload_first_usec) / 1000000.0)
		memory["upload_counter_endpoints"] = [upload_first, upload_last]
		memory["upload_clock_endpoints_usec"] = [upload_first_usec, upload_last_usec]
	_raw["rows"]["M1"] = memory
	_raw["measurement_elapsed_usec"] = sampled_end - start
	_save()


func _run() -> void:
	var args = OS.get_cmdline_user_args()
	if args.size() != 2 or args[0] != "--plan":
		quit(2)
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(args[1]))
	if not parsed is Dictionary:
		quit(2)
		return
	_plan = parsed
	_raw.merge({"job_id": _plan["job_id"], "run": _plan["run"], "adapter": RenderingServer.get_video_adapter_name(),
		"control": _plan["control"], "dynamics": _plan["dynamics"], "tests_enabled": false,
		"approximation": "APPROXIMATION: RTX 3080 with an 8 GB ledger cap; not the mid-range result"})
	_save()
	if DisplayServer.get_name() == "headless" or RenderingServer.get_rendering_device() == null:
		_raw["reason"] = "NOT_RUN: native GPU-backed window required"
		_save()
		quit(3)
		return
	root.size = VIEW
	root.use_taa = false
	root.msaa_3d = Viewport.MSAA_DISABLED
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	var settings = {WIND + "wind_enabled": bool(_plan["dynamics"]), WIND + "wind_strength": 0.4,
		WIND + "wind_frequency": 1.0, WIND + "wind_spatial_frequency": 0.2, WIND + "wind_time_scale": 1.0,
		WIND + "wind_direction_x": 1.0, WIND + "wind_direction_y": 0.0, WIND + "wind_direction_z": 0.0,
		"rendering/gaussian_splatting/compression/per_chunk_quantization": 0,
		"rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb": int(_plan["atlas_mb"])}
	for key in settings:
		_setting(key, settings[key])
	_raw["settings"] = settings
	var refused = _build()
	if refused != "":
		_raw["reason"] = refused
		_save()
		quit(3)
		return
	var deadline = Time.get_ticks_msec() + DEADLINE_MS
	while _renderer == null and Time.get_ticks_msec() <= deadline:
		await RenderingServer.frame_post_draw
		_renderer = _world.get_renderer()
	if _renderer == null or not _renderer.has_method("_test_gs_game_1_control"):
		_raw["reason"] = "NOT_RUN: TESTS_ENABLED GS-GAME-1 bridge absent"
		_save()
		quit(3)
		return
	_renderer.set_perf_capture_force_runtime_statistics(true)
	_raw["tests_enabled"] = true
	await _native("_test_gs_game_1_control", ["none"])
	var warm = await _settle()
	_raw["warmup"] = warm
	if warm.get("ready") != true or _regions.has(-1):
		_raw["reason"] = "NOT_RUN: synthetic semantic T0 did not settle with all three named regions"
		_save()
		quit(3)
		return
	_raw["switch"] = await _native("_test_gs_game_1_control", [str(_plan["control"])])
	await _sample_frames()
	_setting(WIND + "wind_time_scale", 0.0)
	_inputs(0)
	_pose_a()
	var settled = await _settle()
	var stats = _renderer.get_render_stats()
	var misses = stats.get("hlod_cut_residency_missed")
	if misses != null and stats.has("hlod_stream_nonresident_drawn") and stats.has("hlod_stream_drawn_nodes"):
		_raw["rows"]["H1"] = {"settled": settled.get("ready") == true,
			"nonresident_drawn": stats["hlod_stream_nonresident_drawn"], "missing_nodes": misses.size(),
			"expected_nodes": stats["hlod_stream_drawn_nodes"], "stats": stats}
	# Diagnostic readbacks and shader compilation are outside the timing pass.
	var control = str(_plan["control"])
	if control in ["none", "W1", "C1"]:
		await _identity()
	if control in ["none", "W2"]:
		await _wind_lod()
	if control in ["none", "W3", "W4", "O2"]:
		await _deformation_rows()
	if control in ["none", "L1", "L2"]:
		await _lighting_rows()
	if control in ["none", "R1", "R2", "R3"]:
		await _regions_rows()
	await _native("_test_gs_game_1_clear")
	await _native("_test_gs_game_1_control", ["none"])
	_raw["completed"] = true
	_save()
	quit(0)
