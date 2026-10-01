# GaussianSplatWorld3D API Reference

## Purpose
Use `GaussianSplatWorld3D` to render merged multi-asset Gaussian splat worlds from a `GaussianSplatWorld` resource in a `Node3D` scene (`GaussianSplatWorld3D` in `modules/gaussian_splatting/nodes/gaussian_splat_world_3d.h`).

## Payload Semantics

`GaussianSplatWorld3D` submits the actual payload carried by its `GaussianSplatWorld` resource. A normal uncompressed `.gsplatworld` load is `streamable_uncompressed`: it has a valid chunk payload source and no resident `GaussianData`, so chunks can be read from the file on demand. Compressed `.gsplatworld`, `.gsplatcache`, the C++-only `ResourceFormatLoaderGaussianSplatWorld::load_resident()` (not exposed to GDScript), and direct PLY/SPZ instance imports are `resident_only`.

The route policy controls backend selection, not source residency. Check `get_renderer().get_render_stats()` (a `GaussianSplatRenderer` shared by the Gaussian nodes of this `World3D`) for `payload_mode`, `payload_streamable`, `payload_source_active`, `resident_payload_active`, and `payload_resident_only_reason` when debugging whether a world is actually out-of-core streamable.

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
      <td>Assign a merged world resource.</td>
      <td><code>set_world(world)</code></td>
      <td><code>GaussianSplatWorld3D::set_world</code></td>
    </tr>
    <tr>
      <td>Apply world data to the shared renderer.</td>
      <td><code>apply_world()</code></td>
      <td><code>GaussianSplatWorld3D::apply_world</code></td>
    </tr>
    <tr>
      <td>Remove world data and reset bounds.</td>
      <td><code>clear_world()</code></td>
      <td><code>GaussianSplatWorld3D::clear_world</code></td>
    </tr>
    <tr>
      <td>Configure quality and LOD parameters.</td>
      <td><code>set_lod_bias(bias)</code>, <code>set_max_render_distance(distance)</code>, <code>set_max_splat_count(count)</code></td>
      <td><code>GaussianSplatWorld3D::set_lod_bias</code></td>
    </tr>
    <tr>
      <td>Access the shared renderer instance.</td>
      <td><code>get_renderer()</code></td>
      <td><code>GaussianSplatWorld3D::get_renderer</code></td>
    </tr>
  </tbody>
</table>

## API
### Enums

This class does not define any enums.

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
      <td><code>world</code></td>
      <td><code>GaussianSplatWorld</code></td>
      <td><code>set_world</code>, <code>get_world</code></td>
      <td>Resource containing merged Gaussian data, bounds, and static chunks.</td>
      <td><code>GaussianSplatWorld3D::set_world</code></td>
    </tr>
    <tr>
      <td><code>auto_apply_on_ready</code></td>
      <td><code>bool</code></td>
      <td><code>set_auto_apply_on_ready</code>, <code>is_auto_apply_on_ready</code></td>
      <td>When enabled, calls <code>apply_world()</code> automatically on <code>NOTIFICATION_READY</code>.</td>
      <td><code>GaussianSplatWorld3D::apply_world</code></td>
    </tr>
    <tr>
      <td><code>cast_shadow</code></td>
      <td><code>bool</code></td>
      <td><code>set_cast_shadow</code>, <code>is_cast_shadow</code></td>
      <td>Applies shadow casting setting to the render instance.</td>
      <td><code>GaussianSplatWorld3D::set_cast_shadow</code></td>
    </tr>
    <tr>
      <td><code>quality/lod_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_lod_enabled</code>, <code>is_lod_enabled</code></td>
      <td>Toggles level-of-detail processing on the renderer.</td>
      <td><code>GaussianSplatWorld3D::set_lod_enabled</code></td>
    </tr>
    <tr>
      <td><code>quality/lod_bias</code></td>
      <td><code>float</code></td>
      <td><code>set_lod_bias</code>, <code>get_lod_bias</code></td>
      <td>Clamped to <code>0.1..4.0</code>.</td>
      <td><code>GaussianSplatWorld3D::set_lod_bias</code></td>
    </tr>
    <tr>
      <td><code>quality/max_render_distance</code></td>
      <td><code>float</code></td>
      <td><code>set_max_render_distance</code>, <code>get_max_render_distance</code></td>
      <td>Clamped to <code>&gt;= 0.0</code>. Suffix <code>m</code> in inspector.</td>
      <td><code>GaussianSplatWorld3D::set_max_render_distance</code></td>
    </tr>
    <tr>
      <td><code>quality/max_splat_count</code></td>
      <td><code>int</code></td>
      <td><code>set_max_splat_count</code>, <code>get_max_splat_count</code></td>
      <td>Clamped to <code>&gt;= 1000</code>.</td>
      <td><code>GaussianSplatWorld3D::set_max_splat_count</code></td>
    </tr>
    <tr>
      <td><code>rendering/frustum_culling</code></td>
      <td><code>bool</code></td>
      <td><code>set_use_frustum_culling</code>, <code>is_frustum_culling_enabled</code></td>
      <td>Applies immediately to renderer settings when the renderer is valid.</td>
      <td><code>GaussianSplatWorld3D::set_use_frustum_culling</code></td>
    </tr>
    <tr>
      <td><code>rendering/async_upload_enabled</code></td>
      <td><code>bool</code></td>
      <td><code>set_async_upload_enabled</code>, <code>is_async_upload_enabled</code></td>
      <td>Enables asynchronous GPU upload for world data.</td>
      <td><code>GaussianSplatWorld3D::set_async_upload_enabled</code></td>
    </tr>
    <tr>
      <td><code>rendering/opacity</code></td>
      <td><code>float</code></td>
      <td><code>set_opacity</code>, <code>get_opacity</code></td>
      <td>Clamped to <code>0.0..1.0</code>.</td>
      <td><code>GaussianSplatWorld3D::set_opacity</code></td>
    </tr>
  </tbody>
</table>

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
      <td><code>apply_world()</code></td>
      <td>Ensures a renderer exists, then applies the current <code>GaussianSplatWorld</code> resource to the shared renderer, registering ownership and pushing quality/streaming settings.</td>
      <td><code>GaussianSplatWorld3D::apply_world</code></td>
    </tr>
    <tr>
      <td><code>clear_world()</code></td>
      <td>Unregisters the shared renderer, resets bounds to empty, and updates the render instance.</td>
      <td><code>GaussianSplatWorld3D::clear_world</code></td>
    </tr>
    <tr>
      <td><code>get_renderer()</code></td>
      <td>Returns the shared <code>GaussianSplatRenderer</code> instance for the node's World3D.</td>
      <td><code>GaussianSplatWorld3D::get_renderer</code></td>
    </tr>
  </tbody>
</table>

### Signals

This class does not define any signals.

## Examples
```gdscript
extends Node3D

@onready var world_node: GaussianSplatWorld3D = $GaussianSplatWorld3D

func _ready() -> void:
    var world_res := load("res://worlds/cityscape.gsplatworld") as GaussianSplatWorld
    world_node.set_world(world_res)
    world_node.set_lod_bias(1.5)
    world_node.set_max_render_distance(500.0)
    world_node.set_opacity(0.9)
    world_node.apply_world()
```

```gdscript
extends Node3D

## Clears the current world and applies a new one at runtime.
@onready var world_node: GaussianSplatWorld3D = $GaussianSplatWorld3D

func swap_world(new_resource: GaussianSplatWorld) -> void:
    world_node.clear_world()
    world_node.set_world(new_resource)
    world_node.set_async_upload_enabled(true)
    world_node.set_use_frustum_culling(true)
    world_node.set_max_splat_count(2000000)
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
      <td>World data does not appear after assigning a resource.</td>
      <td>Ensure <code>auto_apply_on_ready</code> is enabled, or call <code>apply_world()</code> manually after the node enters the tree.</td>
      <td><code>GaussianSplatWorld3D::apply_world</code></td>
    </tr>
    <tr>
      <td>Non-identity transform warning logged at startup.</td>
      <td>Place the <code>GaussianSplatWorld3D</code> node at the scene origin. Merged world data is assumed to be in world space.</td>
      <td><code>GaussianSplatWorld3D::_apply_world_internal</code></td>
    </tr>
    <tr>
      <td>A second <code>GaussianSplatWorld3D</code> in the same World3D does not render.</td>
      <td>The scene director accepts one world submission per scenario; a later world node is skipped while another holds it (logged only when world debug logging is enabled). Keep one <code>GaussianSplatWorld3D</code> per World3D.</td>
      <td><code>GaussianSplatWorld3D::_register_shared_renderer</code></td>
    </tr>
    <tr>
      <td>The world node renders nothing although a resource is assigned.</td>
      <td>Verify the <code>GaussianSplatWorld</code> resource was exported with splat data (check the source <code>GaussianSplatContainer</code> merge result), and check <code>get_renderer().get_render_stats()</code> for <code>total_splats</code> and <code>payload_mode</code>.</td>
      <td><code>GaussianSplatWorld3D::_register_shared_renderer</code></td>
    </tr>
  </tbody>
</table>
