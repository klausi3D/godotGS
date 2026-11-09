extends SceneTree

# Beyond-VRAM world generator CLI (BV-05; docs/architecture/adr-beyond-vram-worlds.md §6).
#
#   godot --headless --script tests/runtime/flythrough/generate_world.gd -- \
#       --tier <T0|A|A-mini|T-smoke> --out <abs path>/world.gsplatworld \
#       [--seed N] [--overrides <json file>] [--select <json file with a chunk id array>] \
#       [--codec <none|gsq_v1>] [--plan-only] [--verify-only] [--report <json file>]
#
# Exit codes: 0 OK, 1 generation or verification failed, 2 usage error.
# Generated worlds never enter res:// (ADR D-2); --out must be an absolute path.


func _fail_usage(msg: String) -> void:
	printerr("generate_world: " + msg)
	quit(2)


func _read_json(path: String) -> Variant:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return null
	return JSON.parse_string(text)


func _write_json(path: String, value: Variant) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(value, "  ", true))


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	var opts := {}
	var i := 0
	while i < args.size():
		var a: String = args[i]
		if a in ["--plan-only", "--verify-only"]:
			opts[a] = true
			i += 1
			continue
		if not a.begins_with("--") or i + 1 >= args.size():
			_fail_usage("unexpected argument '%s'" % a)
			return
		opts[a] = args[i + 1]
		i += 2

	var out: String = opts.get("--out", "")
	if out.is_empty() or out.begins_with("res://") or not out.is_absolute_path():
		_fail_usage("--out must be an absolute path outside res://")
		return

	if opts.has("--verify-only"):
		var v: Dictionary = GaussianSplatWorldGenerator.verify(out)
		print(JSON.stringify(v, "  ", true))
		if opts.has("--report"):
			_write_json(opts["--report"], v)
		quit(0 if v.get("ok", false) else 1)
		return

	var tier: String = opts.get("--tier", "")
	var params: Dictionary = GaussianSplatWorldGenerator.get_tier_params(tier)
	if params.is_empty():
		_fail_usage("unknown --tier '%s' (known: %s)" % [tier, ", ".join(GaussianSplatWorldGenerator.get_tier_names())])
		return
	var overrides := {}
	if opts.has("--overrides"):
		var o: Variant = _read_json(opts["--overrides"])
		if typeof(o) != TYPE_DICTIONARY:
			_fail_usage("--overrides must name a JSON object file")
			return
		overrides = o
	if opts.has("--seed"):
		overrides["seed"] = int(opts["--seed"])
	if not overrides.is_empty():
		var merged: Dictionary = GaussianSplatWorldGenerator.apply_overrides(params, overrides)
		if not merged.get("ok", false):
			_fail_usage(merged.get("message", "invalid overrides"))
			return
		params = merged["params"]

	var plan: Dictionary = GaussianSplatWorldGenerator.plan(params)
	print("[generate_world] plan: " + JSON.stringify(plan, "", true))
	if not plan.get("ok", false):
		quit(1)
		return
	if opts.has("--plan-only"):
		if opts.has("--report"):
			_write_json(opts["--report"], plan)
		quit(0)
		return

	var selection := PackedInt64Array()
	if opts.has("--select"):
		var s: Variant = _read_json(opts["--select"])
		if typeof(s) != TYPE_ARRAY:
			_fail_usage("--select must name a JSON array file of chunk ids")
			return
		for c in s:
			selection.push_back(int(c))

	var codec: String = opts.get("--codec", "none")
	var report: Dictionary = GaussianSplatWorldGenerator.generate(params, out, selection, true, codec)
	print("[generate_world] report: " + JSON.stringify(report, "", true))
	if opts.has("--report"):
		_write_json(opts["--report"], report)
	quit(0 if report.get("ok", false) else 1)
