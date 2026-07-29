extends SceneTree

const FAIL_MARKER := "[RUNTIME_FAIL]"
const METRICS_MARKER := "[RUNTIME_METRICS]"
const StreamingGpuTierBudget = preload("streaming_gpu_tier_budget.gd")

var failures: Array[String] = []


func _init() -> void:
	call_deferred("_run")


func _record_failure(reason: String, context: Dictionary = {}) -> void:
	var message := reason
	if not context.is_empty():
		message = "%s | context=%s" % [reason, str(context)]
	failures.append(message)
	push_error("%s %s" % [FAIL_MARKER, message])


func _base_tier_result() -> Dictionary:
	return {
		"first_visible_ms": 240.0,
		# #796: 292.4 -> 152.3, the same 1.92x rescale applied to the tier ceilings.
		# 292.4 was a recorded run of the OLD frame metric, which spanned the
		# harness's force_sort_for_view() and get_render_stats() as well as the
		# engine frame. Against the corrected engine-frame-only ceiling (170.0) the
		# old figure reads as a failure, so every case built on this base -- all of
		# which expect a clean frame budget and vary one other field -- began
		# reporting a spurious frame_p95_exceeded. This test correctly caught the
		# metric's change of meaning; the fixture had to follow it.
		"frame_p95_ms": 152.3,
		"frame_p95_to_avg_ratio": 1.24,
		"source_data_available": true,
		"fallback_rate_available": true,
		"fallback_rate": 0.0,
		"source_frame_count": 120
	}


func _base_residency() -> Dictionary:
	return {
		"residency_ratio": 1.0
	}


func _merge(base: Dictionary, overrides: Dictionary) -> Dictionary:
	var merged := base.duplicate(true)
	for key in overrides.keys():
		merged[key] = overrides[key]
	return merged


func _array_equals(actual: Array, expected: Array) -> bool:
	if actual.size() != expected.size():
		return false
	for index in range(actual.size()):
		if String(actual[index]) != String(expected[index]):
			return false
	return true


func _run_case(test_case: Dictionary) -> void:
	var tier := StreamingGpuTierBudget.tier_1m_budget()
	tier = _merge(tier, test_case.get("tier_overrides", {}))
	var tier_result := _merge(_base_tier_result(), test_case.get("result_overrides", {}))
	var residency := _merge(_base_residency(), test_case.get("residency_overrides", {}))

	var evaluation := StreamingGpuTierBudget.evaluate_tier_budget(tier, tier_result, residency)
	var expected_within := bool(test_case.get("within_budget", false))
	var actual_within := bool(evaluation.get("within_budget", false))
	if actual_within != expected_within:
		_record_failure("Budget contract within_budget mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_within,
			"actual": actual_within,
			"evaluation": evaluation
		})

	var expected_budget_failures: Array = test_case.get("budget_failures", [])
	var actual_budget_failures: Array = evaluation.get("budget_failures", [])
	if not _array_equals(actual_budget_failures, expected_budget_failures):
		_record_failure("Budget contract budget_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_budget_failures,
			"actual": actual_budget_failures,
			"evaluation": evaluation
		})

	var expected_telemetry_failures: Array = test_case.get("telemetry_failures", [])
	var actual_telemetry_failures: Array = evaluation.get("telemetry_failures", [])
	if not _array_equals(actual_telemetry_failures, expected_telemetry_failures):
		_record_failure("Budget contract telemetry_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_telemetry_failures,
			"actual": actual_telemetry_failures,
			"evaluation": evaluation
		})


## Explicit, self-contained config so the scaling cases pin an exact boundary
## independent of the (deliberately generous, calibration-deferred) module
## backstop constants. threshold = baseline * (1 + tolerance) = 150 ms/Msplat.
func _scaling_config() -> Dictionary:
	return {
		"scale_sanity_min_ratio": 1.5,
		"marginal_baseline_ms_per_msplat": 100.0,
		"marginal_tolerance": 0.5
	}


func _run_scaling_case(test_case: Dictionary) -> void:
	var samples: Array = test_case.get("samples", [])
	var config: Dictionary = test_case.get("config", _scaling_config())
	var evaluation := StreamingGpuTierBudget.evaluate_scaling_regression(samples, config)

	var expected_verdict := String(test_case.get("expect_verdict", ""))
	var actual_verdict := String(evaluation.get("verdict", ""))
	if actual_verdict != expected_verdict:
		_record_failure("Scaling contract verdict mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_verdict,
			"actual": actual_verdict,
			"evaluation": evaluation
		})

	var expected_blocking := bool(test_case.get("expect_blocking", false))
	var actual_blocking := bool(evaluation.get("blocking", false))
	if actual_blocking != expected_blocking:
		_record_failure("Scaling contract blocking mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_blocking,
			"actual": actual_blocking,
			"evaluation": evaluation
		})

	if test_case.has("expect_marginal"):
		var expected_marginal := float(test_case["expect_marginal"])
		var actual_marginal := float(evaluation.get("marginal_ms_per_msplat", 0.0))
		if not is_equal_approx(expected_marginal, actual_marginal):
			_record_failure("Scaling contract marginal mismatch", {
				"case": test_case.get("name", "<unnamed>"),
				"expected": expected_marginal,
				"actual": actual_marginal,
				"evaluation": evaluation
			})


## The #630 determinism property, stated loudly: two runs of the SAME workload
## that differ only by a constant common-mode offset added to every tier (the
## exact effect of shared-runner load that made the old absolute-p95 gate swing
## ~1.94x) must yield the IDENTICAL marginal cost and the IDENTICAL verdict. An
## absolute-threshold gate fails this; the differential gate does not.
func _run_offset_invariance_check() -> void:
	var config := _scaling_config()
	var clean := [
		{"name": "tier_250k", "size": 250000, "frame_p95_ms": 100.0},
		{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 160.0},
		{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 280.0}
	]
	var offset := 120.0
	var contended: Array = []
	for entry in clean:
		contended.append({
			"name": entry["name"],
			"size": entry["size"],
			"frame_p95_ms": float(entry["frame_p95_ms"]) + offset
		})

	var clean_eval := StreamingGpuTierBudget.evaluate_scaling_regression(clean, config)
	var contended_eval := StreamingGpuTierBudget.evaluate_scaling_regression(contended, config)

	if String(clean_eval.get("verdict", "")) != String(contended_eval.get("verdict", "")):
		_record_failure("Offset-invariance verdict drift", {
			"clean": clean_eval,
			"contended": contended_eval
		})
	if not is_equal_approx(
		float(clean_eval.get("marginal_ms_per_msplat", -1.0)),
		float(contended_eval.get("marginal_ms_per_msplat", -2.0))
	):
		_record_failure("Offset-invariance marginal drift", {
			"clean_marginal": clean_eval.get("marginal_ms_per_msplat"),
			"contended_marginal": contended_eval.get("marginal_ms_per_msplat")
		})


func _make_ramp_samples(count: int) -> Array:
	var samples: Array = []
	for i in range(count):
		samples.append(float(i))
	return samples


func _run_trim_warmup_checks() -> void:
	# Long window: trims exactly WARMUP_DISCARD_SAMPLES from the front.
	var long_window := _make_ramp_samples(120)
	var trimmed := StreamingGpuTierBudget.trim_warmup(long_window)
	var discard := StreamingGpuTierBudget.WARMUP_DISCARD_SAMPLES
	if trimmed.size() != 120 - discard:
		_record_failure("trim_warmup long-window size", {"expected": 120 - discard, "actual": trimmed.size()})
	elif not is_equal_approx(float(trimmed[0]), float(discard)):
		_record_failure("trim_warmup long-window offset", {"expected": float(discard), "actual": trimmed[0]})
	if long_window.size() != 120:
		_record_failure("trim_warmup mutated input", {"actual": long_window.size()})

	# Short window: fewer than WARMUP_MIN_RETAINED_SAMPLES would remain, so the
	# full capture is kept rather than emptied.
	var short_window := _make_ramp_samples(20)
	var short_trimmed := StreamingGpuTierBudget.trim_warmup(short_window)
	if short_trimmed.size() != 20:
		_record_failure("trim_warmup short-window keeps all", {"expected": 20, "actual": short_trimmed.size()})

	# discard <= 0 is a no-op copy.
	var zero_trimmed := StreamingGpuTierBudget.trim_warmup(_make_ramp_samples(50), 0)
	if zero_trimmed.size() != 50:
		_record_failure("trim_warmup zero discard", {"expected": 50, "actual": zero_trimmed.size()})


func _run_scaling_cases() -> Array:
	var scaling_cases := [
		{
			# Clean, well-separated tiers: marginal 80 ms/Msplat < 150 threshold.
			"name": "healthy_pass",
			"samples": [
				{"name": "tier_250k", "size": 250000, "frame_p95_ms": 100.0},
				{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 160.0},
				{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 280.0}
			],
			"expect_verdict": "pass",
			"expect_blocking": false,
			"expect_marginal": 80.0
		},
		{
			# Same workload + constant +120 ms common-mode offset (contended
			# re-run of identical code): marginal is UNCHANGED at 80, still a pass.
			# The old absolute-p95 gate would flip (400 ms tips a wall the 280 ms
			# clean run cleared).
			"name": "contention_offset_invariant",
			"samples": [
				{"name": "tier_250k", "size": 250000, "frame_p95_ms": 220.0},
				{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 280.0},
				{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 400.0}
			],
			"expect_verdict": "pass",
			"expect_blocking": false,
			"expect_marginal": 80.0
		},
		{
			# Genuine per-splat regression: marginal doubles to 160 ms/Msplat > 150
			# with healthy separation -> blocking regression.
			"name": "injected_regression",
			"samples": [
				{"name": "tier_250k", "size": 250000, "frame_p95_ms": 100.0},
				{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 220.0},
				{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 460.0}
			],
			"expect_verdict": "regression",
			"expect_blocking": true,
			"expect_marginal": 160.0
		},
		{
			# Contended flat run (#630 bimodal ~355-362): ratio 1.02 < 1.5 floor ->
			# invalid measurement, re-queue. Not a silent pass, not a hard fail.
			"name": "contended_inconclusive_flat",
			"samples": [
				{"name": "tier_250k", "size": 250000, "frame_p95_ms": 355.0},
				{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 360.0},
				{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 362.0}
			],
			"expect_verdict": "inconclusive",
			"expect_blocking": false
		},
		{
			# Noise inverted the ordering (larger tier measured faster): invalid.
			"name": "inverted_inconclusive",
			"samples": [
				{"name": "tier_250k", "size": 250000, "frame_p95_ms": 200.0},
				{"name": "tier_2_5m", "size": 2500000, "frame_p95_ms": 150.0}
			],
			"expect_verdict": "inconclusive",
			"expect_blocking": false
		},
		{
			# Only one usable tier: cannot form a differential.
			"name": "insufficient_single_tier",
			"samples": [
				{"name": "tier_1m", "size": 1000000, "frame_p95_ms": 190.0}
			],
			"expect_verdict": "insufficient_data",
			"expect_blocking": false
		}
	]
	for scaling_case in scaling_cases:
		_run_scaling_case(scaling_case)
	_run_offset_invariance_check()
	_run_trim_warmup_checks()
	return scaling_cases


func _run() -> void:
	var cases := [
		{
			"name": "clean_current_result",
			"within_budget": true,
			"budget_failures": [],
			"telemetry_failures": []
		},
		{
			"name": "frame_p95_exceeded",
			"result_overrides": {"frame_p95_ms": 325.001},
			"within_budget": false,
			"budget_failures": ["frame_p95_exceeded"],
			"telemetry_failures": []
		},
		{
			"name": "frame_p95_to_avg_ratio_high",
			"result_overrides": {"frame_p95_to_avg_ratio": 2.251},
			"within_budget": false,
			"budget_failures": ["frame_p95_to_avg_ratio_high"],
			"telemetry_failures": []
		},
		{
			"name": "first_visible_missing",
			"result_overrides": {"first_visible_ms": -1.0},
			"within_budget": false,
			"budget_failures": ["first_visible_missing"],
			"telemetry_failures": []
		},
		{
			"name": "first_visible_exceeded",
			"result_overrides": {"first_visible_ms": 3500.001},
			"within_budget": false,
			"budget_failures": ["first_visible_exceeded"],
			"telemetry_failures": []
		},
		{
			"name": "fallback_source_data_missing",
			"result_overrides": {"source_data_available": false},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_source_data_missing"]
		},
		{
			"name": "fallback_rate_missing_flag",
			"result_overrides": {"fallback_rate_available": false, "fallback_rate": null},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_rate_missing"]
		},
		{
			"name": "fallback_rate_high",
			"result_overrides": {"fallback_rate": 0.3501},
			"within_budget": false,
			"budget_failures": ["fallback_rate_high"],
			"telemetry_failures": []
		},
		{
			"name": "residency_ratio_low",
			"residency_overrides": {"residency_ratio": 0.749},
			"within_budget": false,
			"budget_failures": ["residency_ratio_low"],
			"telemetry_failures": []
		}
	]

	for test_case in cases:
		_run_case(test_case)

	var scaling_cases := _run_scaling_cases()

	var summary := {
		"status": "passed" if failures.is_empty() else "failed",
		"cases": cases.size(),
		"scaling_cases": scaling_cases.size(),
		"failures": failures,
		"tier_1m_max_frame_p95_ms": float(StreamingGpuTierBudget.tier_1m_budget().get("max_frame_p95_ms", 0.0)),
		"scale_sanity_min_ratio": float(StreamingGpuTierBudget.SCALE_SANITY_MIN_RATIO),
		"marginal_backstop_ms_per_msplat": float(StreamingGpuTierBudget.MARGINAL_COST_BASELINE_MS_PER_MSPLAT),
		"warmup_discard_samples": int(StreamingGpuTierBudget.WARMUP_DISCARD_SAMPLES),
		"sort_evidence_contract": "covered_by_gpu_streaming_stress_validate_sort_metrics"
	}
	print("%s %s" % [METRICS_MARKER, JSON.stringify(summary)])

	if failures.is_empty():
		print("GPU streaming tier budget contract checks passed.")
		quit(0)
	else:
		quit(1)
