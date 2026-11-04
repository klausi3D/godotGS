extends SceneTree

# GS-COMBINED-1 harness (docs/architecture/adr-one-route-many-producers.md section 9).
#
# Builds the combined game scene from gs_combined_1_content.gd, renders 600 frames at a
# fixed 60 Hz scene step and writes one JSON record. The record holds what the frame
# *showed* (pixel probes, tracer centroids, masks, ROI luminance, image digests) and what
# the renderer *said* (a copy of the stats keys the spec names, copied only when present:
# a key the renderer does not publish stays absent, so the verdict fails it).
#
# This script decides nothing. tests/runtime/gs_combined_1_verdict.py applies every
# threshold, the fail-closed rule and the expectations file. The scenario's completion
# marker therefore means "the record is complete", never "the gate passed"; the runtime
# harness (run_runtime_validation.py) turns the verdict into the scenario result.
#
# Variants: test_gs_combined_1_variant_a.gd / _b.gd override _variant().

const GsRuntimeReport := preload("gs_runtime_report.gd")
const Content := preload("gs_combined_1_content.gd")

const SCENARIOS := {"A": "GS-COMBINED-1 Variant A", "B": "GS-COMBINED-1 Variant B"}
const FRAMES := 600
const FROZEN_START := 240
const FROZEN_FRAMES := 120
const STEP := 1.0 / 60.0
const VIEW_SIZE := Vector2i(1280, 720)
const WARMUP_TIMEOUT_MSEC := 240_000
const RUN_TIMEOUT_MSEC := 900_000
const SCREENSHOT_FRAMES := [0, 100, 199, 240, 300, 399, 450, 599]

# Spec keys (ADR 9.3) first, then the PR1b / renderer counters the verdict also reads.
# Copied only if present; never defaulted.
const STATS_KEYS := [
	"frame_count", "world_drawn_splats", "object_drawn_splats", "sort_input_sources",
	"splat_composite_passes_frame", "route_skip_reason", "dropped_sources", "lights_dropped",
	"shadow_casters_rastered", "shadow_no_caster_rastered", "ledger_bytes", "device_allocated_bytes",
	"ledger_budget_bytes", "silent_truncation_frames",
	"hlod_reason", "hlod_active", "hlod_world_draw_records", "hlod_instance_draw_records",
	"hlod_instance_admitted_objects", "hlod_instance_refused_objects", "hlod_instance_visible_objects",
	"hlod_selected_splats", "hlod_instance_reserve_splats", "hlod_last_refusal_reason",
	"hlod_stream_paged", "hlod_stream_resident_nodes", "hlod_stream_drawn_nodes",
	"hlod_gpu_byte_accounting", "hlod_route_owned_bytes", "hlod_gpu_byte_upper_bound",
	"sort_dispatches_frame", "sort_dispatch_frame", "sorted_splats", "stage_sort_sorted_count",
	"cull_route_uid", "sort_route_uid", "route_uid", "selected_route_uid", "world_submission",
	"cull_visible_static_chunks", "cull_static_chunk_total", "payload_mode", "payload_streamable",
	"vram_cap_hit_frames", "vram_chunk_cap_hit", "gpu_memory_usage_mb",
	"stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status",
]
# P family: frame time plus its serial and lag. Resolved top-level first, then in the
# nested dictionaries the renderer also publishes; the source is written to the record.
const TIMING_KEYS := {
	"gpu_frame_ms": ["gpu_frame_ms", "frame_gpu_ms", "gpu_frame_time_ms"],
	"gpu_frame_valid": ["gpu_frame_valid", "frame_gpu_valid", "gpu_frame_time_valid"],
	"gpu_timing_frame_serial": ["gpu_timing_frame_serial", "timing_frame_serial"],
	"gpu_timing_frames_behind": ["gpu_timing_frames_behind", "timing_frames_behind"],
}
const NESTED_STATS := ["production_metrics", "gpu_performance"]
# Methods a future pick API might be bound as. None is bound today (ADR GR-05, F4).
const PICK_METHODS := ["pick", "pick_splat", "pick_instance", "raycast_splats"]

var _report: GsRuntimeReport
var _variant_id := ""
var _scene: Node3D
var _camera: Camera3D
var _world_node: GaussianSplatWorld3D
var _objects: Array = []
var _assets: Array = []
var _skeleton: Skeleton3D
var _character_root: Node3D
var _sun: DirectionalLight3D
var _omnis: Array = []
var _s3_mesh: MeshInstance3D
var _cursor_px := Vector2.ZERO
var _record: Dictionary = {}
var _out_dir := ""


func _variant() -> String:
	return ""


func _init() -> void:
	call_deferred("_run")


func _fail(reason: String) -> void:
	push_error("%s %s" % [GsRuntimeReport.FAIL_MARKER, reason])
	print("%s %s" % [GsRuntimeReport.METRICS_MARKER, JSON.stringify({"gs_combined_1_status": "failed", "reason": reason, "variant": _variant_id})])
	quit(1)


# ------------------------------------------------------------------------------------------
# Scene


func _pin_settings() -> Dictionary:
	# The project file (tests/runtime/gs_combined_1_project/project.godot) pins the rest;
	# these are the per-variant route settings. Variant B pins the HLOD opt-in because the
	# automatic route (ADR decision 5, PR-auto) has not landed; the record says so.
	var pinned := {
		"rendering/gaussian_splatting/instance_pipeline/enabled": true,
		"rendering/gaussian_splatting/sorting/force_cpu_sort": false,
		"rendering/gaussian_splatting/compression/per_chunk_quantization": 0,
	}
	if _variant_id == "B":
		pinned["rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled"] = true
		pinned["rendering/gaussian_splatting/streaming/route_policy"] = 0
	else:
		pinned["rendering/gaussian_splatting/lod/hlod_runtime_cut_enabled"] = false
		pinned["rendering/gaussian_splatting/streaming/route_policy"] = 1
	for key in pinned:
		ProjectSettings.set_setting(key, pinned[key])
	ProjectSettings.emit_signal("settings_changed")
	return pinned


func _build_world_resource(world_info: Dictionary) -> Dictionary:
	var world := GaussianSplatWorld.new()
	world.gaussian_data = world_info["data"]
	var stamp := "%d_%d" % [OS.get_process_id(), Time.get_ticks_usec()]
	var info := {}
	if _variant_id == "B":
		if world.bake_hlod() != OK or not world.has_hlod_tree():
			return {"error": "bake_hlod failed"}
		# Cold cache: a fresh paged file per run.
		var path := "%s/world_%s.gsplatworld" % [_out_dir, stamp]
		if world.save_hlod_paged(path) != OK:
			return {"error": "save_hlod_paged failed"}
		var loaded: GaussianSplatWorld = ResourceLoader.load(path, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
		if loaded == null:
			return {"error": "paged world reload failed"}
		info = {"resource": loaded, "path": path, "streamed": loaded.has_hlod_paged_source(), "kind": "v4_paged_hlod"}
	else:
		var path := "%s/world_%s.gsplatworld" % [_out_dir, stamp]
		if world.save_to_file(path) != OK:
			return {"error": "save_to_file failed"}
		var loaded: GaussianSplatWorld = ResourceLoader.load(path, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
		if loaded == null:
			return {"error": "chunk world reload failed"}
		info = {"resource": loaded, "path": path, "kind": "chunk_streamed",
				"streamed": loaded.is_streamable_payload() and loaded.has_chunk_payload_source()}
	var res: GaussianSplatWorld = info["resource"]
	info["chunk_count"] = res.get_chunk_count()
	info["payload_mode"] = str(res.get_payload_mode())
	info["resident_only_reason"] = str(res.get_resident_only_reason())
	info["sh_degree"] = res.get_sh_degree()
	return info


func _box_arrays(lo: Vector3, hi: Vector3, bone: int) -> Array:
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	var faces := [
		[Vector3(0, 0, 1), [Vector3(lo.x, lo.y, hi.z), Vector3(hi.x, lo.y, hi.z), Vector3(hi.x, hi.y, hi.z), Vector3(lo.x, hi.y, hi.z)]],
		[Vector3(0, 0, -1), [Vector3(hi.x, lo.y, lo.z), Vector3(lo.x, lo.y, lo.z), Vector3(lo.x, hi.y, lo.z), Vector3(hi.x, hi.y, lo.z)]],
		[Vector3(1, 0, 0), [Vector3(hi.x, lo.y, hi.z), Vector3(hi.x, lo.y, lo.z), Vector3(hi.x, hi.y, lo.z), Vector3(hi.x, hi.y, hi.z)]],
		[Vector3(-1, 0, 0), [Vector3(lo.x, lo.y, lo.z), Vector3(lo.x, lo.y, hi.z), Vector3(lo.x, hi.y, hi.z), Vector3(lo.x, hi.y, lo.z)]],
		[Vector3(0, 1, 0), [Vector3(lo.x, hi.y, hi.z), Vector3(hi.x, hi.y, hi.z), Vector3(hi.x, hi.y, lo.z), Vector3(lo.x, hi.y, lo.z)]],
		[Vector3(0, -1, 0), [Vector3(lo.x, lo.y, lo.z), Vector3(hi.x, lo.y, lo.z), Vector3(hi.x, lo.y, hi.z), Vector3(lo.x, lo.y, hi.z)]],
	]
	for face in faces:
		var q: Array = face[1]
		for idx in [0, 1, 2, 0, 2, 3]:
			verts.append(q[idx])
			normals.append(face[0])
			bones.append_array(PackedInt32Array([bone, 0, 0, 0]))
			weights.append_array(PackedFloat32Array([1.0, 0.0, 0.0, 0.0]))
	return [verts, normals, bones, weights]


# Character boxes in skeleton space at rest: [lo, hi, bone].
const CHAR_BOXES := [[Vector3(-0.25, 0.2, -0.15), Vector3(0.25, 1.4, 0.15), 0],
		[Vector3(0.26, 0.6, -0.07), Vector3(0.4, 1.25, 0.07), 1]]
const SHOULDER := Vector3(0.33, 1.25, 0.0)


func _build_character() -> void:
	_character_root = Node3D.new()
	_character_root.name = "Character"
	_scene.add_child(_character_root)
	_skeleton = Skeleton3D.new()
	_skeleton.name = "Skeleton"
	_character_root.add_child(_skeleton)
	_skeleton.add_bone("torso")
	_skeleton.add_bone("arm")
	_skeleton.set_bone_parent(1, 0)
	_skeleton.set_bone_rest(0, Transform3D())
	_skeleton.set_bone_rest(1, Transform3D(Basis(), SHOULDER))
	_skeleton.reset_bone_poses()
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	for box in CHAR_BOXES:
		var a := _box_arrays(box[0], box[1], box[2])
		verts.append_array(a[0])
		normals.append_array(a[1])
		bones.append_array(a[2])
		weights.append_array(a[3])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Content.CHARACTER_COLOR
	mesh.surface_set_material(0, mat)
	var skin := Skin.new()
	skin.add_bind(0, Transform3D())
	skin.add_bind(1, Transform3D(Basis(), SHOULDER).affine_inverse())
	var mi := MeshInstance3D.new()
	mi.name = "Body"
	mi.mesh = mesh
	mi.skin = skin
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_skeleton.add_child(mi)
	mi.skeleton = mi.get_path_to(_skeleton)


func _build_scene(world_res: GaussianSplatWorld) -> void:
	_scene = Node3D.new()
	_scene.name = "GsCombined1"
	get_root().add_child(_scene)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.25, 0.25, 0.25)
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	var we := WorldEnvironment.new()
	we.environment = env
	_scene.add_child(we)
	_camera = Camera3D.new()
	_camera.fov = 75.0
	_camera.near = 0.1
	_camera.far = 1500.0
	_scene.add_child(_camera)
	_camera.make_current()
	_world_node = GaussianSplatWorld3D.new()
	_world_node.name = "World"
	_world_node.set_auto_apply_on_ready(false)
	_world_node.world = world_res
	_scene.add_child(_world_node)
	_world_node.apply_world()
	for kind in range(4):
		_assets.append(Content.build_asset(kind))
	for i in range(32):
		var n := GaussianSplatNode3D.new()
		n.name = "Object%02d" % i
		n.set_splat_asset(_assets[i / 8]["asset"])
		n.transform = Content.object_transform(i, 0.0)
		n.set("cast_shadow", true)
		_scene.add_child(n)
		_objects.append(n)
	_build_character()
	_sun = DirectionalLight3D.new()
	_sun.shadow_enabled = true
	_scene.add_child(_sun)
	_sun.look_at_from_position(Vector3.ZERO, Content.SUN_DIRECTION, Vector3.UP)
	for k in range(4):
		var o := OmniLight3D.new()
		o.shadow_enabled = true
		o.omni_range = Content.OMNI_RANGE
		o.position = Content.OMNI_POSITIONS[k]
		_scene.add_child(o)
		_omnis.append(o)
		var roof := MeshInstance3D.new()
		var roof_mesh := BoxMesh.new()
		roof_mesh.size = Content.OMNI_ROOF_SIZE
		roof.mesh = roof_mesh
		var ground: Vector3 = Content.OMNI_POSITIONS[k] * Vector3(1, 0, 1)
		# Offset towards the sun so the roof's shadow, not the roof, covers the ROI.
		roof.position = ground - Content.SUN_DIRECTION * (Content.OMNI_ROOF_HEIGHT / -Content.SUN_DIRECTION.y)
		roof.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		_scene.add_child(roof)
	var floor_mi := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Content.S2_FLOOR_SIZE
	var floor_mat := StandardMaterial3D.new()
	floor_mat.albedo_color = Color(0.6, 0.6, 0.6)
	plane.material = floor_mat
	floor_mi.mesh = plane
	floor_mi.position = Content.S2_FLOOR_CENTER
	_scene.add_child(floor_mi)
	_s3_mesh = MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(0.8, 0.8, 0.8)
	var box_mat := StandardMaterial3D.new()
	box_mat.albedo_color = Color(0.8, 0.8, 0.2)
	box.material = box_mat
	_s3_mesh.mesh = box
	_s3_mesh.position = Content.S3_MESH_CENTER
	_s3_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	_scene.add_child(_s3_mesh)


func _apply_inputs(frame: int, t: float) -> void:
	_camera.global_transform = Content.camera_transform(t)
	for i in range(_objects.size()):
		_objects[i].transform = Content.object_transform(i, t)
	var pose := Content.character_pose(frame)
	_character_root.transform = Transform3D(Basis(Vector3.UP, Content.CHARACTER_POSE_YAW[pose]),
			Vector3(Content.CHARACTER_X, 0.0, Content.CHARACTER_POSE_Z[pose]))
	_skeleton.set_bone_pose_rotation(1, Quaternion(Vector3.RIGHT, Content.character_arm_angle(t)))
	_sun.light_energy = Content.sun_intensity(t)
	for k in range(4):
		_omnis[k].light_energy = Content.omni_intensity(k, t)


func _input_digest(t: float) -> String:
	var parts := [_camera.global_transform, t, Engine.time_scale, "wind:none", "effectors:none"]
	for n in _objects:
		parts.append(n.global_transform)
	parts.append(_character_root.global_transform)
	for b in range(_skeleton.get_bone_count()):
		parts.append(_skeleton.get_bone_global_pose(b))
	parts.append([_sun.global_transform, _sun.light_energy])
	for o in _omnis:
		parts.append([o.global_transform, o.light_energy])
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(var_to_str(parts).to_utf8_buffer())
	return ctx.finish().hex_encode()


# ------------------------------------------------------------------------------------------
# Geometry helpers (CPU reference)


static func _ray_aabb(origin: Vector3, dir: Vector3, box: AABB) -> float:
	# Entry distance along the ray, or INF.
	var tmin := -INF
	var tmax := INF
	for a in range(3):
		var o := origin[a]
		var d := dir[a]
		var lo := box.position[a]
		var hi := box.end[a]
		if absf(d) < 1e-9:
			if o < lo or o > hi:
				return INF
			continue
		var t1 := (lo - o) / d
		var t2 := (hi - o) / d
		tmin = maxf(tmin, minf(t1, t2))
		tmax = minf(tmax, maxf(t1, t2))
	if tmax < maxf(tmin, 0.0):
		return INF
	return maxf(tmin, 0.0)


static func _ray_obb(origin: Vector3, dir: Vector3, inv: Transform3D, box: AABB) -> float:
	# `inv` is the inverse of a rigid transform, so local and world distances agree.
	return _ray_aabb(inv * origin, inv.basis * dir, box)


static func _ray_sphere(origin: Vector3, dir: Vector3, center: Vector3, radius: float) -> float:
	var oc := origin - center
	var b := oc.dot(dir)
	var c := oc.dot(oc) - radius * radius
	var disc := b * b - c
	if disc < 0.0:
		return INF
	var t := -b - sqrt(disc)
	return t if t > 0.0 else (0.0 if c < 0.0 else INF)


func _fence_hit(origin: Vector3, dir: Vector3) -> Dictionary:
	if absf(dir.z) < 1e-9:
		return {}
	var t := (Content.FENCE_Z - origin.z) / dir.z
	if t <= 0.0:
		return {}
	var p := origin + dir * t
	return {"t": t, "p": p, "on_fence": p.x > Content.FENCE_X.x and p.x < Content.FENCE_X.y and p.y > Content.FENCE_Y.x and p.y < Content.FENCE_Y.y}


func _slot_margin(x: float) -> float:
	# Signed distance to the nearest slot edge: > 0 inside a slot, < 0 on fence material.
	var best := -INF
	for cx in Content.card_centers():
		best = maxf(best, minf(x - cx, cx + Content.SLOT_WIDTH - x))
	return best


func _character_boxes() -> Array:
	var out := []
	var skel_xf := _skeleton.global_transform
	for box in CHAR_BOXES:
		var bone: int = box[2]
		var rest_inv := Transform3D() if bone == 0 else Transform3D(Basis(), SHOULDER).affine_inverse()
		var xf := skel_xf * _skeleton.get_bone_global_pose(bone) * rest_inv
		out.append([xf, AABB(box[0], box[1] - box[0]), xf.affine_inverse()])
	return out


# Per-frame occluder spheres: [center, radius, owner index, screen centre or null, screen radius].
var _spheres: Array = []


func _build_sphere_cache() -> void:
	_spheres = []
	var focal := VIEW_SIZE.y / (2.0 * tan(deg_to_rad(_camera.fov) * 0.5))
	var origin := _camera.global_position
	for j in range(_objects.size()):
		var a: Dictionary = _assets[j / 8]
		var xf: Transform3D = _objects[j].global_transform
		var ring_points := []
		for t in a["tracers"]:
			ring_points.append(xf * t)
		# The ring sphere is only a broad phase: a ray that enters it is occluded only if it
		# passes within 5 cm of one of the 16 tracer splats (run 1 over-excluded A4 probes).
		for sph in [[xf * a["body_center"], a["body_radius"], []],
				[xf * Vector3(0.0, a["tracers"][4].y, 0.0), Content.TRACER_RING_RADIUS + 0.1, ring_points]]:
			var c: Vector3 = sph[0]
			var r: float = sph[1]
			var screen = null if _camera.is_position_behind(c) else _camera.unproject_position(c)
			var depth := maxf((c - origin).length() - r, 0.05)
			_spheres.append([c, r, j, screen, r * focal / depth + 2.0, sph[2]])


func _spheres_near(lo: Vector2, hi: Vector2) -> Array:
	var out := []
	for sph in _spheres:
		var sc = sph[3]
		if sc == null:
			continue
		var r: float = sph[4]
		if sc.x + r >= lo.x and sc.x - r <= hi.x and sc.y + r >= lo.y and sc.y - r <= hi.y:
			out.append(sph)
	return out


func _slab_box(inset: float) -> AABB:
	var lo := Vector3(Content.SLAB_X.x + inset, Content.SLAB_Y.x + inset, Content.SLAB_Z.x)
	var hi := Vector3(Content.SLAB_X.y - inset, Content.SLAB_Y.y - inset, Content.SLAB_Z.y)
	return AABB(lo, hi - lo)


# Anything other than `skip_object` that the camera ray meets before `limit`.
func _occluded(origin: Vector3, dir: Vector3, limit: float, skip_object: int, char_boxes: Array, check_fence := true, check_slab := true, spheres = null) -> bool:
	for sph in (_spheres if spheres == null else spheres):
		if sph[2] == skip_object:
			continue
		if _ray_sphere(origin, dir, sph[0], sph[1]) < limit:
			var points: Array = sph[5]
			if points.is_empty():
				return true
			for q in points:
				# A tracer's visible footprint (3 sigma plus the raster's dilation) is ~10 cm.
				if _ray_sphere(origin, dir, q, 0.1) < limit:
					return true
	for cb in char_boxes:
		if _ray_obb(origin, dir, cb[2], cb[1]) < limit:
			return true
	if check_slab and _ray_aabb(origin, dir, _slab_box(-0.1)) < limit:
		return true
	if _ray_aabb(origin, dir, AABB(Content.S3_MESH_CENTER - Vector3(0.45, 0.45, 0.45), Vector3(0.9, 0.9, 0.9))) < limit:
		return true
	if check_fence:
		var fh := _fence_hit(origin, dir)
		if not fh.is_empty() and fh["t"] < limit and fh["on_fence"] and _slot_margin(fh["p"].x) < 0.05:
			return true
	return false


# ------------------------------------------------------------------------------------------
# Per-frame measurements


static func _px(img: Image, p: Vector2) -> Color:
	var x := clampi(int(floor(p.x)), 0, img.get_width() - 1)
	var y := clampi(int(floor(p.y)), 0, img.get_height() - 1)
	return img.get_pixel(x, y)


static func _rgb(c: Color) -> Array:
	return [c.r, c.g, c.b]


static func _luma(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b


static func _patch_luma(img: Image, p: Vector2, r: int) -> Variant:
	var cx := int(floor(p.x))
	var cy := int(floor(p.y))
	if cx - r < 0 or cy - r < 0 or cx + r >= img.get_width() or cy + r >= img.get_height():
		return null
	var s := 0.0
	for y in range(cy - r, cy + r + 1):
		for x in range(cx - r, cx + r + 1):
			s += _luma(img.get_pixel(x, y))
	return s / float((2 * r + 1) * (2 * r + 1))


func _on_screen(p: Vector3, margin: float) -> Variant:
	if _camera.is_position_behind(p):
		return null
	var s := _camera.unproject_position(p)
	if s.x < margin or s.y < margin or s.x > VIEW_SIZE.x - margin or s.y > VIEW_SIZE.y - margin:
		return null
	return s


func _measure_a4(img: Image, char_boxes: Array) -> Array:
	var probes := []
	var origin := _camera.global_position
	for k in range(8):
		var xf: Transform3D = _objects[k].global_transform
		var cx: float = Content.card_centers()[k]
		var zc := xf.origin.z
		var entry := {"probe": k, "card_z": zc}
		var p_probe := Vector3(cx - Content.PROBE_OFFSET, Content.CARD_Y, zc)
		var p_obj := Vector3(cx + Content.PROBE_OFFSET, Content.CARD_Y, zc)
		var p_world := Vector3(cx - Content.PROBE_OFFSET, Content.CARD_Y + Content.CARD_HALF + 0.5, Content.FENCE_Z)
		var excluded := ""
		if absf(zc - Content.FENCE_Z) < 0.05:
			excluded = "coplanar"
		var screens := []
		for p in [p_probe, p_obj, p_world]:
			var s = _on_screen(p, 4.0)
			if s == null:
				excluded = "off_screen"
				break
			screens.append(s)
		if excluded == "" or excluded == "coplanar":
			# Validate the layers each ray actually meets (CPU geometry).
			var d_probe := (p_probe - origin).normalized()
			var fh := _fence_hit(origin, d_probe)
			if fh.is_empty() or not fh["on_fence"] or _slot_margin(fh["p"].x) > -0.06:
				excluded = excluded if excluded != "" else "probe_ray_misses_fence"
			var d_obj := (p_obj - origin).normalized()
			var fo := _fence_hit(origin, d_obj)
			if fo.is_empty() or _slot_margin(fo["p"].x) < 0.06:
				excluded = excluded if excluded != "" else "object_ref_ray_hits_fence"
			var far := (p_probe - origin).length() + 1.0
			for d in [d_probe, d_obj, (p_world - origin).normalized()]:
				if _occluded(origin, d, far, k, char_boxes, false):
					excluded = excluded if excluded != "" else "occluded_by_other_layer"
		if excluded != "" or screens.size() != 3:
			entry["cpu_front"] = "excluded"
			entry["excluded"] = excluded
		else:
			entry["cpu_front"] = "object" if zc > Content.FENCE_Z else "world"
			entry["pixel"] = _rgb(_px(img, screens[0]))
			entry["object_ref"] = _rgb(_px(img, screens[1]))
			entry["world_ref"] = _rgb(_px(img, screens[2]))
			entry["screen"] = [screens[0].x, screens[0].y]
		probes.append(entry)
	return probes


func _measure_a5(img: Image, char_boxes: Array) -> Dictionary:
	var errors := []
	var excluded := {}
	var failures := []
	var origin := _camera.global_position
	var r := 3
	# Every tracer's screen position, binned, so crowding is judged across objects too:
	# a neighbour ring's tracer inside the window biases the centroid (run 1).
	var bins := {}
	for i in range(_objects.size()):
		var xf_i: Transform3D = _objects[i].global_transform
		for t in _assets[i / 8]["tracers"]:
			var wp: Vector3 = xf_i * t
			if _camera.is_position_behind(wp):
				continue
			var sp := _camera.unproject_position(wp)
			var key := Vector2i(int(floor(sp.x / 16.0)), int(floor(sp.y / 16.0)))
			if not bins.has(key):
				bins[key] = []
			bins[key].append(sp)
	for i in range(_objects.size()):
		var a: Dictionary = _assets[i / 8]
		var xf: Transform3D = _objects[i].global_transform
		var pts := []
		for t in a["tracers"]:
			pts.append(xf * t)
		var screens := []
		for p in pts:
			screens.append(null if _camera.is_position_behind(p) else _camera.unproject_position(p))
		for ti in range(pts.size()):
			var s = screens[ti]
			var why := ""
			if s == null or s.x < 8 or s.y < 8 or s.x > VIEW_SIZE.x - 8 or s.y > VIEW_SIZE.y - 8:
				why = "off_screen"
			else:
				var tracer_pos: Vector3 = pts[ti]
				var dist := (tracer_pos - origin).length()
				# Footprint below one pixel cannot be centroided to 0.5 px.
				var px_per_m: float = VIEW_SIZE.y / (2.0 * dist * tan(deg_to_rad(_camera.fov) * 0.5))
				if Content.TRACER_SCALE * px_per_m < 0.5:
					why = "sub_pixel"
				else:
					var key := Vector2i(int(floor(s.x / 16.0)), int(floor(s.y / 16.0)))
					var others := 0
					for dy in range(-1, 2):
						for dx in range(-1, 2):
							for other in bins.get(key + Vector2i(dx, dy), []):
								if (other - s).length() < 2 * r + 2:
									others += 1
					if others > 1:  # itself is in the bins once
						why = "crowded"
					if why == "" and _occluded(origin, (tracer_pos - origin).normalized(), dist - 0.05, i, char_boxes):
						why = "occluded"
			if why != "":
				excluded[why] = int(excluded.get(why, 0)) + 1
				continue
			# Background-subtracted luminance centroid in a (2r+1)^2 window.
			var cx := int(floor(s.x))
			var cy := int(floor(s.y))
			var border := []
			for y in range(cy - r, cy + r + 1):
				for x in range(cx - r, cx + r + 1):
					if absi(x - cx) == r or absi(y - cy) == r:
						border.append(_luma(img.get_pixel(x, y)))
			border.sort()
			var bg: float = border[border.size() / 2]
			# A white tracer over a bright background (lit ground) has too little contrast
			# for a 0.5 px centroid; run 3 counted those as misses. Fixed rule, not a fit.
			if bg > 0.45:
				excluded["low_contrast"] = int(excluded.get("low_contrast", 0)) + 1
				continue
			var sw := 0.0
			var sx := 0.0
			var sy := 0.0
			for y in range(cy - r, cy + r + 1):
				for x in range(cx - r, cx + r + 1):
					var w := maxf(0.0, _luma(img.get_pixel(x, y)) - bg - 0.05)
					sw += w
					sx += w * (x + 0.5)
					sy += w * (y + 0.5)
			var err := 99.0
			if sw >= 0.3:
				err = Vector2(sx / sw, sy / sw).distance_to(s)
			# sw < 0.3: CPU says visible, frame shows no tracer -- a failure, not an exclusion.
			errors.append(err)
			if err > 0.5 and failures.size() < 12:
				failures.append({"object": i, "tracer": ti, "screen": [s.x, s.y], "error_px": err, "weight": sw, "bg": bg})
	return {"errors_px": errors, "excluded": excluded, "failures": failures}


func _measure_character(img: Image, frame: int, char_boxes: Array) -> Dictionary:
	var pose := Content.character_pose(frame)
	var origin := _camera.global_position
	# Screen bbox of the character boxes.
	var lo := Vector2(INF, INF)
	var hi := Vector2(-INF, -INF)
	for cb in char_boxes:
		var xf: Transform3D = cb[0]
		var box: AABB = cb[1]
		for c in range(8):
			var s := _camera.unproject_position(xf * box.get_endpoint(c))
			lo = lo.min(s)
			hi = hi.max(s)
	var x0 := maxi(1, int(floor(lo.x)) - 1)
	var y0 := maxi(1, int(floor(lo.y)) - 1)
	var x1 := mini(VIEW_SIZE.x - 2, int(ceil(hi.x)) + 1)
	var y1 := mini(VIEW_SIZE.y - 2, int(ceil(hi.y)) + 1)
	var inner := _slab_box(Content.SLAB_INSET)
	var outer := _slab_box(-0.1)
	var near := _spheres_near(Vector2(x0, y0), Vector2(x1, y1))
	# Hit map over the bbox plus a 1 px border: distance to the character, INF for a miss.
	var w := x1 - x0 + 3
	var h := y1 - y0 + 3
	var tmap := PackedFloat32Array()
	tmap.resize(w * h)
	for gy in range(h):
		for gx in range(w):
			var sp := Vector2(x0 - 1 + gx + 0.5, y0 - 1 + gy + 0.5)
			var o := _camera.project_ray_origin(sp)
			var dir := _camera.project_ray_normal(sp)
			var best := INF
			for cb in char_boxes:
				best = minf(best, _ray_obb(o, dir, cb[2], cb[1]))
			tmap[gy * w + gx] = best
	var reference := 0
	var agree := 0
	var excluded := 0
	var debug: Image = null
	if frame in SCREENSHOT_FRAMES:
		# For human review: white = expected visible, dark = expected hidden, blue = excluded;
		# red marks a pixel where the frame disagrees with the reference.
		debug = Image.create(x1 - x0 + 1, y1 - y0 + 1, false, Image.FORMAT_RGB8)
		debug.fill(Color(0.3, 0.3, 0.3))
	for y in range(y0, y1 + 1):
		for x in range(x0, x1 + 1):
			var gx := x - x0 + 1
			var gy := y - y0 + 1
			var t_char: float = tmap[gy * w + gx]
			# 1 px erosion: the pixel and its 4 neighbours must all hit the character.
			if t_char == INF or tmap[gy * w + gx - 1] == INF or tmap[gy * w + gx + 1] == INF \
					or tmap[(gy - 1) * w + gx] == INF or tmap[(gy + 1) * w + gx] == INF:
				continue
			var sp0 := Vector2(x + 0.5, y + 0.5)
			var d0 := _camera.project_ray_normal(sp0)
			var o0 := _camera.project_ray_origin(sp0)
			var t_inner := _ray_aabb(o0, d0, inner)
			var t_outer := _ray_aabb(o0, d0, outer)
			# Rays through the slab's soft border, or that meet the slab and the character
			# within 5 cm of each other, have no reliable reference.
			if (t_outer < INF and t_inner == INF) or (t_inner < INF and absf(t_inner - t_char) < 0.05) \
					or _occluded(o0, d0, t_char - 0.02, -1, [], true, false, near):
				excluded += 1
				if debug != null:
					debug.set_pixel(x - x0, y - y0, Color(0.1, 0.2, 0.9))
				continue
			var expect_visible := t_inner == INF or t_char < t_inner
			var c := img.get_pixel(x, y)
			var shown := c.r > 0.6 and c.b > 0.6 and c.g < 0.35
			reference += 1
			if shown == expect_visible:
				agree += 1
			if debug != null:
				debug.set_pixel(x - x0, y - y0, Color(1, 0, 0) if shown != expect_visible else (Color.WHITE if expect_visible else Color(0.05, 0.05, 0.05)))
	if debug != null:
		debug.save_png("%s/character_reference_%03d.png" % [_out_dir, frame])
	return {"pose": pose, "reference_pixels": reference, "agreeing_pixels": agree, "excluded_pixels": excluded}


func _measure_lights(img: Image) -> Dictionary:
	var out := {}
	var s = _on_screen(Content.SUN_ROI, 6.0)
	out["directional"] = {"intensity": _sun.light_energy, "roi_luma": null if s == null else _patch_luma(img, s, 2)}
	for k in range(4):
		var p: Vector3 = Content.OMNI_POSITIONS[k]
		var ground := Vector3(p.x, 0.0, p.z)
		var so = _on_screen(ground, 6.0)
		out["omni_%d" % k] = {"intensity": _omnis[k].light_energy, "roi_luma": null if so == null else _patch_luma(img, so, 2)}
	return out


func _footprint(caster: Vector3, receiver_y: float) -> Vector3:
	var d := Content.SUN_DIRECTION
	return caster + d * ((receiver_y - caster.y) / d.y)


func _measure_shadows(img: Image) -> Dictionary:
	var setups := {
		# Ring casters: the ROI sits under the tube (+0.4 m in x), not under the hole.
		"mesh_floor_under_splat_object": _footprint(Content.S2_CASTER + Vector3(0.4, 0, 0), Content.S2_FLOOR_CENTER.y),
		"splat_ground_under_mesh": _footprint(Content.S3_MESH_CENTER, 0.0),
		"splat_object_over_splat_world": _footprint(Content.S4_CASTER + Vector3(0.4, 0, 0), 0.0),
	}
	var out := {}
	for name in setups:
		var p: Vector3 = setups[name]
		var s = _on_screen(p, 6.0)
		var c = _on_screen(p + Content.SHADOW_CONTROL_OFFSET, 6.0)
		out[name] = {"shadow_luma": null if s == null else _patch_luma(img, s, 1),
				"control_luma": null if c == null else _patch_luma(img, c, 1)}
	return out


func _frustum_objects() -> Array:
	var planes := _camera.get_frustum()
	var out := []
	for i in range(_objects.size()):
		var a: Dictionary = _assets[i / 8]
		var xf: Transform3D = _objects[i].global_transform
		var box: AABB = xf * a["body_aabb"]
		var inside := true
		for pl in planes:
			var all_out := true
			for c in range(8):
				if not pl.is_point_over(box.get_endpoint(c)):
					all_out = false
					break
			if all_out:
				inside = false
				break
		if inside:
			out.append(i)
	return out


func _pick(char_boxes: Array) -> Dictionary:
	var target := Content.PICK_TARGET_INDEX
	var a: Dictionary = _assets[target / 8]
	var o := _camera.project_ray_origin(_cursor_px)
	var d := _camera.project_ray_normal(_cursor_px)
	var xf: Transform3D = _objects[target].global_transform
	var t := _ray_sphere(o, d, xf * a["body_center"], 0.3)
	var under := t < INF and not _occluded(o, d, t, target, char_boxes)
	var available := false
	var api := ""
	for holder in [_world_node, _objects[target]]:
		for m in PICK_METHODS:
			if holder.has_method(m):
				available = true
				api = "%s.%s" % [holder.get_class(), m]
	var entry := {"api_available": available, "api": api, "target_under_cursor": under,
			"target_key": str(_objects[target].get_instance_id()), "hit_key": null, "partial": false,
			"ray_region_resident": true}
	# When a pick API is bound, this is where it is called; its signature is F4's to define,
	# so the harness does not guess one. api_available stays false until then.
	return entry


func _resolve_timing(stats: Dictionary) -> Dictionary:
	var out := {}
	var sources := {}
	for key in TIMING_KEYS:
		for alias in TIMING_KEYS[key]:
			if stats.has(alias):
				out[key] = stats[alias]
				sources[key] = alias
				break
		if out.has(key):
			continue
		for nest in NESTED_STATS:
			var d = stats.get(nest)
			if typeof(d) == TYPE_DICTIONARY:
				for alias in TIMING_KEYS[key]:
					if d.has(alias):
						out[key] = d[alias]
						sources[key] = "%s.%s" % [nest, alias]
						break
			if out.has(key):
				break
	return {"values": out, "sources": sources}


func _renderer() -> GaussianSplatRenderer:
	var r: GaussianSplatRenderer = _world_node.get_renderer()
	if r == null and not _objects.is_empty() and _objects[0].has_method("get_renderer"):
		r = _objects[0].get_renderer()
	return r


# ------------------------------------------------------------------------------------------
# Run


func _run() -> void:
	_variant_id = _variant()
	if not SCENARIOS.has(_variant_id):
		_fail("unknown variant %s" % _variant_id)
		return
	_report = GsRuntimeReport.new(SCENARIOS[_variant_id])
	var deadline := Time.get_ticks_msec() + RUN_TIMEOUT_MSEC
	if not FileAccess.file_exists("res://project.godot") or Engine.is_editor_hint():
		_fail("A native game project, without editor or project manager, is required")
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null or OS.has_feature("headless"):
		_fail("Native RenderingDevice required")
		return
	_out_dir = ProjectSettings.globalize_path("user://gs_combined_1/%s" % _variant_id)
	DirAccess.make_dir_recursive_absolute(_out_dir)
	var pinned := _pin_settings()
	get_root().size = VIEW_SIZE
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED
	RenderingServer.set_default_clear_color(Color.BLACK)
	var world_info := Content.build_world()
	var world := _build_world_resource(world_info)
	if world.has("error"):
		_fail(world["error"])
		return
	_build_scene(world["resource"])
	_report.ok()
	_apply_inputs(0, 0.0)
	_cursor_px = _camera.unproject_position(Content.PICK_PATH_CENTER)
	# Warm-up: pump frames until the world and the objects both draw, or a wall-clock
	# deadline. Never a skip: the 600 measured frames run either way and the verdict
	# judges them.
	var warm := {"ready": false, "frames": 0, "msec": 0}
	var warm_start := Time.get_ticks_msec()
	var stable := 0
	while Time.get_ticks_msec() - warm_start < WARMUP_TIMEOUT_MSEC and stable < 30:
		await RenderingServer.frame_post_draw
		warm["frames"] += 1
		var r := _renderer()
		if r == null:
			continue
		var st := r.get_render_stats()
		var ready := r.has_rendered_content()
		if _variant_id == "B":
			ready = ready and str(st.get("hlod_reason", "")) == "active" and int(st.get("hlod_world_draw_records", 0)) > 0 and int(st.get("hlod_instance_draw_records", 0)) > 0
		stable = stable + 1 if ready else 0
	warm["ready"] = stable >= 30
	warm["msec"] = Time.get_ticks_msec() - warm_start
	var renderer := _renderer()
	var frames := []
	var stats_keys_frame0 := []
	var timing_sources := {}
	var shots := {}
	for f in range(FRAMES):
		if Time.get_ticks_msec() > deadline:
			_fail("run exceeded its wall-clock bound at frame %d" % f)
			return
		var frozen := f >= FROZEN_START and f < FROZEN_START + FROZEN_FRAMES
		var t := (FROZEN_START if frozen else f) * STEP
		_apply_inputs(f, t)
		await RenderingServer.frame_post_draw
		var rec := {"frame": f, "engine_frame": Engine.get_frames_drawn(), "t": t, "frozen": frozen}
		var stats: Dictionary = renderer.get_render_stats() if renderer != null else {}
		if f == 0:
			stats_keys_frame0 = stats.keys()
		var copied := {}
		for key in STATS_KEYS:
			if stats.has(key):
				copied[key] = stats[key]
		var timing := _resolve_timing(stats)
		for key in timing["values"]:
			copied[key] = timing["values"][key]
		if f == 0:
			timing_sources = timing["sources"]
		rec["stats"] = copied
		var img := get_root().get_texture().get_image()
		if img == null or img.get_size() != VIEW_SIZE:
			_fail("viewport capture missing at frame %d" % f)
			return
		var ctx := HashingContext.new()
		ctx.start(HashingContext.HASH_SHA256)
		ctx.update(img.get_data())
		rec["frame_digest"] = ctx.finish().hex_encode()
		rec["input_digest"] = _input_digest(t)
		var char_boxes := _character_boxes()
		_build_sphere_cache()
		rec["frustum_objects"] = _frustum_objects()
		rec["a4_probes"] = _measure_a4(img, char_boxes)
		rec["a5_tracers"] = _measure_a5(img, char_boxes)
		rec["pick"] = _pick(char_boxes)
		rec["character"] = _measure_character(img, f, char_boxes)
		rec["lights"] = _measure_lights(img)
		rec["shadow_rois"] = _measure_shadows(img)
		frames.append(rec)
		_report.ok()
		if f in SCREENSHOT_FRAMES:
			var path := "%s/frame_%03d.png" % [_out_dir, f]
			if img.save_png(path) == OK:
				shots[str(f)] = path
	var adapter := RenderingServer.get_video_adapter_name()
	var gpu_class := "rtx3090" if "3090" in adapter else ("rtx3080" if "3080" in adapter else "unknown")
	_record = {
		"schema": "gs-combined-1/record/v1",
		"variant": _variant_id,
		"scenario": SCENARIOS[_variant_id],
		"frames_requested": FRAMES,
		"segments": {"frozen": [FROZEN_START, FROZEN_START + FROZEN_FRAMES],
				"character": {"front": [0, 200], "inside": [200, 400], "behind": [400, 600]}},
		"taa_enabled": get_root().use_taa,
		"gpu_adapter": adapter,
		"gpu_class": gpu_class,
		# An editor binary reports is_debug_build() at every optimisation level, so the lane
		# that built the binary declares it. Absent means not optimised (P1/P2 then invalid).
		"build_optimized": OS.get_environment("GS_COMBINED_1_BUILD_OPTIMIZED") == "1",
		"build_optimized_source": "env:GS_COMBINED_1_BUILD_OPTIMIZED",
		"engine_version": Engine.get_version_info(),
		"pinned_settings": pinned,
		"route_note": "B pins hlod_runtime_cut_enabled=true: the automatic HLOD route (ADR decision 5, PR-auto) has not landed" if _variant_id == "B" else "A pins route_policy=1 (chunk streaming)",
		"world": {"kind": world["kind"], "streamed": world["streamed"], "chunk_count": world["chunk_count"],
				"payload_mode": world["payload_mode"], "resident_only_reason": world["resident_only_reason"],
				"sh_degree": world["sh_degree"], "splats": world_info["splats"], "counts": world_info["counts"],
				"content_digest": world_info["digest"], "area_m2": world_info["area_m2"]},
		"objects": {"count": _objects.size(), "assets": 4, "tracers_per_object": Content.TRACERS,
				"pick_target": Content.PICK_TARGET_INDEX, "interpenetrating": 8},
		"shader_time_note": "No material in the scene reads TIME and no wind or effector is present; the harness cannot freeze the engine's own shader clock.",
		"warmup": warm,
		"stats_keys_frame0": stats_keys_frame0,
		"timing_sources": timing_sources,
		"screenshots": shots,
		"frames": frames,
	}
	var record_path := "%s/record.json" % _out_dir
	var fa := FileAccess.open(record_path, FileAccess.WRITE)
	if fa == null:
		_fail("cannot write record %s" % record_path)
		return
	fa.store_string(JSON.stringify(_record))
	fa.close()
	print("%s %s" % [GsRuntimeReport.METRICS_MARKER, JSON.stringify({
		"gs_combined_1_status": "record_complete", "variant": _variant_id, "record_path": record_path,
		"frames": frames.size(), "warmup": warm, "world_streamed": world["streamed"], "gpu_adapter": adapter,
		"screenshots": shots})])
	_report.emit_pass({"record_path": record_path})
	_scene.queue_free()
	await RenderingServer.frame_post_draw
	quit(0)
