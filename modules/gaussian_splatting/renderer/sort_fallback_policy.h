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
// Unsorted global-composite fallback — reject + observability contract (#586)
// ---------------------------------------------------------------------------
//
// The global-composite path can end up rasterizing tiles in UNSORTED order, which
// is WRONG output: alpha compositing is order-dependent, so an unsorted draw is
// not "slightly off", it is incorrect.
//
// #586: the PERMANENT class of that failure — no sorter exists at all
// (TileGlobalSortResources::sorter is invalid) — no longer rasterizes.
// `unsorted_composite_must_reject_frame()` below marks it fatal for the frame,
// and the choke point in tile_renderer.cpp turns that into a publish REJECT:
// render() returns an invalid RID and the caller presents nothing for the
// frame. The TRANSIENT reasons (sorter exists, one dispatch failed) keep the
// counter + throttled WARN and are still presented; see the rationale on
// `unsorted_composite_must_reject_frame`.
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
	SORTER_UNAVAILABLE = 1, // No sorter could be built at all (probe false, creation failed, or no indirect entry point).
	SORT_DISPATCH_FAILED = 2, // Sorter exists; the sync fallback dispatch returned an error.
	ASYNC_SORT_NOT_SUBMITTED = 3, // Async submit returned timeline=0 and sync fallback is disallowed.
	// NOT produced by classify_unsorted_composite(): the frame is abandoned BEFORE a sort
	// outcome can exist, at the pre-binning resource check in tile_renderer.cpp. It is
	// still a REJECT of a frame that carried translucent work (render() returns an invalid
	// RID), and it is the exit a VRAM-pressure sorter failure takes: disable_sorter() frees
	// the key/value buffers and the same pressure then fails their reallocation. Recorded
	// so that reject telemetry can attribute it instead of counting nothing (#586).
	GLOBAL_SORT_RESOURCES_UNAVAILABLE = 4,
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
		case UnsortedCompositeReason::GLOBAL_SORT_RESOURCES_UNAVAILABLE:
			return "global sort resources unavailable (sort capacity, key/value/tile buffers or binning pipeline)";
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

// #586 FIX: which unsorted-composite reasons must REJECT the frame (nothing is
// published) instead of presenting it. This is the whole correctness decision,
// isolated as one pure function so it is testable without a GPU, and so adding an
// UnsortedCompositeReason forces an explicit fatal / non-fatal choice here: the
// switch has no default label (a new enumerator is a compiler warning), and the
// fall-through after it FAILS CLOSED — an enumerator this function does not know
// is rejected, never presented.
//
// SORTER_UNAVAILABLE is FATAL for the frame that observes it. It is the class where
// no sorter exists at all, so EVERY frame that reaches the composite in this state
// would order translucent splats by atomic-append order. Presenting nothing is
// honest; presenting wrong alpha ordering that looks like a normal render is not.
// This is a PER-FRAME verdict: whether the state persists is decided by
// TileGlobalSortResources::ensure_resources (at this revision it latches until
// reset_state(); making it retry is the follow-up PR for #586).
//
// GLOBAL_SORT_RESOURCES_UNAVAILABLE is recorded at an exit that has ALREADY
// abandoned the frame, so "must reject" is a statement of fact about it; it is
// listed as fatal so the predicate stays total and a future caller that did have
// a choice could not present a frame that was never binned.
//
// The two TRANSIENT reasons are deliberately NOT fatal:
//   * SORT_DISPATCH_FAILED     — the sorter EXISTS and the device is capable; the
//                                sync fallback dispatch returned an error for one frame.
//   * ASYNC_SORT_NOT_SUBMITTED — async submit returned timeline=0 and the sync
//                                fallback is disallowed by the readback policy.
// Both are per-frame and recover on the next frame on working hardware; rejecting
// them would turn a one-frame ordering artefact into a visible hitch. They keep the
// counter + throttled WARN (observable, not silent).
static inline bool unsorted_composite_must_reject_frame(UnsortedCompositeReason p_reason) {
	switch (p_reason) {
		case UnsortedCompositeReason::SORTER_UNAVAILABLE:
		case UnsortedCompositeReason::GLOBAL_SORT_RESOURCES_UNAVAILABLE:
			return true;
		case UnsortedCompositeReason::NONE:
		case UnsortedCompositeReason::SORT_DISPATCH_FAILED:
		case UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED:
			return false;
	}
	return true; // Fail closed: an unclassified reason is never presented.
}

// Warning throttle for the unsorted global-composite fallback.
//
// Every frame classified as wrong-output bumps a persistent counter
// (TilePerformanceMetrics::unsorted_composite_frames, surfaced via
// get_binning_debug_counters()); this predicate rate-limits the paired WARN so the
// degradation is visible in production without one log line per frame. It fires on
// the first degraded frame and then once per interval — NOT one-shot-forever, so a
// persistent degradation keeps re-surfacing in the log. The reject counter
// (TilePerformanceMetrics::global_composite_rejected_frames) uses the same throttle.
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

// ---------------------------------------------------------------------------
// Sorter-init retry policy (GPU-003, refs #922; tile sorter refs #586)
// ---------------------------------------------------------------------------
//
// !! SHARED BY TWO SORTERS. The constants and predicates in this block are
// consumed by BOTH
//   * RenderSortingOrchestrator::refresh_gpu_sorter   (instance/depth sorter, GPU-003), and
//   * TileGlobalSortResources::ensure_resources        (tile-overlap sorter, #586 PR 2).
// Editing a constant or a predicate here moves both retry schedules at once.
// That is deliberate -- two sorters with two policies for the same failure
// class is where divergence bugs live -- but it must be impossible to miss
// when reading this header, hence this paragraph.
//
// Repeated GPU-sorter creation failures (typically transient VRAM pressure at
// startup, or device contention) previously tripped a ONE-WAY latch: after 5
// failures refresh_gpu_sorter() early-returned forever, so GPU sorting stayed
// dead for the whole session even after memory freed -- and the instance route
// has no CPU fallback (see build_sort_fallback_policy above), so splats froze
// at the last sorted order or stopped rendering entirely. The tile-overlap
// sorter had the same shape with a worse consequence: its disable_sorter()
// latched sorter_available=false until renderer teardown, and every
// translucent global-composite frame was composited unsorted (before #976) or
// rejected (after #976) for the rest of the session.
//
// The replacement policy: capped exponential backoff. Below the cap the
// backoff doubles per consecutive failure (60, 120, 240, 480, 960 frames); at
// SORTER_INIT_DEGRADED_FAILURE_THRESHOLD failures the subsystem counts as
// DEGRADED and the capped interval becomes a periodic recovery probe. There is
// deliberately NO failure count that stops retrying: the cap bounds the retry
// RATE (which is all the old latch's OOM-thrash concern needs), never the
// retry COUNT. Degradation is additionally surfaced through
// record_rendering_error (diagnostics) on the instance route, not only a log
// line -- see RenderSortingOrchestrator::refresh_gpu_sorter. The tile sorter
// has no diagnostics sink of its own; it surfaces the state through the
// global_composite_rejected_frames / global_sort_sorter_init_failures counters
// and the same throttled log lines.
//
// What this policy deliberately does NOT do (recorded here, not only in a PR
// thread, per the repository's "disproportionate, not unfixable" rule): it does
// not classify a creation failure as transient vs deterministic and retry only
// the transient class, and it does not lift a latch on a configuration change
// by signature. That refinement (the shape PR #852 built, and the Codex
// finding "Stop retrying deterministic sorter creation failures") is FEASIBLE
// and is deferred to v1.0 -- it was the highest-defect-density component of
// #852 (5 of its 16 review findings), and the public-alpha bar blocks on the
// user-visible defect this policy fixes, not on the precision of which
// failures get re-probed. The model under which the deferral is acceptable:
// every attempt is rate-bounded by the cap; the predictable deterministic
// failures (capability probe, workgroup limit, buffer-size limit) are caught by
// allocation-free preflights BEFORE create_sorter() is called, so a doomed
// attempt of that class costs a few device-limit queries; only a transient
// allocation failure (where retrying is the point) or a deterministic
// shader/pipeline build failure (which no preflight can predict) reaches the
// compile path, measured at 6-14 ms warm / ~120 ms cold per attempt on an RTX
// 3090 -- once per 1800 frames at the cap, on a renderer that is presenting
// nothing anyway. A config change is picked up by the next scheduled probe,
// because every attempt re-reads the live configuration.

static constexpr uint64_t SORTER_INIT_BASE_BACKOFF_FRAMES = 60;
static constexpr uint64_t SORTER_INIT_MAX_BACKOFF_FRAMES = 1800; // ~30 s at 60 fps
static constexpr uint32_t SORTER_INIT_DEGRADED_FAILURE_THRESHOLD = 5;

// Backoff (in frames) imposed after the p_failure_count-th consecutive
// sorter-init failure. 0 failures -> no backoff.
static inline uint64_t sorter_init_backoff_frames(uint32_t p_failure_count) {
	if (p_failure_count == 0) {
		return 0;
	}
	// 60 << 5 already exceeds the cap, so bounding the doubling exponent at 5
	// keeps the shift well-defined for arbitrarily large failure counts.
	const uint32_t doublings = (p_failure_count - 1) < 5 ? (p_failure_count - 1) : 5;
	const uint64_t backoff = SORTER_INIT_BASE_BACKOFF_FRAMES << doublings;
	return backoff < SORTER_INIT_MAX_BACKOFF_FRAMES ? backoff : SORTER_INIT_MAX_BACKOFF_FRAMES;
}

// Whether refresh_gpu_sorter() should attempt sorter creation this frame.
// INVARIANT (the fix for GPU-003): for EVERY failure count, this returns true
// once SORTER_INIT_MAX_BACKOFF_FRAMES frames have elapsed since the last
// failure -- there is no permanently-disabled state.
static inline bool should_attempt_sorter_init(uint32_t p_failure_count, uint64_t p_frames_since_last_failure) {
	if (p_failure_count == 0) {
		return true;
	}
	return p_frames_since_last_failure >= sorter_init_backoff_frames(p_failure_count);
}

// Degraded = the old policy's "permanently disabled" threshold. Used only for
// escalated observability (ERROR log + RenderingError), never to stop retrying.
static inline bool sorter_init_degraded(uint32_t p_failure_count) {
	return p_failure_count >= SORTER_INIT_DEGRADED_FAILURE_THRESHOLD;
}

} // namespace GaussianSplatting

#endif // GAUSSIAN_SORT_FALLBACK_POLICY_H
