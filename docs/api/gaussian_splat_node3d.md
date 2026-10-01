# GaussianSplatNode3D API Reference

## Purpose
Use `GaussianSplatNode3D` to render Gaussian splat assets or procedural splat arrays in a `Node3D` scene (`modules/gaussian_splatting/nodes/gaussian_splat_node_3d.h`).

The tables below are checked against the `GaussianSplatNode3D::_bind_methods()` bindings and `modules/gaussian_splatting/doc_classes/GaussianSplatNode3D.xml`; the generated [GDScript reference](gdscript_reference.md) and the class reference in the editor are the authoritative member lists. Grouped property names such as `rendering/opacity` are single names that contain a `/`: set them with the listed setter or with `set("rendering/opacity", value)`, not with dot access.

A `GaussianSplatNode3D` is always resident (it never streams; see [Streaming](../features/streaming.md#enabling-streaming)). Nodes in the same `World3D` share one renderer; when a renderer is shared with other content, per-node painterly settings are not applied (`_is_renderer_shared_with_other_content()` in `modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp`).

## Usage
<table>
  <thead>
    <tr>
      <th>Task</th>
      <th>Primary API</th>
      <th>Implementation</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>Assign preprocessed asset.</td>
      <td><code>set_splat_asset(asset)</code></td>
      <td><code>GaussianSplatNode3D::set_splat_asset</code></td>
    </tr>
    <tr>
      <td>Reload the assigned asset.</td>
      <td><code>reload_asset()</code></td>
      <td><code>GaussianSplatNode3D::reload_asset</code></td>
    </tr>
    <tr>
      <td>Push procedural data.</td>
      <td><code>set_splat_data(...)</code></td>
      <td><code>GaussianSplatNode3D::set_splat_data</code></td>
    </tr>
    <tr>
      <td>Run manual updates.</td>
      <td><code>set_update_mode(UPDATE_MODE_MANUAL)</code>, <code>update_splats()</code></td>
      <td><code>GaussianSplatNode3D::set_update_mode</code></td>
    </tr>
    <tr>
      <td>Inspect live metrics.</td>
      <td><code>get_visible_splat_count()</code>, <code>get_statistics()</code></td>
      <td><code>GaussianSplatNode3D::get_visible_splat_count</code></td>
    </tr>
  </tbody>
</table>

## API
### Enums
<table>
  <thead>
    <tr>
      <th>Enum</th>
      <th>Values</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>QualityPreset</code></td>
      <td><code>QUALITY_PERFORMANCE</code>, <code>QUALITY_BALANCED</code>, <code>QUALITY_QUALITY</code>, <code>QUALITY_CUSTOM</code></td>
      <td><code>GaussianSplatNode3D::QualityPreset</code></td>
    </tr>
    <tr>
      <td><code>ViewportUpdateMode</code></td>
      <td><code>UPDATE_MODE_ALWAYS</code>, <code>UPDATE_MODE_WHEN_VISIBLE</code>, <code>UPDATE_MODE_WHEN_PARENT_VISIBLE</code>, <code>UPDATE_MODE_MANUAL</code></td>
      <td><code>GaussianSplatNode3D::ViewportUpdateMode</code></td>
    </tr>
    <tr>
      <td><code>DebugDrawMode</code></td>
      <td><code>DEBUG_DRAW_OFF</code>, <code>DEBUG_DRAW_WIREFRAME</code>, <code>DEBUG_DRAW_POINTS</code>, <code>DEBUG_DRAW_HEATMAP</code></td>
      <td><code>GaussianSplatNode3D::DebugDrawMode</code></td>
    </tr>
  </tbody>
</table>

### Properties
<table>
  <thead>
    <tr>
      <th>Inspector path</th>
      <th>Type</th>
      <th>Accessors</th>
      <th>Notes</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>splat_asset</code></td>
      <td><code>GaussianSplatAsset</code></td>
      <td><code>set_splat_asset</code>, <code>get_splat_asset</code></td>
      <td>Assign the GaussianSplatAsset resource that owns the node's splat data.</td>
      <td><code>GaussianSplatNode3D::set_splat_asset</code></td>
    </tr>
    <tr>
      <td><code>quality/preset</code></td>
      <td><code>int (QualityPreset)</code></td>
      <td><code>set_quality_preset</code>, <code>get_quality_preset</code></td>
      <td>Preset values are applied through quality helper config.</td>
      <td><code>GaussianSplatNodeQualityHelper::apply_quality_lod_config</code></td>
    </tr>
    <tr>
      <td><code>quality/lod_bias</code></td>
      <td><code>float</code></td>
      <td><code>set_lod_bias</code>, <code>get_lod_bias</code></td>
      <td>Clamped to <code>0.1..4.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_lod_bias</code></td>
    </tr>
    <tr>
      <td><code>quality/max_render_distance</code></td>
      <td><code>float</code></td>
      <td><code>set_max_render_distance</code>, <code>get_max_render_distance</code></td>
      <td>Clamped to <code>&gt;= 0.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_max_render_distance</code></td>
    </tr>
    <tr>
      <td><code>quality/max_splat_count</code></td>
      <td><code>int</code></td>
      <td><code>set_max_splat_count</code>, <code>get_max_splat_count</code></td>
      <td>Clamped to <code>&gt;= 1000</code>.</td>
      <td><code>GaussianSplatNode3D::set_max_splat_count</code></td>
    </tr>
    <tr>
      <td><code>painterly/enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_enable_painterly</code>, <code>is_painterly_enabled</code></td>
      <td>Enables painterly tuning and streaming flags.</td>
      <td><code>GaussianSplatNode3D::set_enable_painterly</code></td>
    </tr>
    <tr>
      <td><code>painterly/edge_threshold</code></td>
      <td><code>float</code></td>
      <td><code>set_edge_threshold</code>, <code>get_edge_threshold</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_edge_threshold</code></td>
    </tr>
    <tr>
      <td><code>painterly/stroke_opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_stroke_opacity</code>, <code>get_stroke_opacity</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_stroke_opacity</code></td>
    </tr>
    <tr>
      <td><code>painterly/stroke_width</code></td>
      <td><code>float</code></td>
      <td><code>set_stroke_width</code>, <code>get_stroke_width</code></td>
      <td>Clamped to <code>0.1..5.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_stroke_width</code></td>
    </tr>
    <tr>
      <td><code>painterly/temporal_blend</code></td>
      <td><code>float</code></td>
      <td><code>set_temporal_blend</code>, <code>get_temporal_blend</code></td>
      <td>Clamped to <code>0.01..1.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_temporal_blend</code></td>
    </tr>
    <tr>
      <td><code>painterly/seed</code></td>
      <td><code>int</code></td>
      <td><code>set_painterly_seed</code>, <code>get_painterly_seed</code></td>
      <td>Clamped to <code>0..65535</code>.</td>
      <td><code>GaussianSplatNode3D::set_painterly_seed</code></td>
    </tr>
    <tr>
      <td><code>painterly/material</code></td>
      <td><code>PainterlyMaterial</code></td>
      <td><code>set_painterly_material</code>, <code>get_painterly_material</code></td>
      <td>Painterly style resource. Not applied while the node's renderer is shared with other content.</td>
      <td><code>GaussianSplatNode3D::set_painterly_material</code></td>
    </tr>
    <tr>
      <td><code>rendering/update_mode</code></td>
      <td><code>int (ViewportUpdateMode)</code></td>
      <td><code>set_update_mode</code>, <code>get_update_mode</code></td>
      <td>Manual mode disables automatic processing.</td>
      <td><code>GaussianSplatNode3D::set_update_mode</code></td>
    </tr>
    <tr>
      <td><code>rendering/cast_shadow</code></td>
      <td><code>bool</code></td>
      <td><code>set_cast_shadow</code>, <code>get_cast_shadow</code></td>
      <td>Applies cast shadow state to render instance. Default <code>false</code>. Known issue: with a shadow-casting light, turning this on can make the splat cloud shadow itself and render nearly black, depending on camera placement (<a href="https://github.com/klausi3D/godotGS/issues/1089">#1089</a>).</td>
      <td><code>GaussianSplatNode3D::set_cast_shadow</code></td>
    </tr>
    <tr>
      <td><code>rendering/frustum_culling</code></td>
      <td><code>bool</code></td>
      <td><code>set_use_frustum_culling</code>, <code>is_frustum_culling_enabled</code></td>
      <td>Applies immediately to renderer settings.</td>
      <td><code>GaussianSplatNode3D::set_use_frustum_culling</code></td>
    </tr>
    <tr>
      <td><code>rendering/opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_opacity</code>, <code>get_opacity</code></td>
      <td>Per-instance opacity multiplier, clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatNode3D::set_opacity</code></td>
    </tr>
    <tr>
      <td><code>rendering/effect_position_scale</code></td>
      <td><code>float</code></td>
      <td><code>set_effect_position_scale</code>, <code>get_effect_position_scale</code></td>
      <td>Scales how strongly this node responds to sphere position deformation.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/effect_opacity_scale</code></td>
      <td><code>float</code></td>
      <td><code>set_effect_opacity_scale</code>, <code>get_effect_opacity_scale</code></td>
      <td>Scales how strongly this node responds to sphere opacity deformation. Runtime opacity diagnostics stay inactive when this scale is <code>0.0</code>, the node base opacity is <code>0.0</code>, or the matched effector target is neutral.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/scene_effectors_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_scene_effectors_enabled</code>, <code>is_scene_effectors_enabled</code></td>
      <td>Master opt-in for scene-authored sphere effectors. When disabled, all scene-effector runtime diagnostics report inactive.</td>
      <td><code>GaussianSplatNode3D::set_scene_effectors_enabled</code></td>
    </tr>
    <tr>
      <td><code>rendering/scene_effector_layer_mask</code></td>
      <td><code>int</code></td>
      <td><code>set_scene_effector_layer_mask</code>, <code>get_scene_effector_layer_mask</code></td>
      <td>Bitmask filter for matching scene effectors. <code>0</code> disables all matches for this node.</td>
      <td><code>GaussianSplatNode3D::set_scene_effector_layer_mask</code></td>
    </tr>
    <tr>
      <td><code>rendering/scene_effector_scope_root</code></td>
      <td><code>NodePath</code></td>
      <td><code>set_scene_effector_scope_root</code>, <code>get_scene_effector_scope_root</code></td>
      <td>Optional node-side scope narrowing. The path must resolve to this node or one of its ancestors.</td>
      <td><code>GaussianSplatNode3D::set_scene_effector_scope_root</code></td>
    </tr>
    <tr>
      <td><code>rendering/wind_override_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_wind_override_enabled</code>, <code>is_wind_override_enabled</code></td>
      <td>Switches this node from project-wide wind to the per-node wind controls below.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/wind_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_wind_enabled</code>, <code>is_wind_enabled</code></td>
      <td>Enables wind on this node when wind override mode is active.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/wind_strength</code></td>
      <td><code>float</code></td>
      <td><code>set_wind_strength</code>, <code>get_wind_strength</code></td>
      <td>Per-node wind amplitude. Negative input is clamped to <code>0.0</code>.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/wind_direction</code></td>
      <td><code>Vector3</code></td>
      <td><code>set_wind_direction</code>, <code>get_wind_direction</code></td>
      <td>Per-node wind direction used when override mode is enabled.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/wind_frequency</code></td>
      <td><code>float</code></td>
      <td><code>set_wind_frequency</code>, <code>get_wind_frequency</code></td>
      <td>Per-node wind oscillation rate. Negative input is clamped to <code>0.0</code>.</td>
      <td><code>modules/gaussian_splatting/nodes/gaussian_splat_node_3d.cpp</code></td>
    </tr>
    <tr>
      <td><code>rendering/color_grading</code></td>
      <td><code>ColorGradingResource</code></td>
      <td><code>set_color_grading</code>, <code>get_color_grading</code></td>
      <td>Used by real-time grading and baking API.</td>
      <td><code>GaussianSplatNode3D::set_color_grading</code></td>
    </tr>
    <tr>
      <td><code>debug/preview_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_preview_enabled</code>, <code>is_preview_enabled</code></td>
      <td>Controls editor preview visibility path.</td>
      <td><code>GaussianSplatNode3D::set_preview_enabled</code></td>
    </tr>
    <tr>
      <td><code>debug/show_bounds</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_bounds</code>, <code>is_showing_bounds</code></td>
      <td>Toggles gizmo bounds rendering.</td>
      <td><code>GaussianSplatNode3D::set_show_bounds</code></td>
    </tr>
    <tr>
      <td><code>debug/show_statistics</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_statistics</code>, <code>is_showing_statistics</code></td>
      <td>Exposes read-only <code>stats/*</code> inspector fields when active.</td>
      <td><code>GaussianSplatNode3D::set_show_statistics</code></td>
    </tr>
    <tr>
      <td><code>debug/show_tile_grid</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_tile_grid</code>, <code>is_showing_tile_grid</code></td>
      <td>Persists via settings manager and updates renderer when allowed.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_tile_grid</code></td>
    </tr>
    <tr>
      <td><code>debug/show_density_heatmap</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_density_heatmap</code>, <code>is_showing_density_heatmap</code></td>
      <td>Persists via settings manager and updates renderer when allowed.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_density_heatmap</code></td>
    </tr>
    <tr>
      <td><code>debug/show_performance_hud</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_performance_hud</code>, <code>is_showing_performance_hud</code></td>
      <td>Persists via settings manager and updates renderer when allowed.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_performance_hud</code></td>
    </tr>
    <tr>
      <td><code>debug/show_lod_spheres</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_lod_spheres</code>, <code>is_showing_lod_spheres</code></td>
      <td>Updates gizmos only.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_lod_spheres</code></td>
    </tr>
    <tr>
      <td><code>debug/show_performance_overlay</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_performance_overlay</code>, <code>is_showing_performance_overlay</code></td>
      <td>Updates gizmos only.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_performance_overlay</code></td>
    </tr>
    <tr>
      <td><code>debug/overlay_opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_debug_overlay_opacity</code>, <code>get_debug_overlay_opacity</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_debug_overlay_opacity</code></td>
    </tr>
    <tr>
      <td><code>debug/debug_draw_mode</code></td>
      <td><code>int (DebugDrawMode)</code></td>
      <td><code>set_debug_draw_mode</code>, <code>get_debug_draw_mode</code></td>
      <td>Swaps renderer preview mode unless runtime preview override is enabled.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_debug_draw_mode</code></td>
    </tr>
    <tr>
      <td><code>debug/runtime_preview</code></td>
      <td><code>bool</code></td>
      <td><code>set_runtime_preview_enabled</code>, <code>is_runtime_preview_enabled</code></td>
      <td>Temporarily forces runtime modification preview mode in renderer.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_runtime_preview_enabled</code></td>
    </tr>
    <tr>
      <td><code>debug/show_residency_hud</code></td>
      <td><code>bool</code></td>
      <td><code>set_show_residency_hud</code>, <code>is_showing_residency_hud</code></td>
      <td>Persists via settings manager and updates renderer when allowed.</td>
      <td><code>GaussianSplatNodeDebugHelper::set_show_residency_hud</code></td>
    </tr>
  </tbody>
</table>

## Runtime Effect Workflow

The current branch exposes a scene-driven-plus-per-node workflow for gameplay effects:

- Wind can be controlled per node through `rendering/wind_override_enabled`, `rendering/wind_enabled`, `rendering/wind_strength`, `rendering/wind_direction`, and `rendering/wind_frequency`.
- `SphereEffector3D` is the primary authoring surface for sphere deformation and dissolve effects.
- `rendering/effect_position_scale` and `rendering/effect_opacity_scale` let each `GaussianSplatNode3D` partially follow, fully follow, or effectively ignore matched scene effectors.
- `rendering/scene_effectors_enabled`, `rendering/scene_effector_layer_mask`, and `rendering/scene_effector_scope_root` control whether this node participates in scene effectors and which ones may match.
- `rendering/scene_effector_scope_root` is a node-side narrowing filter, not an opt-in to arbitrary world effectors. It must resolve to this node or one of its ancestors, and only effectors with the same resolved scope root will match.
- `rendering/opacity` remains the direct per-node gameplay fade, while scene-effector opacity modulation multiplies on top for dissolve or degeneration effects.
- `get_last_matched_scene_effector_count()`, `get_scene_effector_debug_state()`, `is_scene_effector_position_active()`, and `is_scene_effector_opacity_active()` expose the current runtime match state for gameplay and debugging. `matched_count` is the full logical match set, while `bound_count` is the subset that actually fit into the renderer budget.
- `get_statistics()` mirrors those diagnostics under `matched_scene_effectors`, `bound_scene_effectors`, `scene_effector_truncated`, `scene_effector_position_active`, and `scene_effector_opacity_active`.
- `get_configuration_warnings()` warns about inert scene-effector setups such as both response scales at `0.0`, scene-effector layer mask `0`, opacity modulation with base opacity `0.0`, or an invalid scope root path.

Practical recipes:

- Wind-only: disable `rendering/scene_effectors_enabled` or set both per-node effect scales to `0.0`.
- Sphere position-only: enable a `SphereEffector3D`, keep `rendering/effect_position_scale > 0.0`, and set `rendering/effect_opacity_scale = 0.0`.
- Sphere opacity-only dissolve or fade: enable `SphereEffector3D.affect_opacity`, set `SphereEffector3D.target_opacity` below `1.0`, set `rendering/effect_position_scale = 0.0`, and tune `rendering/effect_opacity_scale`.
- Combined wind + sphere + dissolve: enable per-node wind override and keep both effect scales above zero.

Current bounds:

- Scene-authored effectors are bounded to `4` active bindings per renderer pass. If more match this node, the highest-priority deterministic four are bound and the rest remain logical matches only.
- `SphereEffector3D` defaults to subtree scoping. Put the effector under the same gameplay parent as the splat nodes you want to affect, or use its explicit root/layer controls for broader setups.
- ProjectSettings under `rendering/gaussian_splatting/effects/*` still act as a compatibility fallback when no scene-authored sphere effectors are active. That fallback remains single-global and still honors `rendering/gaussian_splatting/effects/max_effectors` clamped to `0..1`.
- Invalid node-authored runtime inputs are sanitized where supported: opacity clamps to `0.0..1.0`, effect scales clamp to `>= 0.0`, and non-finite opacity or effect scale inputs fall back to stable defaults.
- A matched opacity effector with `target_opacity = 1.0` is intentionally neutral. The node can still report a non-zero matched-effector count while `is_scene_effector_opacity_active()` remains `false`.

Example scenes:

- `tests/examples/godot/test_project/scenes/wind_test.tscn`
- `tests/examples/godot/test_project/scenes/sphere_effector_test.tscn`

See also: [Sphere Effector Workflow](sphere_effector_workflow.md).

### Methods
<table>
  <thead>
    <tr>
      <th>Method</th>
      <th>Behavior</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>get_last_matched_scene_effector_count()</code></td>
      <td>Returns the current number of scene effectors that match this node after scope and layer-mask filtering.</td>
      <td><code>GaussianSplatNode3D::get_last_matched_scene_effector_count</code></td>
    </tr>
    <tr>
      <td><code>get_scene_effector_debug_state()</code></td>
      <td>Returns a Dictionary describing both logical matches and the subset that was actually bound to the renderer, including <code>matched_count</code>, <code>bound_count</code>, <code>truncated</code>, and selected effector ids and names.</td>
      <td><code>GaussianSplatNode3D::get_scene_effector_debug_state</code></td>
    </tr>
    <tr>
      <td><code>is_scene_effector_position_active()</code></td>
      <td>Returns whether any currently matched scene effector can contribute position deformation after node-local scale checks.</td>
      <td><code>GaussianSplatNode3D::is_scene_effector_position_active</code></td>
    </tr>
    <tr>
      <td><code>is_scene_effector_opacity_active()</code></td>
      <td>Returns whether any currently matched scene effector can contribute opacity modulation after node-local opacity and scale checks.</td>
      <td><code>GaussianSplatNode3D::is_scene_effector_opacity_active</code></td>
    </tr>
    <tr>
      <td><code>reload_asset()</code></td>
      <td>Re-runs the asset load for the assigned <code>splat_asset</code> (the same path <code>set_splat_asset()</code> uses). Emits <code>asset_loading_failed</code> when no asset is assigned.</td>
      <td><code>GaussianSplatNode3D::reload_asset</code></td>
    </tr>
    <tr>
      <td><code>is_asset_loading()</code></td>
      <td>Returns asynchronous load state managed by the asset helper.</td>
      <td><code>GaussianSplatNode3D::is_asset_loading</code></td>
    </tr>
    <tr>
      <td><code>set_splat_data(...)</code></td>
      <td>Builds a runtime asset from arrays after validating all optional array lengths.</td>
      <td><code>GaussianSplatNode3D::set_splat_data</code></td>
    </tr>
    <tr>
      <td><code>bake_color_grading()</code></td>
      <td>Bakes the assigned <code>rendering/color_grading</code> into the node's CPU-side data, disables the grading resource, and returns <code>Error</code>. Returns <code>ERR_UNCONFIGURED</code> without a grading resource or without CPU-side data; at this revision only <code>set_splat_data()</code> creates that data, so asset-backed nodes are expected to fail (see "Baking limitation" in the Color Grading Quick Start).</td>
      <td><code>GaussianSplatNode3D::bake_color_grading</code></td>
    </tr>
    <tr>
      <td><code>bake_color_grading_snapshot(grading_snapshot)</code></td>
      <td>Same as <code>bake_color_grading()</code>, but bakes the given <code>ColorGradingResource</code> instead of the assigned one.</td>
      <td><code>GaussianSplatNode3D::bake_color_grading_snapshot</code></td>
    </tr>
    <tr>
      <td><code>restore_color_grading()</code></td>
      <td>Restores original colors and re-enables grading resource if present.</td>
      <td><code>GaussianSplatNode3D::restore_color_grading</code></td>
    </tr>
    <tr>
      <td><code>is_color_grading_baked()</code></td>
      <td>Reports whether bake state is present in the node's CPU-side data; <code>false</code> when there is none.</td>
      <td><code>GaussianSplatNode3D::is_color_grading_baked</code></td>
    </tr>
    <tr>
      <td><code>get_visible_splat_count()</code></td>
      <td>Returns node-level visible splat count after sync with renderer stats.</td>
      <td><code>GaussianSplatNode3D::get_visible_splat_count</code></td>
    </tr>
    <tr>
      <td><code>get_total_splat_count()</code></td>
      <td>Returns node-level total splat count from asset or procedural data.</td>
      <td><code>GaussianSplatNode3D::get_total_splat_count</code></td>
    </tr>
    <tr>
      <td><code>get_last_update_time_ms()</code></td>
      <td>Returns elapsed update time measured in <code>update_splats()</code>.</td>
      <td><code>GaussianSplatNode3D::get_last_update_time_ms</code></td>
    </tr>
    <tr>
      <td><code>get_gpu_memory_mb()</code></td>
      <td>Returns estimated GPU memory derived from loaded or procedural buffers.</td>
      <td><code>GaussianSplatNode3D::get_gpu_memory_mb</code></td>
    </tr>
    <tr>
      <td><code>get_statistics()</code></td>
      <td>Returns node counters and merges any renderer stats dictionary keys.</td>
      <td><code>GaussianSplatNode3D::get_statistics</code></td>
    </tr>
    <tr>
      <td><code>get_configuration_warnings()</code></td>
      <td>Returns warnings for a missing asset or runtime data, zero <code>quality/max_render_distance</code>, inert scene-effector setups (both response scales <code>0</code>, layer mask <code>0</code>, opacity modulation with <code>rendering/opacity</code> <code>0</code>), an invalid <code>rendering/scene_effector_scope_root</code>, and non-uniform scale.</td>
      <td><code>GaussianSplatNode3D::get_configuration_warnings</code></td>
    </tr>
    <tr>
      <td><code>get_renderer()</code></td>
      <td>Returns the shared renderer instance for the node world.</td>
      <td><code>GaussianSplatNode3D::get_renderer</code></td>
    </tr>
    <tr>
      <td><code>update_splats()</code></td>
      <td>Performs the full render update path and refreshes timing and metrics.</td>
      <td><code>GaussianSplatNode3D::update_splats</code></td>
    </tr>
    <tr>
      <td><code>force_update()</code></td>
      <td>Calls <code>update_splats()</code> and emits <code>viewport_visibility_changed</code>.</td>
      <td><code>GaussianSplatNode3D::force_update</code></td>
    </tr>
    <tr>
      <td><code>set_color_variation(variation)</code> / <code>get_color_variation()</code></td>
      <td>Compatibility-only placeholder: stores a value clamped to <code>0.0..0.5</code> (also reachable as the unlisted property <code>painterly/color_variation</code>); the renderer has no color-variation control, so it has no visible effect.</td>
      <td><code>GaussianSplatNode3D::set_color_variation</code></td>
    </tr>
    <tr>
      <td><code>set_use_occlusion_culling(enabled)</code> / <code>is_occlusion_culling_enabled()</code></td>
      <td>Compatibility-only placeholder: stores the flag (also reachable as the unlisted property <code>rendering/occlusion_culling</code>); the renderer has no occlusion-culling toggle, so it has no effect.</td>
      <td><code>GaussianSplatNode3D::set_use_occlusion_culling</code></td>
    </tr>
  </tbody>
</table>

### Signals
<table>
  <thead>
    <tr>
      <th>Signal</th>
      <th>Parameters</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>asset_loaded</code></td>
      <td>None</td>
      <td><code>GaussianSplatNode3D::_bind_methods</code></td>
    </tr>
    <tr>
      <td><code>asset_loading_failed</code></td>
      <td><code>error: String</code></td>
      <td><code>GaussianSplatNode3D::_bind_methods</code></td>
    </tr>
    <tr>
      <td><code>viewport_visibility_changed</code></td>
      <td><code>visible: bool</code></td>
      <td><code>GaussianSplatNode3D::_bind_methods</code></td>
    </tr>
  </tbody>
</table>

## Examples
Both examples expect a `GaussianSplatNode3D` child named `GaussianSplatNode3D`. The first one loads an imported asset: `load()` on a `.ply` returns the `GaussianSplatAsset` that the editor import produced.

```gdscript
extends Node3D

@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D

func _ready() -> void:
    var asset := load("res://splats/scene.ply") as GaussianSplatAsset
    if asset == null:
        push_error("res://splats/scene.ply is not imported as a GaussianSplatAsset")
        return
    splat.set_splat_asset(asset)
    splat.set_quality_preset(GaussianSplatNode3D.QUALITY_BALANCED)
    splat.set_update_mode(GaussianSplatNode3D.UPDATE_MODE_WHEN_VISIBLE)
```

```gdscript
extends Node3D

@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D

func _ready() -> void:
    var positions := PackedVector3Array([Vector3.ZERO])
    var colors := PackedColorArray([Color(1.0, 1.0, 1.0, 1.0)])
    splat.set_splat_data(positions, colors)
    splat.set_update_mode(GaussianSplatNode3D.UPDATE_MODE_MANUAL)
    splat.update_splats()
```

## Troubleshooting
<table>
  <thead>
    <tr>
      <th>Problem</th>
      <th>Action</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>No splats appear after scene start.</td>
      <td>Check <code>get_configuration_warnings()</code> for a missing asset or runtime data and a zero render distance.</td>
      <td><code>GaussianSplatNode3D::get_configuration_warnings</code></td>
    </tr>
    <tr>
      <td><code>set_splat_data()</code> does nothing.</td>
      <td>Ensure every optional array has the same length as <code>positions</code> when provided.</td>
      <td><code>GaussianSplatNode3D::set_splat_data</code></td>
    </tr>
  </tbody>
</table>
