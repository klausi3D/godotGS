extends SceneTree

func _collect_imports(path: String, imports: Array[String], errors: Array[String]) -> void:
	var directory := DirAccess.open(path)
	if directory == null:
		errors.append("Cannot inspect " + path)
		return
	for file in directory.get_files():
		# The QA fixtures require imports for both module source formats.
		if file.get_extension().to_lower() in ["ply", "gsplatworld"]:
			if not FileAccess.file_exists(path.path_join(file + ".import")):
				errors.append("Missing import sidecar for " + path.path_join(file))
		if file.ends_with(".import"):
			var sidecar := path.path_join(file)
			if FileAccess.file_exists(sidecar.trim_suffix(".import")):
				imports.append(sidecar)
	for child in directory.get_directories():
		var subdirectory := path.path_join(child)
		if child in [".godot", ".git"] or FileAccess.file_exists(subdirectory.path_join(".gdignore")):
			continue
		_collect_imports(subdirectory, imports, errors)

func _init() -> void:
	var imports: Array[String] = []
	var errors: Array[String] = []
	_collect_imports("res://", imports, errors)
	var checked := 0
	var skipped := 0
	for sidecar in imports:
		var config := ConfigFile.new()
		if config.load(sidecar) != OK:
			errors.append("Cannot parse " + sidecar)
			continue
		var importer: Variant = config.get_value("remap", "importer", "")
		if importer in ["keep", "skip"]:
			skipped += 1
			continue
		var valid: Variant = config.get_value("remap", "valid", true)
		if not importer is String or importer.is_empty() or not valid is bool or not valid:
			errors.append("Invalid import " + sidecar)
			continue
		var destinations: Variant = config.get_value("deps", "dest_files", null)
		if not (destinations is Array or destinations is PackedStringArray) or destinations.is_empty():
			errors.append("Missing import destinations " + sidecar)
			continue
		for destination in destinations:
			if not destination is String or destination.is_empty() or not FileAccess.file_exists(destination):
				errors.append("Missing artifact %s for %s" % [destination, sidecar])
		var has_remap_path := false
		for key in config.get_section_keys("remap"):
			if key == "path" or key.begins_with("path."):
				has_remap_path = true
				var artifact: Variant = config.get_value("remap", key)
				if not artifact is String or artifact.is_empty() or not FileAccess.file_exists(artifact):
					errors.append("Missing remap artifact for " + sidecar)
		if not has_remap_path:
			errors.append("Missing remap path " + sidecar)
		checked += 1
	if checked == 0:
		errors.append("QA preparation verified no import artifacts")
	if not errors.is_empty():
		for error in errors:
			push_error(error)
		quit(1)
		return
	print("QA_IMPORTS_VERIFIED count=%d skipped=%d" % [checked, skipped])
	quit(0)
