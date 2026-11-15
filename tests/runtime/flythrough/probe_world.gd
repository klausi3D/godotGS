extends SceneTree

# BV-07 probe driver (docs/architecture/adr-beyond-vram-worlds.md section 7, D-4, D-6).
#
#   godot --path <project> --script tests/runtime/flythrough/probe_world.gd -- --plan <plan.json>
#
# run_probe.py writes the plan and parses what this script leaves behind; it is the
# only caller. This script records measurements and checkpoint evidence:
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
# A captured checkpoint and ok=true only mean the plan completed. Checkpoint
# evidence separately requires settlement and count provenance for this pose.
# The chunk route's sort/static_cache_enabled setting (default false) is excluded:
# a cache hit need not publish a fresh count. Read its actual renderer state;
# zero cache hits cannot prove disabled. HLOD bypasses this cache and records that
# applicability separately, including when the getter is unavailable.
#
# Exit codes: 0 the plan ran to the end (the result JSON says what was measured);
# 1 a step failed (reason in the result JSON); 2 usage / plan error.

# Shared with shared_layers_t0.camera_path_sha256. Hash parsed process values,
# not a requested digest; signed zero has one camera representation.
func _camera_hash_value(value) -> Array:
	if value is Dictionary:
		var encoded: Array = ["object"]
		var keys: Array = value.keys()
		keys.sort()
		for key in keys:
			encoded.append([str(key).to_utf8_buffer().hex_encode(), _camera_hash_value(value[key])])
		return encoded
	if value is Array:
		var encoded: Array = ["array"]
		for item in value:
			encoded.append(_camera_hash_value(item))
		return encoded
	if value is int or value is float:
		var bytes := PackedByteArray()
		bytes.resize(4)
		bytes.encode_float(0, float(value))
		if bytes.decode_float(0) == 0.0:
			bytes.fill(0)
		return ["number_f32", bytes.hex_encode()]
	if value == null:
		return ["null"]
	return ["bool", value] if value is bool else ["string", str(value).to_utf8_buffer().hex_encode()]


func _camera_path_sha256(segments) -> String:
	return JSON.stringify(_camera_hash_value(segments)).sha256_text()


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
	["hlod_active", "q"],
	["hlod_stream_desired_nodes", "q"], ["hlod_stream_resident_nodes", "q"],
	["hlod_stream_loading_nodes", "q"], ["hlod_stream_drawn_nodes", "q"],
	["hlod_stream_failed_nodes", "q"], ["hlod_stream_nonresident_drawn", "q"],
	["hlod_stream_blocked_groups", "q"], ["hlod_stream_fragmented_groups", "q"],
	["hlod_stream_uploaded_bytes_this_frame", "q"], ["hlod_stream_uploaded_bytes_total", "q"],
]

# #1445: who spent the wall-clock interval that ENDS at this row (t_usec minus the previous row's
# t_usec), in the order it happens. The four *_usec parts add up to that interval exactly:
#   telemetry  the previous row's _sample_and_write (render-stats query, per-row samples, trace row)
#   capture    viewport readbacks, PNG encode and write (_screenshot) since the previous row
#   script     the rest of this driver's work before it handed control back to the engine
#   engine     from that hand-over to this row's frame_post_draw: the engine's frame, which is
#              what a game without the probe pays (scene step, render CPU, submit, present)
# -1 where the driver could not split the interval (the first row): probe_summary.frame_breakdown
# reads that as not measured, never as zero. render_cpu_ms is the renderer's frame_time_ms and
# renderer_frames_total its total_frames_rendered (a row where it did not advance repeats a held
# value); -1 when the renderer does not publish the key.
const BREAKDOWN_FIELDS := [
	["frame_harness_telemetry_usec", "q"], ["frame_harness_capture_usec", "q"],
	["frame_harness_script_usec", "q"], ["frame_engine_usec", "q"], ["frame_captures", "q"],
	["render_cpu_ms", "d"], ["renderer_frames_total", "q"],
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
] + DROP_FIELDS + BREAKDOWN_FIELDS

var _plan: Dictionary = {}
var _result: Dictionary = {}
var _trace: FileAccess
var _rows := 0
# #1457/#1461: the sampled frames that lost overlap records, read from get_render_stats() (the
# renderer's always-on drop readback; the probe issues none). Kept beside the binary trace, whose
# schema does not change. "not_measured" while the renderer does not publish the counter.
const DROP_FRAME_EVENT_LIMIT := 4096
var _drop_frame_count = "not_measured"
var _drop_frame_events := []
var _drop_frame_events_not_recorded := 0
var _scene: Node3D
var _camera: Camera3D
var _node: GaussianSplatWorld3D
var _last_stats: Dictionary = {}
var _last_streaming: Dictionary = {}
var _shared_layers_peaks: Dictionary = {}
var _shared_layers_payload_peaks: Dictionary = {}
var _shared_layers_settings_samples: Array = []
var _first_engine_frame := -1
var _last_engine_frame := -1
# Viewport captures (plan "screenshots": {"dir", "moving_every_n"}); see _screenshot().
var _shots := {}
var _shot_log := []
# Per-row context set by the phase code.
var _ctx := {"phase": 0, "pass_index": 0, "segment": -1, "view": -1, "state": ST_LOADING, "s_m": 0.0}
# #1445 frame breakdown, all in Time.get_ticks_usec(): the previous row's t_usec (-1 before the
# first row), how long its _sample_and_write took, the capture time and count since then, and
# when this driver last handed control back to the engine (_frame()).
var _iv_prev_t_usec := -1
var _iv_telemetry_usec := 0
var _iv_capture_usec := 0
var _iv_captures := 0
var _iv_yield_usec := -1
# Native counters, separate from the binary trace schema. Retain requested-pose
# context because render stats can lag the probe by 1-2 frames at window boundaries.
var _sync_cut_count = "not_measured"
var _sync_cut_events := []
var _sync_cut_pose_windows := []


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
	_result["static_sort_cache"] = _static_sort_cache_state()
	_result["checkpoint_evidence"] = _checkpoint_evidence_result()
	_result["overlap_sync_cuts"] = {"count": _sync_cut_count, "events": _sync_cut_events}
	_result["gsq_decode_stats"] = _gsq_decode_stats()
	_result["overlap_drop_frames"] = {"count": _drop_frame_count, "events": _drop_frame_events,
			"events_not_recorded": _drop_frame_events_not_recorded}
	if bool(_plan.get("shared_layers_t0", false)):
		_result["shared_layers_peaks"] = _shared_layers_peaks
		_result["shared_layers_payload_peaks"] = _shared_layers_payload_peaks
		_result["shared_layers_settings_samples"] = _shared_layers_settings_samples
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


func _static_sort_cache_state() -> Dictionary:
	# Observe configuration, never infer it from counters. The chunk cache is
	# bypassed by the HLOD route (render_sorting_orchestrator's HLOD branch).
	var enabled = null
	var renderer = _node.get_renderer() if _node != null else null
	if renderer != null and renderer.has_method("is_static_sort_cache_enabled"):
		enabled = renderer.call("is_static_sort_cache_enabled")
	return {"enabled": enabled, "applicable": not _is_hlod_payload_route()}


func _checkpoint_evidence_verdict(settle: Dictionary, cache: Dictionary) -> String:
	if str(settle.get("reason", "")) == "settle_deadline_missed" \
			or bool(settle.get("forward_progress", {}).get("deadline_missed", false)):
		return "checkpoint_settle_deadline_missed"
	if settle.get("settled") != true or not _count_sample_is_fresh(_last_stats, _pose_first_frame) \
			or not _count_sample_is_after_streak(_last_stats, settle.get("streak_first_frame")):
		return "checkpoint_unsettled"
	if bool(cache.get("applicable", true)):
		if cache.get("enabled") == true:
			return "static_sort_cache_unsupported"
		if cache.get("enabled") == null:
			return "static_sort_cache_state_unavailable"
	return "VALID"


func _checkpoint_evidence_result() -> Dictionary:
	var reasons := []
	var checkpoints := []
	for checkpoint in _result.get("checkpoints", []):
		var verdict := str(checkpoint.get("evidence_verdict", "checkpoint_unsettled"))
		checkpoints.append({"checkpoint": checkpoint.get("checkpoint"),
				"pass_index": checkpoint.get("pass_index"), "evidence_verdict": verdict,
				"evidence_reasons": checkpoint.get("evidence_reasons", [])})
		for checkpoint_reason in checkpoint.get("evidence_reasons", [verdict]):
			if checkpoint_reason != "VALID" and not reasons.has(checkpoint_reason):
				reasons.append(checkpoint_reason)
	if checkpoints.is_empty():
		reasons.append("checkpoint_unsettled")
	return {"verdict": "VALID" if reasons.is_empty() else "UNUSABLE",
			"reasons": reasons, "checkpoints": checkpoints}


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
			# The renderer adds the real chunk upload-window/publication owner to
			# the source's page-store counters. Live identity is queried separately
			# so a cached renderer sample cannot hide source replacement.
			var payload = _last_stats.get("payload_telemetry")
			if not (payload is Dictionary and payload.get("route") == "chunk"):
				payload = source.get_decode_stats()
			return {"route": "chunk", "source_generation": source.get_instance_id(),
					"payload_telemetry": payload}
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


# #1457/#1461: record the renderer's latest sampled dropping frame whenever its counter advanced.
# The renderer keeps only the latest one, so frames it sampled between two probe rows are
# counted in events_not_recorded, never described by a neighbour. observed_* is this row's
# context: the sample is a frame or more older than the row that sees it.
func _sample_drop_frames(stats: Dictionary) -> void:
	if not stats.has("drop_frame_samples"):
		return
	var count := int(stats["drop_frame_samples"])
	var previous := int(_drop_frame_count) if _drop_frame_count is int else 0
	_drop_frame_count = count
	if count <= previous:
		return
	var frame = stats.get("drop_frame")
	if not (frame is Dictionary) or _drop_frame_events.size() >= DROP_FRAME_EVENT_LIMIT:
		_drop_frame_events_not_recorded += count - previous
		return
	_drop_frame_events_not_recorded += count - previous - 1
	_drop_frame_events.append({"count": count, "observed_row": _rows, "observed_phase": _ctx["phase"],
			"observed_pass_index": _ctx["pass_index"], "observed_segment": _ctx["segment"],
			"observed_state": _ctx["state"], "frame": frame})


func _sample_and_write() -> void:
	var t_usec := Time.get_ticks_usec()
	var stats := {}
	var r = _node.get_renderer() if _node != null else null
	var t_stats := Time.get_ticks_usec()
	if r != null:
		stats = r.get_render_stats()
	var stats_usec := Time.get_ticks_usec() - t_stats
	_last_stats = stats
	_sample_sync_cuts(stats)
	_update_pose_first_frame()
	var ss: Dictionary = stats.get("streaming_state", {}) if stats.get("streaming_state") is Dictionary else {}
	_last_streaming = ss
	_sample_drop_frames(stats)
	if bool(_plan.get("shared_layers_t0", false)):
		# Actual current node/camera/project policy for every trace row, including
		# HLOD where chunk regulator revision fields do not exist.
		var policy_readback := {}
		for key in _plan.get("project_settings", {}).keys():
			policy_readback[key] = ProjectSettings.get_setting(key)
		_shared_layers_settings_samples.append({"row": _rows, "project_settings_readback": policy_readback,
				"node_readback": {"max_splat_count": _node.get_max_splat_count(),
						"max_render_distance": _node.get_max_render_distance(), "lod_enabled": _node.is_lod_enabled(),
						"hlod_tau_px": _node.get_hlod_tau_px(), "lod_bias": _node.get_lod_bias()},
				"camera": {"fov_deg": _camera.fov, "keep": "width" if _camera.keep_aspect == Camera3D.KEEP_WIDTH else "height",
						"near_m": _camera.near, "far_m": _camera.far},
				"resolution": [get_root().size.x, get_root().size.y], "sh_bands": _node.world.get_sh_degree(),
				"visible_splats": _node.get_max_splat_count(), "drawn_splats": _node.get_max_splat_count()})
		for key in ["hlod_stream_nonresident_drawn", "hlod_stream_stall_frames", "hlod_stream_blocked_groups",
				"hlod_stream_failed_total", "hlod_stream_late_dropped_total", "hlod_stream_drawn_nodes",
				"hlod_stream_resident_nodes", "rendered_splat_count", "instance_count_overflow_events",
				"overflow_drop_events", "binning_drop_events", "raster_tile_cap_drop_events"]:
			if stats.has(key):
				_shared_layers_peaks[key] = max(int(_shared_layers_peaks.get(key, stats[key])), int(stats[key]))
		var payload = _payload_telemetry_result().get("payload_telemetry")
		if payload is Dictionary:
			# Keep transient failures/pressure observable even if later frames drain
			# the queues. These are actual native values, never requested controls.
			for key in ["read_failures", "page_failures", "captures_failed", "digest_verification_failures",
					"stale_generation_publications", "reads_refused_busy", "admission_waits", "upload_hold_frames"]:
				if payload.has(key):
					_shared_layers_payload_peaks[key] = max(int(_shared_layers_payload_peaks.get(key, payload[key])), int(payload[key]))
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
	values.append_array(_frame_breakdown_values(t_usec, stats))
	if bool(_plan.get("shared_layers_t0", false)):
		for f in SHARED_LAYER_FIELDS:
			values.append(int(_g(stats, f[0], -1)))
	for i in range(FIELDS.size()):
		if FIELDS[i][1] == "q":
			_trace.store_64(int(values[i]))
		else:
			_trace.store_double(float(values[i]))
	_rows += 1
	# The next row's interval starts at this row's t_usec; everything up to here is its telemetry.
	_iv_prev_t_usec = t_usec
	_iv_capture_usec = 0
	_iv_captures = 0
	_iv_telemetry_usec = Time.get_ticks_usec() - t_usec


# BREAKDOWN_FIELDS for the row sampled at t_usec, in order. The parts are measured, never
# derived from each other except script (the hand-over minus the two timed parts), so they add up
# to t_usec - _iv_prev_t_usec by construction; probe_summary.frame_breakdown re-checks that.
func _frame_breakdown_values(t_usec: int, stats: Dictionary) -> Array:
	var parts := [-1, -1, -1, -1, -1]
	if _iv_prev_t_usec >= 0 and _iv_yield_usec >= _iv_prev_t_usec + _iv_telemetry_usec + _iv_capture_usec \
			and t_usec >= _iv_yield_usec:
		parts = [_iv_telemetry_usec, _iv_capture_usec,
				_iv_yield_usec - _iv_prev_t_usec - _iv_telemetry_usec - _iv_capture_usec,
				t_usec - _iv_yield_usec, _iv_captures]
	parts.append(float(_g(stats, "frame_time_ms", -1.0)))
	parts.append(int(_g(stats, "total_frames_rendered", -1)))
	return parts


func _frame() -> void:
	# The driver's own work ends here: from this stamp to the resume below is the engine's frame.
	_iv_yield_usec = Time.get_ticks_usec()
	await RenderingServer.frame_post_draw
	_sample_and_write()


# ------------------------------------------------------------------ helpers

func _set_pose(p: Array) -> void:
	if _camera.global_position != Vector3(float(p[0]), float(p[1]), float(p[2])) \
			or _camera.rotation_degrees != Vector3(float(p[4]), float(p[3]), 0.0):
		_pose_first_frame = -1
	_camera.global_position = Vector3(float(p[0]), float(p[1]), float(p[2]))
	_camera.rotation_degrees = Vector3(float(p[4]), float(p[3]), 0.0)
	var context := {"pass_index": _ctx["pass_index"], "window": "not_measured", "segment": "not_measured"}
	var segments: Array = _plan.get("path", {}).get("segments", [])
	var si := int(_ctx["segment"])
	if int(_ctx["phase"]) == 0 and si >= 0 and si < segments.size():
		context["segment"] = segments[si]["id"]
		context["window"] = "moving" if segments[si]["kind"] == "move" else segments[si]["kind"]
	_sync_cut_pose_windows.append({"transform": _camera.global_transform, "context": context})
	if _sync_cut_pose_windows.size() > 16:
		_sync_cut_pose_windows.pop_front()


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


func _sample_sync_cuts(stats: Dictionary) -> void:
	if not stats.has("overlap_sync_cut_count"):
		_sync_cut_count = "not_measured"
		return
	var count := int(stats["overlap_sync_cut_count"])
	var previous := int(_sync_cut_count) if _sync_cut_count is int else 0
	_sync_cut_count = count
	if count <= previous:
		return
	var event := {"count": count, "frame_serial": stats.get("overlap_sync_cut_last_frame", "not_measured"),
			"cpu_wall_ms": stats.get("overlap_sync_cut_readback_cpu_wall_ms", "not_measured"),
			"observed_row": _rows, "window": "not_measured", "segment": "not_measured"}
	var native_transform = stats.get("overlap_sync_cut_transform")
	if native_transform is Transform3D:
		var position: Vector3 = native_transform.origin
		event["camera_position"] = [position.x, position.y, position.z]
		# Match the native cut pose, never simply label a delayed sample with the
		# probe's current segment. An unmatched pose stays explicitly not_measured.
		for i in range(_sync_cut_pose_windows.size() - 1, -1, -1):
			var sample: Dictionary = _sync_cut_pose_windows[i]
			if native_transform.is_equal_approx(sample["transform"]):
				event.merge(sample["context"], true)
				break
	_sync_cut_events.append(event)


# Distance between the camera the last render stats describe and the probe camera. The
# stats lag the pose by a frame or two, so a settle observed before they agree would be
# the previous pose's settle (found on the first T0 run: a teleport "settled" in 5 ms).
func _stats_pose_error() -> float:
	var st := _last_stats
	if not st.has("debug_cam_origin_x"):
		return INF
	var o := Vector3(float(st["debug_cam_origin_x"]), float(st["debug_cam_origin_y"]), float(st["debug_cam_origin_z"]))
	return o.distance_to(_camera.global_position)


func _stats_orientation_matches() -> bool:
	# basis[0][0] alone cannot distinguish yaw signs or pitch. These two vectors
	# come from the same native camera snapshot as debug_cam_origin_*.
	for key in ["debug_cam_forward_x", "debug_cam_forward_y", "debug_cam_forward_z",
			"debug_cam_up_x", "debug_cam_up_y", "debug_cam_up_z"]:
		if not _last_stats.has(key):
			return false
	var forward := Vector3(float(_last_stats["debug_cam_forward_x"]), float(_last_stats["debug_cam_forward_y"]), float(_last_stats["debug_cam_forward_z"]))
	var up := Vector3(float(_last_stats["debug_cam_up_x"]), float(_last_stats["debug_cam_up_y"]), float(_last_stats["debug_cam_up_z"]))
	return forward.distance_to(-_camera.global_transform.basis.z) <= 0.001 \
			and up.distance_to(_camera.global_transform.basis.y) <= 0.001


var _pose_first_frame: int = -1


func _update_pose_first_frame() -> void:
	if _stats_pose_error() > 0.001 or not _stats_orientation_matches() \
			or typeof(_last_stats.get("render_frame_counter")) != TYPE_INT \
			or int(_last_stats.get("render_frame_counter", -1)) < 0:
		_pose_first_frame = -1
		return
	if _pose_first_frame < 0:
		_pose_first_frame = int(_last_stats["render_frame_counter"])


func _count_sample_is_fresh(stats: Dictionary, pose_first_frame: int) -> bool:
	var source_frame = stats.get("rendered_splat_count_source_frame", -1)
	var render_frame = stats.get("render_frame_counter", -1)
	if typeof(source_frame) != TYPE_INT or typeof(render_frame) != TYPE_INT:
		return false
	return pose_first_frame >= 0 and render_frame >= pose_first_frame \
			and source_frame >= pose_first_frame and source_frame <= render_frame


func _count_sample_is_after_streak(stats: Dictionary, streak_first_frame: Variant) -> bool:
	var source_frame = stats.get("rendered_splat_count_source_frame", -1)
	var render_frame = stats.get("render_frame_counter", -1)
	if typeof(streak_first_frame) != TYPE_INT or typeof(source_frame) != TYPE_INT \
			or typeof(render_frame) != TYPE_INT:
		return false
	# A readback sourced at the first settled frame can still precede its final
	# upload arrival. Require a later source frame within the current streak.
	return streak_first_frame >= 0 and source_frame > streak_first_frame and source_frame <= render_frame


func _settle_digest_is_stable(stats: Dictionary, streak_digest: Variant) -> bool:
	var digest = stats.get("hlod_drawn_node_digest")
	# Older binaries and the chunk route may not export a drawn-node digest.
	return typeof(digest) != TYPE_STRING or digest == streak_digest


func _settled() -> bool:
	# Flat settle condition (ADR section 7.3 pass B): missing chunks = 0, on a ready route,
	# with something drawn, in stats that describe this pose. A missing key reads as not
	# settled, never as settled.
	# "Missing" means not GPU-resident: needed_not_resident_chunks counts queued, in-flight
	# and loaded-but-not-uploaded chunks. needed_unserved_chunks leaves those out, so an
	# oracle on it settled while chunks were still in flight (holes on screen).
	if _stats_pose_error() > 0.001 or not _stats_orientation_matches():
		return false
	if not _count_sample_is_fresh(_last_stats, _pose_first_frame):
		return false
	if bool(_plan.get("shared_layers_t0", false)) and _is_hlod_payload_route():
		# Diagnostic HLOD convergence only. Never substitute chunk counts or
		# claim the separate G6/G6t cut-settle acceptance oracle has run.
		if not bool(_last_stats.get("hlod_active", false)):
			return false
		for key in ["hlod_stream_desired_nodes", "hlod_stream_loading_nodes", "hlod_stream_failed_nodes", "hlod_stream_nonresident_drawn",
				"hlod_stream_drawn_nodes", "hlod_stream_blocked_groups", "hlod_stream_fragmented_groups"]:
			if not _last_stats.has(key):
				return false
		# Desired is new demand (excluding keep records); resident includes the
		# entire cache. Equality would reject a healthy already-resident cut.
		return int(_last_stats["hlod_stream_desired_nodes"]) == 0 \
				and int(_last_stats["hlod_stream_loading_nodes"]) == 0 \
				and int(_last_stats["hlod_stream_failed_nodes"]) == 0 \
				and int(_last_stats["hlod_stream_nonresident_drawn"]) == 0 \
				and int(_last_stats["hlod_stream_blocked_groups"]) == 0 \
				and int(_last_stats["hlod_stream_fragmented_groups"]) == 0 \
				and int(_last_stats["hlod_stream_drawn_nodes"]) > 0
	var ss := _last_streaming
	if not ss.has("needed_not_resident_chunks") or not ss.has("needed_chunks"):
		return false
	if int(ss["needed_not_resident_chunks"]) != 0 or int(ss["needed_chunks"]) <= 0:
		return false
	if not bool(ss.get("render_readiness_ready", false)):
		return false
	return int(_last_stats.get("rendered_splat_count", 0)) > 0


# Pump until _settled() holds on SETTLE_STREAK consecutive frames with a stable drawn
# digest (when available) and a count sourced strictly after the streak's first render
# frame. The accepting frame must end before the deadline; receipts include the
# streak_first_frame and optional streak_digest alongside the existing settle fields.
# settle_usec is measured to the first frame of the streak (when the condition began to
# hold); the streak is a fixed lower bound, never an upper one.
const SETTLE_STREAK := 3


func _shared_layers_progress_snapshot() -> Dictionary:
	var route := "hlod" if _is_hlod_payload_route() else "chunk"
	var sample := {"row": _rows - 1, "t_usec": Time.get_ticks_usec(), "route": route, "measured": false}
	var payload = _payload_telemetry_result().get("payload_telemetry")
	if not (payload is Dictionary and payload.has("reads_completed_ok") and payload.has("pages_decoded")):
		sample["reason"] = "native read/decode completion observations unavailable"
		return sample
	var counts := {"read_completions": int(payload["reads_completed_ok"]), "pages_decoded": int(payload["pages_decoded"])}
	if route == "hlod":
		if not (payload.has("upload_completions") and payload.has("upload_bytes") \
				and _last_stats.has("hlod_stream_desired_nodes") and _last_stats.has("hlod_stream_loading_nodes")):
			sample["reason"] = "HLOD completion or outstanding-demand observation unavailable"
			return sample
		sample["completion_count"] = int(payload["upload_completions"])
		counts["upload_bytes"] = int(payload["upload_bytes"])
		sample["demand_outstanding"] = int(_last_stats["hlod_stream_desired_nodes"]) > 0 \
				or int(_last_stats["hlod_stream_loading_nodes"]) > 0
		sample["scope"] = "native HLOD upload completions and desired/loading demand"
	else:
		if not (_last_streaming.has("upload_chunks_total") and _last_streaming.has("upload_bytes_total") \
				and _last_streaming.has("needed_not_resident_chunks")):
			sample["reason"] = "chunk upload completion or outstanding-demand observation unavailable"
			return sample
		sample["completion_count"] = int(_last_streaming["upload_chunks_total"])
		counts["upload_bytes"] = int(_last_streaming["upload_bytes_total"])
		sample["demand_outstanding"] = int(_last_streaming["needed_not_resident_chunks"]) > 0
		sample["scope"] = "native chunk GPU uploads and needed-not-resident demand"
	counts["upload_completions"] = sample["completion_count"]
	for count in counts.values():
		if int(count) < 0:
			sample["reason"] = "negative native forward-progress observation"
			return sample
	sample["progress_counts"] = counts
	sample["measured"] = true
	return sample


func _shared_layers_progress_result(samples: Array, start: int, end: int, deadline_s: float,
		window: String, deadline_missed: bool) -> Dictionary:
	var progress := {"measured": false, "window": window, "deadline_s": deadline_s,
			"elapsed_usec": end - start, "deadline_missed": deadline_missed, "samples": samples,
			"scope": "initial native snapshot and every frame across the existing settle window"}
	if samples.is_empty():
		progress["reason"] = "no native forward-progress samples"
		return progress
	var first: Dictionary = samples[0]
	var observed_usec := int(samples[samples.size() - 1]["t_usec"]) - int(first["t_usec"])
	progress["observed_usec"] = observed_usec
	var previous := {}
	var demand_throughout := true
	for sample in samples:
		if not bool(sample.get("measured", false)) or sample.get("route") != first.get("route"):
			progress["reason"] = "native forward-progress observation unavailable or route changed"
			return progress
		var counts: Dictionary = sample["progress_counts"]
		for key in counts:
			if previous.has(key) and int(counts[key]) < int(previous[key]):
				progress["reason"] = "native progress counter decreased during the settle window"
				return progress
		previous = counts
		demand_throughout = demand_throughout and bool(sample["demand_outstanding"])
	progress["measured"] = true
	progress["route"] = first["route"]
	progress["start_completions"] = int(first["completion_count"])
	progress["end_completions"] = int(previous["upload_completions"])
	progress["completion_delta"] = int(previous["upload_completions"]) - int(first["completion_count"])
	var deltas := {}
	var any_progress := false
	for key in previous:
		deltas[key] = int(previous[key]) - int(first["progress_counts"][key])
		any_progress = any_progress or int(deltas[key]) > 0
	progress["progress_deltas"] = deltas
	progress["demand_outstanding_throughout"] = demand_throughout
	progress["no_forward_progress"] = deadline_missed and end - start >= int(deadline_s * 1000000.0) \
			and observed_usec >= int(deadline_s * 1000000.0) and demand_throughout and not any_progress
	return progress


func _pump_settle(deadline_s: float, window: String = "") -> Dictionary:
	var start := Time.get_ticks_usec()
	var deadline := start + int(deadline_s * 1000000.0)
	var frames := 0
	var streak := 0
	var streak_start := 0
	var streak_first_frame := -1
	var streak_digest = null
	var progress_samples := []
	var capture_progress := bool(_plan.get("shared_layers_t0", false))
	if capture_progress:
		progress_samples.append(_shared_layers_progress_snapshot())
	while true:
		await _frame()
		_update_pose_first_frame()
		frames += 1
		var now := Time.get_ticks_usec()
		if capture_progress:
			progress_samples.append(_shared_layers_progress_snapshot())
		if now > deadline:
			var missed := {"settled": false, "settle_usec": -1, "frames": frames, "deadline_s": deadline_s,
					"reason": "settle_deadline_missed", "pose_error_m": _stats_pose_error(),
					"streak_first_frame": streak_first_frame, "streak_digest": streak_digest}
			if capture_progress:
				missed["forward_progress"] = _shared_layers_progress_result(progress_samples, start, now, deadline_s, window, true)
			return missed
		if _settled():
			if streak == 0 or not _settle_digest_is_stable(_last_stats, streak_digest):
				streak = 0
				streak_start = now
				streak_first_frame = int(_last_stats["render_frame_counter"])
				var digest = _last_stats.get("hlod_drawn_node_digest")
				streak_digest = digest if typeof(digest) == TYPE_STRING else null
			streak += 1
			if streak >= SETTLE_STREAK and _count_sample_is_after_streak(_last_stats, streak_first_frame):
				var settled := {"settled": true, "settle_usec": streak_start - start, "frames": frames,
						"deadline_s": deadline_s, "pose_error_m": _stats_pose_error(),
						"streak_first_frame": streak_first_frame, "streak_digest": streak_digest}
				if capture_progress:
					settled["forward_progress"] = _shared_layers_progress_result(progress_samples, start, now, deadline_s, window, false)
				return settled
		else:
			streak = 0
			streak_first_frame = -1
			streak_digest = null
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
	var cache := _static_sort_cache_state()
	var verdict := _checkpoint_evidence_verdict(settle, cache)
	var reasons := []
	if verdict != "VALID":
		reasons.append(verdict)
	# Keep cache exclusion visible even when settlement also missed its deadline.
	if bool(cache.get("applicable", true)):
		var cache_reason := ""
		if cache.get("enabled") == true:
			cache_reason = "static_sort_cache_unsupported"
		elif cache.get("enabled") == null:
			cache_reason = "static_sort_cache_state_unavailable"
		if not cache_reason.is_empty() and not reasons.has(cache_reason):
			reasons.append(cache_reason)
	return {
		"checkpoint": name, "segment": seg_id, "pass_index": _ctx["pass_index"], "settle": settle,
		"static_sort_cache": cache, "evidence_verdict": verdict, "evidence_reasons": reasons,
		"pose": [_camera.global_position.x, _camera.global_position.y, _camera.global_position.z,
				_camera.rotation_degrees.y, _camera.rotation_degrees.x],
		"drawn_splats": int(_last_stats.get("rendered_splat_count", -1)),
		"rendered_splat_count_source_frame": _last_stats.get("rendered_splat_count_source_frame", -1),
		"render_frame_counter": _last_stats.get("render_frame_counter", -1),
		"pose_first_frame": _pose_first_frame,
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
		# Optional cut diagnostics: older binaries must retain null, never an invented empty cut.
		"hlod_drawn_node_ids": _last_stats.get("hlod_drawn_node_ids"),
		"hlod_drawn_node_digest": _last_stats.get("hlod_drawn_node_digest"),
		"hlod_cut_hysteresis_held": _last_stats.get("hlod_cut_hysteresis_held"),
		"hlod_cut_budget_bound": _last_stats.get("hlod_cut_budget_bound"),
		"hlod_cut_first_cap_refused_node": _last_stats.get("hlod_cut_first_cap_refused_node"),
		"hlod_cut_residency_missed": _last_stats.get("hlod_cut_residency_missed"),
		"hlod_cut_residency_missed_demanded": _last_stats.get("hlod_cut_residency_missed_demanded"),
	}


# ------------------------------------------------------------------ screenshots

# The viewport as drawn on the frame the last trace row describes: nothing is drawn between
# _frame() returning and this readback. Off unless the plan names a directory. Returns {} when
# off, else the log entry ("file" on success, "error" otherwise). The readback stalls the
# process; a checkpoint caller renders one HOLD frame afterwards so the stall lands in a hold
# row, never in a moving row's frame interval. Moving captures (moving_every_n) do perturb the
# moving intervals; the result records that they were on, and each row's frame_harness_capture_usec
# says by how much (#1445).
func _screenshot(name: String) -> Dictionary:
	var dir := str(_shots.get("dir", ""))
	if dir.is_empty():
		return {}
	var entry := {"name": name, "row": _rows - 1, "pass_index": _ctx["pass_index"],
			"segment": _ctx["segment"], "state": _ctx["state"]}
	var t_capture := Time.get_ticks_usec()
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
	# #1445: the readback, encode and write are this driver's work, charged to the capture part of
	# the next row's interval (BREAKDOWN_FIELDS) and recorded per capture.
	entry["capture_usec"] = Time.get_ticks_usec() - t_capture
	_iv_capture_usec += int(entry["capture_usec"])
	_iv_captures += 1
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
				var settle: Dictionary = await _pump_settle(float(settle_cfg.get(key, 10.0)), key)
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

# This runs only in the additional post-measurement saturation arm. The ordinary
# path never invokes the fault seam. Pumping remains deadline-bound and an upload
# hold counts only once a real packed READY output is waiting for transfer.
func _run_shared_layers_saturation() -> String:
	var start := Time.get_ticks_usec()
	var deadline := start + 30000000
	var receipt := {"requested": "saturate", "hook_applied": false, "observed": false,
			"deadline_s": 30.0}
	var renderer = null
	while true:
		renderer = _node.get_renderer()
		if not bool(receipt["hook_applied"]) and renderer != null \
				and renderer.has_method("_test_set_shared_layers_saturation"):
			var err: int = renderer.call("_test_set_shared_layers_saturation", true)
			if err == OK:
				receipt["hook_applied"] = true
		await _frame()
		var now := Time.get_ticks_usec()
		var payload = _last_stats.get("payload_telemetry")
		if now > deadline:
			if renderer != null and renderer.has_method("_test_set_shared_layers_saturation"):
				renderer.call("_test_set_shared_layers_saturation", false)
			receipt["elapsed_usec"] = now - start
			_result["shared_layers_control"] = receipt
			return "saturation deadline missed: no native ready-work upload hold"
		if bool(receipt["hook_applied"]) and payload is Dictionary \
				and int(payload.get("upload_hold_frames", 0)) > 0 and int(payload.get("ready_bytes", 0)) > 0:
			receipt["observed"] = true
			receipt["elapsed_usec"] = now - start
			receipt["payload_telemetry"] = payload.duplicate(true)
			_screenshot("saturation_ready_upload_held")
			renderer.call("_test_set_shared_layers_saturation", false)
			_result["shared_layers_control"] = receipt
			return ""
	return "saturation loop ended without an observation"

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
	if node_cfg.has("hlod_tau_px"):
		_node.set_hlod_tau_px(float(node_cfg["hlod_tau_px"]))
	if node_cfg.has("lod_bias"):
		_node.set_lod_bias(float(node_cfg["lod_bias"]))
	_scene.add_child(_node)
	var first_pose = _plan.get("first_pose")
	if first_pose != null:
		_set_pose(first_pose)
	_result["apply_usec"] = Time.get_ticks_usec()
	_node.apply_world()
	_result["node_readback"] = {"max_splat_count": _node.get_max_splat_count(),
			"max_render_distance": _node.get_max_render_distance(), "lod_enabled": _node.is_lod_enabled()}
	if bool(_plan.get("shared_layers_t0", false)):
		_result["node_readback"]["hlod_tau_px"] = _node.get_hlod_tau_px()
		_result["node_readback"]["lod_bias"] = _node.get_lod_bias()
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
			"camera_path_sha256": _camera_path_sha256(_plan.get("path", {}).get("segments")),
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
			"camera_path_sha256": "SHA256 of typed JSON/f32 parsed path; signed zero normalized; independently checked from process camera_poses",
			"camera_poses": "process parsed arc-length path; application independently observed in trace px/py/pz/yaw_deg/pitch_deg and checkpoint poses",
			"timing_windows": "process parsed timing configuration; observed segment and settle durations retained separately",
			"codec": "producer sidecar codec parameters, independently verified against the generated file before the experiment and bound to process loaded_file_sha256",
		}
		_result["world_payload_readback"] = {"mode": world_res.get_payload_mode(),
				"has_hlod_paged_source": world_res.has_hlod_paged_source(),
				"sh_degree": world_res.get_sh_degree(), "splat_count": world_res.get_splat_count()}

	var err := ""
	if bool(_plan.get("shared_layers_t0", false)) and _plan.get("shared_layers_control") == "saturate":
		err = await _run_shared_layers_saturation()
	elif str(_plan.get("mode")) == "path":
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
