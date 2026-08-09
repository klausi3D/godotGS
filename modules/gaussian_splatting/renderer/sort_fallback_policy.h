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
// #586 FIX: the PERMANENT class of that failure (no sorter could be built at all)
// no longer rasterizes. `unsorted_composite_must_reject_frame()` below marks it
// fatal, and the choke point in tile_renderer.cpp turns it into a publish reject —
// the frame is not presented at all — instead of presenting a plausible-looking
// lie. The remaining, TRANSIENT reasons keep the observability-only treatment
// (they are recoverable next frame on capable hardware; see the rationale on
// `unsorted_composite_must_reject_frame`).
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

// #586 FIX: which unsorted-composite reasons must REJECT the frame rather than
// present it. This is the whole correctness decision, isolated as one pure
// function so it is testable without a GPU and so adding a new
// UnsortedCompositeReason forces an explicit fatal/non-fatal choice here (the
// switch has no default label, so a new enumerator is a compiler warning).
//
// SORTER_UNAVAILABLE is FATAL for the frame that observes it. It is the class where
// no sorter could be constructed at all (indirect-capability probe false, creation
// failed, or the created sorter has no indirect entry point), so EVERY frame that
// reaches the composite in this state would order translucent splats by atomic-append
// order. Presenting nothing is honest; presenting wrong alpha ordering that looks like
// a normal render is not.
//
// This is a PER-FRAME verdict, not a session verdict. Whether the state persists is
// decided by `SorterCreationFailure` / `sorter_creation_failure_is_permanent()` below:
// a permanent capability failure latches for good, a transient recreation failure is
// retried on a bounded backoff. See that block for why the distinction is load-bearing
// (`TileGlobalSortResources::sorter_available` returns to true only on a successful
// (re)creation or a reset_state() teardown — there is no device-change reset on that
// struct, so without the retry a single failed allocation was permanent).
//
// The two TRANSIENT reasons are deliberately NOT fatal:
//   * SORT_DISPATCH_FAILED   — the sorter EXISTS and the hardware is capable; the
//                              sync fallback dispatch returned an error for one frame.
//   * ASYNC_SORT_NOT_SUBMITTED — async submit returned timeline=0 and the sync
//                              fallback is disallowed by the default readback policy.
//                              This is reachable on a perfectly healthy desktop GPU.
// Both are per-frame blips that recover on the next frame. Rejecting them would turn
// a one-frame ordering artefact into a visible hitch (or, under a persistent async
// stall, a black screen) on working hardware — a regression of the healthy path,
// which the #586 fix must not cause. They keep counter + throttled WARN and are
// tracked separately (per-frame recovery, not a route decision).
static inline bool unsorted_composite_must_reject_frame(UnsortedCompositeReason p_reason) {
	switch (p_reason) {
		case UnsortedCompositeReason::SORTER_UNAVAILABLE:
			return true;
		case UnsortedCompositeReason::NONE:
		case UnsortedCompositeReason::SORT_DISPATCH_FAILED:
		case UnsortedCompositeReason::ASYNC_SORT_NOT_SUBMITTED:
			return false;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Sorter-recreation failure: permanent capability vs transient allocation
// ---------------------------------------------------------------------------
//
// Making SORTER_UNAVAILABLE reject the frame (above) is only defensible if
// "unavailable" can end. It could not: TileGlobalSortResources::ensure_resources'
// disable_sorter() latched sorter_available=false for EVERY recreation failure,
// including a transient one — and a capacity growth or key-config change shuts the
// WORKING sorter down BEFORE building its replacement (tile_render_resources.cpp,
// `must_recreate` branch). So one failed buffer allocation, at one instant of VRAM
// pressure, left an otherwise capable GPU with no sorter for the rest of the session.
// Before the reject that meant wrong pixels forever; after it, no pixels forever. Both
// are unacceptable, so the fix is not to soften the reject — it is to make the state
// recoverable exactly when the cause can recover.
//
// These are the three failure sites, classified by what actually produces them (read
// from the callees, not inferred from the identifiers — `probe_supports_indirect` in
// particular is a misnomer):
enum class SorterCreationFailure : uint8_t {
	// GPUSorterFactory::probe_supports_indirect(ALGORITHM_RADIX, device) == false.
	// Resolves to RadixSort::is_supported(device) && a constant caps.supports_indirect.
	// RadixSort::is_supported is a COMPUTE-LIMITS check (max workgroup size, storage
	// buffers per set, shared memory) against the configured radix_bits/workgroup_size.
	// It queries device limits and static config and allocates nothing, so it cannot
	// fail transiently: same device + same config => same answer forever. PERMANENT.
	INDIRECT_CAPABILITY_UNSUPPORTED = 0,
	// GPUSorterFactory::create_sorter() returned an invalid Ref. At this call site the
	// algorithm is RADIX explicitly and the capability probe above has already passed,
	// which makes create_sorter()'s two pre-instantiation policy rejects unreachable
	// (_algorithm_meets_requirements and .supported both hold for RADIX once the probe
	// is true). The only remaining cause is RadixSort::initialize() != OK: shader-variant
	// creation, or one of the >=8 storage_buffer_create() calls returning an invalid RID
	// (ERR_CANT_CREATE), or the capacity-dependent >UINT32_MAX size refusal
	// (ERR_INVALID_PARAMETER). Those depend on momentary VRAM pressure or on the
	// requested capacity — and the requested capacity can shrink back. TRANSIENT.
	CREATION_FAILED = 1,
	// The created sorter reports supports_indirect() == false. That is a compile-time
	// constant of the sorter class (RadixSort::supports_indirect() is a literal `true`),
	// so it cannot change at runtime; the branch is purely defensive. PERMANENT.
	CREATED_SORTER_LACKS_INDIRECT = 2,
};

// No default label, so a new failure site must make an explicit permanent/transient
// choice here rather than silently inheriting one.
static inline bool sorter_creation_failure_is_permanent(SorterCreationFailure p_failure) {
	switch (p_failure) {
		case SorterCreationFailure::INDIRECT_CAPABILITY_UNSUPPORTED:
		case SorterCreationFailure::CREATED_SORTER_LACKS_INDIRECT:
			return true;
		case SorterCreationFailure::CREATION_FAILED:
			return false;
	}
	// Unknown cause: treat as permanent. That is the fail-closed direction here — it
	// costs recoverability, never a retry storm on a device that cannot sort.
	return true;
}

// Retry schedule for a TRANSIENT sorter-recreation failure.
//
// Both obvious designs are traps:
//   * retry every frame — on a device that is genuinely out of VRAM this is an
//     allocation storm forever (create_sorter compiles shader variants and allocates
//     >=8 buffers per attempt).
//   * give up after N attempts — that is the permanent latch again, only delayed, and
//     a REAL recovery (VRAM freed, or overlap demand shrinking back below the capacity
//     that failed) could then never take effect. There is no defensible N.
// So there is no attempt cap at all. The retry is RATE-limited by an exponential
// backoff that saturates: a one-off blip costs a single rejected frame, and a device
// that stays broken costs one creation attempt per SORTER_RETRY_MAX_DELAY_CALLS.
//
// The unit is ensure_resources() CALLS, not frames. TileRenderer calls it 1-3 times per
// composited frame (tile_renderer.cpp: the async auto-resize path, the per-frame sizing
// path, and the post-count grow path), so the saturated delay is ~100-300 frames, i.e.
// roughly 1.7-5 s at 60 fps. Counting calls rather than frames is deliberate: the
// countdown lives where the failure lives and needs no frame-boundary hook.
static constexpr uint32_t SORTER_RETRY_MIN_DELAY_CALLS = 1;
static constexpr uint32_t SORTER_RETRY_MAX_DELAY_CALLS = 300;

// Next backoff length after a transient failure. Monotone non-decreasing: the caller
// deliberately does NOT reset it on a successful recreation. If it reset, a device that
// flaps (succeed, fail, succeed, fail) would restart at the minimum delay and re-pay the
// full creation cost plus a log line every other call. Keeping it monotone for the
// lifetime of the resource state bounds the flapping cost too; reset_state() teardown
// clears it, which is the same event that clears the availability latch.
static inline uint32_t next_sorter_retry_delay_calls(uint32_t p_current_delay_calls) {
	if (p_current_delay_calls == 0) {
		return SORTER_RETRY_MIN_DELAY_CALLS;
	}
	if (p_current_delay_calls >= SORTER_RETRY_MAX_DELAY_CALLS) {
		return SORTER_RETRY_MAX_DELAY_CALLS;
	}
	const uint32_t doubled = p_current_delay_calls * 2u;
	return doubled >= SORTER_RETRY_MAX_DELAY_CALLS ? SORTER_RETRY_MAX_DELAY_CALLS : doubled;
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
//
// #586 FIX: the same throttle also rate-limits the paired REJECT warning, fed by
// TilePerformanceMetrics::global_composite_rejected_frames. Both degraded outcomes
// (presented-unsorted, rejected) use one throttle so neither can spam the log.
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
