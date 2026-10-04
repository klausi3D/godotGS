extends SceneTree

# Run after an actual editor import, with -- <source> <imported artifact>.
func _init() -> void:
	var args := OS.get_cmdline_user_args()
	if args.size() != 2:
		push_error("Expected an editor-imported v1 source and its v2 artifact")
		quit(1)
		return
	var source := args[0]
	var artifact := args[1]
	if not FileAccess.file_exists(source + ".import"):
		push_error("Missing editor-generated import metadata")
		quit(1)
		return
	var source_hash := FileAccess.get_sha256(source)
	var imported: GaussianSplatWorld = ResourceLoader.load(artifact, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
	var resolved: GaussianSplatWorld = ResourceLoader.load(source, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
	if imported == null or resolved == null or not imported.has_hlod_tree() or not resolved.has_hlod_tree():
		push_error("Source-path loading bypassed the HLOD import")
		quit(1)
		return
	if imported.get_splat_count() != resolved.get_splat_count() or imported.get_hlod_info() != resolved.get_hlod_info():
		push_error("Source-path load differs from the imported artifact")
		quit(1)
		return
	var raw_path := "user://hlod_import_remap_raw_%d.gsplatworld" % Time.get_ticks_usec()
	if FileAccess.file_exists(raw_path) or FileAccess.file_exists(raw_path + ".import"):
		push_error("Raw-source control path is already occupied")
		quit(1)
		return
	if DirAccess.copy_absolute(ProjectSettings.globalize_path(source), ProjectSettings.globalize_path(raw_path)) != OK:
		push_error("Could not create a sidecar-free raw-source control")
		quit(1)
		return
	var raw: GaussianSplatWorld = ResourceLoader.load(raw_path, "GaussianSplatWorld", ResourceLoader.CACHE_MODE_IGNORE)
	var raw_ok := raw != null and not raw.has_hlod_tree() and raw.get_splat_count() == imported.get_splat_count()
	raw = null
	DirAccess.remove_absolute(ProjectSettings.globalize_path(raw_path))
	if not raw_ok or FileAccess.get_sha256(source) != source_hash:
		push_error("Raw-v1 loading or source preservation failed")
		quit(1)
		return
	print("HLOD_IMPORT_REMAP_PASS ", JSON.stringify({"splats": resolved.get_splat_count(), "hlod": resolved.get_hlod_info()}))
	quit(0)
