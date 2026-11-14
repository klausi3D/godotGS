extends SceneTree

# BV-07 probe driver (docs/architecture/adr-beyond-vram-worlds.md section 7, D-4, D-6).
#
#   godot --path <project> --script tests/runtime/flythrough/probe_world.gd -- --plan <plan.json>
#
# run_probe.py writes the plan and parses what this script leaves behind; it is the
# only caller. This script does three things and decides nothing:
#
#   * It applies the plan's settings, loads ONE world on the chunk-streaming route and
#     renders it at the plan's resolution with vsync off.
#   * mode "path": it flies the resolved arc-length path (probe_path.py). A moving
#     segment sets the pose at s = speed * wall-clock elapsed, interpolated linearly in
#     the segment's pose table; a stop / teleport / hold PUMPS frames until the settle
#     condition holds or a wall-clock deadline passes, then captures a checkpoint.
#   * mode "sweep": for each view it pumps until the world is settled, warms up, then
#     measures for a wall-clock window (the F/k drawn sweep).
#
# Every rendered frame appends one fixed-size row to the binary trace (header: magic,
# u32 length, JSON field list). The result JSON says how many frames were rendered and
# how many rows were written; run_probe.py checks they are equal (ADR V5).
#
# Pump rule (tests/AGENTS.md, #879/#881): frames drive the async streaming work, so a
# wait is a loop over rendered frames bounded by wall clock, never a frame count, and
# the condition must hold on a frame that ENDED before the deadline. A missed deadline
# is recorded as such, never as settled.
#
# Exit codes: 0 the plan ran to the end (the result JSON says what was measured);
# 1 a step failed (reason in the result JSON); 2 usage / plan error.

const TRACE_MAGIC := "GSBV7TR1"
const TRACE_SCHEMA := "godotgs-bv07-probe-trace/1"
const RESULT_SCHEMA := "godotgs-bv07-probe-result/1"

const PASS_NAMES := ["cull", "projection", "presort", "overlap_count", "prefix", "binning",
		"overlap_sort", "raster", "resolve", "composite"]

# Segment states recorded per row.
const ST_MOVING := 0
const ST_SETTLING := 1
const ST_HOLD := 2
const ST_MEASURE := 3
const ST_WARMUP := 4
const ST_LOADING := 5

# #1393: the renderer's always-on overlap-record drop telemetry (cumulative per renderer, read
# from get_render_stats() without issuing a readback). -1 when the renderer does not publish the
# key: an older binary reads as MISSING in probe_summary.overlap_drops, never as zero drops.
const DROP_FIELDS := [
	["drop_signal_samples", "q"], ["overflow_drop_events", "q"], ["binning_drop_events", "q"],
	["binning_tile_range_drop_events", "q"], ["binning_budget_drop_events", "q"],
	["raster_tile_cap_drop_events", "q"], ["sampled_dropped_records_total", "q"],
	["sampled_overlap_records_total", "q"], ["sampled_overlap_splats_total", "q"],
]

# Diagnostic-only columns. Existing BV-07 traces keep their original layout;
# the self-describing header advertises these only for the paired experiment.
const SHARED_LAYER_FIELDS := [
	["hlod_stream_desired_nodes", "q"], ["hlod_stream_resident_nodes", "q"],
	["hlod_stream_loading_nodes", "q"], ["hlod_stream_drawn_nodes", "q"],
	["hlod_stream_failed_nodes", "q"], ["hlod_stream_nonresident_drawn", "q"],
	["hlod_stream_uploaded_bytes_this_frame", "q"], ["hlod_stream_uploaded_bytes_total", "q"],
]

# Field name, type ("q" int64 / "d" float64). Order is the row layout.
var FIELDS := [
	["row", "q"], ["engine_frame", "q"], ["t_usec", "q"], ["phase", "q"], ["pass_index", "q"],
	["segment", "q"], ["view", "q"], ["state", "q"], ["s_m", "d"],
	["px", "d"], ["py", "d"], ["pz", "d"], ["yaw_deg", "d"], ["pitch_deg", "d"],
	["drawn_splats", "q"], ["visible_chunk_splats", "q"], ["loaded_chunks", "q"], ["visible_chunks", "q"],
	["needed_chunks", "q"], ["needed_unserved_chunks", "q"], ["chunks_evicted_this_frame", "q"],
	["needed_not_resident_chunks", "q"], ["needed_upload_pending_chunks", "q"], ["visible_chunks_evicted_this_frame", "q"],
	["upload_bytes_total", "q"], ["upload_chunks_total", "q"], ["read_bytes_total", "q"],
	["evicted_bytes_total_mb", "d"], ["vram_mb", "d"], ["effective_vram_budget_mb", "d"],
	["effective_max_chunks", "q"], ["regulated_max_chunks", "q"], ["lod_distance_multiplier", "d"],
	["effective_settings_revision", "q"], ["vram_regulation_adjustments", "q"],
	["render_ready", "q"], ["upload_stall_wall_ms", "d"],
	["coherent_frame_serial", "q"], ["coherent_pass_mask", "q"], ["coherent_total_ms", "d"],
	["coherent_resolve_usec", "q"], ["coherent_all_bv04b", "q"], ["last_resolve_usec", "q"],
	["pass_valid_mask", "q"],
	["cull_ms", "d"], ["projection_ms", "d"], ["presort_ms", "d"], ["overlap_count_ms", "d"],
	["prefix_ms", "d"], ["binning_ms", "d"], ["overlap_sort_ms", "d"], ["raster_ms", "d"],
	["resolve_ms", "d"], ["composite_ms", "d"],
	["gpu_frame_ms", "d"], ["gpu_frame_valid", "q"], ["stats_usec", "q"], ["static_memory_bytes", "q"],
] + DROP_FIELDS

var _plan: Dictionary = {}
var _result: Dictionary = {}
var _trace: FileAccess
var _rows := 0
var _scene: Node3D
var _camera: Camera3D
var _node: GaussianSplatWorld3D
var _last_stats: Dictionary = {}
var _last_streaming: Dictionary = {}
var _shared_layers_peaks: Dictionary = {}
var _first_engine_frame := -1
var _last_engine_frame := -1
# Viewport captures (plan "screenshots": {"dir", "moving_every_n"}); see _screenshot().
var _shots := {}
var _shot_log := []
# Per-row context set by the phase code.
var _ctx := {"phase": 0, "pass_index": 0, "segment": -1, "view": -1, "state": ST_LOADING, "s_m": 0.0}


func _init() -> void:
	call_deferred("_run")


func _fail_usage(msg: String) -> void:
	printerr("probe_world: " + msg)
	quit(2)


func _write_json(path: String, value: Variant) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		printerr("probe_world: cannot write %s (%d)" % [path, FileAccess.get_open_error()])
		return
	f.store_string(JSON.stringify(value, "  ", true))
	f.close()


func _finish(ok: bool, reason: String) -> void:
	_result["ok"] = ok
	_result["reason"] = reason
	# Frames between the first and the last traced frame, by the engine's own counter.
	_result["frames_rendered"] = (_last_engine_frame - _first_engine_frame + 1) if _first_engine_frame >= 0 else 0
	_result["first_engine_frame"] = _first_engine_frame
	_result["last_engine_frame"] = _last_engine_frame
	_result["rows_written"] = _rows
	_result["end_usec"] = Time.get_ticks_usec()
	_result["final_streaming"] = _scalars(_last_streaming)
	_result["final_render_stats"] = _scalars(_last_stats)
	_result["gsq_decode_stats"] = _gsq_decode_stats()
	if bool(_plan.get("shared_layers_t0", false)):
		_result["shared_layers_peaks"] = _shared_layers_peaks
		# Hash outside the measured path windows. This receipt comes from the
		# process that actually loaded the file, independently of the Python plan.
		_result["loaded_file_sha256"] = FileAccess.get_sha256(str(_plan.get("world", "")))
	var payload_evidence := _payload_telemetry_result()
	for key in payload_evidence:
		_result[key] = payload_evidence[key]
	_result["screenshots"] = {"dir": _shots.get("dir"), "moving_every_n": int(_shots.get("moving_every_n", 0)),
			"taken": _shot_log}
	if _trace != null:
		_trace.flush()
		_trace.close()
		_trace = null
	_write_json(str(_plan.get("result", "")), _result)
	print("[probe_world] %s: %s (rows %d)" % ["done" if ok else "FAILED", reason, _rows])
	if _node != null:
		_node.clear_world()
	quit(0 if ok else 1)


# BV-K9b: the GSQ source's decode counters (verify, decode, decoded digest); null on other worlds.
func _gsq_decode_stats():
	if _is_hlod_payload_route():
		return null
	if _node == null or _node.world == null or not _node.world.has_method("get_chunk_payload_source"):
		return null
	var src = _node.world.get_chunk_payload_source()
	if src == null or not src.has_method("get_decode_stats"):
		return null
	return src.get_decode_stats()


func _is_hlod_payload_route() -> bool:
	if bool(_last_stats.get("hlod_streaming", false)) or bool(_last_stats.get("hlod_active", false)):
		return true
	return _node != null and _node.world != null and _node.world.has_hlod_paged_source()


func _payload_telemetry_result() -> Dictionary:
	# Query live source identity independently of the cached renderer block. A
	# replaced source with an older renderer sample must remain stale evidence.
	if _is_hlod_payload_route():
		var generation = null
		if _node != null and _node.world != null and _node.world.has_method("get_hlod_payload_source_generation"):
			generation = _node.world.get_hlod_payload_source_generation()
		return {"route": "hlod", "source_generation": generation,
				"payload_telemetry": _last_stats.get("payload_telemetry")}
	if _node != null and _node.world != null:
		var source = _node.world.get_chunk_payload_source()
		if source != null and source.has_method("get_decode_stats"):
			return {"route": "chunk", "source_generation": source.get_instance_id(),
					"payload_telemetry": source.get_decode_stats()}
	return {"route": null, "source_generation": null, "payload_telemetry": null}


func _scalars(d: Dictionary) -> Dictionary:
	var out := {}
	for k in d.keys():
		var v = d[k]
		if v is Dictionary:
			if str(k) in ["streaming_latency_histograms", "payload_telemetry", "device_vram_ledger"]:
				out[k] = v
			continue
		if v is Array:
			continue
		out[k] = v
	return out


# ------------------------------------------------------------------ trace

func _open_trace(path: String) -> bool:
	_trace = FileAccess.open(path, FileAccess.WRITE)
	if _trace == null:
		return false
	var names := []
	var types := ""
	for f in FIELDS:
		names.append(f[0])
		types += f[1]
	var header := JSON.stringify({"schema": TRACE_SCHEMA, "fields": names, "types": types}, "", true)
	var hb := header.to_utf8_buffer()
	_trace.store_buffer(TRACE_MAGIC.to_ascii_buffer())
	_trace.store_32(hb.size())
	_trace.store_buffer(hb)
	return true


func _g(d: Dictionary, k: String, fallback = 0):
	return d.get(k, fallback) if d.has(k) and d[k] != null else fallback


func _sample_and_write() -> void:
	var t_usec := Time.get_ticks_usec()
	var stats := {}
	var r = _node.get_renderer() if _node != null else null
	var t_stats := Time.get_ticks_usec()
	if r != null:
		stats = r.get_render_stats()
	var stats_usec := Time.get_ticks_usec() - t_stats
	_last_stats = stats
	var ss: Dictionary = stats.get("streaming_state", {}) if stats.get("streaming_state") is Dictionary else {}
	_last_streaming = ss
	if bool(_plan.get("shared_layers_t0", false)):
		for key in ["hlod_stream_nonresident_drawn", "hlod_stream_stall_frames", "hlod_stream_blocked_groups",
				"hlod_stream_failed_total", "hlod_stream_late_dropped_total", "hlod_stream_drawn_nodes",
				"hlod_stream_resident_nodes", "rendered_splat_count", "instance_count_overflow_events",
				"overflow_drop_events", "binning_drop_events", "raster_tile_cap_drop_events"]:
			if stats.has(key):
				_shared_layers_peaks[key] = max(int(_shared_layers_peaks.get(key, stats[key])), int(stats[key]))
	var diag: Dictionary = ss.get("diagnostics", {}) if ss.get("diagnostics") is Dictionary else {}
	var gpt: Dictionary = stats.get("gpu_pass_timing", {}) if stats.get("gpu_pass_timing") is Dictionary else {}
	var passes: Dictionary = gpt.get("passes", {}) if gpt.get("passes") is Dictionary else {}
	var pass_ms := []
	var valid_mask := 0
	for i in range(PASS_NAMES.size()):
		var p: Dictionary = passes.get(PASS_NAMES[i], {}) if passes.get(PASS_NAMES[i]) is Dictionary else {}
		var v := bool(p.get("valid", false))
		pass_ms.append(float(p.get("time_ms", 0.0)) if v else 0.0)
		if v:
			valid_mask |= (1 << i)
	var pose := _camera.global_transform.origin
	var rot := _camera.rotation_degrees
	var engine_frame := Engine.get_frames_drawn()
	if _first_engine_frame < 0:
		_first_engine_frame = engine_frame
	_last_engine_frame = engine_frame
	var values := [
		_rows, engine_frame, t_usec, _ctx["phase"], _ctx["pass_index"],
		_ctx["segment"], _ctx["view"], _ctx["state"], _ctx["s_m"],
		pose.x, pose.y, pose.z, rot.y, rot.x,
		int(_g(stats, "rendered_splat_count", -1)), int(_g(ss, "visible_splats", -1)), int(_g(ss, "loaded_chunks", -1)),
		int(_g(diag, "visible_chunks", -1)),
		int(_g(ss, "needed_chunks", -1)), int(_g(ss, "needed_unserved_chunks", -1)), int(_g(ss, "chunks_evicted_this_frame", -1)),
		int(_g(ss, "needed_not_resident_chunks", -1)), int(_g(ss, "needed_upload_pending_chunks", -1)),
		int(_g(ss, "visible_chunks_evicted_this_frame", -1)),
		int(_g(ss, "upload_bytes_total", -1)), int(_g(ss, "upload_chunks_total", -1)), int(_g(ss, "read_bytes_total", -1)),
		float(_g(ss, "evicted_bytes_total_mb", -1.0)), float(_g(ss, "vram_mb", -1.0)), float(_g(ss, "effective_vram_budget_mb", -1.0)),
		int(_g(ss, "effective_max_chunks", -1)), int(_g(ss, "vram_regulated_max_chunks", -1)), float(_g(ss, "lod_distance_multiplier", -1.0)),
		int(_g(ss, "effective_settings_revision", -1)), int(_g(ss, "vram_regulation_adjustments", -1)),
		1 if bool(_g(ss, "render_readiness_ready", false)) else 0, float(_g(diag, "upload_stall_wall_ms", -1.0)),
		int(_g(gpt, "coherent_frame_serial", 0)), int(_g(gpt, "coherent_pass_mask", 0)), float(_g(gpt, "coherent_total_ms", 0.0)),
		int(_g(gpt, "coherent_resolve_usec", 0)), 1 if bool(_g(gpt, "coherent_has_all_bv04b_passes", false)) else 0,
		int(_g(gpt, "last_resolve_usec", 0)),
		valid_mask,
	]
	values.append_array(pass_ms)
	values.append_array([
		float(_g(stats, "gpu_frame_time_ms", 0.0)), 1 if bool(_g(stats, "gpu_frame_time_valid", false)) else 0,
		stats_usec, OS.get_static_memory_usage(),
	])
	for f in DROP_FIELDS:
		values.append(int(_g(stats, f[0], -1)))
	if bool(_plan.get("shared_layers_t0", false)):
		for f in SHARED_LAYER_FIELDS:
			values.append(int(_g(stats, f[0], -1)))
	for i in range(FIELDS.size()):
		if FIELDS[i][1] == "q":
			_trace.store_64(int(values[i]))
		else:
			_trace.store_double(float(values[i]))
	_rows += 1


func _frame() -> void:
	await RenderingServer.frame_post_draw
	_sample_and_write()


# ------------------------------------------------------------------ helpers

func _set_pose(p: Array) -> void:
	_camera.global_position = Vector3(float(p[0]), float(p[1]), float(p[2]))
	_camera.rotation_degrees = Vector3(float(p[4]), float(p[3]), 0.0)


func _interp(table: Array, s: float) -> Array:
	if s <= float(table[0][0]):
		return table[0].slice(1)
	var last: Array = table[table.size() - 1]
	if s >= float(last[0]):
		return last.slice(1)
	var lo := 0
	var hi := table.size() - 1
	while hi - lo > 1:
		var mid := (lo + hi) / 2
		if float(table[mid][0]) <= s:
			lo = mid
		else:
			hi = mid
	var a: Array = table[lo]
	var b: Array = table[hi]
	var f := (s - float(a[0])) / (float(b[0]) - float(a[0]))
	var out := []
	for i in range(1, 6):
		out.append(float(a[i]) + (float(b[i]) - float(a[i])) * f)
	return out


# Distance between the camera the last render stats describe and the probe camera. The
# stats lag the pose by a frame or two, so a settle observed before they agree would be
# the previous pose's settle (found on the first T0 run: a teleport "settled" in 5 ms).
func _stats_pose_error() -> float:
	var st := _last_stats
	if not st.has("debug_cam_origin_x"):
		return INF
	var o := Vector3(float(st["debug_cam_origin_x"]), float(st["debug_cam_origin_y"]), float(st["debug_cam_origin_z"]))
	return o.distance_to(_camera.global_position)


func _settled() -> bool:
	# Flat settle condition (ADR section 7.3 pass B): missing chunks = 0, on a ready route,
	# with something drawn, in stats that describe this pose. A missing key reads as not
	# settled, never as settled.
	# "Missing" means not GPU-resident: needed_not_resident_chunks counts queued, in-flight
	# and loaded-but-not-uploaded chunks. needed_unserved_chunks leaves those out, so an
	# oracle on it settled while chunks were still in flight (holes on screen).
	if _stats_pose_error() > 0.001:
		return false
	if bool(_plan.get("shared_layers_t0", false)) and _is_hlod_payload_route():
		# Diagnostic HLOD convergence only. Never substitute chunk counts or
		# claim the separate G6/G6t cut-settle acceptance oracle has run.
		for key in ["hlod_stream_desired_nodes", "hlod_stream_resident_nodes", "hlod_stream_loading_nodes",
				"hlod_stream_failed_nodes", "hlod_stream_nonresident_drawn", "hlod_stream_drawn_nodes"]:
			if not _last_stats.has(key):
				return false
		return int(_last_stats["hlod_stream_desired_nodes"]) > 0 \
				and int(_last_stats["hlod_stream_desired_nodes"]) == int(_last_stats["hlod_stream_resident_nodes"]) \
				and int(_last_stats["hlod_stream_loading_nodes"]) == 0 \
				and int(_last_stats["hlod_stream_failed_nodes"]) == 0 \
				and int(_last_stats["hlod_stream_nonresident_drawn"]) == 0 \
				and int(_last_stats["hlod_stream_drawn_nodes"]) > 0
	var ss := _last_streaming
	if not ss.has("needed_not_resident_chunks") or not ss.has("needed_chunks"):
		return false
	if int(ss["needed_not_resident_chunks"]) != 0 or int(ss["needed_chunks"]) <= 0:
		return false
	if not bool(ss.get("render_readiness_ready", false)):
		return false
	return int(_last_stats.get("rendered_splat_count", 0)) > 0


# Pump frames until _settled() holds on SETTLE_STREAK consecutive frames, the last of
# which ended before the deadline. Returns {settled, settle_usec, frames, deadline_s}.
# settle_usec is measured to the first frame of the streak (when the condition began to
# hold); the streak is a fixed lower bound, never an upper one.
const SETTLE_STREAK := 3


func _pump_settle(deadline_s: float) -> Dictionary:
	var start := Time.get_ticks_usec()
	var deadline := start + int(deadline_s * 1000000.0)
	var frames := 0
	var streak := 0
	var streak_start := 0
	while true:
		await _frame()
		frames += 1
		var now := Time.get_ticks_usec()
		if now > deadline:
			return {"settled": false, "settle_usec": -1, "frames": frames, "deadline_s": deadline_s,
					"reason": "settle_deadline_missed", "pose_error_m": _stats_pose_error()}
		if _settled():
			if streak == 0:
				streak_start = now
			streak += 1
			if streak >= SETTLE_STREAK:
				return {"settled": true, "settle_usec": streak_start - start, "frames": frames,
						"deadline_s": deadline_s, "pose_error_m": _stats_pose_error()}
		else:
			streak = 0
	return {}


func _pump_for(seconds: float) -> int:
	var end := Time.get_ticks_usec() + int(seconds * 1000000.0)
	var frames := 0
	while Time.get_ticks_usec() < end:
		await _frame()
		frames += 1
	return frames


# BV-13a C-settings (ADR 8.5): change project settings at runtime once the named
# checkpoint of the named pass is captured, then hold for hold_s seconds. Not restored.
func _maybe_inject(checkpoint: String, pass_index: int) -> void:
	var inj = _plan.get("inject")
	if typeof(inj) != TYPE_DICTIONARY:
		return
	if str(inj.get("checkpoint", "")) != checkpoint or int(inj.get("pass_index", -1)) != pass_index:
		return
	var settings: Dictionary = inj.get("project_settings", {})
	for key in settings.keys():
		ProjectSettings.set_setting(key, settings[key])
	ProjectSettings.emit_signal("settings_changed")
	_result["injected"] = {"checkpoint": checkpoint, "pass_index": pass_index, "row": _rows,
			"project_settings": settings}
	_ctx["state"] = ST_HOLD
	await _pump_for(float(inj.get("hold_s", 1.0)))


func _checkpoint(name: String, seg_id: String, settle: Dictionary) -> Dictionary:
	var ss := _last_streaming
	return {
		"checkpoint": name, "segment": seg_id, "pass_index": _ctx["pass_index"], "settle": settle,
		"pose": [_camera.global_position.x, _camera.global_position.y, _camera.global_position.z,
				_camera.rotation_degrees.y, _camera.rotation_degrees.x],
		"drawn_splats": int(_last_stats.get("rendered_splat_count", -1)),
		"visible_chunk_splats": int(ss.get("visible_splats", -1)),
		"loaded_chunks": int(ss.get("loaded_chunks", -1)),
		"needed_chunks": int(ss.get("needed_chunks", -1)),
		"needed_unserved_chunks": int(ss.get("needed_unserved_chunks", -1)),
		"needed_not_resident_chunks": int(ss.get("needed_not_resident_chunks", -1)),
		"needed_upload_pending_chunks": int(ss.get("needed_upload_pending_chunks", -1)),
		"row": _rows - 1,
		"t_usec": Time.get_ticks_usec(),
		"actual_route": "hlod" if _is_hlod_payload_route() else "chunk",
		"hlod_desired_nodes": _last_stats.get("hlod_stream_desired_nodes"),
		"hlod_resident_nodes": _last_stats.get("hlod_stream_resident_nodes"),
		"hlod_drawn_nodes": _last_stats.get("hlod_stream_drawn_nodes"),
	}


# ------------------------------------------------------------------ screenshots

# The viewport as drawn on the frame the last trace row describes: nothing is drawn between
# _frame() returning and this readback. Off unless the plan names a directory. Returns {} when
# off, else the log entry ("file" on success, "error" otherwise). The readback stalls the
# process; a checkpoint caller renders one HOLD frame afterwards so the stall lands in a hold
# row, never in a moving row's frame interval. Moving captures (moving_every_n) do perturb the
# moving intervals; the result records that they were on.
func _screenshot(name: String) -> Dictionary:
	var dir := str(_shots.get("dir", ""))
	if dir.is_empty():
		return {}
	var entry := {"name": name, "row": _rows - 1, "pass_index": _ctx["pass_index"],
			"segment": _ctx["segment"], "state": _ctx["state"]}
	var tex := get_root().get_texture()
	var img: Image = tex.get_image() if tex != null else null
	if img == null or img.is_empty():
		entry["error"] = "no viewport image"
	else:
		var file := name + ".png"
		var err := img.save_png(dir.path_join(file))
		if err != OK:
			entry["error"] = "save_png failed (%d)" % err
		else:
			entry["file"] = file
	_shot_log.append(entry)
	return entry


# ------------------------------------------------------------------ phases

const PROC_IO := "/proc/self/io"


# This process's cumulative block-input bytes since it started (`read_bytes` of
# /proc/self/io: every thread, page-cache hits excluded). -1 where it cannot be read
# (not Linux): run_probe.py then leaves the cold-pass read unmeasured, never zero.
func _proc_read_bytes() -> int:
	var f := FileAccess.open(PROC_IO, FileAccess.READ)
	if f == null:
		return -1
	while not f.eof_reached():
		var line := f.get_line()
		if line.begins_with("read_bytes:"):
			var value := line.substr(11).strip_edges()
			return int(value) if value.is_valid_int() else -1
	return -1


func _run_path() -> String:
	var path: Dictionary = _plan["path"]
	var segs: Array = path["segments"]
	var settle_cfg: Dictionary = _plan.get("settle", {})
	var passes := int(_plan.get("passes", 1))
	var seg_log := []
	var checkpoints := []
	_ctx["phase"] = 0
	# D-14 / V10 (#1403): this process's block reads at each pass boundary, so the lane
	# judges the cold (first) pass alone; the repeat pass and the sweep are warm evidence.
	var pass_io := []
	_result["pass_io"] = {"source": PROC_IO, "passes": pass_io}
	for pass_index in range(passes):
		_ctx["pass_index"] = pass_index
		var io_entry := {"pass_index": pass_index, "read_bytes_start": _proc_read_bytes()}
		pass_io.append(io_entry)
		for si in range(segs.size()):
			var seg: Dictionary = segs[si]
			_ctx["segment"] = si
			var entry := {"pass_index": pass_index, "segment": si, "id": seg["id"], "kind": seg["kind"],
					"start_row": _rows, "start_usec": Time.get_ticks_usec()}
			if seg["kind"] == "move":
				var table: Array = seg["table"]
				var speed := float(seg["speed_m_s"])
				var length := float(seg["length_m"])
				_ctx["state"] = ST_MOVING
				_ctx["s_m"] = 0.0
				_set_pose(_interp(table, 0.0))
				var t0 := Time.get_ticks_usec()
				var moving_frames := 0
				var every_n := int(_shots.get("moving_every_n", 0))
				while true:
					await _frame()
					moving_frames += 1
					if every_n > 0 and moving_frames % every_n == 0:
						_screenshot("p%d_s%02d_row%07d" % [pass_index, si, _rows - 1])
					var s := speed * float(Time.get_ticks_usec() - t0) / 1000000.0
					if s >= length:
						break
					_ctx["s_m"] = s
					_set_pose(_interp(table, s))
				# The final pose of the segment, rendered once.
				_ctx["s_m"] = length
				_set_pose(_interp(table, length))
				await _frame()
			else:
				_ctx["s_m"] = 0.0
				_set_pose(seg["pose"])
				_ctx["state"] = ST_SETTLING
				var key := "stop_deadline_s"
				if seg["kind"] == "teleport":
					key = "teleport_deadline_s"
				elif seg["kind"] == "hold":
					key = "cold_deadline_s" if pass_index == 0 else "stop_deadline_s"
				var settle: Dictionary = await _pump_settle(float(settle_cfg.get(key, 10.0)))
				entry["settle"] = settle
				var extra := float(seg.get("hold_after_ready_s", 0.0))
				if extra > 0.0 and bool(settle.get("settled", false)):
					_ctx["state"] = ST_HOLD
					await _pump_for(extra)
				if seg.get("checkpoint") != null:
					var cp := _checkpoint(str(seg["checkpoint"]), str(seg["id"]), settle)
					var shot := _screenshot("p%d_%s" % [pass_index, str(seg["checkpoint"])])
					cp["screenshot"] = shot.get("file")
					if not shot.is_empty():
						cp["screenshot_error"] = shot.get("error")
					checkpoints.append(cp)
					if not shot.is_empty():
						# The readback's stall goes into a hold row (see _screenshot).
						_ctx["state"] = ST_HOLD
						await _frame()
					await _maybe_inject(str(seg["checkpoint"]), pass_index)
			entry["end_row"] = _rows - 1
			entry["end_usec"] = Time.get_ticks_usec()
			seg_log.append(entry)
		io_entry["read_bytes_end"] = _proc_read_bytes()
	_result["segments"] = seg_log
	_result["checkpoints"] = checkpoints
	return ""


func _run_sweep() -> String:
	var sweep: Dictionary = _plan["sweep"]
	var views: Array = sweep["views"]
	var out := []
	_ctx["phase"] = 1
	_ctx["segment"] = int(sweep.get("point_index", 0))
	for vi in range(views.size()):
		var view: Dictionary = views[vi]
		_ctx["view"] = vi
		_set_pose(view["pose"])
		_camera.far = float(view.get("far_m", _camera.far))
		_ctx["state"] = ST_SETTLING
		var settle: Dictionary = await _pump_settle(float(sweep.get("settle_deadline_s", 30.0)))
		_ctx["state"] = ST_WARMUP
		await _pump_for(float(sweep.get("warmup_s", 1.0)))
		_ctx["state"] = ST_MEASURE
		var first := _rows
		var t0 := Time.get_ticks_usec()
		var frames: int = await _pump_for(float(sweep.get("measure_s", 3.0)))
		out.append({"view": view["id"], "view_index": vi, "settle": settle, "first_row": first,
				"last_row": _rows - 1, "frames": frames, "measure_usec": Time.get_ticks_usec() - t0})
	_result["sweep"] = out
	return ""


# ------------------------------------------------------------------ main

func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var plan_path := ""
	for i in range(args.size()):
		if args[i] == "--plan" and i + 1 < args.size():
			plan_path = args[i + 1]
	if plan_path.is_empty():
		_fail_usage("--plan <file> is required")
		return
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(plan_path))
	if typeof(parsed) != TYPE_DICTIONARY:
		_fail_usage("plan %s is not a JSON object" % plan_path)
		return
	_plan = parsed
	if bool(_plan.get("shared_layers_t0", false)):
		FIELDS += SHARED_LAYER_FIELDS
	_result = {"schema": RESULT_SCHEMA, "mode": _plan.get("mode"), "plan": plan_path,
			"start_usec": Time.get_ticks_usec(), "adapter": RenderingServer.get_video_adapter_name(),
			"adapter_vendor": RenderingServer.get_video_adapter_vendor(),
			"api_version": RenderingServer.get_video_adapter_api_version(),
			"is_debug_build": OS.is_debug_build(), "engine_version": Engine.get_version_info(),
			"display_server": DisplayServer.get_name()}
	if DisplayServer.get_name() == "headless":
		_finish(false, "headless display server: the probe needs a GPU-backed window")
		return

	var settings: Dictionary = _plan.get("project_settings", {})
	for key in settings.keys():
		ProjectSettings.set_setting(key, settings[key])
	ProjectSettings.emit_signal("settings_changed")
	var readback := {}
	for key in settings.keys():
		readback[key] = ProjectSettings.get_setting(key)
	_result["project_settings_readback"] = readback

	var res: Array = _plan.get("resolution", [1920, 1080])
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_result["vsync_mode"] = DisplayServer.window_get_vsync_mode()
	# 0 = uncapped (every measurement). A cap is only used to capture compact test fixtures.
	Engine.max_fps = int(_plan.get("max_fps", 0))
	_result["max_fps"] = Engine.max_fps
	DisplayServer.window_set_size(Vector2i(int(res[0]), int(res[1])))
	get_root().size = Vector2i(int(res[0]), int(res[1]))
	get_root().use_taa = false
	get_root().msaa_3d = Viewport.MSAA_DISABLED

	if typeof(_plan.get("screenshots")) == TYPE_DICTIONARY:
		_shots = _plan["screenshots"]
		var shot_dir := str(_shots.get("dir", ""))
		if not shot_dir.is_empty() and DirAccess.make_dir_recursive_absolute(shot_dir) != OK:
			_finish(false, "cannot create the screenshot directory %s" % shot_dir)
			return
	if not _open_trace(str(_plan["trace"])):
		_finish(false, "cannot open the trace file")
		return

	var cam_cfg: Dictionary = _plan.get("camera", {})
	_scene = Node3D.new()
	get_root().add_child(_scene)
	_camera = Camera3D.new()
	_camera.keep_aspect = Camera3D.KEEP_WIDTH if str(cam_cfg.get("keep", "width")) == "width" else Camera3D.KEEP_HEIGHT
	_camera.fov = float(cam_cfg.get("fov_deg", 107.5))
	_camera.near = float(cam_cfg.get("near_m", 0.05))
	_camera.far = float(cam_cfg.get("far_m", 400.0))
	_scene.add_child(_camera)
	_camera.make_current()

	var t_load := Time.get_ticks_usec()
	var world_res = ResourceLoader.load(str(_plan["world"]))
	_result["load_usec"] = Time.get_ticks_usec() - t_load
	if world_res == null or not (world_res is GaussianSplatWorld):
		_finish(false, "cannot load the world %s" % _plan["world"])
		return
	var node_cfg: Dictionary = _plan.get("node", {})
	_node = GaussianSplatWorld3D.new()
	_node.set_auto_apply_on_ready(false)
	_node.world = world_res
	_node.set_max_splat_count(int(node_cfg.get("max_splat_count", 4000000)))
	_node.set_max_render_distance(float(node_cfg.get("max_render_distance_m", 400.0)))
	if node_cfg.has("lod_enabled"):
		_node.set_lod_enabled(bool(node_cfg["lod_enabled"]))
	_scene.add_child(_node)
	var first_pose = _plan.get("first_pose")
	if first_pose != null:
		_set_pose(first_pose)
	_result["apply_usec"] = Time.get_ticks_usec()
	_node.apply_world()
	_result["node_readback"] = {"max_splat_count": _node.get_max_splat_count(),
			"max_render_distance": _node.get_max_render_distance(), "lod_enabled": _node.is_lod_enabled()}
	if bool(_plan.get("shared_layers_t0", false)):
		var sidecar_path := str(_plan["world"]).get_basename() + ".manifest.json"
		var sidecar = JSON.parse_string(FileAccess.get_file_as_string(sidecar_path))
		var codec_params = null
		if sidecar is Dictionary:
			codec_params = sidecar.get("codec") if world_res.has_hlod_paged_source() else sidecar.get("writer", {}).get("codec")
		if codec_params is Dictionary:
			codec_params = codec_params.duplicate()
			codec_params.erase("codec")
		var node_readback: Dictionary = _result["node_readback"]
		_result["shared_layers_settings"] = {
			"camera_poses": _plan.get("path", {}).get("segments"),
			"resolution": [get_root().size.x, get_root().size.y],
			"camera": {"fov_deg": _camera.fov, "keep": "width" if _camera.keep_aspect == Camera3D.KEEP_WIDTH else "height",
					"near_m": _camera.near, "far_m": _camera.far},
			"codec": {"encoding": "gsq" if codec_params is Dictionary and int(codec_params.get("codec_id", -1)) == 1 else "unknown",
					"sh_bands": world_res.get_sh_degree(), "params": codec_params,
					"per_chunk_quantization": bool(ProjectSettings.get_setting("rendering/gaussian_splatting/compression/per_chunk_quantization"))},
			"sh_bands": world_res.get_sh_degree(), "visible_splats": _node.get_max_splat_count(),
			"drawn_splats": _node.get_max_splat_count(),
			"timing_windows": {"moving_passes": _plan.get("passes"), "settle": _plan.get("settle")},
			"node_readback": node_readback, "project_settings_readback": readback,
		}
		_result["shared_layers_settings_scope"] = {
			"camera_poses": "process parsed arc-length path; application independently observed in trace px/py/pz/yaw_deg/pitch_deg and checkpoint poses",
			"timing_windows": "process parsed timing configuration; observed segment and settle durations retained separately",
			"codec": "producer sidecar codec parameters, independently verified against the generated file before the experiment and bound to process loaded_file_sha256",
		}
		_result["world_payload_readback"] = {"mode": world_res.get_payload_mode(),
				"has_hlod_paged_source": world_res.has_hlod_paged_source(),
				"sh_degree": world_res.get_sh_degree(), "splat_count": world_res.get_splat_count()}

	var err := ""
	if str(_plan.get("mode")) == "path":
		err = await _run_path()
	elif str(_plan.get("mode")) == "sweep":
		err = await _run_sweep()
	else:
		err = "unknown mode %s" % _plan.get("mode")
	if not err.is_empty():
		_finish(false, err)
		return
	_result["final_render_stats"] = _scalars(_last_stats)
	_finish(true, "plan completed")
