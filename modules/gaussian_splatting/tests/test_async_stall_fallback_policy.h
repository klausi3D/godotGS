#pragma once

#include "test_macros.h"

// [VRAMBudgetRegulator] is the strict lane of run_module_tests.py: a case only under an unlisted tag lands in the advisory
// [untagged] net, where a failure cannot fail CI.

#include "../core/streaming_runtime_state.h"

namespace AsyncStallFallbackPolicyTests {

using namespace GaussianStreamingTypes;

static constexpr uint64_t kMsec = 1000;

// Drives the decision the way GaussianStreamingSystem does: each frame first evaluates with the
// previous end_frame() snapshot, then end_frame() recomputes the upload-stall predicate from the
// frame's queues and completed async uploads. The wall clock is DiagnosticsState's own
// update_upload_stall_wall_clock(); the predicate mirrors streaming_diagnostics_surface.cpp
// (queued work and no completed upload, the sync path idle).
struct StallSim {
	DiagnosticsState diagnostics;
	bool forced = false;
	uint32_t last_upload_chunks = 0;
	uint64_t now_usec = 1000000;

	bool frame(uint32_t p_pack_depth, uint32_t p_upload_depth, uint32_t p_completed_uploads, uint64_t p_frame_usec) {
		AsyncStallFallbackInputs in;
		in.currently_forced = forced;
		in.pack_queue_depth = p_pack_depth;
		in.upload_queue_depth = p_upload_depth;
		in.sync_fallback_queue_depth = 0;
		in.last_upload_chunks = last_upload_chunks;
		in.upload_stall_frames = diagnostics.upload_stall_frames;
		in.upload_stall_wall_usec = diagnostics.upload_stall_wall_usec;
		forced = evaluate_async_stall_sync_fallback(in);

		now_usec += p_frame_usec;
		const bool stalled = (p_pack_depth > 0 || p_upload_depth > 0) && p_completed_uploads == 0;
		diagnostics.upload_stall_frames = stalled ? diagnostics.upload_stall_frames + 1 : 0;
		diagnostics.update_upload_stall_wall_clock(stalled, now_usec);
		last_upload_chunks = p_completed_uploads;
		return forced;
	}
};

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][AsyncStallFallback] 2 ms frames and a 350 ms post-teleport pack do not force the sync fallback") {
	// PR #1397: with the pool in VRAM, frames take ~2 ms and the first packs after a teleport
	// 270-350 ms. 30 stalled frames are 60 ms, so the frame count alone fired here.
	StallSim sim;
	bool ever_forced = false;
	uint32_t max_stall_frames = 0;
	// 350 ms of pack work that has not reached the upload queue (narrow signature).
	for (uint64_t t = 0; t < 350 * kMsec; t += 2 * kMsec) {
		ever_forced |= sim.frame(8, 0, 0, 2 * kMsec);
		max_stall_frames = MAX(max_stall_frames, sim.diagnostics.upload_stall_frames);
	}
	// Then 40 ms in the upload queue before the first completion (broader signature).
	for (uint64_t t = 0; t < 40 * kMsec; t += 2 * kMsec) {
		ever_forced |= sim.frame(4, 4, 0, 2 * kMsec);
		max_stall_frames = MAX(max_stall_frames, sim.diagnostics.upload_stall_frames);
	}
	// Discrimination: the frame-count predicate the old decision used alone was satisfied
	// (195 stalled frames >= 30), so only the wall-clock condition keeps the flag clear.
	CHECK(max_stall_frames >= DiagnosticsState::STALL_THRESHOLD_FRAMES);
	CHECK(sim.diagnostics.upload_stall_wall_usec < DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC);
	CHECK_FALSE(ever_forced);

	sim.frame(4, 3, 1, 2 * kMsec);
	CHECK(sim.diagnostics.upload_stall_frames == 0);
	CHECK(sim.diagnostics.upload_stall_wall_usec == 0);
	CHECK_FALSE(sim.frame(4, 3, 1, 2 * kMsec));
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][AsyncStallFallback] a hung pipeline still forces the sync fallback once the wall threshold passes") {
	SUBCASE("pack thread wedged, 2 ms frames") {
		StallSim sim;
		uint32_t frames = 0;
		uint64_t wall_when_forced = 0;
		while (frames < 5000 && !sim.forced) {
			const uint64_t wall_before = sim.diagnostics.upload_stall_wall_usec;
			if (sim.frame(8, 0, 0, 2 * kMsec)) {
				wall_when_forced = wall_before;
			}
			frames++;
		}
		CHECK(sim.forced);
		CHECK(wall_when_forced >= DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC);
		// It fires on the first frame the wall threshold is met, not later: 2 s / 2 ms frames.
		CHECK(wall_when_forced < DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC + 2 * kMsec);
		CHECK(frames <= uint32_t(DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC / (2 * kMsec)) + 2);
	}
	SUBCASE("GPU uploads never complete, 2 ms frames") {
		StallSim sim;
		uint32_t frames = 0;
		while (frames < 5000 && !sim.forced) {
			sim.frame(2, 6, 0, 2 * kMsec);
			frames++;
		}
		CHECK(sim.forced);
		CHECK(sim.diagnostics.upload_stall_wall_usec >= DiagnosticsState::ASYNC_STALL_MIN_WALL_USEC);
	}
	SUBCASE("100 ms frames still need the unchanged 30-frame count") {
		StallSim sim;
		uint32_t frames = 0;
		while (frames < 5000 && !sim.forced) {
			sim.frame(8, 0, 0, 100 * kMsec);
			frames++;
		}
		CHECK(sim.forced);
		// The wall clock passes 2 s after 20 stalled frames; the frame count gates until 30.
		CHECK(frames == DiagnosticsState::STALL_THRESHOLD_FRAMES + 1);
	}
}

TEST_CASE("[GaussianSplatting][VRAMBudgetRegulator][AsyncStallFallback] the forced fallback clears as soon as an async upload completes") {
	AsyncStallFallbackInputs in;
	in.currently_forced = true;
	in.pack_queue_depth = 12;
	in.upload_queue_depth = 3;
	in.sync_fallback_queue_depth = 5;
	in.upload_stall_frames = 0;
	in.upload_stall_wall_usec = 0;

	// Queues still busy and no completion: the flag holds (the old clear needed all three empty).
	in.last_upload_chunks = 0;
	CHECK(evaluate_async_stall_sync_fallback(in));
	// One completed async upload clears it although no queue is empty.
	in.last_upload_chunks = 1;
	CHECK_FALSE(evaluate_async_stall_sync_fallback(in));
	// The old all-queues-empty clear still applies.
	in.last_upload_chunks = 0;
	in.pack_queue_depth = 0;
	in.upload_queue_depth = 0;
	in.sync_fallback_queue_depth = 0;
	CHECK_FALSE(evaluate_async_stall_sync_fallback(in));

	// End to end: a hung pipeline forces the fallback, the first completion clears it, and a
	// stall that resumes needs the full wall threshold again.
	StallSim sim;
	uint32_t frames = 0;
	while (frames < 5000 && !sim.forced) {
		sim.frame(8, 2, 0, 2 * kMsec);
		frames++;
	}
	if (!sim.forced) {
		FAIL("the hung pipeline never forced the sync fallback, so the clear cannot be tested");
		return;
	}
	CHECK(sim.frame(8, 2, 0, 2 * kMsec));
	sim.frame(8, 2, 1, 2 * kMsec); // this frame's end_frame() records the completed upload
	CHECK_FALSE(sim.frame(8, 2, 0, 2 * kMsec)); // the next decision sees it and clears
	bool re_forced_early = false;
	for (uint64_t t = 0; t < 1000 * kMsec; t += 2 * kMsec) {
		re_forced_early |= sim.frame(8, 2, 0, 2 * kMsec);
	}
	CHECK_FALSE(re_forced_early);
}

} // namespace AsyncStallFallbackPolicyTests
