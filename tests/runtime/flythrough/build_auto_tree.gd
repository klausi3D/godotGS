extends SceneTree

# Private JSON bridge for build_auto_tree.py. Python validates the versioned job
# before this script restores integer types lost by Godot's JSON parser. Only
# the C++ production selector computes cuts; no path or selector lives here.


func _finish(path: String, report: Dictionary, code: int) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		printerr("build_auto_tree: producer_report_write_failed")
		quit(1)
		return
	file.store_string(JSON.stringify(report, "  ", true, true))
	file.flush()
	var error := file.get_error()
	file.close()
	quit(code if error == OK else 1)


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 4 or args[0] != "--request" or args[2] != "--report":
		printerr("build_auto_tree: expected --request JSON --report JSON")
		quit(2)
		return
	var report_path: String = args[3]
	var request: Variant = JSON.parse_string(FileAccess.get_file_as_string(args[1]))
	if typeof(request) != TYPE_DICTIONARY:
		_finish(report_path, {"reason": "producer_request_invalid"}, 1)
		return
	if not ClassDB.class_exists("GaussianSplatHlodAutoTree"):
		_finish(report_path, {"reason": "binding_missing: GaussianSplatHlodAutoTree"}, 1)
		return
	var api: Object = ClassDB.instantiate("GaussianSplatHlodAutoTree")
	if api == null or not api.has_method("build") or not api.has_method("oracle_cut"):
		_finish(report_path, {"reason": "binding_missing: GaussianSplatHlodAutoTree.build/oracle_cut"}, 1)
		return
	var job: Dictionary = request["job"]
	var limits: Dictionary = job["limits"].duplicate(true)
	for key in ["max_ram_bytes", "max_scratch_bytes", "max_output_bytes"]:
		limits[key] = int(limits[key])
	var options := {
		"codec": job["codec"],
		"worker_count": int(job["worker_count"]),
		"max_tile_splats": int(job["max_tile_splats"]),
		"source_fingerprint": job["source_fingerprint"],
		"scratch_dir": request["scratch"],
		"limits": limits,
	}
	var builder: Dictionary = api.call("build", request["input"], request["output"], options)
	var report := {"builder": builder}
	if builder.get("error", FAILED) != OK:
		_finish(report_path, report, 1)
		return
	var camera: Dictionary = job["oracle"].duplicate(true)
	camera["width"] = int(camera["width"])
	camera["height"] = int(camera["height"])
	camera["drawn_budget_splats"] = int(job["drawn_budget_splats"])
	camera["leaves_only"] = false
	report["oracle"] = api.call("oracle_cut", request["output"], request["poses"], camera)
	# Separate call => private snapshot. This control never changes the main cut
	# or its verdict, even when full-detail leaves exceed the supplied budget.
	camera["leaves_only"] = true
	report["leaves_only"] = api.call("oracle_cut", request["output"], request["poses"], camera)
	_finish(report_path, report, 0)
