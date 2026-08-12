/**************************************************************************/
/*  gs_test_pump.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/os/os.h"
#include "core/variant/variant.h"

namespace TestGaussianSplatting {

// #879: bounded "pump until ready" for renderer/GPU test cases.
//
// A renderer case has to advance the pipeline before it can assert on it. The
// obvious way to do that -- render a FIXED number of frames and then assert --
// is a race, because the conditions being waited on are satisfied by
// ASYNCHRONOUS work (the streaming async-pack worker threads;
// `rendering/gaussian_splatting/streaming/async_pack_enabled` defaults to true)
// rather than by the frames themselves. The frames only poll.
//
// #879 measured that directly on the "World-backed RenderSceneInstance drives
// GPU streaming + sorting" case: first residency needs ~1.2 s of WALL CLOCK,
// and the number of frames it takes FALLS as the frames are slowed down
// (104 frames with no per-frame sleep, 62 with a 2 ms sleep -- ~1.2 s elapsed
// in every arm). Its 16-frame budget was ~0.2 s on today's runner, a ~6x
// shortfall; the case had been passing only because the runner was slow enough
// (page heap, removed in #875) to spend 1.2 s on 16 frames.
//
// So the budget has to be wall clock. Two properties are non-negotiable:
//
//   1. It KEEPS PUMPING. The frames are what drive the async work forward and
//      what observe its completion. A deadline that merely slept would let the
//      case "pass" for a different wrong reason, so this helper never sleeps --
//      it calls the caller's frame function as fast as it can.
//   2. The deadline is a FAILURE, not a silent pass. This helper only reports;
//      every caller must inspect `ready` and FAIL with the state it observed,
//      naming what never became true. A deadline that swallows the failure is
//      indistinguishable from a fix.
//
// The deadline is a failure bound, not a budget: on a healthy pipeline the loop
// returns on the first frame that satisfies the condition, so the bound costs
// nothing until something is genuinely broken.
static constexpr uint64_t GS_PUMP_DEADLINE_USEC = 10ULL * 1000ULL * 1000ULL; // 10 s ~= 8x the ~1.2 s measured in #879.

struct GSPumpOutcome {
	bool ready = false;
	int frames = 0;
	uint64_t elapsed_usec = 0;

	double elapsed_ms() const { return double(elapsed_usec) / 1000.0; }

	// Tail for a FAIL message, so a deadline expiry always records how hard the
	// case tried before giving up (and lets a future reader tell "the deadline
	// is too tight" apart from "this never converges").
	String describe() const {
		return vformat("after %d frame(s) / %.1f ms of pumping", frames, elapsed_ms());
	}
};

// `p_pump` must advance the pipeline by exactly one frame and return whether the
// awaited condition holds after that frame. Returns as soon as it does;
// otherwise keeps pumping until `p_deadline_usec` of wall clock has elapsed.
// The condition is always evaluated after at least one frame.
//
// `p_min_frames` is a FLOOR, for the handful of cases that must observe a fixed
// number of frames before their assertions are meaningful. A floor is not the
// #879 defect: a fixed *upper* bound loses a race when the machine gets faster,
// while a fixed *lower* bound cannot. Never use it to reintroduce a ceiling.
template <typename TPump>
GSPumpOutcome gs_pump_until(TPump p_pump, uint64_t p_deadline_usec = GS_PUMP_DEADLINE_USEC, int p_min_frames = 1) {
	GSPumpOutcome outcome;
	const OS *os = OS::get_singleton();
	const uint64_t started_usec = os ? os->get_ticks_usec() : 0;
	for (;;) {
		outcome.frames++;
		const bool ready = p_pump();
		outcome.elapsed_usec = os ? (os->get_ticks_usec() - started_usec) : p_deadline_usec;
		if (ready && outcome.frames >= p_min_frames) {
			outcome.ready = true;
			return outcome;
		}
		if (outcome.elapsed_usec >= p_deadline_usec && outcome.frames >= p_min_frames) {
			return outcome;
		}
	}
}

} // namespace TestGaussianSplatting
