extends "res://scripts/qa_test_base.gd"
## Shared base for the world-route vs instance-route A/B QA scenes (#785).
##
## WHY THIS EXISTS AT ALL — the design it replaces was invalid by construction.
##
## qa_visual_diff.tscn and qa_sh_rotation.tscn used to put a GaussianSplatWorld3D
## and a GaussianSplatNode3D in ONE scene and toggle `visible` between captures.
## That can never work: GaussianSplatNode3D always publishes
## SUBMISSION_RESIDENCY_HINT_RESIDENT (core/gs_project_settings.h, the doc block
## on get_streaming_route_policy()), a world submission contributes zero resident
## instances, so resident_instance_contract_publisher.cpp:400 returns
## `resident_no_instances` and gaussian_splat_renderer.cpp skips the frame to
## preserve the single-route-per-frame contract. The world capture was therefore
## the clear colour, blank-vs-blank scored a perfect SSIM 1.0000, and the gate
## stayed green with one node displaced three world units.
##
## The fix is structural: ONE render route per scene, and the two scenes hand the
## comparison to each other through PNGs on disk.
##
##   role = "reference"  -> renders ONE route, proves the frame is non-blank,
##                          writes the captures plus a manifest.
##   role = "candidate"  -> renders the OTHER route, proves ITS frame is
##                          non-blank, then loads the reference captures and
##                          scores SSIM against them.
##
## Everything about the handoff is FAIL-CLOSED, because a stale or missing
## reference is exactly how this comparison goes vacuous again:
##
##   * the manifest records the producing process id, and the candidate refuses
##     any manifest that was not written by the process it is running in. A
##     leftover PNG from a previous run can never be scored.
##   * the manifest records the angle list, the viewport size and the source
##     splat count, and the candidate refuses to score when any of them differs
##     from its own — a fixture that drifts out of sync produces a NAMED failure
##     instead of a low SSIM nobody can explain.
##   * a blank capture on EITHER side is refused before any SSIM is computed, so
##     "the renderer drew nothing" can never be reported as similarity.

const ROLE_REFERENCE := "reference"
const ROLE_CANDIDATE := "candidate"

## Where the reference role parks its captures for the candidate role to read.
## user:// rather than res:// so a read-only/exported project still works and so
## the artifacts survive for a human to look at after the run.
const CAPTURE_DIR := "user://qa_route_captures"

@export var route_role: String = ROLE_REFERENCE
## Names the A/B pair. The reference and candidate scenes of one pair must agree.
@export var capture_slot: String = "route"
## Human-readable name of the render route this scene exercises ("world",
## "instance"). Only used in messages and metrics.
@export var route_label: String = "route"
@export var ssim_threshold: float = 0.95
@export var capture_delay_frames: int = 10
@export var rotation_angles: Array = [0.0]
## The single content node this scene renders. Exactly one route per scene is
## the entire point, so this is one path, not a pair.
@export var content_node_path: NodePath = NodePath("Content")
## Point the camera orbits and looks at.
##
## MUST be the centre of the fixture's content, not the world origin. With
## test_splats.ply (a sphere centred on z = -4.0) an origin-centred orbit puts
## the camera 6.6 units from the cloud at 135 deg and pushes the content into the
## frustum edge, where the two routes clip differently and the comparison stops
## being about SH rotation at all. Cross-checked between the two scenes of a pair
## via the reference manifest, so they can never disagree silently.
@export var orbit_target: Vector3 = Vector3.ZERO

var content_node: Node3D
var camera_node: Camera3D
var base_camera_offset := Vector3(0.0, 3.0, 8.0)

var _angle_index := 0
var _capture_frame_count := 0
var _finished := false
var _failure_reason := ""
var _ssim_values: Array[float] = []
var _reference_manifest: Dictionary = {}
var _captured_paths: Array[String] = []
var _source_splat_count := -1
var _viewport_size := Vector2i.ZERO


func _ready() -> void:
	super._ready()
	content_node = get_node_or_null(content_node_path) as Node3D
	camera_node = get_node_or_null("Camera3D") as Camera3D
	if camera_node != null:
		base_camera_offset = camera_node.global_position - orbit_target
		if base_camera_offset.length_squared() < 0.0001:
			base_camera_offset = Vector3(0.0, 3.0, 8.0)


func _is_headless_runtime() -> bool:
	return OS.has_feature("headless") or DisplayServer.get_name() == "headless"


## Splat count of whatever this scene renders, or -1 when it cannot be read.
##
## Read from the RESOURCE, not from a renderer statistic: the point is to compare
## the two scenes' inputs, and a renderer count is already downstream of culling
## and LOD, which legitimately differ between the two routes.
func _resolve_source_splat_count() -> int:
	if content_node == null:
		return -1
	if content_node.has_method("get_world"):
		var world = content_node.get_world()
		if world != null and world.has_method("get_splat_count"):
			return int(world.get_splat_count())
		return -1
	if content_node.has_method("get_splat_asset"):
		var asset = content_node.get_splat_asset()
		if asset != null and asset.has_method("get_splat_count"):
			return int(asset.get_splat_count())
		return -1
	return -1


func _manifest_path() -> String:
	return "%s/%s_manifest.json" % [CAPTURE_DIR, capture_slot]


func _capture_path(p_role: String, p_index: int) -> String:
	return "%s/%s_%s_%02d.png" % [CAPTURE_DIR, capture_slot, p_role, p_index]


func _on_test_start() -> void:
	if rotation_angles.is_empty():
		rotation_angles = [0.0]
	_angle_index = 0
	_capture_frame_count = 0
	_ssim_values.clear()
	_captured_paths.clear()

	# A run without a RenderingDevice cannot capture anything. Say so with the
	# shared skip token rather than reporting a failure the machine caused --
	# and note that the GPU lane passes --qa-require-capture, which turns this
	# skip back into a hard failure there. A skip is only acceptable on a lane
	# that never promised a GPU.
	if _is_headless_runtime():
		result_metrics["skipped"] = true
		_finish_skip("Requires non-headless viewport.")
		return

	if route_role != ROLE_REFERENCE and route_role != ROLE_CANDIDATE:
		_fail_now("route_role must be '%s' or '%s', got '%s'" % [
			ROLE_REFERENCE, ROLE_CANDIDATE, route_role
		])
		return

	if content_node == null:
		_fail_now("content node '%s' not found; the scene renders no route at all" % str(content_node_path))
		return

	_source_splat_count = _resolve_source_splat_count()
	result_metrics["route_label"] = route_label
	result_metrics["route_role"] = route_role
	result_metrics["source_splat_count"] = _source_splat_count
	result_metrics["angles_tested"] = rotation_angles
	if _source_splat_count <= 0:
		_fail_now("%s route reports splat_count=%d; there is nothing to render" % [
			route_label, _source_splat_count
		])
		return

	if route_role == ROLE_REFERENCE:
		var mk := DirAccess.make_dir_recursive_absolute(CAPTURE_DIR)
		if mk != OK and mk != ERR_ALREADY_EXISTS:
			_fail_now("could not create capture directory %s (err=%d)" % [CAPTURE_DIR, mk])
			return
	else:
		if not _load_reference_manifest():
			return

	_apply_angle(float(rotation_angles[_angle_index]))


func _on_test_frame(_delta: float) -> void:
	if _finished:
		return
	_capture_frame_count += 1
	if _capture_frame_count < capture_delay_frames:
		return
	_capture_frame_count = 0

	var img := capture_viewport()
	var blank := describe_blank_capture(img, "%s route capture" % route_label)
	if not blank.is_empty():
		_record_capture_metrics(img)
		_fail_now("angle %.1f: %s" % [float(rotation_angles[_angle_index]), blank])
		return
	_record_capture_metrics(img)

	if _viewport_size == Vector2i.ZERO:
		_viewport_size = img.get_size()
		# Pin WHICH render route produced this frame. These are string/bool
		# path-identity metrics, which the CI comparator compares for exact
		# equality — so a route silently degrading to a skip (the #785 defect
		# itself) fails the gate even if the pixels happened to stay similar.
		# Recorded once, from the first frame that was proven non-blank.
		append_renderer_diagnostics("", get_gs_renderer(content_node_path))

	var out_path := _capture_path(route_role, _angle_index)
	var save_err := img.save_png(out_path)
	if save_err != OK:
		_fail_now("could not write %s (err=%d)" % [out_path, save_err])
		return
	_captured_paths.append(out_path)

	if route_role == ROLE_CANDIDATE and not _score_against_reference(img):
		return

	_angle_index += 1
	if _angle_index >= rotation_angles.size():
		_complete_capture_sequence()
		return
	_apply_angle(float(rotation_angles[_angle_index]))


func _record_capture_metrics(p_img: Image) -> void:
	var m := measure_capture_content(p_img)
	var suffix := "_%02d" % _angle_index
	result_metrics["capture_luma_variance" + suffix] = float(m["luma_variance"])
	result_metrics["capture_luma_range" + suffix] = float(m["luma_range"])
	result_metrics["capture_non_background_samples" + suffix] = int(m["non_background_samples"])
	result_metrics["capture_sample_count" + suffix] = int(m["sample_count"])


func _score_against_reference(p_candidate: Image) -> bool:
	var ref_path := _capture_path(ROLE_REFERENCE, _angle_index)
	var ref_img := Image.load_from_file(ref_path)
	if ref_img == null:
		_fail_now("reference capture %s could not be loaded; refusing to score" % ref_path)
		return false

	# The reference scene already proved this frame was non-blank before writing
	# it, but re-proving it here is what makes THIS scene's pass self-contained:
	# it must never score against an image it has not checked itself.
	var blank_ref := describe_blank_capture(ref_img, "reference capture %s" % ref_path.get_file())
	if not blank_ref.is_empty():
		_fail_now("angle %.1f: %s" % [float(rotation_angles[_angle_index]), blank_ref])
		return false

	var manifest_size: Array = _reference_manifest.get("viewport_size", [])
	if manifest_size.size() == 2:
		var expected := Vector2i(int(manifest_size[0]), int(manifest_size[1]))
		if expected != p_candidate.get_size():
			_fail_now("viewport size differs: reference captured %v, this scene captured %v" % [
				expected, p_candidate.get_size()
			])
			return false

	var unscorable := describe_unscorable_pair(
		ref_img, p_candidate, "reference route", "%s route" % route_label
	)
	if not unscorable.is_empty():
		_fail_now("angle %.1f, refusing to score: %s" % [float(rotation_angles[_angle_index]), unscorable])
		return false

	var ssim := calculate_ssim(ref_img, p_candidate)
	if is_nan(ssim):
		# Never record a number here. A capture failure is not a similarity
		# score, and a fabricated one could be frozen into the committed
		# baseline, where the comparator's `current >= baseline - 0.02` rule
		# would make this gate permanently unfailable.
		_fail_now("angle %.1f, capture failure, no SSIM computed: %s" % [
			float(rotation_angles[_angle_index]),
			describe_capture_failure(ref_img, p_candidate, "reference route", "%s route" % route_label),
		])
		return false

	_ssim_values.append(ssim)
	result_metrics["ssim_%02d" % _angle_index] = ssim
	print("[QA:%s] angle %.1f SSIM=%.4f (reference=%s)" % [
		test_name, float(rotation_angles[_angle_index]), ssim, ref_path.get_file()
	])
	return true


func _complete_capture_sequence() -> void:
	if route_role == ROLE_REFERENCE:
		if not _write_reference_manifest():
			return
		_finish_ok("captured %d non-blank %s-route frame(s) at angles %s" % [
			_captured_paths.size(), route_label, str(rotation_angles)
		])
		return

	if _ssim_values.is_empty():
		_fail_now("no SSIM samples were produced")
		return

	var min_ssim: float = _ssim_values[0]
	var sum := 0.0
	for v in _ssim_values:
		min_ssim = min(min_ssim, v)
		sum += v
	result_metrics["ssim_threshold"] = ssim_threshold
	result_metrics["ssim_min"] = min_ssim
	result_metrics["ssim_avg"] = sum / float(_ssim_values.size())

	if min_ssim >= ssim_threshold:
		_finish_ok("SSIM min=%.4f avg=%.4f over %d angle(s) (threshold %.2f)" % [
			min_ssim, result_metrics["ssim_avg"], _ssim_values.size(), ssim_threshold
		])
	else:
		_finish_fail("SSIM min=%.4f avg=%.4f over %d angle(s) below threshold %.2f" % [
			min_ssim, result_metrics["ssim_avg"], _ssim_values.size(), ssim_threshold
		])


func _write_reference_manifest() -> bool:
	var payload := {
		"pid": OS.get_process_id(),
		"slot": capture_slot,
		"route_label": route_label,
		"angles": rotation_angles,
		"source_splat_count": _source_splat_count,
		"orbit_target": [orbit_target.x, orbit_target.y, orbit_target.z],
		"viewport_size": [_viewport_size.x, _viewport_size.y],
		"captures": _captured_paths,
	}
	var f := FileAccess.open(_manifest_path(), FileAccess.WRITE)
	if f == null:
		_fail_now("could not write reference manifest %s (err=%d)" % [
			_manifest_path(), FileAccess.get_open_error()
		])
		return false
	f.store_string(JSON.stringify(payload, "  "))
	f.close()
	return true


func _load_reference_manifest() -> bool:
	var path := _manifest_path()
	if not FileAccess.file_exists(path):
		_fail_now(
			"no reference manifest at %s: the '%s' reference scene did not run in this process. "
			% [path, capture_slot]
			+ "This scene scores against the OTHER render route and cannot verify anything alone."
		)
		return false
	var text := FileAccess.get_file_as_string(path)
	var parsed = JSON.parse_string(text)
	if not (parsed is Dictionary):
		_fail_now("reference manifest %s is not a JSON object" % path)
		return false
	_reference_manifest = parsed

	# Process identity is what makes a stale artifact unusable. Both scenes of a
	# pair run inside one qa_test_runner.gd process, so an equal pid means the
	# reference was produced by THIS run; anything else is a leftover file, and
	# scoring against a leftover is how a gate silently stops testing.
	var manifest_pid := int(_reference_manifest.get("pid", -1))
	if manifest_pid != OS.get_process_id():
		_fail_now(
			"reference manifest %s was written by pid %d, this run is pid %d: "
			% [path, manifest_pid, OS.get_process_id()]
			+ "refusing to score against a capture from another run."
		)
		return false

	var manifest_angles: Array = _reference_manifest.get("angles", [])
	if manifest_angles.size() != rotation_angles.size():
		_fail_now("reference captured %d angle(s), this scene tests %d" % [
			manifest_angles.size(), rotation_angles.size()
		])
		return false
	for i in rotation_angles.size():
		if abs(float(manifest_angles[i]) - float(rotation_angles[i])) > 0.001:
			_fail_now("angle %d differs: reference %.3f vs candidate %.3f" % [
				i, float(manifest_angles[i]), float(rotation_angles[i])
			])
			return false

	# Camera geometry is half of what a capture IS. Two scenes that agree on the
	# angles but orbit different points are photographing different things, and
	# the resulting SSIM would be a number about nothing.
	var manifest_target: Array = _reference_manifest.get("orbit_target", [])
	if manifest_target.size() != 3:
		_fail_now("reference manifest %s records no orbit_target" % path)
		return false
	var ref_target := Vector3(
		float(manifest_target[0]), float(manifest_target[1]), float(manifest_target[2])
	)
	if ref_target.distance_to(orbit_target) > 0.001:
		_fail_now("orbit target differs: reference %v vs candidate %v" % [ref_target, orbit_target])
		return false

	var ref_count := int(_reference_manifest.get("source_splat_count", -1))
	result_metrics["reference_source_splat_count"] = ref_count
	if ref_count != _source_splat_count:
		# THE fixture-drift guard. #785 shipped a 10-splat .gsplatworld against a
		# 10000-splat .ply and the mismatch was invisible; the two routes simply
		# scored badly with no stated cause. Now they cannot disagree in silence.
		_fail_now(
			"fixture mismatch: reference route holds %d splats, %s route holds %d. "
			% [ref_count, route_label, _source_splat_count]
			+ "Rebake the world fixture from the current PLY "
			+ "(see tests/examples/godot/test_project/tests/fixtures/README.md)."
		)
		return false
	return true


func _apply_angle(p_angle_deg: float) -> void:
	if camera_node == null:
		return
	var rot := Basis(Vector3.UP, deg_to_rad(p_angle_deg))
	camera_node.global_position = orbit_target + (rot * base_camera_offset)
	camera_node.look_at(orbit_target, Vector3.UP)


func _fail_now(p_reason: String) -> void:
	_failure_reason = p_reason
	_finish_fail(p_reason)


func _finish_skip(p_reason: String) -> void:
	if _finished:
		return
	_finished = true
	_test_result = true
	_test_message = SKIP_MARKER + " " + p_reason
	_finish_test()


func _finish_ok(p_message: String) -> void:
	if _finished:
		return
	_finished = true
	_test_result = true
	_test_message = p_message
	_finish_test()


func _finish_fail(p_message: String) -> void:
	if _finished:
		return
	_finished = true
	_test_result = false
	_test_message = p_message
	_finish_test()


## Called by the base class when the test duration elapses. Reaching here means
## the capture sequence never completed, which is a failure, not a pass: the
## default _on_test_complete() reports success, and inheriting that would make a
## scene that captured nothing look like a scene that verified everything.
func _on_test_complete() -> void:
	if _finished:
		return
	_finished = true
	_test_result = false
	if not _failure_reason.is_empty():
		_test_message = _failure_reason
	else:
		_test_message = "capture sequence did not complete: %d/%d angle(s) captured before the %.1fs budget elapsed" % [
			_angle_index, rotation_angles.size(), test_duration
		]
