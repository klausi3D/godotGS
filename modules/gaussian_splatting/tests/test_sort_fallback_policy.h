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

} // namespace TestGaussianSplatting
