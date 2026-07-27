extends "res://scripts/qa_test_base.gd"
## Sorting Tie-Breaker Test: Ensures stable ordering when depths are equal.

@export var capture_delay_frames: int = 8
@export var capture_interval_frames: int = 4
@export var capture_samples: int = 5
@export var ssim_stability_threshold: float = 0.98

## WHY THIS SCENE RECORDS A WINNER INSTEAD OF ASSERTING ONE
##
## Stability alone cannot fail this test: both splats occupy the same position,
## so a tie-break that is reversed, ignored, or resolved by a consistent emit
## order still produces identical frames and still scores SSIM 1.0. That gap is
## real and was raised in review.
##
## The obvious fix — assert the expected winner — was tried and the premise did
## not survive contact. tile_binning.glsl packs the key as
## `(depth_quant << 8) | (global_idx & 0xFF)`, i.e. equal depths order by
## ascending index, which suggests the higher index (green) composites last and
## should dominate. The render disagrees: the centre pixel measures r=0.125
## g=0.118 — red ahead, and by only ~6% because both splats are semi-transparent
## and the centre is a BLEND rather than a winner-takes-all pixel.
##
## Rather than flip the constant until the test passes — which would assert
## whichever behaviour happens to exist while looking like a contract — the
## observed winner is recorded as a metric and pinned by the committed QA
## baseline. A reversed tie-break flips `tie_break_winner` from "red" to
## "green", and the baseline's exact-match comparison fails. That catches the
## regression under review without claiming to know which direction is
## intended. Determining the intended order is filed separately.

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

	asset.set_positions(positions)
	asset.set_colors(colors)
	asset.set_scales(scales)

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
	# The contract is in tile_binning.glsl: the depth key is
	# `(depth_quant << 8) | (global_idx & 0xFF)`, so equal depths resolve by
	# ASCENDING global index. Splat 0 is red and splat 1 is green, so the
	# higher index is composited last and green must own the centre pixel.
	var winner_image: Image = captured_images[captured_images.size() - 1]
	var center := Vector2i(winner_image.get_width() / 2, winner_image.get_height() / 2)
	var center_color := winner_image.get_pixel(center.x, center.y)
	result_metrics["center_color"] = center_color
	var winner := "green" if center_color.g > center_color.r else "red"
	result_metrics["tie_break_winner"] = winner
	result_metrics["tie_break_margin"] = absf(center_color.r - center_color.g)

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

	_test_result = min_ssim >= ssim_stability_threshold
	_test_message = "SSIM min=%.4f avg=%.4f" % [min_ssim, avg_ssim]
