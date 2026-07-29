extends SceneTree

# Runtime large-tier stress probe for the large-world proof ladder. Exercises
# the radix path and validates that performance counters are being populated.

const StreamingGpuTierBudget = preload("streaming_gpu_tier_budget.gd")
const BASELINE_TIER_NAME := "tier_1m"
const SORT_METHOD_NAME := "GPU_RADIX"
const TARGET_SORT_MS := 2.5
const MAX_VISIBLE_RATIO_DROP := 0.35
const METRICS_MARKER := "[RUNTIME_METRICS]"
const SKIP_MARKER := "[RUNTIME_SKIP]"
const FAIL_MARKER := "[RUNTIME_FAIL]"
const SAMPLE_FRAMES := 120
const FIRST_VISIBLE_TIMEOUT_FRAMES := 240

var renderer: GaussianSplatRenderer
var scene_root: Node3D
var camera: Camera3D
var world_node: GaussianSplatWorld3D
var manager = null
var exit_code = 0
var failures: Array[String] = []
var benchmark_summary: Dictionary = {
    "status": "failed",
    "baseline_tier": BASELINE_TIER_NAME,
    "baseline_passed": false,
    "tiers": [],
    "failures": []
}

## Defers execution until the SceneTree loop is initialized.
func _init() -> void:
    call_deferred("_run")

## Returns true when running on a headless display server.
func _is_headless_runtime() -> bool:
    return OS.has_feature("headless") or DisplayServer.get_name() == "headless"

func _stream_tiers() -> Array:
    return [
        {
            "name": "tier_250k",
            "size": 250000,
            "max_first_visible_ms": 2500.0,
            "min_residency_ratio": 0.70,
            # #796: 90.0 -> 47.0, the same 1.92x metric rescale applied to tier_1m.
            # Strictness is unchanged. Note this ceiling was ALREADY breached before
            # the rescale (observed engine-frame p95 130-248 ms) and still is; it is
            # advisory (enforce=false) so it has been failing invisibly. Arming it is
            # a separate decision and needs the measurement instability fixed first.
            "max_frame_p95_ms": 47.0,
            "max_frame_p95_to_avg_ratio": 2.25,
            "max_fallback_rate": 0.40,
            "enforce": false
        },
        StreamingGpuTierBudget.tier_1m_budget(),
        {
            "name": "tier_2_5m",
            "size": 2500000,
            "max_first_visible_ms": 5000.0,
            "min_residency_ratio": 0.75,
            # #796: 160.0 -> 83.0, same 1.92x metric rescale. Also already breached
            # and advisory: observed engine-frame p95 [153.1, 156.2, 157.9, 520.9,
            # 548.3] ms -- a 3.6x run-to-run spread on an IDLE machine, which is the
            # widest of the three tiers and the clearest sign that this measurement
            # is not yet repeatable enough to gate on.
            "max_frame_p95_ms": 83.0,
            "max_frame_p95_to_avg_ratio": 2.50,
            "max_fallback_rate": 0.35,
            "enforce": false
        }
    ]

## Initializes the renderer and exercises streaming datasets.
func _run() -> void:
    if _is_headless_runtime():
        var skip_reason = "GPU streaming stress test requires a local RenderingDevice (non-headless run)."
        print("%s %s" % [SKIP_MARKER, skip_reason])
        push_warning(skip_reason)
        quit(0)
        return

    print("=== GPU Streaming & Sorter Stress Test ===")
    renderer = GaussianSplatRenderer.new()
    if renderer == null:
        _record_failure("Failed to create GaussianSplatRenderer instance")
        _print_summary()
        quit(1)
        return
    renderer.initialize()

    manager = Engine.get_singleton("GaussianSplatManager")
    if manager == null:
        push_warning("GaussianSplatManager singleton unavailable; residency assertions limited")

    var tier_results: Array = []
    var baseline_found := false
    for tier in _stream_tiers():
        var tier_result := await _exercise_tier(tier)
        tier_results.append(tier_result)
        if String(tier_result.get("name", "")) == BASELINE_TIER_NAME:
            baseline_found = true
            benchmark_summary["baseline_passed"] = bool(tier_result.get("within_budget", false))

    benchmark_summary["tiers"] = tier_results
    _evaluate_frame_scaling(tier_results)
    if not baseline_found:
        benchmark_summary["baseline_passed"] = false
        _record_failure("Missing baseline streaming tier result", {"baseline_tier": BASELINE_TIER_NAME})
    benchmark_summary["failures"] = failures.duplicate()
    benchmark_summary["status"] = "passed" if exit_code == 0 else "failed"
    print("%s %s" % [METRICS_MARKER, JSON.stringify(benchmark_summary)])

    _print_summary()
    if exit_code == 0:
        print("\n✅ GPU streaming stress test completed without regressions")
    else:
        push_error("%s GPU streaming stress test detected failures" % FAIL_MARKER)
    quit(exit_code)

## Applies the deterministic cross-tier frame-scaling verdict (#630).
##
## The absolute per-tier p95 ceiling in evaluate_tier_budget() stays as a gross
## backstop. This adds the contention-robust signal: the per-splat marginal cost
## between the smallest and largest tier cancels the common-mode runner-load
## offset, so it is invariant to how loaded the shared runner is.
##
## Wiring policy (matches the calibration posture the tier budget already
## documents): the scale-sanity "inconclusive" verdict identifies a
## common-mode-dominated, INVALID measurement so CI can re-queue it rather than
## treat runner load as a regression; that is surfaced (non-blocking) here. The
## "regression" verdict is likewise surfaced non-blocking for now because its
## backstop baseline is deliberately generous and must be tightened against
## several clean optimized-build runs in the dedicated calibration PR before it
## is promoted to blocking. The full algorithm (including the blocking-regression
## path) is proven deterministically by test_streaming_gpu_tier_budget_contract.gd.
func _evaluate_frame_scaling(tier_results: Array) -> void:
    var samples: Array = []
    for tier_result in tier_results:
        if not (tier_result is Dictionary):
            continue
        samples.append({
            "name": String(tier_result.get("name", "tier_unknown")),
            "size": int(tier_result.get("dataset_size", 0)),
            "frame_p95_ms": float(tier_result.get("frame_p95_ms", 0.0))
        })
    var scaling := StreamingGpuTierBudget.evaluate_scaling_regression(samples)
    benchmark_summary["frame_scaling"] = scaling
    var verdict := String(scaling.get("verdict", "insufficient_data"))
    var reason := String(scaling.get("reason", ""))
    if verdict == "regression":
        push_warning("[Streaming] Frame-scaling regression (advisory pending calibration): %s" % reason)
    elif verdict == "inconclusive":
        push_warning(
            "[Streaming] Frame-scaling measurement inconclusive (invalid / common-mode dominated); re-run in a quiet window: %s" % reason
        )
    elif verdict == "insufficient_data":
        push_warning("[Streaming] Frame-scaling verdict skipped: %s" % reason)


## Records a failure with context and marks the script as failed.
func _record_failure(reason: String, context: Dictionary = {}) -> void:
    var message = reason
    if not context.is_empty():
        message = "%s | context=%s" % [reason, str(context)]
    failures.append(message)
    exit_code = 1
    push_error("%s %s" % [FAIL_MARKER, message])

## Returns the first integer-valued entry for the provided stat keys.
func _read_stat_int(stats: Dictionary, keys: Array[String], default_value: int = 0) -> int:
    for key in keys:
        if stats.has(key):
            return int(stats.get(key, default_value))
    return default_value

## Returns the maximum integer-valued metric from the provided keys.
func _read_stat_int_max(stats: Dictionary, keys: Array[String], default_value: int = 0) -> int:
    var best = default_value
    for key in keys:
        if stats.has(key):
            best = max(best, int(stats.get(key, default_value)))
    return best

## Returns the first float-valued entry for the provided stat keys.
func _read_stat_float(stats: Dictionary, keys: Array[String], default_value: float = 0.0) -> float:
    for key in keys:
        if stats.has(key):
            return float(stats.get(key, default_value))
    return default_value

## Returns the first optional float-valued entry for the provided stat keys.
func _read_stat_optional_float(stats: Dictionary, keys: Array[String]) -> Variant:
    for key in keys:
        if stats.has(key):
            return float(stats.get(key, 0.0))
    return null

## Returns the first optional string-valued entry for the provided stat keys.
func _read_stat_optional_string(stats: Dictionary, keys: Array[String]) -> Variant:
    for key in keys:
        if stats.has(key):
            return String(stats.get(key, ""))
    return null

## Returns the first optional bool-valued entry for the provided stat keys.
func _read_stat_optional_bool(stats: Dictionary, keys: Array[String]) -> Variant:
    for key in keys:
        if stats.has(key):
            return bool(stats.get(key, false))
    return null

## Returns the nested diagnostics dictionary when present.
func _extract_streaming_diagnostics(stream_state: Dictionary) -> Dictionary:
    if stream_state.has("diagnostics") and stream_state["diagnostics"] is Dictionary:
        return stream_state["diagnostics"]
    return {}

## Prints an explicit failure list for CI log triage.
func _print_summary() -> void:
    if failures.is_empty():
        return
    print("\nFailure details:")
    for failure in failures:
        print(" - ", failure)

func _average(values: Array) -> float:
    if values.is_empty():
        return 0.0
    var total := 0.0
    for value in values:
        total += float(value)
    return total / float(values.size())

func _percentile(values: Array, percentile: float) -> float:
    if values.is_empty():
        return 0.0
    var sorted_values: Array = values.duplicate()
    sorted_values.sort()
    var clamped: float = clampf(percentile, 0.0, 1.0)
    var index := int(round(clamped * float(sorted_values.size() - 1)))
    return float(sorted_values[index])

func _evaluate_sort_evidence(stats: Dictionary, history: Array) -> Dictionary:
    var total_sorts := _read_stat_int(stats, ["gpu_sorter_total_sorts"])
    var history_entries := history.size()
    var evidence_available := total_sorts > 0 or history_entries > 0
    var evidence_status := "available" if evidence_available else "missing"
    return {
        "available": evidence_available,
        "status": evidence_status,
        "details": {
            "gpu_sorter_total_sorts": total_sorts,
            "history_entries": history_entries
        }
    }

## Runs a streaming and sorting pass for a configured tier.
## @param tier: Dictionary containing dataset size and budget thresholds.
func _exercise_tier(tier: Dictionary) -> Dictionary:
    var tier_name := String(tier.get("name", "tier_unknown"))
    var size := int(tier.get("size", 0))
    var enforce := bool(tier.get("enforce", false))
    var tier_result: Dictionary = {
        "name": tier_name,
        "dataset_size": size,
        "enforce": enforce,
        "within_budget": false,
        "budget_failures": [],
        "first_visible_ms": -1.0,
        "frame_avg_ms": 0.0,
        "frame_p95_ms": 0.0,
        "frame_max_ms": 0.0,
        "frame_p95_to_avg_ratio": 0.0,
        "source_data_available": false,
        "source_data_status": "missing",
        "sort_evidence_available": false,
        "sort_evidence_status": "missing",
        "sort_evidence_details": {},
        "fallback_rate_available": false,
        "fallback_rate_status": "missing",
        "fallback_rate": null,
        "fallback_frames": 0,
        "source_frame_count": 0,
        "telemetry_failures": [],
        "uploaded_splats": 0,
        "visible_splats": 0,
        "total_splats": 0,
        "residency_ratio": 0.0,
        "visible_ratio": 0.0,
        "sort_avg_ms": 0.0,
        "queue_pressure_active_frames": 0,
        "queue_pressure_candidate_frames": 0,
        "queue_pressure_no_progress_frames": 0,
        "queue_pressure_scan_starved_frames": 0,
        "queue_pressure_no_progress_ratio": 0.0,
        "queue_pressure_scan_starved_ratio": 0.0,
        "queue_pressure_reason_source": null,
        "queue_pressure_reason_sources": {},
        "pack_queue_latency_max_ms": null,
        "upload_queue_latency_max_ms": null,
        "pack_mutex_wait_max_ms": null,
        "scheduler_queue_pressure_scan_throttle_active": null,
        "scheduler_update_cpu_p95_ms": 0.0,
        "scheduler_cpu_total_attributed_p95_ms": 0.0
    }

    print("\n[Streaming] Preparing dataset with %d splats" % size)
    var dataset = _build_dataset(size)
    if not _setup_world_scene(dataset):
        _record_failure("world-backed setup failed", {"dataset_size": size})
        tier_result["budget_failures"] = ["world_setup_failed"]
        return tier_result

    for _frame in range(3):
        await process_frame
        if world_node != null:
            renderer = world_node.get_renderer()
        if renderer != null:
            break

    if renderer == null:
        _record_failure("renderer unavailable after world apply", {"dataset_size": size})
        tier_result["budget_failures"] = ["renderer_unavailable"]
        await _teardown_world_scene()
        return tier_result

    # Raise the splat budget on the world-owned renderer if the tier needs
    # more than the default. _teardown_world_scene() nils `renderer`, so the
    # budget must be adjusted after the world renderer is re-acquired rather
    # than against the pre-setup handle (which crashed the 1m/2.5m tiers).
    var current_budget := int(renderer.get_max_splats())
    if current_budget < size:
        renderer.set_max_splats(size)

    print("  -> Exercising %s (%s)" % [SORT_METHOD_NAME, tier_name])

    # Warm-up passes to stabilise GPU timers
    for i in range(3):
        await process_frame
        renderer.force_sort_for_view(Transform3D.IDENTITY)

    var benchmark_start_usec := Time.get_ticks_usec()
    var frame_times_ms: Array = []
    # #796: harness-side costs, kept OUT of the frame sample but still reported.
    var harness_forced_sort_ms: Array = []
    var harness_stats_gather_ms: Array = []
    var scheduler_update_cpu_ms_values: Array = []
    var scheduler_cpu_total_attributed_ms_values: Array = []
    var fallback_frames := 0
    var source_frame_count := 0
    var queue_pressure_active_frames := 0
    var queue_pressure_candidate_frames := 0
    var queue_pressure_no_progress_frames := 0
    var queue_pressure_scan_starved_frames := 0
    var queue_pressure_reason_sources: Dictionary = {}
    var last_queue_pressure_reason_source: Variant = null
    var max_pack_queue_latency_max_ms: Variant = null
    var max_upload_queue_latency_max_ms: Variant = null
    var max_pack_mutex_wait_max_ms: Variant = null
    var saw_scheduler_queue_pressure_scan_throttle_active := false
    var scheduler_queue_pressure_scan_throttle_active_known := false
    var first_visible_ms := -1.0
    var stats: Dictionary = {}
    for frame_index in range(SAMPLE_FRAMES):
        # #796: the frame sample is the ENGINE FRAME ONLY. It used to span the two
        # harness calls below as well, and they are not free: force_sort_for_view() is
        # a blocking dispatch to the render thread (it is on run_module_tests.py's
        # DISPATCHING_METHODS list) and get_render_stats() aggregates. Measured, they
        # were ~38% and ~7% of the old sample, so every frame budget calibrated
        # against it was ~1.8x inflated by the harness measuring itself.
        #
        # They are still timed, but as their own diagnostics, because a per-sample
        # blocking sort is a real cost worth watching -- just not a frame time.
        var frame_start_usec := Time.get_ticks_usec()
        await process_frame
        var engine_frame_end_usec := Time.get_ticks_usec()
        renderer.force_sort_for_view(Transform3D.IDENTITY)
        var forced_sort_end_usec := Time.get_ticks_usec()
        stats = renderer.get_render_stats()
        var stats_end_usec := Time.get_ticks_usec()

        var frame_ms := float(engine_frame_end_usec - frame_start_usec) / 1000.0
        frame_times_ms.append(frame_ms)
        harness_forced_sort_ms.append(float(forced_sort_end_usec - engine_frame_end_usec) / 1000.0)
        harness_stats_gather_ms.append(float(stats_end_usec - forced_sort_end_usec) / 1000.0)

        var visible := _read_stat_int_max(stats, ["visible_after_culling", "visible_splats", "cull_cpu_visible_count"])
        if first_visible_ms < 0.0 and visible > 0:
            first_visible_ms = float(Time.get_ticks_usec() - benchmark_start_usec) / 1000.0

        var data_source := String(stats.get("data_source", ""))
        if not data_source.is_empty():
            source_frame_count += 1
            if data_source.findn("stream") == -1:
                fallback_frames += 1

        var stream_state: Variant = stats.get("streaming_state", {})
        if stream_state is Dictionary:
            var s: Dictionary = stream_state
            var diagnostics := _extract_streaming_diagnostics(s)
            scheduler_update_cpu_ms_values.append(_read_stat_float(s, ["scheduler_update_cpu_ms"]))
            scheduler_cpu_total_attributed_ms_values.append(
                _read_stat_float(s, ["scheduler_cpu_total_attributed_ms"])
            )

            if bool(s.get("queue_pressure_active", false)):
                queue_pressure_active_frames += 1
                var load_candidates := _read_stat_int(s, ["scheduler_load_candidates"])
                if load_candidates > 0:
                    queue_pressure_candidate_frames += 1
                    if _read_stat_int(s, ["chunks_loaded_this_frame"]) == 0:
                        queue_pressure_no_progress_frames += 1
                    if _read_stat_int(s, ["scheduler_visible_scan_budget_effective"]) <= 1:
                        queue_pressure_scan_starved_frames += 1

            var queue_pressure_reason_source: Variant = _read_stat_optional_string(
                diagnostics,
                ["queue_pressure_reason_source", "queue_pressure_source"]
            )
            if queue_pressure_reason_source == null:
                queue_pressure_reason_source = _read_stat_optional_string(
                    s,
                    ["queue_pressure_reason_source", "queue_pressure_source"]
                )
            if queue_pressure_reason_source != null and not String(queue_pressure_reason_source).is_empty():
                last_queue_pressure_reason_source = queue_pressure_reason_source
                queue_pressure_reason_sources[String(queue_pressure_reason_source)] = int(
                    queue_pressure_reason_sources.get(queue_pressure_reason_source, 0)
                ) + 1

            var pack_queue_latency_max_ms: Variant = _read_stat_optional_float(
                diagnostics,
                ["pack_queue_latency_max_ms"]
            )
            if pack_queue_latency_max_ms == null:
                pack_queue_latency_max_ms = _read_stat_optional_float(s, ["pack_queue_latency_max_ms"])
            if pack_queue_latency_max_ms != null:
                max_pack_queue_latency_max_ms = float(pack_queue_latency_max_ms) if max_pack_queue_latency_max_ms == null else maxf(
                    float(max_pack_queue_latency_max_ms),
                    float(pack_queue_latency_max_ms)
                )

            var upload_queue_latency_max_ms: Variant = _read_stat_optional_float(
                diagnostics,
                ["upload_queue_latency_max_ms"]
            )
            if upload_queue_latency_max_ms == null:
                upload_queue_latency_max_ms = _read_stat_optional_float(s, ["upload_queue_latency_max_ms"])
            if upload_queue_latency_max_ms != null:
                max_upload_queue_latency_max_ms = float(
                    upload_queue_latency_max_ms
                ) if max_upload_queue_latency_max_ms == null else maxf(
                    float(max_upload_queue_latency_max_ms),
                    float(upload_queue_latency_max_ms)
                )

            var pack_mutex_wait_max_ms: Variant = _read_stat_optional_float(
                diagnostics,
                ["pack_mutex_wait_max_ms"]
            )
            if pack_mutex_wait_max_ms == null:
                pack_mutex_wait_max_ms = _read_stat_optional_float(s, ["pack_mutex_wait_max_ms"])
            if pack_mutex_wait_max_ms != null:
                max_pack_mutex_wait_max_ms = float(pack_mutex_wait_max_ms) if max_pack_mutex_wait_max_ms == null else maxf(
                    float(max_pack_mutex_wait_max_ms),
                    float(pack_mutex_wait_max_ms)
                )

            var throttle_active: Variant = _read_stat_optional_bool(
                s,
                ["scheduler_queue_pressure_scan_throttle_active"]
            )
            if throttle_active == null:
                throttle_active = _read_stat_optional_bool(
                    diagnostics,
                    ["scheduler_queue_pressure_scan_throttle_active"]
                )
            if throttle_active != null:
                scheduler_queue_pressure_scan_throttle_active_known = true
                if bool(throttle_active):
                    saw_scheduler_queue_pressure_scan_throttle_active = true

    if first_visible_ms < 0.0:
        for frame_index in range(max(0, FIRST_VISIBLE_TIMEOUT_FRAMES - SAMPLE_FRAMES)):
            await process_frame
            renderer.force_sort_for_view(Transform3D.IDENTITY)
            stats = renderer.get_render_stats()
            var late_visible := _read_stat_int_max(stats, ["visible_after_culling", "visible_splats", "cull_cpu_visible_count"])
            if late_visible > 0:
                first_visible_ms = float(Time.get_ticks_usec() - benchmark_start_usec) / 1000.0
                break

    # Documented warm-up discard (#630, step 3 of the repeatability protocol in
    # streaming_gpu_tier_budget.gd). The 3 pre-loop warm-up frames stabilise GPU
    # timers; this drops the first WARMUP_DISCARD_SAMPLES retained samples that
    # still ride the ramp before the absolute-p95 backstop and its avg are
    # computed. trim_warmup() keeps the full window when too few samples remain,
    # so a short capture is never emptied. This is a noise reduction on the
    # sample window, not a threshold change.
    var measured_frame_times_ms := StreamingGpuTierBudget.trim_warmup(frame_times_ms)
    var frame_avg_ms := _average(measured_frame_times_ms)
    var frame_p95_ms := _percentile(measured_frame_times_ms, 0.95)
    var frame_max_ms := 0.0
    for value in measured_frame_times_ms:
        frame_max_ms = maxf(frame_max_ms, float(value))
    var frame_p95_to_avg_ratio := frame_p95_ms / maxf(0.001, frame_avg_ms)
    var scheduler_update_cpu_p95_ms := _percentile(scheduler_update_cpu_ms_values, 0.95)
    var scheduler_cpu_total_attributed_p95_ms := _percentile(scheduler_cpu_total_attributed_ms_values, 0.95)
    var queue_pressure_no_progress_ratio := 0.0 if queue_pressure_candidate_frames <= 0 else float(
        queue_pressure_no_progress_frames
    ) / float(queue_pressure_candidate_frames)
    var queue_pressure_scan_starved_ratio := 0.0 if queue_pressure_candidate_frames <= 0 else float(
        queue_pressure_scan_starved_frames
    ) / float(queue_pressure_candidate_frames)
    var source_data_available := source_frame_count > 0
    var source_data_status := "available" if source_data_available else "missing"
    var fallback_rate_available := source_data_available
    var fallback_rate_status := "available" if fallback_rate_available else "insufficient_source_data"
    var fallback_rate: Variant = null
    if fallback_rate_available:
        fallback_rate = float(fallback_frames) / float(source_frame_count)
    var sort_history = renderer.get_sort_metrics_history()
    var sort_evidence := _evaluate_sort_evidence(stats, sort_history)

    _validate_sort_metrics(SORT_METHOD_NAME, size, stats, sort_history)
    var residency := _validate_residency(size, stats)
    var budget_eval := StreamingGpuTierBudget.evaluate_tier_budget(tier, {
        "first_visible_ms": first_visible_ms,
        "frame_p95_ms": frame_p95_ms,
        "frame_p95_to_avg_ratio": frame_p95_to_avg_ratio,
        "source_data_available": source_data_available,
        "fallback_rate_available": fallback_rate_available,
        "fallback_rate": fallback_rate,
        "source_frame_count": source_frame_count
    }, residency)
    var within_budget := bool(budget_eval.get("within_budget", false))
    var budget_failures: Array = budget_eval.get("budget_failures", [])
    var telemetry_failures: Array = budget_eval.get("telemetry_failures", [])

    if not within_budget:
        var budget_context := {
            "tier": tier_name,
            "size": size,
            "first_visible_ms": first_visible_ms,
            "residency_ratio": float(residency.get("residency_ratio", 0.0)),
            "frame_p95_ms": frame_p95_ms,
            "frame_p95_to_avg_ratio": frame_p95_to_avg_ratio,
            "source_data_status": source_data_status,
            "fallback_rate_status": fallback_rate_status,
            "fallback_rate": fallback_rate,
            "budget_failures": budget_failures,
            "telemetry_failures": telemetry_failures
        }
        if enforce:
            _record_failure("[Streaming] Tier budget check failed", budget_context)
        else:
            push_warning("[Streaming] Non-blocking tier budget failed: %s" % str(budget_context))

    tier_result["within_budget"] = within_budget
    tier_result["budget_failures"] = budget_failures
    tier_result["telemetry_failures"] = telemetry_failures
    tier_result["first_visible_ms"] = first_visible_ms
    tier_result["frame_avg_ms"] = frame_avg_ms
    tier_result["frame_p95_ms"] = frame_p95_ms
    # #796: reported, deliberately NOT part of frame_* above. Kept visible so the
    # harness's own per-sample cost stays observable instead of silently dropped --
    # if it grows, the numbers above are still honest but the run got slower.
    tier_result["harness_forced_sort_p95_ms"] = _percentile(harness_forced_sort_ms, 0.95)
    tier_result["harness_stats_gather_p95_ms"] = _percentile(harness_stats_gather_ms, 0.95)
    tier_result["frame_max_ms"] = frame_max_ms
    tier_result["frame_p95_to_avg_ratio"] = frame_p95_to_avg_ratio
    # Raw-vs-retained sample counts document the warm-up discard for calibration
    # evidence (#630): frame_raw_sample_count is the captured window,
    # frame_sample_count is what fed avg/p95 after trim_warmup().
    tier_result["frame_raw_sample_count"] = frame_times_ms.size()
    tier_result["frame_sample_count"] = measured_frame_times_ms.size()
    tier_result["source_data_available"] = source_data_available
    tier_result["source_data_status"] = source_data_status
    tier_result["sort_evidence_available"] = bool(sort_evidence.get("available", false))
    tier_result["sort_evidence_status"] = String(sort_evidence.get("status", "missing"))
    tier_result["sort_evidence_details"] = sort_evidence.get("details", {})
    tier_result["fallback_rate_available"] = fallback_rate_available
    tier_result["fallback_rate_status"] = fallback_rate_status
    tier_result["fallback_rate"] = fallback_rate
    tier_result["fallback_frames"] = fallback_frames
    tier_result["source_frame_count"] = source_frame_count
    tier_result["uploaded_splats"] = int(residency.get("uploaded_splats", 0))
    tier_result["visible_splats"] = int(residency.get("visible_splats", 0))
    tier_result["total_splats"] = int(residency.get("total_splats", 0))
    tier_result["residency_ratio"] = float(residency.get("residency_ratio", 0.0))
    tier_result["visible_ratio"] = float(residency.get("visible_ratio", 0.0))
    tier_result["sort_avg_ms"] = _read_stat_float(stats, ["gpu_sorter_avg_sort_ms", "gpu_sorter_avg_sort_time_ms"])
    tier_result["queue_pressure_active_frames"] = queue_pressure_active_frames
    tier_result["queue_pressure_candidate_frames"] = queue_pressure_candidate_frames
    tier_result["queue_pressure_no_progress_frames"] = queue_pressure_no_progress_frames
    tier_result["queue_pressure_scan_starved_frames"] = queue_pressure_scan_starved_frames
    tier_result["queue_pressure_no_progress_ratio"] = queue_pressure_no_progress_ratio
    tier_result["queue_pressure_scan_starved_ratio"] = queue_pressure_scan_starved_ratio
    tier_result["queue_pressure_reason_source"] = last_queue_pressure_reason_source
    tier_result["queue_pressure_reason_sources"] = queue_pressure_reason_sources
    tier_result["pack_queue_latency_max_ms"] = max_pack_queue_latency_max_ms
    tier_result["upload_queue_latency_max_ms"] = max_upload_queue_latency_max_ms
    tier_result["pack_mutex_wait_max_ms"] = max_pack_mutex_wait_max_ms
    tier_result["scheduler_queue_pressure_scan_throttle_active"] = (
        saw_scheduler_queue_pressure_scan_throttle_active if scheduler_queue_pressure_scan_throttle_active_known else null
    )
    tier_result["scheduler_update_cpu_p95_ms"] = scheduler_update_cpu_p95_ms
    tier_result["scheduler_cpu_total_attributed_p95_ms"] = scheduler_cpu_total_attributed_p95_ms
    await _teardown_world_scene()
    return tier_result

## Validates GPU sort metrics and history for the given dataset size.
## @param method_name: Sorting method label.
## @param size: Dataset size.
## @param stats: Renderer stats dictionary.
## @param history: Sort metrics history array.
func _validate_sort_metrics(method_name: String, size: int, stats: Dictionary, history: Array) -> void:
    var total_sorts = _read_stat_int(stats, ["gpu_sorter_total_sorts"])
    if total_sorts <= 0:
        _record_failure(
            "[Streaming] Expected GPU sorter to run at least once for %s" % method_name,
            {"dataset_size": size, "gpu_sorter_total_sorts": total_sorts}
        )

    var algorithm_label = String(stats.get("gpu_sorter_algorithm", ""))
    if algorithm_label.is_empty() or algorithm_label.to_lower().find("radix") == -1:
        _record_failure(
            "[Streaming] Unexpected sorter label '%s' for %s" % [algorithm_label, method_name],
            {"dataset_size": size}
        )

    var avg_sort = _read_stat_float(stats, ["gpu_sorter_avg_sort_ms", "gpu_sorter_avg_sort_time_ms"])
    if avg_sort <= 0.0:
        _record_failure(
            "[Streaming] Missing average sort timing for %s" % method_name,
            {"dataset_size": size, "available_keys": stats.keys()}
        )

    var target = TARGET_SORT_MS
    if avg_sort > target * 1.5:
        push_warning("[Streaming] %s average sort time %.2f ms exceeds target %.2f ms" % [method_name, avg_sort, target])

    if history.is_empty():
        _record_failure(
            "[Streaming] Sort history missing for %s" % method_name,
            {"dataset_size": size}
        )
    else:
        var last_entry: Dictionary = history[-1]
        var sorted_elements = int(last_entry.get("elements", 0))
        if sorted_elements <= 0:
            _record_failure(
                "[Streaming] Sort history reported zero sorted elements for %s" % method_name,
                {"dataset_size": size, "last_entry": last_entry}
            )

        var total_ms = float(last_entry.get("total_ms", 0.0))
        if total_ms <= 0.0:
            total_ms = float(last_entry.get("gpu_ms", 0.0))
        if total_ms <= 0.0:
            total_ms = float(last_entry.get("cpu_ms", 0.0))
        var throughput = float(sorted_elements) / max(0.0001, total_ms) / 1000.0
        if throughput <= 0.0:
            _record_failure(
                "[Streaming] Invalid throughput computed for %s" % method_name,
                {"dataset_size": size, "last_entry": last_entry}
            )

## Validates residency and visibility ratios for the dataset.
## @param size: Dataset size.
## @param stats: Renderer stats dictionary.
func _validate_residency(size: int, stats: Dictionary) -> Dictionary:
    var residency_result: Dictionary = {
        "uploaded_splats": 0,
        "visible_splats": 0,
        "total_splats": 0,
        "residency_ratio": 0.0,
        "visible_ratio": 0.0
    }

    # `uploaded_splat_count` is reset in render_data_orchestrator.cpp but the
    # streaming render path never increments it, so it reports 0 for streaming
    # worlds even when the pipeline is fully loaded. Use renderer-side
    # residency proxies (`buffer_manager_count`, `total_splats`) as the primary
    # signal, and fall back to `visible_splats` only when the streaming atlas
    # custom monitor is registered — mirroring benchmark_suite_lane.gd's
    # `_proof_last_uploaded_splats → _proof_last_visible_splats` gate.
    var visible = _read_stat_int_max(stats, ["visible_after_culling", "visible_splats", "cull_cpu_visible_count"])
    var total = _read_stat_int_max(stats, ["total_splats", "buffer_manager_count"])
    var residency_numerator := _read_stat_int_max(stats, ["buffer_manager_count", "total_splats"])
    var atlas_monitor_available := (
        Performance.has_custom_monitor("gaussian_splatting/streaming_loaded_chunks")
        or Performance.has_custom_monitor("gaussian_splatting/streaming_atlas_published_chunks")
    )
    if residency_numerator == 0 and atlas_monitor_available and visible > 0:
        residency_numerator = visible

    residency_result["uploaded_splats"] = residency_numerator
    if residency_numerator < size * 0.75:
        _record_failure(
            "[Streaming] Residency dropped below 75%% (%d/%d)" % [residency_numerator, size],
            {"dataset_size": size}
        )

    residency_result["visible_splats"] = visible
    residency_result["total_splats"] = total
    if total <= 0:
        _record_failure(
            "[Streaming] Total splat count missing after streaming",
            {"dataset_size": size, "stats": stats}
        )
        return residency_result

    residency_result["residency_ratio"] = float(residency_numerator) / float(max(1, size))
    var visible_ratio = float(visible) / float(total)
    residency_result["visible_ratio"] = visible_ratio
    if visible_ratio < (1.0 - MAX_VISIBLE_RATIO_DROP):
        push_warning("[Streaming] Visible ratio %.2f below expected threshold" % visible_ratio)

    if manager != null:
        var globals = manager.get_global_stats()
        var global_total = int(globals.get("total_gaussians", 0))
        if global_total < size:
            _record_failure(
                "[Streaming] Manager global residency %d < dataset %d" % [global_total, size],
                {"global_stats": globals}
            )
    return residency_result

## Creates random GaussianData for streaming stress tests.
## @param size: Number of splats to generate.
## @return GaussianData populated with positions/colors/scales.
func _build_dataset(size: int) -> GaussianData:
    var data = GaussianData.new()
    var positions = PackedVector3Array()
    var colors = PackedColorArray()
    var scales = PackedVector3Array()
    var sh_dc = PackedFloat32Array()
    var opacities = PackedFloat32Array()

    var rng = RandomNumberGenerator.new()
    rng.seed = 0xFACEB00C ^ size

    data.resize(size)
    positions.resize(size)
    colors.resize(size)
    scales.resize(size)
    sh_dc.resize(size * 3)
    opacities.resize(size)

    for i in range(size):
        var pos = Vector3(
            rng.randf_range(-60.0, 60.0),
            rng.randf_range(-60.0, 60.0),
            rng.randf_range(-60.0, 60.0)
        )
        positions[i] = pos
        colors[i] = Color(rng.randf(), rng.randf(), rng.randf(), 0.85)
        var sh_index = i * 3
        sh_dc[sh_index + 0] = colors[i].r
        sh_dc[sh_index + 1] = colors[i].g
        sh_dc[sh_index + 2] = colors[i].b
        opacities[i] = colors[i].a
        var scale = rng.randf_range(0.1, 1.0)
        scales[i] = Vector3.ONE * scale

    data.set_positions(positions)
    data.set_scales(scales)
    data.set_opacities(opacities)
    data.set_spherical_harmonics(sh_dc)
    return data

func _build_world_resource(dataset: GaussianData) -> GaussianSplatWorld:
    var world := GaussianSplatWorld.new()
    world.set_gaussian_data(dataset)
    world.set_bounds(dataset.get_aabb())
    return world

func _setup_world_scene(dataset: GaussianData) -> bool:
    scene_root = Node3D.new()
    scene_root.name = "GpuStreamingStressRoot"
    get_root().add_child(scene_root)

    camera = Camera3D.new()
    camera.name = "GpuStreamingStressCamera"
    camera.position = Vector3(0.0, 0.0, 6.0)
    camera.look_at(Vector3.ZERO, Vector3.UP)
    camera.make_current()
    scene_root.add_child(camera)

    world_node = GaussianSplatWorld3D.new()
    world_node.name = "GpuStreamingStressWorld"
    scene_root.add_child(world_node)

    var world := _build_world_resource(dataset)
    world_node.set_world(world)
    world_node.apply_world()
    return true

func _teardown_world_scene() -> void:
    var pending := scene_root
    if pending != null:
        pending.queue_free()
    scene_root = null
    camera = null
    world_node = null
    renderer = null
    if pending == null:
        return
    # Wait until the queued scene root has actually exited the tree and been
    # freed before returning. GaussianSplatWorld3D releases its shared-renderer
    # submission ownership only on NOTIFICATION_EXIT_TREE (see
    # gaussian_splat_world_3d.cpp:113), and the shared renderer is re-bound
    # whenever a new GaussianSplatWorld3D enters the tree via _ensure_renderer()
    # (line 92 of the same file). Without this barrier, the next tier's
    # _setup_world_scene() runs synchronously after queue_free() in the same
    # frame, so the new world binds the same shared renderer while the old
    # world's submission state is still resident — which produced stale
    # 250K totals on tier_1m after tier_250k. Bound the wait so a stuck free
    # cannot hang the test.
    for _i in range(8):
        if not is_instance_valid(pending):
            break
        await process_frame
