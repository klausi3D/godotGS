# Color Grading Reference

!!! info "Scope"
    For developers and technical artists who need field ranges, method contracts, and GPU mapping details.
    This page covers exact API lookup after you already know the workflow.
    It complements the canonical [Color Grading Quick Start](../features/color-grading-quick-start.md).

## Purpose
Use this reference for the current color grading API on `GaussianSplatNode3D`, including runtime grading and bake/restore workflows.

## Usage
| Task | Action | Source |
| --- | --- | --- |
| Assign grading to a node | Call `set_color_grading()` or set `rendering/color_grading` with a `ColorGradingResource` | `GaussianSplatNode3D::set_color_grading` |
| Apply grading in real time | Keep resource `enabled=true`; renderer packs values into tile render params each frame | `ColorGradingResource::enabled`, `TileRenderer::TileRenderParamsBuilder::build_params` |
| Bake grading into splat data | `set_splat_data()` nodes only: call `bake_color_grading()` to update SH DC coefficients in the node's CPU-side `GaussianData`. A `splat_asset` node returns `ERR_UNAVAILABLE` ([#1105](https://github.com/klausi3D/godotGS/issues/1105)); its live grade already applies. See [Color Grading Quick Start](../features/color-grading-quick-start.md#baking-limitation) | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianData::bake_color_grading` |
| Restore original colors | Call `restore_color_grading()` to restore pre-bake SH DC values | `GaussianSplatNode3D::restore_color_grading` |

## API
| Resource field | Type | Range/default | Source |
| --- | --- | --- | --- |
| `enabled` | `bool` | default `true` | `ColorGradingResource::enabled` |
| `exposure` | `float` | `[-5.0, 5.0]`, default `0.0` | `ColorGradingResource::exposure` |
| `contrast` | `float` | `[0.0, 2.0]`, default `1.0` | `ColorGradingResource::contrast` |
| `saturation` | `float` | `[0.0, 2.0]`, default `1.0` | `ColorGradingResource::saturation` |
| `temperature` | `float` | `[-100.0, 100.0]`, default `0.0` | `ColorGradingResource::temperature` |
| `tint` | `float` | `[-100.0, 100.0]`, default `0.0` | `ColorGradingResource::tint` |
| `hue_shift` | `float` | `[-180.0, 180.0]`, default `0.0` | `ColorGradingResource::hue_shift` |

| Node method | Return | Behavior | Source |
| --- | --- | --- | --- |
| `set_color_grading(grading)` | `void` | Assigns resource and marks render state dirty | `GaussianSplatNode3D::set_color_grading` |
| `bake_color_grading()` | `Error` | Applies CPU grading to SH DC and disables runtime grading to avoid double-apply. `ERR_UNAVAILABLE` on a `splat_asset` node (#1105); `ERR_UNCONFIGURED` without a grading resource or without any splat data | `GaussianSplatNode3D::bake_color_grading_snapshot` |
| `restore_color_grading()` | `Error` | Restores backed-up SH DC and re-enables runtime grading. Returns `OK` (also when nothing was baked), `ERR_UNAVAILABLE` on a `splat_asset` node, or `ERR_INVALID_DATA` when the data was replaced since the bake (nothing restored) | `GaussianSplatNode3D::restore_color_grading` |
| `is_color_grading_baked()` | `bool` | Returns bake state from the node's CPU-side `GaussianData`; `false` when there is none | `GaussianSplatNode3D::is_color_grading_baked` |

| GPU mapping | Layout |
| --- | --- |
| `color_grading_primary` | `x=enabled`, `y=exposure`, `z=contrast`, `w=saturation` (`TileRenderParamsGPU`) |
| `color_grading_secondary` | `x=temperature`, `y=tint`, `z=hue_shift`, `w=reserved` (`TileRenderParamsGPU`) |

Source: `TileRenderParamsGPU` in `modules/gaussian_splatting/renderer/gaussian_gpu_layout.h`, `TileRenderer::TileRenderParamsBuilder::build_params`, `apply_color_grading_binning()` in `modules/gaussian_splatting/shaders/includes/color_grading_binning.glsl`.

## Examples
```gdscript
# assumes: var splat_node: GaussianSplatNode3D
var grading := ColorGradingResource.new()
grading.enabled = true
grading.exposure = 0.25
grading.contrast = 1.1
grading.saturation = 1.05
grading.temperature = 8.0
grading.tint = -4.0
grading.hue_shift = 6.0

splat_node.set_color_grading(grading)

# Baking needs set_splat_data() data; a splat_asset node returns ERR_UNAVAILABLE (#1105).
var err := splat_node.bake_color_grading()
if err != OK:
    push_warning("Bake failed: %s" % error_string(err))

if splat_node.is_color_grading_baked():
    splat_node.restore_color_grading()
```

## Troubleshooting
| Symptom | Cause | Action |
| --- | --- | --- |
| `bake_color_grading()` returns `ERR_UNAVAILABLE` | The node renders a `splat_asset`; baking is unsupported there (#1105) | Keep the grading resource enabled; the live grade already applies (`GaussianSplatNode3D::bake_color_grading_snapshot`) |
| `bake_color_grading()` returns `ERR_UNCONFIGURED` | Node has no grading resource, or no `set_splat_data()` data | Assign a `ColorGradingResource` and supply data through `set_splat_data()` (`GaussianSplatNode3D::bake_color_grading_snapshot`) |
| Colors look doubly graded | Runtime grading was re-enabled while baked colors are still applied | Call `restore_color_grading()` before re-baking and keep runtime grading disabled during baked mode (`GaussianSplatNode3D::restore_color_grading`) |
| Runtime sliders do not change output | Resource is unset or disabled | Confirm node property `rendering/color_grading` and `enabled` state (`GaussianSplatNode3D::set_color_grading`, `ColorGradingResource::set_enabled`) |
