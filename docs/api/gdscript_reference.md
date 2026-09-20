# GDScript API Reference

Last generated: 2026-09-19

Scope: `public`

Scripts scanned: `5`

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
      <td>Configures the template scene by wiring the node, overlay, and camera focus.</td>
    </tr>
    <tr>
      <td><pre><code>_wire_overlay()</code></pre></td>
      <td>Binds the overlay to the gaussian node and camera rig.</td>
    </tr>
  </tbody>
</table>

## Script

```
templates/gaussian_splat_template/scripts/ui/performance_overlay.gd
```

### Class

```
GaussianPerformanceOverlay
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
      <td><pre><code>_colorize_buffer_percent(percent: float, value_str: String)</code></pre></td>
      <td>Returns color-coded string for buffer usage percentage</td>
    </tr>
    <tr>
      <td><pre><code>_colorize_gpu_time(time_ms: float, value_str: String)</code></pre></td>
      <td>Returns color-coded string for GPU timing (ms)</td>
    </tr>
    <tr>
      <td><pre><code>_colorize_lod_reduction(percent: float, value_str: String)</code></pre></td>
      <td>Returns color-coded string for LOD reduction percentage (higher = more aggressive = red)</td>
    </tr>
    <tr>
      <td><pre><code>_colorize_vram_percent(percent: float, value_str: String)</code></pre></td>
      <td>Returns color-coded string for VRAM usage percentage</td>
    </tr>
    <tr>
      <td><pre><code>_flag(value: Variant)</code></pre></td>
      <td>Coerces a monitor value to int for BRANCHING only, never for display.</td>
    </tr>
    <tr>
      <td><pre><code>_fmt_saving_pct(value: Variant)</code></pre></td>
      <td>A percentage where HIGHER is better: SH compression ratio. Kept separate from `_fmt_stall_pct` on purpose -- sharing one helper painted a healthy 70 % compression ratio in the stall meter's warning colour.</td>
    </tr>
    <tr>
      <td><pre><code>_fmt_stall_pct(value: Variant)</code></pre></td>
      <td>A percentage where HIGHER is worse: pipeline stall time.</td>
    </tr>
    <tr>
      <td><pre><code>_format_number(value: int)</code></pre></td>
      <td>Formats large numbers with K/M suffixes for readability</td>
    </tr>
    <tr>
      <td><pre><code>_monitor(id: String)</code></pre></td>
      <td>Reads one custom monitor.  Call sites pass the FULL id, `gaussian_splatting/...`, spelled out. That is deliberate and not verbosity: `tests/ci/check_shipped_project_scripts.py` checks every monitor id a shipped script reads against the ids `performance_monitors.cpp` actually registers, and it derives both sides from string literals. Building the id from a prefix constant at run time would hide every read from that check -- a guard that can find nothing to check is not a guard. This file is the reason the check exists.  @param id: Full monitor id, e.g. `gaussian_splatting/cpu_setup_time_ms`. @return The monitor value, or `null` when this build registers no such monitor. `null` renders as `n/a`; it is never coerced to 0. `Performance` is the engine singleton object itself -- `Performance.get_singleton()` is not bound to ClassDB and does not parse.</td>
    </tr>
    <tr>
      <td><pre><code>_nonzero_or_null(value: Variant)</code></pre></td>
      <td>Distinguishes "measured zero" from "no producer to ask" for the *registered* monitors whose getters return a literal 0 when there is no renderer or no RenderingDevice behind them -- `_get_cpu_setup_time_ms` and the three `_get_vram_device_*_mb` (performance_monitors.cpp:617, :819-838). Those are registered, so `_monitor()` hands back a real `0.0` that `n/a` handling cannot catch. A host wall-clock stage that ran takes a non-zero number of microseconds, and a live RenderingDevice never reports zero bytes total, so exactly 0.0 from these four means "nothing answered". @return The value, or `null` when it is the producer's no-renderer default.</td>
    </tr>
    <tr>
      <td><pre><code>_process(delta: float)</code></pre></td>
      <td>Tracks frame timing and refreshes the overlay at the configured interval. @param delta: Frame delta in seconds.</td>
    </tr>
    <tr>
      <td><pre><code>_ready()</code></pre></td>
      <td>Enables processing so the overlay refreshes at runtime.</td>
    </tr>
    <tr>
      <td><pre><code>_refresh_overlay()</code></pre></td>
      <td>Rebuilds the overlay text from the renderer's per-frame statistics and the registered custom monitors.</td>
    </tr>
    <tr>
      <td><pre><code>_section_device_vram(lines: Array[String])</code></pre></td>
      <td>Real device allocation, from `RenderingDevice::get_memory_usage`.  This replaces the old VRAM BUDGET block. That block reported the streaming regulator's budget model, which is structurally 0 on a resident scene, and three quantities -- reserved / allocated-chunks / pool size -- that no producer in this engine computes at all. What building that pool/reservation model would actually take is recorded in #1029.</td>
    </tr>
    <tr>
      <td><pre><code>_section_frame(lines: Array[String])</code></pre></td>
      <td>Frame pacing. Engine-side values; not renderer telemetry.</td>
    </tr>
    <tr>
      <td><pre><code>_section_global(lines: Array[String])</code></pre></td>
      <td>Process-global manager state.  `GaussianSplatManager.get_global_stats()` reports totals over buffers registered with the manager. The node/renderer route used by this template registers none, so `total_gaussians` / `total_memory_mb` / `buffer_count` are structurally 0 here -- printing them next to 768 rendered splats is the same lie as any other unmeasured 0, so the counts are shown only when the registry is non-empty.  `gpu_sorting_enabled` used to be printed here as `GPU sorting: enabled`. It is a DEPRECATED no-op: `gaussian_splat_manager.cpp:286-295` says it "is reported for compatibility ... but does NOT gate the sort path -- no renderer reads it; GPU sorting is always used when available". With the project setting false the row read `disabled` while the GPU sort ran, which is a displayed value that is not a measurement of what its label names -- this panel's own rule. The measured quantity is `sort_route_uid`, now shown in the GPU PASSES block.</td>
    </tr>
    <tr>
      <td><pre><code>_section_gpu_passes(lines: Array[String], stats: Dictionary)</code></pre></td>
      <td>The six resolved GPU pass timestamps, plus the identity check.  Every row is gated on its own `gpu_*_valid` flag. That flag is STICKY by design (`tile_renderer.cpp:2867-2873`): per-pass timestamps resolve only intermittently, so the renderer keeps the last resolved value and clears the flag only after 120 resolves without one (~2 s, `:2908-2927`). A green row is therefore the most recent resolved value, which may be several frames old -- so the staleness is printed, not denied.</td>
    </tr>
    <tr>
      <td><pre><code>_section_host_stages(lines: Array[String], stats: Dictionary)</code></pre></td>
      <td>Host-side wall-clock timings. Kept apart from the GPU pass block because they are measured with `OS::get_ticks_usec()` around a call, not with a GPU timestamp -- the old panel filed the cull time under "GPU".</td>
    </tr>
    <tr>
      <td><pre><code>_section_visibility(lines: Array[String], stats: Dictionary)</code></pre></td>
      <td>What is on screen, named by the domain the renderer actually culls in.</td>
    </tr>
    <tr>
      <td><pre><code>_split_columns(lines: Array[String])</code></pre></td>
      <td>Splits the rendered rows across the panel's two columns on section boundaries, balancing the two by line count.  The whole panel is ~50 lines; a single column of that is taller than a 720 px viewport, and the shipped 120 px auto-scrolling box showed the user only its last five lines. Sections are kept whole -- a header is never separated from its rows -- and once a section spills into the right column every later section follows it, so the panel reads top-left then top-right. @param lines: The rendered rows, section headers included. @return [left_text, right_text].</td>
    </tr>
    <tr>
      <td><pre><code>_stat(stats: Dictionary, key: String)</code></pre></td>
      <td>Reads one `get_statistics()` entry, or `null` when the key is absent.</td>
    </tr>
    <tr>
      <td><pre><code>_streaming_ready()</code></pre></td>
      <td>True when a streaming system is attached AND reporting. Every streaming, LOD and SH-compression monitor is gated behind `_get_active_splat_renderer(true)` in `performance_monitors.cpp`, so on a resident-only scene they all return a default -- 0 for most, but 1 and 1.0 for `lod_splat_skip_factor` and `lod_opacity_multiplier`, which is indistinguishable from a live reading. This flag is what makes those rows say `n/a` instead.</td>
    </tr>
    <tr>
      <td><pre><code>_try_resolve_camera()</code></pre></td>
      <td>Resolves the camera from the configured NodePath when missing.</td>
    </tr>
    <tr>
      <td><pre><code>_valid_ms(stats: Dictionary, time_key: String, valid_key: String)</code></pre></td>
      <td>Reads a GPU pass time, gated on that pass's own validity flag. @return The time in ms, or `null` when the pass did not resolve this frame.</td>
    </tr>
    <tr>
      <td><pre><code>set_camera_node(node: Node3D)</code></pre></td>
      <td>Assigns the camera rig node used for pose reporting. @param node: Camera rig node.</td>
    </tr>
    <tr>
      <td><pre><code>set_gaussian_node(node: GaussianSplatNode3D)</code></pre></td>
      <td>Assigns the Gaussian node used for statistics queries. @param node: GaussianSplatNode3D to monitor.</td>
    </tr>
  </tbody>
</table>

Generated by:

```
scripts/extract_gdscript_docs.py
```
