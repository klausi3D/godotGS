extends MarginContainer
class_name GaussianPerformanceOverlay

@export var update_interval: float = 0.25
@export var show_vram_metrics: bool = true
@export var show_lod_metrics: bool = true
@export var show_streaming_metrics: bool = true
@export var show_compression_metrics: bool = false

const COMPUTE_POLICY_DEFAULT := 0
const COMPUTE_POLICY_FORCE_ON := 1
const COMPUTE_POLICY_FORCE_OFF := 2
const COMPUTE_POLICY_TOGGLE_KEY := KEY_F8

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
var _gpu_prefix_samples: Array[float] = []
const MAX_SAMPLES := 120

## Prefix shared by every custom monitor this module registers.
const MONITOR_PREFIX := "gaussian_splatting/"

## Rendered in place of any quantity this build cannot measure. Never render a
## 0 for an unavailable quantity: a reader cannot tell it from a measurement.
const UNAVAILABLE := "n/a"

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

## Enables processing so the overlay refreshes at runtime.
func _ready() -> void:
	set_process(true)
	set_process_unhandled_input(true)

## Reads one custom monitor.
## @param name: Monitor id without the `gaussian_splatting/` prefix.
## @return The monitor value, or `null` when this build registers no such
## monitor. `null` is rendered as `n/a` by the formatters below; it is never
## coerced to 0, because a reader cannot distinguish a measured 0 from a
## missing producer. `Performance` is the engine singleton object itself --
## `Performance.get_singleton()` is not bound to ClassDB and does not parse.
func _monitor(name: String) -> Variant:
	var id := MONITOR_PREFIX + name
	if not Performance.has_custom_monitor(id):
		return null
	return Performance.get_custom_monitor(id)

## Formats a monitor value in milliseconds, or `n/a` when unavailable.
func _fmt_ms(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return _colorize_gpu_time(float(value), "%.3f ms" % float(value))

## Formats a monitor value as a count, or `n/a` when unavailable.
func _fmt_count(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return _format_number(int(value))

## Formats a monitor value as a percentage, or `n/a` when unavailable.
func _fmt_pct(value: Variant) -> String:
	if value == null:
		return UNAVAILABLE
	return "%.1f%%" % float(value)

## Formats a monitor value with the given printf spec, or `n/a` when unavailable.
func _fmt_num(value: Variant, spec: String) -> String:
	if value == null:
		return UNAVAILABLE
	return spec % float(value)

## Coerces a monitor value to int; unavailable reads count as 0 for the purpose
## of *branching only* (never for display).
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

## Rebuilds the overlay text with the latest renderer statistics using Custom Performance Monitors.
func _refresh_overlay() -> void:
	var lines: Array[String] = []

	# ========================================================================
	# FRAME & FPS
	# ========================================================================
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

	# ========================================================================
	# CAMERA
	# ========================================================================
	if camera_node:
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
			var fov := cam3d.fov
			var size := cam3d.size
			var ortho := cam3d.projection == Camera3D.PROJECTION_ORTHOGONAL
			lines.append("Projection: %s | FOV: %.1f° | Size: %.2f" % ["Ortho" if ortho else "Persp", fov, size])

	# ========================================================================
	# GPU PIPELINE (Custom Performance Monitors)
	# ========================================================================
	lines.append("")
	lines.append("[b]═══ GPU PIPELINE ═══[/b]")

	lines.append("CPU setup: %s" % _fmt_ms(_monitor("cpu_setup_time_ms")))
	lines.append("GPU frustum cull: %s" % _fmt_ms(_monitor("gpu_time_frustum_cull_ms")))
	lines.append("GPU binning: %s" % _fmt_ms(_monitor("gpu_time_binning_ms")))
	lines.append("GPU prefix scan: %s" % _fmt_ms(_monitor("gpu_time_prefix_ms")))
	lines.append("GPU sort: %s" % _fmt_ms(_monitor("gpu_time_sort_ms")))
	lines.append("GPU rasterize: %s" % _fmt_ms(_monitor("gpu_time_raster_ms")))
	lines.append("GPU total: %s" % _fmt_ms(_monitor("gpu_time_frame_ms")))

	# ========================================================================
	# PROJECTION & VISIBILITY
	# ========================================================================
	lines.append("")
	lines.append("[b]═══ PROJECTION & VISIBILITY ═══[/b]")

	var success_rate = _monitor("projection_success_rate_pct")

	lines.append("Visible splats: %s" % _fmt_count(_monitor("visible_splats")))
	lines.append("Frustum culled: %s" % _fmt_count(_monitor("culled_frustum")))

	if success_rate == null:
		lines.append("Projection success: %s" % UNAVAILABLE)
	else:
		var rate := float(success_rate)
		var success_color = "green" if rate > 95.0 else ("yellow" if rate > 85.0 else "orange")
		lines.append("Projection success: [color=%s]%.1f%%[/color]" % [success_color, rate])
	lines.append("Rejects: near %s | behind %s | screen %s | aspect %s" % [
		_fmt_count(_monitor("projection_near_clamp_count")),
		_fmt_count(_monitor("projection_behind_camera_count")),
		_fmt_count(_monitor("projection_screen_culled_count")),
		_fmt_count(_monitor("projection_extreme_aspect_count"))
	])

	var overlap_used = _monitor("overlap_records_used")
	var overlap_budget = _monitor("overlap_record_budget")
	if overlap_used == null or overlap_budget == null:
		lines.append("Overlap: %s / %s (%s)" % [
			_fmt_count(overlap_used), _fmt_count(overlap_budget), UNAVAILABLE
		])
	else:
		var used := int(overlap_used)
		var budget := int(overlap_budget)
		var overlap_pct = (float(used) / float(budget) * 100.0) if budget > 0 else 0.0
		lines.append("Overlap: %s / %s (%s)" % [
			_format_number(used),
			_format_number(budget),
			_colorize_buffer_percent(overlap_pct, "%.1f%%" % overlap_pct)
		])

	# ========================================================================
	# VRAM BUDGET (if enabled)
	# ========================================================================
	if show_vram_metrics:
		lines.append("")
		lines.append("[b]═══ VRAM BUDGET ═══[/b]")

		var vram_usage = _monitor("vram_current_usage_mb")
		var vram_budget = _monitor("vram_budget_mb")
		var vram_percent = _monitor("vram_usage_percent")
		var vram_warning = _flag(_monitor("vram_budget_warning_active"))
		var vram_critical = _flag(_monitor("vram_budget_critical_active"))

		var warning_icon = ""
		if vram_critical > 0:
			warning_icon = "[color=red]⚠ CRITICAL[/color] "
		elif vram_warning > 0:
			warning_icon = "[color=orange]⚠ WARNING[/color] "

		var vram_percent_text := UNAVAILABLE
		if vram_percent != null:
			vram_percent_text = _colorize_vram_percent(float(vram_percent), "%.1f%%" % float(vram_percent))
		lines.append("%sUsage: %s / %s MB (%s)" % [
			warning_icon,
			_fmt_num(vram_usage, "%.1f"),
			_fmt_num(vram_budget, "%.1f"),
			vram_percent_text
		])

		lines.append("Reserved: %s MB | Allocated: %s MB | Pool: %s MB" % [
			_fmt_num(_monitor("vram_reserved_for_streaming_mb"), "%.1f"),
			_fmt_num(_monitor("vram_allocated_chunks_mb"), "%.1f"),
			_fmt_num(_monitor("vram_pool_size_mb"), "%.1f")
		])

		var thrashing = _flag(_monitor("vram_thrashing_detected"))
		var evictions = _monitor("vram_eviction_count")

		if thrashing > 0:
			lines.append("[color=red]⚠ THRASHING DETECTED[/color] | Evictions: %s" % _fmt_count(evictions))
		elif _flag(evictions) > 0:
			lines.append("Evictions: %s" % _fmt_count(evictions))

	# ========================================================================
	# LOD SYSTEM (if enabled)
	# ========================================================================
	if show_lod_metrics:
		lines.append("")
		lines.append("[b]═══ LOD SYSTEM ═══[/b]")

		var lod_reduction = _monitor("lod_reduction_ratio_pct")
		var lod_min_dist = _monitor("lod_min_chunk_distance")
		var lod_max_dist = _monitor("lod_max_chunk_distance")
		var lod_avg_dist = _monitor("lod_avg_chunk_distance")

		var lod_reduction_text := UNAVAILABLE
		if lod_reduction != null:
			lod_reduction_text = _colorize_lod_reduction(float(lod_reduction), "%.1f%%" % float(lod_reduction))
		lines.append("LOD level (avg): %s | Reduction: %s" % [
			_fmt_count(_monitor("lod_current_level")),
			lod_reduction_text
		])

		if _flag(lod_min_dist) > 0 or _flag(lod_max_dist) > 0:
			lines.append("Distance: min %s | avg %s | max %s" % [
				_fmt_num(lod_min_dist, "%.1f"),
				_fmt_num(lod_avg_dist, "%.1f"),
				_fmt_num(lod_max_dist, "%.1f")
			])

		lines.append("Splat skip: %sx | Opacity: %s | Transitioning: %s chunks" % [
			_fmt_count(_monitor("lod_splat_skip_factor")),
			_fmt_num(_monitor("lod_opacity_multiplier"), "%.2f"),
			_fmt_count(_monitor("lod_chunks_in_transition"))
		])

		var quality_degrade = _flag(_monitor("lod_quality_degradation_active"))
		if quality_degrade > 0:
			lines.append("[color=orange]⚠ Quality degradation active (VRAM pressure)[/color]")

	# ========================================================================
	# STREAMING (if enabled)
	# ========================================================================
	if show_streaming_metrics:
		lines.append("")
		lines.append("[b]═══ STREAMING ═══[/b]")

		var pending_loads = _monitor("streaming_pending_chunk_loads")

		lines.append("Chunks: visible %s | loaded %s / %s" % [
			_fmt_count(_monitor("streaming_visible_chunks")),
			_fmt_count(_monitor("streaming_loaded_chunks")),
			_fmt_count(_monitor("streaming_total_chunks"))
		])

		if _flag(pending_loads) > 0:
			lines.append("Pending loads: [color=yellow]%s[/color]" % _fmt_count(pending_loads))

		var buffer_slots = _monitor("streaming_buffer_slot_count")
		var buffer_used = _monitor("streaming_buffer_slots_used")
		var buffer_pct_text := UNAVAILABLE
		if buffer_slots != null and buffer_used != null:
			var buffer_pct = (float(buffer_used) / float(buffer_slots) * 100.0) if float(buffer_slots) > 0.0 else 0.0
			buffer_pct_text = _colorize_buffer_percent(buffer_pct, "%.1f%%" % buffer_pct)

		lines.append("Buffer slots: %s / %s (%s)" % [
			_fmt_count(buffer_used),
			_fmt_count(buffer_slots),
			buffer_pct_text
		])

		# Memory Stream stats
		var stall_pct = _monitor("memory_stream_stall_percent")

		lines.append("Upload: %s MB total | %s MB/s" % [
			_fmt_num(_monitor("memory_stream_total_bytes_uploaded_mb"), "%.2f"),
			_fmt_num(_monitor("memory_stream_upload_rate_mbps"), "%.2f")
		])
		lines.append("Caps: frame %s MB | slice %s MB | bandwidth %s MB/s" % [
			_fmt_num(_monitor("streaming_effective_upload_cap_mb_per_frame"), "%.0f"),
			_fmt_num(_monitor("streaming_effective_upload_cap_mb_per_slice"), "%.0f"),
			_fmt_num(_monitor("streaming_effective_upload_cap_mb_per_second"), "%.0f")
		])
		lines.append("VRAM cap: %s MB | max chunks %s" % [
			_fmt_num(_monitor("streaming_effective_vram_budget_mb"), "%.0f"),
			_fmt_count(_monitor("streaming_effective_vram_max_chunks"))
		])

		var cap_markers := PackedStringArray()
		if _flag(_monitor("streaming_upload_frame_cap_hit")) > 0:
			cap_markers.append("upload/frame")
		if _flag(_monitor("streaming_upload_bandwidth_cap_hit")) > 0:
			cap_markers.append("upload/bandwidth")
		if _flag(_monitor("streaming_chunk_load_cap_hit")) > 0:
			cap_markers.append("chunk-load")
		if _flag(_monitor("streaming_vram_chunk_cap_hit")) > 0:
			cap_markers.append("vram")
		if _flag(_monitor("streaming_queue_pressure_active")) > 0:
			cap_markers.append("queue")
		if cap_markers.size() > 0:
			lines.append("[color=orange]Pressure: %s[/color]" % ", ".join(cap_markers))

		if stall_pct != null and float(stall_pct) > 5.0:
			lines.append("Pipeline stalls: [color=orange]%.1f%%[/color]" % float(stall_pct))
		elif stall_pct != null and float(stall_pct) > 0.1:
			lines.append("Pipeline stalls: %.1f%%" % float(stall_pct))

	# ========================================================================
	# COMPRESSION (if enabled and active)
	# ========================================================================
	if show_compression_metrics:
		var sh_raw = _monitor("sh_compression_raw_mb")
		var sh_compressed = _monitor("sh_compression_compressed_mb")
		var sh_ratio = _monitor("sh_compression_ratio_pct")

		if sh_raw != null and sh_compressed != null and sh_ratio != null and float(sh_raw) > 0.0:
			lines.append("")
			lines.append("[b]═══ SH COMPRESSION ═══[/b]")
			var savings_mb = float(sh_raw) - float(sh_compressed)
			var savings_color = "green" if float(sh_ratio) > 50.0 else "yellow"
			lines.append("%.2f MB → %.2f MB ([color=%s]%.1f%% compressed[/color])" % [
				float(sh_raw),
				float(sh_compressed),
				savings_color,
				float(sh_ratio)
			])
			lines.append("VRAM saved: [color=green]%.2f MB[/color]" % savings_mb)

	# ========================================================================
	# NODE STATISTICS (legacy for compatibility)
	# ========================================================================
	if gaussian_node:
		var node_stats := gaussian_node.get_statistics()
		lines.append("")
		lines.append("[b]═══ NODE ═══[/b]")
		lines.append("Total splats: %s" % _format_number(node_stats.get("total_splats", 0)))
		lines.append("Last update: %.2f ms" % node_stats.get("update_time_ms", 0.0))

		var renderer := gaussian_node.get_renderer()
		if renderer:
			if renderer.has_method("get_debug_compute_raster_policy"):
				var policy := int(renderer.get_debug_compute_raster_policy())
				lines.append("Raster policy: %s (F8 to toggle)" % _format_compute_policy(policy))

	# ========================================================================
	# GLOBAL STATISTICS
	# ========================================================================
	var bootstrap = get_tree().get_root().get_node_or_null("GaussianBootstrap")
	if bootstrap and bootstrap.is_ready:
		var global_stats: Dictionary = bootstrap.get_global_stats()
		if not global_stats.is_empty():
			lines.append("")
			lines.append("[b]═══ GLOBAL ═══[/b]")
			lines.append("Active gaussians: %s" % _format_number(global_stats.get('total_gaussians', 0)))
			lines.append("GPU memory: %.2f MB" % global_stats.get('total_memory_mb', 0.0))
			lines.append("Registered buffers: %s" % global_stats.get('buffer_count', 0))
			lines.append("GPU sorting: %s" % ("enabled" if global_stats.get('gpu_sorting_enabled', false) else "disabled"))

	var columns := _split_columns(lines)
	if body_label:
		body_label.text = columns[0]
	if body_right_label:
		body_right_label.text = columns[1]

	if footer_label:
		footer_label.text = "WASD: move | Space/C: ascend/descend | Shift: boost | RMB: orbit | MMB: pan | Wheel: zoom | F8: raster policy"

## Splits the rendered rows across the panel's two columns on section
## boundaries, balancing the two by line count.
##
## The whole panel is ~50 lines; a single column of that is taller than a
## 720 px viewport, and the shipped 120 px auto-scrolling box showed the user
## only its last five lines. Sections are kept whole -- a header is never
## separated from its rows -- and the split point is chosen so that neither
## column is more than half the total plus one section.
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

	var total := lines.size()
	var left: Array[String] = []
	var right: Array[String] = []
	var half := int(ceil(total / 2.0))
	var used := 0
	var spilled := false
	for section in sections:
		# Once a section spills, every later section follows it: sections must
		# keep their order, so the columns read top-left then top-right.
		if spilled or (used > 0 and used + section.size() > half):
			spilled = true
			for line in section:
				right.append(line)
		else:
			for line in section:
				left.append(line)
			used += section.size()
	return ["\n".join(left).strip_edges(), "\n".join(right).strip_edges()]

## Formats large numbers with K/M suffixes for readability
func _format_number(value: int) -> String:
	if value >= 1000000:
		return "%.2fM" % (value / 1000000.0)
	elif value >= 1000:
		return "%.1fK" % (value / 1000.0)
	else:
		return str(value)
