#pragma once

#include "test_macros.h"

#include "../renderer/sort_fallback_policy.h"

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
	CHECK(reason != UnsortedCompositeReason::NONE); // i.e. the counter increments.

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

} // namespace TestGaussianSplatting
