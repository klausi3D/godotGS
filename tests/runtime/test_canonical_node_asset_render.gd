extends SceneTree

const GsRuntimeReport := preload("gs_runtime_report.gd")
const SKIP_MARKER := GsRuntimeReport.SKIP_MARKER
const FAIL_MARKER := GsRuntimeReport.FAIL_MARKER
const METRICS_MARKER := GsRuntimeReport.METRICS_MARKER

const ASSET_PATH := "res://tests/fixtures/test_splats.ply"
const MAX_RENDERER_WAIT_FRAMES := 120
# The former 240-frame cap represented roughly four seconds at 60 FPS, but
# expired much sooner on a fast runner. Keep that reference budget while making
# the proof allowance independent of frame throughput.
const PROOF_TIMEOUT_MSEC := 4_000
const MIN_VISIBLE_SPLATS := 1
const MIN_VISUAL_LUMA_VARIANCE := 0.00005
const MIN_VISUAL_LUMA_RANGE := 0.05
const MIN_NON_BACKGROUND_SAMPLES := 16
const MIN_NON_BACKGROUND_RATIO := 0.0005
const VISUAL_SAMPLE_STRIDE := 4
const BACKGROUND_LUMA_THRESHOLD := 0.03

# T3 (#891): registry name from GDS_TESTS in run_runtime_validation.py.
var _report := GsRuntimeReport.new("Canonical Node Asset Render")

var scene_root: Node3D
var splat_node: GaussianSplatNode3D
var camera: Camera3D
var renderer = null

var metrics: Dictionary = {
	"renderer_proof_kind": "canonical_node_asset",
	"renderer_proof_status": "not_started",
	"asset_path": ASSET_PATH,
	"asset_load_error": -1,
	"asset_splat_count": 0,
	"frames": 0,
	"proof_timeout_msec": PROOF_TIMEOUT_MSEC,
	"proof_elapsed_msec": 0,
	"renderer_available": false,
	"rendering_server_device_available": false,
	"visible_splats_max": 0,
	"rendered_content_probe_available": false,
	"rendered_content_seen": false,
	"stage_metrics_valid_seen": false,
	"stage_cull_status": "",
	"stage_sort_status": "",
	"stage_raster_status": "",
	"stage_composite_status": "",
	"visual_capture_count": 0,
	"visual_luma_variance_max": 0.0,
	"visual_luma_range_max": 0.0,
	"visual_non_background_samples_max": 0,
	"visual_non_background_ratio_max": 0.0,
	"visual_sample_count_max": 0,
	"visual_width": 0,
	"visual_height": 0,
	"status": "",
	"reason": "",
}


func _init() -> void:
	call_deferred("_run")


func _is_headless_runtime() -> bool:
	return OS.has_feature("headless") or DisplayServer.get_name() == "headless"


func _record_failure(reason: String) -> void:
	push_error("%s %s" % [FAIL_MARKER, reason])


func _emit_metrics(status: String, reason: String) -> void:
	metrics["status"] = status
	metrics["reason"] = reason
	print("%s %s" % [METRICS_MARKER, JSON.stringify(metrics)])


func _skip_unavailable(reason: String) -> void:
	metrics["renderer_proof_status"] = "skipped_unavailable"
	_emit_metrics("skipped", reason)
	print("%s %s" % [SKIP_MARKER, reason])
	_cleanup()
	quit(0)


func _fail(reason: String) -> void:
	metrics["renderer_proof_status"] = "failed"
	_record_failure(reason)
	_emit_metrics("failed", reason)
	_cleanup()
	quit(1)


func _pass(reason: String) -> void:
	metrics["renderer_proof_status"] = "passed"
	# T3 (#891): the three conjuncts of the terminal proof condition
	# (visible splats, visual evidence, rendered-content probe).
	_report.ok()
	_report.ok()
	_report.ok()
	_report.emit_pass()
	_emit_metrics("passed", reason)
	_cleanup()
	quit(0)


func _cleanup() -> void:
	if scene_root != null:
		scene_root.queue_free()
	scene_root = null
	splat_node = null
	camera = null
	renderer = null


func _setup_scene() -> bool:
	scene_root = Node3D.new()
	scene_root.name = "CanonicalNodeAssetRenderRoot"
	get_root().add_child(scene_root)

	var world_environment := WorldEnvironment.new()
	world_environment.name = "ProofEnvironment"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0, 1.0)
	world_environment.environment = environment
	scene_root.add_child(world_environment)

	camera = Camera3D.new()
	camera.name = "ProofCamera"
	camera.position = Vector3(0.0, 0.0, 10.0)
	camera.look_at(Vector3.ZERO, Vector3.UP)
	camera.make_current()
	scene_root.add_child(camera)

	splat_node = GaussianSplatNode3D.new()
	splat_node.name = "CanonicalAssetSplat"
	scene_root.add_child(splat_node)

	var asset := GaussianSplatAsset.new()
	var load_err := asset.load_from_file(ASSET_PATH)
	metrics["asset_load_error"] = load_err
	if load_err != OK:
		_record_failure("Failed to load canonical fixture asset %s (err=%d)" % [ASSET_PATH, load_err])
		return false
	metrics["asset_splat_count"] = asset.get_splat_count()
	splat_node.set_splat_asset(asset)
	splat_node.force_update()
	return true


func _run() -> void:
	if _is_headless_runtime():
		_skip_unavailable("Canonical node asset render proof requires a non-headless viewport.")
		return

	metrics["rendering_server_device_available"] = RenderingServer.get_rendering_device() != null
	if not _setup_scene():
		_fail("Scene setup failed for canonical node asset proof.")
		return

	for i in range(MAX_RENDERER_WAIT_FRAMES):
		await process_frame
		metrics["frames"] = i + 1
		renderer = splat_node.get_renderer()
		if renderer != null:
			metrics["renderer_available"] = true
			break

	if renderer == null:
		_skip_unavailable("Renderer unavailable for canonical node asset proof (local RenderingDevice required).")
		return

	if renderer.has_method("set_debug_pipeline_trace_enabled"):
		renderer.set_debug_pipeline_trace_enabled(true)

	var stage_failure_seen := false
	var visual_ok := false
	var proof_started_msec := Time.get_ticks_msec()
	var proof_deadline_msec := proof_started_msec + PROOF_TIMEOUT_MSEC
	var proof_frame := 0
	while Time.get_ticks_msec() < proof_deadline_msec:
		await process_frame
		if Time.get_ticks_msec() >= proof_deadline_msec:
			metrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec
			break
		proof_frame += 1
		metrics["frames"] = int(metrics.get("frames", 0)) + 1
		metrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec
		splat_node.force_update()

		var stats := _read_renderer_stats()
		_update_stage_metrics(stats)
		var visible := _read_visible_splats(stats)
		metrics["visible_splats_max"] = max(int(metrics.get("visible_splats_max", 0)), visible)
		if renderer.has_method("has_rendered_content"):
			metrics["rendered_content_probe_available"] = true
			if bool(renderer.has_rendered_content()):
				metrics["rendered_content_seen"] = true

		stage_failure_seen = stage_failure_seen or _stage_failed(stats)
		if visible >= MIN_VISIBLE_SPLATS and proof_frame >= 3:
			await RenderingServer.frame_post_draw
			if Time.get_ticks_msec() >= proof_deadline_msec:
				metrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec
				break
			metrics["proof_elapsed_msec"] = Time.get_ticks_msec() - proof_started_msec
			var image := _capture_viewport()
			if image != null:
				var visual_metrics := _compute_visual_metrics(image)
				metrics["visual_capture_count"] = int(metrics.get("visual_capture_count", 0)) + 1
				metrics["visual_width"] = int(visual_metrics.get("width", 0))
				metrics["visual_height"] = int(visual_metrics.get("height", 0))
				metrics["visual_luma_variance_max"] = max(
					float(metrics.get("visual_luma_variance_max", 0.0)),
					float(visual_metrics.get("luma_variance", 0.0))
				)
				metrics["visual_luma_range_max"] = max(
					float(metrics.get("visual_luma_range_max", 0.0)),
					float(visual_metrics.get("luma_range", 0.0))
				)
				metrics["visual_non_background_samples_max"] = max(
					int(metrics.get("visual_non_background_samples_max", 0)),
					int(visual_metrics.get("non_background_samples", 0))
				)
				metrics["visual_non_background_ratio_max"] = max(
					float(metrics.get("visual_non_background_ratio_max", 0.0)),
					float(visual_metrics.get("non_background_ratio", 0.0))
				)
				metrics["visual_sample_count_max"] = max(
					int(metrics.get("visual_sample_count_max", 0)),
					int(visual_metrics.get("sample_count", 0))
				)
				visual_ok = _visual_metrics_pass()

		if visible >= MIN_VISIBLE_SPLATS and visual_ok and _rendered_content_ok():
			if Time.get_ticks_msec() >= proof_deadline_msec:
				break
			_pass("Canonical GaussianSplatNode3D rendered fixture asset with viewport-visible evidence.")
			return

	if stage_failure_seen:
		_fail("Renderer stage failure seen before canonical node asset proof completed.")
		return
	if int(metrics.get("visible_splats_max", 0)) < MIN_VISIBLE_SPLATS:
		_fail("Canonical node asset proof never reported visible splats.")
		return
	if not _rendered_content_ok():
		_fail("Canonical node asset proof never reported rendered content.")
		return
	if not _visual_metrics_pass():
		_fail("Canonical node asset proof did not produce non-blank visual evidence.")
		return
	_fail("Canonical node asset proof exceeded its wall-clock deadline.")


func _read_renderer_stats() -> Dictionary:
	if renderer == null or not renderer.has_method("get_render_stats"):
		return {}
	var stats = renderer.get_render_stats()
	if stats is Dictionary:
		return stats
	return {}


func _read_visible_splats(stats: Dictionary) -> int:
	if splat_node != null and splat_node.has_method("get_visible_splat_count"):
		return int(splat_node.get_visible_splat_count())
	if stats.has("visible_splats"):
		return int(stats.get("visible_splats", 0))
	if stats.has("visible_after_culling"):
		return int(stats.get("visible_after_culling", 0))
	return 0


func _update_stage_metrics(stats: Dictionary) -> void:
	if stats.is_empty():
		return
	if bool(stats.get("stage_metrics_valid", false)):
		metrics["stage_metrics_valid_seen"] = true
	for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
		if stats.has(key):
			metrics[key] = str(stats.get(key, ""))


func _stage_failed(stats: Dictionary) -> bool:
	for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
		var status := str(stats.get(key, "")).to_lower()
		if status == "failed" or status == "failure":
			return true
	return false


func _capture_viewport() -> Image:
	var viewport := get_root()
	if viewport == null:
		return null
	var texture := viewport.get_texture()
	if texture == null:
		return null
	return texture.get_image()


func _compute_visual_metrics(image: Image) -> Dictionary:
	var prepared: Image = image.duplicate() as Image
	if prepared == null:
		return _empty_visual_metrics(0, 0)
	prepared.convert(Image.FORMAT_RGBA8)
	var width := prepared.get_width()
	var height := prepared.get_height()
	if width <= 0 or height <= 0:
		return _empty_visual_metrics(width, height)

	var stride: int = int(max(1, VISUAL_SAMPLE_STRIDE))
	var luma_sum := 0.0
	var luma_sq_sum := 0.0
	var sample_count := 0
	var non_background_samples := 0
	var min_luma := 1.0
	var max_luma := 0.0
	for y in range(0, height, stride):
		for x in range(0, width, stride):
			var c := prepared.get_pixel(x, y)
			var luma := 0.299 * c.r + 0.587 * c.g + 0.114 * c.b
			luma_sum += luma
			luma_sq_sum += luma * luma
			sample_count += 1
			min_luma = min(min_luma, luma)
			max_luma = max(max_luma, luma)
			if luma > BACKGROUND_LUMA_THRESHOLD:
				non_background_samples += 1

	if sample_count <= 0:
		return _empty_visual_metrics(width, height)
	var mean_luma := luma_sum / float(sample_count)
	var variance = max(0.0, (luma_sq_sum / float(sample_count)) - (mean_luma * mean_luma))
	var non_background_ratio := float(non_background_samples) / float(sample_count)
	return {
		"width": width,
		"height": height,
		"luma_variance": variance,
		"luma_range": max(0.0, max_luma - min_luma),
		"non_background_samples": non_background_samples,
		"non_background_ratio": non_background_ratio,
		"sample_count": sample_count,
	}


func _empty_visual_metrics(width: int, height: int) -> Dictionary:
	return {
		"width": width,
		"height": height,
		"luma_variance": 0.0,
		"luma_range": 0.0,
		"non_background_samples": 0,
		"non_background_ratio": 0.0,
		"sample_count": 0,
	}


func _visual_metrics_pass() -> bool:
	return (
		float(metrics.get("visual_luma_variance_max", 0.0)) >= MIN_VISUAL_LUMA_VARIANCE and
		float(metrics.get("visual_luma_range_max", 0.0)) >= MIN_VISUAL_LUMA_RANGE and
		int(metrics.get("visual_non_background_samples_max", 0)) >= MIN_NON_BACKGROUND_SAMPLES and
		float(metrics.get("visual_non_background_ratio_max", 0.0)) >= MIN_NON_BACKGROUND_RATIO
	)


func _rendered_content_ok() -> bool:
	# T3 (#891, ADR section 4.5): probe available AND content seen. The
	# pre-#891 version returned true when the probe was ABSENT, so the repo's
	# only runtime renderer-proof emitter passed exactly when it could not
	# look. A renderer without has_rendered_content() now fails this proof;
	# nightly release-ci (requires_renderer_proof) is the observing lane.
	return (
		bool(metrics.get("rendered_content_probe_available", false)) and
		bool(metrics.get("rendered_content_seen", false))
	)
