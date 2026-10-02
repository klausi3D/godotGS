#include "gaussian_splat_performance_overlay.h"

#include "gaussian_splat_node_3d.h"
#include "gaussian_splat_world_3d.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/performance_monitors.h"
#include "../interfaces/render_thread_dispatcher.h"
#include "../renderer/gaussian_splat_renderer.h"

#include "core/config/engine.h"
#include "core/input/input_event.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
#include "main/performance.h"
#include "scene/3d/camera_3d.h"
#include "scene/gui/box_container.h"
#include "scene/gui/label.h"
#include "scene/gui/panel_container.h"
#include "scene/gui/rich_text_label.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/style_box_flat.h"

#include <atomic>

// ============================================================================
// FrameClock -- the #1084 frame-pacing spec, as pure bookkeeping.
// ============================================================================

void GaussianSplatPerformanceOverlay::FrameClock::reset() {
	*this = FrameClock();
}

bool GaussianSplatPerformanceOverlay::FrameClock::tick(uint64_t p_now_us, uint64_t p_frames_drawn, uint64_t p_window_us) {
	if (!started) {
		// First iteration: nothing to measure an interval against yet.
		started = true;
		window_start_us = p_now_us;
		window_start_frames = p_frames_drawn;
		last_draw_us = p_now_us;
		last_frames_drawn = p_frames_drawn;
		return false;
	}

	// Spec §2: a frame is an iteration that drew. Intervals are measured
	// between iterations at which Engine::get_frames_drawn() advanced, so a
	// low-processor-mode iteration that did not draw does not split an interval.
	if (p_frames_drawn != last_frames_drawn) {
		const uint64_t interval_us = p_now_us > last_draw_us ? p_now_us - last_draw_us : 0;
		last_draw_us = p_now_us;
		last_frames_drawn = p_frames_drawn;
		window_max_interval_us = MAX(window_max_interval_us, interval_us);
		ring[ring_head] = static_cast<uint32_t>(MIN(interval_us, uint64_t(UINT32_MAX)));
		ring_head = (ring_head + 1) % RING_CAPACITY;
		ring_count = MIN(ring_count + 1, RING_CAPACITY);
	}

	const uint64_t elapsed_us = p_now_us > window_start_us ? p_now_us - window_start_us : 0;
	if (elapsed_us < p_window_us || elapsed_us == 0) {
		return false;
	}

	const uint64_t frames = p_frames_drawn - window_start_frames;
	Window w;
	w.seconds = double(elapsed_us) / 1000000.0;
	w.frames = frames;
	if (frames > 0) {
		// Spec §3: frames over wall time. Spec §5: mean interval = window / frames.
		w.valid = true;
		w.fps = double(frames) / w.seconds;
		w.mean_interval_ms = (double(elapsed_us) / 1000.0) / double(frames);
		w.max_interval_ms = double(window_max_interval_us) / 1000.0;
	}
	last_window = w;

	window_start_us = p_now_us;
	window_start_frames = p_frames_drawn;
	window_max_interval_us = 0;
	return true;
}

bool GaussianSplatPerformanceOverlay::FrameClock::get_recent(double &r_fps, double &r_slowest_ms, int &r_count) const {
	if (ring_count <= 0) {
		return false;
	}
	uint64_t sum_us = 0;
	uint32_t slowest_us = 0;
	for (int i = 0; i < ring_count; i++) {
		sum_us += ring[i];
		slowest_us = MAX(slowest_us, ring[i]);
	}
	if (sum_us == 0) {
		return false;
	}
	// Spec §4: N / sum(intervals) -- never the arithmetic mean of per-frame 1/dt.
	r_fps = double(ring_count) * 1000000.0 / double(sum_us);
	r_slowest_ms = double(slowest_us) / 1000.0;
	r_count = ring_count;
	return true;
}

// ============================================================================
// Report building: a pure function of ReportInputs.
// ============================================================================

namespace {

const char *NA = "n/a";

// GPU passes whose timestamps the renderer resolves, in pipeline order:
// label, snapshot key, get_render_stats() time key, validity key. `gpu_frame_ms`
// is the sum of exactly these six, which is the identity checked below.
struct GpuPassRow {
	const char *label;
	const char *snapshot_key;
	const char *time_key;
	const char *valid_key;
};
const GpuPassRow GPU_PASSES[] = {
	{ "Overlap count", "overlap_count_ms", "gpu_overlap_count_ms", "gpu_overlap_count_valid" },
	{ "Prefix scan", "prefix_ms", "gpu_prefix_ms", "gpu_prefix_valid" },
	{ "Overlap emit", "overlap_emit_ms", "gpu_overlap_emit_ms", "gpu_overlap_emit_valid" },
	{ "Overlap sort", "overlap_sort_ms", "gpu_overlap_sort_ms", "gpu_overlap_sort_valid" },
	{ "Rasterize", "raster_ms", "gpu_raster_ms", "gpu_raster_valid" },
	{ "Resolve", "resolve_ms", "gpu_resolve_ms", "gpu_resolve_valid" },
};

// Tolerance for the pass-sum identity: the renderer sums the same six floats.
const double SUM_EPSILON_MS = 0.0005;

bool is_na(const Variant &p_v) {
	return p_v.get_type() == Variant::NIL;
}

Variant stat(const Dictionary &p_d, const char *p_key) {
	const StringName key(p_key);
	return p_d.has(key) ? p_d[key] : Variant();
}

// A GPU pass time, gated on that pass's own validity flag.
Variant valid_ms(const Dictionary &p_d, const char *p_time_key, const char *p_valid_key) {
	const Variant t = stat(p_d, p_time_key);
	const Variant v = stat(p_d, p_valid_key);
	if (is_na(t) || is_na(v) || !bool(v)) {
		return Variant();
	}
	return double(t);
}

// For producers that return a literal 0 when nothing answered (no renderer, no
// RenderingDevice, a stage that has not run): a host stage that ran takes a
// non-zero number of microseconds, and a live device never reports 0 bytes.
Variant nonzero_or_na(const Variant &p_v) {
	if (is_na(p_v) || double(p_v) <= 0.0) {
		return Variant();
	}
	return p_v;
}

// Counts only for BRANCHING, never for display.
int64_t flag(const Variant &p_v) {
	return is_na(p_v) ? 0 : int64_t(p_v);
}

String colorize_time(double p_ms, const String &p_text) {
	const char *color = p_ms < 8.0 ? "green" : (p_ms < 16.0 ? "yellow" : (p_ms < 33.0 ? "orange" : "red"));
	return vformat("[color=%s]%s[/color]", color, p_text);
}

String fmt_ms(const Variant &p_v) {
	if (is_na(p_v)) {
		return NA;
	}
	return colorize_time(double(p_v), vformat("%.3f ms", double(p_v)));
}

String fmt_plain_ms(const Variant &p_v) {
	if (is_na(p_v)) {
		return NA;
	}
	return vformat("%.3f ms", double(p_v));
}

String format_number(int64_t p_value) {
	if (p_value >= 1000000) {
		return vformat("%.2fM", double(p_value) / 1000000.0);
	}
	if (p_value >= 1000) {
		return vformat("%.1fK", double(p_value) / 1000.0);
	}
	return itos(p_value);
}

String fmt_count(const Variant &p_v) {
	return is_na(p_v) ? String(NA) : format_number(int64_t(p_v));
}

String fmt_num(const Variant &p_v, int p_decimals) {
	if (is_na(p_v)) {
		return NA;
	}
	return String::num(double(p_v), p_decimals).pad_decimals(p_decimals);
}

String fmt_text(const Variant &p_v) {
	if (is_na(p_v) || String(p_v).is_empty()) {
		return NA;
	}
	return String(p_v);
}

Variant monitor(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, const char *p_id) {
	return stat(p_in.monitors, p_id);
}

void header(Vector<String> &r_lines, const char *p_title) {
	if (!r_lines.is_empty()) {
		r_lines.push_back(String());
	}
	r_lines.push_back(vformat(U"[b]═══ %s ═══[/b]", p_title));
}

String no_target_line(const GaussianSplatPerformanceOverlay::ReportInputs &p_in) {
	return vformat(U"no splat node or world to describe — %s (%s)", NA,
			p_in.target_problem.is_empty() ? String("none found") : p_in.target_problem);
}

bool streaming_ready(const GaussianSplatPerformanceOverlay::ReportInputs &p_in) {
	return p_in.streaming_monitors_match && flag(monitor(p_in, "gaussian_splatting/streaming_monitor_ready")) == 1;
}

String streaming_absent_reason(const GaussianSplatPerformanceOverlay::ReportInputs &p_in) {
	if (!p_in.streaming_monitors_match && !p_in.monitor_mismatch.is_empty()) {
		return p_in.monitor_mismatch;
	}
	return "no streaming system attached";
}

// ---------------------------------------------------------------- FRAME ----
void section_frame(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "FRAME");
	Dictionary s;
	const auto &w = p_in.window;
	// Engine::get_frames_per_second() is a whole-second count of iterations; it
	// is 0 until the first second has elapsed, which is `n/a`, never 0 FPS.
	const Variant engine_fps = p_in.engine_fps > 0.0 ? Variant(p_in.engine_fps) : Variant();
	const String engine_text = is_na(engine_fps)
			? vformat("%s (engine, first second)", NA)
			: vformat("%d (engine, last 1 s)", int64_t(p_in.engine_fps));
	if (!w.valid) {
		r_lines.push_back(vformat("FPS: %s (measuring) | %s", NA, engine_text));
		r_lines.push_back(vformat("Frame interval (wall clock): %s", NA));
		s["fps"] = Variant();
		s["window_ms"] = Variant();
		s["window_frames"] = Variant();
		s["frame_interval_mean_ms"] = Variant();
		s["frame_interval_max_ms"] = Variant();
	} else {
		const double window_ms = w.seconds * 1000.0;
		r_lines.push_back(vformat("FPS: %.1f (drawn frames / wall time, last %.0f ms) | %s", w.fps, window_ms, engine_text));
		r_lines.push_back(vformat("Frame interval (wall clock): mean %.2f ms | max %.2f ms (last %.0f ms)",
				w.mean_interval_ms, w.max_interval_ms, window_ms));
		s["fps"] = w.fps;
		s["window_ms"] = window_ms;
		s["window_frames"] = int64_t(w.frames);
		s["frame_interval_mean_ms"] = w.mean_interval_ms;
		s["frame_interval_max_ms"] = w.max_interval_ms;
	}
	if (p_in.recent_valid) {
		r_lines.push_back(vformat("Last %d frames: %.1f FPS (frames / summed intervals) | slowest %.2f ms",
				p_in.recent_count, p_in.recent_fps, p_in.recent_slowest_ms));
		s["recent_frames"] = p_in.recent_count;
		s["recent_fps"] = p_in.recent_fps;
		s["recent_slowest_ms"] = p_in.recent_slowest_ms;
	} else {
		s["recent_frames"] = Variant();
		s["recent_fps"] = Variant();
		s["recent_slowest_ms"] = Variant();
	}
	// Spec §5: CPU time only from something that measured CPU work, clock named.
	// Performance TIME_PROCESS is the largest main-loop process time in the last
	// whole second; 0 before the first second.
	const Variant process_ms = p_in.process_time_s > 0.0 ? Variant(p_in.process_time_s * 1000.0) : Variant();
	r_lines.push_back(vformat("Process (CPU clock, max over last 1 s): %s", fmt_plain_ms(process_ms)));
	s["engine_fps"] = engine_fps;
	s["process_time_ms"] = process_ms;
	r_snap["frame"] = s;
}

// --------------------------------------------------------------- CAMERA ----
void section_camera(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "CAMERA");
	Dictionary s;
	if (!p_in.has_camera) {
		r_lines.push_back(vformat(U"no current Camera3D in this viewport — %s", NA));
		s["position"] = Variant();
		s["rotation_degrees"] = Variant();
		s["projection"] = Variant();
		r_snap["camera"] = s;
		return;
	}
	const Vector3 o = p_in.camera_transform.origin;
	const Vector3 e = p_in.camera_transform.basis.get_euler();
	const Vector3 deg(Math::rad_to_deg(e.x), Math::rad_to_deg(e.y), Math::rad_to_deg(e.z));
	r_lines.push_back(vformat("Pos: (%.2f, %.2f, %.2f)", o.x, o.y, o.z));
	r_lines.push_back(vformat(U"Rot: (%.1f°, %.1f°, %.1f°)", deg.x, deg.y, deg.z));
	const char *label = "Persp";
	const char *name = "perspective";
	if (p_in.camera_projection == Camera3D::PROJECTION_ORTHOGONAL) {
		label = "Ortho";
		name = "orthogonal";
	} else if (p_in.camera_projection == Camera3D::PROJECTION_FRUSTUM) {
		label = "Frustum";
		name = "frustum";
	}
	r_lines.push_back(vformat(U"Projection: %s | FOV: %.1f° | Size: %.2f",
			label, p_in.camera_fov, p_in.camera_size));
	s["position"] = o;
	s["rotation_degrees"] = deg;
	s["projection"] = name;
	r_snap["camera"] = s;
}

// ----------------------------------------------------------- GPU PASSES ----
void section_gpu_passes(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "GPU PASSES");
	Dictionary s;
	const Dictionary &st = p_in.render_stats;
	if (!p_in.has_target || st.is_empty()) {
		r_lines.push_back(p_in.has_target ? vformat(U"no renderer statistics this refresh — %s", NA) : no_target_line(p_in));
		for (const GpuPassRow &row : GPU_PASSES) {
			s[row.snapshot_key] = Variant();
		}
		s["pass_total_ms"] = Variant();
		s["timing_frames_behind"] = Variant();
		s["route_uid"] = Variant();
		s["data_source"] = Variant();
		s["sort_route_uid"] = Variant();
		r_snap["gpu_passes"] = s;
		return;
	}

	double sum_ms = 0.0;
	bool all_valid = true;
	bool any_valid = false;
	for (const GpuPassRow &row : GPU_PASSES) {
		const Variant value = valid_ms(st, row.time_key, row.valid_key);
		if (is_na(value)) {
			all_valid = false;
		} else {
			any_valid = true;
			sum_ms += double(value);
		}
		r_lines.push_back(vformat("%s: %s", row.label, fmt_ms(value)));
		s[row.snapshot_key] = value;
	}

	const Variant reported = valid_ms(st, "gpu_frame_ms", "gpu_frame_valid");
	if (!all_valid) {
		r_lines.push_back(vformat("Pass total: %s (a pass above is %s, so the rows do not sum)", fmt_ms(reported), NA));
	} else if (is_na(reported)) {
		r_lines.push_back(vformat("Pass total: %s | rows sum to %s", NA, fmt_plain_ms(sum_ms)));
	} else {
		const double delta = Math::abs(double(reported) - sum_ms);
		if (delta <= SUM_EPSILON_MS) {
			r_lines.push_back(vformat("Pass total: %s (= sum of the six rows)", fmt_ms(reported)));
		} else {
			r_lines.push_back(vformat(U"Pass total: %s | rows sum to %s | [color=orange]Δ %.4f ms[/color]",
					fmt_ms(reported), fmt_plain_ms(sum_ms), delta));
		}
	}
	s["pass_total_ms"] = reported;

	// The validity flags are sticky (~2 s), so print how far behind the rows may
	// be. `gpu_timing_frames_behind` is zero-initialised: before any pass has
	// resolved, "resolved this frame" beside six n/a rows would be a lie.
	const Variant behind = stat(st, "gpu_timing_frames_behind");
	if (!any_valid) {
		r_lines.push_back(vformat("Timing age: %s (no pass has resolved yet)", NA));
		s["timing_frames_behind"] = Variant();
	} else if (is_na(behind)) {
		r_lines.push_back(vformat("Timing age: %s", NA));
		s["timing_frames_behind"] = Variant();
	} else if (int64_t(behind) == 0) {
		r_lines.push_back("Timing age: resolved this frame");
		s["timing_frames_behind"] = int64_t(0);
	} else {
		r_lines.push_back(vformat("Timing age: %d frame(s) behind (flags are sticky to ~2 s)", int64_t(behind)));
		s["timing_frames_behind"] = int64_t(behind);
	}

	const Variant route = stat(st, "route_uid");
	const Variant source = stat(st, "data_source");
	const Variant sort_route = stat(st, "sort_route_uid");
	r_lines.push_back(vformat("Route: %s | source: %s", fmt_text(route), fmt_text(source)));
	r_lines.push_back(vformat("Sort route: %s", fmt_text(sort_route)));
	s["route_uid"] = fmt_text(route) == NA ? Variant() : route;
	s["data_source"] = fmt_text(source) == NA ? Variant() : source;
	s["sort_route_uid"] = fmt_text(sort_route) == NA ? Variant() : sort_route;
	r_snap["gpu_passes"] = s;
}

// ---------------------------------------------------------- HOST STAGES ----
void section_host_stages(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "HOST STAGES (CPU clock)");
	Dictionary s;
	// TileRenderer setup comes from the process-global monitors: shown only when
	// they unambiguously describe this overlay's target.
	Variant setup;
	if (p_in.tile_monitors_match) {
		setup = nonzero_or_na(monitor(p_in, "gaussian_splatting/cpu_setup_time_ms"));
		r_lines.push_back(vformat("TileRenderer setup: %s", fmt_ms(setup)));
	} else {
		r_lines.push_back(vformat("TileRenderer setup: %s (%s)", NA,
				p_in.monitor_mismatch.is_empty() ? String("no target") : p_in.monitor_mismatch));
	}
	// `cull_ms` is zero-initialised on both of its paths before any cull ran.
	const Variant cull = nonzero_or_na(stat(p_in.render_stats, "cull_ms"));
	r_lines.push_back(vformat("Cull stage: %s", fmt_ms(cull)));
	Variant dispatch;
	if (bool(stat(p_in.render_stats, "overlap_sort_cpu_dispatch_valid"))) {
		dispatch = stat(p_in.render_stats, "overlap_sort_cpu_dispatch_ms");
	}
	r_lines.push_back(vformat("Sort dispatch: %s", fmt_ms(dispatch)));
	s["tile_renderer_setup_ms"] = setup;
	s["cull_ms"] = cull;
	s["sort_dispatch_ms"] = dispatch;
	r_snap["host_stages"] = s;
}

// ----------------------------------------------------------- VISIBILITY ----
void section_visibility(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "VISIBILITY");
	Dictionary s;
	const Dictionary &st = p_in.render_stats;
	const char *keys[] = { "resident_splats", "cull_domain", "cull_visible", "cull_candidates", "culled_by_frustum",
		"culled_by_distance", "culled_by_screen", "culled_by_importance", "tile_count", "tiles_processed",
		"tiles_aggregated", "tiles_overflow", "reject_clip", "reject_radius", "reject_viewport", "reject_aspect",
		"overlap_records", "overlap_record_budget" };
	for (const char *k : keys) {
		s[k] = Variant();
	}
	if (!p_in.has_target || st.is_empty()) {
		r_lines.push_back(p_in.has_target ? vformat(U"no renderer statistics this refresh — %s", NA) : no_target_line(p_in));
		r_snap["visibility"] = s;
		return;
	}

	const Variant resident = stat(st, "total_splats");
	r_lines.push_back(vformat("Resident splats: %s", fmt_count(resident)));
	s["resident_splats"] = resident;

	// The cull counters mean something only once a cull stage has run; before
	// that they are zero-initialised ("0 of 1 visible" is not a real state).
	if (!bool(stat(st, "stage_metrics_valid"))) {
		r_lines.push_back(vformat("Cull: %s (no cull stage has run yet)", NA));
	} else {
		// `visible_splats` is not a per-splat frustum count on a resident scene:
		// the cull runs over chunk references, so the domain is named.
		const String domain = fmt_text(stat(st, "stage_cull_visible_domain"));
		const Variant visible = stat(st, "stage_cull_visible_count");
		const Variant candidates = stat(st, "stage_cull_candidate_count");
		r_lines.push_back(vformat(U"Cull domain: %s — %s of %s visible", domain == NA ? String("unknown") : domain,
				fmt_count(visible), fmt_count(candidates)));
		const Variant fr = stat(st, "culled_by_frustum");
		const Variant di = stat(st, "culled_by_distance");
		const Variant sc = stat(st, "culled_by_screen");
		const Variant im = stat(st, "culled_by_importance");
		r_lines.push_back(vformat("Culled: frustum %s | distance %s | screen %s | importance %s",
				fmt_count(fr), fmt_count(di), fmt_count(sc), fmt_count(im)));
		if (!is_na(visible) && !is_na(candidates)) {
			const int64_t accounted = int64_t(visible) + flag(fr) + flag(di) + flag(sc) + flag(im);
			if (accounted != int64_t(candidates)) {
				r_lines.push_back(vformat("[color=orange]Cull accounting incomplete this frame: %d of %d candidates accounted for[/color]",
						accounted, int64_t(candidates)));
			}
		}
		if (domain != "splat") {
			r_lines.push_back("[i]Per-splat visibility is not measured on this route.[/i]");
		}
		s["cull_domain"] = domain == NA ? Variant() : Variant(domain);
		s["cull_visible"] = visible;
		s["cull_candidates"] = candidates;
		s["culled_by_frustum"] = fr;
		s["culled_by_distance"] = di;
		s["culled_by_screen"] = sc;
		s["culled_by_importance"] = im;
	}

	// Tile-projection counters are process-global monitors whose getters return
	// a zero-initialised cache before the tile pass ran; `total_processed > 0`
	// is the liveness test the monitors themselves use.
	if (!p_in.tile_monitors_match) {
		r_lines.push_back(vformat("Tile rejects / tiles: %s (%s)", NA,
				p_in.monitor_mismatch.is_empty() ? String("no target") : p_in.monitor_mismatch));
	} else {
		const Variant processed = monitor(p_in, "gaussian_splatting/total_processed");
		if (is_na(processed) || int64_t(processed) <= 0) {
			r_lines.push_back(vformat("Tile rejects / tiles: %s (no tile pass yet)", NA));
		} else {
			const Variant clip = monitor(p_in, "gaussian_splatting/clip_reject_count");
			const Variant radius = monitor(p_in, "gaussian_splatting/radius_reject_count");
			const Variant viewport = monitor(p_in, "gaussian_splatting/viewport_reject_count");
			const Variant aspect = monitor(p_in, "gaussian_splatting/extreme_aspect_count");
			const Variant tiles = monitor(p_in, "gaussian_splatting/tile_count");
			const Variant aggregated = monitor(p_in, "gaussian_splatting/aggregated_count");
			const Variant overflow = monitor(p_in, "gaussian_splatting/overflow_tile_count");
			r_lines.push_back(vformat("Tile rejects: clip %s | radius %s | viewport %s | aspect %s",
					fmt_count(clip), fmt_count(radius), fmt_count(viewport), fmt_count(aspect)));
			r_lines.push_back(vformat("Tiles: %s | processed: %s | aggregated: %s | overflow: %s",
					fmt_count(tiles), fmt_count(processed), fmt_count(aggregated), fmt_count(overflow)));
			s["reject_clip"] = clip;
			s["reject_radius"] = radius;
			s["reject_viewport"] = viewport;
			s["reject_aspect"] = aspect;
			s["tile_count"] = tiles;
			s["tiles_processed"] = processed;
			s["tiles_aggregated"] = aggregated;
			s["tiles_overflow"] = overflow;
		}
	}

	// A configured budget of 0 means overlap accounting is not running on this
	// route, which is not "0 records used".
	const Variant used = stat(st, "overlap_records");
	const Variant budget = stat(st, "overlap_record_budget");
	if (is_na(budget) || int64_t(budget) <= 0 || is_na(used)) {
		r_lines.push_back(vformat("Overlap records: %s (no record budget configured on this route)", NA));
	} else {
		const double pct = double(used) / double(budget) * 100.0;
		const String pct_text = vformat("%.1f%%", pct);
		r_lines.push_back(vformat("Overlap records: %s / %s (%s)", fmt_count(used), fmt_count(budget),
				pct < 70.0 ? pct_text : vformat("[color=%s]%s[/color]", pct < 85.0 ? "yellow" : "orange", pct_text)));
		s["overlap_records"] = used;
		s["overlap_record_budget"] = budget;
	}
	r_snap["visibility"] = s;
}

// ---------------------------------------------------------- DEVICE VRAM ----
void section_device_vram(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "DEVICE VRAM");
	Dictionary s;
	// Registered monitors that return a literal 0 with no device to ask; a live
	// device never reports zero bytes total, so total is the gate.
	const Variant total = nonzero_or_na(monitor(p_in, "gaussian_splatting/vram_device_total_mb"));
	Variant buffers;
	Variant textures;
	if (is_na(total)) {
		r_lines.push_back(vformat("RenderingDevice: %s (no active device answered)", NA));
	} else {
		buffers = monitor(p_in, "gaussian_splatting/vram_device_buffers_mb");
		textures = monitor(p_in, "gaussian_splatting/vram_device_textures_mb");
		r_lines.push_back(vformat("RenderingDevice total: %s MB", fmt_num(total, 1)));
		r_lines.push_back(vformat("Buffers: %s MB | Textures: %s MB", fmt_num(buffers, 1), fmt_num(textures, 1)));
	}
	s["device_total_mb"] = total;
	s["device_buffers_mb"] = buffers;
	s["device_textures_mb"] = textures;

	s["streaming_usage_mb"] = Variant();
	s["streaming_budget_mb"] = Variant();
	s["streaming_usage_percent"] = Variant();
	if (!streaming_ready(p_in)) {
		r_lines.push_back(vformat("Streaming VRAM budget: %s (%s)", NA, streaming_absent_reason(p_in)));
		r_snap["device_vram"] = s;
		return;
	}
	const Variant usage = monitor(p_in, "gaussian_splatting/vram_current_usage_mb");
	const Variant budget = monitor(p_in, "gaussian_splatting/vram_budget_mb");
	const Variant percent = monitor(p_in, "gaussian_splatting/vram_usage_percent");
	String warning;
	if (flag(monitor(p_in, "gaussian_splatting/vram_budget_warning_active")) > 0) {
		warning = U"[color=orange]⚠ WARNING[/color] ";
	}
	String percent_text = NA;
	if (!is_na(percent)) {
		const double p = double(percent);
		const String t = vformat("%.1f%%", p);
		percent_text = p < 60.0 ? t : vformat("[color=%s]%s[/color]", p < 80.0 ? "yellow" : (p < 95.0 ? "orange" : "red"), t);
	}
	r_lines.push_back(vformat("%sStreaming budget: %s / %s MB (%s)", warning, fmt_num(usage, 1), fmt_num(budget, 1), percent_text));
	r_lines.push_back(vformat("Evicted this frame: %s | thrashing events: %s",
			fmt_count(monitor(p_in, "gaussian_splatting/vram_evicted_this_frame")),
			fmt_count(monitor(p_in, "gaussian_splatting/vram_thrashing_events"))));
	s["streaming_usage_mb"] = usage;
	s["streaming_budget_mb"] = budget;
	s["streaming_usage_percent"] = percent;
	r_snap["device_vram"] = s;
}

// ------------------------------------------------------------------ LOD ----
void section_lod(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "LOD");
	Dictionary s;
	if (!streaming_ready(p_in)) {
		// Every LOD monitor is streaming-gated, and two return 1 / 1.0 with no
		// renderer at all -- exactly what a live reading looks like.
		r_lines.push_back(vformat(U"%s — all rows %s", streaming_absent_reason(p_in), NA));
		s["level"] = Variant();
		s["reduction_pct"] = Variant();
		r_snap["lod"] = s;
		return;
	}
	const Variant reduction = monitor(p_in, "gaussian_splatting/lod_reduction_ratio_pct");
	String reduction_text = NA;
	if (!is_na(reduction)) {
		const double r = double(reduction);
		reduction_text = vformat("[color=%s]%.1f%%[/color]", r < 25.0 ? "green" : (r < 50.0 ? "yellow" : (r < 75.0 ? "orange" : "red")), r);
	}
	const Variant level = monitor(p_in, "gaussian_splatting/lod_current_level");
	r_lines.push_back(vformat("Level (avg): %s | reduction: %s", fmt_count(level), reduction_text));
	r_lines.push_back(vformat("Chunk distance: min %s | avg %s | max %s",
			fmt_num(monitor(p_in, "gaussian_splatting/lod_min_chunk_distance"), 1),
			fmt_num(monitor(p_in, "gaussian_splatting/lod_avg_chunk_distance"), 1),
			fmt_num(monitor(p_in, "gaussian_splatting/lod_max_chunk_distance"), 1)));
	r_lines.push_back(vformat("Splat skip: %sx | opacity: %s | transitioning: %s chunks",
			fmt_count(monitor(p_in, "gaussian_splatting/lod_splat_skip_factor")),
			fmt_num(monitor(p_in, "gaussian_splatting/lod_opacity_multiplier"), 2),
			fmt_count(monitor(p_in, "gaussian_splatting/lod_chunks_in_transition"))));
	if (flag(monitor(p_in, "gaussian_splatting/lod_quality_degradation_active")) > 0) {
		r_lines.push_back(U"[color=orange]⚠ Quality degradation active (VRAM pressure)[/color]");
	}
	s["level"] = level;
	s["reduction_pct"] = reduction;
	r_snap["lod"] = s;
}

// ------------------------------------------------------------ STREAMING ----
void section_streaming(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "STREAMING");
	Dictionary s;
	if (!streaming_ready(p_in)) {
		r_lines.push_back(vformat(U"%s — all rows %s", streaming_absent_reason(p_in), NA));
		s["visible_chunks"] = Variant();
		s["loaded_chunks"] = Variant();
		s["total_chunks"] = Variant();
		r_snap["streaming"] = s;
		return;
	}
	const Variant visible = monitor(p_in, "gaussian_splatting/streaming_visible_chunks");
	const Variant loaded = monitor(p_in, "gaussian_splatting/streaming_loaded_chunks");
	const Variant total = monitor(p_in, "gaussian_splatting/streaming_total_chunks");
	r_lines.push_back(vformat("Chunks: visible %s | loaded %s / %s | resident %s", fmt_count(visible), fmt_count(loaded),
			fmt_count(total), fmt_count(monitor(p_in, "gaussian_splatting/streaming_resident_chunks"))));
	r_lines.push_back(vformat("This frame: +%s loaded | -%s evicted",
			fmt_count(monitor(p_in, "gaussian_splatting/streaming_chunks_loaded_this_frame")),
			fmt_count(monitor(p_in, "gaussian_splatting/streaming_chunks_evicted_this_frame"))));
	r_lines.push_back(vformat("Buffer capacity: %s splats | effective: %s",
			fmt_count(monitor(p_in, "gaussian_splatting/streaming_buffer_capacity_splats")),
			fmt_count(monitor(p_in, "gaussian_splatting/streaming_effective_splat_count"))));
	r_lines.push_back(vformat("Uploaded: %s MB total | queue depth %s",
			fmt_num(monitor(p_in, "gaussian_splatting/memory_stream_total_bytes_uploaded_mb"), 2),
			fmt_count(monitor(p_in, "gaussian_splatting/chunk_upload_queue_depth"))));
	r_lines.push_back(vformat("Caps: frame %s MB | slice %s MB | %s MB/s",
			fmt_num(monitor(p_in, "gaussian_splatting/streaming_effective_upload_cap_mb_per_frame"), 0),
			fmt_num(monitor(p_in, "gaussian_splatting/streaming_effective_upload_cap_mb_per_slice"), 0),
			fmt_num(monitor(p_in, "gaussian_splatting/streaming_effective_upload_cap_mb_per_second"), 0)));
	r_lines.push_back(vformat("VRAM cap: %s MB | max chunks %s",
			fmt_num(monitor(p_in, "gaussian_splatting/streaming_effective_vram_budget_mb"), 0),
			fmt_count(monitor(p_in, "gaussian_splatting/streaming_effective_vram_max_chunks"))));

	PackedStringArray markers;
	if (flag(monitor(p_in, "gaussian_splatting/streaming_upload_frame_cap_hit")) > 0) {
		markers.push_back("upload/frame");
	}
	if (flag(monitor(p_in, "gaussian_splatting/streaming_upload_bandwidth_cap_hit")) > 0) {
		markers.push_back("upload/bandwidth");
	}
	if (flag(monitor(p_in, "gaussian_splatting/streaming_chunk_load_cap_hit")) > 0) {
		markers.push_back("chunk-load");
	}
	if (flag(monitor(p_in, "gaussian_splatting/streaming_vram_chunk_cap_hit")) > 0) {
		markers.push_back("vram");
	}
	if (flag(monitor(p_in, "gaussian_splatting/streaming_queue_pressure_active")) > 0) {
		markers.push_back("queue");
	}
	if (!markers.is_empty()) {
		r_lines.push_back(vformat("[color=orange]Pressure: %s[/color]", String(", ").join(markers)));
	}
	const Variant stall = monitor(p_in, "gaussian_splatting/memory_stream_stall_percent");
	String stall_text = NA;
	if (!is_na(stall)) {
		// Higher is worse.
		stall_text = double(stall) > 5.0 ? vformat("[color=orange]%.1f%%[/color]", double(stall)) : vformat("%.1f%%", double(stall));
	}
	r_lines.push_back(vformat("Pipeline stalls: %s", stall_text));
	s["visible_chunks"] = visible;
	s["loaded_chunks"] = loaded;
	s["total_chunks"] = total;
	r_snap["streaming"] = s;
}

// ------------------------------------------------------- SH COMPRESSION ----
void section_sh_compression(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "SH COMPRESSION");
	Dictionary s;
	if (!streaming_ready(p_in)) {
		r_lines.push_back(vformat(U"%s — all rows %s", streaming_absent_reason(p_in), NA));
		s["ratio_pct"] = Variant();
		r_snap["sh_compression"] = s;
		return;
	}
	const Variant raw = monitor(p_in, "gaussian_splatting/sh_compression_raw_mb");
	const Variant compressed = monitor(p_in, "gaussian_splatting/sh_compression_compressed_mb");
	const Variant ratio = monitor(p_in, "gaussian_splatting/sh_compression_ratio_pct");
	r_lines.push_back(vformat(U"Raw: %s MB → compressed: %s MB", fmt_num(raw, 2), fmt_num(compressed, 2)));
	String ratio_text = NA;
	if (!is_na(ratio)) {
		// Higher is better: a healthy ratio must not be painted in a warning colour.
		ratio_text = vformat("[color=%s]%.1f%%[/color]", double(ratio) > 50.0 ? "green" : "yellow", double(ratio));
	}
	r_lines.push_back(vformat("Ratio: %s", ratio_text));
	s["ratio_pct"] = ratio;
	r_snap["sh_compression"] = s;
}

// ----------------------------------------------------------------- NODE ----
String policy_name(int p_policy) {
	switch (p_policy) {
		case 1:
			return "force_on";
		case 2:
			return "force_off";
		case 0:
			return "default";
		default:
			return NA;
	}
}

void section_node(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	header(r_lines, "NODE");
	Dictionary s;
	if (!p_in.has_target) {
		r_lines.push_back(no_target_line(p_in));
		s["name"] = Variant();
		s["kind"] = Variant();
		s["total_splats"] = Variant();
		s["last_update_ms"] = Variant();
		s["raster_policy"] = Variant();
		r_snap["node"] = s;
		return;
	}
	r_lines.push_back(vformat("%s (%s)", p_in.target_name, p_in.target_kind));
	Variant total;
	Variant update;
	if (p_in.has_node) {
		total = p_in.node_total_splats;
		// `last_update_time_ms` is zero-initialised and written only at the end
		// of update_splats(): before the first update it is not a measurement.
		update = nonzero_or_na(p_in.node_last_update_ms);
	} else {
		total = stat(p_in.render_stats, "total_splats");
	}
	r_lines.push_back(vformat("Total splats: %s", fmt_count(total)));
	r_lines.push_back(vformat("Last update: %s", p_in.has_node ? fmt_ms(update) : vformat("%s (world target has no node update)", NA)));
	// Read-only (D2 of #1084): the overlay observes; the policy is set on the
	// renderer (`debug/compute_raster_policy`), not by a key bound in every project.
	const String policy = policy_name(p_in.raster_policy);
	r_lines.push_back(vformat("Raster policy: %s", policy));
	s["name"] = p_in.target_name;
	s["kind"] = p_in.target_kind;
	s["total_splats"] = total;
	s["last_update_ms"] = update;
	s["raster_policy"] = policy == NA ? Variant() : Variant(policy);
	r_snap["node"] = s;
}

// -------------------------------------------------------------- MANAGER ----
void section_manager(const GaussianSplatPerformanceOverlay::ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snap) {
	if (!p_in.has_manager) {
		return;
	}
	header(r_lines, "MANAGER");
	Dictionary s;
	// Totals over buffers registered with GaussianSplatManager. The node/renderer
	// route registers none, so they are structurally 0 there -- shown only when
	// the registry is non-empty. `gpu_sorting_enabled` is a deprecated no-op
	// (gaussian_splat_manager.cpp) and is deliberately not shown.
	const int64_t buffers = flag(stat(p_in.manager_stats, "buffer_count"));
	if (buffers > 0) {
		r_lines.push_back(vformat("Registered buffers: %d | gaussians: %s | %s MB", buffers,
				fmt_count(stat(p_in.manager_stats, "total_gaussians")), fmt_num(stat(p_in.manager_stats, "total_memory_mb"), 2)));
		s["buffer_count"] = buffers;
		s["total_gaussians"] = stat(p_in.manager_stats, "total_gaussians");
		s["total_memory_mb"] = stat(p_in.manager_stats, "total_memory_mb");
	} else {
		r_lines.push_back(vformat(U"Buffer registry: empty — per-node route registers none, so its totals are %s", NA));
		s["buffer_count"] = Variant();
		s["total_gaussians"] = Variant();
		s["total_memory_mb"] = Variant();
	}
	r_snap["manager"] = s;
}

} // namespace

void GaussianSplatPerformanceOverlay::build_report(const ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snapshot) {
	r_lines.clear();
	r_snapshot.clear();
	const uint32_t sec = p_in.sections;
	if (sec & SECTION_FRAME) {
		section_frame(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_CAMERA) {
		section_camera(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_GPU_PASSES) {
		section_gpu_passes(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_HOST_STAGES) {
		section_host_stages(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_VISIBILITY) {
		section_visibility(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_DEVICE_VRAM) {
		section_device_vram(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_LOD) {
		section_lod(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_STREAMING) {
		section_streaming(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_SH_COMPRESSION) {
		section_sh_compression(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_NODE) {
		section_node(p_in, r_lines, r_snapshot);
	}
	if (sec & SECTION_MANAGER) {
		section_manager(p_in, r_lines, r_snapshot);
	}
}

// Splits rows across the two columns on section boundaries, choosing the split
// that minimises the taller column (a single ~50-row column is taller than a
// 720 px viewport). A header is never separated from its rows.
void GaussianSplatPerformanceOverlay::_split_columns(const Vector<String> &p_lines, Vector<String> &r_left, Vector<String> &r_right) {
	Vector<Vector<String>> blocks;
	Vector<String> current;
	for (const String &line : p_lines) {
		if (line.begins_with(U"[b]═══") && !current.is_empty()) {
			blocks.push_back(current);
			current.clear();
		}
		current.push_back(line);
	}
	if (!current.is_empty()) {
		blocks.push_back(current);
	}
	const int total = p_lines.size();
	int best_split = blocks.size();
	int best_cost = total;
	int running = 0;
	for (int i = 1; i < blocks.size(); i++) {
		running += blocks[i - 1].size();
		const int cost = MAX(running, total - running);
		if (cost < best_cost) {
			best_cost = cost;
			best_split = i;
		}
	}
	r_left.clear();
	r_right.clear();
	for (int i = 0; i < blocks.size(); i++) {
		PackedStringArray section;
		for (const String &line : blocks[i]) {
			section.push_back(line);
		}
		(i < best_split ? r_left : r_right).push_back(String("\n").join(section).strip_edges());
	}
}

void GaussianSplatPerformanceOverlay::_apply_column(int p_column, const Vector<String> &p_sections) {
	Vector<RichTextLabel *> &labels = section_labels[p_column];
	Vector<String> &cache = section_texts[p_column];
	for (int i = 0; i < labels.size(); i++) {
		const bool used = i < p_sections.size();
		if (labels[i]->is_visible() != used) {
			labels[i]->set_visible(used);
		}
		const String text = used ? p_sections[i] : String();
		// Re-set only what changed: set_text() re-parses and re-shapes the whole
		// label, and on a -O0 build that layout, not the statistics read, was the
		// dominant main-thread cost of a refresh (#1084 measurement).
		if (cache[i] != text) {
			cache.write[i] = text;
			labels[i]->set_text(text);
		}
	}
}

// ============================================================================
// Node plumbing
// ============================================================================

void GaussianSplatPerformanceOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_target_path", "path"), &GaussianSplatPerformanceOverlay::set_target_path);
	ClassDB::bind_method(D_METHOD("get_target_path"), &GaussianSplatPerformanceOverlay::get_target_path);
	ClassDB::bind_method(D_METHOD("set_target", "node"), &GaussianSplatPerformanceOverlay::set_target);
	ClassDB::bind_method(D_METHOD("get_target"), &GaussianSplatPerformanceOverlay::get_target);
	ClassDB::bind_method(D_METHOD("set_update_interval", "seconds"), &GaussianSplatPerformanceOverlay::set_update_interval);
	ClassDB::bind_method(D_METHOD("get_update_interval"), &GaussianSplatPerformanceOverlay::get_update_interval);
	ClassDB::bind_method(D_METHOD("set_sections", "sections"), &GaussianSplatPerformanceOverlay::set_sections);
	ClassDB::bind_method(D_METHOD("get_sections"), &GaussianSplatPerformanceOverlay::get_sections);
	ClassDB::bind_method(D_METHOD("set_corner", "corner"), &GaussianSplatPerformanceOverlay::set_corner);
	ClassDB::bind_method(D_METHOD("get_corner"), &GaussianSplatPerformanceOverlay::get_corner);
	ClassDB::bind_method(D_METHOD("set_hide_key", "key"), &GaussianSplatPerformanceOverlay::set_hide_key);
	ClassDB::bind_method(D_METHOD("get_hide_key"), &GaussianSplatPerformanceOverlay::get_hide_key);
	ClassDB::bind_method(D_METHOD("set_title", "title"), &GaussianSplatPerformanceOverlay::set_title);
	ClassDB::bind_method(D_METHOD("get_title"), &GaussianSplatPerformanceOverlay::get_title);
	ClassDB::bind_method(D_METHOD("set_font_size", "size"), &GaussianSplatPerformanceOverlay::set_font_size);
	ClassDB::bind_method(D_METHOD("get_font_size"), &GaussianSplatPerformanceOverlay::get_font_size);
	ClassDB::bind_method(D_METHOD("refresh_now"), &GaussianSplatPerformanceOverlay::refresh_now);
	ClassDB::bind_method(D_METHOD("get_snapshot"), &GaussianSplatPerformanceOverlay::get_snapshot);
	ClassDB::bind_method(D_METHOD("get_last_refresh_usec"), &GaussianSplatPerformanceOverlay::get_last_refresh_usec);
	ClassDB::bind_method(D_METHOD("get_refresh_count"), &GaussianSplatPerformanceOverlay::get_refresh_count);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "target_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "GaussianSplatNode3D,GaussianSplatWorld3D"), "set_target_path", "get_target_path");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "update_interval", PROPERTY_HINT_RANGE, "0.25,2.0,0.05,suffix:s"), "set_update_interval", "get_update_interval");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sections", PROPERTY_HINT_FLAGS, "Frame,Camera,GPU Passes,Host Stages,Visibility,Device VRAM,LOD,Streaming,SH Compression,Node,Manager"), "set_sections", "get_sections");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "corner", PROPERTY_HINT_ENUM, "Top Left,Top Right,Bottom Left,Bottom Right"), "set_corner", "get_corner");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "hide_key"), "set_hide_key", "get_hide_key");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "title"), "set_title", "get_title");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "font_size", PROPERTY_HINT_RANGE, "8,32,1"), "set_font_size", "get_font_size");

	BIND_ENUM_CONSTANT(CORNER_TOP_LEFT);
	BIND_ENUM_CONSTANT(CORNER_TOP_RIGHT);
	BIND_ENUM_CONSTANT(CORNER_BOTTOM_LEFT);
	BIND_ENUM_CONSTANT(CORNER_BOTTOM_RIGHT);

	BIND_BITFIELD_FLAG(SECTION_FRAME);
	BIND_BITFIELD_FLAG(SECTION_CAMERA);
	BIND_BITFIELD_FLAG(SECTION_GPU_PASSES);
	BIND_BITFIELD_FLAG(SECTION_HOST_STAGES);
	BIND_BITFIELD_FLAG(SECTION_VISIBILITY);
	BIND_BITFIELD_FLAG(SECTION_DEVICE_VRAM);
	BIND_BITFIELD_FLAG(SECTION_LOD);
	BIND_BITFIELD_FLAG(SECTION_STREAMING);
	BIND_BITFIELD_FLAG(SECTION_SH_COMPRESSION);
	BIND_BITFIELD_FLAG(SECTION_NODE);
	BIND_BITFIELD_FLAG(SECTION_MANAGER);
	BIND_BITFIELD_FLAG(SECTION_ALL);
	BIND_BITFIELD_FLAG(SECTION_DEFAULT);
}

GaussianSplatPerformanceOverlay::GaussianSplatPerformanceOverlay() {
	set_layer(100);
	// Keep measuring while the tree is paused: pausing does not stop drawing.
	set_process_mode(PROCESS_MODE_ALWAYS);
	_build_ui();
}

void GaussianSplatPerformanceOverlay::_build_ui() {
	Ref<StyleBoxFlat> style;
	style.instantiate();
	style->set_content_margin_individual(12, 8, 12, 8);
	style->set_bg_color(Color(0.08, 0.1, 0.13, 0.92));
	style->set_corner_radius_all(8);
	style->set_border_color(Color(0.2, 0.4, 0.7, 0.8));
	style->set_border_width_all(1);

	// Every Control here ignores the mouse. Control defaults to STOP and does not
	// inherit the filter; a panel that eats events kills RMB-orbit, MMB-pan and
	// wheel-zoom over half the viewport for any _unhandled_input camera rig.
	panel = memnew(PanelContainer);
	panel->set_name("Panel");
	panel->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	panel->add_theme_style_override("panel", style);
	add_child(panel, false, INTERNAL_MODE_FRONT);

	VBoxContainer *vbox = memnew(VBoxContainer);
	vbox->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	panel->add_child(vbox);

	title_label = memnew(Label);
	title_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	title_label->add_theme_color_override("font_color", Color(0.85, 0.95, 1.0));
	vbox->add_child(title_label);

	HBoxContainer *columns = memnew(HBoxContainer);
	columns->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	columns->add_theme_constant_override("separation", 16);
	vbox->add_child(columns);

	for (int column = 0; column < 2; column++) {
		VBoxContainer *column_box = memnew(VBoxContainer);
		column_box->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
		column_box->set_custom_minimum_size(Size2(340, 0));
		column_box->add_theme_constant_override("separation", 6);
		columns->add_child(column_box);
		for (int slot = 0; slot < SECTION_SLOTS; slot++) {
			RichTextLabel *label = memnew(RichTextLabel);
			label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
			label->set_use_bbcode(true);
			label->set_fit_content(true);
			label->set_scroll_active(false);
			label->set_visible(false);
			column_box->add_child(label);
			section_labels[column].push_back(label);
			section_texts[column].push_back(String());
		}
	}
	section_labels[0][0]->set_text(String(U"Measuring… (") + NA + ")");
	section_labels[0][0]->set_visible(true);

	footer_label = memnew(Label);
	footer_label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	footer_label->add_theme_color_override("font_color", Color(0.7, 0.8, 0.9));
	vbox->add_child(footer_label);

	_apply_font_size();
	_apply_corner();
}

void GaussianSplatPerformanceOverlay::_apply_font_size() {
	if (!title_label) {
		return;
	}
	for (int column = 0; column < 2; column++) {
		for (RichTextLabel *label : section_labels[column]) {
			label->add_theme_font_size_override("normal_font_size", font_size);
			label->add_theme_font_size_override("bold_font_size", font_size);
			label->add_theme_font_size_override("italics_font_size", font_size);
		}
	}
	title_label->add_theme_font_size_override("font_size", font_size + 5);
	footer_label->add_theme_font_size_override("font_size", MAX(8, font_size - 1));
}

void GaussianSplatPerformanceOverlay::_apply_corner() {
	if (!panel) {
		return;
	}
	static const Control::LayoutPreset presets[] = { Control::PRESET_TOP_LEFT, Control::PRESET_TOP_RIGHT,
		Control::PRESET_BOTTOM_LEFT, Control::PRESET_BOTTOM_RIGHT };
	panel->set_anchors_and_offsets_preset(presets[corner], Control::PRESET_MODE_MINSIZE, 16);
	const bool right = corner == CORNER_TOP_RIGHT || corner == CORNER_BOTTOM_RIGHT;
	const bool bottom = corner == CORNER_BOTTOM_LEFT || corner == CORNER_BOTTOM_RIGHT;
	panel->set_h_grow_direction(right ? Control::GROW_DIRECTION_BEGIN : Control::GROW_DIRECTION_END);
	panel->set_v_grow_direction(bottom ? Control::GROW_DIRECTION_BEGIN : Control::GROW_DIRECTION_END);
}

void GaussianSplatPerformanceOverlay::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			frame_clock.reset();
			set_process_internal(true);
			set_process_unhandled_key_input(true);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
			target_id = ObjectID();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// Spec §1/§8: one wall-clock read per main-loop iteration, never
			// `delta` (it is scaled by Engine.time_scale), and O(1) work. Nothing
			// here touches the RenderingServer or the renderer.
			const uint64_t window_us = uint64_t(update_interval * 1000000.0);
			const bool completed = frame_clock.tick(OS::get_singleton()->get_ticks_usec(),
					Engine::get_singleton()->get_frames_drawn(), window_us);
			// Statistics are read once per completed window (<= 4 Hz), and only
			// while shown: hidden, the clock keeps counting but nothing is read.
			if (completed && is_visible()) {
				_refresh();
			}
		} break;
	}
}

void GaussianSplatPerformanceOverlay::unhandled_key_input(const Ref<InputEvent> &p_event) {
	const Ref<InputEventKey> key = p_event;
	if (key.is_null() || !key->is_pressed() || key->is_echo() || hide_key == Key::NONE) {
		return;
	}
	if (key->get_keycode() != hide_key) {
		return;
	}
	set_visible(!is_visible());
	if (Viewport *own_vp = Node::get_viewport()) {
		own_vp->set_input_as_handled();
	}
}

// ---------------------------------------------------------------- target ----

Viewport *GaussianSplatPerformanceOverlay::_get_display_viewport() const {
	// The viewport this layer draws into: CanvasLayer's custom_viewport when one
	// is in effect (split screen), else the node's own. CanvasLayer attaches its
	// canvas to exactly that viewport's RID on ENTER_TREE, so comparing RIDs
	// tells the two apart without touching a custom pointer that was already
	// freed when the layer entered the tree (CanvasLayer then fell back itself).
	Viewport *own_vp = Node::get_viewport();
	if (!own_vp || !is_inside_tree() || CanvasLayer::get_viewport() == own_vp->get_viewport_rid()) {
		return own_vp;
	}
	Viewport *custom_vp = Object::cast_to<Viewport>(get_custom_viewport());
	return custom_vp ? custom_vp : own_vp;
}

// One process-wide dispatcher: it outlives every overlay, so a read that timed
// out can still complete (into a Dictionary nobody reads any more) safely.
class GsOverlayStatsDispatcher : public RenderThreadDispatcher {
public:
	// Issues an id the way dispatch_call_on_render_thread_blocking() does, so a
	// test can invoke the read callable as the dispatcher would: since #1133,
	// notify_completed() refuses ids that were never issued.
	uint64_t issue_request_id() { return next_request_id.fetch_add(1, std::memory_order_acq_rel); }
};

static GsOverlayStatsDispatcher &gs_overlay_stats_dispatcher() {
	static GsOverlayStatsDispatcher dispatcher;
	return dispatcher;
}

// Set once a read was dispatched but never completed: later reads show n/a
// instead of stalling the main thread for the dispatcher timeout every refresh.
static std::atomic<bool> gs_overlay_stats_read_failed{ false };

// Everything the overlay reads from renderer-owned state, in one call:
// get_render_stats() and the custom monitors, whose callbacks read the same
// renderer state (#1030). Packs the results into r_out.
static void gs_overlay_read_renderer_side_into(const Ref<GaussianSplatRenderer> &p_renderer, uint32_t p_sections, bool p_want_stats, Dictionary &r_out) {
	if (p_want_stats && p_renderer.is_valid()) {
		r_out["render_stats"] = p_renderer->get_render_stats();
	}
	GaussianSplatPerformanceOverlay::ReportInputs monitors_in;
	monitors_in.sections = p_sections;
	GaussianSplatPerformanceOverlay::read_monitors(monitors_in, p_renderer.ptr());
	r_out["monitors"] = monitors_in.monitors;
	r_out["tile_monitors_match"] = monitors_in.tile_monitors_match;
	r_out["streaming_monitors_match"] = monitors_in.streaming_monitors_match;
	r_out["monitor_mismatch"] = monitors_in.monitor_mismatch;
}

// p_request_id comes FIRST. The dispatcher wraps the callable it is given in
// .bind(request_id) (render_thread_dispatcher.cpp), and a bound callable puts
// its call arguments before its own binds (CallableCustomBind::call), so the
// callee sees (request_id, <binds made here>). The renderer is passed as a
// Variant so that "no target renderer" (null) binds and converts cleanly.
static void gs_overlay_collect_renderer_side(uint64_t p_request_id, const Variant &p_renderer, int64_t p_sections, bool p_want_stats, Dictionary p_out) {
	const Ref<GaussianSplatRenderer> renderer(Object::cast_to<GaussianSplatRenderer>(p_renderer.get_validated_object()));
	gs_overlay_read_renderer_side_into(renderer, uint32_t(p_sections), p_want_stats, p_out);
	gs_overlay_stats_dispatcher().notify_completed(p_request_id);
}

Callable GaussianSplatPerformanceOverlay::make_renderer_read_callable(const Ref<GaussianSplatRenderer> &p_renderer, uint32_t p_sections, bool p_want_stats, const Dictionary &p_out) {
	return callable_mp_static(&gs_overlay_collect_renderer_side).bind(Variant(p_renderer), int64_t(p_sections), p_want_stats, p_out);
}

uint64_t GaussianSplatPerformanceOverlay::get_render_stats_reads_completed() {
	return gs_overlay_stats_dispatcher().get_completed_request_id();
}

uint64_t GaussianSplatPerformanceOverlay::issue_render_stats_request_id() {
	return gs_overlay_stats_dispatcher().issue_request_id();
}

void GaussianSplatPerformanceOverlay::_read_renderer_side(ReportInputs &r_in, const Ref<GaussianSplatRenderer> &p_renderer, bool p_want_stats) {
	// #1030: get_render_stats() reads -- and on a dirty flag rebuilds -- state the
	// render thread writes every frame (build_render_stats(),
	// finalize_frame_metrics()), and the custom monitor callbacks read the same
	// state. With a separate render thread, reading them from here races; a low
	// rate only makes that rarer. So both run ON the render thread, between
	// frames (the command queue is FIFO), while this thread waits, so no
	// main-thread writer races them either. Single-threaded, or with the render
	// loop stopped, nothing writes concurrently and they run inline.
	Dictionary out; // shared with the bound copy, filled on the render thread
	RenderThreadDispatcher &dispatcher = gs_overlay_stats_dispatcher();
	if (!dispatcher.is_render_thread_dispatch_path_active()) {
		gs_overlay_read_renderer_side_into(p_renderer, r_in.sections, p_want_stats, out);
	} else if (gs_overlay_stats_read_failed.load(std::memory_order_acquire)) {
		r_in.monitor_mismatch = "renderer statistics unavailable: an earlier render-thread read did not complete";
		return;
	} else {
		bool dispatched = false;
		if (!dispatcher.dispatch_call_on_render_thread_blocking(make_renderer_read_callable(p_renderer, r_in.sections, p_want_stats, out),
					&dispatched, true, nullptr, "[GaussianSplatPerformanceOverlay] render-stats read")) {
			if (dispatched && !gs_overlay_stats_read_failed.exchange(true, std::memory_order_acq_rel)) {
				ERR_PRINT("[GaussianSplatPerformanceOverlay] A render-thread statistics read did not complete before the dispatcher timeout. "
						  "Renderer statistics rows show n/a for the rest of this run; the overlay will not read them unsynchronized (#1030).");
			}
			// Timed out or not dispatched: show n/a rather than read unsynchronized.
			r_in.monitor_mismatch = "renderer statistics unavailable: the render-thread read did not complete";
			return;
		}
	}
	r_in.render_stats = out.get("render_stats", Dictionary());
	r_in.monitors = out.get("monitors", Dictionary());
	r_in.tile_monitors_match = out.get("tile_monitors_match", false);
	r_in.streaming_monitors_match = out.get("streaming_monitors_match", false);
	r_in.monitor_mismatch = out.get("monitor_mismatch", String());
}

Node *GaussianSplatPerformanceOverlay::_discover_target() const {
	SceneTree *tree = get_tree();
	if (!tree || !tree->get_root()) {
		return nullptr;
	}
	Viewport *display_vp = _get_display_viewport();
	const Ref<World3D> world = display_vp ? display_vp->find_world_3d() : Ref<World3D>();
	const GaussianSplattingPerformanceMonitors *monitors = GaussianSplattingPerformanceMonitors::get_singleton();
	const GaussianSplatRenderer *preferred = monitors ? monitors->get_monitor_source_info().splat_renderer : nullptr;

	// Pre-order walk in tree order. Runs only while there is no target, at most
	// once per refresh window, so it adds no per-node cost to normal frames.
	Node *first = nullptr;
	LocalVector<Node *> stack;
	stack.push_back(tree->get_root());
	while (!stack.is_empty()) {
		Node *n = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		Ref<GaussianSplatRenderer> renderer;
		Node3D *spatial = nullptr;
		if (GaussianSplatNode3D *splat = Object::cast_to<GaussianSplatNode3D>(n)) {
			renderer = splat->get_existing_renderer();
			spatial = splat;
		} else if (GaussianSplatWorld3D *world_node = Object::cast_to<GaussianSplatWorld3D>(n)) {
			renderer = world_node->get_renderer();
			spatial = world_node;
		}
		// Only candidates rendered by THIS overlay's viewport world: an overlay in
		// a SubViewport describes that SubViewport's splats.
		if (spatial && renderer.is_valid() && spatial->is_inside_tree() &&
				(world.is_null() || spatial->get_world_3d() == world)) {
			if (preferred && renderer.ptr() == preferred) {
				return n;
			}
			if (!first) {
				first = n;
			}
		}
		for (int i = n->get_child_count(false) - 1; i >= 0; i--) {
			stack.push_back(n->get_child(i, false));
		}
	}
	return first;
}

Node *GaussianSplatPerformanceOverlay::_resolve_target(String &r_problem) {
	if (!target_path.is_empty()) {
		Node *n = get_node_or_null(target_path);
		if (!n) {
			r_problem = vformat("target_path %s does not resolve", String(target_path));
			return nullptr;
		}
		if (!Object::cast_to<GaussianSplatNode3D>(n) && !Object::cast_to<GaussianSplatWorld3D>(n)) {
			r_problem = "target_path is not a GaussianSplatNode3D or GaussianSplatWorld3D";
			return nullptr;
		}
		return n;
	}
	Node *current = Object::cast_to<Node>(ObjectDB::get_instance(target_id));
	if (target_explicit) {
		// A set_target() choice is never replaced by discovery: while it is gone
		// the rows are n/a, and it is picked up again if the same node returns.
		if (!current) {
			r_problem = "the node passed to set_target() was freed";
			return nullptr;
		}
		if (!current->is_inside_tree()) {
			r_problem = "the node passed to set_target() is not in the scene tree";
			return nullptr;
		}
		return current;
	}
	if (current && current->is_inside_tree()) {
		// A discovered target is kept only while the display viewport's world
		// still renders it: set_custom_viewport() rewires the layer in place,
		// with no exit/enter notification to react to.
		const Node3D *spatial = Object::cast_to<Node3D>(current);
		Viewport *display_vp = _get_display_viewport();
		const Ref<World3D> world = display_vp ? display_vp->find_world_3d() : Ref<World3D>();
		if (!target_auto || !spatial || world.is_null() || spatial->get_world_3d() == world) {
			return current;
		}
	}
	target_id = ObjectID();
	target_auto = false;
	Node *found = _discover_target();
	if (found) {
		target_id = found->get_instance_id();
		target_auto = true;
	} else {
		r_problem = "no GaussianSplatNode3D or GaussianSplatWorld3D with a renderer in this viewport's world";
	}
	return found;
}

void GaussianSplatPerformanceOverlay::read_monitors(ReportInputs &r_in, const GaussianSplatRenderer *p_target_renderer) {
	Performance *perf = Performance::get_singleton();
	const GaussianSplattingPerformanceMonitors *monitors = GaussianSplattingPerformanceMonitors::get_singleton();
	if (!perf || !monitors) {
		r_in.monitor_mismatch = "custom monitors unavailable";
		return;
	}
	auto read = [&](const char *p_id) {
		const StringName id(p_id);
		if (perf->has_custom_monitor(id)) {
			r_in.monitors[id] = perf->get_custom_monitor(id);
		}
	};

	// Device-wide, so not tied to any one renderer.
	if (r_in.sections & SECTION_DEVICE_VRAM) {
		read("gaussian_splatting/vram_device_total_mb");
		read("gaussian_splatting/vram_device_buffers_mb");
		read("gaussian_splatting/vram_device_textures_mb");
	}

	// The per-renderer monitors follow the most recently registered renderer. They
	// describe this overlay's target only when they unambiguously track it.
	const GaussianSplattingPerformanceMonitors::MonitorSourceInfo info = monitors->get_monitor_source_info();
	if (!p_target_renderer) {
		r_in.monitor_mismatch = "no target renderer";
		return;
	}
	r_in.tile_monitors_match = info.splat_renderer_count == 1 && info.tile_renderer_count == 1 &&
			info.splat_renderer == p_target_renderer;
	r_in.streaming_monitors_match = info.streaming_splat_renderer == p_target_renderer;
	if (!r_in.tile_monitors_match) {
		r_in.monitor_mismatch = info.splat_renderer_count > 1
				? vformat("monitors are ambiguous across %d renderers", info.splat_renderer_count)
				: String("monitors track another renderer");
	}

	if (r_in.tile_monitors_match && (r_in.sections & (SECTION_HOST_STAGES | SECTION_VISIBILITY))) {
		read("gaussian_splatting/cpu_setup_time_ms");
		read("gaussian_splatting/total_processed");
		read("gaussian_splatting/clip_reject_count");
		read("gaussian_splatting/radius_reject_count");
		read("gaussian_splatting/viewport_reject_count");
		read("gaussian_splatting/extreme_aspect_count");
		read("gaussian_splatting/tile_count");
		read("gaussian_splatting/aggregated_count");
		read("gaussian_splatting/overflow_tile_count");
	}

	if (!r_in.streaming_monitors_match || !(r_in.sections & (SECTION_DEVICE_VRAM | SECTION_LOD | SECTION_STREAMING | SECTION_SH_COMPRESSION))) {
		return;
	}
	read("gaussian_splatting/streaming_monitor_ready");
	if (flag(stat(r_in.monitors, "gaussian_splatting/streaming_monitor_ready")) != 1) {
		return;
	}
	if (r_in.sections & SECTION_DEVICE_VRAM) {
		read("gaussian_splatting/vram_current_usage_mb");
		read("gaussian_splatting/vram_budget_mb");
		read("gaussian_splatting/vram_usage_percent");
		read("gaussian_splatting/vram_budget_warning_active");
		read("gaussian_splatting/vram_evicted_this_frame");
		read("gaussian_splatting/vram_thrashing_events");
	}
	if (r_in.sections & SECTION_LOD) {
		read("gaussian_splatting/lod_reduction_ratio_pct");
		read("gaussian_splatting/lod_current_level");
		read("gaussian_splatting/lod_min_chunk_distance");
		read("gaussian_splatting/lod_avg_chunk_distance");
		read("gaussian_splatting/lod_max_chunk_distance");
		read("gaussian_splatting/lod_splat_skip_factor");
		read("gaussian_splatting/lod_opacity_multiplier");
		read("gaussian_splatting/lod_chunks_in_transition");
		read("gaussian_splatting/lod_quality_degradation_active");
	}
	if (r_in.sections & SECTION_STREAMING) {
		read("gaussian_splatting/streaming_visible_chunks");
		read("gaussian_splatting/streaming_loaded_chunks");
		read("gaussian_splatting/streaming_total_chunks");
		read("gaussian_splatting/streaming_resident_chunks");
		read("gaussian_splatting/streaming_chunks_loaded_this_frame");
		read("gaussian_splatting/streaming_chunks_evicted_this_frame");
		read("gaussian_splatting/streaming_buffer_capacity_splats");
		read("gaussian_splatting/streaming_effective_splat_count");
		read("gaussian_splatting/memory_stream_total_bytes_uploaded_mb");
		read("gaussian_splatting/chunk_upload_queue_depth");
		read("gaussian_splatting/streaming_effective_upload_cap_mb_per_frame");
		read("gaussian_splatting/streaming_effective_upload_cap_mb_per_slice");
		read("gaussian_splatting/streaming_effective_upload_cap_mb_per_second");
		read("gaussian_splatting/streaming_effective_vram_budget_mb");
		read("gaussian_splatting/streaming_effective_vram_max_chunks");
		read("gaussian_splatting/streaming_upload_frame_cap_hit");
		read("gaussian_splatting/streaming_upload_bandwidth_cap_hit");
		read("gaussian_splatting/streaming_chunk_load_cap_hit");
		read("gaussian_splatting/streaming_vram_chunk_cap_hit");
		read("gaussian_splatting/streaming_queue_pressure_active");
		read("gaussian_splatting/memory_stream_stall_percent");
	}
	if (r_in.sections & SECTION_SH_COMPRESSION) {
		read("gaussian_splatting/sh_compression_raw_mb");
		read("gaussian_splatting/sh_compression_compressed_mb");
		read("gaussian_splatting/sh_compression_ratio_pct");
	}
}

void GaussianSplatPerformanceOverlay::_gather(ReportInputs &r_in, Node *p_target, const String &p_problem) const {
	r_in.sections = sections;

	// FRAME: the frame clock plus two engine values that are plain field reads
	// on the main thread (no RenderingServer involvement).
	r_in.window = frame_clock.last_window;
	r_in.recent_valid = frame_clock.get_recent(r_in.recent_fps, r_in.recent_slowest_ms, r_in.recent_count);
	r_in.engine_fps = Engine::get_singleton()->get_frames_per_second();
	if (Performance *perf = Performance::get_singleton()) {
		r_in.process_time_s = perf->get_monitor(Performance::TIME_PROCESS);
	}

	if (sections & SECTION_CAMERA) {
		Viewport *display_vp = _get_display_viewport();
		Camera3D *cam = display_vp ? display_vp->get_camera_3d() : nullptr;
		if (cam && cam->is_inside_tree()) {
			r_in.has_camera = true;
			r_in.camera_transform = cam->get_global_transform();
			r_in.camera_projection = int(cam->get_projection());
			r_in.camera_fov = cam->get_fov();
			r_in.camera_size = cam->get_size();
		}
	}

	Ref<GaussianSplatRenderer> renderer;
	if (p_target) {
		r_in.has_target = true;
		r_in.target_name = p_target->get_name();
		if (GaussianSplatNode3D *splat = Object::cast_to<GaussianSplatNode3D>(p_target)) {
			r_in.target_kind = "GaussianSplatNode3D";
			r_in.has_node = true;
			r_in.node_total_splats = splat->get_total_splat_count();
			r_in.node_last_update_ms = splat->get_last_update_time_ms();
			renderer = splat->get_existing_renderer();
		} else if (GaussianSplatWorld3D *world_node = Object::cast_to<GaussianSplatWorld3D>(p_target)) {
			r_in.target_kind = "GaussianSplatWorld3D";
			renderer = world_node->get_renderer();
		}
	} else {
		r_in.target_problem = p_problem;
	}

	if (renderer.is_valid()) {
		r_in.raster_policy = renderer->get_debug_compute_raster_policy();
	}
	// One read of renderer statistics and custom monitors per refresh window
	// (<= 4 Hz), made on the render thread (#1030).
	_read_renderer_side(r_in, renderer,
			renderer.is_valid() && (sections & (SECTION_GPU_PASSES | SECTION_HOST_STAGES | SECTION_VISIBILITY | SECTION_NODE)));

	if (sections & SECTION_MANAGER) {
		if (GaussianSplatManager *manager = GaussianSplatManager::get_singleton()) {
			r_in.has_manager = true;
			r_in.manager_stats = manager->get_global_stats();
		}
	}
}

void GaussianSplatPerformanceOverlay::_refresh() {
	const uint64_t start = OS::get_singleton()->get_ticks_usec();

	String problem;
	Node *target = is_inside_tree() ? _resolve_target(problem) : nullptr;
	if (!is_inside_tree()) {
		problem = "overlay is not in the scene tree";
	}
	ReportInputs in;
	_gather(in, target, problem);
	Vector<String> lines;
	Dictionary snapshot;
	build_report(in, lines, snapshot);

	Vector<String> left;
	Vector<String> right;
	_split_columns(lines, left, right);
	_apply_column(0, left);
	_apply_column(1, right);

	String scene_name;
	if (SceneTree *tree = get_tree()) {
		if (Node *scene = tree->get_current_scene()) {
			scene_name = scene->get_scene_file_path().is_empty() ? String(scene->get_name()) : scene->get_scene_file_path().get_file().get_basename();
		}
	}
	const String base_title = title.is_empty() ? String("Gaussian Splatting") : title;
	const String title_text = scene_name.is_empty() ? base_title : vformat(U"%s · %s", base_title, scene_name);
	if (title_label->get_text() != title_text) {
		title_label->set_text(title_text);
	}

	last_refresh_usec = OS::get_singleton()->get_ticks_usec() - start;
	refresh_count++;
	snapshot["overlay_refresh_usec"] = int64_t(last_refresh_usec);
	last_snapshot = snapshot;

	const String key_hint = hide_key == Key::NONE ? String() : vformat("%s: hide | ", keycode_get_string(hide_key));
	footer_label->set_text(vformat("%soverlay refresh %.2f ms (CPU clock), every %.2f s", key_hint,
			double(last_refresh_usec) / 1000.0, update_interval));
}

// ------------------------------------------------------------ properties ----

void GaussianSplatPerformanceOverlay::set_target_path(const NodePath &p_path) {
	target_path = p_path;
	target_id = ObjectID();
	target_auto = false;
	target_explicit = false;
}

void GaussianSplatPerformanceOverlay::set_target(Node *p_node) {
	target_path = NodePath();
	target_id = p_node ? p_node->get_instance_id() : ObjectID();
	target_auto = false;
	target_explicit = p_node != nullptr;
}

Node *GaussianSplatPerformanceOverlay::get_target() const {
	if (!target_path.is_empty()) {
		return is_inside_tree() ? get_node_or_null(target_path) : nullptr;
	}
	return Object::cast_to<Node>(ObjectDB::get_instance(target_id));
}

void GaussianSplatPerformanceOverlay::set_update_interval(double p_interval) {
	update_interval = CLAMP(p_interval, MIN_UPDATE_INTERVAL, MAX_UPDATE_INTERVAL);
}

void GaussianSplatPerformanceOverlay::set_sections(uint32_t p_sections) {
	sections = p_sections & SECTION_ALL;
}

void GaussianSplatPerformanceOverlay::set_corner(Corner p_corner) {
	corner = CLAMP(p_corner, CORNER_TOP_LEFT, CORNER_BOTTOM_RIGHT);
	_apply_corner();
}

void GaussianSplatPerformanceOverlay::set_title(const String &p_title) {
	title = p_title;
}

void GaussianSplatPerformanceOverlay::set_font_size(int p_size) {
	font_size = CLAMP(p_size, 8, 32);
	_apply_font_size();
}

void GaussianSplatPerformanceOverlay::refresh_now() {
	_refresh();
}
