# Gaussian Splat Artist Pipeline

_Last verified: 2026-09-30._

!!! info "Scope"
    For artists and technical artists using inspector tools, hot reload, brush edits, and bake actions inside the editor.
    This page covers in-editor pipeline details after you already know the overall job.
    It complements the canonical [Guides overview](../user/index.md).

## Purpose

Use this workflow to import `.ply`/`.spz` assets, iterate with inspector brush tools, and keep node data synchronized with source-file changes.

## Usage

| Task | Action | Implementation reference |
| --- | --- | --- |
| Import a source file | Copy the `.ply` or `.spz` file into the project (`res://`). Godot imports it automatically with the `gaussian_splat_ply` / `gaussian_splat_spz` importer into a `GaussianSplatAsset`. Import options are in the Import dock. There is no Gaussian bottom panel or `Import Gaussian` button. | `ResourceImporterPLY::import`, `ResourceImporterSPZ::import` |
| Place the asset in a scene | Drag the imported file from the FileSystem dock into the 3D viewport; the editor creates a `GaussianSplatNode3D` with `splat_asset` set. Or add a `GaussianSplatNode3D` yourself and assign the asset to its `splat_asset` property. | `Node3DEditorViewport::_create_gaussian_instance`, `GaussianSplatNode3D::set_splat_asset` |
| Reimport an existing asset | In the `GaussianSplatAsset` inspector, click `Reimport...` to open the Gaussian import dialog with the saved settings. | `GaussianAssetInspectorPlugin::_on_reimport_pressed`, `GaussianEditorPlugin::request_asset_reimport` |
| Enable runtime debug view (debug builds) | In `Gaussian Splat Overview`, toggle `Runtime Preview` and `Residency HUD`. | `GaussianSplatNodeInspectorPlugin::parse_begin` |
| Apply brush edits | Only shown when painterly is enabled on the node and its renderer holds CPU-side Gaussian data. Set center/radius/strength/hardness/color in `Painterly Brush Tools`, then click `Apply Brush`, `Commit`, or `Revert`. The brush values are session-only. | `GaussianSplatNodeInspectorPlugin::parse_begin`, `GaussianEditorPlugin::_apply_brush_stroke` |
| Bake or restore color grading | Use `Bake Color Grading` or `Restore Original` in the node inspector. Shown only for nodes fed by `set_splat_data()`; see the limitation below. | `GaussianSplatNodeInspectorPlugin::parse_begin`, `GaussianSplatNode3D::bake_color_grading` |

!!! warning "Color grading bake on asset-backed nodes"
    Baking is supported only for nodes populated with `set_splat_data()`. On a node that renders a `splat_asset` (the normal import path), `bake_color_grading()`, `bake_color_grading_snapshot()` and `restore_color_grading()` return `ERR_UNAVAILABLE` and change nothing, and the inspector does not show the Bake section ([#1105](https://github.com/klausi3D/godotGS/issues/1105)). The live grade already applies to that node, so keep the `ColorGradingResource` enabled. See the [Color Grading Quick Start](color-grading-quick-start.md#baking-limitation).

| Hot reload behavior | Current behavior | Implementation reference |
| --- | --- | --- |
| Watch registration | Opening the Gaussian import dialog for a source file (for example through `Reimport...`) emits `watch_path_requested`, which registers a watch before the import is confirmed. | `GaussianImportDialog::configure_for_file`, `GaussianEditorPlugin::_on_import_dialog_watch` |
| Poll interval | Watches are polled on a timer set by `rendering/gaussian_splatting/editor/hot_reload_poll_interval_sec` (default `1.0` s, clamped to `0.1`..`10`). | `_hot_reload_poll_interval_seconds()` in `modules/gaussian_splatting/editor/gaussian_editor_plugin.cpp`, `GaussianEditorPlugin::_schedule_hot_reload_poll` |
| `.ply` / `.spz` changes | The editor reimports with the stored options and updates the watched nodes. | `GaussianEditorPlugin::_process_hot_reload_for_watch` |
| Other watched paths | The resource is reloaded from disk and applied to the watched nodes. | `GaussianEditorPlugin::_process_hot_reload_for_watch`, `GaussianEditorPlugin::_apply_hot_reload_asset_to_nodes` |

## API

| API | Behavior | Implementation reference |
| --- | --- | --- |
| `GaussianData.apply_brush_stroke(center, radius, color, opacity, hardness)` | Stages runtime color and opacity edits with radial falloff and records a capped brush history (`2048`). | `GaussianData::apply_brush_stroke` |
| `GaussianData.commit_runtime_changes()` | Writes staged runtime data into base Gaussian data and clears runtime buffers. | `GaussianData::commit_runtime_changes` |
| `GaussianData.revert_runtime_changes()` | Discards staged runtime buffers without clearing recorded brush history. | `GaussianData::revert_runtime_changes` |
| `GaussianData.get_brush_strokes()` | Returns recorded strokes as dictionaries for tooling/serialization hooks. | `GaussianData::get_brush_strokes` |
| `GaussianSplatNode3D.set_runtime_preview_enabled(enabled)` | Switches renderer preview mode to runtime modifications and restores prior mode when disabled. | `GaussianSplatNodeDebugHelper::set_runtime_preview_enabled` |
| `GaussianSplatNode3D.set_show_residency_hud(show)` | Toggles renderer residency HUD and persists the preference into project settings. | `GaussianSplatNode3D::set_show_residency_hud`, `GaussianSplatNodeDebugHelper::set_show_residency_hud` |
| `GaussianSplatNode3D.bake_color_grading()` / `restore_color_grading()` | Bakes grading into SH DC colors, disables live grading after bake, and restores original colors on demand. Both need the node's CPU-side data (see the warning above). | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianSplatNode3D::restore_color_grading` |

## Examples

These mirror what the inspector's brush buttons do (`GaussianEditorPlugin::_apply_brush_stroke`, `_commit_runtime_modifications`, `_revert_runtime_modifications`), without editor undo/redo. Both return early when the node's renderer holds no CPU-side `GaussianData`.

```gdscript
@tool
extends Node

func apply_and_commit_brush(node: GaussianSplatNode3D, world_center: Vector3) -> void:
	var renderer := node.get_renderer()
	if renderer == null:
		return
	var data := renderer.get_gaussian_data()
	if data == null:
		return
	var local_center := node.to_local(world_center)
	data.apply_brush_stroke(local_center, 1.5, Color(1.0, 0.8, 0.6, 1.0), 0.5, 1.0)
	data.commit_runtime_changes()
	node.force_update()
```

```gdscript
@tool
extends Node

func discard_staged_brush_edits(node: GaussianSplatNode3D) -> void:
	var renderer := node.get_renderer()
	if renderer == null:
		return
	var data := renderer.get_gaussian_data()
	if data == null:
		return
	data.revert_runtime_changes()
	node.force_update()
```

## Troubleshooting

| Symptom | Resolution | Implementation reference |
| --- | --- | --- |
| `Runtime Preview` or `Residency HUD` does not appear | Use a debug build because the custom debug inspector controls are compiled only under `DEBUG_ENABLED` and debug properties are hidden in release. | `GaussianSplatNodeInspectorPlugin::parse_begin`, `GaussianSplatNode3D::_get` |
| `Painterly Brush Tools` does not appear | Enable painterly on the node (`painterly/enabled`); the section also needs the node's renderer to hold CPU-side Gaussian data with at least one splat. | `GaussianSplatNodeInspectorPlugin::parse_begin` |
| Brush edits disappear after reimport | Commit edits before reimport because loading new file data resets runtime edits and clears recorded brush strokes. | `GaussianData::_on_gaussian_storage_changed_locked`, `GaussianData::set_gaussian_payload` |
| `Bake Color Grading` fails, or is missing | Assign a `ColorGradingResource` and supply data through `set_splat_data()`. On a `splat_asset` node the section is hidden and the API returns `ERR_UNAVAILABLE` by design (#1105); the live grade applies. | `GaussianSplatNode3D::bake_color_grading_snapshot`, `GaussianSplatNode3D::can_bake_color_grading` |
