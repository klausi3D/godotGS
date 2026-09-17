extends Node

# Painterly smoke test for the test project.
#
# #997: this script used to assert only `visible_splats > 0` with painterly
# enabled -- a count the BASELINE raster reports identically. It never supplied
# a PainterlyMaterial, so RasterStage rejected the painterly path with
# PAINTERLY_MATERIAL_UNAVAILABLE, rendered the baseline, and printed
# PAINTERLY_TEST_PASSED. It could not go red for any painterly defect.
#
# It now does two separable things, and says which one it did:
#
#   * API SURFACE (runs anywhere, headless included): the node must expose
#     `painterly/material`, and a material assigned to it must round-trip.
#     Godot discards a write to an unbound property in silence, so the
#     round-trip is the only way to see the defect this test missed for so long.
#   * RENDER PATH (needs a RenderingDevice): with that material assigned, the
#     painterly path must actually run -- `stage_raster_painterly` must be true.
#
# When no RenderingDevice is available the render-path half is reported as NOT
# VERIFIED, by name, and never counted as a pass. The full pixel-level proof
# (painterly frame differs from the baseline frame) lives in the runtime
# scenario tests/runtime/test_painterly_material_render.gd, which runs on the
# GPU lane; this script is the smoke check the audit runbook invokes.
#
# The material is built inline rather than loaded from
# res://tests/fixtures/painterly_reference_material.tres because that fixture
# lives at the REPOSITORY root, which is a different `res://` from this test
# project's. Keeping one authored copy per project root would drift; a valid
# material is all the painterly path requires.

const RENDER_PATH_WAIT_FRAMES := 90

func _ready() -> void:
    var failures: Array[String] = []
    var splat_obj := ClassDB.instantiate("GaussianSplatNode3D")
    if splat_obj == null or not (splat_obj is Node):
        push_error("GaussianSplatNode3D class unavailable; painterly smoke test cannot run.")
        print("PAINTERLY_TEST_FAILED")
        get_tree().quit(1)
        return

    var splat_node: Node = splat_obj
    add_child(splat_node)

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

    # --- API surface: painterly/material must exist and must round-trip. ---
    var material := _make_material()
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
        await get_tree().process_frame
        var visible_after_enable := _read_visible_splats(splat_node)
        if visible_after_enable <= 0:
            failures.append("visible splats with painterly enabled: %d" % visible_after_enable)

        splat_node.call("set_enable_painterly", false)
        splat_node.call("force_update")
        await get_tree().process_frame
        var visible_after_disable := _read_visible_splats(splat_node)
        if visible_after_disable <= 0:
            failures.append("visible splats with painterly disabled: %d" % visible_after_disable)

        splat_node.call("set_enable_painterly", true)
        splat_node.call("force_update")
        await get_tree().process_frame
        var visible_after_reenable := _read_visible_splats(splat_node)
        if visible_after_reenable <= 0:
            failures.append("visible splats after re-enabling painterly: %d" % visible_after_reenable)

        print("[Painterly Test] visible (enabled/disabled/re-enabled): %d / %d / %d" % [
            visible_after_enable,
            visible_after_disable,
            visible_after_reenable
        ])

        # --- Render path, only where a RenderingDevice exists. ---
        var render_result := await _check_render_path(splat_node)
        var verdict := str(render_result.get("verdict", "not_verified"))
        print("[Painterly Test] render path: %s (%s)" % [verdict, str(render_result.get("detail", ""))])
        if verdict == "ran_baseline":
            failures.append(
                "painterly is enabled with a valid material but the frame came from the " +
                "baseline raster (%s)" % str(render_result.get("detail", "")))
        elif verdict == "not_verified":
            # Absence of a RenderingDevice is not a pass. Say so in the output
            # instead of letting the PASSED marker imply the path was checked.
            print("[Painterly Test] NOT VERIFIED: painterly render path was not observed (%s). " % str(render_result.get("detail", "")) +
                "Run tests/runtime/test_painterly_material_render.gd on the GPU lane for the pixel proof.")

    if failures.is_empty():
        print("PAINTERLY_TEST_PASSED")
        get_tree().quit(0)
        return

    for failure in failures:
        push_error("[Painterly Test] %s" % failure)
    print("PAINTERLY_TEST_FAILED")
    get_tree().quit(1)

func _make_material() -> Resource:
    var m = ClassDB.instantiate("PainterlyMaterial")
    if m == null:
        return null
    m.set("palette_quantization_enabled", true)
    m.set("brush_modulation_enabled", true)
    m.set("lighting_stylization_enabled", true)
    m.set("painterly_mix_strength", 0.85)
    return m

# Returns {"verdict": "painterly"|"ran_baseline"|"not_verified", "detail": String}.
func _check_render_path(splat_node: Node) -> Dictionary:
    if not splat_node.has_method("get_renderer"):
        return {"verdict": "not_verified", "detail": "node exposes no get_renderer()"}
    var renderer = null
    for _i in range(RENDER_PATH_WAIT_FRAMES):
        await get_tree().process_frame
        splat_node.call("force_update")
        renderer = splat_node.call("get_renderer")
        if renderer != null:
            break
    if renderer == null:
        return {"verdict": "not_verified", "detail": "no GaussianSplatRenderer (needs a RenderingDevice)"}
    if not renderer.has_method("get_render_stats"):
        return {"verdict": "not_verified", "detail": "renderer exposes no get_render_stats()"}
    for _i in range(RENDER_PATH_WAIT_FRAMES):
        await get_tree().process_frame
        splat_node.call("force_update")
        var stats = renderer.call("get_render_stats")
        if not (stats is Dictionary) or not stats.has("stage_raster_painterly"):
            continue
        if bool(stats.get("stage_raster_painterly", false)):
            return {"verdict": "painterly", "detail": "stage_raster_painterly=true"}
    var final_stats = renderer.call("get_render_stats")
    if not (final_stats is Dictionary) or not final_stats.has("stage_raster_painterly"):
        return {"verdict": "not_verified", "detail": "get_render_stats() never reported stage_raster_painterly"}
    return {
        "verdict": "ran_baseline",
        "detail": "stage_raster_painterly=false, stage_raster_reason='%s'" % str(final_stats.get("stage_raster_reason", "")),
    }

func _read_visible_splats(splat_node: Node) -> int:
    if splat_node.has_method("get_statistics"):
        var stats = splat_node.call("get_statistics")
        if typeof(stats) == TYPE_DICTIONARY:
            return int(stats.get("visible_splats", -1))
    if splat_node.has_method("get_visible_splat_count"):
        return int(splat_node.call("get_visible_splat_count"))
    return -1
