/**************************************************************************/
/*  test_gs_pump.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

#include "core/os/os.h"
#include "gs_test_pump.h"

namespace TestGaussianSplatting {

// #879 / PR #881: the contract of `gs_pump_until()` itself.
//
// The helper exists to replace a frame budget that a fast machine could break.
// Its own review found the mirror-image hole: with readiness checked before
// expiry, a frame that returns `true` only AFTER the deadline was accepted, so
// every caller passed and the wall-clock bound was enforced against nothing --
// #879 one level in. These cases pin the post-condition
//
//     outcome.ready  =>  outcome.elapsed_usec < deadline
//
// in both directions, plus the floor and the never-converges path.
//
// On the deterministic-fixture rule (tests/AGENTS.md): these cases DO read the
// wall clock, because the wall clock is the unit under test. They do not depend
// on how fast this machine is: the pump lambda drives the clock relation itself
// (it spins until it has certainly crossed, or certainly not crossed, the
// deadline it was given), and the deadline is injected per case rather than
// taken from GS_PUMP_DEADLINE_USEC. Nothing here sleeps, for the same reason the
// helper never sleeps.

// Busy-wait, never sleep. Used to make one pump iteration deliberately slow, the
// way a stalled render or submit makes a real one slow.
static void gs_pump_test_spin_usec(uint64_t p_usec) {
	const OS *os = OS::get_singleton();
	if (os == nullptr) {
		return;
	}
	const uint64_t spin_started_usec = os->get_ticks_usec();
	while (os->get_ticks_usec() - spin_started_usec < p_usec) {
		// Spin. The helper's contract is about elapsed wall clock, so the delay
		// has to be real elapsed time rather than a mocked clock.
	}
}

TEST_CASE("[GaussianSplatting][TestPump] Readiness observed only after the deadline is a failure, not a pass") {
	// The exact shape the PR #881 review named: the frame that FIRST observes the
	// condition is slow and ends past the deadline. Before the ordering fix this
	// returned ready=true at ~2x the deadline and every caller passed.
	if (OS::get_singleton() == nullptr) {
		FAIL("OS singleton unavailable - gs_pump_until() cannot be exercised without a clock");
		return;
	}
	const uint64_t deadline_usec = 20000; // 20 ms.
	int pump_calls = 0;
	const GSPumpOutcome outcome = gs_pump_until([&]() {
		pump_calls++;
		gs_pump_test_spin_usec(deadline_usec * 2);
		return true; // Ready -- but only at ~2x the deadline.
	},
			deadline_usec);

	CHECK_MESSAGE(!outcome.ready,
			vformat("Expected late readiness to be refused, got ready=true %s", outcome.describe()));
	CHECK_MESSAGE(outcome.late_ready,
			"Expected the refused readiness to be recorded as late_ready for the FAIL message");
	CHECK_MESSAGE(outcome.elapsed_usec >= deadline_usec,
			vformat("Expected the reported elapsed time to have crossed the deadline, got %.1f ms",
					outcome.elapsed_ms()));
	CHECK_MESSAGE(pump_calls == 1, vformat("Expected the slow frame to end the loop, saw %d pump call(s)", pump_calls));
	CHECK(outcome.frames == 1);
	CHECK_MESSAGE(outcome.describe().contains("AFTER the deadline"),
			vformat("Expected describe() to distinguish late readiness from never converging, got '%s'",
					outcome.describe()));
}

TEST_CASE("[GaussianSplatting][TestPump] In-time readiness is still accepted on the first satisfying frame") {
	// The other direction of the same ordering: making the deadline bind must not
	// cost a healthy pipeline its pass, and must not cost it extra frames either.
	if (OS::get_singleton() == nullptr) {
		FAIL("OS singleton unavailable - gs_pump_until() cannot be exercised without a clock");
		return;
	}
	const uint64_t deadline_usec = 5ULL * 1000ULL * 1000ULL; // 5 s, far above what 3 trivial frames cost.
	int pump_calls = 0;
	const GSPumpOutcome outcome = gs_pump_until([&]() {
		pump_calls++;
		return pump_calls >= 3;
	},
			deadline_usec);

	CHECK_MESSAGE(outcome.ready, vformat("Expected in-time readiness to be accepted, gave up %s", outcome.describe()));
	CHECK_FALSE(outcome.late_ready);
	CHECK_MESSAGE(outcome.frames == 3,
			vformat("Expected the loop to return on the first satisfying frame, saw %d", outcome.frames));
	CHECK(pump_calls == 3);
	CHECK_MESSAGE(outcome.elapsed_usec < deadline_usec,
			vformat("A ready outcome must be inside its deadline, got %.1f ms", outcome.elapsed_ms()));
}

TEST_CASE("[GaussianSplatting][TestPump] A condition that never holds fails at the deadline") {
	// Anti-vacuity: the deadline must still be reachable and must still report a
	// failure, and it must be distinguishable from the late-readiness failure.
	if (OS::get_singleton() == nullptr) {
		FAIL("OS singleton unavailable - gs_pump_until() cannot be exercised without a clock");
		return;
	}
	const uint64_t deadline_usec = 20000; // 20 ms.
	int pump_calls = 0;
	const GSPumpOutcome outcome = gs_pump_until([&]() {
		pump_calls++;
		gs_pump_test_spin_usec(2000); // 2 ms of "work" per frame, so the frame count stays readable.
		return false;
	},
			deadline_usec);

	CHECK_FALSE(outcome.ready);
	CHECK_MESSAGE(!outcome.late_ready,
			"A condition that never held must not be reported as late readiness");
	CHECK_MESSAGE(outcome.elapsed_usec >= deadline_usec,
			vformat("Expected the loop to run out the full deadline, got %.1f ms", outcome.elapsed_ms()));
	CHECK_MESSAGE(outcome.frames >= 2, vformat("Expected the loop to keep pumping, saw %d frame(s)", outcome.frames));
	CHECK_MESSAGE(pump_calls == outcome.frames,
			vformat("Expected one pump call per reported frame, saw %d calls for %d frames", pump_calls, outcome.frames));
	CHECK_FALSE(outcome.describe().contains("AFTER the deadline"));
}

TEST_CASE("[GaussianSplatting][TestPump] The minimum-frame floor outranks both exits") {
	if (OS::get_singleton() == nullptr) {
		FAIL("OS singleton unavailable - gs_pump_until() cannot be exercised without a clock");
		return;
	}

	SUBCASE("An expired deadline cannot cut the floor short") {
		// Expiry happens on frame 1, but the caller asked for 3 frames: the floor
		// is a lower bound, so the loop owes those frames before either exit.
		const uint64_t deadline_usec = 5000; // 5 ms, crossed by the first frame below.
		int pump_calls = 0;
		const GSPumpOutcome outcome = gs_pump_until([&]() {
			pump_calls++;
			if (pump_calls == 1) {
				gs_pump_test_spin_usec(deadline_usec * 2);
			}
			return true;
		},
				deadline_usec, 3);

		CHECK_MESSAGE(outcome.frames == 3,
				vformat("Expected the floor to be honoured past expiry, saw %d frame(s)", outcome.frames));
		CHECK(pump_calls == 3);
		CHECK_MESSAGE(!outcome.ready, "Readiness observed past the deadline stays refused under a floor too");
		CHECK(outcome.late_ready);
	}

	SUBCASE("Readiness before the floor does not return early") {
		const uint64_t deadline_usec = 5ULL * 1000ULL * 1000ULL; // 5 s.
		int pump_calls = 0;
		const GSPumpOutcome outcome = gs_pump_until([&]() {
			pump_calls++;
			return true; // Ready from the very first frame.
		},
				deadline_usec, 3);

		CHECK_MESSAGE(outcome.ready, vformat("Expected in-time readiness at the floor, gave up %s", outcome.describe()));
		CHECK_MESSAGE(outcome.frames == 3,
				vformat("Expected exactly the floor's frames, saw %d", outcome.frames));
		CHECK(pump_calls == 3);
	}
}

} // namespace TestGaussianSplatting
