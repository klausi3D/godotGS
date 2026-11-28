#pragma once

// #1544: the overlap (tile) sort has no total order on base.
//
// The overlap sort key is (tile, depth, tie) and the tie holds only the low GS_SORT_TIE_BITS (16 here)
// bits of global_idx (tile_binning.glsl gs_pack_sort_key). EMIT takes each record's slot from
// atomicAdd, so two records of one tile with equal full keys reach the (stable) radix sort in emit
// order and leave it in that order. Their blend order then follows the GPU's atomic scheduling and can
// change from frame to frame under a static camera.
//
// INST-T style fixture (ADR instanced HLOD, P18): 2 x 65536 splats on one fronto-parallel plane, so
// every view depth is bitwise equal. Splat i and splat i + 65536 sit at the same position with
// different colours. Equal depth sorts the depth pre-sort by its identity tie, so the pair's
// global_idx values differ by exactly 2^16 and their 16-bit tile ties are equal: every pair is a run of
// equal overlap keys. A test-only hook (GSTileOrderTestControl::permute_emit_order) reverses each
// tile's atomicAdd slots on odd frames, so the emit order is permuted by force rather than left to
// chance.
//
// Oracle: frames whose pixels differ from frame 0 must be 0. The preconditions are measured on the
// GPU by the total-order pass (tile_prefix_scan.glsl pass 4) and must be > 0, so the case cannot pass
// vacuously: sort_equal_key_adjacent_pairs (equal full keys next to each other in the sorted array),
// tile_equal_depth_adjacent_pairs (equal tile and depth), and runs_reordered (runs the sort did not
// leave in global_idx order: the applied-proof that the permutation reached the sort).

#include "gs_test_pump.h"
#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/tile_order_contract.h"
#include "../renderer/tile_renderer.h"

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

namespace TestTileOverlapTotalOrder {

static constexpr int PAIRS = 65536; // 2^GS_SORT_TIE_BITS: a pair's global_idx values differ by exactly this
static constexpr int GRID = 256; // 256 x 256 = PAIRS positions
static constexpr int FRAMES = 8;
static const Vector2i RESOLUTION(640, 360);

struct FrameStats {
	uint32_t passes = 0;
	uint32_t runs_resolved = 0;
	uint32_t runs_reordered = 0;
	uint32_t key_overflow = 0;
	uint32_t equal_key_pairs = 0;
	uint32_t equal_depth_pairs = 0;
	uint32_t truncated_tie_equal = 0;
};

struct RunResult {
	bool ok = false;
	String why;
	int frames_differing = 0; // frames != frame 0
	int frames_reordered = 0; // frames whose sort left at least one run out of global_idx order
	FrameStats total; // summed over FRAMES
	uint32_t min_frame_passes = UINT32_MAX;
	uint64_t missing_pass_frames = 0;
};

static Ref<::GaussianData> _make_fixture() {
	Vector<Gaussian> gaussians;
	if (gaussians.resize(2 * PAIRS) != OK) {
		return Ref<::GaussianData>();
	}
	Gaussian *w = gaussians.ptrw();
	for (int i = 0; i < PAIRS; i++) {
		const float x = -9.0f + 18.0f * (float(i % GRID) + 0.5f) / float(GRID);
		const float y = -5.0f + 10.0f * (float(i / GRID) + 0.5f) / float(GRID);
		for (int member = 0; member < 2; member++) {
			Gaussian g = Gaussian{};
			// z is exact, the view is the identity: every splat's view depth is bitwise 10.
			g.position = Vector3(x, y, -10.0f);
			g.scale = Vector3(0.08f, 0.08f, 0.08f);
			g.rotation = Quaternion();
			g.opacity = 0.6f;
			g.sh_dc = member == 0 ? Color(1.2f, -1.2f, -1.2f) : Color(-1.2f, -1.2f, 1.2f);
			w[i + member * PAIRS] = g;
		}
	}
	Ref<::GaussianData> data;
	data.instantiate();
	data->set_gaussians(gaussians);
	return data;
}

static RID _create_readback_texture(RenderingDevice *p_rd) {
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	format.width = RESOLUTION.x;
	format.height = RESOLUTION.y;
	format.depth = 1;
	format.array_layers = 1;
	format.mipmaps = 1;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.usage_bits = RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	return p_rd->texture_create(format, RD::TextureView());
}

static FrameStats _delta(const GaussianSplatting::TileOrderStatsGPU &p_after, const GaussianSplatting::TileOrderStatsGPU &p_before) {
	FrameStats d;
	d.passes = p_after.passes - p_before.passes;
	d.runs_resolved = p_after.runs_resolved - p_before.runs_resolved;
	d.runs_reordered = p_after.runs_reordered - p_before.runs_reordered;
	d.key_overflow = p_after.key_overflow - p_before.key_overflow;
	d.equal_key_pairs = p_after.equal_key_pairs - p_before.equal_key_pairs;
	d.equal_depth_pairs = p_after.equal_depth_pairs - p_before.equal_depth_pairs;
	d.truncated_tie_equal = p_after.truncated_tie_equal - p_before.truncated_tie_equal;
	return d;
}

// Renders FRAMES frames of the fixture under a static camera with the current GSTileOrderTestControl
// settings and compares each frame's pixels with frame 0.
static RunResult _run(RenderingDevice *p_rd, const String &p_label) {
	RunResult result;
	Ref<::GaussianData> data = _make_fixture();
	if (data.is_null()) {
		result.why = "fixture allocation failed";
		return result;
	}
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(p_rd);
	if (!renderer.is_valid()) {
		result.why = "renderer did not instantiate";
		return result;
	}
	renderer->initialize();
	renderer->set_painterly_enabled(false);
	renderer->set_max_splats(2 * PAIRS);
	if (renderer->set_gaussian_data(data) != OK) {
		result.why = "set_gaussian_data failed";
		return result;
	}
	Projection projection;
	projection.set_perspective(60.0f, float(RESOLUTION.x) / float(RESOLUTION.y), 0.1f, 100.0f);
	const Transform3D view; // identity: the camera sits at the origin and looks down -Z
	const TestGaussianSplatting::GSPumpOutcome outcome = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(view, projection, RID(), RESOLUTION);
	}, 20000000, 2);
	if (!outcome.ready()) {
		result.why = "render_for_view never reported a rendered frame: " + outcome.describe();
		return result;
	}
	Ref<TileRenderer> tile = renderer->get_tile_renderer();
	if (tile.is_null()) {
		result.why = "no tile renderer";
		return result;
	}
	RID target = _create_readback_texture(p_rd);
	if (!target.is_valid()) {
		result.why = "readback texture creation failed";
		return result;
	}
	const uint64_t missing_before = tile->get_tile_order_pass_missing_frames();
	Vector<uint8_t> frame0;
	for (int frame = 0; frame < FRAMES; frame++) {
		GaussianSplatting::TileOrderStatsGPU before, after;
		if (!tile->test_read_tile_order_stats(before)) {
			result.why = "tile order stats unreadable";
			break;
		}
		if (!renderer->render_for_view(view, projection, RID(), RESOLUTION)) {
			result.why = vformat("frame %d did not render", frame);
			break;
		}
		if (!renderer->copy_final_texture_to_target(target, RESOLUTION)) {
			result.why = vformat("frame %d: copy_final_texture_to_target failed", frame);
			break;
		}
		const Vector<uint8_t> pixels = p_rd->texture_get_data(target, 0);
		if (pixels.size() != RESOLUTION.x * RESOLUTION.y * 4) {
			result.why = vformat("frame %d: readback returned %d bytes", frame, pixels.size());
			break;
		}
		if (!tile->test_read_tile_order_stats(after)) {
			result.why = "tile order stats unreadable";
			break;
		}
		const FrameStats d = _delta(after, before);
		result.total.passes += d.passes;
		result.total.runs_resolved += d.runs_resolved;
		result.total.runs_reordered += d.runs_reordered;
		result.total.key_overflow += d.key_overflow;
		result.total.equal_key_pairs += d.equal_key_pairs;
		result.total.equal_depth_pairs += d.equal_depth_pairs;
		result.total.truncated_tie_equal += d.truncated_tie_equal;
		result.min_frame_passes = MIN(result.min_frame_passes, d.passes);
		if (d.runs_reordered > 0) {
			result.frames_reordered++;
		}
		if (frame == 0) {
			frame0 = pixels;
		} else if (pixels != frame0) {
			result.frames_differing++;
		}
		MESSAGE(vformat("%s frame %d: passes=%d equal_key_pairs=%d equal_depth_pairs=%d runs_resolved=%d runs_reordered=%d key_overflow=%d truncated_tie_equal=%d differs_from_frame0=%s",
				p_label, frame, d.passes, d.equal_key_pairs, d.equal_depth_pairs, d.runs_resolved, d.runs_reordered,
				d.key_overflow, d.truncated_tie_equal, frame > 0 && pixels != frame0 ? "yes" : "no"));
	}
	result.missing_pass_frames = tile->get_tile_order_pass_missing_frames() - missing_before;
	p_rd->free(target);
	renderer.unref();
	if (result.why.is_empty()) {
		result.ok = true;
	}
	MESSAGE(vformat("%s: frames!=frame0 %d of %d, equal_key_pairs %d, equal_depth_pairs %d, runs_resolved %d, runs_reordered %d (frames with reordered runs %d), key_overflow %d, truncated_tie_equal %d, missing pass frames %d",
			p_label, result.frames_differing, FRAMES - 1, result.total.equal_key_pairs, result.total.equal_depth_pairs,
			result.total.runs_resolved, result.total.runs_reordered, result.frames_reordered, result.total.key_overflow,
			result.total.truncated_tie_equal, int(result.missing_pass_frames)));
	return result;
}

static RenderingDevice *_device() {
	if (RenderingServer::get_singleton() == nullptr) {
		FAIL("RenderingServer unavailable in a [SceneTree][RequiresGPU] case - the harness is required to provide one");
		return nullptr;
	}
	GaussianSplatManager *manager = GaussianSplatManager::get_singleton();
	if (manager == nullptr) {
		FAIL("GaussianSplatManager unavailable - module registration always provides one");
		return nullptr;
	}
	RenderingDevice *rd = manager->get_primary_rendering_device();
	if (rd == nullptr) {
		FAIL("RenderingDevice unavailable in a [RequiresGPU] case");
	}
	return rd;
}

// Preconditions every run must produce, or its verdict says nothing.
static bool _preconditions(const RunResult &p_run, const String &p_label) {
	if (!p_run.ok) {
		FAIL(p_label, ": ", p_run.why);
		return false;
	}
	bool ok = true;
	if (p_run.min_frame_passes == 0u || p_run.min_frame_passes == UINT32_MAX) {
		FAIL(p_label, ": a frame ran no total-order pass (min passes per frame ", p_run.min_frame_passes, ")");
		ok = false;
	}
	if (p_run.total.equal_key_pairs == 0u) {
		FAIL(p_label, ": precondition sort_equal_key_adjacent_pairs > 0 not produced");
		ok = false;
	}
	if (p_run.total.equal_depth_pairs == 0u) {
		FAIL(p_label, ": precondition tile_equal_depth_adjacent_pairs > 0 not produced");
		ok = false;
	}
	CHECK_MESSAGE(p_run.missing_pass_frames == 0u, p_label, ": sorted frames without the total-order pass");
	return ok;
}

} // namespace TestTileOverlapTotalOrder

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Overlap sort: equal-key records blend in one order every frame under a static camera (#1544)") {
	using namespace TestTileOverlapTotalOrder;
	RenderingDevice *rd = _device();
	if (rd == nullptr) {
		return;
	}
	GSTileOrderTestControl::reset();
	GSTileOrderTestControl::permute_emit_order.store(true);
	const RunResult run = _run(rd, "permuted emit order");
	GSTileOrderTestControl::reset();
	if (!_preconditions(run, "permuted emit order")) {
		return;
	}
	// Applied-proof of the forced permutation: on the reversed frames the sort leaves runs out of
	// global_idx order (runs_reordered is counted before the total-order pass reorders them).
	CHECK_MESSAGE(run.frames_reordered > 0, "the emit-order permutation never reached the sort output");
	CHECK_MESSAGE(run.total.key_overflow == 0u, "a run exceeded the total-order pass cap");
	CHECK_MESSAGE(run.frames_differing == 0,
			vformat("%d of %d frames differ from frame 0 under a static camera", run.frames_differing, FRAMES - 1));
}

TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Overlap sort total order: controls flip the static-frame predicate, overflow is counted (#1544)") {
	using namespace TestTileOverlapTotalOrder;
	RenderingDevice *rd = _device();
	if (rd == nullptr) {
		return;
	}
	struct Control {
		const char *name;
		uint32_t mode;
		uint32_t run_cap;
	};
	// (a') total-order pass disabled: the base behaviour (emit-order ties). Applied-proof: runs_resolved == 0.
	// (c) runs ordered by the 16-bit truncated tie only. Applied-proof: truncated_tie_equal > 0.
	// (o) run cap 1: every run refused. Applied-proof: key_overflow > 0 (tile_order_key_overflow).
	const Control controls[] = {
		{ "control (a') pass disabled", GaussianSplatting::TILE_ORDER_TEST_MODE_PASS_DISABLED, 0u },
		{ "control (c) tie truncated to 16 bits", GaussianSplatting::TILE_ORDER_TEST_MODE_TIE_TRUNCATED_16, 0u },
		{ "control (o) run cap 1", GaussianSplatting::TILE_ORDER_TEST_MODE_PRODUCTION, 1u },
	};
	for (const Control &control : controls) {
		GSTileOrderTestControl::reset();
		GSTileOrderTestControl::permute_emit_order.store(true);
		GSTileOrderTestControl::order_test_mode.store(control.mode);
		GSTileOrderTestControl::run_cap_override.store(control.run_cap);
		const RunResult run = _run(rd, control.name);
		GSTileOrderTestControl::reset();
		if (!_preconditions(run, control.name)) {
			continue;
		}
		bool applied = false;
		if (control.mode == GaussianSplatting::TILE_ORDER_TEST_MODE_PASS_DISABLED) {
			applied = run.total.runs_resolved == 0u;
		} else if (control.mode == GaussianSplatting::TILE_ORDER_TEST_MODE_TIE_TRUNCATED_16) {
			applied = run.total.truncated_tie_equal > 0u;
		} else {
			applied = run.total.key_overflow > 0u && run.total.runs_resolved == 0u;
		}
		const char *outcome = !applied ? "MISSING_CONTROL" : (run.frames_differing > 0 ? "DEMONSTRATED_FAILURE" : "INEFFECTIVE_MUTATION");
		MESSAGE(vformat("%s: %s (frames!=frame0 %d)", control.name, outcome, run.frames_differing));
		CHECK_MESSAGE(applied, control.name, ": applied-proof missing");
		CHECK_MESSAGE(run.frames_differing > 0, control.name, ": the mutation did not flip the static-frame predicate");
	}
	// Legal route: production order, no forced permutation.
	GSTileOrderTestControl::reset();
	const RunResult plain = _run(rd, "production, emit order not permuted");
	if (_preconditions(plain, "production, emit order not permuted")) {
		CHECK(plain.total.key_overflow == 0u);
		CHECK(plain.frames_differing == 0);
	}
}

// Codex P1 on #1570: the total-order counters buffer is required. If its allocation fails, the
// renderer must not present frames without the order pass, and the allocation must be retried.
// The hook refuses the allocation while a resize forces the tile renderer to recreate its buffers.
TEST_CASE("[GaussianSplatting][SceneTree][RequiresGPU] Overlap sort total order: a failed counters allocation refuses frames and is retried (#1544)") {
	using namespace TestTileOverlapTotalOrder;
	RenderingDevice *rd = _device();
	if (rd == nullptr) {
		return;
	}
	GSTileOrderTestControl::reset();
	Ref<::GaussianData> data = _make_fixture();
	if (data.is_null()) {
		FAIL("fixture allocation failed");
		return;
	}
	Ref<GaussianSplatRenderer> renderer;
	renderer.instantiate(rd);
	renderer->initialize();
	renderer->set_painterly_enabled(false);
	renderer->set_max_splats(2 * PAIRS);
	if (renderer->set_gaussian_data(data) != OK) {
		FAIL("set_gaussian_data failed");
		return;
	}
	Projection projection;
	projection.set_perspective(60.0f, float(RESOLUTION.x) / float(RESOLUTION.y), 0.1f, 100.0f);
	const Transform3D view;
	const TestGaussianSplatting::GSPumpOutcome first = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(view, projection, RID(), RESOLUTION);
	}, 20000000, 2);
	if (!first.ready()) {
		FAIL("render_for_view never reported a rendered frame: ", first.describe());
		return;
	}
	Ref<TileRenderer> tile = renderer->get_tile_renderer();
	if (tile.is_null() || !tile->test_tile_order_stats_buffer_valid()) {
		FAIL("premise: the counters buffer exists before the forced failure");
		return;
	}
	// A new size makes _ensure_resources() recreate its buffers; the hook refuses this one.
	const Vector2i resized(RESOLUTION.x - 64, RESOLUTION.y - 32);
	const uint32_t refusals_before = GSTileOrderTestControl::refused_stats_allocations.load();
	const uint64_t missing_before = tile->get_tile_order_pass_missing_frames();
	GSTileOrderTestControl::refuse_stats_allocation.store(true);
	int presented_while_refused = 0;
	for (int frame = 0; frame < 4; frame++) {
		if (renderer->render_for_view(view, projection, RID(), resized)) {
			presented_while_refused++;
		}
	}
	GSTileOrderTestControl::refuse_stats_allocation.store(false);
	const uint32_t refusals = GSTileOrderTestControl::refused_stats_allocations.load() - refusals_before;
	const uint64_t missing_while_refused = tile->get_tile_order_pass_missing_frames() - missing_before;
	MESSAGE(vformat("while refused: allocations refused %d, frames presented %d of 4, tile_order_pass_missing_frames +%d, buffer valid %s",
			refusals, presented_while_refused, int(missing_while_refused), tile->test_tile_order_stats_buffer_valid() ? "yes" : "no"));
	if (refusals == 0u) {
		FAIL("applied-proof missing: the hook never refused the allocation (no buffer recreation happened)");
		return;
	}
	// Refused: no frame may be presented without the order pass, and the frames that could not run it are counted.
	CHECK_MESSAGE(presented_while_refused == 0, "frames were presented without the total-order pass");
	CHECK_MESSAGE(missing_while_refused > 0u, "frames that could not run the total-order pass were not counted");
	// Retried: with the hook cleared, the renderer allocates the buffer again and the pass runs.
	GaussianSplatting::TileOrderStatsGPU before, after;
	const TestGaussianSplatting::GSPumpOutcome again = TestGaussianSplatting::gs_pump_until([&]() {
		return renderer->render_for_view(view, projection, RID(), resized) && tile->test_tile_order_stats_buffer_valid();
	}, 20000000, 1);
	GSTileOrderTestControl::reset();
	if (!again.ready()) {
		FAIL("the counters buffer was never allocated again: ", again.describe());
		return;
	}
	// Every step of the recovered-pass proof must run; an unreadable counter is a failure, never a skip.
	if (!tile->test_read_tile_order_stats(before)) {
		FAIL("after the retry the tile order counters are unreadable (baseline read), so the recovered pass cannot be proven");
		return;
	}
	const bool rendered = renderer->render_for_view(view, projection, RID(), resized);
	if (!rendered) {
		FAIL("the frame after the retry did not render");
		return;
	}
	if (!tile->test_read_tile_order_stats(after)) {
		FAIL("after the retry the tile order counters are unreadable (post-frame read), so the recovered pass cannot be proven");
		return;
	}
	CHECK_MESSAGE(after.passes > before.passes, "the total-order pass did not run after the retry");
}
