#ifndef GS_GPU_PASS_TIMING_H
#define GS_GPU_PASS_TIMING_H

#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"

#include "gaussian_gpu_layout.h"

#include <atomic>
#include <cstdint>

// BV-04b (ADR adr-beyond-vram-worlds.md D-4 / D-5): GPU timing completeness.
//
// Every GPU pass of a Gaussian frame is bracketed by a pair of RenderingDevice timestamps named
// "<prefix><frame serial>_Begin" / "_End". The tile renderer parses the captured names back into a
// per-frame-serial table and publishes one duration per pass. Before BV-04b only the tile passes had
// timestamps; cull, projection (the depth-key pass), the depth pre-sort and the composite were
// untimed, so the fixed cost F of the frame model could not be measured.
//
// This header holds the label vocabulary, the frame-serial source shared by every pass of one frame,
// and the pure parse / reduce functions (device free, so they are unit tested on the CPU).
namespace GaussianSplatting {

// BV-04b: records per splat per HLOD level, from the most recently SAMPLED HLOD-route frame
// (always-on async readback of HlodLevelRecordStatsGPU; frames between samples are not counted).
// Slot i < GS_HLOD_LEVEL_STAT_DEEPEST_SLOT = level i; GS_HLOD_LEVEL_STAT_DEEPEST_SLOT = that level
// and above; GS_HLOD_LEVEL_STAT_OBJECT_SLOT = object (instance) records. frame_serial 0 = no sample.
struct HlodLevelRecordSample {
	uint64_t frame_serial = 0;
	uint64_t sample_count = 0;
	uint32_t records[GS_HLOD_LEVEL_STAT_SLOTS] = {};
	uint32_t splats[GS_HLOD_LEVEL_STAT_SLOTS] = {};
};

namespace GpuPassTiming {

// The order is the order of execution within a frame. PASS_TOTAL is the legacy GaussianSplat_ marker.
enum Pass : uint32_t {
	PASS_CULL = 0, // frustum / instance-chunk cull (gpu_culler)
	PASS_PROJECTION, // depth-key projection: instance chunk dispatch + depth compute + count clamp
	PASS_PRESORT, // depth pre-sort of the visible splats (GPUSortingPipeline's sorter)
	PASS_OVERLAP_COUNT, // tile_binning COUNT pass
	PASS_PREFIX, // tile prefix scan
	PASS_BINNING, // tile_binning EMIT pass (the "overlap emit" pass)
	PASS_OVERLAP_SORT, // overlap-record radix sort
	PASS_RASTER, // tile raster (compute or fragment)
	PASS_RESOLVE, // tile resolve
	PASS_COMPOSITE, // output composite into the engine frame (compute path)
	PASS_TOTAL, // GaussianSplat_ frame bracket (flush-discarded on most frames; not summed)
	PASS_COUNT,
};

// Passes that make up one frame's pipeline (everything except PASS_TOTAL).
static constexpr uint32_t PIPELINE_PASS_COUNT = PASS_TOTAL;
static constexpr uint32_t ALL_PIPELINE_PASSES_MASK = (1u << PIPELINE_PASS_COUNT) - 1u;
// The four passes BV-04b adds.
static constexpr uint32_t BV04B_PASSES_MASK = (1u << PASS_CULL) | (1u << PASS_PROJECTION) | (1u << PASS_PRESORT) |
		(1u << PASS_COMPOSITE);

inline const char *label_prefix(Pass p_pass) {
	switch (p_pass) {
		case PASS_CULL:
			return "GSCull_";
		case PASS_PROJECTION:
			return "GSProjection_";
		case PASS_PRESORT:
			return "GSPreSort_";
		case PASS_OVERLAP_COUNT:
			return "TileOverlapCount_";
		case PASS_PREFIX:
			return "TilePrefix_";
		case PASS_BINNING:
			return "TileBinning_";
		case PASS_OVERLAP_SORT:
			return "TileOverlapSort_";
		case PASS_RASTER:
			return "TileRaster_";
		case PASS_RESOLVE:
			return "TileResolve_";
		case PASS_COMPOSITE:
			return "GSComposite_";
		case PASS_TOTAL:
			return "GaussianSplat_";
		default:
			return "";
	}
}

// Name used in telemetry keys (gpu_<name>_time_ms, ...).
inline const char *pass_name(Pass p_pass) {
	switch (p_pass) {
		case PASS_CULL:
			return "cull";
		case PASS_PROJECTION:
			return "projection";
		case PASS_PRESORT:
			return "presort";
		case PASS_OVERLAP_COUNT:
			return "overlap_count";
		case PASS_PREFIX:
			return "prefix";
		case PASS_BINNING:
			return "binning";
		case PASS_OVERLAP_SORT:
			return "overlap_sort";
		case PASS_RASTER:
			return "raster";
		case PASS_RESOLVE:
			return "resolve";
		case PASS_COMPOSITE:
			return "composite";
		case PASS_TOTAL:
			return "total";
		default:
			return "";
	}
}

// ---------------------------------------------------------------------------------------------
// Frame serial. One serial names every pass of one pipeline pass (cull .. composite), so the parser
// can tell which durations were measured in the same frame. RenderPipelineStages::prepare_frame_context
// allocates it; the tile renderer takes it through RenderParams::frame_serial. Render thread only;
// atomic so a stray caller on another thread cannot tear it.
inline std::atomic<uint64_t> &_frame_serial_counter() {
	static std::atomic<uint64_t> counter{ 0 };
	return counter;
}
inline std::atomic<uint64_t> &_current_frame_serial() {
	static std::atomic<uint64_t> current{ 0 };
	return current;
}

// Allocates a fresh serial (never 0) and makes it the current one.
inline uint64_t allocate_frame_serial() {
	const uint64_t serial = _frame_serial_counter().fetch_add(1, std::memory_order_relaxed) + 1;
	_current_frame_serial().store(serial, std::memory_order_relaxed);
	return serial;
}

// The serial of the pipeline pass being recorded (0 = none allocated yet).
inline uint64_t current_frame_serial() {
	return _current_frame_serial().load(std::memory_order_relaxed);
}

// A caller-supplied serial (e.g. the painterly path's frame counter) was used: never hand it out again.
inline void note_external_frame_serial(uint64_t p_serial) {
	uint64_t seen = _frame_serial_counter().load(std::memory_order_relaxed);
	while (seen < p_serial && !_frame_serial_counter().compare_exchange_weak(seen, p_serial, std::memory_order_relaxed)) {
	}
	_current_frame_serial().store(p_serial, std::memory_order_relaxed);
}

inline String make_label(Pass p_pass, uint64_t p_serial) {
	return String(label_prefix(p_pass)) + String::num_uint64(p_serial);
}

// Brackets one pass with "<prefix><serial>_Begin" ... "_End" on p_device. The _End marker is written
// only by finish(), after the pass's last compute list has ended; a pass that bails out early leaves an
// unpaired _Begin, which the parser treats as "not measured" (never as a 0 ms sample).
// RenderingDevice refuses capture_timestamp inside a compute list with dispatches, so construct this
// before compute_list_begin() and call finish() after compute_list_end().
class ScopedPassTimestamp {
	RenderingDevice *device = nullptr;
	String label;

public:
	ScopedPassTimestamp(RenderingDevice *p_device, Pass p_pass, bool p_enabled, uint64_t p_serial = current_frame_serial()) {
		if (!p_device || !p_enabled || p_serial == 0) {
			return;
		}
		device = p_device;
		label = make_label(p_pass, p_serial);
		device->capture_timestamp(label + String("_Begin"));
	}
	void finish() {
		if (device) {
			device->capture_timestamp(label + String("_End"));
			device = nullptr;
		}
	}
	bool is_armed() const { return device != nullptr; }

	ScopedPassTimestamp(const ScopedPassTimestamp &) = delete;
	ScopedPassTimestamp &operator=(const ScopedPassTimestamp &) = delete;
};

// ---------------------------------------------------------------------------------------------
// Parsing and reduction (pure).

struct StageTimes {
	double begin_ns = -1.0;
	double end_ns = -1.0;
	bool is_complete() const { return begin_ns >= 0.0 && end_ns >= 0.0; }
	// A pass is measured only when both markers exist and are ordered.
	bool is_ordered() const { return is_complete() && end_ns >= begin_ns; }
};

struct FrameStages {
	StageTimes stages[PASS_COUNT];
};

// Parses "<prefix><serial>_Begin|_End". Returns false for any name that is not a pass marker.
inline bool parse_label(const String &p_name, Pass &r_pass, bool &r_is_begin, uint64_t &r_serial) {
	bool is_begin = false;
	int64_t suffix_pos = -1;
	if (p_name.ends_with("_Begin")) {
		is_begin = true;
		suffix_pos = p_name.length() - 6;
	} else if (p_name.ends_with("_End")) {
		suffix_pos = p_name.length() - 4;
	} else {
		return false;
	}
	for (uint32_t i = 0; i < PASS_COUNT; i++) {
		const Pass pass = Pass(i);
		const String prefix = label_prefix(pass);
		if (!p_name.begins_with(prefix)) {
			continue;
		}
		const int64_t prefix_length = prefix.length();
		if (suffix_pos <= prefix_length) {
			return false;
		}
		const String serial_str = p_name.substr(prefix_length, suffix_pos - prefix_length);
		if (!serial_str.is_valid_int()) {
			return false;
		}
		r_pass = pass;
		r_is_begin = is_begin;
		r_serial = uint64_t(serial_str.to_int());
		return true;
	}
	return false;
}

inline void accumulate(HashMap<uint64_t, FrameStages> &r_frames, const String &p_name, double p_gpu_ns) {
	Pass pass = PASS_COUNT;
	bool is_begin = false;
	uint64_t serial = 0;
	if (!parse_label(p_name, pass, is_begin, serial)) {
		return;
	}
	FrameStages *entry = r_frames.getptr(serial);
	if (!entry) {
		r_frames.insert(serial, FrameStages());
		entry = r_frames.getptr(serial);
	}
	StageTimes &stage = entry->stages[pass];
	if (is_begin) {
		stage.begin_ns = p_gpu_ns;
	} else {
		stage.end_ns = p_gpu_ns;
	}
}

// The newest frame serial that has at least one measured pass, with every pass measured in THAT frame.
// Serial 0 is never a frame (no allocated serial is 0); markers labelled 0 come from a caller that had
// no serial and are ignored instead of being read as some frame's timing.
struct FrameDurations {
	bool has_data = false;
	uint64_t serial = 0;
	double ms[PASS_COUNT] = {};
	bool valid[PASS_COUNT] = {};

	uint32_t pipeline_mask() const {
		uint32_t mask = 0;
		for (uint32_t i = 0; i < PIPELINE_PASS_COUNT; i++) {
			if (valid[i]) {
				mask |= 1u << i;
			}
		}
		return mask;
	}
	double pipeline_sum_ms() const {
		double sum = 0.0;
		for (uint32_t i = 0; i < PIPELINE_PASS_COUNT; i++) {
			if (valid[i]) {
				sum += ms[i];
			}
		}
		return sum;
	}
};

// Passes the tile renderer itself brackets (plus its GaussianSplat_ frame marker): the legacy
// gpu_tile_* fields keep selecting their frame among these only, exactly as before BV-04b.
static constexpr uint32_t TILE_PASSES_MASK = (1u << PASS_OVERLAP_COUNT) | (1u << PASS_PREFIX) | (1u << PASS_BINNING) |
		(1u << PASS_OVERLAP_SORT) | (1u << PASS_RASTER) | (1u << PASS_RESOLVE) | (1u << PASS_TOTAL);
static constexpr uint32_t ANY_PASS_MASK = (1u << PASS_COUNT) - 1u;

// p_select_mask: a frame qualifies only if at least one pass in the mask was measured in it.
inline FrameDurations latest_frame(const HashMap<uint64_t, FrameStages> &p_frames, uint32_t p_select_mask = ANY_PASS_MASK) {
	FrameDurations out;
	for (const KeyValue<uint64_t, FrameStages> &kv : p_frames) {
		if (kv.key == 0) {
			continue;
		}
		bool any = false;
		for (uint32_t i = 0; i < PASS_COUNT; i++) {
			any = any || (((p_select_mask >> i) & 1u) != 0u && kv.value.stages[i].is_ordered());
		}
		if (!any || (out.has_data && kv.key <= out.serial)) {
			continue;
		}
		out = FrameDurations();
		out.has_data = true;
		out.serial = kv.key;
		for (uint32_t i = 0; i < PASS_COUNT; i++) {
			const StageTimes &stage = kv.value.stages[i];
			out.valid[i] = stage.is_ordered();
			out.ms[i] = out.valid[i] ? (stage.end_ns - stage.begin_ns) / 1e6 : 0.0;
		}
	}
	return out;
}

// ---------------------------------------------------------------------------------------------
// Published per-pass state.
//
// Per-pass values are "sticky": the mid-frame flushes of RenderingDevice discard some markers on most
// frames, so a pass keeps its last measured value until it has not been measured for STALE_RESOLVES
// resolves (then it reads invalid and 0, never a frozen value). Because sticky values of different
// passes can come from different frames, every pass also publishes the frame serial it was measured
// in, and the snapshot carries the newest COHERENT frame: the set of passes measured in one and the
// same frame serial, and their sum. A consumer that needs a same-frame breakdown (the fixed cost F)
// reads the coherent fields, or compares the per-pass serials; it never adds sticky values blindly.
struct PassTimingSnapshot {
	float ms[PIPELINE_PASS_COUNT] = {};
	bool valid[PIPELINE_PASS_COUNT] = {};
	// Frame serial the published value was measured in. 0 while the pass is invalid.
	uint64_t frame_serial[PIPELINE_PASS_COUNT] = {};
	// Newest frame with at least one measured pass. 0 = none (or aged out).
	uint64_t coherent_frame_serial = 0;
	// Bit i set = pass i was measured in coherent_frame_serial.
	uint32_t coherent_pass_mask = 0;
	// Sum of the passes in coherent_pass_mask, all from coherent_frame_serial.
	float coherent_total_ms = 0.0f;
	// Resolves since the coherent frame was last refreshed (0 = refreshed by the latest resolve).
	uint64_t coherent_age_resolves = 0;
	// Monotonic resolve counter (every resolve call, data or not).
	uint64_t resolve_count = 0;

	bool all_bv04b_passes_coherent() const { return (coherent_pass_mask & BV04B_PASSES_MASK) == BV04B_PASSES_MASK; }
};

class PassTimingAccumulator {
public:
	static constexpr uint64_t STALE_RESOLVES = 120; // ~2 s at 60 fps; same bound as the tile passes

private:
	PassTimingSnapshot snapshot;
	uint64_t resolved_at[PIPELINE_PASS_COUNT] = {};
	uint64_t coherent_resolved_at = 0;

public:
	void reset() {
		snapshot = PassTimingSnapshot();
		for (uint32_t i = 0; i < PIPELINE_PASS_COUNT; i++) {
			resolved_at[i] = 0;
		}
		coherent_resolved_at = 0;
	}

	// Called once per resolve, with or without fresh data, so stale values age out on an empty view.
	void ingest(const FrameDurations &p_durations) {
		const uint64_t now = ++snapshot.resolve_count;
		for (uint32_t i = 0; i < PIPELINE_PASS_COUNT; i++) {
			if (p_durations.has_data && p_durations.valid[i]) {
				snapshot.ms[i] = float(p_durations.ms[i]);
				snapshot.valid[i] = true;
				snapshot.frame_serial[i] = p_durations.serial;
				resolved_at[i] = now;
			} else if (snapshot.valid[i] && now - resolved_at[i] > STALE_RESOLVES) {
				snapshot.valid[i] = false;
				snapshot.ms[i] = 0.0f;
				snapshot.frame_serial[i] = 0;
			}
		}
		if (p_durations.has_data && p_durations.pipeline_mask() != 0u) {
			snapshot.coherent_frame_serial = p_durations.serial;
			snapshot.coherent_pass_mask = p_durations.pipeline_mask();
			snapshot.coherent_total_ms = float(p_durations.pipeline_sum_ms());
			coherent_resolved_at = now;
		} else if (snapshot.coherent_pass_mask != 0u && now - coherent_resolved_at > STALE_RESOLVES) {
			snapshot.coherent_frame_serial = 0;
			snapshot.coherent_pass_mask = 0;
			snapshot.coherent_total_ms = 0.0f;
		}
		snapshot.coherent_age_resolves = snapshot.coherent_pass_mask != 0u ? now - coherent_resolved_at : 0u;
	}

	const PassTimingSnapshot &get_snapshot() const { return snapshot; }
};

// Telemetry surface (render stats and streaming_state). Flat keys for the four BV-04b passes, and a
// nested "gpu_pass_timing" dictionary with every pass:
//   passes.<name> = { time_ms, valid, frame_serial }
//   coherent_frame_serial, coherent_passes (names), coherent_pass_mask, coherent_total_ms,
//   coherent_age_resolves, coherent_has_all_bv04b_passes, resolve_count
// A pass that was not measured reads valid = false and time_ms = 0; nothing is inferred from a 0.
inline void append_to_dictionary(Dictionary &r_metrics, const PassTimingSnapshot &p_snapshot) {
	static const Pass flat_passes[] = { PASS_CULL, PASS_PROJECTION, PASS_PRESORT, PASS_COMPOSITE };
	for (const Pass pass : flat_passes) {
		const String base = String("gpu_") + pass_name(pass);
		r_metrics[base + "_time_ms"] = p_snapshot.ms[pass];
		r_metrics[base + "_time_valid"] = p_snapshot.valid[pass];
		r_metrics[base + "_frame_serial"] = static_cast<int64_t>(p_snapshot.frame_serial[pass]);
	}
	Dictionary timing;
	Dictionary passes;
	Array coherent_passes;
	for (uint32_t i = 0; i < PIPELINE_PASS_COUNT; i++) {
		Dictionary entry;
		entry["time_ms"] = p_snapshot.ms[i];
		entry["valid"] = p_snapshot.valid[i];
		entry["frame_serial"] = static_cast<int64_t>(p_snapshot.frame_serial[i]);
		passes[pass_name(Pass(i))] = entry;
		if ((p_snapshot.coherent_pass_mask >> i) & 1u) {
			coherent_passes.push_back(pass_name(Pass(i)));
		}
	}
	timing["passes"] = passes;
	timing["coherent_frame_serial"] = static_cast<int64_t>(p_snapshot.coherent_frame_serial);
	timing["coherent_passes"] = coherent_passes;
	timing["coherent_pass_mask"] = static_cast<int64_t>(p_snapshot.coherent_pass_mask);
	timing["coherent_total_ms"] = p_snapshot.coherent_total_ms;
	timing["coherent_age_resolves"] = static_cast<int64_t>(p_snapshot.coherent_age_resolves);
	timing["coherent_has_all_bv04b_passes"] = p_snapshot.all_bv04b_passes_coherent();
	timing["resolve_count"] = static_cast<int64_t>(p_snapshot.resolve_count);
	r_metrics["gpu_pass_timing"] = timing;
}

} // namespace GpuPassTiming
} // namespace GaussianSplatting

#endif // GS_GPU_PASS_TIMING_H
