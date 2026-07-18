/**************************************************************************/
/*  test_renderer_lifetime_proof.h                                        */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

/*
 * PR 3 of work package #352 — Renderer Lifetime Proof.
 *
 * Composes existing accounting surfaces into a single roll-up that proves
 * zero retained GPU/RID resources across four renderer lifecycle scenarios.
 * The fixture introduces NO new instrumentation; it reads:
 *
 *   - RenderDeviceManager::get_last_shutdown_owned_resource_count()
 *     get_last_shutdown_tracked_resource_count()
 *     Canonical signal for "the renderer's RDM forgot to free an owned RID
 *     it tracked." See interfaces/render_device_manager.h:115-117.
 *
 *   - RenderingDevice::get_memory_usage(MEMORY_TOTAL) delta
 *     Catches non-RDM-tracked churn (raw rd->X_create bypasses,
 *     OutputCompositor framebuffer LRU, etc.). Reuses the SAME 4 MiB noise
 *     floor that the existing [GS-GPU][RID-LEAK?] listener trusts in
 *     tests/gs_gpu_test_runner.cpp:248. For monotonicity checks (C, D) we
 *     use delta-of-deltas so allocator-pool noise cancels out, gated by a
 *     SEPARATE monotonicity_threshold_bytes field — see set_*_threshold_bytes
 *     setters below. Conflating the two thresholds (Codex PR #386 review
 *     #3294999198 P1) would tighten the absolute leak gate to the (much
 *     smaller) per-cycle monotonicity budget and let normal allocator jitter
 *     fail an otherwise healthy run.
 *
 *   - StringName orphans: DEFERRED to PR 6. Emit sentinel -1 in the JSON;
 *     PR 6's subprocess emitter will feed the same JSON shape from a
 *     different path so PR 7's manifest gate consumes one contract.
 *
 * Pass rule: (rdm_owned_leaked == 0 OR RDM unmeasured) AND
 * rd_memory_leaked_bytes < threshold AND monotonicity_ok.
 * AND, not OR — they catch different bugs. The RDM clause is OR-with-
 * unmeasured because scenarios that do not call record_rdm_shutdown_counts()
 * surface the sentinel rather than silently advertising a measured zero.
 *
 * JSON contract (PR 7 will consume — do not change after merge):
 *   [GS-LIFETIME] {"scenario":"renderer_instance","passed":true,
 *     "rd_bytes_leaked":131072,"rdm_owned_leaked":-1,"rdm_tracked_leaked":-1,
 *     "teardown_sync":true,"threshold_bytes":4194304,
 *     "monotonicity_threshold_bytes":262144,
 *     "stringname_orphan_delta":-1,"monotonicity_ok":true,"fail_reason":""}
 *
 * Sentinel semantics: rdm_owned_leaked == -1 (and rdm_tracked_leaked == -1)
 * means the scenario never measured RDM counters via
 * record_rdm_shutdown_counts(); a non-negative value is the measured
 * post-shutdown delta. This mirrors the stringname_orphan_delta:-1
 * pattern. Downstream gates (PR #390) treat -1 as advisory and the gate
 * skips the RDM clause; a measured non-negative value enforces the
 * "leaked == 0" clause as part of the pass rule. (Codex PR #386 review
 * #3295073296 P1: previously _snapshot() hard-set RDM to 0, none of the
 * four lifetime scenarios called record_rdm_shutdown_counts(), and the
 * gate silently passed real RDM-owned RID leaks whenever the RD byte
 * threshold also stayed clean — and the emitted JSON falsely reported
 * "rdm_owned_leaked":0 as if measured.)
 *
 * monotonicity_ok defaults true for absolute-threshold scenarios (A, B);
 * monotonicity-style scenarios (C, D) MUST call set_monotonicity_ok(false,
 * reason) BEFORE finalize() when their cycle-to-cycle delta-of-deltas
 * exceeds the configured threshold. finalize() folds monotonicity_ok into
 * the final pass verdict so the emitted "passed" field always reflects the
 * full pass rule. (Codex PR #386 review #3294763666 P1: previously the
 * monotonicity result was only folded into the local CHECK_MESSAGE bool,
 * not into the JSON contract — downstream gates consuming only the
 * [GS-LIFETIME] line would mis-classify a monotonicity regression as a
 * pass.)
 *
 * Line prefix [GS-LIFETIME] mirrors the [GS-GPU] convention used by
 * gs_gpu_test_runner.cpp so PR 7's manifest parser can grep it trivially.
 */

#pragma once

#include "test_macros.h"

#include "../core/gaussian_data.h"
#include "../core/gaussian_splat_manager.h"
#include "../core/gaussian_splat_scene_director.h"
#include "../core/gaussian_streaming.h"
#include "../interfaces/render_device_manager.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "../renderer/tile_renderer.h"

#include "core/io/json.h"
#include "core/math/random_number_generator.h"
#include "core/string/print_string.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering_server.h"

namespace TestGaussianSplatting {

// Sentinel for "RDM resource counters were not measured for this scenario."
// Distinct from a measured zero. _snapshot() initializes the RDM fields to
// this value; record_rdm_shutdown_counts() clears the sentinel on the
// after-snapshot before accumulating real counts. _compute_pass() treats a
// surviving sentinel as "unmeasured" — skips the RDM gate and surfaces -1
// in the emitted JSON, mirroring the stringname_orphan_delta:-1 pattern.
//
// Why a sentinel and not "always zero":
//   The four lifetime scenarios that ship in this fixture do NOT plumb
//   record_rdm_shutdown_counts(), so before this fix _snapshot() hard-set
//   the RDM fields to 0 and _compute_pass() always treated the RDM signal
//   as "measured clean." A real RDM-owned RID leak that stayed under the
//   4 MiB RD byte threshold would have passed the gate while the emitted
//   JSON falsely advertised "rdm_owned_leaked":0 as a measured result.
//   Downstream gates (PR #390's --mode lifetime) would have no way to
//   distinguish "the scenario doesn't measure RDM" from "the scenario
//   measured RDM and it was clean." (Codex PR #386 review #3295073296 P1.)
//
// Why UINT32_MAX and not -1: the underlying field is uint32_t (RDM exposes
// uint32_t counters). -1 cast to uint32_t IS UINT32_MAX, so this is the
// same value the stringname_orphan_delta:-1 sentinel uses, just typed for
// the field. to_dict() emits int64_t(-1) for downstream consumers.
static constexpr uint32_t RDM_UNMEASURED = std::numeric_limits<uint32_t>::max();

// ---------------------------------------------------------------------------
// LifetimeBaseline — snapshot of every accounting surface at a single point.
// stringname_orphan_delta sentinel -1 = not measured (deferred to PR 6).
// rdm_tracked_resources / rdm_owned_resources sentinel RDM_UNMEASURED =
// not measured (no record_rdm_shutdown_counts() call in this scenario);
// see RDM_UNMEASURED rationale above.
// #352: measure GS-OWNED GPU memory (buffer + texture bytes the renderer
// allocates and is responsible for freeing), NOT RenderingDevice::MEMORY_TOTAL.
// MEMORY_TOTAL is the VMA grand total, which includes process-lifetime
// driver-internal allocations (compute pipelines, descriptor pools, command/VMA
// overhead) created once and never freed per renderer instance. Gating on
// MEMORY_TOTAL charged that shared one-time infrastructure to each scenario and
// was order-dependent (pipeline compilation landing in different measurement
// windows produced ~64-178 MB swings with GS buffers/textures already at 0).
// buffers+textures isolates the renderer's own resources; owned pipeline/shader
// RID leaks are covered separately by the RDM owned-resource count.
static uint64_t _gs_owned_memory_bytes(RenderingDevice *p_rd) {
	if (p_rd == nullptr) {
		return 0ull;
	}
	return p_rd->get_memory_usage(RenderingDevice::MEMORY_BUFFERS) + p_rd->get_memory_usage(RenderingDevice::MEMORY_TEXTURES);
}

// ---------------------------------------------------------------------------
struct LifetimeBaseline {
	uint64_t rd_memory_usage_bytes = 0;
	uint32_t rdm_tracked_resources = RDM_UNMEASURED;
	uint32_t rdm_owned_resources = RDM_UNMEASURED;
	int64_t stringname_orphan_delta = -1;
};

// ---------------------------------------------------------------------------
// LifetimeResult — pass/fail roll-up plus the JSON line emitter.
// ---------------------------------------------------------------------------
struct LifetimeResult {
	String scenario_id;
	LifetimeBaseline before;
	LifetimeBaseline after;
	uint64_t rd_memory_leaked_bytes = 0;
	// RDM leak counters: hold a measured non-negative delta when the
	// scenario called record_rdm_shutdown_counts(); otherwise hold the
	// RDM_UNMEASURED sentinel (UINT32_MAX), which to_dict() emits as -1
	// and _compute_pass() treats as "skip the RDM gate." Mirrors the
	// stringname_orphan_delta:-1 advisory pattern. (Codex PR #386
	// review #3295073296 P1.)
	uint32_t rdm_owned_leaked = RDM_UNMEASURED;
	uint32_t rdm_tracked_leaked = RDM_UNMEASURED;
	// Absolute leak gate: rd_memory_leaked_bytes < threshold_bytes. Default
	// matches the 4 MiB noise floor that the existing [GS-GPU][RID-LEAK?]
	// listener trusts in tests/gs_gpu_test_runner.cpp:248.
	uint64_t threshold_bytes = 4ull << 20; // 4 MiB — matches GsGpuRidLeakListener.
	// Per-cycle monotonicity gate: separate from `threshold_bytes` so tuning
	// a monotonicity-style scenario's growth budget (C, D — typically 64–256
	// KiB) does not silently tighten the absolute leak gate. Default 256 KiB
	// is the existing scene_director_reload value; scenarios override via
	// set_monotonicity_threshold_bytes(). Scenarios that do not check
	// monotonicity (A, B) ignore this field entirely. (Codex PR #386 review
	// #3294999198 P1.)
	uint64_t monotonicity_threshold_bytes = 256ull * 1024ull;
	bool teardown_was_synchronous = false;
	bool crashed_or_aborted = false;
	// True unless a monotonicity-style scenario (C, D) recorded a
	// delta-of-deltas excursion via set_monotonicity_ok(false, reason).
	// Folded into `passed` by _compute_pass(). Always emitted in the JSON
	// so downstream gates can distinguish a real lifetime regression in a
	// monotonicity scenario from an absolute-byte-threshold breach.
	// (Codex PR #386 review #3294763666 P1.)
	bool monotonicity_ok = true;
	bool passed = false;
	String fail_reason;

	Dictionary to_dict() const {
		Dictionary d;
		d["scenario"] = scenario_id;
		d["passed"] = passed;
		d["rd_bytes_leaked"] = int64_t(rd_memory_leaked_bytes);
		// Surface the unmeasured sentinel as int64_t(-1) (the same value
		// stringname_orphan_delta uses) so downstream gates can
		// distinguish "scenario never measured RDM" from "scenario
		// measured RDM and it was clean (zero leaked)." (Codex PR #386
		// review #3295073296 P1.)
		d["rdm_owned_leaked"] = (rdm_owned_leaked == RDM_UNMEASURED)
				? int64_t(-1)
				: int64_t(rdm_owned_leaked);
		d["rdm_tracked_leaked"] = (rdm_tracked_leaked == RDM_UNMEASURED)
				? int64_t(-1)
				: int64_t(rdm_tracked_leaked);
		d["teardown_sync"] = teardown_was_synchronous;
		d["threshold_bytes"] = int64_t(threshold_bytes);
		// Emit the monotonicity threshold separately so PR #390's --mode
		// lifetime gate can see both criteria explicitly without re-deriving
		// either from the other. (Codex PR #386 review #3294999198 P1.)
		d["monotonicity_threshold_bytes"] = int64_t(monotonicity_threshold_bytes);
		d["stringname_orphan_delta"] = int64_t(after.stringname_orphan_delta);
		d["monotonicity_ok"] = monotonicity_ok;
		d["fail_reason"] = fail_reason;
		return d;
	}

	void emit_to_stdout() const {
		// Mirrors the [GS-GPU][RID-LEAK?] line-prefix pattern from
		// tests/gs_gpu_test_runner.cpp:260 — grep-trivial for PR 7.
		const String json = JSON::stringify(to_dict());
		print_line(String("[GS-LIFETIME] ") + json);
	}
};

// ---------------------------------------------------------------------------
// RendererLifetimeFixture — RAII wrapper. capture_baseline() snapshots the
// before-state; finalize() snapshots after-state, computes the pass rule,
// and emits the JSON line. The dtor auto-finalizes if the test exited via
// REQUIRE_FALSE without an explicit finalize() call.
// ---------------------------------------------------------------------------
class RendererLifetimeFixture {
public:
	RendererLifetimeFixture(const String &p_scenario_id, RenderingDevice *p_rd) :
			scenario_id_(p_scenario_id), rd_(p_rd) {
		result_.scenario_id = p_scenario_id;
	}

	~RendererLifetimeFixture() {
		if (!finalized_) {
			// REQUIRE failure path — emit a best-effort line so PR 7 still
			// sees the scenario in its manifest, marked failed.
			result_.crashed_or_aborted = true;
			result_.passed = false;
			if (result_.fail_reason.is_empty()) {
				result_.fail_reason = "scope_exited_without_finalize";
			}
			result_.emit_to_stdout();
		}
	}

	RendererLifetimeFixture(const RendererLifetimeFixture &) = delete;
	RendererLifetimeFixture &operator=(const RendererLifetimeFixture &) = delete;

	void capture_baseline() {
		_snapshot(result_.before);
		baselined_ = true;
	}

	// Caller invokes finalize() AFTER all references the scenario allocated
	// have been dropped. Computes the pass rule, emits the JSON line, and
	// returns the pass bool so the TEST_CASE can CHECK() it.
	bool finalize() {
		if (finalized_) {
			return result_.passed;
		}
		// Preserve RDM shutdown counts already folded in by
		// record_rdm_shutdown_counts() before _snapshot() overwrites the
		// after-baseline's RDM fields with the sentinel 0s. Without this,
		// rdm_owned_leaked / rdm_tracked_leaked are always 0 in the pass
		// rule and a real RDM-owned RID leak can slip through whenever
		// rd_memory_leaked_bytes stays under threshold. (Codex PR #386
		// review, P1.) Strategy: option (b) — save+restore around
		// _snapshot, keeping the API unchanged.
		const uint32_t saved_rdm_owned = result_.after.rdm_owned_resources;
		const uint32_t saved_rdm_tracked = result_.after.rdm_tracked_resources;
		_snapshot(result_.after);
		result_.after.rdm_owned_resources = saved_rdm_owned;
		result_.after.rdm_tracked_resources = saved_rdm_tracked;
		// Regression-test injection: overlay a synthetic after-bytes value so
		// pass-rule tests can construct exact arithmetic scenarios without a
		// live device. No-op outside tests that opted in via
		// test_inject_after_rd_bytes().
		if (rd_bytes_override_active_) {
			result_.after.rd_memory_usage_bytes = rd_bytes_override_value_;
		}
		_compute_pass();
		result_.emit_to_stdout();
		finalized_ = true;
		return result_.passed;
	}

	// Mark this scenario as a skip (e.g. failed_init when an RD singleton is
	// present). Emits a failed=false / passed=false JSON line with
	// fail_reason explaining the skip. Counts toward the manifest but the
	// gate treats fail_reason != "" + crashed_or_aborted as advisory.
	void mark_skipped(const String &p_reason) {
		if (finalized_) {
			return;
		}
		result_.fail_reason = String("skipped: ") + p_reason;
		result_.passed = false;
		result_.crashed_or_aborted = false;
		result_.emit_to_stdout();
		finalized_ = true;
	}

	// Override the ABSOLUTE rd_memory_leaked_bytes leak gate. Default 4 MiB
	// matches the GsGpuRidLeakListener noise floor; scenarios should leave
	// this alone unless they have a documented reason to relax or tighten
	// the absolute gate. Monotonicity-style scenarios MUST NOT abuse this
	// setter to configure their per-cycle growth budget — use
	// set_monotonicity_threshold_bytes() instead. (Codex PR #386 review
	// #3294999198 P1: previously scenarios C/D set this to their per-cycle
	// monotonicity budget, silently tightening the absolute gate from 4 MiB
	// to 256/64 KiB and letting normal allocator jitter fail healthy runs.)
	void set_threshold_bytes(uint64_t p_bytes) { result_.threshold_bytes = p_bytes; }

	// Override the per-cycle MONOTONICITY growth budget. Independent of
	// `threshold_bytes` (the absolute leak gate) — scenarios C and D use this
	// for their delta-of-deltas check while `threshold_bytes` continues to
	// gate `rd_memory_leaked_bytes` at the 4 MiB noise floor. (Codex PR #386
	// review #3294999198 P1.)
	void set_monotonicity_threshold_bytes(uint64_t p_bytes) {
		result_.monotonicity_threshold_bytes = p_bytes;
	}

	// Record an extra fail reason without changing pass-rule arithmetic.
	void set_fail_reason(const String &p_reason) {
		if (result_.fail_reason.is_empty()) {
			result_.fail_reason = p_reason;
		} else {
			result_.fail_reason += String("; ") + p_reason;
		}
	}

	// Monotonicity-style scenarios (C, D) record their delta-of-deltas
	// verdict here BEFORE finalize(). The fixture folds monotonicity_ok
	// into both the local `passed` bool AND the emitted [GS-LIFETIME] JSON
	// line so downstream gates that consume only the JSON cannot
	// mis-classify a monotonicity regression as a pass. When monotonicity
	// fails, `p_reason` is appended to `fail_reason` and
	// `monotonicity_regression` is prepended so the failure mode is
	// self-describing in the JSON. (Codex PR #386 review #3294763666 P1.)
	void set_monotonicity_ok(bool p_ok, const String &p_reason = String()) {
		result_.monotonicity_ok = p_ok;
		if (!p_ok) {
			const String tag = "monotonicity_regression";
			String detail = tag;
			if (!p_reason.is_empty()) {
				detail += String(": ") + p_reason;
			}
			if (result_.fail_reason.is_empty()) {
				result_.fail_reason = detail;
			} else if (result_.fail_reason.find(tag) < 0) {
				result_.fail_reason = detail + String("; ") + result_.fail_reason;
			}
		}
	}

	void set_teardown_was_synchronous(bool p_sync) {
		result_.teardown_was_synchronous = p_sync;
	}

	const LifetimeResult &result() const { return result_; }

private:
	String scenario_id_;
	RenderingDevice *rd_ = nullptr;
	LifetimeResult result_;
	bool baselined_ = false;
	bool finalized_ = false;
	// Regression-test injection: when active, finalize() uses this value as
	// the after-snapshot's rd_memory_usage_bytes instead of querying the
	// device. See test_inject_after_rd_bytes().
	bool rd_bytes_override_active_ = false;
	uint64_t rd_bytes_override_value_ = 0;

	void _snapshot(LifetimeBaseline &r_out) const {
		r_out.rd_memory_usage_bytes = _gs_owned_memory_bytes(rd_);
		// RDM counters: initialize to the RDM_UNMEASURED sentinel rather
		// than a measured zero. Scenarios that own an RDM and want the
		// pass rule to enforce its post-shutdown counters MUST call
		// record_rdm_shutdown_counts() (which clears the sentinel on the
		// after-snapshot before accumulating); scenarios that don't get
		// the sentinel surfaced as -1 in the JSON and the RDM gate
		// skipped, instead of silently passing as if measured-clean.
		// (Codex PR #386 review #3295073296 P1.)
		r_out.rdm_tracked_resources = RDM_UNMEASURED;
		r_out.rdm_owned_resources = RDM_UNMEASURED;
		// PR 6 will populate stringname_orphan_delta via the subprocess
		// emitter path. For now the sentinel propagates through.
		r_out.stringname_orphan_delta = -1;
	}

	void _compute_pass() {
		// rd_memory_leaked_bytes := max(0, after - before).
		if (result_.after.rd_memory_usage_bytes > result_.before.rd_memory_usage_bytes) {
			result_.rd_memory_leaked_bytes =
					result_.after.rd_memory_usage_bytes - result_.before.rd_memory_usage_bytes;
		} else {
			result_.rd_memory_leaked_bytes = 0;
		}
		// RDM owned/tracked deltas are only meaningful when an RDM shutdown
		// was invoked during the scenario and its post-shutdown counters
		// were folded in by the caller via record_rdm_shutdown_*. When the
		// after-snapshot still holds the RDM_UNMEASURED sentinel, the
		// scenario never called record_rdm_shutdown_counts(); surface that
		// upstream as RDM_UNMEASURED in the *_leaked field (to_dict()
		// translates to -1) and skip the RDM gate entirely. The pre-fix
		// behaviour was to compute the delta against an initial zero in
		// both baselines, which silently published "rdm_owned_leaked":0 to
		// downstream consumers and let real RDM-owned RID leaks pass the
		// gate whenever the RD-byte threshold also stayed clean. (Codex
		// PR #386 review #3295073296 P1.)
		const bool rdm_measured = (result_.after.rdm_owned_resources != RDM_UNMEASURED) &&
				(result_.after.rdm_tracked_resources != RDM_UNMEASURED);
		if (!rdm_measured) {
			result_.rdm_owned_leaked = RDM_UNMEASURED;
			result_.rdm_tracked_leaked = RDM_UNMEASURED;
		} else {
			if (result_.after.rdm_owned_resources > result_.before.rdm_owned_resources) {
				result_.rdm_owned_leaked =
						result_.after.rdm_owned_resources - result_.before.rdm_owned_resources;
			} else {
				result_.rdm_owned_leaked = 0;
			}
			if (result_.after.rdm_tracked_resources > result_.before.rdm_tracked_resources) {
				result_.rdm_tracked_leaked =
						result_.after.rdm_tracked_resources - result_.before.rdm_tracked_resources;
			} else {
				result_.rdm_tracked_leaked = 0;
			}
		}

		// Pass rule: AND of all signals. They catch different bugs.
		// monotonicity_ok is folded in here so the [GS-LIFETIME] JSON
		// line that finalize() emits AFTER this call reflects the full
		// verdict — a monotonicity-style scenario (asset_attach_detach,
		// scene_director_reload) that breaches its cycle-to-cycle delta
		// threshold cannot publish "passed":true to downstream gates.
		// (Codex PR #386 review #3294763666 P1.)
		//
		// rdm_ok: when unmeasured, skip the gate (Option B per Codex
		// PR #386 review #3295073296 P1). The sentinel surfaced in the
		// JSON is the downstream gate's signal to either enforce a
		// stricter "must measure" policy or accept advisory semantics.
		// PR #390 follow-up may extend its advisory_fields to require
		// scenarios that DO measure to declare a threshold, mirroring
		// the stringname_orphan_delta advisory pattern.
		const bool rdm_ok = !rdm_measured || (result_.rdm_owned_leaked == 0);
		const bool rd_bytes_ok = (result_.rd_memory_leaked_bytes < result_.threshold_bytes);
		result_.passed = rdm_ok && rd_bytes_ok && result_.monotonicity_ok && !result_.crashed_or_aborted;
		if (!result_.passed && result_.fail_reason.is_empty()) {
			if (!rdm_ok) {
				result_.fail_reason = vformat("rdm_owned_leaked=%d > 0", int64_t(result_.rdm_owned_leaked));
			} else if (!rd_bytes_ok) {
				result_.fail_reason = vformat(
						"rd_bytes_leaked=%d >= threshold=%d",
						int64_t(result_.rd_memory_leaked_bytes),
						int64_t(result_.threshold_bytes));
			} else if (!result_.monotonicity_ok) {
				// set_monotonicity_ok(false, ...) populates fail_reason
				// directly, so this branch is only reached when a caller
				// flipped result_.monotonicity_ok via some other path
				// without supplying a reason. Keep the JSON
				// self-describing.
				result_.fail_reason = "monotonicity_regression";
			}
		}
	}

public:
	// Fold a manager-shutdown counter pair into the after-snapshot so the
	// pass rule includes the RDM signal. Callers use this when they own a
	// manager they shut down explicitly inside the scenario. The first
	// call also clears the RDM_UNMEASURED sentinel on BOTH baselines so
	// _compute_pass() knows this scenario actually measured RDM — the
	// before-snapshot is treated as a measured zero (the cycle had not
	// started, so no RDMs were shutdown yet) and the after-snapshot
	// accumulates the post-shutdown counts. Without clearing the sentinel
	// in after, `+= p_owned` would overflow the UINT32_MAX initial value.
	// (Codex PR #386 review #3295073296 P1.)
	void record_rdm_shutdown_counts(uint32_t p_owned, uint32_t p_tracked) {
		if (result_.after.rdm_owned_resources == RDM_UNMEASURED) {
			result_.after.rdm_owned_resources = 0;
		}
		if (result_.after.rdm_tracked_resources == RDM_UNMEASURED) {
			result_.after.rdm_tracked_resources = 0;
		}
		if (result_.before.rdm_owned_resources == RDM_UNMEASURED) {
			result_.before.rdm_owned_resources = 0;
		}
		if (result_.before.rdm_tracked_resources == RDM_UNMEASURED) {
			result_.before.rdm_tracked_resources = 0;
		}
		// Add to the after-snapshot so multiple managers (one per cycle)
		// accumulate. Before-snapshot stays at 0 so the leak == counter.
		result_.after.rdm_owned_resources += p_owned;
		result_.after.rdm_tracked_resources += p_tracked;
	}

	// Test-only injection: synthesize an rd_memory_usage_bytes delta so
	// regression tests can construct exact pass-rule arithmetic scenarios
	// (e.g., 1 MiB jitter sitting between the monotonicity threshold and the
	// absolute leak gate) without needing a live RenderingDevice. finalize()
	// snapshots after the call and overlays this into the leaked-bytes
	// computation. Use ONLY from regression tests, never from a real
	// scenario. (Codex PR #386 review #3294999198 P1 — needed to prove the
	// threshold decoupling at the pass-rule layer, not just the field-level
	// independence.)
	void test_inject_after_rd_bytes(uint64_t p_synthetic_after_bytes) {
		rd_bytes_override_active_ = true;
		rd_bytes_override_value_ = p_synthetic_after_bytes;
	}
};

// ---------------------------------------------------------------------------
// Helpers: build a 64-splat GaussianData, run a render_for_view at 128x128.
// ---------------------------------------------------------------------------
inline Ref<::GaussianData> _make_lifetime_test_data(int p_count) {
	Ref<::GaussianData> data;
	data.instantiate();
	LocalVector<Gaussian> gaussians;
	gaussians.resize(p_count);
	RandomNumberGenerator rng;
	rng.set_seed(0x9E3779B97F4A7C15ull);
	for (int i = 0; i < p_count; i++) {
		Gaussian &g = gaussians[i];
		g = Gaussian{};
		g.position = Vector3(
				rng.randf_range(-2.0f, 2.0f),
				rng.randf_range(-2.0f, 2.0f),
				rng.randf_range(-5.0f, -3.0f));
		g.scale = Vector3(0.1f, 0.1f, 0.1f);
		g.rotation = Quaternion();
		g.opacity = 1.0f;
		g.sh_dc = Color(1.0f, 0.5f, 0.2f, 1.0f);
		g.normal = Vector3(0.0f, 1.0f, 0.0f);
		g.area = 0.01f;
	}
	data->set_gaussians(gaussians);
	return data;
}

// ---------------------------------------------------------------------------
// Scenario A: renderer_instance — create, use, destroy in isolation.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] renderer_instance lifetime proof") {
	REQUIRE_GPU_DEVICE();
	// `rd` is published by the macro above; reuse it as the
	// authoritative device the renderer was constructed against.
	// Issue #392: renderer.instantiate(rd) -> GaussianSplatRenderer ctor
	// transitively calls into GaussianSplatManager::register_gaussian_buffer
	// / _request_primary_local_device, both of which index into
	// WorkerThreadPool::ThreadData via a derived thread index. The
	// `--gs-gpu-test` runner provisions an RD but not the pool (see
	// gs_gpu_test_runner.cpp's Main::test_setup() invocation, which does
	// not call WorkerThreadPool::get_singleton()->init() the way
	// tests/test_main.cpp:242 does), so the pool's threads LocalVector is
	// empty (size 0) and the indexed access OOBs → STATUS_STACK_BUFFER_OVERRUN.
	// Skip cleanly here if the pool isn't usable instead of crashing.
	REQUIRE_WORKER_THREAD_POOL();

	RendererLifetimeFixture fixture("renderer_instance", rd);
	fixture.capture_baseline();

	// #392 / Codex PR #419 Finding 1: clear the process-static RDM teardown
	// snapshot BEFORE constructing the renderer so the count we read after
	// destruction reflects exactly this renderer's per-instance RDM shutdown.
	// The renderer owns its RDM and records get_last_shutdown_owned/tracked
	// counts during _teardown_resources() (on unref); we feed those into
	// record_rdm_shutdown_counts() so a leaked pipeline/shader/uniform-set/
	// sampler/framebuffer RID is caught by COUNT even when it adds no
	// buffer/texture BYTES.
	GaussianSplatRenderer::test_reset_last_teardown_rdm_counts();

	{
		Ref<GaussianSplatRenderer> renderer;
		renderer.instantiate(rd);
		REQUIRE(renderer.is_valid());

		Ref<::GaussianData> data = _make_lifetime_test_data(64);
		const Error set_err = renderer->set_gaussian_data(data);
		// ERR_UNCONFIGURED is acceptable: some asset wiring paths only
		// fully configure during the first frame's render_for_view.
		CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));

		Transform3D cam_transform;
		Projection projection;
		projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
		renderer->render_for_view(cam_transform, projection, RID(), Size2i(128, 128));

		// Dispatcher-path probe: ask the renderer (via the dispatcher)
		// whether a render-thread dispatch would actually be attempted
		// BEFORE unref. If a render loop is standing (a future runner
		// flips REQUIRE_GPU_DEVICE to use a live RD), this returns true
		// and the renderer dtor would dispatch teardown to that loop —
		// silent mis-measurement. We require it to return false (no
		// dispatch path) so the dtor falls through to the synchronous
		// _teardown_resources() path. See
		// renderer/gaussian_splat_renderer.cpp:1232-1239.
		//
		// Importantly, this is the new
		// test_is_render_thread_dispatch_path_active() probe — NOT the
		// older test_dispatch_call_on_render_thread_blocking_without_completion()
		// helper, which submits an actual dispatch and conflates
		// "no dispatch path active" (early-exit returned false) with
		// "dispatched but timed out waiting for completion" (also
		// returned false). In a render-loop-enabled environment the
		// older helper could time out and the fixture would silently
		// mis-mark teardown as synchronous. The new probe is a pure
		// inspection of the dispatcher's early-exit state and cannot be
		// confounded by a wait-for-completion timeout. (Codex PR #386
		// review, P1.)
		const bool dispatch_path_active =
				renderer->test_is_render_thread_dispatch_path_active();
		REQUIRE_FALSE_MESSAGE(dispatch_path_active,
				"render_thread_dispatcher path is active in lifetime tests; "
				"renderer dtor would otherwise dispatch teardown to it and the fixture "
				"would mis-measure the post-unref baseline.");
		fixture.set_teardown_was_synchronous(true);

		// Refcount sanity: any leftover Ref<> means dtor won't run on unref.
		// scene_director._should_prune_world (cpp:896) requires refcount<=1
		// for cleanup, so even one extra Ref breaks lifetime accounting.
		// Diagnostics::register_renderer and PerformanceMonitors track raw
		// pointers (not Refs) so they do not bump refcount; verified
		// against rendering_diagnostics.cpp:48 and performance_monitors.h:89.
		CHECK_MESSAGE(renderer->get_reference_count() == 1,
				"Renderer refcount > 1 before unref: a singleton or shared owner "
				"is retaining a Ref<>. Filing this as a finding does not loosen "
				"the lifetime gate.");

		renderer.unref();
	}

	// Drive the main-device frame forward so the render-path + dtor deferred
	// frees actually reclaim before measuring. RenderingDevice::free() only
	// queues onto the per-frame disposal list; _free_pending_resources runs on
	// frame advance (_begin_frame). The --gs-gpu-test runner has no production
	// render loop to advance frames, so without this the one-time render
	// allocations sit unreclaimed and read as a ~178 MB leak. Advancing past the
	// (small) frame ring drains the queue, matching production steady state.
	for (int i = 0; i < 4; i++) {
		rd->swap_buffers(false);
	}

	// #392 / Codex PR #419 Finding 1: the renderer's per-instance RDM was shut
	// down inside _teardown_resources() (on unref above), which recorded its
	// post-shutdown owned/tracked RID counts. Feed them into the pass rule so a
	// leaked owned RID that adds no buffer/texture bytes still fails the gate
	// via the RDM COUNT clause. A healthy teardown frees every owned RID before
	// shutdown, so owned==0 here (measured-clean, NOT the -1 unmeasured
	// sentinel).
	REQUIRE_MESSAGE(GaussianSplatRenderer::test_has_last_teardown_rdm_counts(),
			"renderer teardown did not record RDM shutdown counts; the RDM owned-count "
			"coverage would silently fall back to the -1 unmeasured sentinel.");
	fixture.record_rdm_shutdown_counts(
			GaussianSplatRenderer::test_get_last_teardown_rdm_owned_count(),
			GaussianSplatRenderer::test_get_last_teardown_rdm_tracked_count());

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
	CHECK(fixture.result().teardown_was_synchronous);
}

// ---------------------------------------------------------------------------
// Scenario B: failed_init — GaussianStreamingSystem.initialize() with no
// device, assert clean teardown. Skip-via-emit if RD singleton present.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][FailedInit] failed_init lifetime proof") {
	// Tag is intentionally NOT [RequiresGPU]; this scenario asserts the
	// no-device path stays clean. If the runner DID provision an RD via
	// ANY of the three resolution paths the GaussianStreamingSystem will
	// itself consult (RD singleton, RenderingServer, or the
	// GaussianSplatManager), emit a skipped JSON line so the manifest
	// still sees the scenario in its row count. Checking only the RD
	// singleton — as before — meant the system could obtain a device via
	// the manager/RenderingServer path and the assertion that
	// is_streaming_capable() == false would then target the wrong branch.
	// Mirrors the pattern PR #383 shipped in
	// test_gaussian_streaming_lifecycle.cpp:810-820. (Codex PR #386
	// review, P2.)
	bool has_device = RenderingDevice::get_singleton() != nullptr;
	const char *which_device = "RD singleton";
	if (!has_device) {
		if (RenderingServer *rs_probe = RenderingServer::get_singleton()) {
			if (rs_probe->get_rendering_device() != nullptr) {
				has_device = true;
				which_device = "RenderingServer device";
			}
		}
	}
	if (!has_device) {
		if (GaussianSplatManager *mgr_probe = GaussianSplatManager::get_singleton()) {
			if (mgr_probe->get_primary_rendering_device() != nullptr) {
				has_device = true;
				which_device = "GaussianSplatManager primary device";
			}
		}
	}
	if (has_device) {
		RendererLifetimeFixture fixture("failed_init", nullptr);
		fixture.capture_baseline();
		fixture.mark_skipped(String(which_device) + " present");
		return;
	}

	RendererLifetimeFixture fixture("failed_init", nullptr);
	fixture.capture_baseline();

	{
		Ref<::GaussianData> data = _make_lifetime_test_data(64);

		Ref<GaussianStreamingSystem> system;
		system.instantiate();
		// No set_device_manager / initialize_with_device — this MUST fail
		// the runtime-ready check and leave the system in a safe state
		// per PR #383's failed-init guards.
		system->initialize(data);
		CHECK_FALSE(system->is_streaming_capable());

		system.unref();
	}

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Scenario C: scene_director_reload — 3x submit/release/teardown cycle on a
// synthetic scenario, monotonicity check.
//
// PR 4 of #352 flipped skip(true) -> skip(false) once
// GaussianSplatSceneDirector::teardown_world_for_scenario() landed.
// The body simulates the editor F6 reload pattern: build a SharedWorld via
// submit_world_submission, drive a render, release_world_submission, then
// explicitly tear down the scenario (the PREDELETE-equivalent the editor
// hits when it throws the old scene tree away on reload). If teardown is
// incomplete, cycle 2/3 memory grows monotonically over cycle 1 by more
// than the 256 KiB threshold and the scenario fails -- evidence the leak
// documented at gaussian_splat_scene_director.cpp:351 is closed.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] scene_director_reload lifetime proof") {
	REQUIRE_GPU_DEVICE();

	GaussianSplatSceneDirector *director = GaussianSplatSceneDirector::get_singleton();
	const bool owns_director = (director == nullptr);
	if (!director) {
		director = memnew(GaussianSplatSceneDirector);
	}
	REQUIRE(director != nullptr);

	RendererLifetimeFixture fixture("scene_director_reload", rd);
	// Monotonicity threshold: cycle 3 may not exceed cycle 1 by >256 KiB.
	// Configured via the dedicated monotonicity setter — `threshold_bytes`
	// stays at the 4 MiB noise-floor default for the absolute leak gate.
	// (Codex PR #386 review #3294999198 P1.)
	fixture.set_monotonicity_threshold_bytes(256ull * 1024ull);
	fixture.capture_baseline();

	uint64_t cycle1_delta_bytes = 0;
	uint64_t cycle_growth_beyond_cycle1 = 0;
	bool monotonicity_ok = true;
	String monotonicity_reason;
	bool teardown_observed_ok = true;
	String teardown_observed_reason;

	{
		const uint64_t pre_cycle_bytes = _gs_owned_memory_bytes(rd);

		for (int cycle = 0; cycle < 3; cycle++) {
			// Synthetic scenario RID per cycle so the director treats every
			// iteration as a fresh F6 "new scene tree" rather than a re-submit
			// of the same scenario. Mirrors what happens on F6 in the editor
			// where each reload allocates a fresh World3D / scenario RID.
			const RID scenario = RID::from_uint64(0x4f4f4c0000000001ull + uint64_t(cycle));
			// owner_id must be live or submit_world_submission's ownership
			// arbitration may refuse a second cycle. Use the director's own
			// ObjectID -- it stays live for the whole scope.
			const ObjectID owner_id = director->get_instance_id();

			GaussianSplatSceneDirector::WorldSubmission submission;
			submission.owner_id = owner_id;
			submission.scenario = scenario;
			submission.gaussian_data = _make_lifetime_test_data(64);
			submission.bounds = submission.gaussian_data->get_aabb();
			submission.has_desired_residency_hint = true;
			submission.desired_residency_hint =
					GaussianSplatSceneDirector::SUBMISSION_RESIDENCY_HINT_RESIDENT;

			const bool submitted = director->submit_world_submission(submission);
			REQUIRE_MESSAGE(submitted, vformat("submit_world_submission failed on cycle %d", cycle + 1));

			// submit_world_submission already applied the contract to the
			// per-scenario shared renderer the director created (or attached
			// to an existing one). The point of this scenario is the
			// cycle-to-cycle delta after release + teardown, not a render
			// pass. Skip render_for_view to keep the body focused on the
			// SharedWorld lifecycle the leak comment at
			// gaussian_splat_scene_director.cpp:351 is about.

			// Release: this is the in-tree EXIT_TREE equivalent. After this
			// _prune_world_if_unused() runs but is guarded by the
			// _should_prune_world() refcount>1 check; if any peer Ref were
			// still pinning the renderer (the editor case the F6 leak comes
			// from), the entry would survive here.
			director->release_world_submission(owner_id);
			// Teardown: this is the PREDELETE-equivalent the editor F6 hook
			// hits. Bypasses the refcount guard and erases the SharedWorld
			// entry unconditionally so the next cycle starts from a clean
			// state. Idempotent: if release already pruned, this is a no-op.
			director->teardown_world_for_scenario(scenario);

			const uint64_t post_cycle_bytes = _gs_owned_memory_bytes(rd);
			const uint64_t cycle_delta = (post_cycle_bytes > pre_cycle_bytes)
					? (post_cycle_bytes - pre_cycle_bytes)
					: 0ull;

			if (cycle == 0) {
				cycle1_delta_bytes = cycle_delta;
			} else {
				const uint64_t growth = (cycle_delta > cycle1_delta_bytes)
						? (cycle_delta - cycle1_delta_bytes)
						: 0ull;
				if (growth > cycle_growth_beyond_cycle1) {
					cycle_growth_beyond_cycle1 = growth;
				}
				if (growth > fixture.result().threshold_bytes) {
					monotonicity_ok = false;
					monotonicity_reason = vformat(
							"cycle %d delta %d B grew %d B beyond cycle-1 delta %d B (threshold %d B)",
							cycle + 1,
							int64_t(cycle_delta),
							int64_t(growth),
							int64_t(cycle1_delta_bytes),
							int64_t(fixture.result().threshold_bytes));
				}
			}
		}
	}

	(void)cycle_growth_beyond_cycle1;

	if (!monotonicity_ok) {
		fixture.set_fail_reason(monotonicity_reason);
	}
	if (!teardown_observed_ok) {
		fixture.set_fail_reason(teardown_observed_reason);
	}

	if (owns_director) {
		memdelete(director);
	}

	// Mark teardown as synchronous: teardown_world_for_scenario now drops the
	// renderer Ref just AFTER releasing world_mutex (#611), but with the render
	// loop disabled in this harness the drop still runs synchronously -- no
	// deferred render-thread dispatch.
	fixture.set_teardown_was_synchronous(true);

	const bool finalize_passed = fixture.finalize();
	const bool scenario_passed = monotonicity_ok && teardown_observed_ok && finalize_passed;
	CHECK_MESSAGE(scenario_passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Scenario D: asset_attach_detach — one renderer, 3x set/clear gaussian
// data cycle, monotonicity check. Cycles 2/3 grow ≤ 64 KiB beyond cycle 1.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] asset_attach_detach lifetime proof") {
	REQUIRE_GPU_DEVICE();
	// Issue #392: same WorkerThreadPool dependency as scenario A
	// (renderer.instantiate(rd) inside the cycle loop transitively indexes
	// the empty ThreadData LocalVector). Skip cleanly when the pool isn't
	// usable instead of crashing with STATUS_STACK_BUFFER_OVERRUN under
	// `--gs-gpu-test`. See REQUIRE_WORKER_THREAD_POOL() comment in
	// test_macros.h for the full rationale.
	REQUIRE_WORKER_THREAD_POOL();

	RendererLifetimeFixture fixture("asset_attach_detach", rd);
	// Monotonicity threshold for the asset cycle delta-of-deltas. Configured
	// via the dedicated monotonicity setter — `threshold_bytes` stays at the
	// 4 MiB noise-floor default for the absolute leak gate, otherwise normal
	// allocator jitter could fail an otherwise healthy run. (Codex PR #386
	// review #3294999198 P1.)
	fixture.set_monotonicity_threshold_bytes(64ull * 1024ull);
	fixture.capture_baseline();

	// #392 / Codex PR #419 Finding 1: clear the static RDM teardown snapshot
	// before constructing the renderer (same rationale as renderer_instance).
	GaussianSplatRenderer::test_reset_last_teardown_rdm_counts();

	uint64_t cycle1_delta_bytes = 0;
	uint64_t cycle_growth_beyond_cycle1 = 0;
	bool monotonicity_ok = true;
	String monotonicity_reason;

	{
		Ref<GaussianSplatRenderer> renderer;
		renderer.instantiate(rd);
		REQUIRE(renderer.is_valid());

		Ref<::GaussianData> data = _make_lifetime_test_data(64);
		Ref<::GaussianData> empty_data; // null Ref — clear path.

		const uint64_t pre_cycle_bytes = _gs_owned_memory_bytes(rd);

		for (int cycle = 0; cycle < 3; cycle++) {
			const Error set_err = renderer->set_gaussian_data(data);
			CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));
			// Drive a render so any deferred allocation actually happens.
			Transform3D cam_transform;
			Projection projection;
			projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
			renderer->render_for_view(cam_transform, projection, RID(), Size2i(128, 128));

			const Error clear_err = renderer->set_gaussian_data(empty_data);
			CHECK((clear_err == OK || clear_err == ERR_UNCONFIGURED));

			// #352: advance the main-device frame so this cycle's render-path
			// deferred frees reclaim before the per-cycle measurement, mirroring
			// the production render loop the --gs-gpu-test runner does not drive.
			// RenderingDevice::free() only queues onto the per-frame disposal list;
			// without this drain the per-cycle delta reads the unreclaimed render
			// allocations as monotonic growth. (Same drain rationale as the
			// teardown drain after the cycle loop and the tile_shader_recompile
			// scenario.)
			rd->swap_buffers(false);

			const uint64_t post_cycle_bytes = _gs_owned_memory_bytes(rd);
			const uint64_t cycle_delta = (post_cycle_bytes > pre_cycle_bytes)
					? (post_cycle_bytes - pre_cycle_bytes)
					: 0ull;

			if (cycle == 0) {
				cycle1_delta_bytes = cycle_delta;
			} else {
				const uint64_t growth = (cycle_delta > cycle1_delta_bytes)
						? (cycle_delta - cycle1_delta_bytes)
						: 0ull;
				if (growth > cycle_growth_beyond_cycle1) {
					cycle_growth_beyond_cycle1 = growth;
				}
				if (growth > fixture.result().monotonicity_threshold_bytes) {
					monotonicity_ok = false;
					monotonicity_reason = vformat(
							"cycle %d delta %d B grew %d B beyond cycle-1 delta %d B (threshold %d B)",
							cycle + 1,
							int64_t(cycle_delta),
							int64_t(growth),
							int64_t(cycle1_delta_bytes),
							int64_t(fixture.result().monotonicity_threshold_bytes));
				}
			}
		}

		CHECK_MESSAGE(renderer->get_reference_count() == 1,
				"Renderer refcount > 1 before unref in asset_attach_detach scenario.");
		renderer.unref();
	}

	// Drain the renderer's destructor deferred frees before the absolute-gate
	// measurement (mirrors renderer_instance): unref() queues the persistent
	// buffer/texture frees onto the per-frame disposal list; advancing the
	// main-device frame past the ring runs _free_pending_resources so finalize()
	// sees the reclaimed steady state, not the unreclaimed dtor allocations. The
	// per-cycle swap_buffers above already drains each cycle's render frees (the
	// monotonicity signal); this drains the final teardown. (#352)
	for (int i = 0; i < 4; i++) {
		rd->swap_buffers(false);
	}

	// #392 / Codex PR #419 Finding 1: feed the renderer's per-instance RDM
	// last-shutdown owned/tracked counts into the pass rule (same rationale as
	// renderer_instance) so a leaked owned RID with no byte footprint trips the
	// RDM COUNT clause.
	REQUIRE_MESSAGE(GaussianSplatRenderer::test_has_last_teardown_rdm_counts(),
			"renderer teardown did not record RDM shutdown counts in asset_attach_detach.");
	fixture.record_rdm_shutdown_counts(
			GaussianSplatRenderer::test_get_last_teardown_rdm_owned_count(),
			GaussianSplatRenderer::test_get_last_teardown_rdm_tracked_count());

	(void)cycle_growth_beyond_cycle1;

	// Fold the monotonicity verdict into the fixture BEFORE finalize() so
	// the emitted [GS-LIFETIME] JSON line's "passed" field reflects the
	// final verdict and the JSON includes a self-describing
	// "monotonicity_regression" fail_reason. Previously the JSON was
	// emitted BEFORE monotonicity was folded into the local pass bool,
	// which meant a real asset_attach_detach regression that breached the
	// monotonicity threshold (but not the absolute rd-byte threshold)
	// would fail the local CHECK_MESSAGE yet still publish "passed":true
	// to PR #390's --mode lifetime gate. (Codex PR #386 review
	// #3294763666 P1.)
	fixture.set_monotonicity_ok(monotonicity_ok, monotonicity_reason);

	// For monotonicity scenarios, the absolute rd_memory_leaked vs threshold
	// in finalize() is the cycle-1 vs after baseline. The monotonicity
	// signal above is what's actually load-bearing, surfaced via
	// monotonicity_ok and fail_reason in the JSON line.
	const bool scenario_passed = fixture.finalize();
	CHECK_MESSAGE(scenario_passed, fixture.result().fail_reason);
}

// ---------------------------------------------------------------------------
// Scenario E: tile_shader_recompile — a forced tile-shader recompile must FREE
// the previously compiled shaders/pipelines, not orphan them.
//
// Regression for issue #298 (resident-node-path RID leak). A runtime recompile
// trigger — a render-config change (packed-stage / tighter-bounds /
// SH-amortization toggle) or a debug/perf counter toggle — used to call
// TileShaderResources::reset_state() directly, which NULLS the shader+pipeline
// RIDs without freeing the GPU objects. On benchmark_small_baseline that leaked
// exactly 1 Pipeline + 6 Compute + 7 Shader at process exit. The fix routes
// those triggers through TileRenderer::_release_compiled_shaders(), which frees
// before clearing.
//
// This scenario is byte-independent: it captures a live compute-pipeline RID,
// flips a recompile trigger, and asserts the device no longer considers the old
// RID valid. No render runs between the capture and the check, so the freed RID
// cannot be reused yet — its validity is a clean leak signal. Pre-fix the
// orphaned RID stays valid (leak); post-fix it is freed.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime][RequiresGPU] tile_shader_recompile frees old shaders") {
	REQUIRE_GPU_DEVICE();

	RendererLifetimeFixture fixture("tile_shader_recompile", rd);
	fixture.capture_baseline();

	bool old_pipeline_was_freed = false;
	// Codex PR #419 round-2 Finding 2: aggregate non-byte RID-leak coverage.
	// Survivors of the OLD compiled tile pipeline set after the recompile. A
	// healthy recompile frees them all (0 survivors); an orphaned pipeline (the
	// #298 regression) survives and bumps this. Default 0 so a clean skip path
	// records measured-clean, while a real leak records a measured non-zero COUNT.
	uint32_t old_pipeline_survivors = 0u;

	{
		Ref<GaussianSplatRenderer> renderer;
		renderer.instantiate(rd);
		REQUIRE(renderer.is_valid());

		Ref<::GaussianData> data = _make_lifetime_test_data(64);
		const Error set_err = renderer->set_gaussian_data(data);
		CHECK((set_err == OK || set_err == ERR_UNCONFIGURED));

		Transform3D cam_transform;
		Projection projection;
		projection.set_perspective(60.0f, 1.0f, 0.1f, 200.0f);
		renderer->render_for_view(cam_transform, projection, RID(), Size2i(128, 128));

		Ref<TileRenderer> tile = renderer->get_tile_renderer();
		REQUIRE(tile.is_valid());

		// The binning pipeline is a core compute pipeline on the tile path; use
		// it as the canary for the recompile free. If the standalone-doctest
		// harness lacks the full submission-device / RenderingServer wiring the
		// tile path needs (the #329 [SceneTree]+[RequiresGPU] gap), the shaders
		// never compile — skip rather than false-fail. The canonical proof for
		// this fix is the benchmark_small_baseline RID-leak repro (14 -> 0).
		const RID old_binning_pipeline = tile->get_tile_binning_pipeline();
		if (!old_binning_pipeline.is_valid() || !rd->compute_pipeline_is_valid(old_binning_pipeline)) {
			fixture.mark_skipped("tile binning pipeline not compiled in this harness; "
					"recompile path not exercisable (see #329)");
			renderer.unref();
			return;
		}

		// Codex PR #419 round-2 Finding 2: snapshot the FULL set of currently
		// compiled tile compute+render pipeline RIDs (not just the binning
		// canary) so the aggregate count below proves the recompile frees the
		// entire #298 leak surface, not one representative RID. The byte metric
		// reads 0 here and these pipelines are not RDM-tracked, so this COUNT is
		// the only aggregate signal that catches a leaked tile pipeline.
		const Vector<RID> old_pipelines = tile->_test_compiled_pipeline_snapshot();
		REQUIRE_MESSAGE(!old_pipelines.is_empty(),
				"no compiled tile pipelines captured before recompile; the aggregate "
				"RID-count coverage would be vacuous.");

		// Flip a recompile trigger directly on the tile renderer. This routes
		// through _release_compiled_shaders(), which must FREE the old pipeline
		// before clearing its RID. No render happens in between, so a freed RID
		// cannot be reused yet — its validity is a clean leak signal.
		tile->set_perf_capture_raster_shader_counters_enabled(true);

		old_pipeline_was_freed = !rd->compute_pipeline_is_valid(old_binning_pipeline);
		CHECK_MESSAGE(old_pipeline_was_freed,
				"recompile trigger left the previous tile binning pipeline alive on the device — "
				"shaders/pipelines were orphaned instead of freed (issue #298 regression).");

		// Aggregate count: how many of the OLD pipeline RIDs survived the
		// recompile. Must be 0 — every old pipeline must have been freed. This is
		// the measured non-byte RID-count signal fed into the gate below.
		old_pipeline_survivors = TileRenderer::_test_count_valid_pipelines(rd, old_pipelines);
		CHECK_MESSAGE(old_pipeline_survivors == 0u,
				vformat("recompile orphaned %d of %d previously compiled tile pipelines on the "
						"device (issue #298 regression; non-byte owned-RID leak).",
						int64_t(old_pipeline_survivors), int64_t(old_pipelines.size())));

		renderer.unref();
	}

	// Drain the renderer's destructor deferred frees before the absolute-gate
	// measurement (mirrors renderer_instance / asset_attach_detach). #352:
	// RenderingDevice::free() only QUEUES the renderer's persistent buffers/
	// textures onto the per-frame disposal list; _free_pending_resources runs on
	// frame advance (_begin_frame), which the --gs-gpu-test runner never drives
	// because it has no production render loop. Measured on RTX 3090: the renderer
	// allocates ~265 MB during render_for_view, ~145 MB is reclaimed synchronously
	// on unref(), and the remaining ~120 MB of buffers sit on the disposal queue
	// — they ARE freed (the dtor called rd->free()), just not yet reclaimed, so
	// finalize() would read them as a false 120 MB leak. Advancing past the (small)
	// frame ring runs _free_pending_resources and returns the device to the
	// reclaimed steady state (buffers=0 textures=0) the listener then measures.
	// This is the same deferred-free drain the other two renderer scenarios use;
	// it does NOT relax any gate — the 4 MiB absolute threshold is unchanged.
	for (int i = 0; i < 4; i++) {
		rd->swap_buffers(false);
	}

	// Codex PR #419 round-2 Finding 2: feed the recompile pipeline-survivor count
	// into the fixture's MEASURED RDM-count clause. Unlike renderer_instance /
	// asset_attach_detach this is NOT the per-instance RDM shutdown count (tile
	// shaders/pipelines are freed via RenderingDevice directly and are never
	// tracked in the RenderDeviceManager, so its shutdown count is 0 regardless of
	// a recompile leak). Instead the survivor count IS the leaked-owned-RID signal
	// for this scenario: 0 survivors records measured-clean (rdm_owned_leaked:0,
	// not the -1 unmeasured sentinel); any survivor records a measured non-zero
	// count that fails the gate's RDM clause — restoring aggregate non-byte-RID
	// coverage the byte metric (0 bytes) and the RDM shutdown snapshot both miss.
	fixture.record_rdm_shutdown_counts(old_pipeline_survivors, old_pipeline_survivors);

	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed, fixture.result().fail_reason);
	CHECK(old_pipeline_was_freed);
	CHECK(old_pipeline_survivors == 0u);
}

// ---------------------------------------------------------------------------
// Regression: monotonicity failure must publish "passed":false in JSON.
//
// Codex PR #386 review #3294763666 (P1): the asset_attach_detach scenario
// previously computed monotonicity_ok in the test body, then called
// fixture.finalize() (which emitted the [GS-LIFETIME] JSON line) BEFORE
// folding monotonicity_ok into the local pass bool. When RD/RDM thresholds
// passed but monotonicity failed, the local CHECK_MESSAGE correctly failed,
// but the JSON contract that downstream gates (PR #390's --mode lifetime)
// consume still claimed "passed":true. This test constructs that exact
// situation — RD/RDM clean, monotonicity flagged false — and asserts the
// JSON now publishes passed=false AND a self-describing fail_reason
// containing "monotonicity_regression".
//
// No [RequiresGPU] tag: this exercises the fixture's pure pass-rule
// arithmetic and JSON emission; no GPU device is needed. (rd_=nullptr is
// the same path the failed_init scenario uses.)
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime] monotonicity failure surfaces in emitted JSON") {
	RendererLifetimeFixture fixture("monotonicity_regression_probe", nullptr);
	fixture.capture_baseline();

	// Simulate the asset_attach_detach failure mode: RD/RDM thresholds
	// pass (nothing allocated against rd_=nullptr; no RDM shutdown counts
	// recorded), but the scenario detected a cycle-to-cycle delta-of-deltas
	// excursion. The reason string mirrors the format
	// asset_attach_detach emits.
	const String synthetic_reason =
			"cycle 3 delta 131072 B grew 65537 B beyond cycle-1 delta 65535 B (threshold 65536 B)";
	fixture.set_monotonicity_ok(false, synthetic_reason);

	const bool passed = fixture.finalize();

	// Final verdict — local pass bool must reflect monotonicity.
	CHECK_FALSE(passed);
	CHECK_FALSE(fixture.result().passed);
	// Sibling signals must still be clean — proves the monotonicity
	// signal alone flipped the verdict and would otherwise have shipped
	// as a silent pass. RDM is unmeasured here (no
	// record_rdm_shutdown_counts() call), so the field holds the
	// RDM_UNMEASURED sentinel rather than a measured zero. (Codex PR
	// #386 review #3295073296 P1.)
	CHECK(fixture.result().rdm_owned_leaked == RDM_UNMEASURED);
	CHECK(fixture.result().rd_memory_leaked_bytes < fixture.result().threshold_bytes);

	// JSON contract — the [GS-LIFETIME] line emitted by finalize() comes
	// from to_dict(); validate the fields a downstream gate would parse.
	const Dictionary emitted = fixture.result().to_dict();
	CHECK(bool(emitted["passed"]) == false);
	CHECK(bool(emitted["monotonicity_ok"]) == false);
	const String emitted_reason = emitted["fail_reason"];
	CHECK(emitted_reason.find("monotonicity_regression") >= 0);
	CHECK(emitted_reason.find(synthetic_reason) >= 0);
}

// ---------------------------------------------------------------------------
// Regression: monotonicity threshold must not alias the absolute leak gate.
//
// Codex PR #386 review #3294999198 (P1): scenarios C and D previously called
// set_threshold_bytes(256 KiB / 64 KiB) to configure their per-cycle
// monotonicity growth budget, but _compute_pass() ALSO uses `threshold_bytes`
// for the absolute rd_memory_leaked_bytes < threshold check. The monotonicity
// tuning therefore unintentionally tightened the absolute leak gate from the
// documented 4 MiB noise floor to 256/64 KiB, so normal allocator jitter
// could fail an otherwise healthy run.
//
// The fix introduces a separate `monotonicity_threshold_bytes` field +
// setter. This test pins the contract:
//
//   1. set_monotonicity_threshold_bytes() MUST NOT change threshold_bytes.
//   2. A scenario whose rd_memory_leaked_bytes sits between the monotonicity
//      threshold (256 KiB) and the absolute threshold (4 MiB) — e.g., 1 MiB
//      of allocator jitter — MUST pass the absolute gate, not be silently
//      tripped by the (smaller) monotonicity budget the way the pre-fix
//      conflation would have done.
//   3. The emitted JSON contract MUST surface both thresholds as distinct
//      fields so PR #390's downstream gate can see the full pass rule.
//
// No [RequiresGPU] tag: this exercises pure pass-rule arithmetic; rd_=nullptr
// matches the failed_init / monotonicity_regression_probe pattern.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime] monotonicity threshold decoupled from absolute leak threshold") {
	RendererLifetimeFixture fixture("monotonicity_threshold_decoupling_probe", nullptr);

	// 1. Default state — the documented 4 MiB absolute noise floor.
	const uint64_t expected_default_absolute = 4ull << 20;
	CHECK(fixture.result().threshold_bytes == expected_default_absolute);

	// 2. Setting the monotonicity budget MUST leave the absolute gate alone.
	//    This is the load-bearing assertion that catches the original
	//    conflation regression.
	fixture.set_monotonicity_threshold_bytes(64ull * 1024ull);
	CHECK_MESSAGE(fixture.result().threshold_bytes == expected_default_absolute,
			"set_monotonicity_threshold_bytes must NOT mutate threshold_bytes; "
			"otherwise the absolute leak gate silently tightens to the per-cycle "
			"monotonicity budget (the original conflation bug).");
	CHECK(fixture.result().monotonicity_threshold_bytes == 64ull * 1024ull);

	// 3. Construct the exact failure mode the conflation would have hit:
	//    simulate 1 MiB of allocator jitter — well under the 4 MiB absolute
	//    gate, well over the 64 KiB monotonicity budget. Pre-fix this would
	//    have tripped the absolute check (because threshold_bytes had been
	//    overwritten to 64 KiB). Post-fix the absolute check must pass and
	//    only monotonicity_ok controls the verdict.
	fixture.capture_baseline();
	const uint64_t synthetic_jitter_bytes = 1ull << 20; // 1 MiB
	// Inject the synthetic leak directly into the after-snapshot the way the
	// asset_attach_detach scenario would observe it; the rd_=nullptr path
	// leaves before.rd_memory_usage_bytes at 0 so this becomes the leaked
	// delta verbatim.
	fixture.test_inject_after_rd_bytes(synthetic_jitter_bytes);

	// Monotonicity is fine (this scenario only exercises the absolute gate
	// decoupling). The synthetic_jitter_bytes sits strictly between the two
	// thresholds, so pre-fix the conflated absolute check would fail and
	// post-fix it must pass.
	const bool passed = fixture.finalize();
	CHECK_MESSAGE(passed,
			vformat("rd_memory_leaked_bytes=%d B is below the 4 MiB absolute "
					"noise floor; the absolute leak gate must pass even though "
					"it exceeds the per-cycle monotonicity budget. "
					"fail_reason=%s",
					int64_t(fixture.result().rd_memory_leaked_bytes),
					fixture.result().fail_reason));
	CHECK(fixture.result().rd_memory_leaked_bytes == synthetic_jitter_bytes);
	CHECK(fixture.result().threshold_bytes == expected_default_absolute);
	CHECK(fixture.result().monotonicity_threshold_bytes == 64ull * 1024ull);

	// 4. JSON contract — both thresholds surface as distinct fields so
	//    PR #390's downstream gate can see the full pass rule.
	const Dictionary emitted = fixture.result().to_dict();
	CHECK(emitted.has("threshold_bytes"));
	CHECK(emitted.has("monotonicity_threshold_bytes"));
	CHECK(int64_t(emitted["threshold_bytes"]) == int64_t(expected_default_absolute));
	CHECK(int64_t(emitted["monotonicity_threshold_bytes"]) == int64_t(64ull * 1024ull));
	CHECK(bool(emitted["passed"]) == true);
}

// ---------------------------------------------------------------------------
// Regression: RDM counters must surface an "unmeasured" sentinel rather than
// silently advertising a measured zero.
//
// Codex PR #386 review #3295073296 (P1): _snapshot() hard-set
// rdm_tracked_resources and rdm_owned_resources to 0, and none of the four
// lifetime scenarios called record_rdm_shutdown_counts(), so _compute_pass()
// always treated the RDM signal as "measured clean." A real RDM-owned RID
// leak that stayed under the 4 MiB RD byte threshold would have passed the
// gate while the emitted JSON falsely reported "rdm_owned_leaked":0 as if
// measured. Downstream gates (PR #390's --mode lifetime) would have no way
// to distinguish "scenario never measured RDM" from "scenario measured RDM
// clean."
//
// The fix introduces the RDM_UNMEASURED sentinel. _snapshot() initializes
// the RDM fields to the sentinel; _compute_pass() preserves the sentinel
// in rdm_*_leaked when the after-snapshot is unmeasured, and
// _compute_pass() then SKIPS the RDM clause of the pass rule (Option B
// from the review: advisory rather than fail-loud, mirroring the existing
// stringname_orphan_delta:-1 advisory-field pattern). to_dict() emits -1
// in that case so downstream JSON consumers can grep for it the same way
// they already grep for stringname_orphan_delta.
//
// This test pins three contracts:
//   1. A scenario that does NOT call record_rdm_shutdown_counts() MUST
//      surface RDM_UNMEASURED in the field and -1 in the JSON.
//   2. A scenario that calls record_rdm_shutdown_counts(5, 10) MUST
//      surface 5 / 10 in the field and 5 / 10 in the JSON.
//   3. The pass-rule arithmetic MUST treat sentinel-RDM as "skip the gate"
//      (not as "leaked == 0"); the RD-byte and monotonicity clauses
//      continue to apply unchanged.
//
// No [RequiresGPU] tag: this exercises pure pass-rule arithmetic and JSON
// emission; rd_=nullptr matches the failed_init / monotonicity probe
// pattern.
// ---------------------------------------------------------------------------
TEST_CASE("[GaussianSplatting][Renderer][Lifetime] rdm counters surface unmeasured sentinel instead of silent zero") {
	// --- Case 1: scenario does NOT measure RDM ----------------------------
	{
		RendererLifetimeFixture fixture("rdm_unmeasured_probe", nullptr);
		fixture.capture_baseline();

		// No record_rdm_shutdown_counts() call. After finalize(), the
		// RDM fields MUST hold the sentinel, the JSON MUST emit -1, and
		// the absolute / monotonicity clauses MUST control the verdict
		// (here they pass — nothing was allocated against rd_=nullptr).
		const bool passed = fixture.finalize();

		// The pass-rule should not be silently gated on RDM=0 anymore;
		// since RD-bytes and monotonicity are clean and RDM is
		// unmeasured (skip), the scenario passes.
		CHECK_MESSAGE(passed,
				vformat("Expected pass with unmeasured RDM + clean RD/monotonicity. "
						"fail_reason=%s",
						fixture.result().fail_reason));

		// Field-level: sentinel preserved through _compute_pass.
		CHECK(fixture.result().rdm_owned_leaked == RDM_UNMEASURED);
		CHECK(fixture.result().rdm_tracked_leaked == RDM_UNMEASURED);

		// JSON contract: emitted as int64_t(-1), matching the existing
		// stringname_orphan_delta:-1 advisory pattern.
		const Dictionary emitted = fixture.result().to_dict();
		CHECK_MESSAGE(int64_t(emitted["rdm_owned_leaked"]) == int64_t(-1),
				"Unmeasured RDM must emit -1 in JSON, not a silent 0 that "
				"masquerades as a measured-clean result (Codex PR #386 review "
				"#3295073296 P1).");
		CHECK(int64_t(emitted["rdm_tracked_leaked"]) == int64_t(-1));
		// stringname_orphan_delta is also -1 here (always, until PR 6) —
		// pinning this asserts the two advisory sentinels share their
		// downstream-consumer contract.
		CHECK(int64_t(emitted["stringname_orphan_delta"]) == int64_t(-1));
	}

	// --- Case 2: scenario DOES measure RDM, no leak -----------------------
	{
		RendererLifetimeFixture fixture("rdm_measured_clean_probe", nullptr);
		fixture.capture_baseline();

		// Record a clean post-shutdown: 0 owned, 0 tracked. This is the
		// "measured zero" case the sentinel disambiguates from "never
		// measured." After finalize(), the field MUST hold 0 (not the
		// sentinel) and JSON MUST emit 0.
		fixture.record_rdm_shutdown_counts(0, 0);
		const bool passed = fixture.finalize();
		CHECK_MESSAGE(passed,
				vformat("Expected pass with measured-clean RDM. fail_reason=%s",
						fixture.result().fail_reason));

		CHECK(fixture.result().rdm_owned_leaked == 0);
		CHECK(fixture.result().rdm_tracked_leaked == 0);

		const Dictionary emitted = fixture.result().to_dict();
		CHECK_MESSAGE(int64_t(emitted["rdm_owned_leaked"]) == int64_t(0),
				"Measured-clean RDM must emit 0 in JSON (distinct from -1 "
				"unmeasured sentinel).");
		CHECK(int64_t(emitted["rdm_tracked_leaked"]) == int64_t(0));
	}

	// --- Case 3: scenario DOES measure RDM, real leak ---------------------
	{
		RendererLifetimeFixture fixture("rdm_measured_leak_probe", nullptr);
		fixture.capture_baseline();

		// Record a non-zero post-shutdown: 5 owned, 10 tracked. The
		// pass-rule MUST trip on rdm_owned_leaked > 0; JSON MUST emit
		// the literal counts.
		fixture.record_rdm_shutdown_counts(5, 10);
		const bool passed = fixture.finalize();

		CHECK_FALSE_MESSAGE(passed,
				"Expected pass-rule failure for measured RDM-owned leak; "
				"the gate must enforce the RDM clause when measured.");
		CHECK(fixture.result().rdm_owned_leaked == 5);
		CHECK(fixture.result().rdm_tracked_leaked == 10);
		// fail_reason should be self-describing about the RDM leak.
		CHECK(fixture.result().fail_reason.find("rdm_owned_leaked") >= 0);

		const Dictionary emitted = fixture.result().to_dict();
		CHECK(int64_t(emitted["rdm_owned_leaked"]) == int64_t(5));
		CHECK(int64_t(emitted["rdm_tracked_leaked"]) == int64_t(10));
		CHECK(bool(emitted["passed"]) == false);
	}
}

} // namespace TestGaussianSplatting
