extends "res://scripts/qa_test_base.gd"
## Depth-tested composite: an opaque mesh must OCCLUDE the splats behind it.
##
## ## WHY THIS SCENE EXISTS
##
## qa_composite_production_defaults (#903/#965) proves splats are PRESENT under
## `composite/depth_test=true`. It contains a splat node and a camera and
## nothing else -- there is no mesh geometry, so `depth_test=true` has no
## occluder to test against and the two scale-0.75 configurations there returned
## bit-identical variance. It proves the splats survive the depth-tested
## composite; it does NOT prove the depth test produces correct occlusion.
##
## Rejecting those pixels is the whole content of the feature:
## `modules/gaussian_splatting/shaders/viewport_blit.glsl:170` drops a splat
## texel outright when the scene surface at that pixel is nearer
## (`scene_view_depth <= gs_view_depth - params.depth_epsilon` -> `return`),
## under the `params.depth_test_enabled != 0` branch opened at :145. That branch
## is on by default -- `GS_SCENE_COMPOSITE_DEPTH_TEST_DEFAULT = true` at
## `modules/gaussian_splatting/renderer/render_pipeline_stages.cpp:200`, read
## per frame through `get_setting_with_override(...)` at :216, which is what
## makes the runtime toggling below sound. Nothing observes that rejection on
## pixels today.
##
## ## THE TRAP THIS ORACLE HAS TO AVOID
##
## "Splat correctly hidden behind a mesh" and "splat missing entirely" are THE
## SAME PIXELS. A presence test over the occluded region cannot tell correct
## occlusion from the GPU-001 (#921) disappearance bug -- it scores both as the
## splats being gone, i.e. as a pass. A scene built that way would certify the
## depth test while being maximally green precisely when the renderer draws
## nothing, which is the failure `describe_blank_capture()` exists to prevent
## and the shape #965's first oracle was measured to have: it passed with the
## splat asset never assigned.
##
## This is not hypothetical here either. Running this scene on the pre-#924
## binary with `depth_test=true` forced to viewport scale 0.75 -- GPU-001
## itself -- produced occluded=-0.8980 against mesh_only=-0.8980, i.e. a
## residual of exactly 0.0000 and a control contrast of 1.3967. Both
## behind-the-mesh gates were perfectly satisfied BY THE BUG. Only the exposed
## wall gave it away, at visible warmth 0.0000.
##
## Two independent separations close it, and BOTH are required to pass:
##
##   1. WITHIN ONE FRAME, at depth_test=true: the region behind the mesh must be
##      free of splat colour while a region of the SAME splat wall that the mesh
##      does not cover must be full of it. Splats vanishing everywhere fails on
##      the second half, and the message says so.
##
##   2. ACROSS CONFIGURATIONS: the same occluded region must fill with splat
##      colour when `depth_test=false`. That is the existence proof that there
##      was something behind the mesh to hide. Without it, a wall that simply
##      does not extend behind the mesh reads as perfect occlusion.
##
## Separation 2 is what makes this scene fail closed. If the depth-test setting
## stops reaching the compositor in either direction, one of the two
## configurations produces the other one's picture and the scene goes red rather
## than quietly measuring one config twice.
##
## ## THE MEASURE
##
## Warmth = mean(red) - mean(blue) over a screen region, the same discriminator
## qa_sort_depth_order.gd already uses (`red_minus_blue`). The splat wall is
## saturated warm, the occluder mesh is saturated cool and unshaded, and the
## clear colour is neutral grey, so the three surfaces separate on one scalar
## and the assertions are CONTRASTS between regions rather than absolute levels
## -- an exposure or tonemap change moves all four regions together and does not
## read as an occlusion failure.
##
## Non-background sample count is deliberately NOT used: #965 measured it
## saturating at exactly 1.0000 under this project's default environment, where
## it passed with the splat asset never assigned.
##
## ## THE FOUR REGIONS
##
## Screen rectangles derived at runtime from the actual node geometry via
## `Camera3D.unproject_position()`, never hard-coded pixel boxes, so a viewport
## resize cannot silently move a region off its subject:
##
##       +---------------------------+
##       | MESH_ONLY  |  BACKGROUND  |   mesh above the wall | clear colour
##       |------------|--------------|
##       | OCCLUDED   |   VISIBLE    |   wall behind mesh    | bare wall
##       |------------|--------------|
##       +---------------------------+
##
## MESH_ONLY vs BACKGROUND proves the occluder rendered at all. Without it, a
## missing occluder plus missing splats would leave OCCLUDED cold and read as a
## pass; and a diagnosis of "not occluded" would be wrong when the real fault is
## that the mesh never drew.
##
## ## WHAT THIS SCENE DOES NOT COVER
##
## Named rather than implied, because a scene that reads as "the depth test is
## verified" would be a worse lie than one that verifies nothing:
##
##   * ONE viewport scale (whatever the project pins, natively 1.0). The
##     interaction between the depth-tested composite and 3D scaling / FSR2 is
##     qa_composite_production_defaults' subject, not this one's, and running
##     both here would make a scale-only regression and an occlusion-only
##     regression share a failure message.
##   * WHOLE-REGION occlusion only. Per-pixel correctness along the silhouette
##     is deliberately excluded by REGION_INSET; a depth test off by a few
##     pixels at the mesh edge passes this scene.
##   * The `depth_epsilon` boundary is not probed. The wall sits 3 world units
##     behind the occluder, far outside it, so this scene says nothing about
##     coplanar or near-coplanar surfaces.
##   * Gate 6 cannot separate "the wall does not reach behind the occluder"
##     from "depth_test=false stopped reaching the compositor". Both produce
##     the same pixels; the message names both rather than guessing.

@export var settle_frames: int = 24
@export var capture_stride: int = 4

const DEPTH_TEST_SETTING := "rendering/gaussian_splatting/composite/depth_test"
const INDIRECT_SH_SCALE_SETTING := "rendering/gaussian_splatting/lighting/indirect_sh_scale"
const DIRECT_LIGHT_SCALE_SETTING := "rendering/gaussian_splatting/lighting/direct_light_scale"

## Splat wall geometry, in the scene's local space. The camera sits at the
## origin looking down -Z, so these are also the world-space extents used to
## derive the wall's screen rectangle.
const WALL_Z := -6.0
const WALL_HALF_WIDTH := 6.5
const WALL_HALF_HEIGHT := 0.9
const WALL_SPACING := 0.35
const WALL_SPLAT_SCALE := 0.35

## Opacity travels through the logit lane, not Color.a: `set_splat_count()`
## allocates a zero-filled logit lane that decodes to 0.5 and takes precedence,
## so a fixture relying on Color(..., 1.0) is semi-transparent in practice (the
## #956 finding). A high finite logit makes the wall effectively opaque, which
## this scene needs -- a translucent wall would let the occluder's colour bleed
## through the VISIBLE region and shrink the very contrast being measured.
const OPAQUE_LOGIT := 8.0

## Saturated and opposed on the red/blue axis so warmth separates the two
## surfaces by construction rather than by luck of the tonemapper.
const SPLAT_COLOR := Color(1.0, 0.35, 0.0, 1.0)

## Fraction of each candidate rectangle trimmed from every edge before
## sampling. Gaussians fall off softly and the mesh silhouette is a hard edge;
## the inset keeps both transitions out of every region so a region's mean is
## the mean of one surface.
const REGION_INSET := 0.18

## A region below this many samples is not measured, it is guessed. Fail rather
## than average four pixels -- an empty region is the quiet way this scene would
## become vacuous after a viewport or framing change.
const MIN_REGION_SAMPLES := 200

## Warmth a region must carry to count as "the splat wall reached the screen
## here". Measured on the reference run: 0.5010 for exposed wall, 0.0540 for
## bare background. The floor sits between them with a wide margin on both
## sides so an exposure change does not read as a disappearance.
const MIN_SPLAT_WARMTH := 0.15

## How much warmer the occluded region may be than the SAME MESH with nothing
## behind it before the wall counts as leaking through. Measured: 0.0000 when
## the depth test occludes -- the two regions are literally the same surface --
## against 1.2901 with the depth-test setting withheld from the compositor. The
## budget sits at 1/21 of the signal it has to reject and 0.06 above the signal
## it has to accept, so it is not a near-threshold judgement in either
## direction.
const MAX_OCCLUDED_RESIDUAL := 0.06

## Warmth swing the depth_test=false control must produce in the occluded
## region. This is the existence proof, and the floor is the same order as
## MIN_SPLAT_WARMTH because it is the same question: did splat colour arrive.
const MIN_OCCLUSION_CONTRAST := 0.15

## Warmth contrast between the bare clear colour and the unshaded occluder.
## The occluder is strongly blue and the clear colour is neutral grey, so this
## is a large signal; it exists to distinguish "the mesh occluded the splats"
## from "there was no mesh and no splats".
const MIN_MESH_CONTRAST := 0.15

var splat_node: GaussianSplatNode3D
var occluder: MeshInstance3D
var camera: Camera3D

## depth_test=true is FIRST so the production default is what the scene measures
## before it has touched anything else, and the control runs second.
var _configs: Array[Dictionary] = [
	{"name": "depth_true", "depth_test": true},
	{"name": "depth_false", "depth_test": false},
]
var _config_index: int = 0
var _frames_in_config: int = 0
var _applied: bool = false
var _measurements: Array[Dictionary] = []
var _fatal: String = ""
var _last_visible_splats: int = -1

var _prev_settings: Dictionary = {}


func _ready():
	test_name = "Composite Depth Occlusion"
	# Wall-clock deadline, not a frame budget. Reaching it without all
	# configurations measured is a FAIL that names what never became true; it is
	# never a silent pass.
	test_duration = 30.0
	warmup_frames = 10
	super._ready()
	splat_node = get_node_or_null("SplatNode")
	occluder = get_node_or_null("Occluder")
	camera = get_node_or_null("Camera3D")


func _on_test_start():
	for key in [DEPTH_TEST_SETTING, INDIRECT_SH_SCALE_SETTING, DIRECT_LIGHT_SCALE_SETTING]:
		_prev_settings[key] = ProjectSettings.get_setting(key)

	# Render the splat wall from its DC colour alone. Scene lighting would make
	# the wall's warmth depend on light placement, and this scene asserts on
	# colour; qa_sort_depth_order.gd pins the same two settings for the same
	# reason.
	ProjectSettings.set_setting(INDIRECT_SH_SCALE_SETTING, 1.0)
	ProjectSettings.set_setting(DIRECT_LIGHT_SCALE_SETTING, 0.0)

	if splat_node != null:
		splat_node.splat_asset = _build_wall()

	_config_index = 0
	_frames_in_config = 0
	_applied = false
	_measurements.clear()


## A dense opaque wall spanning the full width of the frame at WALL_Z.
##
## Generated, not a committed .ply: a stale import cache has silently thinned a
## fixture from 10000 splats to 410 in this repo (#790), and a thinned wall
## would weaken exactly the contrast this scene measures without failing
## anything. The wall must be wide enough to appear BOTH behind the occluder and
## clear of it in the same frame -- that simultaneity is the oracle.
func _build_wall() -> GaussianSplatAsset:
	var asset := GaussianSplatAsset.new()

	var xs: Array[float] = []
	var x := -WALL_HALF_WIDTH
	while x <= WALL_HALF_WIDTH + 0.0001:
		xs.append(x)
		x += WALL_SPACING
	var ys: Array[float] = []
	var y := -WALL_HALF_HEIGHT
	while y <= WALL_HALF_HEIGHT + 0.0001:
		ys.append(y)
		y += WALL_SPACING

	var count := xs.size() * ys.size()
	asset.set_splat_count(count)

	var positions := PackedFloat32Array()
	var colors := PackedColorArray()
	var scales := PackedFloat32Array()
	var logits := PackedFloat32Array()
	for wy in ys:
		for wx in xs:
			positions.append_array(PackedFloat32Array([wx, wy, WALL_Z]))
			colors.append(SPLAT_COLOR)
			scales.append_array(PackedFloat32Array([
				WALL_SPLAT_SCALE, WALL_SPLAT_SCALE, WALL_SPLAT_SCALE
			]))
			logits.append(OPAQUE_LOGIT)

	asset.set_positions(positions)
	asset.set_colors(colors)
	asset.set_scales(scales)
	asset.set_opacity_logits(logits)
	result_metrics["wall_splat_count"] = count
	return asset


func _apply_current_config() -> void:
	var config: Dictionary = _configs[_config_index]
	ProjectSettings.set_setting(DEPTH_TEST_SETTING, config["depth_test"])
	_applied = true


func _on_test_frame(_delta: float):
	if not _fatal.is_empty():
		return
	if _config_index >= _configs.size():
		return

	if not _applied:
		_apply_current_config()
		_frames_in_config = 0
		return

	_frames_in_config += 1
	# A fixed LOWER bound only. The composite re-reads the setting every frame,
	# but the swap still has to travel through the sorter and the presented
	# image before a capture means anything. The upper bound is the wall-clock
	# deadline in _on_test_complete(), not a frame count.
	if _frames_in_config < settle_frames:
		return

	# Readiness, not the oracle: keep pumping until the cull stage has actually
	# admitted the wall. A cull-stage count cannot see a composite-stage
	# disappearance -- GPU-001 kept every internal counter green -- so this
	# gates WHEN to capture and never WHAT passed. Recorded so the deadline
	# message below can name whether readiness was the thing that never came.
	if splat_node != null and splat_node.has_method("get_visible_splat_count"):
		_last_visible_splats = int(splat_node.get_visible_splat_count())
		if _last_visible_splats <= 0:
			return

	var image := capture_viewport()
	if image == null:
		return

	var blank := describe_blank_capture(image, _configs[_config_index]["name"])
	if not blank.is_empty():
		_fatal = blank
		_finish_test()
		return

	var regions := _resolve_regions(image)
	if regions.is_empty():
		_finish_test()
		return

	var measurement := {"name": _configs[_config_index]["name"]}
	for region_name in regions:
		var stats := _region_stats(image, regions[region_name])
		if int(stats["samples"]) < MIN_REGION_SAMPLES:
			_fatal = ("region '%s' collected %d samples < %d in configuration '%s': "
					+ "the measurement window does not cover its subject") % [
				region_name, int(stats["samples"]), MIN_REGION_SAMPLES,
				_configs[_config_index]["name"]
			]
			_finish_test()
			return
		measurement[region_name] = stats
	_measurements.append(measurement)

	_config_index += 1
	_applied = false
	if _config_index >= _configs.size():
		_finish_test()


## Screen rectangles for the four regions, derived from the scene's own
## geometry. Returns {} and sets _fatal when the layout cannot be resolved --
## never a silently degraded rectangle.
func _resolve_regions(image: Image) -> Dictionary:
	if camera == null or occluder == null:
		_fatal = "scene is missing its Camera3D or its Occluder mesh; nothing could be occluded"
		return {}

	var bounds := Rect2i(Vector2i.ZERO, image.get_size())

	# The wall's extents are the constants _build_wall() generates from, taken
	# through the splat node's own transform rather than assumed to be world
	# space. Both rectangles therefore follow the nodes if either is moved, and
	# a move that breaks the layout is reported below instead of quietly
	# sampling the wrong pixels.
	var wall_basis: Transform3D = (
		splat_node.global_transform if splat_node != null else Transform3D.IDENTITY
	)
	var wall_rect := _project_rect([
		wall_basis * Vector3(-WALL_HALF_WIDTH, -WALL_HALF_HEIGHT, WALL_Z),
		wall_basis * Vector3(WALL_HALF_WIDTH, -WALL_HALF_HEIGHT, WALL_Z),
		wall_basis * Vector3(-WALL_HALF_WIDTH, WALL_HALF_HEIGHT, WALL_Z),
		wall_basis * Vector3(WALL_HALF_WIDTH, WALL_HALF_HEIGHT, WALL_Z),
	], image)

	var aabb := occluder.get_aabb()
	var occluder_corners: Array[Vector3] = []
	for i in range(8):
		occluder_corners.append(occluder.global_transform * aabb.get_endpoint(i))
	var occluder_rect := _project_rect(occluder_corners, image)

	if wall_rect.size.x <= 0 or wall_rect.size.y <= 0:
		_fatal = "the splat wall projects to an empty screen rectangle"
		return {}
	if occluder_rect.size.x <= 0 or occluder_rect.size.y <= 0:
		_fatal = "the occluder mesh projects to an empty screen rectangle"
		return {}

	# Horizontal band where the mesh stands in front of the wall, and the band
	# to its right where the wall is exposed.
	var covered_x0: int = max(wall_rect.position.x, occluder_rect.position.x)
	var covered_x1: int = min(wall_rect.end.x, occluder_rect.end.x)
	var exposed_x0: int = occluder_rect.end.x
	var exposed_x1: int = wall_rect.end.x
	# Vertical band of the wall, and the band above it where the mesh continues
	# but the wall does not reach.
	var mid_y0: int = wall_rect.position.y
	var mid_y1: int = wall_rect.end.y
	var top_y0: int = occluder_rect.position.y
	var top_y1: int = wall_rect.position.y

	var raw := {
		"occluded": Rect2i(
			Vector2i(covered_x0, mid_y0),
			Vector2i(covered_x1 - covered_x0, mid_y1 - mid_y0)
		),
		"visible": Rect2i(
			Vector2i(exposed_x0, mid_y0),
			Vector2i(exposed_x1 - exposed_x0, mid_y1 - mid_y0)
		),
		"mesh_only": Rect2i(
			Vector2i(covered_x0, top_y0),
			Vector2i(covered_x1 - covered_x0, top_y1 - top_y0)
		),
		"background": Rect2i(
			Vector2i(exposed_x0, top_y0),
			Vector2i(exposed_x1 - exposed_x0, top_y1 - top_y0)
		),
	}

	# Inset, clip to the image, and refuse any region that did not survive. A
	# region can collapse three ways -- the bands never overlapped, the inset ate
	# it, or it fell off the frame -- and all three mean the same thing here:
	# this scene would be averaging the wrong pixels, which is the only way its
	# verdict becomes fiction while still being a number.
	var regions := {}
	var degenerate: Array[String] = []
	for region_name in raw:
		var rect: Rect2i = raw[region_name]
		if rect.size.x <= 0 or rect.size.y <= 0:
			degenerate.append("%s=%s (bands do not overlap)" % [region_name, str(rect)])
			continue
		var clipped: Rect2i = _inset(rect).intersection(bounds)
		if clipped.size.x <= 0 or clipped.size.y <= 0:
			degenerate.append("%s=%s (inset or clipped to nothing)" % [region_name, str(rect)])
			continue
		regions[region_name] = clipped
	if not degenerate.is_empty():
		_fatal = ("region layout is degenerate (%s) for wall=%s occluder=%s: the mesh no longer "
				+ "overlaps the wall the way this scene assumes") % [
			", ".join(degenerate), str(wall_rect), str(occluder_rect)
		]
		return {}

	# PRINTED, NOT RECORDED. These are pixel rectangles, so they are a function
	# of the runner's window size. run_baseline_qa.py compares a STRING metric
	# by exact equality as a path-identity pin, so recording this would freeze
	# the capture resolution into a blocking baseline and go red on any runner
	# that windows differently -- the machine, not the renderer. Everything this
	# scene gates on is a ratio between regions and carries no such dependence.
	print("[QA:%s]   region layout wall=%s occluder=%s" % [
		test_name, str(wall_rect), str(occluder_rect)
	])
	return regions


func _project_rect(points: Array, image: Image) -> Rect2i:
	var min_p := Vector2(INF, INF)
	var max_p := Vector2(-INF, -INF)
	var viewport_size: Vector2 = camera.get_viewport().get_visible_rect().size
	var to_image := Vector2(
		float(image.get_width()) / max(1.0, viewport_size.x),
		float(image.get_height()) / max(1.0, viewport_size.y)
	)
	for p in points:
		var screen: Vector2 = camera.unproject_position(p) * to_image
		min_p = min_p.min(screen)
		max_p = max_p.max(screen)
	return Rect2i(
		Vector2i(int(floor(min_p.x)), int(floor(min_p.y))),
		Vector2i(int(ceil(max_p.x - min_p.x)), int(ceil(max_p.y - min_p.y)))
	)


func _inset(rect: Rect2i) -> Rect2i:
	var dx := int(round(float(rect.size.x) * REGION_INSET))
	var dy := int(round(float(rect.size.y) * REGION_INSET))
	return Rect2i(
		rect.position + Vector2i(dx, dy),
		rect.size - Vector2i(dx * 2, dy * 2)
	)


func _region_stats(image: Image, rect: Rect2i) -> Dictionary:
	var stride: int = int(max(1, capture_stride))
	var r_sum := 0.0
	var g_sum := 0.0
	var b_sum := 0.0
	var samples := 0
	var y := rect.position.y
	while y < rect.end.y:
		var x := rect.position.x
		while x < rect.end.x:
			var c := image.get_pixel(x, y)
			r_sum += c.r
			g_sum += c.g
			b_sum += c.b
			samples += 1
			x += stride
		y += stride
	if samples <= 0:
		return {"samples": 0, "warmth": 0.0, "r": 0.0, "g": 0.0, "b": 0.0}
	var r := r_sum / float(samples)
	var g := g_sum / float(samples)
	var b := b_sum / float(samples)
	return {"samples": samples, "warmth": r - b, "r": r, "g": g, "b": b}


func _on_test_complete():
	_restore_settings()

	if not _fatal.is_empty():
		_test_result = false
		_test_message = "%s could not measure depth occlusion: %s" % [SKIP_MARKER, _fatal]
		return

	if _measurements.size() != _configs.size():
		result_metrics["last_visible_splats"] = _last_visible_splats
		# Name what never became true, rather than reporting a bare timeout.
		var never := ("the cull stage reported %d visible splats but the capture and measure "
				+ "step never completed for every configuration") % _last_visible_splats
		if _last_visible_splats <= 0:
			never = ("the cull stage never admitted a splat (get_visible_splat_count stayed %d), "
					+ "so no frame was ever worth capturing") % _last_visible_splats
		_test_result = false
		_test_message = ("%s deadline expired with %d of %d composite configurations measured: "
				+ "%s") % [
			SKIP_MARKER, _measurements.size(), _configs.size(), never
		]
		return

	var depth_true: Dictionary = _measurements[0]
	var depth_false: Dictionary = _measurements[1]

	# Every region of every configuration is recorded and printed, on PASS as
	# well as FAIL. A bare verdict cannot be told apart from a scene that
	# measured one frame twice, and the four raw warmths are what a reviewer
	# needs to see that the two configurations really did differ.
	for m in _measurements:
		for region_name in ["occluded", "visible", "mesh_only", "background"]:
			var stats: Dictionary = m[region_name]
			result_metrics["%s_%s_warmth" % [m["name"], region_name]] = stats["warmth"]
			print("[QA:%s]   %s/%s warmth=%.4f rgb=(%.3f,%.3f,%.3f) samples=%d" % [
				test_name, m["name"], region_name, stats["warmth"],
				stats["r"], stats["g"], stats["b"], int(stats["samples"])
			])

	var visible_warmth_true: float = float(depth_true["visible"]["warmth"])
	var visible_warmth_false: float = float(depth_false["visible"]["warmth"])

	# GATE: the region behind the mesh must look like THE SAME MESH with nothing
	# behind it. Comparing occluded against mesh_only rather than against the
	# exposed wall is deliberate -- the exposed-wall contrast is inflated by the
	# occluder's own blue and would still clear its floor with the splat wall
	# entirely absent, which is the confusion this whole scene exists to avoid.
	var occluded_residual: float = (
		float(depth_true["occluded"]["warmth"]) - float(depth_true["mesh_only"]["warmth"])
	)
	# GATE: cross-configuration existence proof. The same occluded region must
	# fill with splat colour when the depth test is off.
	var control_contrast: float = (
		float(depth_false["occluded"]["warmth"]) - float(depth_true["occluded"]["warmth"])
	)
	# GATE: the occluder itself rendered -- unshaded blue mesh vs neutral clear
	# colour, measured where the wall does not reach.
	var mesh_contrast: float = (
		float(depth_true["background"]["warmth"]) - float(depth_true["mesh_only"]["warmth"])
	)
	# RECORDED, NOT A GATE. In-frame exposed-vs-occluded separation. It is the
	# intuitive oracle, which is exactly why it is written down as not being
	# one: it is contaminated by the occluder's own blue, so it stays large even
	# when no splats reach the screen at all. Measured under the GPU-001
	# reproduction -- zero splats presented -- it still scored 0.8980, far above
	# any floor it could plausibly be given. occluded_residual is the gate.
	var occlusion_contrast: float = (
		visible_warmth_true - float(depth_true["occluded"]["warmth"])
	)

	result_metrics["occluded_residual"] = occluded_residual
	result_metrics["control_contrast"] = control_contrast
	result_metrics["mesh_contrast"] = mesh_contrast
	result_metrics["occlusion_contrast_not_gated"] = occlusion_contrast
	result_metrics["visible_warmth_delta"] = visible_warmth_true - visible_warmth_false
	# The four acceptance thresholds are emitted under names ending in
	# `_threshold` DELIBERATELY. run_baseline_qa.py::_metric_rule() classifies
	# that suffix as an `exact_contract` and pins it by equality, while a
	# measurement gets a tolerance. Loosening a floor here to make a red scene
	# green is the one edit no measurement tolerance can detect, so it is the
	# one that has to be pinned; the naming is what wires these into that
	# machinery.
	result_metrics["splat_warmth_threshold"] = MIN_SPLAT_WARMTH
	result_metrics["occluded_residual_threshold"] = MAX_OCCLUDED_RESIDUAL
	result_metrics["control_contrast_threshold"] = MIN_OCCLUSION_CONTRAST
	result_metrics["mesh_contrast_threshold"] = MIN_MESH_CONTRAST

	var summary := ("occluded_residual=%.4f control_contrast=%.4f mesh_contrast=%.4f "
			+ "visible_warmth(true/false)=%.4f/%.4f") % [
		occluded_residual, control_contrast, mesh_contrast,
		visible_warmth_true, visible_warmth_false
	]

	# ORDER MATTERS. Several faults collapse more than one statistic at once, so
	# the checks run from "this scene could not have measured anything" outwards
	# to "it measured, and the answer is wrong". Reordering them does not change
	# PASS/FAIL, it changes whether the message names the real fault.

	# 1. No occluder -> nothing in this scene could occlude anything.
	if mesh_contrast < MIN_MESH_CONTRAST:
		_test_result = false
		_test_message = ("Occluder mesh did not render (mesh_contrast=%.4f < %.4f): the region "
				+ "above the wall is indistinguishable from bare background, so nothing here "
				+ "could have occluded anything and no occlusion verdict is possible. %s") % [
			mesh_contrast, MIN_MESH_CONTRAST, summary
		]
		return

	# 2. No splats in EITHER configuration -> a broad rendering failure, not an
	#    occlusion result. Separated from case 3 because the two reds mean very
	#    different things and #965 showed how easily they get conflated.
	if visible_warmth_true < MIN_SPLAT_WARMTH and visible_warmth_false < MIN_SPLAT_WARMTH:
		_test_result = false
		_test_message = ("Splats ABSENT in BOTH configurations (visible_warmth true=%.4f "
				+ "false=%.4f, both < %.4f) -- a broad rendering failure, NOT an occlusion "
				+ "result: the exposed wall the mesh never covers is gone too. %s") % [
			visible_warmth_true, visible_warmth_false, MIN_SPLAT_WARMTH, summary
		]
		return

	# 3. Splats present with the depth test OFF and gone with it ON, on wall the
	#    mesh does not cover. That is the #921 disappearance, and it would be
	#    read as flawless occlusion by any oracle that only looked behind the
	#    mesh.
	if visible_warmth_true < MIN_SPLAT_WARMTH:
		_test_result = false
		_test_message = ("Splats ABSENT under depth_test=true (visible_warmth=%.4f < %.4f) on "
				+ "wall the occluder does not cover, while present at depth_test=false (%.4f) "
				+ "-- the #921 disappearance signature, NOT occlusion. %s") % [
			visible_warmth_true, MIN_SPLAT_WARMTH, visible_warmth_false, summary
		]
		return

	# 4. The control frame itself is broken; its verdict below would be noise.
	if visible_warmth_false < MIN_SPLAT_WARMTH:
		_test_result = false
		_test_message = ("The depth_test=false control frame has no splats (visible_warmth=%.4f "
				+ "< %.4f) although depth_test=true does (%.4f); the control cannot establish "
				+ "what was behind the mesh. %s") % [
			visible_warmth_false, MIN_SPLAT_WARMTH, visible_warmth_true, summary
		]
		return

	# 5. The wall leaked through the opaque mesh.
	if occluded_residual > MAX_OCCLUDED_RESIDUAL:
		_test_result = false
		_test_message = ("Splats NOT occluded (occluded_residual=%.4f > %.4f): with "
				+ "depth_test=true the wall behind the opaque mesh is warmer than the same mesh "
				+ "with nothing behind it, i.e. the depth test rejected nothing. %s") % [
			occluded_residual, MAX_OCCLUDED_RESIDUAL, summary
		]
		return

	# 6. The occluded region is clean -- but was there ever anything there? Two
	#    causes are indistinguishable from pixels alone and the message says so
	#    rather than picking one.
	if control_contrast < MIN_OCCLUSION_CONTRAST:
		_test_result = false
		_test_message = ("Control FAILED (control_contrast=%.4f < %.4f): the occluded region is "
				+ "clean, but turning the depth test OFF did not fill it with splats -- either "
				+ "the wall does not reach behind the occluder, or depth_test=false no longer "
				+ "reaches the compositor. Either way 'occluded' here is unfalsifiable. %s") % [
			control_contrast, MIN_OCCLUSION_CONTRAST, summary
		]
		return

	_test_result = true
	_test_message = ("Depth-tested composite occludes correctly: hidden behind the mesh, drawn "
			+ "beside it, restored when the depth test is off. %s") % summary


func _restore_settings() -> void:
	for key in _prev_settings:
		if _prev_settings[key] != null:
			ProjectSettings.set_setting(key, _prev_settings[key])
