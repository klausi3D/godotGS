# Color Grading Quick Start

!!! tip "Canonical task page"
    Use this page as the source of truth for applying, tuning, baking, and restoring color grading in a scene workflow.
    For exact fields and API lookup, use [Color Grading Reference](../reference/color-grading.md) as the technical complement.

## Purpose

Use `ColorGradingResource` on `GaussianSplatNode3D` for real-time grading and optional baking into SH DC color data.

## Usage

| Step | Action | Implementation reference |
| --- | --- | --- |
| 1 | Assign a `ColorGradingResource` to `rendering/color_grading` on the node. | `GaussianSplatNode3D::set_color_grading` |
| 2 | Tune `exposure`, `contrast`, `saturation`, `temperature`, `tint`, and `hue_shift` on the resource. | `ColorGradingResource::_bind_methods` |
| 3 | Keep `enabled = true` for real-time grading in tile binning. | `ColorGradingResource::_bind_methods`, `apply_color_grading_binning()` in `modules/gaussian_splatting/shaders/includes/color_grading_binning.glsl` |
| 4 | Optional, `set_splat_data()` nodes only: run `bake_color_grading()` or click `Bake Color Grading` to write grading into base SH DC values. A `splat_asset` node returns `ERR_UNAVAILABLE`; see [Baking limitation](#baking-limitation). | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianSplatNodeInspectorPlugin::parse_begin` |
| 5 | `set_splat_data()` nodes only: run `restore_color_grading()` or click `Restore Original` to restore pre-bake colors. | `GaussianSplatNode3D::restore_color_grading` |

### Baking limitation

Baking works only on a node whose splats come from `set_splat_data()`. A node that renders a `splat_asset` (the normal import path) cannot bake: `bake_color_grading()`, `bake_color_grading_snapshot()` and `restore_color_grading()` return `ERR_UNAVAILABLE` and change nothing, and the inspector does not show the Bake section for it ([#1105](https://github.com/klausi3D/godotGS/issues/1105)). You lose nothing by it: real-time grading (steps 1-3) already applies to that node, per instance, so keep `enabled = true`. The asset may be shared by other nodes and is never rewritten. Separately, a bake does not yet reproduce the live look exactly ([#1124](https://github.com/klausi3D/godotGS/issues/1124)).

## API

| Item | Type | Behavior | Implementation reference |
| --- | --- | --- | --- |
| `rendering/color_grading` | Node property | Stores the grading resource used by the renderer. | `GaussianSplatNode3D::_bind_methods` |
| `set_color_grading(grading)` | Method | Updates node state and marks render state dirty for re-upload. | `GaussianSplatNode3D::set_color_grading` |
| `bake_color_grading()` | Method | Applies grading on CPU to SH DC coefficients and disables grading to avoid double-application. Returns `ERR_UNAVAILABLE` on a `splat_asset` node (see [Baking limitation](#baking-limitation)) and `ERR_UNCONFIGURED` without a grading resource or without any splat data. | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianData::bake_color_grading` |
| `restore_color_grading()` | Method | Restores original SH DC coefficients and re-enables grading. Returns `Error`: `OK`, `ERR_UNAVAILABLE` on a `splat_asset` node, or `ERR_INVALID_DATA` if the data was replaced since the bake (nothing restored, grading stays disabled). | `GaussianSplatNode3D::restore_color_grading` |
| `is_color_grading_baked()` | Method | Reports whether bake state is active in `GaussianData`. | `GaussianSplatNode3D::is_color_grading_baked`, `GaussianData::is_color_grading_baked` |

| `ColorGradingResource` field | Default | Clamp range | Implementation reference |
| --- | --- | --- | --- |
| `enabled` | `true` | bool | `ColorGradingResource::enabled` |
| `exposure` | `0.0` | `-5.0..5.0` | `ColorGradingResource::exposure` |
| `contrast` | `1.0` | `0.0..2.0` | `ColorGradingResource::contrast` |
| `saturation` | `1.0` | `0.0..2.0` | `ColorGradingResource::saturation` |
| `temperature` | `0.0` | `-100.0..100.0` | `ColorGradingResource::temperature` |
| `tint` | `0.0` | `-100.0..100.0` | `ColorGradingResource::tint` |
| `hue_shift` | `0.0` | `-180.0..180.0` | `ColorGradingResource::hue_shift` |

| Pipeline step | Behavior | Implementation reference |
| --- | --- | --- |
| Resource registration | `ColorGradingResource` is already registered at module init. | `initialize_gaussian_splatting_module` in `modules/gaussian_splatting/register_types.cpp` |
| Renderer upload | Renderer writes grading values into render params each frame. | `TileRenderer::TileRenderParamsBuilder::build_params` |
| GPU layout | Render params include two `vec4` grading fields, `color_grading_primary` and `color_grading_secondary`. | `TileRenderParamsGPU` in `modules/gaussian_splatting/renderer/gaussian_gpu_layout.h`, `modules/gaussian_splatting/shaders/includes/gs_render_params.glsl` |
| Shader application | Tile binning applies grading after SH evaluation, including cached SH path. | `apply_color_grading_binning()` in `modules/gaussian_splatting/shaders/includes/color_grading_binning.glsl`, called from `modules/gaussian_splatting/shaders/tile_binning.glsl` |

## Examples

Attach this script to a `Node3D` that has a `GaussianSplatNode3D` child named `GaussianSplatNode3D`.

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
	# ERR_UNAVAILABLE on a node that renders a splat_asset; see "Baking limitation".
	var err := splat.bake_color_grading()
	if err != OK:
		push_warning("bake_color_grading failed: %s" % error_string(err))

func restore_grading() -> void:
	splat.restore_color_grading()
```

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| `bake_color_grading()` returns `ERR_UNAVAILABLE` | The node renders a `splat_asset`; baking is not supported there (see [Baking limitation](#baking-limitation)). | Nothing to fix: keep the grading resource enabled, the live grade already applies. Bake only `set_splat_data()` nodes. | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| `bake_color_grading()` returns `ERR_UNCONFIGURED` | No grading resource, or the node has no splat data at all. | Assign `rendering/color_grading`, and supply data through `set_splat_data()`. | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| Inspector changes have no visible effect | Grading is disabled or resource is not assigned. | Set `enabled = true` and re-check the node property assignment. | `ColorGradingResource::_bind_methods`, `TileRenderer::TileRenderParamsBuilder::build_params` |
| Colors look graded twice after manual workflow changes | Grading remained enabled after custom bake flow. | Disable grading after bake or call node bake API which already disables it. | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| Restore does not bring back expected base colors | No previous bake state was recorded. | Bake once before expecting restore behavior. | `GaussianData::restore_original_colors` |
| `restore_color_grading()` returns `ERR_INVALID_DATA` | `set_splat_data()` replaced the node's data after the bake, so the saved colors no longer match. Nothing was restored. | Re-supply the original data, or leave grading disabled. | `GaussianData::restore_original_colors` |
