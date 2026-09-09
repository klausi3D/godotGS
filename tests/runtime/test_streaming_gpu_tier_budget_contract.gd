extends SceneTree

const GsRuntimeReport := preload("gs_runtime_report.gd")
const FAIL_MARKER := GsRuntimeReport.FAIL_MARKER
const METRICS_MARKER := GsRuntimeReport.METRICS_MARKER
const StreamingGpuTierBudget = preload("streaming_gpu_tier_budget.gd")

var failures: Array[String] = []

# T3 (#891): registry name from GDS_TESTS in run_runtime_validation.py. This
# scenario accumulates failures instead of quitting, so verified checks are
# counted where a section/case completes without growing `failures`.
var _report := GsRuntimeReport.new("Streaming GPU Tier Budget Contract")


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
		# #796/#797: 292.4 -> 162.0, using the CORRECTED measured factor 1.805 for this
		# tier (the first revision used 1.92, which had summed percentiles -- see
		# streaming_gpu_tier_budget.gd). This value only has to be a clean in-budget
		# recording; it is rescaled alongside the ceiling so the two stay comparable.
		# 292.4 was a recorded run of the OLD frame metric, which spanned the
		# harness's force_sort_for_view() and get_render_stats() as well as the
		# engine frame. Against the corrected engine-frame-only ceiling the
		# old figure reads as a failure, so every case built on this base -- all of
		# which expect a clean frame budget and vary one other field -- began
		# reporting a spurious frame_p95_exceeded. This test correctly caught the
		# metric's change of meaning; the fixture had to follow it.
		#
		# Against the ceiling (180.0) this must stay comfortably in budget.
		"frame_p95_ms": 162.0,
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
	# Removing a key is not the same as setting it to null -- `fallback_rate: null`
	# is a real fixture value elsewhere in this file, so absence needs its own
	# channel. Used to prove the enforce_timing_budgets DEFAULT, which only
	# applies when the key is genuinely missing.
	for key_to_erase in test_case.get("tier_erase", []):
		tier.erase(key_to_erase)
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

	# #883: the enforcement split is pinned on EVERY case, in both directions.
	# `blocking_failures` is what may fail the gate; `advisory_failures` is what
	# was measured, breached, and deliberately not enforced. Asserting both means
	# a future edit cannot quietly move a correctness check into the advisory
	# bucket (the gate would keep passing but this test goes red) and cannot
	# quietly delete a demoted check either (it would vanish from advisory).
	var expected_blocking: Array = test_case.get("blocking_failures", [])
	var actual_blocking: Array = evaluation.get("blocking_failures", [])
	if not _array_equals(actual_blocking, expected_blocking):
		_record_failure("Budget contract blocking_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_blocking,
			"actual": actual_blocking,
			"evaluation": evaluation
		})

	var expected_advisory: Array = test_case.get("advisory_failures", [])
	var actual_advisory: Array = evaluation.get("advisory_failures", [])
	if not _array_equals(actual_advisory, expected_advisory):
		_record_failure("Budget contract advisory_failures mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_advisory,
			"actual": actual_advisory,
			"evaluation": evaluation
		})

	# The gate's actual verdict, stated separately from `within_budget` so the
	# two cannot drift into meaning the same thing again.
	var expected_blocking_within := expected_blocking.is_empty()
	var actual_blocking_within := bool(evaluation.get("blocking_within_budget", false))
	if actual_blocking_within != expected_blocking_within:
		_record_failure("Budget contract blocking_within_budget mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"expected": expected_blocking_within,
			"actual": actual_blocking_within,
			"evaluation": evaluation
		})

	# Every demoted check must carry its measured value and the budget it broke,
	# so the live gate can print a number rather than a name. A demoted check
	# reported without its value is halfway to deleted.
	var details: Array = evaluation.get("advisory_details", [])
	if details.size() != actual_advisory.size():
		_record_failure("Budget contract advisory_details cardinality mismatch", {
			"case": test_case.get("name", "<unnamed>"),
			"advisory_failures": actual_advisory,
			"advisory_details": details
		})
	for detail in details:
		if not (detail is Dictionary) or not detail.has("measured") or not detail.has("budget"):
			_record_failure("Budget contract advisory_details missing measured/budget", {
				"case": test_case.get("name", "<unnamed>"),
				"detail": detail
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
		var failures_before := failures.size()
		_run_scaling_case(scaling_case)
		if failures.size() == failures_before:
			_report.ok()
	var failures_before_offset := failures.size()
	_run_offset_invariance_check()
	if failures.size() == failures_before_offset:
		_report.ok()
	var failures_before_trim := failures.size()
	_run_trim_warmup_checks()
	if failures.size() == failures_before_trim:
		_report.ok()
	return scaling_cases


func _run() -> void:
	# #797: DERIVE the exceed-boundary from the enforced ceiling instead of hardcoding it.
	# This case used to pass 325.001 -- a literal matching the pre-#796 ceiling. Once the
	# ceiling was rescaled for the engine-only metric that value was still far above it, so
	# the case passed no matter
	# what: restoring or accidentally raising the ceiling anywhere up to 325 would have left
	# this test green while it claimed to verify the boundary. A test whose fixture is a
	# copy of the value under test stops testing it the moment the value moves, which is
	# why the number is now read from the budget rather than repeated here.
	var enforced_p95_ceiling := float(StreamingGpuTierBudget.tier_1m_budget().get("max_frame_p95_ms", 0.0))

	# #797: deriving the fixture above makes the boundary cases robust, but it also makes
	# them indifferent to the ceiling's VALUE -- so on its own it would let a silent
	# recalibration through. Pin the calibrated number too. tests/AGENTS.md forbids
	# unjustified threshold changes; this turns "someone moved the ceiling" from an
	# invisible edit into a failing test that has to be answered in review.
	# Updating this constant is legitimate -- it just has to be deliberate, and land with
	# the evidence for the new value.
	const CALIBRATED_TIER_1M_P95_CEILING_MS := 180.0
	if not is_equal_approx(enforced_p95_ceiling, CALIBRATED_TIER_1M_P95_CEILING_MS):
		_record_failure("tier_1m max_frame_p95_ms moved away from its calibrated value", {
			"expected": CALIBRATED_TIER_1M_P95_CEILING_MS,
			"actual": enforced_p95_ceiling,
			"why": "the ceiling is metric-specific (#796 engine-frame-only); changing it needs its own measured justification"
		})
	else:
		_report.ok()
	# #883: the shipped tier_1m posture, pinned. `enforce` (per-tier, covering
	# correctness) must stay ON; `enforce_timing_budgets` (per-metric-class) is
	# the interim OFF. Both are asserted so that neither half can move without a
	# deliberate edit here: turning `enforce` off would gut the gate, and turning
	# `enforce_timing_budgets` back on is the intended restoration and must be an
	# explicit, reviewed change rather than a silent one.
	var shipped_tier := StreamingGpuTierBudget.tier_1m_budget()
	if not bool(shipped_tier.get("enforce", false)):
		_record_failure("tier_1m stopped enforcing correctness budgets", {
			"why": "#883 demoted the TIMING budgets only; `enforce` covers residency, "
				+ "fallback rate and telemetry and must stay true"
		})
	else:
		_report.ok()
	if bool(shipped_tier.get("enforce_timing_budgets", true)):
		_record_failure("tier_1m timing budgets are enforced again", {
			"why": "this is the intended end state (#778/#523), but it must land with the "
				+ "evidence that the measurement is stable -- update this test alongside it"
		})
	else:
		_report.ok()

	# The demotion list itself, pinned exactly. Adding a correctness code here
	# would demote it invisibly at every call site at once; removing a timing
	# code would silently re-arm it. Neither may happen by accident.
	var expected_timing_codes := ["first_visible_exceeded", "frame_p95_exceeded", "frame_p95_to_avg_ratio_high"]
	if not _array_equals(StreamingGpuTierBudget.TIMING_BUDGET_FAILURES, expected_timing_codes):
		_record_failure("TIMING_BUDGET_FAILURES membership changed", {
			"expected": expected_timing_codes,
			"actual": StreamingGpuTierBudget.TIMING_BUDGET_FAILURES,
			"why": "only wall-clock/dispersion budgets may be demoted; `first_visible_missing` "
				+ "is a correctness failure observed through a timer and must not join this list"
		})
	else:
		_report.ok()

	var cases := [
		{
			"name": "clean_current_result",
			"within_budget": true,
			"budget_failures": [],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": []
		},
		{
			# Just above the ceiling, so it fails for the right reason and would stop
			# failing if the check were removed.
			# #883: still detected and still reported -- just not blocking.
			"name": "frame_p95_exceeded",
			"result_overrides": {"frame_p95_ms": enforced_p95_ceiling + 0.001},
			"within_budget": false,
			"budget_failures": ["frame_p95_exceeded"],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": ["frame_p95_exceeded"]
		},
		{
			# The paired NEGATIVE boundary: exactly AT the ceiling must pass. Without this,
			# a check mutated to `>=` (or a ceiling driven to 0) would still satisfy the
			# case above, so the boundary would be asserted from one side only.
			"name": "frame_p95_at_ceiling_is_within_budget",
			"result_overrides": {"frame_p95_ms": enforced_p95_ceiling},
			"within_budget": true,
			"budget_failures": [],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": []
		},
		{
			# The #883 metric itself. 3.69 was measured on an idle runner against
			# this 2.25 budget; the budget is unchanged and the breach is still
			# recorded, it just no longer fails the job.
			"name": "frame_p95_to_avg_ratio_high",
			"result_overrides": {"frame_p95_to_avg_ratio": 2.251},
			"within_budget": false,
			"budget_failures": ["frame_p95_to_avg_ratio_high"],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": ["frame_p95_to_avg_ratio_high"]
		},
		{
			# NOT demoted, and this case is the reason the distinction exists:
			# "never became visible at all" is a correctness failure that happens
			# to be observed through a timer. It stays blocking.
			"name": "first_visible_missing",
			"result_overrides": {"first_visible_ms": -1.0},
			"within_budget": false,
			"budget_failures": ["first_visible_missing"],
			"telemetry_failures": [],
			"blocking_failures": ["first_visible_missing"],
			"advisory_failures": []
		},
		{
			"name": "first_visible_exceeded",
			"result_overrides": {"first_visible_ms": 3500.001},
			"within_budget": false,
			"budget_failures": ["first_visible_exceeded"],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": ["first_visible_exceeded"]
		},
		{
			"name": "fallback_source_data_missing",
			"result_overrides": {"source_data_available": false},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_source_data_missing"],
			"blocking_failures": ["fallback_source_data_missing"],
			"advisory_failures": []
		},
		{
			"name": "fallback_rate_missing_flag",
			"result_overrides": {"fallback_rate_available": false, "fallback_rate": null},
			"within_budget": false,
			"budget_failures": [],
			"telemetry_failures": ["fallback_rate_missing"],
			"blocking_failures": ["fallback_rate_missing"],
			"advisory_failures": []
		},
		{
			"name": "fallback_rate_high",
			"result_overrides": {"fallback_rate": 0.3501},
			"within_budget": false,
			"budget_failures": ["fallback_rate_high"],
			"telemetry_failures": [],
			"blocking_failures": ["fallback_rate_high"],
			"advisory_failures": []
		},
		{
			"name": "residency_ratio_low",
			"residency_overrides": {"residency_ratio": 0.749},
			"within_budget": false,
			"budget_failures": ["residency_ratio_low"],
			"telemetry_failures": [],
			"blocking_failures": ["residency_ratio_low"],
			"advisory_failures": []
		},
		{
			# The case that proves the demotion cannot mask a real failure: a
			# timing breach and a correctness breach in the SAME run. The timing
			# one goes advisory, the correctness one still blocks. If the split
			# were implemented as "any advisory failure makes the tier advisory",
			# this case would come back blocking-clean and go red here.
			"name": "timing_advisory_does_not_mask_correctness",
			"result_overrides": {"frame_p95_to_avg_ratio": 2.251, "fallback_rate": 0.3501},
			"within_budget": false,
			"budget_failures": ["frame_p95_to_avg_ratio_high", "fallback_rate_high"],
			"telemetry_failures": [],
			"blocking_failures": ["fallback_rate_high"],
			"advisory_failures": ["frame_p95_to_avg_ratio_high"]
		},
		{
			# All three demoted checks at once, so the list is asserted whole
			# rather than one member at a time.
			"name": "all_three_timing_budgets_advisory_together",
			"result_overrides": {
				"first_visible_ms": 3500.001,
				"frame_p95_ms": enforced_p95_ceiling + 0.001,
				"frame_p95_to_avg_ratio": 2.251
			},
			"within_budget": false,
			"budget_failures": ["first_visible_exceeded", "frame_p95_exceeded", "frame_p95_to_avg_ratio_high"],
			"telemetry_failures": [],
			"blocking_failures": [],
			"advisory_failures": ["first_visible_exceeded", "frame_p95_exceeded", "frame_p95_to_avg_ratio_high"]
		},
		{
			# The restoration path, proven rather than promised: flipping the one
			# flag back to true re-arms all three budgets with no other edit. This
			# is what #778/#523 landing looks like, and it is exercised today.
			"name": "restoring_enforcement_rearms_all_three",
			"tier_overrides": {"enforce_timing_budgets": true},
			"result_overrides": {
				"first_visible_ms": 3500.001,
				"frame_p95_ms": enforced_p95_ceiling + 0.001,
				"frame_p95_to_avg_ratio": 2.251
			},
			"within_budget": false,
			"budget_failures": ["first_visible_exceeded", "frame_p95_exceeded", "frame_p95_to_avg_ratio_high"],
			"telemetry_failures": [],
			"blocking_failures": ["first_visible_exceeded", "frame_p95_exceeded", "frame_p95_to_avg_ratio_high"],
			"advisory_failures": []
		},
		{
			# A tier that never opts out keeps its old behaviour exactly: the
			# default is ENFORCED, so this change is inert for tier_250k and
			# tier_2_5m and for any tier added later that forgets the flag.
			"name": "default_when_flag_absent_is_enforced",
			"tier_erase": ["enforce_timing_budgets"],
			"result_overrides": {"frame_p95_to_avg_ratio": 2.251},
			"within_budget": false,
			"budget_failures": ["frame_p95_to_avg_ratio_high"],
			"telemetry_failures": [],
			"blocking_failures": ["frame_p95_to_avg_ratio_high"],
			"advisory_failures": []
		}
	]

	for test_case in cases:
		var failures_before := failures.size()
		_run_case(test_case)
		if failures.size() == failures_before:
			_report.ok()

	var scaling_cases := _run_scaling_cases()

	var summary := {
		"status": "passed" if failures.is_empty() else "failed",
		"cases": cases.size(),
		"scaling_cases": scaling_cases.size(),
		"failures": failures,
		"tier_1m_max_frame_p95_ms": float(StreamingGpuTierBudget.tier_1m_budget().get("max_frame_p95_ms", 0.0)),
		# #883 posture, emitted so the demotion is visible in the evidence
		# artefacts and not only in the source.
		"tier_1m_enforce": bool(StreamingGpuTierBudget.tier_1m_budget().get("enforce", false)),
		"tier_1m_enforce_timing_budgets": bool(
			StreamingGpuTierBudget.tier_1m_budget().get("enforce_timing_budgets", true)
		),
		"timing_budget_failures": StreamingGpuTierBudget.TIMING_BUDGET_FAILURES,
		"scale_sanity_min_ratio": float(StreamingGpuTierBudget.SCALE_SANITY_MIN_RATIO),
		"marginal_backstop_ms_per_msplat": float(StreamingGpuTierBudget.MARGINAL_COST_BASELINE_MS_PER_MSPLAT),
		"warmup_discard_samples": int(StreamingGpuTierBudget.WARMUP_DISCARD_SAMPLES),
		"sort_evidence_contract": "covered_by_gpu_streaming_stress_validate_sort_metrics"
	}
	print("%s %s" % [METRICS_MARKER, JSON.stringify(summary)])

	if failures.is_empty():
		print("GPU streaming tier budget contract checks passed.")
		_report.emit_pass()
		quit(0)
	else:
		quit(1)
