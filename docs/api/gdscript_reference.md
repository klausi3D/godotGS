# GDScript API Reference

Scope: `public`

Scripts scanned: `4`

Undocumented members are omitted by default. Use `--include-undocumented` to include them.

## Script

```
scripts/core/gaussian_splatting_manager.gd
```

### Class

```
gaussian_splatting_manager
```

<table>
  <thead>
    <tr>
      <th>Member</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>_deferred_gpu_init()</code></pre></td>
      <td>Allocates a local RenderingDevice and prints adapter information.</td>
    </tr>
    <tr>
      <td><pre><code>_initialize_gpu_resources()</code></pre></td>
      <td>Schedules GPU initialization after the rendering server is ready.</td>
    </tr>
    <tr>
      <td><pre><code>_process(_delta: float)</code></pre></td>
      <td>Updates frame counters and emits periodic FPS metrics. @param _delta: Frame delta in seconds.</td>
    </tr>
    <tr>
      <td><pre><code>_ready()</code></pre></td>
      <td>Initializes GPU resources for the Gaussian Splatting manager.</td>
    </tr>
    <tr>
      <td><pre><code>get_performance_stats()</code></pre></td>
      <td>Returns performance metrics including sort/render times and GPU memory usage.</td>
    </tr>
    <tr>
      <td><pre><code>load_compute_shaders()</code></pre></td>
      <td>Validates availability of embedded radix-sort compute kernels.</td>
    </tr>
    <tr>
      <td><pre><code>sort_keys_gpu(keys: PackedInt32Array, values: PackedInt32Array = PackedInt32Array())</code></pre></td>
      <td>Sorts key/value pairs using the GPU radix sort pipeline (CPU fallback for now). @param keys: Keys to sort. @param values: Optional values to keep in sync with keys. @return Sorted keys array.</td>
    </tr>
  </tbody>
</table>

## Script

```
templates/gaussian_splat_template/autoload/gaussian_bootstrap.gd
```

### Class

```
gaussian_bootstrap
```

<table>
  <thead>
    <tr>
      <th>Member</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>_log_runtime_configuration()</code></pre></td>
      <td>Prints adapter and sorting configuration information for diagnostics.</td>
    </tr>
    <tr>
      <td><pre><code>_ready()</code></pre></td>
      <td>Resolves the GaussianSplatManager singleton and logs runtime configuration.</td>
    </tr>
    <tr>
      <td><pre><code>ensure_submission_lock()</code></pre></td>
      <td>Acquires a submission lock from the manager when supported. @return Lock object or null if unavailable.</td>
    </tr>
    <tr>
      <td><pre><code>get_global_stats()</code></pre></td>
      <td>Returns global renderer statistics when available.</td>
    </tr>
  </tbody>
</table>

## Script

```
templates/gaussian_splat_template/scripts/camera/orbit_camera_rig.gd
```

### Class

```
OrbitCameraRig
```

<table>
  <thead>
    <tr>
      <th>Member</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>_handle_mouse_button(event: InputEventMouseButton)</code></pre></td>
      <td>Updates orbit/pan state and applies zoom on wheel input. @param event: Mouse button event.</td>
    </tr>
    <tr>
      <td><pre><code>_handle_mouse_motion(event: InputEventMouseMotion)</code></pre></td>
      <td>Applies orbit rotation or panning based on the current input state. @param event: Mouse motion event.</td>
    </tr>
    <tr>
      <td><pre><code>_physics_process(delta: float)</code></pre></td>
      <td>Handles keyboard-driven translation for the orbit rig. @param delta: Frame delta in seconds.</td>
    </tr>
    <tr>
      <td><pre><code>_ready()</code></pre></td>
      <td>Initializes the camera reference and cached orbit angles.</td>
    </tr>
    <tr>
      <td><pre><code>_unhandled_input(event: InputEvent)</code></pre></td>
      <td>Dispatches mouse events to orbit or pan handlers. @param event: Input event from the scene tree.</td>
    </tr>
    <tr>
      <td><pre><code>_zoom(amount: float)</code></pre></td>
      <td>Moves the camera along its local forward axis. @param amount: Positive or negative zoom distance.</td>
    </tr>
    <tr>
      <td><pre><code>focus(bounds: AABB)</code></pre></td>
      <td>Repositions the rig to frame the provided bounds. @param bounds: Axis-aligned bounds to focus on.</td>
    </tr>
  </tbody>
</table>

## Script

```
templates/gaussian_splat_template/scripts/main_scene.gd
```

### Class

```
GaussianTemplateRoot
```

<table>
  <thead>
    <tr>
      <th>Member</th>
      <th>Description</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>_configure_gaussian_node()</code></pre></td>
      <td>Applies template defaults to the GaussianSplatNode3D instance.</td>
    </tr>
    <tr>
      <td><pre><code>_focus_camera()</code></pre></td>
      <td>Centers the orbit camera on the current Gaussian bounds.  Two reasons this is not the one-liner it looks like:  1. `GaussianSplatNode3D::get_aabb()` exists in C++ but is NOT bound to ClassDB, so calling it from GDScript raises "Invalid call. Nonexistent function 'get_aabb'". The bounds are reachable from GDScript only as `get_statistics()["bounds"]`, which is bound. 2. The bounds are not final at `_ready()`: they are computed when the asset payload is uploaded, one or more frames later. Focusing once in `_ready()` frames nothing and leaves the shipped camera transform in place with no diagnostic -- which is why the template opened on an unframed blob. 3. `stats["bounds"]` is the node's LOCAL aabb (`gaussian_splat_node_3d.cpp:1465`), while `OrbitCameraRig.focus()` puts the rig at the centre it is handed, in world space. Passing the local box straight through aimed the camera at the node's local origin: with the template's `GaussianSplatNode3D` at y = 1.5 the rig landed at y = 0.04 and the cloud sat in the upper half of the frame. Transform it first.  So: read the bound accessor, put it in world space, and retry to a wall-clock deadline (never a fixed frame count -- import and upload cost is machine-dependent). If the bounds never arrive, say so rather than failing silently.</td>
    </tr>
    <tr>
      <td><pre><code>_ready()</code></pre></td>
      <td>Configures the template scene: node defaults and camera focus. The performance overlay (`GaussianSplatPerformanceOverlay`) needs no wiring: it discovers the splat node and the viewport camera on its own.</td>
    </tr>
  </tbody>
</table>

Generated by:

```
scripts/extract_gdscript_docs.py
```
