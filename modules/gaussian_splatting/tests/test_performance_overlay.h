#pragma once

// #1084: GaussianSplatPerformanceOverlay, the one performance overlay.
//
// Three layers, so that each property is tested where it can actually fail:
//  - FrameClock: the #1084 frame-pacing spec as pure arithmetic (drawn frames
//    over wall time, harmonic means, n/a before the first window);
//  - build_report(): every row is a measurement or null/"n/a", GPU pass rows are
//    gated on their own *_valid flag, monitor rows only when the monitors
//    describe the overlay's target;
//  - the node in a live SceneTree: discovery, the hidden-means-no-reads rule, and
//    (RequiresGPU) that the rows it shows are the target renderer's.

#include "test_macros.h"

#include "../nodes/gaussian_splat_node_3d.h"
#include "../nodes/gaussian_splat_performance_overlay.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../core/gaussian_splat_asset.h"

#include "core/input/input_event.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"

#if defined(TESTS_ENABLED) || defined(TOOLS_ENABLED)

namespace gs_overlay_test {

using Overlay = GaussianSplatPerformanceOverlay;

inline Ref<GaussianSplatAsset> make_overlay_test_asset(float p_x) {
	Ref<GaussianSplatAsset> asset;
	asset.instantiate();
	asset->set_splat_count(1);
	PackedFloat32Array positions;
	positions.push_back(p_x);
	positions.push_back(0.0f);
	positions.push_back(0.0f);
	asset->set_positions(positions);
	PackedFloat32Array scales;
	scales.push_back(1.0f);
	scales.push_back(1.0f);
	scales.push_back(1.0f);
	asset->set_scales(scales);
	PackedFloat32Array rotations;
	rotations.push_back(1.0f);
	rotations.push_back(0.0f);
	rotations.push_back(0.0f);
	rotations.push_back(0.0f);
	asset->set_rotations(rotations);
	PackedFloat32Array sh_dc;
	sh_dc.push_back(1.0f);
	sh_dc.push_back(1.0f);
	sh_dc.push_back(1.0f);
	asset->set_sh_dc_coefficients(sh_dc);
	PackedFloat32Array opacity_logits;
	opacity_logits.push_back(10.0f);
	asset->set_opacity_logits(opacity_logits);
	return asset;
}

inline String joined(const Vector<String> &p_lines) {
	String out;
	for (const String &line : p_lines) {
		out += line + "\n";
	}
	return out;
}

inline Dictionary section(const Dictionary &p_snapshot, const char *p_name) {
	return p_snapshot.get(p_name, Dictionary());
}

inline bool is_null(const Dictionary &p_section, const char *p_key) {
	return p_section.has(p_key) && p_section[p_key].get_type() == Variant::NIL;
}

inline const char *const PASS_TIME_KEYS[] = { "gpu_overlap_count_ms", "gpu_prefix_ms", "gpu_overlap_emit_ms",
	"gpu_overlap_sort_ms", "gpu_raster_ms", "gpu_resolve_ms" };
inline const char *const PASS_VALID_KEYS[] = { "gpu_overlap_count_valid", "gpu_prefix_valid", "gpu_overlap_emit_valid",
	"gpu_overlap_sort_valid", "gpu_raster_valid", "gpu_resolve_valid" };
inline const char *const PASS_SNAPSHOT_KEYS[] = { "overlap_count_ms", "prefix_ms", "overlap_emit_ms", "overlap_sort_ms",
	"raster_ms", "resolve_ms" };

} // namespace gs_overlay_test

// ---------------------------------------------------------------------------
// FrameClock: the #1084 spec
// ---------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] FrameClock reports n/a until the first window completes") {
	gs_overlay_test::Overlay::FrameClock clock;
	const uint64_t window_us = 250000;
	CHECK_FALSE(clock.tick(1000000, 100, window_us)); // first tick only anchors
	CHECK_FALSE(clock.last_window.valid);
	// 10 drawn frames at 16 ms: 160 ms < 250 ms, still measuring.
	for (int i = 1; i <= 10; i++) {
		CHECK_FALSE(clock.tick(1000000 + i * 16000, 100 + i, window_us));
	}
	CHECK_FALSE(clock.last_window.valid);
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] FrameClock FPS is drawn frames over wall time, not 1/one-interval") {
	gs_overlay_test::Overlay::FrameClock clock;
	const uint64_t window_us = 250000;
	uint64_t now = 5000000;
	uint64_t drawn = 7;
	clock.tick(now, drawn, window_us);
	// A steady 20 ms cadence with ONE 100 ms hitch at the very end of the window.
	// 1/(last interval) would read 10 FPS; frames/wall time reads what happened.
	bool completed = false;
	int frames = 0;
	while (!completed) {
		const uint64_t step = (now - 5000000 >= 180000) ? 100000 : 20000;
		now += step;
		drawn += 1;
		frames += 1;
		completed = clock.tick(now, drawn, window_us);
	}
	const double seconds = double(now - 5000000) / 1000000.0;
	CHECK(clock.last_window.valid);
	CHECK(clock.last_window.frames == uint64_t(frames));
	CHECK(clock.last_window.fps == doctest::Approx(double(frames) / seconds));
	CHECK(clock.last_window.fps > 15.0); // not the 10 FPS of the hitch frame
	CHECK(clock.last_window.max_interval_ms == doctest::Approx(100.0));
	CHECK(clock.last_window.mean_interval_ms == doctest::Approx(seconds * 1000.0 / frames));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] FrameClock recent FPS is the harmonic mean, not the mean of 1/dt") {
	gs_overlay_test::Overlay::FrameClock clock;
	clock.tick(0, 0, 10000000);
	clock.tick(10000, 1, 10000000); // 10 ms  -> 1/dt = 100
	clock.tick(40000, 2, 10000000); // 30 ms  -> 1/dt = 33.3
	double fps = 0.0;
	double slowest = 0.0;
	int count = 0;
	if (!clock.get_recent(fps, slowest, count)) {
		FAIL("two drawn-frame intervals were recorded; get_recent() must report them");
		return;
	}
	CHECK(count == 2);
	// N / sum = 2 / 0.040 s = 50 FPS. The arithmetic mean of 1/dt is 66.7.
	CHECK(fps == doctest::Approx(50.0));
	CHECK(slowest == doctest::Approx(30.0));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] FrameClock counts drawn frames, not loop iterations") {
	gs_overlay_test::Overlay::FrameClock clock;
	const uint64_t window_us = 250000;
	clock.tick(0, 50, window_us);
	// Low-processor mode: 30 iterations in 300 ms, but only every third one drew.
	bool completed = false;
	uint64_t drawn = 50;
	for (int i = 1; i <= 30 && !completed; i++) {
		if (i % 3 == 0) {
			drawn++;
		}
		completed = clock.tick(uint64_t(i) * 10000, drawn, window_us);
	}
	CHECK(completed);
	CHECK(clock.last_window.frames == 8); // iterations 3,6,...,24 drew before 250 ms
	CHECK(clock.last_window.fps == doctest::Approx(8.0 / 0.25));
	CHECK(clock.last_window.max_interval_ms == doctest::Approx(30.0));

	// A window in which nothing was drawn is "not measured", never 0 FPS.
	gs_overlay_test::Overlay::FrameClock idle;
	idle.tick(0, 9, window_us);
	CHECK(idle.tick(300000, 9, window_us));
	CHECK_FALSE(idle.last_window.valid);
}

// ---------------------------------------------------------------------------
// build_report(): the row contract
// ---------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] With nothing measured every row is n/a, never 0") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_ALL;
	in.target_problem = "none found";
	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	const String text = gs_overlay_test::joined(lines);

	CHECK(text.contains("n/a"));
	CHECK_FALSE(text.contains("0.000 ms"));
	CHECK_FALSE(text.contains(" 0.0 FPS"));
	const Dictionary frame = gs_overlay_test::section(snap, "frame");
	CHECK(gs_overlay_test::is_null(frame, "fps"));
	CHECK(gs_overlay_test::is_null(frame, "engine_fps"));
	CHECK(gs_overlay_test::is_null(frame, "process_time_ms"));
	CHECK(gs_overlay_test::is_null(frame, "frame_interval_mean_ms"));
	const Dictionary gpu = gs_overlay_test::section(snap, "gpu_passes");
	for (const char *key : gs_overlay_test::PASS_SNAPSHOT_KEYS) {
		CHECK_MESSAGE(gs_overlay_test::is_null(gpu, key), key);
	}
	CHECK(gs_overlay_test::is_null(gpu, "pass_total_ms"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "device_vram"), "device_total_mb"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "node"), "total_splats"));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] A GPU pass row is shown iff its own validity flag is set") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_GPU_PASSES;
	in.has_target = true;
	in.target_name = "Splat";
	double sum = 0.0;
	for (int i = 0; i < 6; i++) {
		in.render_stats[gs_overlay_test::PASS_TIME_KEYS[i]] = 1.0 + i;
		// Every other pass is valid: a stale value with a cleared flag must not show.
		in.render_stats[gs_overlay_test::PASS_VALID_KEYS[i]] = (i % 2) == 0;
	}
	in.render_stats["gpu_frame_ms"] = 21.0;
	in.render_stats["gpu_frame_valid"] = true;
	in.render_stats["gpu_timing_frames_behind"] = 3;

	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	Dictionary gpu = gs_overlay_test::section(snap, "gpu_passes");
	for (int i = 0; i < 6; i++) {
		const char *key = gs_overlay_test::PASS_SNAPSHOT_KEYS[i];
		if ((i % 2) == 0) {
			CHECK_MESSAGE(double(gpu[key]) == doctest::Approx(1.0 + i), key);
		} else {
			CHECK_MESSAGE(gs_overlay_test::is_null(gpu, key), key);
		}
	}
	// Some pass is n/a, so the rows cannot sum to the total; the text says so.
	CHECK(gs_overlay_test::joined(lines).contains("do not sum"));
	CHECK(int64_t(gpu["timing_frames_behind"]) == 3);

	// All six valid and summing to the total: the identity is stated.
	for (int i = 0; i < 6; i++) {
		in.render_stats[gs_overlay_test::PASS_VALID_KEYS[i]] = true;
		sum += 1.0 + i;
	}
	in.render_stats["gpu_frame_ms"] = sum;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	gpu = gs_overlay_test::section(snap, "gpu_passes");
	CHECK(double(gpu["pass_total_ms"]) == doctest::Approx(sum));
	CHECK(gs_overlay_test::joined(lines).contains("= sum of the six rows"));

	// No pass resolved yet: the zero-initialised frames-behind is not "this frame".
	for (int i = 0; i < 6; i++) {
		in.render_stats[gs_overlay_test::PASS_VALID_KEYS[i]] = false;
	}
	in.render_stats["gpu_frame_valid"] = false;
	in.render_stats["gpu_timing_frames_behind"] = 0;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	gpu = gs_overlay_test::section(snap, "gpu_passes");
	CHECK(gs_overlay_test::is_null(gpu, "timing_frames_behind"));
	CHECK_FALSE(gs_overlay_test::joined(lines).contains("resolved this frame"));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] Frame rows name their window and clock and use no 1/delta") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_FRAME;
	in.window.valid = true;
	in.window.seconds = 0.25;
	in.window.frames = 15;
	in.window.fps = 60.0;
	in.window.mean_interval_ms = 16.67;
	in.window.max_interval_ms = 33.4;
	in.engine_fps = 0.0; // first second
	in.process_time_s = 0.004;
	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	const String text = gs_overlay_test::joined(lines);
	CHECK(text.contains("FPS: 60.0 (drawn frames / wall time, last 250 ms)"));
	CHECK(text.contains("(engine, first second)"));
	CHECK(text.contains("Frame interval (wall clock)"));
	CHECK_FALSE(text.contains("CPU frame"));
	const Dictionary frame = gs_overlay_test::section(snap, "frame");
	CHECK(double(frame["fps"]) == doctest::Approx(60.0));
	CHECK(gs_overlay_test::is_null(frame, "engine_fps"));
	CHECK(double(frame["process_time_ms"]) == doctest::Approx(4.0));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] Monitor rows are n/a when the monitors describe another renderer") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_HOST_STAGES | gs_overlay_test::Overlay::SECTION_VISIBILITY |
			gs_overlay_test::Overlay::SECTION_DEVICE_VRAM;
	in.has_target = true;
	in.render_stats["total_splats"] = 5;
	// The values ARE there -- they belong to some other renderer.
	in.monitors["gaussian_splatting/cpu_setup_time_ms"] = 1.25;
	in.monitors["gaussian_splatting/total_processed"] = 10;
	in.monitors["gaussian_splatting/tile_count"] = 4;
	in.monitors["gaussian_splatting/vram_device_total_mb"] = 0.0; // "nobody answered"
	in.tile_monitors_match = false;
	in.monitor_mismatch = "monitors are ambiguous across 2 renderers";
	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "host_stages"), "tile_renderer_setup_ms"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "visibility"), "tile_count"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "device_vram"), "device_total_mb"));
	CHECK(gs_overlay_test::joined(lines).contains("ambiguous across 2 renderers"));

	// Same values, monitors now tracking the target: shown.
	in.tile_monitors_match = true;
	in.monitor_mismatch = String();
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	CHECK(double(gs_overlay_test::section(snap, "host_stages")["tile_renderer_setup_ms"]) == doctest::Approx(1.25));
	CHECK(int64_t(gs_overlay_test::section(snap, "visibility")["tile_count"]) == 4);
}

// ---------------------------------------------------------------------------
// The node in a SceneTree (no GPU needed)
// ---------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][PerformanceOverlay][SceneTree] With no renderer anywhere the overlay finds no target and shows n/a") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required");
		return;
	}
	Window *root = tree->get_root();
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	root->add_child(node);
	// Precondition, asserted rather than assumed: this headless lane has no
	// RenderingDevice, so the node's enter-tree _ensure_renderer() gets nothing
	// from the scene director. Where a device exists (the GPU harness) the node
	// holds a renderer from enter-tree on, so no renderer-free fixture exists
	// there; this case is not tagged [RequiresGPU] for that reason.
	if (node->get_existing_renderer().is_valid()) {
		FAIL("precondition: the splat node must have no renderer (headless, no RenderingDevice)");
		root->remove_child(node);
		memdelete(node);
		return;
	}

	gs_overlay_test::Overlay *overlay = memnew(gs_overlay_test::Overlay);
	root->add_child(overlay);
	overlay->refresh_now();
	const Dictionary snap = overlay->get_snapshot();
	CHECK(overlay->get_refresh_count() == 1);
	CHECK_FALSE(node->get_existing_renderer().is_valid());
	CHECK(overlay->get_target() == nullptr);
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "node"), "total_splats"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "visibility"), "total_splats"));
	const Dictionary gpu = gs_overlay_test::section(snap, "gpu_passes");
	for (const char *key : gs_overlay_test::PASS_SNAPSHOT_KEYS) {
		CHECK_MESSAGE(gs_overlay_test::is_null(gpu, key), key);
	}
	root->remove_child(overlay);
	root->remove_child(node);
	memdelete(overlay);
	memdelete(node);
}

TEST_CASE("[GaussianSplatting][PerformanceOverlay][SceneTree] Hidden, the overlay reads nothing; F3 shows it again") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required");
		return;
	}
	Window *root = tree->get_root();
	gs_overlay_test::Overlay *overlay = memnew(gs_overlay_test::Overlay);
	root->add_child(overlay);
	CHECK(overlay->get_update_interval() == doctest::Approx(0.25));
	overlay->set_update_interval(0.05);
	CHECK_MESSAGE(overlay->get_update_interval() == doctest::Approx(gs_overlay_test::Overlay::MIN_UPDATE_INTERVAL),
			"statistics must not be polled faster than 4 Hz (#1030, D5)");

	overlay->set_visible(false);
	// Two full refresh windows of wall time. The frame clock keeps counting
	// while hidden, but a completed window must not trigger a refresh.
	for (int i = 0; i < 3; i++) {
		tree->process(0.0);
		OS::get_singleton()->delay_usec(300000);
	}
	tree->process(0.0);
	overlay->refresh_now(); // hidden: an explicit refresh reads nothing either
	CHECK(overlay->get_refresh_count() == 0);

	Ref<InputEventKey> key;
	key.instantiate();
	key->set_keycode(Key::F3);
	key->set_pressed(true);
	root->push_input(key);
	CHECK(overlay->is_visible());

	for (int i = 0; i < 2; i++) {
		tree->process(0.0);
		OS::get_singleton()->delay_usec(300000);
	}
	tree->process(0.0);
	CHECK(overlay->get_refresh_count() >= 1);

	root->push_input(key);
	CHECK_FALSE(overlay->is_visible());

	root->remove_child(overlay);
	memdelete(overlay);
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] Each camera projection is reported as itself") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_CAMERA;
	in.has_camera = true;
	in.camera_fov = 70.0;
	in.camera_size = 3.5;
	const struct {
		Camera3D::ProjectionType projection;
		const char *name;
		const char *label;
	} cases[] = {
		{ Camera3D::PROJECTION_PERSPECTIVE, "perspective", "Projection: Persp" },
		{ Camera3D::PROJECTION_ORTHOGONAL, "orthogonal", "Projection: Ortho" },
		{ Camera3D::PROJECTION_FRUSTUM, "frustum", "Projection: Frustum" },
	};
	for (const auto &c : cases) {
		in.camera_projection = int(c.projection);
		Vector<String> lines;
		Dictionary snap;
		gs_overlay_test::Overlay::build_report(in, lines, snap);
		CHECK_MESSAGE(String(gs_overlay_test::section(snap, "camera")["projection"]) == String(c.name), c.name);
		CHECK(double(gs_overlay_test::section(snap, "camera").get("fov_degrees", -1.0)) == doctest::Approx(in.camera_fov));
		CHECK(double(gs_overlay_test::section(snap, "camera").get("size", -1.0)) == doctest::Approx(in.camera_size));
		CHECK_MESSAGE(gs_overlay_test::joined(lines).contains(c.label), c.label);
		// Non-ASCII text is decoded as UTF-8, not Latin-1 ("Â°" mojibake).
		CHECK(gs_overlay_test::joined(lines).contains(String::utf8("FOV: 70.0° |")));
	}
	in.has_camera = false;
	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	CHECK(gs_overlay_test::joined(lines).contains(String::utf8("in this viewport — ")));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] Node names and target paths are shown literally, not parsed as BBCode") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_NODE;
	in.has_target = true;
	in.has_node = true;
	in.target_kind = "GaussianSplatNode3D";
	in.target_name = "[font_size=100]Cloud";
	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	const String text = gs_overlay_test::joined(lines);
	CHECK(text.contains("[lb]font_size=100]Cloud"));
	CHECK_FALSE(text.contains("[font_size=100]"));
	// The snapshot keeps the real name.
	CHECK(String(gs_overlay_test::section(snap, "node").get("name", "")) == "[font_size=100]Cloud");

	in.has_target = false;
	in.target_problem = "target_path ^\"[b]Bold\" does not resolve";
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	CHECK_FALSE(gs_overlay_test::joined(lines).contains("[b]Bold"));
	CHECK(gs_overlay_test::joined(lines).contains("[lb]b]Bold"));
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] The snapshot carries every displayed LOD, streaming and SH value") {
	gs_overlay_test::Overlay::ReportInputs in;
	in.sections = gs_overlay_test::Overlay::SECTION_LOD | gs_overlay_test::Overlay::SECTION_STREAMING |
			gs_overlay_test::Overlay::SECTION_SH_COMPRESSION | gs_overlay_test::Overlay::SECTION_DEVICE_VRAM;
	in.has_target = true;
	const char *const ids[] = { "lod_current_level", "lod_reduction_ratio_pct", "lod_min_chunk_distance",
		"lod_avg_chunk_distance", "lod_max_chunk_distance", "lod_splat_skip_factor", "lod_opacity_multiplier",
		"lod_chunks_in_transition", "lod_quality_degradation_active", "streaming_visible_chunks",
		"streaming_loaded_chunks", "streaming_total_chunks", "streaming_resident_chunks",
		"streaming_chunks_loaded_this_frame", "streaming_chunks_evicted_this_frame",
		"streaming_buffer_capacity_splats", "streaming_effective_splat_count", "memory_stream_total_bytes_uploaded_mb",
		"chunk_upload_queue_depth", "streaming_effective_upload_cap_mb_per_frame",
		"streaming_effective_upload_cap_mb_per_slice", "streaming_effective_upload_cap_mb_per_second",
		"streaming_effective_vram_budget_mb", "streaming_effective_vram_max_chunks", "streaming_upload_frame_cap_hit",
		"streaming_upload_bandwidth_cap_hit", "streaming_chunk_load_cap_hit", "streaming_vram_chunk_cap_hit",
		"streaming_queue_pressure_active", "memory_stream_stall_percent", "sh_compression_raw_mb",
		"sh_compression_compressed_mb", "sh_compression_ratio_pct", "vram_budget_warning_active",
		"vram_evicted_this_frame", "vram_thrashing_events" };
	in.streaming_monitors_match = true;
	in.monitors["gaussian_splatting/streaming_monitor_ready"] = 1;
	for (const char *id : ids) {
		in.monitors[String("gaussian_splatting/") + id] = 1;
	}
	const struct {
		const char *section;
		int rows;
	} expected[] = { { "lod", 9 }, { "streaming", 21 }, { "sh_compression", 3 } };

	Vector<String> lines;
	Dictionary snap;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	for (const auto &e : expected) {
		const Dictionary sec = gs_overlay_test::section(snap, e.section);
		CHECK_MESSAGE(sec.size() == e.rows, e.section);
		for (const Variant &key : sec.keys()) {
			CHECK_MESSAGE(sec.get(key, Variant()).get_type() != Variant::NIL, (String(e.section) + "." + String(key)));
		}
	}
	// DEVICE VRAM's displayed streaming counters are in the snapshot too.
	const char *const vram_keys[] = { "streaming_budget_warning_active", "streaming_evicted_this_frame",
		"streaming_thrashing_events" };
	const Dictionary vram = gs_overlay_test::section(snap, "device_vram");
	for (const char *key : vram_keys) {
		CHECK_MESSAGE(vram.get(key, Variant()).get_type() != Variant::NIL, key);
	}

	// Not ready: the same keys, every one null (n/a), never 0.
	in.streaming_monitors_match = false;
	gs_overlay_test::Overlay::build_report(in, lines, snap);
	for (const auto &e : expected) {
		const Dictionary sec = gs_overlay_test::section(snap, e.section);
		CHECK_MESSAGE(sec.size() == e.rows, e.section);
		for (const Variant &key : sec.keys()) {
			CHECK_MESSAGE(sec.get(key, Variant()).get_type() == Variant::NIL, (String(e.section) + "." + String(key)));
		}
	}
}

TEST_CASE("[GaussianSplatting][Node][PerformanceOverlay] The render-thread stats read completes when invoked as the dispatcher invokes it") {
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate();
	if (!renderer.is_valid()) {
		FAIL("a GaussianSplatRenderer could not be created");
		return;
	}
	const Dictionary direct = renderer->get_render_stats();
	if (direct.is_empty()) {
		FAIL("get_render_stats() returned nothing to compare against");
		return;
	}
	Dictionary out;
	// An id issued as the dispatcher issues it (#1133 refuses never-issued ids).
	uint64_t request_id = gs_overlay_test::Overlay::issue_render_stats_request_id();
	// Exactly what RenderThreadDispatcher::dispatch_call_on_render_thread_blocking
	// and RenderingServerDefault::_call_on_render_thread do with it:
	// call_on_render_thread(p_callable.bind(request_id)), then .call() with no
	// arguments on the render thread.
	gs_overlay_test::Overlay::make_renderer_read_callable(renderer, gs_overlay_test::Overlay::SECTION_ALL, true, out)
			.bind(request_id)
			.call();
	CHECK_MESSAGE(gs_overlay_test::Overlay::get_render_stats_reads_completed() >= request_id,
			"the read never completed its request: the dispatcher would wait out its timeout");
	const Dictionary stats = out.get("render_stats", Dictionary());
	CHECK(stats.has("painterly_enabled"));
	// The custom monitors are read in the same render-thread call (#1030).
	CHECK(out.get("monitors", Variant()).get_type() == Variant::DICTIONARY);
	CHECK(out.has("monitor_mismatch"));

	// No target renderer: the call still completes, and still reads the monitors.
	Dictionary no_target;
	request_id = gs_overlay_test::Overlay::issue_render_stats_request_id();
	gs_overlay_test::Overlay::make_renderer_read_callable(Ref<GaussianSplatRenderer>(), gs_overlay_test::Overlay::SECTION_ALL, false, no_target)
			.bind(request_id)
			.call();
	CHECK(gs_overlay_test::Overlay::get_render_stats_reads_completed() >= request_id);
	CHECK_FALSE(no_target.has("render_stats"));
	CHECK(String(no_target.get("monitor_mismatch", String())).length() > 0);
}

TEST_CASE("[GaussianSplatting][PerformanceOverlay][SceneTree] A set_target() choice is kept while its node is away, never re-discovered") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required");
		return;
	}
	Window *root = tree->get_root();
	GaussianSplatNode3D *chosen = memnew(GaussianSplatNode3D);
	chosen->set_name("ChosenTarget");
	root->add_child(chosen);
	gs_overlay_test::Overlay *overlay = memnew(gs_overlay_test::Overlay);
	root->add_child(overlay);
	overlay->set_target(chosen);
	overlay->refresh_now();
	CHECK(overlay->get_target() == chosen);

	// A node that is neither kind is refused, and the current choice is kept.
	Node *plain = memnew(Node);
	root->add_child(plain);
	ERR_PRINT_OFF;
	overlay->set_target(plain);
	ERR_PRINT_ON;
	overlay->refresh_now();
	CHECK(overlay->get_target() == chosen);
	CHECK(String(gs_overlay_test::section(overlay->get_snapshot(), "node").get("name", "")) == "ChosenTarget");
	root->remove_child(plain);
	memdelete(plain);

	// Away: the rows are n/a, and the explicit choice is not replaced.
	root->remove_child(chosen);
	overlay->refresh_now();
	CHECK(overlay->get_target() == chosen);
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(overlay->get_snapshot(), "node"), "name"));

	// Back: described again without a new set_target().
	root->add_child(chosen);
	overlay->refresh_now();
	CHECK(overlay->get_target() == chosen);
	CHECK(String(gs_overlay_test::section(overlay->get_snapshot(), "node").get("name", "")) == "ChosenTarget");

	// The overlay itself leaving and re-entering the tree keeps the choice too.
	root->remove_child(overlay);
	root->add_child(overlay);
	overlay->refresh_now();
	CHECK(overlay->get_target() == chosen);
	CHECK(String(gs_overlay_test::section(overlay->get_snapshot(), "node").get("name", "")) == "ChosenTarget");

	// null returns to automatic discovery.
	overlay->set_target(nullptr);
	overlay->refresh_now();
	if (!chosen->get_existing_renderer().is_valid()) {
		// No renderer anywhere, so discovery finds nothing -- the old choice is gone.
		CHECK(overlay->get_target() == nullptr);
	}

	root->remove_child(overlay);
	memdelete(overlay);
	root->remove_child(chosen);
	memdelete(chosen);
}

TEST_CASE("[GaussianSplatting][PerformanceOverlay][SceneTree] A custom_viewport overlay reports that viewport's camera") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required");
		return;
	}
	Window *root = tree->get_root();
	// Split screen: the overlay lives under the root but draws into a
	// SubViewport with its own world and its own (frustum) camera. The root
	// viewport has a different camera, so reading the node's viewport shows up.
	Camera3D *root_cam = memnew(Camera3D);
	root->add_child(root_cam);
	root_cam->make_current();
	SubViewport *sub = memnew(SubViewport);
	sub->set_size(Size2i(64, 64));
	sub->set_use_own_world_3d(true);
	root->add_child(sub);
	Camera3D *sub_cam = memnew(Camera3D);
	sub_cam->set_frustum(2.0, Vector2(), 0.05, 100.0);
	sub->add_child(sub_cam);
	sub_cam->set_position(Vector3(1.0, 2.0, 3.0));
	sub_cam->make_current();

	gs_overlay_test::Overlay *overlay = memnew(gs_overlay_test::Overlay);
	overlay->set_custom_viewport(sub);
	root->add_child(overlay);
	overlay->refresh_now();
	const Dictionary cam = gs_overlay_test::section(overlay->get_snapshot(), "camera");
	CHECK(Vector3(cam.get("position", Vector3(-1, -1, -1))).is_equal_approx(Vector3(1.0, 2.0, 3.0)));
	CHECK(String(cam.get("projection", String())) == "frustum");

	root->remove_child(overlay);
	memdelete(overlay);
	sub->remove_child(sub_cam);
	memdelete(sub_cam);
	root->remove_child(sub);
	memdelete(sub);
	root->remove_child(root_cam);
	memdelete(root_cam);
}

// ---------------------------------------------------------------------------
// Live values (GPU)
// ---------------------------------------------------------------------------

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Performance overlay shows its own world's renderer and gates every pass row") {
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		FAIL("SceneTree with a root window required");
		return;
	}
	Window *root = tree->get_root();

	// Target: a splat node in the root viewport's world. Registered FIRST, so it
	// is NOT the monitors' "most recently registered" renderer.
	GaussianSplatNode3D *node = memnew(GaussianSplatNode3D);
	node->set_splat_asset(gs_overlay_test::make_overlay_test_asset(0.0f));
	root->add_child(node);
	tree->process(0.0);
	Ref<GaussianSplatRenderer> renderer = node->get_renderer();

	// Decoy: a splat node in a SubViewport with its OWN World3D, placed BEFORE the
	// target in tree order and registered AFTER it. Tree order alone, or "prefer
	// the monitors' renderer" alone, would both pick the decoy; only the
	// same-world rule rejects it.
	SubViewport *sub = memnew(SubViewport);
	sub->set_size(Size2i(64, 64));
	sub->set_use_own_world_3d(true);
	root->add_child(sub);
	root->move_child(sub, 0);
	GaussianSplatNode3D *decoy = memnew(GaussianSplatNode3D);
	decoy->set_splat_asset(gs_overlay_test::make_overlay_test_asset(5.0f));
	sub->add_child(decoy);
	tree->process(0.0);
	Ref<GaussianSplatRenderer> decoy_renderer = decoy->get_renderer();

	auto cleanup = [&]() {
		sub->remove_child(decoy);
		root->remove_child(sub);
		root->remove_child(node);
		memdelete(decoy);
		memdelete(sub);
		memdelete(node);
	};
	// #595: no environment skip -- a skip is scored as a PASS.
	if (!renderer.is_valid() || !decoy_renderer.is_valid() || renderer == decoy_renderer) {
		FAIL("two distinct renderers required: this case runs only in the [RequiresGPU] harness");
		cleanup();
		return;
	}
	for (int i = 0; i < 5; i++) {
		tree->process(0.0);
	}

	gs_overlay_test::Overlay *overlay = memnew(gs_overlay_test::Overlay);
	overlay->set_sections(gs_overlay_test::Overlay::SECTION_ALL);
	root->add_child(overlay);
	overlay->refresh_now();
	const Dictionary snap = overlay->get_snapshot();
	const Dictionary stats = renderer->get_render_stats();

	CHECK(overlay->get_target() == node);
	const Dictionary node_section = gs_overlay_test::section(snap, "node");
	CHECK(String(node_section.get("name", "")) == String(node->get_name()));
	CHECK(int64_t(node_section.get("total_splats", -1)) == int64_t(node->get_total_splat_count()));

	// Every pass row is the target renderer's value when (and only when) its
	// validity flag is set in the renderer's own statistics. This checks the
	// overlay against whatever this harness's renderer reports, which can be
	// "no pass resolved"; the gate itself (shown iff valid, n/a otherwise) is
	// proven with set flags by the pure case "A GPU pass row is shown iff its
	// own validity flag is set". This case's own claims are target selection,
	// world filtering and monitor ambiguity, which need no resolved timestamps.
	const Dictionary gpu = gs_overlay_test::section(snap, "gpu_passes");
	for (int i = 0; i < 6; i++) {
		const char *key = gs_overlay_test::PASS_SNAPSHOT_KEYS[i];
		const bool valid = bool(stats.get(gs_overlay_test::PASS_VALID_KEYS[i], false));
		if (valid) {
			CHECK_MESSAGE(double(gpu[key]) == doctest::Approx(double(stats[gs_overlay_test::PASS_TIME_KEYS[i]])), key);
		} else {
			CHECK_MESSAGE(gs_overlay_test::is_null(gpu, key), key);
		}
	}
	const String route = stats.get("route_uid", String());
	if (route.is_empty()) {
		CHECK(gs_overlay_test::is_null(gpu, "route_uid"));
	} else {
		CHECK(String(gpu["route_uid"]) == route);
	}

	// Two renderers are registered, so the process-global monitors cannot be
	// said to describe the target: those rows are n/a, not the decoy's numbers.
	const Dictionary vis = gs_overlay_test::section(snap, "visibility");
	CHECK(gs_overlay_test::is_null(vis, "tile_count"));
	CHECK(gs_overlay_test::is_null(gs_overlay_test::section(snap, "host_stages"), "tile_renderer_setup_ms"));

	// Split screen: the same kind of overlay drawn into the SubViewport through
	// CanvasLayer::custom_viewport describes the SubViewport's world, i.e. the
	// decoy, although the overlay node itself sits in the root viewport.
	gs_overlay_test::Overlay *split = memnew(gs_overlay_test::Overlay);
	split->set_custom_viewport(sub);
	root->add_child(split);
	split->refresh_now();
	CHECK(split->get_target() == decoy);
	// Re-pointed at runtime (set_custom_viewport() rewires the layer in place,
	// no exit/enter): the discovered target follows the display viewport.
	split->set_custom_viewport(root);
	split->refresh_now();
	CHECK(split->get_target() == node);
	root->remove_child(split);
	memdelete(split);

	// The overlay leaves with the target: removing the target clears it.
	root->remove_child(overlay);
	memdelete(overlay);
	cleanup();
}

#endif // TESTS_ENABLED || TOOLS_ENABLED
