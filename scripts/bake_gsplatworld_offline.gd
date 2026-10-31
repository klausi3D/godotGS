## Private worker for bake_gsplatworld_offline.py. Run the supervisor, not this file.
extends SceneTree

var report: Dictionary = {"status": "FAILED"}

func finish(error: int, reason: String) -> void:
	report["error"] = error
	report["reason"] = reason
	var file := FileAccess.open("res://result.json", FileAccess.WRITE)
	if file == null:
		quit(2)
		return
	file.store_string(JSON.stringify(report, "\t"))
	file.close()
	quit(0 if error == OK else 1)

func _initialize() -> void:
	var request: Variant = JSON.parse_string(FileAccess.get_file_as_string("res://request.json"))
	if not request is Dictionary:
		finish(ERR_INVALID_DATA, "Missing frozen request")
		return
	report["engine"] = Engine.get_version_info()
	if report["engine"].get("hash", "") != request["implementation"]["source_sha"]:
		finish(ERR_INVALID_DATA, "Build source identity mismatch")
		return
	if ProjectSettings.get_setting("threading/worker_pool/max_threads") != 1:
		finish(ERR_INVALID_DATA, "Worker count mismatch")
		return
	if ProjectSettings.get_setting("rendering/gaussian_splatting/import/use_gsplatworld_cache"):
		finish(ERR_INVALID_DATA, "Source-side caches must be disabled")
		return
	var source: Dictionary = request["source"]
	if FileAccess.get_sha256(source["path"]) != source["sha256"]:
		finish(ERR_INVALID_DATA, "Source identity mismatch")
		return
	var world: GaussianSplatWorld
	var started := Time.get_ticks_usec()
	if String(source["path"]).get_extension().to_lower() == "ply":
		var data := GaussianData.new()
		var error := data.load_from_file(source["path"])
		if error != OK:
			finish(error, "Source load failed")
			return
		world = GaussianSplatWorld.new()
		world.set_gaussian_data(data)
	else:
		world = ResourceLoader.load(source["path"], "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
	if world == null:
		finish(ERR_INVALID_DATA, "Source world failed strict loading")
		return
	report["load_ms"] = float(Time.get_ticks_usec() - started) / 1000.0
	if world.get_splat_count() != source["count"] or world.get_sh_degree() != source["sh_degree"]:
		finish(ERR_INVALID_DATA, "Source count or SH identity mismatch")
		return
	report["source_hlod"] = world.get_hlod_info()
	report["preparation"] = world.bake_hlod_offline(request["limits"], request["candidate_path"])
	var error: int = report["preparation"]["error"]
	if error != OK:
		finish(error, "Bounded preparation failed")
		return
	report["candidate_sha256"] = FileAccess.get_sha256(request["candidate_path"])
	report["status"] = "VALIDATED_CANDIDATE"
	finish(OK, "Strict candidate reload passed; bounded verification worker owns publication")
