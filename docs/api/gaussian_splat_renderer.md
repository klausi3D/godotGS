# GaussianSplatRenderer API Reference

## Purpose
`GaussianSplatRenderer` is the GPU rendering backend for Gaussian Splatting. It manages the complete rendering pipeline including tile-based rasterization, GPU radix sorting, frustum culling, LOD, painterly post-processing, debug overlays, and performance monitoring. Not typically instantiated directly by users -- accessed through `GaussianSplatNode3D.get_renderer()` (`GaussianSplatNode3D::get_renderer`) or `GaussianSplatWorld3D.get_renderer()`.

The renderer returned there is shared by every Gaussian node in the same `World3D` (`GaussianSplatSceneDirector::get_shared_renderer`). Settings made on it are renderer-wide, and the nodes push their own settings (quality, culling, painterly, debug toggles) onto it when those change (`GaussianSplatNode3D::_apply_renderer_settings`), so a value you set directly on the renderer can be overwritten by a node. Prefer the node or world properties where one exists. This page lists the hand-picked, commonly used members; the generated [GDScript reference](gdscript_reference.md) and the editor class reference are the complete lists.

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
      <td>Initialize the rendering pipeline.</td>
      <td><code>initialize()</code></td>
      <td><code>GaussianSplatRenderer::initialize</code></td>
    </tr>
    <tr>
      <td>Assign splat data for rendering (low-level path).</td>
      <td><code>set_gaussian_data(data)</code></td>
      <td><code>GaussianSplatRenderer::set_gaussian_data</code></td>
    </tr>
    <tr>
      <td>Tune culling and LOD behavior.</td>
      <td><code>set_lod_enabled()</code>, <code>set_frustum_culling()</code>, <code>set_max_splats()</code></td>
      <td><code>GaussianSplatRenderer::set_lod_enabled</code></td>
    </tr>
    <tr>
      <td>Enable stylized brush-stroke rendering.</td>
      <td><code>set_painterly_enabled(true)</code>, <code>set_painterly_stroke_opacity()</code></td>
      <td><code>GaussianSplatRenderer::set_painterly_enabled</code></td>
    </tr>
    <tr>
      <td>Inspect per-frame render statistics.</td>
      <td><code>get_render_stats()</code>, <code>get_visible_splat_count()</code></td>
      <td><code>GaussianSplatRenderer::get_render_stats</code></td>
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
      <td><code>RenderMode</code></td>
      <td><code>MODE_3D</code>, <code>MODE_2D</code>, <code>MODE_HYBRID</code></td>
      <td><code>GaussianSplatRenderer::RenderMode</code></td>
    </tr>
    <tr>
      <td><code>InteractiveState</code></td>
      <td><code>STATE_NORMAL</code>, <code>STATE_HOVERED</code>, <code>STATE_SELECTED</code>, <code>STATE_DISABLED</code></td>
      <td><code>GaussianSplatRenderer::InteractiveState</code></td>
    </tr>
    <tr>
      <td><code>DebugPreviewMode</code></td>
      <td><code>DEBUG_PREVIEW_OFF</code>, <code>DEBUG_PREVIEW_WIREFRAME</code>, <code>DEBUG_PREVIEW_POINTS</code>, <code>DEBUG_PREVIEW_DEPTH</code>, <code>DEBUG_PREVIEW_HEATMAP</code>, <code>DEBUG_PREVIEW_RUNTIME_MODIFICATIONS</code></td>
      <td><code>GaussianSplatRenderer::DebugPreviewMode</code></td>
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
      <td><code>gaussian_data</code></td>
      <td><code>GaussianData</code></td>
      <td><code>set_gaussian_data</code>, <code>get_gaussian_data</code></td>
      <td>Low-level raw-data binding surface. Scene-facing code should normally use nodes, worlds, or submissions instead of writing this directly.</td>
      <td><code>GaussianSplatRenderer::set_gaussian_data</code></td>
    </tr>
    <tr>
      <td><code>painterly_material</code></td>
      <td><code>PainterlyMaterial</code></td>
      <td><code>set_painterly_material</code>, <code>get_painterly_material</code></td>
      <td>Material resource for stylized rendering configuration.</td>
      <td><code>GaussianSplatNode3D::painterly_material</code></td>
    </tr>
    <tr>
      <td><code>render_mode</code></td>
      <td><code>int (RenderMode)</code></td>
      <td><code>set_render_mode</code>, <code>get_render_mode</code></td>
      <td>Selects 3D, 2D, or Hybrid Gaussian projection.</td>
      <td><code>GaussianSplatRenderer::set_render_mode</code></td>
    </tr>
    <tr>
      <td><code>render/opacity_multiplier</code></td>
      <td><code>float</code></td>
      <td><code>set_opacity_multiplier</code>, <code>get_opacity_multiplier</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatRenderer::set_opacity_multiplier</code></td>
    </tr>
    <tr>
      <td><code>sort/static_cache_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_static_sort_cache_enabled</code>, <code>is_static_sort_cache_enabled</code></td>
      <td>Caches sort results for static cameras to avoid re-sorting unchanged views.</td>
      <td><code>GaussianSplatRenderer::set_static_sort_cache_enabled</code></td>
    </tr>
    <tr>
      <td><code>render/cached_render_reuse_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_cached_render_reuse_enabled</code>, <code>is_cached_render_reuse_enabled</code></td>
      <td>Enables reuse of the previous frame's final output when the view is unchanged.</td>
      <td><code>GaussianSplatRenderer::set_cached_render_reuse_enabled</code></td>
    </tr>
    <tr>
      <td><code>interactive_state</code></td>
      <td><code>int (InteractiveState)</code></td>
      <td><code>set_interactive_state</code>, <code>get_interactive_state</code></td>
      <td>Editor visual feedback state: Normal, Hovered, Selected, or Disabled.</td>
      <td><code>GaussianSplatRenderer::set_interactive_state</code></td>
    </tr>
    <tr>
      <td><code>lod_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_lod_enabled</code>, <code>get_lod_enabled</code></td>
      <td>Enables level-of-detail-based splat culling.</td>
      <td><code>GaussianSplatRenderer::set_lod_enabled</code></td>
    </tr>
    <tr>
      <td><code>lod_bias</code></td>
      <td><code>float</code></td>
      <td><code>set_lod_bias</code>, <code>get_lod_bias</code></td>
      <td>Clamped to <code>0.01..8.0</code>. Higher values preserve more detail at distance.</td>
      <td><code>GaussianSplatRenderer::set_lod_bias</code></td>
    </tr>
    <tr>
      <td><code>lod_min_screen_size</code></td>
      <td><code>float</code></td>
      <td><code>set_lod_min_screen_size</code>, <code>get_lod_min_screen_size</code></td>
      <td>Clamped to <code>0.0..64.0</code> pixels. Splats smaller than this are culled.</td>
      <td><code>GaussianSplatRenderer::set_lod_min_screen_size</code></td>
    </tr>
    <tr>
      <td><code>lod_max_distance</code></td>
      <td><code>float</code></td>
      <td><code>set_lod_max_distance</code>, <code>get_lod_max_distance</code></td>
      <td>Clamped to <code>0.0..1000.0</code>. Maximum render distance for splats.</td>
      <td><code>GaussianSplatRenderer::set_lod_max_distance</code></td>
    </tr>
    <tr>
      <td><code>cull/importance_threshold</code></td>
      <td><code>float</code></td>
      <td><code>set_importance_cull_threshold</code>, <code>get_importance_cull_threshold</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Minimum importance value to keep a splat.</td>
      <td><code>GaussianSplatRenderer::set_importance_cull_threshold</code></td>
    </tr>
    <tr>
      <td><code>cull/radius_multiplier</code></td>
      <td><code>float</code></td>
      <td><code>set_cull_radius_multiplier</code>, <code>get_cull_radius_multiplier</code></td>
      <td>Clamped to <code>0.5..16.0</code>. Multiplier applied to splat bounding radius during frustum culling.</td>
      <td><code>GaussianSplatRenderer::set_cull_radius_multiplier</code></td>
    </tr>
    <tr>
      <td><code>cull/frustum_plane_slack</code></td>
      <td><code>float</code></td>
      <td><code>set_cull_frustum_plane_slack</code>, <code>get_cull_frustum_plane_slack</code></td>
      <td>Clamped to <code>1.0..8.0</code>. Extra distance added to frustum planes.</td>
      <td><code>GaussianSplatRenderer::set_cull_frustum_plane_slack</code></td>
    </tr>
    <tr>
      <td><code>cull/near_tolerance</code></td>
      <td><code>float</code></td>
      <td><code>set_cull_near_tolerance</code>, <code>get_cull_near_tolerance</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Distance added to near plane for depth culling.</td>
      <td><code>GaussianSplatRenderer::set_cull_near_tolerance</code></td>
    </tr>
    <tr>
      <td><code>cull/far_tolerance</code></td>
      <td><code>float</code></td>
      <td><code>set_cull_far_tolerance</code>, <code>get_cull_far_tolerance</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Distance subtracted from far plane for depth culling.</td>
      <td><code>GaussianSplatRenderer::set_cull_far_tolerance</code></td>
    </tr>
    <tr>
      <td><code>cull/tiny_splat_screen_radius</code></td>
      <td><code>float</code></td>
      <td><code>set_tiny_splat_screen_radius</code>, <code>get_tiny_splat_screen_radius</code></td>
      <td>Clamped to <code>0.0..10.0</code> pixels. Splats with screen radius below this are culled.</td>
      <td><code>GaussianSplatRenderer::set_tiny_splat_screen_radius</code></td>
    </tr>
    <tr>
      <td><code>cull/opacity_aware_culling</code></td>
      <td><code>bool</code></td>
      <td><code>set_opacity_aware_culling</code>, <code>is_opacity_aware_culling</code></td>
      <td>FlashGS optimization: computes splat radii based on opacity, reducing tile-Gaussian pairs by ~94%.</td>
      <td><code>GaussianSplatRenderer::set_opacity_aware_culling</code></td>
    </tr>
    <tr>
      <td><code>cull/visibility_threshold</code></td>
      <td><code>float</code></td>
      <td><code>set_visibility_threshold</code>, <code>get_visibility_threshold</code></td>
      <td>Clamped to <code>0.001..0.1</code>. Opacity-aware culling tau parameter (default 0.01).</td>
      <td><code>GaussianSplatRenderer::set_visibility_threshold</code></td>
    </tr>
    <tr>
      <td><code>cull/distance_cull_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_distance_cull_enabled</code>, <code>is_distance_cull_enabled</code></td>
      <td>Enables probabilistic culling of splats beyond a start distance during tile binning.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_enabled</code></td>
    </tr>
    <tr>
      <td><code>cull/distance_cull_start</code></td>
      <td><code>float</code></td>
      <td><code>set_distance_cull_start</code>, <code>get_distance_cull_start</code></td>
      <td>Clamped to <code>0.0..1000.0</code>. Distance in world units where culling ramp begins.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_start</code></td>
    </tr>
    <tr>
      <td><code>cull/distance_cull_max_rate</code></td>
      <td><code>float</code></td>
      <td><code>set_distance_cull_max_rate</code>, <code>get_distance_cull_max_rate</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Maximum cull probability at far distances.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_max_rate</code></td>
    </tr>
    <tr>
      <td><code>cull/overflow_autotune_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_overflow_autotune_enabled</code>, <code>is_overflow_autotune_enabled</code></td>
      <td>Dynamically adjusts culling to prevent tile overflow.</td>
      <td><code>GaussianSplatRenderer::set_overflow_autotune_enabled</code></td>
    </tr>
    <tr>
      <td><code>max_splats</code></td>
      <td><code>int</code></td>
      <td><code>set_max_splats</code>, <code>get_max_splats</code></td>
      <td>Clamped to <code>1000..10000000</code>. Maximum splats rendered per frame (default 2,000,000).</td>
      <td><code>GaussianSplatRenderer::set_max_splats</code></td>
    </tr>
    <tr>
      <td><code>frustum_culling</code></td>
      <td><code>bool</code></td>
      <td><code>set_frustum_culling</code>, <code>get_frustum_culling</code></td>
      <td>Enables GPU frustum culling of splats outside the view.</td>
      <td><code>GaussianSplatRenderer::set_frustum_culling</code></td>
    </tr>
    <tr>
      <td><code>painterly/enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_painterly_enabled</code>, <code>get_painterly_enabled</code></td>
      <td>Enables brush-stroke post-processing pipeline.</td>
      <td><code>GaussianSplatRenderer::set_painterly_enabled</code></td>
    </tr>
    <tr>
      <td><code>painterly/low_end_mode</code></td>
      <td><code>bool</code></td>
      <td><code>set_painterly_low_end_mode</code>, <code>get_painterly_low_end_mode</code></td>
      <td>Uses simplified painterly effects for better performance on lower-end hardware.</td>
      <td><code>GaussianSplatRenderer::set_painterly_low_end_mode</code></td>
    </tr>
    <tr>
      <td><code>painterly/enable_strokes</code></td>
      <td><code>bool</code></td>
      <td><code>set_painterly_enable_strokes</code>, <code>get_painterly_enable_strokes</code></td>
      <td>Toggles visible brush stroke overlay rendering.</td>
      <td><code>GaussianSplatRenderer::set_painterly_enable_strokes</code></td>
    </tr>
    <tr>
      <td><code>painterly/internal_scale</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_internal_scale</code>, <code>get_painterly_internal_scale</code></td>
      <td>Clamped to <code>0.25..1.0</code>. Internal render resolution scale for painterly effects.</td>
      <td><code>GaussianSplatRenderer::set_painterly_internal_scale</code></td>
    </tr>
    <tr>
      <td><code>painterly/edge_threshold</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_edge_threshold</code>, <code>get_painterly_edge_threshold</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Lower values detect more edges for outline effects.</td>
      <td><code>GaussianSplatRenderer::set_painterly_edge_threshold</code></td>
    </tr>
    <tr>
      <td><code>painterly/edge_intensity</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_edge_intensity</code>, <code>get_painterly_edge_intensity</code></td>
      <td>Clamped to <code>0.0..8.0</code>. Multiplier for edge outline visibility.</td>
      <td><code>GaussianSplatRenderer::set_painterly_edge_intensity</code></td>
    </tr>
    <tr>
      <td><code>painterly/stroke_length</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_stroke_length</code>, <code>get_painterly_stroke_length</code></td>
      <td>Clamped to <code>1..128</code> pixels. Maximum brush stroke length.</td>
      <td><code>GaussianSplatRenderer::set_painterly_stroke_length</code></td>
    </tr>
    <tr>
      <td><code>painterly/stroke_opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_stroke_opacity</code>, <code>get_painterly_stroke_opacity</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatRenderer::set_painterly_stroke_opacity</code></td>
    </tr>
    <tr>
      <td><code>painterly/gamma</code></td>
      <td><code>float</code></td>
      <td><code>set_painterly_gamma</code>, <code>get_painterly_gamma</code></td>
      <td>Clamped to <code>0.5..4.0</code>. Gamma correction for painterly output (2.2 for sRGB).</td>
      <td><code>GaussianSplatRenderer::set_painterly_gamma</code></td>
    </tr>
    <tr>
      <td><code>debug/show_tile_grid</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_tile_grid</code>, <code>is_debug_show_tile_grid</code></td>
      <td>Overlays the tile grid used by the tile-based rasterizer.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_tile_grid</code></td>
    </tr>
    <tr>
      <td><code>debug/show_density_heatmap</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_density_heatmap</code>, <code>is_debug_show_density_heatmap</code></td>
      <td>Shows splat density as a color heatmap overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_density_heatmap</code></td>
    </tr>
    <tr>
      <td><code>debug/show_performance_hud</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_performance_hud</code>, <code>is_debug_show_performance_hud</code></td>
      <td>Displays a heads-up performance overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_performance_hud</code></td>
    </tr>
    <tr>
      <td><code>debug/show_residency_hud</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_residency_hud</code>, <code>is_debug_show_residency_hud</code></td>
      <td>Displays GPU memory residency information overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_residency_hud</code></td>
    </tr>
    <tr>
      <td><code>debug/show_tile_bounds</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_tile_bounds</code>, <code>get_debug_show_tile_bounds</code></td>
      <td>Overlays tile bounding regions for projection verification.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_tile_bounds</code></td>
    </tr>
    <tr>
      <td><code>debug/show_splat_coverage</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_splat_coverage</code>, <code>get_debug_show_splat_coverage</code></td>
      <td>Visualizes per-pixel splat coverage contribution.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_splat_coverage</code></td>
    </tr>
    <tr>
      <td><code>debug/show_overflow_tiles</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_overflow_tiles</code>, <code>get_debug_show_overflow_tiles</code></td>
      <td>Highlights tiles that exceeded their overlap budget.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_overflow_tiles</code></td>
    </tr>
    <tr>
      <td><code>debug/show_projection_issues</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_projection_issues</code>, <code>get_debug_show_projection_issues</code></td>
      <td>Marks splats with projection problems (behind camera, degenerate covariance).</td>
      <td><code>GaussianSplatRenderer::set_debug_show_projection_issues</code></td>
    </tr>
    <tr>
      <td><code>debug/show_white_albedo</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_white_albedo</code>, <code>get_debug_show_white_albedo</code></td>
      <td>Renders all splats with white albedo to isolate lighting/geometry issues.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_white_albedo</code></td>
    </tr>
    <tr>
      <td><code>debug/show_shadow_opacity</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_shadow_opacity</code>, <code>get_debug_show_shadow_opacity</code></td>
      <td>Visualizes shadow opacity contribution per splat.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_shadow_opacity</code></td>
    </tr>
    <tr>
      <td><code>debug/show_resolve_input</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_resolve_input</code>, <code>get_debug_show_resolve_input</code></td>
      <td>Shows the raw input to the resolve/composite pass.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_resolve_input</code></td>
    </tr>
    <tr>
      <td><code>debug/show_resolve_output</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_resolve_output</code>, <code>get_debug_show_resolve_output</code></td>
      <td>Shows the output of the resolve/composite pass.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_resolve_output</code></td>
    </tr>
    <tr>
      <td><code>debug/show_device_boundaries</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_device_boundaries</code>, <code>is_debug_show_device_boundaries</code></td>
      <td>Outlines regions owned by different RenderingDevice instances.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_device_boundaries</code></td>
    </tr>
    <tr>
      <td><code>debug/show_texture_states</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_show_texture_states</code>, <code>is_debug_show_texture_states</code></td>
      <td>Visualizes GPU texture state transitions.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_texture_states</code></td>
    </tr>
    <tr>
      <td><code>debug/compute_raster_policy</code></td>
      <td><code>int</code></td>
      <td><code>set_debug_compute_raster_policy</code>, <code>get_debug_compute_raster_policy</code></td>
      <td>Enum: Default (0), ForceOn (1), ForceOff (2). Overrides compute rasterization policy.</td>
      <td><code>GaussianSplatRenderer::set_debug_compute_raster_policy</code></td>
    </tr>
    <tr>
      <td><code>debug/dump_gpu_counters</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_dump_gpu_counters</code>, <code>get_debug_dump_gpu_counters</code></td>
      <td>Logs raw GPU counters from each pipeline pass to the output log.</td>
      <td><code>GaussianSplatRenderer::set_debug_dump_gpu_counters</code></td>
    </tr>
    <tr>
      <td><code>debug/enable_pipeline_trace</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_pipeline_trace_enabled</code>, <code>get_debug_pipeline_trace_enabled</code></td>
      <td>Records per-stage pipeline events for later snapshot or JSON dump.</td>
      <td><code>GaussianSplatRenderer::set_debug_pipeline_trace_enabled</code></td>
    </tr>
    <tr>
      <td><code>debug/enable_splat_audit</code></td>
      <td><code>bool</code></td>
      <td><code>set_debug_splat_audit_enabled</code>, <code>get_debug_splat_audit_enabled</code></td>
      <td>Enables per-splat data validation on the GPU.</td>
      <td><code>GaussianSplatRenderer::set_debug_splat_audit_enabled</code></td>
    </tr>
    <tr>
      <td><code>debug/splat_audit_sample_count</code></td>
      <td><code>int</code></td>
      <td><code>set_debug_splat_audit_sample_count</code>, <code>get_debug_splat_audit_sample_count</code></td>
      <td>Clamped to <code>1..64</code>. Number of splats sampled per audit pass.</td>
      <td><code>GaussianSplatRenderer::set_debug_splat_audit_sample_count</code></td>
    </tr>
    <tr>
      <td><code>debug/overlay_opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_debug_overlay_opacity</code>, <code>get_debug_overlay_opacity</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Controls transparency of all debug overlays.</td>
      <td><code>GaussianSplatRenderer::set_debug_overlay_opacity</code></td>
    </tr>
    <tr>
      <td><code>render/solid_coverage_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_solid_coverage_enabled</code>, <code>is_solid_coverage_enabled</code></td>
      <td>Treats splats above an alpha floor as solid for depth/coverage tests.</td>
      <td><code>GaussianSplatRenderer::set_solid_coverage_enabled</code></td>
    </tr>
    <tr>
      <td><code>render/solid_coverage_alpha_floor</code></td>
      <td><code>float</code></td>
      <td><code>set_solid_coverage_alpha_floor</code>, <code>get_solid_coverage_alpha_floor</code></td>
      <td>Clamped to <code>0.0..1.0</code>. Alpha threshold for solid coverage classification.</td>
      <td><code>GaussianSplatRenderer::set_solid_coverage_alpha_floor</code></td>
    </tr>
    <tr>
      <td><code>debug/preview_mode</code></td>
      <td><code>int (DebugPreviewMode)</code></td>
      <td><code>set_debug_preview_mode</code>, <code>get_debug_preview_mode</code></td>
      <td>Selects debug visualization: Off, Wireframe, Points, Depth, Heatmap, or Runtime Modifications.</td>
      <td><code>GaussianSplatRenderer::set_debug_preview_mode</code></td>
    </tr>
  </tbody>
</table>

### Methods

#### Initialization
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
      <td><code>initialize()</code></td>
      <td>Creates GPU resources, shader programs, and the tile-based rendering pipeline. Must be called before any rendering operations.</td>
      <td><code>GaussianSplatRenderer::initialize</code></td>
    </tr>
  </tbody>
</table>

#### Data Management
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
      <td><code>set_gaussian_data(data: GaussianData)</code></td>
      <td>Assigns a GaussianData resource for rendering. This is the renderer's low-level raw-data upload path, used by tests, tools, preview flows, and internal plumbing. Scene-facing workflows should normally prefer node/world submission paths. Dispatches GPU buffer upload on the render thread and returns an <code>Error</code> code.</td>
      <td><code>GaussianSplatRenderer::set_gaussian_data</code></td>
    </tr>
    <tr>
      <td><code>get_gaussian_data() -> GaussianData</code></td>
      <td>Returns the currently assigned GaussianData resource.</td>
      <td><code>GaussianSplatRenderer::get_gaussian_data</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_material(material: PainterlyMaterial)</code></td>
      <td>Assigns a PainterlyMaterial resource for stylized rendering configuration.</td>
      <td><code>GaussianSplatRenderer::set_painterly_material</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_material() -> PainterlyMaterial</code></td>
      <td>Returns the current PainterlyMaterial resource.</td>
      <td><code>GaussianSplatRenderer::get_painterly_material</code></td>
    </tr>
    <tr>
      <td><code>force_sort_for_view(camera_transform: Transform3D)</code></td>
      <td>Forces a fresh depth sort for the given world-to-camera transform, bypassing sort cache.</td>
      <td><code>GaussianSplatRenderer::force_sort_for_view</code></td>
    </tr>
  </tbody>
</table>

#### Rendering Configuration
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
      <td><code>set_render_mode(mode: RenderMode)</code></td>
      <td>Selects the Gaussian projection mode: <code>MODE_3D</code>, <code>MODE_2D</code>, or <code>MODE_HYBRID</code>.</td>
      <td><code>GaussianSplatRenderer::set_render_mode</code></td>
    </tr>
    <tr>
      <td><code>get_render_mode() -> RenderMode</code></td>
      <td>Returns the current render mode.</td>
      <td><code>GaussianSplatRenderer::get_render_mode</code></td>
    </tr>
    <tr>
      <td><code>set_opacity_multiplier(opacity: float)</code></td>
      <td>Sets a global opacity multiplier in range [0, 1] applied to all splats.</td>
      <td><code>GaussianSplatRenderer::set_opacity_multiplier</code></td>
    </tr>
    <tr>
      <td><code>get_opacity_multiplier() -> float</code></td>
      <td>Returns the current opacity multiplier.</td>
      <td><code>GaussianSplatRenderer::get_opacity_multiplier</code></td>
    </tr>
    <tr>
      <td><code>set_static_sort_cache_enabled(enabled: bool)</code></td>
      <td>Enables caching of sort results for static camera views, avoiding re-sorting when the view does not change.</td>
      <td><code>GaussianSplatRenderer::set_static_sort_cache_enabled</code></td>
    </tr>
    <tr>
      <td><code>is_static_sort_cache_enabled() -> bool</code></td>
      <td>Returns true if static sort caching is enabled.</td>
      <td><code>GaussianSplatRenderer::is_static_sort_cache_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_cached_render_reuse_enabled(enabled: bool)</code></td>
      <td>Enables reuse of the previous frame's final output in the output compositor when the view is unchanged.</td>
      <td><code>GaussianSplatRenderer::set_cached_render_reuse_enabled</code></td>
    </tr>
    <tr>
      <td><code>is_cached_render_reuse_enabled() -> bool</code></td>
      <td>Returns true if cached render reuse is enabled.</td>
      <td><code>GaussianSplatRenderer::is_cached_render_reuse_enabled</code></td>
    </tr>
  </tbody>
</table>

#### LOD Controls
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
      <td><code>set_lod_enabled(enabled: bool)</code></td>
      <td>Enables or disables level-of-detail-based splat culling on the GPU.</td>
      <td><code>GaussianSplatRenderer::set_lod_enabled</code></td>
    </tr>
    <tr>
      <td><code>get_lod_enabled() -> bool</code></td>
      <td>Returns true if LOD culling is enabled.</td>
      <td><code>GaussianSplatRenderer::get_lod_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_lod_bias(bias: float)</code></td>
      <td>Sets the LOD bias (0.01-8.0). Higher values preserve more detail at distance.</td>
      <td><code>GaussianSplatRenderer::set_lod_bias</code></td>
    </tr>
    <tr>
      <td><code>get_lod_bias() -> float</code></td>
      <td>Returns the current LOD bias.</td>
      <td><code>GaussianSplatRenderer::get_lod_bias</code></td>
    </tr>
    <tr>
      <td><code>set_lod_min_screen_size(pixels: float)</code></td>
      <td>Sets minimum screen-space size in pixels. Splats smaller than this value are culled.</td>
      <td><code>GaussianSplatRenderer::set_lod_min_screen_size</code></td>
    </tr>
    <tr>
      <td><code>get_lod_min_screen_size() -> float</code></td>
      <td>Returns the LOD minimum screen size.</td>
      <td><code>GaussianSplatRenderer::get_lod_min_screen_size</code></td>
    </tr>
    <tr>
      <td><code>set_lod_max_distance(distance: float)</code></td>
      <td>Sets maximum render distance for splats (0.0-1000.0 world units).</td>
      <td><code>GaussianSplatRenderer::set_lod_max_distance</code></td>
    </tr>
    <tr>
      <td><code>get_lod_max_distance() -> float</code></td>
      <td>Returns the LOD maximum distance.</td>
      <td><code>GaussianSplatRenderer::get_lod_max_distance</code></td>
    </tr>
  </tbody>
</table>

#### Culling Parameters
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
      <td><code>set_importance_cull_threshold(threshold: float)</code></td>
      <td>Sets the minimum importance value in [0, 1] required to keep a splat during culling.</td>
      <td><code>GaussianSplatRenderer::set_importance_cull_threshold</code></td>
    </tr>
    <tr>
      <td><code>get_importance_cull_threshold() -> float</code></td>
      <td>Returns the importance cull threshold.</td>
      <td><code>GaussianSplatRenderer::get_importance_cull_threshold</code></td>
    </tr>
    <tr>
      <td><code>set_cull_radius_multiplier(multiplier: float)</code></td>
      <td>Multiplier (0.5-16.0) applied to splat bounding radius for frustum culling. Larger values reduce culling aggressiveness.</td>
      <td><code>GaussianSplatRenderer::set_cull_radius_multiplier</code></td>
    </tr>
    <tr>
      <td><code>get_cull_radius_multiplier() -> float</code></td>
      <td>Returns the cull radius multiplier.</td>
      <td><code>GaussianSplatRenderer::get_cull_radius_multiplier</code></td>
    </tr>
    <tr>
      <td><code>set_frustum_culling(enabled: bool)</code></td>
      <td>Enables or disables GPU frustum culling for splats outside the camera view.</td>
      <td><code>GaussianSplatRenderer::set_frustum_culling</code></td>
    </tr>
    <tr>
      <td><code>get_frustum_culling() -> bool</code></td>
      <td>Returns true if frustum culling is enabled.</td>
      <td><code>GaussianSplatRenderer::get_frustum_culling</code></td>
    </tr>
    <tr>
      <td><code>set_max_splats(count: int)</code></td>
      <td>Sets the maximum number of splats rendered per frame (1000-10000000). Default is 2,000,000.</td>
      <td><code>GaussianSplatRenderer::set_max_splats</code></td>
    </tr>
    <tr>
      <td><code>get_max_splats() -> int</code></td>
      <td>Returns the maximum splat count.</td>
      <td><code>GaussianSplatRenderer::get_max_splats</code></td>
    </tr>
    <tr>
      <td><code>set_opacity_aware_culling(enabled: bool)</code></td>
      <td>Enables the FlashGS optimization where splat radii are computed from opacity, reducing tile-Gaussian pairs by ~94%.</td>
      <td><code>GaussianSplatRenderer::set_opacity_aware_culling</code></td>
    </tr>
    <tr>
      <td><code>is_opacity_aware_culling() -> bool</code></td>
      <td>Returns true if opacity-aware culling is enabled.</td>
      <td><code>GaussianSplatRenderer::is_opacity_aware_culling</code></td>
    </tr>
    <tr>
      <td><code>set_overflow_autotune_enabled(enabled: bool)</code></td>
      <td>Enables dynamic adjustment of culling parameters to prevent tile overflow at runtime.</td>
      <td><code>GaussianSplatRenderer::set_overflow_autotune_enabled</code></td>
    </tr>
    <tr>
      <td><code>is_overflow_autotune_enabled() -> bool</code></td>
      <td>Returns true if overflow auto-tuning is enabled.</td>
      <td><code>GaussianSplatRenderer::is_overflow_autotune_enabled</code></td>
    </tr>
  </tbody>
</table>

#### Painterly Post-Processing
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
      <td><code>set_painterly_enabled(enabled: bool)</code></td>
      <td>Enables or disables the brush-stroke post-processing pipeline.</td>
      <td><code>GaussianSplatRenderer::set_painterly_enabled</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_enabled() -> bool</code></td>
      <td>Returns true if painterly rendering is enabled.</td>
      <td><code>GaussianSplatRenderer::get_painterly_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_low_end_mode(enabled: bool)</code></td>
      <td>Uses simplified painterly effects for reduced GPU cost.</td>
      <td><code>GaussianSplatRenderer::set_painterly_low_end_mode</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_enable_strokes(enabled: bool)</code></td>
      <td>Toggles visible brush stroke overlay rendering.</td>
      <td><code>GaussianSplatRenderer::set_painterly_enable_strokes</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_internal_scale(scale: float)</code></td>
      <td>Sets internal render resolution scale (0.25-1.0) for painterly effects. Lower values improve performance.</td>
      <td><code>GaussianSplatRenderer::set_painterly_internal_scale</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_edge_threshold(threshold: float)</code></td>
      <td>Sets edge detection threshold (0-1). Lower values detect more edges for outlines.</td>
      <td><code>GaussianSplatRenderer::set_painterly_edge_threshold</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_edge_intensity(intensity: float)</code></td>
      <td>Sets edge intensity multiplier (0-8) controlling outline visibility.</td>
      <td><code>GaussianSplatRenderer::set_painterly_edge_intensity</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_stroke_length(pixels: float)</code></td>
      <td>Sets maximum brush stroke length in screen pixels (1-128).</td>
      <td><code>GaussianSplatRenderer::set_painterly_stroke_length</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_stroke_opacity(opacity: float)</code></td>
      <td>Sets brush stroke opacity (0-1).</td>
      <td><code>GaussianSplatRenderer::set_painterly_stroke_opacity</code></td>
    </tr>
    <tr>
      <td><code>set_painterly_gamma(gamma: float)</code></td>
      <td>Sets gamma correction for painterly output (0.5-4.0). Use 2.2 for sRGB.</td>
      <td><code>GaussianSplatRenderer::set_painterly_gamma</code></td>
    </tr>
  </tbody>
</table>

#### Quality Presets
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
      <td><code>set_quality_preset(preset: String)</code></td>
      <td>Applies a named quality configuration. Accepted values (case-insensitive): <code>"ultra"</code>/<code>"quality"</code>/<code>"high"</code> (max splats, LOD bias 0.8), <code>"balanced"</code>/<code>"medium"</code> (LOD bias 1.0), <code>"performance"</code>/<code>"low"</code> (LOD bias 1.5).</td>
      <td><code>GaussianSplatRenderer::set_quality_preset</code></td>
    </tr>
    <tr>
      <td><code>get_quality_preset() -> String</code></td>
      <td>Returns the normalized preset name: <code>"ultra"</code>, <code>"high"</code>, <code>"medium"</code>, or <code>"low"</code>.</td>
      <td><code>GaussianSplatRenderer::get_quality_preset</code></td>
    </tr>
  </tbody>
</table>

#### Interactive States
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
      <td><code>set_interactive_state(state: InteractiveState)</code></td>
      <td>Sets the visual feedback state for editor interaction: <code>STATE_NORMAL</code>, <code>STATE_HOVERED</code>, <code>STATE_SELECTED</code>, or <code>STATE_DISABLED</code>.</td>
      <td><code>GaussianSplatRenderer::set_interactive_state</code></td>
    </tr>
    <tr>
      <td><code>get_interactive_state() -> InteractiveState</code></td>
      <td>Returns the current interactive state.</td>
      <td><code>GaussianSplatRenderer::get_interactive_state</code></td>
    </tr>
    <tr>
      <td><code>enable_highlight_effect(color: Color)</code></td>
      <td>Applies a highlight tint effect over rendered splats.</td>
      <td><code>GaussianSplatRenderer::enable_highlight_effect</code></td>
    </tr>
    <tr>
      <td><code>enable_outline_effect(color: Color, width: float)</code></td>
      <td>Renders a colored outline around splats with the specified pixel width.</td>
      <td><code>GaussianSplatRenderer::enable_outline_effect</code></td>
    </tr>
    <tr>
      <td><code>remove_visual_effects()</code></td>
      <td>Removes all interactive visual effects (highlight, outline).</td>
      <td><code>GaussianSplatRenderer::remove_visual_effects</code></td>
    </tr>
  </tbody>
</table>

#### Performance Monitoring / Stats
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
      <td><code>get_render_stats() -> Dictionary</code></td>
      <td>Returns a Dictionary of timing, memory, and splat count metrics for the last rendered frame.</td>
      <td><code>GaussianSplatRenderer::get_render_stats</code></td>
    </tr>
    <tr>
      <td><code>get_binning_debug_counters() -> Dictionary</code></td>
      <td>Returns debug counters from the tile binning pass including projection success rates and rejection stats.</td>
      <td><code>GaussianSplatRenderer::get_binning_debug_counters</code></td>
    </tr>
    <tr>
      <td><code>benchmark_sorting_performance()</code></td>
      <td>Runs sorting performance benchmarks and logs results to the output console.</td>
      <td><code>GaussianSplatRenderer::benchmark_sorting_performance</code></td>
    </tr>
    <tr>
      <td><code>run_sort_benchmark(sizes: PackedInt32Array) -> Array</code></td>
      <td>Benchmarks sorting at each element count in the provided array and returns an Array of result Dictionaries.</td>
      <td><code>GaussianSplatRenderer::run_sort_benchmark</code></td>
    </tr>
    <tr>
      <td><code>get_last_sort_metrics() -> Dictionary</code></td>
      <td>Returns timing and algorithm info from the most recent sorting pass.</td>
      <td><code>GaussianSplatRenderer::get_last_sort_metrics</code></td>
    </tr>
    <tr>
      <td><code>get_sort_metrics_history() -> Array</code></td>
      <td>Returns an Array of SortFrameMetrics Dictionaries for recent frames (history depth set by manager config).</td>
      <td><code>GaussianSplatRenderer::get_sort_metrics_history</code></td>
    </tr>
    <tr>
      <td><code>get_sort_time_ms() -> float</code></td>
      <td>Returns the sorting time in milliseconds for the last frame.</td>
      <td><code>GaussianSplatRenderer::get_sort_time_ms</code></td>
    </tr>
    <tr>
      <td><code>get_render_time_ms() -> float</code></td>
      <td>Returns the total render time in milliseconds for the last frame.</td>
      <td><code>GaussianSplatRenderer::get_render_time_ms</code></td>
    </tr>
    <tr>
      <td><code>get_visible_splat_count() -> int</code></td>
      <td>Returns the number of splats that passed culling in the last frame.</td>
      <td><code>GaussianSplatRenderer::get_visible_splat_count</code></td>
    </tr>
    <tr>
      <td><code>get_overflow_tile_count() -> int</code></td>
      <td>Returns the cached count of tiles that exceeded their overlap budget. Call <code>get_overflow_stats()</code> for a fresh readback.</td>
      <td><code>GaussianSplatRenderer::get_overflow_tile_count</code></td>
    </tr>
    <tr>
      <td><code>get_clamped_records() -> int</code></td>
      <td>Returns the cached count of overlap records that were clamped during tile binning.</td>
      <td><code>GaussianSplatRenderer::get_clamped_records</code></td>
    </tr>
    <tr>
      <td><code>get_aggregated_count() -> int</code></td>
      <td>Returns the cached total overlap records emitted across all tiles.</td>
      <td><code>GaussianSplatRenderer::get_aggregated_count</code></td>
    </tr>
    <tr>
      <td><code>get_overflow_stats() -> Dictionary</code></td>
      <td>Triggers a GPU readback and returns overflow statistics. May return the previous frame's data if a readback is pending.</td>
      <td><code>GaussianSplatRenderer::get_overflow_stats</code></td>
    </tr>
  </tbody>
</table>

#### Debug Overlays and Diagnostics
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
      <td><code>get_runtime_diagnostic_snapshot() -> Dictionary</code></td>
      <td>Returns a Dictionary with error stats, timing history, and device capability info.</td>
      <td><code>GaussianSplatRenderer::get_runtime_diagnostic_snapshot</code></td>
    </tr>
    <tr>
      <td><code>get_pipeline_trace_snapshot() -> Dictionary</code></td>
      <td>Returns pipeline events and stage I/O summaries. Requires <code>debug/enable_pipeline_trace</code> to be true.</td>
      <td><code>GaussianSplatRenderer::get_pipeline_trace_snapshot</code></td>
    </tr>
    <tr>
      <td><code>get_pipeline_trace_json() -> String</code></td>
      <td>Serializes the pipeline trace snapshot to a JSON string for external analysis.</td>
      <td><code>GaussianSplatRenderer::get_pipeline_trace_json</code></td>
    </tr>
    <tr>
      <td><code>dump_pipeline_trace_to_file(path: String) -> Error</code></td>
      <td>Writes pipeline trace JSON to the specified file path (e.g., <code>user://pipeline_trace.json</code>).</td>
      <td><code>GaussianSplatRenderer::dump_pipeline_trace_to_file</code></td>
    </tr>
    <tr>
      <td><code>get_tile_renderer() -> TileRenderer</code></td>
      <td>Returns the internal TileRenderer instance for advanced tile-level debugging.</td>
      <td><code>GaussianSplatRenderer::get_tile_renderer</code></td>
    </tr>
    <tr>
      <td><code>was_last_viewport_copy_successful() -> bool</code></td>
      <td>Returns true if the last viewport copy operation completed without error.</td>
      <td><code>GaussianSplatRenderer::was_last_viewport_copy_successful</code></td>
    </tr>
    <tr>
      <td><code>get_last_viewport_copy_source_size() -> Vector2i</code></td>
      <td>Returns the source texture size used in the last viewport copy.</td>
      <td><code>GaussianSplatRenderer::get_last_viewport_copy_source_size</code></td>
    </tr>
    <tr>
      <td><code>get_last_viewport_copy_dest_size() -> Vector2i</code></td>
      <td>Returns the destination texture size used in the last viewport copy.</td>
      <td><code>GaussianSplatRenderer::get_last_viewport_copy_dest_size</code></td>
    </tr>
    <tr>
      <td><code>reload_pipeline_feature_set()</code></td>
      <td>Re-detects GPU capabilities and reloads the pipeline feature set configuration.</td>
      <td><code>GaussianSplatRenderer::reload_pipeline_feature_set</code></td>
    </tr>
  </tbody>
</table>

#### Compact Reference: Remaining Bound Methods

The following methods are bound via `ClassDB::bind_method` and available from GDScript. They follow the standard getter/setter pattern for the properties listed in the Properties table above.

<table>
  <thead>
    <tr>
      <th>Method</th>
      <th>Description</th>
      <th>Source</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><code>get_painterly_low_end_mode() -> bool</code></td>
      <td>Returns true if painterly low-end mode is enabled.</td>
      <td><code>GaussianSplatRenderer::get_painterly_low_end_mode</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_enable_strokes() -> bool</code></td>
      <td>Returns true if brush strokes are enabled.</td>
      <td><code>GaussianSplatRenderer::get_painterly_enable_strokes</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_internal_scale() -> float</code></td>
      <td>Returns the painterly internal render scale.</td>
      <td><code>GaussianSplatRenderer::get_painterly_internal_scale</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_edge_threshold() -> float</code></td>
      <td>Returns the edge detection threshold.</td>
      <td><code>GaussianSplatRenderer::get_painterly_edge_threshold</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_edge_intensity() -> float</code></td>
      <td>Returns the edge intensity multiplier.</td>
      <td><code>GaussianSplatRenderer::get_painterly_edge_intensity</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_stroke_length() -> float</code></td>
      <td>Returns the maximum stroke length in pixels.</td>
      <td><code>GaussianSplatRenderer::get_painterly_stroke_length</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_stroke_opacity() -> float</code></td>
      <td>Returns the stroke opacity.</td>
      <td><code>GaussianSplatRenderer::get_painterly_stroke_opacity</code></td>
    </tr>
    <tr>
      <td><code>get_painterly_gamma() -> float</code></td>
      <td>Returns the painterly gamma value.</td>
      <td><code>GaussianSplatRenderer::get_painterly_gamma</code></td>
    </tr>
    <tr>
      <td><code>set_cull_frustum_plane_slack(slack: float)</code></td>
      <td>Sets extra distance (1.0-8.0) added to frustum planes for conservative culling.</td>
      <td><code>GaussianSplatRenderer::set_cull_frustum_plane_slack</code></td>
    </tr>
    <tr>
      <td><code>get_cull_frustum_plane_slack() -> float</code></td>
      <td>Returns the frustum plane slack distance.</td>
      <td><code>GaussianSplatRenderer::get_cull_frustum_plane_slack</code></td>
    </tr>
    <tr>
      <td><code>set_cull_near_tolerance(tolerance: float)</code></td>
      <td>Sets near plane depth tolerance (0-1).</td>
      <td><code>GaussianSplatRenderer::set_cull_near_tolerance</code></td>
    </tr>
    <tr>
      <td><code>get_cull_near_tolerance() -> float</code></td>
      <td>Returns the near plane tolerance.</td>
      <td><code>GaussianSplatRenderer::get_cull_near_tolerance</code></td>
    </tr>
    <tr>
      <td><code>set_cull_far_tolerance(tolerance: float)</code></td>
      <td>Sets far plane depth tolerance (0-1).</td>
      <td><code>GaussianSplatRenderer::set_cull_far_tolerance</code></td>
    </tr>
    <tr>
      <td><code>get_cull_far_tolerance() -> float</code></td>
      <td>Returns the far plane tolerance.</td>
      <td><code>GaussianSplatRenderer::get_cull_far_tolerance</code></td>
    </tr>
    <tr>
      <td><code>set_tiny_splat_screen_radius(pixels: float)</code></td>
      <td>Sets screen radius threshold (0-10 pixels) for tiny splat culling.</td>
      <td><code>GaussianSplatRenderer::set_tiny_splat_screen_radius</code></td>
    </tr>
    <tr>
      <td><code>get_tiny_splat_screen_radius() -> float</code></td>
      <td>Returns the tiny splat screen radius threshold.</td>
      <td><code>GaussianSplatRenderer::get_tiny_splat_screen_radius</code></td>
    </tr>
    <tr>
      <td><code>set_visibility_threshold(threshold: float)</code></td>
      <td>Sets opacity-aware culling tau parameter (0.001-0.1).</td>
      <td><code>GaussianSplatRenderer::set_visibility_threshold</code></td>
    </tr>
    <tr>
      <td><code>get_visibility_threshold() -> float</code></td>
      <td>Returns the visibility threshold.</td>
      <td><code>GaussianSplatRenderer::get_visibility_threshold</code></td>
    </tr>
    <tr>
      <td><code>set_distance_cull_enabled(enabled: bool)</code></td>
      <td>Enables probabilistic distance-based culling during tile binning.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_enabled</code></td>
    </tr>
    <tr>
      <td><code>is_distance_cull_enabled() -> bool</code></td>
      <td>Returns true if distance culling is enabled.</td>
      <td><code>GaussianSplatRenderer::is_distance_cull_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_distance_cull_start(distance: float)</code></td>
      <td>Sets the world-space distance where culling ramp begins.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_start</code></td>
    </tr>
    <tr>
      <td><code>get_distance_cull_start() -> float</code></td>
      <td>Returns the distance cull start.</td>
      <td><code>GaussianSplatRenderer::get_distance_cull_start</code></td>
    </tr>
    <tr>
      <td><code>set_distance_cull_max_rate(rate: float)</code></td>
      <td>Sets maximum cull probability (0-1) at far distances.</td>
      <td><code>GaussianSplatRenderer::set_distance_cull_max_rate</code></td>
    </tr>
    <tr>
      <td><code>get_distance_cull_max_rate() -> float</code></td>
      <td>Returns the maximum distance cull rate.</td>
      <td><code>GaussianSplatRenderer::get_distance_cull_max_rate</code></td>
    </tr>
    <tr>
      <td><code>set_solid_coverage_enabled(enabled: bool)</code></td>
      <td>Enables solid coverage treatment for splats above the alpha floor.</td>
      <td><code>GaussianSplatRenderer::set_solid_coverage_enabled</code></td>
    </tr>
    <tr>
      <td><code>is_solid_coverage_enabled() -> bool</code></td>
      <td>Returns true if solid coverage is enabled.</td>
      <td><code>GaussianSplatRenderer::is_solid_coverage_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_solid_coverage_alpha_floor(alpha_floor: float)</code></td>
      <td>Sets the alpha threshold (0-1) for solid coverage classification.</td>
      <td><code>GaussianSplatRenderer::set_solid_coverage_alpha_floor</code></td>
    </tr>
    <tr>
      <td><code>get_solid_coverage_alpha_floor() -> float</code></td>
      <td>Returns the solid coverage alpha floor.</td>
      <td><code>GaussianSplatRenderer::get_solid_coverage_alpha_floor</code></td>
    </tr>
    <tr>
      <td><code>set_debug_preview_mode(mode: DebugPreviewMode)</code></td>
      <td>Sets the debug visualization mode (Off, Wireframe, Points, Depth, Heatmap, Runtime Modifications).</td>
      <td><code>GaussianSplatRenderer::set_debug_preview_mode</code></td>
    </tr>
    <tr>
      <td><code>get_debug_preview_mode() -> DebugPreviewMode</code></td>
      <td>Returns the current debug preview mode.</td>
      <td><code>GaussianSplatRenderer::get_debug_preview_mode</code></td>
    </tr>
    <tr>
      <td><code>set_jacobian_bypass_radius_depth_floor(enabled: bool)</code></td>
      <td>Diagnostic toggle: bypasses radius depth floor in Jacobian projection.</td>
      <td><code>GaussianSplatRenderer::set_jacobian_bypass_radius_depth_floor</code></td>
    </tr>
    <tr>
      <td><code>get_jacobian_bypass_radius_depth_floor() -> bool</code></td>
      <td>Returns the Jacobian radius depth floor bypass state.</td>
      <td><code>GaussianSplatRenderer::get_jacobian_bypass_radius_depth_floor</code></td>
    </tr>
    <tr>
      <td><code>set_jacobian_bypass_j_col2_clamp(enabled: bool)</code></td>
      <td>Diagnostic toggle: bypasses J column 2 clamping in Jacobian projection.</td>
      <td><code>GaussianSplatRenderer::set_jacobian_bypass_j_col2_clamp</code></td>
    </tr>
    <tr>
      <td><code>get_jacobian_bypass_j_col2_clamp() -> bool</code></td>
      <td>Returns the Jacobian column 2 clamp bypass state.</td>
      <td><code>GaussianSplatRenderer::get_jacobian_bypass_j_col2_clamp</code></td>
    </tr>
    <tr>
      <td><code>set_jacobian_invert_j_col2_sign(enabled: bool)</code></td>
      <td>Diagnostic toggle: inverts J column 2 sign for radial stretching investigation.</td>
      <td><code>GaussianSplatRenderer::set_jacobian_invert_j_col2_sign</code></td>
    </tr>
    <tr>
      <td><code>get_jacobian_invert_j_col2_sign() -> bool</code></td>
      <td>Returns the Jacobian column 2 sign inversion state.</td>
      <td><code>GaussianSplatRenderer::get_jacobian_invert_j_col2_sign</code></td>
    </tr>
    <tr>
      <td><code>set_max_conic_aspect(aspect: float)</code></td>
      <td>Sets the maximum allowed conic section aspect ratio for projection clamping.</td>
      <td><code>GaussianSplatRenderer::set_max_conic_aspect</code></td>
    </tr>
    <tr>
      <td><code>get_max_conic_aspect() -> float</code></td>
      <td>Returns the maximum conic aspect ratio.</td>
      <td><code>GaussianSplatRenderer::get_max_conic_aspect</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_tile_grid(enabled: bool)</code></td>
      <td>Enables the tile grid overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_tile_grid</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_density_heatmap(enabled: bool)</code></td>
      <td>Enables the density heatmap overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_density_heatmap</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_performance_hud(enabled: bool)</code></td>
      <td>Enables the performance HUD overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_performance_hud</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_residency_hud(enabled: bool)</code></td>
      <td>Enables the residency HUD overlay.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_residency_hud</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_device_boundaries(enabled: bool)</code></td>
      <td>Outlines regions owned by different RenderingDevice instances.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_device_boundaries</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_texture_states(enabled: bool)</code></td>
      <td>Visualizes GPU texture state transitions.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_texture_states</code></td>
    </tr>
    <tr>
      <td><code>set_debug_compute_raster_policy(policy: int)</code></td>
      <td>Overrides compute rasterization policy: 0=Default, 1=ForceOn, 2=ForceOff.</td>
      <td><code>GaussianSplatRenderer::set_debug_compute_raster_policy</code></td>
    </tr>
    <tr>
      <td><code>set_debug_dump_gpu_counters(enabled: bool)</code></td>
      <td>Enables logging of raw GPU counters.</td>
      <td><code>GaussianSplatRenderer::set_debug_dump_gpu_counters</code></td>
    </tr>
    <tr>
      <td><code>set_debug_binning_counters_enabled(enabled: bool)</code></td>
      <td>Enables collection of tile binning debug counters.</td>
      <td><code>GaussianSplatRenderer::set_debug_binning_counters_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_debug_pipeline_trace_enabled(enabled: bool)</code></td>
      <td>Enables per-stage pipeline event recording.</td>
      <td><code>GaussianSplatRenderer::set_debug_pipeline_trace_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_debug_cull_guardrails_enabled(enabled: bool)</code></td>
      <td>Enables extra validation in the culling pipeline.</td>
      <td><code>GaussianSplatRenderer::set_debug_cull_guardrails_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_debug_splat_audit_enabled(enabled: bool)</code></td>
      <td>Enables per-splat GPU data validation.</td>
      <td><code>GaussianSplatRenderer::set_debug_splat_audit_enabled</code></td>
    </tr>
    <tr>
      <td><code>set_debug_splat_audit_sample_count(count: int)</code></td>
      <td>Sets the number of splats sampled per audit pass (1-64).</td>
      <td><code>GaussianSplatRenderer::set_debug_splat_audit_sample_count</code></td>
    </tr>
    <tr>
      <td><code>set_debug_overlay_opacity(opacity: float)</code></td>
      <td>Sets the opacity (0-1) of all debug overlays.</td>
      <td><code>GaussianSplatRenderer::set_debug_overlay_opacity</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_resolve_input(enabled: bool)</code></td>
      <td>Shows the raw resolve/composite pass input.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_resolve_input</code></td>
    </tr>
    <tr>
      <td><code>set_debug_show_resolve_output(enabled: bool)</code></td>
      <td>Shows the resolve/composite pass output.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_resolve_output</code></td>
    </tr>
  </tbody>
</table>

### Signals
This class does not expose any signals through `ADD_SIGNAL`.

## Examples
```gdscript
extends Node3D

@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D

func _ready() -> void:
    var renderer: GaussianSplatRenderer = splat.get_renderer()
    if renderer == null:
        return

    # Configure culling for a dense outdoor scene
    renderer.set_frustum_culling(true)
    renderer.set_lod_enabled(true)
    renderer.set_lod_bias(1.2)
    renderer.set_opacity_aware_culling(true)
    renderer.set_max_splats(3000000)

    # Enable painterly brush-stroke rendering
    renderer.set_painterly_enabled(true)
    renderer.set_painterly_stroke_opacity(0.6)
    renderer.set_painterly_edge_threshold(0.3)
    renderer.set_painterly_stroke_length(24.0)
```

```gdscript
extends Node3D

@onready var splat: GaussianSplatNode3D = $GaussianSplatNode3D

func _process(_delta: float) -> void:
    var renderer: GaussianSplatRenderer = splat.get_renderer()
    if renderer == null:
        return

    # Monitor per-frame performance
    var stats: Dictionary = renderer.get_render_stats()
    var visible: int = renderer.get_visible_splat_count()
    var sort_ms: float = renderer.get_sort_time_ms()
    var render_ms: float = renderer.get_render_time_ms()

    if sort_ms > 4.0:
        # Sort is taking too long -- apply a more aggressive quality preset
        renderer.set_quality_preset("performance")

    # Enable debug overlays when investigating tile overflow
    var overflow: Dictionary = renderer.get_overflow_stats()
    if overflow.has("overflow_tile_count") and overflow["overflow_tile_count"] > 0:
        renderer.set_debug_show_overflow_tiles(true)
        renderer.set_overflow_autotune_enabled(true)
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
      <td>No splats rendered despite data being loaded.</td>
      <td>Verify the renderer is initialized by checking <code>get_render_stats()</code>. Ensure <code>initialize()</code> was called (handled automatically by <code>GaussianSplatNode3D</code>). Check that <code>max_splats</code> is not set to a very low value.</td>
      <td><code>GaussianSplatRenderer::get_render_stats</code></td>
    </tr>
    <tr>
      <td>Sorting is slow on large datasets (>1M splats).</td>
      <td>Call <code>get_sort_time_ms()</code> to measure. Consider enabling <code>set_static_sort_cache_enabled(true)</code> for static cameras, or apply a <code>"performance"</code> quality preset to increase LOD bias and reduce sorted splat count.</td>
      <td><code>GaussianSplatRenderer::get_sort_time_ms</code></td>
    </tr>
    <tr>
      <td>Visual artifacts with radial stretching at screen edges.</td>
      <td>Use the Jacobian diagnostic toggles (<code>set_jacobian_bypass_radius_depth_floor</code>, <code>set_jacobian_bypass_j_col2_clamp</code>, <code>set_jacobian_invert_j_col2_sign</code>) and <code>set_max_conic_aspect</code> to isolate the projection issue.</td>
      <td><code>GaussianSplatRenderer::set_jacobian_bypass_radius_depth_floor</code></td>
    </tr>
    <tr>
      <td>Tile overflow warnings in the output log.</td>
      <td>Enable <code>set_debug_show_overflow_tiles(true)</code> to visualize affected tiles. Then enable <code>set_overflow_autotune_enabled(true)</code> for automatic adjustment, or increase <code>set_cull_radius_multiplier()</code> to cull more aggressively.</td>
      <td><code>GaussianSplatRenderer::set_debug_show_overflow_tiles</code></td>
    </tr>
    <tr>
      <td>Painterly effects not visible after enabling.</td>
      <td>Enable painterly on the node (<code>painterly/enabled</code>), which pushes it to the renderer. When the renderer is shared with other content in the same <code>World3D</code>, node painterly settings are not applied. Also check that <code>set_painterly_enable_strokes(true)</code> is enabled and <code>set_painterly_stroke_opacity()</code> is above zero.</td>
      <td><code>GaussianSplatRenderer::set_painterly_enabled</code></td>
    </tr>
  </tbody>
</table>
