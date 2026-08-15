extends RefCounted

# gs_runtime_report.gd -- shared runtime-scenario reporting library (T3 / #891,
# ADR adr-phase1-guard-hardening.md section 4.2).
#
# Owns the runtime marker constants and the assertion counter so scenarios do
# not each re-declare them. The scenario emits its own completion marker; the
# harness (tests/runtime/run_runtime_validation.py) only verifies it. The
# marker is bound to the scenario's REGISTRY name (the key in GDS_TESTS): the
# harness fails a run whose marker names a different scenario, which is what
# makes a copy-pasted emitter loud instead of a silent false pass.
#
# Usage, from a SceneTree-derived scenario script:
#
#     const GsRuntimeReport := preload("gs_runtime_report.gd")
#     var _report := GsRuntimeReport.new("Registry Name From GDS_TESTS")
#
#     _report.ok()                # after each check that passed
#     _report.emit_pass()         # terminal success path, right before quit(0)
#
# A scenario that legitimately asserts nothing calls
# `emit_pass({"no_assertions_reason": "why"})` AND needs an unexpired
# `zero_assertion_allowlist` entry in runtime_scenarios.json; either half alone
# fails the run (ADR section 4.3).
#
# Declared limit: the counter counts checks the scenario routes through ok();
# a check that neither fails the run nor calls ok() is invisible to the count.
# The step-2 assertion floor therefore ratchets what scenarios REPORT, and the
# report is only as complete as the scenario's instrumentation.

const PASS_MARKER := "[RUNTIME_PASS]"
const FAIL_MARKER := "[RUNTIME_FAIL]"
const SKIP_MARKER := "[RUNTIME_SKIP]"
const METRICS_MARKER := "[RUNTIME_METRICS]"

var scenario_name: String
var assertions: int = 0


func _init(p_scenario_name: String) -> void:
	scenario_name = p_scenario_name


# Record one verified check. Call after every assertion-shaped condition the
# scenario evaluated and found healthy (the failing branch reports through the
# scenario's own [RUNTIME_FAIL] path instead).
func ok() -> void:
	assertions += 1


# Emit the positive completion marker. One line, marker token + one JSON
# object, same grammar as [RUNTIME_METRICS]. Call exactly once, on the
# terminal success path, immediately before quit(0).
func emit_pass(extra: Dictionary = {}) -> void:
	var payload := {
		"scenario": scenario_name,
		"assertions": assertions,
	}
	for key in extra.keys():
		payload[key] = extra[key]
	print("%s %s" % [PASS_MARKER, JSON.stringify(payload)])
