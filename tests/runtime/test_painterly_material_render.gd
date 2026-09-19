extends SceneTree

# Painterly Material Render (#997, #851)
#
# WHY THIS SCENARIO EXISTS
#
# Every painterly test this repo shipped before it was vacuous. They all
# asserted `visible_splats > 0`, which is identical on the baseline raster, and
# none of them ever supplied a PainterlyMaterial -- so RasterStage rejected the
# painterly path with RenderFallbackReason::PAINTERLY_MATERIAL_UNAVAILABLE
# (modules/gaussian_splatting/renderer/render_pipeline_stages.cpp:2561-2566),
# rendered the baseline, and the tests passed. A painterly defect could not
# turn any of them red.
#
# This scenario is built so that it CANNOT pass on a baseline frame. It proves
# three things in one run, and each one is an independent way for the probe to
# be caught lying:
#
#   1. DISCRIMINATION. The same `stage_raster_painterly` probe
#      (StageMetrics::raster.painterly_active, published at
#      modules/gaussian_splatting/renderer/render_diagnostics_orchestrator.cpp:520)
#      must report false for painterly-off AND for painterly-on-without-a-
#      material, and true only for painterly-on-with-a-material. A probe stuck
#      on one value fails the run.
#
#      It is read INSTEAD of the `raster_path` string on purpose. Measured on a
#      real scan at base: `raster_path` is "compute" on the baseline raster and
#      "cached" whenever the render cache serves the frame -- it reads
#      "painterly" only on a frame that actually re-rastered, so asserting the
#      string would fail on a correct painterly frame that got cached. The
#      boolean survives cache reuse, and because it survives it can also be
#      STALE, so it is never trusted alone: the pixel delta in check 3 has to
#      agree with it.
#   2. THE PROPERTY IS BOUND. `painterly/material` is set through the NODE
#      property -- the same way the shipped demo scenes author it -- and the
#      value must round-trip back off the node and reach the renderer. Godot
#      discards a write to an unbound property silently, so the round-trip is
#      the only thing that can see the defect.
#   3. THE IMAGE ACTUALLY CHANGES. A painterly capture must differ from the
#      baseline capture by more than MIN_PAINTERLY_DELTA_RATIO of the covered
#      pixels. Binding the property but producing a bit-identical frame is the
#      failure mode that "painterly ran" alone would not catch.
#
# Mutation contract: reverting the `painterly/material` binding in
# modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp turns check 2 red
# first, then 1 and 3. Reverting only the renderer push in
# gaussian_splat_node_helpers.cpp leaves 2 green and turns 1 and 3 red.

const GsRuntimeReport := preload("gs_runtime_report.gd")
const SKIP_MARKER := GsRuntimeReport.SKIP_MARKER
const FAIL_MARKER := GsRuntimeReport.FAIL_MARKER
const METRICS_MARKER := GsRuntimeReport.METRICS_MARKER

const ASSET_PATH := "res://tests/fixtures/test_splats.ply"
const MATERIAL_PATH := "res://tests/fixtures/painterly_reference_material.tres"

const MAX_RENDERER_WAIT_FRAMES := 120
# Frames to settle after every path switch. Toggling painterly changes the
# shader permutation set and invalidates the render cache, so the first frames
# after a switch can still carry the previous path's output.
const SETTLE_FRAMES := 12
const PHASE_TIMEOUT_MSEC := 8_000

const MIN_VISIBLE_SPLATS := 1
# Splats must cover at least this fraction of the frame for a pixel comparison
# to mean anything. Below it the capture is effectively background and any
# "no difference" result would be vacuous.
const MIN_COVERAGE_RATIO := 0.005
# Painterly must move at least this fraction of the covered pixels. #999
# measured a ~2% silhouette disagreement plus a 0.88 luminance ratio on a real
# scan, so a correct painterly frame moves far more than this; the floor is set
# low only so the check fails on "identical", not on "differently tuned".
const MIN_PAINTERLY_DELTA_RATIO := 0.02
# Per-channel 8-bit tolerance. 1 LSB of dither must not count as a difference.
const CHANNEL_DELTA_TOLERANCE := 2
const SAMPLE_STRIDE := 2
const BACKGROUND_DELTA_TOLERANCE := 2
const VIEWPORT_WIDTH := 960
const VIEWPORT_HEIGHT := 540
# #1018 wind animation. The control floor only has to prove animation is
# observable at all in this run; the real assertion is the painterly/baseline
# ratio below it. Measured at the base of this stack on a real scan: painterly
# 989 px vs baseline 342,792 px = ratio 0.0029, so the 0.10 floor fails the
# frozen clock with two orders of magnitude to spare, while the fixed path
# measured 335,356 vs 357,345 = ratio 0.94.
const MIN_WIND_CONTROL_DIFFER_PX := 200
const MIN_WIND_PAINTERLY_RATIO := 0.10

var _report := GsRuntimeReport.new("Painterly Material Render")

var sub_viewport: SubViewport
var scene_root: Node3D
var splat_node: GaussianSplatNode3D
var camera: Camera3D
var renderer = null
var painterly_material: Resource = null

var metrics: Dictionary = {
	"painterly_proof_kind": "painterly_material_render",
	"painterly_proof_status": "not_started",
	"asset_path": ASSET_PATH,
	"material_path": MATERIAL_PATH,
	"asset_load_error": -1,
	"material_load_ok": false,
	"asset_splat_count": 0,
	"renderer_available": false,
	"node_property_bound": false,
	"node_property_roundtrip": false,
	"renderer_material_valid": false,
	"raster_path_painterly_off": "",
	"raster_path_no_material": "",
	"raster_path_painterly_on": "",
	"painterly_active_painterly_off": true,
	"painterly_active_no_material": true,
	"painterly_active_painterly_on": false,
	"stage_raster_reason_no_material": "",
	"stage_raster_reason_painterly_on": "",
	"visible_splats_painterly_off": 0,
	"visible_splats_painterly_on": 0,
	"sample_count": 0,
	"covered_px_painterly_off": 0,
	"covered_px_painterly_on": 0,
	"coverage_ratio_painterly_off": 0.0,
	"coverage_ratio_painterly_on": 0.0,
	"delta_px_painterly_vs_baseline": 0,
	"delta_ratio_painterly_vs_baseline": 0.0,
	"mean_luma_painterly_off": 0.0,
	"mean_luma_painterly_on": 0.0,
	"painterly_over_standard_luma": 0.0,
	"capture_width": 0,
	"capture_height": 0,
	# Fail-safe defaults: an unset wind measurement must not read as a pass.
	"wind_baseline_frames_differ_px": -1,
	"wind_painterly_frames_differ_px": -1,
	"wind_painterly_over_baseline_ratio": 0.0,
	"renderer_route_valid_immediately": false,
	"renderer_route_valid_after_frames": false,
	"renderer_route_painterly_active": false,
	"renderer_route_stage_raster_reason": "",
	"status": "",
	"reason": "",
}


func _init() -> void:
	call_deferred("_run")


func _is_headless_runtime() -> bool:
	return OS.has_feature("headless") or DisplayServer.get_name() == "headless"


func _emit_metrics(status: String, reason: String) -> void:
	metrics["status"] = status
	metrics["reason"] = reason
	print("%s %s" % [METRICS_MARKER, JSON.stringify(metrics)])


func _skip_unavailable(reason: String) -> void:
	metrics["painterly_proof_status"] = "skipped_unavailable"
	_emit_metrics("skipped", reason)
	print("%s %s" % [SKIP_MARKER, reason])
	_cleanup()
	quit(0)


func _fail(reason: String) -> void:
	metrics["painterly_proof_status"] = "failed"
	push_error("%s %s" % [FAIL_MARKER, reason])
	_emit_metrics("failed", reason)
	_cleanup()
	quit(1)


func _pass(reason: String) -> void:
	metrics["painterly_proof_status"] = "passed"
	_report.emit_pass()
	_emit_metrics("passed", reason)
	_cleanup()
	quit(0)


func _cleanup() -> void:
	if sub_viewport != null:
		sub_viewport.queue_free()
	sub_viewport = null
	scene_root = null
	splat_node = null
	camera = null
	renderer = null
	painterly_material = null


func _setup_scene() -> bool:
	# Render into an owned SubViewport of a KNOWN size rather than into the host
	# window. Measured on the GPU lane: reading get_root().get_texture() there
	# produced four byte-identical captures across painterly-off, painterly-on
	# and splats-hidden while `stage_raster_painterly` correctly reported the
	# path changing -- the window read simply does not carry this content in a
	# --script SceneTree run. A SubViewport with own_world_3d is the same surface
	# PR #999 measured painterly on, and its texture read is deterministic.
	sub_viewport = SubViewport.new()
	sub_viewport.name = "PainterlyProofViewport"
	sub_viewport.size = Vector2i(VIEWPORT_WIDTH, VIEWPORT_HEIGHT)
	sub_viewport.own_world_3d = true
	sub_viewport.transparent_bg = false
	sub_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	get_root().add_child(sub_viewport)

	scene_root = Node3D.new()
	scene_root.name = "PainterlyMaterialRenderRoot"
	sub_viewport.add_child(scene_root)

	var world_environment := WorldEnvironment.new()
	world_environment.name = "PainterlyProofEnvironment"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0, 0.0, 0.0, 1.0)
	world_environment.environment = environment
	scene_root.add_child(world_environment)

	camera = Camera3D.new()
	camera.name = "PainterlyProofCamera"
	scene_root.add_child(camera)
	# Pinned pose, not fitted to the asset's AABB: a real scan's AABB is
	# dominated by outlier splats, and a fitted camera makes two runs
	# incomparable the moment the fixture changes.
	camera.position = Vector3(0.0, 0.0, 10.0)
	camera.rotation = Vector3.ZERO
	camera.near = 0.05
	camera.far = 4000.0
	camera.make_current()

	splat_node = GaussianSplatNode3D.new()
	splat_node.name = "PainterlyProofSplat"
	scene_root.add_child(splat_node)

	var asset := GaussianSplatAsset.new()
	var load_err := asset.load_from_file(ASSET_PATH)
	metrics["asset_load_error"] = load_err
	if load_err != OK:
		return false
	metrics["asset_splat_count"] = asset.get_splat_count()
	splat_node.set_splat_asset(asset)
	splat_node.force_update()
	return true


func _load_material() -> bool:
	if not ResourceLoader.exists(MATERIAL_PATH):
		return false
	painterly_material = load(MATERIAL_PATH)
	metrics["material_load_ok"] = painterly_material != null
	return painterly_material != null


# The bound-property check. `Object.set()` on an unbound property is silent in
# Godot, so "did the write land" can only be answered by reading it back and by
# confirming the renderer received it -- which is the whole of #997's defect.
func _assign_material_through_node_property() -> bool:
	var property_names: Array = []
	for entry in splat_node.get_property_list():
		property_names.append(str(entry.get("name", "")))
	metrics["node_property_bound"] = property_names.has("painterly/material")

	splat_node.set("painterly/material", painterly_material)
	var read_back = splat_node.get("painterly/material")
	metrics["node_property_roundtrip"] = read_back == painterly_material
	return bool(metrics["node_property_bound"]) and bool(metrics["node_property_roundtrip"])


func _read_renderer_stats() -> Dictionary:
	if renderer == null or not renderer.has_method("get_render_stats"):
		return {}
	var stats = renderer.get_render_stats()
	if stats is Dictionary:
		return stats
	return {}


func _read_visible_splats() -> int:
	if splat_node != null and splat_node.has_method("get_visible_splat_count"):
		return int(splat_node.get_visible_splat_count())
	return 0


func _capture_viewport() -> Image:
	var viewport: Viewport = sub_viewport
	if viewport == null:
		return null
	var texture := viewport.get_texture()
	if texture == null:
		return null
	var image := texture.get_image()
	if image == null:
		return null
	var prepared: Image = image.duplicate() as Image
	if prepared == null:
		return null
	prepared.convert(Image.FORMAT_RGBA8)
	return prepared


# Settle the pipeline, then capture one frame and the stats that produced it.
#
# `expect_painterly` makes the settle CONVERGE rather than count. The first
# painterly frame compiles shader permutations, and this machine is also the CI
# runner, so a fixed frame budget can expire mid-compile and capture a
# pre-painterly frame -- a flaky fail-closed, which is still a fail. When the
# caller knows which path the frame should be on, keep settling until
# `stage_raster_painterly` says so, bounded by PHASE_TIMEOUT_MSEC. Passing the
# expectation does NOT weaken the assertion: on timeout it returns whatever the
# pipeline actually reported, and the caller's own check still fails.
# `EXPECT_ANY` keeps the old count-only behaviour for phases that are measuring
# rather than waiting.
const EXPECT_ANY := 0
const EXPECT_PAINTERLY := 1
const EXPECT_BASELINE := 2

func _settle_and_capture(expect_painterly: int = EXPECT_ANY) -> Dictionary:
	var deadline := Time.get_ticks_msec() + PHASE_TIMEOUT_MSEC
	var frames := 0
	while Time.get_ticks_msec() < deadline:
		await process_frame
		if splat_node != null:
			splat_node.force_update()
		frames += 1
		if frames < SETTLE_FRAMES:
			continue
		if expect_painterly == EXPECT_ANY:
			break
		var probe := _read_renderer_stats()
		if not probe.has("stage_raster_painterly"):
			break
		var active := bool(probe.get("stage_raster_painterly", false))
		if (expect_painterly == EXPECT_PAINTERLY) == active:
			break
	await RenderingServer.frame_post_draw
	var stats := _read_renderer_stats()
	# Default the painterly flag to TRUE when the key is absent: a missing probe
	# must fail the discrimination phases rather than silently satisfy them.
	return {
		"image": _capture_viewport(),
		"raster_path": str(stats.get("raster_path", "")),
		"painterly_active": bool(stats.get("stage_raster_painterly", true)),
		"stage_raster_reason": str(stats.get("stage_raster_reason", "")),
		"has_painterly_key": stats.has("stage_raster_painterly"),
	}


func _luma(c: Color) -> float:
	return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b


func _channel_delta(a: Color, b: Color) -> int:
	var dr: int = int(abs(a.r - b.r) * 255.0 + 0.5)
	var dg: int = int(abs(a.g - b.g) * 255.0 + 0.5)
	var db: int = int(abs(a.b - b.b) * 255.0 + 0.5)
	return int(max(dr, max(dg, db)))


# Coverage = pixels differing from the splats-hidden reference, the same
# definition #999 used, so the numbers in the two PRs are comparable.
func _compare(reference: Image, baseline: Image, painterly: Image) -> Dictionary:
	var width := reference.get_width()
	var height := reference.get_height()
	var samples := 0
	var covered_baseline := 0
	var covered_painterly := 0
	var delta_px := 0
	var luma_sum_baseline := 0.0
	var luma_sum_painterly := 0.0
	var covered_luma_sum_baseline := 0.0
	var covered_luma_sum_painterly := 0.0
	for y in range(0, height, SAMPLE_STRIDE):
		for x in range(0, width, SAMPLE_STRIDE):
			var ref_c := reference.get_pixel(x, y)
			var base_c := baseline.get_pixel(x, y)
			var paint_c := painterly.get_pixel(x, y)
			samples += 1
			luma_sum_baseline += _luma(base_c)
			luma_sum_painterly += _luma(paint_c)
			var base_covered := _channel_delta(ref_c, base_c) > BACKGROUND_DELTA_TOLERANCE
			var paint_covered := _channel_delta(ref_c, paint_c) > BACKGROUND_DELTA_TOLERANCE
			if base_covered:
				covered_baseline += 1
				covered_luma_sum_baseline += _luma(base_c)
			if paint_covered:
				covered_painterly += 1
				covered_luma_sum_painterly += _luma(paint_c)
			if (base_covered or paint_covered) and _channel_delta(base_c, paint_c) > CHANNEL_DELTA_TOLERANCE:
				delta_px += 1
	var covered_union: int = int(max(covered_baseline, covered_painterly))
	return {
		"samples": samples,
		"covered_baseline": covered_baseline,
		"covered_painterly": covered_painterly,
		"delta_px": delta_px,
		"delta_ratio": (float(delta_px) / float(covered_union)) if covered_union > 0 else 0.0,
		"mean_luma_baseline": (covered_luma_sum_baseline / float(covered_baseline)) if covered_baseline > 0 else 0.0,
		"mean_luma_painterly": (covered_luma_sum_painterly / float(covered_painterly)) if covered_painterly > 0 else 0.0,
		"frame_luma_baseline": luma_sum_baseline / float(max(samples, 1)),
		"frame_luma_painterly": luma_sum_painterly / float(max(samples, 1)),
	}


func _run() -> void:
	if _is_headless_runtime():
		_skip_unavailable("Painterly material render proof requires a non-headless viewport.")
		return

	if not _load_material():
		# Name the likely cause rather than letting a missing fixture read as a
		# painterly defect. The .tres is committed at the REPOSITORY root only;
		# run_runtime_validation.py also accepts --project-path
		# tests/examples/godot/test_project, and under that root this res:// path
		# does not resolve. Unlike the .ply fixtures, a .tres is not staged into
		# both roots by prepare_synthetic_assets.py.
		_fail(
			"Painterly reference material %s could not be loaded. " % MATERIAL_PATH
			+ "It is committed under the repository root only; if this run used "
			+ "--project-path tests/examples/godot/test_project the fixture is not "
			+ "staged there. This is a rig problem, not a painterly defect."
		)
		return
	_report.ok()

	if not _setup_scene():
		_fail("Scene setup failed (asset %s load error %d)." % [ASSET_PATH, int(metrics["asset_load_error"])])
		return
	_report.ok()

	for i in range(MAX_RENDERER_WAIT_FRAMES):
		await process_frame
		renderer = splat_node.get_renderer()
		if renderer != null:
			metrics["renderer_available"] = true
			break
	if renderer == null:
		_skip_unavailable("Renderer unavailable for painterly material proof (local RenderingDevice required).")
		return
	if renderer.has_method("set_debug_pipeline_trace_enabled"):
		renderer.set_debug_pipeline_trace_enabled(true)

	# Phase 0 -- splats hidden. The coverage reference.
	splat_node.visible = false
	var hidden: Dictionary = await _settle_and_capture()
	if not bool(hidden.get("has_painterly_key", false)):
		_fail("get_render_stats() has no 'stage_raster_painterly' key; the painterly-path probe this scenario depends on is gone.")
		return
	_report.ok()
	var reference_image: Image = hidden.get("image")
	if reference_image == null:
		_skip_unavailable("Viewport capture unavailable; painterly proof needs a readable viewport texture.")
		return
	metrics["capture_width"] = reference_image.get_width()
	metrics["capture_height"] = reference_image.get_height()
	splat_node.visible = true

	# Phase A -- painterly off. Baseline raster.
	splat_node.set_enable_painterly(false)
	var off: Dictionary = await _settle_and_capture(EXPECT_BASELINE)
	metrics["raster_path_painterly_off"] = str(off.get("raster_path", ""))
	metrics["painterly_active_painterly_off"] = bool(off.get("painterly_active", true))
	metrics["visible_splats_painterly_off"] = _read_visible_splats()
	var baseline_image: Image = off.get("image")
	if baseline_image == null:
		_fail("Painterly-off capture returned no image.")
		return
	if bool(metrics["painterly_active_painterly_off"]):
		_fail("Painterly-off frame reported the painterly path active (raster_path=%s)." % metrics["raster_path_painterly_off"])
		return
	_report.ok()

	# Phase B -- painterly on, still no material. This is exactly the state
	# every pre-#997 painterly test ran in, and it MUST report the baseline.
	# It is the discrimination half of the proof: if the probe reported
	# "painterly" here, its "painterly" in phase C would mean nothing.
	splat_node.set_enable_painterly(true)
	var no_material: Dictionary = await _settle_and_capture(EXPECT_BASELINE)
	metrics["raster_path_no_material"] = str(no_material.get("raster_path", ""))
	metrics["painterly_active_no_material"] = bool(no_material.get("painterly_active", true))
	metrics["stage_raster_reason_no_material"] = str(no_material.get("stage_raster_reason", ""))
	if bool(metrics["painterly_active_no_material"]):
		_fail(
			"Painterly-on-without-material reported the painterly path active; the documented fallback is the baseline raster (reason='%s')."
			% metrics["stage_raster_reason_no_material"]
		)
		return
	_report.ok()

	# Phase C -- painterly on WITH a material, assigned through the node
	# property the shipped demo scenes author.
	if not _assign_material_through_node_property():
		_fail(
			"GaussianSplatNode3D does not accept 'painterly/material' (bound=%s roundtrip=%s). "
			% [metrics["node_property_bound"], metrics["node_property_roundtrip"]]
			+ "Godot discards the write silently, so every authored painterly material is dropped at scene load (#997)."
		)
		return
	_report.ok()

	if renderer.has_method("get_painterly_material"):
		var renderer_material = renderer.get_painterly_material()
		metrics["renderer_material_valid"] = renderer_material != null
		if renderer_material == null:
			_fail("The node accepted 'painterly/material' but never pushed it to the shared GaussianSplatRenderer.")
			return
		_report.ok()

	var on: Dictionary = await _settle_and_capture(EXPECT_PAINTERLY)
	metrics["raster_path_painterly_on"] = str(on.get("raster_path", ""))
	# The probe key is re-checked here, not only in phase 0. `painterly_active`
	# defaults to true when the key is missing, which is the fail-closed default
	# for the two discrimination phases but would be fail-OPEN for this one: the
	# central "painterly actually ran" assertion would then be satisfied by a
	# literal rather than by a measurement.
	if not bool(on.get("has_painterly_key", false)):
		_fail("get_render_stats() stopped publishing 'stage_raster_painterly' before the painterly phase; the proof has no probe.")
		return
	_report.ok()
	metrics["painterly_active_painterly_on"] = bool(on.get("painterly_active", false))
	metrics["stage_raster_reason_painterly_on"] = str(on.get("stage_raster_reason", ""))
	metrics["visible_splats_painterly_on"] = _read_visible_splats()
	var painterly_image: Image = on.get("image")
	if painterly_image == null:
		_fail("Painterly-on capture returned no image.")
		return
	if not bool(metrics["painterly_active_painterly_on"]):
		_fail(
			"Painterly-on frame with a valid material still ran the baseline raster (raster_path=%s, stage_raster_reason='%s'). "
			% [metrics["raster_path_painterly_on"], metrics["stage_raster_reason_painterly_on"]]
			+ "Nothing about painterly was tested."
		)
		return
	_report.ok()

	if int(metrics["visible_splats_painterly_on"]) < MIN_VISIBLE_SPLATS:
		_fail("Painterly frame reported %d visible splats." % int(metrics["visible_splats_painterly_on"]))
		return
	_report.ok()

	var cmp: Dictionary = _compare(reference_image, baseline_image, painterly_image)
	metrics["sample_count"] = int(cmp["samples"])
	metrics["covered_px_painterly_off"] = int(cmp["covered_baseline"])
	metrics["covered_px_painterly_on"] = int(cmp["covered_painterly"])
	metrics["coverage_ratio_painterly_off"] = float(cmp["covered_baseline"]) / float(max(int(cmp["samples"]), 1))
	metrics["coverage_ratio_painterly_on"] = float(cmp["covered_painterly"]) / float(max(int(cmp["samples"]), 1))
	metrics["delta_px_painterly_vs_baseline"] = int(cmp["delta_px"])
	metrics["delta_ratio_painterly_vs_baseline"] = float(cmp["delta_ratio"])
	metrics["mean_luma_painterly_off"] = float(cmp["mean_luma_baseline"])
	metrics["mean_luma_painterly_on"] = float(cmp["mean_luma_painterly"])
	if float(cmp["mean_luma_baseline"]) > 0.0:
		metrics["painterly_over_standard_luma"] = float(cmp["mean_luma_painterly"]) / float(cmp["mean_luma_baseline"])

	# A "no difference" verdict is only meaningful if the splats were on screen
	# at all. Without this the whole comparison could pass vacuously on an empty
	# frame -- the exact shape of defect this scenario exists to kill.
	if float(metrics["coverage_ratio_painterly_off"]) < MIN_COVERAGE_RATIO:
		_fail(
			"Baseline capture covered only %.4f of sampled pixels (floor %.4f); the pixel comparison would be vacuous."
			% [float(metrics["coverage_ratio_painterly_off"]), MIN_COVERAGE_RATIO]
		)
		return
	_report.ok()

	if float(metrics["delta_ratio_painterly_vs_baseline"]) < MIN_PAINTERLY_DELTA_RATIO:
		_fail(
			"Painterly and baseline frames differ on only %.4f of covered pixels (floor %.4f, %d px). "
			% [
				float(metrics["delta_ratio_painterly_vs_baseline"]),
				MIN_PAINTERLY_DELTA_RATIO,
				int(metrics["delta_px_painterly_vs_baseline"]),
			]
			+ "raster_path said 'painterly' but the image is the baseline's."
		)
		return
	_report.ok()

	# Phase D -- #1018. Does painterly's wind clock advance?
	#
	# This is measured RELATIVE TO THE BASELINE PATH, not against a fixed floor.
	# Measured on a real scan at the base of this stack, painterly moved 989 of
	# 518,400 pixels between two time-separated frames while the baseline moved
	# 342,792 -- so "did any pixel change?" is satisfied by residual noise and
	# would have passed on the frozen clock. The baseline capture in the same
	# run is the control: it establishes what animation looks like on this
	# machine, this scene and this frame spacing, and painterly has to reach a
	# fraction of it.
	if not await _measure_wind_animation():
		return

	# Phase E -- the renderer-API route must survive the node.
	if not await _check_renderer_route_survives():
		return

	_pass(
		"Painterly rendered with an authored PainterlyMaterial: %d/%d covered px, delta %d px (%.4f), painterly/standard luma %.4f; wind moved %d px vs baseline %d px."
		% [
			int(metrics["covered_px_painterly_on"]),
			int(metrics["sample_count"]),
			int(metrics["delta_px_painterly_vs_baseline"]),
			float(metrics["delta_ratio_painterly_vs_baseline"]),
			float(metrics["painterly_over_standard_luma"]),
			int(metrics["wind_painterly_frames_differ_px"]),
			int(metrics["wind_baseline_frames_differ_px"]),
		]
	)


# Two captures of one configuration, separated in time. Returns the number of
# sampled pixels that changed.
func _capture_animation_delta() -> int:
	var first: Dictionary = await _settle_and_capture()
	var second: Dictionary = await _settle_and_capture()
	var a: Image = first.get("image")
	var b: Image = second.get("image")
	if a == null or b == null:
		return -1
	var changed := 0
	for y in range(0, a.get_height(), SAMPLE_STRIDE):
		for x in range(0, a.get_width(), SAMPLE_STRIDE):
			if _channel_delta(a.get_pixel(x, y), b.get_pixel(x, y)) > CHANNEL_DELTA_TOLERANCE:
				changed += 1
	return changed


# Phase E -- regression guard for the P1 found by independent review on #1028.
#
# GaussianSplatRenderer.painterly_material is the script route, and the only one
# that existed before `painterly/material` was bound. Binding the node property
# must not take it away. The failure it guards is specifically a DELAYED one:
# the node's push runs from apply_renderer_settings(), which is reached every
# frame, so a renderer-set material reads back fine immediately and is gone one
# frame later. Checking right after the call would see nothing -- the frames in
# between are the whole test.
func _check_renderer_route_survives() -> bool:
	if not renderer.has_method("set_painterly_material") or not renderer.has_method("get_painterly_material"):
		_fail("GaussianSplatRenderer has no painterly_material accessors; the script route is gone.")
		return false

	# Clear the node's authored material so the node is a node that never
	# authored one -- the exact shape of every pre-existing script.
	splat_node.set("painterly/material", null)
	for _i in range(SETTLE_FRAMES):
		await process_frame
		splat_node.force_update()

	renderer.set_painterly_material(painterly_material)
	metrics["renderer_route_valid_immediately"] = renderer.get_painterly_material() != null
	if not bool(metrics["renderer_route_valid_immediately"]):
		_fail("renderer.set_painterly_material() did not take effect at all.")
		return false
	_report.ok()

	var survived: Dictionary = await _settle_and_capture(EXPECT_PAINTERLY)
	metrics["renderer_route_valid_after_frames"] = renderer.get_painterly_material() != null
	metrics["renderer_route_painterly_active"] = bool(survived.get("painterly_active", false))
	metrics["renderer_route_stage_raster_reason"] = str(survived.get("stage_raster_reason", ""))
	if not bool(metrics["renderer_route_valid_after_frames"]):
		_fail(
			"A material set through GaussianSplatRenderer.painterly_material was cleared after %d frames "
			% SETTLE_FRAMES
			+ "(stage_raster_reason='%s'). The node is overwriting the script route every frame; "
			% metrics["renderer_route_stage_raster_reason"]
			+ "the node must push only when the node itself changed the material."
		)
		return false
	_report.ok()

	if not bool(metrics["renderer_route_painterly_active"]):
		_fail(
			"The renderer still holds the material but the frame ran the baseline raster (reason='%s')."
			% metrics["renderer_route_stage_raster_reason"]
		)
		return false
	_report.ok()
	return true


func _measure_wind_animation() -> bool:
	ProjectSettings.set_setting("rendering/gaussian_splatting/animation/wind_enabled", true)
	ProjectSettings.set_setting("rendering/gaussian_splatting/animation/wind_strength", 0.75)
	ProjectSettings.set_setting("rendering/gaussian_splatting/animation/wind_frequency", 2.0)
	ProjectSettings.set_setting("rendering/gaussian_splatting/animation/wind_spatial_frequency", 0.5)
	ProjectSettings.set_setting("rendering/gaussian_splatting/animation/wind_time_scale", 1.0)

	# Control first: the baseline path must be seen animating, or a "painterly
	# does not animate" verdict would be unattributable -- it could equally mean
	# the two captures are simply not separated in time on this runner.
	splat_node.set_enable_painterly(false)
	var baseline_changed: int = await _capture_animation_delta()
	metrics["wind_baseline_frames_differ_px"] = baseline_changed
	if baseline_changed < 0:
		_fail("Wind animation control capture returned no image.")
		return false
	if baseline_changed < MIN_WIND_CONTROL_DIFFER_PX:
		_fail(
			"Baseline path moved only %d of %d sampled px between two time-separated frames (floor %d). "
			% [baseline_changed, int(metrics["sample_count"]), MIN_WIND_CONTROL_DIFFER_PX]
			+ "Wind animation is not observable in this run at all, so nothing can be concluded about painterly."
		)
		return false
	_report.ok()

	splat_node.set_enable_painterly(true)
	var painterly_changed: int = await _capture_animation_delta()
	metrics["wind_painterly_frames_differ_px"] = painterly_changed
	if painterly_changed < 0:
		_fail("Wind animation painterly capture returned no image.")
		return false
	metrics["wind_painterly_over_baseline_ratio"] = float(painterly_changed) / float(max(baseline_changed, 1))
	if float(metrics["wind_painterly_over_baseline_ratio"]) < MIN_WIND_PAINTERLY_RATIO:
		_fail(
			"Painterly moved %d of %d sampled px between two time-separated frames while the baseline moved %d (ratio %.4f, floor %.4f). "
			% [
				painterly_changed, int(metrics["sample_count"]), baseline_changed,
				float(metrics["wind_painterly_over_baseline_ratio"]), MIN_WIND_PAINTERLY_RATIO,
			]
			+ "Painterly's wind clock is not advancing (#1018)."
		)
		return false
	_report.ok()
	return true
