extends "res://scripts/qa_test_base.gd"
## Production-default composite test: splats must be PRESENT with depth_test=true.
##
## ## WHY THIS SCENE EXISTS (#903, guarding the #921/#924 fix)
##
## The QA project pins `composite/depth_test=false`
## (tests/examples/godot/test_project/project.godot), while the shipped default
## is TRUE (gaussian_splat_manager.cpp:998). Every other scene in this suite
## therefore renders a configuration no user ships, and the baseline it compares
## against was captured under that same pin -- so both sides drift together and
## the depth-tested composite path is exercised by no per-PR pixel gate at all.
##
## That is not theoretical. GPU-001 (#921) was splats ABSENT at bilinear scale
## 0.75 and under FSR2, *only* when depth_test=true, while every internal
## counter reported success. The defect shipped as the default configuration and
## the blocking visual gate could not see it. #924 fixed it by compositing at
## the pre-upscale seam; this scene is what stops it coming back.
##
## ## THE ORACLE
##
## Presence, not similarity. Two blank frames score SSIM 1.0, so a
## similarity-only scene reports its strongest pass precisely when the renderer
## drew nothing -- the failure mode `describe_blank_capture()` exists to catch.
## Here the assertion is categorical: each configuration must put real content
## on screen, measured as non-background samples via `measure_capture_content()`.
##
## ## THE CONFIGURATIONS
##
## The three the Phase-0 audit matrix distinguished:
##
##   1. depth_test=true,  scale 1.0   -- the shipped default at native size
##   2. depth_test=true,  scale 0.75  -- ABSENT before #924; the actual defect
##   3. depth_test=false, scale 0.75  -- the QA pin's own config, always PRESENT
##
## Case 3 is a control and is load-bearing. If it were omitted, a total
## rendering failure would fail this scene the same way a depth-composite
## regression does. With it, a #921-shaped regression produces a *discriminating*
## signature -- 1 and 2 absent while 3 stays present -- which names the
## subsystem instead of just reporting red.
##
## ## PHASE 2: TEMPORAL STABILITY (#929)
##
## Presence is not the only way the pre-upscale composite can be wrong. The
## other way is SUB-PIXEL PLACEMENT, and nothing in this suite could see it.
##
## The splat pipeline builds its GPU projection from the raw camera projection
## with a flip_y and nothing else, while every mesh in the frame is rendered
## through `RenderSceneDataRD::get_cam_projection()`, which adds the engine's
## per-frame `taa_jitter`. FSR2, handed that same jitter immediately below the
## splat composite hook, UN-jitters the whole internal buffer by it. A layer that
## was never jittered therefore comes out displaced by +jitter, changing every
## frame along the Halton sequence: splats swim against the geometry beside them.
##
## Phase 2 measures that directly, in a dedicated 480x270 SubViewport carrying a
## splat cluster on the left and a high-frequency checker-textured UNSHADED box
## on the right, in DISJOINT screen rectangles of one frame. The box is the
## in-frame control: same frame, same temporal stage, same capture path, but
## drawn by ordinary geometry with the jitter-corrected projection, so whatever
## it does is by definition what a correctly integrated layer does here.
##
## The oracle is the sub-pixel displacement of each region against its own
## window-mean frame (`scripts/qa_subpixel_swim.gd`), in PIXELS, gated as an
## absolute ceiling calibrated from measurements of BOTH populations -- see
## SWIM_MAX_DISPLACEMENT_PX for the numbers and for why it is not a ratio
## against the control.
##
## Two cheaper statistics are recorded and deliberately NOT gated, because each
## was measured not to discriminate:
##
##   * Colour-difference magnitude. On the real-scan rig the defective splat
##     region scored 1.089 mean LSB against the correct mesh control's 2.365 --
##     the broken layer looked BETTER, because contrast and not displacement
##     dominates that number.
##   * The periodicity that ATTRIBUTED the defect (autocorrelation +1.00 at the
##     engine's jitter phase). Measured on the same captures the mesh control
##     autocorrelates +1.000 at lag 8 as well: FSR2's resolve is phase-periodic
##     under a static camera whether or not the layer feeding it is jittered.
##
## Three gates run BEFORE any displacement is scored, in this order:
##
##   1. RIG DETERMINISM -- with no temporal stage, both regions must be bit
##      identical frame to frame (delta exactly 0). If the rig itself jitters,
##      nothing measured after it means anything.
##
##      TWO WARNINGS FOR ANYONE EXTENDING THIS SCENE TO A MOVING CAMERA, both
##      found the hard way by #1025's investigation and neither affecting the
##      static-camera gate here:
##
##      * **Exact-zero determinism would be flaky.** Under a moving camera the GS
##        pipeline is very nearly but not exactly frame-deterministic: an earlier
##        matrix measured 3-9 LSB over <=963 of 16.6M pixels in 5 of 6 runs
##        (<=0.006%), intermittently. With a static camera it is exactly 0 in
##        every run ever taken, which is why this gate can demand exact equality.
##        A moving-camera variant needs a tolerance, and choosing one needs its
##        own evidence.
##      * **The checker control below is not usable under motion.** A 2 px checker
##        is the right worst case for a static-camera jitter test and the wrong
##        one here: at 6-8 px/frame it aliases, and a shift-mixture model explains
##        only 52-68% of its variance (residual 0.32-0.48) against 0.03-0.11 for a
##        band-limited smooth-noise box. Under motion, use band-limited content.
##   2. REGION DISCRIMINATION -- hiding the splat node must change the splat
##      rectangle and leave the mesh rectangle alone, and vice versa. Without
##      this the two rectangles could both be pointing at background, and every
##      ratio below would compare noise with noise.
##   3. STAGE ENGAGED -- in each temporal configuration the MESH region must
##      move during the engage window. TAA or FSR2 silently not running produces
##      a perfectly stable splat region, which is indistinguishable from a
##      perfectly integrated one unless something proves the stage ran (#997).
##
## A region whose gradient energy is too low to solve is reported UNMEASURABLE
## and fails; it is never recorded as 0.0 px, which would read as flawless.

@export var settle_frames: int = 24
@export var capture_stride: int = 3

## Wall-clock floor per configuration, enforced ALONGSIDE settle_frames.
##
## A frame count alone is not a settle condition. On a fast runner 24 frames can
## elapse in a fraction of a second -- before the freshly assigned asset has
## finished uploading and the sorter has produced a presentable frame -- and the
## scene would then measure an unsettled frame and record it as the result. For
## `depth_true_scale_075` specifically that reads as variance ~0, i.e. a FALSE
## GPU-001 regression, which is the most damaging way this scene could be wrong.
##
## Deliberately a FLOOR and not a wait-for-content loop: waiting until content
## appears would mask a genuine disappearance, which is the defect this scene
## exists to catch. Both conditions must be met, so a real ABSENT still reads as
## absent once the renderer has had a fair chance in wall-clock terms.
@export var settle_seconds: float = 1.5

## Upper bound on how long one configuration may take to produce content.
##
## settle_seconds alone is a MINIMUM: after it elapses this scene used to record
## the next eligible frame whatever it contained, so on a loaded runner -- where
## asset upload and the first sort can exceed 1.5 s -- the recorded frame is
## still transiently blank and the scene reports a FALSE GPU-001 disappearance
## on a healthy build.
##
## So after the minimum, keep sampling until content appears or this deadline
## expires. A genuine absence is still caught: nothing ever appears, the deadline
## expires, and the last (blank) measurement is recorded and fails the variance
## floor. The difference is that a slow frame no longer looks like a missing one.
@export var readiness_deadline_seconds: float = 12.0

const DEPTH_TEST_SETTING := "rendering/gaussian_splatting/composite/depth_test"

## A configuration is PRESENT when the captured frame carries at least this
## much luma variance, i.e. structure rather than a flat field.
##
## WHY VARIANCE AND NOT THE NON-BACKGROUND SAMPLE COUNT. The obvious oracle --
## "how many pixels are brighter than the background" -- is VACUOUS here, and
## that was measured, not assumed. Under this project's default environment the
## sky already exceeds `CAPTURE_BACKGROUND_LUMA_THRESHOLD`, so the ratio pins at
## exactly 1.0000 for every configuration. An early version of this scene using
## that oracle passed all three configurations WITH THE SPLAT ASSET NEVER
## ASSIGNED. Variance separates cleanly on the same frames: ~0.008 when splats
## reach the presented image, exactly 0.000000 when they do not.
##
## The floor sits roughly an order of magnitude under the healthy value, so an
## exposure or framing change does not read as a disappearance, while the ABSENT
## case fails by the entire margin.
const MIN_PRESENT_LUMA_VARIANCE := 0.001

## Opacity is set through the logit lane, not Color.a. `set_splat_count()`
## allocates a zero-filled logit lane that decodes to 0.5 and takes precedence,
## so a fixture relying on Color(..., 1.0) is semi-transparent in practice
## (the #956 finding). A high finite logit makes the cluster effectively opaque.
const OPAQUE_LOGIT := 8.0

var splat_node: GaussianSplatNode3D

var _configs: Array[Dictionary] = [
	{"name": "depth_true_scale_100", "depth_test": true, "scale": 1.0},
	{"name": "depth_true_scale_075", "depth_test": true, "scale": 0.75},
	{"name": "depth_false_scale_075", "depth_test": false, "scale": 0.75},
]
var _config_index: int = 0
var _frames_in_config: int = 0
var _config_started_at: float = 0.0
var _measurements: Array[Dictionary] = []
var _applied: bool = false

var _prev_depth_test = null
var _prev_scaling_mode = null
var _prev_scaling_scale = null

# ---------------------------------------------------------------- phase 2 --
const Swim := preload("res://scripts/qa_subpixel_swim.gd")

const SWIM_VIEW_SIZE := Vector2i(480, 270)
## Disjoint screen rectangles, with a gutter so neither can bleed into the other.
## Proved to name the right content at runtime by the discrimination gate.
const SWIM_SPLAT_RECT := Rect2i(16, 16, 192, 238)
const SWIM_MESH_RECT := Rect2i(272, 16, 192, 238)

const SWIM_WARM_MIN_FRAMES := 24
const SWIM_WARM_MAX_FRAMES := 300
const SWIM_REF_SETTLE := 8
const SWIM_RESET_FRAMES := 6
const SWIM_ENGAGE_FRAMES := 6
## FSR2 accumulates a history; a window captured before it converges measures the
## transient, not the steady state the user looks at.
const SWIM_SETTLE_FRAMES := 56
const SWIM_SEQ_FRAMES := 16

## A region must change by at least this many LSB (mean, per channel) when its
## content is hidden, or the rectangle is not looking at that content.
const SWIM_DISCRIMINATION_MIN_LSB := 1.0
## ...and the OTHER region must not change by more than this when it does.
const SWIM_CROSSTALK_MAX_LSB := 0.5

## THE GATE, and how its value was chosen.
##
## An ABSOLUTE displacement in pixels, because that is the quantity the defect
## produces: the engine's Halton jitter spans +-0.5 pixel, so a layer that is not
## carrying it is misplaced by up to half a pixel every frame, while a layer that
## is carrying it is misplaced by whatever the resolve's own reconstruction noise
## amounts to. Measured on THIS scene, both sides, same binary configuration
## (optimize=speed_trace, RTX 3090, 480x270 SubViewport):
##
##     config      splat px, jitter ABSENT   splat px, jitter APPLIED   mesh control
##     fsr2_100            0.3066                      0.0929              0.0030
##     fsr2_050            0.2444                      0.0542              0.0022
##
## 0.15 px is very nearly the geometric midpoint of the two NEAREST NEIGHBOURS
## across every measurement taken -- sqrt(0.0929 * 0.2219) = 0.144 -- where
## 0.0929 is the largest jitter-applied value seen and 0.2219 the smallest
## jitter-absent one. That is 1.61x above every passing measurement and 1.48x
## below every failing one.
##
## Every observation, on one RTX 3090 / Vulkan 1.4.325:
##   jitter ABSENT   fsr2_100: 0.3045 0.3066 0.3047 0.3052 0.3041 0.3066 0.3047
##                   fsr2_050: 0.2444 0.2347 0.2252 0.2219 0.2172 0.2403
##   jitter APPLIED  fsr2_100: 0.0929 0.0929 0.0929 0.0929 0.09286
##                   fsr2_050: 0.0542 0.0569 0.0614 0.0570 0.0545
## (the 0.3041 / 0.2252 pair is a mutation build that reverts ONLY the jitter, so
## the defect population is not just "an older tree"; the 0.09286 / 0.0545 pair is
## a CI run on a DIFFERENT build configuration -- dev_build rather than
## optimize=speed_trace -- which is why the pass side is quoted to five figures
## there. The last two of each fsr2_100 row and the last of each fsr2_050 row were
## taken after a stray GPU process that had been loading the machine since
## 2026-09-16 was killed; they are indistinguishable from the ones taken under it,
## which is the evidence that this metric does not respond to GPU contention.)
##
## Separation as measured: highest passing 0.0929, lowest failing 0.2172. 0.15 px
## is 1.61x above the former and 1.45x below the latter, and within 6% of their
## geometric midpoint.
##
## This is ONE GPU and one driver. A different FSR2 reconstruction could move the
## pass-side value, and 1.61x of headroom is the whole budget for that; if this
## ever reds on a correct tree, widen it toward the 0.2172 px lower edge of the
## defect population and record the new measurement here -- do not delete the gate.
##
## NOT a ratio against the mesh control. That was the first design and the
## measurement rejected it: with the jitter applied the splat layer still sits
## around 30x the control. The reason is now measured rather than guessed, and it
## is not the one first written here: an earlier revision blamed "no depth
## write-back, no motion vectors and no reactive mask", but #1025 established that
## the composite DOES feed FSR2's reactive mask -- splat coverage lands in the
## destination alpha, which is literally what `params.reactive` samples
## (`viewport_blit.glsl:206-208` -> `render_scene_buffers_rd.h:257-264`), clamped
## to 0.9. FSR2 is therefore deliberately NOT accumulating history on splat pixels,
## so they track the raw jittered frame instead of converging like the mesh does.
## The residual is a consequence of that suppression, not of missing information. Any ratio tight enough to catch the missing jitter would
## also fail a correctly jittered layer. The control is still load-bearing --
## it is what proves the temporal stage ran at all -- and its displacement is
## reported beside every measurement; it is just not the threshold.
const SWIM_MAX_DISPLACEMENT_PX := 0.15

enum SwimPhase { PRESENCE, TEMPORAL, DONE }

var _swim_phase: int = SwimPhase.PRESENCE
var _swim_state: String = "init"
var _swim_frames: int = 0
var _swim_config: int = 0
var _swim_view: SubViewport = null
var _swim_splat: GaussianSplatNode3D = null
var _swim_mesh: MeshInstance3D = null
var _swim_camera: Camera3D = null
var _swim_engage: Array = []
var _swim_seq: Array = []
var _swim_refs: Dictionary = {}
var _swim_results: Array[Dictionary] = []
var _swim_failures: Array[String] = []
var _swim_notes: Array[String] = []

## Engine jitter phase lengths, for the reported (never gated) periodicity
## diagnostic: renderer_viewport.cpp:252-260 -- 8*(target/render)^2 for temporal
## upscaling, a flat 16 for TAA.
var _swim_configs: Array[Dictionary] = [
	{"name": "none", "taa": false, "fsr2": false, "scale": 1.0, "phase": 0},
	{"name": "taa", "taa": true, "fsr2": false, "scale": 1.0, "phase": 16},
	{"name": "fsr2_100", "taa": false, "fsr2": true, "scale": 1.0, "phase": 8},
	{"name": "fsr2_050", "taa": false, "fsr2": true, "scale": 0.5, "phase": 32},
]


func _ready():
	test_name = "Composite Production Defaults"
	# Must comfortably exceed configs x readiness_deadline_seconds. On a BROKEN
	# build the absent configuration burns its full deadline, and a run cut short
	# would report a SKIP instead of the failure -- turning the defect this scene
	# exists to catch into a non-result.
	#
	# A CEILING, not a runtime: both phases call _finish_test() as soon as they
	# are done (the qa_composite_depth_occlusion.gd:361 pattern), so the suite
	# does not pay for the headroom phase 2 needs on a slow runner.
	test_duration = 150.0
	warmup_frames = 10
	super._ready()
	splat_node = get_node_or_null("SplatNode")


func _on_test_start():
	_prev_depth_test = ProjectSettings.get_setting(DEPTH_TEST_SETTING)
	var viewport := get_viewport()
	if viewport != null:
		_prev_scaling_mode = viewport.scaling_3d_mode
		_prev_scaling_scale = viewport.scaling_3d_scale

	if splat_node != null:
		splat_node.splat_asset = _build_cluster()

	_config_index = 0
	_frames_in_config = 0
	_measurements.clear()
	_applied = false


## A compact wall of opaque splats squarely in front of the camera.
##
## Deliberately synthetic rather than a fixture: this scene asks "did anything
## reach the presented image", and a generated cluster cannot be thinned by a
## stale import cache the way a committed .ply can (#790). Coverage is large so
## the PRESENT/ABSENT separation is not a near-threshold judgement call.
func _build_cluster() -> GaussianSplatAsset:
	var asset := GaussianSplatAsset.new()
	var grid := 5
	var count := grid * grid
	asset.set_splat_count(count)

	var positions := PackedFloat32Array()
	var colors := PackedColorArray()
	var scales := PackedFloat32Array()
	var logits := PackedFloat32Array()
	for y in range(grid):
		for x in range(grid):
			var fx := (float(x) - float(grid - 1) * 0.5) * 0.42
			var fy := (float(y) - float(grid - 1) * 0.5) * 0.42
			positions.append_array(PackedFloat32Array([fx, fy, -2.0]))
			colors.append(Color(0.85, 0.55, 0.25, 1.0))
			scales.append_array(PackedFloat32Array([0.30, 0.30, 0.30]))
			logits.append(OPAQUE_LOGIT)

	asset.set_positions(positions)
	asset.set_colors(colors)
	asset.set_scales(scales)
	asset.set_opacity_logits(logits)
	return asset


func _apply_current_config() -> void:
	var config: Dictionary = _configs[_config_index]
	ProjectSettings.set_setting(DEPTH_TEST_SETTING, config["depth_test"])
	var viewport := get_viewport()
	if viewport != null:
		viewport.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
		viewport.scaling_3d_scale = config["scale"]
	_applied = true


func _on_test_frame(delta: float):
	if _swim_phase == SwimPhase.TEMPORAL:
		_temporal_frame()
		return
	if _swim_phase == SwimPhase.DONE:
		return
	_presence_frame(delta)


func _presence_frame(_delta: float):
	if _config_index >= _configs.size():
		# Presence is finished: hand over to the temporal-stability phase rather
		# than idling until test_duration expires. The presence node is hidden
		# first -- phase 2 renders its own node in its own World3D, and leaving a
		# second resident node drawing into the main viewport every frame buys
		# nothing and costs GPU time this scene does not need.
		if splat_node != null:
			splat_node.visible = false
		_swim_phase = SwimPhase.TEMPORAL
		_swim_state = "init"
		_swim_frames = 0
		return

	if not _applied:
		_apply_current_config()
		_frames_in_config = 0
		_config_started_at = Time.get_ticks_msec() / 1000.0
		return

	_frames_in_config += 1
	# The composite path re-reads the setting per frame
	# (output_compositor.cpp:1663), but the swap still has to travel through
	# viewport resize and the sorter before the presented image settles.
	# Frames AND wall-clock: see settle_seconds for why a frame count alone is
	# not a settle condition on a fast runner.
	if _frames_in_config < settle_frames:
		return
	if (Time.get_ticks_msec() / 1000.0) - _config_started_at < settle_seconds:
		return
	if (_frames_in_config - settle_frames) % capture_stride != 0:
		return

	var image := capture_viewport()
	var config: Dictionary = _configs[_config_index]
	var content := measure_capture_content(image)
	var samples := int(content.get("sample_count", 0))
	var non_background := int(content.get("non_background_samples", 0))
	var ratio := (float(non_background) / float(samples)) if samples > 0 else 0.0
	var variance := float(content.get("luma_variance", 0.0))

	# Keep sampling until content appears or the deadline expires. Without the
	# deadline this would wait forever on a genuine disappearance -- the very
	# defect the scene exists to catch -- so an expiry RECORDS the blank frame
	# and lets the variance floor fail it, rather than retrying indefinitely.
	var elapsed := (Time.get_ticks_msec() / 1000.0) - _config_started_at
	if variance < MIN_PRESENT_LUMA_VARIANCE and elapsed < readiness_deadline_seconds:
		return

	_measurements.append({
		"name": config["name"],
		"depth_test": config["depth_test"],
		"scale": config["scale"],
		"present_ratio": ratio,
		"luma_variance": variance,
		"readiness_seconds": elapsed,
		"sample_count": samples,
	})

	_config_index += 1
	_applied = false


# ============================ PHASE 2: temporal stability (#929) ============
#
# Frame-driven state machine rather than `await`: _on_test_frame() is called
# from qa_test_base.gd's _process(), and an async body would re-enter on the
# next tick while the previous one was still suspended.


func _temporal_frame() -> void:
	_swim_frames += 1
	match _swim_state:
		"init":
			_swim_build_rig()
			_swim_goto("warm")
		"warm":
			# The splat asset has to upload and the sorter has to produce a
			# presentable frame before any reference capture means anything.
			# Bounded on BOTH sides: a minimum so a fast runner cannot sample an
			# unsettled frame, and a deadline so a genuine failure to render is
			# still recorded and fails at the discrimination gate with its real
			# reason instead of hanging here.
			if _swim_frames < SWIM_WARM_MIN_FRAMES:
				return
			if _swim_frames < SWIM_WARM_MAX_FRAMES \
					and not describe_blank_capture(_swim_capture(), "temporal rig warm-up").is_empty():
				return
			_swim_goto("ref_both")
		"ref_both":
			if _swim_frames < SWIM_REF_SETTLE:
				return
			_swim_refs["both"] = _swim_capture()
			_swim_set_visibility(false, true)
			_swim_goto("ref_mesh_only")
		"ref_mesh_only":
			if _swim_frames < SWIM_REF_SETTLE:
				return
			_swim_refs["mesh_only"] = _swim_capture()
			_swim_set_visibility(true, false)
			_swim_goto("ref_splat_only")
		"ref_splat_only":
			if _swim_frames < SWIM_REF_SETTLE:
				return
			_swim_refs["splat_only"] = _swim_capture()
			_swim_set_visibility(true, true)
			_swim_goto("ref_restore")
		"ref_restore":
			if _swim_frames < SWIM_REF_SETTLE:
				return
			if not _swim_check_discrimination():
				_swim_finish()
				return
			_swim_goto("cfg_reset")
		"cfg_reset":
			if _swim_frames == 1:
				# Always come back through a known non-temporal state: switching
				# straight from one temporal mode to another carries history
				# buffers across and the "engage" transient would be missing.
				_swim_view.use_taa = false
				_swim_view.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
				_swim_view.scaling_3d_scale = 1.0
				return
			if _swim_frames < SWIM_RESET_FRAMES:
				return
			_swim_apply_config()
			_swim_engage.clear()
			_swim_seq.clear()
			_swim_goto("engage")
		"engage":
			_swim_engage.append(_swim_capture())
			if _swim_engage.size() >= SWIM_ENGAGE_FRAMES:
				_swim_goto("settle")
		"settle":
			if _swim_frames < SWIM_SETTLE_FRAMES:
				return
			_swim_goto("seq")
		"seq":
			_swim_seq.append(_swim_capture())
			if _swim_seq.size() < SWIM_SEQ_FRAMES:
				return
			_swim_analyse_config()
			_swim_config += 1
			if _swim_config >= _swim_configs.size():
				_swim_finish()
			else:
				_swim_goto("cfg_reset")


func _swim_goto(state: String) -> void:
	_swim_state = state
	_swim_frames = 0


func _swim_finish() -> void:
	_swim_phase = SwimPhase.DONE
	_finish_test()


func _swim_set_visibility(splat_visible: bool, mesh_visible: bool) -> void:
	if _swim_splat != null:
		_swim_splat.visible = splat_visible
	if _swim_mesh != null:
		_swim_mesh.visible = mesh_visible


func _swim_capture() -> Image:
	if _swim_view == null:
		return null
	var tex := _swim_view.get_texture()
	if tex == null:
		return null
	return tex.get_image()


func _swim_region(img: Image, rect: Rect2i) -> Image:
	if img == null:
		return null
	if rect.position.x < 0 or rect.position.y < 0 \
			or rect.position.x + rect.size.x > img.get_width() \
			or rect.position.y + rect.size.y > img.get_height():
		return null
	var region := img.get_region(rect)
	# compute_image_metrics() refuses HDR formats outright (core/io/image.cpp:
	# "Metrics on HDR images are not supported") and returns INF, which would
	# reach the caller as "deltas could not be computed". Normalise once here so
	# a viewport configured for an HDR 2D target still produces a real number.
	if region != null and region.get_format() != Image.FORMAT_RGBA8:
		region.convert(Image.FORMAT_RGBA8)
	return region


## High-frequency albedo for the control mesh. Sub-pixel detail is what makes a
## temporal stage's behaviour observable at all; a flat surface would read ~0
## whether or not the stage ran.
func _swim_checker_texture() -> ImageTexture:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var on := ((x / 2) + (y / 2)) % 2 == 0
			img.set_pixel(x, y, Color(1, 1, 1, 1) if on else Color(0.05, 0.05, 0.05, 1))
	return ImageTexture.create_from_image(img)


## A deterministic high-contrast splat grid, framed into SWIM_SPLAT_RECT.
##
## Synthetic for the same reason _build_cluster() is (#790: a committed .ply can
## be thinned by a stale import cache), and checkerboarded in colour so adjacent
## blobs differ strongly -- the displacement estimator needs gradient, and a flat
## wall of one colour has almost none in its interior.
func _swim_build_cluster() -> GaussianSplatAsset:
	var asset := GaussianSplatAsset.new()
	var grid := 10
	asset.set_splat_count(grid * grid)
	var positions := PackedFloat32Array()
	var colors := PackedColorArray()
	var scales := PackedFloat32Array()
	var logits := PackedFloat32Array()
	for y in range(grid):
		for x in range(grid):
			var fx := -2.15 + (float(x) / float(grid - 1)) * 1.45
			var fy := -1.05 + (float(y) / float(grid - 1)) * 2.10
			positions.append_array(PackedFloat32Array([fx, fy, -2.0]))
			if (x + y) % 2 == 0:
				colors.append(Color(0.95, 0.62, 0.20, 1.0))
			else:
				colors.append(Color(0.06, 0.12, 0.38, 1.0))
			scales.append_array(PackedFloat32Array([0.045, 0.045, 0.045]))
			logits.append(OPAQUE_LOGIT)
	asset.set_positions(positions)
	asset.set_colors(colors)
	asset.set_scales(scales)
	asset.set_opacity_logits(logits)
	return asset


func _swim_build_rig() -> void:
	# The shipped default, NOT this project's pin. The whole point of the scene
	# is the configuration users actually run; _restore_settings() puts the pin
	# back, and the runner keeps config-mutating scenes last for exactly this.
	ProjectSettings.set_setting(DEPTH_TEST_SETTING, true)

	_swim_view = SubViewport.new()
	_swim_view.size = SWIM_VIEW_SIZE
	# Its own world, so the presence phase's node and camera in the main world
	# cannot appear here and change what the rectangles contain.
	_swim_view.own_world_3d = true
	_swim_view.transparent_bg = false
	_swim_view.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(_swim_view)

	_swim_camera = Camera3D.new()
	_swim_camera.current = true
	_swim_camera.near = 0.05
	_swim_camera.far = 200.0
	_swim_view.add_child(_swim_camera)

	_swim_splat = GaussianSplatNode3D.new()
	_swim_splat.set("quality/max_splat_count", 256)
	_swim_splat.splat_asset = _swim_build_cluster()
	_swim_view.add_child(_swim_splat)

	_swim_mesh = MeshInstance3D.new()
	var bm := BoxMesh.new()
	# Sized and placed so every rotated corner projects inside SWIM_MESH_RECT
	# with >= 30 px of margin at the default 75 deg vertical FOV: the rectangles
	# are fixed, so the framing has to have slack rather than the gate having
	# tolerance. The runtime discrimination gate proves it rather than assuming.
	bm.size = Vector3(0.8, 0.8, 0.8)
	_swim_mesh.mesh = bm
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = _swim_checker_texture()
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_swim_mesh.material_override = mat
	_swim_view.add_child(_swim_mesh)
	# Rotated off axis so it presents oblique edges, the most jitter-sensitive
	# feature there is, and placed inside SWIM_MESH_RECT.
	_swim_mesh.transform = Transform3D(
			Basis(Vector3(0.3, 1.0, 0.15).normalized(), 0.6), Vector3(1.45, 0.0, -2.0))


func _swim_apply_config() -> void:
	var cfg: Dictionary = _swim_configs[_swim_config]
	_swim_view.use_taa = bool(cfg["taa"])
	if bool(cfg["fsr2"]):
		_swim_view.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
	else:
		_swim_view.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
	_swim_view.scaling_3d_scale = float(cfg["scale"])


## Gate 2: prove the two rectangles name the two subjects.
##
## Without this, both could be pointing at background -- every delta would be
## zero, every ratio would be 0/0, and the scene would report its strongest pass
## while measuring nothing at all.
func _swim_check_discrimination() -> bool:
	var both: Image = _swim_refs.get("both")
	var mesh_only: Image = _swim_refs.get("mesh_only")
	var splat_only: Image = _swim_refs.get("splat_only")
	if both == null or mesh_only == null or splat_only == null:
		# Deliberately NOT marked as a skip: the presence phase above already
		# decides the no-RenderingDevice case, and laundering a missing capture
		# into a skip here would hide a rig that stopped producing frames.
		_swim_failures.append("temporal rig SubViewport produced no capture")
		return false

	var blank := describe_blank_capture(both, "temporal rig reference frame")
	if not blank.is_empty():
		_swim_failures.append("temporal rig drew nothing: %s" % blank)
		return false

	# Hiding the splats must move the splat rectangle and leave the mesh alone.
	var splat_rect_on_hide := _swim_region_delta(both, mesh_only, SWIM_SPLAT_RECT)
	var mesh_rect_on_hide := _swim_region_delta(both, mesh_only, SWIM_MESH_RECT)
	# Hiding the mesh must move the mesh rectangle and leave the splats alone.
	var mesh_rect_on_mesh_hide := _swim_region_delta(both, splat_only, SWIM_MESH_RECT)
	var splat_rect_on_mesh_hide := _swim_region_delta(both, splat_only, SWIM_SPLAT_RECT)

	result_metrics["swim_discrim_splat_rect_on_splat_hide"] = splat_rect_on_hide
	result_metrics["swim_discrim_mesh_rect_on_splat_hide"] = mesh_rect_on_hide
	result_metrics["swim_discrim_mesh_rect_on_mesh_hide"] = mesh_rect_on_mesh_hide
	result_metrics["swim_discrim_splat_rect_on_mesh_hide"] = splat_rect_on_mesh_hide

	var problems: Array[String] = []
	if splat_rect_on_hide < SWIM_DISCRIMINATION_MIN_LSB:
		problems.append("splat rect barely changed when the splats were hidden (%.3f < %.3f LSB): the rect does not contain the splats, or they never rendered"
				% [splat_rect_on_hide, SWIM_DISCRIMINATION_MIN_LSB])
	if mesh_rect_on_mesh_hide < SWIM_DISCRIMINATION_MIN_LSB:
		problems.append("mesh rect barely changed when the mesh was hidden (%.3f < %.3f LSB): the control never rendered"
				% [mesh_rect_on_mesh_hide, SWIM_DISCRIMINATION_MIN_LSB])
	if mesh_rect_on_hide > SWIM_CROSSTALK_MAX_LSB:
		problems.append("mesh rect moved %.3f LSB when only the splats were hidden (> %.3f): the rects overlap"
				% [mesh_rect_on_hide, SWIM_CROSSTALK_MAX_LSB])
	if splat_rect_on_mesh_hide > SWIM_CROSSTALK_MAX_LSB:
		problems.append("splat rect moved %.3f LSB when only the mesh was hidden (> %.3f): the rects overlap"
				% [splat_rect_on_mesh_hide, SWIM_CROSSTALK_MAX_LSB])
	if problems.is_empty():
		return true
	_swim_failures.append("temporal rig region discrimination failed: %s" % "; ".join(problems))
	return false


## -2.0 means "not computable", distinct from a real correlation in [-1, 1].
func _swim_finite(v: float) -> float:
	return -2.0 if (is_nan(v) or is_inf(v)) else v


func _swim_region_delta(a: Image, b: Image, rect: Rect2i) -> float:
	var ra := _swim_region(a, rect)
	var rb := _swim_region(b, rect)
	if ra == null or rb == null:
		return -1.0
	var m: Dictionary = rb.compute_image_metrics(ra, false)
	var v := float(m.get("mean", INF))
	return -1.0 if (is_inf(v) or is_nan(v)) else v


func _swim_analyse_config() -> void:
	var cfg: Dictionary = _swim_configs[_swim_config]
	var cfg_name: String = cfg["name"]

	var splat_engage: Array = []
	var mesh_engage: Array = []
	for img in _swim_engage:
		splat_engage.append(_swim_region(img, SWIM_SPLAT_RECT))
		mesh_engage.append(_swim_region(img, SWIM_MESH_RECT))
	var splat_seq: Array = []
	var mesh_seq: Array = []
	for img in _swim_seq:
		splat_seq.append(_swim_region(img, SWIM_SPLAT_RECT))
		mesh_seq.append(_swim_region(img, SWIM_MESH_RECT))

	var s_engage: Dictionary = Swim.consecutive_frame_delta(splat_engage)
	var m_engage: Dictionary = Swim.consecutive_frame_delta(mesh_engage)
	var s_seq: Dictionary = Swim.consecutive_frame_delta(splat_seq)
	var m_seq: Dictionary = Swim.consecutive_frame_delta(mesh_seq)

	var s_grids: Array = []
	var m_grids: Array = []
	for img in splat_seq:
		s_grids.append(Swim.luma_grid(img))
	for img in mesh_seq:
		m_grids.append(Swim.luma_grid(img))
	var s_disp: Dictionary = Swim.window_displacement(s_grids)
	var m_disp: Dictionary = Swim.window_displacement(m_grids)

	var entry := {
		"name": cfg_name,
		"scale": cfg["scale"],
		"taa": cfg["taa"],
		"fsr2": cfg["fsr2"],
		"phase": cfg["phase"],
		"splat_delta_mean": float(s_seq.get("mean", -1.0)),
		"splat_delta_max": float(s_seq.get("max", -1.0)),
		"mesh_delta_mean": float(m_seq.get("mean", -1.0)),
		"mesh_delta_max": float(m_seq.get("max", -1.0)),
		"splat_engage_mean": float(s_engage.get("mean", -1.0)),
		"mesh_engage_mean": float(m_engage.get("mean", -1.0)),
		"deltas_valid": bool(s_seq.get("valid", false)) and bool(m_seq.get("valid", false))
				and bool(s_engage.get("valid", false)) and bool(m_engage.get("valid", false)),
		"splat_disp": s_disp,
		"mesh_disp": m_disp,
	}
	# Periodicity diagnostic. See the header: the mesh control locks to the same
	# phase, so this can never be an oracle -- it is recorded so #929's
	# ATTRIBUTION stays reproducible from a CI artifact.
	#
	# Only computed where the window can actually support the lag.
	# `autocorrelation_at_lag()` needs at least 4 overlapping pairs, and
	# SWIM_SEQ_FRAMES = 16 yields 15 pairs, so the largest usable lag is 11.
	# That covers the fsr2 @1.0 phase (8) and not the TAA phase (16) or the
	# fsr2 @0.5 phase (32). Lengthening the window to 40 frames to cover lag 32
	# would multiply this blocking lane's capture and Lucas-Kanade cost by ~2.5x
	# for a statistic that is deliberately NOT gated -- a cost this scene declines
	# to pay. What it does NOT do is pretend: the configs where the diagnostic is
	# unavailable say so in a reason string instead of recording a number.
	var phase := int(cfg["phase"])
	var max_lag := int(s_seq.get("per_pair", PackedFloat32Array()).size()) - 4
	if phase > 0 and phase <= max_lag:
		# NAN would still be substituted, because result_metrics goes through the
		# runner's JSON.stringify and a NAN there yields a document
		# run_baseline_qa.py cannot parse.
		entry["splat_autocorr_at_phase"] = _swim_finite(Swim.autocorrelation_at_lag(
				s_seq.get("per_pair", PackedFloat32Array()), phase))
		entry["mesh_autocorr_at_phase"] = _swim_finite(Swim.autocorrelation_at_lag(
				m_seq.get("per_pair", PackedFloat32Array()), phase))
	elif phase > 0:
		entry["autocorr_unavailable"] = "jitter phase %d exceeds the largest lag a %d-pair window supports (%d)" % [
			phase, s_seq.get("per_pair", PackedFloat32Array()).size(), max(max_lag, 0)]
	_swim_results.append(entry)

	# Free the captures now rather than holding four configs' worth of RGBA8.
	_swim_engage.clear()
	_swim_seq.clear()


func _swim_evaluate() -> void:
	if _swim_results.size() != _swim_configs.size():
		_swim_failures.append("only %d of %d temporal configurations were measured"
				% [_swim_results.size(), _swim_configs.size()])
		return

	var summary: Array[String] = []
	for r in _swim_results:
		var n: String = r["name"]
		var sd: Dictionary = r["splat_disp"]
		var md: Dictionary = r["mesh_disp"]
		result_metrics["swim_%s_splat_delta_mean" % n] = r["splat_delta_mean"]
		result_metrics["swim_%s_splat_delta_max" % n] = r["splat_delta_max"]
		result_metrics["swim_%s_mesh_delta_mean" % n] = r["mesh_delta_mean"]
		result_metrics["swim_%s_mesh_delta_max" % n] = r["mesh_delta_max"]
		result_metrics["swim_%s_mesh_engage_mean" % n] = r["mesh_engage_mean"]
		result_metrics["swim_%s_splat_disp_px" % n] = sd.get("mean_px", -1.0)
		result_metrics["swim_%s_splat_disp_max_px" % n] = sd.get("max_px", -1.0)
		result_metrics["swim_%s_mesh_disp_px" % n] = md.get("mean_px", -1.0)
		result_metrics["swim_%s_mesh_disp_max_px" % n] = md.get("max_px", -1.0)
		if r.has("splat_autocorr_at_phase"):
			result_metrics["swim_%s_splat_autocorr_at_phase" % n] = r["splat_autocorr_at_phase"]
			result_metrics["swim_%s_mesh_autocorr_at_phase" % n] = r["mesh_autocorr_at_phase"]
		elif r.has("autocorr_unavailable"):
			# Recorded as the REASON, not as a number: "could not be measured" and
			# "measured and came out flat" must not read the same in the artifact.
			result_metrics["swim_%s_autocorr_unavailable" % n] = r["autocorr_unavailable"]

		if not bool(r["deltas_valid"]):
			_swim_failures.append("%s: frame deltas could not be computed" % n)
			continue
		if not bool(sd.get("measurable", false)):
			_swim_failures.append("%s: splat displacement UNMEASURABLE (%s)" % [n, sd.get("reason", "?")])
			continue
		if not bool(md.get("measurable", false)):
			_swim_failures.append("%s: mesh control displacement UNMEASURABLE (%s)" % [n, md.get("reason", "?")])
			continue

		var sp: float = sd["mean_px"]
		var mp: float = md["mean_px"]
		summary.append("%s splat=%.4fpx mesh=%.4fpx (dLSB %.3f/%.3f)" % [
			n, sp, mp, r["splat_delta_mean"], r["mesh_delta_mean"]])

		if int(r["phase"]) == 0:
			# Gate 1: rig determinism. No temporal stage means taa_jitter is
			# exactly (0,0) (renderer_scene_cull.cpp:2681-2696), so BOTH layers
			# must be bit-identical frame to frame. Anything else and every
			# number measured afterwards is confounded.
			if r["splat_delta_mean"] != 0.0 or r["mesh_delta_mean"] != 0.0 \
					or r["splat_delta_max"] != 0.0 or r["mesh_delta_max"] != 0.0:
				_swim_failures.append(
					"rig is not deterministic with no temporal stage: splat delta %.4f/%.0f, mesh delta %.4f/%.0f (both must be exactly 0)"
					% [r["splat_delta_mean"], r["splat_delta_max"], r["mesh_delta_mean"], r["mesh_delta_max"]])
			continue

		# Gate 3: the stage engaged. A viewport that silently refuses TAA or FSR2
		# leaves BOTH layers perfectly stable, which is indistinguishable from
		# perfect integration unless the control proves the stage ran (#997).
		if float(r["mesh_engage_mean"]) <= 0.0:
			_swim_failures.append(
				"%s never engaged: the mesh control did not move at all during the engage window (%.4f LSB). Stable, never-engaged and swimming must stay distinguishable."
				% [n, r["mesh_engage_mean"]])
			continue

		# The oracle. Only FSR2 un-jitters its input, so only FSR2 can expose an
		# unjittered layer as displacement; TAA's numbers are recorded and
		# reported but not gated (Godot's taa_resolve.glsl never receives the
		# jitter -- it only accumulates).
		if not bool(r["fsr2"]):
			continue
		result_metrics["swim_%s_ratio_vs_mesh" % n] = (sp / mp) if mp > 1e-9 else -1.0
		if sp > SWIM_MAX_DISPLACEMENT_PX:
			_swim_failures.append(
				"%s: the splat layer displaces %.4f px against its own window mean, over the %.3f px ceiling, while the in-frame mesh control -- same frame, same stage, drawn with the engine's jitter-corrected projection -- displaces %.4f px. The splat projection is not carrying the engine's taa_jitter (#929)."
				% [n, sp, SWIM_MAX_DISPLACEMENT_PX, mp])

	result_metrics["swim_configs_measured"] = _swim_results.size()
	result_metrics["swim_summary"] = ", ".join(summary)
	_swim_notes = summary

	# Printed on PASS as well as FAIL. The verdict is one number per config; the
	# evidence for why it is that number -- did the stage engage, did the control
	# move, was the periodicity there -- is the rest of the row, and a green run
	# that shows no rows cannot be distinguished from a green run that measured
	# nothing.
	print("[QA:%s] temporal stability, 480x270 SubViewport, depth_test=true (shipped default):" % test_name)
	print("[QA:%s]   %-9s %6s %6s | %10s %10s | %9s %9s | %8s | %s" % [
		test_name, "config", "scale", "taa", "splat px", "mesh px", "splat LSB", "mesh LSB",
		"engage", "acorr@phase S/M"])
	for r in _swim_results:
		var sd: Dictionary = r["splat_disp"]
		var md: Dictionary = r["mesh_disp"]
		print("[QA:%s]   %-9s %6.2f %6s | %10s %10s | %9.3f %9.3f | %8.3f | %s" % [
			test_name, r["name"], r["scale"], str(r["taa"]),
			("%.4f" % sd["mean_px"]) if bool(sd.get("measurable", false)) else "UNMEASURABLE",
			("%.4f" % md["mean_px"]) if bool(md.get("measurable", false)) else "UNMEASURABLE",
			r["splat_delta_mean"], r["mesh_delta_mean"], r["mesh_engage_mean"],
			("%+.3f/%+.3f" % [r["splat_autocorr_at_phase"], r["mesh_autocorr_at_phase"]]) if r.has("splat_autocorr_at_phase")
					else ("unavailable: " + String(r["autocorr_unavailable"]) if r.has("autocorr_unavailable") else "n/a (no temporal stage)"),
		])


func _on_test_complete():
	_restore_settings()
	_swim_evaluate()

	if _measurements.size() != _configs.size():
		_test_result = false
		_test_message = "%s only %d of %d composite configurations were measured" % [
			SKIP_MARKER, _measurements.size(), _configs.size()
		]
		return

	var absent: Array[String] = []
	for m in _measurements:
		result_metrics["composite_%s_luma_variance" % m["name"]] = m["luma_variance"]
		# Recorded, deliberately NOT the oracle -- it saturates at 1.0 under the
		# default environment. Kept so the baseline carries the evidence that it
		# is uninformative, rather than someone rediscovering it as a good idea.
		result_metrics["composite_%s_present_ratio" % m["name"]] = m["present_ratio"]
		if m["luma_variance"] < MIN_PRESENT_LUMA_VARIANCE:
			absent.append("%s (depth_test=%s scale=%.2f) luma_variance=%.6f" % [
				m["name"], str(m["depth_test"]), m["scale"], m["luma_variance"]
			])

	result_metrics["composite_configs_measured"] = _measurements.size()
	result_metrics["composite_configs_absent"] = absent.size()

	# Report the measured ratios on PASS as well as FAIL. A bare "all present"
	# cannot be distinguished from a scene that measured the same frame three
	# times, which is the failure this scene is most likely to have.
	var summary: Array[String] = []
	for m in _measurements:
		summary.append("%s var=%.6f" % [m["name"], m["luma_variance"]])

	if absent.is_empty():
		# Presence passed. Phase 2's verdict is independent and equally blocking:
		# splats that are PRESENT but placed at the wrong sub-pixel position are
		# still wrong, and nothing above can see that.
		if not _swim_failures.is_empty():
			_test_result = false
			_test_message = "Composite presence OK (%s) but TEMPORAL STABILITY failed: %s" % [
				", ".join(summary), " | ".join(_swim_failures)
			]
			return
		_test_result = true
		_test_message = "All %d composite configurations present (variance floor %.4f): %s; temporal stability OK: %s" % [
			_measurements.size(), MIN_PRESENT_LUMA_VARIANCE, ", ".join(summary), ", ".join(_swim_notes)
		]
		return

	_test_result = false
	# Name the shape, not just the failure: depth-true absent while
	# depth-false renders is the #921 signature and points at the composite
	# seam; everything absent is a broader rendering failure.
	var depth_true_absent := 0
	for entry in absent:
		if entry.find("depth_test=true") != -1:
			depth_true_absent += 1
	var shape := "composite depth-test path (the #921 signature)" if (
		depth_true_absent > 0 and absent.size() < _measurements.size()
	) else "all configurations, i.e. a broader rendering failure"
	_test_message = "Splats ABSENT in %d/%d configurations -- %s: %s" % [
		absent.size(), _measurements.size(), shape, "; ".join(absent)
	]


func _restore_settings() -> void:
	if _prev_depth_test != null:
		ProjectSettings.set_setting(DEPTH_TEST_SETTING, _prev_depth_test)
	var viewport := get_viewport()
	if viewport != null:
		if _prev_scaling_mode != null:
			viewport.scaling_3d_mode = _prev_scaling_mode
		if _prev_scaling_scale != null:
			viewport.scaling_3d_scale = _prev_scaling_scale
