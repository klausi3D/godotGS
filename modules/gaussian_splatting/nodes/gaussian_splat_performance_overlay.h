/**
 * @file gaussian_splat_performance_overlay.h
 * @brief The one runtime performance overlay GodotGS ships (#1084).
 *
 * GaussianSplatPerformanceOverlay replaces the two GDScript overlays that the
 * starter template and the test project used to carry. Any project can
 * instance it; with no configuration it finds the splat node or world it
 * should describe on its own.
 *
 * Contract for every row (carried over from the template overlay, #833):
 * - a displayed number is a measurement of what its label names, or the row
 *   says `n/a` -- never `0` for "nothing answered";
 * - a GPU pass time is shown only while the renderer's `gpu_*_valid` flag for
 *   that pass is set, and the (sticky) timing age is printed next to them;
 * - a subsystem that is not running says so once, in its section header.
 *
 * Frame pacing follows the #1084 measurement spec (issuecomment-5871767854):
 * wall clock sampled once per main-loop iteration, drawn frames counted, FPS
 * as frames / wall time over a named window, harmonic means only, and no
 * per-frame RenderingServer getter or statistics sweep.
 */

#pragma once

#include "core/math/transform_3d.h"
#include "core/object/object_id.h"
#include "core/os/keyboard.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/main/canvas_layer.h"

class Label;
class PanelContainer;
class RichTextLabel;
class Node;
class GaussianSplatRenderer;

class GaussianSplatPerformanceOverlay : public CanvasLayer {
	GDCLASS(GaussianSplatPerformanceOverlay, CanvasLayer);

public:
	enum Corner {
		CORNER_TOP_LEFT = 0,
		CORNER_TOP_RIGHT = 1,
		CORNER_BOTTOM_LEFT = 2,
		CORNER_BOTTOM_RIGHT = 3,
	};

	enum Section {
		SECTION_FRAME = 1 << 0,
		SECTION_CAMERA = 1 << 1,
		SECTION_GPU_PASSES = 1 << 2,
		SECTION_HOST_STAGES = 1 << 3,
		SECTION_VISIBILITY = 1 << 4,
		SECTION_DEVICE_VRAM = 1 << 5,
		SECTION_LOD = 1 << 6,
		SECTION_STREAMING = 1 << 7,
		SECTION_SH_COMPRESSION = 1 << 8,
		SECTION_NODE = 1 << 9,
		SECTION_MANAGER = 1 << 10,
		SECTION_ALL = (1 << 11) - 1,
		SECTION_DEFAULT = SECTION_ALL & ~SECTION_SH_COMPRESSION,
	};

	/// Statistics are polled once per refresh window, and a window is never
	/// shorter than this: 4 Hz is the cadence at which #1030's render-thread
	/// race on get_render_stats() was not observed, and D5 of #1084 caps it.
	static constexpr double MIN_UPDATE_INTERVAL = 0.25;
	static constexpr double MAX_UPDATE_INTERVAL = 2.0;

	/**
	 * Wall-clock frame pacing, per the #1084 spec. Pure bookkeeping, O(1) per
	 * tick, no engine calls: the caller passes the clock and the drawn-frame
	 * counter, which is what makes it unit-testable.
	 */
	struct FrameClock {
		static constexpr int RING_CAPACITY = 240;

		struct Window {
			bool valid = false; // false until the first window completes (spec §7)
			double seconds = 0.0; // wall time the window actually covered
			uint64_t frames = 0; // drawn frames in the window
			double fps = 0.0; // frames / seconds (spec §3)
			double mean_interval_ms = 0.0; // seconds / frames (spec §5)
			double max_interval_ms = 0.0; // largest drawn-frame interval (spec §5)
		};

		bool started = false;
		uint64_t window_start_us = 0;
		uint64_t window_start_frames = 0;
		uint64_t window_max_interval_us = 0;
		uint64_t last_draw_us = 0;
		uint64_t last_frames_drawn = 0;
		Window last_window;

		uint32_t ring[RING_CAPACITY] = {};
		int ring_count = 0;
		int ring_head = 0;

		void reset();
		/// One main-loop iteration. Returns true when this tick completed a window.
		bool tick(uint64_t p_now_us, uint64_t p_frames_drawn, uint64_t p_window_us);
		/// Harmonic FPS over the recorded drawn-frame intervals (N / sum, spec §4).
		bool get_recent(double &r_fps, double &r_slowest_ms, int &r_count) const;
	};

	/// Everything one refresh reads, gathered first so the report is a pure
	/// function of it (tests build these by hand).
	struct ReportInputs {
		uint32_t sections = SECTION_DEFAULT;

		// FRAME
		FrameClock::Window window;
		bool recent_valid = false;
		double recent_fps = 0.0;
		double recent_slowest_ms = 0.0;
		int recent_count = 0;
		double engine_fps = 0.0; // Engine::get_frames_per_second(); 0 = first second
		double process_time_s = 0.0; // Performance TIME_PROCESS; 0 = first second

		// CAMERA
		bool has_camera = false;
		Transform3D camera_transform;
		bool camera_orthogonal = false;
		double camera_fov = 0.0;
		double camera_size = 0.0;

		// Target
		bool has_target = false;
		String target_name;
		String target_kind; // "GaussianSplatNode3D" / "GaussianSplatWorld3D"
		String target_problem; // why there is no target, when there is none
		Dictionary render_stats; // the target renderer's get_render_stats()

		// NODE (GaussianSplatNode3D targets only)
		bool has_node = false;
		int64_t node_total_splats = 0;
		double node_last_update_ms = 0.0;
		int raster_policy = -1; // -1 = no renderer to ask

		// Custom monitors, keyed by full id; an absent key is "not read / not registered".
		Dictionary monitors;
		bool tile_monitors_match = false;
		bool streaming_monitors_match = false;
		String monitor_mismatch;

		// MANAGER
		bool has_manager = false;
		Dictionary manager_stats;
	};

	static void build_report(const ReportInputs &p_in, Vector<String> &r_lines, Dictionary &r_snapshot);

private:
	NodePath target_path;
	double update_interval = MIN_UPDATE_INTERVAL;
	uint32_t sections = SECTION_DEFAULT;
	Corner corner = CORNER_TOP_LEFT;
	Key hide_key = Key::F3;
	String title;
	int font_size = 13;

	ObjectID target_id;
	FrameClock frame_clock;
	Dictionary last_snapshot;
	uint64_t last_refresh_usec = 0;
	uint64_t refresh_count = 0;

	PanelContainer *panel = nullptr;
	Label *title_label = nullptr;
	RichTextLabel *body_left = nullptr;
	RichTextLabel *body_right = nullptr;
	Label *footer_label = nullptr;

	void _build_ui();
	void _apply_corner();
	void _apply_font_size();
	Node *_resolve_target(String &r_problem);
	Node *_discover_target() const;
	void _gather(ReportInputs &r_in, Node *p_target, const String &p_problem) const;
	void _read_monitors(ReportInputs &r_in, const GaussianSplatRenderer *p_target_renderer) const;
	void _refresh();
	static void _split_columns(const Vector<String> &p_lines, String &r_left, String &r_right);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void unhandled_key_input(const Ref<InputEvent> &p_event) override;

public:
	GaussianSplatPerformanceOverlay();

	void set_target_path(const NodePath &p_path);
	NodePath get_target_path() const { return target_path; }
	void set_target(Node *p_node);
	Node *get_target() const;

	void set_update_interval(double p_interval);
	double get_update_interval() const { return update_interval; }
	void set_sections(uint32_t p_sections);
	uint32_t get_sections() const { return sections; }
	void set_corner(Corner p_corner);
	Corner get_corner() const { return corner; }
	void set_hide_key(Key p_key) { hide_key = p_key; }
	Key get_hide_key() const { return hide_key; }
	void set_title(const String &p_title);
	String get_title() const { return title; }
	void set_font_size(int p_size);
	int get_font_size() const { return font_size; }

	void refresh_now();
	Dictionary get_snapshot() const { return last_snapshot.duplicate(true); }
	int64_t get_last_refresh_usec() const { return static_cast<int64_t>(last_refresh_usec); }
	int64_t get_refresh_count() const { return static_cast<int64_t>(refresh_count); }
};

VARIANT_ENUM_CAST(GaussianSplatPerformanceOverlay::Corner);
VARIANT_BITFIELD_CAST(GaussianSplatPerformanceOverlay::Section);
