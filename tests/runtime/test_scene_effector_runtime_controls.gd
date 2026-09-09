extends SceneTree

const ASSET_PATH := "res://tests/fixtures/test_splats.ply"
const GsRuntimeReport := preload("gs_runtime_report.gd")
const SKIP_MARKER := GsRuntimeReport.SKIP_MARKER
const FAIL_MARKER := GsRuntimeReport.FAIL_MARKER
const METRICS_MARKER := GsRuntimeReport.METRICS_MARKER
const MAX_RENDERER_WAIT_FRAMES := 120

var failures: Array[String] = []

# T3 (#891): registry name from GDS_TESTS in run_runtime_validation.py.
var _report := GsRuntimeReport.new("Scene Effector Runtime Controls")
var metrics := {
	"frames": 0,
	"baseline_matched_count": 0,
	"baseline_position_active": false,
	"baseline_opacity_active": false,
	"baseline_bound_count": 0,
	"baseline_selected_name": "",
	"baseline_truncated": false,
	"combined_wind_sphere_active": false,
	"outside_subtree_matched_count": 0,
	"world_scope_outside_matched_count": 0,
	"explicit_scope_matched_count": 0,
	"reparent_into_subtree_matched_count": 0,
	"reparent_outside_subtree_matched_count": 0,
	"reparent_effector_widened_matched_count": 0,
	"target_opacity_partial_active": false,
	"target_opacity_neutral_active": false,
}

var scene_root: Node3D
var runtime_camera: Camera3D
var effect_group: Node3D
var default_branch: Node3D
var scoped_branch: Node3D
var outside_group: Node3D
var effector: SphereEffector3D
var node_a: GaussianSplatNode3D
var node_b: GaussianSplatNode3D
var node_c: GaussianSplatNode3D
var node_d: GaussianSplatNode3D
var node_e: GaussianSplatNode3D

func _init() -> void:
	call_deferred("_run")

func _is_headless_runtime() -> bool:
	return OS.has_feature("headless") or DisplayServer.get_name() == "headless"

func _record_check(condition: bool, label: String, context: Dictionary = {}) -> void:
	if condition:
		_report.ok()
		print("PASS ", label)
		return
	_record_failure(label, context)

func _record_failure(label: String, context: Dictionary = {}) -> void:
	var reason := label
	if not context.is_empty():
		reason = "%s | context=%s" % [label, str(context)]
	failures.append(reason)
	push_error("%s %s" % [FAIL_MARKER, reason])

func _emit_metrics(status: String, reason: String = "") -> void:
	var payload := metrics.duplicate(true)
	payload["status"] = status
	if reason != "":
		payload["reason"] = reason
	print("%s %s" % [METRICS_MARKER, JSON.stringify(payload)])

func _cleanup() -> void:
	if scene_root != null:
		scene_root.queue_free()
	scene_root = null
	runtime_camera = null
	effect_group = null
	default_branch = null
	scoped_branch = null
	outside_group = null
	effector = null
	node_a = null
	node_b = null
	node_c = null
	node_d = null
	node_e = null

func _create_splat_node(asset: GaussianSplatAsset, node_name: String, node_position: Vector3) -> GaussianSplatNode3D:
	var node := GaussianSplatNode3D.new()
	node.name = node_name
	node.splat_asset = asset
	node.position = node_position
	node.set_effect_position_scale(1.0)
	node.set_effect_opacity_scale(1.0)
	return node

func _await_runtime_sync(frame_count: int = 2) -> void:
	for i in range(frame_count):
		await process_frame

func _record_node_state(node: GaussianSplatNode3D, label: String, expected_count: int, expect_position_active: bool, expect_opacity_active: bool) -> void:
	var matched_count := int(node.get_last_matched_scene_effector_count())
	var position_active := bool(node.is_scene_effector_position_active())
	var opacity_active := bool(node.is_scene_effector_opacity_active())
	var context := {
		"matched_count": matched_count,
		"position_active": position_active,
		"opacity_active": opacity_active,
	}
	_record_check(matched_count == expected_count, "%s matched count" % label, context)
	_record_check(position_active == expect_position_active, "%s position activity" % label, context)
	_record_check(opacity_active == expect_opacity_active, "%s opacity activity" % label, context)

func _record_statistics_surface(node: GaussianSplatNode3D, label: String) -> void:
	var stats := node.get_statistics()
	_record_check(stats.has("matched_scene_effectors"), "%s statistics include matched_scene_effectors" % label, {"stats": stats})
	_record_check(stats.has("bound_scene_effectors"), "%s statistics include bound_scene_effectors" % label, {"stats": stats})
	_record_check(stats.has("scene_effector_truncated"), "%s statistics include scene_effector_truncated" % label, {"stats": stats})
	_record_check(stats.has("scene_effector_position_active"), "%s statistics include scene_effector_position_active" % label, {"stats": stats})
	_record_check(stats.has("scene_effector_opacity_active"), "%s statistics include scene_effector_opacity_active" % label, {"stats": stats})
	if stats.has("matched_scene_effectors"):
		_record_check(
			int(stats["matched_scene_effectors"]) == int(node.get_last_matched_scene_effector_count()),
			"%s statistics matched_scene_effectors mirrors runtime query" % label,
			{"stats": stats}
		)
	if stats.has("scene_effector_position_active"):
		_record_check(
			bool(stats["scene_effector_position_active"]) == bool(node.is_scene_effector_position_active()),
			"%s statistics position flag mirrors runtime query" % label,
			{"stats": stats}
		)
	if stats.has("scene_effector_opacity_active"):
		_record_check(
			bool(stats["scene_effector_opacity_active"]) == bool(node.is_scene_effector_opacity_active()),
			"%s statistics opacity flag mirrors runtime query" % label,
			{"stats": stats}
		)

func _record_debug_state_surface(
	node: GaussianSplatNode3D,
	label: String,
	expected_matched_count: int,
	expected_bound_count: int,
	expect_position_active: bool,
	expect_opacity_active: bool,
	expected_selected_names: Array = [],
	expected_truncated: bool = false,
	extra_expectations: Dictionary = {}
) -> void:
	var state := node.get_scene_effector_debug_state()
	var required_keys := [
		"matched_count",
		"bound_count",
		"truncated",
		"position_active",
		"opacity_active",
		"selected_effector_ids",
		"selected_effector_names",
	]
	for key in required_keys:
		_record_check(state.has(key), "%s debug state includes %s" % [label, key], {"state": state})

	var selected_names: Array = state.get("selected_effector_names", [])
	var context := {
		"state": state,
		"selected_names": selected_names,
	}
	_record_check(int(state.get("matched_count", -1)) == expected_matched_count, "%s debug matched_count" % label, context)
	_record_check(int(state.get("bound_count", -1)) == expected_bound_count, "%s debug bound_count" % label, context)
	_record_check(bool(state.get("truncated", true)) == expected_truncated, "%s debug truncated flag" % label, context)
	_record_check(bool(state.get("position_active", false)) == expect_position_active, "%s debug position flag" % label, context)
	_record_check(bool(state.get("opacity_active", false)) == expect_opacity_active, "%s debug opacity flag" % label, context)
	_record_check(selected_names == expected_selected_names, "%s debug selected names" % label, context)

	for key in extra_expectations.keys():
		_record_check(
			state.get(key) == extra_expectations[key],
			"%s debug %s expectation" % [label, key],
			{"state": state, "expected": extra_expectations}
		)

func _reparent_preserve_global(node: Node3D, new_parent: Node3D) -> void:
	if node == null or new_parent == null:
		return
	var previous_transform := node.global_transform
	var current_parent := node.get_parent()
	if current_parent != null:
		current_parent.remove_child(node)
	new_parent.add_child(node)
	node.global_transform = previous_transform

func _setup_scene() -> bool:
	scene_root = Node3D.new()
	scene_root.name = "SceneEffectorRuntimeControls"
	root.add_child(scene_root)

	runtime_camera = Camera3D.new()
	runtime_camera.name = "RuntimeCamera"
	runtime_camera.position = Vector3(0.0, 2.0, 8.0)
	runtime_camera.look_at(Vector3.ZERO, Vector3.UP)
	runtime_camera.make_current()
	scene_root.add_child(runtime_camera)

	effect_group = Node3D.new()
	effect_group.name = "EffectGroup"
	scene_root.add_child(effect_group)

	default_branch = Node3D.new()
	default_branch.name = "DefaultBranch"
	effect_group.add_child(default_branch)

	scoped_branch = Node3D.new()
	scoped_branch.name = "ScopedBranch"
	effect_group.add_child(scoped_branch)

	outside_group = Node3D.new()
	outside_group.name = "OutsideGroup"
	scene_root.add_child(outside_group)

	var asset := GaussianSplatAsset.new()
	var load_err := asset.load_from_file(ASSET_PATH)
	if load_err != OK:
		_record_failure("Failed to load fixture asset", {"path": ASSET_PATH, "err": load_err})
		return false

	effector = SphereEffector3D.new()
	effector.name = "RuntimeEffector"
	effector.enabled = true
	effector.radius = 12.0
	effector.strength = 1.0
	effector.falloff = 2.0
	effector.frequency = 2.0
	effector.affect_position = true
	effector.affect_opacity = true
	effector.opacity_strength = 0.8
	effector.target_opacity = 0.35
	effect_group.add_child(effector)

	node_a = _create_splat_node(asset, "NodeA", Vector3(-1.5, 0.0, 0.0))
	default_branch.add_child(node_a)

	node_b = _create_splat_node(asset, "NodeB", Vector3(1.5, 0.0, 0.0))
	node_b.set_scene_effector_layer_mask(2)
	default_branch.add_child(node_b)

	node_c = _create_splat_node(asset, "NodeC", Vector3(0.0, 0.0, 0.0))
	outside_group.add_child(node_c)

	node_d = _create_splat_node(asset, "NodeD", Vector3(0.0, 0.0, -2.0))
	node_d.set_scene_effector_scope_root(NodePath(".."))
	scoped_branch.add_child(node_d)

	node_e = _create_splat_node(asset, "NodeE", Vector3(0.0, 0.0, 2.0))
	node_e.set_wind_override_enabled(true)
	node_e.set_wind_enabled(true)
	node_e.set_wind_strength(0.85)
	node_e.set_wind_direction(Vector3(0.0, 0.0, 1.0))
	node_e.set_wind_frequency(1.5)
	default_branch.add_child(node_e)

	return true

func _wait_for_renderer() -> GaussianSplatRenderer:
	for i in range(MAX_RENDERER_WAIT_FRAMES):
		await process_frame
		metrics["frames"] = i + 1
		if node_a != null:
			var renderer: GaussianSplatRenderer = node_a.get_renderer()
			if renderer != null:
				return renderer
	return null

func _run() -> void:
	if _is_headless_runtime():
		var skip_reason := "Scene effector runtime controls require non-headless execution."
		_emit_metrics("skipped", skip_reason)
		print("%s %s" % [SKIP_MARKER, skip_reason])
		quit(0)
		return

	if not _setup_scene():
		_emit_metrics("failed", "scene_setup_failed")
		_cleanup()
		quit(1)
		return

	var renderer := await _wait_for_renderer()
	if renderer == null:
		var skip_reason := "Renderer unavailable (local RenderingDevice required)."
		_emit_metrics("skipped", skip_reason)
		print("%s %s" % [SKIP_MARKER, skip_reason])
		_cleanup()
		quit(0)
		return

	await _await_runtime_sync()
	metrics["baseline_matched_count"] = node_a.get_last_matched_scene_effector_count()
	metrics["baseline_position_active"] = node_a.is_scene_effector_position_active()
	metrics["baseline_opacity_active"] = node_a.is_scene_effector_opacity_active()
	metrics["outside_subtree_matched_count"] = node_c.get_last_matched_scene_effector_count()
	var baseline_debug_state := node_a.get_scene_effector_debug_state()
	metrics["baseline_bound_count"] = int(baseline_debug_state.get("bound_count", 0))
	metrics["baseline_truncated"] = bool(baseline_debug_state.get("truncated", false))
	var baseline_names: Array = baseline_debug_state.get("selected_effector_names", [])
	if not baseline_names.is_empty():
		metrics["baseline_selected_name"] = str(baseline_names[0])
	metrics["combined_wind_sphere_active"] = false

	_record_node_state(node_a, "Node A baseline subtree match", 1, true, true)
	_record_debug_state_surface(node_a, "Node A baseline subtree match", 1, 1, true, true, ["RuntimeEffector"], false)
	_record_node_state(node_b, "Node B layer-mask exclusion", 0, false, false)
	_record_node_state(node_c, "Node C default subtree exclusion", 0, false, false)
	_record_node_state(node_d, "Node D node-side scope narrowing", 0, false, false)
	_record_debug_state_surface(
		node_d,
		"Node D node-side scope narrowing",
		0,
		0,
		false,
		false,
		[],
		false,
		{
			"scope_filter_present": true,
			"scope_filter_valid": true,
		}
	)
	_record_node_state(node_e, "Node E combined wind + sphere baseline", 1, true, true)
	_record_debug_state_surface(node_e, "Node E combined wind + sphere baseline", 1, 1, true, true, ["RuntimeEffector"], false)
	_record_statistics_surface(node_a, "Node A")
	_record_statistics_surface(node_e, "Node E")
	metrics["combined_wind_sphere_active"] = node_e.is_scene_effector_position_active() and node_e.is_scene_effector_opacity_active()

	node_e.set_wind_enabled(false)
	await _await_runtime_sync()
	_record_node_state(node_e, "Node E sphere path survives wind disable", 1, true, true)
	_record_debug_state_surface(node_e, "Node E sphere path survives wind disable", 1, 1, true, true, ["RuntimeEffector"], false)

	node_e.set_wind_enabled(true)
	node_e.set_wind_frequency(2.25)
	node_e.set_wind_direction(Vector3(1.0, 0.0, 0.25))
	await _await_runtime_sync()
	_record_node_state(node_e, "Node E combined wind + sphere after wind retune", 1, true, true)

	node_b.set_scene_effector_layer_mask(1)
	await _await_runtime_sync()
	_record_node_state(node_b, "Node B layer-mask opt-in", 1, true, true)

	node_b.set_scene_effector_layer_mask(2)
	await _await_runtime_sync()
	_record_node_state(node_b, "Node B layer-mask opt-out", 0, false, false)

	effector.scope_mode = SphereEffector3D.SCOPE_WORLD
	await _await_runtime_sync()
	metrics["world_scope_outside_matched_count"] = node_c.get_last_matched_scene_effector_count()
	_record_node_state(node_a, "Node A world scope", 1, true, true)
	_record_node_state(node_c, "Node C world scope inclusion", 1, true, true)
	_record_node_state(node_d, "Node D world scope still filtered by node scope", 0, false, false)

	effector.scope_mode = SphereEffector3D.SCOPE_EXPLICIT_ROOT
	effector.scope_root = NodePath("../ScopedBranch")
	await _await_runtime_sync()
	metrics["explicit_scope_matched_count"] = node_d.get_last_matched_scene_effector_count()
	_record_node_state(node_a, "Node A explicit-root exclusion", 0, false, false)
	_record_node_state(node_c, "Node C explicit-root exclusion", 0, false, false)
	_record_node_state(node_d, "Node D explicit-root inclusion", 1, true, true)
	_record_statistics_surface(node_d, "Node D")

	effector.scope_mode = SphereEffector3D.SCOPE_SUBTREE
	effector.scope_root = NodePath("")
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A subtree scope restored", 1, true, true)
	_record_node_state(node_c, "Node C subtree scope restored", 0, false, false)
	_record_node_state(node_d, "Node D subtree scope restored", 0, false, false)

	_reparent_preserve_global(node_c, default_branch)
	await _await_runtime_sync()
	metrics["reparent_into_subtree_matched_count"] = node_c.get_last_matched_scene_effector_count()
	_record_node_state(node_c, "Node C reparented into subtree", 1, true, true)
	_record_debug_state_surface(node_c, "Node C reparented into subtree", 1, 1, true, true, ["RuntimeEffector"], false)

	_reparent_preserve_global(node_c, outside_group)
	await _await_runtime_sync()
	metrics["reparent_outside_subtree_matched_count"] = node_c.get_last_matched_scene_effector_count()
	_record_node_state(node_c, "Node C reparented back outside subtree", 0, false, false)

	_reparent_preserve_global(effector, scene_root)
	await _await_runtime_sync()
	metrics["reparent_effector_widened_matched_count"] = node_c.get_last_matched_scene_effector_count()
	_record_node_state(node_a, "Node A matched after effector reparent", 1, true, true)
	_record_node_state(node_c, "Node C matched after effector reparent to scene root", 1, true, true)
	_record_node_state(node_d, "Node D remains narrowed after effector reparent", 0, false, false)

	_reparent_preserve_global(effector, effect_group)
	await _await_runtime_sync()
	_record_node_state(node_c, "Node C excluded after effector reparent restore", 0, false, false)

	effector.enabled = false
	await _await_runtime_sync()
	_record_node_state(node_a, "Disabled effector", 0, false, false)
	_record_node_state(node_c, "Disabled effector outside subtree", 0, false, false)

	effector.enabled = true
	effector.target_opacity = 0.6
	await _await_runtime_sync()
	metrics["target_opacity_partial_active"] = node_a.is_scene_effector_opacity_active()
	_record_node_state(node_a, "Re-enabled effector with partial target opacity", 1, true, true)

	effector.affect_opacity = false
	await _await_runtime_sync()
	_record_node_state(node_a, "Opacity channel disabled at runtime", 1, true, false)

	effector.affect_opacity = true
	effector.target_opacity = 1.0
	await _await_runtime_sync()
	metrics["target_opacity_neutral_active"] = node_a.is_scene_effector_opacity_active()
	_record_node_state(node_a, "Neutral target opacity remains matched but inert", 1, true, false)

	effector.target_opacity = 0.6
	await _await_runtime_sync()
	_record_node_state(node_a, "Partial target opacity restores active opacity channel", 1, true, true)

	node_a.set_scene_effectors_enabled(false)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A scene-effector toggle off", 0, false, false)

	node_a.set_scene_effectors_enabled(true)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A scene-effector toggle on", 1, true, true)

	node_a.set_effect_position_scale(0.0)
	node_a.set_effect_opacity_scale(0.0)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A zero response scales", 1, false, false)

	node_a.set_effect_position_scale(1.0)
	node_a.set_effect_opacity_scale(1.0)
	node_a.set_opacity(0.0)
	await _await_runtime_sync()
	_record_node_state(node_a, "Node A base opacity zero", 1, true, false)
	_record_statistics_surface(node_a, "Node A after base opacity zero")

	if failures.is_empty():
		_report.emit_pass()
	_emit_metrics("passed" if failures.is_empty() else "failed")
	_cleanup()
	quit(0 if failures.is_empty() else 1)
