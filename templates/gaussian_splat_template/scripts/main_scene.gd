extends Node3D
class_name GaussianTemplateRoot

@export var default_splat_asset: GaussianSplatAsset
## Matches `painterly/enabled = false` in `scenes/main.tscn`. It used to default
## to `true`, which was invisible only because this script never ran: the moment
## it does, `_configure_gaussian_node()` overrides the scene's own value. Two
## sources of truth for the same setting is a defect; the scene wins.
@export var enable_demo_painterly: bool = false

## How long `_focus_camera()` waits for the asset's bounds to become usable
## before giving up and saying so. A wall-clock deadline, not a frame count:
## import and upload cost is machine-dependent.
const FOCUS_TIMEOUT_MS := 5000

@onready var gaussian_node: GaussianSplatNode3D = $GaussianSplatNode3D
@onready var performance_overlay: GaussianPerformanceOverlay = $CanvasLayer/PerformanceOverlay
@onready var camera_rig: OrbitCameraRig = $CameraRig

## Configures the template scene by wiring the node, overlay, and camera focus.
func _ready() -> void:
    _configure_gaussian_node()
    _wire_overlay()
    _focus_camera()

## Applies template defaults to the GaussianSplatNode3D instance.
func _configure_gaussian_node() -> void:
    if gaussian_node == null:
        push_error("GaussianSplatNode3D is missing from the template scene")
        return

    if default_splat_asset != null and gaussian_node.get_splat_asset() == null:
        gaussian_node.set_splat_asset(default_splat_asset)

    gaussian_node.set_quality_preset(GaussianSplatNode3D.QUALITY_BALANCED)
    gaussian_node.set_lod_bias(1.0)
    gaussian_node.set_max_render_distance(150.0)
    gaussian_node.set_max_splat_count(750000)

    gaussian_node.set_enable_painterly(enable_demo_painterly)
    gaussian_node.set_edge_threshold(0.25)
    gaussian_node.set_stroke_opacity(0.85)
    gaussian_node.set_stroke_width(1.1)
    gaussian_node.set_color_variation(0.12)
    gaussian_node.set_temporal_blend(0.35)
    gaussian_node.set_painterly_seed(1337)

    gaussian_node.set_update_mode(GaussianSplatNode3D.UPDATE_MODE_WHEN_VISIBLE)
    gaussian_node.set_cast_shadow(true)
    gaussian_node.set_use_frustum_culling(true)
    gaussian_node.set_use_occlusion_culling(true)
    gaussian_node.set_opacity(1.0)

    gaussian_node.set_preview_enabled(true)
    gaussian_node.set_show_bounds(false)
    gaussian_node.set_show_statistics(false)
    gaussian_node.set_show_tile_grid(false)
    gaussian_node.set_show_density_heatmap(false)
    gaussian_node.set_show_performance_hud(false)
    gaussian_node.set_show_performance_overlay(false)
    gaussian_node.set_debug_draw_mode(GaussianSplatNode3D.DEBUG_DRAW_POINTS)
    gaussian_node.set_runtime_preview_enabled(false)
    gaussian_node.set_show_residency_hud(false)

## Binds the overlay to the gaussian node and camera rig.
func _wire_overlay() -> void:
    if performance_overlay:
        performance_overlay.set_gaussian_node(gaussian_node)
        performance_overlay.set_camera_node(camera_rig)

## Centers the orbit camera on the current Gaussian bounds.
##
## Two reasons this is not the one-liner it looks like:
##
## 1. `GaussianSplatNode3D::get_aabb()` exists in C++ but is NOT bound to
##    ClassDB, so calling it from GDScript raises "Invalid call. Nonexistent
##    function 'get_aabb'". The bounds are reachable from GDScript only as
##    `get_statistics()["bounds"]`, which is bound.
## 2. The bounds are not final at `_ready()`: they are computed when the asset
##    payload is uploaded, one or more frames later. Focusing once in `_ready()`
##    frames nothing and leaves the shipped camera transform in place with no
##    diagnostic -- which is why the template opened on an unframed blob.
##
## So: read the bound accessor, and retry to a wall-clock deadline (never a
## fixed frame count -- import and upload cost is machine-dependent). If the
## bounds never arrive, say so rather than failing silently.
func _focus_camera() -> void:
    if camera_rig == null or gaussian_node == null:
        return
    var deadline: int = Time.get_ticks_msec() + FOCUS_TIMEOUT_MS
    var last_bounds: AABB = AABB()
    var last_count: int = 0
    while Time.get_ticks_msec() < deadline:
        var stats: Dictionary = gaussian_node.get_statistics()
        var bounds: AABB = stats.get("bounds", AABB())
        last_count = int(stats.get("total_splats", 0))
        if bounds.size.length() > 0.0 and last_count > 0:
            camera_rig.focus(bounds)
            return
        last_bounds = bounds
        if not is_inside_tree():
            return
        await get_tree().process_frame
    push_warning(
        "GaussianTemplateRoot: splat bounds stayed unusable for %d ms (last %s, %d splats); camera left at its scene transform."
        % [FOCUS_TIMEOUT_MS, last_bounds, last_count])
