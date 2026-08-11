#pragma once

#include "test_macros.h"

#include "../renderer/sort_fallback_policy.h"
#include "../renderer/tile_render_types.h"

#include <string.h>

namespace TestGaussianSplatting {

using namespace GaussianSplatting;

TEST_CASE("[GaussianSplatting][SortFallback] force_cpu_sort policy stays deterministic across domains") {
	const SortFallbackPolicyDecision instance_policy =
			build_sort_fallback_policy(SortFallbackScenario::FORCE_CPU_OVERRIDE, true);
	CHECK(instance_policy.action_count == 2);
	CHECK(instance_policy.actions[0] == SortFallbackAction::REUSE_PREVIOUS_SORT);
	CHECK(instance_policy.actions[1] == SortFallbackAction::FAIL);
	CHECK(!instance_policy.cpu_sort_forced);

	const SortFallbackPolicyDecision global_policy =
			build_sort_fallback_policy(SortFallbackScenario::FORCE_CPU_OVERRIDE, false);
	CHECK(global_policy.action_count == 2);
	CHECK(global_policy.actions[0] == SortFallbackAction::RUN_CPU_SORT);
	CHECK(global_policy.actions[1] == SortFallbackAction::FAIL);
	CHECK(global_policy.cpu_sort_forced);
}

TEST_CASE("[GaussianSplatting][SortFallback] GPU fallback policy reuses then CPU-sorts or fails") {
	for (SortFallbackScenario scenario : { SortFallbackScenario::SORTER_UNAVAILABLE, SortFallbackScenario::GPU_SORT_FAILED }) {
		SUBCASE(scenario == SortFallbackScenario::SORTER_UNAVAILABLE ? "sorter unavailable" : "gpu sort failed") {
			const SortFallbackPolicyDecision instance_policy = build_sort_fallback_policy(scenario, true);
			CHECK(instance_policy.action_count == 2);
			CHECK(instance_policy.actions[0] == SortFallbackAction::REUSE_PREVIOUS_SORT);
			CHECK(instance_policy.actions[1] == SortFallbackAction::FAIL);
			CHECK(!instance_policy.cpu_sort_forced);

			const SortFallbackPolicyDecision global_policy = build_sort_fallback_policy(scenario, false);
			CHECK(global_policy.action_count == 3);
			CHECK(global_policy.actions[0] == SortFallbackAction::REUSE_PREVIOUS_SORT);
			CHECK(global_policy.actions[1] == SortFallbackAction::RUN_CPU_SORT);
			CHECK(global_policy.actions[2] == SortFallbackAction::FAIL);
			CHECK(!global_policy.cpu_sort_forced);
		}
	}
}

TEST_CASE("[GaussianSplatting][SortFallback] instance domain never schedules an unsorted fallback") {
	// Identity publication and cull-order bootstrap have been removed. The
	// instance domain must only ever see REUSE_PREVIOUS_SORT followed by FAIL,
	// regardless of scenario or strict-mode flag.
	for (SortFallbackScenario scenario : { SortFallbackScenario::FORCE_CPU_OVERRIDE,
				 SortFallbackScenario::SORTER_UNAVAILABLE,
				 SortFallbackScenario::GPU_SORT_FAILED }) {
		for (bool strict : { false, true }) {
			const SortFallbackPolicyDecision policy = build_sort_fallback_policy(scenario, true, strict);
			for (uint32_t i = 0; i < policy.action_count; i++) {
				CHECK(policy.actions[i] != SortFallbackAction::RUN_CPU_SORT);
				// No stand-in for the deleted PUBLISH_INSTANCE_IDENTITY survives in
				// the enum, so simply verify the remaining set is just reuse / fail.
				CHECK((policy.actions[i] == SortFallbackAction::REUSE_PREVIOUS_SORT ||
						policy.actions[i] == SortFallbackAction::FAIL));
			}
		}
	}
}

TEST_CASE("[GaussianSplatting][SortFallback] strict mode gates unsorted CPU fallback when positions are unavailable") {
	CHECK(allow_unsorted_cpu_fallback_in_orchestrator(false, false));
	CHECK(allow_unsorted_cpu_fallback_in_orchestrator(true, true));
	CHECK_FALSE(allow_unsorted_cpu_fallback_in_orchestrator(true, false));
}

// #586: the unsorted global-composite fallback must be OBSERVABLE, not one-shot.
// should_warn_unsorted_composite drives the per-frame throttled WARN paired with
// the persistent unsorted_composite_frames counter. This locks the rate-limit
// contract: warn on the first degraded frame and then once per interval, and NEVER
// warn when there is no degraded frame (counter == 0).
TEST_CASE("[GaussianSplatting][SortFallback] unsorted-composite warning is rate-limited, not one-shot (#586)") {
	// No degraded frame yet -> never warn.
	CHECK_FALSE(should_warn_unsorted_composite(0));

	// First degraded frame always warns so the degradation is immediately visible.
	CHECK(should_warn_unsorted_composite(1));

	// Immediately-subsequent frames are throttled (no per-frame log spam).
	CHECK_FALSE(should_warn_unsorted_composite(2));
	CHECK_FALSE(should_warn_unsorted_composite(UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES - 1));

	// The throttle re-fires once per interval -> a persistent degradation keeps
	// re-surfacing in the log (this is the whole point vs. the old one-shot bool).
	CHECK(should_warn_unsorted_composite(UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES));
	CHECK_FALSE(should_warn_unsorted_composite(UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES + 1));
	CHECK(should_warn_unsorted_composite(UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES * 2));
	CHECK(should_warn_unsorted_composite(UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES * 7));
}

// Simulate the render loop bumping the persistent counter every degraded frame and
// prove the warning fires MORE THAN ONCE over a sustained fallback (the failure the
// audit flagged: the previous sorter_missing_logged bool logged exactly once per
// process). Over N sustained frames we expect 1 (first) + floor(N / interval) warns.
TEST_CASE("[GaussianSplatting][SortFallback] sustained unsorted fallback keeps warning across frames (#586)") {
	const uint64_t total_frames = UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES * 3 + 5;
	uint64_t counter = 0;
	uint64_t warn_count = 0;
	uint64_t first_warn_frame = 0;
	for (uint64_t frame = 0; frame < total_frames; frame++) {
		// Mirrors tile_renderer.cpp: pre-increment, then throttle off the new value.
		const uint64_t value = ++counter;
		if (should_warn_unsorted_composite(value)) {
			if (warn_count == 0) {
				first_warn_frame = value;
			}
			warn_count++;
		}
	}
	CHECK_EQ(counter, total_frames); // every degraded frame is counted (observable).
	CHECK_EQ(first_warn_frame, 1u); // first degraded frame surfaces immediately.
	CHECK(warn_count > 1u); // NOT one-shot: re-warns on a sustained degradation.
	CHECK_EQ(warn_count, 1u + (total_frames / UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES));
}

// #586 REGRESSION: the unsorted-output counter must observe the GPU-driven/indirect
// work path. In async (GPU-driven) mode the CPU overlap count and splat_count are
// stale by design and can read 0 while the real work is described by the instance
// indirect-dispatch buffer. An earlier revision re-derived the counter's work
// predicate as `splat_count > 0` only — dropping the indirect term — so every
// GPU-driven frame rasterized UNSORTED without being counted. That under-reporting
// is worse than no counter: it makes the wrong-output path look rare.
TEST_CASE("[GaussianSplatting][SortFallback] indirect (GPU-driven) work counts as translucent work (#586)") {
	// THE PREVIOUSLY-MISSED PATH: async mode, CPU splat_count == 0, work described
	// solely by the instance indirect-dispatch buffer.
	CHECK(global_composite_has_translucent_work(
			/*allow_sync_readback=*/false, /*overlap_record_count=*/0,
			/*splat_count=*/0, /*has_instance_indirect=*/true));

	// ...and with no indirect buffer and no splats there is genuinely nothing to
	// composite, so it must NOT be counted (no false positives).
	CHECK_FALSE(global_composite_has_translucent_work(false, 0, 0, false));

	// Async mode still counts CPU-visible splats with no indirect buffer.
	CHECK(global_composite_has_translucent_work(false, 0, 128, false));

	// Sync-readback mode: the freshly read overlap-record count is authoritative,
	// and an indirect buffer alone must not fabricate work.
	CHECK(global_composite_has_translucent_work(true, 42, 0, false));
	CHECK_FALSE(global_composite_has_translucent_work(true, 0, 4096, true));
}

// The counter fires on the previously-missed indirect path end-to-end: work present
// via the indirect buffer + no usable sorter => classified as wrong output.
TEST_CASE("[GaussianSplatting][SortFallback] unsorted counter fires on the indirect-work path (#586)") {
	const bool has_work = global_composite_has_translucent_work(
			/*allow_sync_readback=*/false, /*overlap_record_count=*/0,
			/*splat_count=*/0, /*has_instance_indirect=*/true);
	REQUIRE(has_work); // the path exists at all.

	// Sorter unavailable => no sort dispatched => UNSORTED output, and it is counted.
	const UnsortedCompositeReason reason =
			classify_unsorted_composite(has_work, GlobalSortAttemptOutcome::NOT_ATTEMPTED);
	CHECK(reason == UnsortedCompositeReason::SORTER_UNAVAILABLE);
	// i.e. the choke point acts on this frame. Post-#586-fix the action is a REJECT rather
	// than an increment-and-present; the reject decision itself is locked in its own case below.
	CHECK(reason != UnsortedCompositeReason::NONE);

	// Proof the OLD predicate would have missed exactly this frame: the superseded
	// `splat_count > 0` test is false here, so the old code took no branch at all.
	const uint32_t splat_count = 0;
	CHECK_FALSE(splat_count > 0);
}

// Every sort-dispatch outcome is classified, so no failure mode can be added to the
// renderer without the choke point deciding whether it produces wrong output.
TEST_CASE("[GaussianSplatting][SortFallback] every sort outcome is classified (#586)") {
	// Outcomes that still yield correctly ordered output.
	CHECK(classify_unsorted_composite(true, GlobalSortAttemptOutcome::SUBMITTED) ==
			UnsortedCompositeReason::NONE);
	CHECK(classify_unsorted_composite(true, GlobalSortAttemptOutcome::SYNC_FALLBACK_OK) ==
			UnsortedCompositeReason::NONE);

	// Outcomes that produce UNSORTED (incorrect) output — all must be counted.
	CHECK(classify_unsorted_composite(true, GlobalSortAttemptOutcome::NOT_ATTEMPTED) ==
			UnsortedCompositeReason::SORTER_UNAVAILABLE);
	CHECK(classify_unsorted_composite(true, GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED) ==
			UnsortedCompositeReason::SORT_DISPATCH_FAILED);
	CHECK(classify_unsorted_composite(true, GlobalSortAttemptOutcome::NOT_SUBMITTED) ==
			UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED);

	// With no translucent work nothing is composited, so no outcome is wrong output.
	for (GlobalSortAttemptOutcome outcome : {
				 GlobalSortAttemptOutcome::NOT_ATTEMPTED,
				 GlobalSortAttemptOutcome::SUBMITTED,
				 GlobalSortAttemptOutcome::SYNC_FALLBACK_OK,
				 GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED,
				 GlobalSortAttemptOutcome::NOT_SUBMITTED,
		 }) {
		CHECK(classify_unsorted_composite(false, outcome) == UnsortedCompositeReason::NONE);
	}

	// Every non-NONE reason has a distinct, non-empty name for the log/telemetry.
	CHECK(String(unsorted_composite_reason_name(UnsortedCompositeReason::SORTER_UNAVAILABLE)) !=
			String(unsorted_composite_reason_name(UnsortedCompositeReason::SORT_DISPATCH_FAILED)));
	CHECK(String(unsorted_composite_reason_name(UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED)).length() > 0);
}

// The sort-dispatch failure paths (sorter EXISTS but the sort does not land) were
// also uncounted before this change: they logged "rendering unsorted tiles" and
// returned, bumping nothing. Simulate a sustained failure and prove it is counted.
TEST_CASE("[GaussianSplatting][SortFallback] sort-dispatch failures are counted, not just logged (#586)") {
	struct Case {
		GlobalSortAttemptOutcome outcome;
		UnsortedCompositeReason expected;
	};
	const Case cases[] = {
		{ GlobalSortAttemptOutcome::NOT_ATTEMPTED, UnsortedCompositeReason::SORTER_UNAVAILABLE },
		{ GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED, UnsortedCompositeReason::SORT_DISPATCH_FAILED },
		{ GlobalSortAttemptOutcome::NOT_SUBMITTED, UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED },
	};

	for (const Case &c : cases) {
		uint64_t counter = 0;
		uint8_t last_reason = 0;
		// Mirrors the renderer choke point over 3 degraded frames, on the indirect path.
		for (int frame = 0; frame < 3; frame++) {
			const bool has_work = global_composite_has_translucent_work(false, 0, 0, true);
			const UnsortedCompositeReason reason = classify_unsorted_composite(has_work, c.outcome);
			if (reason != UnsortedCompositeReason::NONE) {
				counter++;
				last_reason = uint8_t(reason);
			}
		}
		CHECK_EQ(counter, 3u); // every wrong-output frame counted.
		CHECK_EQ(last_reason, uint8_t(c.expected)); // and attributed to the right cause.
	}

	// Control: a successful sort over the same frames counts zero.
	uint64_t ok_counter = 0;
	for (int frame = 0; frame < 3; frame++) {
		const bool has_work = global_composite_has_translucent_work(false, 0, 0, true);
		if (classify_unsorted_composite(has_work, GlobalSortAttemptOutcome::SUBMITTED) !=
				UnsortedCompositeReason::NONE) {
			ok_counter++;
		}
	}
	CHECK_EQ(ok_counter, 0u);
}

// #586 FIX. The correctness decision itself: which unsorted-composite reasons must make
// the renderer REJECT the frame instead of presenting incorrect alpha compositing.
//
// This locks BOTH directions, because either one alone is a bug:
//   * SORTER_UNAVAILABLE must be fatal        -> otherwise the wrong output still ships.
//   * the transient reasons must NOT be fatal -> otherwise a one-frame async blip on a
//     perfectly healthy desktop GPU turns into a dropped frame (a healthy-path regression).
TEST_CASE("[GaussianSplatting][SortFallback] the permanent sorter-unavailable cause rejects the frame, transient causes do not (#586)") {
	// The permanent, latched class: no sorter could be built at all.
	CHECK(unsorted_composite_must_reject_frame(UnsortedCompositeReason::SORTER_UNAVAILABLE));

	// Transient classes, reachable on capable hardware; presented, counted, recovered next frame.
	CHECK_FALSE(unsorted_composite_must_reject_frame(UnsortedCompositeReason::SORT_DISPATCH_FAILED));
	CHECK_FALSE(unsorted_composite_must_reject_frame(UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED));

	// A correctly sorted frame is never rejected — this is the healthy-path control, and it
	// is what keeps the fix from passing by rejecting everything.
	CHECK_FALSE(unsorted_composite_must_reject_frame(UnsortedCompositeReason::NONE));

	// End-to-end through the choke point, on the GPU-driven indirect path (splat_count == 0,
	// work described only by the indirect dispatch buffer) so the reject cannot be reached by
	// a stale CPU-side splat count either.
	const bool has_work = global_composite_has_translucent_work(
			/*allow_sync_readback=*/false, /*overlap_record_count=*/0,
			/*splat_count=*/0, /*has_instance_indirect=*/true);
	if (!has_work) {
		FAIL("premise: the indirect-work path must report translucent work");
		return;
	}
	CHECK(unsorted_composite_must_reject_frame(
			classify_unsorted_composite(has_work, GlobalSortAttemptOutcome::NOT_ATTEMPTED)));
	CHECK_FALSE(unsorted_composite_must_reject_frame(
			classify_unsorted_composite(has_work, GlobalSortAttemptOutcome::SUBMITTED)));
	CHECK_FALSE(unsorted_composite_must_reject_frame(
			classify_unsorted_composite(has_work, GlobalSortAttemptOutcome::SYNC_FALLBACK_OK)));

	// No translucent work => nothing is composited => no frame is ever rejected. A renderer
	// that rejected empty frames would black-screen an empty scene.
	for (GlobalSortAttemptOutcome outcome : {
				 GlobalSortAttemptOutcome::NOT_ATTEMPTED,
				 GlobalSortAttemptOutcome::SUBMITTED,
				 GlobalSortAttemptOutcome::SYNC_FALLBACK_OK,
				 GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED,
				 GlobalSortAttemptOutcome::NOT_SUBMITTED,
		 }) {
		CHECK_FALSE(unsorted_composite_must_reject_frame(classify_unsorted_composite(false, outcome)));
	}
}

// #586 FIX, counter split. The renderer keeps two distinct persistent counters and a frame
// must land in exactly one of them: rejected (nothing published) or presented-unsorted
// (wrong pixels shipped). Mirrors the choke-point bookkeeping so a future edit that folds
// them back together, or that double-counts a rejected frame as also-presented, fails here.
TEST_CASE("[GaussianSplatting][SortFallback] rejected frames and presented-unsorted frames are counted separately (#586)") {
	struct Case {
		GlobalSortAttemptOutcome outcome;
		uint64_t expect_rejected;
		uint64_t expect_presented_unsorted;
	};
	const Case cases[] = {
		{ GlobalSortAttemptOutcome::NOT_ATTEMPTED, 3u, 0u }, // permanent -> rejected, never presented
		{ GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED, 0u, 3u }, // transient -> presented, counted
		{ GlobalSortAttemptOutcome::NOT_SUBMITTED, 0u, 3u }, // transient -> presented, counted
		{ GlobalSortAttemptOutcome::SUBMITTED, 0u, 0u }, // healthy control -> neither
		{ GlobalSortAttemptOutcome::SYNC_FALLBACK_OK, 0u, 0u }, // healthy control -> neither
	};

	for (const Case &c : cases) {
		uint64_t rejected = 0;
		uint64_t presented_unsorted = 0;
		uint8_t last_reject_reason = 0;
		for (int frame = 0; frame < 3; frame++) {
			const bool has_work = global_composite_has_translucent_work(false, 0, 0, true);
			const UnsortedCompositeReason reason = classify_unsorted_composite(has_work, c.outcome);
			if (unsorted_composite_must_reject_frame(reason)) {
				rejected++;
				last_reject_reason = uint8_t(reason);
				continue; // the renderer returns here; the frame is NOT presented.
			}
			if (reason != UnsortedCompositeReason::NONE) {
				presented_unsorted++;
			}
		}
		CHECK_EQ(rejected, c.expect_rejected);
		CHECK_EQ(presented_unsorted, c.expect_presented_unsorted);
		if (c.expect_rejected > 0) {
			CHECK_EQ(last_reject_reason, uint8_t(UnsortedCompositeReason::SORTER_UNAVAILABLE));
		}
	}
}

// Round-1 review of #586: making SORTER_UNAVAILABLE reject the frame converted one
// permanent failure mode (wrong pixels forever) into another (no pixels forever),
// because disable_sorter() latched for EVERY recreation failure — including a transient
// buffer allocation that failed while a capacity growth had already shut the working
// sorter down. This locks the classification that makes the latch recoverable exactly
// when the cause can recover, and NOT otherwise.
TEST_CASE("[GaussianSplatting][SortFallback] permanent capability failure latches, transient recreation failure does not (#586 round-1)") {
	// Capability: RadixSort::is_supported() is a device-limits + static-config query
	// that allocates nothing, so its answer cannot change. Latch it.
	CHECK(sorter_creation_failure_is_permanent(SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED));
	// supports_indirect() is a compile-time constant of the sorter class. Latch it.
	CHECK(sorter_creation_failure_is_permanent(SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT));

	// THE FINDING: creation failure is an allocation/initialization failure at this call
	// site (the capability probe has already passed), so it must be retryable. If this
	// flips to true, one transient VRAM blip black-screens a capable GPU for the session.
	CHECK_FALSE(sorter_creation_failure_is_permanent(SorterCreationFailure::CREATION_FAILED));
	// #586 round-7: and its deterministic sibling is the opposite. A shader/pipeline that will
	// not build does not build on the next attempt either, so retrying it is a recompile loop
	// that can never succeed.
	CHECK(sorter_creation_failure_is_permanent(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC));

	// Both directions must be represented, otherwise a classifier that answers a constant
	// would satisfy the case above.
	bool saw_permanent = false;
	bool saw_transient = false;
	for (SorterCreationFailure failure : {
				 SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED,
				 SorterCreationFailure::CREATION_FAILED,
				 SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT,
				 SorterCreationFailure::CREATION_FAILED_DETERMINISTIC,
		 }) {
		if (sorter_creation_failure_is_permanent(failure)) {
			saw_permanent = true;
		} else {
			saw_transient = true;
		}
	}
	CHECK(saw_permanent);
	CHECK(saw_transient);
}

// The retry schedule has to dodge two traps at once: a per-frame allocation storm on a
// device that genuinely cannot allocate, and an attempt cap that re-creates the permanent
// latch it was meant to remove. The contract is "always eventually retries, never more
// often than the saturated backoff".
TEST_CASE("[GaussianSplatting][SortFallback] transient-failure retry backs off but never gives up (#586 round-1)") {
	// First failure schedules the soonest possible retry: a one-off blip costs one frame.
	CHECK_EQ(next_sorter_retry_delay_calls(0), SORTER_RETRY_MIN_DELAY_CALLS);
	CHECK(SORTER_RETRY_MIN_DELAY_CALLS >= 1u); // a zero delay would be a same-call retry loop.

	// It doubles...
	CHECK_EQ(next_sorter_retry_delay_calls(1), 2u);
	CHECK_EQ(next_sorter_retry_delay_calls(2), 4u);
	CHECK_EQ(next_sorter_retry_delay_calls(64), 128u);

	// ...and saturates rather than overflowing or growing without bound.
	CHECK_EQ(next_sorter_retry_delay_calls(256), SORTER_RETRY_MAX_DELAY_CALLS);
	CHECK_EQ(next_sorter_retry_delay_calls(SORTER_RETRY_MAX_DELAY_CALLS), SORTER_RETRY_MAX_DELAY_CALLS);
	CHECK_EQ(next_sorter_retry_delay_calls(UINT32_MAX), SORTER_RETRY_MAX_DELAY_CALLS);

	// NEVER-GIVES-UP: whatever the failure history, the next delay is finite and non-zero,
	// so a recovery (VRAM freed, or demand shrinking back) is always eventually retried.
	uint32_t delay = 0;
	for (int failure = 0; failure < 64; failure++) {
		delay = next_sorter_retry_delay_calls(delay);
		CHECK(delay >= SORTER_RETRY_MIN_DELAY_CALLS);
		CHECK(delay <= SORTER_RETRY_MAX_DELAY_CALLS);
	}
	CHECK_EQ(delay, SORTER_RETRY_MAX_DELAY_CALLS); // sustained failure settles at the cap.
}

// End-to-end simulation of ensure_resources()'s countdown, both directions. This is the
// CPU-side mirror of the on-GPU case; it proves the schedule as a state machine rather
// than as three isolated function calls.
TEST_CASE("[GaussianSplatting][SortFallback] retry state machine recovers transient failures and leaves permanent ones latched (#586 round-1)") {
	struct Sim {
		bool available = true;
		bool permanent = false;
		SorterCreationFailure cause = SorterCreationFailure::CREATION_FAILED;
		uint32_t delay = 0;
		uint32_t countdown = 0;
		uint32_t creation_attempts = 0;
		uint32_t probe_calls = 0;
		uint32_t rejected_calls = 0;
		// Models g_gpu_sorting_config as seen by probe_supports_indirect(): the capability
		// answer is a function of LIVE configuration (radix_bits x workgroup_size vs the
		// device's compute limits), so the user can change it while the renderer runs.
		bool config_supported = true;

		// Mirrors GPUSorterFactory::probe_supports_indirect(ALGORITHM_RADIX, device):
		// allocation-free, answers from live config + cached device limits.
		bool probe() {
			probe_calls++;
			return config_supported;
		}

		// Mirrors disable_sorter().
		void fail(SorterCreationFailure p_failure) {
			available = false;
			cause = p_failure;
			permanent = sorter_creation_failure_is_permanent(p_failure);
			if (permanent) {
				delay = 0;
				countdown = 0;
			} else {
				delay = next_sorter_retry_delay_calls(delay);
				countdown = delay;
			}
		}

		// Mirrors one ensure_resources() call. p_device_healthy models whether a retried
		// create_sorter() would now succeed.
		void tick(bool p_device_healthy) {
			// Mirrors the round-2 re-probe block: a permanent latch whose cause is decided
			// by the (free) capability probe is re-evaluated, so correcting the sorting
			// configuration lifts it. Causes that are not probe-decided stay latched.
			if (!available && permanent && sorter_permanent_failure_is_reprobable(cause) && probe()) {
				permanent = false;
				countdown = 0; // retry on THIS call; delay stays monotone.
			}
			bool retry_due = false;
			if (!available && !permanent) {
				if (countdown > 0) {
					countdown--;
				}
				retry_due = (countdown == 0);
			}
			if (!available && retry_due) {
				// Mirrors the recreation branch: the capability probe GATES create_sorter(),
				// so a still-unsupported configuration costs a probe, never an allocation.
				if (!probe()) {
					fail(SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED);
				} else {
					creation_attempts++;
					if (p_device_healthy) {
						available = true;
						permanent = false;
						cause = SorterCreationFailure::CREATION_FAILED;
						countdown = 0; // delay deliberately kept (monotone).
						return;
					}
					fail(SorterCreationFailure::CREATION_FAILED);
				}
			}
			if (!available) {
				rejected_calls++; // #586: the frame publishes nothing.
			}
		}
	};

	SUBCASE("transient failure on a device that recovers resumes publishing") {
		Sim sim;
		sim.fail(SorterCreationFailure::CREATION_FAILED);
		CHECK_FALSE(sim.available); // the failing call itself rejects.
		for (int call = 0; call < 32; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK(sim.available); // THE FIX: it comes back.
		CHECK_EQ(sim.creation_attempts, 1u); // and does not keep re-creating once healthy.
		CHECK_EQ(sim.rejected_calls, 0u); // recovery landed on the very first retry.
	}

	SUBCASE("transient failure on a device that stays broken retries forever but rarely") {
		Sim sim;
		sim.fail(SorterCreationFailure::CREATION_FAILED);
		const uint32_t calls = SORTER_RETRY_MAX_DELAY_CALLS * 8u;
		for (uint32_t call = 0; call < calls; call++) {
			sim.tick(/*p_device_healthy=*/false);
		}
		CHECK_FALSE(sim.available);
		// Never gives up...
		CHECK(sim.creation_attempts > 1u);
		// ...but is nowhere near per-call: the backoff saturates, so attempts are bounded
		// by roughly calls / MAX_DELAY plus the short doubling ramp.
		CHECK(sim.creation_attempts < calls / 8u);
		CHECK_EQ(sim.delay, SORTER_RETRY_MAX_DELAY_CALLS);
		// A recovery arriving late still lands.
		sim.countdown = 0;
		sim.tick(/*p_device_healthy=*/true);
		CHECK(sim.available);
	}

	SUBCASE("permanent capability failure never retries and never publishes") {
		for (SorterCreationFailure failure : {
					 SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED,
					 SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT,
			 }) {
			Sim sim;
			// Keep the modelled state COHERENT: an indirect-capability failure can only be
			// produced by a probe that answers false, so the configuration must be one the
			// device cannot run. CREATED_SORTER_LACKS_INDIRECT is the opposite case — the
			// probe passes and the built sorter still has no indirect entry point — so its
			// config stays supported, which is exactly what makes it the discriminator for
			// "re-probing must not lift THIS latch".
			sim.config_supported = (failure != SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED);
			sim.fail(failure);
			const uint32_t calls = SORTER_RETRY_MAX_DELAY_CALLS * 8u;
			for (uint32_t call = 0; call < calls; call++) {
				// Even with a "healthy" device the permanent latch must not rebuild while its
				// cause still holds: #586's protection must hold.
				sim.tick(/*p_device_healthy=*/true);
			}
			CHECK_FALSE(sim.available);
			CHECK_EQ(sim.creation_attempts, 0u); // no allocation storm.
			CHECK_EQ(sim.rejected_calls, calls); // #586 protection intact: nothing published.
		}
	}

	// #586 ROUND-7 REVIEW (P2). THE FINDING, as a state machine. Round 2 classified every
	// invalid Ref from create_sorter() as CREATION_FAILED, i.e. transient, arguing that once
	// the capability probe passes the only remaining causes are VRAM/capacity ones. But
	// RadixSort::create_variant() compiles four shaders and builds four compute pipelines, and
	// a device can pass the compute-limit probe and still reject the generated radix GLSL.
	// That failure reproduces on every attempt, so the "never gives up" backoff turned into an
	// unbounded recompile loop: a full create_sorter() every saturated-backoff interval,
	// forever, on a renderer that is rejecting every translucent frame regardless.
	//
	// config_supported stays TRUE here, and that is the entire point — this is the state Codex
	// described, a device that PASSES the probe and fails afterwards. It also makes the subcase
	// discriminating against the re-probe: if this cause were made re-probable, tick() would
	// lift the latch on the first call and creation_attempts would climb.
	SUBCASE("a deterministic creation failure never retries, even though the capability probe still passes (#586 round-7)") {
		Sim sim;
		sim.config_supported = true;
		sim.fail(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC);
		const uint32_t calls = SORTER_RETRY_MAX_DELAY_CALLS * 8u;
		for (uint32_t call = 0; call < calls; call++) {
			// "Healthy" device: this must NOT be what decides it. The cause is deterministic
			// for this device and configuration, so nothing about the passage of time or the
			// device's allocation health can make the shader compile.
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK_FALSE(sim.available);
		// THE DISCRIMINATING ASSERTION. Pre-round-7 this cause did not exist and the same
		// failure arrived as CREATION_FAILED, which the "transient failure on a device that
		// stays broken" subcase above shows produces creation_attempts > 1 over this many
		// calls — one full shader recompile + buffer allocation cycle each.
		CHECK_EQ(sim.creation_attempts, 0u);
		// And it must not be spending probe calls on a question that cannot change either.
		CHECK_EQ(sim.probe_calls, 0u);
		CHECK_EQ(sim.rejected_calls, calls); // #586 protection intact: nothing published.
	}

	// THE LOAD-BEARING CONTROL for the subcase above, stated as its own scenario rather than
	// left implicit: the round-7 latch must not swallow the transient case that rounds 1-2
	// exist to keep recoverable. Same Sim, same call count, same "healthy device" — only the
	// CAUSE differs — so a fix that latched everything (or a classifier that answered a
	// constant) fails here while passing the subcase above.
	SUBCASE("the round-7 latch does not swallow a transient failure on the same device (#586 round-7 control)") {
		Sim deterministic;
		deterministic.fail(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC);
		Sim transient;
		transient.fail(SorterCreationFailure::CREATION_FAILED);
		const uint32_t calls = SORTER_RETRY_MAX_DELAY_CALLS * 8u;
		for (uint32_t call = 0; call < calls; call++) {
			deterministic.tick(/*p_device_healthy=*/true);
			transient.tick(/*p_device_healthy=*/true);
		}
		CHECK_FALSE(deterministic.available);
		CHECK(transient.available); // recovers, exactly as before round 7.
		CHECK_EQ(transient.creation_attempts, 1u);
		CHECK_EQ(transient.rejected_calls, 0u);
	}
}

// #586 ROUND-7 REVIEW (P2). "Stop retrying deterministic sorter creation failures."
//
// The retry policy is only as good as the classification feeding it, and before this round
// the classification had nothing to feed on: GPUSorterFactory::create_sorter() discarded
// RadixSort::initialize()'s Error and returned a bare invalid Ref, so the call site could not
// tell an out-of-VRAM histogram buffer from a scatter shader the driver refuses to compile and
// called both transient.
//
// The mapping below is read from the callees (gpu_sorter.cpp), not inferred from names — the
// error-code contract above RadixSort::initialize() documents the same table from the other
// side. Both directions are asserted because each failure mode is a real regression:
// classifying an allocation failure as deterministic re-creates the permanent black screen
// round 1 removed, and classifying a shader failure as transient is the recompile loop round 7
// removes.
TEST_CASE("[GaussianSplatting][SortFallback] the preserved init error separates allocation failures from deterministic ones (#586 round-7)") {
	// --- RETRYABLE: the resource-shaped errors. ---
	// storage_buffer_create() returned an invalid RID under momentary VRAM pressure, or
	// _init_sorter_devices() has not seen a GaussianSplatManager yet. Both can differ on the
	// next attempt with nothing else changing.
	CHECK_EQ(uint8_t(classify_sorter_creation_error(ERR_CANT_CREATE)), uint8_t(SorterCreationFailure::CREATION_FAILED));
	CHECK_EQ(uint8_t(classify_sorter_creation_error(ERR_OUT_OF_MEMORY)), uint8_t(SorterCreationFailure::CREATION_FAILED));
	// The >UINT32_MAX size refusals are a function of the REQUESTED CAPACITY, and capacity
	// shrinks back (the bounded-shrink path recreates at measured demand).
	CHECK_EQ(uint8_t(classify_sorter_creation_error(ERR_INVALID_PARAMETER)), uint8_t(SorterCreationFailure::CREATION_FAILED));
	// ...and being retryable is what they are FOR: this is the round-1 contract, restated on
	// the new path so the round-7 change cannot quietly revoke it.
	CHECK_FALSE(sorter_creation_failure_is_permanent(classify_sorter_creation_error(ERR_CANT_CREATE)));

	// --- DETERMINISTIC: the errors that reproduce on every attempt. ---
	// THE FINDING. create_compute_shader_from_spirv() / compute_pipeline_create() failing is a
	// property of (generated GLSL, device, driver). Retrying recompiles the same source on the
	// same device and gets the same answer.
	CHECK_EQ(uint8_t(classify_sorter_creation_error(ERR_COMPILATION_FAILED)),
			uint8_t(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC));
	// A capability answer (device_supports_workgroup, and create_sorter()'s pre-instantiation
	// policy rejects). Reads limits and static config; allocates nothing.
	CHECK_EQ(uint8_t(classify_sorter_creation_error(ERR_UNAVAILABLE)),
			uint8_t(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC));
	CHECK(sorter_creation_failure_is_permanent(classify_sorter_creation_error(ERR_COMPILATION_FAILED)));
	// ...and NOT re-probable, which is not a nicety: create_sorter() is only reached after the
	// capability probe passes, so a re-probable deterministic latch would be lifted on the very
	// next call and the loop would be back, just routed through the round-2 escape hatch.
	CHECK_FALSE(sorter_permanent_failure_is_reprobable(classify_sorter_creation_error(ERR_COMPILATION_FAILED)));

	// --- FAIL-CLOSED on anything unrecognised. ---
	// The switch is over Godot's whole Error enum, so it cannot get the "a new enumerator must
	// choose" property from omitting a default. It latches instead. OK is included on purpose:
	// reaching the classifier at all means the sorter came back invalid, and an OK alongside
	// that is a contract violation, not a reason to retry forever.
	for (Error unrecognised : { OK, FAILED, ERR_UNCONFIGURED, ERR_BUSY, ERR_TIMEOUT, ERR_BUG }) {
		CHECK_EQ(uint8_t(classify_sorter_creation_error(unrecognised)),
				uint8_t(SorterCreationFailure::CREATION_FAILED_DETERMINISTIC));
	}

	// The classifier must not be a constant function — without this, "everything latches"
	// satisfies every deterministic assertion above.
	bool saw_retryable = false;
	bool saw_deterministic = false;
	for (Error err : { OK, FAILED, ERR_UNAVAILABLE, ERR_CANT_CREATE, ERR_OUT_OF_MEMORY,
				 ERR_INVALID_PARAMETER, ERR_COMPILATION_FAILED, ERR_UNCONFIGURED }) {
		if (sorter_creation_failure_is_permanent(classify_sorter_creation_error(err))) {
			saw_deterministic = true;
		} else {
			saw_retryable = true;
		}
	}
	CHECK(saw_retryable);
	CHECK(saw_deterministic);
}

// #586 ROUND-2 REVIEW (P2). "Permanent" was permanent for the PROCESS, not for the
// configuration that produced it — and one of the two permanent causes is decided by a
// query over live project settings. A user on a GPU where a valid-but-nonportable radix
// configuration makes probe_supports_indirect() answer false could therefore CORRECT the
// setting and never get rendering back: reload_gpu_sorting_config_from_project_settings()
// rebuilds only the instance sorter and never touches TileGlobalSortResources, so every
// translucent frame stayed rejected until renderer teardown.
//
// This locks the classification that makes the latch liftable exactly when its cause can
// be re-evaluated for free, and NOT otherwise. Both directions matter: a latch that any
// change lifts is not a latch, and a latch nothing lifts is the dead end.
TEST_CASE("[GaussianSplatting][SortFallback] a permanent latch is re-probable only when live config decides it (#586 round-2)") {
	// THE FINDING: this cause IS probe_supports_indirect(), which reads
	// g_gpu_sorting_config.radix_bits / .workgroup_size. Its answer changes when the user
	// changes those, so it must be re-asked.
	CHECK(sorter_permanent_failure_is_reprobable(SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED));

	// ...but this one is a compile-time constant of the sorter class, and re-testing it
	// costs a full create_sorter(). Re-probing it per call would be the allocation storm
	// the permanent latch exists to prevent. It stays hard-latched.
	CHECK_FALSE(sorter_permanent_failure_is_reprobable(SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT));

	// The transient cause is not "re-probable": it is already recovered by the retry
	// backoff, which re-attempts CREATION rather than re-asking the capability question.
	// Answering true here would run both recovery mechanisms over one state.
	CHECK_FALSE(sorter_permanent_failure_is_reprobable(SorterCreationFailure::CREATION_FAILED));

	// Structural invariants, so neither predicate can degenerate into a constant and so a
	// newly added cause cannot silently inherit a classification.
	uint32_t permanent_count = 0;
	uint32_t reprobable_count = 0;
	for (SorterCreationFailure failure : {
				 SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED,
				 SorterCreationFailure::CREATION_FAILED,
				 SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT,
		 }) {
		if (sorter_creation_failure_is_permanent(failure)) {
			permanent_count++;
		}
		if (sorter_permanent_failure_is_reprobable(failure)) {
			reprobable_count++;
			// Re-probing is only ever consulted for a PERMANENT cause; a transient one is
			// already retried. A re-probable-but-transient cause would double-recover.
			CHECK(sorter_creation_failure_is_permanent(failure));
		}
	}
	CHECK_EQ(permanent_count, 2u);
	CHECK_EQ(reprobable_count, 1u); // exactly one: the config-decided capability probe.
}

// End-to-end state machine for the round-2 finding, mirroring ensure_resources()'s
// re-probe block. Every direction lives in this one case so it cannot pass by lifting
// every latch or by lifting none.
TEST_CASE("[GaussianSplatting][SortFallback] a corrected sorting configuration lifts the capability latch, a still-bad one does not (#586 round-2)") {
	struct Sim {
		bool available = true;
		bool permanent = false;
		SorterCreationFailure cause = SorterCreationFailure::CREATION_FAILED;
		uint32_t delay = 0;
		uint32_t countdown = 0;
		uint32_t creation_attempts = 0;
		uint32_t probe_calls = 0;
		uint32_t rejected_calls = 0;
		bool config_supported = true;

		bool probe() {
			probe_calls++;
			return config_supported;
		}

		void fail(SorterCreationFailure p_failure) {
			available = false;
			cause = p_failure;
			permanent = sorter_creation_failure_is_permanent(p_failure);
			if (permanent) {
				delay = 0;
				countdown = 0;
			} else {
				delay = next_sorter_retry_delay_calls(delay);
				countdown = delay;
			}
		}

		void tick(bool p_device_healthy) {
			if (!available && permanent && sorter_permanent_failure_is_reprobable(cause) && probe()) {
				permanent = false;
				countdown = 0;
			}
			bool retry_due = false;
			if (!available && !permanent) {
				if (countdown > 0) {
					countdown--;
				}
				retry_due = (countdown == 0);
			}
			if (!available && retry_due) {
				if (!probe()) {
					fail(SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED);
				} else {
					creation_attempts++;
					if (p_device_healthy) {
						available = true;
						permanent = false;
						cause = SorterCreationFailure::CREATION_FAILED;
						countdown = 0;
						return;
					}
					fail(SorterCreationFailure::CREATION_FAILED);
				}
			}
			if (!available) {
				rejected_calls++;
			}
		}

		// The user hits a nonportable radix/workgroup pair: the probe answers false and the
		// next recreation latches the sorter off.
		void latch_on_unsupported_config() {
			config_supported = false;
			fail(SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED);
		}
	};

	SUBCASE("correcting the setting resumes publishing") {
		Sim sim;
		sim.latch_on_unsupported_config();

		// While the setting is still wrong: rejected forever, and never an allocation.
		for (int call = 0; call < 64; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		if (sim.available) {
			FAIL("premise: an unsupported sorting configuration must keep the sorter latched off");
			return;
		}
		CHECK_EQ(sim.creation_attempts, 0u); // the probe gates creation: no storm.
		CHECK_EQ(sim.rejected_calls, 64u); // #586 protection intact: nothing published.

		// THE FIX: the user corrects radix_bits / workgroup_size.
		sim.config_supported = true;
		sim.tick(/*p_device_healthy=*/true);

		CHECK(sim.available); // rendering comes back...
		CHECK_FALSE(sim.permanent);
		CHECK_EQ(sim.creation_attempts, 1u); // ...on the very first call after the change.
		CHECK_EQ(sim.rejected_calls, 64u); // and that call published.

		// Steady state afterwards: no further probing storm, no further rebuilds.
		const uint32_t attempts_after_recovery = sim.creation_attempts;
		const uint32_t probes_after_recovery = sim.probe_calls;
		for (int call = 0; call < 16; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK(sim.available);
		CHECK_EQ(sim.creation_attempts, attempts_after_recovery);
		CHECK_EQ(sim.probe_calls, probes_after_recovery); // healthy path does not re-probe at all.
		CHECK_EQ(sim.rejected_calls, 64u);
	}

	SUBCASE("a different but still-unsupported setting must NOT lift the latch") {
		// The control that stops "clear the latch on any settings change" from passing: a
		// device that genuinely cannot run the configured sort must keep rejecting.
		Sim sim;
		sim.latch_on_unsupported_config();
		for (int call = 0; call < 32; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		// The user edits the setting again — to another value this device cannot run.
		// (config_supported stays false: that is what "still unsupported" means.)
		for (int call = 0; call < 32; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK_FALSE(sim.available); // still rejecting, correctly.
		CHECK(sim.permanent);
		CHECK_EQ(sim.creation_attempts, 0u); // and still never allocating.
		CHECK_EQ(sim.rejected_calls, 64u);
	}

	SUBCASE("a non-probe permanent cause stays latched however the configuration changes") {
		Sim sim;
		// The probe passes; the sorter that got built has no indirect entry point. No
		// configuration edit can change that, and re-testing it costs a create_sorter().
		sim.config_supported = true;
		sim.fail(SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT);
		for (int call = 0; call < 128; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK_FALSE(sim.available);
		CHECK(sim.permanent);
		CHECK_EQ(sim.creation_attempts, 0u); // no per-call create_sorter() storm.
		CHECK_EQ(sim.probe_calls, 0u); // not even a probe: the cause is not probe-decided.
		CHECK_EQ(sim.rejected_calls, 128u);
	}

	SUBCASE("the transient backoff is not short-circuited by the re-probe") {
		// A transient failure must keep its backoff: the re-probe block must not fire for
		// it (that would be a per-call retry, the storm the backoff prevents).
		Sim sim;
		sim.fail(SorterCreationFailure::CREATION_FAILED);
		sim.delay = SORTER_RETRY_MAX_DELAY_CALLS;
		sim.countdown = SORTER_RETRY_MAX_DELAY_CALLS;
		for (uint32_t call = 0; call < SORTER_RETRY_MAX_DELAY_CALLS - 1u; call++) {
			sim.tick(/*p_device_healthy=*/true);
		}
		CHECK_FALSE(sim.available); // still waiting out the backoff.
		CHECK_EQ(sim.creation_attempts, 0u);
		sim.tick(/*p_device_healthy=*/true);
		CHECK(sim.available); // and it does arrive.
		CHECK_EQ(sim.creation_attempts, 1u);
	}
}

// #586 round-3. The global-composite path abandons a frame at TEN exits, not one. Nine of
// them are before the choke point (the pre-binning resource check, four uniform-set
// acquisitions, two tile-range builds, the zero-effective-capacity check) and every one of
// them makes render() return an invalid RID — a real reject. Before round 3 they carried no
// reason at all, so the reject counter and its last-reason field stayed at zero while the
// renderer published nothing. These two enumerators are what makes such an exit attributable.
//
// The load-bearing property locked here is that they are FATAL: nothing may treat "we could
// not even bin this frame" as a presentable outcome.
TEST_CASE("[GaussianSplatting][SortFallback] pre-outcome global-sort failures are fatal and attributable (#586 round-3)") {
	CHECK(unsorted_composite_must_reject_frame(UnsortedCompositeReason::GLOBAL_SORT_RESOURCES_UNAVAILABLE));
	CHECK(unsorted_composite_must_reject_frame(UnsortedCompositeReason::GLOBAL_SORT_SETUP_FAILED));

	// They are NOT classifier outputs: classify_unsorted_composite() maps (work, outcome)
	// pairs, and these describe frames abandoned before an outcome exists. If a future edit
	// made the classifier return one, the choke point would be reporting a pre-binning exit
	// for a frame that reached the sort dispatch — a lie about where the frame died.
	for (bool has_work : { false, true }) {
		for (GlobalSortAttemptOutcome outcome : {
					 GlobalSortAttemptOutcome::NOT_ATTEMPTED,
					 GlobalSortAttemptOutcome::SUBMITTED,
					 GlobalSortAttemptOutcome::SYNC_FALLBACK_OK,
					 GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED,
					 GlobalSortAttemptOutcome::NOT_SUBMITTED,
			 }) {
			const UnsortedCompositeReason reason = classify_unsorted_composite(has_work, outcome);
			CHECK(reason != UnsortedCompositeReason::GLOBAL_SORT_RESOURCES_UNAVAILABLE);
			CHECK(reason != UnsortedCompositeReason::GLOBAL_SORT_SETUP_FAILED);
		}
	}

	// Every reason must name itself. Totality of the switch is enforced by the compiler (no
	// default label => -Wswitch on a new enumerator); what a compiler cannot catch is a case
	// that falls through to a name already used by another reason, which would make telemetry
	// unable to tell two different degradations apart.
	const UnsortedCompositeReason all_reasons[] = {
		UnsortedCompositeReason::NONE,
		UnsortedCompositeReason::SORTER_UNAVAILABLE,
		UnsortedCompositeReason::SORT_DISPATCH_FAILED,
		UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED,
		UnsortedCompositeReason::GLOBAL_SORT_RESOURCES_UNAVAILABLE,
		UnsortedCompositeReason::GLOBAL_SORT_SETUP_FAILED,
	};
	for (size_t i = 0; i < sizeof(all_reasons) / sizeof(all_reasons[0]); i++) {
		const char *name_i = unsorted_composite_reason_name(all_reasons[i]);
		CHECK(strcmp(name_i, "unknown") != 0);
		for (size_t j = i + 1; j < sizeof(all_reasons) / sizeof(all_reasons[0]); j++) {
			CHECK(strcmp(name_i, unsorted_composite_reason_name(all_reasons[j])) != 0);
		}
	}
	// The list above is complete for the enum as it stands; an added enumerator that is not
	// appended here is caught by this, because the reason_name switch would still name it and
	// the guard below would find an unnamed successor value.
	CHECK(strcmp(unsorted_composite_reason_name(
						  static_cast<UnsortedCompositeReason>(uint8_t(UnsortedCompositeReason::GLOBAL_SORT_SETUP_FAILED) + 1u)),
				  "unknown") == 0);
}

// #586 round-3. Reject accounting moved to the ONE boundary where render() can return an
// invalid RID, and FrameRejectStage is how a reject is attributed there. An operator asking
// "why is nothing being drawn" gets a stage without log scraping — which matters because the
// individual failure sites use ERR_PRINT_ONCE, i.e. one line per process, while the
// degradation can persist for the rest of the session.
TEST_CASE("[GaussianSplatting][SortFallback] every frame-reject stage is distinctly named (#586 round-3)") {
	const FrameRejectStage all_stages[] = {
		FrameRejectStage::NONE,
		FrameRejectStage::SETTINGS,
		FrameRejectStage::VIEWPORT_RESOURCES,
		FrameRejectStage::PARAMS,
		FrameRejectStage::GLOBAL_SORT,
		FrameRejectStage::RASTER_PATH,
		// #586 round-7: the resource-device precondition. It used to reject from
		// TileRenderer::render() itself, before the executor existed, so it had no stage at all.
		FrameRejectStage::DEVICE,
	};
	for (size_t i = 0; i < sizeof(all_stages) / sizeof(all_stages[0]); i++) {
		const char *name_i = frame_reject_stage_name(all_stages[i]);
		CHECK(strcmp(name_i, "unknown") != 0);
		for (size_t j = i + 1; j < sizeof(all_stages) / sizeof(all_stages[0]); j++) {
			CHECK(strcmp(name_i, frame_reject_stage_name(all_stages[j])) != 0);
		}
	}
	CHECK(strcmp(frame_reject_stage_name(
						  static_cast<FrameRejectStage>(uint8_t(FrameRejectStage::DEVICE) + 1u)),
				  "unknown") == 0);
	// NONE is reserved for "the frame was published", so it must be the zero value: telemetry
	// reads last_reject_stage as a raw uint8_t and a freshly constructed metrics struct must
	// not claim a stage.
	CHECK_EQ(uint8_t(FrameRejectStage::NONE), uint8_t(0));
}

} // namespace TestGaussianSplatting
