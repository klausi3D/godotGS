# Gaussian Splatting ProjectSettings — migration notes

One authoritative place describing the supported
`rendering/gaussian_splatting/*` ProjectSettings surface after the settings
remediation tracked by [#162](https://github.com/klausi3D/godotGS/issues/162),
and the migration path for any key that was removed, renamed, or turned into a
read-only alias.

If you are updating a `project.godot` or saved content authored before this
cleanup, this page tells you what to change and why — no need to reverse-engineer
it from source.

## Where the surface is defined

- **Inventory + labels:** `modules/gaussian_splatting/config/project_settings_manifest.json`
  (every key, its `publicness`, `visibility`, `scope`, and notes).
- **Public API contract:** `modules/gaussian_splatting/config/project_settings_public_api_baseline.json`
  (`public_settings[]` and `retired_settings[]`), enforced by
  `modules/gaussian_splatting/tests/check_project_settings_manifest.py`.
- **Design rationale + labeling rules:** [`docs/architecture/gaussian-project-settings-contract.md`](../architecture/gaussian-project-settings-contract.md).
- **Generated reference of live keys:** [`docs/reference/project-settings.md`](project-settings.md).

The manifest and baseline are the source of truth; this document is the
human-readable migration companion.

## Deprecation lifecycle

A renamed key is not deleted immediately. It becomes a **read-only deprecated
alias**:

1. The **canonical** key is registered normally and is what the editor shows.
2. The old (alias) key is still **read** as a fallback — but only when the
   canonical key is not explicitly set — and reading it emits a **one-time
   deprecation `WARN`**.
3. When a project that still carries the old key is **saved**, the value is
   written to the canonical key and the alias is cleared (self-heal). After one
   save-and-reload, the warning stops.

So existing projects keep working unchanged; they just log one warning until
re-saved. A removed (not aliased) key has no runtime effect at all — delete any
`project.godot` entry for it at your convenience.

## Removed keys

These were registered but had **no runtime effect** (never read, or read but
never reaching the render path). Removing them changes no behavior. Delete any
`project.godot` entry; there is nothing to migrate to. Keys whose underlying
feature is planned carry a tracking issue for a future, deliberate
re-introduction.

| Removed key (`rendering/gaussian_splatting/…`) | Why it was inert | Future work |
| --- | --- | --- |
| `compression/adaptive_chunk_size` | configured feature not implemented; value never consumed | [#482](https://github.com/klausi3D/godotGS/issues/482) |
| `compression/max_chunk_size` | loaded/validated by `QuantizationConfig` but unused in chunk construction | [#482](https://github.com/klausi3D/godotGS/issues/482) |
| `compression/min_chunk_size` | loaded only for a cosmetic compression-ratio log line | [#482](https://github.com/klausi3D/godotGS/issues/482) |
| `debug/enable_mainloop_probes` | registered but never read | — |
| `max_gpu_buffer_count` | registered but never read | — |
| `pipeline/enable_two_stage_sort` | configured feature not implemented; value never consumed | [#481](https://github.com/klausi3D/godotGS/issues/481) |
| `pipeline/sh_amortization_disable_on_visibility_change` | validated by `PipelineFeatureSet` but never reaches `render_params` | [#487](https://github.com/klausi3D/godotGS/issues/487) |
| `pipeline/sh_amortization_visibility_threshold` | validated by `PipelineFeatureSet` but never reaches `render_params` | [#487](https://github.com/klausi3D/godotGS/issues/487) |
| `sorting/hybrid_batch_size` | configured feature not implemented; value never consumed | [#480](https://github.com/klausi3D/godotGS/issues/480) |
| `sorting/hybrid_trigger_elements` | configured feature not implemented; value never consumed | [#480](https://github.com/klausi3D/godotGS/issues/480) |
| `sorting/onesweep_max_elements` | read/sanitized but the AUTO selector uses only the two documented strategy boundaries | — (incoherent under the 2-boundary AUTO model) |
| `streaming/async_io_enabled` | registered but never read | — |
| `streaming/sh_progressive_load` | configured feature not implemented; value never consumed | [#483](https://github.com/klausi3D/godotGS/issues/483) |

**`debug/enable_state_guardrails`** was also removed (with its
`GaussianSplatRenderer` node property and the
`set_debug_state_guardrails_enabled` / `get_debug_state_guardrails_enabled`
methods). Its only gate had already been deleted, so the toggle was inert
(stored but never consumed) and it was not part of the public API baseline. No
migration is needed; remove any `project.godot` entry or node override.

## Renamed keys (read-only deprecated aliases)

The old key still works as a read-only fallback with a one-time warning and
self-heals on save (see [Deprecation lifecycle](#deprecation-lifecycle)). Move to
the canonical key when convenient.

| Old key (`rendering/gaussian_splatting/…`) | Canonical key (`rendering/gaussian_splatting/…`) | Why renamed |
| --- | --- | --- |
| `lod/debug_visualization` | `lod/diagnostic_logging` | name implied a visual overlay; it only toggles diagnostic logging ([#167](https://github.com/klausi3D/godotGS/issues/167)) |
| `sorting/target_sort_time_ms` | `diagnostics/sort_target_time_ms` | it is a diagnostics/telemetry target, not a sorting control ([#168](https://github.com/klausi3D/godotGS/issues/168)) |
| `gpu_sorting/target_sort_time_ms` | `diagnostics/sort_target_time_ms` | older spelling of the same key; `sorting/…` is the intermediate alias ([#168](https://github.com/klausi3D/godotGS/issues/168)) |
| `pipeline/enable_all_experimental` | `pipeline/enable_all_pipeline_experimental` | old name wrongly implied engine-wide scope ([#169](https://github.com/klausi3D/godotGS/issues/169)) |
| `debug/layout_hint_validation_strict` | `streaming/layout_hint_validation_strict` | control belongs to the streaming layout path, not generic debug ([#173](https://github.com/klausi3D/godotGS/issues/173)) |

Read precedence for the target-sort-time family is
`diagnostics/sort_target_time_ms` → `sorting/target_sort_time_ms` →
`gpu_sorting/target_sort_time_ms` → default; the first explicitly set key wins.

## Deprecated no-op kept for ABI

| Key | Status |
| --- | --- |
| `gpu_sorting_enabled` | Deprecated no-op. GPU sorting is always used when available; no renderer consults this flag. It stays registered and bound for ABI stability and warns when set. To force CPU sorting, use `sorting/force_cpu_sort`. |

## Ownership boundaries (intentionally not global settings)

Some behavior is deliberately owned somewhere other than a global
ProjectSetting. Do not expect a project-wide key for these:

- **Per-renderer node properties with a project-wide default.** The culling
  knobs `culling/visibility_threshold`, `culling/opacity_aware_bounds`, and
  `cull/overflow_autotune_enabled` are ProjectSettings that only seed the
  **default** for the matching `GaussianSplatRenderer` property
  (`cull/visibility_threshold`, `cull/opacity_aware_culling`,
  `cull/overflow_autotune_enabled`). A per-node value set in the inspector or from script
  overrides the project default; `GPUCuller::update_culling_settings()` applies
  the precedence.
- **Per-node debug toggles.** `debug/enable_pipeline_trace`,
  `debug/enable_splat_audit`, and `debug/splat_audit_sample_count` are bound
  `GaussianSplatRenderer` node properties (with a raw ProjectSettings read as the
  seed). Prefer setting them per node when you only want to instrument one
  renderer.
- **Per-asset / import-time controls.** Import and decode defaults are captured
  into the imported asset at import time; changing the ProjectSetting later does
  not retroactively rewrite already-imported `.gsplatworld` content — re-import
  to pick up new defaults.
- **Test-only and diagnostic levers.** Keys under `scope: debug` /
  `scope: diagnostic` (e.g. `sorting/validate_sorted_output`,
  `gpu_sorting/enable_stage_timestamps`, `gpu_sorting/enable_prefix_readback`,
  `gpu_sorting/debug_validate_prefix`,
  `gpu_sorting/profiling_preserve_gpu_timestamps`) are advanced
  debug/profiling instrumentation. They are editor-visible and settable via
  ProjectSettings (`PROPERTY_USAGE_NO_EDITOR` is a no-op for ProjectSettings —
  see [#491](https://github.com/klausi3D/godotGS/issues/491)) but are not general
  runtime controls; several force GPU sync stalls and should stay off in
  shipping projects.

## History

This surface was cleaned up across the S1–S7 slices under
[#162](https://github.com/klausi3D/godotGS/issues/162): dead-key removals
(#488), renames to canonical names with aliases (#489), and label/de-dup of the
debug/profiling hooks plus removal of the inert `enable_state_guardrails` (#490).
Earlier slices wired or truthfully documented the remaining public keys.
