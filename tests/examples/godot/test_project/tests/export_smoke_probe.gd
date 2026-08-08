extends SceneTree

## Export smoke probe (#825).
##
## Runs INSIDE an exported game binary (not the editor), launched by
## `tests/runtime/run_export_smoke.py` as:
##
##     <exported>.exe --render-thread safe --display-driver windows \
##         --rendering-driver vulkan --script res://tests/export_smoke_probe.gd
##
## Deliberately NOT `--headless`: main.cpp maps `--headless` to the dummy
## rendering driver, so there would be no RenderingDevice and every "it
## rendered" assertion below would be vacuous. This script therefore FAILS
## (it does not skip) when it finds itself on a headless display server.
##
## Every Gaussian Splatting type is reached through ClassDB by name rather than
## as a static type. That is on purpose: a stock (non-GS) export template still
## parses and runs this script, and reports the specific
## `gaussian_splatting module missing from export template` failure instead of
## dying with an unrelated parse error.
##
## `splat_node` is still annotated `Node3D` even though the GS-only methods it
## calls (`set_splat_asset`, `force_update`, `get_visible_splat_count`) are not
## on `Node3D`, and that is safe. GDScript only raises "Function not found in
## base" when the base is `self` or a BUILTIN Variant type
## (`modules/gdscript/gdscript_analyzer.cpp`, `reduce_call`); for a NATIVE
## Object-derived base it emits the `UNSAFE_METHOD_ACCESS` warning instead --
## which defaults to `IGNORE` (`gdscript_warning.h`) and is compiled out
## entirely in a `template_release` build, where `DEBUG_ENABLED` is off. Verified
## empirically: this file passes `--check-only` under a stock Godot 4.5.2 with no
## gaussian_splatting module, and the equivalent typed/dynamic pair both reach
## the `ClassDB.instantiate() == null` branch and report EXIT_MODULE_MISSING.
## Do not "fix" the annotation away on the theory that it breaks parsing.
##
## The camera orbits. With a perfectly static camera the renderer legitimately
## takes the INSTANCE.RASTER.CACHED route and reports
## `stage_raster_status="skipped" (reused cached render)`, so a still camera
## would prove nothing about the raster pass.

const FAIL_MARKER := "[EXPORT_SMOKE_FAIL]"
const METRICS_MARKER := "[EXPORT_SMOKE_METRICS]"

const ASSET_PATH := "res://tests/fixtures/synthetic_cube.ply"
const REQUIRED_CLASSES: Array[String] = ["GaussianSplatNode3D", "GaussianSplatAsset"]
const MAX_RENDERER_WAIT_FRAMES := 240
const MAX_PROOF_FRAMES := 360
const MIN_VISIBLE_SPLATS := 1
const MIN_NON_BACKGROUND_SAMPLES := 16
const MIN_VISUAL_LUMA_RANGE := 0.05
const VISUAL_SAMPLE_STRIDE := 4
const BACKGROUND_LUMA_THRESHOLD := 0.03
const ORBIT_RADIUS := 14.0
const ORBIT_HEIGHT := 2.0
const ORBIT_SPEED := 0.02

# Distinct exit codes so the Python runner can name the failure mode instead of
# reporting a generic non-zero exit.
const EXIT_OK := 0
const EXIT_GENERIC_FAILURE := 1
const EXIT_MODULE_MISSING := 2
const EXIT_NO_RENDERING_DEVICE := 3
const EXIT_NO_VISUAL_EVIDENCE := 4

var scene_root: Node3D
var splat_node: Node3D
var camera: Camera3D
var renderer = null

var metrics: Dictionary = {
	"probe": "export_smoke",
	"asset_path": ASSET_PATH,
	"display_server": "",
	"rendering_method": "",
	"rendering_device_available": false,
	"missing_classes": [],
	"asset_loaded": false,
	"asset_splat_count": 0,
	"frames": 0,
	"renderer_available": false,
	"visible_splats_max": 0,
	"sorted_splats_max": 0,
	"stage_cull_status": "",
	"stage_sort_status": "",
	"stage_raster_status": "",
	"stage_composite_status": "",
	"raster_path": "",
	"raster_executed_seen": false,
	"pipeline_evidence_ok": false,
	"visual_capture_count": 0,
	"visual_luma_range_max": 0.0,
	"visual_non_background_samples_max": 0,
	"visual_width": 0,
	"visual_height": 0,
	"visual_evidence_ok": false,
	"status": "",
	"reason": "",
}


func _init() -> void:
	call_deferred("_run")


func _emit(status: String, reason: String) -> void:
	metrics["status"] = status
	metrics["reason"] = reason
	print("%s %s" % [METRICS_MARKER, JSON.stringify(metrics)])


func _fail(reason: String, code: int) -> void:
	push_error("%s %s" % [FAIL_MARKER, reason])
	print("%s %s" % [FAIL_MARKER, reason])
	_emit("failed", reason)
	_cleanup()
	quit(code)


func _fail_visual(reason: String) -> void:
	# Everything except the on-screen pixel evidence held up. Reported under its
	# own status + exit code so the runner can tell "this build renders nothing"
	# apart from "this session has no composited window to read back from".
	push_error("%s %s" % [FAIL_MARKER, reason])
	print("%s %s" % [FAIL_MARKER, reason])
	_emit("failed_visual_evidence", reason)
	_cleanup()
	quit(EXIT_NO_VISUAL_EVIDENCE)


func _pass(reason: String) -> void:
	_emit("passed", reason)
	_cleanup()
	quit(EXIT_OK)


func _cleanup() -> void:
	if scene_root != null:
		scene_root.queue_free()
	scene_root = null
	splat_node = null
	camera = null
	renderer = null


func _missing_gs_classes() -> Array[String]:
	var missing: Array[String] = []
	for gs_class in REQUIRED_CLASSES:
		if not ClassDB.class_exists(gs_class):
			missing.append(gs_class)
	return missing


func _run() -> void:
	metrics["display_server"] = DisplayServer.get_name()
	metrics["rendering_method"] = str(ProjectSettings.get_setting("rendering/renderer/rendering_method", ""))

	# Discriminator 1: the exported binary must actually contain the module.
	# A stock Godot export template reaches this branch and exits 2.
	var missing := _missing_gs_classes()
	metrics["missing_classes"] = missing
	if not missing.is_empty():
		_fail(
			"gaussian_splatting module missing from export template (absent classes: %s). custom_template/release was probably unset or pointed at a stock template." % ", ".join(missing),
			EXIT_MODULE_MISSING
		)
		return

	# Discriminator 2: refuse to pass without a real rendering device, so a
	# headless/dummy-driver misconfiguration can never produce a green run.
	if DisplayServer.get_name() == "headless" or OS.has_feature("headless"):
		_fail("Export smoke probe ran on the headless display server; render assertions would be vacuous.", EXIT_NO_RENDERING_DEVICE)
		return
	metrics["rendering_device_available"] = RenderingServer.get_rendering_device() != null
	if not bool(metrics["rendering_device_available"]):
		_fail("No RenderingDevice in the exported binary; render assertions would be vacuous.", EXIT_NO_RENDERING_DEVICE)
		return

	if not _setup_scene():
		return

	for i in range(MAX_RENDERER_WAIT_FRAMES):
		await process_frame
		metrics["frames"] = i + 1
		_advance_camera(i)
		if splat_node.has_method("get_renderer"):
			renderer = splat_node.get_renderer()
		if renderer != null:
			metrics["renderer_available"] = true
			break

	if renderer == null:
		_fail("Exported binary never brought up a Gaussian Splatting renderer.", EXIT_GENERIC_FAILURE)
		return

	for frame in range(MAX_PROOF_FRAMES):
		await process_frame
		metrics["frames"] = int(metrics.get("frames", 0)) + 1
		_advance_camera(frame)
		if splat_node.has_method("force_update"):
			splat_node.force_update()

		var stats := _read_render_stats()
		_update_pipeline_metrics(stats)
		if _stage_failed(stats):
			_fail(
				"Render pipeline reported a stage failure in the exported binary (cull=%s sort=%s raster=%s composite=%s)." % [
					metrics["stage_cull_status"], metrics["stage_sort_status"],
					metrics["stage_raster_status"], metrics["stage_composite_status"],
				],
				EXIT_GENERIC_FAILURE
			)
			return

		if int(metrics.get("visible_splats_max", 0)) < MIN_VISIBLE_SPLATS:
			continue

		await RenderingServer.frame_post_draw
		_sample_viewport()
		if _visual_evidence_ok() and _pipeline_evidence_ok():
			metrics["visual_evidence_ok"] = true
			metrics["pipeline_evidence_ok"] = true
			_pass("Exported binary rendered %d splats with non-background viewport evidence." % int(metrics["visible_splats_max"]))
			return

	metrics["visual_evidence_ok"] = _visual_evidence_ok()
	metrics["pipeline_evidence_ok"] = _pipeline_evidence_ok()

	if int(metrics.get("visible_splats_max", 0)) < MIN_VISIBLE_SPLATS:
		_fail("Exported binary never reported a visible splat.", EXIT_GENERIC_FAILURE)
		return
	if not bool(metrics["pipeline_evidence_ok"]):
		_fail(
			"Exported binary never executed a successful raster pass (cull=%s sort=%s raster=%s composite=%s, raster_path=%s)." % [
				metrics["stage_cull_status"], metrics["stage_sort_status"],
				metrics["stage_raster_status"], metrics["stage_composite_status"],
				metrics["raster_path"],
			],
			EXIT_GENERIC_FAILURE
		)
		return
	_fail_visual(
		"Exported binary drove a successful GPU raster pass over %d splats but the window read back blank (%d captures, luma range %.5f). Needs an interactive desktop session; the in-repo Canonical Node Asset Render proof has the same requirement." % [
			int(metrics["visible_splats_max"]),
			int(metrics["visual_capture_count"]),
			float(metrics["visual_luma_range_max"]),
		]
	)


func _advance_camera(frame: int) -> void:
	if camera == null or not camera.is_inside_tree():
		return
	var angle := float(frame) * ORBIT_SPEED
	camera.position = Vector3(sin(angle) * ORBIT_RADIUS, ORBIT_HEIGHT, cos(angle) * ORBIT_RADIUS)
	camera.look_at(Vector3.ZERO, Vector3.UP)


func _setup_scene() -> bool:
	scene_root = Node3D.new()
	scene_root.name = "ExportSmokeRoot"
	get_root().add_child(scene_root)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0, 1.0)
	world_environment.environment = environment
	scene_root.add_child(world_environment)

	camera = Camera3D.new()
	camera.name = "ExportSmokeCamera"
	scene_root.add_child(camera)
	# look_at() requires the node to be inside the tree, so orient after adding.
	_advance_camera(0)
	camera.make_current()

	var asset = load(ASSET_PATH)
	if asset == null:
		_fail("Could not load %s from the exported pack (was the project imported before export?)." % ASSET_PATH, EXIT_GENERIC_FAILURE)
		return false
	metrics["asset_loaded"] = true
	if asset.has_method("get_splat_count"):
		metrics["asset_splat_count"] = int(asset.get_splat_count())
	if int(metrics["asset_splat_count"]) <= 0:
		_fail("Exported asset %s carries no splats (imported cache is empty or stale)." % ASSET_PATH, EXIT_GENERIC_FAILURE)
		return false

	splat_node = ClassDB.instantiate("GaussianSplatNode3D")
	if splat_node == null:
		_fail("ClassDB.instantiate(\"GaussianSplatNode3D\") returned null in the exported binary.", EXIT_MODULE_MISSING)
		return false
	splat_node.name = "ExportSmokeSplat"
	scene_root.add_child(splat_node)
	splat_node.set_splat_asset(asset)
	splat_node.force_update()
	return true


func _read_render_stats() -> Dictionary:
	if renderer == null or not renderer.has_method("get_render_stats"):
		return {}
	var stats = renderer.get_render_stats()
	return stats if stats is Dictionary else {}


func _update_pipeline_metrics(stats: Dictionary) -> void:
	var visible := 0
	if splat_node != null and splat_node.has_method("get_visible_splat_count"):
		visible = int(splat_node.get_visible_splat_count())
	elif stats.has("visible_splats"):
		visible = int(stats.get("visible_splats", 0))
	metrics["visible_splats_max"] = max(int(metrics.get("visible_splats_max", 0)), visible)

	if stats.is_empty():
		return
	metrics["sorted_splats_max"] = max(int(metrics.get("sorted_splats_max", 0)), int(stats.get("sorted_splats", 0)))
	for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status", "raster_path"]:
		if stats.has(key):
			metrics[key] = str(stats.get(key, ""))
	# "skipped" here means the renderer reused a cached raster output; only a
	# real "success" proves the GPU raster pass ran for this content.
	if str(stats.get("stage_raster_status", "")) == "success":
		metrics["raster_executed_seen"] = true


func _stage_failed(stats: Dictionary) -> bool:
	for key in ["stage_cull_status", "stage_sort_status", "stage_raster_status", "stage_composite_status"]:
		var status := str(stats.get(key, "")).to_lower()
		if status == "failed" or status == "failure" or status == "error":
			return true
	return false


func _pipeline_evidence_ok() -> bool:
	return (
		bool(metrics.get("raster_executed_seen", false)) and
		int(metrics.get("sorted_splats_max", 0)) >= MIN_VISIBLE_SPLATS and
		str(metrics.get("stage_cull_status", "")) == "success" and
		str(metrics.get("stage_sort_status", "")) == "success" and
		str(metrics.get("stage_composite_status", "")) == "success"
	)


func _sample_viewport() -> void:
	var viewport := get_root()
	if viewport == null:
		return
	var texture := viewport.get_texture()
	if texture == null:
		return
	var image: Image = texture.get_image()
	if image == null:
		return
	image.convert(Image.FORMAT_RGBA8)
	var width := image.get_width()
	var height := image.get_height()
	if width <= 0 or height <= 0:
		return
	metrics["visual_width"] = width
	metrics["visual_height"] = height
	metrics["visual_capture_count"] = int(metrics.get("visual_capture_count", 0)) + 1

	var stride: int = int(max(1, VISUAL_SAMPLE_STRIDE))
	var min_luma := 1.0
	var max_luma := 0.0
	var non_background := 0
	for y in range(0, height, stride):
		for x in range(0, width, stride):
			var c := image.get_pixel(x, y)
			var luma := 0.299 * c.r + 0.587 * c.g + 0.114 * c.b
			min_luma = min(min_luma, luma)
			max_luma = max(max_luma, luma)
			if luma > BACKGROUND_LUMA_THRESHOLD:
				non_background += 1

	metrics["visual_luma_range_max"] = max(
		float(metrics.get("visual_luma_range_max", 0.0)),
		max(0.0, max_luma - min_luma)
	)
	metrics["visual_non_background_samples_max"] = max(
		int(metrics.get("visual_non_background_samples_max", 0)),
		non_background
	)


func _visual_evidence_ok() -> bool:
	return (
		float(metrics.get("visual_luma_range_max", 0.0)) >= MIN_VISUAL_LUMA_RANGE and
		int(metrics.get("visual_non_background_samples_max", 0)) >= MIN_NON_BACKGROUND_SAMPLES
	)
