extends SceneTree

# S1b metrics: PSNR/SSIM from the repository's own BenchmarkVisualMetrics.
#
# Input job JSON: {"out_json": path, "pairs": [{"id": str, "a": png, "b": png}, ...]}
# Output: {"status": "ok"|"failed", "results": [{"id", "psnr", "ssim", "width", "height"}]}
#
# BenchmarkVisualMetrics returns 0.0 for a null/size-mismatched/too-small input and caps
# PSNR at 100.0 for (near-)identical images; this script reports the raw values and the
# image sizes so s1b_quality.py can treat 0.0-from-failure as a failure, not a score.
#
# Usage: godot --headless --path tests/runtime/hlod_s1b_quality --script s1b_metrics.gd -- --job=<job.json>

const Metrics := preload("../../examples/godot/test_project/scripts/benchmark_visual_metrics.gd")


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var job_path := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--job="):
			job_path = arg.substr(6)
	var job = JSON.parse_string(FileAccess.get_file_as_string(job_path))
	if typeof(job) != TYPE_DICTIONARY or not job.has("pairs") or not job.has("out_json"):
		push_error("[S1B_METRICS_FAIL] unreadable job")
		quit(1)
		return
	var results: Array = []
	var status := "ok"
	for pair in job["pairs"]:
		var a: Image = Metrics.load_image(str(pair["a"]))
		var b: Image = Metrics.load_image(str(pair["b"]))
		var entry := {"id": pair["id"], "psnr": null, "ssim": null, "width": 0, "height": 0, "error": ""}
		if a == null or b == null:
			entry["error"] = "image failed to load"
			status = "failed"
		elif a.get_size() != b.get_size():
			entry["error"] = "size mismatch %s vs %s" % [str(a.get_size()), str(b.get_size())]
			status = "failed"
		else:
			entry["width"] = a.get_width()
			entry["height"] = a.get_height()
			entry["psnr"] = Metrics.calculate_psnr(a, b)
			entry["ssim"] = Metrics.calculate_ssim(a, b)
		results.append(entry)
	var f := FileAccess.open(str(job["out_json"]), FileAccess.WRITE)
	if f == null:
		quit(1)
		return
	f.store_string(JSON.stringify({"status": status, "results": results}, "\t", false, true))
	f.close()
	print("[S1B_METRICS] %s pairs=%d" % [status, results.size()])
	quit(0 if status == "ok" else 1)
