extends SceneTree

const GsRuntimeReport := preload("gs_runtime_report.gd")
const FAIL_MARKER := GsRuntimeReport.FAIL_MARKER

const TELEMETRY_MONITOR_ID := "gaussian_splatting/telemetry_active"
const CREATE_DESTROY_CYCLES := 3

# T3 (#891): registry name from GDS_TESTS in run_runtime_validation.py.
var _report := GsRuntimeReport.new("Monitor Lifecycle Hardening")


func _init() -> void:
    call_deferred("_run")


func _record_failure(reason: String, context: Dictionary = {}) -> void:
    var message := reason
    if not context.is_empty():
        message = "%s | context=%s" % [reason, str(context)]
    push_error("%s %s" % [FAIL_MARKER, message])
    quit(1)


func _collect_gaussian_monitor_names() -> Array:
    var names: Array = []
    for monitor_name in Performance.get_custom_monitor_names():
        var monitor_id := str(monitor_name)
        if monitor_id.begins_with("gaussian_splatting/"):
            names.append(monitor_id)
    names.sort()
    return names


func _count_occurrences(values: Array, target: String) -> int:
    var count := 0
    for value in values:
        if str(value) == target:
            count += 1
    return count


func _has_duplicates(values: Array) -> bool:
    var seen := {}
    for value in values:
        var key := str(value)
        if seen.has(key):
            return true
        seen[key] = true
    return false


func _monitor_int(monitor_id: String) -> int:
    if not Performance.has_custom_monitor(monitor_id):
        return -1
    return int(Performance.get_custom_monitor(monitor_id))


func _wait_for_telemetry_value(expected: int, max_frames: int) -> bool:
    for _index in range(max_frames):
        if _monitor_int(TELEMETRY_MONITOR_ID) == expected:
            return true
        await process_frame
    return false


func _run() -> void:
    if not Performance.has_custom_monitor(TELEMETRY_MONITOR_ID):
        _record_failure("Missing telemetry lifecycle monitor", {"monitor": TELEMETRY_MONITOR_ID})
        return
    _report.ok()

    var baseline_monitors := _collect_gaussian_monitor_names()
    if baseline_monitors.is_empty():
        _record_failure("Gaussian monitor set is unexpectedly empty at runtime start")
        return
    _report.ok()
    if _has_duplicates(baseline_monitors):
        _record_failure("Gaussian monitor set contains duplicate IDs", {"monitors": baseline_monitors})
        return
    _report.ok()
    if _count_occurrences(baseline_monitors, TELEMETRY_MONITOR_ID) != 1:
        _record_failure("Telemetry lifecycle monitor is not unique", {"monitors": baseline_monitors})
        return
    _report.ok()

    var baseline_signature := JSON.stringify(baseline_monitors)
    var baseline_count := baseline_monitors.size()

    for cycle in range(CREATE_DESTROY_CYCLES):
        var renderer := GaussianSplatRenderer.new()
        if renderer == null:
            _record_failure("Failed to instantiate GaussianSplatRenderer", {"cycle": cycle})
            return
        _report.ok()

        if not await _wait_for_telemetry_value(1, 16):
            _record_failure("Telemetry did not activate after renderer creation", {
                "cycle": cycle,
                "telemetry_active": _monitor_int(TELEMETRY_MONITOR_ID)
            })
            return
        _report.ok()

        renderer = null

        if not await _wait_for_telemetry_value(0, 60):
            _record_failure("Telemetry remained active after renderer destruction", {
                "cycle": cycle,
                "telemetry_active": _monitor_int(TELEMETRY_MONITOR_ID)
            })
            return
        _report.ok()

        var current_monitors := _collect_gaussian_monitor_names()
        if _has_duplicates(current_monitors):
            _record_failure("Gaussian monitor set contains duplicates after lifecycle cycle", {
                "cycle": cycle,
                "monitors": current_monitors
            })
            return
        _report.ok()
        if current_monitors.size() != baseline_count:
            _record_failure("Gaussian monitor count changed across lifecycle cycle", {
                "cycle": cycle,
                "before_count": baseline_count,
                "after_count": current_monitors.size()
            })
            return
        _report.ok()
        if JSON.stringify(current_monitors) != baseline_signature:
            _record_failure("Gaussian monitor IDs changed across lifecycle cycle", {
                "cycle": cycle,
                "before": baseline_monitors,
                "after": current_monitors
            })
            return
        _report.ok()
        if _count_occurrences(current_monitors, TELEMETRY_MONITOR_ID) != 1:
            _record_failure("Telemetry lifecycle monitor count drifted", {
                "cycle": cycle,
                "monitor_count": _count_occurrences(current_monitors, TELEMETRY_MONITOR_ID)
            })
            return
        _report.ok()

    print("✅ Monitor lifecycle hardening checks passed.")
    _report.emit_pass()
    quit(0)
