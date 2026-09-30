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
> | `octree_max_depth` | **live** | Reaches the active path through `GPUCuller::CullingState::culling_octree_max_depth`, consumed in `ensure_hierarchical_structure()`. |
> | `min_gaussians_per_leaf` | **live** | Reaches the active path through `GPUCuller::CullingState::culling_min_gaussians`. |
> | `opacity_aware_bounds` | **live (default)** | Wired in [#167](https://github.com/klausi3D/godotGS/issues/167): read by `GPUCuller::update_culling_settings()` as the project-wide DEFAULT behind the per-renderer `cull/opacity_aware_culling` property (bound in `GaussianSplatRenderer::_bind_methods()`). An explicit per-node value overrides the global. Registered default `true` equals the construction default, so the wiring is behavior-neutral. |
> | `visibility_threshold` | **live (default)** | Wired in [#167](https://github.com/klausi3D/godotGS/issues/167): read by `GPUCuller::update_culling_settings()` as the project-wide DEFAULT behind the per-renderer `cull/visibility_threshold` property (bound in `GaussianSplatRenderer::_bind_methods()`), clamped to `0.0001..0.1`. Explicit per-node value wins. Registered default `gs::RASTER_ALPHA_THRESHOLD` (1/255) equals the construction default. |
>
> The `cull/overflow_autotune_enabled` sibling (different section) was wired the
> same way in [#167](https://github.com/klausi3D/godotGS/issues/167): it is the
> project-wide default behind the per-renderer `overflow_autotune` property
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
Use this reference to map Gaussian Splatting project setting keys to source definitions and runtime lookup paths.

## Usage
| Task | Action |
| --- | --- |
| Regenerate this reference | Run `python3 scripts/generate_project_settings_reference.py`. |
| Audit key usage in module code | Run `rg -n "rendering/gaussian_splatting/" modules/gaussian_splatting --glob '*.{h,cpp}'`. |

## API

### Registered keys
These settings are registered with `GLOBAL_DEF(...)` and grouped by key prefix.

| Coverage | Count |
| --- | ---: |
| Registered keys | 123 |
| Runtime-only keys | 28 |
| Registered keys without additional literal lookup | 1 |

#### Core

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### World

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Import

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Quality

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Streaming

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Culling

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Cull

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Lod

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
    </tr>
  </thead>
  <tbody>
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
      <td><pre><code>rendering/gaussian_splatting/lod/hysteresis_zone</code></pre></td>
      <td><pre><code>0.5f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
    <tr>
      <td><pre><code>rendering/gaussian_splatting/lod/importance_threshold</code></pre></td>
      <td><pre><code>-1.0f</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/lod/lod_config.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

#### Sorting

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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
  </tbody>
</table>

#### Debug

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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
  </tbody>
</table>

#### Logging

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Diagnostics

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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

#### Other

<table>
  <thead>
    <tr>
      <th>Setting</th>
      <th>Default</th>
      <th>Defined In</th>
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
    <tr>
      <td><pre><code>rendering/gaussian_splatting/cache/spirv_cache_enabled</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/register_types.cpp</code></pre></td>
    </tr>
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
    <tr>
      <td><pre><code>rendering/gaussian_splatting/init/eager_raster_pipeline</code></pre></td>
      <td><pre><code>true</code></pre></td>
      <td><pre><code>modules/gaussian_splatting/core/gaussian_splat_manager.cpp</code></pre></td>
    </tr>
  </tbody>
</table>

### Runtime-only keys
These keys are used by module code but are not registered with `GLOBAL_DEF(...)`.

| Setting | First reference |
| --- | --- |
| `rendering/gaussian_splatting/cache/spirv_cache_max_mb` | `modules/gaussian_splatting/register_types.cpp` |
| `rendering/gaussian_splatting/cull/frustum_plane_slack` | `modules/gaussian_splatting/interfaces/gpu_culler.cpp` |
| `rendering/gaussian_splatting/debug/enable_pipeline_trace` | `modules/gaussian_splatting/logger/gs_debug_trace.cpp` |
| `rendering/gaussian_splatting/debug/enable_splat_audit` | `modules/gaussian_splatting/renderer/render_debug_state_orchestrator.cpp` |
| `rendering/gaussian_splatting/debug/force_unclustered_lights` | `modules/gaussian_splatting/renderer/gpu_debug_utils.h` |
| `rendering/gaussian_splatting/debug/layout_hint_validation_strict` | `modules/gaussian_splatting/core/streaming_layout_hint.cpp` |
| `rendering/gaussian_splatting/debug/show_density_heatmap` | `modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp` |
| `rendering/gaussian_splatting/debug/show_performance_hud` | `modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp` |
| `rendering/gaussian_splatting/debug/show_residency_hud` | `modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp` |
| `rendering/gaussian_splatting/debug/show_tile_grid` | `modules/gaussian_splatting/core/gaussian_splat_settings_manager.cpp` |
| `rendering/gaussian_splatting/debug/splat_audit_sample_count` | `modules/gaussian_splatting/renderer/render_debug_state_orchestrator.cpp` |
| `rendering/gaussian_splatting/diagnostics/sort_target_time_ms` | `modules/gaussian_splatting/core/module_string_names.cpp` |
| `rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms` | `modules/gaussian_splatting/core/module_string_names.cpp` |
| `rendering/gaussian_splatting/lighting/direct_light_scale` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lighting/indirect_sh_scale` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lighting/shadow_receiver_bias_max` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lighting/shadow_receiver_bias_min` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lighting/shadow_receiver_bias_scale` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lighting/shadow_strength` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lod/bias` | `modules/gaussian_splatting/interfaces/gpu_culler.cpp` |
| `rendering/gaussian_splatting/lod/enabled` | `modules/gaussian_splatting/renderer/gaussian_splat_renderer.cpp` |
| `rendering/gaussian_splatting/lod/min_screen_size_pixels` | `modules/gaussian_splatting/interfaces/gpu_culler.cpp` |
| `rendering/gaussian_splatting/rasterization/low_pass_filter` | `modules/gaussian_splatting/core/gaussian_splat_manager.cpp` |
| `rendering/gaussian_splatting/resident/atlas_vram_budget_override_mb` | `modules/gaussian_splatting/core/gaussian_splat_manager.cpp` |
| `rendering/gaussian_splatting/sorting/strict_global_sort` | `modules/gaussian_splatting/renderer/gpu_sorting_config.cpp` |
| `rendering/gaussian_splatting/sorting/target_sort_time_ms` | `modules/gaussian_splatting/core/module_string_names.cpp` |
| `rendering/gaussian_splatting/sorting/validate_sorted_output` | `modules/gaussian_splatting/renderer/gpu_sorting_config.cpp` |
| `rendering/gaussian_splatting/streaming/layout_hint_validation_strict` | `modules/gaussian_splatting/core/streaming_layout_hint.cpp` |

### Registered keys without additional literal lookup
These registered keys have no additional string-literal references beyond their registration line.

| Setting | Registered in |
| --- | --- |
| `rendering/gaussian_splatting/import/gsplatworld_compression_enabled` | `modules/gaussian_splatting/core/gaussian_splat_manager.cpp` |

## Examples
```bash
python3 scripts/generate_project_settings_reference.py
```

```bash
rg -n "rendering/gaussian_splatting/" modules/gaussian_splatting --glob '*.{h,cpp}'
```

## Troubleshooting
| Issue | Cause | Fix |
| --- | --- | --- |
| A key exists in code but not under registered sections | The key is read at runtime without `GLOBAL_DEF(...)`. | Check the `Runtime-only keys` section and decide whether to register it. |
| A registered key appears unconsumed | No extra string-literal lookup path is present in module runtime code. | Verify lookup paths or remove stale registration. |
| This reference disagrees with the source | A setting was added, removed, renamed, moved to another file, or its default changed, and this file was not regenerated. | Regenerate this file. The docs CI check (`docs_pages.yml`, job `docs-build`) fails until the committed file matches the generator output. |
