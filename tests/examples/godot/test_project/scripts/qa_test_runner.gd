extends SceneTree
## QA Test Runner - Runs all QA tests sequentially and reports results.
##
## Usage:
##   godot --path <project> --script res://scripts/qa_test_runner.gd

const QA_TEST_BASE = preload("res://scripts/qa_test_base.gd")

var test_scenes: Array[String] = [
	"res://scenes/qa/qa_scale_validation.tscn",
	"res://scenes/qa/qa_static_fast_path.tscn",
	"res://scenes/qa/qa_sort_depth_order.tscn",
	"res://scenes/qa/qa_sort_tie_breaker.tscn",
	"res://scenes/qa/qa_sort_multi_instance.tscn",
	"res://scenes/qa/qa_performance_budget.tscn",
]

## Scenes deliberately not run, and why.
##
## Previously these were commented-out lines in test_scenes: invisible in the
## run output, invisible in the results JSON, and impossible to audit. A
## commented-out test is a coverage gap that looks like nothing at all. Listing
## them here makes every exclusion show up in the suite summary and in
## qa_results.json, so the gap is counted rather than forgotten.
##
## Removing an entry is the whole fix for that scene — it starts gating again
## the moment the reason stops being true.
const QUARANTINED_SCENES := {
	"res://scenes/qa/qa_visual_diff.tscn":
		"#785: compares GaussianSplatWorld3D against GaussianSplatNode3D in ONE scene, "
		+ "but the renderer commits to a single route per frame and the node always "
		+ "publishes RESIDENT, so the world submission is skipped "
		+ "(route_uid COMMON.SKIP.RESIDENT_NOT_FEASIBLE.RESIDENT_NO_INSTANCES) and its "
		+ "capture is the clear colour. Scored SSIM 1.0000 blank-vs-blank and stayed "
		+ "green with one node displaced 3 world units. Also: the two fixtures hold "
		+ "10 vs 10000 splats.",
	"res://scenes/qa/qa_sh_rotation.tscn":
		"#785: same invalid two-routes-in-one-scene design as qa_visual_diff; the world "
		+ "path is blank at every camera angle, so the SSIM measured the instance path "
		+ "against the clear colour.",
	"res://scenes/qa/qa_stream_visual_smoke.tscn":
		"#786: streaming/visual readiness is never reached (luma variance 0.00009 vs the "
		+ "0.0002 gate) reproducibly across runs. Likely the same subsystem as #787, "
		+ "where GPU Streaming Stress crashes with STATUS_STACK_BUFFER_OVERRUN.",
	"res://scenes/qa/qa_stream_multi_asset.tscn":
		"Pre-existing: disabled until the runtime surface can prove true "
		+ "resident/streaming coexistence.",
	"res://scenes/qa/qa_stream_chunk_loading.tscn":
		"Pre-existing: streaming monitors not populated.",
	"res://scenes/qa/qa_stream_eviction_churn.tscn":
		"Pre-existing: streaming monitors not populated.",
}

var results: Array[Dictionary] = []
var current_test_index: int = -1
var current_test_instance: Node = null
var current_test_node: Node = null
var suite_start_time: float = 0.0

func _init() -> void:
	call_deferred("_run")

func _run() -> void:
	suite_start_time = Time.get_ticks_msec() / 1000.0
	print("")
	print("=" .repeat(60))
	print("[QA Runner] Gaussian Splatting QA Test Suite")
	print("=" .repeat(60))
	print("Tests to run: %d" % test_scenes.size())
	print("")

	await create_timer(0.5).timeout
	_run_next_test()

func _run_next_test():
	current_test_index += 1

	if current_test_index >= test_scenes.size():
		_print_summary()
		return

	var scene_path = test_scenes[current_test_index]
	print("[QA Runner] Loading test %d/%d: %s" % [
		current_test_index + 1,
		test_scenes.size(),
		scene_path.get_file()
	])

	if not ResourceLoader.exists(scene_path):
		print("[QA Runner] SKIP: Scene not found: %s" % scene_path)
		results.append({
			"scene": scene_path,
			"passed": false,
			"message": "Scene not found"
		})
		_run_next_test()
		return

	var scene = load(scene_path)
	if scene == null:
		print("[QA Runner] SKIP: Failed to load: %s" % scene_path)
		results.append({
			"scene": scene_path,
			"passed": false,
			"message": "Failed to load scene"
		})
		_run_next_test()
		return

	current_test_instance = scene.instantiate()
	if current_test_instance == null:
		print("[QA Runner] SKIP: Failed to instantiate: %s" % scene_path)
		results.append({
			"scene": scene_path,
			"passed": false,
			"message": "Failed to instantiate"
		})
		_run_next_test()
		return

	# Resolve and configure test node before entering tree to avoid auto_start race.
	current_test_node = _find_qa_test(current_test_instance)
	if current_test_node != null:
		current_test_node.auto_start = false
		current_test_node.quit_on_complete = false

	root.add_child(current_test_instance)

	# Connect to test completion signal
	if current_test_node != null and current_test_node.has_signal("test_completed"):
		current_test_node.test_completed.connect(_on_test_completed)
		current_test_node.start_test()
	else:
		# No QA test found, wait a bit and skip
		print("[QA Runner] SKIP: No GSQATest found in scene")
		await create_timer(1.0).timeout
		results.append({
			"scene": scene_path,
			"passed": false,
			"message": "No GSQATest found"
		})
		_cleanup_current_test()
		_run_next_test()

func _find_qa_test(node: Node) -> Node:
	# Use duck typing instead of class check for headless compatibility
	if node.has_signal("test_completed") and node.has_method("start_test"):
		return node
	for child in node.get_children():
		var result = _find_qa_test(child)
		if result != null:
			return result
	return null

func _on_test_completed(passed: bool, message: String):
	var scene_path = test_scenes[current_test_index]
	var metrics: Dictionary = {}
	if current_test_node != null and current_test_node.has_method("get_result_metrics"):
		metrics = current_test_node.get_result_metrics()
	results.append({
		"scene": scene_path,
		"passed": passed,
		"skipped": _is_skip_message(message),
		"message": message,
		"metrics": metrics
	})

	await create_timer(0.5).timeout
	_cleanup_current_test()
	_run_next_test()

## A scene that self-skips (e.g. qa_stream_visual_smoke when there is no
## non-headless viewport) reports passed=true, because from its own point of
## view nothing went wrong. Recorded that way it is indistinguishable from a
## scene that ran and verified something — so the suite could report all-green
## while several scenes silently did nothing. Surface the distinction in the
## JSON so the CI comparator can refuse to freeze a skip into a baseline.
##
## Preloaded explicitly rather than via the `GSQATest` global class name: this
## script is launched with `--script`, and global class names resolve from the
## editor-populated script-class cache, which a fresh CI checkout has not
## built. Referencing GSQATest here is a parse error that kills the whole run.
func _is_skip_message(message: String) -> bool:
	return message.contains(QA_TEST_BASE.SKIP_MARKER)

func _cleanup_current_test():
	if current_test_instance != null:
		current_test_instance.queue_free()
		current_test_instance = null
		current_test_node = null

func _print_summary():
	var suite_end_time = Time.get_ticks_msec() / 1000.0
	print("")
	print("=" .repeat(60))
	print("[QA Runner] TEST RESULTS")
	print("=" .repeat(60))
	print("")

	var passed_count = 0
	var failed_count = 0
	var skipped_count = 0

	for result in results:
		var skipped: bool = result.get("skipped", false)
		var status = "SKIP" if skipped else ("PASS" if result["passed"] else "FAIL")
		var scene_name = result["scene"].get_file()
		print("  [%s] %s" % [status, scene_name])
		print("        %s" % result["message"])

		if skipped:
			skipped_count += 1
		if result["passed"]:
			passed_count += 1
		else:
			failed_count += 1

	if not QUARANTINED_SCENES.is_empty():
		print("")
		print("  QUARANTINED (not run, not verified):")
		for scene_path in QUARANTINED_SCENES:
			print("  [QUAR] %s" % String(scene_path).get_file())
			print("        %s" % QUARANTINED_SCENES[scene_path])

	print("")
	print("-" .repeat(60))
	print("  Total: %d | Passed: %d | Failed: %d | Skipped: %d | Quarantined: %d" % [
		results.size(), passed_count, failed_count, skipped_count, QUARANTINED_SCENES.size()
	])
	print("=" .repeat(60))
	print("")

	_write_results_json(suite_end_time, passed_count, failed_count, skipped_count)

	# Exit with appropriate code
	await create_timer(2.0).timeout
	quit(0 if failed_count == 0 else 1)

func _write_results_json(suite_end_time: float, passed_count: int, failed_count: int, skipped_count: int) -> void:
	var output_path = _resolve_output_path()
	if output_path.is_empty():
		return

	# `skipped` is reported alongside, not instead of, `passed`: a self-skip
	# still counts as passed for the exit code (the scene did nothing wrong),
	# but CI needs to see that it verified nothing. run_baseline_qa.py fails
	# the run when capture was declared required and anything skipped.
	var summary = {
		"start_time": suite_start_time,
		"end_time": suite_end_time,
		"duration_s": suite_end_time - suite_start_time,
		"total_tests": results.size(),
		"passed": passed_count,
		"failed": failed_count,
		"skipped": skipped_count,
		"quarantined": QUARANTINED_SCENES.size(),
	}

	var payload = {
		"summary": summary,
		"results": results,
		# Emitted so the quarantine is auditable from the CI artifact alone —
		# a reviewer can see what the suite did NOT verify without reading
		# this script. tests/ci/run_baseline_qa.py surfaces the count.
		"quarantined": QUARANTINED_SCENES,
	}

	var file = FileAccess.open(output_path, FileAccess.WRITE)
	if file == null:
		print("[QA Runner] WARN: Could not write results to %s" % output_path)
		return
	file.store_string(JSON.stringify(payload, "  "))
	print("[QA Runner] Results written to %s" % output_path)

func _resolve_output_path() -> String:
	var args = OS.get_cmdline_args()
	for i in range(args.size()):
		var arg = String(args[i])
		if arg.begins_with("--qa-output="):
			return arg.replace("--qa-output=", "")
		if arg == "--qa-output" and i + 1 < args.size():
			return String(args[i + 1])
	if OS.has_environment("QA_OUTPUT_PATH"):
		return OS.get_environment("QA_OUTPUT_PATH")
	return ""
