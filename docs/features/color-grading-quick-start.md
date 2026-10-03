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
| 4 | Optional: run `bake_color_grading()` or click `Bake Color Grading` to write grading into base SH DC values. Check the returned `Error`; see [Baking limitation](#baking-limitation). | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianSplatNodeInspectorPlugin::parse_begin` |
| 5 | Run `restore_color_grading()` or click `Restore Original` to restore pre-bake colors. | `GaussianSplatNode3D::restore_color_grading` |

### Baking limitation

`bake_color_grading()` and `restore_color_grading()` operate on the node's CPU-side data (`renderer_data`). At this revision that data is created only when the node is populated with `set_splat_data()` (`GaussianSplatNode3D::_ensure_renderer_data_for_splats`); a node that renders a `splat_asset` never creates it. Code reading and a headless run show `bake_color_grading()` returning `ERR_UNCONFIGURED` ("Cannot bake color grading: no gaussian data loaded") on an asset-backed node. This is not yet confirmed on a GPU run and is tracked as a suspected defect. Real-time grading (steps 1-3) does not depend on it.

## API

| Item | Type | Behavior | Implementation reference |
| --- | --- | --- | --- |
| `rendering/color_grading` | Node property | Stores the grading resource used by the renderer. | `GaussianSplatNode3D::_bind_methods` |
| `set_color_grading(grading)` | Method | Updates node state and marks render state dirty for re-upload. | `GaussianSplatNode3D::set_color_grading` |
| `bake_color_grading()` | Method | Applies grading on CPU to SH DC coefficients and disables grading to avoid double-application. Returns `ERR_UNCONFIGURED` without a grading resource or without CPU-side node data (see [Baking limitation](#baking-limitation)). | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianData::bake_color_grading` |
| `restore_color_grading()` | Method | Restores original SH DC coefficients and re-enables grading. | `GaussianSplatNode3D::restore_color_grading` |
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
	# ERR_UNCONFIGURED on asset-backed nodes at this revision; see "Baking limitation".
	var err := splat.bake_color_grading()
	if err != OK:
		push_warning("bake_color_grading failed: %s" % error_string(err))

func restore_grading() -> void:
	splat.restore_color_grading()
```

## Troubleshooting

| Symptom | Cause | Fix | Implementation reference |
| --- | --- | --- | --- |
| `bake_color_grading()` returns `ERR_UNCONFIGURED` | No grading resource, or the node has no CPU-side data (asset-backed nodes, see [Baking limitation](#baking-limitation)). | Assign `rendering/color_grading`. For asset-backed nodes, keep real-time grading instead of baking. | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| Inspector changes have no visible effect | Grading is disabled or resource is not assigned. | Set `enabled = true` and re-check the node property assignment. | `ColorGradingResource::_bind_methods`, `TileRenderer::TileRenderParamsBuilder::build_params` |
| Colors look graded twice after manual workflow changes | Grading remained enabled after custom bake flow. | Disable grading after bake or call node bake API which already disables it. | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| Restore does not bring back expected base colors | No previous bake state was recorded. | Bake once before expecting restore behavior. | `GaussianData::restore_original_colors` |
