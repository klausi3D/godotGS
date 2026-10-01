# Color Grading Quick Start

!!! tip "Canonical task page"
    Use this page as the source of truth for applying, tuning, baking, and restoring color grading in a scene workflow.
    For exact fields and API lookup, use [Color Grading Reference](../reference/color-grading.md) as the technical complement.

## Purpose

Use `ColorGradingResource` on `GaussianSplatNode3D` for real-time grading and optional baking into SH DC color data.

!!! warning "Baking limitation"
    Baking works only on a node whose splats come from `set_splat_data()`. A node that renders a
    `splat_asset` (the normal import path) cannot bake: `bake_color_grading()`,
    `bake_color_grading_snapshot()` and `restore_color_grading()` return `ERR_UNAVAILABLE` and change
    nothing, and the inspector does not show the Bake section for it
    ([#1105](https://github.com/klausi3D/godotGS/issues/1105)). You lose nothing by it: the live
    grade from steps 1-3 already applies to that node, per instance, so keep `enabled = true`.
    The asset may be shared by other nodes and is never rewritten.
    Separately, a bake does not yet reproduce the live look exactly
    ([#1124](https://github.com/klausi3D/godotGS/issues/1124)).

## Usage

| Step | Action | Implementation reference |
| --- | --- | --- |
| 1 | Assign a `ColorGradingResource` to `rendering/color_grading` on the node. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:192` |
| 2 | Tune `exposure`, `contrast`, `saturation`, `temperature`, `tint`, and `hue_shift` on the resource. | `modules/gaussian_splatting/resources/color_grading_resource.cpp:20` |
| 3 | Keep `enabled = true` for real-time grading in tile binning. | `modules/gaussian_splatting/resources/color_grading_resource.cpp:15`, `modules/gaussian_splatting/shaders/includes/color_grading_binning.glsl:32` |
| 4 | `set_splat_data()` nodes only: run `bake_color_grading()` or click `Bake Color Grading` to write grading into base SH DC values. A `splat_asset` node returns `ERR_UNAVAILABLE` (see Baking limitation). | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3183`, `modules/gaussian_splatting/editor/gaussian_inspector_plugins.cpp:465` |
| 5 | `set_splat_data()` nodes only: run `restore_color_grading()` or click `Restore Original` to restore pre-bake colors. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3236`, `modules/gaussian_splatting/editor/gaussian_inspector_plugins.cpp:493` |

## API

| Item | Type | Behavior | Implementation reference |
| --- | --- | --- | --- |
| `rendering/color_grading` | Node property | Stores the grading resource used by the renderer. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:192` |
| `set_color_grading(grading)` | Method | Updates node state and marks render state dirty for re-upload. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:1788` |
| `bake_color_grading()` | Method | Applies grading on CPU to SH DC coefficients and disables grading to avoid double-application. Returns `ERR_UNAVAILABLE` on a `splat_asset` node (`modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3207`). | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3183`, `modules/gaussian_splatting/core/gaussian_data.cpp:1816` |
| `restore_color_grading()` | Method | Restores original SH DC coefficients and re-enables grading. Returns `Error`: `OK`, or `ERR_UNAVAILABLE` on a `splat_asset` node (`modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3236`). | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3236`, `modules/gaussian_splatting/core/gaussian_data.cpp:1855` |
| `is_color_grading_baked()` | Method | Reports whether bake state is active in `GaussianData`. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:1837`, `modules/gaussian_splatting/core/gaussian_data.h:763` |

| `ColorGradingResource` field | Default | Clamp range | Implementation reference |
| --- | --- | --- | --- |
| `enabled` | `true` | bool | `modules/gaussian_splatting/resources/color_grading_resource.h:10` |
| `exposure` | `0.0` | `-5.0..5.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:11` |
| `contrast` | `1.0` | `0.0..2.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:12` |
| `saturation` | `1.0` | `0.0..2.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:13` |
| `temperature` | `0.0` | `-100.0..100.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:14` |
| `tint` | `0.0` | `-100.0..100.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:15` |
| `hue_shift` | `0.0` | `-180.0..180.0` | `modules/gaussian_splatting/resources/color_grading_resource.h:16` |

| Pipeline step | Behavior | Implementation reference |
| --- | --- | --- |
| Resource registration | `ColorGradingResource` is already registered at module init. | `modules/gaussian_splatting/register_types.cpp:123` |
| Build inclusion | `resources/*.cpp` is already compiled by module `SCsub`. | `modules/gaussian_splatting/SCsub:33`, `modules/gaussian_splatting/SCsub:47` |
| Renderer upload | Renderer writes grading values into render params each frame. | `modules/gaussian_splatting/renderer/tile_render_stages.cpp:321` |
| GPU layout | Render params include two `vec4` grading fields. | `modules/gaussian_splatting/renderer/gaussian_gpu_layout.h:353`, `modules/gaussian_splatting/shaders/includes/gs_render_params.glsl:58` |
| Shader application | Tile binning applies grading after SH evaluation, including cached SH path. | `modules/gaussian_splatting/shaders/tile_binning.glsl:1358`, `modules/gaussian_splatting/shaders/tile_binning.glsl:1365` |

## Examples

```gdscript
extends Node3D

@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D

func _ready() -> void:
	var grading := ColorGradingResource.new()
	grading.enabled = true
	grading.exposure = 0.35
	grading.contrast = 1.1
	grading.saturation = 1.15
	grading.temperature = 8.0
	grading.tint = -4.0
	grading.hue_shift = 6.0
	splat.set_color_grading(grading)

func bake_grading() -> void:
	var err := splat.bake_color_grading()
	if err != OK:
		push_warning("bake_color_grading failed: %d" % err)

func restore_grading() -> void:
	splat.restore_color_grading()
```

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| `bake_color_grading()` returns `ERR_UNAVAILABLE` | The node renders a `splat_asset`; baking is not supported there (#1105). | Nothing to fix: keep the grading resource enabled, the live grade already applies. Bake only `set_splat_data()` nodes. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3204` |
| `bake_color_grading()` returns `ERR_UNCONFIGURED` | No grading resource, or the node has no splat data at all. | Assign `rendering/color_grading`, and supply data through `set_splat_data()`. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3212`, `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:3217` |
| Inspector changes have no visible effect | Grading is disabled or resource is not assigned. | Set `enabled = true` and re-check the node property assignment. | `modules/gaussian_splatting/resources/color_grading_resource.cpp:15`, `modules/gaussian_splatting/renderer/tile_render_stages.cpp:322` |
| Colors look graded twice after manual workflow changes | Grading remained enabled after custom bake flow. | Disable grading after bake or call node bake API which already disables it. | `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp:1811` |
| Restore does not bring back expected base colors | No previous bake state was recorded. | Bake once before expecting restore behavior. | `modules/gaussian_splatting/core/gaussian_data.cpp:1822`, `modules/gaussian_splatting/core/gaussian_data.cpp:1857` |
