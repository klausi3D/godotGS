# Project Settings Reference

> **Stability (manual annotation, 2026-09-26; [#1010](https://github.com/klausi3D/godotGS/issues/1010)).**
> No setting on this page carries a stability promise during the public alpha. Any key
> may be renamed, change meaning or default, or be removed in any release, without a
> deprecation period (maintainer decision, 2026-09-27; a deprecation lifecycle begins at
> v1.0); see [API Stability](../development/api-stability.md). The `publicness` field in
> `modules/gaussian_splatting/config/project_settings_manifest.json` does not change
> that: it is a v1.0 classification exercise in progress. As of `12f2feb61da` (counts
> hand-copied; the manifest is the authority), 153 of the 193 settings resolve to
> `public`, 150 of them by inheriting a family default rather than by a decision about
> that key, and 139 of the 193 are `test_coverage: inventory_only`, with nothing
> verifying they do anything. Read `public` as "the bucket this key's family defaults to
> today", not as "supported".
>
> `modules/gaussian_splatting/config/project_settings_public_api_baseline.json` is a
> **change record**, not a promise either. Its guard compares the baseline against the
> default branch's history: the merge-base with `origin/main`, `main`, `origin/master` or
> `master`, or the ref in `GS_PROJECT_SETTINGS_MANIFEST_BASE_REF`. A key present there
> has to stay listed and gain a `retired_settings` entry when it is removed. The
> comparison does not cover a stacked PR's own base, and it is skipped when none of
> those refs resolves. The [migration notes](gaussian-project-settings-migration.md)
> describe each recorded removal.
> A recorded removal is still a removal, and during the alpha it may happen in any
> release, with or without a deprecated alias first.
>
> **Culling status (manual annotation, 2026-04-26).** Per-key state of the
> `rendering/gaussian_splatting/culling/*` settings:
>
> | Key | Status | Where it goes today |
> |---|---|---|
> | `octree_max_depth` | **fallback only** | Read into `GPUCuller::CullingState::culling_octree_max_depth` and consumed in `ensure_hierarchical_structure()`, which only the CPU octree fallback of `GPUCuller::cull_for_view` calls. The default path is the GPU instance/chunk cull, which does not read it; the fallback runs only when that cull fails or outside the frame pipeline (see [Culling and hierarchy](../architecture/culling-and-hierarchy.md); status corrected 2026-10-01). |
> | `min_gaussians_per_leaf` | **fallback only** | Read into `GPUCuller::CullingState::culling_min_gaussians`; like `octree_max_depth`, it reaches only the CPU octree fallback through `ensure_hierarchical_structure()`. |
> | `opacity_aware_bounds` | **live (default)** | Wired in [#167](https://github.com/klausi3D/godotGS/issues/167): read by `GPUCuller::update_culling_settings()` as the project-wide DEFAULT behind the per-renderer `cull/opacity_aware_culling` property (bound in `GaussianSplatRenderer::_bind_methods()`). An explicit per-node value overrides the global. Registered default `true` equals the construction default, so the wiring is behavior-neutral. |
> | `visibility_threshold` | **live (default)** | Wired in [#167](https://github.com/klausi3D/godotGS/issues/167): read by `GPUCuller::update_culling_settings()` as the project-wide DEFAULT behind the per-renderer `cull/visibility_threshold` property (bound in `GaussianSplatRenderer::_bind_methods()`), clamped to `0.0001..0.1`. Explicit per-node value wins. Registered default `gs::RASTER_ALPHA_THRESHOLD` (1/255) equals the construction default. |
>
> The `cull/overflow_autotune_enabled` sibling (different section) was wired the
> same way in [#167](https://github.com/klausi3D/godotGS/issues/167): it is the
> project-wide default behind the per-renderer `cull/overflow_autotune_enabled` property
> (registered default `false`, still experimental). All three keys stay
> behavior-neutral because each registered global default equals the construction
> default the culler already used.
>
> The dormant `ClusterCuller` stack and its three companion project settings
> (`cluster_culling_enabled`, `cluster_target_size`, `cluster_frustum_slack`) were
> deleted per [#293](https://github.com/klausi3D/godotGS/issues/293); see
> [../architecture/culling-and-hierarchy.md](../architecture/culling-and-hierarchy.md)
> for historical context and
> [../architecture/tier2_cluster_culling_spec.md](../architecture/tier2_cluster_culling_spec.md)
> for the future-rewrite spec.
>
> **Logging levels (manual annotation, 2026-07-12; #172).** The Gaussian logger
> uses one severity ordering, from most to least quiet:
> `off < error < warn < info < debug < trace`.
>
> - `logging/verbosity` is the **master** level (default `warn` -> error +
>   warnings). The legacy value `silent` is a **back-compat alias that now maps to
>   `off`** (fully silent); loading a project still pinned to `silent` prints a
>   one-time migration warning. The editor enum offers
>   `off,error,warn,info,debug,trace` (`silent` is still accepted but hidden).
> - `logging/<category>` (`general`, `renderer`, `streaming`, `gpu_sort`,
>   `gpu_memory`, `compositor`, `command_buffer`, `tests`) is a **per-category
>   override**. Each defaults to the `inherit` sentinel = "follow `verbosity`".
>   Set a concrete level to override verbosity for that one category (an explicit
>   level wins even over `verbosity=off`). The enum offers
>   `inherit,off,error,warn,info,debug,trace`.
> - **Effective level** = `category == inherit ? verbosity : category`. A message
>   at level L is shown iff L's severity `<=` the effective ceiling and the ceiling
>   is not `off`. The default (`verbosity=warn`, all categories `inherit`) shows
>   error + warn for every category -- identical to the pre-#172 behavior.
> - These values are read **once at startup** (`std::call_once` in
>   `gs_logger.cpp`); changing them at runtime has no effect until restart.
>
> This block is hand-maintained. Everything below it is regenerated by `scripts/generate_project_settings_reference.py`.

## Purpose
Every Gaussian Splatting project setting key, its registered default, and the source file that registers it.

The key set comes from `modules/gaussian_splatting/config/project_settings_manifest.json`. The defaults and
registering files come from the module source. Generation fails when the two disagree.
A default is shown as the C++ expression passed at registration (for example a named constant), not as an evaluated value.

## Usage
| Task | Action |
| --- | --- |
| Regenerate this reference | Run `python3 scripts/generate_project_settings_reference.py`. |
| Check this page covers the manifest | Run `python3 tests/ci/check_project_settings_reference.py`. |

## API

### Coverage

| Coverage | Count |
| --- | ---: |
| Keys in the manifest | 193 |
| Registered by the module at startup | 182 |
| Read by the module but not registered | 6 |
| Deprecated aliases (read only, never registered) | 5 |

### Registered keys
Registered through `GLOBAL_DEF(...)`, `GLOBAL_DEF_RST(...)` or `ProjectSettings::set_initial_value(...)`, grouped by key prefix.

#### (top level)

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/renderdoc_compatibility</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/shared_submission_device_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### animation

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_direction_x</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_direction_y</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_direction_z</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_frequency</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_spatial_frequency</code></pre></td>
      <td><pre><code>0.1f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_strength</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/animation/wind_time_scale</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### cache

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/cache/spirv_cache_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/register_types.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/cache/spirv_cache_max_mb</code></pre></td>
      <td><pre><code>64</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/register_types.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### composite

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/composite/depth_test</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/composite/per_splat_depth_clip</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/composite/scene_depth_policy</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### compression

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/compression/per_chunk_quantization</code></pre></td>
      <td><pre><code>-1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/quantization_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/compression/position_bits</code></pre></td>
      <td><pre><code>16</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/quantization_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/compression/quantize_scales</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/quantization_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/compression/scale_bits</code></pre></td>
      <td><pre><code>12</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/quantization_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### cull

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/cull/overflow_autotune_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### culling

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/culling/alpha_clip</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/culling/min_gaussians_per_leaf</code></pre></td>
      <td><pre><code>32</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/culling/octree_max_depth</code></pre></td>
      <td><pre><code>8</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/culling/opacity_aware_bounds</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/culling/visibility_threshold</code></pre></td>
      <td><pre><code>gs::RASTER_ALPHA_THRESHOLD</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### debug

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/cull_guardrail_drop_ratio</code></pre></td>
      <td><pre><code>0.75f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/cull_guardrail_min_visible</code></pre></td>
      <td><pre><code>256</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/cull_guardrail_position_epsilon</code></pre></td>
      <td><pre><code>0.05f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/cull_guardrail_rotation_epsilon</code></pre></td>
      <td><pre><code>0.01f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_all_debug</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_autotune_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_binning_counters</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_cull_counters</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_cull_guardrails</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_data_logging</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_frame_logging</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_frame_logging_verbose</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_gpu_counter_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_sort_path_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_tile_dispatch_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_tile_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/enable_tile_pipeline_logs</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/frame_log_frequency</code></pre></td>
      <td><pre><code>300</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/show_density_heatmap</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/show_performance_hud</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/show_residency_hud</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/debug/show_tile_grid</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### diagnostics

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/perf_gate_budget_ms</code></pre></td>
      <td><pre><code>16.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/perf_gate_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/perf_gate_splat_threshold</code></pre></td>
      <td><pre><code>100000</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/sort_target_time_ms</code></pre></td>
      <td><pre><code>2.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp<br>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/startup_trace</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/register_types.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/summary_history_size</code></pre></td>
      <td><pre><code>60</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/summary_interval_frames</code></pre></td>
      <td><pre><code>600</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/diagnostics/validate_production_metrics</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### editor

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/editor/hot_reload_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/editor/hot_reload_poll_interval_sec</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### effects

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/max_effectors</code></pre></td>
      <td><pre><code>1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_affect_opacity</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_affect_position</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_center_x</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_center_y</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_center_z</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_falloff</code></pre></td>
      <td><pre><code>2.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_frequency</code></pre></td>
      <td><pre><code>2.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_opacity_strength</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_radius</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_strength</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/effects/sphere_effector_target_opacity</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### gpu_sorting

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/adaptive_overlap_budget_enabled</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.adaptive_overlap_budget_enabled</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/bounded_buffer_shrink_enabled</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.bounded_buffer_shrink_enabled</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/debug_validate_prefix</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.debug_validate_prefix</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/depth_bits</code></pre></td>
      <td><pre><code>GPUSortingConstants::DEFAULT_DEPTH_BITS</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_bandwidth_monitoring</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.enable_bandwidth_monitoring</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_compute_raster</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.enable_compute_raster</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_performance_logging</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.enable_performance_logging</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_prefix_readback</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.enable_prefix_readback</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_stage_timestamps</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.enable_stage_timestamps</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/enable_tie_breaker</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/gpu_preset</code></pre></td>
      <td><pre><code>"high"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/key_bits</code></pre></td>
      <td><pre><code>GPUSortingConstants::DEFAULT_KEY_BITS</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/max_overlap_records</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/max_overlap_records_adaptive_min</code></pre></td>
      <td><pre><code>100000</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/max_raster_splats_per_tile</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/max_sort_elements</code></pre></td>
      <td><pre><code>50000000</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/performance_log_interval</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.performance_log_interval</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/profiling_preserve_gpu_timestamps</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.profiling_preserve_gpu_timestamps</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/radix_bits</code></pre></td>
      <td><pre><code>GPUSortingConstants::DEFAULT_RADIX_BITS</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/subgroup_prefix_mode</code></pre></td>
      <td><pre><code>int(g_gpu_sorting_config.subgroup_prefix_mode)</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/tile_bits</code></pre></td>
      <td><pre><code>GPUSortingConstants::DEFAULT_TILE_BITS</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/gpu_sorting/workgroup_size</code></pre></td>
      <td><pre><code>GPUSortingConstants::DEFAULT_WORKGROUP_SIZE</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### import

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/import/gsplatworld_compression_enabled</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/import/use_gsplatworld_cache</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### init

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/init/eager_raster_pipeline</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### lighting

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/direct_light_scale</code></pre></td>
      <td><pre><code>0.5f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/indirect_sh_scale</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/shadow_receiver_bias_max</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/shadow_receiver_bias_min</code></pre></td>
      <td><pre><code>0.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/shadow_receiver_bias_scale</code></pre></td>
      <td><pre><code>0.2f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lighting/shadow_strength</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### lod

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/base_threshold</code></pre></td>
      <td><pre><code>-1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/bias</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/blend_distance</code></pre></td>
      <td><pre><code>5.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/blend_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/diagnostic_logging</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/hysteresis_zone</code></pre></td>
      <td><pre><code>0.5f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/importance_threshold</code></pre></td>
      <td><pre><code>-1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/max_distance</code></pre></td>
      <td><pre><code>-1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/min_screen_size_pixels</code></pre></td>
      <td><pre><code>1.5f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/num_levels</code></pre></td>
      <td><pre><code>4</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/opacity_fade_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/sh_reduction_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/splat_skip_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### logging

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/command_buffer</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/compositor</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/general</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/gpu_memory</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/gpu_sort</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/rate_limit_ms</code></pre></td>
      <td><pre><code>1000</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/renderer</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/streaming</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/tests</code></pre></td>
      <td><pre><code>"inherit"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/logging/verbosity</code></pre></td>
      <td><pre><code>"warn"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### pipeline

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/enable_all_pipeline_experimental</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.enable_all_pipeline_experimental</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/enable_fast_raster</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.enable_fast_raster</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/enable_packed_stage_data</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.enable_packed_stage_data</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/enable_sh_amortization</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.enable_sh_amortization</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/enable_tighter_bounds</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.enable_tighter_bounds</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/pipeline/sh_amortization_divisor</code></pre></td>
      <td><pre><code>g_pipeline_feature_set.sh_amortization_divisor</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/pipeline_feature_set.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### quality

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/quality/tier_apply_pipeline_toggles</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/quality/tier_apply_streaming_budgets</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/quality/tier_preset</code></pre></td>
      <td><pre><code>"custom"</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### rasterization

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/rasterization/low_pass_filter</code></pre></td>
      <td><pre><code>0.05f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### rendering

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/rendering/sh_bands</code></pre></td>
      <td><pre><code>-1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/sh_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### resident

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### sorting

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/bitonic_max_elements</code></pre></td>
      <td><pre><code>(int)sorting_bitonic_max</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/force_algorithm</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/force_cpu_sort</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/history_size</code></pre></td>
      <td><pre><code>(int)sorting_history_size</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/log_interval_frames</code></pre></td>
      <td><pre><code>(int)sorting_log_interval</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/log_metrics</code></pre></td>
      <td><pre><code>sorting_log_metrics</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/radix_max_elements</code></pre></td>
      <td><pre><code>(int)sorting_radix_max</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/strict_global_sort</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.strict_global_sort</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/sorting/validate_sorted_output</code></pre></td>
      <td><pre><code>g_gpu_sorting_config.validate_sorted_output</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/renderer/gpu_sorting_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### streaming

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/async_pack_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/auto_regulate_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/chunk_frustum_culling_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/chunk_frustum_padding</code></pre></td>
      <td><pre><code>1.5f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/eviction_hysteresis_frames</code></pre></td>
      <td><pre><code>5</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_chunk_loads_per_frame</code></pre></td>
      <td><pre><code>16</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_chunks_in_vram</code></pre></td>
      <td><pre><code>128</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_evictions_per_frame</code></pre></td>
      <td><pre><code>4</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_pack_jobs_in_flight</code></pre></td>
      <td><pre><code>4</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_prefetch_chunk_scan_per_frame</code></pre></td>
      <td><pre><code>4096</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_prefetch_loads_per_frame</code></pre></td>
      <td><pre><code>6</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_sync_fallback_loads_per_frame</code></pre></td>
      <td><pre><code>1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_sync_fallback_queue_size</code></pre></td>
      <td><pre><code>2048</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_upload_mb_per_frame</code></pre></td>
      <td><pre><code>128</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_upload_mb_per_second</code></pre></td>
      <td><pre><code>0</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_upload_mb_per_slice</code></pre></td>
      <td><pre><code>16</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/max_visible_chunk_scan_per_frame</code></pre></td>
      <td><pre><code>4096</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/min_chunks_in_vram</code></pre></td>
      <td><pre><code>4</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/pack_worker_threads</code></pre></td>
      <td><pre><code>2</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/predictive_prefetch_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/prefetch_lookahead_distance</code></pre></td>
      <td><pre><code>10.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/queue_pressure_candidate_scan_throttle_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/queue_pressure_candidate_scan_throttle_min_queue_depth</code></pre></td>
      <td><pre><code>1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/queue_pressure_prefetch_scan_cap</code></pre></td>
      <td><pre><code>1024</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/queue_pressure_visible_scan_cap</code></pre></td>
      <td><pre><code>1024</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/regulation_step_percent</code></pre></td>
      <td><pre><code>1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/route_policy</code></pre></td>
      <td><pre><code>1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/validate_upload_payload_checksums</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/vram_budget_mb</code></pre></td>
      <td><pre><code>STREAMING_UNKNOWN_CAPACITY_FALLBACK_VRAM_BUDGET_MB</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/vram_warning_threshold_percent</code></pre></td>
      <td><pre><code>85</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/zero_visible_recovery_cooldown_frames</code></pre></td>
      <td><pre><code>30</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/zero_visible_recovery_log_interval_frames</code></pre></td>
      <td><pre><code>120</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/zero_visible_recovery_mode</code></pre></td>
      <td><pre><code>1</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/streaming/zero_visible_recovery_trigger_frames</code></pre></td>
      <td><pre><code>16</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### world

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default (as registered)</th>
      <th>Registered in</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/world/strict_identity_transform</code></pre></td>
      <td><pre><code>false</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

### Read but not registered
Listed in the manifest and read by module code, but not registered: they do not appear in the editor's Project Settings until a project sets them.

| Setting | What it controls (manifest `effective_state`) |
| --- | --- |
| `rendering/gaussian_splatting/cull/frustum_plane_slack` | GPUCuller/Tile renderer culling state.frustum_plane_slack |
| `rendering/gaussian_splatting/debug/enable_pipeline_trace` | none |
| `rendering/gaussian_splatting/debug/enable_splat_audit` | none |
| `rendering/gaussian_splatting/debug/force_unclustered_lights` | none |
| `rendering/gaussian_splatting/debug/splat_audit_sample_count` | none |
| `rendering/gaussian_splatting/streaming/layout_hint_validation_strict` | StreamingUploadPipeline/StreamingVisibilityController/VRAMBudgetRegulator.layout_hint_validation_strict |

### Deprecated aliases
Old key spellings still read when a project sets them explicitly; see the [migration notes](gaussian-project-settings-migration.md).

| Setting | Manifest note |
| --- | --- |
| `rendering/gaussian_splatting/debug/layout_hint_validation_strict` | Deprecated read-only alias (#173) of the canonical streaming/layout_hint_validation_strict. Consulted only when the canonical key is not explicitly set, and emits a one-time deprecation WARN. Kept for project.godot compatibility. |
| `rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms` | Older deprecated alias (#168): renamed to diagnostics/sort_target_time_ms (the sorting/ spelling is the intermediate alias). Still read as a read-only fallback with a one-time deprecation warning. |
| `rendering/gaussian_splatting/lod/debug_visualization` | Deprecated alias (#167): renamed to lod/diagnostic_logging. Still read as a read-only fallback (with a one-time deprecation warning) so existing project.godot files keep working; not auto-migrated on save, so rename it to the canonical key to stop the warning. |
| `rendering/gaussian_splatting/pipeline/enable_all_experimental` | Deprecated alias (#169): renamed to pipeline/enable_all_pipeline_experimental (the old name wrongly implied engine-wide scope). Still read as a read-only fallback (with a one-time deprecation warning) so existing project.godot files keep working; not auto-migrated on save, so rename it to the canonical key to stop the warning. |
| `rendering/gaussian_splatting/sorting/target_sort_time_ms` | Deprecated alias (#168): renamed to diagnostics/sort_target_time_ms. Still read as a read-only fallback (with a one-time deprecation warning) so existing project.godot files keep working; not auto-migrated on save, so rename it to the canonical key to stop the warning. |

## Examples
```bash
python3 scripts/generate_project_settings_reference.py
python3 tests/ci/check_project_settings_reference.py
```

## Troubleshooting
| Issue | Cause | Fix |
| --- | --- | --- |
| Generation fails: key missing from the manifest | Code registers a key the manifest does not list. | Add the key to `project_settings_manifest.json` (the manifest guard requires it too), then regenerate. |
| Generation fails: cannot resolve a key expression | A registration uses a key form the scanner does not understand. | Use a literal, a `const String`/`StringName` constant, a `#define`, or extend the scanner; never drop the registration from the page by hand. |
| Generation fails: conflicting defaults | One key is registered in two places with different defaults. | Make the registrations agree. |
| `check_project_settings_reference.py` fails | This page and the manifest list different keys. | Regenerate this file. |
| `docs-build` reports this page stale | The manifest or a registration changed without regenerating. | Regenerate this file and commit it. |
