extends SceneTree

# Painterly API-surface smoke check for the test project.
#
# WHAT IT CHECKS, AND WHAT IT DELIBERATELY DOES NOT
#
# #997: this script used to assert only `visible_splats > 0` with painterly
# enabled -- a count the BASELINE raster reports identically. It supplied no
# PainterlyMaterial, so RasterStage rejected the painterly path with
# PAINTERLY_MATERIAL_UNAVAILABLE, rendered the baseline, and still printed
# PAINTERLY_TEST_PASSED. It could not go red for any painterly defect.
#
# It now checks the thing that was actually broken and that IS checkable here:
# `painterly/material` must exist as a property and must round-trip. Godot
# discards a write to an unbound property in silence, which is exactly how four
# authored materials in the shipped demo scenes were dropped at scene load.
#
# It does NOT claim to verify the painterly RENDER PATH. The audit runbook
# (modules/gaussian_splatting/tests/PAINTERLY_AUDIT_RUNBOOK.md) invokes this
# with --headless, where GaussianSplatSceneDirector refuses to create a renderer
# without a primary RenderingDevice, and this script builds no camera or
# viewport for splats to be drawn into. A render-path assertion here would
# therefore be either always-skipped or a false failure. The pixel-level proof
# -- painterly frame measurably different from the baseline frame -- lives in
# tests/runtime/test_painterly_material_render.gd, which builds its own
# SubViewport and camera and runs on the GPU lanes (release-ci,
# painterly-gpu-ci). This script is listed in the ALLOWLIST of
# tests/ci/check_painterly_test_non_vacuity.py for exactly that reason.
#
# It `extends SceneTree`, not Node: the runbook runs it via `--script`, and
# main/main.cpp rejects a script that does not inherit SceneTree or MainLoop
# ("Can't load the script ... as it doesn't inherit from SceneTree or
# MainLoop"). As a Node it aborted before _ready() ever existed, so neither the
# old assertions nor any new one ran at all.
#
# The material is built inline rather than loaded from
# res://tests/fixtures/painterly_reference_material.tres because that fixture
# lives at the REPOSITORY root, a different `res://` from this test project's.

func _init() -> void:
    call_deferred("_run")


func _run() -> void:
    var failures: Array[String] = []
    # Explicitly typed, not `:=`. ClassDB.instantiate() returns Variant and this
    # project treats "type inferred from a Variant value" as an error, so the
    # inferred form made the whole script unloadable -- which is the second,
    # independent reason (alongside `extends Node` under --script) that this
    # file's assertions have never executed.
    var splat_obj: Variant = ClassDB.instantiate("GaussianSplatNode3D")
    if splat_obj == null or not (splat_obj is Node):
        push_error("GaussianSplatNode3D class unavailable; painterly smoke test cannot run.")
        print("PAINTERLY_TEST_FAILED")
        quit(1)
        return

    var splat_node: Node = splat_obj
    var scene_root := Node3D.new()
    scene_root.name = "PainterlySmokeRoot"
    get_root().add_child(scene_root)
    scene_root.add_child(splat_node)

    if not splat_node.has_method("set_splat_data"):
        failures.append("set_splat_data method unavailable")
    else:
        var positions := PackedVector3Array([Vector3(0.0, 0.0, -3.0)])
        var colors := PackedColorArray([Color(1.0, 0.95, 0.9, 0.9)])
        var scales := PackedVector3Array([Vector3(0.4, 0.4, 0.4)])
        splat_node.call("set_splat_data", positions, colors, scales)

    if not splat_node.has_method("set_enable_painterly"):
        failures.append("set_enable_painterly method unavailable")
    if not splat_node.has_method("force_update"):
        failures.append("force_update method unavailable")

    # The check this file existed to make and never made.
    var material: Resource = _make_material()
    if material == null:
        failures.append("PainterlyMaterial class unavailable")
    else:
        var property_names: Array = []
        for entry in splat_node.get_property_list():
            property_names.append(str(entry.get("name", "")))
        if not property_names.has("painterly/material"):
            failures.append(
                "GaussianSplatNode3D exposes no 'painterly/material' property; " +
                "every authored painterly material is discarded at scene load (#997)")
        else:
            splat_node.set("painterly/material", material)
            if splat_node.get("painterly/material") != material:
                failures.append(
                    "'painterly/material' did not round-trip; the assignment was discarded")

    if failures.is_empty():
        splat_node.call("set_enable_painterly", true)
        splat_node.call("force_update")
        await process_frame
        var visible_after_enable := _read_visible_splats(splat_node)

        splat_node.call("set_enable_painterly", false)
        splat_node.call("force_update")
        await process_frame
        var visible_after_disable := _read_visible_splats(splat_node)

        splat_node.call("set_enable_painterly", true)
        splat_node.call("force_update")
        await process_frame
        var visible_after_reenable := _read_visible_splats(splat_node)

        print("[Painterly Test] visible (enabled/disabled/re-enabled): %d / %d / %d" % [
            visible_after_enable,
            visible_after_disable,
            visible_after_reenable
        ])

        # The toggle-safety half needs a renderer, and the scene director
        # refuses to create one without a primary RenderingDevice -- which the
        # runbook's --headless invocation never has. Asserting `> 0` there is a
        # guaranteed failure that says nothing about painterly, so the
        # availability of the renderer decides whether this half is a real
        # assertion or an honestly-reported gap. Absence of the count is never
        # reported as a pass.
        var toggle_renderer: Variant = splat_node.call("get_renderer") if splat_node.has_method("get_renderer") else null
        if toggle_renderer == null:
            print("[Painterly Test] NOT VERIFIED: splat-visibility across the painterly toggle. " +
                "No GaussianSplatRenderer exists (headless / no RenderingDevice), so the counts above are structurally 0.")
        else:
            if visible_after_enable <= 0:
                failures.append("visible splats with painterly enabled: %d" % visible_after_enable)
            if visible_after_disable <= 0:
                failures.append("visible splats with painterly disabled: %d" % visible_after_disable)
            if visible_after_reenable <= 0:
                failures.append("visible splats after re-enabling painterly: %d" % visible_after_reenable)

    # Say plainly what this run did NOT establish, so the PASSED marker below
    # cannot be read as "painterly renders correctly".
    print("[Painterly Test] SCOPE: property surface + toggle safety only. " +
        "The painterly RENDER PATH is NOT verified here -- run " +
        "tests/runtime/test_painterly_material_render.gd on a GPU lane for that proof.")

    scene_root.queue_free()

    if failures.is_empty():
        print("PAINTERLY_TEST_PASSED")
        quit(0)
        return

    for failure in failures:
        push_error("[Painterly Test] %s" % failure)
    print("PAINTERLY_TEST_FAILED")
    quit(1)


func _make_material() -> Resource:
    var instantiated: Variant = ClassDB.instantiate("PainterlyMaterial")
    if instantiated == null or not (instantiated is Resource):
        return null
    var m: Resource = instantiated
    m.set("palette_quantization_enabled", true)
    m.set("brush_modulation_enabled", true)
    m.set("lighting_stylization_enabled", true)
    m.set("painterly_mix_strength", 0.85)
    return m


func _read_visible_splats(splat_node: Node) -> int:
    if splat_node.has_method("get_statistics"):
        var stats = splat_node.call("get_statistics")
        if typeof(stats) == TYPE_DICTIONARY:
            return int(stats.get("visible_splats", -1))
    if splat_node.has_method("get_visible_splat_count"):
        return int(splat_node.call("get_visible_splat_count"))
    return -1
