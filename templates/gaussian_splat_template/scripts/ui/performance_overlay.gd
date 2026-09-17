extends MarginContainer
class_name GaussianPerformanceOverlay

## Runtime performance overlay for the starter template.
##
## Contract for every row in this panel (#833): a displayed number is a
## measurement of the thing its label names, or it is not displayed. Concretely:
##
## * A quantity this build cannot measure renders as `n/a`, never as `0`. A
##   reader cannot tell a measured 0 from a missing producer, so a `0` in this
##   panel always means "measured, and it was zero".
## * A GPU pass time is shown only when the renderer's own validity flag for
##   that pass is set on the frame being reported. Those flags live in
##   `GaussianSplatNode3D.get_statistics()` (`gpu_*_valid`); the custom-monitor
##   layer drops them, which is why the pass rows are sourced from
##   `get_statistics()` and not from `Performance.get_custom_monitor()`.
## * Labels name the pass that is actually measured. The row this overlay used
##   to call "GPU binning" is the overlap-EMIT pass -- `gpu_binning_ms` and
##   `gpu_tile_overlap_emit_time_ms` are bit-identical (measured: equal on
##   368/368 frames, same 78 distinct values). The row it called "GPU frustum
##   cull" is a host wall-clock measurement of the cull stage
##   (`gpu_culler.cpp:1441` uses `OS::get_ticks_usec()`), not a GPU timestamp,
##   so it lives under HOST STAGES.
## * The pass rows add up to the pass total. All six resolved passes are shown;
##   the previous panel showed four of six against a total of all six, so the
##   visible rows could never sum to the visible total. When the sum and the
##   reported total disagree, the discrepancy is printed rather than hidden.
## * Rows belonging to a subsystem that is not running say so once, at the
##   section header, instead of printing a screen of zeros. `streaming_monitor_ready`
##   is the availability flag.
##
## Sources, and why each one:
## * `GaussianSplatNode3D.get_statistics()` -- per-node, per-frame, carries the
##   validity flags and the route/domain strings. Primary source.
## * `Performance.get_custom_monitor()` -- process-global values that are not
##   per-node (device VRAM, the streaming/LOD groups). Always read through
##   `_monitor()`, which returns `null` for an id this build does not register.

@export var update_interval: float = 0.25
@export var show_vram_metrics: bool = true
@export var show_lod_metrics: bool = true
@export var show_streaming_metrics: bool = true
@export var show_compression_metrics: bool = false

const COMPUTE_POLICY_DEFAULT := 0
const COMPUTE_POLICY_FORCE_ON := 1
const COMPUTE_POLICY_FORCE_OFF := 2
const COMPUTE_POLICY_TOGGLE_KEY := KEY_F8

## The complete panel is ~50 rows and, at the default 1280x720, covers the
## middle of the viewport -- including whatever the camera just framed. A
## diagnostics panel that hides the thing being diagnosed needs a way off.
const VISIBILITY_TOGGLE_KEY := KEY_F3

@onready var title_label: Label = get_node_or_null("Panel/VBox/TitleLabel")
@onready var body_label: RichTextLabel = get_node_or_null("Panel/VBox/Columns/BodyLeft")
@onready var body_right_label: RichTextLabel = get_node_or_null("Panel/VBox/Columns/BodyRight")
@onready var footer_label: Label = get_node_or_null("Panel/VBox/Footer")
@export var camera_path: NodePath

var gaussian_node: GaussianSplatNode3D
var camera_node: Node3D
var _time_since_update := 0.0
var _ema_frame_ms := 0.0
var _ema_fps := 0.0
var _last_instant_ms := 0.0
var _last_instant_fps := 0.0
var _fps_samples: Array[float] = []
const MAX_SAMPLES := 120

## Rendered in place of any quantity this build cannot measure on this frame.
const UNAVAILABLE := "n/a"

## The six GPU passes whose timestamps the renderer resolves, in pipeline order:
## [label, get_statistics() time key, get_statistics() validity key].
## `gpu_frame_ms` is defined as the sum of exactly these six
## (`tile_renderer.cpp:2931-2938`), which is the identity `_gpu_passes()` checks.
const GPU_PASSES := [
	["Overlap count", "gpu_overlap_count_ms", "gpu_overlap_count_valid"],
	["Prefix scan", "gpu_prefix_ms", "gpu_prefix_valid"],
	["Overlap emit", "gpu_overlap_emit_ms", "gpu_overlap_emit_valid"],
	["Overlap sort", "gpu_overlap_sort_ms", "gpu_overlap_sort_valid"],
	["Rasterize", "gpu_raster_ms", "gpu_raster_valid"],
	["Resolve", "gpu_resolve_ms", "gpu_resolve_valid"],
]

## Color thresholds for metrics
const GPU_TIME_GOOD := 8.0      # <8ms = green (120+ FPS headroom)
const GPU_TIME_OK := 16.0       # 8-16ms = yellow (60-120 FPS)
const GPU_TIME_BAD := 33.0      # 16-33ms = orange (30-60 FPS)
# >33ms = red (<30 FPS)

const VRAM_USAGE_GOOD := 60.0   # <60% = white
const VRAM_USAGE_WARN := 80.0   # 60-80% = yellow
const VRAM_USAGE_CRITICAL := 95.0 # 80-95% = orange
# >95% = red

const BUFFER_USAGE_GOOD := 70.0  # <70% = white
const BUFFER_USAGE_WARN := 85.0  # 70-85% = yellow
# >85% = orange

## Tolerance for the pass-sum identity, in milliseconds. The renderer computes
## the total as a float sum of the same six values, so anything above float
## noise is a real disagreement and gets printed.
const SUM_EPSILON_MS := 0.0005

## Enables processing so the overlay refreshes at runtime.
func _ready() -> void:
	set_process(true)
	set_process_unhandled_input(true)

# ============================================================================
# Reading values
# ============================================================================

## Reads one custom monitor.
##
## Call sites pass the FULL id, `gaussian_splatting/...`, spelled out. That is
## deliberate and not verbosity: `tests/ci/check_shipped_project_scripts.py`
## checks every monitor id a shipped script reads against the ids
## `performance_monitors.cpp` actually registers, and it derives both sides
## from string literals. Building the id from a prefix constant at run time
## would hide every read from that check -- a guard that can find nothing to
## check is not a guard. This file is the reason the check exists.
##
## @param id: Full monitor id, e.g. `gaussian_splatting/cpu_setup_time_ms`.
## @return The monitor value, or `null` when this build registers no such
## monitor. `null` renders as `n/a`; it is never coerced to 0. `Performance` is
## the engine singleton object itself -- `Performance.get_singleton()` is not
## bound to ClassDB and does not parse.
func _monitor(id: String) -> Variant:
	if not Performance.has_custom_monitor(id):
		return null
	return Performance.get_custom_monitor(id)

## Reads one `get_statistics()` entry, or `null` when the key is absent.
func _stat(stats: Dictionary, key: String) -> Variant:
	if not stats.has(key):
		return null
	return stats[key]

## Reads a GPU pass time, gated on that pass's own validity flag.
## @return The time in ms, or `null` when the pass did not resolve this frame.
func _valid_ms(stats: Dictionary, time_key: String, valid_key: String) -> Variant:
	if not stats.has(time_key) or not stats.has(valid_key):
		return null
	if not bool(stats[valid_key]):
		return null
	return float(stats[time_key])

## True when a streaming system is attached AND reporting. Every streaming, LOD
## and SH-compression monitor is gated behind `_get_active_splat_renderer(true)`
## in `performance_monitors.cpp`, so on a resident-only scene they all return a
## default -- 0 for most, but 1 and 1.0 for `lod_splat_skip_factor` and
## `lod_opacity_multiplier`, which is indistinguishable from a live reading.
## This flag is what makes those rows say `n/a` instead.
func _streaming_ready() -> bool:
	return _flag(_monitor("gaussian_splatting/streaming_monitor_ready")) == 1

# ============================================================================
# Formatting. Every formatter renders `null` as `n/a`.
# ============================================================================

func _fmt_ms(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return _colorize_gpu_time(float(value), "%.3f ms" % float(value))

func _fmt_plain_ms(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return "%.3f ms" % float(value)

func _fmt_count(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return _format_number(int(value))

func _fmt_num(value: Variant, spec: String) -> String:
	if value == null:
		return UNAVAILABLE
	return spec % float(value)

## Coerces a monitor value to int for BRANCHING only, never for display.
func _flag(value: Variant) -> int:
	if value == null:
		return 0
	return int(value)

## Assigns the Gaussian node used for statistics queries.
## @param node: GaussianSplatNode3D to monitor.
func set_gaussian_node(node: GaussianSplatNode3D) -> void:
	gaussian_node = node
	_try_resolve_camera()

## Assigns the camera rig node used for pose reporting.
## @param node: Camera rig node.
func set_camera_node(node: Node3D) -> void:
	camera_node = node

## Resolves the camera from the configured NodePath when missing.
func _try_resolve_camera() -> void:
	if camera_node:
		return
	if camera_path != NodePath():
		var node_obj = get_node_or_null(camera_path)
		if node_obj and node_obj is Node3D:
			camera_node = node_obj

## Tracks frame timing and refreshes the overlay at the configured interval.
## @param delta: Frame delta in seconds.
func _process(delta: float) -> void:
	_try_resolve_camera()
	if delta > 0.0:
		_last_instant_ms = delta * 1000.0
		_last_instant_fps = 1.0 / delta
		var alpha := 0.12
		_ema_frame_ms = _last_instant_ms if _ema_frame_ms == 0.0 else lerp(_ema_frame_ms, _last_instant_ms, alpha)
		_ema_fps = _last_instant_fps if _ema_fps == 0.0 else lerp(_ema_fps, _last_instant_fps, alpha)
		_fps_samples.append(_last_instant_fps)
		if _fps_samples.size() > MAX_SAMPLES:
			_fps_samples.pop_front()

	_time_since_update += delta
	if _time_since_update < update_interval:
		return
	_time_since_update = 0.0
	if not visible:
		# Hidden by F3: nothing would be read, so do not pay for the refresh.
		return
	_refresh_overlay()

## Returns color-coded string for GPU timing (ms)
func _colorize_gpu_time(time_ms: float, value_str: String) -> String:
	if time_ms < GPU_TIME_GOOD:
		return "[color=green]%s[/color]" % value_str
	elif time_ms < GPU_TIME_OK:
		return "[color=yellow]%s[/color]" % value_str
	elif time_ms < GPU_TIME_BAD:
		return "[color=orange]%s[/color]" % value_str
	else:
		return "[color=red]%s[/color]" % value_str

## Returns color-coded string for VRAM usage percentage
func _colorize_vram_percent(percent: float, value_str: String) -> String:
	if percent < VRAM_USAGE_GOOD:
		return value_str
	elif percent < VRAM_USAGE_WARN:
		return "[color=yellow]%s[/color]" % value_str
	elif percent < VRAM_USAGE_CRITICAL:
		return "[color=orange]%s[/color]" % value_str
	else:
		return "[color=red]%s[/color]" % value_str

## Returns color-coded string for buffer usage percentage
func _colorize_buffer_percent(percent: float, value_str: String) -> String:
	if percent < BUFFER_USAGE_GOOD:
		return value_str
	elif percent < BUFFER_USAGE_WARN:
		return "[color=yellow]%s[/color]" % value_str
	else:
		return "[color=orange]%s[/color]" % value_str

## Returns color-coded string for LOD reduction percentage (higher = more aggressive = red)
func _colorize_lod_reduction(percent: float, value_str: String) -> String:
	if percent < 25.0:
		return "[color=green]%s[/color]" % value_str
	elif percent < 50.0:
		return "[color=yellow]%s[/color]" % value_str
	elif percent < 75.0:
		return "[color=orange]%s[/color]" % value_str
	else:
		return "[color=red]%s[/color]" % value_str

func _format_compute_policy(policy: int) -> String:
	match policy:
		COMPUTE_POLICY_FORCE_ON:
			return "force_on"
		COMPUTE_POLICY_FORCE_OFF:
			return "force_off"
		_:
			return "default"

func _toggle_compute_raster_policy() -> void:
	if not gaussian_node:
		return
	var renderer := gaussian_node.get_renderer()
	if renderer and renderer.has_method("set_debug_compute_raster_policy"):
		var current := COMPUTE_POLICY_DEFAULT
		if renderer.has_method("get_debug_compute_raster_policy"):
			current = int(renderer.get_debug_compute_raster_policy())
		var next := (current + 1) % 3
		renderer.set_debug_compute_raster_policy(next)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == COMPUTE_POLICY_TOGGLE_KEY:
			_toggle_compute_raster_policy()
		elif event.keycode == VISIBILITY_TOGGLE_KEY:
			visible = not visible

# ============================================================================
# Sections
# ============================================================================

## Frame pacing. Engine-side values; not renderer telemetry.
func _section_frame(lines: Array[String]) -> void:
	lines.append("[b]═══ FRAME ═══[/b]")
	lines.append("FPS: %.1f (inst) / %.1f (engine avg)" % [_last_instant_fps, Engine.get_frames_per_second()])
	lines.append("CPU frame: %.2f ms (inst) / %.2f ms (EMA)" % [_last_instant_ms, _ema_frame_ms])
	if _fps_samples.size() > 0:
		var fps_min = _fps_samples.min()
		var fps_max = _fps_samples.max()
		var fps_avg = 0.0
		for v in _fps_samples:
			fps_avg += v
		fps_avg /= _fps_samples.size()
		lines.append("FPS trends (%d frames): avg %.1f | min %.1f | max %.1f" % [_fps_samples.size(), fps_avg, fps_min, fps_max])

func _section_camera(lines: Array[String]) -> void:
	if camera_node == null:
		return
	var basis: Basis = camera_node.global_transform.basis
	var origin: Vector3 = camera_node.global_transform.origin
	var euler := basis.get_euler()
	lines.append("")
	lines.append("[b]═══ CAMERA ═══[/b]")
	lines.append("Pos: (%.2f, %.2f, %.2f)" % [origin.x, origin.y, origin.z])
	lines.append("Rot: (%.1f°, %.1f°, %.1f°)" % [rad_to_deg(euler.x), rad_to_deg(euler.y), rad_to_deg(euler.z)])
	var cam = camera_node.get_node_or_null("Camera3D")
	if cam and cam is Camera3D:
		var cam3d: Camera3D = cam
		lines.append("Projection: %s | FOV: %.1f° | Size: %.2f" % [
			"Ortho" if cam3d.projection == Camera3D.PROJECTION_ORTHOGONAL else "Persp",
			cam3d.fov, cam3d.size])

## The six resolved GPU pass timestamps, plus the identity check.
##
## Every row is gated on its own `gpu_*_valid` flag, so a pass that did not
## resolve this frame reads `n/a` rather than carrying the renderer's
## last-known-good value forward as if it were fresh.
func _section_gpu_passes(lines: Array[String], stats: Dictionary) -> void:
	lines.append("")
	lines.append("[b]═══ GPU PASSES ═══[/b]")
	if stats.is_empty():
		lines.append("no node statistics this frame — %s" % UNAVAILABLE)
		return

	var sum_ms := 0.0
	var all_valid := true
	for entry in GPU_PASSES:
		var value = _valid_ms(stats, entry[1], entry[2])
		if value == null:
			all_valid = false
		else:
			sum_ms += float(value)
		lines.append("%s: %s" % [entry[0], _fmt_ms(value)])

	var reported = _valid_ms(stats, "gpu_frame_ms", "gpu_frame_valid")
	if not all_valid:
		lines.append("Pass total: %s (a pass above is %s, so the rows do not sum)"
			% [_fmt_ms(reported), UNAVAILABLE])
	elif reported == null:
		lines.append("Pass total: %s | rows sum to %s" % [UNAVAILABLE, _fmt_plain_ms(sum_ms)])
	else:
		var delta: float = abs(float(reported) - sum_ms)
		if delta <= SUM_EPSILON_MS:
			lines.append("Pass total: %s (= sum of the six rows)" % _fmt_ms(reported))
		else:
			lines.append("Pass total: %s | rows sum to %s | [color=orange]Δ %.4f ms[/color]"
				% [_fmt_ms(reported), _fmt_plain_ms(sum_ms), delta])

	var route = _stat(stats, "route_uid")
	var source = _stat(stats, "data_source")
	lines.append("Route: %s | source: %s" % [
		str(route) if route != null and str(route) != "" else UNAVAILABLE,
		str(source) if source != null and str(source) != "" else UNAVAILABLE])

## Host-side wall-clock timings. Kept apart from the GPU pass block because
## they are measured with `OS::get_ticks_usec()` around a call, not with a GPU
## timestamp -- the old panel filed the cull time under "GPU".
func _section_host_stages(lines: Array[String], stats: Dictionary) -> void:
	lines.append("")
	lines.append("[b]═══ HOST STAGES (CPU clock) ═══[/b]")
	lines.append("TileRenderer setup: %s" % _fmt_ms(_monitor("gaussian_splatting/cpu_setup_time_ms")))
	lines.append("Cull stage: %s" % _fmt_ms(_stat(stats, "cull_ms")))
	var dispatch = null
	if not stats.is_empty() and bool(stats.get("overlap_sort_cpu_dispatch_valid", false)):
		dispatch = stats.get("overlap_sort_cpu_dispatch_ms")
	lines.append("Sort dispatch: %s" % _fmt_ms(dispatch))

## What is on screen, named by the domain the renderer actually culls in.
func _section_visibility(lines: Array[String], stats: Dictionary) -> void:
	lines.append("")
	lines.append("[b]═══ VISIBILITY ═══[/b]")
	if stats.is_empty():
		lines.append("no node statistics this frame — %s" % UNAVAILABLE)
		return

	lines.append("Resident splats: %s" % _fmt_count(_stat(stats, "total_splats")))

	# `visible_splats` is NOT a frustum-culled splat count on a resident scene:
	# the cull runs over chunk references (`stage_cull_visible_domain`), so it
	# reports the whole atlas until the single chunk leaves the frustum. Name
	# the domain instead of implying per-splat visibility.
	var domain = _stat(stats, "stage_cull_visible_domain")
	var domain_name := str(domain) if domain != null and str(domain) != "" else "unknown"
	lines.append("Cull domain: %s — %s of %s visible" % [
		domain_name,
		_fmt_count(_stat(stats, "stage_cull_visible_count")),
		_fmt_count(_stat(stats, "stage_cull_candidate_count"))])
	lines.append("Culled: frustum %s | distance %s | screen %s | importance %s" % [
		_fmt_count(_stat(stats, "culled_by_frustum")),
		_fmt_count(_stat(stats, "culled_by_distance")),
		_fmt_count(_stat(stats, "culled_by_screen")),
		_fmt_count(_stat(stats, "culled_by_importance"))])
	if domain_name != "splat":
		lines.append("[i]Per-splat visibility is not measured on this route.[/i]")

	# Tile-projection reject buckets. These are the registered counters; the
	# `projection_near_clamp_count` / `projection_behind_camera_count` /
	# `projection_screen_culled_count` names the old panel read have no
	# producer anywhere and are not synonyms for these.
	lines.append("Tile rejects: clip %s | radius %s | viewport %s | aspect %s" % [
		_fmt_count(_monitor("gaussian_splatting/clip_reject_count")),
		_fmt_count(_monitor("gaussian_splatting/radius_reject_count")),
		_fmt_count(_monitor("gaussian_splatting/viewport_reject_count")),
		_fmt_count(_monitor("gaussian_splatting/extreme_aspect_count"))])
	lines.append("Tiles: %s | aggregated: %s | overflow: %s" % [
		_fmt_count(_monitor("gaussian_splatting/tile_count")),
		_fmt_count(_monitor("gaussian_splatting/aggregated_count")),
		_fmt_count(_monitor("gaussian_splatting/overflow_tile_count"))])

	# Overlap accounting is only populated when the rasterizer configures a
	# record budget. A configured budget of 0 means the accounting is not
	# running on this route, which is not the same as "0 records used".
	var used = _stat(stats, "overlap_records")
	var budget = _stat(stats, "overlap_record_budget")
	if budget == null or int(budget) <= 0:
		lines.append("Overlap records: %s (no record budget configured on this route)" % UNAVAILABLE)
	else:
		var pct := float(used if used != null else 0) / float(budget) * 100.0
		lines.append("Overlap records: %s / %s (%s)" % [
			_fmt_count(used), _fmt_count(budget),
			_colorize_buffer_percent(pct, "%.1f%%" % pct)])

## Real device allocation, from `RenderingDevice::get_memory_usage`.
##
## This replaces the old VRAM BUDGET block. That block reported the streaming
## regulator's budget model, which is structurally 0 on a resident scene, and
## three quantities -- reserved / allocated-chunks / pool size -- that no
## producer in this engine computes at all. The pool/reservation model is
## tracked separately; see the issue linked from #833.
func _section_device_vram(lines: Array[String]) -> void:
	lines.append("")
	lines.append("[b]═══ DEVICE VRAM ═══[/b]")
	lines.append("RenderingDevice total: %s MB" % _fmt_num(_monitor("gaussian_splatting/vram_device_total_mb"), "%.1f"))
	lines.append("Buffers: %s MB | Textures: %s MB" % [
		_fmt_num(_monitor("gaussian_splatting/vram_device_buffers_mb"), "%.1f"),
		_fmt_num(_monitor("gaussian_splatting/vram_device_textures_mb"), "%.1f")])

	if not _streaming_ready():
		lines.append("Streaming VRAM budget: %s (no streaming system attached)" % UNAVAILABLE)
		return
	var usage = _monitor("gaussian_splatting/vram_current_usage_mb")
	var budget = _monitor("gaussian_splatting/vram_budget_mb")
	var percent = _monitor("gaussian_splatting/vram_usage_percent")
	var warning := ""
	if _flag(_monitor("gaussian_splatting/vram_budget_warning_active")) > 0:
		warning = "[color=orange]⚠ WARNING[/color] "
	var percent_text := UNAVAILABLE
	if percent != null:
		percent_text = _colorize_vram_percent(float(percent), "%.1f%%" % float(percent))
	lines.append("%sStreaming budget: %s / %s MB (%s)" % [
		warning, _fmt_num(usage, "%.1f"), _fmt_num(budget, "%.1f"), percent_text])
	lines.append("Evicted this frame: %s | thrashing events: %s" % [
		_fmt_count(_monitor("gaussian_splatting/vram_evicted_this_frame")),
		_fmt_count(_monitor("gaussian_splatting/vram_thrashing_events"))])

func _section_lod(lines: Array[String]) -> void:
	lines.append("")
	lines.append("[b]═══ LOD ═══[/b]")
	if not _streaming_ready():
		# Every LOD monitor is streaming-gated. Two of them return 1 and 1.0
		# with no renderer at all, which reads exactly like a live measurement;
		# that is precisely why this block must not print them here.
		lines.append("no streaming system attached — all rows %s" % UNAVAILABLE)
		return
	var reduction = _monitor("gaussian_splatting/lod_reduction_ratio_pct")
	var reduction_text := UNAVAILABLE
	if reduction != null:
		reduction_text = _colorize_lod_reduction(float(reduction), "%.1f%%" % float(reduction))
	lines.append("Level (avg): %s | reduction: %s" % [
		_fmt_count(_monitor("gaussian_splatting/lod_current_level")), reduction_text])
	lines.append("Chunk distance: min %s | avg %s | max %s" % [
		_fmt_num(_monitor("gaussian_splatting/lod_min_chunk_distance"), "%.1f"),
		_fmt_num(_monitor("gaussian_splatting/lod_avg_chunk_distance"), "%.1f"),
		_fmt_num(_monitor("gaussian_splatting/lod_max_chunk_distance"), "%.1f")])
	lines.append("Splat skip: %sx | opacity: %s | transitioning: %s chunks" % [
		_fmt_count(_monitor("gaussian_splatting/lod_splat_skip_factor")),
		_fmt_num(_monitor("gaussian_splatting/lod_opacity_multiplier"), "%.2f"),
		_fmt_count(_monitor("gaussian_splatting/lod_chunks_in_transition"))])
	if _flag(_monitor("gaussian_splatting/lod_quality_degradation_active")) > 0:
		lines.append("[color=orange]⚠ Quality degradation active (VRAM pressure)[/color]")

func _section_streaming(lines: Array[String]) -> void:
	lines.append("")
	lines.append("[b]═══ STREAMING ═══[/b]")
	if not _streaming_ready():
		lines.append("no streaming system attached — all rows %s" % UNAVAILABLE)
		return
	lines.append("Chunks: visible %s | loaded %s / %s | resident %s" % [
		_fmt_count(_monitor("gaussian_splatting/streaming_visible_chunks")),
		_fmt_count(_monitor("gaussian_splatting/streaming_loaded_chunks")),
		_fmt_count(_monitor("gaussian_splatting/streaming_total_chunks")),
		_fmt_count(_monitor("gaussian_splatting/streaming_resident_chunks"))])
	lines.append("This frame: +%s loaded | -%s evicted" % [
		_fmt_count(_monitor("gaussian_splatting/streaming_chunks_loaded_this_frame")),
		_fmt_count(_monitor("gaussian_splatting/streaming_chunks_evicted_this_frame"))])
	lines.append("Buffer capacity: %s splats | effective: %s" % [
		_fmt_count(_monitor("gaussian_splatting/streaming_buffer_capacity_splats")),
		_fmt_count(_monitor("gaussian_splatting/streaming_effective_splat_count"))])
	lines.append("Uploaded: %s MB total | queue depth %s" % [
		_fmt_num(_monitor("gaussian_splatting/memory_stream_total_bytes_uploaded_mb"), "%.2f"),
		_fmt_count(_monitor("gaussian_splatting/chunk_upload_queue_depth"))])
	lines.append("Caps: frame %s MB | slice %s MB | %s MB/s" % [
		_fmt_num(_monitor("gaussian_splatting/streaming_effective_upload_cap_mb_per_frame"), "%.0f"),
		_fmt_num(_monitor("gaussian_splatting/streaming_effective_upload_cap_mb_per_slice"), "%.0f"),
		_fmt_num(_monitor("gaussian_splatting/streaming_effective_upload_cap_mb_per_second"), "%.0f")])
	lines.append("VRAM cap: %s MB | max chunks %s" % [
		_fmt_num(_monitor("gaussian_splatting/streaming_effective_vram_budget_mb"), "%.0f"),
		_fmt_count(_monitor("gaussian_splatting/streaming_effective_vram_max_chunks"))])

	var cap_markers := PackedStringArray()
	if _flag(_monitor("gaussian_splatting/streaming_upload_frame_cap_hit")) > 0:
		cap_markers.append("upload/frame")
	if _flag(_monitor("gaussian_splatting/streaming_upload_bandwidth_cap_hit")) > 0:
		cap_markers.append("upload/bandwidth")
	if _flag(_monitor("gaussian_splatting/streaming_chunk_load_cap_hit")) > 0:
		cap_markers.append("chunk-load")
	if _flag(_monitor("gaussian_splatting/streaming_vram_chunk_cap_hit")) > 0:
		cap_markers.append("vram")
	if _flag(_monitor("gaussian_splatting/streaming_queue_pressure_active")) > 0:
		cap_markers.append("queue")
	if cap_markers.size() > 0:
		lines.append("[color=orange]Pressure: %s[/color]" % ", ".join(cap_markers))

	var stall = _monitor("gaussian_splatting/memory_stream_stall_percent")
	lines.append("Pipeline stalls: %s" % _fmt_pct_colored(stall))

func _fmt_pct_colored(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	var pct := float(value)
	if pct > 5.0:
		return "[color=orange]%.1f%%[/color]" % pct
	return "%.1f%%" % pct

func _section_compression(lines: Array[String]) -> void:
	lines.append("")
	lines.append("[b]═══ SH COMPRESSION ═══[/b]")
	if not _streaming_ready():
		lines.append("no streaming system attached — all rows %s" % UNAVAILABLE)
		return
	var raw = _monitor("gaussian_splatting/sh_compression_raw_mb")
	var compressed = _monitor("gaussian_splatting/sh_compression_compressed_mb")
	var ratio = _monitor("gaussian_splatting/sh_compression_ratio_pct")
	lines.append("Raw: %s MB → compressed: %s MB" % [
		_fmt_num(raw, "%.2f"), _fmt_num(compressed, "%.2f")])
	lines.append("Ratio: %s" % _fmt_pct_colored(ratio))

func _section_node(lines: Array[String], stats: Dictionary) -> void:
	if gaussian_node == null:
		return
	lines.append("")
	lines.append("[b]═══ NODE ═══[/b]")
	lines.append("Total splats: %s" % _fmt_count(_stat(stats, "total_splats")))
	lines.append("Last update: %s" % _fmt_ms(_stat(stats, "update_time_ms")))
	var renderer := gaussian_node.get_renderer()
	if renderer and renderer.has_method("get_debug_compute_raster_policy"):
		lines.append("Raster policy: %s (F8 to toggle)"
			% _format_compute_policy(int(renderer.get_debug_compute_raster_policy())))

## Process-global manager state.
##
## `GaussianSplatManager.get_global_stats()` reports totals over buffers
## registered with the manager. The node/renderer route used by this template
## registers none, so `total_gaussians` / `total_memory_mb` / `buffer_count`
## are structurally 0 here -- printing them next to 768 rendered splats is the
## same lie as any other unmeasured 0, so the counts are shown only when the
## registry is non-empty. `gpu_sorting_enabled` is a configuration value and is
## always meaningful.
func _section_global(lines: Array[String]) -> void:
	var bootstrap = get_tree().get_root().get_node_or_null("GaussianBootstrap")
	if bootstrap == null or not bootstrap.is_ready:
		return
	var global_stats: Dictionary = bootstrap.get_global_stats()
	if global_stats.is_empty():
		return
	lines.append("")
	lines.append("[b]═══ MANAGER ═══[/b]")
	lines.append("GPU sorting: %s" % ("enabled" if global_stats.get("gpu_sorting_enabled", false) else "disabled"))
	var buffer_count := int(global_stats.get("buffer_count", 0))
	if buffer_count > 0:
		lines.append("Registered buffers: %d | gaussians: %s | %s MB" % [
			buffer_count,
			_format_number(int(global_stats.get("total_gaussians", 0))),
			"%.2f" % float(global_stats.get("total_memory_mb", 0.0))])
	else:
		lines.append("Buffer registry: empty — per-node route registers none, so its totals are %s" % UNAVAILABLE)

## Rebuilds the overlay text from the renderer's per-frame statistics and the
## registered custom monitors.
func _refresh_overlay() -> void:
	var lines: Array[String] = []
	var stats: Dictionary = {}
	if gaussian_node != null:
		stats = gaussian_node.get_statistics()

	_section_frame(lines)
	_section_camera(lines)
	_section_gpu_passes(lines, stats)
	_section_host_stages(lines, stats)
	_section_visibility(lines, stats)
	if show_vram_metrics:
		_section_device_vram(lines)
	if show_lod_metrics:
		_section_lod(lines)
	if show_streaming_metrics:
		_section_streaming(lines)
	if show_compression_metrics:
		_section_compression(lines)
	_section_node(lines, stats)
	_section_global(lines)

	var columns := _split_columns(lines)
	if body_label:
		body_label.text = columns[0]
	if body_right_label:
		body_right_label.text = columns[1]

	if footer_label:
		footer_label.text = "WASD: move | Space/C: ascend/descend | Shift: boost | RMB: orbit | MMB: pan | Wheel: zoom | F3: hide this panel | F8: raster policy"

## Splits the rendered rows across the panel's two columns on section
## boundaries, balancing the two by line count.
##
## The whole panel is ~50 lines; a single column of that is taller than a
## 720 px viewport, and the shipped 120 px auto-scrolling box showed the user
## only its last five lines. Sections are kept whole -- a header is never
## separated from its rows -- and once a section spills into the right column
## every later section follows it, so the panel reads top-left then top-right.
## @param lines: The rendered rows, section headers included.
## @return [left_text, right_text].
func _split_columns(lines: Array[String]) -> Array:
	var sections: Array = []
	var current: Array[String] = []
	for line in lines:
		if line.begins_with("[b]═══") and current.size() > 0:
			sections.append(current)
			current = []
		current.append(line)
	if current.size() > 0:
		sections.append(current)

	# Choose the section boundary that minimises the taller column, rather than
	# spilling at the first section that crosses the halfway line: with
	# unevenly sized sections the latter leaves one column half empty and
	# pushes the other past the bottom of the viewport.
	var total := lines.size()
	var best_split := sections.size()
	var best_cost := total
	var running := 0
	for i in range(1, sections.size()):
		running += (sections[i - 1] as Array).size()
		var cost: int = max(running, total - running)
		if cost < best_cost:
			best_cost = cost
			best_split = i

	var left: Array[String] = []
	var right: Array[String] = []
	for i in range(sections.size()):
		var target := left if i < best_split else right
		for line in sections[i]:
			target.append(line)
	return ["\n".join(left).strip_edges(), "\n".join(right).strip_edges()]

## Formats large numbers with K/M suffixes for readability
func _format_number(value: int) -> String:
	if value >= 1000000:
		return "%.2fM" % (value / 1000000.0)
	elif value >= 1000:
		return "%.1fK" % (value / 1000.0)
	else:
		return str(value)
