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

// Warning throttle for the global-composite UNSORTED fallback.
//
// When the global-composite path has translucent content to composite but no
// usable GPU sorter (capability-gated: the indirect-compute probe failed, sorter
// creation failed, or the created sorter lacked indirect support), it rasterizes
// tiles in UNSORTED order — wrong alpha compositing. On hardware that genuinely
// cannot build the sorter, dropping the frame would black-screen, which is worse
// than a slightly-wrong z-order, so the renderer keeps rendering; but the
// degradation must stay OBSERVABLE. Every such frame bumps a persistent counter
// (TilePerformanceMetrics::unsorted_composite_frames, surfaced via
// get_binning_debug_counters()); this predicate rate-limits the paired WARN so it
// is visible in production without one line per frame. It fires on the first
// degraded frame and then once per interval — NOT one-shot-forever, so a
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
