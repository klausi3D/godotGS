#ifndef GAUSSIAN_SORT_FALLBACK_POLICY_H
#define GAUSSIAN_SORT_FALLBACK_POLICY_H

#include <stdint.h>

namespace GaussianSplatting {

enum class SortFallbackAction : uint8_t {
	REUSE_PREVIOUS_SORT = 0,
	RUN_CPU_SORT = 1,
	FAIL = 2,
};

enum class SortFallbackScenario : uint8_t {
	FORCE_CPU_OVERRIDE = 0,
	SORTER_UNAVAILABLE = 1,
	GPU_SORT_FAILED = 2,
};

struct SortFallbackPolicyDecision {
	SortFallbackAction actions[4] = {
		SortFallbackAction::FAIL,
		SortFallbackAction::FAIL,
		SortFallbackAction::FAIL,
		SortFallbackAction::FAIL,
	};
	uint32_t action_count = 0;
	bool cpu_sort_forced = false;
};

// CPU fallback for the global sort domain is still the correctness-preserving
// path when positions are available. Strict mode hard-fails the case where the
// CPU fallback would have to publish unsorted cull order because positions are
// not yet produced.
static inline bool allow_unsorted_cpu_fallback_in_orchestrator(bool p_strict_global_sort, bool p_positions_ready) {
	return p_positions_ready || !p_strict_global_sort;
}

// ---------------------------------------------------------------------------
// Unsorted global-composite fallback — observability contract (#586)
// ---------------------------------------------------------------------------
//
// The global-composite path can end up rasterizing tiles in UNSORTED order, which
// is WRONG output: alpha compositing is order-dependent, so an unsorted draw is
// not "slightly off", it is incorrect. #586 tracks FIXING that. The helpers below
// only make the wrong-output frames COUNTABLE — observability, not correctness.
//
// DESIGN RULE (this is the point of the refactor): the "is this frame producing
// unsorted output?" decision is derived from exactly TWO pure inputs computed once
// per frame — `global_composite_has_translucent_work()` and a
// `GlobalSortAttemptOutcome` — and classified at ONE choke point by
// `classify_unsorted_composite()`. Instrumentation must NEVER be re-derived at the
// individual failure sites: an earlier revision did that and the counter's copy of
// the work predicate silently omitted the indirect-work term, so GPU-driven frames
// (CPU splat_count == 0, real work described by the instance indirect-dispatch
// buffer) rasterized unsorted WITHOUT being counted. An under-reporting counter is
// worse than no counter — it manufactures confidence that the wrong-output path is
// rare. Adding a new sort-failure mode must mean adding a `GlobalSortAttemptOutcome`
// value, which this switch then forces you to classify.

// Why the sort did not produce a correctly-ordered result for this frame.
enum class UnsortedCompositeReason : uint8_t {
	NONE = 0, // Output is correctly sorted (or there was nothing to composite).
	SORTER_UNAVAILABLE = 1, // Capability-gated: no sorter could be built at all.
	SORT_DISPATCH_FAILED = 2, // Sorter exists; the sync fallback dispatch returned an error.
	ASYNC_SORT_NOT_SUBMITTED = 3, // Async submit returned timeline=0 and sync fallback is disallowed.
};

static inline const char *unsorted_composite_reason_name(UnsortedCompositeReason p_reason) {
	switch (p_reason) {
		case UnsortedCompositeReason::NONE:
			return "none";
		case UnsortedCompositeReason::SORTER_UNAVAILABLE:
			return "sorter unavailable (capability-gated)";
		case UnsortedCompositeReason::SORT_DISPATCH_FAILED:
			return "sync sort dispatch failed";
		case UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED:
			return "async sort not submitted (timeline=0, sync fallback disallowed)";
	}
	return "unknown";
}

// What actually happened at the sort-dispatch site this frame.
enum class GlobalSortAttemptOutcome : uint8_t {
	NOT_ATTEMPTED = 0, // No sort dispatched (sorter invalid, or no work).
	SUBMITTED = 1, // Async indirect sort submitted (timeline != 0) -> ordered output.
	SYNC_FALLBACK_OK = 2, // Async returned 0; sync fallback ran and succeeded -> ordered output.
	SYNC_FALLBACK_FAILED = 3, // Async returned 0; sync fallback ran and failed -> UNSORTED.
	NOT_SUBMITTED = 4, // Async returned 0 and sync fallback disallowed -> UNSORTED.
};

// THE single definition of "this frame has translucent content the global-composite
// path will rasterize". The sort gate and the unsorted-output counter MUST both be
// derived from this one call — never from a hand-copied expression.
//
// In sync-readback mode the CPU has a fresh overlap-record count, so that is
// authoritative. In async (GPU-driven) mode the CPU count is stale by design, and
// work may be described ONLY by the instance indirect-dispatch buffer, with
// CPU-side splat_count reading 0. Dropping that `p_has_instance_indirect` term is
// exactly the under-reporting bug this function exists to prevent.
static inline bool global_composite_has_translucent_work(
		bool p_allow_sync_readback,
		uint32_t p_overlap_record_count,
		uint32_t p_splat_count,
		bool p_has_instance_indirect) {
	if (p_allow_sync_readback) {
		return p_overlap_record_count > 0;
	}
	return p_splat_count > 0 || p_has_instance_indirect;
}

// The single choke point: map (work present, sort outcome) -> wrong-output reason.
// If there is no translucent work, nothing is composited, so no frame can be wrong.
static inline UnsortedCompositeReason classify_unsorted_composite(
		bool p_has_translucent_work,
		GlobalSortAttemptOutcome p_outcome) {
	if (!p_has_translucent_work) {
		return UnsortedCompositeReason::NONE;
	}
	switch (p_outcome) {
		case GlobalSortAttemptOutcome::NOT_ATTEMPTED:
			// Work exists but no sort was dispatched -> the sorter was unavailable.
			return UnsortedCompositeReason::SORTER_UNAVAILABLE;
		case GlobalSortAttemptOutcome::SUBMITTED:
		case GlobalSortAttemptOutcome::SYNC_FALLBACK_OK:
			return UnsortedCompositeReason::NONE;
		case GlobalSortAttemptOutcome::SYNC_FALLBACK_FAILED:
			return UnsortedCompositeReason::SORT_DISPATCH_FAILED;
		case GlobalSortAttemptOutcome::NOT_SUBMITTED:
			return UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED;
	}
	return UnsortedCompositeReason::NONE;
}

// Warning throttle for the unsorted global-composite fallback.
//
// Every frame classified as wrong-output bumps a persistent counter
// (TilePerformanceMetrics::unsorted_composite_frames, surfaced via
// get_binning_debug_counters()); this predicate rate-limits the paired WARN so the
// degradation is visible in production without one log line per frame. It fires on
// the first degraded frame and then once per interval — NOT one-shot-forever, so a
// persistent degradation keeps re-surfacing in the log.
//
// Argument is the post-increment counter value (first degraded frame == 1).
static constexpr uint64_t UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES = 300;

static inline bool should_warn_unsorted_composite(uint64_t p_unsorted_composite_frames) {
	if (p_unsorted_composite_frames == 0) {
		return false;
	}
	return p_unsorted_composite_frames == 1 ||
			(p_unsorted_composite_frames % UNSORTED_COMPOSITE_WARN_INTERVAL_FRAMES) == 0;
}

// Sort fallback policy. The instance domain has no safe unsorted fallback, so
// the only options are reuse-previous or fail. The global domain keeps the CPU
// sort as the correctness-preserving fallback when the GPU sort fails.
static inline SortFallbackPolicyDecision build_sort_fallback_policy(
		SortFallbackScenario p_scenario, bool p_instance_pipeline_active, bool /*p_strict_global_sort*/ = false) {
	SortFallbackPolicyDecision decision;
	auto push_action = [&](SortFallbackAction p_action) {
		if (decision.action_count < 4) {
			decision.actions[decision.action_count++] = p_action;
		}
	};

	switch (p_scenario) {
		case SortFallbackScenario::FORCE_CPU_OVERRIDE: {
			if (p_instance_pipeline_active) {
				push_action(SortFallbackAction::REUSE_PREVIOUS_SORT);
				push_action(SortFallbackAction::FAIL);
			} else {
				decision.cpu_sort_forced = true;
				push_action(SortFallbackAction::RUN_CPU_SORT);
				push_action(SortFallbackAction::FAIL);
			}
		} break;
		case SortFallbackScenario::SORTER_UNAVAILABLE:
		case SortFallbackScenario::GPU_SORT_FAILED: {
			push_action(SortFallbackAction::REUSE_PREVIOUS_SORT);
			if (!p_instance_pipeline_active) {
				push_action(SortFallbackAction::RUN_CPU_SORT);
			}
			push_action(SortFallbackAction::FAIL);
		} break;
	}

	return decision;
}

} // namespace GaussianSplatting

#endif // GAUSSIAN_SORT_FALLBACK_POLICY_H
