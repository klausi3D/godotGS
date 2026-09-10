extends "res://scripts/qa_test_base.gd"
## Sorting Tie-Breaker Test: Ensures stable ordering when depths are equal.

@export var capture_delay_frames: int = 8
@export var capture_interval_frames: int = 4
@export var capture_samples: int = 5
@export var ssim_stability_threshold: float = 0.98

const EXPECTED_TIE_BREAK_WINNER := "red"
const OPAQUE_LOGIT := 8.0
const MINIMUM_TIE_BREAK_MARGIN := 5.0 / 255.0

## WHY RED IS THE EXPECTED WINNER
##
## Stability alone cannot fail this test: both splats occupy the same position,
## so a tie-break that is reversed, ignored, or resolved by a consistent emit
## order still produces identical frames and still scores SSIM 1.0. That gap is
## real and was raised in review.
##
## #792 traced the production 64-bit path end to end: equal depths sort by
## ascending global index, the rasterizer traverses the sorted range from index
## zero upward, and front-to-back remaining-alpha accumulation gives the first
## contributor priority. Index 0 is red, so red must win.
##
## The fixture must set opacity explicitly. set_splat_count() creates a complete
## opacity-logit lane initialized to zero, which decodes to 0.5 and takes
## precedence over Color.a. Relying on Color(..., 1.0) therefore made both
## splats semi-transparent and left only a one- or two-LSB presented-image
## margin. A high finite logit makes the pair effectively opaque without using
## infinities; the rasterizer clamps the resulting alpha to its normal 0.99
## ceiling. The corrected fixture measures eight 8-bit channel steps locally;
## require at least five so a weak but directionally correct signal fails in
## the scene itself. The committed baseline remains a second guard, while the
## scene asserts both the approved direction and a non-trivial signal directly.

var splat_node: GaussianSplatNode3D
var captured_images: Array[Image] = []
var _prev_tie_breaker = null

func _ready():
	test_name = "Sort Tie-Breaker"
	test_duration = 6.0
	warmup_frames = 5
	super._ready()

	splat_node = get_node_or_null("SplatNode")

func _on_test_start():
	_prev_tie_breaker = ProjectSettings.get_setting("rendering/gaussian_splatting/gpu_sorting/enable_tie_breaker")
	ProjectSettings.set_setting("rendering/gaussian_splatting/gpu_sorting/enable_tie_breaker", true)

	if splat_node == null:
		return

	var asset := GaussianSplatAsset.new()
	asset.set_splat_count(2)

	var positions := PackedFloat32Array([0.0, 0.0, -1.5, 0.0, 0.0, -1.5])
	var colors := PackedColorArray([Color(1.0, 0.0, 0.0, 1.0), Color(0.0, 1.0, 0.0, 1.0)])
	var scales := PackedFloat32Array([0.7, 0.7, 0.7, 0.7, 0.7, 0.7])
	var opacity_logits := PackedFloat32Array([OPAQUE_LOGIT, OPAQUE_LOGIT])

	asset.set_positions(positions)
	asset.set_colors(colors)
	asset.set_scales(scales)
	asset.set_opacity_logits(opacity_logits)

	splat_node.splat_asset = asset
	captured_images.clear()

func _on_test_frame(_delta: float):
	if frame_count < capture_delay_frames:
		return
	if captured_images.size() >= capture_samples:
		return
	if (frame_count - capture_delay_frames) % capture_interval_frames != 0:
		return

	var image = capture_viewport()
	if image != null:
		captured_images.append(image)
	if captured_images.size() >= capture_samples:
		_finish_test()

func _on_test_complete():
	if _prev_tie_breaker != null:
		ProjectSettings.set_setting("rendering/gaussian_splatting/gpu_sorting/enable_tie_breaker", _prev_tie_breaker)
	if captured_images.size() < 2:
		_test_result = false
		_test_message = "Insufficient captures"
		return

	result_metrics["ssim_threshold"] = ssim_stability_threshold

	# WHICH splat wins the tie, not just that the answer is stable.
	#
	# Stability alone cannot fail here: the two splats sit at the same position,
	# so if the tie-break is reversed, ignored, or resolved by emit order that
	# happens to be consistent, every frame is still identical and the SSIM
	# comparison still reports 1.0. A reversed tie-break would have been
	# invisible to this scene.
	#
	# #792 established that ascending global index is traversed first by the
	# front-to-back rasterizer. Index 0 is red, so record the observed direction
	# and margin and assert red directly. The committed baseline independently
	# catches a collapse in the strength of that ordering signal.
	var winner_image: Image = captured_images[captured_images.size() - 1]
	var center := Vector2i(winner_image.get_width() / 2, winner_image.get_height() / 2)
	var center_color := winner_image.get_pixel(center.x, center.y)
	result_metrics["center_color"] = center_color
	var channel_delta := center_color.g - center_color.r
	var winner := "tie"
	if channel_delta > 0.0:
		winner = "green"
	elif channel_delta < 0.0:
		winner = "red"
	result_metrics["tie_break_winner"] = winner
	var tie_break_margin := absf(channel_delta)
	result_metrics["tie_break_margin"] = tie_break_margin

	var min_ssim = 1.0
	var sum_ssim = 0.0
	var comparisons = 0

	for i in range(1, captured_images.size()):
		# Non-vacuity precondition. This scene asserts frame-to-frame
		# STABILITY, so it is the most easily faked of all: two consecutive
		# blank frames are perfectly stable and score 1.0. Prove the frames
		# hold rendered content before scoring them.
		var unscorable := describe_unscorable_pair(
			captured_images[i - 1], captured_images[i], "frame %d" % (i - 1), "frame %d" % i
		)
		if not unscorable.is_empty():
			_test_result = false
			_test_message = "No comparable render, refusing to score stability: %s" % unscorable
			return
		var ssim = calculate_ssim(captured_images[i - 1], captured_images[i])
		if is_nan(ssim):
			# One frame pair was not comparable. min()/+= would silently
			# propagate NAN into a recorded metric, so stop and report the
			# capture fault instead of a fabricated stability score.
			_test_result = false
			_test_message = "Capture failure between frames %d and %d, no SSIM computed: %s" % [
				i - 1, i, describe_capture_failure(
					captured_images[i - 1], captured_images[i], "frame %d" % (i - 1), "frame %d" % i
				)
			]
			return
		min_ssim = min(min_ssim, ssim)
		sum_ssim += ssim
		comparisons += 1

	var avg_ssim = sum_ssim / float(comparisons)
	result_metrics["ssim_min"] = min_ssim
	result_metrics["ssim_avg"] = avg_ssim

	var ordering_matches := winner == EXPECTED_TIE_BREAK_WINNER
	var margin_sufficient := tie_break_margin >= MINIMUM_TIE_BREAK_MARGIN
	_test_result = min_ssim >= ssim_stability_threshold and ordering_matches and margin_sufficient
	if not ordering_matches:
		_test_message = "Wrong equal-depth order: expected %s, observed %s (margin=%.6f)" % [
			EXPECTED_TIE_BREAK_WINNER, winner, tie_break_margin
		]
	elif not margin_sufficient:
		_test_message = "Equal-depth ordering signal too weak: margin=%.6f minimum=%.6f" % [
			tie_break_margin, MINIMUM_TIE_BREAK_MARGIN
		]
	else:
		_test_message = "SSIM min=%.4f avg=%.4f; tie-break winner=%s margin=%.6f" % [
			min_ssim, avg_ssim, winner, tie_break_margin
		]
