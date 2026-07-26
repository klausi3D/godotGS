extends RefCounted

# Shared runtime budget contract for the streaming GPU stress gate.
# Sort-evidence checks intentionally stay in test_gpu_streaming_stress.gd's
# GPU-backed _validate_sort_metrics() path; this helper only owns the tier
# budget and telemetry invariants evaluated from already-collected metrics.
#
# ---------------------------------------------------------------------------
# Deterministic frame-scaling protocol (#630)
# ---------------------------------------------------------------------------
# The absolute per-tier `max_frame_p95_ms` ceiling below is a *gross-regression
# backstop* only. On the shared self-hosted runner it is non-deterministic:
# local/agent builds add a roughly constant common-mode offset C to every
# tier's wall-clock frame time, and a fixed wall-clock threshold flips on C
# rather than on renderer physics (#630 measured a 1.94x pass<->fail swing on a
# printf-only diff, and docs-only PRs failing the gate). C swamps the workload
# signal, so under contention the absolute gate is a coin flip in BOTH
# directions: it false-fails clean code and cannot see a real regression.
#
# The contention-robust signal is the *cross-tier differential*, because the
# common-mode offset cancels in a subtraction. Model each tier as
#
#     observed_p95(tier) ~= C + k * size(tier)          (k = per-splat cost)
#
# so the marginal per-splat cost between the smallest and largest tier
#
#     marginal = (p95(large) - p95(small)) / (size(large) - size(small))
#
# is invariant to C to first order: a re-run under different runner load (a
# different C) yields the same marginal, while a genuine per-splat regression
# raises it. `evaluate_scaling_regression()` turns raw per-tier p95 samples into
# two deterministic verdicts:
#
#   * scale-sanity: if the tiers fail to separate (ratio < floor), the run is
#     common-mode dominated and the measurement is INVALID -> "inconclusive"
#     (re-queue in a quiet window), NEVER a silent pass and NEVER a hard fail.
#     This is the #630 "convert a silent false-negative into a loud invalid
#     run" item; it is cheap and needs no wall-clock calibration.
#   * marginal-cost regression: only evaluated once scaling is healthy (so it
#     cannot fire on contention), and offset-invariant (so re-runs on unchanged
#     code do not trip). A gross per-splat regression raises the marginal beyond
#     a generous, deliberately-conservative backstop baseline -> "regression".
#
# Repeatability protocol used by the live gate (test_gpu_streaming_stress.gd):
#   1. 3 warm-up frames stabilise GPU timers before any sample is retained.
#   2. SAMPLE_FRAMES raw per-frame samples are captured.
#   3. `trim_warmup()` discards the first WARMUP_DISCARD_SAMPLES of the retained
#      window (residual ramp) BEFORE avg/p95 are computed. This is a noise
#      reduction on the sample window, NOT a threshold change.
#   4. The absolute ceiling stays as a gross backstop; the deterministic verdict
#      comes from the cross-tier differential above.
#
# The concrete thresholds here are conservative backstops, not a tight envelope.
# Tightening them (and moving the gate to an optimized build) is deferred to a
# dedicated calibration PR, exactly as the `max_frame_p95_ms` comment already
# prescribes. The determinism of the *algorithm* (offset-invariance, regression
# sensitivity, invalid-run detection) is proven independently of these numbers
# by test_streaming_gpu_tier_budget_contract.gd, which supplies its own config.

# Retained raw samples discarded as residual warm-up before avg/p95 (step 3).
const WARMUP_DISCARD_SAMPLES := 12
# Never trim below this many retained samples; a short window keeps its samples
# rather than emptying the percentile input.
const WARMUP_MIN_RETAINED_SAMPLES := 30
# Scale-sanity floor: p95(largest tier) / p95(smallest tier) must exceed this
# for the measurement to be trusted. Healthy clean master evidence separated
# ~4.97x (250k->2.5M); contended runs collapsed to ~1.06-1.12x (#630 Evidence
# 3). 1.20 sits below the healthiest observed contended cluster and above it;
# it is a floor on measurement validity, not a product target. Widen after
# optimized-build calibration.
const SCALE_SANITY_MIN_RATIO := 1.20
# Backstop baseline for the per-splat marginal cost, in milliseconds per one
# million splats, measured across the smallest<->largest tier span. Clean dev
# evidence sat well under this (master@base ~258 ms/Msplat on the noisy 2.5M
# tier). Deliberately generous so it never false-fails a healthy run; a
# dedicated calibration PR tightens it against several clean optimized runs.
const MARGINAL_COST_BASELINE_MS_PER_MSPLAT := 320.0
# Fractional headroom over the baseline before a healthy-scaling run is called a
# regression (0.5 => trips at 1.5x baseline).
const MARGINAL_COST_REGRESSION_TOLERANCE := 0.5


static func tier_1m_budget() -> Dictionary:
	return {
		"name": "tier_1m",
		"size": 1000000,
		"max_first_visible_ms": 3500.0,
		"min_residency_ratio": 0.75,
		# Runner-specific end-to-end guardrail, not a renderer product target.
		# Current clean Windows Vulkan evidence has tier_1m p95 at 236.772 ms
		# (PR #361 run 26112198038), 240.731 ms (master run 26095036307),
		# 292.4 ms (checked-in report), and 304.903 ms (PR #351 run
		# 26112075257) with residency=1.0, fallback_rate=0.0, and telemetry
		# present. 325 ms stays above that current self-hosted runner envelope
		# while preserving a hard ceiling for gross regressions. Tighten this in
		# a dedicated calibration PR after several clean master runs establish a
		# lower stable envelope.
		"max_frame_p95_ms": 325.0,
		"max_frame_p95_to_avg_ratio": 2.25,
		"max_fallback_rate": 0.35,
		"enforce": true
	}


static func evaluate_tier_budget(tier: Dictionary, tier_result: Dictionary, residency: Dictionary) -> Dictionary:
	var budget_failures: Array[String] = []
	var telemetry_failures: Array[String] = []
	var within_budget := true

	var first_visible_ms := float(tier_result.get("first_visible_ms", -1.0))
	var residency_ratio := float(residency.get("residency_ratio", 0.0))
	var frame_p95_ms := float(tier_result.get("frame_p95_ms", 0.0))
	var frame_p95_to_avg_ratio := float(tier_result.get("frame_p95_to_avg_ratio", 0.0))
	var source_data_available := bool(tier_result.get("source_data_available", false))
	var fallback_rate_available := bool(tier_result.get("fallback_rate_available", false))
	var fallback_rate: Variant = tier_result.get("fallback_rate", null)

	var max_first_visible_ms := float(tier.get("max_first_visible_ms", 0.0))
	if first_visible_ms < 0.0:
		within_budget = false
		budget_failures.append("first_visible_missing")
	elif max_first_visible_ms > 0.0 and first_visible_ms > max_first_visible_ms:
		within_budget = false
		budget_failures.append("first_visible_exceeded")

	var min_residency_ratio := float(tier.get("min_residency_ratio", 0.0))
	if residency_ratio < min_residency_ratio:
		within_budget = false
		budget_failures.append("residency_ratio_low")

	var max_frame_p95_ms := float(tier.get("max_frame_p95_ms", 0.0))
	if max_frame_p95_ms > 0.0 and frame_p95_ms > max_frame_p95_ms:
		within_budget = false
		budget_failures.append("frame_p95_exceeded")
	var max_frame_p95_to_avg_ratio := float(tier.get("max_frame_p95_to_avg_ratio", 0.0))
	if max_frame_p95_to_avg_ratio > 0.0 and frame_p95_to_avg_ratio > max_frame_p95_to_avg_ratio:
		within_budget = false
		budget_failures.append("frame_p95_to_avg_ratio_high")

	var max_fallback_rate := float(tier.get("max_fallback_rate", 1.0))
	if not source_data_available:
		within_budget = false
		telemetry_failures.append("fallback_source_data_missing")
	elif not fallback_rate_available:
		within_budget = false
		telemetry_failures.append("fallback_rate_missing")
	elif fallback_rate == null:
		within_budget = false
		telemetry_failures.append("fallback_rate_missing")
	elif float(fallback_rate) > max_fallback_rate:
		within_budget = false
		budget_failures.append("fallback_rate_high")

	return {
		"within_budget": within_budget,
		"budget_failures": budget_failures,
		"telemetry_failures": telemetry_failures
	}


# Discards the first `discard` retained samples (residual warm-up) so the
# absolute-p95 backstop is computed over a steadier window. Refuses to trim when
# too few samples would remain, so a short capture keeps its data intact. Pure
# and deterministic; returns a NEW array and never mutates the input.
static func trim_warmup(samples: Array, discard: int = WARMUP_DISCARD_SAMPLES) -> Array:
	if discard <= 0:
		return samples.duplicate()
	if samples.size() - discard < WARMUP_MIN_RETAINED_SAMPLES:
		return samples.duplicate()
	return samples.slice(discard)


# Contention-robust cross-tier regression verdict (#630).
#
# `samples` is an Array of Dictionaries, one per tier, each carrying at least
# "name", "size" (splat count) and "frame_p95_ms". `config` optionally overrides
# the module backstop constants ("scale_sanity_min_ratio",
# "marginal_baseline_ms_per_msplat", "marginal_tolerance") so callers and the
# contract test can pin an exact, self-contained boundary.
#
# Verdicts:
#   "insufficient_data" - fewer than two usable tiers, or no size span.
#   "inconclusive"      - tiers did not separate (larger tier not slower, or
#                         ratio below the scale-sanity floor). The measurement is
#                         common-mode dominated and INVALID: re-queue, do not
#                         pass silently and do not hard-fail.
#   "regression"        - scaling is healthy AND the per-splat marginal cost
#                         exceeded the backstop threshold. This is the only
#                         blocking verdict, and it is offset-invariant.
#   "pass"              - scaling healthy and marginal within budget.
static func evaluate_scaling_regression(samples: Array, config: Dictionary = {}) -> Dictionary:
	var min_ratio := float(config.get("scale_sanity_min_ratio", SCALE_SANITY_MIN_RATIO))
	var baseline := float(config.get("marginal_baseline_ms_per_msplat", MARGINAL_COST_BASELINE_MS_PER_MSPLAT))
	var tolerance := float(config.get("marginal_tolerance", MARGINAL_COST_REGRESSION_TOLERANCE))
	var threshold := baseline * (1.0 + tolerance)

	var result := {
		"verdict": "insufficient_data",
		"reason": "",
		"scale_ratio": 0.0,
		"marginal_ms_per_msplat": 0.0,
		"marginal_baseline_ms_per_msplat": baseline,
		"marginal_threshold_ms_per_msplat": threshold,
		"scale_sanity_min_ratio": min_ratio,
		"small_tier": {},
		"large_tier": {},
		"usable_samples": 0,
		"blocking": false
	}

	# Keep only tiers with a positive size and a positive measured p95.
	var usable: Array = []
	for entry in samples:
		if not (entry is Dictionary):
			continue
		var size := int(entry.get("size", 0))
		var p95 := float(entry.get("frame_p95_ms", 0.0))
		if size > 0 and p95 > 0.0:
			usable.append({
				"name": String(entry.get("name", "tier_unknown")),
				"size": size,
				"frame_p95_ms": p95
			})
	result["usable_samples"] = usable.size()

	if usable.size() < 2:
		result["reason"] = "fewer than two usable tiers with positive size and p95"
		return result

	var small: Dictionary = usable[0]
	var large: Dictionary = usable[0]
	for entry in usable:
		if int(entry["size"]) < int(small["size"]):
			small = entry
		if int(entry["size"]) > int(large["size"]):
			large = entry

	var size_span := int(large["size"]) - int(small["size"])
	if size_span <= 0:
		result["reason"] = "no size span between smallest and largest tier"
		return result

	result["small_tier"] = small
	result["large_tier"] = large

	var small_p95 := float(small["frame_p95_ms"])
	var large_p95 := float(large["frame_p95_ms"])
	var d_p95 := large_p95 - small_p95
	var scale_ratio := large_p95 / small_p95
	var span_msplat := float(size_span) / 1000000.0
	var marginal := d_p95 / span_msplat
	result["scale_ratio"] = scale_ratio
	result["marginal_ms_per_msplat"] = marginal

	if d_p95 <= 0.0:
		result["verdict"] = "inconclusive"
		result["reason"] = "larger tier (%s) is not slower than smaller tier (%s); measurement is noise/common-mode dominated, re-run in a quiet window" % [String(large["name"]), String(small["name"])]
		return result

	if scale_ratio < min_ratio:
		result["verdict"] = "inconclusive"
		result["reason"] = "scale ratio %.3f below floor %.3f; common-mode runner load dominates the workload signal, re-run in a quiet window" % [scale_ratio, min_ratio]
		return result

	if marginal > threshold:
		result["verdict"] = "regression"
		result["blocking"] = true
		result["reason"] = "per-splat marginal cost %.1f ms/Msplat exceeds backstop %.1f ms/Msplat (baseline %.1f x %.2f) with healthy scaling %.2fx" % [marginal, threshold, baseline, 1.0 + tolerance, scale_ratio]
		return result

	result["verdict"] = "pass"
	result["reason"] = "healthy scaling %.2fx, per-splat marginal cost %.1f ms/Msplat within backstop %.1f ms/Msplat" % [scale_ratio, marginal, threshold]
	return result
