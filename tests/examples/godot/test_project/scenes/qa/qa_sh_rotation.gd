extends "res://scripts/qa_test_base.gd"
## SH Rotation Test: Orbits camera around world path + instance path and compares output.

@export var ssim_threshold: float = 0.95
@export var capture_delay_frames: int = 10
@export var rotation_angles: Array = [0.0, 45.0, 90.0, 135.0]

var world_path_node: Node3D
var instance_node: Node3D
var camera_node: Camera3D
var orbit_target := Vector3.ZERO
var base_camera_offset := Vector3(0.0, 3.0, 8.0)

var world_path_image: Image
var instance_image: Image
var _unscorable_reasons: Array = []

var angle_index := 0
var capture_frame_count := 0
var ssim_values: Array[float] = []

enum CapturePhase { WORLD_PATH, INSTANCE_PATH }
var capture_phase: CapturePhase = CapturePhase.WORLD_PATH

func _ready():
	test_name = "World Path vs Instance Path SH Rotation"
	test_duration = 10.0
	super._ready()

	world_path_node = get_node_or_null("WorldPath")
	instance_node = get_node_or_null("InstancePath")
	camera_node = get_node_or_null("Camera3D")
	if camera_node != null:
		base_camera_offset = camera_node.global_position - orbit_target
		if base_camera_offset.length_squared() < 0.0001:
			base_camera_offset = Vector3(0.0, 3.0, 8.0)

func _on_test_start():
	if rotation_angles.is_empty():
		rotation_angles = [0.0]
	angle_index = 0
	ssim_values.clear()
	_apply_angle(rotation_angles[angle_index])
	capture_phase = CapturePhase.WORLD_PATH
	capture_frame_count = 0
	_set_visibility(true, false)

func _on_test_frame(_delta: float):
	capture_frame_count += 1

	if capture_phase == CapturePhase.WORLD_PATH and capture_frame_count >= capture_delay_frames:
		world_path_image = capture_viewport()
		_set_visibility(false, true)
		capture_phase = CapturePhase.INSTANCE_PATH
		capture_frame_count = 0
		return

	if capture_phase == CapturePhase.INSTANCE_PATH and capture_frame_count >= capture_delay_frames:
		instance_image = capture_viewport()
		# Non-vacuity precondition: a blank pair scores 1.0, so record NAN
		# (which _on_test_complete refuses to average) rather than a score
		# that would certify a frame nothing was drawn into.
		var unscorable := describe_unscorable_pair(
			world_path_image, instance_image, "world path", "instance path"
		)
		if not unscorable.is_empty():
			_unscorable_reasons.append(
				"angle %.1f: %s" % [float(rotation_angles[angle_index]), unscorable]
			)
			ssim_values.append(NAN)
		else:
			ssim_values.append(calculate_ssim(world_path_image, instance_image))
		angle_index += 1
		if angle_index >= rotation_angles.size():
			_finish_test()
			return
		_apply_angle(rotation_angles[angle_index])
		_set_visibility(true, false)
		capture_phase = CapturePhase.WORLD_PATH
		capture_frame_count = 0

func _on_test_complete():
	if ssim_values.is_empty():
		_test_result = false
		_test_message = "No SSIM samples captured"
		return

	result_metrics["ssim_threshold"] = ssim_threshold
	result_metrics["angles_tested"] = rotation_angles

	# A NAN anywhere means at least one angle never produced a comparable pair.
	# Averaging it away, or letting min() pick it, would turn a capture failure
	# into a number — so refuse to report any SSIM for this run.
	var failed_angles: Array = []
	for i in ssim_values.size():
		if is_nan(ssim_values[i]):
			var angle: float = rotation_angles[i] if i < rotation_angles.size() else -1.0
			failed_angles.append(angle)
	if not failed_angles.is_empty():
		_test_result = false
		var detail := "; ".join(_unscorable_reasons) if not _unscorable_reasons.is_empty() else "capture failure"
		_test_message = "No comparable render at angle(s) %s, refusing to score: %s" % [
			str(failed_angles), detail
		]
		return

	var min_ssim = ssim_values[0]
	var sum = 0.0
	for v in ssim_values:
		min_ssim = min(min_ssim, v)
		sum += v
	var avg_ssim = sum / float(ssim_values.size())

	result_metrics["ssim_min"] = min_ssim
	result_metrics["ssim_avg"] = avg_ssim

	_test_result = min_ssim >= ssim_threshold
	_test_message = "SSIM min=%.4f avg=%.4f (threshold %.2f)" % [min_ssim, avg_ssim, ssim_threshold]

func _apply_angle(angle_deg: float) -> void:
	if camera_node == null:
		return
	var rot = Basis(Vector3.UP, deg_to_rad(angle_deg))
	camera_node.global_position = orbit_target + (rot * base_camera_offset)
	camera_node.look_at(orbit_target, Vector3.UP)

func _set_visibility(world_path_visible: bool, instance_path_visible: bool) -> void:
	if world_path_node != null:
		world_path_node.visible = world_path_visible
	if instance_node != null:
		instance_node.visible = instance_path_visible
