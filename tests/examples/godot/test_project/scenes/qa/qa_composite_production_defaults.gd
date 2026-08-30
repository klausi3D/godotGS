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


func _ready():
	test_name = "Composite Production Defaults"
	# Must comfortably exceed configs x readiness_deadline_seconds. On a BROKEN
	# build the absent configuration burns its full deadline, and a run cut short
	# would report a SKIP instead of the failure -- turning the defect this scene
	# exists to catch into a non-result.
	test_duration = 60.0
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


func _on_test_frame(_delta: float):
	if _config_index >= _configs.size():
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


func _on_test_complete():
	_restore_settings()

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
		_test_result = true
		_test_message = "All %d composite configurations present (variance floor %.4f): %s" % [
			_measurements.size(), MIN_PRESENT_LUMA_VARIANCE, ", ".join(summary)
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
