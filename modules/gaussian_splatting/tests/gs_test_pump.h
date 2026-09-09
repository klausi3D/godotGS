/**************************************************************************/
/*  gs_test_pump.h                                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "tests/test_macros.h"

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
// So the budget has to be wall clock. Three properties are non-negotiable:
//
//   1. It KEEPS PUMPING. The frames are what drive the async work forward and
//      what observe its completion. A deadline that merely slept would let the
//      case "pass" for a different wrong reason, so this helper never sleeps --
//      it calls the caller's frame function as fast as it can.
//   2. The deadline is a FAILURE, not a silent pass. Every caller must inspect
//      `ready()` and FAIL with the state it observed, naming what never became
//      true. A deadline that swallows the failure is indistinguishable from a
//      fix. This one is ENFORCED rather than documented -- see the unchecked-
//      outcome guard on `GSPumpOutcome` below and the note on why.
//   3. The deadline BINDS THE PASS, not just the loop. Readiness is only accepted
//      from a frame that also finished inside the deadline (see the ordering note
//      on `gs_pump_until` below). A helper that reports ready off a frame which
//      ran past its own bound has reproduced #879 one level in: the pass is again
//      something a slow machine can manufacture.
//
// The deadline is a failure bound, not a budget: on a healthy pipeline the loop
// returns on the first frame that satisfies the condition, so the bound costs
// nothing until something is genuinely broken.
static constexpr uint64_t GS_PUMP_DEADLINE_USEC = 10ULL * 1000ULL * 1000ULL; // 10 s ~= 8x the ~1.2 s measured in #879.

struct GSPumpOutcome;

// `[[nodiscard]]` catches the coarsest misuse -- calling the pump purely for its
// side effect and dropping the outcome on the floor. It does NOT catch the three
// sites this round fixed (they all stored the outcome), which is why the guard on
// the type below exists as well.
template <typename TPump>
[[nodiscard]] GSPumpOutcome gs_pump_until(TPump p_pump, uint64_t p_deadline_usec = GS_PUMP_DEADLINE_USEC, int p_min_frames = 1);

// THE UNCHECKED-OUTCOME GUARD (PR #881 review, round 3).
//
// Property 2 above was written as a rule for callers to remember, and callers
// forgot it: of the ten call sites this helper had, THREE never looked at the
// outcome at all. They re-derived readiness from the observable itself -- "the
// streaming system is current now", "the metrics dictionary has the key now" --
// and passed on a pump that had already reported failure. The helper was honest
// and the callers threw the answer away, which is #879 a third level in: the
// wall-clock bound existed, was correctly enforced, and bound nothing.
//
// A rule that must be remembered will eventually be forgotten; the fix is to
// make forgetting it impossible rather than to write the rule down again. C++
// cannot express "you must branch on this bool" -- `[[nodiscard]]` only catches
// dropping the whole value, which none of the three sites did (all three stored
// the outcome and used `describe()` in a message). So the obligation is enforced
// where it CAN be: `ready()` is the only way to read the success flag, and it
// records that it was read; the destructor fails the case if it never was. It
// does so WITHOUT throwing -- see the note on the destructor for why a guard
// that unwound out of a destructor destroyed more coverage than it protected.
//
// The guard is unconditional -- it fires on a ready outcome as loudly as on an
// expired one -- deliberately. A guard that only fired on expiry would sit dark
// through every healthy run and only wake up on the slow machine years later,
// which is the same latent-failure shape as the bug it guards. Unconditional
// means every call site exercises it on every run, so a NEW caller that forgets
// to check fails immediately, on the developer's own healthy box.
struct GSPumpOutcome {
	// DIAGNOSTIC ONLY -- never a success signal, never something a caller may
	// branch on. The condition DID hold, but only on a frame that ended past the
	// deadline, so `gs_pump_until()` refused it (see the ordering note there).
	// It exists so "converging, but slower than the bound" reads differently from
	// "this never converges" in the FAIL message.
	bool late_ready = false;
	int frames = 0;
	uint64_t elapsed_usec = 0;

	// The ONLY success signal, and the only route to it. True if and only if the
	// condition held on a frame that also finished inside the deadline. Reading
	// it discharges the obligation the destructor below enforces, so a caller
	// that decides readiness any other way -- by re-reading the renderer, or by
	// re-testing the dictionary the pump was waiting on -- still fails.
	[[nodiscard]] bool ready() const {
		consumed = true;
		return ready_flag;
	}

	// For the guard's own contract test only. Reports whether `ready()` has been
	// called WITHOUT counting as that call, so a case can prove the bookkeeping the
	// destructor depends on. Never a substitute for `ready()`.
	bool test_verdict_was_read() const { return consumed; }

	double elapsed_ms() const { return double(elapsed_usec) / 1000.0; }

	// Tail for a FAIL message, so a deadline expiry always records how hard the
	// case tried before giving up (and lets a future reader tell "the deadline
	// is too tight" apart from "this never converges").
	String describe() const {
		String tail = vformat("after %d frame(s) / %.1f ms of pumping", frames, elapsed_ms());
		if (late_ready) {
			tail += " (the condition DID hold, but only on a frame that ended AFTER the deadline - "
					"the pipeline is converging, just slower than the bound)";
		}
		return tail;
	}

	GSPumpOutcome() = default;
	// Copying transfers the obligation to the copy rather than duplicating it, so
	// `return outcome;` out of the helper (and any caller that stores a copy)
	// leaves exactly one object that still owes a `ready()` call.
	GSPumpOutcome(const GSPumpOutcome &p_other) :
			late_ready(p_other.late_ready),
			frames(p_other.frames),
			elapsed_usec(p_other.elapsed_usec),
			ready_flag(p_other.ready_flag) {
		p_other.consumed = true;
	}
	// Deleted rather than written: assigning over an outcome whose `ready()` was
	// never called would drop that obligation where the destructor cannot see it,
	// which is the one way the guard could be defeated by accident. No call site
	// assigns an outcome, so deleting it costs nothing and closes the hole at
	// compile time.
	GSPumpOutcome &operator=(const GSPumpOutcome &) = delete;

	// A DESTRUCTOR MAY NOT THROW (PR #881 review, round 4).
	//
	// This guard reported the violation with `FAIL`, whose severity is
	// `is_require`; doctest's `MessageBuilder::react()` throws
	// `TestFailureException` for exactly that severity. A destructor is
	// implicitly `noexcept`, so with `disable_exceptions=false` -- a supported
	// build of this repo -- the throw never reached the doctest runner: it hit
	// `std::terminate` and killed the whole test binary, erasing every case that
	// had not run yet. A guard that exists to stop one case from passing
	// unverified would have deleted the coverage of every case behind it, which
	// is strictly worse than the defect it guards.
	//
	// `FAIL_CHECK` has severity `is_check`, and `react()` throws only for
	// `is_require`. So it records the same failed assertion against the same
	// running case, through the same reporter, and the run still exits non-zero
	// -- it simply does not unwind. That is the whole difference.
	//
	// Nothing is weakened by this, in either configuration:
	//
	//   * The obligation is unchanged and still UNCONDITIONAL. Every outcome,
	//     ready or expired, must have had `ready()` called on it, and any that
	//     did not still fails its case.
	//   * In the DEFAULT build (`disable_exceptions=True`) this is a provable
	//     no-op. That build defines DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
	//     (see `tests/SCsub`), under which `throwException()` has an empty body --
	//     so `FAIL` already did not unwind, and already behaved exactly as
	//     `FAIL_CHECK` does. Nothing about today's CI changes.
	//   * The abort semantics `is_require` would buy are not available here
	//     anyway. This runs at the end of the outcome's scope, after the case has
	//     already made the assertions it was going to make, so there is nothing
	//     left to abort -- and it can also run while ANOTHER failure is unwinding,
	//     which is the second way the old form reached `std::terminate`.
	//
	// The failing branch still cannot be asserted from a passing case (it fails
	// the case by construction), so it stays mutation-proven -- see the note on
	// the "Reading the verdict" case in `test_gs_pump.h`.
	~GSPumpOutcome() {
		if (consumed) {
			return;
		}
		FAIL_CHECK("A gs_pump_until() outcome was discarded without calling ready() - the deadline it "
				"reports can therefore not have been honoured by this case. Inspect ready() and FAIL "
				"on it; re-deriving readiness from the observable the pump was waiting on is exactly "
				"the defect this guard exists to stop. Outcome: ready=",
				ready_flag, " ", describe());
	}

private:
	bool ready_flag = false;
	// `mutable` so `ready()` can stay const: callers hold the outcome by const
	// reference or as a `const` local, and making the check non-const would push
	// them back towards not calling it.
	mutable bool consumed = false;

	template <typename TPump>
	friend GSPumpOutcome gs_pump_until(TPump p_pump, uint64_t p_deadline_usec, int p_min_frames);
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
//
// ORDER OF THE TWO EXITS (PR #881 review). Once the floor is satisfied, EXPIRY IS
// CHECKED FIRST and readiness second, so the post-condition is exact:
//
//     outcome.ready()  =>  outcome.elapsed_usec < p_deadline_usec
//
// Checking readiness first -- the obvious order, and the one this helper shipped
// with -- lets the deadline be evaded by exactly the thing the deadline exists to
// bound. A frame that begins at 9.9 s and takes 5 s (a slow render, a stalled
// submit, a driver hiccup) returns `true` at 14.9 s; with readiness checked first
// the helper reports ready, every caller passes, and the 10 s wall-clock bound was
// enforced against nothing. That is #879 one level in: a pass that a slower machine
// can manufacture.
//
// The boundary case is deliberate, not an oversight: a frame that STARTS before the
// deadline and ENDS after it is REJECTED. Only two timestamps exist -- the frame's
// start and its end -- and the observation itself happened somewhere between them,
// at an instant nobody records. Crediting the start would restore the hole above,
// because a frame's start says nothing about when its observation was taken; the
// only enforceable bound is the upper one, "the condition was seen to hold no later
// than X". So the frame end is what the deadline is measured against. The cost is
// the honest one: a pipeline that genuinely converges at 9.99 s on a frame that
// straddles 10 s is reported as a failure. That is the intended reading of a
// FAILURE BOUND set at ~8x the measured ~1.2 s requirement -- at 10 s something is
// already wrong -- and `describe()` says the condition did hold but too late, so
// "too tight a deadline" is still one line away from "never converges".
template <typename TPump>
GSPumpOutcome gs_pump_until(TPump p_pump, uint64_t p_deadline_usec, int p_min_frames) {
	GSPumpOutcome outcome;
	const OS *os = OS::get_singleton();
	const uint64_t started_usec = os ? os->get_ticks_usec() : 0;
	for (;;) {
		outcome.frames++;
		const bool ready = p_pump();
		// No clock means the wall-clock bound cannot be enforced at all, so treat
		// it as already expired: fail-closed. Accepting readiness in that state
		// would be a pass obtained without the bound this helper exists to apply.
		outcome.elapsed_usec = os ? (os->get_ticks_usec() - started_usec) : p_deadline_usec;
		if (outcome.frames < p_min_frames) {
			// The floor outranks both exits: below it, neither readiness nor
			// expiry may end the loop.
			continue;
		}
		if (outcome.elapsed_usec >= p_deadline_usec) {
			// `ready` is recorded, never honoured: `outcome.ready()` stays false.
			outcome.late_ready = ready;
			return outcome;
		}
		if (ready) {
			outcome.ready_flag = true;
			return outcome;
		}
	}
}

} // namespace TestGaussianSplatting
