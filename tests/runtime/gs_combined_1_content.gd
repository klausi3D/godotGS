extends RefCounted

# GS-COMBINED-1 content (docs/architecture/adr-one-route-many-producers.md section 9.2).
#
# Everything is generated from SEED, so no large binary is committed and both variants
# render identical content. World coordinates: y up, the camera looks down -Z.
#
#   - World, >= 1 km^2, stored as SH degree 3 (48 floats per splat): a 1 km x 1 km coarse
#     terrain with random higher SH bands, a dense 48 m x 48 m "set" ground (DC-only, so
#     its ROIs do not change with view direction), a red fence with slots (the A4 probe
#     wall) and a green foliage slab (the C1-C3 occluder).
#   - 4 assets x 8 instances = 32 GaussianSplatNode3D on scripted paths. Each asset carries
#     16 white tracer splats on a vertical ring above its body. Cards (asset 0) cross the
#     fence plane (+-0.4 m): these are the 8 objects that interpenetrate world geometry, and the A4
#     probes. Balls (asset 1) dip through the ground; ball 0 is the pick target.
#   - Shadow setups S2-S4: a mesh floor under ring 0, a mesh box over the splat ground,
#     ring 1 over the splat ground.
#
# Scale is pilot scale, inside the HLOD runtime caps (core/gaussian_splat_hlod_runtime_data.h).

const SEED := 1306
const SH_FLOATS := 48
const K_DC := 0.28209479177387814

const CAMERA_BASE := Vector3(0.0, 1.8, 2.0)
const CAMERA_TARGET := Vector3(0.0, 1.4, -10.0)

const FENCE_Z := -12.0
const FENCE_X := Vector2(-5.6, 5.8)
const FENCE_Y := Vector2(0.4, 3.2)
const FENCE_COLOR := Color(0.9, 0.1, 0.1)
const CARD_HALF := 0.4
const CARD_Y := 1.8
const SLOT_WIDTH := 0.6
# Probe rays aim this far either side of a slot edge (ADR 9.3 A4 geometry).
const PROBE_OFFSET := 0.2
const CARD_Z_AMPLITUDE := 0.4
const CARD_COLOR := Color(0.1, 0.35, 1.0)

const SLAB_X := Vector2(5.0, 9.0)
const SLAB_Y := Vector2(0.0, 2.6)
const SLAB_Z := Vector2(-8.15, -7.85)
const SLAB_COLOR := Color(0.1, 0.6, 0.15)
# The slab's splats have soft edges; the depth reference only trusts this inset box.
const SLAB_INSET := 0.12

const SET_X := Vector2(-24.0, 24.0)
const SET_Z := Vector2(-44.0, 4.0)
const SET_COLOR := Color(0.35, 0.35, 0.35)

const CHARACTER_X := 7.0
const CHARACTER_POSE_Z := {"front": -6.8, "inside": -8.0, "behind": -9.4}
const CHARACTER_POSE_YAW := {"front": 0.0, "inside": 1.05, "behind": 0.0}
const CHARACTER_COLOR := Color(1.0, 0.0, 1.0)

const S2_FLOOR_CENTER := Vector3(-8.0, 0.03, -6.0)
const S2_FLOOR_SIZE := Vector2(4.0, 4.0)
const S2_CASTER := Vector3(-8.0, 1.2, -6.0)
const S3_MESH_CENTER := Vector3(-12.0, 1.2, -16.0)
const S4_CASTER := Vector3(-3.5, 1.0, -5.0)
const SHADOW_CONTROL_OFFSET := Vector3(1.6, 0.0, 0.0)

# Directional light travel direction (unit), from above and in front of the scene.
const SUN_DIRECTION := Vector3(0.0, -0.94, -0.34)
# Each omni ROI (the ground point below its light) has a clear line of sight from the
# camera: outside the fence and the foliage slab, away from moving objects, the S2-S4
# setups and each other (run 1 had them behind the fence).
const OMNI_POSITIONS := [Vector3(17.0, 1.0, -15.0), Vector3(-13.0, 1.0, -9.0),
		Vector3(12.5, 1.0, -11.0), Vector3(-18.0, 1.0, -13.0)]
const OMNI_RANGE := 3.0
# A mesh roof over each omni ROI keeps it in the directional light's shadow, so the only
# light that varies there is its own omni (run 2: the sun ramp confounded L2's Pearson r).
const OMNI_ROOF_HEIGHT := 1.6
const OMNI_ROOF_SIZE := Vector3(3.0, 0.05, 3.0)
const SUN_ROI := Vector3(-1.5, 0.0, -3.0)

# Ball 0 (the pick target) sweeps through the fixed cursor point, below the card probes.
const PICK_PATH_CENTER := Vector3(0.0, 0.9, -10.0)
const PICK_TARGET_INDEX := 8

const ASSET_NAMES := ["card", "ball", "ring", "pillar"]
const ASSET_COLORS := [CARD_COLOR, Color(0.2, 0.9, 0.3), Color(1.0, 0.75, 0.1), Color(0.5, 0.2, 0.9)]
const TRACER_COLOR := Color(1.0, 1.0, 1.0)
const TRACERS := 16
const TRACER_RING_RADIUS := 0.55
const TRACER_SCALE := 0.022


static func dc(c: Color) -> PackedFloat32Array:
	return PackedFloat32Array([(c.r - 0.5) / K_DC, (c.g - 0.5) / K_DC, (c.b - 0.5) / K_DC])


static func card_centers() -> Array:
	# Card k sits on the left edge of slot k: its left half over the fence, its right half
	# over the gap, so the same frame holds the probe, the object-only and the world-only pixel.
	var out := []
	for k in range(8):
		out.append(-4.2 + 1.2 * k)
	return out


static func _in_slot(x: float) -> bool:
	for cx in card_centers():
		if x >= cx and x <= cx + SLOT_WIDTH:
			return true
	return false


# ------------------------------------------------------------------------------------------
# World


class WorldBuilder:
	var pos := PackedVector3Array()
	var sc := PackedVector3Array()
	var rot: Array[Quaternion] = []
	var op := PackedFloat32Array()
	var sh := PackedFloat32Array()

	func add(p: Vector3, s: Vector3, c: Color, o: float, rest: PackedFloat32Array) -> void:
		# V4 paged save (save_hlod_paged) requires every leaf centre to round-trip exactly
		# through float(cell centre) + float(position - centre); arbitrary decimals fail
		# with hlod_center_reconstruction_not_exact. A 1/256 m dyadic grid is exact in
		# float across this 1 km extent (500 * 256 < 2^24).
		pos.append((p * 256.0).round() / 256.0)
		sc.append(s)
		rot.append(Quaternion())
		op.append(o)
		var k := 0.28209479177387814
		sh.append_array(PackedFloat32Array([(c.r - 0.5) / k, (c.g - 0.5) / k, (c.b - 0.5) / k]))
		sh.append_array(rest)


static func _zero_rest() -> PackedFloat32Array:
	var z := PackedFloat32Array()
	z.resize(SH_FLOATS - 3)
	z.fill(0.0)
	return z


static func build_world() -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED
	var b := WorldBuilder.new()
	var zero := _zero_rest()
	var counts := {}
	# 1. 1 km x 1 km terrain, 5 m spacing, carved out where the set ground is.
	var n := 0
	for iz in range(200):
		for ix in range(200):
			var x := -500.0 + 5.0 * (ix + 0.5)
			var z := -500.0 + 5.0 * (iz + 0.5)
			if x > SET_X.x - 1.0 and x < SET_X.y + 1.0 and z > SET_Z.x - 1.0 and z < SET_Z.y + 1.0:
				continue
			var rest := PackedFloat32Array()
			for j in range(SH_FLOATS - 3):
				rest.append(rng.randf_range(-0.05, 0.05))
			var shade := rng.randf_range(-0.04, 0.04)
			b.add(Vector3(x, -0.02, z), Vector3(3.2, 0.05, 3.2), Color(0.30 + shade, 0.33 + shade, 0.22 + shade), 0.95, rest)
			n += 1
	counts["terrain"] = n
	# 2. Dense set ground, DC only.
	n = 0
	var step := 0.3
	var nx := int((SET_X.y - SET_X.x) / step)
	var nz := int((SET_Z.y - SET_Z.x) / step)
	for iz in range(nz):
		for ix in range(nx):
			b.add(Vector3(SET_X.x + step * (ix + 0.5), 0.0, SET_Z.x + step * (iz + 0.5)), Vector3(0.2, 0.02, 0.2), SET_COLOR, 0.98, zero)
			n += 1
	counts["set_ground"] = n
	# 3. Fence with slots (A4 world layer).
	n = 0
	step = 0.05
	var fx := int((FENCE_X.y - FENCE_X.x) / step)
	var fy := int((FENCE_Y.y - FENCE_Y.x) / step)
	# Two layers 1 cm apart and splats wider than their spacing: A4's CPU reference assumes
	# each layer is opaque (run 1 showed cards through a single sparse layer).
	for layer in [-0.005, 0.005]:
		for iy in range(fy):
			for ix in range(fx):
				var x := FENCE_X.x + step * (ix + 0.5)
				if _in_slot(x):
					continue
				b.add(Vector3(x, FENCE_Y.x + step * (iy + 0.5), FENCE_Z + layer), Vector3(0.07, 0.07, 0.01), FENCE_COLOR, 0.97, zero)
				n += 1
	counts["fence"] = n
	# 4. Foliage slab (C1-C3 occluder), three layers deep.
	n = 0
	var sx := int((SLAB_X.y - SLAB_X.x) / step)
	var sy := int((SLAB_Y.y - SLAB_Y.x) / step)
	for layer in range(3):
		var z := lerpf(SLAB_Z.x, SLAB_Z.y, (layer + 0.5) / 3.0)
		for iy in range(sy):
			for ix in range(sx):
				b.add(Vector3(SLAB_X.x + step * (ix + 0.5), SLAB_Y.x + step * (iy + 0.5), z), Vector3(0.04, 0.04, 0.03), SLAB_COLOR, 0.98, zero)
				n += 1
	counts["foliage"] = n
	var data := GaussianData.new()
	data.resize(b.pos.size())
	data.set_positions(b.pos)
	data.set_scales(b.sc)
	data.set_rotations(b.rot)
	data.set_opacities(b.op)
	data.set_spherical_harmonics(b.sh)
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(PackedVector3Array(b.pos).to_byte_array())
	ctx.update(b.sc.to_byte_array())
	ctx.update(b.op.to_byte_array())
	ctx.update(b.sh.to_byte_array())
	return {"data": data, "counts": counts, "splats": b.pos.size(), "digest": ctx.finish().hex_encode(),
			"sh_floats_per_splat": SH_FLOATS, "area_m2": 1000.0 * 1000.0}


# ------------------------------------------------------------------------------------------
# Objects


static func _tracer_locals(top: float) -> Array:
	var out := []
	for i in range(TRACERS):
		var a := TAU * float(i) / TRACERS
		out.append(Vector3(TRACER_RING_RADIUS * cos(a), top + 0.6 + TRACER_RING_RADIUS * sin(a), 0.0))
	return out


static func build_asset(kind: int) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = SEED * 31 + kind
	var p := PackedFloat32Array()
	var s := PackedFloat32Array()
	var r := PackedFloat32Array()
	var d := PackedFloat32Array()
	var o := PackedFloat32Array()
	var body := dc(ASSET_COLORS[kind])
	var lo := Vector3(INF, INF, INF)
	var hi := -lo
	var pts: Array = []
	var scl: Array = []
	match kind:
		0:  # card: flat 0.6 m square facing +Z
			# Two layers 1 cm apart: an opaque layer, as A4's reference assumes (run 2 showed
			# up to 4/255 of fence red through a single layer).
			for layer in [-0.005, 0.005]:
				for iy in range(32):
					for ix in range(32):
						pts.append(Vector3(-CARD_HALF + 0.025 * (ix + 0.5), -CARD_HALF + 0.025 * (iy + 0.5), layer))
						scl.append(Vector3(0.035, 0.035, 0.005))
		1:  # ball
			for i in range(1500):
				var v := Vector3(rng.randf_range(-1, 1), rng.randf_range(-1, 1), rng.randf_range(-1, 1))
				if v.length() > 1.0:
					v = v.normalized()
				pts.append(v * 0.35)
				scl.append(Vector3(0.07, 0.07, 0.07))
		2:  # ring (torus)
			for i in range(1200):
				var a := rng.randf() * TAU
				var b2 := rng.randf() * TAU
				pts.append(Vector3((0.4 + 0.1 * cos(b2)) * cos(a), 0.1 * sin(b2), (0.4 + 0.1 * cos(b2)) * sin(a)))
				scl.append(Vector3(0.05, 0.05, 0.05))
		3:  # pillar
			for i in range(1200):
				pts.append(Vector3(rng.randf_range(-0.15, 0.15), rng.randf_range(-0.6, 0.6), rng.randf_range(-0.15, 0.15)))
				scl.append(Vector3(0.06, 0.06, 0.06))
	for i in range(pts.size()):
		var v: Vector3 = pts[i]
		lo = lo.min(v)
		hi = hi.max(v)
		p.append_array(PackedFloat32Array([v.x, v.y, v.z]))
		var sv: Vector3 = scl[i]
		s.append_array(PackedFloat32Array([sv.x, sv.y, sv.z]))
		r.append_array(PackedFloat32Array([0.0, 0.0, 0.0, 1.0]))
		d.append_array(body)
		o.append(6.0)
	var tracers := _tracer_locals(hi.y)
	var white := dc(TRACER_COLOR)
	for t in tracers:
		var tv: Vector3 = t
		p.append_array(PackedFloat32Array([tv.x, tv.y, tv.z]))
		s.append_array(PackedFloat32Array([TRACER_SCALE, TRACER_SCALE, TRACER_SCALE]))
		r.append_array(PackedFloat32Array([0.0, 0.0, 0.0, 1.0]))
		d.append_array(white)
		o.append(6.0)
	var asset := GaussianSplatAsset.new()
	asset.set_splat_count(pts.size() + tracers.size())
	asset.set_positions(p)
	asset.set_scales(s)
	asset.set_rotations(r)
	asset.set_sh_dc_coefficients(d)
	asset.set_opacity_logits(o)
	var margin := 0.1
	return {"asset": asset, "name": ASSET_NAMES[kind], "body_aabb": AABB(lo - Vector3.ONE * margin, hi - lo + Vector3.ONE * 2.0 * margin),
			"body_radius": (hi - lo).length() * 0.5 + margin, "body_center": (lo + hi) * 0.5, "tracers": tracers,
			"splats": pts.size() + tracers.size()}


# Instance i in [0, 32): asset i / 8, slot i % 8. All paths are functions of the scene time t
# (seconds at a fixed 60 Hz step), so a frozen t freezes every object.
static func object_transform(i: int, t: float) -> Transform3D:
	var kind := i / 8
	var k := i % 8
	match kind:
		0:  # cards: cross the fence plane, phase-shifted so few are coplanar at once
			var z := FENCE_Z + CARD_Z_AMPLITUDE * sin(TAU * (t / 2.5 + float(k) / 8.0))
			return Transform3D(Basis(), Vector3(card_centers()[k], CARD_Y, z))
		1:  # balls dip through the set ground; ball 0 crosses the cursor ray
			if k == 0:
				return Transform3D(Basis(), PICK_PATH_CENTER + Vector3(3.0 * sin(TAU * t / 4.0), 0.0, 0.0))
			var c := Vector3(-14.0 + 4.0 * (k - 1), 0.25 + 0.45 * sin(TAU * (t / 3.0 + k * 0.13)), -30.0 + 2.0 * cos(TAU * t / 6.0 + k))
			return Transform3D(Basis(), c)
		2:  # rings: 0 and 1 are the static S2 / S4 casters, the rest orbit far back
			if k == 0:
				return Transform3D(Basis(), S2_CASTER)
			if k == 1:
				return Transform3D(Basis(), S4_CASTER)
			var a := TAU * (t / 8.0 + float(k) / 6.0)
			return Transform3D(Basis(Vector3.UP, a), Vector3(10.0 * cos(a), 2.2, -32.0 + 4.0 * sin(a)))
		_:  # pillars slide through the far ground
			var x := -18.0 + 5.0 * k + 1.5 * sin(TAU * t / 5.0 + k)
			return Transform3D(Basis(Vector3.UP, 0.3 * t + k), Vector3(x, 0.3, -38.0))


static func camera_transform(t: float) -> Transform3D:
	var p := CAMERA_BASE + Vector3(0.6 * sin(TAU * t / 10.0), 0.15 * sin(TAU * t / 7.0), 0.0)
	return Transform3D(Basis(), p).looking_at(CAMERA_TARGET, Vector3.UP)


static func sun_intensity(t: float) -> float:
	return 1.0 + 0.25 * sin(TAU * t / 5.0)


static func omni_intensity(k: int, t: float) -> float:
	return 1.0 + 0.8 * sin(TAU * t / 3.3 + k * PI * 0.5)


static func character_pose(frame: int) -> String:
	if frame < 200:
		return "front"
	if frame < 400:
		return "inside"
	return "behind"


static func character_arm_angle(t: float) -> float:
	return 0.9 * sin(TAU * t / 1.5)
